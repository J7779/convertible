// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// EPS / PS writer: DSC structure, options, round trips through our reader.
#include <cmath>
#include <string>
#include <vector>

#include "engine/registry.hpp"
#include "families/vector/font.hpp"
#include "families/vector/geom.hpp"
#include "families/vector/ps_emit.hpp"
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

Path rect(double x, double y, double w, double h) {
  Path p;
  add_rect(p, x, y, w, h);
  return p;
}

Scene sample_scene() {
  Scene sc;
  Page pg;
  pg.width = 400;
  pg.height = 300;
  Shape a;
  a.path = rect(10, 20, 100, 50);
  a.fill = Paint::solid(Color::rgb8(200, 30, 40));
  pg.root.children.push_back(Node{a});
  Shape b;
  add_ellipse(b.path, 250, 80, 40, 30);
  b.fill_rule = FillRule::EvenOdd;
  b.fill = Paint::solid(Color::rgb8(0, 128, 255));
  b.stroke.paint = Paint::solid(Color::rgb8(10, 10, 10));
  b.stroke.width = 3;
  b.stroke.cap = LineCap::Round;
  b.stroke.join = LineJoin::Bevel;
  b.stroke.dash = {8, 4};
  pg.root.children.push_back(Node{b});
  // A transformed, clipped group.
  Group g;
  g.transform = Matrix::translate(50, 150) * Matrix::rotate(0.3);
  auto clip = std::make_shared<ClipPath>();
  clip->items.push_back(ClipPath::Item{rect(0, 0, 60, 60), FillRule::NonZero});
  g.clip = clip;
  Shape c;
  c.path = rect(-20, -20, 200, 200);
  c.fill = Paint::solid(Color::rgb8(0, 160, 0));
  g.children.push_back(Node{c});
  pg.root.children.push_back(Node{g});
  // A linear gradient.
  auto grad = std::make_shared<Gradient>();
  grad->p1 = {200, 0};
  grad->p2 = {380, 0};
  grad->stops = {GradientStop{0, {1, 0, 0, 1}}, GradientStop{0.5, {1, 1, 0, 1}}, GradientStop{1, {0, 0, 1, 1}}};
  Shape d;
  d.path = rect(200, 150, 180, 60);
  d.fill.type = Paint::Type::Gradient;
  d.fill.gradient = grad;
  pg.root.children.push_back(Node{d});
  // A 3x2 picture.
  ir::Frame fr = ir::Frame::filled(3, 2, {255, 255, 255, 255});
  const std::uint8_t colors[6][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {10, 20, 30}, {200, 100, 50}, {0, 0, 0}};
  for (std::size_t i = 0; i < 6; ++i) {
    for (std::size_t c3 = 0; c3 < 3; ++c3) fr.rgba[i * 4 + c3] = colors[i][c3];
  }
  Picture pic;
  pic.pixels = std::make_shared<const ir::Frame>(fr);
  pic.bounds = Rect::xywh(20, 200, 90, 60);
  pic.smooth = false;
  pg.root.children.push_back(Node{pic});
  sc.pages.push_back(pg);
  return sc;
}

struct Leaf {
  const Node* node = nullptr;
  Matrix to_page;
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

Result<Bytes> write(const Scene& s, std::string_view id = "eps", std::string_view options = {},
                    std::vector<std::string>* warnings = nullptr) {
  auto opts = Options::parse(options);
  if (!opts) return fail(opts.error());
  WriteContext ctx;
  ctx.registry = &Registry::instance();
  ctx.options = *opts;
  ctx.format_id = std::string(id);
  auto out = write_scene(id, s, ctx, *opts);
  if (warnings != nullptr) *warnings = ctx.warnings.list();
  return out;
}

bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

/// Colours compared at 8 bits per channel.
bool same8(const Color& a, const Color& b) {
  const auto q = [](float v) { return static_cast<int>(std::lround(v * 255.0f)); };
  return q(a.r) == q(b.r) && q(a.g) == q(b.g) && q(a.b) == q(b.b) && q(a.a) == q(b.a);
}

}  // namespace

