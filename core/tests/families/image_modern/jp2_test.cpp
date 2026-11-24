// Tests of the JPEG 2000 codecs (jp2, j2k, jpx, jpf).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "engine/registry.hpp"
#include "families/image/image_api.hpp"
#include "families/image/image_ops.hpp"
#include "ir/image.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"
#include "util/bytes.hpp"

using namespace cvt;
using namespace std::string_view_literals;

namespace {

Bytes fx(std::string_view name) { return test::fixture("image_modern/jp2/" + std::string(name)); }

ir::Image load(std::string_view format, std::string_view name, std::vector<std::string>* warnings = nullptr) {
  auto image = test::read_as<ir::Image>(format, fx(name), {}, warnings);
  if (!image) {
    FAIL_CHECK("decoding ", name, ": ", image.error().message);
    return {};
  }
  return std::move(*image);
}

/// Binary PPM (P6, maxval 255) as written by opj_decompress and make_sources.py.
ir::Frame ppm(std::string_view name) {
  const Bytes data = fx(name);
  const std::string_view text = as_chars(data);
  std::size_t pos = 2;
  const auto number = [&]() -> std::uint32_t {
    while (pos < text.size()) {
      if (text[pos] == '#') {
        while (pos < text.size() && text[pos] != '\n') ++pos;
      } else if (text[pos] == ' ' || text[pos] == '\n' || text[pos] == '\r' || text[pos] == '\t') {
        ++pos;
      } else {
        break;
      }
    }
    std::uint32_t v = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') v = v * 10 + static_cast<std::uint32_t>(text[pos++] - '0');
    return v;
  };
  ir::Frame frame;
  if (text.substr(0, 2) != "P6") return frame;
  const std::uint32_t w = number();
  const std::uint32_t h = number();
  if (number() != 255) return frame;
  ++pos;
  if (text.size() < pos + std::size_t{w} * h * 3) return frame;
  frame = ir::Frame::filled(w, h, {0, 0, 0, 255});
  for (std::size_t i = 0; i < std::size_t{w} * h; ++i) {
    for (std::size_t c = 0; c < 3; ++c) frame.rgba[i * 4 + c] = static_cast<std::uint8_t>(text[pos + i * 3 + c]);
  }
  return frame;
}

/// 16-bit binary PGM (P5, maxval 65535): the samples, row-major.
std::vector<std::uint16_t> pgm16(std::string_view name, std::uint32_t& w, std::uint32_t& h) {
  const Bytes data = fx(name);
  util::ByteReader r(data);
  std::vector<std::uint16_t> out;
  if (!r.consume("P5\n")) return out;
  std::string header;
  int fields = 0;
  std::uint32_t values[3] = {0, 0, 0};
  while (fields < 3 && !r.at_end()) {
    const char c = static_cast<char>(r.u8());
    if (c >= '0' && c <= '9') {
      values[fields] = values[fields] * 10 + static_cast<std::uint32_t>(c - '0');
    } else if (values[fields] != 0) {
      ++fields;
    }
  }
  w = values[0];
  h = values[1];
  for (std::size_t i = 0; i < std::size_t{w} * h; ++i) out.push_back(r.u16be());
  if (!r.ok()) out.clear();
  return out;
}

/// The 16-bit sample (0..65535) behind channel `c` of pixel `i`, recovered from `hdr`.
std::uint16_t wide_sample(const ir::Image& image, std::size_t i, std::size_t c) {
  if (c == 3) return static_cast<std::uint16_t>(std::lround(image.hdr->rgba[i * 4 + 3] * 65535.0f));
  return image::linear_to_srgb16(image.hdr->rgba[i * 4 + c]);
}

ir::Image single(ir::Frame frame) {
  ir::Image image;
  image.frames.push_back(std::move(frame));
  return image;
}

ir::Frame photo(std::uint32_t w, std::uint32_t h) {
  ir::Frame frame = test::gradient_frame(w, h);
  const Bytes noise = test::random_bytes(frame.rgba.size(), 17);
  for (std::size_t i = 0; i < frame.rgba.size(); ++i) {
    if (i % 4 == 3) continue;
    const int v = frame.rgba[i] + std::to_integer<int>(noise[i]) % 21 - 10;
    frame.rgba[i] = static_cast<std::uint8_t>(std::clamp(v, 0, 255));
  }
  return frame;
}

/// Position of the first marker `code` in a codestream, or npos.
std::size_t marker(ByteView cs, std::uint16_t code) {
  for (std::size_t i = 0; i + 1 < cs.size(); ++i) {
    if (std::to_integer<unsigned>(cs[i]) == (code >> 8u) && std::to_integer<unsigned>(cs[i + 1]) == (code & 0xFFu)) {
      return i;
    }
  }
  return std::string_view::npos;
}

std::uint32_t be32(ByteView b, std::size_t at) {
  return std::to_integer<std::uint32_t>(b[at]) << 24 | std::to_integer<std::uint32_t>(b[at + 1]) << 16 |
         std::to_integer<std::uint32_t>(b[at + 2]) << 8 | std::to_integer<std::uint32_t>(b[at + 3]);
}

/// The codestream inside a JP2 file (after the 'jp2c' box header).
ByteView codestream_of(ByteView file) {
  const std::size_t at = as_chars(file).find("jp2c");
  return at == std::string_view::npos ? ByteView{} : file.subspan(at + 4);
}

bool contains(ByteView data, std::string_view needle) { return as_chars(data).find(needle) != std::string_view::npos; }

}  // namespace

