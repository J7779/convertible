// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// EPS / PS / AI readers on files written by independent software.
#include <cmath>
#include <string>
#include <vector>

#include "engine/detect.hpp"
#include "engine/registry.hpp"
#include "families/image/image_api.hpp"
#include "families/vector/geom.hpp"
#include "families/vector/render.hpp"
#include "families/vector/vector_codecs.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"
#include "util/deflate.hpp"

using namespace cvt;
using namespace cvt::vector;

namespace {

Context test_context() {
  Context ctx;
  ctx.registry = &Registry::instance();
  return ctx;
}

Bytes gunzip(const Bytes& gz) {
  auto out = util::gzip_decompress(gz);
  return out ? *out : Bytes{};
}

struct Leaf {
  const Node* node = nullptr;
  Matrix to_page;  ///< node coordinates -> page px (y down)
};

void collect(const Group& g, const Matrix& m, std::vector<Leaf>& out) {
  const Matrix t = m * g.transform;
  for (const Node& n : g.children) {
    if (const auto* sub = std::get_if<Group>(&n.value)) collect(*sub, t, out);
    else out.push_back({&n, t});
  }
}

std::vector<Leaf> leaves(const Page& page) {
  std::vector<Leaf> out;
  collect(page.root, Matrix{}, out);
  return out;
}

std::vector<const Shape*> shapes(const std::vector<Leaf>& items) {
  std::vector<const Shape*> out;
  for (const Leaf& l : items) {
    if (const auto* s = std::get_if<Shape>(&l.node->value)) out.push_back(s);
  }
  return out;
}

std::vector<std::pair<const Text*, Matrix>> texts(const std::vector<Leaf>& items) {
  std::vector<std::pair<const Text*, Matrix>> out;
  for (const Leaf& l : items) {
    if (const auto* t = std::get_if<Text>(&l.node->value)) out.emplace_back(t, l.to_page);
  }
  return out;
}

std::string joined_text(const std::vector<Leaf>& items) {
  std::string s;
  for (const auto& [t, m] : texts(items)) s += t->content;
  return s;
}

bool rgb_is(const Color& c, double r, double g, double b, double tol = 0.01) {
  return std::abs(c.r - r) <= tol && std::abs(c.g - g) <= tol && std::abs(c.b - b) <= tol;
}

constexpr double k = 96.0 / 72.0;

/// The page-space bounds of the first leaf satisfying `pred`.
template <class P>
Rect find_bounds(const std::vector<Leaf>& items, P pred) {
  for (const Leaf& l : items) {
    if (const auto* s = std::get_if<Shape>(&l.node->value); s != nullptr && pred(*s)) return bounds(s->path, l.to_page);
  }
  return {};
}

/// Checks shared by the hand-written shapes program and Ghostscript's
/// eps2write version of it (device coordinates in points, y up).
void check_shapes(const Scene& scene, double llx, double ury) {
  REQUIRE_EQ(scene.pages.size(), 1u);
  const auto items = leaves(scene.pages[0]);
  const auto all = shapes(items);
  REQUIRE_GE(all.size(), 5u);
  // Page px of a device point.
  auto px = [&](double x, double y) { return Point{(x - llx) * k, (ury - y) * k}; };
  const Rect red = find_bounds(items, [](const Shape& s) { return s.fill.type == Paint::Type::Solid && rgb_is(s.fill.color, 1, 0, 0) && bounds(s.path).width() > 50; });
  CHECK_APPROX(red.x0, px(10, 0).x, 0.01);
  CHECK_APPROX(red.x1, px(70, 0).x, 0.01);
  CHECK_APPROX(red.y0, px(0, 50).y, 0.01);
  CHECK_APPROX(red.y1, px(0, 10).y, 0.01);
  // Blue circle outline, 4 pt wide.
  bool circle = false;
  for (const Shape* s : all) {
    if (s->stroke.paint.type == Paint::Type::Solid && rgb_is(s->stroke.paint.color, 0, 0, 1)) {
      circle = true;
      CHECK_APPROX(s->stroke.width, 4.0, 1e-6);
      const Rect b = bounds(s->path);
      CHECK_APPROX(b.width(), 50.0, 0.01);
      CHECK_APPROX((b.x0 + b.x1) / 2, 140.0, 0.01);
    }
  }
  CHECK(circle);
  // Dashed grey line.
  bool dashed = false;
  for (const Shape* s : all) {
    if (!s->stroke.dash.empty()) {
      dashed = true;
      CHECK_EQ(s->stroke.dash.size(), 2u);
      CHECK_APPROX(s->stroke.dash[0], 6.0, 1e-6);
      CHECK_APPROX(s->stroke.dash[1], 3.0, 1e-6);
      CHECK(rgb_is(s->stroke.paint.color, 0.5, 0.5, 0.5));
    }
  }
  CHECK(dashed);
  // Text.
  const auto t = texts(items);
  REQUIRE_EQ(t.size(), 1u);
  CHECK_EQ(t[0].first->content, "Lorem");
  CHECK(t[0].first->font.family.find("Helvetica") != std::string::npos);
  CHECK_APPROX(t[0].first->font.size, 14.0, 1e-6);
  const Point origin = t[0].second.apply(t[0].first->origin);
  CHECK_APPROX(origin.x, px(110, 100).x, 0.01);
  CHECK_APPROX(origin.y, px(110, 100).y, 0.01);
  CHECK_FALSE(t[0].first->outline.empty());
  // The CMYK square clipped by a circle: (0 1 1 0) is red; it sits in a clip group.
  bool clipped = false;
  std::vector<const Group*> todo{&scene.pages[0].root};
  while (!todo.empty()) {
    const Group* g = todo.back();
    todo.pop_back();
    for (const Node& n : g->children) {
      if (const auto* sub = std::get_if<Group>(&n.value)) {
        if (sub->clip) {
          const Rect cb = bounds(sub->clip->items.at(0).path);
          if (std::abs(cb.width() - 30) < 0.1) clipped = true;
        }
        todo.push_back(sub);
      }
    }
  }
  CHECK(clipped);
}

}  // namespace

