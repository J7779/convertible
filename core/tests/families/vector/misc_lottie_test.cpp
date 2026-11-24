// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <string>
#include <vector>

#include "engine/detect.hpp"
#include "engine/registry.hpp"
#include "families/image/image_api.hpp"
#include "families/vector/geom.hpp"
#include "families/vector/vector_codecs.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"

using namespace cvt;
using namespace cvt::vector;

namespace {

struct Placed {
  const Node* node = nullptr;
  Matrix total;
  double opacity = 1.0;
  const Group* parent = nullptr;
};

void collect(const Group& group, const Matrix& parent, double opacity, std::vector<Placed>& out) {
  const Matrix total = parent * group.transform;
  opacity *= group.opacity;
  for (const Node& node : group.children) {
    if (const auto* g = std::get_if<Group>(&node.value)) {
      collect(*g, total, opacity, out);
    } else {
      out.push_back({&node, total, opacity, &group});
    }
  }
}

struct Flat {
  Scene scene;
  std::vector<Placed> items;
  std::vector<const Shape*> shapes;
};

Flat flatten_scene(Scene scene) {
  Flat f;
  f.scene = std::move(scene);
  collect(f.scene.pages.at(0).root, Matrix{}, 1.0, f.items);
  for (const Placed& p : f.items) {
    if (const auto* s = std::get_if<Shape>(&p.node->value)) f.shapes.push_back(s);
  }
  return f;
}

Context test_context() {
  Context ctx;
  ctx.registry = &Registry::instance();
  return ctx;
}

Result<Flat> read_lottie(ByteView bytes, std::string_view options = {}) {
  auto opts = Options::parse(options);
  if (!opts) return std::unexpected(opts.error());
  CVT_TRY_ASSIGN(Scene scene, read_scene("lottie", bytes, test_context(), *opts));
  return flatten_scene(std::move(scene));
}

bool near(double a, double b, double tol = 1e-3) { return std::abs(a - b) <= tol; }

Rect placed_bounds(const Placed& p) {
  if (const auto* s = std::get_if<Shape>(&p.node->value)) return bounds(s->path, p.total);
  if (const auto* pic = std::get_if<Picture>(&p.node->value)) return transform_rect(pic->bounds, p.total);
  if (const auto* t = std::get_if<Text>(&p.node->value)) return bounds(t->outline, p.total);
  return {};
}

const Placed* find_fill(const Flat& f, float r, float g, float b) {
  for (const Placed& p : f.items) {
    if (const auto* s = std::get_if<Shape>(&p.node->value);
        s != nullptr && s->fill.type == Paint::Type::Solid && near(s->fill.color.r, r, 0.01) &&
        near(s->fill.color.g, g, 0.01) && near(s->fill.color.b, b, 0.01)) {
      return &p;
    }
  }
  return nullptr;
}

}  // namespace

TEST_CASE("vector/lottie: decodes python-lottie shapes") {
  auto f = read_lottie(test::fixture("vector/misc/lottie_shapes.json"));
  REQUIRE_OK(f);
  CHECK(f->scene.pages[0].width == 120);
  CHECK(f->scene.pages[0].height == 80);
  const Placed* rect = find_fill(*f, 1, 0, 0);
  REQUIRE(rect != nullptr);
  const Rect r = placed_bounds(*rect);
  CHECK(near(r.x0, 5));
  CHECK(near(r.y0, 10));
  CHECK(near(r.x1, 35));
  CHECK(near(r.y1, 30));
  const Placed* ellipse = find_fill(*f, 0, 0, 1);
  REQUIRE(ellipse != nullptr);
  const Rect e = placed_bounds(*ellipse);
  CHECK(near(e.x0, 50, 0.01));
  CHECK(near(e.x1, 70, 0.01));
  // The stroke of the ellipse group is a separate shape on top of the fill.
  bool stroke = false;
  for (const Shape* s : f->shapes) {
    if (s->stroke.paint.visible() && near(s->stroke.width, 2)) stroke = true;
  }
  CHECK(stroke);
  // Five-pointed star: the top point is at (95, 20 - 15).
  const Placed* star = find_fill(*f, 1, 1, 0);
  REQUIRE(star != nullptr);
  const Rect s = placed_bounds(*star);
  CHECK(near(s.y0, 5, 0.01));
  const auto* star_shape = std::get_if<Shape>(&star->node->value);
  CHECK(star_shape->path.verbs.size() == 12);  // move, 10 edges, close
  // Hexagon with 50% group opacity.
  const Placed* hexagon = find_fill(*f, 0.5, 0, 0.5);
  REQUIRE(hexagon != nullptr);
  CHECK(near(hexagon->opacity, 0.5));
  // Bezier path with a curved edge.
  const Placed* path = find_fill(*f, 0, 0.5, 0);
  REQUIRE(path != nullptr);
  bool curve = false;
  for (Verb v : std::get_if<Shape>(&path->node->value)->path.verbs) curve = curve || v == Verb::Cubic;
  CHECK(curve);
}

