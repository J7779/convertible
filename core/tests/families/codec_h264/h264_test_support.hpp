// Helpers shared by the codec_h264 tests.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "codec/codec.hpp"
#include "codec/ids.hpp"
#include "families/codec_h264/h264_bitstream.hpp"
#include "families/codec_h264/h264_codec.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"
#include "util/bytes.hpp"
#include "util/hash.hpp"
#include "util/hex.hpp"
#include "util/text.hpp"

namespace cvt::test::h264 {

/// A video track extracted from an MP4 by the fixture tooling (see
/// tests/data/codec_h264/SOURCES.md for the layout): the avcC record and the
/// samples exactly as stored, with their container timestamps.
struct PacketFile {
  std::uint32_t timescale = 0;
  Bytes avcc;
  std::vector<codec::Packet> packets;
};

inline PacketFile read_packet_file(ByteView data) {
  PacketFile file;
  util::ByteReader reader(data);
  if (reader.chars(8) != std::string_view("CVTPKT1\0", 8)) return file;
  file.timescale = reader.u32be();
  const ByteView avcc = reader.bytes(reader.u32be());
  file.avcc.assign(avcc.begin(), avcc.end());
  const std::uint32_t count = reader.u32be();
  for (std::uint32_t i = 0; i < count && reader.ok(); ++i) {
    codec::Packet packet;
    const std::uint32_t size = reader.u32be();
    packet.pts = reader.i64be();
    packet.dts = reader.i64be();
    packet.keyframe = reader.u8() != 0;
    const ByteView payload = reader.bytes(size);
    packet.bytes.assign(payload.begin(), payload.end());
    file.packets.push_back(std::move(packet));
  }
  if (!reader.ok()) file.packets.clear();
  return file;
}

/// The per-frame MD5 column of an ffmpeg `-f framemd5` file.
inline std::vector<std::string> read_frame_md5(std::string_view text) {
  std::vector<std::string> hashes;
  for (const std::string_view line : util::split(text, '\n')) {
    if (line.empty() || line.front() == '#') continue;
    const std::size_t comma = line.rfind(',');
    if (comma == std::string_view::npos) continue;
    hashes.emplace_back(util::trim(line.substr(comma + 1)));
  }
  return hashes;
}

/// MD5 of a frame's planes, tightly packed (the layout ffmpeg hashes).
inline std::string frame_md5(const codec::VideoFrame& frame) {
  util::Md5 md5;
  for (std::size_t p = 0; p < frame.planes.size(); ++p) {
    const std::size_t rows = ir::plane_height(frame.format, p, frame.height);
    const std::size_t row_bytes = ir::plane_row_bytes(frame.format, p, frame.width);
    for (std::size_t r = 0; r < rows; ++r) {
      md5.update(as_bytes(std::span<const std::uint8_t>(frame.planes[p].data.data() + r * frame.planes[p].stride,
                                                         row_bytes)));
    }
  }
  const util::Md5::Digest digest = md5.finish();
  return util::hex_encode(as_bytes(std::span<const std::uint8_t>(digest)));
}

struct Decoded {
  std::vector<codec::VideoFrame> frames;
  std::vector<std::string> warnings;
};

/// Run a decoder over packets following the documented calling pattern.
inline Result<Decoded> decode_packets(const codec::CodecParams& params, const std::vector<codec::Packet>& packets) {
  cvt::h264::H264Decoder decoder;
  Decoded result;
  CVT_TRY(decoder.configure(params));
  const auto collect = [&]() -> Result<void> {
    for (;;) {
      CVT_TRY_ASSIGN(std::optional<codec::VideoFrame> frame, decoder.receive_frame());
      if (!frame) return {};
      result.frames.push_back(std::move(*frame));
    }
  };
  for (const codec::Packet& packet : packets) {
    CVT_TRY(decoder.send_packet(packet));
    CVT_TRY(collect());
  }
  CVT_TRY(decoder.flush());
  CVT_TRY(collect());
  result.warnings = decoder.take_warnings();
  return result;
}

/// An Annex B stream cut into one packet per access unit (no timestamps).
inline std::vector<codec::Packet> access_unit_packets(ByteView stream) {
  std::vector<codec::Packet> packets;
  for (const ByteView unit : cvt::h264::split_access_units(stream)) {
    codec::Packet packet;
    packet.bytes.assign(unit.begin(), unit.end());
    packets.push_back(std::move(packet));
  }
  return packets;
}

/// "Natural-like" content: smooth shading, two moving soft blobs, a moving
/// edge and fine deterministic grain - harder than the synthetic ramps.
inline codec::VideoFrame natural_frame(std::uint32_t width, std::uint32_t height, std::uint32_t index) {
  codec::VideoFrame frame = codec::VideoFrame::allocate(ir::PixelFormat::I420, width, height);
  frame.pts_ms = static_cast<std::int64_t>(index) * 40;
  frame.duration_ms = 40;
  const Bytes grain = test::random_bytes(std::size_t{width} * height, 7 + index);
  const double t = index;
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const double bx = width * (0.3 + 0.2 * std::sin(t * 0.15)), by = height * (0.4 + 0.1 * std::cos(t * 0.1));
      const double d1 = ((x - bx) * (x - bx) + (y - by) * (y - by)) / (0.02 * width * width);
      const double cx = width * (0.7 - 0.01 * t), cy = height * 0.6;
      const double d2 = ((x - cx) * (x - cx) + (y - cy) * (y - cy)) / (0.01 * width * width);
      double v = 60 + 80.0 * y / height + 70 * std::exp(-d1) - 50 * std::exp(-d2);
      v += (x + 2 * t > width * 0.5 && x + 2 * t < width * 0.52) ? 40 : 0;  // a thin moving bar
      v += (std::to_integer<int>(grain[std::size_t{y} * width + x]) - 128) / 32.0;
      frame.planes[0].data[std::size_t{y} * frame.planes[0].stride + x] =
          static_cast<std::uint8_t>(std::clamp(v, 16.0, 235.0));
    }
  }
  for (std::size_t p = 1; p < 3; ++p) {
    const std::uint32_t cw = (width + 1) / 2, ch = (height + 1) / 2;
    for (std::uint32_t y = 0; y < ch; ++y) {
      for (std::uint32_t x = 0; x < cw; ++x) {
        const double v = 128 + (p == 1 ? 30.0 : -20.0) * std::sin((x + t) * 0.05) * std::cos(y * 0.07);
        frame.planes[p].data[std::size_t{y} * frame.planes[p].stride + x] = static_cast<std::uint8_t>(v);
      }
    }
  }
  return frame;
}

