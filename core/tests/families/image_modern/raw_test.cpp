// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// Camera RAW readers: DNG (CFA and linear; uncompressed, packed, lossless
// JPEG, Deflate float, lossy JPEG), maker containers (CR2, NEF, ARW2) and
// the preview-only containers (ORF, RW2, RAF, CR3, CRW). Fixtures are
// synthetic (see tests/data/image_modern/raw/SOURCES.md): four flat colour
// patches whose linear sRGB values are known exactly.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "engine/detect.hpp"
#include "families/image/image_api.hpp"
#include "families/image/image_ops.hpp"
#include "families/image_modern/raw_ljpeg.hpp"
#include "families/image_modern/raw_tiff.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"

using namespace cvt;

namespace {

using Rgb = std::array<double, 3>;

// Linear sRGB of the scene's four patches (top-left, top-right, bottom-left, bottom-right).
constexpr std::array<Rgb, 4> scene = {{{0.60, 0.25, 0.10}, {0.10, 0.45, 0.20}, {0.12, 0.18, 0.62}, {0.40, 0.40, 0.40}}};
// The same patches in white-balanced camera RGB (what maker files without a
// colour profile show) and in camera RGB before white balance.
constexpr std::array<Rgb, 4> camera_balanced = {{{0.42374, 0.25218, 0.14707},
                                                 {0.21111, 0.36899, 0.25020},
                                                 {0.21769, 0.26128, 0.51119},
                                                 {0.40000, 0.40000, 0.40000}}};
constexpr std::array<Rgb, 4> camera_unbalanced = {{{0.21187, 0.25218, 0.10295},
                                                   {0.10556, 0.36899, 0.17514},
                                                   {0.10884, 0.26128, 0.35783},
                                                   {0.20000, 0.40000, 0.28000}}};

std::string raw_fixture(std::string_view name) { return "image_modern/raw/" + std::string(name); }

ir::Image load(std::string_view format, std::string_view name, std::string_view options = {},
               std::vector<std::string>* warnings = nullptr) {
  auto image = test::read_as<ir::Image>(format, test::fixture(raw_fixture(name)), options, warnings);
  if (!image) {
    FAIL_CHECK("decoding ", name, ": ", image.error().message);
    return {};
  }
  return std::move(*image);
}

// Mean linear colour of a 3x3 window of the HDR pixels.
Rgb hdr_at(const ir::Image& image, std::uint32_t x, std::uint32_t y) {
  Rgb sum{};
  const ir::HdrPixels& p = *image.hdr;
  for (std::uint32_t dy = 0; dy < 3; ++dy) {
    for (std::uint32_t dx = 0; dx < 3; ++dx) {
      const std::size_t at = (std::size_t{y + dy - 1} * p.width + (x + dx - 1)) * 4;
      for (std::size_t c = 0; c < 3; ++c) sum[c] += p.rgba[at + c] / 9.0;
    }
  }
  return sum;
}

// Check the centres of the four quadrants against expected linear colours.
void check_patches(const ir::Image& image, const std::array<Rgb, 4>& expected, std::array<int, 4> order,
                   double tolerance) {
  REQUIRE(image.hdr.has_value());
  const std::uint32_t w = image.width(), h = image.height();
  const std::array<std::array<std::uint32_t, 2>, 4> centres = {
      {{w / 4, h / 4}, {w * 3 / 4, h / 4}, {w / 4, h * 3 / 4}, {w * 3 / 4, h * 3 / 4}}};
  for (std::size_t i = 0; i < 4; ++i) {
    const Rgb got = hdr_at(image, centres[i][0], centres[i][1]);
    const Rgb& want = expected[static_cast<std::size_t>(order[i])];
    for (std::size_t c = 0; c < 3; ++c) {
      INFO("quadrant ", i, " channel ", c);
      CHECK_APPROX(got[c], want[c], tolerance);
    }
    // The 8-bit rendition is the sRGB encoding of the clipped linear value.
    const std::size_t at = image.frames[0].offset(centres[i][0], centres[i][1]);
    for (std::size_t c = 0; c < 3; ++c) {
      const int expected8 = image::linear_to_srgb8(static_cast<float>(got[c]));
      CHECK(std::abs(int{image.frames[0].rgba[at + c]} - expected8) <= 2);
    }
  }
}

constexpr std::array<int, 4> upright = {0, 1, 2, 3};

// 16-bit binary PPM written by the reference decoder.
struct Ppm16 {
  std::uint32_t width = 0, height = 0;
  std::vector<double> rgb;  // 0..1
};
Ppm16 parse_ppm16(const Bytes& data) {
  Ppm16 out;
  const std::string_view text = as_chars(data);
  std::size_t pos = 2;
  const auto number = [&]() {
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\n')) ++pos;
    std::uint32_t v = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') v = v * 10 + static_cast<std::uint32_t>(text[pos++] - '0');
    return v;
  };
  if (text.substr(0, 2) != "P6") return out;
  out.width = number();
  out.height = number();
  if (number() != 65535) return {};
  ++pos;
  const std::size_t n = std::size_t{out.width} * out.height * 3;
  if (text.size() < pos + n * 2) return {};
  for (std::size_t i = 0; i < n; ++i) {
    const auto hi = static_cast<unsigned char>(text[pos + i * 2]);
    const auto lo = static_cast<unsigned char>(text[pos + i * 2 + 1]);
    out.rgb.push_back(((hi << 8) | lo) / 65535.0);
  }
  return out;
}

// PSNR (peak 1.0) of our linear output against the reference, which covers a
// larger area starting `ox`, `oy` pixels earlier.
double psnr_against(const ir::Image& image, const Ppm16& ref, std::uint32_t ox, std::uint32_t oy) {
  const ir::HdrPixels& p = *image.hdr;
  double sse = 0;
  std::size_t n = 0;
  for (std::uint32_t y = 0; y < p.height; ++y) {
    for (std::uint32_t x = 0; x < p.width; ++x) {
      for (std::size_t c = 0; c < 3; ++c) {
        const double ours = std::clamp(static_cast<double>(p.rgba[(std::size_t{y} * p.width + x) * 4 + c]), 0.0, 1.0);
        const double theirs = ref.rgb[(std::size_t{y + oy} * ref.width + (x + ox)) * 3 + c];
        sse += (ours - theirs) * (ours - theirs);
        ++n;
      }
    }
  }
  return sse == 0 ? 200.0 : 10.0 * std::log10(static_cast<double>(n) / sse);
}

bool has_warning(const std::vector<std::string>& warnings, std::string_view text) {
  return std::ranges::any_of(warnings, [&](const std::string& w) { return w.find(text) != std::string::npos; });
}

std::string detect(std::string_view name, std::string_view filename) {
  const Bytes data = test::fixture(raw_fixture(name));
  const auto found = detect_format(Registry::instance(), data, filename);
  return found ? found->format->id : std::string("(none)");
}

}  // namespace

