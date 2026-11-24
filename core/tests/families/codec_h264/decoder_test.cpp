// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// H.264 decoding of streams written by x264 and Apple VideoToolbox, checked
// frame by frame against ffmpeg's decoder. H.264 decoding is bit-exact, so
// every frame must have exactly the reference MD5; the one known deviation of
// the imported decoder (MBAFF with implicit weighted bi-prediction) is tested
// separately with a PSNR floor.
#include <algorithm>
#include <string>

#include "codec/ids.hpp"
#include "families/codec_h264/h264_test_support.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"
#include "util/deflate.hpp"

using namespace cvt;
namespace th = cvt::test::h264;

namespace {

struct Stream {
  const char* name;
  std::uint32_t width;
  std::uint32_t height;
  std::size_t frames;
};

// Every bit-exact Annex B fixture with its ffmpeg reference (see SOURCES.md).
constexpr Stream streams[] = {
    {"baseline_cavlc_176x144", 176, 144, 12},
    {"main_cabac_bframes_176x144", 176, 144, 16},
    {"high_8x8_weighted_multiref_176x144", 176, 144, 16},
    {"high_cavlc_4slices_176x144", 176, 144, 10},
    {"mbaff_176x144", 176, 144, 10},
    {"crop_98x58_fullrange_bt709", 98, 58, 10},
    {"crop_70x34_bt601", 70, 34, 8},
    {"videotoolbox_main_bframes_176x144", 176, 144, 12},
};

// Fixtures that also exist as MP4 samples with avcC and container timestamps.
constexpr const char* packet_streams[] = {"main_cabac_bframes_176x144", "high_8x8_weighted_multiref_176x144",
                                          "mbaff_176x144", "crop_98x58_fullrange_bt709",
                                          "videotoolbox_main_bframes_176x144"};

std::string path(std::string_view name, std::string_view extension) {
  return "codec_h264/" + std::string(name) + std::string(extension);
}

const Stream& stream_named(std::string_view name) {
  for (const Stream& stream : streams) {
    if (stream.name == name) return stream;
  }
  return streams[0];
}

codec::CodecParams h264_params() {
  codec::CodecParams params;
  params.codec = std::string(codec::id::h264);
  return params;
}

void check_against_reference(const Stream& stream, const std::vector<codec::VideoFrame>& frames) {
  const std::vector<std::string> reference = th::read_frame_md5(test::fixture_text(path(stream.name, ".md5")));
  REQUIRE(reference.size() == stream.frames);
  REQUIRE(frames.size() == reference.size());
  for (std::size_t i = 0; i < frames.size(); ++i) {
    INFO("frame ", i);
    CHECK(frames[i].format == ir::PixelFormat::I420);
    CHECK(frames[i].width == stream.width);
    CHECK(frames[i].height == stream.height);
    CHECK(frames[i].valid());
    CHECK(th::frame_md5(frames[i]) == reference[i]);
  }
}

/// Decode without asserting anything: the damaged-input tests only require
/// that this returns.
std::size_t decode_quietly(const codec::CodecParams& params, const std::vector<codec::Packet>& packets) {
  cvt::h264::H264Decoder decoder;
  if (!decoder.configure(params)) return 0;
  std::size_t frames = 0;
  for (const codec::Packet& packet : packets) {
    if (!decoder.send_packet(packet)) return frames;
    for (;;) {
      auto frame = decoder.receive_frame();
      if (!frame || !*frame) break;
      if (!(*frame)->valid()) return frames;
      ++frames;
    }
  }
  if (!decoder.flush()) return frames;
  for (;;) {
    auto frame = decoder.receive_frame();
    if (!frame || !*frame) break;
    ++frames;
  }
  return frames;
}

}  // namespace

TEST_CASE("codec_h264/decoder: Annex B access units match ffmpeg bit for bit") {
  for (const Stream& stream : streams) {
    INFO(stream.name);
    const Bytes data = test::fixture(path(stream.name, ".h264"));
    const auto decoded = th::decode_packets(h264_params(), th::access_unit_packets(data));
    REQUIRE_OK(decoded);
    check_against_reference(stream, decoded->frames);
    CHECK(decoded->warnings.empty());
    // Without container timestamps frames are timed from the frame rate.
    for (std::size_t i = 1; i < decoded->frames.size(); ++i) {
      CHECK(decoded->frames[i].pts_ms > decoded->frames[i - 1].pts_ms);
      CHECK(decoded->frames[i - 1].duration_ms > 0);
    }
  }
}

