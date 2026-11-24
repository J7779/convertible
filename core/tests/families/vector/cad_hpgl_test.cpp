// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <string>
#include <vector>

#include "engine/detect.hpp"
#include "families/vector/cad_test_util.hpp"
#include "families/vector/font.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"

using namespace cvt;
using namespace cvt::vector;
using namespace cvt::cadtest;
using namespace std::string_view_literals;

namespace {

constexpr double plu = 0.025 * px_per_mm;  // px per plotter unit

Result<Scene> read_hpgl_text(std::string_view text) { return read_scene("hpgl", as_bytes(text), test_context()); }
Result<Scene> read_hpgl_fixture(std::string_view name) {
  return read_scene("hpgl", test::fixture(std::string("vector/cad/") + std::string(name)), test_context());
}

// Bounds of every shape in plotter units (the page root maps plotter units to px).
std::vector<Rect> shape_bounds(const Scene& scene, std::size_t page = 0) {
  std::vector<Rect> out;
  for (const Placed& p : leaves(scene, page)) {
    if (const auto* s = std::get_if<Shape>(&p.node->value)) out.push_back(bounds(s->path, p.local));
  }
  return out;
}

const Shape* find_shape(const Scene& scene, double x0, double y0, double x1, double y1, double tol) {
  for (const Placed& p : leaves(scene)) {
    if (const auto* s = std::get_if<Shape>(&p.node->value)) {
      const Rect b = bounds(s->path, p.local);
      if (near(b.x0, x0, tol) && near(b.y0, y0, tol) && near(b.x1, x1, tol) && near(b.y1, y1, tol)) return s;
    }
  }
  return nullptr;
}

Rect content_bounds_of(const Scene& scene) {
  Rect r;
  for (const Rect& b : shape_bounds(scene)) r.add(b);
  return r;
}

// Polyline-encoded number (8-bit mode, base 64).
std::string pe_number(std::int64_t v) {
  std::uint64_t n = v < 0 ? (static_cast<std::uint64_t>(-v) << 1) | 1u : static_cast<std::uint64_t>(v) << 1;
  std::string out;
  while (n >= 64) {
    out.push_back(static_cast<char>(63 + (n & 63)));
    n >>= 6;
  }
  out.push_back(static_cast<char>(191 + n));
  return out;
}
std::string pe_number7(std::int64_t v) {
  std::uint64_t n = v < 0 ? (static_cast<std::uint64_t>(-v) << 1) | 1u : static_cast<std::uint64_t>(v) << 1;
  std::string out;
  while (n >= 32) {
    out.push_back(static_cast<char>(63 + (n & 31)));
    n >>= 5;
  }
  out.push_back(static_cast<char>(95 + n));
  return out;
}

}  // namespace

TEST_CASE("vector/hpgl: decodes an HP-GL/2 drawing from GNU plotutils") {
  auto scene = read_hpgl_fixture("shapes_hpgl2.hpgl");
  REQUIRE_OK(scene);
  REQUIRE(scene->pages.size() == 1);
  CHECK(near(scene->pages[0].root.transform.a, plu, 1e-12));
  // CI313 in user units scaled by SC0,10000 over IP0..8128: radius 254.4 plotter units.
  const double r = 313 * 0.8128;
  const Shape* circle = find_shape(*scene, 5250 * 0.8128 - r, 5906 * 0.8128 - r, 5250 * 0.8128 + r, 5906 * 0.8128 + r, 0.01);
  REQUIRE(circle != nullptr);
  // PW0.0832 with WU1: 0.0832 % of the P1-P2 diagonal.
  CHECK(near(circle->stroke.width, 0.0832 / 100 * 8128 * std::sqrt(2.0), 1e-6));
  CHECK(rgb_is(circle->stroke.paint.color, 0, 0, 0));
  // The box EA3063,5594 -> 4313,6219.
  CHECK(find_shape(*scene, 3063 * 0.8128, 5594 * 0.8128, 4313 * 0.8128, 6219 * 0.8128, 0.01) != nullptr);
  // The arrow head is filled (PM ... FP), the half circle arc (AA ... 180) is open.
  bool filled = false;
  for (const Placed& p : leaves(*scene)) {
    if (const auto* s = std::get_if<Shape>(&p.node->value)) filled = filled || s->fill.visible();
  }
  CHECK(filled);
  const Shape* arc = find_shape(*scene, 6188 * 0.8128, (4656 - 375) * 0.8128, (6188 + 750) * 0.8128, 4656 * 0.8128, 0.01);
  REQUIRE(arc != nullptr);
  CHECK(arc->path.verbs.back() != Verb::Close);
}

