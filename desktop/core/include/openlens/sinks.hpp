// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "openlens/media.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace openlens {

class FrameSink {
public:
  virtual ~FrameSink() = default;
  virtual void configure(int width, int height, int fps) = 0;
  virtual void push(const VideoFrame& frame) = 0;
  // The phone reports how many degrees clockwise its frames need to appear
  // upright; sinks that render for people can honour it, others ignore it.
  virtual void orientation(int) {}
  virtual void placeholder(std::string_view message) = 0;
  virtual void flush() = 0;
  virtual void stop() = 0;
};

class NullSink final : public FrameSink {
public:
  void configure(int width, int height, int fps) override;
  void push(const VideoFrame& frame) override;
  void placeholder(std::string_view message) override;
  void flush() override;
  void stop() override;
  [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }

private:
  std::uint64_t frames_{};
};

class V4l2Sink final : public FrameSink {
public:
  explicit V4l2Sink(std::string device);
  ~V4l2Sink() override;
  void configure(int width, int height, int fps) override;
  void push(const VideoFrame& frame) override;
  void placeholder(std::string_view message) override;
  void flush() override;
  void stop() override;

private:
  std::string device_;
  int descriptor_{-1};
  int width_{};
  int height_{};
  std::uint32_t pixel_format_{};
  std::vector<std::uint8_t> converted_;
  void write_all(const std::uint8_t* data, std::size_t size);
};

[[nodiscard]] VideoFrame make_test_pattern(int width, int height, std::uint64_t frame_index,
                                           std::int64_t pts_us);

} // namespace openlens
