// The bridges from Geo to Data, Table, Document and Image.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/geo/synthetic.hpp"
#include "ir/data.hpp"
#include "ir/document.hpp"
#include "ir/image.hpp"
#include "ir/table.hpp"

using namespace cvt;
using namespace cvt::test::geo;

namespace {

template <class T>
Result<T> bridge(const ir::Geo& geo, std::string_view options_json = {}, std::vector<std::string>* warnings = nullptr) {
  for (const Bridge& b : Registry::instance().bridges()) {
    if (b.from != Kind::Geo || b.to != kind_of<T>) continue;
    CVT_TRY_ASSIGN(const Options options, Options::parse(options_json));
    Context ctx;
    CVT_TRY_ASSIGN(ctx.options, options.normalized(b.options));
    ctx.registry = &Registry::instance();
    auto out = b.fn(AnyIR(ir::Geo(geo)), ctx);
    if (warnings != nullptr) *warnings = ctx.warnings.list();
    if (!out) return std::unexpected(out.error());
    return std::move(*out).template get<T>();
  }
  return fail(Status::UnsupportedConversion, "no bridge");
}

ir::Geo track_geo() {
  ir::Geo geo;
  ir::Feature f;
  f.role = ir::FeatureRole::Track;
  f.name = "Hotel Run";
  f.geometry.shape = synthetic_track();
  geo.features.push_back(f);
  return geo;
}

std::array<std::uint8_t, 4> pixel(const ir::Image& image, std::uint32_t x, std::uint32_t y) {
  const ir::Frame& f = image.frames[0];
  const std::size_t at = f.offset(x, y);
  return {f.rgba[at], f.rgba[at + 1], f.rgba[at + 2], f.rgba[at + 3]};
}

}  // namespace

TEST_CASE("geo/bridges: Geo to Data is the GeoJSON model") {
  const auto data = bridge<ir::Data>(synthetic_features());
  REQUIRE_OK(data);
  CHECK(data->value["type"] == Json("FeatureCollection"));
  REQUIRE(data->value["features"].size() == 7);
  CHECK(data->value["features"][0]["properties"]["label"] == Json("Ünïcødé ✓ 東京"));
  const auto rounded = bridge<ir::Data>(synthetic_features(), R"({"precision":1})");
  REQUIRE_OK(rounded);
  CHECK(rounded->value["features"][0]["geometry"]["coordinates"][0] == Json(-29.5));
}

TEST_CASE("geo/bridges: Geo to Table, one row per feature with typed cells") {
  const auto table = bridge<ir::Table>(synthetic_features());
  REQUIRE_OK(table);
  REQUIRE(table->sheets.size() == 1);
  const ir::Sheet& s = table->sheets[0];
  CHECK(s.has_header);
  REQUIRE(s.rows.size() == 8);
  CHECK(s.rows[0][0].text == "WKT");
  CHECK(s.rows[1][0].text == "POINT (-29.4812345 40.1234567)");
  CHECK(s.rows[0][3].text == "rank");
  CHECK(s.rows[1][3].type == ir::CellType::Number);
  CHECK(s.rows[1][3].number == 3.0);
  CHECK(s.rows[1][5].type == ir::CellType::Bool);
  CHECK(s.rows[4][0].text.starts_with("POLYGON ((-29.5 40, -29.4 40"));
  ir::Geo points;
  points.features = {synthetic_features().features[0], synthetic_features().features[1]};
  const auto latlon = bridge<ir::Table>(points);
  REQUIRE_OK(latlon);
  CHECK(latlon->sheets[0].rows[0][0].text == "latitude");
  CHECK(latlon->sheets[0].rows[1][1].number == -29.4812345);
}