TEST_CASE("vector/hpgl: HP-GL/1, 1.5 and PCL 5 variants draw the same picture") {
  auto reference = read_hpgl_fixture("shapes_hpgl2.hpgl");
  REQUIRE_OK(reference);
  const Rect want = content_bounds_of(*reference);
  for (const char* name : {"shapes_hpgl15.hpgl", "shapes_hpgl1.hpgl", "shapes_pcl5.pcl"}) {
    INFO(name);
    auto scene = read_hpgl_fixture(name);
    REQUIRE_OK(scene);
    const Rect got = content_bounds_of(*scene);
    // HP-GL/1 has no pen widths and the PCL file (P1 moved up by IP) places
    // labels instead of stroked letters: the sizes agree to a few plotter units.
    CHECK(near(got.width(), want.width(), 30));
    if (std::string_view(name) != "shapes_pcl5.pcl") CHECK(near(got.height(), want.height(), 30));
  }
  // The PCL 5 file wraps HP-GL/2 (ESC%0B ... ESC%0A) and uses real labels and Béziers.
  auto pcl = read_hpgl_fixture("shapes_pcl5.pcl");
  REQUIRE_OK(pcl);
  CHECK(all_text(*pcl) == "Box|Lorem|");
}

TEST_CASE("vector/hpgl: pens, labels and fills of other writers") {
  auto graph = read_hpgl_fixture("graph_hpgl2.hpgl");
  REQUIRE_OK(graph);
  bool green = false;
  for (const Placed& p : leaves(*graph)) {
    if (const auto* s = std::get_if<Shape>(&p.node->value)) green = green || rgb_is(s->stroke.paint.color, 0, 255, 0);
  }
  CHECK(green);  // SP3 = pen 3 of the HP-GL/2 default palette

  auto ps = read_hpgl_fixture("pstoedit.hpgl");
  REQUIRE_OK(ps);
  const auto runs = texts(*ps);
  REQUIRE(runs.size() == 1);
  CHECK(runs[0]->content == "Lorem");
  CHECK(near(runs[0]->font.size * 0.727, 0.338667 * 400, 1e-3));  // SI height in cm
  bool disc = false;
  for (const Placed& p : leaves(*ps)) {
    if (const auto* s = std::get_if<Shape>(&p.node->value)) disc = disc || s->fill.visible();
  }
  CHECK(disc);
}

