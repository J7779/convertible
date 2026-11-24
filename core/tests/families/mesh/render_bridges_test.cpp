// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/registry.hpp"
#include "families/mesh/mesh_ops.hpp"
#include "families/mesh/render.hpp"
#include "families/mesh/scenes.hpp"
#include "ir/data.hpp"
#include "ir/table.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

using namespace cvt;

namespace {

Context make_context(std::string_view options = {}) {
  Context ctx;
  ctx.registry = &Registry::instance();
  if (!options.empty()) ctx.options = *Options::parse(options);
  return ctx;
}

double coverage(const ir::Frame& frame) {
  std::size_t opaque = 0;
  for (std::size_t i = 3; i < frame.rgba.size(); i += 4) opaque += frame.rgba[i] > 127 ? 1 : 0;
  return static_cast<double>(opaque) / (frame.width * frame.height);
}

const Bridge* bridge_to(Kind to) {
  for (const Bridge& bridge : Registry::instance().bridges()) {
    if (bridge.from == Kind::Mesh && bridge.to == to) return &bridge;
  }
  return nullptr;
}

}  // namespace

TEST_CASE("mesh/render: a framed, shaded, anti-aliased picture") {
  const Context ctx = make_context();
  mesh::RenderOptions options;
  options.size = 96;
  options.view = "front";
  const auto picture = mesh::render_mesh(test::scenes::unit_cube(), options, ctx);
  REQUIRE_OK(picture);
  REQUIRE(picture->frames.size() == 1);
  const ir::Frame& frame = picture->frames[0];
  CHECK(frame.width == 96);
  // A cube seen face-on fills a centred square; stills are framed so the
  // model spans 95% of the picture: 0.95^2 = 0.9025 of the pixels.
  const double c = coverage(frame);
  CHECK(c > 0.88);
  CHECK(c < 0.94);
  CHECK(frame.rgba[3] == 0);                                 // transparent corner
  CHECK(frame.rgba[frame.offset(48, 48) + 3] == 255);        // opaque centre
  // Slanted edges are anti-aliased: some partially covered pixels exist.
  options.view = "iso";
  const auto iso = mesh::render_mesh(test::scenes::unit_cube(), options, ctx);
  REQUIRE_OK(iso);
  std::size_t partial = 0;
  for (std::size_t i = 3; i < iso->frames[0].rgba.size(); i += 4) {
    partial += iso->frames[0].rgba[i] > 0 && iso->frames[0].rgba[i] < 255 ? 1 : 0;
  }
  CHECK(partial > 20);
  options.view = "front";
  // Deterministic.
  const auto again = mesh::render_mesh(test::scenes::unit_cube(), options, ctx);
  REQUIRE_OK(again);
  CHECK(again->frames[0].rgba == frame.rgba);
}

TEST_CASE("mesh/render: colours, backgrounds, views and turntables") {
  const Context ctx = make_context();
  ir::Mesh red = test::scenes::uv_sphere();
  red.colors.assign(red.positions.size(), ir::Color{1, 0, 0, 1});
  mesh::RenderOptions options;
  options.size = 64;
  options.background = "dark";
  const auto picture = mesh::render_mesh(red, options, ctx);
  REQUIRE_OK(picture);
  const ir::Frame& frame = picture->frames[0];
  const std::size_t centre = frame.offset(32, 32);
  CHECK(frame.rgba[centre] > 100);
  CHECK(frame.rgba[centre + 1] < 40);
  CHECK(frame.rgba[3] == 255);  // opaque background
  CHECK(frame.rgba[0] == 28);   // "dark" is sRGB (0.11, 0.12, 0.14)
  options.background = "zz";
  CHECK_STATUS(mesh::render_mesh(red, options, ctx), Status::InvalidArgument);

  mesh::RenderOptions turntable;
  turntable.size = 48;
  turntable.frames = 6;
  const auto spin = mesh::render_mesh(test::scenes::two_objects(), turntable, ctx);
  REQUIRE_OK(spin);
  REQUIRE(spin->frames.size() == 6);
  CHECK(spin->frames[0].delay_ms > 0);
  CHECK(spin->frames[0].rgba != spin->frames[1].rgba);
  for (const char* view : {"iso", "front", "back", "left", "right", "top", "bottom"}) {
    mesh::RenderOptions o;
    o.size = 32;
    o.view = view;
    const auto p = mesh::render_mesh(test::scenes::two_objects(), o, ctx);
    REQUIRE_OK(p);
    CHECK(coverage(p->frames[0]) > 0.02);
  }
}

