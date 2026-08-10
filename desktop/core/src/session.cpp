// SPDX-License-Identifier: GPL-2.0-or-later
#include "openlens/session.hpp"

#include "openlens/media.hpp"
#include "openlens/protocol.hpp"
#include "openlens/wifi_discovery.hpp"
#include "openlens/wifi_transport.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <thread>

namespace openlens {
namespace {

class ActiveConnection {
public:
  std::optional<WifiStream> wifi;

  [[nodiscard]] std::ptrdiff_t read(std::span<std::byte> buffer) {
    if (wifi)
      return wifi->read(buffer);
    return 0;
  }

  void send(const protocol::Message& message) {
    if (wifi) {
      const auto bytes = protocol::serialize(message);
      wifi->write_all(bytes);
    } else
      throw std::runtime_error("Wi-Fi connection is closed");
  }

  void close() noexcept {
    if (wifi) {
      wifi->close();
      wifi.reset();
    }
  }
};

protocol::Message metadata(protocol::MessageType type, std::uint64_t sequence, std::string json) {
  protocol::Message message;
  message.header.type = static_cast<std::uint16_t>(type);
  message.header.flags = static_cast<std::uint16_t>(protocol::Flag::Required);
  message.header.sequence = sequence;
  message.payload.resize(json.size());
  std::memcpy(message.payload.data(), json.data(), json.size());
  return message;
}

} // namespace

OpenLensSession::OpenLensSession(SessionOptions options) : options_(std::move(options)) {}

SessionStats OpenLensSession::run(FrameSink& sink, std::atomic_bool& cancelled) {
  auto wifi_store = std::make_unique<WifiIdentityStore>();
  if (!options_.wifi_device) {
    const auto devices = discover_wifi_devices(std::chrono::milliseconds(1500));
    for (const auto& device : devices) {
      if (!wifi_store->peer(device.device_id))
        continue;
      if (options_.wifi_device)
        throw std::runtime_error("multiple paired phones are available; select one in OpenLens");
      options_.wifi_device = device;
    }
    if (!options_.wifi_device)
      throw std::runtime_error("no paired OpenLens phone was found on local Wi-Fi");
  }
  ActiveConnection connection;
  connection.wifi.emplace(connect_wifi_stream(*options_.wifi_device, *wifi_store));

  std::ofstream encoded;
  if (!options_.encoded_output.empty())
    encoded.open(options_.encoded_output, std::ios::binary);
  std::uint64_t desktop_sequence = 1;
  connection.send(
      metadata(protocol::MessageType::Hello, desktop_sequence++,
               R"({"schema":1,"client":"desktop","version":"0.2.0","transport":"wifi"})"));

  protocol::StreamParser parser;
  H264Decoder decoder;
  SessionStats stats;
  std::array<std::byte, 64U * 1024U> buffer{};
  std::uint64_t expected_sequence = 0;
  bool configured = false;
  bool controls_sent = false;
  const auto started = std::chrono::steady_clock::now();
  auto last_receive = started;
  while (!cancelled) {
    if (options_.duration.count() > 0 &&
        std::chrono::steady_clock::now() - started >= options_.duration)
      break;
    const std::ptrdiff_t count = connection.read(buffer);
    bool disconnected = count == 0;
    if (count < 0) {
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        if (std::chrono::steady_clock::now() - last_receive < std::chrono::seconds(5))
          continue;
        disconnected = true;
      } else {
        disconnected = true;
      }
    }
    if (disconnected) {
      connection.close();
      sink.placeholder("Phone disconnected—waiting for the same device");
      sink.flush();
      decoder.flush();
      parser.reset();
      configured = false;
      controls_sent = false;
      const auto reconnect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
      bool ready = false;
      while (!cancelled && std::chrono::steady_clock::now() < reconnect_deadline) {
        try {
          const auto devices = discover_wifi_devices(std::chrono::milliseconds(700));
          const auto wanted =
              std::find_if(devices.begin(), devices.end(), [&](const WifiDevice& device) {
                return device.device_id == options_.wifi_device->device_id;
              });
          if (wanted != devices.end()) {
            options_.wifi_device = *wanted;
            connection.wifi.emplace(connect_wifi_stream(*wanted, *wifi_store));
            ready = true;
            last_receive = std::chrono::steady_clock::now();
          }
          if (ready)
            connection.send(metadata(protocol::MessageType::Hello, desktop_sequence++,
                                     R"({"schema":1,"client":"desktop","version":"0.2.0"})"));
        } catch (const std::exception&) {
        }
        if (ready)
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      }
      if (!ready)
        throw std::runtime_error("the selected phone did not reconnect within 60 seconds");
      continue;
    }
    last_receive = std::chrono::steady_clock::now();
    stats.bytes += static_cast<std::uint64_t>(count);
    parser.feed(std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(count)));
    for (;;) {
      auto result = parser.next();
      if (result.status == protocol::ParseStatus::NeedMoreData)
        break;
      if (result.status == protocol::ParseStatus::Invalid || !result.message) {
        throw std::runtime_error("invalid phone protocol message: " + result.error);
      }
      const auto& message = *result.message;
      ++stats.messages;
      if (expected_sequence != 0 && message.header.sequence > expected_sequence) {
        stats.sequence_gaps += message.header.sequence - expected_sequence;
      }
      expected_sequence = message.header.sequence + 1U;
      const auto type = static_cast<protocol::MessageType>(message.header.type);
      if (type == protocol::MessageType::HelloAck && !configured) {
        const int width = options_.preset == "720p30" ? 1280 : 1920;
        const int height = options_.preset == "720p30" ? 720 : 1080;
        const int default_bitrate = options_.preset == "720p30" ? 4000000 : 8000000;
        const int bitrate = options_.bitrate > 0 ? options_.bitrate : default_bitrate;
        const std::string json = "{\"schema\":1,\"width\":" + std::to_string(width) +
                                 ",\"height\":" + std::to_string(height) +
                                 ",\"fps\":30,\"bitrate\":" + std::to_string(bitrate) +
                                 ",\"facing\":\"" + options_.facing + "\"}";
        connection.send(metadata(protocol::MessageType::Configure, desktop_sequence++, json));
        configured = true;
      } else if (type == protocol::MessageType::Configured && !controls_sent) {
        const std::string json = "{\"schema\":1,\"zoom\":" + std::to_string(options_.zoom) +
                                 ",\"exposure\":" + std::to_string(options_.exposure) +
                                 ",\"torch\":" + std::string(options_.torch ? "true" : "false") +
                                 "}";
        connection.send(metadata(protocol::MessageType::Control, desktop_sequence++, json));
        controls_sent = true;
      } else if (type == protocol::MessageType::Ping) {
        auto pong = metadata(protocol::MessageType::Pong, desktop_sequence++, "{}");
        pong.header.flags = static_cast<std::uint16_t>(protocol::Flag::Acknowledgement);
        connection.send(pong);
      } else if (type == protocol::MessageType::VideoConfig ||
                 type == protocol::MessageType::VideoFrame) {
        if (encoded)
          encoded.write(reinterpret_cast<const char*>(message.payload.data()),
                        static_cast<std::streamsize>(message.payload.size()));
        try {
          for (auto& frame :
               decoder.decode(message.payload, static_cast<std::int64_t>(message.header.pts_us))) {
            sink.push(frame);
            ++stats.frames;
          }
        } catch (const std::exception&) {
          ++stats.decode_errors;
        }
      } else if (type == protocol::MessageType::EndStream) {
        cancelled = true;
      } else if (type == protocol::MessageType::Error) {
        const std::string reason(reinterpret_cast<const char*>(message.payload.data()),
                                 message.payload.size());
        throw std::runtime_error("phone reported: " + reason);
      }
    }
  }
  if (connection.wifi) {
    try {
      connection.send(metadata(protocol::MessageType::EndStream, desktop_sequence++, "{}"));
    } catch (const std::exception&) {
    }
    connection.close();
  }
  sink.stop();
  return stats;
}

} // namespace openlens
