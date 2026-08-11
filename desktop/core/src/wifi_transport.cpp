// SPDX-License-Identifier: GPL-2.0-or-later
#include "openlens/wifi_transport.hpp"

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <netdb.h>
#include <netinet/tcp.h>
#include <pwd.h>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace openlens {
namespace {

template <typename T, auto Free> using OpenSslPtr = std::unique_ptr<T, decltype(Free)>;
using SslContext = OpenSslPtr<SSL_CTX, SSL_CTX_free>;
using Ssl = OpenSslPtr<SSL, SSL_free>;
using Certificate = OpenSslPtr<X509, X509_free>;
using PublicKey = OpenSslPtr<EVP_PKEY, EVP_PKEY_free>;

[[nodiscard]] std::string openssl_error(std::string_view message) {
  const unsigned long code = ERR_get_error();
  std::array<char, 256> detail{};
  if (code != 0)
    ERR_error_string_n(code, detail.data(), detail.size());
  return std::string(message) + (code == 0 ? "" : ": " + std::string(detail.data()));
}

[[nodiscard]] std::string default_config_directory() {
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0')
    return std::string(xdg) + "/openlens";
  const passwd* user = ::getpwuid(::getuid());
  if (user == nullptr || user->pw_dir == nullptr)
    throw std::runtime_error("could not locate the user configuration directory");
  return std::string(user->pw_dir) + "/.config/openlens";
}

[[nodiscard]] std::string hex(std::span<const std::byte> bytes) {
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const auto byte : bytes)
    output << std::setw(2) << std::to_integer<unsigned int>(byte);
  return output.str();
}

[[nodiscard]] pairing::Digest unhex_digest(std::string_view value) {
  if (value.size() != 64)
    throw std::runtime_error("invalid saved OpenLens identity pin");
  pairing::Digest result{};
  for (std::size_t index = 0; index < result.size(); ++index) {
    unsigned int parsed = 0;
    const auto begin = value.data() + static_cast<std::ptrdiff_t>(index * 2U);
    const auto converted = std::from_chars(begin, begin + 2, parsed, 16);
    if (converted.ec != std::errc{} || converted.ptr != begin + 2)
      throw std::runtime_error("invalid saved OpenLens identity pin");
    result[index] = static_cast<std::byte>(parsed);
  }
  return result;
}

[[nodiscard]] pairing::Digest certificate_spki(X509* certificate) {
  PublicKey key(X509_get_pubkey(certificate), EVP_PKEY_free);
  if (!key)
    throw std::runtime_error("peer certificate has no public key");
  const int size = i2d_PUBKEY(key.get(), nullptr);
  if (size <= 0)
    throw std::runtime_error(openssl_error("could not encode peer public key"));
  std::vector<std::byte> encoded(static_cast<std::size_t>(size));
  auto* cursor = reinterpret_cast<unsigned char*>(encoded.data());
  if (i2d_PUBKEY(key.get(), &cursor) != size)
    throw std::runtime_error(openssl_error("could not encode peer public key"));
  return pairing::sha256(encoded);
}

