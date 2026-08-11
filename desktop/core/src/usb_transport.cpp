// SPDX-License-Identifier: GPL-2.0-or-later
#include "openlens/usb_transport.hpp"

#include <libusb.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace openlens {
namespace {

// Android Open Accessory protocol constants.
constexpr std::uint16_t google_vendor = 0x18d1;
constexpr std::uint16_t accessory_products[] = {0x2d00, 0x2d01, 0x2d04, 0x2d05};
constexpr std::uint8_t request_get_protocol = 51;
constexpr std::uint8_t request_send_string = 52;
constexpr std::uint8_t request_start = 53;
constexpr unsigned control_timeout_ms = 500;
constexpr unsigned bulk_timeout_ms = 250;

// These strings must match the accessory filter in the Android app manifest.
constexpr const char* accessory_strings[] = {
    "OpenLens",              // manufacturer
    "OpenLens desktop",      // model
    "OpenLens camera link",  // description
    "1",                     // version
    "https://openlens.dev",  // URI
    "openlens",              // serial
};

using Context = std::unique_ptr<libusb_context, decltype(&libusb_exit)>;
using Handle = std::unique_ptr<libusb_device_handle, decltype(&libusb_close)>;

[[nodiscard]] Context make_context() {
  libusb_context* raw = nullptr;
  if (libusb_init(&raw) != 0)
    throw std::runtime_error("could not initialise USB support");
  return Context(raw, libusb_exit);
}

[[nodiscard]] bool is_accessory_product(std::uint16_t product) {
  for (const auto candidate : accessory_products)
    if (candidate == product)
      return true;
  return false;
}

[[nodiscard]] std::string read_string(libusb_device_handle* handle, std::uint8_t index) {
  if (index == 0)
    return {};
  std::array<unsigned char, 128> buffer{};
  const int count =
      libusb_get_string_descriptor_ascii(handle, index, buffer.data(), buffer.size());
  return count > 0 ? std::string(reinterpret_cast<char*>(buffer.data()),
                                 static_cast<std::size_t>(count))
                   : std::string();
}

// Answers the accessory protocol version, or 0 when the device is not an
// Android phone with accessory support.
[[nodiscard]] std::uint16_t probe_protocol(libusb_device_handle* handle) {
  std::array<unsigned char, 2> version{};
  const int count = libusb_control_transfer(
      handle, static_cast<std::uint8_t>(LIBUSB_ENDPOINT_IN) | static_cast<std::uint8_t>(LIBUSB_REQUEST_TYPE_VENDOR), request_get_protocol, 0, 0,
      version.data(), version.size(), control_timeout_ms);
  if (count != 2)
    return 0;
  return static_cast<std::uint16_t>(version[0] | (version[1] << 8));
}

void start_accessory_mode(libusb_device_handle* handle) {
  for (std::uint16_t index = 0; index < 6; ++index) {
    const char* text = accessory_strings[index];
    const int status = libusb_control_transfer(
        handle, static_cast<std::uint8_t>(LIBUSB_ENDPOINT_OUT) | static_cast<std::uint8_t>(LIBUSB_REQUEST_TYPE_VENDOR), request_send_string, 0, index,
        reinterpret_cast<unsigned char*>(const_cast<char*>(text)),
        static_cast<std::uint16_t>(std::strlen(text) + 1), control_timeout_ms);
    if (status < 0)
      throw std::runtime_error("could not describe OpenLens to the phone over USB");
  }
  if (libusb_control_transfer(handle, static_cast<std::uint8_t>(LIBUSB_ENDPOINT_OUT) | static_cast<std::uint8_t>(LIBUSB_REQUEST_TYPE_VENDOR),
                              request_start, 0, 0, nullptr, 0, control_timeout_ms) < 0)
    throw std::runtime_error("could not switch the phone into USB accessory mode");
}

struct AccessoryEndpoints {
  std::uint8_t in{};
  std::uint8_t out{};
  int interface_number{-1};
};

[[nodiscard]] AccessoryEndpoints find_bulk_endpoints(libusb_device* device) {
  AccessoryEndpoints result;
  libusb_config_descriptor* raw = nullptr;
  if (libusb_get_active_config_descriptor(device, &raw) != 0 || raw == nullptr)
    throw std::runtime_error("could not inspect the phone USB configuration");
  std::unique_ptr<libusb_config_descriptor, decltype(&libusb_free_config_descriptor)> config(
      raw, libusb_free_config_descriptor);
  for (int interface_index = 0; interface_index < config->bNumInterfaces; ++interface_index) {
    const libusb_interface_descriptor& description =
        config->interface[interface_index].altsetting[0];
    std::uint8_t in = 0;
    std::uint8_t out = 0;
    for (int endpoint = 0; endpoint < description.bNumEndpoints; ++endpoint) {
      const libusb_endpoint_descriptor& value = description.endpoint[endpoint];
      if ((value.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK)
        continue;
      if ((value.bEndpointAddress & LIBUSB_ENDPOINT_IN) != 0)
        in = value.bEndpointAddress;
      else
        out = value.bEndpointAddress;
    }
    if (in != 0 && out != 0) {
      result.in = in;
      result.out = out;
      result.interface_number = description.bInterfaceNumber;
      return result;
    }
  }
  throw std::runtime_error("the phone USB accessory endpoints were not found");
}

struct FoundDevice {
  Handle handle{nullptr, libusb_close};
  libusb_device* device{};
  std::string serial;
  std::string product;
  bool accessory_mode{};
};

// Locates a phone: prefers a device already in accessory mode, otherwise
// probes for accessory support without switching modes.
[[nodiscard]] std::vector<FoundDevice> scan(libusb_context* context, bool probe_candidates) {
  std::vector<FoundDevice> result;
  libusb_device** list = nullptr;
  const ssize_t count = libusb_get_device_list(context, &list);
  if (count < 0)
    return result;
  for (ssize_t index = 0; index < count; ++index) {
    libusb_device* device = list[index];
    libusb_device_descriptor descriptor{};
    if (libusb_get_device_descriptor(device, &descriptor) != 0)
      continue;
    if (descriptor.bDeviceClass == LIBUSB_CLASS_HUB)
      continue;
    const bool accessory =
        descriptor.idVendor == google_vendor && is_accessory_product(descriptor.idProduct);
    if (!accessory && !probe_candidates)
      continue;
    libusb_device_handle* raw = nullptr;
    if (libusb_open(device, &raw) != 0)
      continue;
    Handle handle(raw, libusb_close);
    if (!accessory && probe_protocol(handle.get()) < 1)
      continue;
    FoundDevice found;
    found.serial = read_string(handle.get(), descriptor.iSerialNumber);
    found.product = read_string(handle.get(), descriptor.iProduct);
    found.accessory_mode = accessory;
    found.device = device;
    found.handle = std::move(handle);
    result.push_back(std::move(found));
  }
  libusb_free_device_list(list, 1);
  return result;
}

// Both ends of the accessory pipe can hold stale bytes from an earlier
// connection (an abandoned TLS handshake, tail of a video stream). Before TLS
// starts, the sides resynchronise: the desktop repeats SYN ("OLNS" + nonce)
// until the phone echoes it back ("OLNE" + nonce), then answers GO
// ("OLGO" + nonce). Each side discards everything before its expected token,
// which deterministically flushes stale data in both directions.
void synchronise_link(libusb_device_handle* handle, const AccessoryEndpoints& endpoints,
                      std::chrono::milliseconds timeout) {
  std::array<unsigned char, 12> syn{'O', 'L', 'N', 'S'};
  std::random_device random;
  for (std::size_t index = 4; index < syn.size(); ++index)
    syn[index] = static_cast<unsigned char>(random());
  std::array<unsigned char, 12> expected_echo{'O', 'L', 'N', 'E'};
  std::copy(syn.begin() + 4, syn.end(), expected_echo.begin() + 4);
  std::array<unsigned char, 12> go{'O', 'L', 'G', 'O'};
  std::copy(syn.begin() + 4, syn.end(), go.begin() + 4);

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  // Not time_point::min(): subtracting it from now overflows the duration.
  auto last_sent = std::chrono::steady_clock::now() - std::chrono::hours(1);
  std::array<unsigned char, 12> tail{};
  std::array<unsigned char, 4096> buffer{};
  while (std::chrono::steady_clock::now() < deadline) {
    if (std::chrono::steady_clock::now() - last_sent >= std::chrono::seconds(1)) {
      int transferred = 0;
      const int sent_status = libusb_bulk_transfer(
          handle, endpoints.out, syn.data(), static_cast<int>(syn.size()), &transferred, 1000);
      if (sent_status != 0 && sent_status != LIBUSB_ERROR_TIMEOUT)
        throw std::runtime_error("could not reach the phone over the USB accessory link");
      last_sent = std::chrono::steady_clock::now();
    }
    int transferred = 0;
    const int status = libusb_bulk_transfer(handle, endpoints.in, buffer.data(),
                                            static_cast<int>(buffer.size()), &transferred, 200);
    if (status != 0 && status != LIBUSB_ERROR_TIMEOUT)
      throw std::runtime_error("the USB accessory link failed while connecting");
    for (int index = 0; index < transferred; ++index) {
      std::copy(tail.begin() + 1, tail.end(), tail.begin());
      tail.back() = buffer[static_cast<std::size_t>(index)];
      if (tail == expected_echo) {
        int sent = 0;
        if (libusb_bulk_transfer(handle, endpoints.out, go.data(), static_cast<int>(go.size()),
                                 &sent, 1000) != 0)
          throw std::runtime_error("could not confirm the USB connection to the phone");
        return;
      }
    }
  }
  throw std::runtime_error(
      "the phone did not answer over USB; make sure the OpenLens app is open");
}

void write_all_fd(int descriptor, const unsigned char* data, std::size_t size) {
  std::size_t offset = 0;
  while (offset < size) {
    const ssize_t count = ::write(descriptor, data + offset, size - offset);
    if (count <= 0) {
      if (count < 0 && errno == EINTR)
        continue;
      throw std::runtime_error("bridge closed");
    }
    offset += static_cast<std::size_t>(count);
  }
}

} // namespace

std::vector<UsbPhone> list_usb_phones() {
  const auto context = make_context();
  std::vector<UsbPhone> result;
  for (const auto& found : scan(context.get(), true))
    result.push_back(UsbPhone{found.serial, found.product, found.accessory_mode});
  return result;
}

struct UsbAccessoryLink::Impl {
  Context context{nullptr, libusb_exit};
  Handle handle{nullptr, libusb_close};
  AccessoryEndpoints endpoints;
  std::string serial;
  std::string product;
  int bridge_descriptor{-1};
  int stream_descriptor{-1};
  std::atomic_bool stopping{false};
  std::thread usb_to_socket;
  std::thread socket_to_usb;