TEST_CASE("vector/eps: writer emits DSC 3.0 structure without personal data") {
  auto out = write(sample_scene());
  REQUIRE_OK(out);
  const std::string text = test::str(*out);
  CHECK(text.starts_with("%!PS-Adobe-3.0 EPSF-3.0\n"));
  CHECK(text.find("%%BoundingBox: 0 0 300 225\n") != std::string::npos);
  CHECK(text.find("%%HiResBoundingBox: 0 0 300 225\n") != std::string::npos);
  CHECK(text.find("%%LanguageLevel: 2\n") != std::string::npos);
  CHECK(text.find("%%Pages: 1\n") != std::string::npos);
  for (const char* key : {"%%EndComments", "%%BeginProlog", "%%EndProlog", "%%Page: 1 1", "%%PageTrailer",
                          "%%Trailer", "%%EOF"}) {
    CHECK_MESSAGE(text.find(key) != std::string::npos, key);
  }
  for (const char* bad : {"%%Creator", "%%CreationDate", "%%For", "%%Title", "setpagedevice"}) {
    CHECK_MESSAGE(text.find(bad) == std::string::npos, bad);
  }
  // 7-bit clean.
  bool clean = true;
  for (const char c : text) clean = clean && static_cast<unsigned char>(c) < 128;
  CHECK(clean);
  // Deterministic.
  auto again = write(sample_scene());
  REQUIRE_OK(again);
  CHECK(*again == *out);
}

TEST_CASE("vector/eps: write -> read round trip keeps geometry and colours") {
  auto out = write(sample_scene());
  REQUIRE_OK(out);
  auto back = read_scene("eps", *out, test_context());
  REQUIRE_OK(back);
  REQUIRE_EQ(back->pages.size(), 1u);
  CHECK(near(back->pages[0].width, 400, 0.01));
  CHECK(near(back->pages[0].height, 300, 0.01));
  const auto items = leaves(back->pages[0]);
  const Shape* red = nullptr;
  const Shape* ellipse = nullptr;
  const Shape* outline = nullptr;
  Matrix red_m, ellipse_m, outline_m;
  const Shape* gradient = nullptr;
  const Picture* pic = nullptr;
  Matrix pic_m;
  bool clipped_green = false;
  for (const Leaf& l : items) {
    if (const auto* s = std::get_if<Shape>(&l.node->value)) {
      if (s->fill.type == Paint::Type::Solid && same8(s->fill.color, Color::rgb8(200, 30, 40))) {
        red = s;
        red_m = l.to_page;
      }
      if (s->fill_rule == FillRule::EvenOdd && s->fill.type == Paint::Type::Solid) {
        ellipse = s;
        ellipse_m = l.to_page;
      }
      if (!s->stroke.dash.empty()) {
        outline = s;
        outline_m = l.to_page;
      }
      if (s->fill.type == Paint::Type::Gradient) gradient = s;
      if (s->fill.type == Paint::Type::Solid && same8(s->fill.color, Color::rgb8(0, 160, 0))) clipped_green = true;
    }
    if (const auto* p = std::get_if<Picture>(&l.node->value)) {
      pic = p;
      pic_m = l.to_page;
    }
  }
  REQUIRE(red != nullptr);
  const Rect rb = bounds(red->path, red_m);
  CHECK(near(rb.x0, 10, 0.01));
  CHECK(near(rb.y0, 20, 0.01));
  CHECK(near(rb.x1, 110, 0.01));
  CHECK(near(rb.y1, 70, 0.01));
  REQUIRE(ellipse != nullptr);
  const Rect eb = bounds(ellipse->path, ellipse_m);
  CHECK(near(eb.x0, 210, 0.01));
  CHECK(near(eb.y1, 110, 0.01));
  // Stroke parameters survive (width in page px: 3).
  REQUIRE(outline != nullptr);
  CHECK(near(outline->stroke.width * outline_m.mean_scale(), 3, 1e-3));
  CHECK(outline->stroke.cap == LineCap::Round);
  CHECK(outline->stroke.join == LineJoin::Bevel);
  REQUIRE_EQ(outline->stroke.dash.size(), 2u);
  CHECK(near(outline->stroke.dash[0] * outline_m.mean_scale(), 8, 1e-3));
  CHECK(same8(outline->stroke.paint.color, Color::rgb8(10, 10, 10)));
  CHECK(clipped_green);
  REQUIRE(gradient != nullptr);
  CHECK_EQ(gradient->fill.gradient->stops.size(), 3u);
  CHECK(same8(gradient->fill.gradient->stops[1].color, Color{1, 1, 0, 1}));
  REQUIRE(pic != nullptr);
  REQUIRE(pic->pixels != nullptr);
  REQUIRE_EQ(pic->pixels->width, 3u);
  const ir::Frame& f = *pic->pixels;
  CHECK_EQ(f.rgba[f.offset(1, 0) + 1], 255);
  CHECK_EQ(f.rgba[f.offset(1, 1)], 200);
  CHECK_EQ(f.rgba[f.offset(1, 1) + 2], 50);
  const Rect pb = transform_rect(pic->bounds, pic_m);
  CHECK(near(pb.x0, 20, 0.01));
  CHECK(near(pb.y0, 200, 0.01));
  CHECK(near(pb.x1, 110, 0.01));
  CHECK(near(pb.y1, 260, 0.01));
}