// --------------------------------------------------------------------------- decoding files made by other software

TEST_CASE("image_modern/jp2: lossless 5/3 files from opj_compress decode exactly") {
  const ir::Frame source = ppm("src_rgb.ppm");
  REQUIRE(source.valid());
  const ir::Image jp2 = load("jp2", "rgb_lossless.jp2");
  REQUIRE(jp2.valid());
  CHECK(test::pixels_equal(jp2.frames[0], source));
  CHECK(jp2.source_bit_depth == 8);
  CHECK(!jp2.hdr);
  // Tiles of 16x16, three quality layers (last one lossless), RPCL order.
  const ir::Image tiled = load("j2k", "tiles_layers_rpcl.j2k");
  CHECK(test::pixels_equal(tiled.frames[0], source));
  // A JPX file (brand 'jpx ', reader requirements box) around the same codestream.
  const ir::Image jpx = load("jpx", "rgb.jpx");
  CHECK(test::pixels_equal(jpx.frames[0], source));
}

TEST_CASE("image_modern/jp2: lossy 9/7 files match the opj_decompress reference") {
  const ir::Frame source = ppm("src_rgb.ppm");
  const ir::Image lossy = load("jp2", "rgb_lossy97.jp2");
  const double vs_reference = test::psnr(lossy.frames[0], ppm("rgb_lossy97.ref.ppm"));
  INFO("PSNR vs opj_decompress: ", vs_reference);
  CHECK(vs_reference >= 50.0);
  CHECK(test::psnr(lossy.frames[0], source) >= 22.0);  // compression ratio 12 on a noisy pattern

  const ir::Image layered = load("jp2", "cprl_layers97.jp2");
  const double layered_vs_reference = test::psnr(layered.frames[0], ppm("cprl_layers97.ref.ppm"));
  INFO("PSNR vs opj_decompress (CPRL, 3 layers): ", layered_vs_reference);
  CHECK(layered_vs_reference >= 50.0);
  CHECK(test::psnr(layered.frames[0], source) >= 38.0);  // last layer targets 44 dB
}

