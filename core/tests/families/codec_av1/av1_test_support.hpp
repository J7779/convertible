// Helpers shared by the codec_av1 tests: a tiny IVF reader, reference MD5
// files, and drivers for the registered decoder and encoder.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "codec/codec.hpp"
#include "codec/ids.hpp"
#include "codec/registry.hpp"
#include "engine/registry.hpp"
#include "support/fixtures.hpp"
#include "util/bytes.hpp"
#include "util/hash.hpp"
#include "util/hex.hpp"
#include "util/text.hpp"

namespace cvt::av1test {

struct IvfFrame {
  ByteView data;
  std::uint64_t pts = 0;
};

struct Ivf {
  std::uint16_t width = 0;
  std::uint16_t height = 0;
  std::uint32_t rate = 0;   ///< time base denominator
  std::uint32_t scale = 0;  ///< time base numerator
  std::vector<IvfFrame> frames;
};

/// Parses an IVF file ("DKIF", 32-byte header, then 12-byte frame headers).
inline std::optional<Ivf> read_ivf(ByteView data) {
  util::ByteReader r(data);
  if (!r.consume("DKIF")) return std::nullopt;
  (void)r.u16le();  // version
  const std::uint16_t header_size = r.u16le();
  if (!r.consume("AV01")) return std::nullopt;
  Ivf ivf;
  ivf.width = r.u16le();
  ivf.height = r.u16le();
  ivf.rate = r.u32le();
  ivf.scale = r.u32le();
  (void)r.u32le();  // frame count (often wrong)
  if (!r.seek(header_size)) return std::nullopt;
  while (!r.at_end()) {
    const std::uint32_t size = r.u32le();
    const std::uint64_t pts = r.u64le();
    const ByteView frame = r.bytes(size);
    if (!r.ok()) return std::nullopt;
    ivf.frames.push_back({frame, pts});
  }
  return ivf;
}

/// MD5 lines of a reference file (comment lines start with '#').
inline std::vector<std::string> read_md5s(std::string_view fixture_name) {
  std::vector<std::string> out;
  for (const std::string_view line : util::split(test::fixture_text(fixture_name), '\n')) {
    const std::string_view trimmed = util::trim(line);
    if (trimmed.empty() || trimmed.front() == '#') continue;
    out.emplace_back(trimmed);
  }
  return out;
}

/// MD5 of the visible samples of a frame, planes back to back, row padding
/// excluded (the layout of the reference files).
inline std::string frame_md5(const ir::VideoFrame& frame) {
  util::Md5 md5;
  for (std::size_t p = 0; p < frame.planes.size(); ++p) {
    const std::size_t row = ir::plane_row_bytes(frame.format, p, frame.width);
    const std::size_t rows = ir::plane_height(frame.format, p, frame.height);
    for (std::size_t y = 0; y < rows; ++y) {
      md5.update(as_bytes(std::span<const std::uint8_t>(frame.planes[p].data.data() + y * frame.planes[p].stride, row)));
    }
  }
  const util::Md5::Digest digest = md5.finish();
  return util::hex_encode(as_bytes(std::span<const std::uint8_t>(digest)));
}

inline std::unique_ptr<codec::VideoDecoder> decoder() {
  return Registry::instance().media().make_video_decoder(codec::id::av1);
}
inline std::unique_ptr<codec::VideoEncoder> encoder() {
  return Registry::instance().media().make_video_encoder(codec::id::av1);
}

/// Feeds packets to a fresh decoder, flushes, returns every frame.
inline Result<std::vector<ir::VideoFrame>> decode_packets(const std::vector<ir::Packet>& packets,
                                                          std::uint32_t timescale, const Bytes& extradata = {}) {
  auto dec = decoder();
  if (!dec) return fail(Status::Internal, "no AV1 decoder registered");
  codec::CodecParams params;
  params.codec = std::string(codec::id::av1);
  params.timescale = timescale;
  params.extradata = extradata;
  CVT_TRY(dec->configure(params));
  std::vector<ir::VideoFrame> frames;
  const auto drain = [&]() -> Result<void> {
    for (;;) {
      CVT_TRY_ASSIGN(std::optional<ir::VideoFrame> frame, dec->receive_frame());
      if (!frame) return {};
      frames.push_back(std::move(*frame));
    }
  };
  for (const ir::Packet& packet : packets) {
    CVT_TRY(dec->send_packet(packet));
    CVT_TRY(drain());
  }
  CVT_TRY(dec->flush());
  CVT_TRY(drain());
  return frames;
}

/// The packets of an IVF file (pts in IVF ticks, duration 0).
inline std::vector<ir::Packet> ivf_packets(const Ivf& ivf) {
  std::vector<ir::Packet> packets;
  for (const IvfFrame& f : ivf.frames) {
    ir::Packet packet;
    packet.bytes.assign(f.data.begin(), f.data.end());
    packet.pts = static_cast<std::int64_t>(f.pts * (ivf.scale == 0 ? 1 : ivf.scale));
    packet.dts = packet.pts;
    packets.push_back(std::move(packet));
  }
  return packets;
}

struct Encoded {
  std::vector<ir::Packet> packets;
  Bytes extradata;
  std::size_t total_bytes = 0;
};

/// Encodes frames with a fresh encoder.
inline Result<Encoded> encode_frames(const std::vector<ir::VideoFrame>& frames, codec::VideoEncodeParams params) {
  auto enc = encoder();
  if (!enc) return fail(Status::Internal, "no AV1 encoder registered");
  CVT_TRY(enc->configure(params));
  Encoded out;
  const auto drain = [&]() -> Result<void> {
    for (;;) {
      CVT_TRY_ASSIGN(std::optional<ir::Packet> packet, enc->receive_packet());
      if (!packet) return {};
      out.total_bytes += packet->bytes.size();
      out.packets.push_back(std::move(*packet));
    }
  };
  for (const ir::VideoFrame& frame : frames) {
    CVT_TRY(enc->send_frame(frame));
    CVT_TRY(drain());
  }
  CVT_TRY(enc->flush());
  CVT_TRY(drain());
  out.extradata = enc->extradata();
  return out;
}

}  // namespace cvt::av1test
