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
  // Impl is defined only inside the transport implementation; the public
  // constructor exists so transports sharing the TLS layer can build streams.
  struct Impl;
  explicit WifiStream(std::unique_ptr<Impl> implementation);
  WifiStream(WifiStream&&) noexcept;
  WifiStream& operator=(WifiStream&&) noexcept;
  WifiStream(const WifiStream&) = delete;
  WifiStream& operator=(const WifiStream&) = delete;
  ~WifiStream();

  [[nodiscard]] std::ptrdiff_t read(std::span<std::byte> buffer);
  void write_all(std::span<const std::byte> bytes);
  void close() noexcept;

private:
  std::unique_ptr<Impl> implementation_;
};

using PairingConfirmation = std::function<bool(std::string_view sas)>;

[[nodiscard]] PairingResult
pair_wifi_device(const WifiDevice& device, WifiIdentityStore& store,
                 const PairingConfirmation& confirm,
                 std::chrono::milliseconds timeout = std::chrono::seconds(45));
[[nodiscard]] WifiStream
connect_wifi_stream(const WifiDevice& device, WifiIdentityStore& store,
                    std::chrono::milliseconds timeout = std::chrono::seconds(10));

// Run the same TLS + pairing exchange over an already-connected stream
// descriptor (used by the USB accessory transport). Takes ownership of the
// descriptor in both success and failure.
[[nodiscard]] PairingResult
pair_connected_descriptor(int descriptor, const std::string& device_id,
                          const std::string& device_name, WifiIdentityStore& store,
                          const PairingConfirmation& confirm);
// Open a camera stream over an already-connected descriptor. The phone is
// authenticated by matching its TLS identity against any stored peer pin.
[[nodiscard]] WifiStream connect_stream_descriptor(int descriptor, WifiIdentityStore& store);

} // namespace openlens
