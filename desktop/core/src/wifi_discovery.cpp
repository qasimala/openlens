// SPDX-License-Identifier: GPL-2.0-or-later
#include "openlens/wifi_discovery.hpp"

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/address.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/strlst.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>

namespace openlens {
namespace {

struct Context {
  AvahiClient* client{};
  std::vector<WifiDevice> devices;
};

[[nodiscard]] std::string txt_value(AvahiStringList* txt, const char* key) {
  AvahiStringList* match = avahi_string_list_find(txt, key);
  if (match == nullptr)
    return {};
  char* value = nullptr;
  std::size_t size = 0;
  if (avahi_string_list_get_pair(match, nullptr, &value, &size) != 0 || value == nullptr)
    return {};
  std::string result(value, size);
  avahi_free(value);
  return result;
}

void resolve_callback(AvahiServiceResolver* resolver, AvahiIfIndex interface_index, AvahiProtocol,
                      AvahiResolverEvent event, const char* name, const char*, const char*,
                      const char*, const AvahiAddress* address, std::uint16_t port,
                      AvahiStringList* txt, AvahiLookupResultFlags, void* userdata) {
  auto* context = static_cast<Context*>(userdata);
  if (event == AVAHI_RESOLVER_FOUND && address != nullptr && txt_value(txt, "v") == "2" &&
      txt_value(txt, "tls") == "1") {
    std::array<char, AVAHI_ADDRESS_STR_MAX> buffer{};
    avahi_address_snprint(buffer.data(), buffer.size(), address);
    WifiDevice device{
        .service_name = name != nullptr ? name : "OpenLens phone",
        .address = buffer.data(),
        .device_id = txt_value(txt, "id"),
        .port = port,
        .interface_index = interface_index,
        .pairing = txt_value(txt, "pair") == "1",
        .busy = txt_value(txt, "busy") == "1",
    };
    if (!device.device_id.empty()) {
      const auto existing = std::find_if(
          context->devices.begin(), context->devices.end(),
          [&](const WifiDevice& value) { return value.device_id == device.device_id; });
      if (existing == context->devices.end()) {
        context->devices.push_back(std::move(device));
      } else if (existing->address.find(':') != std::string::npos &&
                 device.address.find(':') == std::string::npos) {
        *existing = std::move(device);
      }
    }
  }
  avahi_service_resolver_free(resolver);
}

void browse_callback(AvahiServiceBrowser*, AvahiIfIndex interface_index, AvahiProtocol protocol,
                     AvahiBrowserEvent event, const char* name, const char* type,
                     const char* domain, AvahiLookupResultFlags, void* userdata) {
  auto* context = static_cast<Context*>(userdata);
  if (event == AVAHI_BROWSER_NEW) {
    static_cast<void>(avahi_service_resolver_new(context->client, interface_index, protocol, name,
                                                 type, domain, AVAHI_PROTO_UNSPEC, {},
                                                 resolve_callback, context));
  }
}

} // namespace

std::vector<WifiDevice> discover_wifi_devices(std::chrono::milliseconds duration) {
  using Poll = std::unique_ptr<AvahiSimplePoll, decltype(&avahi_simple_poll_free)>;
  Poll poll(avahi_simple_poll_new(), avahi_simple_poll_free);
  if (!poll)
    throw std::runtime_error("could not create Avahi event loop");
  int error = 0;
  using Client = std::unique_ptr<AvahiClient, decltype(&avahi_client_free)>;
  Client client(avahi_client_new(avahi_simple_poll_get(poll.get()), {}, nullptr, nullptr, &error),
                avahi_client_free);
  if (!client)
    throw std::runtime_error(std::string("Avahi is unavailable: ") + avahi_strerror(error));
  Context context{.client = client.get(), .devices = {}};
  using Browser = std::unique_ptr<AvahiServiceBrowser, decltype(&avahi_service_browser_free)>;
  Browser browser(avahi_service_browser_new(client.get(), AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
                                            "_openlens._tcp", nullptr, {}, browse_callback,
                                            &context),
                  avahi_service_browser_free);
  if (!browser)
    throw std::runtime_error(std::string("could not browse OpenLens services: ") +
                             avahi_strerror(avahi_client_errno(client.get())));
  const auto deadline = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < deadline) {
    if (avahi_simple_poll_iterate(poll.get(), 100) != 0)
      break;
  }
  return context.devices;
}

} // namespace openlens
