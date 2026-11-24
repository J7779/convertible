// Test helpers for the codec_legacy family: packet-dump and reference-frame
// fixtures (see core/tests/data/codec_legacy/tools/fixture_tool.py for the
// formats), and drivers for the decoder/encoder calling pattern.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "codec/codec.hpp"
#include "codec/colorspace.hpp"
#include "families/codec_legacy/codec_legacy.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test.hpp"
#include "util/bytes.hpp"
#include "util/deflate.hpp"
#include "util/fs.hpp"
#include "util/hash.hpp"

namespace cvt::test::legacy {

/// A CVTPKT01 packet dump: one video stream of a real file.
struct PacketFile {
  std::string codec;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t timescale = 1000;
  Bytes extradata;
  std::vector<ir::Packet> packets;

  [[nodiscard]] codec::CodecParams params() const {
    codec::CodecParams p;
    p.codec = codec;
    p.extradata = extradata;
    p.timescale = timescale;
    p.width = width;
    p.height = height;
    return p;
  }
};

inline constexpr std::int64_t kNoTimestamp = INT64_MIN;

[[nodiscard]] inline std::optional<PacketFile> parse_packets(ByteView data) {
  if (data.size() < 8) return std::nullopt;
  Bytes inflated;
  util::ByteReader r(data);
  if (as_chars(data.first(8)) == "CVTPKTZ1") {
    auto body = util::zlib_decompress(data.subspan(8), 64u << 20);
    if (!body) return std::nullopt;
    inflated = std::move(*body);
    r = util::ByteReader(ByteView(inflated));
  } else if (as_chars(data.first(8)) == "CVTPKT01") {
    r.skip(8);
  } else {
    return std::nullopt;
  }
  PacketFile file;
  const std::size_t codec_length = r.u8();
  file.codec = std::string(as_chars(r.bytes(codec_length)));
  file.width = r.u32le();
  file.height = r.u32le();
  file.timescale = r.u32le();
  const std::size_t extra = r.u32le();
  file.extradata = to_bytes(r.bytes(extra));
  const std::uint32_t count = r.u32le();
  for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
    ir::Packet p;
    const std::size_t size = r.u32le();
    p.pts = static_cast<std::int64_t>(r.u64le());
    p.dts = static_cast<std::int64_t>(r.u64le());
    p.keyframe = (r.u8() & 1) != 0;
    p.bytes = to_bytes(r.bytes(size));
    file.packets.push_back(std::move(p));
  }
  if (!r.ok()) return std::nullopt;
  return file;
}

/// Loads tests/data/codec_legacy/<name>.pkt.
[[nodiscard]] inline PacketFile load_packets(std::string_view name) {
  const Bytes data = fixture("codec_legacy/" + std::string(name) + ".pkt");
  auto file = parse_packets(data);
  REQUIRE_MESSAGE(file.has_value(), "malformed packet fixture");
  return std::move(*file);
}

/// A CVTREF01 reference: ffmpeg's decode of the same file.
struct RefFile {
  std::string pix_fmt;  ///< ffmpeg pixel format name of the raw frames
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t total = 0;              ///< frames ffmpeg decoded
  std::vector<std::uint32_t> indices;   ///< which of them are stored
  bool md5 = false;                     ///< digests instead of pixels
  std::vector<Bytes> frames;            ///< raw frames (pixel references)
  std::vector<Bytes> digests;           ///< 16-byte MD5 per stored frame
};

/// Bytes of one frame of `pix_fmt` in ffmpeg's packed rawvideo layout; 0 if unknown.
[[nodiscard]] inline std::size_t raw_frame_size(std::string_view pix_fmt, std::uint32_t w, std::uint32_t h) {
  const std::size_t n = std::size_t{w} * h;
  const std::size_t cw = (w + 1) / 2;
  const std::size_t ch = (h + 1) / 2;
  if (pix_fmt == "yuv420p") return n + 2 * cw * ch;
  if (pix_fmt == "yuv422p") return n + 2 * cw * h;
  if (pix_fmt == "yuv444p") return 3 * n;
  if (pix_fmt == "yuv411p") return n + 2 * ((w + 3) / 4) * std::size_t{h};
  if (pix_fmt == "yuv420p10le") return 2 * (n + 2 * cw * ch);
  if (pix_fmt == "yuv422p10le") return 2 * (n + 2 * cw * h);
  if (pix_fmt == "yuv444p10le") return 6 * n;
  if (pix_fmt == "yuva444p10le") return 8 * n;
  if (pix_fmt == "gbrp10le" || pix_fmt == "rgb48le") return 6 * n;
  if (pix_fmt == "rgba") return 4 * n;
  return 0;
}