TEST_CASE("vector/hpgl: commands, scaling, rotation and pages") {
  auto scene = read_hpgl_text(
      "IN;IP0,0,4000,4000;SC0,100,0,100;SP2;PA10,10;PD20,10,20,20;PU;"   // red L
      "PA50,50;CI10;"                                                     // circle r 400 plu
      "PA0,0;EA100,50;"                                                   // rectangle edge
      "SC;PU1000,1000;PD;AR500,0,180;PU;"                                 // half circle, centre (1500,1000)
      "PA3000,3000;AT3500,3500,4000,3000;"                                // pen up arc: nothing
      "PU3000,3000;PD;AT3500,3500,4000,3000;PU;"
      "SP5;FT1;PA2000,0;RA2500,500;"                                      // filled blue rectangle
      "PA100,100;WG300,0,90;"                                             // filled wedge
      "LT2,4;PA0,5000;PD1000,5000;PU;LT;"                                 // dashed
      "DT*;SI0.5,1;DI0,1;PA0,6000;LBRotated*;"                            // custom terminator, 90 degrees
      "PG;SP1;PA0,0;PD100,0;PU;");
  REQUIRE_OK(scene);
  CHECK(scene->pages.size() == 2);
  const std::vector<Rect> b = shape_bounds(*scene);
  const auto has = [&](double x0, double y0, double x1, double y1) {
    return std::any_of(b.begin(), b.end(), [&](const Rect& r) {
      return near(r.x0, x0, 0.01) && near(r.y0, y0, 0.01) && near(r.x1, x1, 0.01) && near(r.y1, y1, 0.01);
    });
  };
  CHECK(has(400, 400, 800, 800));      // PD in user units (x40)
  CHECK(has(1600, 1600, 2400, 2400));  // CI10 -> r 400
  CHECK(has(0, 0, 4000, 2000));        // EA
  CHECK(has(1000, 500, 2000, 1000));   // AR 180 degrees counter-clockwise
  CHECK(has(3000, 3000, 4000, 3500));  // AT through (3500,3500)
  CHECK(has(2000, 0, 2500, 500));      // RA
  const Shape* fill = find_shape(*scene, 2000, 0, 2500, 500, 0.01);
  REQUIRE(fill != nullptr);
  CHECK(rgb_is(fill->fill.color, 0, 0, 255));
  CHECK(has(100, 100, 400, 400));      // WG quarter wedge
  const Shape* dashed = find_shape(*scene, 0, 5000, 1000, 5000, 0.01);
  REQUIRE(dashed != nullptr);
  CHECK(dashed->stroke.dash.size() == 2);
  const auto runs = texts(*scene);
  REQUIRE(runs.size() == 1);
  CHECK(runs[0]->content == "Rotated");
  CHECK(near(runs[0]->font.size * 0.727, 400, 1e-6));  // SI height 1 cm
  for (const Placed& p : leaves(*scene)) {
    if (std::holds_alternative<Text>(p.node->value)) CHECK(near(std::atan2(p.local.b, p.local.a), std::numbers::pi / 2, 1e-9));
  }
  CHECK(shape_bounds(*scene, 1).size() == 1);

  // Isotropic scaling centres the drawing; RO 90 rotates the page.
  auto iso = read_hpgl_text("IN;IP0,0,2000,1000;SC0,10,0,10,1;PA0,0;PD10,10;");
  REQUIRE_OK(iso);
  CHECK(shape_bounds(*iso).at(0).x0 == test::approx(500).abs(1e-9));
  CHECK(shape_bounds(*iso).at(0).x1 == test::approx(1500).abs(1e-9));
  auto rotated = read_hpgl_text("IN;RO90;PA0,0;PD4000,0;");
  REQUIRE_OK(rotated);
  CHECK(near(rotated->pages[0].height, 4000 * plu, 2));  // the line is now vertical
}

TEST_CASE("vector/hpgl: polyline encoded PE in 8-bit and 7-bit modes") {
  const std::string pe = "IN;SP1;PE<=" + pe_number(1000) + pe_number(1000) + pe_number(500) + pe_number(0) +
                         pe_number(0) + pe_number(-500) + ";";
  auto scene = read_hpgl_text(pe);
  REQUIRE_OK(scene);
  REQUIRE(shape_bounds(*scene).size() == 1);
  const Rect r = shape_bounds(*scene)[0];
  CHECK(r.x0 == 1000);
  CHECK(r.x1 == 1500);
  CHECK(r.y0 == 500);
  CHECK(r.y1 == 1000);
  const std::string pe7 = "IN;SP1;PE7<=" + pe_number7(100) + pe_number7(200) + ">" + pe_number7(1) + pe_number7(50) +
                          pe_number7(-50) + ";";
  auto seven = read_hpgl_text(pe7);
  REQUIRE_OK(seven);
  REQUIRE(shape_bounds(*seven).size() == 1);
  const Rect s = shape_bounds(*seven)[0];
  CHECK(s.x0 == 100);
  CHECK(s.x1 == 125);  // 50 with one fractional bit
  CHECK(s.y0 == 175);
  // An endless PE (no terminator, garbage after it) stops at the end of the data.
  std::string endless = "IN;SP1;PD0,0;PE";
  for (int i = 0; i < 5000; ++i) endless += pe_number(i % 7) + "\x80";
  CHECK_OK(read_hpgl_text(endless));
}

