// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace openlens {

struct VideoFrame {
  int width{};
  int height{};
  std::int64_t pts_us{};
  std::vector<std::uint8_t> i420;
  bool discontinuity{};
};

class H264Decoder {
public:
  H264Decoder();
  ~H264Decoder();
  H264Decoder(H264Decoder&&) noexcept;
  H264Decoder& operator=(H264Decoder&&) noexcept;
  H264Decoder(const H264Decoder&) = delete;
  H264Decoder& operator=(const H264Decoder&) = delete;
  [[nodiscard]] std::vector<VideoFrame> decode(std::span<const std::byte> access_unit,
                                               std::int64_t pts_us);
  void flush();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] VideoFrame transform_frame(const VideoFrame& input, int rotation_degrees,
                                         bool mirror_horizontal);

// Scales the frame to fit inside a fixed canvas, preserving aspect ratio and
// centering it on black bars. Used when live phone rotation changes the frame
// shape while the virtual camera format must stay constant.
[[nodiscard]] VideoFrame fit_frame(const VideoFrame& input, int canvas_width, int canvas_height);

} // namespace openlens