TEST_CASE("image_modern/jp2: 16-bit greyscale keeps every bit in hdr") {
  std::uint32_t w = 0, h = 0;
  const std::vector<std::uint16_t> source = pgm16("src_gray16.pgm", w, h);
  REQUIRE(source.size() == std::size_t{40} * 32);
  // Decoded through the image API, as the PDF and DICOM codecs do.
  Context ctx;
  ctx.registry = &Registry::instance();
  const auto image = image::decode("j2k", fx("gray16.j2k"), ctx);
  REQUIRE_OK(image);
  REQUIRE(image->width() == w);
  REQUIRE(image->hdr.has_value());
  CHECK(image->source_bit_depth == 16);
  CHECK(image->source_grayscale);
  std::size_t mismatches = 0;
  for (std::size_t i = 0; i < source.size(); ++i) {
    if (wide_sample(*image, i, 0) != source[i]) ++mismatches;
    // The 8-bit frame is the same value rounded.
    if (image->frames[0].rgba[i * 4] != static_cast<std::uint8_t>((source[i] * 255u + 32767u) / 65535u)) ++mismatches;
  }
  CHECK(mismatches == 0);
}

TEST_CASE("image_modern/jp2: signed 12-bit samples are offset and recoverable") {
  const Bytes raw = fx("src_s12_32x24.raw");
  REQUIRE(raw.size() == std::size_t{32} * 24 * 2);
  const ir::Image image = load("j2k", "s12_signed.j2k");
  REQUIRE(image.hdr.has_value());
  CHECK(image.source_bit_depth == 12);
  std::size_t mismatches = 0;
  for (std::size_t i = 0; i < std::size_t{32} * 24; ++i) {
    const auto original = static_cast<std::int16_t>(std::to_integer<std::uint16_t>(raw[i * 2]) << 8 |
                                                    std::to_integer<std::uint16_t>(raw[i * 2 + 1]));
    const std::uint32_t v16 = wide_sample(image, i, 0);
    const int recovered = static_cast<int>((v16 * 4095u + 32767u) / 65535u) - 2048;
    if (recovered != original) ++mismatches;
    // In the 8-bit frame, -2048 is black and 0 is mid-grey.
    const int expected8 = ((original + 2048) * 255 + 2047) / 4095;
    if (image.frames[0].rgba[i * 4] != expected8) ++mismatches;
  }
  CHECK(mismatches == 0);
}

TEST_CASE("image_modern/jp2: sYCC 4:2:0 converts to RGB like opj_decompress") {
  // Raw codestream: recognised as YCbCr by its sub-sampled chroma components.
  const ir::Image raw = load("j2k", "ycc420.j2k");
  const double raw_psnr = test::psnr(raw.frames[0], ppm("ycc420.ref.ppm"));
  INFO("j2k PSNR vs opj_decompress: ", raw_psnr);
  CHECK(raw_psnr >= 38.0);
  // JP2 with colour specification 18 (sYCC).
  const ir::Image file = load("jp2", "sycc420.jp2");
  const double file_psnr = test::psnr(file.frames[0], ppm("sycc420.ref.ppm"));
  INFO("jp2 PSNR vs opj_decompress: ", file_psnr);
  CHECK(file_psnr >= 38.0);
  CHECK(test::psnr(raw.frames[0], file.frames[0]) >= 60.0);
}

TEST_CASE("image_modern/jp2: cdef alpha channel and pclr palette") {
  const auto png = test::read_as<ir::Image>("png", fx("src_rgba.png"));
  REQUIRE_OK(png);
  const ir::Image alpha = load("jp2", "rgba_cdef.jp2");
  CHECK(test::pixels_equal(alpha.frames[0], png->frames[0]));
  CHECK(alpha.frames[0].rgba[3] == 0);  // fully transparent corner

  const ir::Image palette = load("jp2", "palette.jp2");
  const ir::Frame reference = ppm("palette.ref.ppm");
  CHECK(test::pixels_equal(palette.frames[0], reference));
  CHECK(palette.frames[0].rgba[0] == 0);
  CHECK(palette.frames[0].rgba[1] == 40);
  CHECK(palette.frames[0].rgba[2] == 255);
}

TEST_CASE("image_modern/jp2: restricted ICC profile and resolution box") {
  const ir::Image image = load("jp2", "icc_res.jp2");
  CHECK(test::pixels_equal(image.frames[0], ppm("src_rgb.ppm")));
  REQUIRE(image.icc_profile.size() == 132);
  CHECK(as_chars(ByteView(image.icc_profile).subspan(36, 4)) == "acsp");
  CHECK_APPROX(image.dpi_x, 300.0, 0.05);
  CHECK_APPROX(image.dpi_y, 150.0, 0.05);
}