// ============================================================================ DNG

TEST_CASE("image_modern/raw: DNG CFA 16-bit strips decode to the known scene") {
  std::vector<std::string> warnings;
  const ir::Image image = load("dng", "dng_cfa_u16_strips.dng", {}, &warnings);
  REQUIRE(image.valid());
  // Active area 60x38, default crop 2 px on each side.
  CHECK(image.width() == 56);
  CHECK(image.height() == 34);
  CHECK(image.source_bit_depth == 16);
  CHECK(image.metadata.at("Make") == "Synthetic");
  check_patches(image, scene, upright, 0.004);
  CHECK(warnings.empty());
}

TEST_CASE("image_modern/raw: DNG output matches LibRaw's development") {
  // LibRaw (independent decoder) output of the same files, linear sRGB, 16 bit,
  // covering the active area (LibRaw ignores DefaultCrop).
  const ir::Image strips = load("dng", "dng_cfa_u16_strips.dng");
  const Ppm16 ref1 = parse_ppm16(test::fixture(raw_fixture("dng_cfa_u16_strips.libraw.ppm")));
  REQUIRE(ref1.width == 60);
  CHECK(psnr_against(strips, ref1, 2, 2) > 33.0);
  const ir::Image tiles = load("dng", "dng_cfa_lj92_tiles.dng");
  const Ppm16 ref2 = parse_ppm16(test::fixture(raw_fixture("dng_cfa_lj92_tiles.libraw.ppm")));
  REQUIRE(ref2.width == 48);
  CHECK(psnr_against(tiles, ref2, 0, 0) > 33.0);
}

