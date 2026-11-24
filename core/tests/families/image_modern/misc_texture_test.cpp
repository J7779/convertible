// Tests of the GPU texture containers (KTX, KTX2, PVR, ASTC, VTF) and the
// block decoders behind them.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "families/image_modern/misc_test_util.hpp"
#include "util/bytes.hpp"

using namespace cvt;
using test::misc_util::data;
using test::misc_util::load;
using test::misc_util::max_diff_visible;
using test::misc_util::reference;
using test::misc_util::single;

namespace {

ir::Image tex(std::string_view format, std::string_view name) { return load(format, std::string("tex/") + std::string(name)); }
ir::Frame tex_ref(std::string_view name) { return reference(std::string("tex/") + std::string(name)); }

/// Largest difference over the channels in `channels` (e.g. "rg").
int channel_diff(const ir::Frame& a, const ir::Frame& b, std::string_view channels) {
  if (!a.valid() || !b.valid() || a.width != b.width || a.height != b.height) return 1000;
  int worst = 0;
  for (std::size_t i = 0; i < a.rgba.size(); i += 4) {
    for (const char c : channels) {
      const std::size_t k = c == 'r' ? 0 : c == 'g' ? 1 : c == 'b' ? 2 : 3;
      worst = std::max(worst, std::abs(int{a.rgba[i + k]} - int{b.rgba[i + k]}));
    }
  }
  return worst;
}

struct BlockCase {
  const char* file;
  const char* reference;
  const char* channels;
  int max_diff;
};

}  // namespace

TEST_CASE("image_modern/misc/ktx2: block formats decode like texture2ddecoder") {
  // Blocks from etcpak, containers from KTX-Software ("ktx create --raw"),
  // references decoded from the same blocks by texture2ddecoder.
  const BlockCase cases[] = {
      {"bc1.ktx2", "bc1_ref.png", "rgba", 1},   {"bc3.ktx2", "bc3_ref.png", "rgba", 1},
      {"bc4.ktx2", "bc4_ref.png", "r", 1},       {"bc5.ktx2", "bc5_ref.png", "rg", 1},
      {"bc7.ktx2", "bc7_ref.png", "rgba", 0},    {"etc1.ktx2", "etc1_ref.png", "rgb", 0},
      {"etc2.ktx2", "etc2_ref.png", "rgb", 0},   {"etc2a8.ktx2", "etc2a8_ref.png", "rgba", 0},
      {"eacr.ktx2", "eacr_ref.png", "r", 1},     {"eacrg.ktx2", "eacrg_ref.png", "rg", 1},
      {"astc8x8.ktx2", "astc8x8_ktx2_ref.png", "rgba", 1},
  };
  for (const BlockCase& c : cases) {
    INFO(c.file);
    const ir::Image image = tex("ktx2", c.file);
    REQUIRE(image.valid());
    const int diff = channel_diff(image.frames[0], tex_ref(c.reference), c.channels);
    INFO("diff ", diff);
    CHECK(diff <= c.max_diff);
  }
  // The compressed pictures still resemble the source.
  CHECK(test::psnr(tex("ktx2", "bc7.ktx2").frames[0], tex_ref("src_rgba.png")) > 35.0);
}

TEST_CASE("image_modern/misc/ktx2: uncompressed, zlib, 16-bit and single-channel textures") {
  const ir::Frame src = tex_ref("src_rgba.png");
  CHECK(test::pixels_equal(tex("ktx2", "rgba8_srgb.ktx2").frames[0], src));
  CHECK(test::pixels_equal(tex("ktx2", "rgba8_zlib.ktx2").frames[0], src));
  const ir::Image wide = tex("ktx2", "rgba16.ktx2");
  CHECK(test::pixels_equal(wide.frames[0], src));
  CHECK(wide.hdr.has_value());
  CHECK(channel_diff(tex("ktx2", "r8.ktx2").frames[0], tex_ref("src_grey.png"), "rgb") == 0);
  // Supercompression schemes that are not implemented fail clearly.
  for (const char* name : {"tex/rgba8_zstd.ktx2", "tex/basis.ktx2"}) {
    INFO(name);
    const auto result = test::read_as<ir::Image>("ktx2", data(name));
    REQUIRE(!result);
    CHECK(result.error().code == Status::UnsupportedConversion);
  }
}

