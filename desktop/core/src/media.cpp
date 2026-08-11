// SPDX-License-Identifier: GPL-2.0-or-later
#include "openlens/media.hpp"

#include <algorithm>
#include <stdexcept>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace openlens {

struct H264Decoder::Impl {
  AVCodecContext* context{};
  AVFrame* decoded{};
  AVFrame* converted{};
  SwsContext* scaler{};

  Impl() {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (codec == nullptr)
      throw std::runtime_error("FFmpeg H.264 decoder is unavailable");
    context = avcodec_alloc_context3(codec);
    decoded = av_frame_alloc();
    converted = av_frame_alloc();
    if (context == nullptr || decoded == nullptr || converted == nullptr)
      throw std::runtime_error("could not initialize FFmpeg H.264 decoder");
    // Must be set before avcodec_open2: the default frame-threaded decoding
    // buffers one frame per thread, adding hundreds of milliseconds of latency.
    context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    context->thread_count = 1;
    if (avcodec_open2(context, codec, nullptr) < 0)
      throw std::runtime_error("could not initialize FFmpeg H.264 decoder");
  }

  ~Impl() {
    sws_freeContext(scaler);
    av_frame_free(&converted);
    av_frame_free(&decoded);
    avcodec_free_context(&context);
  }
};

H264Decoder::H264Decoder() : impl_(std::make_unique<Impl>()) {}
H264Decoder::~H264Decoder() = default;
H264Decoder::H264Decoder(H264Decoder&&) noexcept = default;
H264Decoder& H264Decoder::operator=(H264Decoder&&) noexcept = default;

std::vector<VideoFrame> H264Decoder::decode(std::span<const std::byte> access_unit,
                                            std::int64_t pts_us) {
  AVPacket* packet = av_packet_alloc();
  if (packet == nullptr)
    throw std::runtime_error("could not allocate decoder packet");
  packet->data = reinterpret_cast<std::uint8_t*>(const_cast<std::byte*>(access_unit.data()));
  packet->size = static_cast<int>(access_unit.size());
  packet->pts = pts_us;
  const int sent = avcodec_send_packet(impl_->context, packet);
  av_packet_free(&packet);
  if (sent < 0 && sent != AVERROR(EAGAIN))
    throw std::runtime_error("H.264 decoder rejected an access unit");

  std::vector<VideoFrame> frames;
  while (avcodec_receive_frame(impl_->context, impl_->decoded) == 0) {
    const int width = impl_->decoded->width;
    const int height = impl_->decoded->height;
    impl_->scaler = sws_getCachedContext(
        impl_->scaler, width, height, static_cast<AVPixelFormat>(impl_->decoded->format), width,
        height, AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (impl_->scaler == nullptr)
      throw std::runtime_error("could not create frame converter");
    VideoFrame output;
    output.width = width;
    output.height = height;
    output.pts_us = impl_->decoded->pts == AV_NOPTS_VALUE ? pts_us : impl_->decoded->pts;
    output.i420.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3U /
                       2U);
    std::uint8_t* planes[4] = {output.i420.data(),
                               output.i420.data() + static_cast<std::size_t>(width) *
                                                        static_cast<std::size_t>(height),
                               output.i420.data() + static_cast<std::size_t>(width) *
                                                        static_cast<std::size_t>(height) * 5U / 4U,
                               nullptr};
    const int strides[4] = {width, width / 2, width / 2, 0};
    static_cast<void>(sws_scale(impl_->scaler, impl_->decoded->data, impl_->decoded->linesize, 0,
                                height, planes, strides));
    frames.push_back(std::move(output));
    av_frame_unref(impl_->decoded);
  }
  return frames;
}

void H264Decoder::flush() { avcodec_flush_buffers(impl_->context); }

namespace {

void transform_plane(const std::uint8_t* source, int source_width, int source_height,
                     std::uint8_t* destination, int rotation, bool mirror) {
  const int destination_width = rotation == 90 || rotation == 270 ? source_height : source_width;
  for (int y = 0; y < source_height; ++y) {
    for (int x = 0; x < source_width; ++x) {
      int destination_x = x;
      int destination_y = y;
      if (rotation == 90) {
        destination_x = source_height - 1 - y;
        destination_y = x;
      } else if (rotation == 180) {
        destination_x = source_width - 1 - x;
        destination_y = source_height - 1 - y;
      } else if (rotation == 270) {
        destination_x = y;
        destination_y = source_width - 1 - x;
      }
      if (mirror)
        destination_x = destination_width - 1 - destination_x;
      destination[destination_y * destination_width + destination_x] = source[y * source_width + x];
    }
  }
}

} // namespace