TEST_CASE("image_modern/raw: DNG lossless JPEG tiles with predictors 1-7 and restart markers") {
  const ir::Image image = load("dng", "dng_cfa_lj92_tiles.dng");
  REQUIRE(image.valid());
  CHECK(image.width() == 48);
  CHECK(image.height() == 32);
  CHECK(image.source_bit_depth == 14);
  check_patches(image, scene, upright, 0.004);
}

TEST_CASE("image_modern/raw: DNG 12-bit packed with linearisation, black pattern and orientation") {
  const ir::Image image = load("dng", "dng_cfa_packed12_rot90.dng");
  REQUIRE(image.valid());
  // 40x28 sensor rotated 90 degrees clockwise.
  CHECK(image.width() == 28);
  CHECK(image.height() == 40);
  CHECK(image.source_bit_depth == 12);
  check_patches(image, scene, {2, 0, 3, 1}, 0.006);
}

TEST_CASE("image_modern/raw: DNG X-Trans 6x6 pattern and dual-illuminant matrices") {
  const ir::Image image = load("dng", "dng_cfa_xtrans.dng");
  REQUIRE(image.valid());
  CHECK(image.width() == 36);
  check_patches(image, scene, upright, 0.004);
}

TEST_CASE("image_modern/raw: DNG gain map opcodes are applied") {
  std::vector<std::string> warnings;
  const ir::Image image = load("dng", "dng_cfa_gainmap.dng", {}, &warnings);
  REQUIRE(image.valid());
  check_patches(image, scene, upright, 0.004);
  CHECK(warnings.empty());
}

TEST_CASE("image_modern/raw: linear DNG, 32-bit float, Deflate + floating-point predictor, forward matrix") {
  const ir::Image image = load("dng", "dng_linear_float_deflate.dng");
  REQUIRE(image.valid());
  CHECK(image.width() == 32);
  CHECK(image.height() == 24);
  CHECK(image.source_bit_depth == 32);
  // Stored at half exposure with BaselineExposure +1.
  check_patches(image, scene, upright, 0.002);
}

TEST_CASE("image_modern/raw: linear DNG with lossy JPEG tiles") {
  const ir::Image image = load("dng", "dng_linear_lossy_jpeg.dng");
  REQUIRE(image.valid());
  CHECK(image.source_bit_depth == 8);
  check_patches(image, scene, upright, 0.03);
}

TEST_CASE("image_modern/raw: options") {
  SUBCASE("embedded preview") {
    const ir::Image image = load("dng", "dng_cfa_u16_strips.dng", R"({"raw_preview":true})");
    CHECK(image.width() == 30);
    CHECK(image.height() == 18);
    CHECK_FALSE(image.hdr.has_value());
  }
  SUBCASE("demosaic methods agree on flat areas") {
    for (const char* method : {"malvar", "bilinear", "adaptive"}) {
      INFO(method);
      const ir::Image image = load("dng", "dng_cfa_u16_strips.dng", std::string(R"({"raw_demosaic":")") + method + "\"}");
      check_patches(image, scene, upright, 0.004);
    }
    const ir::Image a = load("dng", "dng_cfa_u16_strips.dng", R"({"raw_demosaic":"bilinear"})");
    const ir::Image b = load("dng", "dng_cfa_u16_strips.dng", R"({"raw_demosaic":"adaptive"})");
    CHECK(a.hdr->rgba != b.hdr->rgba);  // they differ at the patch edges
  }
  SUBCASE("exposure") {
    const ir::Image image = load("dng", "dng_cfa_u16_strips.dng", R"({"raw_exposure":1})");
    std::array<Rgb, 4> doubled = scene;
    for (Rgb& p : doubled) {
      for (double& v : p) v *= 2.0;
    }
    check_patches(image, doubled, upright, 0.008);
  }
  SUBCASE("white balance") {
    const ir::Image none = load("dng", "dng_cfa_u16_strips.dng", R"({"raw_white_balance":"none"})");
    const ir::Image camera = load("dng", "dng_cfa_u16_strips.dng");
    CHECK(none.hdr->rgba != camera.hdr->rgba);
    // Grey world: a different balance, still a valid picture.
    const ir::Image automatic = load("dng", "dng_cfa_u16_strips.dng", R"({"raw_white_balance":"auto"})");
    CHECK(automatic.valid());
    CHECK(automatic.hdr->rgba != camera.hdr->rgba);
  }
  SUBCASE("tone mapping") {
    const ir::Image clip = load("dng", "dng_cfa_u16_strips.dng");
    const ir::Image filmic = load("dng", "dng_cfa_u16_strips.dng", R"({"raw_tone":"filmic"})");
    const ir::Image reinhard = load("dng", "dng_cfa_u16_strips.dng", R"({"raw_tone":"reinhard"})");
    CHECK(clip.hdr->rgba == filmic.hdr->rgba);
    CHECK(clip.frames[0].rgba != filmic.frames[0].rgba);
    CHECK(reinhard.valid());
  }
}

