// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/geo/synthetic.hpp"

using namespace cvt;
using namespace cvt::test::geo;

TEST_CASE("geo/geojson: reads the synthetic features written by Python") {
  const auto geo = test::read_as<ir::Geo>("geojson", test::fixture("geo/synthetic_features.geojson"));
  REQUIRE_OK(geo);
  CHECK_MESSAGE(features_diff(synthetic_features(), *geo).empty(), features_diff(synthetic_features(), *geo));
  CHECK(geo->features[0].name == "Alpha Beacon");
}

TEST_CASE("geo/geojson: write and read back is exact") {
  const ir::Geo original = synthetic_features();
  const auto bytes = test::write_from("geojson", original);
  REQUIRE_OK(bytes);
  const auto back = test::read_as<ir::Geo>("geojson", *bytes);
  REQUIRE_OK(back);
  CHECK_MESSAGE(features_diff(original, *back, true, 0.0).empty(), features_diff(original, *back, true, 0.0));
}
