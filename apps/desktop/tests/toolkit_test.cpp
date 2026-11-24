// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// Toolkit tests: JSON, UTF-8 and formatting helpers, the renderer, PNG output.
#include <cmath>
#include <cstring>

#include "base/json.hpp"
#include "base/text_util.hpp"
#include "gfx/canvas.hpp"
#include "gfx/image_io.hpp"
#include "gfx/path.hpp"
#include "support/test.hpp"
#include "util/deflate.hpp"

using namespace cvd;

TEST_CASE("desktop/json: parses and serialises documents") {
  const auto j = Json::parse(R"({"a": [1, 2.5, true, null, "x\u00e9\n"], "b": {"c": -3e2}})");
  REQUIRE(j.has_value());
  CHECK(j->is_object());
  CHECK(j->size() == 2u);
  CHECK((*j)["a"].size() == 5u);
  CHECK((*j)["a"][0].as_int() == 1);
  CHECK((*j)["a"][1].as_number() == 2.5);
  CHECK((*j)["a"][2].as_bool());
  CHECK((*j)["a"][3].is_null());
  CHECK((*j)["a"][4].str() == "x\xC3\xA9\n");
  CHECK((*j)["b"]["c"].as_number() == -300.0);
  CHECK((*j)["missing"].is_null());
  // Round trip keeps member order and values.
  const auto again = Json::parse(j->dump());
  REQUIRE(again.has_value());
  CHECK(*again == *j);
  CHECK(Json::parse(j->dump(2)).value() == *j);
}

TEST_CASE("desktop/json: rejects malformed input") {
  CHECK_FALSE(Json::parse("").has_value());
  CHECK_FALSE(Json::parse("{").has_value());
  CHECK_FALSE(Json::parse("[1,]").has_value());
  CHECK_FALSE(Json::parse("{\"a\" 1}").has_value());
  CHECK_FALSE(Json::parse("\"unterminated").has_value());
  CHECK_FALSE(Json::parse("01x").has_value());
  CHECK_FALSE(Json::parse("[1] 2").has_value());
}

TEST_CASE("desktop/json: surrogate pairs and escaping") {
  const auto j = Json::parse(R"("\ud83d\ude00")");
  REQUIRE(j.has_value());
  CHECK(j->str() == "\xF0\x9F\x98\x80");
  CHECK(json_quote("a\"b\\c\n\x01") == "\"a\\\"b\\\\c\\n\\u0001\"");
}

TEST_CASE("desktop/json: mutation is copy-on-write") {
  Json a = Json::object();
  a.set("k", 1);
  Json b = a;
  b.set("k", 2);
  CHECK(a["k"].as_int() == 1);
  CHECK(b["k"].as_int() == 2);
  Json arr = Json::array();
  arr.push_back("x");
  arr.push_back(3);
  CHECK(arr.dump() == "[\"x\",3]");
}

TEST_CASE("desktop/utf8: decoding, stepping and invalid bytes") {
  const std::string s = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
  const auto cps = utf8_decode(s);
  REQUIRE(cps.size() == 4u);
  CHECK(cps[0] == 'a');
  CHECK(cps[1] == 0xE9u);
  CHECK(cps[2] == 0x20ACu);
  CHECK(cps[3] == 0x1F600u);
  CHECK(utf8_length(s) == 4u);
  CHECK(utf8_prev(s, s.size()) == 6u);
  CHECK(utf8_prev(s, 6) == 3u);
  CHECK(utf8_prev(s, 1) == 0u);
  std::string out;
  for (const auto cp : cps) utf8_append(out, cp);
  CHECK(out == s);
  // Truncated and overlong sequences become U+FFFD without overrunning.
  CHECK(utf8_decode("\xE2\x82") == std::vector<std::uint32_t>{0xFFFD, 0xFFFD});
  CHECK(utf8_decode("\xC0\xAF")[0] == 0xFFFDu);
}

