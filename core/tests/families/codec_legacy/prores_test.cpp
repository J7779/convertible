// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <limits>
#include <string>

#include "codec/colorspace.hpp"
#include "families/codec_legacy/legacy_test_support.hpp"
#include "support/test.hpp"

using namespace cvt;
namespace tl = cvt::test::legacy;

namespace {

double fixture_psnr(const std::string& name) {
  const tl::PacketFile file = tl::load_packets(name);
  const tl::RefFile ref = tl::load_reference(name);
  auto frames = tl::decode_file(file);
  REQUIRE_OK(frames);
  CHECK(frames->size() == ref.total);
  const double value = tl::compare_with_reference(*frames, ref);
  MESSAGE(name, " PSNR ", value);
  return value;
}

}  // namespace

TEST_CASE("codec_legacy/prores: decodes Proxy, LT, standard, HQ and interlaced 4:2:2") {
  // ffmpeg's decode as the reference (10-bit PSNR, peak 1023).
  for (const char* name : {"prores_p0_64x64", "prores_p1_64x64", "prores_p2_64x64", "prores_p3_64x64", "prores_interlaced_64x64"}) {
    INFO(name);
    CHECK(fixture_psnr(name) >= 55.0);
  }
}

TEST_CASE("codec_legacy/prores: decodes 4444 with alpha") {
  const tl::PacketFile file = tl::load_packets("prores_4444_alpha_64x64");
  const tl::RefFile ref = tl::load_reference("prores_4444_alpha_64x64");
  auto frames = tl::decode_file(file);
  REQUIRE_OK(frames);
  REQUIRE(frames->size() == ref.total);
  const ir::VideoFrame& ours = (*frames)[ref.indices[0]];
  REQUIRE(ours.format == ir::PixelFormat::RGBA8);
  // Reference: ffmpeg's yuva444p10le decode, converted to RGBA the same way.
  ir::VideoFrame yuv = tl::raw_to_frame(ref, ref.frames[0]);
  yuv.color = ours.color;
  auto rgba = codec::to_rgba(yuv);
  REQUIRE_OK(rgba);
  const auto* alpha = reinterpret_cast<const std::uint8_t*>(ref.frames[0].data()) + 6 * 64 * 64;
  for (std::size_t i = 0; i < 64 * 64; ++i) {
    const int a10 = alpha[2 * i] | alpha[2 * i + 1] << 8;
    rgba->planes[0].data[i * 4 + 3] = static_cast<std::uint8_t>((a10 * 255 + 511) / 1023);
  }
  CHECK(test::psnr(ours, *rgba) >= 45.0);
}

TEST_CASE("codec_legacy/prores: encoder round trip quality and size, 4:2:2 and 4444 with alpha") {
  const auto frames = tl::pattern_frames(ir::PixelFormat::I422P10, 70, 34, 3);
  std::vector<std::size_t> sizes;
  for (const int q : {100, 90, 60, 30}) {
    codec::VideoEncodeParams params;
    params.width = 70;
    params.height = 34;
    params.quality = q;
    params.pixel_format = ir::PixelFormat::I422P10;
    params.color = frames[0].color;
    auto encoded = tl::encode_frames("prores", params, frames);
    REQUIRE_OK(encoded);
    tl::dump_for_interop("prores_q" + std::to_string(q), "prores", 70, 34, params.timescale, *encoded);
    tl::dump_raw_for_interop("prores_q" + std::to_string(q), frames);
    auto decoder = cvt::legacy::make_video_decoder("prores");
    codec::CodecParams cp;
    cp.timescale = params.timescale;
    auto decoded = tl::decode_packets(*decoder, cp, encoded->packets);
    REQUIRE_OK(decoded);
    REQUIRE(decoded->size() == 3);
    for (auto& f : *decoded) f.color = frames[0].color;
    const double psnr = test::min_psnr(frames, *decoded);
    MESSAGE("prores q", q, ": ", encoded->total_bytes(), " bytes, ", psnr, " dB");
    CHECK(psnr >= (q >= 90 ? 55.0 : 40.0));
    sizes.push_back(encoded->total_bytes());
  }
  CHECK(sizes[0] > sizes[1]);
  CHECK(sizes[1] > sizes[2]);
  CHECK(sizes[2] > sizes[3]);

  // RGBA input -> 4444 with alpha.
  std::vector<ir::VideoFrame> rgba;
  for (std::uint32_t i = 0; i < 2; ++i) {
    ir::VideoFrame f = test::test_pattern_frame(ir::PixelFormat::RGBA8, 40, 24, i);
    for (std::size_t k = 3; k < f.planes[0].data.size(); k += 4) f.planes[0].data[k] = static_cast<std::uint8_t>((k / 4) % 40 * 6);
    rgba.push_back(std::move(f));
  }
  codec::VideoEncodeParams params;
  params.width = 40;
  params.height = 24;
  params.pixel_format = ir::PixelFormat::RGBA8;
  auto encoded = tl::encode_frames("prores", params, rgba);
  REQUIRE_OK(encoded);
  tl::dump_for_interop("prores_4444", "prores", 40, 24, params.timescale, *encoded);
  auto decoder = cvt::legacy::make_video_decoder("prores");
  auto decoded = tl::decode_packets(*decoder, {}, encoded->packets);
  REQUIRE_OK(decoded);
  REQUIRE(decoded->size() == 2);
  CHECK((*decoded)[0].format == ir::PixelFormat::RGBA8);
  CHECK(test::psnr((*decoded)[0], rgba[0]) >= 38.0);
  for (std::size_t k = 3; k < rgba[0].planes[0].data.size(); k += 4) {
    CHECK(std::abs(static_cast<int>((*decoded)[0].planes[0].data[k]) - static_cast<int>(rgba[0].planes[0].data[k])) <= 1);
  }
}

TEST_CASE("codec_legacy/prores: damaged input never crashes") {
  tl::fuzz_decoder(tl::load_packets("prores_p2_64x64"), 2);
  tl::fuzz_decoder(tl::load_packets("prores_4444_alpha_64x64"), 2);
  tl::fuzz_decoder(tl::load_packets("prores_interlaced_64x64"), 1);
}