// ============================================================================ maker formats

TEST_CASE("image_modern/raw: Canon CR2 lossless JPEG slices") {
  std::vector<std::string> warnings;
  const ir::Image image = load("cr2", "cr2_lj92_slices.cr2", {}, &warnings);
  REQUIRE(image.valid());
  // Sensor 56x36 with left border 8 and top border 4.
  CHECK(image.width() == 48);
  CHECK(image.height() == 32);
  CHECK(image.source_bit_depth == 14);
  check_patches(image, camera_balanced, upright, 0.004);
  CHECK(has_warning(warnings, "colours are approximate"));
  const ir::Image preview = load("cr2", "cr2_lj92_slices.cr2", R"({"raw_preview":true})");
  CHECK(preview.width() == 48);
  CHECK(preview.height() == 32);
}

TEST_CASE("image_modern/raw: Nikon NEF uncompressed with maker-note white balance and preview") {
  const ir::Image image = load("nef", "nef_uncompressed.nef");
  REQUIRE(image.valid());
  CHECK(image.width() == 48);
  check_patches(image, camera_balanced, upright, 0.004);
  const ir::Image preview = load("nef", "nef_uncompressed.nef", R"({"raw_preview":true})");
  CHECK(preview.width() == 40);  // the maker-note preview beats the 16x10 thumbnail
  CHECK(preview.height() == 26);
}

TEST_CASE("image_modern/raw: Sony ARW2 compressed blocks") {
  std::vector<std::string> warnings;
  const ir::Image image = load("arw", "arw2_blocks.arw", R"({"raw_white_balance":"none"})", &warnings);
  REQUIRE(image.valid());
  CHECK(image.width() == 64);
  CHECK(image.height() == 24);
  check_patches(image, camera_unbalanced, upright, 0.006);
  // Without a recorded white balance, "camera" falls back to grey world with a warning.
  warnings.clear();
  (void)load("arw", "arw2_blocks.arw", {}, &warnings);
  CHECK(has_warning(warnings, "grey world"));
}

TEST_CASE("image_modern/raw: preview-only containers") {
  struct Case {
    const char* format;
    const char* file;
    std::uint32_t width, height;
  };
  for (const Case& c : {Case{"rw2", "rw2_preview.rw2", 40, 24}, Case{"raf", "raf_preview.raf", 40, 24},
                        Case{"cr3", "cr3_preview.cr3", 40, 24}, Case{"crw", "crw_preview.crw", 40, 24},
                        Case{"orf", "orf_preview.orf", 24, 40}}) {
    INFO(c.file);
    std::vector<std::string> warnings;
    const ir::Image image = load(c.format, c.file, {}, &warnings);
    REQUIRE(image.valid());
    CHECK(image.width() == c.width);
    CHECK(image.height() == c.height);
    CHECK(has_warning(warnings, "embedded preview"));
    // The preview's left half is blue (40, 90, 200), the rest green (30, 160, 90);
    // the ORF preview is rotated by its orientation 8 (270 degrees clockwise).
    const ir::Frame& f = image.frames[0];
    const std::size_t blue = std::string_view(c.format) == "orf" ? f.offset(12, 30) : f.offset(10, 12);
    const std::size_t green = std::string_view(c.format) == "orf" ? f.offset(12, 10) : f.offset(30, 12);
    CHECK(std::abs(int{f.rgba[blue + 2]} - 200) < 16);
    CHECK(std::abs(int{f.rgba[green + 1]} - 160) < 16);
    const ir::Image preview = load(c.format, c.file, R"({"raw_preview":true})");
    CHECK(preview.width() == c.width);
  }
}

