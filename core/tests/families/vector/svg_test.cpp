// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// SVG reading, rendering against rsvg-convert references, writing and robustness.
#include <string>
#include <vector>

#include "families/image/image_api.hpp"
#include "families/image/image_ops.hpp"
#include "families/vector/geom.hpp"
#include "families/vector/render.hpp"
#include "families/vector/scene_json.hpp"
#include "families/vector/vector_codecs.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

using namespace cvt;
using namespace cvt::vector;

namespace {

Context make_ctx() {
  Context ctx;
  ctx.registry = &Registry::instance();
  return ctx;
}

Scene read(std::string_view fixture, std::string_view format = "svg") {
  auto scene = read_scene(format, test::fixture(std::string("vector/svg/") + std::string(fixture)), make_ctx());
  if (!scene) return {};
  return std::move(*scene);
}

// Every shape / text / picture in drawing order, with its accumulated transform.
struct Flat {
  const Node* node;
  Matrix m;
};
void flatten_nodes(const Group& g, const Matrix& m, std::vector<Flat>& out) {
  const Matrix gm = m * g.transform;
  for (const Node& n : g.children) {
    if (const auto* child = std::get_if<Group>(&n.value)) {
      flatten_nodes(*child, gm, out);
    } else {
      out.push_back({&n, gm});
    }
  }
}
std::vector<Flat> leaves(const Scene& s) {
  std::vector<Flat> out;
  if (!s.pages.empty()) flatten_nodes(s.pages[0].root, {}, out);
  return out;
}
const Shape* shape_at(const std::vector<Flat>& f, std::size_t i) {
  return i < f.size() ? std::get_if<Shape>(&f[i].node->value) : nullptr;
}

ir::Image flatten_white(ir::Image image) {
  image::flatten_alpha(image, {255, 255, 255});
  return image;
}

// Render a fixture with our rasteriser at the size of an rsvg reference and compare.
struct Compared {
  double psnr = 0;
  double ssim = 0;
};
Compared compare_with_reference(std::string_view svg, std::string_view png, std::string_view format = "svg") {
  const Context ctx = make_ctx();
  auto reference = image::decode("png", test::fixture(std::string("vector/svg/") + std::string(png)), ctx);
  if (!reference) return {};
  const std::string options = std::format(R"({{"width":{},"height":{},"background":"ffffff"}})",
                                          reference->width(), reference->height());
  auto ours = test::read_as<ir::Image>(format, test::fixture(std::string("vector/svg/") + std::string(svg)), options);
  if (!ours) return {};
  const ir::Image ref = flatten_white(std::move(*reference));
  return {test::psnr(*ours, ref), test::ssim(*ours, ref)};
}

}  // namespace

TEST_CASE("vector/svg: basic shapes decode with exact geometry and paint") {
  const Scene s = read("shapes.svg");
  REQUIRE(s.pages.size() == 1);
  CHECK(s.pages[0].width == 200);
  CHECK(s.pages[0].height == 150);
  const auto f = leaves(s);
  REQUIRE(f.size() == 9);
  const Shape* rect = shape_at(f, 0);
  REQUIRE(rect != nullptr);
  const Rect b = bounds(rect->path, f[0].m);
  CHECK_APPROX(b.x0, 10.0, 1e-9);
  CHECK_APPROX(b.y1, 50.0, 1e-9);
  CHECK(rect->fill.type == Paint::Type::Solid);
  CHECK_APPROX(rect->fill.color.r, 224 / 255.0, 1e-6);
  const Shape* rounded = shape_at(f, 1);
  REQUIRE(rounded != nullptr);
  CHECK(rounded->fill.type == Paint::Type::None);
  CHECK(rounded->stroke.width == 4.0);
  const Shape* circle = shape_at(f, 2);
  REQUIRE(circle != nullptr);
  CHECK_APPROX(circle->fill.opacity, 0.5, 1e-9);
  const Rect cb = bounds(circle->path);
  CHECK_APPROX(cb.x0, 150.0, 1e-6);
  CHECK_APPROX(cb.y1, 50.0, 1e-6);
  const Shape* line = shape_at(f, 4);
  REQUIRE(line != nullptr);
  CHECK(line->stroke.cap == LineCap::Round);
  const Shape* dashed = shape_at(f, 7);
  REQUIRE(dashed != nullptr);
  CHECK(dashed->stroke.dash == std::vector<double>{6, 3});
  const Shape* rotated = shape_at(f, 8);
  REQUIRE(rotated != nullptr);
  CHECK(rotated->fill_rule == FillRule::EvenOdd);
  // translate(150 110) rotate(30) scale(0.8) applied to (-15, -10).
  const Point p = f[8].m.apply({-15, -10});
  CHECK_APPROX(p.x, 150 + 0.8 * (-15 * std::cos(std::numbers::pi / 6) + 10 * std::sin(std::numbers::pi / 6)), 1e-9);
}

