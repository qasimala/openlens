// SPDX-License-Identifier: GPL-2.0-or-later
#include "openlens/session.hpp"
#include "openlens/wifi_discovery.hpp"

#include <obs/obs-module.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("OpenLens contributors")

namespace {

std::mutex active_mutex;
std::set<std::string> active_devices;

std::uint64_t monotonic_ns() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
}

class ObsSink final : public openlens::FrameSink {
public:
  ObsSink(obs_source_t* source, bool mirror, int rotation, int buffer_ms)
      : source_(source), mirror_(mirror), rotation_(rotation),
        buffer_delay_ns_(static_cast<std::uint64_t>(buffer_ms) * 1000000U) {}

  void configure(int width, int height, int) override {
    source_width_ = width;
    source_height_ = height;
  }

  void push(const openlens::VideoFrame& input) override {
    source_width_ = input.width;
    source_height_ = input.height;
    const auto transformed = openlens::transform_frame(input, rotation_, mirror_);
    if (first_pts_us_ < 0) {
      first_pts_us_ = transformed.pts_us;
      first_host_ns_ = monotonic_ns();
    }
    obs_source_frame frame{};
    frame.width = static_cast<std::uint32_t>(transformed.width);
    frame.height = static_cast<std::uint32_t>(transformed.height);
    frame.format = VIDEO_FORMAT_I420;
    frame.full_range = false;
    frame.timestamp = first_host_ns_ +
                      static_cast<std::uint64_t>(transformed.pts_us - first_pts_us_) * 1000U +
                      buffer_delay_ns_;
    const std::size_t y_size =
        static_cast<std::size_t>(transformed.width) * static_cast<std::size_t>(transformed.height);
    frame.data[0] = const_cast<std::uint8_t*>(transformed.i420.data());
    frame.data[1] = const_cast<std::uint8_t*>(transformed.i420.data() + y_size);
    frame.data[2] = const_cast<std::uint8_t*>(transformed.i420.data() + y_size * 5U / 4U);
    frame.linesize[0] = static_cast<std::uint32_t>(transformed.width);
    frame.linesize[1] = static_cast<std::uint32_t>(transformed.width / 2);
    frame.linesize[2] = static_cast<std::uint32_t>(transformed.width / 2);
    static_cast<void>(video_format_get_parameters(VIDEO_CS_709, VIDEO_RANGE_PARTIAL,
                                                  frame.color_matrix, frame.color_range_min,
                                                  frame.color_range_max));
    obs_source_output_video(source_, &frame);
  }

  void placeholder(std::string_view message) override {
    blog(LOG_WARNING, "[OpenLens] %.*s", static_cast<int>(message.size()), message.data());
    openlens::VideoFrame disconnected;
    disconnected.width = source_width_ > 0 ? source_width_ : 1280;
    disconnected.height = source_height_ > 0 ? source_height_ : 720;
    disconnected.pts_us = 0;
    const auto y_size = static_cast<std::size_t>(disconnected.width) *
                        static_cast<std::size_t>(disconnected.height);
    disconnected.i420.resize(y_size * 3U / 2U);
    for (int y = 0; y < disconnected.height; ++y) {
      for (int x = 0; x < disconnected.width; ++x) {
        const int diagonal = y * disconnected.width / disconnected.height;
        const bool marker =
            std::abs(x - diagonal) < 12 || std::abs((disconnected.width - 1 - x) - diagonal) < 12;
        disconnected
            .i420[static_cast<std::size_t>(y) * static_cast<std::size_t>(disconnected.width) +
                  static_cast<std::size_t>(x)] = marker ? 170U : 28U;
      }
    }
    std::fill(disconnected.i420.begin() + static_cast<std::ptrdiff_t>(y_size),
              disconnected.i420.begin() + static_cast<std::ptrdiff_t>(y_size * 5U / 4U), 96U);
    std::fill(disconnected.i420.begin() + static_cast<std::ptrdiff_t>(y_size * 5U / 4U),
              disconnected.i420.end(), 190U);
    first_pts_us_ = -1;
    push(disconnected);
  }
  void flush() override {}
  void stop() override {}

private:
  obs_source_t* source_{};
  bool mirror_{};
  int rotation_{};
  std::uint64_t buffer_delay_ns_{};
  std::int64_t first_pts_us_{-1};
  std::uint64_t first_host_ns_{};
  int source_width_{};
  int source_height_{};
};