// ============================================================================ detection

TEST_CASE("image_modern/raw: detection") {
  CHECK(detect("dng_cfa_u16_strips.dng", "a.dng") == "dng");
  CHECK(detect("dng_cfa_u16_strips.dng", "") == "dng");
  CHECK(detect("dng_cfa_u16_strips.dng", "renamed.tif") == "dng");
  CHECK(detect("cr2_lj92_slices.cr2", "") == "cr2");
  CHECK(detect("nef_uncompressed.nef", "") == "nef");
  CHECK(detect("nef_uncompressed.nef", "x.nrw") == "nrw");
  CHECK(detect("arw2_blocks.arw", "") == "arw");
  CHECK(detect("orf_preview.orf", "") == "orf");
  CHECK(detect("rw2_preview.rw2", "") == "rw2");
  CHECK(detect("rw2_preview.rw2", "y.rwl") == "rwl");
  CHECK(detect("raf_preview.raf", "") == "raf");
  CHECK(detect("cr3_preview.cr3", "") == "cr3");
  CHECK(detect("crw_preview.crw", "") == "crw");

  // A plain TIFF stays a TIFF, even from a camera maker, unless the RAW
  // structure is there (or the extension says so).
  ir::Image picture;
  picture.frames.push_back(ir::Frame::filled(4, 4, {10, 20, 30, 255}));
  const Context ctx;
  const auto tiff = image::encode("tiff", picture, Options{}, ctx);
  REQUIRE_OK(tiff);
  const auto plain = detect_format(Registry::instance(), *tiff, "");
  REQUIRE(plain.has_value());
  CHECK(plain->format->id == "tiff");
  // Hand-made TIFF with Make "NIKON" and no RAW directory.
  const Bytes nikon_tiff = test::bytes(std::string_view(
      "II*\x00\x08\x00\x00\x00\x01\x00\x0f\x01\x02\x00\x06\x00\x00\x00\x1a\x00\x00\x00\x00\x00\x00\x00NIKON\x00", 32));
  const auto scan = detect_format(Registry::instance(), nikon_tiff, "scan.tif");
  REQUIRE(scan.has_value());
  CHECK(scan->format->id == "tiff");
  const auto named = detect_format(Registry::instance(), nikon_tiff, "photo.nef");
  REQUIRE(named.has_value());
  CHECK(named->format->id == "nef");
}

TEST_CASE("image_modern/raw: converts through the engine") {
  const Bytes dng = test::fixture(raw_fixture("dng_cfa_u16_strips.dng"));
  const test::Converted png = test::convert(dng, "", "png", "", "shot.dng");
  REQUIRE(png.ok());
  const auto back = image::decode("png", png.data, Context{});
  REQUIRE_OK(back);
  CHECK(back->width() == 56);
  const test::Converted preview = test::convert(dng, "dng", "jpg", R"({"raw_preview":true})");
  CHECK(preview.ok());
}

// ============================================================================ robustness

TEST_CASE("image_modern/raw: lossless JPEG decoder") {
  // The first tile of the LJ92 fixture: 8 wide, 2 components, 16 high, 14-bit, predictor 1.
  const Bytes dng = test::fixture(raw_fixture("dng_cfa_lj92_tiles.dng"));
  const auto tree = image_modern::raw::parse_tree(dng);
  REQUIRE(tree.has_value());
  const auto& ifd = tree->ifds.front();
  const auto offsets = tree->tiff.integers(ifd, 324);
  const auto counts = tree->tiff.integers(ifd, 325);
  REQUIRE(offsets.size() == 6);
  for (std::size_t k = 0; k < offsets.size(); ++k) {
    INFO("tile ", k);
    const ByteView stream = tree->tiff.range(offsets[k], counts[k]);
    const auto frame = image_modern::raw::decode_ljpeg(stream, 1u << 20);
    REQUIRE_OK(frame);
    CHECK(frame->width == 8);
    CHECK(frame->height == 16);
    CHECK(frame->components == 2);
    CHECK(frame->precision == 14);
    CHECK(frame->predictor == (k == 4 ? 1 : (k > 4 ? static_cast<int>(k % 6 + 2) : static_cast<int>(k % 7 + 1))));
    // Every truncation fails cleanly.
    for (std::size_t n = 0; n < stream.size(); n += 7) {
      const auto cut = image_modern::raw::decode_ljpeg(stream.first(n), 1u << 20);
      if (cut) CHECK(cut->samples.size() == 256);
    }
  }
  CHECK_STATUS(image_modern::raw::decode_ljpeg(tree->tiff.range(offsets[0], counts[0]), 100), Status::Limit);
}

