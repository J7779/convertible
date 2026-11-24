// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>
#include <vector>

#include "engine/registry.hpp"
#include "families/vector/font.hpp"
#include "families/vector/geom.hpp"
#include "families/vector/vector_codecs.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"

using namespace cvt;
using namespace cvt::vector;

namespace {

Context test_context() {
  Context ctx;
  ctx.registry = &Registry::instance();
  return ctx;
}

std::string write_tikz(const Scene& scene, std::string_view options = {}, std::vector<std::string>* warnings = nullptr) {
  Context ctx = test_context();
  auto parsed = Options::parse(options);
  if (!parsed) return {};
  auto out = write_scene("tikz", scene, ctx, *parsed);
  if (warnings != nullptr) *warnings = ctx.warnings.list();
  return out ? test::str(*out) : std::string("ERROR: ") + out.error().message;
}

bool contains(const std::string& text, std::string_view piece) { return text.find(piece) != std::string::npos; }

std::size_t count(const std::string& text, std::string_view piece) {
  std::size_t n = 0;
  for (std::size_t at = text.find(piece); at != std::string::npos; at = text.find(piece, at + 1)) ++n;
  return n;
}

Scene sample_scene() {
  Scene scene;
  Page page;
  page.width = 96;  // one inch = 72 pt
  page.height = 48;
  Shape rect;
  add_rect(rect.path, 0, 0, 48, 24);
  rect.fill = Paint::solid(Color{1, 0, 0, 1}, 0.5);
  page.root.children.push_back(Node{rect});
  Shape curve;
  curve.path.move_to({0, 48});
  curve.path.cubic_to({10, 30}, {20, 30}, {30, 48});
  curve.path.quad_to({40, 30}, {50, 48});
  curve.stroke.paint = Paint::solid(Color{0, 0, 1, 1});
  curve.stroke.width = 4;
  curve.stroke.cap = LineCap::Round;
  curve.stroke.join = LineJoin::Bevel;
  curve.stroke.dash = {8, 4};
  page.root.children.push_back(Node{curve});
  Group clipped;
  clipped.transform = Matrix::translate(48, 0) * Matrix::scale(2, 2);
  clipped.opacity = 0.5;
  auto clip = std::make_shared<ClipPath>();
  ClipPath::Item item;
  add_rect(item.path, 0, 0, 10, 10);
  item.rule = FillRule::EvenOdd;
  clip->items.push_back(item);
  clipped.clip = clip;
  Shape ring;
  add_ellipse(ring.path, 5, 5, 5, 5);
  add_ellipse(ring.path, 5, 5, 2, 2);
  ring.fill = Paint::solid(Color{0, 0.5f, 0, 1});
  ring.fill_rule = FillRule::EvenOdd;
  ring.stroke.paint = Paint::solid(Color{0, 0, 0, 1});
  ring.stroke.width = 1;
  clipped.children.push_back(Node{ring});
  page.root.children.push_back(Node{clipped});
  page.root.children.push_back(
      Node{make_text("50% & $5 {x}_#~^\\", FontSpec{"sans-serif", 12, 700, true}, {2, 40}, Paint::solid(Color{0, 0, 0, 1}))});
  Picture picture;
  picture.bounds = Rect::xywh(0, 0, 4, 4);
  page.root.children.push_back(Node{picture});
  scene.pages.push_back(std::move(page));
  return scene;
}

}  // namespace

