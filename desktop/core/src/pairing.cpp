// SPDX-License-Identifier: GPL-2.0-or-later
#include "openlens/pairing.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace openlens::pairing {
namespace {

constexpr std::string_view kCommitDomain = "OpenLens Pair v2 commit";
constexpr std::string_view kTranscriptDomain = "OpenLens Pair v2 transcript";
constexpr std::string_view kSasDomain = "OpenLens Pair v2 sas";
constexpr std::string_view kSasRetryDomain = "OpenLens Pair v2 sas retry";
constexpr std::uint32_t kSasRange = 1'000'000U;
constexpr std::uint32_t kSasAcceptanceLimit = 16'000'000U;

void append(std::vector<std::byte>& output, std::string_view value) {
  output.insert(output.end(), reinterpret_cast<const std::byte*>(value.data()),
                reinterpret_cast<const std::byte*>(value.data() + value.size()));
}

void append(std::vector<std::byte>& output, std::span<const std::byte> value) {
  output.insert(output.end(), value.begin(), value.end());
}

[[nodiscard]] std::uint32_t first_24_bits(std::span<const std::byte, 32> digest) {
  return (std::to_integer<std::uint32_t>(digest[0]) << 16U) |
         (std::to_integer<std::uint32_t>(digest[1]) << 8U) |
         std::to_integer<std::uint32_t>(digest[2]);
}

} // namespace

Digest sha256(std::span<const std::byte> bytes) {
  using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
  Context context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(context.get(), bytes.data(), bytes.size()) != 1)
    throw std::runtime_error("could not initialize SHA-256");
  Digest result{};
  unsigned int length = 0;
  if (EVP_DigestFinal_ex(context.get(), reinterpret_cast<unsigned char*>(result.data()), &length) !=
          1 ||
      length != result.size())
    throw std::runtime_error("could not finish SHA-256");
  return result;
}

Digest commitment(std::span<const std::byte, 32> attempt, Role role,
                  std::span<const std::byte, 32> nonce, std::span<const std::byte, 32> phone_spki,
                  std::span<const std::byte, 32> desktop_spki) {
  std::vector<std::byte> input;
  input.reserve(kCommitDomain.size() + 129U);
  append(input, kCommitDomain);
  append(input, attempt);
  input.push_back(static_cast<std::byte>(role));
  append(input, nonce);
  append(input, phone_spki);
  append(input, desktop_spki);
  return sha256(input);
}

Digest transcript(std::span<const std::byte, 32> attempt, std::span<const std::byte, 32> phone_spki,
                  std::span<const std::byte, 32> desktop_spki,
                  std::span<const std::byte, 32> phone_commitment,
                  std::span<const std::byte, 32> desktop_commitment,
                  std::span<const std::byte, 32> phone_nonce,
                  std::span<const std::byte, 32> desktop_nonce) {
  std::vector<std::byte> input;
  input.reserve(kTranscriptDomain.size() + 224U);
  append(input, kTranscriptDomain);
  append(input, attempt);
  append(input, phone_spki);
  append(input, desktop_spki);
  append(input, phone_commitment);
  append(input, desktop_commitment);
  append(input, phone_nonce);
  append(input, desktop_nonce);
  return sha256(input);
}

std::string six_digit_sas(std::span<const std::byte, 32> transcript_hash) {
  std::vector<std::byte> input;
  input.reserve(kSasDomain.size() + transcript_hash.size());
  append(input, kSasDomain);
  append(input, transcript_hash);
  Digest material = sha256(input);
  std::uint32_t counter = 0;
  while (first_24_bits(material) >= kSasAcceptanceLimit) {
    std::vector<std::byte> retry;
    retry.reserve(kSasRetryDomain.size() + transcript_hash.size() + 4U);
    append(retry, kSasRetryDomain);
    append(retry, transcript_hash);
    ++counter;
    retry.push_back(static_cast<std::byte>((counter >> 24U) & 0xffU));
    retry.push_back(static_cast<std::byte>((counter >> 16U) & 0xffU));
    retry.push_back(static_cast<std::byte>((counter >> 8U) & 0xffU));
    retry.push_back(static_cast<std::byte>(counter & 0xffU));
    material = sha256(retry);
  }
  const std::uint32_t value = first_24_bits(material) % kSasRange;
  std::ostringstream output;
  output << std::setw(6) << std::setfill('0') << value;
  return output.str();
}

bool constant_time_equal(std::span<const std::byte> left,
                         std::span<const std::byte> right) noexcept {
  if (left.size() != right.size())
    return false;
  return CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}

} // namespace openlens::pairing