TEST_CASE("image_modern/raw: damaged and hostile files fail cleanly") {
  for (const auto& [format, file] : std::initializer_list<std::pair<const char*, const char*>>{
           {"dng", "dng_cfa_u16_strips.dng"},   {"dng", "dng_cfa_lj92_tiles.dng"},
           {"dng", "dng_cfa_packed12_rot90.dng"}, {"dng", "dng_cfa_xtrans.dng"},
           {"dng", "dng_linear_float_deflate.dng"}, {"dng", "dng_linear_lossy_jpeg.dng"},
           {"dng", "dng_cfa_gainmap.dng"},
           {"cr2", "cr2_lj92_slices.cr2"},       {"nef", "nef_uncompressed.nef"},
           {"arw", "arw2_blocks.arw"},           {"orf", "orf_preview.orf"},
           {"rw2", "rw2_preview.rw2"},           {"raf", "raf_preview.raf"},
           {"cr3", "cr3_preview.cr3"},           {"crw", "crw_preview.crw"}}) {
    INFO(file);
    const Bytes data = test::fixture(raw_fixture(file));
    CHECK(test::fuzz_reader<ir::Image>(format, data, 160) > 0);
    CHECK_STATUS(test::read_as<ir::Image>(format, ByteView(data).first(12)), Status::Decode);
  }
  // Pixel limit.
  Limits small;
  small.max_pixels = 100;
  CHECK_STATUS(test::read_as<ir::Image>("dng", test::fixture(raw_fixture("dng_cfa_u16_strips.dng")), {}, nullptr, small),
               Status::Limit);
  // An IFD that links to itself, and a SubIFD pointing at its parent.
  const Bytes loop = test::bytes(std::string_view(
      "II*\x00\x08\x00\x00\x00\x02\x00\x12\xc6\x01\x00\x04\x00\x00\x00\x01\x04\x00\x00\x4a\x01\x04\x00\x01\x00\x00\x00"
      "\x08\x00\x00\x00\x08\x00\x00\x00",
      38));
  CHECK_STATUS(test::read_as<ir::Image>("dng", loop), Status::Decode);
  // A CFA image claiming 60000 x 60000 pixels with 4 bytes of data.
  const Bytes bomb = test::bytes(std::string_view(
      "II*\x00\x08\x00\x00\x00\x07\x00"
      "\x00\x01\x04\x00\x01\x00\x00\x00\x60\xea\x00\x00"
      "\x01\x01\x04\x00\x01\x00\x00\x00\x60\xea\x00\x00"
      "\x02\x01\x03\x00\x01\x00\x00\x00\x10\x00\x00\x00"
      "\x06\x01\x03\x00\x01\x00\x00\x00\x4c\x88\x00\x00"
      "\x11\x01\x04\x00\x01\x00\x00\x00\x62\x00\x00\x00"
      "\x17\x01\x04\x00\x01\x00\x00\x00\x04\x00\x00\x00"
      "\x12\xc6\x01\x00\x04\x00\x00\x00\x01\x04\x00\x00"
      "\x00\x00\x00\x00\x01\x02\x03\x04",
      102));
  const auto bombed = test::read_as<ir::Image>("dng", bomb);
  REQUIRE_FALSE(bombed.has_value());
  CHECK((bombed.error().code == Status::Decode || bombed.error().code == Status::Limit));
  CHECK_STATUS(test::read_as<ir::Image>("dng", test::random_bytes(4096, 7)), Status::Decode);
}