TEST_CASE("vector/svg: renders like rsvg-convert") {
  const struct {
    const char* svg;
    const char* png;
    double psnr;
    double ssim;
  } cases[] = {
      {"shapes.svg", "shapes_rsvg.png", 40, 0.97},
      {"shapes_inkscape_plain.svg", "shapes_rsvg.png", 40, 0.97},
      {"paint.svg", "paint_rsvg.png", 30, 0.95},
      {"clipmask.svg", "clipmask_rsvg.png", 38, 0.97},
      {"css.svg", "css_rsvg.png", 45, 0.99},
      {"filters.svg", "filters_rsvg.png", 40, 0.98},
      {"layers_inkscape.svg", "layers_inkscape_rsvg.png", 40, 0.98},
      // Text uses our bundled font instead of DejaVu, so only the layout matches.
      {"plot_matplotlib.svg", "plot_matplotlib_rsvg.png", 20, 0.80},
      // Inkscape converted the text to DejaVu outlines: pure path rendering.
      {"text_inkscape_paths.svg", "text_rsvg.png", 21, 0.85},
  };
  for (const auto& c : cases) {
    CAPTURE(c.svg);
    const Compared r = compare_with_reference(c.svg, c.png);
    CHECK_GT(r.psnr, c.psnr);
    CHECK_GT(r.ssim, c.ssim);
  }
  // svgz written by gzip decodes like the plain file.
  const Compared z = compare_with_reference("paint.svgz", "paint_rsvg.png", "svgz");
  CHECK_GT(z.psnr, 30.0);
}

TEST_CASE("vector/svg: CSS cascade, presentation attributes and currentColor") {
  const Scene scene = read("css.svg");
  const auto f = leaves(scene);
  REQUIRE(f.size() == 6);
  const auto color_of = [&](std::size_t i) { return shape_at(f, i)->fill.color; };
  CHECK(color_of(0) == Color::rgb8(0x88, 0x88, 0x88));  // type selector
  CHECK(color_of(1) == Color::rgb8(0xee, 0x33, 0x33));  // class beats type
  CHECK(color_of(2) == Color::rgb8(0x33, 0x33, 0xee));  // !important beats style attribute
  CHECK(shape_at(f, 1)->stroke.width == 2.0);
  CHECK(color_of(3) == Color::rgb8(0x00, 0xaa, 0x00));  // style attribute beats selectors
  CHECK(color_of(4) == Color::rgb8(0x22, 0xaa, 0x77));  // currentColor from the root's color
  CHECK_APPROX(shape_at(f, 5)->fill.opacity, 0.3, 1e-9);  // only the first child matches :first-child
}

TEST_CASE("vector/svg: text runs keep their strings, fonts and anchors") {
  const Scene scene = read("text.svg");
  const auto f = leaves(scene);
  std::vector<const Text*> texts;
  for (const Flat& x : f) {
    if (const auto* t = std::get_if<Text>(&x.node->value)) texts.push_back(t);
  }
  REQUIRE(texts.size() >= 6);
  CHECK(texts[0]->content == "Hello SVG");
  CHECK(texts[0]->font.size == 20);
  CHECK(texts[0]->origin == Point{10, 30});
  CHECK(texts[1]->content == "Mono ");
  CHECK(texts[2]->content == "bold");
  CHECK(texts[2]->font.weight == 700);
  CHECK(texts[2]->fill.color == Color::rgb8(0xcc, 0, 0));
  // text-anchor="middle": the whole line is centred on x = 120.
  const Rect line = [&] {
    Rect r;
    for (std::size_t i = 1; i <= 3; ++i) r.add(bounds(texts[i]->outline));
    return r;
  }();
  CHECK_APPROX((line.x0 + line.x1) / 2, 120.0, 3.0);
  CHECK(texts[4]->content == "Lorem ipsum");
  CHECK(texts[4]->font.italic);
  CHECK_APPROX(bounds(texts[4]->outline).x1, 230.0, 2.0);
  CHECK(texts[5]->content == "spaced out");  // white space collapsed
}