struct SourceState {
  obs_source_t* source{};
  std::string device_id;
  std::string device_key;
  std::string preset;
  std::string facing{"back"};
  int bitrate{8000000};
  double zoom{1.0};
  int exposure{};
  bool torch{};
  bool mirror{};
  int rotation{};
  int buffer_ms{};
  std::atomic_bool stop_when_hidden{};
  bool owns_device{};
  std::atomic_bool stop{false};
  std::thread worker;
  std::mutex settings_mutex;

  ~SourceState() {
    stop = true;
    if (worker.joinable())
      worker.join();
    if (owns_device) {
      std::lock_guard lock(active_mutex);
      active_devices.erase(device_key);
    }
  }
};

const char* source_name(void*) { return "OpenLens Phone Camera"; }

void source_defaults(obs_data_t* settings) {
  obs_data_set_default_string(settings, "device_id", "");
  obs_data_set_default_string(settings, "preset", "1080p30");
  obs_data_set_default_int(settings, "bitrate", 8000000);
  obs_data_set_default_string(settings, "facing", "back");
  obs_data_set_default_double(settings, "zoom", 1.0);
  obs_data_set_default_int(settings, "exposure", 0);
  obs_data_set_default_bool(settings, "torch", false);
  obs_data_set_default_bool(settings, "mirror", false);
  obs_data_set_default_int(settings, "rotation", 0);
  obs_data_set_default_int(settings, "buffer_ms", 0);
  obs_data_set_default_bool(settings, "stop_when_hidden", false);
}