[[nodiscard]] inline RefFile load_reference(std::string_view name) {
  const Bytes data = fixture("codec_legacy/" + std::string(name) + ".ref");
  RefFile ref;
  util::ByteReader r(data);
  REQUIRE(as_chars(r.bytes(8)) == "CVTREF01");
  const std::size_t n = r.u8();
  ref.pix_fmt = std::string(as_chars(r.bytes(n)));
  ref.width = r.u32le();
  ref.height = r.u32le();
  ref.total = r.u32le();
  const std::uint32_t stored = r.u32le();
  for (std::uint32_t i = 0; i < stored; ++i) ref.indices.push_back(r.u32le());
  const char mode = static_cast<char>(r.u8());
  REQUIRE(r.ok());
  if (mode == 'M') {
    ref.md5 = true;
    for (std::uint32_t i = 0; i < stored; ++i) ref.digests.push_back(to_bytes(r.bytes(16)));
    REQUIRE(r.ok());
    return ref;
  }
  REQUIRE(mode == 'Z');
  auto raw = util::zlib_decompress(r.bytes(r.remaining()), 256u << 20);
  REQUIRE(raw.has_value());
  const std::size_t size = raw_frame_size(ref.pix_fmt, ref.width, ref.height);
  REQUIRE(size > 0);
  REQUIRE(raw->size() == size * stored);
  for (std::uint32_t i = 0; i < stored; ++i) {
    ref.frames.emplace_back(raw->begin() + static_cast<std::ptrdiff_t>(i * size),
                            raw->begin() + static_cast<std::ptrdiff_t>((i + 1) * size));
  }
  return ref;
}

/// Copies `rows` rows of `row_bytes` from a packed source into a plane.
inline void copy_plane(const std::uint8_t*& src, ir::Plane& plane, std::size_t row_bytes, std::size_t rows) {
  for (std::size_t y = 0; y < rows; ++y) {
    std::memcpy(plane.data.data() + y * plane.stride, src, row_bytes);
    src += row_bytes;
  }
}

/// A raw ffmpeg frame as an ir::VideoFrame for metric comparison:
/// yuv420p/yuv422p/yuv444p -> I420/I422/I444, the 10-bit variants -> P10,
/// yuv411p -> I422 (each chroma sample doubled horizontally), rgba -> RGBA8.
/// Alpha of yuva444p10le is dropped (compare it separately).
[[nodiscard]] inline ir::VideoFrame raw_to_frame(const RefFile& ref, const Bytes& raw) {
  const std::uint32_t w = ref.width;
  const std::uint32_t h = ref.height;
  const auto* src = reinterpret_cast<const std::uint8_t*>(raw.data());
  using ir::PixelFormat;
  PixelFormat format = PixelFormat::I420;
  const std::string& f = ref.pix_fmt;
  if (f == "rgba") format = PixelFormat::RGBA8;
  else if (f == "yuv420p") format = PixelFormat::I420;
  else if (f == "yuv422p" || f == "yuv411p") format = PixelFormat::I422;
  else if (f == "yuv444p") format = PixelFormat::I444;
  else if (f == "yuv420p10le") format = PixelFormat::I420P10;
  else if (f == "yuv422p10le") format = PixelFormat::I422P10;
  else if (f == "yuv444p10le" || f == "yuva444p10le") format = PixelFormat::I444P10;
  ir::VideoFrame frame = ir::VideoFrame::allocate(format, w, h);
  if (f == "yuv411p") {
    copy_plane(src, frame.planes[0], w, h);
    const std::size_t cw411 = (w + 3) / 4;
    for (std::size_t p = 1; p < 3; ++p) {
      for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < frame.planes[p].stride; ++x) {
          frame.planes[p].data[y * frame.planes[p].stride + x] = src[y * cw411 + x / 2];
        }
      }
      src += cw411 * h;
    }
    return frame;
  }
  for (std::size_t p = 0; p < frame.planes.size(); ++p) {
    copy_plane(src, frame.planes[p], ir::plane_row_bytes(format, p, w), ir::plane_height(format, p, h));
  }
  return frame;
}