TEST_CASE("vector/lottie: keyframes are evaluated at the requested frame") {
  const Bytes data = test::fixture("vector/misc/lottie_animated.json");
  auto first = read_lottie(data);
  REQUIRE_OK(first);
  REQUIRE(first->shapes.size() == 1);
  Rect r = placed_bounds(first->items[0]);
  CHECK(near((r.x0 + r.x1) / 2, 20));
  CHECK(near((r.y0 + r.y1) / 2, 50));
  CHECK(near(first->items[0].opacity, 1.0));

  auto middle = read_lottie(data, R"({"frame":30})");
  REQUIRE_OK(middle);
  r = placed_bounds(middle->items[0]);
  CHECK(near((r.x0 + r.x1) / 2, 50, 0.01));  // linear easing halfway
  CHECK(near(middle->items[0].opacity, 0.5, 0.01));
  // Rotated by 45 degrees: the 20 px square's bounds grow to 20*sqrt2.
  CHECK(near(r.x1 - r.x0, 20 * std::sqrt(2.0), 0.01));
}

TEST_CASE("vector/lottie: gradients, dashes and precompositions") {
  auto f = read_lottie(test::fixture("vector/misc/lottie_gradient_precomp.json"));
  REQUIRE_OK(f);
  const Shape* gradient = nullptr;
  const Shape* dashed = nullptr;
  for (const Shape* s : f->shapes) {
    if (s->fill.type == Paint::Type::Gradient) gradient = s;
    if (!s->stroke.dash.empty()) dashed = s;
  }
  REQUIRE(gradient != nullptr);
  CHECK(gradient->fill.gradient->p1 == Point{5, 15});
  CHECK(gradient->fill.gradient->p2 == Point{45, 15});
  REQUIRE(gradient->fill.gradient->stops.size() == 2);
  CHECK(gradient->fill.gradient->stops[0].color == Color{1, 0, 0, 1});
  REQUIRE(dashed != nullptr);
  CHECK(dashed->stroke.dash == std::vector<double>{6, 4});
  CHECK(dashed->stroke.width == 3);
  // The precomp square (10..30 in the precomp) lands at 60..80 x 20..40.
  const Placed* green = find_fill(*f, 0, 1, 0);
  REQUIRE(green != nullptr);
  const Rect g = placed_bounds(*green);
  CHECK(near(g.x0, 60));
  CHECK(near(g.x1, 80));
  CHECK(near(g.y0, 20));
  CHECK(near(g.y1, 40));
}