TEST_CASE("codec_h264/decoder: a whole stream in one packet decodes the same") {
  for (const Stream& stream : streams) {
    INFO(stream.name);
    codec::Packet packet;
    packet.bytes = test::fixture(path(stream.name, ".h264"));
    const auto decoded = th::decode_packets(h264_params(), {packet});
    REQUIRE_OK(decoded);
    check_against_reference(stream, decoded->frames);
  }
}

TEST_CASE("codec_h264/decoder: MP4 samples with avcC carry their timestamps through reordering") {
  for (const char* name : packet_streams) {
    INFO(name);
    const th::PacketFile file = th::read_packet_file(test::fixture(path(name, ".pkt")));
    REQUIRE(!file.packets.empty());
    REQUIRE(cvt::h264::is_avcc(file.avcc));
    codec::CodecParams params = h264_params();
    params.extradata = file.avcc;
    params.timescale = file.timescale;
    const auto decoded = th::decode_packets(params, file.packets);
    REQUIRE_OK(decoded);
    check_against_reference(stream_named(name), decoded->frames);
    // Frames come out in presentation order with the container's PTS.
    std::vector<std::int64_t> pts;
    for (const codec::Packet& packet : file.packets) pts.push_back(packet.pts);
    std::ranges::sort(pts);
    REQUIRE(decoded->frames.size() == pts.size());
    for (std::size_t i = 0; i < pts.size(); ++i) {
      INFO("frame ", i);
      const std::int64_t expected = (pts[i] * 1000 + file.timescale / 2) / file.timescale;
      CHECK(decoded->frames[i].pts_ms == expected);
    }
    // B-frames really were reordered in these files.
    bool reordered = false;
    for (std::size_t i = 1; i < file.packets.size(); ++i) reordered = reordered || file.packets[i].pts < file.packets[i - 1].pts;
    if (std::string_view(name) != "crop_98x58_fullrange_bt709") CHECK(reordered);
  }
}

TEST_CASE("codec_h264/decoder: colour information and cropping come from the SPS") {
  {
    const auto decoded = th::decode_packets(h264_params(), th::access_unit_packets(test::fixture(path("crop_98x58_fullrange_bt709", ".h264"))));
    REQUIRE_OK(decoded);
    REQUIRE(!decoded->frames.empty());
    const codec::VideoFrame& frame = decoded->frames.front();
    CHECK(frame.width == 98);
    CHECK(frame.height == 58);
    CHECK(frame.color.matrix == ir::ColorMatrix::BT709);
    CHECK(frame.color.range == ir::ColorRange::Full);
    CHECK(frame.color.siting == ir::ChromaSiting::Left);
    CHECK(frame.duration_ms == 33);  // 30 fps from the VUI timing information
  }
  {
    const auto decoded = th::decode_packets(h264_params(), th::access_unit_packets(test::fixture(path("crop_70x34_bt601", ".h264"))));
    REQUIRE_OK(decoded);
    REQUIRE(!decoded->frames.empty());
    CHECK(decoded->frames.front().color.matrix == ir::ColorMatrix::BT601);
    CHECK(decoded->frames.front().color.range == ir::ColorRange::Limited);
  }
  {
    // Unspecified matrix: BT.601 for standard definition.
    const auto decoded = th::decode_packets(h264_params(), th::access_unit_packets(test::fixture(path("baseline_cavlc_176x144", ".h264"))));
    REQUIRE_OK(decoded);
    REQUIRE(!decoded->frames.empty());
    CHECK(decoded->frames.front().color.matrix == ir::ColorMatrix::BT601);
  }
}