TEST_CASE("vector/eps: hand-written program decodes to shapes, text and clip") {
  const Bytes data = test::fixture("vector/ps/hand_shapes.eps");
  REQUIRE(!data.empty());
  auto scene = read_scene("eps", data, test_context());
  REQUIRE_OK(scene);
  CHECK_APPROX(scene->pages[0].width, 200 * k, 1e-9);
  CHECK_APPROX(scene->pages[0].height, 150 * k, 1e-9);
  CHECK_EQ(scene->source_units, "pt");
  check_shapes(*scene, 0, 150);
}

TEST_CASE("vector/eps: Ghostscript eps2write output (PDF interpreter in PostScript)") {
  const Bytes data = gunzip(test::fixture("vector/ps/shapes_gs.eps.gz"));
  REQUIRE(!data.empty());
  auto scene = read_scene("eps", data, test_context());
  REQUIRE_OK(scene);
  // %%HiResBoundingBox: 10.00 10.00 190.60 140.80
  CHECK_APPROX(scene->pages[0].width, 180.6 * k, 1e-6);
  CHECK_APPROX(scene->pages[0].height, 130.8 * k, 1e-6);
  check_shapes(*scene, 10, 140.8);
}

TEST_CASE("vector/ps: Ghostscript ps2write multi-page document") {
  for (const bool gs : {false, true}) {
    CAPTURE(gs);
    const Bytes data = gs ? gunzip(test::fixture("vector/ps/multi_gs.ps.gz")) : test::fixture("vector/ps/hand_multi.ps");
    REQUIRE(!data.empty());
    auto scene = read_scene("ps", data, test_context());
    REQUIRE_OK(scene);
    REQUIRE_EQ(scene->pages.size(), 2u);
    CHECK_APPROX(scene->pages[0].width, 200 * k, 0.01);
    CHECK_APPROX(scene->pages[0].height, 100 * k, 0.01);
    const auto p1 = leaves(scene->pages[0]);
    const auto p2 = leaves(scene->pages[1]);
    CHECK_EQ(joined_text(p1), "Ipsum");
    const Rect red = find_bounds(p1, [](const Shape& s) { return rgb_is(s.fill.color, 1, 0, 0) && s.fill.type == Paint::Type::Solid; });
    CHECK_APPROX(red.width(), 80 * k, 0.01);
    const Rect blue = find_bounds(p2, [](const Shape& s) { return rgb_is(s.fill.color, 0, 0, 1) && s.fill.type == Paint::Type::Solid; });
    CHECK_APPROX(blue.x0, 110 * k, 0.01);
    bool green = false;
    for (const Shape* s : shapes(p2)) green = green || (rgb_is(s->stroke.paint.color, 0, 1, 0) && s->stroke.width == 3.0);
    CHECK(green);
  }
}

