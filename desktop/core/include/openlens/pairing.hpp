// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace openlens::pairing {

using Digest = std::array<std::byte, 32>;
enum class Role : std::uint8_t { Phone = 1, Desktop = 2 };

[[nodiscard]] Digest sha256(std::span<const std::byte> bytes);
[[nodiscard]] Digest commitment(std::span<const std::byte, 32> attempt, Role role,
                                std::span<const std::byte, 32> nonce,
                                std::span<const std::byte, 32> phone_spki,
                                std::span<const std::byte, 32> desktop_spki);
[[nodiscard]] Digest transcript(std::span<const std::byte, 32> attempt,
                                std::span<const std::byte, 32> phone_spki,
                                std::span<const std::byte, 32> desktop_spki,
                                std::span<const std::byte, 32> phone_commitment,
                                std::span<const std::byte, 32> desktop_commitment,
                                std::span<const std::byte, 32> phone_nonce,
                                std::span<const std::byte, 32> desktop_nonce);
[[nodiscard]] std::string six_digit_sas(std::span<const std::byte, 32> transcript_hash);
[[nodiscard]] bool constant_time_equal(std::span<const std::byte> left,
                                       std::span<const std::byte> right) noexcept;

} // namespace openlens::pairing
