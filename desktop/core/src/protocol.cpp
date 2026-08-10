// SPDX-License-Identifier: GPL-2.0-or-later
#include "openlens/protocol.hpp"

#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>

namespace openlens::protocol {
namespace {

template <typename Value> void append_big_endian(std::vector<std::byte>& output, Value value) {
  static_assert(std::is_unsigned_v<Value>);
  for (std::size_t shift = sizeof(Value); shift > 0; --shift) {
    const auto bits = static_cast<unsigned int>((shift - 1U) * 8U);
    output.push_back(static_cast<std::byte>((value >> bits) & static_cast<Value>(0xffU)));
  }
}

template <typename Value>
[[nodiscard]] Value read_big_endian(std::span<const std::byte> bytes, std::size_t offset) {
  static_assert(std::is_unsigned_v<Value>);
  Value value{};
  for (std::size_t index = 0; index < sizeof(Value); ++index) {
    value = static_cast<Value>(
        (value << 8U) | static_cast<Value>(std::to_integer<unsigned int>(bytes[offset + index])));
  }
  return value;
}

[[nodiscard]] ParseResult invalid(std::string error) {
  return {.status = ParseStatus::Invalid,
          .consumed = 0,
          .message = std::nullopt,
          .error = std::move(error)};
}

[[nodiscard]] std::uint32_t payload_limit(std::uint16_t type) noexcept {
  return is_metadata_type(type) ? kMaxMetadataSize : kMaxPayloadSize;
}

} // namespace

bool is_known_type(std::uint16_t type) noexcept {
  return type >= static_cast<std::uint16_t>(MessageType::Hello) &&
         type <= static_cast<std::uint16_t>(MessageType::EndStream);
}

bool is_metadata_type(std::uint16_t type) noexcept {
  return type != static_cast<std::uint16_t>(MessageType::VideoConfig) &&
         type != static_cast<std::uint16_t>(MessageType::VideoFrame);
}

std::vector<std::byte> serialize(const Message& message) {
  if (message.header.major != kMajor)
    throw ProtocolError("unsupported protocol major");
  if (message.header.header_length != kBaseHeaderSize)
    throw ProtocolError("serializer requires base header");
  if (message.payload.size() > payload_limit(message.header.type))
    throw ProtocolError("payload exceeds limit");
  if (message.payload.size() > std::numeric_limits<std::uint32_t>::max())
    throw ProtocolError("payload too large");
  std::vector<std::byte> output;
  output.reserve(kBaseHeaderSize + message.payload.size());
  output.insert(output.end(), kMagic.begin(), kMagic.end());
  output.push_back(static_cast<std::byte>(message.header.major));
  output.push_back(static_cast<std::byte>(message.header.minor));
  append_big_endian(output, message.header.type);
  append_big_endian(output, message.header.flags);
  append_big_endian(output, message.header.header_length);
  append_big_endian(output, message.header.stream_id);
  append_big_endian(output, message.header.sequence);
  append_big_endian(output, message.header.pts_us);
  append_big_endian(output, static_cast<std::uint32_t>(message.payload.size()));
  output.insert(output.end(), message.payload.begin(), message.payload.end());
  return output;
}

ParseResult parse_one(std::span<const std::byte> bytes) {
  if (bytes.size() < kBaseHeaderSize)
    return {
        .status = ParseStatus::NeedMoreData, .consumed = 0, .message = std::nullopt, .error = {}};
  if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin()))
    return invalid("invalid PHCM magic");
  Header header;
  header.major = std::to_integer<std::uint8_t>(bytes[4]);
  header.minor = std::to_integer<std::uint8_t>(bytes[5]);
  header.type = read_big_endian<std::uint16_t>(bytes, 6);
  header.flags = read_big_endian<std::uint16_t>(bytes, 8);
  header.header_length = read_big_endian<std::uint16_t>(bytes, 10);
  header.stream_id = read_big_endian<std::uint32_t>(bytes, 12);
  header.sequence = read_big_endian<std::uint64_t>(bytes, 16);
  header.pts_us = read_big_endian<std::uint64_t>(bytes, 24);
  header.payload_length = read_big_endian<std::uint32_t>(bytes, 32);
  if (header.major != kMajor)
    return invalid("unsupported protocol major");
  if (header.header_length < kBaseHeaderSize || header.header_length > kMaxHeaderSize)
    return invalid("invalid header length");
  if (header.payload_length > payload_limit(header.type))
    return invalid("payload exceeds message limit");
  const bool required = (header.flags & static_cast<std::uint16_t>(Flag::Required)) != 0U;
  if (!is_known_type(header.type) && required)
    return invalid("unknown required message type");
  const std::size_t header_size = header.header_length;
  const std::size_t payload_size = header.payload_length;
  if (payload_size > std::numeric_limits<std::size_t>::max() - header_size)
    return invalid("message size overflow");
  const std::size_t total_size = header_size + payload_size;
  if (bytes.size() < total_size)
    return {
        .status = ParseStatus::NeedMoreData, .consumed = 0, .message = std::nullopt, .error = {}};
  Message message{.header = header, .payload = {}};
  message.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(header_size),
                         bytes.begin() + static_cast<std::ptrdiff_t>(total_size));
  return {.status = ParseStatus::Complete,
          .consumed = total_size,
          .message = std::move(message),
          .error = {}};
}

void StreamParser::feed(std::span<const std::byte> bytes) {
  const std::size_t maximum = static_cast<std::size_t>(kMaxPayloadSize) + kMaxHeaderSize;
  if (buffer_.size() > maximum || bytes.size() > maximum - buffer_.size())
    throw ProtocolError("parser buffer limit exceeded");
  buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

ParseResult StreamParser::next() {
  ParseResult result = parse_one(buffer_);
  if (result.status == ParseStatus::Complete)
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(result.consumed));
  return result;
}

std::size_t StreamParser::buffered_bytes() const noexcept { return buffer_.size(); }
void StreamParser::reset() noexcept { buffer_.clear(); }

} // namespace openlens::protocol