TEST_CASE("vector/svg: Inkscape layers, hidden layers and metadata") {
  const Scene s = read("layers_inkscape.svg");
  REQUIRE(s.pages.size() == 1);
  CHECK_APPROX(s.pages[0].width, 100 * px_per_mm, 1e-6);
  std::vector<const Group*> layers;
  for (const Node& n : s.pages[0].root.children) {
    if (const auto* g = std::get_if<Group>(&n.value); g && g->layer) layers.push_back(g);
  }
  REQUIRE(layers.size() == 3);
  CHECK(layers[0]->layer_name == "Background");
  CHECK(layers[1]->layer_name == "Shapes");
  CHECK_FALSE(layers[2]->visible);
  CHECK(s.metadata.at("title") == "Layer test");

  // Writing strips editor data and metadata by default and keeps the layers.
  const auto out = test::convert(test::fixture("vector/svg/layers_inkscape.svg"), "svg", "svg");
  REQUIRE(out.ok());
  const std::string text = out.text();
  CHECK(text.find("sodipodi") == std::string::npos);
  CHECK(text.find("namedview") == std::string::npos);
  CHECK(text.find("Layer test") == std::string::npos);
  CHECK(text.find("id=\"rect") == std::string::npos);
  CHECK(text.find("inkscape:label=\"Shapes\"") != std::string::npos);
  CHECK(text.find("display=\"none\"") != std::string::npos);
  const auto kept = test::convert(test::fixture("vector/svg/layers_inkscape.svg"), "svg", "svg", R"({"strip_metadata":false})");
  REQUIRE(kept.ok());
  CHECK(kept.text().find("<title>Layer test</title>") != std::string::npos);
}

TEST_CASE("vector/svg: write -> read keeps the picture; minify and precision shrink it") {
  for (const char* name : {"shapes.svg", "paint.svg", "clipmask.svg", "filters.svg", "css.svg"}) {
    CAPTURE(name);
    const Bytes source = test::fixture(std::string("vector/svg/") + name);
    const auto normal = test::convert(source, "svg", "svg");
    const auto minified = test::convert(source, "svg", "svg", R"({"minify":true,"precision":2})");
    REQUIRE(normal.ok());
    REQUIRE(minified.ok());
    CHECK(minified.data.size() < normal.data.size());
    const auto a = test::read_as<ir::Image>("svg", source, R"({"background":"ffffff"})");
    const auto b = test::read_as<ir::Image>("svg", normal.data, R"({"background":"ffffff"})");
    const auto c = test::read_as<ir::Image>("svg", minified.data, R"({"background":"ffffff"})");
    REQUIRE_OK(a);
    REQUIRE_OK(b);
    REQUIRE_OK(c);
    CHECK_GT(test::psnr(*a, *b), 45.0);
    CHECK_GT(test::psnr(*a, *c), 38.0);
    // Deterministic output.
    CHECK(test::convert(source, "svg", "svg").data == normal.data);
  }
  // Lower precision -> fewer bytes.
  const Bytes source = test::fixture("vector/svg/shapes.svg");
  const auto p1 = test::convert(source, "svg", "svg", R"({"precision":1})");
  const auto p6 = test::convert(source, "svg", "svg", R"({"precision":6})");
  REQUIRE(p1.ok());
  REQUIRE(p6.ok());
  CHECK(p1.data.size() < p6.data.size());
}

TEST_CASE("vector/svg: text written as text or outlines") {
  const Bytes source = test::fixture("vector/svg/text.svg");
  const auto as_text = test::convert(source, "svg", "svg");
  const auto as_paths = test::convert(source, "svg", "svg", R"({"svg_text":"outlines"})");
  REQUIRE(as_text.ok());
  REQUIRE(as_paths.ok());
  CHECK(as_text.text().find(">Hello SVG</text>") != std::string::npos);
  CHECK(as_paths.text().find("<text") == std::string::npos);
  const auto a = test::read_as<ir::Image>("svg", as_text.data, R"({"background":"ffffff"})");
  const auto b = test::read_as<ir::Image>("svg", as_paths.data, R"({"background":"ffffff"})");
  REQUIRE_OK(a);
  REQUIRE_OK(b);
  CHECK_GT(test::psnr(*a, *b), 30.0);
}

TEST_CASE("vector/svg: scene envelope is lossless") {
  for (const char* name : {"shapes.svg", "paint.svg", "clipmask.svg", "filters.svg", "text.svg"}) {
    CAPTURE(name);
    const Scene s = read(name);
    const Json j = scene_to_json(s);
    const auto back = scene_from_json(j);
    REQUIRE_OK(back);
    CHECK(scene_to_json(*back) == j);
  }
  CHECK_STATUS(scene_from_json(Json::object({{"a", 1}})), Status::Encode);
}

