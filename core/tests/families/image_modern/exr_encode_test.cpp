// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// OpenEXR writer (round trips, options, same-format path) and reader
// robustness (truncation, corruption, hostile sizes, limits, cancellation).
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/context.hpp"
#include "engine/registry.hpp"
#include "families/image/image_ops.hpp"
#include "ir/image.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

using namespace cvt;

namespace {

ir::Image decode(ByteView data, std::vector<std::string>* warnings = nullptr, const Limits& limits = {}) {
  auto image = test::read_as<ir::Image>("exr", data, {}, warnings, limits);
  if (!image) {
    FAIL_CHECK("decoding: ", image.error().message);
    return {};
  }
  return std::move(*image);
}

Bytes encode(const ir::Image& image, std::string_view options = {}, std::vector<std::string>* warnings = nullptr) {
  auto out = test::write_from("exr", image, options, warnings);
  if (!out) {
    FAIL_CHECK("encoding: ", out.error().message);
    return {};
  }
  return std::move(*out);
}

float half_bits_value(std::uint16_t h) {
  const std::uint32_t exponent = (h >> 10) & 0x1Fu;
  const std::uint32_t mantissa = h & 0x3FFu;
  const double v =
      exponent == 0 ? std::ldexp(mantissa, -24) : std::ldexp(1024.0 + mantissa, static_cast<int>(exponent) - 25);
  return static_cast<float>((h & 0x8000u) != 0 ? -v : v);
}

/// An opaque HDR image whose colour values are all exactly representable in
/// half precision (finite, any sign, denormals included), or arbitrary floats.
ir::Image hdr_image(std::uint32_t w, std::uint32_t h, bool half_exact, std::uint32_t seed = 3) {
  ir::HdrPixels px;
  px.width = w;
  px.height = h;
  px.rgba.resize(std::size_t{w} * h * 4);
  const Bytes noise = test::random_bytes(px.rgba.size() * 4, seed);
  for (std::size_t i = 0; i < px.rgba.size(); ++i) {
    if (i % 4 == 3) {
      px.rgba[i] = 1.0f;
      continue;
    }
    const auto b = [&](std::size_t k) { return std::to_integer<std::uint32_t>(noise[i * 4 + k]); };
    const std::size_t x = (i / 4) % w;
    if (half_exact) {
      // Mostly a smooth ramp (what real pictures look like) plus noise bits.
      std::uint16_t bits = static_cast<std::uint16_t>(0x3000 + x * 37 + (b(0) & 0x0F));
      if ((b(1) & 7) == 0) bits = static_cast<std::uint16_t>((b(2) << 8 | b(3)) & 0x7BFF);  // any finite magnitude
      if ((b(1) & 0x30) == 0) bits = static_cast<std::uint16_t>(bits | 0x8000);
      px.rgba[i] = half_bits_value(bits);
    } else {
      px.rgba[i] = static_cast<float>(x) / 7.0f + static_cast<float>(b(0) * 256 + b(1)) / 65536.0f - 0.25f;
    }
  }
  ir::Image image;
  image.frames.push_back(image::tone_map(px));
  image.hdr = std::move(px);
  image.source_bit_depth = half_exact ? 16 : 32;
  return image;
}

/// Names of the header attributes of a single-part file.
std::vector<std::string> attribute_names(const Bytes& file) {
  std::vector<std::string> names;
  util::ByteReader r(file);
  r.skip(8);
  while (true) {
    const std::string name(r.cstring());
    if (!r.ok() || name.empty()) break;
    (void)r.cstring();
    const std::int32_t size = r.i32le();
    r.skip(static_cast<std::size_t>(size));
    names.push_back(name);
  }
  return names;
}

/// Offset of the line offset table of a single-part file.
std::size_t table_offset(const Bytes& file) {
  util::ByteReader r(file);
  r.skip(8);
  while (true) {
    const std::string_view name = r.cstring();
    if (name.empty()) break;
    (void)r.cstring();
    r.skip(static_cast<std::size_t>(r.i32le()));
  }
  return r.position();
}

void put64(Bytes& file, std::size_t at, std::uint64_t value) {
  for (std::size_t i = 0; i < 8; ++i) file[at + i] = static_cast<std::byte>((value >> (8 * i)) & 0xFFu);
}

}  // namespace

