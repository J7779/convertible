// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// Scene page -> PDF content stream and resources.
#include <string>

#include "engine/registry.hpp"
#include "families/vector/geom.hpp"
#include "families/vector/pdf_content.hpp"
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

Path rect(double x, double y, double w, double h) {
  Path p;
  add_rect(p, x, y, w, h);
  return p;
}

Scene scene_with(std::vector<Node> nodes, double w = 200, double h = 100) {
  Scene sc;
  Page pg;
  pg.width = w;
  pg.height = h;
  pg.root.children = std::move(nodes);
  sc.pages.push_back(std::move(pg));
  return sc;
}

std::string content_of(const pdf::PdfPageContent& p) { return test::str(p.content); }

}  // namespace

TEST_CASE("vector/pdf content: paths, fills and strokes") {
  Shape s;
  s.path = rect(10, 20, 30, 40);
  s.fill = Paint::solid(Color::rgb8(255, 0, 0));
  s.fill_rule = FillRule::EvenOdd;
  s.stroke.paint = Paint::solid({0, 0, 1, 1});
  s.stroke.width = 2;
  s.stroke.dash = {4, 2};
  s.stroke.cap = LineCap::Round;
  Shape q;
  q.path.move_to({0, 0});
  q.path.quad_to({30, 30}, {60, 0});
  q.fill = Paint::solid({0.5f, 0.5f, 0.5f, 1});
  auto page = pdf::page_content(scene_with({Node{s}, Node{q}}), 0, test_context());
  REQUIRE_OK(page);
  CHECK_APPROX(page->width_pt, 150.0, 1e-9);
  CHECK_APPROX(page->height_pt, 75.0, 1e-9);
  const std::string c = content_of(*page);
  CHECK(c.starts_with("0.75 0 0 -0.75 0 75 cm\n"));
  CHECK(c.find("10 20 m\n40 20 l\n40 60 l\n10 60 l\nh\n") != std::string::npos);
  CHECK(c.find("1 0 0 rg") != std::string::npos);
  CHECK(c.find("f*") != std::string::npos);
  CHECK(c.find("2 w 1 J") != std::string::npos);
  CHECK(c.find("[4 2] 0 d") != std::string::npos);
  CHECK(c.find("0 0 1 RG") != std::string::npos);
  CHECK(c.find("\nS\n") != std::string::npos);
  CHECK(c.find("0.5 g") != std::string::npos);
  // The quadratic becomes a cubic with control points at 2/3.
  CHECK(c.find("20 20 40 20 60 0 c") != std::string::npos);
  CHECK(page->objects.empty());
  CHECK(page->resources.empty());
  CHECK_STATUS(pdf::page_content(scene_with({}), 3, test_context()), Status::Encode);
}