void make_identity(const std::string& certificate_path, const std::string& key_path) {
  EVP_PKEY_CTX* raw_context = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
  OpenSslPtr<EVP_PKEY_CTX, EVP_PKEY_CTX_free> key_context(raw_context, EVP_PKEY_CTX_free);
  EVP_PKEY* raw_key = nullptr;
  if (!key_context || EVP_PKEY_keygen_init(key_context.get()) != 1 ||
      EVP_PKEY_CTX_set_ec_paramgen_curve_nid(key_context.get(), NID_X9_62_prime256v1) != 1 ||
      EVP_PKEY_keygen(key_context.get(), &raw_key) != 1)
    throw std::runtime_error(openssl_error("could not create the desktop identity key"));
  PublicKey key(raw_key, EVP_PKEY_free);
  Certificate certificate(X509_new(), X509_free);
  std::array<unsigned char, 20> serial{};
  if (RAND_bytes(serial.data(), serial.size()) != 1)
    throw std::runtime_error(openssl_error("could not create the identity serial"));
  serial[0] &= 0x7fU;
  serial.back() |= 0x01U;
  if (!certificate || X509_set_version(certificate.get(), 2) != 1 ||
      ASN1_STRING_set(X509_get_serialNumber(certificate.get()), serial.data(),
                      static_cast<int>(serial.size())) != 1 ||
      X509_gmtime_adj(X509_get_notBefore(certificate.get()), -60) == nullptr ||
      X509_gmtime_adj(X509_get_notAfter(certificate.get()), 60L * 60L * 24L * 365L * 25L) ==
          nullptr ||
      X509_set_pubkey(certificate.get(), key.get()) != 1)
    throw std::runtime_error(openssl_error("could not create the desktop identity certificate"));
  X509_NAME* name = X509_get_subject_name(certificate.get());
  constexpr std::string_view common_name = "OpenLens desktop";
  if (name == nullptr ||
      X509_NAME_add_entry_by_NID(name, NID_commonName, MBSTRING_ASC,
                                 reinterpret_cast<const unsigned char*>(common_name.data()),
                                 static_cast<int>(common_name.size()), -1, 0) != 1 ||
      X509_set_issuer_name(certificate.get(), name) != 1 ||
      X509_sign(certificate.get(), key.get(), EVP_sha256()) <= 0)
    throw std::runtime_error(openssl_error("could not sign the desktop identity certificate"));

  FILE* key_file = std::fopen(key_path.c_str(), "wx");
  if (key_file == nullptr)
    throw std::runtime_error("could not create the desktop identity key file");
  const bool key_written =
      PEM_write_PrivateKey(key_file, key.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1;
  std::fclose(key_file);
  ::chmod(key_path.c_str(), S_IRUSR | S_IWUSR);
  if (!key_written)
    throw std::runtime_error(openssl_error("could not save the desktop identity key"));
  FILE* certificate_file = std::fopen(certificate_path.c_str(), "wx");
  if (certificate_file == nullptr || PEM_write_X509(certificate_file, certificate.get()) != 1) {
    if (certificate_file != nullptr)
      std::fclose(certificate_file);
    throw std::runtime_error(openssl_error("could not save the desktop identity certificate"));
  }
  std::fclose(certificate_file);
  ::chmod(certificate_path.c_str(), S_IRUSR | S_IWUSR);
}

[[nodiscard]] int connect_tcp(const WifiDevice& device, std::chrono::milliseconds timeout) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* raw = nullptr;
  const std::string port = std::to_string(device.port);
  const int status = ::getaddrinfo(device.address.c_str(), port.c_str(), &hints, &raw);
  if (status != 0)
    throw std::runtime_error(std::string("could not resolve phone address: ") +
                             gai_strerror(status));
  std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(raw, freeaddrinfo);
  int connected = -1;
  for (addrinfo* candidate = addresses.get(); candidate != nullptr;
       candidate = candidate->ai_next) {
    if (candidate->ai_family == AF_INET6 && device.interface_index > 0) {
      auto* ipv6 = reinterpret_cast<sockaddr_in6*>(candidate->ai_addr);
      if (IN6_IS_ADDR_LINKLOCAL(&ipv6->sin6_addr))
        ipv6->sin6_scope_id = static_cast<std::uint32_t>(device.interface_index);
    }
    const int descriptor = ::socket(candidate->ai_family, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    if (descriptor < 0)
      continue;
    timeval value{static_cast<long>(timeout.count() / 1000),
                  static_cast<long>((timeout.count() % 1000) * 1000)};
    ::setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
    ::setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value));
    const int no_delay = 1;
    ::setsockopt(descriptor, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));
    if (::connect(descriptor, candidate->ai_addr, candidate->ai_addrlen) == 0) {
      connected = descriptor;
      break;
    }
    ::close(descriptor);
  }
  if (connected < 0)
    throw std::runtime_error("could not connect to the phone on the local network");
  return connected;
}