TEST_CASE("image_modern/exr: lossless round trips for every writable compression") {
  const ir::Image halfs = hdr_image(67, 45, true);
  const ir::Image floats = hdr_image(67, 45, false);
  for (const char* method : {"none", "rle", "zips", "zip", "piz"}) {
    INFO(method);
    const Bytes h = encode(halfs, std::string(R"({"compression":")") + method + "\"}");
    const ir::Image back = decode(h);
    REQUIRE(back.hdr.has_value());
    CHECK(back.hdr->rgba == halfs.hdr->rgba);
    CHECK_EQ(back.source_bit_depth, 16);
    const Bytes f = encode(floats, std::string(R"({"pixel_type":"float","compression":")") + method + "\"}");
    const ir::Image back_f = decode(f);
    REQUIRE(back_f.hdr.has_value());
    CHECK(back_f.hdr->rgba == floats.hdr->rgba);
    CHECK_EQ(back_f.source_bit_depth, 32);
  }
  // Half output of float data rounds to the nearest half.
  const ir::Image rounded = decode(encode(floats));
  REQUIRE(rounded.hdr.has_value());
  double worst = 0;
  for (std::size_t i = 0; i < floats.hdr->rgba.size(); ++i) {
    const double v = floats.hdr->rgba[i];
    worst = std::max(worst, std::fabs(rounded.hdr->rgba[i] - v) / std::max(std::fabs(v), 6.2e-5));
  }
  CHECK_LE(worst, 1.0 / 2048);
}

TEST_CASE("image_modern/exr: alpha, grey and 8-bit sources") {
  // Premultiplied storage keeps straight colour exactly for these values.
  ir::Image alpha = hdr_image(33, 20, true);
  for (std::size_t i = 0; i < alpha.hdr->rgba.size(); i += 4) {
    const std::size_t p = i / 4;
    alpha.hdr->rgba[i] = static_cast<float>(p % 29) / 8.0f;
    alpha.hdr->rgba[i + 1] = static_cast<float>(p % 7) / 4.0f;
    alpha.hdr->rgba[i + 2] = 0.5f;
    alpha.hdr->rgba[i + 3] = static_cast<float>(p % 4 + 1) / 4.0f;
  }
  alpha.frames[0] = image::tone_map(*alpha.hdr);
  const Bytes a = encode(alpha);
  CHECK(attribute_names(a).size() == 8u);
  const ir::Image back = decode(a);
  REQUIRE(back.hdr.has_value());
  CHECK(back.hdr->rgba == alpha.hdr->rgba);

  // Grey pictures are stored as a single Y channel.
  ir::Image grey = hdr_image(16, 9, true);
  for (std::size_t i = 0; i < grey.hdr->rgba.size(); i += 4) grey.hdr->rgba[i + 1] = grey.hdr->rgba[i + 2] = grey.hdr->rgba[i];
  grey.frames[0] = image::tone_map(*grey.hdr);
  const Bytes g = encode(grey);
  const std::string text(as_chars(g));
  CHECK(text.find(std::string("Y\0\1\0\0\0", 6)) != std::string::npos);
  CHECK(text.find(std::string("R\0\1\0\0\0", 6)) == std::string::npos);
  const ir::Image grey_back = decode(g);
  REQUIRE(grey_back.hdr.has_value());
  CHECK(grey_back.hdr->rgba == grey.hdr->rgba);
  CHECK(grey_back.source_grayscale);
  CHECK(g.size() < encode(hdr_image(16, 9, true)).size());

  // 8-bit sRGB pictures go through linear half floats and come back intact.
  for (const ir::Frame& frame : {test::gradient_frame(40, 24), test::alpha_gradient_frame(40, 24)}) {
    ir::Image image;
    image.frames.push_back(frame);
    const ir::Image back8 = decode(encode(image));
    REQUIRE(back8.valid());
    CHECK_LE(test::max_abs_diff(back8.frames[0], frame), 1.0);
    CHECK_GE(test::psnr(back8.frames[0], frame), 55.0);
  }
}