VideoFrame transform_frame(const VideoFrame& input, int rotation_degrees, bool mirror_horizontal) {
  if (rotation_degrees != 0 && rotation_degrees != 90 && rotation_degrees != 180 &&
      rotation_degrees != 270) {
    throw std::invalid_argument("rotation must be 0, 90, 180, or 270 degrees");
  }
  if (rotation_degrees == 0 && !mirror_horizontal)
    return input;
  VideoFrame output;
  output.width = rotation_degrees == 90 || rotation_degrees == 270 ? input.height : input.width;
  output.height = rotation_degrees == 90 || rotation_degrees == 270 ? input.width : input.height;
  output.pts_us = input.pts_us;
  output.discontinuity = input.discontinuity;
  const std::size_t y_size =
      static_cast<std::size_t>(input.width) * static_cast<std::size_t>(input.height);
  output.i420.resize(y_size * 3U / 2U);
  transform_plane(input.i420.data(), input.width, input.height, output.i420.data(),
                  rotation_degrees, mirror_horizontal);
  transform_plane(input.i420.data() + y_size, input.width / 2, input.height / 2,
                  output.i420.data() + y_size, rotation_degrees, mirror_horizontal);
  transform_plane(input.i420.data() + y_size * 5U / 4U, input.width / 2, input.height / 2,
                  output.i420.data() + y_size * 5U / 4U, rotation_degrees, mirror_horizontal);
  return output;
}

VideoFrame fit_frame(const VideoFrame& input, int canvas_width, int canvas_height) {
  if (canvas_width <= 0 || canvas_height <= 0 ||
      (input.width == canvas_width && input.height == canvas_height))
    return input;
  const double scale = std::min(static_cast<double>(canvas_width) / input.width,
                                static_cast<double>(canvas_height) / input.height);
  const int scaled_width =
      std::max(2, static_cast<int>(static_cast<double>(input.width) * scale) & ~1);
  const int scaled_height =
      std::max(2, static_cast<int>(static_cast<double>(input.height) * scale) & ~1);
  const int offset_x = ((canvas_width - scaled_width) / 2) & ~1;
  const int offset_y = ((canvas_height - scaled_height) / 2) & ~1;

  VideoFrame output;
  output.width = canvas_width;
  output.height = canvas_height;
  output.pts_us = input.pts_us;
  output.discontinuity = input.discontinuity;
  const std::size_t canvas_y_size =
      static_cast<std::size_t>(canvas_width) * static_cast<std::size_t>(canvas_height);
  output.i420.assign(canvas_y_size * 3U / 2U, 128);
  std::fill_n(output.i420.begin(), canvas_y_size, static_cast<std::uint8_t>(0));

  SwsContext* scaler =
      sws_getContext(input.width, input.height, AV_PIX_FMT_YUV420P, scaled_width, scaled_height,
                     AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (scaler == nullptr)
    throw std::runtime_error("could not create the frame scaler");
  const std::size_t input_y_size =
      static_cast<std::size_t>(input.width) * static_cast<std::size_t>(input.height);
  const std::uint8_t* source_planes[3] = {input.i420.data(), input.i420.data() + input_y_size,
                                          input.i420.data() + input_y_size * 5U / 4U};
  const int source_strides[3] = {input.width, input.width / 2, input.width / 2};
  const auto luma_offset = static_cast<std::size_t>(offset_y) * static_cast<std::size_t>(canvas_width) +
                           static_cast<std::size_t>(offset_x);
  const auto chroma_offset =
      static_cast<std::size_t>(offset_y / 2) * static_cast<std::size_t>(canvas_width / 2) +
      static_cast<std::size_t>(offset_x / 2);
  std::uint8_t* destination_planes[3] = {output.i420.data() + luma_offset,
                                         output.i420.data() + canvas_y_size + chroma_offset,
                                         output.i420.data() + canvas_y_size * 5U / 4U +
                                             chroma_offset};
  const int destination_strides[3] = {canvas_width, canvas_width / 2, canvas_width / 2};
  sws_scale(scaler, source_planes, source_strides, 0, input.height, destination_planes,
            destination_strides);
  sws_freeContext(scaler);
  return output;
}

} // namespace openlens