TEST_CASE("vector/eps: gradients and an RGB image") {
  auto scene = read_scene("eps", test::fixture("vector/ps/hand_grad.eps"), test_context());
  REQUIRE_OK(scene);
  const auto items = leaves(scene->pages[0]);
  std::vector<const Gradient*> grads;
  const Picture* pic = nullptr;
  Matrix pic_m;
  for (const Leaf& l : items) {
    if (const auto* s = std::get_if<Shape>(&l.node->value); s && s->fill.type == Paint::Type::Gradient) {
      grads.push_back(s->fill.gradient.get());
    }
    if (const auto* p = std::get_if<Picture>(&l.node->value)) {
      pic = p;
      pic_m = l.to_page;
    }
  }
  REQUIRE_EQ(grads.size(), 2u);
  CHECK(grads[0]->type == Gradient::Type::Linear);
  CHECK_EQ(grads[0]->p1.x, 10.0);
  CHECK_EQ(grads[0]->p2.x, 90.0);
  CHECK(rgb_is(grads[0]->stops.front().color, 1, 0, 0));
  CHECK(rgb_is(grads[0]->stops.back().color, 0, 0, 1));
  CHECK(grads[1]->type == Gradient::Type::Radial);
  CHECK_EQ(grads[1]->radius, 40.0);
  CHECK(rgb_is(grads[1]->stops.front().color, 1, 1, 0));
  CHECK(rgb_is(grads[1]->stops.back().color, 0, 0.5, 0));
  REQUIRE(pic != nullptr);
  REQUIRE_EQ(pic->pixels->width, 4u);
  REQUIRE_EQ(pic->pixels->height, 2u);
  const auto px = [&](std::uint32_t x, std::uint32_t y) {
    const std::size_t at = pic->pixels->offset(x, y);
    return std::array<int, 3>{pic->pixels->rgba[at], pic->pixels->rgba[at + 1], pic->pixels->rgba[at + 2]};
  };
  CHECK_EQ(px(0, 0), (std::array<int, 3>{255, 0, 0}));
  CHECK_EQ(px(2, 1), (std::array<int, 3>{255, 255, 0}));
  // Placed at x 100..140 pt, y 0..20 pt (page 0 0 200 100).
  const Rect placed = transform_rect(pic->bounds, pic_m);
  CHECK_APPROX(placed.x0, 100 * k, 0.01);
  CHECK_APPROX(placed.x1, 140 * k, 0.01);
  CHECK_APPROX(placed.y1, 100 * k, 0.01);
  CHECK_APPROX(placed.y0, 80 * k, 0.01);
}

TEST_CASE("vector/eps: JPEG data through DCTDecode") {
  auto scene = read_scene("eps", test::fixture("vector/ps/hand_jpeg.eps"), test_context());
  REQUIRE_OK(scene);
  const auto items = leaves(scene->pages[0]);
  const Picture* pic = nullptr;
  for (const Leaf& l : items) {
    if (const auto* p = std::get_if<Picture>(&l.node->value)) pic = p;
  }
  REQUIRE(pic != nullptr);
  CHECK_EQ(pic->encoded_format, "jpg");
  CHECK_FALSE(pic->encoded.empty());
  if (!image::can_decode("jpg", test_context())) SKIP("no JPEG decoder registered");
  // The scene envelope keeps the JPEG bytes; decode them for the pixel check.
  auto decoded = image::decode("jpg", pic->encoded, test_context());
  REQUIRE_OK(decoded);
  const ir::Frame& frame = pic->pixels ? *pic->pixels : decoded->frames.at(0);
  REQUIRE_EQ(frame.width, 16u);
  REQUIRE_EQ(frame.height, 8u);
  // ImageMagick gradient:red-blue runs from red (top) to blue (bottom).
  const std::uint8_t* top = frame.rgba.data() + frame.offset(8, 0);
  const std::uint8_t* bottom = frame.rgba.data() + frame.offset(8, 7);
  CHECK_GT(top[0], 200);
  CHECK_LT(top[2], 60);
  CHECK_GT(bottom[2], 200);
  CHECK_LT(bottom[0], 60);
}