// Phone identities are self-signed; chain trust is intentionally waived here because
// authentication comes from SAS confirmation and SPKI pinning after the handshake.
[[nodiscard]] int accept_self_signed_peer(int, X509_STORE_CTX*) { return 1; }

[[nodiscard]] SslContext make_server_context(WifiIdentityStore& store) {
  store.ensure_identity();
  SslContext context(SSL_CTX_new(TLS_server_method()), SSL_CTX_free);
  if (!context || SSL_CTX_set_min_proto_version(context.get(), TLS1_3_VERSION) != 1 ||
      SSL_CTX_set_max_proto_version(context.get(), TLS1_3_VERSION) != 1 ||
      SSL_CTX_use_certificate_file(context.get(), store.certificate_path().c_str(),
                                   SSL_FILETYPE_PEM) != 1 ||
      SSL_CTX_use_PrivateKey_file(context.get(), store.private_key_path().c_str(),
                                  SSL_FILETYPE_PEM) != 1 ||
      SSL_CTX_check_private_key(context.get()) != 1)
    throw std::runtime_error(openssl_error("could not load the desktop TLS identity"));
  SSL_CTX_set_verify(context.get(), SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                     accept_self_signed_peer);
  return context;
}

struct ConnectedTls {
  int descriptor{-1};
  SslContext context{nullptr, SSL_CTX_free};
  Ssl ssl{nullptr, SSL_free};
  pairing::Digest peer_pin{};

  ConnectedTls() = default;
  ConnectedTls(ConnectedTls&& other) noexcept
      : descriptor(std::exchange(other.descriptor, -1)), context(std::move(other.context)),
        ssl(std::move(other.ssl)), peer_pin(other.peer_pin) {}
  ConnectedTls& operator=(ConnectedTls&& other) noexcept {
    if (this != &other) {
      ssl = std::move(other.ssl);
      context = std::move(other.context);
      if (descriptor >= 0)
        ::close(descriptor);
      descriptor = std::exchange(other.descriptor, -1);
      peer_pin = other.peer_pin;
    }
    return *this;
  }
  ~ConnectedTls() {
    if (descriptor >= 0)
      ::close(descriptor);
  }
};

// The desktop opens the transport connection but acts as the TLS server on it: Android's
// Conscrypt server mode is unreliable on some devices while its client mode is not.
// Takes ownership of the descriptor, which may be a TCP socket or any other
// stream-like descriptor (such as the USB accessory bridge).
[[nodiscard]] ConnectedTls tls_from_descriptor(int descriptor, WifiIdentityStore& store) {
  ConnectedTls connection;
  connection.context = make_server_context(store);
  connection.descriptor = descriptor;
  connection.ssl.reset(SSL_new(connection.context.get()));
  if (!connection.ssl || SSL_set_fd(connection.ssl.get(), connection.descriptor) != 1 ||
      SSL_accept(connection.ssl.get()) != 1)
    throw std::runtime_error(openssl_error("secure connection to the phone failed"));
  Certificate peer(SSL_get1_peer_certificate(connection.ssl.get()), X509_free);
  if (!peer)
    throw std::runtime_error("phone did not present an identity certificate");
  connection.peer_pin = certificate_spki(peer.get());
  return connection;
}

[[nodiscard]] ConnectedTls connect_tls(const WifiDevice& device, WifiIdentityStore& store,
                                       std::chrono::milliseconds timeout) {
  return tls_from_descriptor(connect_tcp(device, timeout), store);
}

void ssl_write_all(SSL* ssl, std::string_view text) {
  std::size_t offset = 0;
  while (offset < text.size()) {
    const int count = SSL_write(ssl, text.data() + offset, static_cast<int>(text.size() - offset));
    if (count <= 0)
      throw std::runtime_error(openssl_error("secure phone connection closed while sending"));
    offset += static_cast<std::size_t>(count);
  }
}

