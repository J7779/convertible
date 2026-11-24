// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

#include "engine/detect.hpp"
#include "engine/registry.hpp"
#include "families/vector/font.hpp"
#include "families/vector/geom.hpp"
#include "families/vector/misc_common.hpp"
#include "families/vector/vector_codecs.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"

using namespace cvt;
using namespace cvt::vector;

namespace {

struct Placed {
  Shape shape;
  Matrix total;
  double opacity = 1.0;
};

void collect(const Group& group, const Matrix& parent, double opacity, std::vector<Placed>& out,
             std::vector<const Group*>* groups = nullptr) {
  const Matrix total = parent * group.transform;
  opacity *= group.opacity;
  if (groups != nullptr) groups->push_back(&group);
  for (const Node& node : group.children) {
    if (const auto* s = std::get_if<Shape>(&node.value)) out.push_back({*s, total, opacity});
    if (const auto* g = std::get_if<Group>(&node.value)) collect(*g, total, opacity, out, groups);
  }
}

std::vector<Placed> shapes_of(const Scene& scene, std::vector<const Group*>* groups = nullptr) {
  std::vector<Placed> out;
  collect(scene.pages.at(0).root, Matrix{}, 1.0, out, groups);
  return out;
}

Context test_context() {
  Context ctx;
  ctx.registry = &Registry::instance();
  return ctx;
}

Result<Scene> read_vd(ByteView bytes) { return read_scene("vectordrawable", bytes, test_context()); }

bool near(double a, double b, double tol = 1e-3) { return std::abs(a - b) <= tol; }

bool same_color(const Color& c, int r, int g, int b, int a = 255) {
  const auto byte = [](float v) { return static_cast<int>(std::lround(v * 255.0f)); };
  return byte(c.r) == r && byte(c.g) == g && byte(c.b) == b && byte(c.a) == a;
}

}  // namespace

TEST_CASE("vector/vectordrawable: path data parser handles every command and compact numbers") {
  auto parsed = misc::parse_path_data("M10-20l5.5.5H0v1c1 2 3 4 5 6s1 1 2 2q1 1 2 2t3 3a5 5 0 0 1 10 0zm1 1", 1000);
  REQUIRE_OK(parsed);
  CHECK_FALSE(parsed->error);
  const Path& p = parsed->path;
  REQUIRE(p.verbs.size() >= 9);
  CHECK(p.verbs[0] == Verb::Move);
  CHECK(p.points[0] == Point{10, -20});
  CHECK(p.points[1] == Point{15.5, -19.5});  // "5.5.5" is two numbers
  CHECK(p.points[2] == Point{0, -19.5});
  CHECK(p.points[3] == Point{0, -18.5});
  // The relative move after z starts at the subpath start (10,-20).
  CHECK(p.points.back() == Point{11, -19});

  auto broken = misc::parse_path_data("M0 0L10 10L20", 1000);
  REQUIRE_OK(broken);
  CHECK(broken->error);
  CHECK(broken->path.verbs.size() == 2);  // rendered up to the error

  CHECK_STATUS(misc::parse_path_data([] {
    std::string s = "M0 0";
    for (int i = 0; i < 100; ++i) s += "L1 1";
    return s;
  }(), 10), Status::Limit);
}

TEST_CASE("vector/vectordrawable: minified path data reads back within the precision") {
  Path path;
  path.move_to({0.123456, 10});
  path.line_to({20.5, 10});
  path.line_to({20.5, -3.25});
  path.cubic_to({1, 2}, {3, 4}, {5, 6});
  path.cubic_to({7, 8}, {9, 9}, {10, 11});
  path.quad_to({12, 13}, {14, 15});
  path.close();
  path.move_to({100, 100});
  path.line_to({101.0004, 99.9996});
  for (int precision : {0, 2, 3, 5}) {
    const std::string text = misc::minify_path_data(path, precision);
    auto back = misc::parse_path_data(text, 1000);
    REQUIRE_OK(back);
    CHECK_FALSE(back->error);
    CHECK(back->path.verbs == path.verbs);
    const double tol = 0.5 * std::pow(10.0, -precision) + 1e-9;
    REQUIRE(back->path.points.size() == path.points.size());
    for (std::size_t i = 0; i < path.points.size(); ++i) {
      INFO("precision ", precision, " point ", i, " text ", text);
      CHECK(near(back->path.points[i].x, path.points[i].x, tol));
      CHECK(near(back->path.points[i].y, path.points[i].y, tol));
    }
  }
  CHECK(misc::minify_path_data(path, 0).size() < misc::minify_path_data(path, 4).size());
  CHECK(misc::format_number(-0.0001, 3) == "0");
  CHECK(misc::format_number(0.5, 3, true) == ".5");
  CHECK(misc::format_number(-0.5, 3, true) == "-.5");
  CHECK(misc::format_number(12.3400, 4) == "12.34");
}