/// Our frame in ffmpeg's rawvideo layout for `pix_fmt` (for MD5 comparison).
/// Empty when the formats do not correspond.
[[nodiscard]] inline Bytes frame_to_raw(const ir::VideoFrame& frame, std::string_view pix_fmt) {
  using ir::PixelFormat;
  PixelFormat want = PixelFormat::I420;
  if (pix_fmt == "rgba") want = PixelFormat::RGBA8;
  else if (pix_fmt == "yuv420p") want = PixelFormat::I420;
  else if (pix_fmt == "yuv422p") want = PixelFormat::I422;
  else if (pix_fmt == "yuv444p") want = PixelFormat::I444;
  else if (pix_fmt == "yuv420p10le") want = PixelFormat::I420P10;
  else if (pix_fmt == "yuv422p10le") want = PixelFormat::I422P10;
  else if (pix_fmt == "yuv444p10le") want = PixelFormat::I444P10;
  else return {};
  if (frame.format != want || !frame.valid()) return {};
  Bytes out;
  for (std::size_t p = 0; p < frame.planes.size(); ++p) {
    const std::size_t row = ir::plane_row_bytes(frame.format, p, frame.width);
    const std::size_t rows = ir::plane_height(frame.format, p, frame.height);
    for (std::size_t y = 0; y < rows; ++y) {
      const auto* begin = reinterpret_cast<const std::byte*>(frame.planes[p].data.data() + y * frame.planes[p].stride);
      out.insert(out.end(), begin, begin + row);
    }
  }
  return out;
}

/// Runs the full calling pattern; frames in output order. A decoding error
/// is returned as is (the frames decoded before it are lost).
[[nodiscard]] inline Result<std::vector<ir::VideoFrame>> decode_packets(codec::VideoDecoder& decoder,
                                                                         const codec::CodecParams& params,
                                                                         const std::vector<ir::Packet>& packets) {
  std::vector<ir::VideoFrame> frames;
  CVT_TRY(decoder.configure(params));
  const auto drain = [&]() -> Result<void> {
    while (true) {
      CVT_TRY_ASSIGN(auto frame, decoder.receive_frame());
      if (!frame) return {};
      frames.push_back(std::move(*frame));
    }
  };
  for (const ir::Packet& packet : packets) {
    CVT_TRY(decoder.send_packet(packet));
    CVT_TRY(drain());
  }
  CVT_TRY(decoder.flush());
  CVT_TRY(drain());
  return frames;
}

[[nodiscard]] inline Result<std::vector<ir::VideoFrame>> decode_file(const PacketFile& file) {
  auto decoder = cvt::legacy::make_video_decoder(file.codec);
  if (!decoder) return fail(Status::InvalidArgument, "no decoder for {}", file.codec);
  return decode_packets(*decoder, file.params(), file.packets);
}

/// Encoder output: packets in decode order plus the extradata.
struct Encoded {
  std::vector<ir::Packet> packets;
  Bytes extradata;
  [[nodiscard]] std::size_t total_bytes() const {
    std::size_t n = 0;
    for (const auto& p : packets) n += p.bytes.size();
    return n;
  }
};