[[nodiscard]] std::string ssl_read_line(SSL* ssl, std::size_t limit = 4096) {
  std::string line;
  while (line.size() < limit) {
    char character = '\0';
    const int count = SSL_read(ssl, &character, 1);
    if (count <= 0)
      throw std::runtime_error(openssl_error("secure phone connection closed during pairing"));
    if (character == '\n')
      return line;
    if (character != '\r')
      line.push_back(character);
  }
  throw std::runtime_error("phone sent an oversized pairing message");
}

[[nodiscard]] std::vector<std::string> words(const std::string& line) {
  std::istringstream input(line);
  std::vector<std::string> result;
  for (std::string word; input >> word;)
    result.push_back(std::move(word));
  return result;
}

[[nodiscard]] pairing::Digest random_digest() {
  pairing::Digest result{};
  if (RAND_bytes(reinterpret_cast<unsigned char*>(result.data()), result.size()) != 1)
    throw std::runtime_error(openssl_error("could not generate pairing randomness"));
  return result;
}

[[nodiscard]] std::string safe_field(std::string value) {
  for (char& character : value)
    if (character == '\t' || character == '\n' || character == '\r')
      character = ' ';
  return value;
}

} // namespace

WifiIdentityStore::WifiIdentityStore() : WifiIdentityStore(default_config_directory()) {}

WifiIdentityStore::WifiIdentityStore(std::string directory)
    : directory_(std::move(directory)), certificate_path_(directory_ + "/identity-cert.pem"),
      private_key_path_(directory_ + "/identity-key.pem"), peers_path_(directory_ + "/peers.tsv") {}

void WifiIdentityStore::ensure_identity() {
  std::filesystem::create_directories(directory_);
  ::chmod(directory_.c_str(), S_IRWXU);
  const bool certificate_exists = std::filesystem::exists(certificate_path_);
  const bool key_exists = std::filesystem::exists(private_key_path_);
  if (certificate_exists && key_exists)
    return;
  if (certificate_exists != key_exists)
    throw std::runtime_error("OpenLens desktop identity is incomplete; remove it and pair again");
  make_identity(certificate_path_, private_key_path_);
}

const std::string& WifiIdentityStore::certificate_path() const noexcept {
  return certificate_path_;
}
const std::string& WifiIdentityStore::private_key_path() const noexcept {
  return private_key_path_;
}

pairing::Digest WifiIdentityStore::local_spki_pin() {
  ensure_identity();
  FILE* file = std::fopen(certificate_path_.c_str(), "r");
  if (file == nullptr)
    throw std::runtime_error("could not open desktop identity certificate");
  Certificate certificate(PEM_read_X509(file, nullptr, nullptr, nullptr), X509_free);
  std::fclose(file);
  if (!certificate)
    throw std::runtime_error(openssl_error("could not read desktop identity certificate"));
  return certificate_spki(certificate.get());
}

std::vector<WifiPeer> WifiIdentityStore::peers() const {
  std::vector<WifiPeer> result;
  std::ifstream input(peers_path_);
  for (std::string line; std::getline(input, line);) {
    const auto first = line.find('\t');
    const auto second = first == std::string::npos ? first : line.find('\t', first + 1U);
    if (first == std::string::npos || second == std::string::npos)
      continue;
    try {
      result.push_back(WifiPeer{line.substr(0, first), line.substr(second + 1U),
                                unhex_digest(line.substr(first + 1U, second - first - 1U))});
    } catch (const std::exception&) {
    }
  }
  return result;
}

std::optional<WifiPeer> WifiIdentityStore::peer(std::string_view device_id) const {
  for (auto& record : peers())
    if (record.device_id == device_id)
      return record;
  return std::nullopt;
}

