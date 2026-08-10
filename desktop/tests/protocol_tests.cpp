// SPDX-License-Identifier: GPL-2.0-or-later
#include "openlens/protocol.hpp"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using openlens::protocol::Flag;
using openlens::protocol::Message;
using openlens::protocol::MessageType;
using openlens::protocol::ParseStatus;

[[nodiscard]] std::vector<std::byte> read_hex(const std::string& path) {
  std::ifstream input(path);
  if (!input)
    throw std::runtime_error("cannot open fixture: " + path);
  const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
  std::vector<std::byte> bytes;
  int high = -1;
  for (const char raw_character : text) {
    const auto character = static_cast<unsigned char>(raw_character);
    if (std::isspace(character) != 0)
      continue;
    const int value = character >= '0' && character <= '9'   ? character - '0'
                      : character >= 'a' && character <= 'f' ? character - 'a' + 10
                      : character >= 'A' && character <= 'F' ? character - 'A' + 10
                                                             : -1;
    if (value < 0)
      throw std::runtime_error("invalid hex fixture");
    if (high < 0)
      high = value;
    else {
      bytes.push_back(static_cast<std::byte>((high << 4) | value));
      high = -1;
    }
  }
  if (high >= 0)
    throw std::runtime_error("odd hex fixture length");
  return bytes;
}

void require(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}

void test_golden_hello() {
  const auto golden =
      read_hex(std::string{OPENLENS_SOURCE_DIR} + "/protocol/fixtures/valid/hello.hex");
  const std::string json = R"({"schema":1,"client":"desktop","nonce":"0123456789abcdef"})";
  Message message{.header = {.type = static_cast<std::uint16_t>(MessageType::Hello),
                             .flags = static_cast<std::uint16_t>(Flag::Required),
                             .sequence = 1},
                  .payload = {reinterpret_cast<const std::byte*>(json.data()),
                              reinterpret_cast<const std::byte*>(json.data() + json.size())}};
  require(openlens::protocol::serialize(message) == golden, "HELLO differs from golden fixture");
  const auto parsed = openlens::protocol::parse_one(golden);
  require(parsed.status == ParseStatus::Complete, "golden HELLO did not parse");
  require(parsed.message->header.sequence == 1, "HELLO sequence differs");
  require(parsed.message->payload == message.payload, "HELLO payload differs");
}

void test_incremental_parser() {
  const auto bytes =
      read_hex(std::string{OPENLENS_SOURCE_DIR} + "/protocol/fixtures/valid/hello.hex");
  openlens::protocol::StreamParser parser;
  for (const std::byte value : bytes)
    parser.feed(std::span{&value, 1U});
  require(parser.next().status == ParseStatus::Complete, "byte-at-a-time parse failed");
  require(parser.buffered_bytes() == 0, "parser retained a complete message");
}

void test_malformed_fixtures() {
  const std::vector<std::string> names{"bad-magic.hex", "bad-major.hex", "short-header.hex",
                                       "oversized-metadata.hex", "unknown-required.hex"};
  for (const auto& name : names) {
    const auto bytes =
        read_hex(std::string{OPENLENS_SOURCE_DIR} + "/protocol/fixtures/malformed/" + name);
    require(openlens::protocol::parse_one(bytes).status == ParseStatus::Invalid,
            name + " was not rejected");
  }
}

void test_unknown_optional() {
  Message message{.header = {.type = 0x8000, .sequence = 9}, .payload = {}};
  const auto result = openlens::protocol::parse_one(openlens::protocol::serialize(message));
  require(result.status == ParseStatus::Complete, "unknown optional type was rejected");
  require(!openlens::protocol::is_known_type(result.message->header.type),
          "unknown type became known");
}
} // namespace

int main() {
  try {
    test_golden_hello();
    test_incremental_parser();
    test_malformed_fixtures();
    test_unknown_optional();
    std::cout << "protocol_tests: all tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "protocol_tests: " << error.what() << '\n';
    return 1;
  }
}
