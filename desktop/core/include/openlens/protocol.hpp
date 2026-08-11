// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace openlens::protocol {

inline constexpr std::array<std::byte, 4> kMagic{std::byte{'P'}, std::byte{'H'}, std::byte{'C'},
                                                 std::byte{'M'}};
inline constexpr std::uint8_t kMajor = 2;
inline constexpr std::uint8_t kMinor = 0;
inline constexpr std::uint16_t kBaseHeaderSize = 36;
inline constexpr std::uint16_t kMaxHeaderSize = 4096;
inline constexpr std::uint32_t kMaxPayloadSize = 8U * 1024U * 1024U;
inline constexpr std::uint32_t kMaxMetadataSize = 256U * 1024U;

enum class MessageType : std::uint16_t {
  Hello = 1,
  HelloAck = 2,
  Capabilities = 3,
  Configure = 4,
  Configured = 5,
  VideoConfig = 6,
  VideoFrame = 7,
  Control = 8,
  ControlAck = 9,
  Stats = 10,
  Ping = 11,
  Pong = 12,
  Error = 13,
  EndStream = 14,
  Orientation = 15,
};

enum class Flag : std::uint16_t {
  Required = 1U << 0U,
  Keyframe = 1U << 1U,
  Config = 1U << 2U,
  Acknowledgement = 1U << 3U,
  EndOfStream = 1U << 4U,
};

[[nodiscard]] constexpr std::uint16_t operator|(Flag left, Flag right) {
  return static_cast<std::uint16_t>(left) | static_cast<std::uint16_t>(right);
}

struct Header {
  std::uint8_t major{kMajor};
  std::uint8_t minor{kMinor};
  std::uint16_t type{};
  std::uint16_t flags{};
  std::uint16_t header_length{kBaseHeaderSize};
  std::uint32_t stream_id{};
  std::uint64_t sequence{};
  std::uint64_t pts_us{};
  std::uint32_t payload_length{};
};

struct Message {
  Header header;
  std::vector<std::byte> payload;
};
enum class ParseStatus { Complete, NeedMoreData, Invalid };
struct ParseResult {
  ParseStatus status{ParseStatus::NeedMoreData};
  std::size_t consumed{};
  std::optional<Message> message;
  std::string error;
};

class ProtocolError final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

[[nodiscard]] bool is_known_type(std::uint16_t type) noexcept;
[[nodiscard]] bool is_metadata_type(std::uint16_t type) noexcept;
[[nodiscard]] std::vector<std::byte> serialize(const Message& message);
[[nodiscard]] ParseResult parse_one(std::span<const std::byte> bytes);

class StreamParser {
public:
  void feed(std::span<const std::byte> bytes);
  [[nodiscard]] ParseResult next();
  [[nodiscard]] std::size_t buffered_bytes() const noexcept;
  void reset() noexcept;

private:
  std::vector<std::byte> buffer_;
};

} // namespace openlens::protocol
