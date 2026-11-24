// Tests of the JSON dialects: GeoJSON (GDAL fixtures, sequences, foreign
// members), TopoJSON, Esri JSON and Google location history.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/geo/synthetic.hpp"
#include "util/json.hpp"

using namespace cvt;
using namespace cvt::test::geo;

namespace {

ir::Geo subset(std::initializer_list<int> indices) {
  const ir::Geo all = synthetic_features();
  ir::Geo out;
  for (int i : indices) out.features.push_back(all.features[static_cast<std::size_t>(i)]);
  return out;
}

}  // namespace

// --------------------------------------------------------------------------- GeoJSON

TEST_CASE("geo/geojson: reads GDAL output, plain, sequences and RFC 8142") {
  for (const auto& [format, name] : {std::pair{"geojson", "geo/json/features_gdal.geojson"},
                                     std::pair{"geojsonseq", "geo/json/features_gdal.geojsonl"},
                                     std::pair{"geojsonseq", "geo/json/features_gdal_rs.geojsons"}}) {
    INFO(name);
    const auto geo = test::read_as<ir::Geo>(format, test::fixture(name));
    REQUIRE_OK(geo);
    const std::string diff = features_diff(synthetic_features(), *geo, true, 0.0);
    CHECK_MESSAGE(diff.empty(), diff);
    // Detection tells the three apart.
    CHECK(test::convert(test::fixture(name), "", format).format_id == format);
  }
}

TEST_CASE("geo/geojson: projected input from GDAL (legacy crs member) is converted to WGS 84") {
  const auto geo = test::read_as<ir::Geo>("geojson", test::fixture("geo/json/features_3857_gdal.geojson"));
  REQUIRE_OK(geo);
  CHECK(geo->crs.empty());
  const std::string diff = features_diff(synthetic_features(), *geo, true, 1e-9);
  CHECK_MESSAGE(diff.empty(), diff);
}

TEST_CASE("geo/geojson: ids, bbox, foreign members, collections and null geometry") {
  const auto geo = test::read_as<ir::Geo>("geojson", test::fixture("geo/synthetic_rich.geojson"));
  REQUIRE_OK(geo);
  REQUIRE(geo->features.size() == 4);
  CHECK(geo->name == "rich sample");
  CHECK(geo->extra["x-producer-note"]["level"] == Json(2));
  CHECK(geo->features[0].id == Json("alpha"));
  CHECK(geo->features[0].extra["x-extra"].size() == 3);
  CHECK(geo->features[1].id == Json(42));
  const auto& gc = std::get<ir::GeometryCollection>(geo->features[2].geometry.shape);
  REQUIRE(gc.geometries.size() == 2);
  CHECK(std::get<ir::Point>(gc.geometries[0].shape).coord.ele == 15.5);
  CHECK(geo->features[2].properties["nested"]["a"].size() == 2);
  CHECK(std::get<ir::GeometryCollection>(geo->features[3].geometry.shape).geometries.empty());
  // Written back: foreign members, ids and the null geometry survive.
  const auto bytes = test::write_from("geojson", *geo, R"({"bbox":true})");
  REQUIRE_OK(bytes);
  const auto doc = util::parse_json(test::str(*bytes));
  REQUIRE_OK(doc);
  CHECK((*doc)["x-producer-note"]["purpose"] == Json("foreign member test"));
  CHECK((*doc)["features"][0]["x-extra"][2] == Json(3));
  CHECK((*doc)["features"][1]["id"] == Json(42));
  CHECK((*doc)["features"][3]["geometry"].is_null());
  CHECK((*doc)["bbox"].size() == 4);
}

TEST_CASE("geo/geojson: RFC 7946 orientation and antimeridian splitting on write") {
  ir::Geo geo;
  ir::Feature f;
  // A clockwise square crossing the antimeridian.
  f.geometry.shape = ir::Polygon{{closed({ll(179, 10), ll(179, 0), ll(-179, 0), ll(-179, 10)})}};
  geo.features.push_back(f);
  ir::Feature line;
  line.geometry.shape = ir::LineString{{ll(170, 0), ll(-170, 10)}};
  geo.features.push_back(line);
  const auto bytes = test::write_from("geojson", geo);
  REQUIRE_OK(bytes);
  const auto back = test::read_as<ir::Geo>("geojson", *bytes);
  REQUIRE_OK(back);
  const auto& mp = std::get<ir::MultiPolygon>(back->features[0].geometry.shape);
  REQUIRE(mp.polygons.size() == 2);
  for (const auto& p : mp.polygons) {
    CHECK(cvt::geo::signed_area(p.rings[0]) > 0);  // counter-clockwise
    for (const auto& c : p.rings[0]) CHECK(std::fabs(c.lon) >= 179.0);
  }
  const auto& ml = std::get<ir::MultiLineString>(back->features[1].geometry.shape);
  REQUIRE(ml.lines.size() == 2);
  CHECK(ml.lines[0].points.back().lon == 180.0);
  CHECK(ml.lines[1].points.front().lon == -180.0);
  CHECK_APPROX(ml.lines[0].points.back().lat, 5.0, 1e-9);
  // Without the RFC rules the geometry stays as it is.
  const auto raw = test::read_as<ir::Geo>("geojson", *test::write_from("geojson", geo, R"({"rfc7946":false})"));
  REQUIRE_OK(raw);
  CHECK(std::holds_alternative<ir::Polygon>(raw->features[0].geometry.shape));
}