struct Encoded {
  Bytes extradata;
  std::vector<codec::Packet> packets;
  std::size_t bytes = 0;
};

inline Result<Encoded> encode(const codec::VideoEncodeParams& params, const std::vector<codec::VideoFrame>& frames) {
  cvt::h264::H264Encoder encoder;
  CVT_TRY(encoder.configure(params));
  Encoded result;
  const auto drain = [&]() -> Result<void> {
    for (;;) {
      CVT_TRY_ASSIGN(std::optional<codec::Packet> packet, encoder.receive_packet());
      if (!packet) return {};
      result.bytes += packet->bytes.size();
      result.packets.push_back(std::move(*packet));
    }
  };
  for (const codec::VideoFrame& frame : frames) {
    CVT_TRY(encoder.send_frame(frame));
    CVT_TRY(drain());
  }
  CVT_TRY(encoder.flush());
  CVT_TRY(drain());
  result.extradata = encoder.extradata();
  return result;
}

inline Result<std::vector<codec::VideoFrame>> decode(const Encoded& encoded) {
  codec::CodecParams params;
  params.codec = std::string(codec::id::h264);
  params.extradata = encoded.extradata;
  params.timescale = 90000;
  CVT_TRY_ASSIGN(Decoded decoded, decode_packets(params, encoded.packets));
  return decoded.frames;
}

inline std::vector<codec::VideoFrame> synthetic_clip(std::uint32_t width, std::uint32_t height, std::uint32_t count) {
  std::vector<codec::VideoFrame> frames;
  for (std::uint32_t i = 0; i < count; ++i) frames.push_back(test::test_pattern_frame(ir::PixelFormat::I420, width, height, i));
  return frames;
}

inline std::vector<codec::VideoFrame> natural_clip(std::uint32_t width, std::uint32_t height, std::uint32_t count) {
  std::vector<codec::VideoFrame> frames;
  for (std::uint32_t i = 0; i < count; ++i) frames.push_back(natural_frame(width, height, i));
  return frames;
}

inline codec::VideoEncodeParams params_for(std::uint32_t width, std::uint32_t height, int quality) {
  codec::VideoEncodeParams params;
  params.width = width;
  params.height = height;
  params.fps = {25, 1};
  params.quality = quality;
  params.timescale = 90000;
  return params;
}

}  // namespace cvt::test::h264