TEST_CASE("vector/hpgl: write -> read round trip in both dialects") {
  Scene scene;
  Page page;
  page.width = 400;
  page.height = 300;
  Shape line;
  line.path.move_to({10, 10});
  line.path.line_to({200, 10});
  line.path.line_to({200, 100});
  line.stroke.paint = Paint::solid(Color::rgb8(255, 0, 0));
  line.stroke.width = 0.5 * px_per_mm;
  page.root.children.push_back(Node{line});
  Shape disc;
  add_ellipse(disc.path, 300, 200, 50, 50);
  disc.fill = Paint::solid(Color::rgb8(0, 0, 255));
  page.root.children.push_back(Node{disc});
  page.root.children.push_back(
      Node{make_text("Label", FontSpec{"sans-serif", 30, 400, false}, {20, 250}, Paint::solid(Color::rgb8(0, 128, 0)))});
  scene.pages.push_back(page);

  auto hpgl2 = write_scene("hpgl", scene, test_context());
  REQUIRE_OK(hpgl2);
  const std::string text = test::str(*hpgl2);
  CHECK(text.starts_with("BP;IN;"));
  CHECK(text.find("PC1,255,0,0;") != std::string::npos);
  CHECK(text.find("PW0.5;") != std::string::npos);
  CHECK(text.find("FP1;") != std::string::npos);  // non-zero fill rule
  CHECK(text.find("LBLabel\x03") != std::string::npos);
  auto back = read_scene("hpgl", *hpgl2, test_context());
  REQUIRE_OK(back);
  // Page px -> plotter units (y flipped at the page height) and back.
  const double k = 1 / plu;
  const Shape* red = find_shape(*back, 10 * k, (300 - 100) * k, 200 * k, (300 - 10) * k, 1.0);
  REQUIRE(red != nullptr);
  CHECK(rgb_is(red->stroke.paint.color, 255, 0, 0));
  CHECK(near(red->stroke.width * 0.025, 0.5, 1e-9));
  const Shape* blue = find_shape(*back, 250 * k, 50 * k, 350 * k, 150 * k, 1.0);
  REQUIRE(blue != nullptr);
  CHECK(rgb_is(blue->fill.color, 0, 0, 255));
  REQUIRE(texts(*back).size() == 1);
  CHECK(texts(*back)[0]->content == "Label");
  CHECK(near(texts(*back)[0]->font.size * 0.727 * 0.025, 30 * 0.727 / px_per_mm, 1e-3));

  std::vector<std::string> warnings;
  const test::Converted plain = test::convert(*hpgl2, "hpgl", "hpgl", R"({"dialect":"hpgl","text":"outline"})");
  REQUIRE(plain.ok());
  CHECK(plain.text().starts_with("IN;"));
  CHECK(plain.text().find("PC") == std::string::npos);
  CHECK(plain.text().find("PW") == std::string::npos);
  CHECK(plain.text().find("PM0") == std::string::npos);
  CHECK(plain.text().find("LB") == std::string::npos);
  auto nofill = write_scene("hpgl", scene, test_context(), *Options::parse(R"({"fills":false})"));
  REQUIRE_OK(nofill);
  CHECK(test::str(*nofill).find("FP") == std::string::npos);
  // Several pages are separated by PG.
  Scene two = scene;
  two.pages.push_back(page);
  auto pages = write_scene("hpgl", two, test_context());
  REQUIRE_OK(pages);
  auto both = read_scene("hpgl", *pages, test_context());
  REQUIRE_OK(both);
  CHECK(both->pages.size() == 2);
}

TEST_CASE("vector/hpgl: detection") {
  const auto detect = [](ByteView data, std::string_view name = {}) {
    const auto result = detect_format(Registry::instance(), data, name);
    return result && result->format != nullptr ? result->format->id : std::string("?");
  };
  for (const char* name : {"shapes_hpgl2.hpgl", "shapes_hpgl1.hpgl", "shapes_pcl5.pcl", "pstoedit.hpgl", "graph_hpgl2.hpgl"}) {
    INFO(name);
    CHECK(detect(test::fixture(std::string("vector/cad/") + name)) == "hpgl");
  }
  CHECK(detect(as_bytes("\x1b%-12345X@PJL ENTER LANGUAGE=HPGL2\r\n\x1b" "E\x1b%0BIN;SP1;PU0,0;PD100,100;"sv)) == "hpgl");
  CHECK(detect(as_bytes("IN THE BEGINNING there was text."sv)) != "hpgl");
  CHECK(detect(as_bytes("IN;SP1;PU;"sv), "drawing.plt") == "hpgl");
}

TEST_CASE("vector/hpgl: damaged input never crashes") {
  for (const char* name : {"shapes_hpgl2.hpgl", "shapes_pcl5.pcl", "pstoedit.hpgl"}) {
    INFO(name);
    CHECK(test::fuzz_reader<ir::Data>("hpgl", test::fixture(std::string("vector/cad/") + name), 128) > 0);
  }
  CHECK_STATUS(test::read_as<ir::Data>("hpgl", as_bytes("IN;SP1;"sv)), Status::Decode);  // draws nothing
  CHECK_OK(test::read_as<ir::Data>("hpgl", as_bytes("IN;SP1;PD;CI99999999999999;LB\x1b"sv)));
}
