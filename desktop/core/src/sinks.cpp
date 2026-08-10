// SPDX-License-Identifier: GPL-2.0-or-later
#include "openlens/sinks.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <unistd.h>

namespace openlens {

void NullSink::configure(int, int, int) {}
void NullSink::push(const VideoFrame&) { ++frames_; }
void NullSink::placeholder(std::string_view) {}
void NullSink::flush() {}
void NullSink::stop() {}

V4l2Sink::V4l2Sink(std::string device) : device_(std::move(device)) {}
V4l2Sink::~V4l2Sink() { stop(); }

void V4l2Sink::configure(int width, int height, int fps) {
  stop();
  descriptor_ = ::open(device_.c_str(), O_WRONLY | O_CLOEXEC);
  if (descriptor_ < 0)
    throw std::runtime_error("could not open " + device_ + ": " + std::to_string(errno));
  constexpr std::array<__u32, 3> candidates{
      V4L2_PIX_FMT_YUV420,
      V4L2_PIX_FMT_NV12,
      V4L2_PIX_FMT_YUYV,
  };
  pixel_format_ = 0;
  for (const __u32 candidate : candidates) {
    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    format.fmt.pix.width = static_cast<__u32>(width);
    format.fmt.pix.height = static_cast<__u32>(height);
    format.fmt.pix.pixelformat = candidate;
    format.fmt.pix.field = V4L2_FIELD_NONE;
    format.fmt.pix.bytesperline = static_cast<__u32>(width);
    format.fmt.pix.sizeimage = candidate == V4L2_PIX_FMT_YUYV
                                   ? static_cast<__u32>(width * height * 2)
                                   : static_cast<__u32>(width * height * 3 / 2);
    if (::ioctl(descriptor_, VIDIOC_S_FMT, &format) == 0 &&
        format.fmt.pix.pixelformat == candidate) {
      pixel_format_ = candidate;
      break;
    }
  }
  if (pixel_format_ == 0) {
    stop();
    throw std::runtime_error("V4L2 device does not accept I420, NV12, or YUYV");
  }
  v4l2_streamparm parameters{};
  parameters.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  parameters.parm.output.timeperframe.numerator = 1;
  parameters.parm.output.timeperframe.denominator = static_cast<__u32>(fps);
  static_cast<void>(::ioctl(descriptor_, VIDIOC_S_PARM, &parameters));
  width_ = width;
  height_ = height;
  converted_.clear();
}

void V4l2Sink::write_all(const std::uint8_t* data, std::size_t size) {
  std::size_t offset = 0;
  while (offset < size) {
    const ssize_t written = ::write(descriptor_, data + offset, size - offset);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
    } else if (written < 0 && errno == EINTR) {
      continue;
    } else {
      throw std::runtime_error("V4L2 frame write failed: " + std::to_string(errno));
    }
  }
}

void V4l2Sink::push(const VideoFrame& frame) {
  if (descriptor_ < 0)
    configure(frame.width, frame.height, 30);
  if (frame.width != width_ || frame.height != height_)
    configure(frame.width, frame.height, 30);
  if (pixel_format_ == V4L2_PIX_FMT_YUV420) {
    write_all(frame.i420.data(), frame.i420.size());
    return;
  }
  const std::size_t y_size =
      static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height);
  const std::size_t chroma_size = y_size / 4U;
  const std::uint8_t* const y_plane = frame.i420.data();
  const std::uint8_t* const u_plane = y_plane + y_size;
  const std::uint8_t* const v_plane = u_plane + chroma_size;
  if (pixel_format_ == V4L2_PIX_FMT_NV12) {
    converted_.resize(y_size + chroma_size * 2U);
    std::copy_n(y_plane, y_size, converted_.data());
    for (std::size_t index = 0; index < chroma_size; ++index) {
      converted_[y_size + index * 2U] = u_plane[index];
      converted_[y_size + index * 2U + 1U] = v_plane[index];
    }
  } else {
    converted_.resize(y_size * 2U);
    for (int row = 0; row < frame.height; ++row) {
      for (int column = 0; column < frame.width; column += 2) {
        const std::size_t y_index = static_cast<std::size_t>(row * frame.width + column);
        const std::size_t chroma_index =
            static_cast<std::size_t>((row / 2) * (frame.width / 2) + column / 2);
        const std::size_t output = y_index * 2U;
        converted_[output] = y_plane[y_index];
        converted_[output + 1U] = u_plane[chroma_index];
        converted_[output + 2U] = y_plane[y_index + 1U];
        converted_[output + 3U] = v_plane[chroma_index];
      }
    }
  }
  write_all(converted_.data(), converted_.size());
}

void V4l2Sink::placeholder(std::string_view) {
  if (descriptor_ < 0 || width_ <= 0 || height_ <= 0)
    return;
  const auto frame = make_test_pattern(width_, height_, 0, 0);
  push(frame);
}

void V4l2Sink::flush() {}
void V4l2Sink::stop() {
  if (descriptor_ >= 0)
    ::close(descriptor_);
  descriptor_ = -1;
  pixel_format_ = 0;
  converted_.clear();
}

VideoFrame make_test_pattern(int width, int height, std::uint64_t frame_index,
                             std::int64_t pts_us) {
  VideoFrame frame;
  frame.width = width;
  frame.height = height;
  frame.pts_us = pts_us;
  const std::size_t y_size = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  frame.i420.resize(y_size * 3U / 2U);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const auto value =
          static_cast<std::uint8_t>((x + y + static_cast<int>(frame_index * 4U)) & 0xff);
      frame.i420[static_cast<std::size_t>(y * width + x)] = value;
    }
  }
  std::fill(frame.i420.begin() + static_cast<std::ptrdiff_t>(y_size), frame.i420.end(),
            std::uint8_t{128});
  return frame;
}

} // namespace openlens