TEST_CASE("image_modern/jp2: files written by macOS ImageIO (sips) match its own decode") {
  const auto rgb_ref = test::read_as<ir::Image>("png", fx("sips_rgb.ref.png"));
  REQUIRE_OK(rgb_ref);
  const ir::Image rgb = load("jp2", "sips_rgb.jp2");
  const double rgb_psnr = test::psnr(rgb.frames[0], rgb_ref->frames[0]);
  INFO("PSNR vs sips: ", rgb_psnr);
  CHECK(rgb_psnr >= 45.0);
  CHECK(!rgb.icc_profile.empty());  // sips embeds an sRGB profile
  CHECK(test::psnr(rgb.frames[0], ppm("src_rgb.ppm")) >= 22.0);  // sips encodes this noisy pattern coarsely

  const auto rgba_ref = test::read_as<ir::Image>("png", fx("sips_rgba.ref.png"));
  REQUIRE_OK(rgba_ref);
  const ir::Image rgba = load("jp2", "sips_rgba.jp2");
  const double rgba_psnr = test::psnr(rgba.frames[0], rgba_ref->frames[0]);
  INFO("PSNR vs sips (RGBA): ", rgba_psnr);
  CHECK(rgba_psnr >= 40.0);
  CHECK(rgba.frames[0].has_transparency());
}

// --------------------------------------------------------------------------- encoding

TEST_CASE("image_modern/jp2: lossless writing is exact for RGB, RGBA, grey and 16 bits") {
  ir::Frame rgba = photo(45, 33);
  for (std::size_t i = 3; i < rgba.rgba.size(); i += 8) rgba.rgba[i] = static_cast<std::uint8_t>(i % 256);
  ir::Frame grey = photo(30, 20);
  image::to_grayscale(grey);
  for (const std::string_view id : {"jp2"sv, "j2k"sv, "jpx"sv, "jpf"sv}) {
    for (const ir::Frame& frame : {photo(45, 33), rgba, grey}) {
      INFO(id, " ", frame.width, "x", frame.height);
      const auto encoded = test::write_from(id, single(frame), R"({"lossless":true})");
      REQUIRE_OK(encoded);
      const auto decoded = test::read_as<ir::Image>(id, *encoded);
      REQUIRE_OK(decoded);
      CHECK(test::pixels_equal(decoded->frames[0], frame));
    }
  }
  // 16-bit source (e.g. from a 16-bit PNG): written at 16 bits, read back exactly.
  ir::Image wide;
  std::uint32_t w = 0, h = 0;
  const std::vector<std::uint16_t> source = pgm16("src_gray16.pgm", w, h);
  std::vector<std::uint16_t> rgba16;
  for (const std::uint16_t v : source) {
    rgba16.insert(rgba16.end(), {v, static_cast<std::uint16_t>(65535 - v), v, 65535});
  }
  wide.hdr = image::linear_from_srgb16(rgba16, w, h);
  wide.frames.push_back(image::from_linear(*wide.hdr));
  wide.source_bit_depth = 16;
  const auto encoded = test::write_from("jp2", wide, R"({"lossless":true})");
  REQUIRE_OK(encoded);
  const auto decoded = test::read_as<ir::Image>("jp2", *encoded);
  REQUIRE_OK(decoded);
  REQUIRE(decoded->hdr.has_value());
  CHECK(decoded->source_bit_depth == 16);
  std::size_t mismatches = 0;
  for (std::size_t i = 0; i < source.size(); ++i) {
    for (std::size_t c = 0; c < 4; ++c) mismatches += wide_sample(*decoded, i, c) != rgba16[i * 4 + c] ? 1u : 0u;
  }
  CHECK(mismatches == 0);
}