TEST_CASE("vector/pdf content: transparency, gradients, masks and pictures") {
  std::vector<Node> nodes;
  Shape translucent;
  translucent.path = rect(0, 0, 10, 10);
  translucent.fill = Paint::solid({0, 0, 1, 1}, 0.5);
  nodes.push_back(Node{translucent});
  auto g = std::make_shared<Gradient>();
  g->p1 = {0, 0};
  g->p2 = {100, 0};
  g->stops = {GradientStop{0, {1, 0, 0, 1}}, GradientStop{0.3, {0, 1, 0, 1}}, GradientStop{1, {0, 0, 1, 0.25f}}};
  Shape grad;
  grad.path = rect(0, 0, 100, 50);
  grad.fill.type = Paint::Type::Gradient;
  grad.fill.gradient = g;
  nodes.push_back(Node{grad});
  Group group;
  group.opacity = 0.4;
  group.blend = BlendMode::Multiply;
  auto mask = std::make_shared<Mask>();
  auto mc = std::make_shared<Group>();
  Shape ms;
  add_ellipse(ms.path, 50, 50, 20, 20);
  ms.fill = Paint::solid({1, 1, 1, 1});
  mc->children.push_back(Node{ms});
  mask->content = mc;
  group.mask = mask;
  Shape inner;
  inner.path = rect(20, 20, 60, 60);
  inner.fill = Paint::solid({1, 0.5f, 0, 1});
  group.children.push_back(Node{inner});
  nodes.push_back(Node{group});
  ir::Frame fr = ir::Frame::filled(2, 2, {10, 20, 30, 255});
  fr.rgba[3] = 0;
  Picture pic;
  pic.pixels = std::make_shared<const ir::Frame>(fr);
  pic.bounds = Rect::xywh(100, 10, 40, 20);
  nodes.push_back(Node{pic});

  auto page = pdf::page_content(scene_with(std::move(nodes)), 0, test_context());
  REQUIRE_OK(page);
  const std::string res = page->resources_dict();
  CHECK(res.find("/ExtGState <<") != std::string::npos);
  CHECK(res.find("/ca 0.5") != std::string::npos);
  CHECK(res.find("/BM /Multiply") != std::string::npos);
  CHECK(res.find("/SMask << /Type /Mask /S /Luminosity /G {obj") != std::string::npos);
  CHECK(res.find("/ShadingType 2") != std::string::npos);
  CHECK(res.find("/FunctionType 3") != std::string::npos);
  CHECK(res.find("/Bounds [0.3 ]") != std::string::npos);
  CHECK(res.find("/ColorSpace /DeviceGray") != std::string::npos);  // the stop-alpha soft mask
  const std::string c = content_of(*page);
  CHECK(c.find(" sh\n") != std::string::npos);
  CHECK(c.find(" Do") != std::string::npos);
  CHECK(c.find("40 0 0 -20 100 30 cm") != std::string::npos);  // picture placement, first row on top
  // Objects: soft-mask forms, the transparency group, the image and its SMask.
  int forms = 0, images = 0, smasks = 0;
  for (const pdf::PdfObject& o : page->objects) {
    if (o.dict.find("/Subtype /Form") != std::string::npos) {
      ++forms;
      CHECK(o.dict.find("/Resources {resources}") != std::string::npos);
      CHECK(o.stream.has_value());
    }
    if (o.dict.find("/Subtype /Image") != std::string::npos) {
      ++images;
      if (o.dict.find("/SMask") != std::string::npos) ++smasks;
      REQUIRE(o.stream.has_value());
      auto raw = util::zlib_decompress(*o.stream);
      REQUIRE_OK(raw);
      CHECK((raw->size() == 4 || raw->size() == 12));
    }
  }
  CHECK_EQ(forms, 3);  // gradient alpha mask, group, group mask
  CHECK_EQ(images, 2);
  CHECK_EQ(smasks, 1);
  // Placeholders resolve to object references.
  const auto ref = [](std::size_t i) { return std::to_string(10 + i) + " 0 R"; };
  const std::string resolved = pdf::resolve(res, ref, "");
  CHECK(resolved.find("{obj") == std::string::npos);
  CHECK(resolved.find(" 0 R") != std::string::npos);
  CHECK_EQ(pdf::resolve("<< /G {obj 2} /Resources {resources} /X {obj x} >>", ref, "<< >>"),
           "<< /G 12 0 R /Resources << >> /X {obj x} >>");
}

TEST_CASE("vector/pdf content: patterns, clips and JPEG pictures") {
  auto tile = std::make_shared<Group>();
  Shape dot;
  dot.path = rect(0, 0, 5, 5);
  dot.fill = Paint::solid({0, 0, 0, 1});
  tile->children.push_back(Node{dot});
  auto pp = std::make_shared<PatternPaint>();
  pp->tile = Rect{0, 0, 10, 10};
  pp->content = tile;
  Shape patterned;
  patterned.path = rect(0, 0, 50, 50);
  patterned.fill.type = Paint::Type::Pattern;
  patterned.fill.pattern = pp;
  Group clipped;
  auto cp = std::make_shared<ClipPath>();
  cp->items.push_back(ClipPath::Item{rect(0, 0, 25, 25), FillRule::EvenOdd});
  clipped.clip = cp;
  clipped.transform = Matrix::translate(5, 5);
  clipped.children.push_back(Node{patterned});
  Picture jpeg;
  jpeg.pixels = std::make_shared<const ir::Frame>(ir::Frame::filled(1, 1, {1, 2, 3, 255}));
  jpeg.bounds = Rect::xywh(0, 0, 10, 10);
  jpeg.encoded = test::bytes(std::string_view("\xFF\xD8\xFF\xC0\x00\x0B\x08\x00\x01\x00\x01\x03\x01\x11\x00\xFF\xD9", 17));
  jpeg.encoded_format = "jpg";
  auto page = pdf::page_content(scene_with({Node{clipped}, Node{jpeg}}), 0, test_context());
  REQUIRE_OK(page);
  const std::string c = content_of(*page);
  CHECK(c.find("1 0 0 1 5 5 cm") != std::string::npos);
  CHECK(c.find("W* n") != std::string::npos);
  CHECK(c.find("/Pattern cs /P0 scn") != std::string::npos);
  bool pattern = false, dct = false;
  for (const pdf::PdfObject& o : page->objects) {
    if (o.dict.find("/PatternType 1") != std::string::npos) {
      pattern = true;
      // Pattern space -> default user space: the page flip and the group translation.
      CHECK(o.dict.find("/Matrix [0.75 0 0 -0.75 3.75 71.25]") != std::string::npos);
    }
    if (o.dict.find("/DCTDecode") != std::string::npos) {
      dct = true;
      CHECK(o.dict.find("/ColorSpace /DeviceRGB") != std::string::npos);
      CHECK(*o.stream == jpeg.encoded);
    }
  }
  CHECK(pattern);
  CHECK(dct);
}