TEST_CASE("geo/bridges: Geo to Table, one row per track point (GPS logs)") {
  const auto table = bridge<ir::Table>(track_geo());
  REQUIRE_OK(table);
  const ir::Sheet& s = table->sheets[0];
  REQUIRE(s.rows.size() == 12);
  std::vector<std::string> header;
  for (const auto& c : s.rows[0]) header.push_back(c.text);
  CHECK(header == std::vector<std::string>{"feature", "segment", "latitude", "longitude", "elevation", "time",
                                           "heart_rate", "cadence", "power", "temperature"});
  CHECK(s.rows[7][1].number == 2.0);
  CHECK(s.rows[7][5].type == ir::CellType::DateTime);
  CHECK(s.rows[7][5].text == "2024-03-10T08:05:00Z");
  CHECK(s.rows[7][5].number == static_cast<double>(track_start_ms + 300'000) / 1000.0);
  CHECK(s.rows[7][6].number == 130.0);
  const auto features = bridge<ir::Table>(track_geo(), R"({"table_rows":"features"})");
  REQUIRE_OK(features);
  CHECK(features->sheets[0].rows.size() == 2);
  const auto no_sensors = bridge<ir::Table>(track_geo(), R"({"keep_sensors":false,"keep_time":false,"table_rows":"points"})");
  REQUIRE_OK(no_sensors);
  CHECK(no_sensors->sheets[0].rows[0].size() == 5);
}

TEST_CASE("geo/bridges: Geo to Document is a report with statistics, a map and the properties") {
  ir::Geo geo = synthetic_features();
  geo.name = "Survey";
  geo.features.push_back(track_geo().features[0]);
  const auto doc = bridge<ir::Document>(geo);
  REQUIRE_OK(doc);
  CHECK(doc->metadata.title == "Survey");
  bool track_heading = false, map = false, distance = false;
  std::size_t tables = 0;
  for (const auto& block : doc->blocks) {
    if (const auto* h = block.get_if<ir::Heading>()) {
      track_heading = track_heading || std::get<ir::Text>(h->content[0].node).text == "Hotel Run";
    }
    if (const auto* img = block.get_if<ir::ImageBlock>()) map = std::holds_alternative<ir::Image>(img->source);
    if (const auto* t = block.get_if<ir::TableBlock>()) {
      ++tables;
      for (const auto& row : t->rows) {
        const auto& first = std::get<ir::Text>(row.cells[0].content[0].node).text;
        if (first == "Duration") distance = std::get<ir::Text>(row.cells[1].content[0].node).text == "0:05:40";
      }
    }
  }
  CHECK(track_heading);
  CHECK(map);
  CHECK(distance);
  CHECK(tables == 3);
  const auto plain = bridge<ir::Document>(geo, R"({"include_map":false})");
  REQUIRE_OK(plain);
  CHECK(plain->blocks.size() < doc->blocks.size());
}

TEST_CASE("geo/bridges: Geo to Image renders an anti-aliased sketch") {
  const auto image = bridge<ir::Image>(synthetic_features(), R"({"map_width":400})");
  REQUIRE_OK(image);
  REQUIRE(image->frames.size() == 1);
  CHECK(image->width() == 400);
  const std::uint32_t h = image->height();
  CHECK(h >= 64);
  CHECK(pixel(*image, 0, 0) == std::array<std::uint8_t, 4>{247, 246, 242, 255});
  // Count the kinds of pixels: background, red points, blue strokes, tinted fills, and edge blends.
  std::size_t red = 0, blue = 0, blended = 0;
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < 400; ++x) {
      const auto p = pixel(*image, x, y);
      if (p[0] > 180 && p[1] < 80) ++red;
      if (p[2] > 150 && p[0] < 80) ++blue;
      if (p[0] < 240 && p[0] > 60 && p[2] > p[0]) ++blended;
    }
  }
  CHECK(red > 50);
  CHECK(blue > 200);
  CHECK(blended > 200);
  // Options: size, colours, no graticule; bad colours are invalid arguments.
  const auto custom = bridge<ir::Image>(synthetic_features(),
                                        R"({"map_width":200,"map_height":100,"map_background":"#000000","map_graticule":false})");
  REQUIRE_OK(custom);
  CHECK(custom->height() == 100);
  CHECK(pixel(*custom, 0, 0) == std::array<std::uint8_t, 4>{0, 0, 0, 255});
  CHECK_STATUS(bridge<ir::Image>(synthetic_features(), R"({"map_background":"blue"})"), Status::InvalidArgument);
  // A single point and an empty collection still render.
  ir::Geo one;
  one.features.push_back(synthetic_features().features[0]);
  CHECK_OK(bridge<ir::Image>(one));
  CHECK_OK(bridge<ir::Image>(ir::Geo{}));
  // Feature styles win over the defaults.
  ir::Geo styled;
  styled.features.push_back(synthetic_features().features[3]);
  styled.features[0].style.fill = "#00ff00";
  styled.features[0].style.fill_opacity = 1.0;
  const auto green = bridge<ir::Image>(styled, R"({"map_width":100,"map_graticule":false})");
  REQUIRE_OK(green);
  const auto centre = pixel(*green, 15, green->height() / 2);  // inside the outer ring, left of the hole
  CHECK(centre[1] > centre[0]);
}

TEST_CASE("geo/bridges: conversions through the engine reach other kinds when their writers exist") {
  // Writers of other families are registered only after the merge; skip when absent.
  for (const char* target : {"csv", "json", "html", "png"}) {
    if (!Registry::instance().has_writer(target)) continue;
    INFO(target);
    CHECK(test::convert(test::fixture("geo/synthetic_track.gpx"), "", target).ok());
  }
}