TEST_CASE("vector/vectordrawable: colours and transform decomposition") {
  CHECK(misc::parse_android_color("#F00") == Color{1, 0, 0, 1});
  CHECK(same_color(*misc::parse_android_color("#8F00"), 255, 0, 0, 0x88));
  CHECK(same_color(*misc::parse_android_color("#336699"), 0x33, 0x66, 0x99));
  CHECK(same_color(*misc::parse_android_color("#40800080"), 0x80, 0, 0x80, 0x40));
  CHECK_FALSE(misc::parse_android_color("#12345").has_value());
  CHECK_FALSE(misc::parse_android_color("red").has_value());
  CHECK(misc::android_color_string(Color::rgb8(1, 2, 3)) == "#010203");
  CHECK(misc::android_color_string(Color::rgb8(1, 2, 3, 128)) == "#80010203");

  const Matrix m = Matrix::translate(5, 6) * Matrix::rotate(0.5) * Matrix::scale(2, -3);
  const auto d = misc::decompose(m);
  REQUIRE(d.has_value());
  const Matrix back = Matrix::translate(d->tx, d->ty) * Matrix::rotate(d->rotation * std::numbers::pi / 180) *
                      Matrix::scale(d->sx, d->sy);
  CHECK(near(back.a, m.a, 1e-9));
  CHECK(near(back.b, m.b, 1e-9));
  CHECK(near(back.c, m.c, 1e-9));
  CHECK(near(back.d, m.d, 1e-9));
  CHECK_FALSE(misc::decompose(Matrix::skew_x(0.3)).has_value());
}

TEST_CASE("vector/vectordrawable: decodes svg2vectordrawable shapes") {
  auto scene = read_vd(test::fixture("vector/misc/shapes_vd.xml"));
  REQUIRE_OK(scene);
  REQUIRE(scene->pages.size() == 1);
  CHECK(scene->pages[0].width == 48);
  CHECK(scene->pages[0].height == 48);
  const auto shapes = shapes_of(*scene);
  REQUIRE(shapes.size() == 4);
  // Red rectangle 2,2 8x6 in a 24 viewport drawn at 48 px.
  const Rect r0 = bounds(shapes[0].shape.path, shapes[0].total);
  CHECK(near(r0.x0, 4));
  CHECK(near(r0.y0, 4));
  CHECK(near(r0.x1, 20));
  CHECK(near(r0.y1, 16));
  CHECK(same_color(shapes[0].shape.fill.color, 255, 0, 0));
  // Arc: the half circle reaches y = 10 (viewport) above its chord at 14.
  const Rect r1 = bounds(shapes[1].shape.path, shapes[1].total);
  CHECK(near(r1.y0, 20, 0.01));
  CHECK(near(r1.x0, 4, 0.01));
  CHECK(near(r1.x1, 20, 0.01));
  CHECK(same_color(shapes[1].shape.fill.color, 0x33, 0x66, 0x99));
  // Rotated square centred on (17,17): diagonal half-width 3*sqrt2.
  const Rect r2 = bounds(shapes[2].shape.path, shapes[2].total);
  CHECK(near(r2.x0, 2 * (17 - 3 * std::sqrt(2.0)), 0.01));
  CHECK(near(r2.y1, 2 * (17 + 3 * std::sqrt(2.0)), 0.01));
  CHECK(shapes[3].shape.fill_rule == FillRule::EvenOdd);
}

TEST_CASE("vector/vectordrawable: decodes strokes, alphas and group scale") {
  auto scene = read_vd(test::fixture("vector/misc/strokes_vd.xml"));
  REQUIRE_OK(scene);
  const auto shapes = shapes_of(*scene);
  REQUIRE(shapes.size() == 4);
  const Shape& circle = shapes[0].shape;
  CHECK(same_color(circle.fill.color, 0, 255, 0, 0x80));
  CHECK(same_color(circle.stroke.paint.color, 0, 0, 255));
  CHECK(circle.stroke.width == 1.5);
  CHECK(circle.stroke.cap == LineCap::Round);
  CHECK(circle.stroke.join == LineJoin::Round);
  const Rect rc = bounds(circle.path, shapes[0].total);
  CHECK(near(rc.x0, 3, 0.01));
  CHECK(near(rc.x1, 11, 0.01));
  const Shape& chevron = shapes[1].shape;
  CHECK_FALSE(chevron.fill.visible());
  CHECK(same_color(chevron.stroke.paint.color, 0x80, 0, 0x80, 0x40));
  CHECK(chevron.stroke.cap == LineCap::Square);
  CHECK(chevron.stroke.join == LineJoin::Bevel);
  const Rect rs = bounds(shapes[3].shape.path, shapes[3].total);
  CHECK(near(rs.x0, 16));
  CHECK(near(rs.x1, 20));
  CHECK(near(rs.y0, 19.5));
  CHECK(near(rs.y1, 20.5));
}

