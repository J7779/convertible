// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "engine/detect.hpp"
#include "families/vector/cad_test_util.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"

using namespace cvt;
using namespace cvt::vector;
using namespace cvt::cadtest;
using namespace std::string_view_literals;

namespace {

Result<Scene> read_cgm_bytes(ByteView data) { return read_scene("cgm", data, test_context()); }
Result<Scene> read_cgm_fixture(std::string_view name) {
  return read_cgm_bytes(test::fixture(std::string("vector/cad/") + std::string(name)));
}

// Leaves described in VDC units (the page root maps VDC to px), sorted.
std::vector<std::string> signature(const Scene& scene) {
  std::vector<std::string> out;
  for (std::size_t page = 0; page < scene.pages.size(); ++page) {
    for (const Placed& p : leaves(scene, page)) {
      const auto c = [](const Color& col) {
        return std::format("{:02x}{:02x}{:02x}", std::lround(col.r * 255), std::lround(col.g * 255), std::lround(col.b * 255));
      };
      if (const auto* s = std::get_if<Shape>(&p.node->value)) {
        const Rect b = bounds(s->path, p.local);
        out.push_back(std::format("{} shape {:.1f} {:.1f} {:.1f} {:.1f} {} {} {:.2f}", page, b.x0, b.y0, b.x1, b.y1,
                                  s->fill.visible() ? c(s->fill.color) : "-",
                                  s->stroke.paint.visible() ? c(s->stroke.paint.color) : "-", s->stroke.width));
      } else if (const auto* t = std::get_if<Text>(&p.node->value)) {
        const Point o = p.local.apply({0, 0});
        out.push_back(std::format("{} text {} {:.1f} {:.1f} {:.2f}", page, t->content, o.x, o.y, t->font.size));
      } else if (const auto* pic = std::get_if<Picture>(&p.node->value)) {
        out.push_back(std::format("{} picture {}x{}", page, pic->pixels->width, pic->pixels->height));
      }
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

const Shape* find_shape(const Scene& scene, Rect want, double tol) {
  for (const Placed& p : leaves(scene)) {
    if (const auto* s = std::get_if<Shape>(&p.node->value)) {
      const Rect b = bounds(s->path, p.local);
      if (near(b.x0, want.x0, tol) && near(b.y0, want.y0, tol) && near(b.x1, want.x1, tol) && near(b.y1, want.y1, tol)) {
        return s;
      }
    }
  }
  return nullptr;
}

// A binary element: 16-bit header (long form when needed) and padded parameters.
void element(Bytes& out, int cls, int id, const Bytes& params) {
  const auto put16 = [&](unsigned v) {
    out.push_back(static_cast<std::byte>(v >> 8));
    out.push_back(static_cast<std::byte>(v & 0xFF));
  };
  if (params.size() < 31) {
    put16(static_cast<unsigned>((cls << 12) | (id << 5) | static_cast<int>(params.size())));
  } else {
    put16(static_cast<unsigned>((cls << 12) | (id << 5) | 31));
    put16(static_cast<unsigned>(params.size()));
  }
  out.insert(out.end(), params.begin(), params.end());
  if (params.size() % 2 == 1) out.push_back(std::byte{0});
}
Bytes i16(std::initializer_list<int> values) {
  Bytes b;
  for (const int v : values) {
    b.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
    b.push_back(static_cast<std::byte>(v & 0xFF));
  }
  return b;
}
Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes b;
  for (const Bytes& p : parts) b.insert(b.end(), p.begin(), p.end());
  return b;
}
Bytes u8(std::initializer_list<int> values) {
  Bytes b;
  for (const int v : values) b.push_back(static_cast<std::byte>(v));
  return b;
}

}  // namespace

TEST_CASE("vector/cgm: binary and clear-text files from GNU plotutils decode alike") {
  for (const auto& [binary, clear] : {std::pair{"shapes_binary.cgm", "shapes_clear.cgm"}, std::pair{"graph_binary.cgm", "graph_clear.cgm"}}) {
    INFO(binary);
    auto a = read_cgm_fixture(binary);
    auto b = read_cgm_fixture(clear);
    REQUIRE_OK(a);
    REQUIRE_OK(b);
    CHECK(signature(*a) == signature(*b));
    CHECK(near(a->pages[0].width, b->pages[0].width, 1e-3));  // binary scale factor is a 32-bit float
  }
}

TEST_CASE("vector/cgm: decodes the primitives of the shapes picture") {
  auto scene = read_cgm_fixture("shapes_clear.cgm");
  REQUIRE_OK(scene);
  REQUIRE(scene->pages.size() == 1);
  CHECK(scene->source_units == "mm");
  // VDCEXT -8191..8191 with SCALEMODE metric 0.01240386 mm per unit.
  CHECK(near(scene->pages[0].width, 16382 * 0.01240386 * px_per_mm, 1e-6));
  const Shape* rect = find_shape(*scene, {-3174, 973, -1126, 1997}, 1e-9);
  REQUIRE(rect != nullptr);
  CHECK_FALSE(rect->fill.visible());  // INTSTYLE empty, EDGEVIS on
  CHECK(rgb_is(rect->stroke.paint.color, 0, 0, 0));
  CHECK(near(rect->stroke.width, 19, 1e-9));  // EDGEWIDTH 19 in abs mode
  CHECK(find_shape(*scene, {410 - 512, 1485 - 512, 410 + 512, 1485 + 512}, 1e-9) != nullptr);  // CIRCLE
  // ARCCTR centre (2560,-563), start (-615,0), end (614,0), radius 615: the lower half.
  const Shape* arc = find_shape(*scene, {2560 - 615, -563 - 615, 2560 + 615, -563}, 0.5);
  REQUIRE(arc != nullptr);
  CHECK_FALSE(arc->fill.visible());
  // ELLIPSE centre (-1126,-563), conjugate diameters to (-307,-563) and (-1126,-154).
  CHECK(find_shape(*scene, {-1126 - 819, -563 - 409, -1126 + 819, -563 + 409}, 1e-6) != nullptr);
  // The filled arrow head and the Bézier spline.
  const Shape* head = find_shape(*scene, {-307, 1434, -102, 1536}, 1e-9);
  REQUIRE(head != nullptr);
  CHECK(rgb_is(head->fill.color, 0, 0, 0));
  bool bezier = false;
  for (const Placed& p : leaves(*scene)) {
    if (const auto* s = std::get_if<Shape>(&p.node->value)) {
      bezier = bezier || std::count(s->path.verbs.begin(), s->path.verbs.end(), Verb::Cubic) == 2;
    }
  }
  CHECK(bezier);
  // RESTRTEXT with CHARHEIGHT 204, centred.
  const auto runs = texts(*scene);
  REQUIRE(runs.size() == 2);
  CHECK(runs[0]->content == "Box");
  CHECK(runs[1]->content == "Lorem");
  CHECK(near(runs[0]->font.size * 0.727, 204, 1e-9));
  // Colours of the graph: a red data line.
  auto graph = read_cgm_fixture("graph_binary.cgm");
  REQUIRE_OK(graph);
  bool red = false;
  for (const Placed& p : leaves(*graph)) {
    if (const auto* s = std::get_if<Shape>(&p.node->value)) red = red || rgb_is(s->stroke.paint.color, 255, 0, 0);
  }
  CHECK(red);
}

TEST_CASE("vector/cgm: clear text attributes, polygon sets, markers, cell arrays and pages") {
  const std::string text = R"(BEGMF "t"; % comment %
MFVERSION 1; VDCTYPE integer; COLRVALUEEXT 0 0 0 255 255 255;
BEGPIC "one"; COLRMODE indexed; VDCEXT (0,0) (100,100); LINEWIDTHMODE abs; BEGPICBODY;
COLRTABLE 2 255 0 0 0 0 255;
LINECOLR 2; LINEWIDTH 2; LINETYPE 2; LINE (0,0) (50,0) (50,50);
INTSTYLE solid; FILLCOLR 3; EDGEVIS off;
POLYGONSET (10,10) vis (20,10) vis (20,20) closevis (12,12) vis (18,12) vis (18,18) closevis;
MARKERTYPE 4; MARKERCOLR 2; MARKER (80,80);
CELLARRAY (60,10) (70,0) (70,10) 2 2 8 2 3 3 2;
APNDTEXT final "x";
ENDPIC;
BEGPIC "two"; VDCEXT (0,0) (10,10); BEGPICBODY; DISJTLINE (0,0) (1,1) (2,2) (3,3); ENDPIC;
ENDMF;)";
  auto scene = read_cgm_bytes(as_bytes(text));
  REQUIRE_OK(scene);
  REQUIRE(scene->pages.size() == 2);
  const Shape* line = find_shape(*scene, {0, 0, 50, 50}, 1e-9);
  REQUIRE(line != nullptr);
  CHECK(rgb_is(line->stroke.paint.color, 255, 0, 0));
  CHECK(line->stroke.width == 2);
  CHECK(line->stroke.dash.size() == 2);
  const Shape* set = find_shape(*scene, {10, 10, 20, 20}, 1e-9);
  REQUIRE(set != nullptr);
  CHECK(rgb_is(set->fill.color, 0, 0, 255));
  CHECK(set->fill_rule == FillRule::EvenOdd);
  CHECK(std::count(set->path.verbs.begin(), set->path.verbs.end(), Verb::Close) == 2);
  const Picture* picture = nullptr;
  Matrix placed;
  for (const Placed& p : leaves(*scene)) {
    if (const auto* pic = std::get_if<Picture>(&p.node->value)) {
      picture = pic;
      placed = p.local;
    }
  }
  REQUIRE(picture != nullptr);
  REQUIRE(picture->pixels->width == 2);
  CHECK(picture->pixels->rgba[0] == 255);   // index 2 = red
  CHECK(picture->pixels->rgba[6] == 255);   // index 3 = blue
  CHECK(picture->pixels->rgba[12 + 0] == 255);
  const Rect where = transform_rect(picture->bounds, placed);
  CHECK(where == Rect{60, 0, 70, 10});
  CHECK(all_text(*scene) == "x|");
  CHECK(signature(*scene).back().starts_with("1 shape 0.0 0.0 3.0 3.0"));
}

TEST_CASE("vector/cgm: binary precisions, direct colour and a packed cell array") {
  Bytes cgm;
  element(cgm, 0, 1, u8({1, 'b'}));                         // BEGIN METAFILE "b"
  element(cgm, 1, 7, i16({16}));                            // COLOUR PRECISION 16 (integer prec 16)
  element(cgm, 1, 10, i16({0, 0, 0, 65535, 65535, 65535})); // COLOUR VALUE EXTENT
  element(cgm, 0, 3, u8({0}));                              // BEGIN PICTURE ""
  element(cgm, 2, 2, i16({1}));                             // COLOUR SELECTION MODE direct
  element(cgm, 2, 6, i16({0, 0, 1000, 500}));               // VDC EXTENT
  element(cgm, 0, 4, {});                                   // BEGIN PICTURE BODY
  element(cgm, 5, 23, i16({0, 65535, 0}));                  // FILL COLOUR green
  element(cgm, 5, 22, i16({1}));                            // INTERIOR STYLE solid
  element(cgm, 4, 11, i16({100, 100, 300, 200}));           // RECTANGLE
  // CELL ARRAY 3x1, local precision 8, packed: P, Q, R, nx, ny, prec, mode, colours.
  element(cgm, 4, 9, cat({i16({400, 400, 430, 390, 430, 400, 3, 1, 8, 1}), u8({255, 0, 0, 0, 255, 0, 0, 0, 255})}));
  // A long-form, partitioned POLYLINE.
  Bytes poly = i16({0, 0});
  for (int i = 1; i <= 20; ++i) poly = cat({poly, i16({i * 10, (i % 2) * 50})});
  const auto put16 = [&](unsigned v) {
    cgm.push_back(static_cast<std::byte>(v >> 8));
    cgm.push_back(static_cast<std::byte>(v & 0xFF));
  };
  put16((4u << 12) | (1u << 5) | 31u);
  put16(0x8000u | 40u);
  cgm.insert(cgm.end(), poly.begin(), poly.begin() + 40);
  put16(static_cast<unsigned>(poly.size() - 40));
  cgm.insert(cgm.end(), poly.begin() + 40, poly.end());
  element(cgm, 0, 5, {});  // END PICTURE
  element(cgm, 0, 2, {});  // END METAFILE

  const auto detected = detect_format(Registry::instance(), cgm, "");
  REQUIRE(detected.has_value());
  CHECK(detected->format->id == "cgm");
  auto scene = read_cgm_bytes(cgm);
  REQUIRE_OK(scene);
  CHECK(scene->source_units == "px");
  CHECK(near(scene->pages[0].width, 1000, 1e-9));  // abstract: 1000 px on the long side
  const Shape* rect = find_shape(*scene, {100, 100, 300, 200}, 1e-9);
  REQUIRE(rect != nullptr);
  CHECK(rgb_is(rect->fill.color, 0, 255, 0));
  CHECK(find_shape(*scene, {0, 0, 200, 50}, 1e-9) != nullptr);
  const Picture* picture = nullptr;
  for (const Placed& p : leaves(*scene)) {
    if (const auto* pic = std::get_if<Picture>(&p.node->value)) picture = pic;
  }
  REQUIRE(picture != nullptr);
  REQUIRE(picture->pixels->width == 3);
  const std::vector<std::uint8_t> want = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255};
  CHECK(picture->pixels->rgba == want);