TEST_CASE("vector/tikz: writes paths, styles, clips, transforms and text") {
  std::vector<std::string> warnings;
  const std::string tex = write_tikz(sample_scene(), {}, &warnings);
  INFO(tex);
  CHECK(contains(tex, "\\begin{tikzpicture}[x=1pt,y=1pt]"));
  CHECK(contains(tex, "\\end{tikzpicture}"));
  CHECK_FALSE(contains(tex, "\\documentclass"));
  CHECK(contains(tex, "\\path[use as bounding box] (0,0) rectangle (72,36);"));
  CHECK(contains(tex, "\\definecolor{"));
  CHECK(contains(tex, "{RGB}{255,0,0}"));
  // The rectangle: y flipped, px -> pt.
  CHECK(contains(tex, "\\fill[fill=") );
  CHECK(contains(tex, "fill opacity=0.5"));
  CHECK(contains(tex, "(0,36) -- (36,36) -- (36,18) -- (0,18) -- cycle"));
  // The curve: controls, the quadratic raised to a cubic, stroke styles.
  CHECK(contains(tex, ".. controls (7.5,13.5) and (15,13.5) .. (22.5,0)"));
  CHECK(contains(tex, "line width=3pt"));
  CHECK(contains(tex, "line cap=round"));
  CHECK(contains(tex, "line join=bevel"));
  CHECK(contains(tex, "dash pattern=on 6pt off 3pt"));
  // The group: transparency group, cm transform conjugated by the flip, clip with even-odd.
  CHECK(contains(tex, "transparency group,opacity=0.5"));
  CHECK(contains(tex, "cm={2,0,0,2,(36,-36)}"));
  CHECK(contains(tex, "\\clip[even odd rule]"));
  CHECK(contains(tex, "\\filldraw["));
  CHECK(contains(tex, "even odd rule"));
  // Line width inside the scaled group grows with it (TikZ does not scale lines).
  CHECK(contains(tex, "line width=1.5pt"));
  // Text as a node with escaped content and font switches.
  CHECK(contains(tex, "\\node[anchor=base west"));
  CHECK(contains(tex, "\\bfseries\\itshape"));
  CHECK(contains(tex, "{50\\% \\& \\$5 \\{x\\}\\_\\#\\textasciitilde{}\\textasciicircum{}\\textbackslash{}}"));
  CHECK(warnings.size() == 1);  // the picture
}

TEST_CASE("vector/tikz: options change the output") {
  const Scene scene = sample_scene();
  const std::string standalone = write_tikz(scene, R"({"standalone":true})");
  CHECK(contains(standalone, "\\documentclass[tikz]{standalone}"));
  CHECK(contains(standalone, "\\begin{document}"));
  CHECK(contains(standalone, "\\end{document}"));
  const std::string cm = write_tikz(scene, R"({"units":"cm"})");
  CHECK(contains(cm, "[x=1cm,y=1cm]"));
  CHECK(contains(cm, "rectangle (2.54,1.27)"));
  const std::string outlines = write_tikz(scene, R"({"text":"outlines"})");
  CHECK_FALSE(contains(outlines, "\\node"));
  CHECK(count(outlines, "\\fill") > count(write_tikz(scene), "\\fill"));
  const std::string coarse = write_tikz(scene, R"({"precision":0})");
  const std::string fine = write_tikz(scene, R"({"precision":6})");
  CHECK(coarse.size() < fine.size());
}

TEST_CASE("vector/tikz: converts real drawings end to end") {
  const auto from_vd = test::convert(test::fixture("vector/misc/shapes_vd.xml"), "", "tikz", "", "shapes.xml");
  INFO(from_vd.message);
  REQUIRE(from_vd.ok());
  CHECK(from_vd.extension == "tikz");
  const std::string tex = from_vd.text();
  CHECK(count(tex, "\\fill[") == 4);
  CHECK(contains(tex, "cm={0.707107,-0.707107,0.707107,0.707107,"));  // the 45 degree group, clockwise
  const auto from_lottie = test::convert(test::fixture("vector/misc/lottie_gradient_precomp.json"), "", "tikz");
  REQUIRE(from_lottie.ok());
  CHECK(contains(from_lottie.text(), "\\clip"));  // the precomp bounds
  CHECK(contains(from_lottie.text(), "dash pattern="));
  CHECK_FALSE(from_lottie.warnings.empty());  // the gradient became a solid colour
  // TikZ is write-only: no reader.
  CHECK(test::find_reader("tikz", Kind::Data) == nullptr);
  Scene empty;
  Context ctx = test_context();
  CHECK_STATUS(write_scene("tikz", empty, ctx), Status::Encode);
}