TEST_CASE("codec_h264/decoder: MBAFF with implicit weighted bi-prediction (known libavc deviation)") {
  // libavc decodes the reference pictures of this x264 MBAFF stream exactly,
  // but its B pictures (implicit weighted bi-prediction in field macroblock
  // pairs) differ slightly from ffmpeg's. They are not used as references,
  // so the error does not propagate. See SOURCES.md.
  const Bytes data = test::fixture(path("mbaff_implicit_weights_176x144", ".h264"));
  const auto decoded = th::decode_packets(h264_params(), th::access_unit_packets(data));
  REQUIRE_OK(decoded);
  const std::vector<std::string> reference = th::read_frame_md5(test::fixture_text(path("mbaff_implicit_weights_176x144", ".md5")));
  REQUIRE(decoded->frames.size() == reference.size());
  const auto raw = util::zlib_decompress(test::fixture(path("mbaff_implicit_weights_176x144", ".frames_1_3.yuv.zlib")));
  REQUIRE_OK(raw);
  const std::size_t frame_bytes = 176 * 144 * 3 / 2;
  REQUIRE(raw->size() == 2 * frame_bytes);
  std::size_t exact = 0;
  for (std::size_t i = 0; i < decoded->frames.size(); ++i) {
    INFO("frame ", i);
    if (th::frame_md5(decoded->frames[i]) == reference[i]) {
      ++exact;
      continue;
    }
    // Only the two B pictures may differ, and only slightly.
    REQUIRE((i == 1 || i == 3));
    codec::VideoFrame expected = codec::VideoFrame::allocate(ir::PixelFormat::I420, 176, 144);
    const std::size_t offset = (i == 1 ? 0 : frame_bytes);
    std::size_t at = offset;
    for (ir::Plane& plane : expected.planes) {
      std::copy_n(reinterpret_cast<const std::uint8_t*>(raw->data()) + at, plane.data.size(), plane.data.begin());
      at += plane.data.size();
    }
    const double psnr = test::psnr(expected, decoded->frames[i]);
    MESSAGE("MBAFF implicit-weight B picture ", i, ": ", psnr, " dB against ffmpeg");
    CHECK(psnr >= 50.0);
  }
  CHECK(exact >= 8);
}

TEST_CASE("codec_h264/decoder: unsupported profiles fail with a clear message") {
  struct Case {
    const char* name;
    const char* words;
  };
  constexpr Case cases[] = {{"unsupported_high10_32x32", "10-bit samples is not supported yet"},
                            {"unsupported_high422_32x32", "4:2:2 chroma) is not supported yet"}};
  for (const Case& c : cases) {
    INFO(c.name);
    const auto decoded = th::decode_packets(h264_params(), th::access_unit_packets(test::fixture(path(c.name, ".h264"))));
    REQUIRE_STATUS(decoded, Status::Decode);
    CHECK(decoded.error().message.find(c.words) != std::string::npos);
    CHECK(decoded.error().message.find("H.264 High") == 0);
  }
}

TEST_CASE("codec_h264/decoder: a change of picture size mid-stream is followed") {
  Bytes joined = test::fixture(path("baseline_cavlc_176x144", ".h264"));
  const Bytes second = test::fixture(path("crop_70x34_bt601", ".h264"));
  joined.insert(joined.end(), second.begin(), second.end());
  const auto decoded = th::decode_packets(h264_params(), th::access_unit_packets(joined));
  REQUIRE_OK(decoded);
  REQUIRE(decoded->frames.size() == 12 + 8);
  CHECK(decoded->warnings.empty());
  const std::vector<std::string> first_ref = th::read_frame_md5(test::fixture_text(path("baseline_cavlc_176x144", ".md5")));
  const std::vector<std::string> second_ref = th::read_frame_md5(test::fixture_text(path("crop_70x34_bt601", ".md5")));
  for (std::size_t i = 0; i < 12; ++i) CHECK(th::frame_md5(decoded->frames[i]) == first_ref[i]);
  for (std::size_t i = 0; i < 8; ++i) {
    CHECK(decoded->frames[12 + i].width == 70);
    CHECK(th::frame_md5(decoded->frames[12 + i]) == second_ref[i]);
  }
}

