#include <obs/obs.h>
#include <obs/util/base.h>

#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define OBS_V4L2_MODULE "/usr/lib/obs-plugins/linux-v4l2.so"
#define OBS_V4L2_DATA "/usr/share/obs/obs-plugins/linux-v4l2"
#define V4L2_PIX_FMT_YUV420 842093913

static uint64_t pack_pair(uint32_t first, uint32_t second) {
  return ((uint64_t)second << 32) | first;
}

static uint64_t monotonic_ns(void) {
  struct timespec value;
  clock_gettime(CLOCK_MONOTONIC, &value);
  return (uint64_t)value.tv_sec * 1000000000ULL + (uint64_t)value.tv_nsec;
}

static void sleep_ms(long milliseconds) {
  const struct timespec duration = {
      .tv_sec = milliseconds / 1000,
      .tv_nsec = (milliseconds % 1000) * 1000000L,
  };
  nanosleep(&duration, NULL);
}

struct capture_state {
  atomic_uint_fast64_t frames;
  atomic_uint_fast64_t first_frame_ns;
  atomic_uint_fast64_t last_frame_ns;
  atomic_uint width;
  atomic_uint height;
};

static void capture_log_handler(int level, const char* format, va_list args, void* parameter) {
  (void)level;
  struct capture_state* state = parameter;
  char message[2048];
  va_list copy;
  va_copy(copy, args);
  vsnprintf(message, sizeof(message), format, copy);
  va_end(copy);

  unsigned int width = 0;
  unsigned int height = 0;
  if (sscanf(message, "v4l2-input: Resolution: %ux%u", &width, &height) == 2) {
    atomic_store(&state->width, width);
    atomic_store(&state->height, height);
  }

  if (strstr(message, "v4l2-input:") != NULL && strstr(message, ": ts:") != NULL) {
    const uint64_t now = monotonic_ns();
    uint64_t expected = 0;
    atomic_compare_exchange_strong(&state->first_frame_ns, &expected, now);
    atomic_store(&state->last_frame_ns, now);
    atomic_fetch_add(&state->frames, 1);
  }
}

int main(int argc, char** argv) {
  const char* device = argc > 1 ? argv[1] : "/dev/video42";
  const uint64_t deadline_ns = monotonic_ns() + 8000000000ULL;
  obs_module_t* module = NULL;
  obs_data_t* settings = NULL;
  obs_source_t* source = NULL;
  struct capture_state capture = {0};
  log_handler_t previous_log_handler = NULL;
  void* previous_log_parameter = NULL;
  int exit_code = EXIT_FAILURE;
  base_get_log_handler(&previous_log_handler, &previous_log_parameter);
  base_set_log_handler(capture_log_handler, &capture);

  if (!obs_startup("en-US", NULL, NULL)) {
    fprintf(stderr, "obs_startup failed\n");
    goto cleanup;
  }

  const int module_result = obs_open_module(&module, OBS_V4L2_MODULE, OBS_V4L2_DATA);
  if (module_result != MODULE_SUCCESS || !obs_init_module(module)) {
    fprintf(stderr, "Failed to load OBS linux-v4l2 module: %d\n", module_result);
    goto cleanup;
  }

  settings = obs_get_source_defaults("v4l2_input");
  if (settings == NULL)
    settings = obs_data_create();
  obs_data_set_string(settings, "device_id", device);
  obs_data_set_int(settings, "input", 0);
  obs_data_set_int(settings, "pixelformat", V4L2_PIX_FMT_YUV420);
  obs_data_set_int(settings, "resolution", (long long)pack_pair(1920, 1080));
  obs_data_set_int(settings, "framerate", (long long)pack_pair(30, 1));

  source = obs_source_create("v4l2_input", "OpenLens Phase 0", settings, NULL);
  if (source == NULL) {
    fprintf(stderr, "OBS could not create the v4l2_input source for %s\n", device);
    goto cleanup;
  }

  obs_source_inc_showing(source);
  while (monotonic_ns() < deadline_ns)
    sleep_ms(5);
  obs_source_dec_showing(source);
  sleep_ms(100);

  const uint64_t frames = atomic_load(&capture.frames);
  const uint64_t first_frame_ns = atomic_load(&capture.first_frame_ns);
  const uint64_t last_frame_ns = atomic_load(&capture.last_frame_ns);
  const uint32_t width = atomic_load(&capture.width);
  const uint32_t height = atomic_load(&capture.height);
  const double media_seconds =
      last_frame_ns > first_frame_ns ? (last_frame_ns - first_frame_ns) / 1000000000.0 : 0.0;
  const double measured_fps = media_seconds > 0.0 ? (frames - 1) / media_seconds : 0.0;
  printf("{\"source_id\":\"v4l2_input\",\"device\":\"%s\","
         "\"width\":%u,\"height\":%u,\"unique_frames\":%" PRIu64 ","
         "\"media_seconds\":%.6f,\"measured_fps\":%.3f}\n",
         device, width, height, frames, media_seconds, measured_fps);

  if (width == 1920 && height == 1080 && frames >= 120 && measured_fps >= 25.0)
    exit_code = EXIT_SUCCESS;

cleanup:
  if (source != NULL)
    obs_source_release(source);
  if (settings != NULL)
    obs_data_release(settings);
  if (obs_initialized())
    obs_shutdown();
  base_set_log_handler(previous_log_handler, previous_log_parameter);
  return exit_code;
}
