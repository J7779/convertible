// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/geo/synthetic.hpp"

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

TEST_CASE("geo/gml: reads OGR GML 2, 3.1 and 3.2 with their axis orders") {
  for (const char* name : {"geo/gml/features_gml2.gml", "geo/gml/features_gml3.gml", "geo/gml/features_gml32.gml"}) {
    INFO(name);
    const auto geo = test::read_as<ir::Geo>("gml", test::fixture(name));
    REQUIRE_OK(geo);
    const std::string diff = features_diff(synthetic_features(), *geo, true, 1e-12);
    CHECK_MESSAGE(diff.empty(), diff);
    CHECK(geo->features[0].id.is_string());
  }
}

TEST_CASE("geo/gml: a projected GML (UTM 26N) is converted to WGS 84") {
  const auto geo = test::read_as<ir::Geo>("gml", test::fixture("geo/gml/features_gml3_utm.gml"));
  REQUIRE_OK(geo);
  CHECK(geo->crs.empty());
  const std::string diff = features_diff(synthetic_features(), *geo, true, 1e-8);
  CHECK_MESSAGE(diff.empty(), diff);
}

TEST_CASE("geo/gml: write GML 3.2 and read it back, also projected") {
  const auto bytes = test::write_from("gml", synthetic_features());
  REQUIRE_OK(bytes);
  const std::string text = test::str(*bytes);
  CHECK(text.find("srsName=\"urn:ogc:def:crs:EPSG::4326\"") != std::string::npos);
  CHECK(text.find("<gml:pos>40.1234567 -29.4812345</gml:pos>") != std::string::npos);  // latitude first
  const auto back = test::read_as<ir::Geo>("gml", *bytes);
  REQUIRE_OK(back);
  CHECK(features_diff(synthetic_features(), *back, true, 0.0).empty());
  const auto utm = test::write_from("gml", synthetic_features(), R"({"target_crs":"utm"})");
  REQUIRE_OK(utm);
  CHECK(test::str(*utm).find("urn:ogc:def:crs:EPSG::32626") != std::string::npos);
  const auto utm_back = test::read_as<ir::Geo>("gml", *utm);
  REQUIRE_OK(utm_back);
  CHECK(features_diff(synthetic_features(), *utm_back, true, 1e-9).empty());
}

TEST_CASE("geo/gml: curves, surfaces, envelopes, xlinks and forced axis order") {
  const std::string gml = R"(<gml:MultiSurface xmlns:gml="http://www.opengis.net/gml/3.2" xmlns:xlink="http://www.w3.org/1999/xlink" srsName="EPSG:4326">
  <gml:surfaceMember><gml:Surface><gml:patches><gml:PolygonPatch><gml:exterior><gml:LinearRing>
    <gml:posList>0 0 1 0 1 1 0 0</gml:posList></gml:LinearRing></gml:exterior></gml:PolygonPatch></gml:patches></gml:Surface></gml:surfaceMember>
  <gml:surfaceMember xlink:href="#elsewhere"/>
</gml:MultiSurface>)";
  std::vector<std::string> warnings;
  const auto geo = test::read_as<ir::Geo>("gml", test::bytes(gml), "", &warnings);
  REQUIRE_OK(geo);
  const auto* mp = std::get_if<ir::MultiPolygon>(&geo->features[0].geometry.shape);
  REQUIRE(mp != nullptr);
  REQUIRE(mp->polygons.size() == 1);
  REQUIRE(mp->polygons[0].rings.size() == 1);
  REQUIRE(mp->polygons[0].rings[0].size() == 4);
  CHECK(mp->polygons[0].rings[0][1].lon == 1.0);
  CHECK(warnings.size() == 1);
  const std::string point = R"(<gml:Point xmlns:gml="http://www.opengis.net/gml" srsName="EPSG:4326"><gml:pos>10 20</gml:pos></gml:Point>)";
  const auto forced = test::read_as<ir::Geo>("gml", test::bytes(point), R"({"axis_order":"latlon"})");
  REQUIRE_OK(forced);
  CHECK(std::get<ir::Point>(forced->features[0].geometry.shape).coord.lat == 10.0);
  CHECK_STATUS(test::read_as<ir::Geo>("gml", test::bytes(R"(<gml:Point xmlns:gml="http://www.opengis.net/gml"><gml:posList srsDimension="3">1 2</gml:posList></gml:Point>)")),
               Status::Decode);
}

TEST_CASE("geo/gml: detection, conversion and hostile input") {
  CHECK(test::convert(test::fixture("geo/gml/features_gml32.gml"), "", "geojson").ok());
  CHECK(test::convert(test::fixture("geo/gml/features_gml2.gml"), "", "gml").ok());
  std::string deep = R"(<gml:MultiGeometry xmlns:gml="http://www.opengis.net/gml">)";
  for (int i = 0; i < 30; ++i) deep += "<gml:geometryMember><gml:MultiGeometry>";
  for (int i = 0; i < 30; ++i) deep += "</gml:MultiGeometry></gml:geometryMember>";
  deep += "</gml:MultiGeometry>";
  CHECK_STATUS(test::read_as<ir::Geo>("gml", test::bytes(deep)), Status::Limit);
  CHECK(test::fuzz_reader<ir::Geo>("gml", test::fixture("geo/gml/features_gml3.gml")) > 0);
}

TEST_CASE("geo/georss: reads GeoRSS Simple and GeoRSS GML feeds written by OGR") {
  const auto simple = test::read_as<ir::Geo>("georss", test::fixture("geo/gml/features_georss_simple.xml"));
  REQUIRE_OK(simple);
  const ir::Geo expected = subset({0, 1, 3, 2});
  // GeoRSS Simple has no holes: only the outer ring of Delta Field is written.
  ir::Geo outer = expected;
  std::get<ir::Polygon>(outer.features[2].geometry.shape).rings.resize(1);
  CHECK_MESSAGE(features_diff(outer, *simple, false).empty(), features_diff(outer, *simple, false));
  CHECK(simple->features[0].description == "Ünïcødé ✓ 東京");
  CHECK(simple->features[3].name == "Charlie Path");
  // OGR writes GeoRSS GML positions longitude first, against the GeoRSS convention.
  const auto gml = test::read_as<ir::Geo>("georss", test::fixture("geo/gml/features_georss_gml.xml"), R"({"axis_order":"lonlat"})");
  REQUIRE_OK(gml);
  CHECK(features_diff(outer, *gml, false).empty());
  CHECK(test::convert(test::fixture("geo/gml/features_georss_simple.xml"), "", "geojson").format_id == "geojson");
  const std::string atom = R"(<feed xmlns="http://www.w3.org/2005/Atom" xmlns:geo="http://www.w3.org/2003/01/geo/wgs84_pos#">
<entry><title>Kilo</title><link href="http://example.invalid/k"/><geo:lat>1.5</geo:lat><geo:long>2.5</geo:long></entry></feed>)";
  const auto w3c = test::read_as<ir::Geo>("georss", test::bytes(atom));
  REQUIRE_OK(w3c);
  REQUIRE(w3c->features.size() == 1);
  CHECK(std::get<ir::Point>(w3c->features[0].geometry.shape).coord.lon == 2.5);
  CHECK(w3c->features[0].properties["link"] == Json("http://example.invalid/k"));
  CHECK(test::fuzz_reader<ir::Geo>("georss", test::fixture("geo/gml/features_georss_gml.xml")) > 0);
}
