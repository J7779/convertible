// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "engine/detect.hpp"
#include "families/vector/cad_test_util.hpp"
#include "families/vector/font.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

using namespace cvt;
using namespace cvt::vector;
using namespace cvt::cadtest;
using namespace std::string_view_literals;

namespace {

Result<Scene> read_dxf_fixture(std::string_view name, const Options& options = Options{}) {
  return read_scene("dxf", test::fixture(std::string("vector/cad/") + std::string(name)), test_context(), options);
}

Options opts(std::string_view json) { return *Options::parse(json); }

// A version-independent description of every drawn leaf: kind, layer,
// colour and bounds in drawing units (rounded), sorted.
std::vector<std::string> signature(const Scene& scene) {
  std::vector<std::string> out;
  for (const Placed& p : leaves(scene)) {
    const std::string layer = p.layer != nullptr ? p.layer->layer_name : "-";
    const auto r = [](double v) { return std::format("{:.2f}", std::abs(v) < 0.005 ? 0.0 : v); };
    const auto c = [](const Color& col) {
      return std::format("{:02x}{:02x}{:02x}", std::lround(col.r * 255), std::lround(col.g * 255), std::lround(col.b * 255));
    };
    if (const auto* s = std::get_if<Shape>(&p.node->value)) {
      const Rect b = bounds(s->path, p.local);
      out.push_back(std::format("shape {} {} {} {} {} {} fill={} stroke={} dashed={}", layer, r(b.x0), r(b.y0), r(b.x1),
                                r(b.y1), s->path.verbs.size() > 0 ? "p" : "-",
                                s->fill.visible() ? c(s->fill.color) : "none",
                                s->stroke.paint.visible() ? c(s->stroke.paint.color) : "none", !s->stroke.dash.empty()));
    } else if (const auto* t = std::get_if<Text>(&p.node->value)) {
      const Point o = p.local.apply(t->origin);
      out.push_back(std::format("text {} {} {} {} {}", layer, t->content, r(o.x), r(o.y), c(t->fill.color)));
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

const Shape* shape_near(const Scene& scene, Point a, Point b, double tol = 0.05) {
  for (const Placed& p : leaves(scene)) {
    if (const auto* s = std::get_if<Shape>(&p.node->value)) {
      const Rect r = bounds(s->path, p.local);
      if (near(r.x0, a.x, tol) && near(r.y0, a.y, tol) && near(r.x1, b.x, tol) && near(r.y1, b.y, tol)) return s;
    }
  }
  return nullptr;
}

bool scene_layers_only(const Scene& scene) {
  bool ok = true;
  for (const Node& node : scene.pages.at(0).root.children) {
    const auto* g = std::get_if<Group>(&node.value);
    ok = ok && g != nullptr && (g->layer_name == "WALLS" || g->layer_name == "DASHED_LAYER");
  }
  return ok && layer_group(scene, "WALLS") != nullptr && layer_group(scene, "DASHED_LAYER") != nullptr;
}

std::size_t count_shapes(const Scene& scene) {
  std::size_t n = 0;
  for (const Placed& p : leaves(scene)) n += std::holds_alternative<Shape>(p.node->value) ? 1 : 0;
  return n;
}

}  // namespace

TEST_CASE("vector/dxf: decodes entities, layers and colours of the common drawing") {
  std::vector<std::string> warnings;
  auto scene = read_dxf_fixture("common_r2010.dxf");
  REQUIRE_OK(scene);
  REQUIRE(scene->pages.size() == 1);
  CHECK(scene->source_units == "mm");
  const Matrix& root = scene->pages[0].root.transform;
  CHECK(near(root.a, px_per_mm, 1e-9));
  CHECK(near(root.d, -px_per_mm, 1e-9));  // y up -> y down

  const Group* walls = layer_group(*scene, "WALLS");
  const Group* dashed = layer_group(*scene, "DASHED_LAYER");
  const Group* off = layer_group(*scene, "OFF_LAYER");
  const Group* frozen = layer_group(*scene, "FROZEN_LAYER");
  REQUIRE(walls != nullptr);
  REQUIRE(dashed != nullptr);
  REQUIRE(off != nullptr);
  REQUIRE(frozen != nullptr);
  CHECK(walls->visible);
  CHECK_FALSE(off->visible);
  CHECK_FALSE(frozen->visible);

  // LINE (0,0)-(100,0), red by layer, 0.50 mm line weight.
  const Shape* line = shape_near(*scene, {0, 0}, {100, 0});
  REQUIRE(line != nullptr);
  CHECK(rgb_is(line->stroke.paint.color, 255, 0, 0));
  CHECK(near(line->stroke.width, 0.5, 1e-9));
  // LWPOLYLINE with a bulge of 1 from (40,10) to (40,30): a half circle through (50,20).
  const Shape* poly = shape_near(*scene, {0, 10}, {50, 30}, 1e-3);
  REQUIRE(poly != nullptr);
  CHECK(distance_to(poly->path, {50, 20}) < 1e-3);
  CHECK(distance_to(poly->path, {40 + 10 * std::cos(0.5), 20 + 10 * std::sin(0.5)}) < 3e-3);  // cubic arcs
  // CIRCLE r=10 at (70,20) in ACI 3 (green), default 0.25 mm weight.
  const Shape* circle = shape_near(*scene, {60, 10}, {80, 30}, 1e-6);
  REQUIRE(circle != nullptr);
  CHECK(rgb_is(circle->stroke.paint.color, 0, 255, 0));
  CHECK(near(circle->stroke.width, 0.25, 1e-9));
  // ARC 0..90 deg on the dashed blue layer.
  const Shape* arc = shape_near(*scene, {120, 20}, {135, 35}, 1e-6);
  REQUIRE(arc != nullptr);
  CHECK(rgb_is(arc->stroke.paint.color, 0, 0, 255));
  CHECK(arc->stroke.dash.size() == 2);
  // SOLID 100..110, vertex order 1-2-4-3 gives a square (not a bow tie).
  const Shape* solid = shape_near(*scene, {100, 100}, {110, 110}, 1e-9);
  REQUIRE(solid != nullptr);
  CHECK(rgb_is(solid->fill.color, 255, 255, 0));
  CHECK(std::abs(signed_area(flatten(solid->path, 0.01).at(0).points)) == test::approx(100).abs(1e-9));
  // INSERT of MARK at (200,20), scale 2, rotation 45, magenta: BYBLOCK lines take the insert colour.
  const Shape* mark_circle = shape_near(*scene, {194, 14}, {206, 26}, 1e-6);
  REQUIRE(mark_circle != nullptr);
  CHECK(rgb_is(mark_circle->stroke.paint.color, 0, 255, 0));
  const double d = 10 / std::sqrt(2.0);
  const Shape* mark_line = shape_near(*scene, {200 - d, 20 - d}, {200 + d, 20 + d}, 1e-6);
  REQUIRE(mark_line != nullptr);
  CHECK(rgb_is(mark_line->stroke.paint.color, 255, 0, 255));
  // TEXT and its cap height.
  const auto runs = texts(*scene);
  REQUIRE(runs.size() == 1);
  CHECK(runs[0]->content == "Hello DXF");
  CHECK(near(runs[0]->font.size * 0.727, 5, 1e-9));
}

TEST_CASE("vector/dxf: every version from R12 to 2018, ASCII and binary, decodes the same drawing") {
  auto reference = read_dxf_fixture("common_r2010.dxf");
  REQUIRE_OK(reference);
  const std::vector<std::string> expected = signature(*reference);
  CHECK(expected.size() >= 10);
  for (const char* name : {"common_r12.dxf", "common_r12_binary.dxf", "common_r2000.dxf", "common_r2004.dxf",
                           "common_r2007.dxf", "common_r2010_binary.dxf", "common_r2013.dxf", "common_r2018.dxf"}) {
    INFO(name);
    auto scene = read_dxf_fixture(name);
    REQUIRE_OK(scene);
    const std::vector<std::string> got = signature(*scene);
    CHECK(got == expected);
  }
}

TEST_CASE("vector/dxf: splines, hatches, MTEXT, arrays, attributes and dimensions of the 2018 drawing") {
  auto scene = read_dxf_fixture("rich_r2018.dxf");
  REQUIRE_OK(scene);
  const std::vector<Placed> items = leaves(*scene);
  // Reference points evaluated with ezdxf 1.3.4 when the fixture was made.
  const std::vector<Point> spline_ref = {{0, 50}, {15, 59.6875}, {30, 52.5}, {45, 54.0625}, {60, 50}};
  const auto passes_all = [&](const std::vector<Point>& pts, double tol) {
    for (const Placed& p : items) {
      const auto* s = std::get_if<Shape>(&p.node->value);
      if (s == nullptr) continue;
      bool all = true;
      for (const Point& q : pts) all = all && distance_to(transformed(s->path, p.local), q) < tol;
      if (all) return true;
    }
    return false;
  };
  CHECK(passes_all(spline_ref, 2e-4));                 // cubic B-spline, exact Béziers
  CHECK(passes_all({{80, 50}, {90, 63.333333333333}, {100, 50}}, 1e-3));  // rational quadratic
  CHECK(passes_all({{0, 160}, {20, 175}, {40, 160}, {60, 175}}, 1e-9));    // fit points
  // ELLIPSE (150,20), major 20, ratio 0.5.
  CHECK(shape_near(*scene, {130, 10}, {170, 30}, 1e-6) != nullptr);
  // MINSERT 3 x 2 of MARK at (200,60), spacing 15 / 10: six green circles.
  for (int r = 0; r < 2; ++r) {
    for (int c = 0; c < 3; ++c) {
      const Point center{200.0 + 15 * c, 60.0 + 10 * r};
      CHECK(shape_near(*scene, center - Point{3, 3}, center + Point{3, 3}, 1e-6) != nullptr);
    }
  }
  // Solid hatch (yellow), edge-path hatch with an arc (cyan), pattern hatch (ACI 7 lines).
  const Shape* solid = shape_near(*scene, {60, 60}, {80, 80}, 1e-9);
  REQUIRE(solid != nullptr);
  CHECK(rgb_is(solid->fill.color, 255, 255, 0));
  const Shape* edges = shape_near(*scene, {120, 60}, {150, 80}, 1e-6);
  REQUIRE(edges != nullptr);
  CHECK(rgb_is(edges->fill.color, 0, 255, 255));
  CHECK(distance_to(edges->path, {150, 70}) < 1e-6);
  bool pattern = false;
  for (const Placed& p : items) {
    const auto* s = std::get_if<Shape>(&p.node->value);
    if (s == nullptr || s->fill.visible() || !rgb_is(s->stroke.paint.color, 0, 0, 0)) continue;
    const Rect b = bounds(s->path, p.local);
    if (b.x0 >= 160 - 1e-6 && b.x1 <= 180 + 1e-6 && b.y0 >= 60 - 1e-6 && b.y1 <= 80 + 1e-6) {
      std::size_t moves = 0;
      for (const Verb v : s->path.verbs) moves += v == Verb::Move ? 1 : 0;
      pattern = moves >= 8;  // ANSI31: 45 degree lines 3.175 apart
    }
  }
  CHECK(pattern);
  // True colour 0x123456 and a 1.00 mm line weight.
  const Shape* tc = shape_near(*scene, {0, 120}, {50, 120}, 1e-9);
  REQUIRE(tc != nullptr);
  CHECK(rgb_is(tc->stroke.paint.color, 0x12, 0x34, 0x56));
  const Shape* heavy = shape_near(*scene, {0, 125}, {50, 125}, 1e-9);
  REQUIRE(heavy != nullptr);
  CHECK(near(heavy->stroke.width, 1.0, 1e-9));
  // Nested blocks OUTER -> INNER (rotated 90 degrees) at (300,0).
  CHECK(shape_near(*scene, {300, 0}, {300, 10}, 1e-9) != nullptr);
  CHECK(shape_near(*scene, {320, 0}, {330, 0}, 1e-9) != nullptr);
  // OCS: extrusion (0,0,-1) mirrors x: the circle at OCS (10,10) is at WCS (-10,10).
  CHECK(shape_near(*scene, {-12, 8}, {-8, 12}, 1e-9) != nullptr);
  // Text: TEXT rotated 30 degrees, MTEXT paragraphs with a red run, ATTRIB, dimension text.
  std::map<std::string, const Placed*> by_text;
  for (const Placed& p : items) {
    if (const auto* t = std::get_if<Text>(&p.node->value)) by_text[t->content] = &p;
  }
  REQUIRE(by_text.contains("Middle"));
  const Matrix& m = by_text["Middle"]->local;
  CHECK(near(std::atan2(m.b, m.a) * 180 / std::numbers::pi, 30, 1e-9));
  CHECK(by_text.contains("First line"));
  CHECK(by_text.contains("Second"));
  REQUIRE(by_text.contains("red"));
  CHECK(rgb_is(std::get<Text>(by_text["red"]->node->value).fill.color, 255, 0, 0));
  CHECK(by_text["Second"]->local.apply({0, 0}).y < by_text["First line"]->local.apply({0, 0}).y);
  CHECK(by_text.contains("Pump 7"));
  CHECK(by_text.contains("100"));
}

TEST_CASE("vector/dxf: options select paper space and layers") {
  auto paper = read_dxf_fixture("rich_r2018.dxf", opts(R"({"space":"paper"})"));
  REQUIRE_OK(paper);
  CHECK(count_shapes(*paper) == 1);
  const Shape* line = shape_near(*paper, {0, 0}, {30, 30}, 1e-9);
  REQUIRE(line != nullptr);
  CHECK(rgb_is(line->stroke.paint.color, 255, 0, 0));

  auto walls = read_dxf_fixture("common_r2000.dxf", opts(R"({"dxf_layers":"walls, DASHED_LAYER"})"));
  REQUIRE_OK(walls);
  CHECK(scene_layers_only(*walls));
}

TEST_CASE("vector/dxf: code pages, unicode escapes, percent codes and units") {
  auto cyrillic = read_dxf_fixture("cp1251_r2000.dxf");
  REQUIRE_OK(cyrillic);
  REQUIRE(texts(*cyrillic).size() == 1);
  CHECK(texts(*cyrillic)[0]->content == "\u041F\u0440\u0438\u0432\u0435\u0442 \u03A9");  // Привет Ω (\U+03A9)
  CHECK(cyrillic->source_units == "m");
  CHECK(near(cyrillic->pages[0].root.transform.a, px_per_mm * 1000, 1e-6));

  auto inches = read_dxf_fixture("utf8_inches_r2018.dxf");
  REQUIRE_OK(inches);
  CHECK(inches->source_units == "in");
  CHECK(near(inches->pages[0].root.transform.a, 96, 1e-9));
  const std::string text = all_text(*inches);
  CHECK(text.find("Gr\u00FC\u00DFe \u2300 \u00D810 \u00B0 \u00B1") != std::string::npos);
  CHECK(text.find("A\u00A0B") != std::string::npos);  // \~
  CHECK(text.find("\\ {x} 1/2") != std::string::npos);  // \\ \{ \} and \S1/2;
  CHECK(text.find("Under") != std::string::npos);
  CHECK(text.find("Bold") != std::string::npos);
  bool bold = false;
  for (const Text* t : texts(*inches)) bold = bold || (t->content == "Bold" && t->font.weight == 700);
  CHECK(bold);
}

TEST_CASE("vector/dxf: DXF written by pstoedit and other programs") {
  for (const char* name : {"pstoedit_dxf14.dxf", "pstoedit_dxf9.dxf"}) {
    INFO(name);
    auto scene = read_dxf_fixture(name);
    REQUIRE_OK(scene);
    bool red = false, blue = false, lorem = false;
    for (const Placed& p : leaves(*scene)) {
      if (const auto* s = std::get_if<Shape>(&p.node->value)) {
        red = red || rgb_is(s->stroke.paint.color, 255, 0, 0);
        blue = blue || rgb_is(s->stroke.paint.color, 0, 0, 255);
      } else if (const auto* t = std::get_if<Text>(&p.node->value)) {
        lorem = lorem || t->content == "Lorem";
      }
    }
    CHECK(red);
    CHECK(blue);
    CHECK(lorem);
  }
}

TEST_CASE("vector/dxf: 3D faces become a mesh (+Z up to +Y up)") {
  std::vector<std::string> warnings;
  auto mesh = test::read_as<ir::Mesh>("dxf", test::fixture("vector/cad/mesh3d_r2018.dxf"), {}, &warnings);
  REQUIRE_OK(mesh);
  CHECK(mesh->valid());
  // 3DFACE quad + 3DFACE triangle + polyface cube (6) + 3x3 polygon mesh (4) + MESH cube (6).
  CHECK(mesh->face_count() == 18);
  float max_y = -1e30f, min_z = 1e30f;
  for (const ir::Vec3& v : mesh->positions) {
    max_y = std::max(max_y, v.y);
    min_z = std::min(min_z, v.z);
  }
  CHECK(max_y == 8.0f);    // the triangle's apex, DXF z = 8
  CHECK(min_z == -10.0f);  // DXF y = 10 -> -z
  CHECK(std::find(warnings.begin(), warnings.end(), "3D faces were converted from DXF's +Z up axes to +Y up.") !=
        warnings.end());
  auto r12 = test::read_as<ir::Mesh>("dxf", test::fixture("vector/cad/mesh3d_r12.dxf"));
  REQUIRE_OK(r12);
  CHECK(r12->face_count() == 2);
  // A drawing without 3D faces has no mesh.
  CHECK_STATUS(test::read_as<ir::Mesh>("dxf", test::fixture("vector/cad/common_r12.dxf")), Status::Decode);

  // Mesh -> DXF 3DFACEs -> mesh.
  auto written = test::write_from("dxf", *mesh);
  REQUIRE_OK(written);
  auto again = test::read_as<ir::Mesh>("dxf", *written);
  REQUIRE_OK(again);
  CHECK(again->face_count() == 18);
  CHECK(test::bounding_box(*again).min == test::bounding_box(*mesh).min);
  CHECK(test::bounding_box(*again).max == test::bounding_box(*mesh).max);
}

TEST_CASE("vector/dxf: write -> read round trip in R12, 2000 and 2018") {
  Scene scene;
  scene.source_units = "mm";
  Page page;
  page.width = 400;
  page.height = 300;
  Group layer;
  layer.layer = true;
  layer.layer_name = "Parts";
  Shape rect;
  add_rect(rect.path, 10, 20, 100, 50);
  rect.stroke.paint = Paint::solid(Color::rgb8(255, 0, 0));
  rect.stroke.width = 0.5 * px_per_mm;
  layer.children.push_back(Node{rect});
  Shape disc;
  add_ellipse(disc.path, 200, 150, 40, 40);
  disc.fill = Paint::solid(Color::rgb8(0, 0, 255));
  disc.stroke.paint = Paint::solid(Color::rgb8(0, 128, 0));
  layer.children.push_back(Node{disc});
  Shape curve;
  curve.path.move_to({20, 250});
  curve.path.cubic_to({60, 200}, {100, 300}, {140, 250});
  curve.stroke.paint = Paint::solid(Color::rgb8(10, 20, 30));
  layer.children.push_back(Node{curve});
  layer.children.push_back(Node{make_text("Hello", FontSpec{"sans-serif", 20, 400, false}, {50, 120},
                                          Paint::solid(Color::rgb8(255, 0, 255)))});
  page.root.children.push_back(Node{layer});
  scene.pages.push_back(page);

  for (const char* version : {"r12", "2000", "2018"}) {
    INFO(version);
    std::vector<std::string> warnings;
    auto bytes = write_scene("dxf", scene, test_context(), opts(std::format(R"({{"version":"{}"}})", version)));
    REQUIRE_OK(bytes);
    const std::string text = test::str(*bytes);
    CHECK(text.find(std::string(version) == "r12" ? "AC1009" : std::string(version) == "2000" ? "AC1015" : "AC1032") !=
          std::string::npos);
    auto back = read_scene("dxf", *bytes, test_context());
    REQUIRE_OK(back);
    const bool r12 = std::string(version) == "r12";
    CHECK(back->source_units == (r12 ? "unitless" : "mm"));  // R12 has no $INSUNITS; read as mm
    REQUIRE(layer_group(*back, "Parts") != nullptr);
    // Positions in mm: the page height is 300 px; y flips.
    const double k = 1 / px_per_mm;
    const Shape* r = shape_near(*back, {10 * k, (300 - 70) * k}, {110 * k, (300 - 20) * k}, 1e-5);
    REQUIRE(r != nullptr);
    CHECK(rgb_is(r->stroke.paint.color, 255, 0, 0));
    if (std::string(version) != "r12") CHECK(near(r->stroke.width, 0.5, 1e-9));
    const Shape* d = nullptr;
    for (const Placed& p : leaves(*back)) {
      const auto* s = std::get_if<Shape>(&p.node->value);
      if (s == nullptr) continue;
      const Rect b = bounds(s->path, p.local);
      if (near(b.x0, 160 * k, 1e-4) && near(b.x1, 240 * k, 1e-4) && near(b.y0, 110 * k, 1e-4)) {
        if (s->fill.visible()) d = s;
        else CHECK(rgb_is(s->stroke.paint.color, 0, 128, 0));
      }
    }
    if (std::string(version) == "r12") {
      CHECK(d == nullptr);  // no fills in R12
    } else {
      REQUIRE(d != nullptr);
      CHECK(rgb_is(d->fill.color, 0, 0, 255));
    }
    bool curve_ok = false;
    for (const Placed& p : leaves(*back)) {
      const auto* s = std::get_if<Shape>(&p.node->value);
      if (s != nullptr && (r12 ? !s->fill.visible() && bounds(s->path, p.local).x0 < 21 * k
                               : rgb_is(s->stroke.paint.color, 10, 20, 30))) {
        const Path mm = transformed(s->path, p.local);
        curve_ok = curve_ok || (distance_to(mm, {20 * k, 50 * k}) < 1e-3 && distance_to(mm, {140 * k, 50 * k}) < 1e-3 &&
                                distance_to(mm, {80 * k, (300 - 250) * k}) < 0.03);
      }
    }
    CHECK(curve_ok);
    const auto runs = texts(*back);
    REQUIRE(runs.size() == 1);
    CHECK(runs[0]->content == "Hello");
    CHECK(near(runs[0]->font.size, 20 * k, 1e-5));
    CHECK(rgb_is(runs[0]->fill.color, 255, 0, 255));
  }
  // R2018 keeps UTF-8 text, older versions escape it.
  Scene t;
  Page tp;
  tp.width = tp.height = 100;
  tp.root.children.push_back(Node{make_text("\u0416\u00E9", FontSpec{}, {10, 50}, Paint::solid(Color{}))});
  t.pages.push_back(tp);
  auto r2000 = write_scene("dxf", t, test_context(), opts(R"({"version":"2000"})"));
  REQUIRE_OK(r2000);
  CHECK(test::str(*r2000).find("\\U+0416\xE9") != std::string::npos);
  auto r2018 = write_scene("dxf", t, test_context(), opts(R"({"version":"2018"})"));
  REQUIRE_OK(r2018);
  CHECK(test::str(*r2018).find("\u0416\u00E9") != std::string::npos);
  for (const Bytes& b : {*r2000, *r2018}) {
    auto back = read_scene("dxf", b, test_context());
    REQUIRE_OK(back);
    CHECK(all_text(*back) == "\u0416\u00E9|");
  }
}

TEST_CASE("vector/dxf: writer options") {
  auto source = read_dxf_fixture("rich_r2018.dxf");
  REQUIRE_OK(source);
  auto mm = write_scene("dxf", *source, test_context());
  auto inch = write_scene("dxf", *source, test_context(), opts(R"({"units":"in"})"));
  auto nofill = write_scene("dxf", *source, test_context(), opts(R"({"fills":false})"));
  auto coarse = write_scene("dxf", *source, test_context(), opts(R"({"precision":2})"));
  REQUIRE_OK(mm);
  REQUIRE_OK(inch);
  REQUIRE_OK(nofill);
  REQUIRE_OK(coarse);
  CHECK(test::str(*mm).find("$INSUNITS\n 70\n4\n") != std::string::npos);
  CHECK(test::str(*inch).find("$INSUNITS\n 70\n1\n") != std::string::npos);
  CHECK(test::str(*mm).find("HATCH") != std::string::npos);
  CHECK(test::str(*nofill).find("HATCH") == std::string::npos);
  CHECK(coarse->size() < mm->size());
  auto back = read_scene("dxf", *inch, test_context());
  REQUIRE_OK(back);
  CHECK(back->source_units == "in");
  // Same drawing size whatever the unit.
  CHECK(near(back->pages[0].width, source->pages[0].width, 0.05));
  CHECK_STATUS(write_scene("dxf", *source, test_context(), opts(R"({"version":"r14"})")), Status::InvalidArgument);
  // Dashed strokes become line types; hidden layers stay hidden.
  auto common = read_dxf_fixture("common_r2010.dxf");
  REQUIRE_OK(common);
  auto rewritten = write_scene("dxf", *common, test_context());
  REQUIRE_OK(rewritten);
  CHECK(test::str(*rewritten).find("DASH1") != std::string::npos);
  auto reread = read_scene("dxf", *rewritten, test_context());
  REQUIRE_OK(reread);
  const Shape* arc = shape_near(*reread, {120, 20}, {135, 35}, 1e-5);
  REQUIRE(arc != nullptr);
  CHECK(arc->stroke.dash.size() == 2);
  REQUIRE(layer_group(*reread, "OFF_LAYER") != nullptr);
  CHECK_FALSE(layer_group(*reread, "OFF_LAYER")->visible);
  // Same-format conversion through the engine.
  const test::Converted same = test::convert(test::fixture("vector/cad/common_r2010_binary.dxf"), "", "dxf");
  CHECK(same.ok());
  CHECK(same.text().find("AC1015") != std::string::npos);
}

TEST_CASE("vector/dxf: detection of ASCII, binary, CRLF and indented files") {
  const Registry& registry = Registry::instance();
  const auto detect = [&](ByteView data, std::string_view name = {}) {
    const auto result = detect_format(registry, data, name);
    return result && result->format != nullptr ? result->format->id : std::string("?");
  };
  CHECK(detect(test::fixture("vector/cad/common_r2000.dxf")) == "dxf");
  CHECK(detect(test::fixture("vector/cad/common_r12_binary.dxf")) == "dxf");
  CHECK(detect(test::fixture("vector/cad/common_r2010_binary.dxf")) == "dxf");
  CHECK(detect(test::fixture("vector/cad/pstoedit_dxf9.dxf")) == "dxf");
  // CRLF line ends and unindented group codes.
  std::string crlf;
  for (const char c : test::fixture_text("vector/cad/common_r12.dxf")) {
    if (c == '\n') crlf += "\r\n";
    else crlf.push_back(c);
  }
  CHECK(detect(as_bytes(crlf)) == "dxf");
  auto crlf_scene = read_scene("dxf", as_bytes(crlf), test_context());
  REQUIRE_OK(crlf_scene);
  auto lf_scene = read_dxf_fixture("common_r12.dxf");
  REQUIRE_OK(lf_scene);
  CHECK(signature(*crlf_scene) == signature(*lf_scene));
  const std::string tiny = "999\ncomment\n0\nSECTION\n2\nENTITIES\n0\nLINE\n8\n0\n10\n0\n20\n0\n11\n5\n21\n5\n0\nENDSEC\n0\nEOF\n";
  CHECK(detect(as_bytes(tiny)) == "dxf");
  const std::string indented = "  0\r\nSECTION\r\n  2\r\nENTITIES\r\n  0\r\nCIRCLE\r\n 10\r\n1\r\n 20\r\n1\r\n 40\r\n2\r\n  0\r\nENDSEC\r\n  0\r\nEOF";
  CHECK(detect(as_bytes(indented)) == "dxf");
  auto circle = read_scene("dxf", as_bytes(indented), test_context());
  REQUIRE_OK(circle);
  CHECK(shape_near(*circle, {-1, -1}, {3, 3}, 1e-9) != nullptr);
  CHECK(detect(as_bytes("0\nSECTIONS of a text\n")) != "dxf");
}

TEST_CASE("vector/dxf: DWG files are detected and refused with the version") {
  const Bytes dwg = test::bytes("AC1027\0\0\0\0\0\x0F\x01"sv);
  const auto detected = detect_format(Registry::instance(), dwg, "");
  REQUIRE(detected.has_value());
  REQUIRE(detected->format != nullptr);
  CHECK(detected->format->id == "dwg");
  const test::Converted out = test::convert(dwg, "", "dxf");
  CHECK(out.status == Status::Decode);
  CHECK(out.message.find("DWG (AutoCAD 2013-2017, AC1027) is not supported yet; export DXF from your CAD program") !=
        std::string::npos);
  const test::Converted r14 = test::convert(test::bytes("AC1014\0\0\0\0\0\0\x01\x02\x03"sv), "dwg", "png");
  CHECK(r14.message.find("AutoCAD Release 14, AC1014") != std::string::npos);
}

TEST_CASE("vector/dxf: hostile drawings fail cleanly") {
  const auto with_blocks = [](const std::string& blocks, const std::string& entities) {
    return "0\nSECTION\n2\nBLOCKS\n" + blocks + "0\nENDSEC\n0\nSECTION\n2\nENTITIES\n" + entities + "0\nENDSEC\n0\nEOF\n";
  };
  // A block that inserts itself: drawn once, the recursion is cut with a warning.
  const std::string self = with_blocks("0\nBLOCK\n2\nA\n10\n0\n20\n0\n0\nLINE\n10\n0\n20\n0\n11\n1\n21\n0\n0\nINSERT\n2\nA\n10\n1\n20\n0\n0\nENDBLK\n",
                                       "0\nINSERT\n2\nA\n10\n0\n20\n0\n");
  std::vector<std::string> warnings;
  CHECK_OK(test::read_as<ir::Data>("dxf", as_bytes(self), {}, &warnings));
  CHECK(!warnings.empty());
  // Exponential nesting: 8 levels of 10 inserts each.
  std::string blocks = "0\nBLOCK\n2\nB8\n0\nLINE\n10\n0\n20\n0\n11\n1\n21\n1\n0\nENDBLK\n";
  for (int level = 7; level >= 0; --level) {
    blocks += std::format("0\nBLOCK\n2\nB{}\n", level);
    for (int i = 0; i < 10; ++i) blocks += std::format("0\nINSERT\n2\nB{}\n10\n{}\n20\n0\n", level + 1, i);
    blocks += "0\nENDBLK\n";
  }
  Limits small;
  small.max_archive_entries = 1000;  // block expansions are capped at ten times this
  CHECK_STATUS(test::read_as<ir::Data>("dxf", as_bytes(with_blocks(blocks, "0\nINSERT\n2\nB0\n")), {}, nullptr, small),
               Status::Limit);
  // A huge MINSERT array.
  const std::string array = with_blocks("0\nBLOCK\n2\nC\n0\nPOINT\n10\n0\n20\n0\n0\nENDBLK\n",
                                        "0\nINSERT\n2\nC\n70\n100000\n71\n100000\n44\n1\n45\n1\n");
  CHECK_STATUS(test::read_as<ir::Data>("dxf", as_bytes(array)), Status::Limit);
  // Malformed group codes and truncation.
  CHECK_STATUS(test::read_as<ir::Data>("dxf", as_bytes("0\nSECTION\nxx\nENTITIES\n"sv)), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Data>("dxf", as_bytes("0\nSECTION\n2\n"sv)), Status::Decode);
  const Bytes binary = test::fixture("vector/cad/common_r2010_binary.dxf");
  CHECK_STATUS(test::read_as<ir::Data>("dxf", ByteView(binary).first(200)), Status::Decode);
  // Absurd counts in a MESH and a HATCH do not allocate.
  const std::string mesh = with_blocks("", "0\nMESH\n92\n2000000000\n10\n0\n20\n0\n30\n0\n93\n2000000000\n90\n4\n");
  CHECK_OK(test::read_as<ir::Data>("dxf", as_bytes(mesh)));
  const std::string hatch = with_blocks("", "0\nHATCH\n70\n1\n91\n2000000000\n92\n2\n72\n0\n73\n1\n93\n2000000000\n10\n0\n20\n0\n");
  CHECK_OK(test::read_as<ir::Data>("dxf", as_bytes(hatch)));
}

TEST_CASE("vector/dxf: truncated and corrupted fixtures never crash") {
  for (const char* name : {"common_r12.dxf", "common_r12_binary.dxf", "common_r2010_binary.dxf", "rich_r2018.dxf",
                           "cp1251_r2000.dxf", "utf8_inches_r2018.dxf", "mesh3d_r2018.dxf", "mesh3d_r12.dxf",
                           "pstoedit_dxf14.dxf"}) {
    INFO(name);
    CHECK(test::fuzz_reader<ir::Data>("dxf", test::fixture(std::string("vector/cad/") + name), 96) > 0);
  }
  CHECK(test::fuzz_reader<ir::Mesh>("dxf", test::fixture("vector/cad/mesh3d_r2018.dxf"), 64) > 0);
}