TEST_CASE("image_modern/misc/ktx: KTX 1 files from toktx") {
  CHECK(test::pixels_equal(tex("ktx", "rgba8.ktx").frames[0], tex_ref("src_rgba.png")));
  // 61x45 RGB: rows padded to 4 bytes.
  CHECK(test::pixels_equal(tex("ktx", "rgb8_odd.ktx").frames[0], tex_ref("src_rgb_odd.png")));
  const ir::Image wide = tex("ktx", "rgba16.ktx");
  CHECK(test::pixels_equal(wide.frames[0], tex_ref("src_rgba.png")));
  CHECK(wide.source_bit_depth == 16);
}

TEST_CASE("image_modern/misc/ktx: compressed KTX 1 files from Compressonator") {
  // Noisy pictures push the encoder into BC7 modes 0, 1, 2 and 7 and the
  // ETC2 T, H, planar and punch-through modes.
  const BlockCase cases[] = {
      {"bc7_noise_rgb.ktx", "bc7_noise_rgb_ref.png", "rgba", 0},
      {"bc7_noise.ktx", "bc7_noise_ref.png", "rgba", 0},
      {"etc2_noise.ktx", "etc2_noise_ref.png", "rgb", 0},
      {"bc1_noise.ktx", "bc1_noise_ref.png", "rgba", 1},
  };
  for (const BlockCase& c : cases) {
    INFO(c.file);
    const ir::Image image = tex("ktx", c.file);
    REQUIRE(image.valid());
    const int diff = channel_diff(image.frames[0], tex_ref(c.reference), c.channels);
    INFO("diff ", diff);
    CHECK(diff <= c.max_diff);
  }
  // Punch-through alpha: transparent texels are (0, 0, 0, 0) per the
  // specification; texture2ddecoder keeps a colour there, so compare what is visible.
  const ir::Image punch = tex("ktx", "etc2a1_noise.ktx");
  CHECK(max_diff_visible(punch.frames[0], tex_ref("etc2a1_noise_ref.png")) == 0);
  CHECK(punch.frames[0].has_transparency());
  // BC2 (no independent decoder at hand): close to the source picture.
  const ir::Image bc2 = tex("ktx", "bc2_noise.ktx");
  REQUIRE(bc2.valid());
  CHECK(test::psnr(bc2.frames[0], tex_ref("noise_rgba.png")) > 30.0);
}

TEST_CASE("image_modern/misc/astc: astcenc files decode like astcenc") {
  for (const char* size : {"4x4", "6x6", "8x5", "10x8", "12x12"}) {
    INFO(size);
    const ir::Image image = tex("astc", std::string("astc") + size + ".astc");
    REQUIRE(image.valid());
    const int diff = channel_diff(image.frames[0], tex_ref(std::string("astc") + size + "_ref.png"), "rgba");
    INFO("diff ", diff);
    CHECK(diff <= 1);
  }
}

TEST_CASE("image_modern/misc/pvr: PVR v3 containers") {
  CHECK(test::pixels_equal(tex("pvr", "rgba8888.pvr").frames[0], tex_ref("src_rgba.png")));
  const BlockCase cases[] = {
      {"etc1.pvr", "etc1_ref.png", "rgb", 0},
      {"dxt1.pvr", "bc1_ref.png", "rgba", 1},
      {"bc7.pvr", "bc7_ref.png", "rgba", 0},
      {"etc2a8.pvr", "etc2a8_ref.png", "rgba", 0},
      {"astc6x6.pvr", "astc6x6_ref.png", "rgba", 1},
  };
  for (const BlockCase& c : cases) {
    INFO(c.file);
    CHECK(channel_diff(tex("pvr", c.file).frames[0], tex_ref(c.reference), c.channels) <= c.max_diff);
  }
}

TEST_CASE("image_modern/misc/vtf: Valve textures written by srctools") {
  const BlockCase cases[] = {
      {"rgba8888_75.vtf", "rgba8888_75_ref0.png", "rgba", 0}, {"bgra8888_72.vtf", "bgra8888_72_ref0.png", "rgba", 0},
      {"dxt1_75.vtf", "dxt1_75_ref0.png", "rgba", 1},         {"dxt5_72.vtf", "dxt5_72_ref0.png", "rgba", 1},
      {"rgb888_74.vtf", "rgb888_74_ref0.png", "rgb", 0},      {"i8_75.vtf", "i8_75_ref0.png", "r", 0},
  };
  for (const BlockCase& c : cases) {
    INFO(c.file);
    const ir::Image image = tex("vtf", c.file);
    REQUIRE(image.valid());
    CHECK(max_diff_visible(image.frames[0], image.frames[0]) == 0);
    CHECK(channel_diff(image.frames[0], tex_ref(c.reference), c.channels) <= c.max_diff);
  }
  const ir::Image animated = tex("vtf", "dxt5_72.vtf");
  REQUIRE(animated.frames.size() == 2);
  CHECK(channel_diff(animated.frames[1], tex_ref("dxt5_72_ref1.png"), "rgba") <= 1);
}