TEST_CASE("image_modern/exr: options and header contents") {
  const ir::Image image = hdr_image(64, 48, true);
  const Bytes zip = encode(image);
  const Bytes none = encode(image, R"({"compression":"none"})");
  const Bytes piz = encode(image, R"({"compression":"piz"})");
  const Bytes fast = encode(image, R"({"compression_level":1})");
  const Bytes small = encode(image, R"({"preset":"small"})");
  const Bytes floats = encode(image, R"({"pixel_type":"float"})");
  CHECK_LT(zip.size(), none.size());
  CHECK_LT(piz.size(), none.size());
  CHECK_LE(small.size(), fast.size());
  CHECK_GT(floats.size(), zip.size());
  // Exactly the required attributes, sorted, and nothing that identifies
  // the machine, the user or the time.
  const std::vector<std::string> expected = {"channels",  "compression",      "dataWindow",         "displayWindow",
                                             "lineOrder", "pixelAspectRatio", "screenWindowCenter", "screenWindowWidth"};
  CHECK(attribute_names(zip) == expected);
  CHECK(attribute_names(piz) == expected);
  // Size options apply.
  const ir::Image half_size = decode(encode(image, R"({"scale":0.5})"));
  CHECK_EQ(half_size.width(), 32u);
  CHECK_EQ(half_size.height(), 24u);
  CHECK_FALSE(test::write_from("exr", image, R"({"compression":"dwaa"})").has_value());
  // A multi-frame image keeps the first frame, with a warning.
  ir::Image two = image;
  two.frames.push_back(two.frames[0]);
  std::vector<std::string> warnings;
  (void)encode(two, {}, &warnings);
  CHECK_EQ(warnings.size(), 1u);
}

TEST_CASE("image_modern/exr: same-format conversion") {
  // A stored (uncompressed) file shrinks and keeps every sample.
  const Bytes stored = test::fixture("image_modern/exr/rgb_half_none.exr");
  const test::Converted out = test::convert(stored, "", "exr");
  REQUIRE(out.ok());
  CHECK_LT(out.data.size(), stored.size() / 2);
  CHECK(decode(out.data).hdr->rgba == decode(stored).hdr->rgba);
  // DWA (lossy) input is re-stored losslessly from its decoded samples.
  const Bytes dwa = test::fixture("image_modern/exr/rgb_half_dwaa.exr");
  const test::Converted from_dwa = test::convert(dwa, "exr", "exr", R"({"compression":"piz"})");
  REQUIRE(from_dwa.ok());
  CHECK(decode(from_dwa.data).hdr->rgba == decode(dwa).hdr->rgba);
  // To and from other formats.
  const test::Converted png = test::convert(test::fixture("image/png/rgba8.png"), "", "exr");
  REQUIRE(png.ok());
  CHECK_EQ(png.format_id, "exr");
  const test::Converted hdr = test::convert(stored, "", "hdr");
  CHECK(hdr.ok());
}

TEST_CASE("image_modern/exr: truncated, corrupt and garbage input fails cleanly") {
  std::size_t tried = 0;
  for (const char* name : {"rgb_half_rle.exr", "rgb_half_zip.exr", "rgb_half_piz.exr", "rgb_half_pxr24.exr",
                           "noise_b44.exr", "rgb_half_b44a.exr", "rgbaz_dwaa.exr", "noise_float_dwab.exr",
                           "tiled_zip.exr", "multipart.exr", "luma_chroma_piz.exr", "ripmap_rle.exr"}) {
    INFO(name);
    tried += test::fuzz_reader<ir::Image>("exr", test::fixture(std::string("image_modern/exr/") + name), 256);
  }
  tried += test::fuzz_reader<ir::Image>("exr", encode(hdr_image(40, 40, true), R"({"compression":"piz"})"), 256);
  CHECK_GT(tried, 1000u);
  CHECK_FALSE(test::read_as<ir::Image>("exr", test::bytes("v/1\x01")).has_value());
}