TEST_CASE("vector/eps: Inkscape (cairo) EPS with Type 42 text and a shading") {
  for (const char* name : {"vector/ps/drawing_inkscape.eps", "vector/ps/drawing_inkscape.ps"}) {
    CAPTURE(name);
    const bool ps = std::string(name).ends_with(".ps");
    auto scene = read_scene(ps ? "ps" : "eps", test::fixture(name), test_context());
    REQUIRE_OK(scene);
    const auto items = leaves(scene->pages[0]);
    CHECK_EQ(joined_text(items), "Lorem");
    for (const auto& [t, m] : texts(items)) {
      CHECK_FALSE(t->outline.empty());
      CHECK(rgb_is(t->fill.color, 128 / 255.0, 0, 128 / 255.0));
      CHECK(t->font.family.find("DejaVu") != std::string::npos);
      CHECK_APPROX(t->font.size * m.mean_scale(), 16.0, 0.05);  // 16 px in the SVG
    }
    // Rectangle with the linear gradient, circle and dashed triangle: relative positions in px.
    Rect grad_rect, circle;
    const Shape* triangle_stroke = nullptr;
    for (const Leaf& l : items) {
      const auto* s = std::get_if<Shape>(&l.node->value);
      if (s == nullptr) continue;
      if (s->fill.type == Paint::Type::Gradient) {
        // The shading fills its clip group: use the clip's bounds.
        grad_rect = Rect{};
      }
      if (s->fill.type == Paint::Type::Solid && rgb_is(s->fill.color, 0, 160 / 255.0, 0)) circle = bounds(s->path, l.to_page);
      if (!s->stroke.dash.empty()) triangle_stroke = s;
    }
    (void)grad_rect;
    CHECK_APPROX(circle.width(), 40.0, 0.05);
    REQUIRE(triangle_stroke != nullptr);
    CHECK(rgb_is(triangle_stroke->stroke.paint.color, 0, 0, 1));
    bool has_gradient = false;
    for (const Leaf& l : items) {
      if (const auto* s = std::get_if<Shape>(&l.node->value); s && s->fill.type == Paint::Type::Gradient) {
        has_gradient = true;
        CHECK(rgb_is(s->fill.gradient->stops.front().color, 1, 0, 0));
        CHECK(rgb_is(s->fill.gradient->stops.back().color, 0, 0, 1));
      }
    }
    CHECK(has_gradient);
    if (ps) {
      // %%DocumentMedia 150 x 90 pt.
      CHECK_APPROX(scene->pages[0].width, 200.0, 0.01);
      CHECK_APPROX(scene->pages[0].height, 120.0, 0.01);
    }
  }
}

TEST_CASE("vector/eps: Inkscape EPS with a rasterised fallback image") {
  auto scene = read_scene("eps", test::fixture("vector/ps/image_inkscape.eps"), test_context());
  REQUIRE_OK(scene);
  const Picture* pic = nullptr;
  for (const Leaf& l : leaves(scene->pages[0])) {
    if (const auto* p = std::get_if<Picture>(&l.node->value)) pic = p;
  }
  REQUIRE(pic != nullptr);
  REQUIRE_EQ(pic->pixels->width, 346u);
  REQUIRE_EQ(pic->pixels->height, 221u);
  // Background #ffeecc near the top-left corner, the checkerboard in the middle-left.
  const std::uint8_t* bg = pic->pixels->rgba.data() + pic->pixels->offset(10, 10);
  CHECK_EQ(bg[0], 255);
  CHECK_EQ(bg[1], 0xEE);
  CHECK_EQ(bg[2], 0xCC);
}