TEST_CASE("image_modern/misc/ktx2: writer round trips and compresses") {
  for (const ir::Frame& f : {test::alpha_gradient_frame(37, 21), test::noise_frame(16, 16, 3)}) {
    for (const char* options : {"{}", R"({"ktx2_supercompression":"zlib"})"}) {
      INFO(options);
      const auto encoded = test::write_from("ktx2", single(f), options);
      REQUIRE_OK(encoded);
      const auto decoded = test::read_as<ir::Image>("ktx2", *encoded);
      REQUIRE_OK(decoded);
      CHECK(test::pixels_equal(decoded->frames[0], f));
    }
  }
  const ir::Image smooth = single(test::gradient_frame(64, 64));
  const auto plain = test::write_from("ktx2", smooth);
  const auto zlib = test::write_from("ktx2", smooth, R"({"ktx2_supercompression":"zlib"})");
  REQUIRE_OK(plain);
  REQUIRE_OK(zlib);
  CHECK(zlib->size() < plain->size());
  // Same-format path: KTX2 -> KTX2 with zlib keeps the pixels and shrinks the file.
  const test::Converted same =
      test::convert(data("tex/rgba8_srgb.ktx2"), "ktx2", "ktx2", R"({"ktx2_supercompression":"zlib"})");
  REQUIRE(same.ok());
  CHECK(same.data.size() < data("tex/rgba8_srgb.ktx2").size());
  CHECK(test::pixels_equal(test::read_as<ir::Image>("ktx2", same.data)->frames[0], tex_ref("src_rgba.png")));
  // Every texture converts to PNG through the engine (detection included).
  for (const char* name : {"tex/bc7.ktx2", "tex/rgba8.ktx", "tex/etc1.pvr", "tex/astc6x6.astc", "tex/dxt1_75.vtf"}) {
    INFO(name);
    CHECK(test::convert(data(name), "", "png").ok());
  }
}

TEST_CASE("image_modern/misc/textures: damaged files are errors, never crashes") {
  const std::pair<const char*, const char*> cases[] = {
      {"ktx2", "tex/bc7.ktx2"},     {"ktx2", "tex/rgba8_zlib.ktx2"}, {"ktx2", "tex/astc8x8.ktx2"},
      {"ktx2", "tex/etc2a8.ktx2"},  {"ktx", "tex/rgb8_odd.ktx"},     {"ktx", "tex/rgba16.ktx"},
      {"pvr", "tex/astc6x6.pvr"},   {"pvr", "tex/etc1.pvr"},         {"astc", "tex/astc12x12.astc"},
      {"astc", "tex/astc4x4.astc"}, {"vtf", "tex/dxt5_72.vtf"},      {"vtf", "tex/rgba8888_75.vtf"},
      {"ktx2", "tex/eacrg.ktx2"},   {"ktx2", "tex/bc5.ktx2"},
  };
  for (const auto& [format, name] : cases) {
    INFO(name);
    CHECK(test::fuzz_reader<ir::Image>(format, data(name), 200) > 0);
  }
  // Random bytes as ASTC blocks never crash the block decoder.
  util::ByteWriter astc;
  astc.u32le(0x5CA1AB13).u8(6).u8(6).u8(1).u24le(60).u24le(60).u24le(1);
  astc.bytes(test::random_bytes(10 * 10 * 16, 7));
  const auto noisy = test::read_as<ir::Image>("astc", astc.data());
  CHECK(noisy.has_value());
  // Absurd sizes are rejected before allocating.
  util::ByteWriter huge;
  huge.u32le(0x5CA1AB13).u8(4).u8(4).u8(1).u24le(0xFFFFFF).u24le(0xFFFFFF).u24le(1);
  CHECK(!test::read_as<ir::Image>("astc", huge.data()));
}

TEST_CASE("image_modern/misc/textures: detection") {
  using test::misc_util::detected;
  CHECK(detected(data("tex/bc7.ktx2")) == "ktx2");
  CHECK(detected(data("tex/rgba8.ktx")) == "ktx");
  CHECK(detected(data("tex/etc1.pvr")) == "pvr");
  CHECK(detected(data("tex/astc4x4.astc")) == "astc");
  CHECK(detected(data("tex/dxt1_75.vtf")) == "vtf");
  for (const char* name : {"a.ktx", "a.ktx2", "a.pvr", "a.astc", "a.vtf"}) {
    INFO(name);
    CHECK(detected({}, name) == std::string(name).substr(2));
  }
}