TEST_CASE("image_modern/jp2: lossy quality levels meet PSNR/SSIM floors and shrink") {
  const ir::Image original = single(photo(96, 80));
  struct Level {
    int quality;
    double min_psnr;
    double min_ssim;
  };
  // quality 90 -> 42 dB target: visually transparent.
  const Level levels[] = {{100, 42.0, 0.99}, {90, 40.0, 0.98}, {75, 37.0, 0.95}, {50, 32.0, 0.85}, {20, 26.0, 0.6}};
  std::size_t previous = SIZE_MAX;
  for (const Level& level : levels) {
    const auto encoded = test::write_from("jp2", original, std::format(R"({{"quality":{}}})", level.quality));
    REQUIRE_OK(encoded);
    const auto decoded = test::read_as<ir::Image>("jp2", *encoded);
    REQUIRE_OK(decoded);
    const double psnr = test::psnr(*decoded, original);
    const double ssim = test::ssim(*decoded, original);
    INFO("quality ", level.quality, ": ", encoded->size(), " bytes, PSNR ", psnr, ", SSIM ", ssim);
    CHECK(psnr >= level.min_psnr);
    CHECK(ssim >= level.min_ssim);
    CHECK(encoded->size() < previous);
    previous = encoded->size();
  }
  const auto lossless = test::write_from("jp2", original, R"({"lossless":true})");
  REQUIRE_OK(lossless);
  const auto q90 = test::write_from("jp2", original, R"({"quality":90})");
  REQUIRE_OK(q90);
  CHECK(q90->size() < lossless->size());
}

TEST_CASE("image_modern/jp2: writer options shape the codestream") {
  const ir::Image original = single(photo(80, 64));
  SUBCASE("layers") {
    const auto encoded = test::write_from("j2k", original, R"({"layers":4})");
    REQUIRE_OK(encoded);
    const std::size_t cod = marker(*encoded, 0xFF52);
    REQUIRE(cod != std::string_view::npos);
    // COD: Lcod(2) Scod(1) progression(1) layers(2)
    CHECK(std::to_integer<int>((*encoded)[cod + 6]) * 256 + std::to_integer<int>((*encoded)[cod + 7]) == 4);
    const auto decoded = test::read_as<ir::Image>("j2k", *encoded);
    REQUIRE_OK(decoded);
    CHECK(test::psnr(*decoded, original) >= 40.0);
  }
  SUBCASE("progression") {
    const auto encoded = test::write_from("j2k", original, R"({"progression":"RPCL"})");
    REQUIRE_OK(encoded);
    const std::size_t cod = marker(*encoded, 0xFF52);
    REQUIRE(cod != std::string_view::npos);
    CHECK(std::to_integer<int>((*encoded)[cod + 5]) == 2);  // 0 LRCP, 1 RLCP, 2 RPCL
    CHECK_OK(test::read_as<ir::Image>("j2k", *encoded));
  }
  SUBCASE("tile_size") {
    const auto encoded = test::write_from("j2k", original, R"({"tile_size":32,"lossless":true})");
    REQUIRE_OK(encoded);
    CHECK(be32(*encoded, 4 + 2 + 2 + 16) == 32);  // XTsiz
    const auto decoded = test::read_as<ir::Image>("j2k", *encoded);
    REQUIRE_OK(decoded);
    CHECK(test::pixels_equal(*decoded, original));
  }
  SUBCASE("bit_depth") {
    const auto encoded = test::write_from("j2k", original, R"({"bit_depth":"16","lossless":true})");
    REQUIRE_OK(encoded);
    // Without HDR pixels there is nothing to widen: 8 bits stay 8 bits.
    CHECK(std::to_integer<int>((*encoded)[42]) == 7);
  }
  SUBCASE("max_width") {
    const auto encoded = test::write_from("jp2", original, R"({"max_width":40})");
    REQUIRE_OK(encoded);
    const auto decoded = test::read_as<ir::Image>("jp2", *encoded);
    REQUIRE_OK(decoded);
    CHECK(decoded->width() == 40);
    CHECK(decoded->height() == 32);
  }
  SUBCASE("invalid choice") {
    CHECK_STATUS(test::write_from("jp2", original, R"({"progression":"XYZ"})"), Status::InvalidArgument);
  }
}