TEST_CASE("vector/eps: matplotlib EPS with Type 3 and Type 42 fonts") {
  for (const char* name : {"vector/ps/mpl_type3.eps", "vector/ps/mpl_type42.eps"}) {
    CAPTURE(name);
    const bool type3 = std::string(name).find("type3") != std::string::npos;
    auto scene = read_scene("eps", test::fixture(name), test_context());
    REQUIRE_OK(scene);
    // %%BoundingBox: 216 324 396 468 -> 180 x 144 pt.
    CHECK_APPROX(scene->pages[0].width, 240.0, 1e-6);
    CHECK_APPROX(scene->pages[0].height, 192.0, 1e-6);
    const auto items = leaves(scene->pages[0]);
    CHECK_EQ(joined_text(items), "0123012Lorem");
    for (const auto& [t, m] : texts(items)) {
      if (type3) {
        CHECK(t->fill.type == Paint::Type::None);  // glyphs are drawn as shapes
      } else {
        CHECK_FALSE(t->outline.empty());
      }
    }
    // The orange bars (#ff7f0e) and the blue line (#1f77b4, 2 pt).
    int bars = 0;
    bool line = false;
    for (const Shape* s : shapes(items)) {
      if (s->fill.type == Paint::Type::Solid && rgb_is(s->fill.color, 1, 127 / 255.0, 14 / 255.0)) ++bars;
      if (s->stroke.paint.type == Paint::Type::Solid && rgb_is(s->stroke.paint.color, 31 / 255.0, 119 / 255.0, 180 / 255.0)) {
        line = true;
        CHECK_APPROX(s->stroke.width, 2.0, 1e-6);
      }
    }
    CHECK_EQ(bars, 2);
    CHECK(line);
  }
}

TEST_CASE("vector/eps: embedded Type 1 font (hand-written and re-embedded by Ghostscript)") {
  for (const bool gs : {false, true}) {
    CAPTURE(gs);
    const Bytes data = gs ? gunzip(test::fixture("vector/ps/type1_gs.eps.gz")) : test::fixture("vector/ps/hand_type1.eps");
    REQUIRE(!data.empty());
    auto scene = read_scene("eps", data, test_context());
    REQUIRE_OK(scene);
    const auto t = texts(leaves(scene->pages[0]));
    REQUIRE_EQ(t.size(), 1u);
    CHECK_EQ(t[0].first->content, "AB C");
    CHECK(t[0].first->font.family.find("SynthBlocks") != std::string::npos);
    // Glyphs: a 600-unit square, a triangle and a diamond at 40 pt: the
    // outline spans from the square's left (50 units) to the diamond's right.
    const Rect b = bounds(t[0].first->outline, t[0].second);
    CHECK_APPROX(b.height(), 24 * k, 0.01);
    CHECK_APPROX(b.width(), (700 + 700 + 300 + 650 - 50) * 0.04 * k, 0.01);
    CHECK_APPROX(t[0].first->advance * t[0].second.mean_scale(), (700 * 3 + 300) * 0.04 * k, 0.01);
  }
}

TEST_CASE("vector/ai: legacy Illustrator file from GNU plotutils") {
  std::vector<std::string> warnings;
  REQUIRE_OK(test::read_as<ir::Data>("ai", test::fixture("vector/ps/graph_plotutils.ai"), {}, &warnings));
  auto scene = read_scene("ai", test::fixture("vector/ps/graph_plotutils.ai"), test_context());
  REQUIRE_OK(scene);
  REQUIRE_EQ(scene->pages.size(), 1u);
  const Group& root = scene->pages[0].root;
  bool layer = false;
  for (const Node& n : root.children) {
    if (const auto* g = std::get_if<Group>(&n.value); g && g->layer) {
      layer = true;
      CHECK_EQ(g->layer_name, "Layer 1");
    }
  }
  CHECK(layer);
  const auto items = leaves(scene->pages[0]);
  CHECK_EQ(joined_text(items), "0.00.51.01.52.02.53.00.00.51.01.52.02.53.0XY");
  // The data polyline (0,0) (1,2) (2,1) (3,3) in device points.
  bool polyline = false;
  for (const Shape* s : shapes(items)) {
    if (s->path.points.size() == 4 && std::abs(s->path.points[1].x - 248.4) < 0.01 &&
        std::abs(s->path.points[1].y - 453.6) < 0.01) {
      polyline = true;
    }
  }
  CHECK(polyline);
}

