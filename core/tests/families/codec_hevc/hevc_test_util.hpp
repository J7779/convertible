// Helpers shared by the codec_hevc tests.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "support/fixtures.hpp"
#include "support/test_support.hpp"

#include "codec/codec.hpp"
#include "codec/ids.hpp"
#include "codec/registry.hpp"
#include "engine/registry.hpp"
#include "families/codec_hevc/hevc_api.hpp"
#include "util/bytes.hpp"
#include "util/hash.hpp"
#include "util/hex.hpp"
#include "util/text.hpp"

namespace cvt::test::hevc_util {

/// MD5 (hex) of the planes of a frame, row by row without padding, as
/// ffmpeg's framemd5 computes it. `luma_only` for 4:0:0 references.
[[nodiscard]] inline std::string frame_md5(const ir::VideoFrame& frame, bool luma_only = false) {
  util::Md5 md5;
  const std::size_t planes = luma_only ? 1 : ir::plane_count(frame.format);
  for (std::size_t p = 0; p < planes; ++p) {
    const std::size_t rows = ir::plane_height(frame.format, p, frame.height);
    const std::size_t row_bytes = ir::plane_row_bytes(frame.format, p, frame.width);
    for (std::size_t y = 0; y < rows; ++y) {
      md5.update(ByteView(reinterpret_cast<const std::byte*>(frame.planes[p].data.data() + y * frame.planes[p].stride),
                          row_bytes));
    }
  }
  const auto digest = md5.finish();
  std::string hex;
  static constexpr char digits[] = "0123456789abcdef";
  for (const std::uint8_t b : digest) {
    hex.push_back(digits[b >> 4]);
    hex.push_back(digits[b & 15]);
  }
  return hex;
}

/// The reference MD5s of a fixture's `.md5` file (comment lines skipped).
[[nodiscard]] inline std::vector<std::string> reference_md5(std::string_view name) {
  std::vector<std::string> lines;
  const std::string text = fixture_text(std::string("codec_hevc/") + std::string(name) + ".md5");
  std::size_t start = 0;
  while (start < text.size()) {
    std::size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty() && line[0] != '#') lines.push_back(line);
    start = end + 1;
  }
  return lines;
}

/// Every frame of a raw stream, decoded through the registered "hevc" reader.
[[nodiscard]] inline Result<std::vector<ir::VideoFrame>> decode_stream(ByteView data, const Limits& limits = {}) {
  CVT_TRY_ASSIGN(ir::Video video, read_as<ir::Video>("hevc", data, {}, nullptr, limits));
  std::vector<ir::VideoFrame> frames;
  if (!video.next_frame) return fail(Status::Decode, "no frame function");
  for (int guard = 0; guard < 10000; ++guard) {
    CVT_TRY_ASSIGN(std::optional<ir::VideoFrame> frame, video.next_frame());
    if (!frame) return frames;
    frames.push_back(std::move(*frame));
  }
  return fail(Status::Internal, "too many frames");
}

/// A length-prefixed sample of the test container written by the fixture
/// extractor (see SOURCES.md): "HVCS", u32 hvcC size, hvcC, u32 count, then
/// per sample u32 size, i32 pts, i32 dts, bytes. Big-endian.
struct Sample {
  Bytes bytes;
  std::int64_t pts = 0;
  std::int64_t dts = 0;
};
struct SampleFile {
  Bytes hvcc;
  std::vector<Sample> samples;
};

[[nodiscard]] inline std::optional<SampleFile> read_sample_file(ByteView data) {
  util::ByteReader r(data);
  if (r.chars(4) != "HVCS") return std::nullopt;
  SampleFile file;
  const std::uint32_t hvcc_size = r.u32be();
  const ByteView hvcc = r.bytes(hvcc_size);
  file.hvcc.assign(hvcc.begin(), hvcc.end());
  const std::uint32_t count = r.u32be();
  for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
    Sample sample;
    const std::uint32_t size = r.u32be();
    sample.pts = r.i32be();
    sample.dts = r.i32be();
    const ByteView bytes = r.bytes(size);
    sample.bytes.assign(bytes.begin(), bytes.end());
    file.samples.push_back(std::move(sample));
  }
  if (!r.ok()) return std::nullopt;
  return file;
}

/// Run packets through a fresh registered decoder and collect every frame.
[[nodiscard]] inline Result<std::vector<ir::VideoFrame>> decode_packets(const codec::CodecParams& params,
                                                                       const std::vector<ir::Packet>& packets) {
  auto decoder = Registry::instance().media().make_video_decoder(codec::id::hevc);
  if (!decoder) return fail(Status::Internal, "no hevc decoder");
  CVT_TRY(decoder->configure(params));
  std::vector<ir::VideoFrame> frames;
  const auto collect = [&]() -> Result<void> {
    while (true) {
      CVT_TRY_ASSIGN(std::optional<ir::VideoFrame> frame, decoder->receive_frame());
      if (!frame) return {};
      frames.push_back(std::move(*frame));
    }
  };
  for (const ir::Packet& packet : packets) {
    CVT_TRY(decoder->send_packet(packet));
    CVT_TRY(collect());
  }
  CVT_TRY(decoder->flush());
  CVT_TRY(collect());
  return frames;
}

/// Encode frames with the registered encoder; returns the packets in decode order.
[[nodiscard]] inline Result<std::vector<ir::Packet>> encode_frames(const codec::VideoEncodeParams& params,
                                                                  const std::vector<ir::VideoFrame>& frames,
                                                                  Bytes* extradata = nullptr) {
  auto encoder = Registry::instance().media().make_video_encoder(codec::id::hevc);
  if (!encoder) return fail(Status::Internal, "no hevc encoder");
  CVT_TRY(encoder->configure(params));
  if (extradata != nullptr) *extradata = encoder->extradata();
  std::vector<ir::Packet> packets;
  const auto collect = [&]() -> Result<void> {
    while (true) {
      CVT_TRY_ASSIGN(std::optional<ir::Packet> packet, encoder->receive_packet());
      if (!packet) return {};
      packets.push_back(std::move(*packet));
    }
  };
  for (const ir::VideoFrame& frame : frames) {
    CVT_TRY(encoder->send_frame(frame));
    CVT_TRY(collect());
  }
  CVT_TRY(encoder->flush());
  CVT_TRY(collect());
  return packets;
}

[[nodiscard]] inline std::size_t total_size(const std::vector<ir::Packet>& packets) {
  std::size_t total = 0;
  for (const ir::Packet& packet : packets) total += packet.bytes.size();
  return total;
}

}  // namespace cvt::test::hevc_util