TEST_CASE("vector/vectordrawable: decodes aapt:attr gradients") {
  auto scene = read_vd(test::fixture("vector/misc/gradient_vd.xml"));
  REQUIRE_OK(scene);
  const auto shapes = shapes_of(*scene);
  REQUIRE(shapes.size() == 2);
  const Paint& linear = shapes[0].shape.fill;
  REQUIRE(linear.type == Paint::Type::Gradient);
  CHECK(linear.gradient->type == Gradient::Type::Linear);
  CHECK(linear.gradient->p2 == Point{32, 0});
  REQUIRE(linear.gradient->stops.size() == 2);
  CHECK(same_color(linear.gradient->stops[0].color, 255, 0, 0));
  CHECK(same_color(linear.gradient->stops[1].color, 0, 0, 255));
  const Paint& radial = shapes[1].shape.fill;
  REQUIRE(radial.type == Paint::Type::Gradient);
  CHECK(radial.gradient->type == Gradient::Type::Radial);
  CHECK(radial.gradient->center == Point{16, 24});
  CHECK(radial.gradient->radius == 8);
}

TEST_CASE("vector/vectordrawable: groups, clip paths, trim paths, sweep and references") {
  std::vector<std::string> warnings;
  auto data = test::read_as<ir::Data>("vectordrawable", test::fixture("vector/misc/features_hand.xml"), {}, &warnings);
  REQUIRE_OK(data);
  CHECK_FALSE(warnings.empty());  // the @color reference
  auto scene = read_vd(test::fixture("vector/misc/features_hand.xml"));
  REQUIRE_OK(scene);
  CHECK(scene->pages[0].width == 200);
  CHECK(scene->pages[0].height == 100);
  CHECK(near(scene->pages[0].root.opacity, 0.8));
  std::vector<const Group*> groups;
  const auto shapes = shapes_of(*scene, &groups);
  // Pivoted rotation: 15..35 x 20..30 turned about (25,25), at 2x.
  REQUIRE(!shapes.empty());
  CHECK(shapes[0].shape.id == "bar");
  const Rect bar = bounds(shapes[0].shape.path, shapes[0].total);
  CHECK(near(bar.x0, 40));
  CHECK(near(bar.x1, 60));
  CHECK(near(bar.y0, 30));
  CHECK(near(bar.y1, 70));
  // The clip applies to the paths after it in the group.
  bool found_clip = false;
  for (const Group* g : groups) {
    if (g->clip) {
      const Rect c = bounds(g->clip->items.at(0).path);
      if (near(c.x1, 25) && near(c.y1, 50)) found_clip = true;
    }
  }
  CHECK(found_clip);
  // Trim keeps the middle half of the line: 25..75.
  const Placed* trimmed = nullptr;
  for (const Placed& p : shapes) {
    if (p.shape.id == "trimmed") trimmed = &p;
  }
  REQUIRE(trimmed != nullptr);
  const Rect t = bounds(trimmed->shape.path);
  CHECK(near(t.x0, 25, 0.01));
  CHECK(near(t.x1, 75, 0.01));
  // Sweep: a clipped group of solid wedges.
  std::size_t wedges = 0;
  for (const Group* g : groups) {
    if (g->clip && g->children.size() >= 36) wedges = g->children.size();
  }
  CHECK(wedges >= 36);
  // Unresolvable @color: black, alpha kept.
  const Placed& last = shapes.back();
  CHECK(same_color(last.shape.fill.color, 0, 0, 0));
  CHECK(near(last.shape.fill.opacity, 0.5));
}

TEST_CASE("vector/vectordrawable: tint recolours the drawing") {
  const std::string vd = R"(<vector xmlns:android="http://schemas.android.com/apk/res/android" android:width="24dp"
    android:height="24dp" android:viewportWidth="24" android:viewportHeight="24" android:tint="#FF00FF00">
    <path android:fillColor="@android:color/white" android:pathData="M0 0h24v24z"/></vector>)";
  auto scene = read_vd(as_bytes(vd));
  REQUIRE_OK(scene);
  const auto shapes = shapes_of(*scene);
  REQUIRE(shapes.size() == 1);
  CHECK(same_color(shapes[0].shape.fill.color, 0, 255, 0));
}