TEST_CASE("codec_h264/decoder: calling pattern and bad configuration") {
  cvt::h264::H264Decoder decoder;
  CHECK_STATUS(decoder.send_packet({}), Status::InvalidArgument);  // not configured
  codec::CodecParams params = h264_params();
  params.extradata = test::bytes("\x01\x64\x00");  // avcC version 1, cut short
  CHECK_STATUS(decoder.configure(params), Status::Decode);

  cvt::h264::H264Decoder fresh;
  REQUIRE_OK(fresh.configure(h264_params()));
  CHECK_STATUS(fresh.configure(h264_params()), Status::InvalidArgument);  // only once
  const auto none = fresh.receive_frame();
  REQUIRE_OK(none);
  CHECK(!none->has_value());
  // Slices before any parameter set are skipped with a warning.
  const std::vector<codec::Packet> packets = th::access_unit_packets(test::fixture(path("baseline_cavlc_176x144", ".h264")));
  codec::Packet slice_only;
  for (const ByteView nal : cvt::h264::split_annexb(packets[1].bytes)) {
    if (cvt::h264::is_vcl(cvt::h264::nal_type(nal))) slice_only.bytes = cvt::h264::to_annexb(std::vector<ByteView>{nal});
  }
  REQUIRE(!slice_only.bytes.empty());
  CHECK_OK(fresh.send_packet(slice_only));
  CHECK_OK(fresh.flush());
  const auto after = fresh.receive_frame();
  REQUIRE_OK(after);
  CHECK(!after->has_value());
  CHECK(!fresh.take_warnings().empty());
  CHECK_STATUS(fresh.send_packet(slice_only), Status::InvalidArgument);  // after flush
}

TEST_CASE("codec_h264/decoder: damaged streams are concealed or rejected, never a crash") {
  for (const Stream& stream : streams) {
    INFO(stream.name);
    const Bytes data = test::fixture(path(stream.name, ".h264"));
    const std::vector<codec::Packet> packets = th::access_unit_packets(data);
    // Truncated streams: 30 prefix lengths spread over the file.
    for (std::size_t length = 1; length < data.size(); length += data.size() / 30 + 1) {
      codec::Packet packet;
      packet.bytes.assign(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(length));
      (void)decode_quietly(h264_params(), {packet});
    }
    // Bit flips and byte runs in the slices (headers kept intact).
    for (std::uint32_t seed = 1; seed <= 6; ++seed) {
      std::vector<codec::Packet> damaged = packets;
      const Bytes noise = test::random_bytes(64, seed);
      for (std::size_t k = 0; k + 3 < noise.size(); k += 4) {
        codec::Packet& packet = damaged[std::to_integer<std::size_t>(noise[k]) % damaged.size()];
        if (packet.bytes.size() < 64) continue;
        const std::size_t at = 32 + (std::to_integer<std::size_t>(noise[k + 1]) << 8 | std::to_integer<std::size_t>(noise[k + 2])) % (packet.bytes.size() - 32);
        packet.bytes[at] ^= static_cast<std::byte>(1u << (std::to_integer<unsigned>(noise[k + 3]) & 7));
        if (seed % 3 == 0) packet.bytes[at] = std::byte{0};  // fake start codes too
      }
      (void)decode_quietly(h264_params(), damaged);
    }
    // Lost packets.
    std::vector<codec::Packet> lossy;
    for (std::size_t i = 0; i < packets.size(); ++i) {
      if (i % 3 != 1) lossy.push_back(packets[i]);
    }
    CHECK(decode_quietly(h264_params(), lossy) > 0);
  }
  // Garbage in either framing, with and without a valid avcC.
  const th::PacketFile file = th::read_packet_file(test::fixture(path("main_cabac_bframes_176x144", ".pkt")));
  for (std::uint32_t seed = 1; seed <= 8; ++seed) {
    codec::Packet noise;
    noise.bytes = test::random_bytes(512 + seed * 100, seed);
    (void)decode_quietly(h264_params(), {noise, noise});
    codec::CodecParams params = h264_params();
    params.extradata = file.avcc;
    (void)decode_quietly(params, {file.packets[0], noise, file.packets[1]});
    codec::CodecParams bad = h264_params();
    bad.extradata = file.avcc;
    bad.extradata[8 + seed] ^= std::byte{0x55};  // damaged SPS inside avcC
    (void)decode_quietly(bad, file.packets);
  }
}