  CHECK(test::fuzz_reader<ir::Data>("cgm", cgm, 128) > 0);
}

TEST_CASE("vector/cgm: detection and damaged input") {
  const auto detect = [](ByteView data) {
    const auto result = detect_format(Registry::instance(), data, "");
    return result && result->format != nullptr ? result->format->id : std::string("?");
  };
  for (const char* name : {"shapes_binary.cgm", "shapes_clear.cgm", "graph_binary.cgm", "graph_clear.cgm"}) {
    INFO(name);
    CHECK(detect(test::fixture(std::string("vector/cad/") + name)) == "cgm");
    CHECK(test::fuzz_reader<ir::Data>("cgm", test::fixture(std::string("vector/cad/") + name), 128) > 0);
  }
  CHECK_STATUS(test::read_as<ir::Data>("cgm", as_bytes("BEGMF; ENDMF;"sv)), Status::Decode);  // no picture
  // A cell array larger than the pixel limit.
  Limits small;
  small.max_pixels = 1000;
  CHECK_STATUS(test::read_as<ir::Data>("cgm",
                                       as_bytes("BEGMF; BEGPIC; BEGPICBODY; CELLARRAY (0,0) (1,1) (1,0) 100 100 8 1; ENDPIC; ENDMF;"sv),
                                       {}, nullptr, small),
               Status::Limit);
  const Bytes binary = test::fixture("vector/cad/shapes_binary.cgm");
  CHECK_STATUS(test::read_as<ir::Data>("cgm", ByteView(binary).first(binary.size() / 2 + 1)), Status::Decode);
}