TEST_CASE("vector/vectordrawable: write and read back keep geometry and colours") {
  auto original = read_vd(test::fixture("vector/misc/strokes_vd.xml"));
  REQUIRE_OK(original);
  Context ctx = test_context();
  auto written = write_scene("vectordrawable", *original, ctx);
  REQUIRE_OK(written);
  auto back = read_vd(*written);
  REQUIRE_OK(back);
  const auto a = shapes_of(*original);
  const auto b = shapes_of(*back);
  REQUIRE(a.size() == b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    INFO("shape ", i);
    const Rect ra = bounds(a[i].shape.path, a[i].total);
    const Rect rb = bounds(b[i].shape.path, b[i].total);
    CHECK(near(ra.x0, rb.x0, 0.01));
    CHECK(near(ra.y0, rb.y0, 0.01));
    CHECK(near(ra.x1, rb.x1, 0.01));
    CHECK(near(ra.y1, rb.y1, 0.01));
    CHECK(a[i].shape.fill.color == b[i].shape.fill.color);
    CHECK(a[i].shape.stroke.paint.color == b[i].shape.stroke.paint.color);
    CHECK(near(a[i].shape.stroke.width * a[i].total.mean_scale(), b[i].shape.stroke.width * b[i].total.mean_scale(), 1e-3));
    CHECK(a[i].shape.stroke.cap == b[i].shape.stroke.cap);
  }

  auto gradients = read_vd(test::fixture("vector/misc/gradient_vd.xml"));
  REQUIRE_OK(gradients);
  auto g_written = write_scene("vectordrawable", *gradients, ctx);
  REQUIRE_OK(g_written);
  CHECK(test::str(*g_written).find("aapt:attr") != std::string::npos);
  auto g_back = read_vd(*g_written);
  REQUIRE_OK(g_back);
  const auto gs = shapes_of(*g_back);
  REQUIRE(gs.size() == 2);
  REQUIRE(gs[1].shape.fill.type == Paint::Type::Gradient);
  CHECK(gs[1].shape.fill.gradient->radius == 8);
}

TEST_CASE("vector/vectordrawable: writer handles skew, dashes, text, pictures and gradient transforms") {
  Scene scene;
  Page page;
  page.width = 100;
  page.height = 50;
  Group skewed;
  skewed.transform = Matrix::skew_x(0.4);
  Shape box;
  add_rect(box.path, 10, 10, 20, 20);
  box.fill = Paint::solid(Color{0, 0, 1, 1});
  skewed.children.push_back(Node{box});
  page.root.children.push_back(Node{skewed});
  Shape dashed;
  dashed.path.move_to({0, 40});
  dashed.path.line_to({100, 40});
  dashed.stroke.paint = Paint::solid(Color{1, 0, 0, 1});
  dashed.stroke.width = 2;
  dashed.stroke.dash = {10, 10};
  page.root.children.push_back(Node{dashed});
  Shape graded;
  add_rect(graded.path, 50, 0, 50, 30);
  auto gradient = std::make_shared<Gradient>();
  gradient->p1 = {0, 0};
  gradient->p2 = {1, 0};
  gradient->transform = Matrix::translate(50, 0) * Matrix::scale(50, 30);  // objectBoundingBox-like
  gradient->stops = {{0, Color{1, 0, 0, 1}}, {1, Color{0, 1, 0, 1}}};
  graded.fill.type = Paint::Type::Gradient;
  graded.fill.gradient = gradient;
  page.root.children.push_back(Node{graded});
  page.root.children.push_back(Node{make_text("Hi", FontSpec{}, {5, 45}, Paint::solid(Color{0, 0, 0, 1}))});
  Picture picture;
  picture.bounds = Rect::xywh(0, 0, 5, 5);
  page.root.children.push_back(Node{picture});
  scene.pages.push_back(page);

  Context ctx = test_context();
  auto written = write_scene("vectordrawable", scene, ctx);
  REQUIRE_OK(written);
  const std::string text = test::str(*written);
  CHECK(text.find("<group") == std::string::npos);  // the skew was baked into the path data
  CHECK(text.find("android:startX=\"50\"") != std::string::npos);
  CHECK(text.find("android:endX=\"100\"") != std::string::npos);
  CHECK(ctx.warnings.list().size() >= 2);  // dashes, picture
  auto back = read_vd(*written);
  REQUIRE_OK(back);
  const auto shapes = shapes_of(*back);
  REQUIRE(shapes.size() >= 4);
  const Rect skew_box = bounds(shapes[0].shape.path, shapes[0].total);
  CHECK(near(skew_box.x0, 10 + 10 * std::tan(0.4), 0.01));
  CHECK(near(skew_box.x1, 30 + 30 * std::tan(0.4), 0.01));
  // Dashes: five separate subpaths.
  std::size_t moves = 0;
  for (Verb v : shapes[1].shape.path.verbs) moves += v == Verb::Move ? 1 : 0;
  CHECK(moves == 5);
  CHECK(shapes[1].shape.stroke.dash.empty());
}

