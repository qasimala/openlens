// SPDX-License-Identifier: GPL-2.0-or-later
#include "openlens/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const auto bytes = std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), size);
  const auto result = openlens::protocol::parse_one(bytes);
  if (result.status == openlens::protocol::ParseStatus::Complete && result.message) {
    const auto serialized = openlens::protocol::serialize(*result.message);
    static_cast<void>(openlens::protocol::parse_one(serialized));
  }
  openlens::protocol::StreamParser parser;
  const std::size_t accepted =
      size > openlens::protocol::kMaxPayloadSize ? openlens::protocol::kMaxPayloadSize : size;
  parser.feed(bytes.first(accepted));
  static_cast<void>(parser.next());
  return 0;
}
