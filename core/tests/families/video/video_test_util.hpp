// Helpers for the video family tests: drain an ir::Video into frames and
// samples, collect packets, read the ffprobe reference tables recorded next
// to the fixtures, and a tiny fake codec for pipeline tests.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "support/fixtures.hpp"
#include "support/test_support.hpp"

#include "codec/codec.hpp"
#include "codec/registry.hpp"
#include "engine/convert.hpp"
#include "families/families.hpp"
#include "ir/video.hpp"
#include "util/crc.hpp"
#include "util/deflate.hpp"
#include "util/text.hpp"

namespace cvt::test::video {

/// Everything a Video yields when decoded completely.
struct Drained {
  std::vector<ir::VideoFrame> frames;
  std::vector<std::int64_t> audio_pts_ms;
  std::vector<float> samples;
};

/// Pull every frame (up to `max_frames`) and every audio chunk out of `video`.
inline Result<Drained> drain(ir::Video& video, std::size_t max_frames = 10000) {
  Drained out;
  if (video.next_frame) {
    while (out.frames.size() < max_frames) {
      CVT_TRY_ASSIGN(auto frame, video.next_frame());
      if (!frame) break;
      out.frames.push_back(std::move(*frame));
    }
  }
  if (video.next_audio) {
    while (true) {
      CVT_TRY_ASSIGN(auto chunk, video.next_audio());
      if (!chunk) break;
      out.audio_pts_ms.push_back(chunk->pts_ms);
      out.samples.insert(out.samples.end(), chunk->samples.begin(), chunk->samples.end());
    }
  }
  return out;
}

/// Every packet of a track.
inline Result<std::vector<ir::Packet>> packets_of(const ir::Track& track) {
  std::vector<ir::Packet> out;
  if (!track.next_packet) return out;
  while (true) {
    CVT_TRY_ASSIGN(auto p, track.next_packet());
    if (!p) break;
    out.push_back(std::move(*p));
  }
  return out;
}

/// One row of a `probe/<fixture>.packets.csv` table (ffprobe -show_packets).
struct ProbePacket {
  int stream = 0;
  std::optional<std::int64_t> pts;
  std::optional<std::int64_t> dts;
  std::optional<std::int64_t> duration;
  std::uint32_t size = 0;
  bool key = false;
  bool discard = false;
  std::uint32_t crc = 0;
};

/// One row of a `probe/<fixture>.streams.csv` table (ffprobe -show_streams).
struct ProbeStream {
  int index = 0;
  std::string type;
  std::string codec;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t sample_rate = 0;
  std::uint32_t channels = 0;
  std::string time_base;
  std::uint32_t extradata_size = 0;
  std::uint32_t extradata_crc = 0;
  std::size_t packets = 0;
};

inline std::vector<std::vector<std::string>> csv_rows(std::string_view name) {
  std::vector<std::vector<std::string>> rows;
  const std::string text = fixture_text(std::string("video/probe/") + std::string(name));
  bool header = true;
  for (const std::string_view line : util::split_lines(text)) {
    if (line.empty()) continue;
    if (header) {
      header = false;
      continue;
    }
    std::vector<std::string> cells;
    for (const std::string_view cell : util::split(line, ',')) cells.emplace_back(cell);
    rows.push_back(std::move(cells));
  }
  return rows;
}

inline std::optional<std::int64_t> opt_int(const std::string& s) {
  if (s == "NA" || s.empty()) return std::nullopt;
  return util::parse_int(s);
}

inline std::vector<ProbePacket> probe_packets(std::string_view fixture_name) {
  std::vector<ProbePacket> out;
  for (const auto& r : csv_rows(std::string(fixture_name) + ".packets.csv")) {
    if (r.size() < 7) continue;
    ProbePacket p;
    p.stream = static_cast<int>(util::parse_int(r[0]).value_or(-1));
    p.pts = opt_int(r[1]);
    p.dts = opt_int(r[2]);
    p.duration = opt_int(r[3]);
    p.size = static_cast<std::uint32_t>(util::parse_int(r[4]).value_or(0));
    p.key = r[5].find('K') != std::string::npos;
    p.discard = r[5].find('D') != std::string::npos;
    p.crc = static_cast<std::uint32_t>(util::parse_int(r[6], 16).value_or(0));
    out.push_back(p);
  }
  return out;
}

inline std::vector<ProbeStream> probe_streams(std::string_view fixture_name) {
  std::vector<ProbeStream> out;
  for (const auto& r : csv_rows(std::string(fixture_name) + ".streams.csv")) {
    if (r.size() < 12) continue;
    ProbeStream s;
    s.index = static_cast<int>(util::parse_int(r[0]).value_or(-1));
    s.type = r[1];
    s.codec = r[2];
    s.width = static_cast<std::uint32_t>(util::parse_int(r[4]).value_or(0));
    s.height = static_cast<std::uint32_t>(util::parse_int(r[5]).value_or(0));
    s.sample_rate = static_cast<std::uint32_t>(util::parse_int(r[6]).value_or(0));
    s.channels = static_cast<std::uint32_t>(util::parse_int(r[7]).value_or(0));
    s.time_base = r[8];
    s.extradata_size = static_cast<std::uint32_t>(util::parse_int(r[9]).value_or(0));
    s.extradata_crc = static_cast<std::uint32_t>(util::parse_int(r[10], 16).value_or(0));
    s.packets = static_cast<std::size_t>(util::parse_int(r[11]).value_or(0));
    out.push_back(s);
  }
  return out;
}

/// Packets of one ffprobe stream, in file order.
inline std::vector<ProbePacket> probe_packets_of(const std::vector<ProbePacket>& all, int stream) {
  std::vector<ProbePacket> out;
  for (const ProbePacket& p : all) {
    if (p.stream == stream) out.push_back(p);
  }
  return out;
}

/// Reference frames stored as gzip-compressed raw I420.
inline std::vector<ir::VideoFrame> reference_i420(std::string_view name, std::uint32_t width, std::uint32_t height) {
  std::vector<ir::VideoFrame> frames;
  const Bytes packed = fixture(std::string("video/") + std::string(name));
  auto raw = util::gzip_decompress(packed, 64u << 20);
  if (!raw) return frames;
  const std::size_t frame_size = std::size_t{width} * height * 3 / 2;
  for (std::size_t at = 0; at + frame_size <= raw->size(); at += frame_size) {
    ir::VideoFrame f = ir::VideoFrame::allocate(ir::PixelFormat::I420, width, height);
    std::size_t offset = at;
    for (ir::Plane& p : f.planes) {
      std::memcpy(p.data.data(), raw->data() + offset, p.data.size());
      offset += p.data.size();
    }
    frames.push_back(std::move(f));
  }
  return frames;
}

// --------------------------------------------------------------------------- a registry for pipeline tests

/// A registry with the real families plus extra codecs registered by the test.
struct TestRegistry {
  Registry registry;
  template <class Setup>
  explicit TestRegistry(Setup&& setup) {
    setup(registry.media());
    register_all(registry);
    registry.finalize();
  }
};

/// Run a conversion through a given registry (like test::convert).
inline Converted convert_with(const Registry& registry, ByteView input, std::string_view source, std::string_view target,
                              std::string_view options_json = {}) {
  Converted out;
  auto options = Options::parse(options_json);
  if (!options) {
    out.status = options.error().code;
    out.message = options.error().message;
    return out;
  }
  ConvertRequest request;
  request.input = input;
  request.source_id = std::string(source);
  request.target_id = std::string(target);
  request.options = std::move(*options);
  auto result = run_conversion(registry, request);
  if (!result) {
    out.status = result.error().code;
    out.message = result.error().message;
    return out;
  }
  out.data = std::move(result->data);
  out.format_id = std::move(result->format_id);
  out.warnings = std::move(result->warnings);
  return out;
}

/// Read a file of `format` with a given registry.
template <IrType T>
Result<T> read_with(const Registry& registry, std::string_view format, ByteView input, std::string_view options_json = {}) {
  const Codec* codec = nullptr;
  for (const Codec* c : registry.codecs_for(format)) {
    if (c->kind == kind_of<T> && c->read) codec = c;
  }
  if (codec == nullptr) return fail(Status::UnsupportedConversion, "no reader");
  CVT_TRY_ASSIGN(const Options options, Options::parse(options_json));
  ReadContext ctx;
  CVT_TRY_ASSIGN(ctx.options, options.normalized(codec->read_options));
  ctx.registry = &registry;
  ctx.input = input;
  ctx.format_id = std::string(format);
  CVT_TRY_ASSIGN(AnyIR value, codec->read(ctx));
  return std::move(value).template get<T>();
}

}  // namespace cvt::test::video
