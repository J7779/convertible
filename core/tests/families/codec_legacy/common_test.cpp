// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

#include "codec/ids.hpp"
#include "engine/registry.hpp"
#include "families/codec_legacy/common/bitstream.hpp"
#include "families/codec_legacy/common/codec_base.hpp"
#include "families/codec_legacy/common/dct.hpp"
#include "families/codec_legacy/common/vlc.hpp"
#include "families/codec_legacy/legacy_test_support.hpp"
#include "support/test.hpp"

using namespace cvt;

TEST_CASE("codec_legacy/common: bit reader and writer round trip") {
  legacy::BitWriter w;
  for (int n = 1; n <= 32; ++n) w.put(n, 0x9E3779B9u >> (32 - n));
  w.put_signed(7, -5);
  w.align(true);
  const std::vector<std::uint8_t> bytes = w.take();
  legacy::BitReader r{std::span<const std::uint8_t>(bytes)};
  for (int n = 1; n <= 32; ++n) CHECK(r.bits(n) == (0x9E3779B9u >> (32 - n)));
  CHECK(r.sbits(7) == -5);
  r.align();
  CHECK(r.bits_left() == 0);
  CHECK_FALSE(r.overrun());
  CHECK(r.bits(5) == 0u);
  CHECK(r.overrun());
  r.seek(3);
  CHECK(r.position() == 3u);
  CHECK(r.bits(2) == ((bytes[0] >> 3) & 3u));
}

TEST_CASE("codec_legacy/common: VLC tables decode every code and reject garbage") {
  const legacy::VlcCode codes[] = {legacy::vlc("1", 0), legacy::vlc("01", 1), legacy::vlc("001", 2),
                                   legacy::vlc("0001 0000 0000 0001", 3), legacy::vlc("0001 0000 0000 0000 01", 4)};
  legacy::Vlc table(codes, 4);
  REQUIRE(table.ok());
  legacy::BitWriter w;
  for (const auto& c : codes) w.put(c.length, c.code);
  w.put(20, 0);  // not a code
  const auto bytes = w.take();
  legacy::BitReader r{std::span<const std::uint8_t>(bytes)};
  for (const auto& c : codes) CHECK(table.decode(r) == c.symbol);
  CHECK(table.decode(r) == legacy::Vlc::invalid);
  const legacy::VlcCode bad[] = {legacy::vlc("1", 0), legacy::vlc("10", 1)};
  CHECK_FALSE(legacy::Vlc(bad).ok());
}

TEST_CASE("codec_legacy/common: IDCT meets IEEE 1180 accuracy and FDCT inverts it") {
  // Random blocks in [-256, 255] as in IEEE 1180, compared with a double
  // precision reference: peak error <= 1, mean square error far below 0.06.
  std::uint32_t seed = 1;
  const auto rnd = [&seed](int lo, int hi) {
    seed = seed * 1103515245u + 12345u;
    return lo + static_cast<int>((seed >> 8) % static_cast<std::uint32_t>(hi - lo + 1));
  };
  double sq = 0;
  int peak = 0;
  for (int trial = 0; trial < 2000; ++trial) {
    std::int32_t pixels[64];
    for (auto& p : pixels) p = rnd(-256, 255);
    double coeff[64];
    for (int v = 0; v < 8; ++v) {
      for (int u = 0; u < 8; ++u) {
        double s = 0;
        for (int y = 0; y < 8; ++y) {
          for (int x = 0; x < 8; ++x) {
            s += pixels[y * 8 + x] * std::cos((2 * x + 1) * u * std::numbers::pi / 16) *
                 std::cos((2 * y + 1) * v * std::numbers::pi / 16);
          }
        }
        s *= 0.25 * (u == 0 ? std::numbers::sqrt2 / 2 : 1.0) * (v == 0 ? std::numbers::sqrt2 / 2 : 1.0);
        coeff[v * 8 + u] = s;
      }
    }
    std::int32_t q[64];
    for (int i = 0; i < 64; ++i) q[i] = static_cast<std::int32_t>(std::lround(coeff[i]));
    std::int32_t ours[64];
    legacy::fdct8x8(pixels, ours);
    for (int i = 0; i < 64; ++i) CHECK(std::abs(ours[i] - q[i]) <= 1);
    std::int32_t back[64];
    legacy::idct8x8(q, back);
    for (int y = 0; y < 8; ++y) {
      for (int x = 0; x < 8; ++x) {
        double s = 0;
        for (int v = 0; v < 8; ++v) {
          for (int u = 0; u < 8; ++u) {
            s += (u == 0 ? std::numbers::sqrt2 / 2 : 1.0) * (v == 0 ? std::numbers::sqrt2 / 2 : 1.0) * q[v * 8 + u] *
                 std::cos((2 * x + 1) * u * std::numbers::pi / 16) * std::cos((2 * y + 1) * v * std::numbers::pi / 16);
          }
        }
        const int reference = static_cast<int>(std::lround(s / 4));
        const int error = back[y * 8 + x] - reference;
        peak = std::max(peak, std::abs(error));
        sq += error * error;
      }
    }
  }
  CHECK(peak <= 1);
  CHECK(sq / (2000.0 * 64) < 0.001);
}

TEST_CASE("codec_legacy/common: timestamps and quality mapping") {
  CHECK(legacy::ticks_to_ms(90000, 90000) == 1000);
  CHECK(legacy::ticks_to_ms(-1, 1000) == -1);
  CHECK(legacy::ticks_to_ms(1001, 30000) == 33);
  CHECK(legacy::ms_to_ticks(40, 90000) == 3600);
  CHECK(legacy::quality_to_scale(90, 1, 3, 31) == 3.0);
  CHECK(legacy::quality_to_scale(100, 1, 3, 31) == 1.0);
  CHECK(legacy::quality_to_scale(1, 1, 3, 31) == 31.0);
  CHECK(legacy::quality_to_scale(50, 1, 3, 31) > legacy::quality_to_scale(70, 1, 3, 31));
}

TEST_CASE("codec_legacy/common: registration is clean and complete") {
  const Registry& registry = Registry::instance();
  for (const std::string& problem : registry.validate()) CHECK_MESSAGE(problem.find("media codec") == std::string::npos, problem);
  for (const legacy::CodecEntry& entry : legacy::codec_entries()) {
    INFO(entry.id);
    CHECK((entry.decoder || entry.encoder));
    if (codec::id::is_known(entry.id)) {
      CHECK(registry.media().has_video_decoder(entry.id) == static_cast<bool>(entry.decoder));
      CHECK(registry.media().has_video_encoder(entry.id) == static_cast<bool>(entry.encoder));
    }
    if (entry.decoder) CHECK(legacy::make_video_decoder(entry.id) != nullptr);
    if (entry.encoder) CHECK(legacy::make_video_encoder(entry.id) != nullptr);
  }
}

TEST_CASE("codec_legacy/common: packet and reference fixtures load") {
  const auto file = test::legacy::load_packets("mpeg1_96x64");
  CHECK(file.codec == "mpeg1video");
  CHECK(file.width == 96);
  CHECK(file.packets.size() == 12);
  const auto ref = test::legacy::load_reference("mpeg1_96x64");
  CHECK(ref.total == 12);
  CHECK(ref.frames.size() == ref.indices.size());
  const auto md5 = test::legacy::load_reference("huffyuv_yuv422_left_64x64");
  CHECK(md5.md5);
  CHECK(md5.digests.size() == md5.total);
}