TEST_CASE("image_modern/exr: damaged chunk tables and hostile sizes") {
  Bytes file = test::fixture("image_modern/exr/rgb_half_zip.exr");  // 2 chunks (16 + 7 lines)
  const std::size_t table = table_offset(file);
  // One bad offset: the rest decodes, with a warning; the missing lines are transparent.
  Bytes one = file;
  put64(one, table + 8, 1ull << 40);
  std::vector<std::string> warnings;
  const ir::Image partial = decode(one, &warnings);
  REQUIRE(partial.hdr.has_value());
  CHECK_EQ(warnings.size(), 1u);
  CHECK_EQ(partial.hdr->rgba[(std::size_t{20} * 37) * 4 + 3], 0.0f);
  CHECK_EQ(partial.hdr->rgba[3], 1.0f);
  // Every offset bad, or pointing into the header: an error.
  Bytes none = file;
  put64(none, table, 0);
  put64(none, table + 8, 8);
  CHECK_FALSE(test::read_as<ir::Image>("exr", none).has_value());
  // A truncated file keeps the chunks it still has.
  const Bytes cut(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(file.size() - 10));
  warnings.clear();
  const ir::Image shorter = decode(cut, &warnings);
  CHECK(shorter.valid());
  CHECK_EQ(warnings.size(), 1u);

  // Absurd data windows are refused before anything is allocated.
  const auto with_window = [&](std::int32_t xmax, std::int32_t ymax) {
    Bytes f = file;
    const std::size_t at = std::string_view(as_chars(f)).find("dataWindow") + 11 + 6 + 4;
    for (const auto& [offset, value] : {std::pair{8u, xmax}, std::pair{12u, ymax}}) {
      for (std::size_t i = 0; i < 4; ++i) {
        f[at + offset + i] = static_cast<std::byte>((static_cast<std::uint32_t>(value) >> (8 * i)) & 0xFFu);
      }
    }
    return f;
  };
  for (const auto& [xmax, ymax] : {std::pair{0x7FFFFFF0, 22}, std::pair{36, 0x7FFFFFF0}, std::pair{-5, 22},
                                   std::pair{65535, 65535}}) {
    INFO(xmax, "x", ymax);
    const auto result = test::read_as<ir::Image>("exr", with_window(xmax, ymax));
    REQUIRE_FALSE(result.has_value());
    CHECK((result.error().code == Status::Decode || result.error().code == Status::Limit));
  }

  // Limits: pixels and decompressed bytes.
  Limits small_pixels;
  small_pixels.max_pixels = 100;
  auto limited = test::read_as<ir::Image>("exr", file, {}, nullptr, small_pixels);
  REQUIRE_FALSE(limited.has_value());
  CHECK_EQ(limited.error().code, Status::Limit);
  Limits small_inflate;
  small_inflate.max_decompressed_bytes = 1000;
  limited = test::read_as<ir::Image>("exr", file, {}, nullptr, small_inflate);
  REQUIRE_FALSE(limited.has_value());
  CHECK_EQ(limited.error().code, Status::Limit);
}

TEST_CASE("image_modern/exr: cancellation") {
  const Codec* codec = test::find_reader("exr", Kind::Image);
  REQUIRE(codec != nullptr);
  const Bytes data = test::fixture("image_modern/exr/rgb_half_zips.exr");
  ReadContext ctx;
  ctx.registry = &Registry::instance();
  ctx.input = data;
  ctx.format_id = "exr";
  ctx.progress = Progress([](double, std::string_view) { return true; });
  const auto result = codec->read(ctx);
  REQUIRE_FALSE(result.has_value());
  CHECK_EQ(result.error().code, Status::Cancelled);
}