[[nodiscard]] inline Result<Encoded> encode_frames(std::string_view codec_id, const codec::VideoEncodeParams& params,
                                                   const std::vector<ir::VideoFrame>& frames) {
  auto encoder = cvt::legacy::make_video_encoder(codec_id);
  if (!encoder) return fail(Status::InvalidArgument, "no encoder for {}", codec_id);
  Encoded out;
  CVT_TRY(encoder->configure(params));
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

/// Our encoder output as a CVTPKT01 file (the fixture format), e.g. for the
/// interop check: `fixture_tool.py mkv` wraps it so that ffmpeg can decode it.
[[nodiscard]] inline Bytes serialize_packets(std::string_view codec_id, std::uint32_t width, std::uint32_t height,
                                             std::uint32_t timescale, const Encoded& encoded) {
  util::ByteWriter w;
  w.text("CVTPKT01");
  w.u8(static_cast<std::uint8_t>(codec_id.size())).text(codec_id);
  w.u32le(width).u32le(height).u32le(timescale);
  w.u32le(static_cast<std::uint32_t>(encoded.extradata.size())).bytes(ByteView(encoded.extradata));
  w.u32le(static_cast<std::uint32_t>(encoded.packets.size()));
  for (const ir::Packet& p : encoded.packets) {
    w.u32le(static_cast<std::uint32_t>(p.bytes.size())).i64le(p.pts).i64le(p.dts).u8(p.keyframe ? 1 : 0);
    w.bytes(ByteView(p.bytes));
  }
  return w.take();
}

/// Development aid: when the environment variable CVT_LEGACY_DUMP names a
/// directory, writes `<dir>/<name>.pkt` there (for checking our streams
/// with independent decoders). Does nothing otherwise.
inline void dump_for_interop(std::string_view name, std::string_view codec_id, std::uint32_t width,
                             std::uint32_t height, std::uint32_t timescale, const Encoded& encoded) {
#if defined(_MSC_VER)
  (void)name, (void)codec_id, (void)width, (void)height, (void)timescale, (void)encoded;
#else
  const char* dir = std::getenv("CVT_LEGACY_DUMP");
  if (dir == nullptr || *dir == '\0') return;
  const Bytes data = serialize_packets(codec_id, width, height, timescale, encoded);
  (void)util::write_file(std::filesystem::path(dir) / (std::string(name) + ".pkt"), data);
#endif
}

/// Moving synthetic input: test::test_pattern_frame with 40 ms spacing.
[[nodiscard]] inline std::vector<ir::VideoFrame> pattern_frames(ir::PixelFormat format, std::uint32_t w, std::uint32_t h,
                                                                std::uint32_t count) {
  std::vector<ir::VideoFrame> frames;
  for (std::uint32_t i = 0; i < count; ++i) frames.push_back(test::test_pattern_frame(format, w, h, i));
  return frames;
}

/// Harder moving input for lossy encoders (8-bit YUV formats only): the test
/// pattern plus a fixed fine texture and a sharp-edged square moving 3 pixels
/// right and 2 down per frame, so that motion search, quantisation and the
/// coefficient VLCs all get exercised.
[[nodiscard]] inline std::vector<ir::VideoFrame> textured_frames(ir::PixelFormat format, std::uint32_t w,
                                                                 std::uint32_t h, std::uint32_t count) {
  std::vector<ir::VideoFrame> frames = pattern_frames(format, w, h, count);
  for (std::uint32_t i = 0; i < count; ++i) {
    ir::Plane& luma = frames[i].planes[0];
    for (std::uint32_t y = 0; y < h; ++y) {
      for (std::uint32_t x = 0; x < w; ++x) {
        std::uint32_t hash = (x * 73856093u) ^ (y * 19349663u);
        hash = (hash ^ (hash >> 13)) * 0x5bd1e995u;
        int v = luma.data[y * luma.stride + x] + static_cast<int>((hash >> 24) % 9) - 4;
        const std::uint32_t sx = (5 + 3 * i) % w;
        const std::uint32_t sy = (4 + 2 * i) % h;
        if (x >= sx && x < sx + w / 4 && y >= sy && y < sy + h / 4) v = ((x / 2 + y / 2) & 1) != 0 ? 220 : 40;
        luma.data[y * luma.stride + x] = static_cast<std::uint8_t>(std::clamp(v, 16, 235));
      }
    }
  }
  return frames;
}

/// Development aid next to dump_for_interop: the source frames as raw planes
/// (`<dir>/<name>.yuv`), for PSNR measurements with independent tools.
inline void dump_raw_for_interop(std::string_view name, const std::vector<ir::VideoFrame>& frames) {
#if defined(_MSC_VER)
  (void)name, (void)frames;
#else
  const char* dir = std::getenv("CVT_LEGACY_DUMP");
  if (dir == nullptr || *dir == '\0') return;
  Bytes all;
  for (const ir::VideoFrame& f : frames) {
    for (std::size_t p = 0; p < f.planes.size(); ++p) {
      const std::size_t row = ir::plane_row_bytes(f.format, p, f.width);
      for (std::size_t y = 0; y < ir::plane_height(f.format, p, f.height); ++y) {
        const auto* begin = reinterpret_cast<const std::byte*>(f.planes[p].data.data() + y * f.planes[p].stride);
        all.insert(all.end(), begin, begin + row);
      }
    }
  }
  (void)util::write_file(std::filesystem::path(dir) / (std::string(name) + ".yuv"), all);
#endif
}

/// Compares decoded frames with a reference file: frame count, then MD5 or
/// PSNR (minimum over the stored frames, all planes) per stored frame.
/// Returns the minimum PSNR (+inf for exact MD5 matches, NaN on mismatch).
[[nodiscard]] inline double compare_with_reference(const std::vector<ir::VideoFrame>& frames, const RefFile& ref) {
  if (frames.size() != ref.total) return detail::not_a_number;
  double lowest = detail::infinity;
  for (std::size_t i = 0; i < ref.indices.size(); ++i) {
    const ir::VideoFrame& ours = frames[ref.indices[i]];
    if (ref.md5) {
      const Bytes raw = frame_to_raw(ours, ref.pix_fmt);
      const auto digest = util::md5(raw);
      if (raw.empty() || std::memcmp(digest.data(), ref.digests[i].data(), 16) != 0) return detail::not_a_number;
    } else {
      const double value = test::psnr(ours, raw_to_frame(ref, ref.frames[i]));
      if (std::isnan(value)) return value;
      lowest = std::min(lowest, value);
    }
  }
  return lowest;
}

/// Feeds every truncation of each packet (up to `max_prefixes` per packet)
/// and bit-flipped copies through fresh decoders; they must only return.
inline void fuzz_decoder(const PacketFile& file, std::size_t max_packets = 4, std::uint32_t seed = 7) {
  std::uint32_t state = seed;
  const auto next = [&state] {
    state = state * 1664525u + 1013904223u;
    return state;
  };
  const std::size_t n = std::min(max_packets, file.packets.size());
  for (std::size_t round = 0; round < 3 * n; ++round) {
    auto decoder = cvt::legacy::make_video_decoder(file.codec);
    REQUIRE(decoder != nullptr);
    std::vector<ir::Packet> packets(file.packets.begin(), file.packets.begin() + static_cast<std::ptrdiff_t>(n));
    ir::Packet& victim = packets[round % n];
    if (round < n) {
      victim.bytes.resize(victim.bytes.size() * (next() % 100) / 100);  // truncated
    } else if (round < 2 * n) {
      for (int k = 0; k < 8 && !victim.bytes.empty(); ++k) {
        victim.bytes[next() % victim.bytes.size()] ^= static_cast<std::byte>(1u << (next() % 8));  // bit flips
      }
    } else {
      for (auto& b : victim.bytes) b = static_cast<std::byte>(next() >> 24);  // garbage of the same length
    }
    (void)decode_packets(*decoder, file.params(), packets);
  }
  // Every prefix of the first packet (capped for big packets).
  const ir::Packet& first = file.packets.front();
  const std::size_t step = std::max<std::size_t>(1, first.bytes.size() / 64);
  for (std::size_t cut = 0; cut < first.bytes.size(); cut += step) {
    auto decoder = cvt::legacy::make_video_decoder(file.codec);
    ir::Packet p = first;
    p.bytes.resize(cut);
    (void)decode_packets(*decoder, file.params(), {p});
  }
}

}  // namespace cvt::test::legacy
