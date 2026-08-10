// SPDX-License-Identifier: GPL-2.0-or-later
#include "openlens/session.hpp"
#include "openlens/sinks.hpp"
#include "openlens/wifi_discovery.hpp"
#include "openlens/wifi_transport.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

std::atomic_bool cancelled{false};
void stop(int) { cancelled = true; }

void usage() {
  std::cout << "OpenLens " << OPENLENS_VERSION << "\n\n"
            << "Most people should open OpenLens Desktop instead of using these tools.\n\n"
            << "Usage:\n"
            << "  openlens doctor [--json]\n"
            << "  openlens devices [--json]\n"
            << "  openlens pair [--id DEVICE_ID]\n"
            << "  openlens forget --id DEVICE_ID\n"
            << "  openlens start [--id DEVICE_ID] [--preset 1080p30|720p30] [camera controls]\n"
            << "  openlens receive --sink null [--seconds N] [--output FILE] [camera controls]\n"
            << "  openlens synthetic [--video /dev/video42] [--seconds N]\n";
}

std::optional<std::string> option(int argc, char** argv, std::string_view name) {
  for (int index = 2; index + 1 < argc; ++index)
    if (std::string_view(argv[index]) == name)
      return std::string(argv[index + 1]);
  return std::nullopt;
}

bool flag(int argc, char** argv, std::string_view name) {
  for (int index = 2; index < argc; ++index)
    if (std::string_view(argv[index]) == name)
      return true;
  return false;
}

int as_int(const std::optional<std::string>& value, int fallback, int minimum, int maximum,
           std::string_view name) {
  if (!value)
    return fallback;
  const int parsed = std::stoi(*value);
  if (parsed < minimum || parsed > maximum)
    throw std::runtime_error(std::string(name) + " is outside its supported range");
  return parsed;
}

double as_double(const std::optional<std::string>& value, double fallback, double minimum,
                 double maximum, std::string_view name) {
  if (!value)
    return fallback;
  const double parsed = std::stod(*value);
  if (parsed < minimum || parsed > maximum)
    throw std::runtime_error(std::string(name) + " is outside its supported range");
  return parsed;
}

std::string json_string(std::string_view value) {
  std::string output{"\""};
  for (const char character : value) {
    if (character == '"' || character == '\\')
      output.push_back('\\');
    if (static_cast<unsigned char>(character) >= 0x20U)
      output.push_back(character);
  }
  output.push_back('"');
  return output;
}

std::vector<openlens::WifiDevice> discover() {
  return openlens::discover_wifi_devices(std::chrono::milliseconds(1500));
}

openlens::WifiDevice choose_device(const std::vector<openlens::WifiDevice>& devices,
                                   const std::optional<std::string>& id, bool require_paired) {
  openlens::WifiIdentityStore identity;
  std::optional<openlens::WifiDevice> selected;
  for (const auto& device : devices) {
    if (id && device.device_id != *id)
      continue;
    if (require_paired && !identity.peer(device.device_id))
      continue;
    if (selected)
      throw std::runtime_error("more than one matching phone is available; pass --id DEVICE_ID");
    selected = device;
  }
  if (!selected)
    throw std::runtime_error(require_paired ? "no paired OpenLens phone was found on local Wi-Fi"
                                            : "no OpenLens phone was found on local Wi-Fi");
  return *selected;
}

int doctor(bool json) {
  bool avahi = true;
  std::vector<openlens::WifiDevice> devices;
  std::string error;
  try {
    devices = discover();
  } catch (const std::exception& failure) {
    avahi = false;
    error = failure.what();
  }
  openlens::WifiIdentityStore identity;
  std::size_t paired = 0;
  for (const auto& device : devices)
    if (identity.peer(device.device_id))
      ++paired;
  const bool v4l2 = std::filesystem::exists("/dev/video42");
  if (json) {
    std::cout << "{\"schema\":2,\"discovery\":" << (avahi ? "true" : "false")
              << ",\"phones\":" << devices.size() << ",\"paired\":" << paired
              << ",\"v4l2\":" << (v4l2 ? "true" : "false") << ",\"error\":" << json_string(error)
              << "}\n";
  } else {
    std::cout << "Local Wi-Fi discovery: " << (avahi ? "ready" : "unavailable") << '\n'
              << "OpenLens phones found: " << devices.size() << '\n'
              << "Paired phones found: " << paired << '\n'
              << "OBS virtual camera /dev/video42: " << (v4l2 ? "ready" : "not found") << '\n';
    if (!error.empty())
      std::cout << "Discovery detail: " << error << '\n';
  }
  return avahi && v4l2 ? 0 : 4;
}

int devices(bool json) {
  const auto found = discover();
  openlens::WifiIdentityStore identity;
  if (json)
    std::cout << "{\"schema\":2,\"devices\":[";
  for (std::size_t index = 0; index < found.size(); ++index) {
    const auto& device = found[index];
    const bool paired = identity.peer(device.device_id).has_value();
    if (json) {
      if (index > 0)
        std::cout << ',';
      std::cout << "{\"id\":" << json_string(device.device_id)
                << ",\"name\":" << json_string(device.service_name)
                << ",\"address\":" << json_string(device.address)
                << ",\"port\":" << device.port
                << ",\"paired\":" << (paired ? "true" : "false")
                << ",\"busy\":" << (device.busy ? "true" : "false") << '}';
    } else {
      std::cout << device.service_name << "\t" << device.device_id << "\t" << device.address
                << ':' << device.port << "\t"
                << (paired ? "paired" : "not paired") << '\n';
    }
  }
  if (json)
    std::cout << "]}\n";
  return 0;
}