TEST_CASE("image_modern/jp2: boxes, metadata and privacy") {
  ir::Image image = single(photo(24, 16));
  image.frames[0].rgba[3] = 10;
  image.dpi_x = 300.0;
  image.dpi_y = 72.0;
  image.xmp = R"(<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF/></x:xmpmeta>)";
  // A minimal TIFF-structured EXIF block with no entries.
  image.exif = test::bytes("MM\x00\x2A\x00\x00\x00\x08\x00\x00\x00\x00\x00\x00"sv);
  const Bytes icc = test::fixture("image_modern/jp2/icc_res.jp2");
  const auto stripped = test::write_from("jp2", image);
  REQUIRE_OK(stripped);
  CHECK(contains(*stripped, "cdef"));
  CHECK(contains(*stripped, "resc"));
  CHECK(!contains(*stripped, "xmpmeta"));
  const auto decoded = test::read_as<ir::Image>("jp2", *stripped);
  REQUIRE_OK(decoded);
  CHECK_APPROX(decoded->dpi_x, 300.0, 0.01);
  CHECK_APPROX(decoded->dpi_y, 72.0, 0.01);
  CHECK(std::abs(decoded->frames[0].rgba[3] - 10) <= 3);  // lossy by default

  const ir::Image with_profile = load("jp2", "icc_res.jp2");
  ir::Image kept_source = image;
  kept_source.icc_profile = with_profile.icc_profile;
  const auto kept = test::write_from("jpx", kept_source, R"({"strip_metadata":false})");
  REQUIRE_OK(kept);
  CHECK(contains(*kept, "xmpmeta"));
  CHECK(contains(*kept, "JpgTiffExif->JP2"));
  CHECK(contains(*kept, "rreq"));
  const auto back = test::read_as<ir::Image>("jpx", *kept);
  REQUIRE_OK(back);
  CHECK(back->icc_profile == with_profile.icc_profile);
  CHECK(back->xmp == image.xmp);
  CHECK(!back->exif.empty());
  // Brands
  CHECK(as_chars(ByteView(*kept).subspan(20, 4)) == "jpx ");
  CHECK(as_chars(ByteView(*stripped).subspan(20, 4)) == "jp2 ");
  const auto raw = test::write_from("j2k", image);
  REQUIRE_OK(raw);
  CHECK(as_chars(*raw).substr(0, 4) == "\xFF\x4F\xFF\x51"sv);
  (void)icc;
}

TEST_CASE("image_modern/jp2: raw codestreams and JP2 files decode through the image API under any id") {
  Context ctx;
  ctx.registry = &Registry::instance();
  const Bytes file = fx("rgb_lossless.jp2");
  const auto a = image::decode("j2k", codestream_of(file), ctx);
  const auto b = image::decode("jp2", file, ctx);
  const auto c = image::decode("jp2", codestream_of(file), ctx);  // PDF JPXDecode streams may be either
  REQUIRE_OK(a);
  REQUIRE_OK(b);
  REQUIRE_OK(c);
  CHECK(test::pixels_equal(*a, *b));
  CHECK(test::pixels_equal(*a, *c));
  CHECK(image::can_encode("jp2", ctx));
}

// --------------------------------------------------------------------------- hostile input, limits, cancellation

TEST_CASE("image_modern/jp2: damaged files are errors, never crashes") {
  for (const std::string_view name : {"rgb_lossless.jp2"sv, "rgb_lossy97.jp2"sv, "palette.jp2"sv, "rgba_cdef.jp2"sv,
                                      "s12_signed.j2k"sv, "sycc420.jp2"sv, "rgb.jpx"sv}) {
    INFO(name);
    CHECK(test::fuzz_reader<ir::Image>("jp2", fx(name), 300) > 0);
  }
  CHECK(test::fuzz_reader<ir::Image>("j2k", fx("tiles_layers_rpcl.j2k"), 300) > 0);
  const Bytes file = fx("rgb_lossless.jp2");
  CHECK_STATUS(test::read_as<ir::Image>("jp2", ByteView(file).first(60)), Status::Decode);  // header only
  CHECK_STATUS(test::read_as<ir::Image>("jp2", test::bytes("not a picture")), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Image>("jp2", ByteView(file).first(12)), Status::Decode);
  // A codestream cut inside the tile data still decodes partially, with a warning.
  std::vector<std::string> warnings;
  const auto partial = test::read_as<ir::Image>("jp2", ByteView(file).first(file.size() - 400), {}, &warnings);
  if (partial) CHECK(!warnings.empty());
}