TEST_CASE("geo/geojson: precision, pretty printing, sequences and tracks with coordinate properties") {
  const auto rounded = test::write_from("geojson", synthetic_features(), R"({"precision":2})");
  REQUIRE_OK(rounded);
  CHECK(test::str(*rounded).find("-29.48,40.12") != std::string::npos);
  const auto pretty = test::write_from("geojson", synthetic_features(), R"({"pretty":true})");
  REQUIRE_OK(pretty);
  CHECK(pretty->size() > rounded->size());
  ir::Geo track;
  ir::Feature f;
  f.role = ir::FeatureRole::Track;
  f.geometry.shape = synthetic_track();
  track.features.push_back(f);
  const auto bytes = test::write_from("geojson", track);
  REQUIRE_OK(bytes);
  const auto back = test::read_as<ir::Geo>("geojson", *bytes);
  REQUIRE_OK(back);
  CHECK(back->features[0].role == ir::FeatureRole::Track);
  CHECK(std::get<ir::MultiLineString>(back->features[0].geometry.shape).lines[1].points[3] == track_point(1, 3));
  const auto seq = test::write_from("geojsonseq", synthetic_features(), R"({"record_separator":true})");
  REQUIRE_OK(seq);
  CHECK(test::str(*seq).starts_with("\x1E{"));
  const auto seq_back = test::read_as<ir::Geo>("geojsonseq", *seq);
  REQUIRE_OK(seq_back);
  CHECK(features_diff(synthetic_features(), *seq_back, true, 0.0).empty());
}

TEST_CASE("geo/geojson: hostile and damaged input") {
  CHECK_STATUS(test::read_as<ir::Geo>("geojson", test::bytes("{\"type\":\"Point\",\"coordinates\":[1]}")), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Geo>("geojson", test::bytes("{\"type\":\"Blob\"}")), Status::Decode);
  std::string deep = "{\"type\":\"GeometryCollection\",\"geometries\":[";
  std::string tail = "]}";
  for (int i = 0; i < 30; ++i) {
    deep += "{\"type\":\"GeometryCollection\",\"geometries\":[";
    tail += "]}";
  }
  CHECK_STATUS(test::read_as<ir::Geo>("geojson", test::bytes(deep + tail)), Status::Limit);
  CHECK(test::fuzz_reader<ir::Geo>("geojson", test::fixture("geo/synthetic_rich.geojson")) > 0);
  CHECK(test::fuzz_reader<ir::Geo>("geojsonseq", test::fixture("geo/json/features_gdal.geojsonl")) > 0);
}

// --------------------------------------------------------------------------- TopoJSON

TEST_CASE("geo/topojson: reads topojson-server output, quantised and plain") {
  const auto plain = test::read_as<ir::Geo>("topojson", test::fixture("geo/topojson/features_plain.topojson"));
  REQUIRE_OK(plain);
  CHECK_MESSAGE(features_diff(synthetic_features(), *plain, true, 1e-12).empty(),
                features_diff(synthetic_features(), *plain, true, 1e-12));
  const auto q = test::read_as<ir::Geo>("topojson", test::fixture("geo/topojson/features_q1e5.topojson"));
  REQUIRE_OK(q);
  // 1e5 positions over 0.4 x 0.31 degrees: steps of about 4e-6 degrees.
  CHECK_MESSAGE(features_diff(synthetic_features(), *q, true, 4e-6).empty(), features_diff(synthetic_features(), *q, true, 4e-6));
  CHECK(q->name == "features");
}

TEST_CASE("geo/topojson: write and read back, quantised and exact, with shared arcs") {
  ir::Geo geo = synthetic_features();
  ir::Feature twin = geo.features[3];  // the same polygon again: its rings must reuse the arcs
  twin.name = "Delta Twin";
  twin.properties.set("name", "Delta Twin");
  geo.features.push_back(twin);
  const auto exact = test::write_from("topojson", geo, R"({"quantization":0})");
  REQUIRE_OK(exact);
  const auto doc = util::parse_json(test::str(*exact));
  REQUIRE_OK(doc);
  CHECK((*doc)["arcs"].size() == 1 + 2 + 3 + 2);  // line, polygon rings, multipolygon rings, multiline lines
  const auto back = test::read_as<ir::Geo>("topojson", *exact);
  REQUIRE_OK(back);
  CHECK(features_diff(geo, *back, true, 0.0).empty());
  const auto quantised = test::write_from("topojson", geo);
  REQUIRE_OK(quantised);
  const auto qback = test::read_as<ir::Geo>("topojson", *quantised);
  REQUIRE_OK(qback);
  CHECK(features_diff(geo, *qback, true, 1e-6).empty());
  const auto coarse = test::write_from("topojson", geo, R"({"quantization":1000})");
  REQUIRE_OK(coarse);
  CHECK(coarse->size() < quantised->size());
}