TEST_CASE("vector/eps: writer options") {
  SUBCASE("level 3 uses Flate and plain shfill") {
    auto out = write(sample_scene(), "eps", R"({"level": "3"})");
    REQUIRE_OK(out);
    const std::string text = test::str(*out);
    CHECK(text.find("%%LanguageLevel: 3") != std::string::npos);
    CHECK(text.find("/FlateDecode") != std::string::npos);
    CHECK(text.find("/shfill where") == std::string::npos);
    CHECK(text.find("shfill") != std::string::npos);
    auto level2 = write(sample_scene());
    REQUIRE_OK(level2);
    const std::string t2 = test::str(*level2);
    CHECK(t2.find("/LZWDecode") != std::string::npos);
    CHECK(t2.find("/shfill where") != std::string::npos);
    CHECK_OK(read_scene("eps", *out, test_context()));
  }
  SUBCASE("precision") {
    auto coarse = write(sample_scene(), "eps", R"({"precision": 0})");
    auto fine = write(sample_scene(), "eps", R"({"precision": 5})");
    REQUIRE_OK(coarse);
    REQUIRE_OK(fine);
    CHECK_LT(coarse->size(), fine->size());
  }
  SUBCASE("text as outlines or as real text") {
    Scene sc;
    Page pg;
    pg.width = 200;
    pg.height = 100;
    Text t = make_text("Lorem \xC3\xA9t\xC3\xA9", FontSpec{"Times, serif", 20, 700, false}, {10, 50},
                       Paint::solid({0, 0, 0.5f, 1}));
    pg.root.children.push_back(Node{t});
    sc.pages.push_back(pg);
    auto outlines = write(sc);
    REQUIRE_OK(outlines);
    CHECK(test::str(*outlines).find(" show") == std::string::npos);
    auto real = write(sc, "eps", R"({"ps_text": "text"})");
    REQUIRE_OK(real);
    const std::string text = test::str(*real);
    CHECK(text.find("%%DocumentNeededResources: font Times-Bold") != std::string::npos);
    CHECK(text.find("selectfont") != std::string::npos);
    CHECK(text.find("(Lorem \\351t\\351) show") != std::string::npos);
    auto back = read_scene("eps", *real, test_context());
    REQUIRE_OK(back);
    std::string content;
    for (const Leaf& l : leaves(back->pages[0])) {
      if (const auto* tx = std::get_if<Text>(&l.node->value)) content += tx->content;
    }
    CHECK_EQ(content, "Lorem \xC3\xA9t\xC3\xA9");
  }
}

TEST_CASE("vector/ps: multi-page PostScript and single-page EPS") {
  Scene sc = sample_scene();
  Page second;
  second.width = 200;
  second.height = 500;
  Shape s;
  s.path = rect(10, 10, 20, 20);
  s.fill = Paint::solid({0, 0, 0, 1});
  second.root.children.push_back(Node{s});
  sc.pages.push_back(second);
  auto ps = write(sc, "ps");
  REQUIRE_OK(ps);
  const std::string text = test::str(*ps);
  CHECK(text.starts_with("%!PS-Adobe-3.0\n"));
  CHECK(text.find("%%Pages: 2") != std::string::npos);
  CHECK(text.find("%%Page: 2 2") != std::string::npos);
  CHECK(text.find("%%PageBoundingBox: 0 0 150 375") != std::string::npos);
  auto back = read_scene("ps", *ps, test_context());
  REQUIRE_OK(back);
  REQUIRE_EQ(back->pages.size(), 2u);
  CHECK(near(back->pages[1].width, 200, 0.01));
  CHECK(near(back->pages[1].height, 500, 0.01));
  std::vector<std::string> warnings;
  auto eps = write(sc, "eps", {}, &warnings);
  REQUIRE_OK(eps);
  CHECK(test::str(*eps).find("%%Pages: 1") != std::string::npos);
  CHECK_FALSE(warnings.empty());
}

