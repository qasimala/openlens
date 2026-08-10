// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace openlens {

struct WifiDevice {
  std::string service_name;
  std::string address;
  std::string device_id;
  std::uint16_t port{};
  int interface_index{};
  bool pairing{};
  bool busy{};
};

[[nodiscard]] std::vector<WifiDevice>
discover_wifi_devices(std::chrono::milliseconds duration = std::chrono::milliseconds(1500));

} // namespace openlens