void WifiIdentityStore::save_peer(const WifiPeer& record) {
  ensure_identity();
  auto existing = peers();
  existing.erase(
      std::remove_if(existing.begin(), existing.end(),
                     [&](const WifiPeer& value) { return value.device_id == record.device_id; }),
      existing.end());
  existing.push_back(record);
  const std::string temporary = peers_path_ + ".new";
  std::ofstream output(temporary, std::ios::trunc);
  if (!output)
    throw std::runtime_error("could not save the paired phone");
  for (const auto& value : existing)
    output << safe_field(value.device_id) << '\t' << hex(value.spki_pin) << '\t'
           << safe_field(value.name) << '\n';
  output.close();
  ::chmod(temporary.c_str(), S_IRUSR | S_IWUSR);
  std::filesystem::rename(temporary, peers_path_);
}

void WifiIdentityStore::forget_peer(std::string_view device_id) {
  auto existing = peers();
  existing.erase(
      std::remove_if(existing.begin(), existing.end(),
                     [&](const WifiPeer& value) { return value.device_id == device_id; }),
      existing.end());
  const std::string temporary = peers_path_ + ".new";
  std::ofstream output(temporary, std::ios::trunc);
  for (const auto& value : existing)
    output << safe_field(value.device_id) << '\t' << hex(value.spki_pin) << '\t'
           << safe_field(value.name) << '\n';
  output.close();
  ::chmod(temporary.c_str(), S_IRUSR | S_IWUSR);
  std::filesystem::rename(temporary, peers_path_);
}

struct WifiStream::Impl {
  ConnectedTls connection;
};

WifiStream::WifiStream(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}
WifiStream::WifiStream(WifiStream&&) noexcept = default;
WifiStream& WifiStream::operator=(WifiStream&&) noexcept = default;
WifiStream::~WifiStream() { close(); }

std::ptrdiff_t WifiStream::read(std::span<std::byte> buffer) {
  if (!implementation_ || !implementation_->connection.ssl)
    return 0;
  const int count = SSL_read(implementation_->connection.ssl.get(), buffer.data(),
                             static_cast<int>(buffer.size()));
  if (count > 0)
    return count;
  const int error = SSL_get_error(implementation_->connection.ssl.get(), count);
  if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
    errno = EAGAIN;
    return -1;
  }
  return 0;
}

void WifiStream::write_all(std::span<const std::byte> bytes) {
  if (!implementation_ || !implementation_->connection.ssl)
    throw std::runtime_error("Wi-Fi connection is closed");
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const int count = SSL_write(implementation_->connection.ssl.get(), bytes.data() + offset,
                                static_cast<int>(bytes.size() - offset));
    if (count <= 0)
      throw std::runtime_error(openssl_error("secure phone connection closed while sending"));
    offset += static_cast<std::size_t>(count);
  }
}

void WifiStream::close() noexcept {
  if (implementation_ && implementation_->connection.ssl)
    SSL_shutdown(implementation_->connection.ssl.get());
  implementation_.reset();
}