TEST_CASE("vector/eps: JPEG pictures are copied through") {
  auto scene = read_scene("eps", test::fixture("vector/ps/hand_jpeg.eps"), test_context());
  REQUIRE_OK(scene);
  auto out = write(*scene);
  REQUIRE_OK(out);
  CHECK(test::str(*out).find("/DCTDecode cvt_image") != std::string::npos);
  auto back = read_scene("eps", *out, test_context());
  REQUIRE_OK(back);
  bool jpeg = false;
  for (const Leaf& l : leaves(back->pages[0])) {
    if (const auto* p = std::get_if<Picture>(&l.node->value)) jpeg = p->encoded_format == "jpg";
  }
  CHECK(jpeg);
}

TEST_CASE("vector/eps: transparency is flattened or reported") {
  Scene sc = sample_scene();
  Group g;
  g.opacity = 0.5;
  Shape s;
  s.path = rect(0, 0, 100, 100);
  s.fill = Paint::solid({0, 0, 1, 1});
  g.children.push_back(Node{s});
  sc.pages[0].root.children.push_back(Node{g});
  std::vector<std::string> warnings;
  auto out = write(sc, "eps", {}, &warnings);
  REQUIRE_OK(out);
  REQUIRE_FALSE(warnings.empty());
  CHECK_OK(read_scene("eps", *out, test_context()));
}

TEST_CASE("vector/eps: helpers - ASCII85, LZW and numbers") {
  CHECK_EQ(emit::ascii85(test::bytes("Hello world")), "87cURD]j7BEbo7~>");
  CHECK_EQ(emit::ascii85(test::bytes(std::string_view("\0\0\0\0", 4))), "z~>");
  CHECK_EQ(emit::num(1.23456, 3), "1.235");
  CHECK_EQ(emit::num(-0.0001, 3), "0");
  CHECK_EQ(emit::num(2.5, 0), "3");
  CHECK_EQ(emit::num(1e20, 2), "1000000000000000");
  CHECK_EQ(emit::ps_string("a(b)\\\n"), "(a\\(b\\)\\\\\\012)");
  // LZW: long repetitive data through our reader's LZWDecode filter.
  std::string data;
  for (int i = 0; i < 20000; ++i) data.push_back(static_cast<char>("abcabcabd"[i % 9] + (i / 997) % 3));
  const Bytes lzw = emit::lzw_encode(as_bytes(data));
  CHECK_LT(lzw.size(), data.size() / 3);
  std::string hex;
  for (const std::byte b : lzw) {
    static const char* d = "0123456789abcdef";
    hex.push_back(d[std::to_integer<int>(b) >> 4]);
    hex.push_back(d[std::to_integer<int>(b) & 15]);
  }
  const std::string program = "%!PS-Adobe-3.0 EPSF-3.0\n%%BoundingBox: 0 0 10 10\n"
                              "/d currentfile /ASCIIHexDecode filter /LZWDecode filter 30000 string readstring\n" +
                              hex + ">\npop length 0 0 moveto 0 rlineto 0 1 rlineto closepath fill\n";
  auto scene = read_scene("eps", as_bytes(program), test_context());
  REQUIRE_OK(scene);
  const auto& sh = std::get<Shape>(scene->pages[0].root.children.at(0).value);
  CHECK_EQ(sh.path.points.at(1).x, 20000.0);
}

TEST_CASE("vector/eps: interop - EPS from SVG-like scenes through test::convert") {
  // eps -> ps conversion keeps the drawing.
  const auto out = test::convert(test::fixture("vector/ps/hand_grad.eps"), "eps", "ps");
  REQUIRE_MESSAGE(out.ok(), out.message);
  auto back = read_scene("ps", out.data, test_context());
  REQUIRE_OK(back);
  int gradients = 0;
  for (const Leaf& l : leaves(back->pages[0])) {
    if (const auto* s = std::get_if<Shape>(&l.node->value); s && s->fill.type == Paint::Type::Gradient) ++gradients;
  }
  CHECK_EQ(gradients, 2);
}