int pair(int argc, char** argv) {
  const auto device = choose_device(discover(), option(argc, argv, "--id"), false);
  openlens::WifiIdentityStore identity;
  const auto result = openlens::pair_wifi_device(device, identity, [](std::string_view sas) {
    std::cout << "\nPairing code: " << sas << "\n"
              << "Confirm the same code on the phone, then type yes here: " << std::flush;
    std::string answer;
    std::getline(std::cin, answer);
    return answer == "yes" || answer == "y";
  });
  std::cout << "Paired with " << result.peer.name << ".\n";
  return 0;
}

int forget(int argc, char** argv) {
  const auto id = option(argc, argv, "--id");
  if (!id)
    throw std::runtime_error("forget requires --id DEVICE_ID");
  openlens::WifiIdentityStore().forget_peer(*id);
  std::cout << "Forgot the phone on this computer. Also tap Forget paired computer on the phone.\n";
  return 0;
}

int run_session(int argc, char** argv, bool v4l2) {
  openlens::SessionOptions options;
  options.wifi_device = choose_device(discover(), option(argc, argv, "--id"), true);
  options.preset = option(argc, argv, "--preset").value_or("1080p30");
  if (options.preset != "1080p30" && options.preset != "720p30")
    throw std::runtime_error("--preset must be 1080p30 or 720p30");
  options.facing = option(argc, argv, "--facing").value_or("back");
  if (options.facing != "back" && options.facing != "front")
    throw std::runtime_error("--facing must be back or front");
  options.bitrate = as_int(option(argc, argv, "--bitrate"), 0, 2000000, 16000000, "--bitrate");
  options.zoom = as_double(option(argc, argv, "--zoom"), 1.0, 1.0, 100.0, "--zoom");
  options.exposure = as_int(option(argc, argv, "--exposure"), 0, -100, 100, "--exposure");
  options.torch = flag(argc, argv, "--torch");
  options.video_device = option(argc, argv, "--video").value_or("/dev/video42");
  options.duration =
      std::chrono::seconds(as_int(option(argc, argv, "--seconds"), 0, 0, 86400, "--seconds"));
  options.encoded_output = option(argc, argv, "--output").value_or("");
  std::unique_ptr<openlens::FrameSink> sink =
      v4l2 ? std::unique_ptr<openlens::FrameSink>(
                 std::make_unique<openlens::V4l2Sink>(options.video_device))
           : std::unique_ptr<openlens::FrameSink>(std::make_unique<openlens::NullSink>());
  std::cout << "Starting the paired phone camera over private local Wi-Fi.\n";
  const auto stats = openlens::OpenLensSession(options).run(*sink, cancelled);
  std::cout << "frames=" << stats.frames << " messages=" << stats.messages
            << " sequence_gaps=" << stats.sequence_gaps << " decode_errors=" << stats.decode_errors
            << " bytes=" << stats.bytes << '\n';
  return stats.frames > 0 ? 0 : 5;
}

int synthetic(int argc, char** argv) {
  const std::string device = option(argc, argv, "--video").value_or("/dev/video42");
  const int seconds = as_int(option(argc, argv, "--seconds"), 10, 0, 86400, "--seconds");
  openlens::V4l2Sink sink(device);
  sink.configure(1280, 720, 30);
  const std::uint64_t count = static_cast<std::uint64_t>(seconds) * 30U;
  const auto begin = std::chrono::steady_clock::now();
  for (std::uint64_t index = 0; index < count && !cancelled; ++index) {
    sink.push(
        openlens::make_test_pattern(1280, 720, index, static_cast<std::int64_t>(index * 33333U)));
    std::this_thread::sleep_until(
        begin + std::chrono::microseconds(static_cast<std::int64_t>((index + 1U) * 33333U)));
  }
  sink.stop();
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, stop);
  std::signal(SIGTERM, stop);
  try {
    if (argc < 2) {
      usage();
      return 2;
    }
    const std::string_view command{argv[1]};
    if (command == "--help" || command == "help") {
      usage();
      return 0;
    }
    if (command == "--version" || command == "version") {
      std::cout << OPENLENS_VERSION << '\n';
      return 0;
    }
    if (command == "doctor")
      return doctor(flag(argc, argv, "--json"));
    if (command == "devices")
      return devices(flag(argc, argv, "--json"));
    if (command == "pair")
      return pair(argc, argv);
    if (command == "forget")
      return forget(argc, argv);
    if (command == "start")
      return run_session(argc, argv, true);
    if (command == "receive")
      return run_session(argc, argv, false);
    if (command == "synthetic")
      return synthetic(argc, argv);
    usage();
    return 2;
  } catch (const std::exception& error) {
    std::cerr << "openlens: " << error.what() << '\n';
    return 1;
  }
}