TEST_CASE("mesh/render: point clouds, textures, degenerate input and limits") {
  const Context ctx = make_context();
  mesh::RenderOptions options;
  options.size = 64;
  const auto points = mesh::render_mesh(test::scenes::point_grid(), options, ctx);
  REQUIRE_OK(points);
  CHECK(coverage(points->frames[0]) > 0.02);

  // Textured quad: uses the texture when a PNG reader is registered, the
  // material colour otherwise; either way the quad is drawn.
  ir::Mesh quad = test::scenes::read_fixture("glb", "mesh/gltf/quad_blender.glb");
  options.view = "front";
  const auto textured = mesh::render_mesh(quad, options, ctx);
  REQUIRE_OK(textured);
  CHECK(coverage(textured->frames[0]) > 0.2);
  if (Registry::instance().has_reader("png")) {
    // The texture is a 4x4 one-texel checker with a linear magnification
    // filter (as the file asks): the drawn quad shows its pattern as clearly
    // darker and lighter areas, where the material colour alone would be flat.
    const ir::Frame& f = textured->frames[0];
    int darkest = 255, lightest = 0;
    for (std::uint32_t y = 20; y < 44; y += 4) {
      for (std::uint32_t x = 20; x < 44; x += 4) {
        darkest = std::min<int>(darkest, f.rgba[f.offset(x, y)]);
        lightest = std::max<int>(lightest, f.rgba[f.offset(x, y)]);
      }
    }
    CHECK(darkest < 100);
    CHECK(lightest > 200);
  }

  ir::Mesh single;
  single.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
  single.indices = {0, 1, 2};
  CHECK_OK(mesh::render_mesh(single, options, ctx));
  ir::Mesh flat;
  flat.positions.assign(5, ir::Vec3{3, 3, 3});
  flat.indices = {0, 1, 2, 2, 3, 4};
  CHECK_OK(mesh::render_mesh(flat, options, ctx));
  ir::Mesh far = test::scenes::unit_cube();
  mesh::transform(far, mesh::translation(1e7f, -1e7f, 1e7f));
  CHECK_OK(mesh::render_mesh(far, options, ctx));
  Context limited = make_context();
  limited.limits.max_pixels = 1000;
  CHECK_STATUS(mesh::render_mesh(single, options, limited), Status::Limit);
}

TEST_CASE("mesh/bridges: Mesh -> Image through the registry") {
  const Bridge* bridge = bridge_to(Kind::Image);
  REQUIRE(bridge != nullptr);
  Context ctx = make_context();
  ctx.options = *Options::parse(R"({"render_size":40,"turntable_frames":3})")->normalized(bridge->options);
  auto image = bridge->fn(AnyIR(test::scenes::uv_sphere()), ctx);
  REQUIRE_OK(image);
  CHECK(image->get<ir::Image>().frames.size() == 3);
  CHECK(image->get<ir::Image>().width() == 40);
  if (Registry::instance().has_writer("png")) {
    const test::Converted png = test::convert(test::fixture("mesh/gltf/cube_blender.glb"), "glb", "png");
    CHECK(png.ok());
  }
}

TEST_CASE("mesh/bridges: Mesh -> Data summary and Mesh -> Table vertex list") {
  const Bridge* data = bridge_to(Kind::Data);
  REQUIRE(data != nullptr);
  Context ctx = make_context();
  ctx.options = *Options().normalized(data->options);
  auto summary = data->fn(AnyIR(test::scenes::read_fixture("glb", "mesh/gltf/two_objects_blender.glb")), ctx);
  REQUIRE_OK(summary);
  const Json& json = summary->get<ir::Data>().value;
  CHECK(json["triangles"].as_uint64() == 236);
  CHECK(json["objects"].size() == 2);
  CHECK_APPROX(json["bounds"]["size"][0].as_double(), 5.0, 1e-5);
  CHECK(json["materials"][0]["name"].as_string() == "Red");
  CHECK(json["volume"].as_double() > 1.0);

  const Bridge* table = bridge_to(Kind::Table);
  REQUIRE(table != nullptr);
  ctx.options = *Options::parse(R"({"include_faces":true})")->normalized(table->options);
  auto sheets = table->fn(AnyIR(test::scenes::textured_quad()), ctx);
  REQUIRE_OK(sheets);
  const ir::Table& t = sheets->get<ir::Table>();
  REQUIRE(t.sheets.size() == 2);
  CHECK(t.sheets[0].rows.size() == 5);
  CHECK(t.sheets[0].rows[0][0].text == "x");
  CHECK(t.sheets[0].column_count() == 3 + 3 + 2 + 4);
  CHECK(t.sheets[1].rows[1].size() == 4);
  if (Registry::instance().has_writer("json")) {
    CHECK(test::convert(test::fixture("mesh/off/cube_meshlab.off"), "off", "json").ok());
  }
}
