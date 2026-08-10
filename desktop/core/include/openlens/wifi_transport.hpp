// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "openlens/pairing.hpp"
#include "openlens/wifi_discovery.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace openlens {

struct WifiPeer {
  std::string device_id;
  std::string name;
  pairing::Digest spki_pin{};
};

class WifiIdentityStore {
public:
  WifiIdentityStore();
  explicit WifiIdentityStore(std::string directory);

  void ensure_identity();
  [[nodiscard]] const std::string& certificate_path() const noexcept;
  [[nodiscard]] const std::string& private_key_path() const noexcept;
  [[nodiscard]] pairing::Digest local_spki_pin();
  [[nodiscard]] std::optional<WifiPeer> peer(std::string_view device_id) const;
  [[nodiscard]] std::vector<WifiPeer> peers() const;
  void save_peer(const WifiPeer& peer);
  void forget_peer(std::string_view device_id);

private:
  std::string directory_;
  std::string certificate_path_;
  std::string private_key_path_;
  std::string peers_path_;
};

struct PairingResult {
  WifiPeer peer;
  std::string sas;
};

class WifiStream {
public:
  WifiStream(WifiStream&&) noexcept;
  WifiStream& operator=(WifiStream&&) noexcept;
  WifiStream(const WifiStream&) = delete;
  WifiStream& operator=(const WifiStream&) = delete;
  ~WifiStream();

  [[nodiscard]] std::ptrdiff_t read(std::span<std::byte> buffer);
  void write_all(std::span<const std::byte> bytes);
  void close() noexcept;

private:
  struct Impl;
  explicit WifiStream(std::unique_ptr<Impl> implementation);
  std::unique_ptr<Impl> implementation_;
  friend WifiStream connect_wifi_stream(const WifiDevice&, WifiIdentityStore&,
                                        std::chrono::milliseconds);
};

using PairingConfirmation = std::function<bool(std::string_view sas)>;

[[nodiscard]] PairingResult
pair_wifi_device(const WifiDevice& device, WifiIdentityStore& store,
                 const PairingConfirmation& confirm,
                 std::chrono::milliseconds timeout = std::chrono::seconds(45));
[[nodiscard]] WifiStream
connect_wifi_stream(const WifiDevice& device, WifiIdentityStore& store,
                    std::chrono::milliseconds timeout = std::chrono::seconds(10));

} // namespace openlens