TEST_CASE("image_modern/jp2: absurd headers are refused before allocating") {
  const Bytes file = fx("rgb_lossless.jp2");
  const ByteView view = codestream_of(file);
  Bytes cs(view.begin(), view.end());
  REQUIRE(cs.size() > 60);
  const auto put32 = [&](std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) cs[at + i] = static_cast<std::byte>(v >> (24 - 8 * i));
  };
  SUBCASE("huge image") {
    put32(8, 0x7FFFFFFF);   // Xsiz
    put32(12, 0x7FFFFFFF);  // Ysiz
    put32(24, 0x7FFFFFFF);  // XTsiz
    put32(28, 0x7FFFFFFF);  // YTsiz
    CHECK_STATUS(test::read_as<ir::Image>("j2k", cs), Status::Limit);
  }
  SUBCASE("too many tiles") {
    put32(8, 100000);
    put32(12, 100000);
    put32(24, 1);
    put32(28, 1);
    CHECK_STATUS(test::read_as<ir::Image>("j2k", cs), Status::Decode);
  }
  SUBCASE("limits option") {
    Limits limits;
    limits.max_pixels = 100;
    CHECK_STATUS(test::read_as<ir::Image>("jp2", fx("rgb_lossless.jp2"), {}, nullptr, limits), Status::Limit);
  }
  SUBCASE("broken geometry") {
    put32(16, 0x7FFFFFFF);  // XOsiz beyond Xsiz
    CHECK_STATUS(test::read_as<ir::Image>("j2k", cs), Status::Decode);
  }
}

TEST_CASE("image_modern/jp2: cancellation stops decoding and encoding") {
  Context ctx;
  ctx.registry = &Registry::instance();
  ctx.progress = Progress([](double, std::string_view) { return true; });
  CHECK_STATUS(image::decode("jp2", fx("rgb_lossless.jp2"), ctx), Status::Cancelled);
  CHECK_STATUS(image::encode("jp2", single(photo(64, 64)), Options{}, ctx), Status::Cancelled);
}

// --------------------------------------------------------------------------- integration

TEST_CASE("image_modern/jp2: detection, conversion and the compress path") {
  const Bytes jp2 = fx("rgb_lossless.jp2");
  CHECK(test::convert(jp2, "", "png").format_id == "png");
  const test::Converted detected = test::convert(jp2, "", "jp2", R"({"quality":60})", "picture.jp2");
  REQUIRE(detected.ok());
  CHECK(detected.data.size() < jp2.size());  // compress: re-encoded smaller
  const test::Converted to_j2k = test::convert(fx("rgb.jpx"), "", "j2k", {}, "x.jpx");
  CHECK(to_j2k.ok());
  CHECK(test::convert(fx("rgb.jpx"), "", "jpf", {}, "x.jpf").ok());
  CHECK(test::convert(fx("gray16.j2k"), "", "jp2", {}, "x.j2c").ok());

  const Registry& registry = Registry::instance();
  CHECK(registry.find("jp2")->sniff(jp2, "") >= 90);
  CHECK(registry.find("jpx")->sniff(jp2, "") == 0);
  CHECK(registry.find("jpx")->sniff(fx("rgb.jpx"), "a.jpx") >= 90);
  CHECK(registry.find("jpf")->sniff(fx("rgb.jpx"), "a.jpf") > registry.find("jpx")->sniff(fx("rgb.jpx"), "a.jpf"));
  CHECK(registry.find("jpf")->sniff(fx("rgb.jpx"), "a.jpx") == 0);
  for (const std::string& problem : registry.validate()) {
    if (problem.find("jp") != std::string::npos) FAIL_CHECK(problem);
  }
}