  void pump_usb_to_socket() {
    std::array<unsigned char, 16384> buffer{};
    while (!stopping.load()) {
      int transferred = 0;
      const int status =
          libusb_bulk_transfer(handle.get(), endpoints.in, buffer.data(),
                               static_cast<int>(buffer.size()), &transferred, bulk_timeout_ms);
      if (status == LIBUSB_ERROR_TIMEOUT)
        continue;
      if (status != 0)
        break;
      try {
        if (transferred > 0)
          write_all_fd(bridge_descriptor, buffer.data(), static_cast<std::size_t>(transferred));
      } catch (const std::exception&) {
        break;
      }
    }
    ::shutdown(bridge_descriptor, SHUT_RDWR);
  }

  void pump_socket_to_usb() {
    std::array<unsigned char, 16384> buffer{};
    while (!stopping.load()) {
      const ssize_t count = ::read(bridge_descriptor, buffer.data(), buffer.size());
      if (count < 0 && errno == EINTR)
        continue;
      if (count <= 0)
        break;
      std::size_t offset = 0;
      bool failed = false;
      while (offset < static_cast<std::size_t>(count)) {
        int transferred = 0;
        const int status = libusb_bulk_transfer(
            handle.get(), endpoints.out, buffer.data() + offset,
            static_cast<int>(static_cast<std::size_t>(count) - offset), &transferred, 0);
        if (status != 0) {
          failed = true;
          break;
        }
        offset += static_cast<std::size_t>(transferred);
      }
      if (failed)
        break;
    }
    ::shutdown(bridge_descriptor, SHUT_RDWR);
  }