TEST_CASE("vector/vectordrawable: precision option and the compress path") {
  const Bytes original = test::fixture("vector/misc/features_hand.xml");
  const auto coarse = test::convert(original, "vectordrawable", "vectordrawable", R"({"precision":0})");
  const auto fine = test::convert(original, "vectordrawable", "vectordrawable", R"({"precision":6})");
  REQUIRE(coarse.ok());
  REQUIRE(fine.ok());
  CHECK(coarse.data.size() <= fine.data.size());
  const auto compressed = test::convert(test::fixture("vector/misc/shapes_vd.xml"), "", "vectordrawable", "", "icon.xml");
  INFO(compressed.message);
  REQUIRE(compressed.ok());
  CHECK(compressed.format_id == "vectordrawable");
  CHECK(compressed.text().find("android:pathData") != std::string::npos);
}

TEST_CASE("vector/vectordrawable: detection does not steal other XML files") {
  const Registry& registry = Registry::instance();
  CHECK(registry.validate().empty());
  const auto vd = detect_format(registry, test::fixture("vector/misc/shapes_vd.xml"), "ic_shapes.xml");
  REQUIRE(vd.has_value());
  CHECK(vd->format->id == "vectordrawable");
  const auto hand = detect_format(registry, test::fixture("vector/misc/features_hand.xml"), "");
  REQUIRE(hand.has_value());
  CHECK(hand->format->id == "vectordrawable");
  const std::string plain = "<?xml version=\"1.0\"?>\n<catalog><vector>1</vector></catalog>\n";
  const auto xml = detect_format(registry, as_bytes(plain), "data.xml");
  REQUIRE(xml.has_value());
  CHECK(xml->format->id != "vectordrawable");
  // A <vector> without the Android namespace (e.g. some other schema) is not claimed either.
  const std::string other = "<vector xmlns=\"urn:example\"><item pathData=\"M0 0\"/></vector>";
  const auto other_detect = detect_format(registry, as_bytes(other), "vec.xml");
  CHECK((!other_detect.has_value() || other_detect->format->id != "vectordrawable"));
  const auto svg = detect_format(registry, test::fixture("vector/misc/shapes.svg"), "shapes.svg");
  REQUIRE(svg.has_value());
  CHECK(svg->format->id != "vectordrawable");
}

TEST_CASE("vector/vectordrawable: hostile input fails cleanly") {
  for (const char* name : {"shapes_vd.xml", "gradient_vd.xml", "strokes_vd.xml", "features_hand.xml"}) {
    CHECK(test::fuzz_reader<ir::Data>("vectordrawable", test::fixture(std::string("vector/misc/") + name)) > 0);
  }
  const std::string head = R"(<vector xmlns:android="http://schemas.android.com/apk/res/android" android:width="24dp" android:height="24dp" android:viewportWidth="24" android:viewportHeight="24">)";
  CHECK_STATUS(read_vd(as_bytes(std::string("<vector/>"))), Status::Decode);
  CHECK_STATUS(read_vd(as_bytes(std::string("<svg/>"))), Status::Decode);
  CHECK_STATUS(read_vd(as_bytes(std::string(R"(<vector xmlns:android="http://schemas.android.com/apk/res/android" android:width="-4dp" android:height="nan" android:viewportWidth="0" android:viewportHeight="24"/>)"))),
               Status::Decode);
  std::string deep = head;
  for (int i = 0; i < 300; ++i) deep += "<group>";
  for (int i = 0; i < 300; ++i) deep += "</group>";
  deep += "</vector>";
  const auto deep_result = read_vd(as_bytes(deep));
  CHECK_FALSE(deep_result.has_value());
  // Garbage attribute values never crash and fall back to defaults.
  const std::string garbage = head +
      R"(<group android:rotation="x" android:scaleX="1e999"><path android:pathData="M0 0L1e308 1e308 Z" android:fillColor="#GGG" android:strokeWidth="-5" android:trimPathEnd="7"/></group></vector>)";
  CHECK_OK(read_vd(as_bytes(garbage)));
}
