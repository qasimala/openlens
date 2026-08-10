// SPDX-License-Identifier: GPL-2.0-or-later
#include "openlens/pairing.hpp"
#include "openlens/sinks.hpp"
#include "openlens/wifi_transport.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
void require(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}
} // namespace

int main() {
  try {
    auto first = openlens::make_test_pattern(64, 32, 10, 333330);
    auto same = openlens::make_test_pattern(64, 32, 10, 333330);
    auto next = openlens::make_test_pattern(64, 32, 11, 366663);
    require(first.i420 == same.i420, "test pattern is not deterministic");
    require(first.i420 != next.i420, "test pattern did not advance");
    require(first.i420.size() == 64U * 32U * 3U / 2U, "I420 buffer size differs");
    const auto chroma = first.i420.begin() + static_cast<std::ptrdiff_t>(64U * 32U);
    require(std::all_of(chroma, first.i420.end(), [](std::uint8_t value) { return value == 128U; }),
            "test pattern chroma differs");

    openlens::NullSink sink;
    sink.configure(64, 32, 30);
    sink.push(first);
    sink.push(next);
    require(sink.frames() == 2U, "null sink frame count differs");
    const auto rotated = openlens::transform_frame(first, 90, true);
    require(rotated.width == 32, "rotated width differs");
    require(rotated.height == 64, "rotated height differs");
    require(rotated.i420.size() == first.i420.size(), "rotated buffer size differs");

    std::array<std::byte, 32> attempt{};
    std::array<std::byte, 32> phone_nonce{};
    std::array<std::byte, 32> desktop_nonce{};
    std::array<std::byte, 32> phone_spki{};
    std::array<std::byte, 32> desktop_spki{};
    for (std::size_t index = 0; index < attempt.size(); ++index) {
      attempt[index] = static_cast<std::byte>(index);
      phone_nonce[index] = static_cast<std::byte>(index + 32U);
      desktop_nonce[index] = static_cast<std::byte>(index + 64U);
      phone_spki[index] = std::byte{0xa1};
      desktop_spki[index] = std::byte{0xd2};
    }
    const auto phone_commit = openlens::pairing::commitment(attempt, openlens::pairing::Role::Phone,
                                                            phone_nonce, phone_spki, desktop_spki);
    const auto desktop_commit = openlens::pairing::commitment(
        attempt, openlens::pairing::Role::Desktop, desktop_nonce, phone_spki, desktop_spki);
    const auto pairing_transcript =
        openlens::pairing::transcript(attempt, phone_spki, desktop_spki, phone_commit,
                                      desktop_commit, phone_nonce, desktop_nonce);
    require(openlens::pairing::six_digit_sas(pairing_transcript) == "141454",
            "pairing SAS differs from shared vector");
    require(openlens::pairing::constant_time_equal(phone_commit, phone_commit),
            "equal pairing digests differ");
    require(!openlens::pairing::constant_time_equal(phone_commit, desktop_commit),
            "different pairing digests matched");

    std::array<char, 64> temporary{};
    const std::string pattern =
        (std::filesystem::temp_directory_path() / "openlens-identity-XXXXXX").string();
    std::copy(pattern.begin(), pattern.end(), temporary.begin());
    char* directory = ::mkdtemp(temporary.data());
    require(directory != nullptr, "temporary identity directory was not created");
    {
      openlens::WifiIdentityStore identity(directory);
      identity.ensure_identity();
      const auto local_pin = identity.local_spki_pin();
      require(std::any_of(local_pin.begin(), local_pin.end(),
                          [](std::byte value) { return value != std::byte{0}; }),
              "desktop identity pin is empty");
      const openlens::WifiPeer peer{"test-phone", "OpenLens phone", phone_spki};
      identity.save_peer(peer);
      const auto loaded = identity.peer("test-phone");
      require(loaded.has_value(), "saved Wi-Fi peer was not loaded");
      require(openlens::pairing::constant_time_equal(loaded->spki_pin, phone_spki),
              "saved Wi-Fi peer pin differs");
      identity.forget_peer("test-phone");
      require(!identity.peer("test-phone"), "forgotten Wi-Fi peer was retained");
    }
    std::filesystem::remove_all(directory);
    std::cout << "core_tests: all tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "core_tests: " << error.what() << '\n';
    return 1;
  }
}