obs_properties_t* source_properties(void*) {
  obs_properties_t* properties = obs_properties_create();
  obs_properties_add_text(properties, "device_id", "Paired phone ID (blank: only available phone)",
                          OBS_TEXT_DEFAULT);
  obs_property_t* preset = obs_properties_add_list(properties, "preset", "Quality",
                                                   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
  obs_property_list_add_string(preset, "1080p30 (8 Mbps)", "1080p30");
  obs_property_list_add_string(preset, "720p30 (4 Mbps)", "720p30");
  obs_properties_add_int_slider(properties, "bitrate", "Bitrate", 2000000, 16000000, 500000);
  obs_property_t* facing = obs_properties_add_list(properties, "facing", "Camera",
                                                   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
  obs_property_list_add_string(facing, "Rear", "back");
  obs_property_list_add_string(facing, "Front", "front");
  obs_properties_add_float_slider(properties, "zoom", "Zoom", 1.0, 10.0, 0.1);
  obs_properties_add_int_slider(properties, "exposure", "Exposure compensation", -20, 20, 1);
  obs_properties_add_bool(properties, "torch", "Torch");
  obs_properties_add_bool(properties, "mirror", "Mirror horizontally");
  obs_property_t* rotation = obs_properties_add_list(properties, "rotation", "Rotation",
                                                     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
  obs_property_list_add_int(rotation, "0°", 0);
  obs_property_list_add_int(rotation, "90°", 90);
  obs_property_list_add_int(rotation, "180°", 180);
  obs_property_list_add_int(rotation, "270°", 270);
  obs_property_t* buffer = obs_properties_add_list(properties, "buffer_ms", "Buffer mode",
                                                   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
  obs_property_list_add_int(buffer, "Lowest latency", 0);
  obs_property_list_add_int(buffer, "Balanced (66 ms)", 66);
  obs_property_list_add_int(buffer, "Smooth (133 ms)", 133);
  obs_properties_add_bool(properties, "stop_when_hidden", "Stop camera when source is hidden");
  obs_properties_add_text(properties, "status", "Status appears in the OBS log and phone UI",
                          OBS_TEXT_INFO);
  return properties;
}

void start_worker(SourceState* state) {
  if (state->worker.joinable())
    state->worker.join();
  state->stop = false;
  state->worker = std::thread([state] {
    try {
      openlens::SessionOptions options;
      bool mirror = false;
      int rotation = 0;
      int buffer_ms = 0;
      {
        std::lock_guard lock(state->settings_mutex);
        if (!state->device_id.empty()) {
          const auto devices = openlens::discover_wifi_devices(std::chrono::milliseconds(1500));
          const auto selected =
              std::find_if(devices.begin(), devices.end(), [&](const openlens::WifiDevice& device) {
                return device.device_id == state->device_id;
              });
          if (selected == devices.end())
            throw std::runtime_error("selected paired phone was not found on local Wi-Fi");
          options.wifi_device = *selected;
        }
        options.preset = state->preset;
        options.facing = state->facing;
        options.bitrate = state->bitrate;
        options.zoom = state->zoom;
        options.exposure = state->exposure;
        options.torch = state->torch;
        mirror = state->mirror;
        rotation = state->rotation;
        buffer_ms = state->buffer_ms;
      }
      ObsSink sink(state->source, mirror, rotation, buffer_ms);
      openlens::OpenLensSession session(std::move(options));
      const auto stats = session.run(sink, state->stop);
      blog(LOG_INFO, "[OpenLens] session ended: %llu frames, %llu gaps, %llu decode errors",
           static_cast<unsigned long long>(stats.frames),
           static_cast<unsigned long long>(stats.sequence_gaps),
           static_cast<unsigned long long>(stats.decode_errors));
    } catch (const std::exception& error) {
      if (!state->stop)
        blog(LOG_ERROR, "[OpenLens] %s", error.what());
    }
  });
}

void* source_create(obs_data_t* settings, obs_source_t* source) {
  auto state = std::make_unique<SourceState>();
  state->source = source;
  state->device_id = obs_data_get_string(settings, "device_id");
  state->device_key = state->device_id.empty() ? "auto" : state->device_id;
  state->preset = obs_data_get_string(settings, "preset");
  state->facing = obs_data_get_string(settings, "facing");
  state->bitrate = static_cast<int>(obs_data_get_int(settings, "bitrate"));
  state->zoom = obs_data_get_double(settings, "zoom");
  state->exposure = static_cast<int>(obs_data_get_int(settings, "exposure"));
  state->torch = obs_data_get_bool(settings, "torch");
  state->mirror = obs_data_get_bool(settings, "mirror");
  state->rotation = static_cast<int>(obs_data_get_int(settings, "rotation"));
  state->buffer_ms = static_cast<int>(obs_data_get_int(settings, "buffer_ms"));
  state->stop_when_hidden = obs_data_get_bool(settings, "stop_when_hidden");
  {
    std::lock_guard lock(active_mutex);
    if (active_devices.contains(state->device_key)) {
      blog(LOG_ERROR, "[OpenLens] Device is already controlled by another source");
      return state.release();
    }
    active_devices.insert(state->device_key);
    state->owns_device = true;
  }
  SourceState* raw = state.get();
  start_worker(raw);
  return state.release();
}

void source_destroy(void* data) { delete static_cast<SourceState*>(data); }

void source_update(void* data, obs_data_t* settings) {
  auto* state = static_cast<SourceState*>(data);
  std::lock_guard lock(state->settings_mutex);
  state->preset = obs_data_get_string(settings, "preset");
  state->facing = obs_data_get_string(settings, "facing");
  state->bitrate = static_cast<int>(obs_data_get_int(settings, "bitrate"));
  state->zoom = obs_data_get_double(settings, "zoom");
  state->exposure = static_cast<int>(obs_data_get_int(settings, "exposure"));
  state->torch = obs_data_get_bool(settings, "torch");
  state->mirror = obs_data_get_bool(settings, "mirror");
  state->rotation = static_cast<int>(obs_data_get_int(settings, "rotation"));
  state->buffer_ms = static_cast<int>(obs_data_get_int(settings, "buffer_ms"));
  state->stop_when_hidden = obs_data_get_bool(settings, "stop_when_hidden");
  blog(LOG_INFO, "[OpenLens] Property changes apply on the next connection");
}

void source_show(void* data) {
  auto* state = static_cast<SourceState*>(data);
  if (state->stop_when_hidden && state->stop)
    start_worker(state);
}

void source_hide(void* data) {
  auto* state = static_cast<SourceState*>(data);
  if (state->stop_when_hidden)
    state->stop = true;
}

obs_source_info source_info{};

} // namespace

extern "C" const char* obs_module_name(void) { return "OpenLens Phone Camera"; }
extern "C" const char* obs_module_description(void) {
  return "Uses a paired Android phone as a private low-latency OBS camera over local Wi-Fi.";
}

extern "C" bool obs_module_load(void) {
  source_info.id = "openlens_phone_camera";
  source_info.type = OBS_SOURCE_TYPE_INPUT;
  source_info.output_flags = OBS_SOURCE_ASYNC_VIDEO;
  source_info.get_name = source_name;
  source_info.create = source_create;
  source_info.destroy = source_destroy;
  source_info.get_defaults = source_defaults;
  source_info.get_properties = source_properties;
  source_info.update = source_update;
  source_info.show = source_show;
  source_info.hide = source_hide;
  obs_register_source(&source_info);
  blog(LOG_INFO, "[OpenLens] native source loaded");
  return true;
}
