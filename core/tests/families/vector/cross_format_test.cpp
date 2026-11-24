// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// Vector -> vector conversions stay vector: svg -> eps/ps/emf/wmf/dxf/hpgl/
// VectorDrawable -> back, compared by re-rendering, plus the routes the
// planner chooses.
#include <functional>
#include <string>

#include "engine/planner.hpp"
#include "families/vector/vector_api.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

using namespace cvt;

namespace {

Result<ir::Image> render(std::string_view format, ByteView bytes) {
  return test::read_as<ir::Image>(format, bytes, R"({"width":200,"height":150,"background":"ffffff"})");
}

}  // namespace

TEST_CASE("vector/cross: svg through other vector formats and back") {
  const Bytes svg = test::fixture("vector/svg/shapes.svg");
  const auto reference = render("svg", svg);
  REQUIRE_OK(reference);
  const struct {
    const char* format;
    const char* options;
    double min_psnr;
  } cases[] = {
      {"svg", "", 45},  {"svgz", "", 45}, {"eps", "", 30}, {"ps", "", 30},         {"emf", "", 28},
      {"wmf", "", 22},  {"vectordrawable", "", 30},
  };
  for (const auto& c : cases) {
    CAPTURE(c.format);
    const auto out = test::convert(svg, "svg", c.format, c.options);
    INFO(out.message);
    REQUIRE(out.ok());
    // The route never rasterises.
    const auto plan = plan_conversion(Registry::instance(), "svg", c.format);
    REQUIRE_OK(plan);
    CHECK(plan->reader->kind == Kind::Data);
    const auto back = render(c.format, out.data);
    REQUIRE_OK(back);
    CHECK_GT(test::psnr(*reference, *back), c.min_psnr);
  }
  // CAD formats have no page: the drawing keeps its true size (extents).
  const Context ctx = [] {
    Context c;
    c.registry = &Registry::instance();
    return c;
  }();
  const auto extent = [](const vector::Scene& scene) {
    vector::Rect r;
    std::function<void(const vector::Group&, const vector::Matrix&)> walk = [&](const vector::Group& g,
                                                                              const vector::Matrix& m) {
      const vector::Matrix gm = m * g.transform;
      for (const vector::Node& n : g.children) {
        if (const auto* s = std::get_if<vector::Shape>(&n.value)) r.add(vector::bounds(s->path, gm));
        if (const auto* c = std::get_if<vector::Group>(&n.value)) walk(*c, gm);
      }
    };
    walk(scene.pages.at(0).root, {});
    return r;
  };
  const auto source_scene = vector::read_scene("svg", svg, ctx);
  REQUIRE_OK(source_scene);
  for (const char* cad : {"dxf", "hpgl"}) {
    CAPTURE(cad);
    const auto out = test::convert(svg, "svg", cad);
    REQUIRE(out.ok());
    const auto back = vector::read_scene(cad, out.data, ctx);
    REQUIRE_OK(back);
    const vector::Rect a = extent(*source_scene), b = extent(*back);
    CHECK_APPROX(b.width(), a.width(), a.width() * 0.03);
    CHECK_APPROX(b.height(), a.height(), a.height() * 0.03);
  }
  // HP-GL keeps only the outlines: compare the geometry through a re-read instead.
  const auto hpgl = test::convert(svg, "svg", "hpgl");
  REQUIRE(hpgl.ok());
  CHECK(render("hpgl", hpgl.data).has_value());
}

TEST_CASE("vector/cross: routes and targets") {
  // A raster target is rendered; a vector target stays vector.
  const auto to_png = plan_conversion(Registry::instance(), "svg", "png");
  REQUIRE_OK(to_png);
  CHECK(to_png->reader->kind == Kind::Image);
  const auto eps_to_dxf = plan_conversion(Registry::instance(), "eps", "dxf");
  REQUIRE_OK(eps_to_dxf);
  CHECK(eps_to_dxf->reader->kind == Kind::Data);
  CHECK(Registry::instance().validate().empty());
  bool compress = false;
  for (const TargetInfo& t : list_targets(Registry::instance(), "svg")) {
    if (t.format->id == "svg") compress = t.same_format;
  }
  CHECK(compress);
}