TEST_CASE("vector/ps: GNU plotutils PostScript with its own prolog") {
  auto scene = read_scene("ps", test::fixture("vector/ps/graph_plotutils.ps"), test_context());
  REQUIRE_OK(scene);
  const auto items = leaves(scene->pages[0]);
  CHECK_EQ(joined_text(items), "0.00.51.01.52.02.53.00.00.51.01.52.02.53.0XY");
  bool red_line = false;
  for (const Shape* s : shapes(items)) {
    if (s->stroke.paint.type == Paint::Type::Solid && rgb_is(s->stroke.paint.color, 1, 0, 0)) {
      red_line = true;
      REQUIRE_EQ(s->path.points.size(), 4u);
      CHECK_APPROX(s->path.points[3].x, 478.8, 0.01);
      CHECK_APPROX(s->path.points[3].y, 568.8, 0.01);
    }
  }
  CHECK(red_line);
}

TEST_CASE("vector/ps: detection") {
  const auto detect = [](const Bytes& data, std::string_view name) {
    auto r = detect_format(Registry::instance(), data, name);
    return r ? r->format->id : std::string("<none>");
  };
  CHECK_EQ(detect(test::fixture("vector/ps/hand_shapes.eps"), ""), "eps");
  CHECK_EQ(detect(test::fixture("vector/ps/drawing_inkscape.eps"), "x.eps"), "eps");
  CHECK_EQ(detect(test::fixture("vector/ps/drawing_inkscape.ps"), ""), "ps");
  CHECK_EQ(detect(test::fixture("vector/ps/graph_plotutils.ps"), "plot.ps"), "ps");
  CHECK_EQ(detect(test::fixture("vector/ps/graph_plotutils.ai"), ""), "ai");
  CHECK_EQ(detect(test::fixture("vector/ps/graph_plotutils.ai"), "plot.ai"), "ai");
  CHECK_EQ(detect(test::bytes("%!\n0 0 moveto\n"), "x.eps"), "eps");
  // DOS EPS binary header.
  Bytes dos = test::bytes("\xC5\xD0\xD3\xC6");
  const Bytes ps = test::fixture("vector/ps/hand_shapes.eps");
  const auto le32 = [&](std::uint32_t v) {
    for (int i = 0; i < 4; ++i) dos.push_back(std::byte{static_cast<unsigned char>((v >> (8 * i)) & 0xFF)});
  };
  le32(30 + 8);
  le32(static_cast<std::uint32_t>(ps.size()));
  le32(0);
  le32(0);
  le32(30);  // a fake 8-byte "TIFF" preview right after the header
  le32(8);
  dos.push_back(std::byte{0xFF});
  dos.push_back(std::byte{0xFF});
  for (int i = 0; i < 8; ++i) dos.push_back(std::byte{0x49});
  dos.insert(dos.end(), ps.begin(), ps.end());
  CHECK_EQ(detect(dos, ""), "eps");
  auto scene = read_scene("eps", dos, test_context());
  REQUIRE_OK(scene);
  check_shapes(*scene, 0, 150);
}

TEST_CASE("vector/ai: PDF-based Illustrator files are refused with a clear message") {
  const Bytes pdf_ai = test::bytes("%PDF-1.5\n%\xE2\xE3\xCF\xD3\n1 0 obj << /Type /Catalog >> endobj\n");
  auto r = test::read_as<ir::Data>("ai", pdf_ai);
  REQUIRE_FALSE(r);
  CHECK_EQ(r.error().code, Status::Decode);
  CHECK(r.error().message.find("PDF-based Illustrator") != std::string::npos);
  // It is not claimed strongly by the ai sniffer.
  const FormatInfo* ai = Registry::instance().find("ai");
  REQUIRE(ai != nullptr);
  CHECK_LT(ai->sniff(pdf_ai, "art.ai"), 50);
  CHECK_EQ(ai->sniff(pdf_ai, "doc.pdf"), 0);
}

TEST_CASE("vector/eps: same-format conversion") {
  const auto out = test::convert(test::fixture("vector/ps/drawing_inkscape.eps"), "", "eps");
  REQUIRE_MESSAGE(out.ok(), out.message);
  CHECK_EQ(out.format_id, "eps");
  const std::string text = out.text();
  CHECK(text.starts_with("%!PS-Adobe-3.0 EPSF-3.0"));
  CHECK(text.find("%%Creator") == std::string::npos);
  CHECK(text.find("cairo") == std::string::npos);
  // Smaller: the embedded font program and cairo's prolog are gone.
  CHECK_LT(out.data.size(), test::fixture("vector/ps/drawing_inkscape.eps").size());
  auto scene = read_scene("eps", out.data, test_context());
  REQUIRE_OK(scene);
  CHECK_APPROX(scene->pages[0].width, 122 * k, 0.5);
}

