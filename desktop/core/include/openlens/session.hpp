// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "openlens/sinks.hpp"
#include "openlens/wifi_discovery.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace openlens {

struct SessionOptions {
  std::optional<WifiDevice> wifi_device;
  std::string preset{"1080p30"};
  std::string facing{"back"};
  int bitrate{};
  double zoom{1.0};
  int exposure{};
  bool torch{};
  std::string video_device{"/dev/video42"};
  std::chrono::seconds duration{0};
  std::string encoded_output;
};

struct SessionStats {
  std::uint64_t messages{};
  std::uint64_t frames{};
  std::uint64_t sequence_gaps{};
  std::uint64_t decode_errors{};
  std::uint64_t bytes{};
};

class OpenLensSession {
public:
  explicit OpenLensSession(SessionOptions options);
  [[nodiscard]] SessionStats run(FrameSink& sink, std::atomic_bool& cancelled);

private:
  SessionOptions options_;
};

} // namespace openlens