TEST_CASE("geo/topojson: bad arcs fail cleanly") {
  CHECK_STATUS(test::read_as<ir::Geo>("topojson", test::bytes(
                   R"({"type":"Topology","objects":{"a":{"type":"LineString","arcs":[5]}},"arcs":[[[0,0],[1,1]]]})")),
               Status::Decode);
  CHECK_STATUS(test::read_as<ir::Geo>("topojson", test::bytes(
                   R"({"type":"Topology","objects":{"a":{"type":"LineString","arcs":[-9223372036854775807]}},"arcs":[]})")),
               Status::Decode);
  CHECK(test::fuzz_reader<ir::Geo>("topojson", test::fixture("geo/topojson/features_q1e5.topojson")) > 0);
}

// --------------------------------------------------------------------------- Esri JSON

TEST_CASE("geo/esrijson: reads feature sets with rings and points") {
  const auto polys = test::read_as<ir::Geo>("esrijson", test::fixture("geo/esrijson/features.esrijson"));
  REQUIRE_OK(polys);
  const ir::Geo expected = subset({3, 4});
  CHECK_MESSAGE(features_diff(expected, *polys, false, 0.0).empty(), features_diff(expected, *polys, false, 0.0));
  CHECK(polys->features[1].properties["label"].as_string() == "two islands");
  const auto points = test::read_as<ir::Geo>("esrijson", test::fixture("geo/esrijson/features_points.esrijson"));
  REQUIRE_OK(points);
  CHECK(features_diff(subset({0, 1}), *points, false, 0.0).empty());
  CHECK(points->features[0].properties["label"].as_string() == "Ünïcødé ✓ 東京");
}

TEST_CASE("geo/esrijson: write and read back, Web Mercator output") {
  const auto bytes = test::write_from("esrijson", subset({3, 4}));
  REQUIRE_OK(bytes);
  const auto back = test::read_as<ir::Geo>("esrijson", *bytes);
  REQUIRE_OK(back);
  ir::Geo expected = subset({3, 4});
  for (auto& f : expected.features) f.properties.set("active", f.properties["active"].as_bool() ? 1 : 0);  // no booleans
  CHECK(features_diff(expected, *back, true, 0.0).empty());
  const auto mercator = test::write_from("esrijson", subset({0, 2}), R"({"target_crs":"EPSG:3857"})");
  REQUIRE_OK(mercator);
  CHECK(test::str(*mercator).find("\"wkid\":3857") != std::string::npos);
  const auto mback = test::read_as<ir::Geo>("esrijson", *mercator);
  REQUIRE_OK(mback);
  CHECK(features_diff(subset({0, 2}), *mback, false, 1e-9).empty());
  CHECK(test::convert(test::fixture("geo/esrijson/features.esrijson"), "", "geojson").ok());
  CHECK(test::fuzz_reader<ir::Geo>("esrijson", test::fixture("geo/esrijson/features.esrijson")) > 0);
}

// --------------------------------------------------------------------------- Takeout

TEST_CASE("geo/takeout: reads the three location history layouts") {
  const auto records = test::read_as<ir::Geo>("takeout-location", test::fixture("geo/takeout/records.json"));
  REQUIRE_OK(records);
  REQUIRE(records->features.size() == 1);
  const auto& pts = std::get<ir::MultiLineString>(records->features[0].geometry.shape).lines[0].points;
  REQUIRE(pts.size() == 11);
  CHECK(pts[0].time_ms == track_start_ms);
  CHECK(pts[1].time_ms == track_start_ms + 10'000);  // timestampMs variant
  CHECK_APPROX(pts[6].lon, track_point(1, 0).lon, 1e-7);
  const auto semantic = test::read_as<ir::Geo>("takeout-location", test::fixture("geo/takeout/semantic.json"));
  REQUIRE_OK(semantic);
  REQUIRE(semantic->features.size() == 2);
  CHECK(semantic->features[0].name == "India Camp");
  CHECK(semantic->features[1].properties["activity"].as_string() == "RUNNING");
  CHECK(std::get<ir::MultiLineString>(semantic->features[1].geometry.shape).lines[0].points.size() == 6);
  const auto timeline = test::read_as<ir::Geo>("takeout-location", test::fixture("geo/takeout/timeline.json"));
  REQUIRE_OK(timeline);
  REQUIRE(timeline->features.size() == 2);
  const auto& path = std::get<ir::MultiLineString>(timeline->features[1].geometry.shape).lines[0].points;
  REQUIRE(path.size() == 11);
  CHECK(path[10].time_ms == track_point(1, 4).time_ms);
  CHECK(test::convert(test::fixture("geo/takeout/records.json"), "", "gpx", "", "Records.json").ok());
  CHECK(test::fuzz_reader<ir::Geo>("takeout-location", test::fixture("geo/takeout/semantic.json")) > 0);
}
