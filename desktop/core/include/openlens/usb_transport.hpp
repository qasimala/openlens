// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace openlens {

struct UsbPhone {
  std::string serial;
  std::string product;
  bool accessory_mode{};
};

// Phones currently reachable over USB: devices already in Android Open
// Accessory mode plus devices that answer the accessory protocol probe.
[[nodiscard]] std::vector<UsbPhone> list_usb_phones();

// A byte pipe to the phone over Android Open Accessory bulk endpoints.
// The accessory handshake switches the phone into accessory mode when needed,
// then background pumps bridge the bulk endpoints to a socketpair so the TLS
// layer can treat the link as an ordinary connected stream descriptor.
class UsbAccessoryLink {
public:
  // Finds the phone, performs the accessory switch, and starts the pumps.
  // Throws std::runtime_error when no phone is reachable over USB.
  static UsbAccessoryLink open(std::chrono::milliseconds timeout = std::chrono::seconds(25));

  UsbAccessoryLink(UsbAccessoryLink&&) noexcept;
  UsbAccessoryLink& operator=(UsbAccessoryLink&&) noexcept;
  UsbAccessoryLink(const UsbAccessoryLink&) = delete;
  UsbAccessoryLink& operator=(const UsbAccessoryLink&) = delete;
  ~UsbAccessoryLink();

  // Serial number of the connected phone, for identifying the pairing record.
  [[nodiscard]] const std::string& serial() const noexcept;
  [[nodiscard]] const std::string& product() const noexcept;

  // Hands out the connected stream descriptor. The caller owns it and must
  // close it (closing it also winds down the pumps). Can be taken once.
  [[nodiscard]] int release_descriptor();

private:
  struct Impl;
  explicit UsbAccessoryLink(std::unique_ptr<Impl> implementation);
  std::unique_ptr<Impl> implementation_;
};

} // namespace openlens