TEST_CASE("vector/svg: detection") {
  CHECK(test::convert(test::fixture("vector/svg/shapes.svg"), "", "svg").ok());
  CHECK(test::convert(test::fixture("vector/svg/plot_matplotlib.svg"), "", "png").format_id == "png");
  const auto z = test::convert(test::fixture("vector/svg/paint.svgz"), "", "svg", "", "drawing.svgz");
  CHECK(z.ok());
  // An XML file that is not SVG is not taken for one, even when named .svg.
  const auto notsvg = test::convert(as_bytes("<?xml version=\"1.0\"?><html><body/></html>"), "svg", "png");
  CHECK_FALSE(notsvg.ok());
  const auto bom = test::convert(as_bytes("\xEF\xBB\xBF<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"4\" height=\"4\"/>"),
                                 "", "png");
  CHECK(bom.ok());
}

TEST_CASE("vector/svg: rasterisation options") {
  const Bytes source = test::fixture("vector/svg/shapes.svg");
  const auto big = test::read_as<ir::Image>("svg", source, R"({"dpi":192})");
  REQUIRE_OK(big);
  CHECK(big->width() == 400);
  const auto sized = test::read_as<ir::Image>("svg", source, R"({"width":100})");
  REQUIRE_OK(sized);
  CHECK(sized->width() == 100);
  CHECK(sized->height() == 75);
  const auto bg = test::read_as<ir::Image>("svg", source, R"({"background":"00ff00"})");
  REQUIRE_OK(bg);
  CHECK(bg->frames[0].rgba[bg->frames[0].offset(199, 0) + 1] == 255);
  CHECK_STATUS(test::read_as<ir::Image>("svg", source, R"({"background":"chartreuse-ish"})"), Status::InvalidArgument);
  CHECK_STATUS(test::read_as<ir::Image>("svg", source, R"({"page":3})"), Status::InvalidArgument);
}

TEST_CASE("vector/svg: hostile documents fail cleanly") {
  // A reference loop and an exponential <use> bomb.
  const std::string loop =
      R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="10" height="10">)"
      R"(<g id="a"><use xlink:href="#a"/></g></svg>)";
  CHECK_OK(test::read_as<ir::Image>("svg", as_bytes(loop)));
  std::string bomb = R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="10" height="10"><defs><rect id="l0" width="1" height="1"/>)";
  for (int i = 1; i <= 30; ++i) {
    bomb += std::format(R"(<g id="l{}"><use xlink:href="#l{}"/><use xlink:href="#l{}"/></g>)", i, i - 1, i - 1);
  }
  bomb += R"(</defs><use xlink:href="#l30"/></svg>)";
  CHECK_STATUS(test::read_as<ir::Data>("svg", as_bytes(bomb)), Status::Limit);
  // Absurd size: rasterising fails with a limit, never allocates.
  const std::string huge = R"(<svg xmlns="http://www.w3.org/2000/svg" width="900000" height="900000"/>)";
  CHECK_STATUS(test::read_as<ir::Image>("svg", as_bytes(huge)), Status::Limit);
  // Malformed path data draws up to the error.
  const std::string partial =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10"><path d="M0 0 L10 0 L10 10 Z L x"/></svg>)";
  std::vector<std::string> warnings;
  CHECK_OK(test::read_as<ir::Image>("svg", as_bytes(partial), {}, &warnings));
  CHECK_FALSE(warnings.empty());
  CHECK_STATUS(test::read_as<ir::Data>("svg", as_bytes("<html/>")), Status::Decode);
  CHECK(test::fuzz_reader<ir::Data>("svg", test::fixture("vector/svg/clipmask.svg"), 128) > 0);
  CHECK(test::fuzz_reader<ir::Data>("svg", test::fixture("vector/svg/css.svg"), 128) > 0);
  CHECK(test::fuzz_reader<ir::Data>("svgz", test::fixture("vector/svg/paint.svgz"), 128) > 0);
}

TEST_CASE("vector/svg: documents and pictures") {
  const auto doc = test::read_as<ir::Document>("svg", test::fixture("vector/svg/text.svg"));
  REQUIRE_OK(doc);
  REQUIRE(doc->blocks.size() >= 2);
  CHECK(std::holds_alternative<ir::ImageBlock>(doc->blocks[0].node));
  bool found = false;
  for (const ir::Block& b : doc->blocks) {
    if (const auto* p = std::get_if<ir::Paragraph>(&b.node)) {
      if (const auto* t = std::get_if<ir::Text>(&p->content.at(0).node); t && t->text == "Hello SVG") found = true;
    }
  }
  CHECK(found);
  // A raster picture embedded unchanged renders back identically.
  ir::Image picture;
  picture.frames.push_back(test::gradient_frame(32, 24));
  const auto svg = test::write_from("svg", picture);
  REQUIRE_OK(svg);
  const auto back = test::read_as<ir::Image>("svg", *svg);
  REQUIRE_OK(back);
  CHECK_GT(test::psnr(*back, picture), 45.0);
}