namespace {

[[nodiscard]] PairingResult pair_over(ConnectedTls connection, const std::string& device_id,
                                      const std::string& device_name, WifiIdentityStore& store,
                                      const PairingConfirmation& confirm) {
  ssl_write_all(connection.ssl.get(), "PAIR 2\n");
  const auto first = words(ssl_read_line(connection.ssl.get()));
  if (first.size() != 4 || first[0] != "PAIR1")
    throw std::runtime_error("phone did not begin a valid OpenLens pairing exchange");
  const auto attempt = unhex_digest(first[1]);
  const auto phone_commitment = unhex_digest(first[2]);
  const auto claimed_phone_pin = unhex_digest(first[3]);
  if (!pairing::constant_time_equal(claimed_phone_pin, connection.peer_pin))
    throw std::runtime_error("phone pairing identity did not match its TLS certificate");
  const auto desktop_pin = store.local_spki_pin();
  const auto desktop_nonce = random_digest();
  const auto desktop_commitment = pairing::commitment(
      attempt, pairing::Role::Desktop, desktop_nonce, connection.peer_pin, desktop_pin);
  ssl_write_all(connection.ssl.get(),
                "PAIR2 " + hex(desktop_commitment) + " " + hex(desktop_pin) + "\n");
  const auto reveal = words(ssl_read_line(connection.ssl.get()));
  if (reveal.size() != 2 || reveal[0] != "PAIR3")
    throw std::runtime_error("phone sent an invalid pairing reveal");
  const auto phone_nonce = unhex_digest(reveal[1]);
  if (!pairing::constant_time_equal(pairing::commitment(attempt, pairing::Role::Phone, phone_nonce,
                                                        connection.peer_pin, desktop_pin),
                                    phone_commitment))
    throw std::runtime_error("phone pairing commitment verification failed");
  ssl_write_all(connection.ssl.get(), "PAIR4 " + hex(desktop_nonce) + "\n");
  const std::string sas = pairing::six_digit_sas(
      pairing::transcript(attempt, connection.peer_pin, desktop_pin, phone_commitment,
                          desktop_commitment, phone_nonce, desktop_nonce));
  if (!confirm(sas)) {
    ssl_write_all(connection.ssl.get(), "PAIR_CANCEL\n");
    throw std::runtime_error("pairing was cancelled on this computer");
  }
  ssl_write_all(connection.ssl.get(), "PAIR_CONFIRM\n");
  const auto result = words(ssl_read_line(connection.ssl.get()));
  if (result.size() != 1 || result[0] != "PAIR_OK")
    throw std::runtime_error("pairing was not confirmed on the phone");
  WifiPeer peer{device_id, device_name, connection.peer_pin};
  store.save_peer(peer);
  return PairingResult{peer, sas};
}

[[nodiscard]] WifiStream stream_over(ConnectedTls connection) {
  ssl_write_all(connection.ssl.get(), "OPENLENS 2\n");
  return WifiStream(std::make_unique<WifiStream::Impl>(WifiStream::Impl{std::move(connection)}));
}

} // namespace

PairingResult pair_wifi_device(const WifiDevice& device, WifiIdentityStore& store,
                               const PairingConfirmation& confirm,
                               std::chrono::milliseconds timeout) {
  return pair_over(connect_tls(device, store, timeout), device.device_id, device.service_name,
                   store, confirm);
}

WifiStream connect_wifi_stream(const WifiDevice& device, WifiIdentityStore& store,
                               std::chrono::milliseconds timeout) {
  const auto peer = store.peer(device.device_id);
  const auto peers = store.peers();
  if (!peer && peers.empty())
    throw std::runtime_error("this phone has not been paired with OpenLens yet");
  auto connection = connect_tls(device, store, timeout);
  if (peer) {
    if (!pairing::constant_time_equal(peer->spki_pin, connection.peer_pin))
      throw std::runtime_error("the phone identity changed; forget it and pair again");
  } else {
    // The phone may have been paired over another transport (USB) under a
    // different record id; its pinned TLS identity is what authenticates it.
    const bool trusted = std::any_of(peers.begin(), peers.end(), [&](const WifiPeer& record) {
      return pairing::constant_time_equal(record.spki_pin, connection.peer_pin);
    });
    if (!trusted)
      throw std::runtime_error("this phone has not been paired with OpenLens yet");
  }
  return stream_over(std::move(connection));
}

PairingResult pair_connected_descriptor(int descriptor, const std::string& device_id,
                                        const std::string& device_name, WifiIdentityStore& store,
                                        const PairingConfirmation& confirm) {
  return pair_over(tls_from_descriptor(descriptor, store), device_id, device_name, store, confirm);
}

WifiStream connect_stream_descriptor(int descriptor, WifiIdentityStore& store) {
  auto connection = tls_from_descriptor(descriptor, store);
  // Descriptor transports (USB) carry no advertised device id, so the phone is
  // recognised purely by its pinned TLS identity.
  const auto peers = store.peers();
  const bool trusted = std::any_of(peers.begin(), peers.end(), [&](const WifiPeer& peer) {
    return pairing::constant_time_equal(peer.spki_pin, connection.peer_pin);
  });
  if (!trusted)
    throw std::runtime_error("this phone has not been paired with OpenLens yet");
  return stream_over(std::move(connection));
}

} // namespace openlens