TEST_CASE("desktop/format: sizes and changes") {
  CHECK(format_size(0) == "0 bytes");
  CHECK(format_size(1) == "1 byte");
  CHECK(format_size(999) == "999 bytes");
  CHECK(format_size(1000) == "1.00 KB");
  CHECK(format_size(1234) == "1.23 KB");
  CHECK(format_size(56789) == "56.8 KB");
  CHECK(format_size(999999) == "1.00 MB");
  CHECK(format_size(5'600'000) == "5.60 MB");
  CHECK(format_size(123'000'000) == "123 MB");
  CHECK(format_size(2'500'000'000ULL) == "2.50 GB");
  CHECK(format_size_change(100, 50) == "\xE2\x88\x92" "50%");
  CHECK(format_size_change(100, 150) == "+50%");
  CHECK(format_size_change(100, 100) == "0%");
  CHECK(format_size_change(0, 0) == "0%");
  CHECK(format_percent(0.0) == "0%");
  CHECK(format_percent(0.426) == "42%");
  CHECK(format_percent(1.0) == "100%");
  CHECK(format_percent(7.0) == "100%");
}

TEST_CASE("desktop/format: path and string helpers") {
  CHECK(path_filename("/a/b/c.txt") == "c.txt");
  CHECK(path_filename("C:\\x\\y.png") == "y.png");
  CHECK(path_stem("archive.tar.gz") == "archive.tar");
  CHECK(path_stem(".hidden") == ".hidden");
  CHECK(path_extension("Photo.JPG") == "jpg");
  CHECK(path_extension("noext") == "");
  CHECK(path_parent("/a/b/c.txt") == "/a/b");
  CHECK(path_parent("/c.txt") == "/");
  CHECK(contains_ci("Base64 Text", "64 t"));
  CHECK_FALSE(contains_ci("hex", "hexa"));
  CHECK(starts_with_ci("JPEG", "jp"));
  CHECK(split("a,b,,c", ',').size() == 4u);
  CHECK(trim("  x y \n") == "x y");
}

TEST_CASE("desktop/color: hex parsing, premultiplication and contrast") {
  const Color c = Color::hex("#7C6CFF");
  CHECK(std::lround(c.r * 255) == 0x7C);
  CHECK(std::lround(c.g * 255) == 0x6C);
  CHECK(std::lround(c.b * 255) == 0xFF);
  CHECK_APPROX(Color::hex("#05060ACC").a, 0.8f, 0.002f);
  CHECK(Color::hex("#FFFFFF").premul_u32() == 0xFFFFFFFFu);
  CHECK(Color(1, 0, 0, 0.5f).premul_u32() == 0x80800000u);
  CHECK_APPROX(contrast_ratio(Color::hex("#000000"), Color::hex("#FFFFFF")), 21.0f, 0.01f);
  CHECK_APPROX(contrast_ratio(Color::hex("#F3F4F9"), Color::hex("#0D0F16")), 17.43f, 0.05f);
}

namespace {

std::uint32_t px(const gfx::Bitmap& b, int x, int y) { return b.at(x, y); }
int alpha(std::uint32_t p) { return static_cast<int>(p >> 24); }

}  // namespace

TEST_CASE("desktop/canvas: exact coverage for axis-aligned rectangles") {
  gfx::Bitmap bmp(10, 10);
  gfx::Canvas c(bmp);
  c.fill_rect({2.0f, 2.0f, 4.0f, 4.0f}, Color(1, 1, 1, 1));
  CHECK(px(bmp, 3, 3) == 0xFFFFFFFFu);
  CHECK(px(bmp, 1, 3) == 0u);
  CHECK(px(bmp, 6, 3) == 0u);
  gfx::Bitmap half(4, 4);
  gfx::Canvas hc(half);
  hc.fill_rect({0.5f, 0.0f, 2.0f, 4.0f}, Color(1, 1, 1, 1));
  CHECK(std::abs(alpha(px(half, 0, 1)) - 128) <= 1);
  CHECK(alpha(px(half, 1, 1)) == 255);
  CHECK(std::abs(alpha(px(half, 2, 1)) - 128) <= 1);
}

TEST_CASE("desktop/canvas: paths are anti-aliased with the non-zero rule") {
  gfx::Bitmap bmp(40, 40);
  gfx::Canvas c(bmp);
  gfx::Path p;
  p.add_circle({20.0f, 20.0f}, 12.0f);
  c.fill_path(p, Color(0, 0, 0, 1));
  CHECK(alpha(px(bmp, 20, 20)) == 255);
  CHECK(alpha(px(bmp, 1, 1)) == 0);
  // Pixels on the circle's edge are partially covered.
  const int edge = alpha(px(bmp, 31, 15));
  CHECK(edge > 0);
  CHECK(edge < 255);
  // Total coverage approximates the area (pi r^2).
  double sum = 0.0;
  for (const auto v : bmp.pixels) sum += static_cast<double>(v >> 24) / 255.0;
  CHECK_APPROX(sum, 3.14159265 * 144.0, 4.5);  // within 1%
}

TEST_CASE("desktop/canvas: overlapping stroke pieces do not double-darken") {
  gfx::Bitmap bmp(60, 60);
  gfx::Canvas c(bmp);
  gfx::StrokeStyle st;
  st.width = 8.0f;
  st.join = gfx::LineJoin::Miter;
  c.stroke_polyline({{10, 10}, {50, 10}, {50, 50}}, false, st, Color(1, 1, 1, 0.5f));
  // The corner region is covered by two segment quads and a miter piece; it
  // must still blend exactly once.
  CHECK(std::abs(alpha(px(bmp, 50, 10)) - 128) <= 1);
  CHECK(std::abs(alpha(px(bmp, 30, 10)) - 128) <= 1);
  CHECK(alpha(px(bmp, 30, 30)) == 0);
}

TEST_CASE("desktop/canvas: rounded rectangles, clipping and opacity") {
  gfx::Bitmap bmp(50, 50);
  gfx::Canvas c(bmp);
  c.save();
  c.clip_rect({0, 0, 25, 50});
  c.set_opacity(0.5f);
  c.fill_rrect({5, 5, 40, 40}, 10.0f, Color(1, 1, 1, 1));
  c.restore();
  CHECK(std::abs(alpha(px(bmp, 20, 25)) - 128) <= 1);
  CHECK(alpha(px(bmp, 30, 25)) == 0);  // clipped
  CHECK(alpha(px(bmp, 5, 5)) == 0);    // outside the rounded corner
  // Stroke draws only a ring.
  gfx::Bitmap ring(50, 50);
  gfx::Canvas rc(ring);
  rc.stroke_rrect({5, 5, 40, 40}, 8.0f, 2.0f, Color(1, 1, 1, 1));
  CHECK(alpha(px(ring, 25, 25)) == 0);
  CHECK(alpha(px(ring, 25, 5)) == 255);
}

TEST_CASE("desktop/canvas: hairline rounded rects at HiDPI (text caret)") {
  // A 1.5 pt caret at 2x lands on an empty interior span; it must still draw and terminate.
  for (const float x : {10.0f, 10.25f, 10.5f, 10.75f}) {
    gfx::Bitmap bmp(40, 40);
    gfx::Canvas c(bmp);
    c.scale(2.0f, 2.0f);
    c.fill_rrect({x, 2.0f, 1.5f, 12.0f}, 0.75f, Color(1, 1, 1, 1));
    int lit = 0;
    for (const auto p : bmp.pixels) lit += (p >> 24) > 0 ? 1 : 0;
    CHECK(lit >= 3 * 20);
  }
}

TEST_CASE("desktop/canvas: gradients and shadows") {
  gfx::Bitmap bmp(100, 10);
  gfx::Canvas c(bmp);
  c.fill_rect({0, 0, 100, 10}, gfx::Paint::linear({0, 0}, {100, 0}, {{0.0f, Color(0, 0, 0, 1)}, {1.0f, Color(1, 1, 1, 1)}}));
  const int left = static_cast<int>(px(bmp, 2, 5) & 0xFF);
  const int mid = static_cast<int>(px(bmp, 50, 5) & 0xFF);
  const int right = static_cast<int>(px(bmp, 97, 5) & 0xFF);
  CHECK(left < 20);
  CHECK(std::abs(mid - 128) < 6);
  CHECK(right > 235);
  gfx::Bitmap sh(80, 80);
  gfx::Canvas sc(sh);
  sc.draw_shadow({20, 20, 40, 40}, 8.0f, 16.0f, 0.0f, {0, 0}, Color(0, 0, 0, 1));
  CHECK(alpha(px(sh, 40, 40)) > 240);              // deep inside
  CHECK(alpha(px(sh, 2, 40)) < alpha(px(sh, 16, 40)));  // falls off outwards
  CHECK(alpha(px(sh, 16, 40)) > 0);
}

TEST_CASE("desktop/path: SVG parsing, flattening and trimming") {
  const auto p = gfx::Path::parse_svg("M772 260H512A252 252 0 0 0 260 512V764H864");
  REQUIRE(p.has_value());
  const auto flat = p->flatten({}, 0.05f);
  REQUIRE(flat.size() == 1u);
  const gfx::PolylineMeasure m(flat[0].points);
  CHECK_APPROX(m.length(), 1511.841f, 0.5f);
  const Vec2 half = m.point_at(0.17198f);
  CHECK_APPROX(half.x, 512.0f, 0.5f);
  CHECK_APPROX(half.y, 260.0f, 0.5f);
  const auto part = m.trim(0.0f, 0.5f);
  REQUIRE(part.size() >= 2u);
  CHECK_APPROX(gfx::PolylineMeasure(part).length(), 1511.841f * 0.5f, 0.6f);
  CHECK_FALSE(gfx::Path::parse_svg("M 1").has_value());
  CHECK(gfx::Path::parse_svg("m1 1l2 0 0 2z").has_value());
}

TEST_CASE("desktop/png: encoder output is a valid zlib PNG") {
  gfx::Bitmap bmp(23, 7);
  for (int y = 0; y < bmp.height; ++y) {
    for (int x = 0; x < bmp.width; ++x) {
      bmp.row(y)[x] = 0xFF000000u | static_cast<std::uint32_t>(x * 10) << 16 | static_cast<std::uint32_t>(y * 30);
    }
  }
  const auto png = gfx::encode_png(bmp);
  REQUIRE(png.size() > 33u);
  CHECK(std::memcmp(png.data(), "\x89PNG\r\n\x1a\n", 8) == 0);
  // IHDR then IDAT; decompress the IDAT with the core's inflater.
  const auto be32 = [&](std::size_t o) {
    return (static_cast<std::uint32_t>(png[o]) << 24) | (static_cast<std::uint32_t>(png[o + 1]) << 16) |
           (static_cast<std::uint32_t>(png[o + 2]) << 8) | png[o + 3];
  };
  CHECK(be32(16) == 23u);
  CHECK(be32(20) == 7u);
  const std::size_t idat = 33;
  const std::uint32_t len = be32(idat);
  CHECK(std::memcmp(png.data() + idat + 4, "IDAT", 4) == 0);
  const auto* data = reinterpret_cast<const std::byte*>(png.data() + idat + 8);
  const auto raw = cvt::util::zlib_decompress(cvt::ByteView(data, len));
  REQUIRE(raw.has_value());
  CHECK(raw->size() == static_cast<std::size_t>((23 * 4 + 1) * 7));
  // The CRC of each chunk covers type and data.
  CHECK(be32(idat + 8 + len) == gfx::crc32(png.data() + idat + 4, len + 4));
  const auto bmp_file = gfx::encode_bmp(bmp);
  CHECK(bmp_file.size() == static_cast<std::size_t>(54 + 23 * 7 * 4));
}