  ~Impl() {
    stopping.store(true);
    if (bridge_descriptor >= 0)
      ::shutdown(bridge_descriptor, SHUT_RDWR);
    if (usb_to_socket.joinable())
      usb_to_socket.join();
    if (socket_to_usb.joinable())
      socket_to_usb.join();
    if (bridge_descriptor >= 0)
      ::close(bridge_descriptor);
    if (stream_descriptor >= 0)
      ::close(stream_descriptor);
    if (handle && endpoints.interface_number >= 0)
      libusb_release_interface(handle.get(), endpoints.interface_number);
  }
};

UsbAccessoryLink UsbAccessoryLink::open(std::chrono::milliseconds timeout) {
  auto implementation = std::make_unique<Impl>();
  implementation->context = make_context();
  libusb_context* context = implementation->context.get();

  const auto deadline = std::chrono::steady_clock::now() + timeout;

  // Claims the accessory interface and synchronises with the phone app.
  const auto establish = [&implementation](FoundDevice& found,
                                           std::chrono::milliseconds sync_timeout) {
    implementation->endpoints = find_bulk_endpoints(found.device);
    libusb_set_auto_detach_kernel_driver(found.handle.get(), 1);
    if (libusb_claim_interface(found.handle.get(), implementation->endpoints.interface_number) !=
        0)
      throw std::runtime_error("could not claim the phone USB accessory interface; "
                               "check the udev permissions for accessory devices");
    try {
      synchronise_link(found.handle.get(), implementation->endpoints, sync_timeout);
    } catch (const std::exception&) {
      libusb_release_interface(found.handle.get(), implementation->endpoints.interface_number);
      throw;
    }
    implementation->serial = found.serial;
    implementation->product = found.product.empty() ? "Android phone" : found.product;
    implementation->handle = std::move(found.handle);
  };

  auto devices = scan(context, false);
  bool established = false;
  if (!devices.empty()) {
    // The phone may still be serving the accessory session from an earlier
    // connection; the sync exchange tells us quickly whether it is alive.
    try {
      establish(devices.front(), std::chrono::seconds(4));
      established = true;
    } catch (const std::exception&) {
      // Dead session (for example the app was restarted, which closes its
      // accessory descriptor for good): reset and switch modes freshly.
      if (devices.front().handle)
        libusb_reset_device(devices.front().handle.get());
      devices.clear();
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
  }
  if (!established) {
    // Probe for an accessory-capable phone and switch it. Some phones silently
    // ignore a switch request (particularly right after a previous accessory
    // session), so keep re-sending it until the accessory identity appears.
    std::string switched_serial;
    bool ever_switched = false;
    // Not time_point::min(): subtracting it from now overflows the duration.
    auto last_switch = std::chrono::steady_clock::now() - std::chrono::hours(1);
    while (std::chrono::steady_clock::now() < deadline) {
      devices = scan(context, false);
      if (!devices.empty())
        break;
      if (std::chrono::steady_clock::now() - last_switch >= std::chrono::seconds(4)) {
        auto candidates = scan(context, true);
        if (!candidates.empty()) {
          switched_serial = candidates.front().serial;
          try {
            start_accessory_mode(candidates.front().handle.get());
            ever_switched = true;
          } catch (const std::exception&) {
          }
          last_switch = std::chrono::steady_clock::now();
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (devices.empty())
      throw std::runtime_error(
          ever_switched
              ? "the phone did not enter USB accessory mode; make sure the OpenLens app is "
                "installed and the phone is unlocked"
              : "no phone was found over USB; connect the cable and open the OpenLens app");
    if (devices.front().serial.empty())
      devices.front().serial = switched_serial;
    establish(devices.front(), std::chrono::seconds(15));
  }

  int pair[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0)
    throw std::runtime_error("could not create the USB bridge sockets");
  implementation->bridge_descriptor = pair[0];
  implementation->stream_descriptor = pair[1];
  Impl* raw = implementation.get();
  implementation->usb_to_socket = std::thread([raw] { raw->pump_usb_to_socket(); });
  implementation->socket_to_usb = std::thread([raw] { raw->pump_socket_to_usb(); });
  return UsbAccessoryLink(std::move(implementation));
}

UsbAccessoryLink::UsbAccessoryLink(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}
UsbAccessoryLink::UsbAccessoryLink(UsbAccessoryLink&&) noexcept = default;
UsbAccessoryLink& UsbAccessoryLink::operator=(UsbAccessoryLink&&) noexcept = default;
UsbAccessoryLink::~UsbAccessoryLink() = default;

const std::string& UsbAccessoryLink::serial() const noexcept { return implementation_->serial; }
const std::string& UsbAccessoryLink::product() const noexcept { return implementation_->product; }

int UsbAccessoryLink::release_descriptor() {
  const int descriptor = implementation_->stream_descriptor;
  implementation_->stream_descriptor = -1;
  return descriptor;
}

} // namespace openlens