TEST_CASE("vector/lottie: parenting, hold keys, trim, masks, images and layer ranges") {
  const Bytes data = test::fixture("vector/misc/lottie_features_hand.json");
  auto f = read_lottie(data);
  REQUIRE_OK(f);
  CHECK(f->scene.metadata.at("title") == "features");
  const Placed* box = find_fill(*f, 0, 0, 1);
  REQUIRE(box != nullptr);
  Rect r = placed_bounds(*box);
  CHECK(near(r.x0, 10));
  CHECK(near(r.y1, 30));
  // Trimmed line keeps 50..150.
  const Shape* line = nullptr;
  for (const Shape* s : f->shapes) {
    if (s->stroke.paint.visible()) line = s;
  }
  REQUIRE(line != nullptr);
  const Rect lb = bounds(line->path);
  CHECK(near(lb.x0, 50, 0.01));
  CHECK(near(lb.x1, 150, 0.01));
  CHECK(line->stroke.cap == LineCap::Butt);
  // Masked solid: opacity 0 at the first frame, clipped to its left half.
  const Placed* solid = find_fill(*f, 1, 128.0f / 255.0f, 0);
  REQUIRE(solid != nullptr);
  CHECK(near(solid->opacity, 0.0));
  REQUIRE(solid->parent->clip != nullptr);
  const Rect clip = bounds(solid->parent->clip->items.at(0).path);
  CHECK(near(clip.x1, 25));
  // The embedded PNG picture.
  const Picture* picture = nullptr;
  for (const Placed& p : f->items) {
    if (const auto* pic = std::get_if<Picture>(&p.node->value)) {
      picture = pic;
      const Rect pb = placed_bounds(p);
      CHECK(near(pb.y0, 40));
      CHECK(near(pb.x1, 20));
    }
  }
  if (image::can_decode("png", test_context())) {
    REQUIRE(picture != nullptr);
    REQUIRE(picture->pixels != nullptr);
    CHECK(picture->pixels->width == 2);
    CHECK(picture->pixels->rgba[1] == 255);
  }
  // The "late" layer starts at frame 40.
  CHECK(find_fill(*f, 0, 0, 0) == nullptr);

  auto later = read_lottie(data, R"({"frame":45})");
  REQUIRE_OK(later);
  CHECK(find_fill(*later, 0, 0, 0) != nullptr);
  const Placed* moved = find_fill(*later, 0, 0, 1);
  REQUIRE(moved != nullptr);
  r = placed_bounds(*moved);
  CHECK(near(r.x0, 110));  // parent moved after its hold keyframe

  auto hold = read_lottie(data, R"({"frame":20})");
  REQUIRE_OK(hold);
  r = placed_bounds(*find_fill(*hold, 0, 0, 1));
  CHECK(near(r.x0, 10));  // still held at frame 20
  // Bezier easing (ease in-out) is symmetric: halfway gives half opacity.
  auto half = read_lottie(data, R"({"frame":25})");
  REQUIRE_OK(half);
  CHECK(near(find_fill(*half, 1, 128.0f / 255.0f, 0)->opacity, 0.5, 0.01));
}

TEST_CASE("vector/lottie: SVG imported by python-lottie, as JSON and as .lottie") {
  auto json = read_lottie(test::fixture("vector/misc/lottie_from_svg.json"));
  REQUIRE_OK(json);
  auto zipped = read_lottie(test::fixture("vector/misc/lottie_from_svg.lottie"));
  REQUIRE_OK(zipped);
  CHECK(json->shapes.size() == zipped->shapes.size());
  // Orange rectangle 4..20 x 14..20, teal square scaled (2, 0.5) around (18,20).
  const Placed* orange = find_fill(*json, 1, 0.502f, 0);
  REQUIRE(orange != nullptr);
  const Rect o = placed_bounds(*orange);
  CHECK(near(o.x0, 4));
  CHECK(near(o.y1, 20));
  const Placed* teal = find_fill(*json, 0, 0.502f, 0.502f);
  REQUIRE(teal != nullptr);
  const Rect t = placed_bounds(*teal);
  CHECK(near(t.x0, 16));
  CHECK(near(t.x1, 20));
  CHECK(near(t.y0, 19.5));
  CHECK(near(t.y1, 20.5));
}