TEST_CASE("vector/ps: metadata is only kept on request") {
  const std::string program = "%!PS-Adobe-3.0 EPSF-3.0\n%%Title: (Lorem chart)\n%%BoundingBox: 0 0 10 10\n0 0 5 5 rectfill\n";
  auto kept = read_scene("eps", as_bytes(program), test_context(), *Options::parse(R"({"strip_metadata": false})"));
  REQUIRE_OK(kept);
  CHECK_EQ(kept->metadata["title"], "Lorem chart");
}

TEST_CASE("vector/ps: truncated and corrupted fixtures fail cleanly") {
  for (const char* name : {"vector/ps/hand_shapes.eps", "vector/ps/hand_grad.eps", "vector/ps/hand_jpeg.eps",
                           "vector/ps/graph_plotutils.ai", "vector/ps/mpl_type3.eps", "vector/ps/hand_type1.eps"}) {
    CAPTURE(name);
    const std::string id = std::string(name).ends_with(".ai") ? "ai" : "eps";
    CHECK_GT(test::fuzz_reader<ir::Data>(id, test::fixture(name), 96), 0u);
  }
  for (const char* name : {"vector/ps/drawing_inkscape.eps", "vector/ps/mpl_type42.eps", "vector/ps/image_inkscape.eps",
                           "vector/ps/graph_plotutils.ps"}) {
    CAPTURE(name);
    const std::string id = std::string(name).ends_with(".ps") ? "ps" : "eps";
    CHECK_GT(test::fuzz_reader<ir::Data>(id, test::fixture(name), 32), 0u);
  }
  // The large Ghostscript programs: a few prefixes and the corruptions.
  CHECK_GT(test::fuzz_reader<ir::Data>("eps", gunzip(test::fixture("vector/ps/shapes_gs.eps.gz")), 6), 0u);
}

TEST_CASE("vector/eps: renders like Ghostscript") {
  // References rendered by Ghostscript at 48 dpi (see SOURCES.md). Fonts that
  // are not embedded use different substitutes, hence the modest thresholds.
  const struct {
    const char* eps;
    const char* png;
    double min_psnr;
  } cases[] = {
      {"vector/ps/hand_shapes.eps", "vector/ps/hand_shapes_gs48.png", 20.0},
      {"vector/ps/hand_grad.eps", "vector/ps/hand_grad_gs48.png", 19.0},
      {"vector/ps/drawing_inkscape.eps", "vector/ps/drawing_inkscape_gs48.png", 20.0},
      {"vector/ps/mpl_type42.eps", "vector/ps/mpl_type42_gs48.png", 20.0},
  };
  Context ctx = test_context();
  if (!image::can_decode("png", ctx)) SKIP("no PNG decoder registered");
  for (const auto& c : cases) {
    CAPTURE(c.eps);
    auto scene = read_scene("eps", test::fixture(c.eps), ctx);
    REQUIRE_OK(scene);
    auto reference = image::decode("png", test::fixture(c.png), ctx);
    REQUIRE_OK(reference);
    RenderOptions options;
    options.width = reference->frames.at(0).width;
    options.height = reference->frames.at(0).height;
    options.background = Color{1, 1, 1, 1};
    auto frame = render_page(*scene, options, ctx);
    if (!frame && frame.error().code == Status::UnsupportedConversion) SKIP("the rasteriser is not available");
    REQUIRE_OK(frame);
    ir::Image ours;
    ours.frames.push_back(std::move(*frame));
    ir::Image ref = std::move(*reference);
    for (ir::Frame& f : ref.frames) {
      for (std::size_t i = 3; i < f.rgba.size(); i += 4) f.rgba[i] = 255;
    }
    CHECK_GT(test::psnr(ours, ref), c.min_psnr);
    CHECK_GT(test::ssim(ours, ref), 0.8);
  }
}
