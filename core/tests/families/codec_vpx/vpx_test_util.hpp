// Helpers of the codec_vpx tests: an IVF reader and writer, ffmpeg
// "framemd5" parsing, and decode / encode loops over the registered codecs.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "codec/codec.hpp"
#include "codec/registry.hpp"
#include "engine/registry.hpp"
#include "ir/video.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"
#include "util/bytes.hpp"
#include "util/hash.hpp"
#include "util/text.hpp"

namespace cvt::test::vpx {

/// An IVF file: 32-byte "DKIF" header, then (u32 size, u64 pts, payload)*.
struct Ivf {
  std::string fourcc;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t rate = 0;   ///< time base denominator
  std::uint32_t scale = 0;  ///< time base numerator
  std::vector<ir::Packet> packets;
};

/// Packet timestamps are kept in IVF units (`scale / rate` seconds).
inline std::optional<Ivf> parse_ivf(ByteView file) {
  util::ByteReader reader(file);
  if (!reader.consume("DKIF")) return std::nullopt;
  Ivf ivf;
  (void)reader.u16le();  // version
  const std::uint16_t header_size = reader.u16le();
  ivf.fourcc = std::string(as_chars(reader.bytes(4)));
  ivf.width = reader.u16le();
  ivf.height = reader.u16le();
  ivf.rate = reader.u32le();
  ivf.scale = reader.u32le();
  if (!reader.ok() || header_size < 32 || header_size > file.size()) return std::nullopt;
  reader = util::ByteReader(file.subspan(header_size));
  while (reader.remaining() >= 12) {
    const std::uint32_t size = reader.u32le();
    const std::uint64_t pts = reader.u64le();
    if (reader.remaining() < size) return std::nullopt;
    ir::Packet packet;
    const ByteView payload = reader.bytes(size);
    packet.bytes.assign(payload.begin(), payload.end());
    packet.pts = static_cast<std::int64_t>(pts);
    packet.dts = packet.pts;
    ivf.packets.push_back(std::move(packet));
  }
  return ivf;
}

inline Bytes write_ivf(std::string_view fourcc, std::uint32_t width, std::uint32_t height, std::uint32_t rate,
                       std::uint32_t scale, const std::vector<ir::Packet>& packets) {
  util::ByteWriter out;
  out.text("DKIF").u16le(0).u16le(32).text(fourcc);
  out.u16le(static_cast<std::uint16_t>(width)).u16le(static_cast<std::uint16_t>(height));
  out.u32le(rate).u32le(scale).u32le(static_cast<std::uint32_t>(packets.size())).u32le(0);
  for (const ir::Packet& packet : packets) {
    out.u32le(static_cast<std::uint32_t>(packet.bytes.size())).u64le(static_cast<std::uint64_t>(packet.pts));
    out.bytes(packet.bytes);
  }
  return out.take();
}

/// One line of ffmpeg's framemd5 output.
struct FrameMd5 {
  std::int64_t pts = 0;
  std::int64_t duration = 0;
  std::size_t size = 0;
  std::string md5;
};

inline std::vector<FrameMd5> parse_framemd5(std::string_view text) {
  std::vector<FrameMd5> lines;
  for (std::string_view line : util::split(text, '\n')) {
    line = util::trim(line);
    if (line.empty() || line.front() == '#') continue;
    const std::vector<std::string_view> fields = util::split(line, ',');
    if (fields.size() != 6) continue;
    FrameMd5 entry;
    entry.pts = util::parse_int(util::trim(fields[2])).value_or(-1);
    entry.duration = util::parse_int(util::trim(fields[3])).value_or(-1);
    entry.size = static_cast<std::size_t>(util::parse_int(util::trim(fields[4])).value_or(0));
    entry.md5 = std::string(util::trim(fields[5]));
    lines.push_back(std::move(entry));
  }
  return lines;
}

/// MD5 of a YCbCr frame's tightly packed planes (Y, Cb, Cr; 16-bit words
/// little-endian), the layout ffmpeg's framemd5 hashes.
inline std::string frame_md5(const ir::VideoFrame& frame, std::size_t* size = nullptr) {
  util::Md5 md5;
  std::size_t total = 0;
  for (std::size_t p = 0; p < frame.planes.size(); ++p) {
    const std::size_t rows = ir::plane_height(frame.format, p, frame.height);
    const std::size_t row_bytes = ir::plane_row_bytes(frame.format, p, frame.width);
    for (std::size_t y = 0; y < rows; ++y) {
      md5.update(as_bytes(std::span<const std::uint8_t>(frame.planes[p].data.data() + y * frame.planes[p].stride,
                                                        row_bytes)));
      total += row_bytes;
    }
  }
  if (size != nullptr) *size = total;
  const auto digest = md5.finish();
  static constexpr char digits[] = "0123456789abcdef";
  std::string hex;
  for (const std::uint8_t b : digest) {
    hex += digits[b >> 4];
    hex += digits[b & 15];
  }
  return hex;
}

inline const codec::Registry& media() { return Registry::instance().media(); }

/// Decode `packets` with the registered decoder for `codec_id`; the error
/// of the first failing call, or every frame.
inline Result<std::vector<ir::VideoFrame>> decode_all(std::string_view codec_id,
                                                      const std::vector<ir::Packet>& packets,
                                                      std::uint32_t timescale = 1000, const Bytes& extradata = {}) {
  auto decoder = media().make_video_decoder(codec_id);
  if (!decoder) return fail(Status::UnsupportedConversion, "no decoder");
  codec::CodecParams params;
  params.codec = std::string(codec_id);
  params.timescale = timescale;
  params.extradata = extradata;
  CVT_TRY(decoder->configure(params));
  std::vector<ir::VideoFrame> frames;
  const auto drain = [&]() -> Result<void> {
    while (true) {
      CVT_TRY_ASSIGN(auto frame, decoder->receive_frame());
      if (!frame) return {};
      frames.push_back(std::move(*frame));
    }
  };
  for (const ir::Packet& packet : packets) {
    CVT_TRY(decoder->send_packet(packet));
    CVT_TRY(drain());
  }
  CVT_TRY(decoder->flush());
  CVT_TRY(drain());
  return frames;
}

struct Encoded {
  std::vector<ir::Packet> packets;
  Bytes extradata;
  [[nodiscard]] std::size_t bytes() const {
    std::size_t total = 0;
    for (const ir::Packet& packet : packets) total += packet.bytes.size();
    return total;
  }
};

inline Result<Encoded> encode_all(std::string_view codec_id, const codec::VideoEncodeParams& params,
                                  const std::vector<ir::VideoFrame>& frames) {
  auto encoder = media().make_video_encoder(codec_id);
  if (!encoder) return fail(Status::UnsupportedConversion, "no encoder");
  CVT_TRY(encoder->configure(params));
  Encoded out;
  const auto drain = [&]() -> Result<void> {
    while (true) {
      CVT_TRY_ASSIGN(auto packet, encoder->receive_packet());
      if (!packet) return {};
      out.packets.push_back(std::move(*packet));
    }
  };
  for (const ir::VideoFrame& frame : frames) {
    CVT_TRY(encoder->send_frame(frame));
    CVT_TRY(drain());
  }
  CVT_TRY(encoder->flush());
  CVT_TRY(drain());
  out.extradata = encoder->extradata();
  return out;
}

/// A moving test sequence with real texture: smooth ramps (the shared test
/// pattern) plus a drifting block of deterministic noise and a sharp-edged
/// bar, so that quantisation has detail to remove and motion to follow.
inline std::vector<ir::VideoFrame> textured_sequence(ir::PixelFormat format, std::uint32_t width,
                                                     std::uint32_t height, std::uint32_t count) {
  std::vector<ir::VideoFrame> frames;
  const bool wide = ir::bytes_per_sample(format) == 2;
  const Bytes noise = random_bytes(std::size_t{width} * height, 7);
  for (std::uint32_t i = 0; i < count; ++i) {
    ir::VideoFrame frame = test_pattern_frame(format, width, height, i);
    ir::Plane& luma = frame.planes[0];
    const std::uint32_t block_x = (i * 3) % (width / 2 + 1);
    const std::uint32_t block_y = (i * 2) % (height / 2 + 1);
    for (std::uint32_t y = 0; y < height; ++y) {
      for (std::uint32_t x = 0; x < width; ++x) {
        std::uint32_t value = 0;
        bool set = false;
        if (x >= block_x && x < block_x + width / 3 && y >= block_y && y < block_y + height / 3) {
          // Noise texture that moves with the block.
          const std::size_t at = std::size_t{(y - block_y)} * width + (x - block_x);
          value = 40u + std::to_integer<std::uint32_t>(noise[at]) * 160u / 255u;
          set = true;
        } else if (((x + i * 2) / 6) % 5 == 0) {
          value = 220;  // vertical bars sweeping right
          set = true;
        }
        if (!set) continue;
        if (wide) {
          const std::uint32_t v10 = value * 4;
          luma.data[y * luma.stride + x * 2] = static_cast<std::uint8_t>(v10 & 0xFF);
          luma.data[y * luma.stride + x * 2 + 1] = static_cast<std::uint8_t>(v10 >> 8);
        } else {
          luma.data[y * luma.stride + x] = static_cast<std::uint8_t>(value);
        }
      }
    }
    frames.push_back(std::move(frame));
  }
  return frames;
}

}  // namespace cvt::test::vpx