TEST_CASE("vector/lottie: detection claims Lottie but not other JSON") {
  const Registry& registry = Registry::instance();
  const auto lottie = detect_format(registry, test::fixture("vector/misc/lottie_shapes.json"), "anim.json");
  REQUIRE(lottie.has_value());
  CHECK(lottie->format->id == "lottie");
  const auto dot = detect_format(registry, test::fixture("vector/misc/lottie_from_svg.lottie"), "");
  REQUIRE(dot.has_value());
  CHECK(dot->format->id == "lottie");
  const std::string plain = R"({"name": "demo", "version": "1.0.0", "dependencies": {}})";
  const auto json = detect_format(registry, as_bytes(plain), "package.json");
  REQUIRE(json.has_value());
  CHECK(json->format->id != "lottie");
  const std::string almost = R"({"v": 1, "w": 2, "h": 3, "list": []})";
  const auto other = detect_format(registry, as_bytes(almost), "data.json");
  CHECK((!other.has_value() || other->format->id != "lottie"));
}

TEST_CASE("vector/lottie: converts to other vector formats and to an animation") {
  const auto vd = test::convert(test::fixture("vector/misc/lottie_shapes.json"), "", "vectordrawable", "", "a.json");
  INFO(vd.message);
  CHECK(vd.ok());
  const auto image = test::read_as<ir::Image>("lottie", test::fixture("vector/misc/lottie_animated.json"),
                                              R"({"animation_frames":4})");
  if (image.has_value()) {
    CHECK(image->frames.size() == 4);
    CHECK(image->frames[0].delay_ms == 500);
    CHECK(image->width() == 100);
  } else {
    // The rasteriser is not available in this build yet.
    CHECK(image.error().code == Status::UnsupportedConversion);
  }
}

TEST_CASE("vector/lottie: hostile input fails cleanly") {
  for (const char* name : {"lottie_shapes.json", "lottie_animated.json", "lottie_gradient_precomp.json",
                           "lottie_features_hand.json", "lottie_from_svg.lottie"}) {
    CHECK(test::fuzz_reader<ir::Data>("lottie", test::fixture(std::string("vector/misc/") + name)) > 0);
  }
  CHECK_STATUS(read_lottie(as_bytes(std::string("{}"))), Status::Decode);
  CHECK_STATUS(read_lottie(as_bytes(std::string(R"({"w":0,"h":10,"layers":[]})"))), Status::Decode);
  // A precomp that references itself.
  const std::string loop = R"({"v":"5","fr":30,"ip":0,"op":10,"w":10,"h":10,
    "assets":[{"id":"a","layers":[{"ty":0,"refId":"a","ks":{}}]}],"layers":[{"ty":0,"refId":"a","ks":{}}]})";
  CHECK_STATUS(read_lottie(as_bytes(loop)), Status::Limit);
  // Garbage values never crash.
  const std::string garbage = R"({"v":"5","fr":-1,"ip":0,"op":10,"w":10,"h":10,"layers":[
    {"ty":4,"parent":1,"ind":1,"ks":{"p":{"a":1,"k":[{"t":"x"},{"t":5,"s":"y"}]},"s":{"k":[1e308,-1e308]}},
     "shapes":[{"ty":"sr","pt":{"k":1e9}},{"ty":"sh","ks":{"k":{"v":[[1],[2,3]],"i":[],"o":5}}},{"ty":"fl","c":{"k":"red"}},
               {"ty":"gf","g":{"p":1e12,"k":{"k":[1]}}},{"ty":"tm","s":{"k":-50},"e":{"k":500}},{"ty":"rd","r":{"k":1e300}},
               {"ty":"gr","it":5},{"ty":"st","d":[{"n":"d","v":{"k":0}}]}]},
    {"ty":5,"t":{"d":{"k":[{"s":{"t":"Hi\rthere","s":1e9,"fc":[1,0,0]}}]}},"ks":{}}]})";
  CHECK_OK(read_lottie(as_bytes(garbage)));
}
