// Convertible core - geo family: coordinate operations (see geo_ops.hpp).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/geo/geo_ops.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "families/geo/geo.hpp"
#include "util/text.hpp"

namespace cvt::geo {

namespace {

constexpr double pi = std::numbers::pi;
constexpr double deg = pi / 180.0;

[[nodiscard]] double rad(double degrees) { return degrees * deg; }
[[nodiscard]] double degs(double radians) { return radians / deg; }

// --------------------------------------------------------------------------- transverse Mercator (Krueger, 6th order)

struct Ellipsoid {
  double a;
  double f;
};
constexpr Ellipsoid wgs84{wgs84_a, wgs84_f};
constexpr Ellipsoid grs80{6378137.0, 1.0 / 298.257222101};
constexpr Ellipsoid airy1830{6377563.396, 1.0 - 6356256.909 / 6377563.396};

struct TmParams {
  Ellipsoid ellipsoid;
  double k0;
  double lat0;  // degrees
  double lon0;  // degrees
  double false_easting;
  double false_northing;
};

struct Krueger {
  double e = 0.0;
  double big_a = 0.0;
  std::array<double, 7> alpha{};
  std::array<double, 7> beta{};

  explicit Krueger(const Ellipsoid& el) {
    const double f = el.f;
    e = std::sqrt(f * (2 - f));
    const double n = f / (2 - f);
    const double n2 = n * n, n3 = n2 * n, n4 = n3 * n, n5 = n4 * n, n6 = n5 * n;
    big_a = el.a / (1 + n) * (1 + n2 / 4 + n4 / 64 + n6 / 256);
    alpha = {0,
             1.0 / 2 * n - 2.0 / 3 * n2 + 5.0 / 16 * n3 + 41.0 / 180 * n4 - 127.0 / 288 * n5 + 7891.0 / 37800 * n6,
             13.0 / 48 * n2 - 3.0 / 5 * n3 + 557.0 / 1440 * n4 + 281.0 / 630 * n5 - 1983433.0 / 1935360 * n6,
             61.0 / 240 * n3 - 103.0 / 140 * n4 + 15061.0 / 26880 * n5 + 167603.0 / 181440 * n6,
             49561.0 / 161280 * n4 - 179.0 / 168 * n5 + 6601661.0 / 7257600 * n6,
             34729.0 / 80640 * n5 - 3418889.0 / 1995840 * n6,
             212378941.0 / 319334400 * n6};
    beta = {0,
            1.0 / 2 * n - 2.0 / 3 * n2 + 37.0 / 96 * n3 - 1.0 / 360 * n4 - 81.0 / 512 * n5 + 96199.0 / 604800 * n6,
            1.0 / 48 * n2 + 1.0 / 15 * n3 - 437.0 / 1440 * n4 + 46.0 / 105 * n5 - 1118711.0 / 3870720 * n6,
            17.0 / 480 * n3 - 37.0 / 840 * n4 - 209.0 / 4480 * n5 + 5569.0 / 90720 * n6,
            4397.0 / 161280 * n4 - 11.0 / 504 * n5 - 830251.0 / 7257600 * n6,
            4583.0 / 161280 * n5 - 108847.0 / 3991680 * n6,
            20648693.0 / 638668800 * n6};
  }

  // Conformal latitude helper: tau' from tau.
  [[nodiscard]] double tau_prime(double tau) const {
    const double sigma = std::sinh(e * std::atanh(e * tau / std::sqrt(1 + tau * tau)));
    return tau * std::sqrt(1 + sigma * sigma) - sigma * std::sqrt(1 + tau * tau);
  }

  // (xi, eta) of a position with longitude relative to the central meridian.
  void forward(double phi, double lambda, double& xi, double& eta) const {
    const double tau = std::tan(phi);
    const double tp = tau_prime(tau);
    const double xi_p = std::atan2(tp, std::cos(lambda));
    const double eta_p = std::asinh(std::sin(lambda) / std::sqrt(tp * tp + std::cos(lambda) * std::cos(lambda)));
    xi = xi_p;
    eta = eta_p;
    for (int j = 1; j <= 6; ++j) {
      xi += alpha[static_cast<std::size_t>(j)] * std::sin(2 * j * xi_p) * std::cosh(2 * j * eta_p);
      eta += alpha[static_cast<std::size_t>(j)] * std::cos(2 * j * xi_p) * std::sinh(2 * j * eta_p);
    }
  }

  void inverse(double xi, double eta, double& phi, double& lambda) const {
    double xi_p = xi, eta_p = eta;
    for (int j = 1; j <= 6; ++j) {
      xi_p -= beta[static_cast<std::size_t>(j)] * std::sin(2 * j * xi) * std::cosh(2 * j * eta);
      eta_p -= beta[static_cast<std::size_t>(j)] * std::cos(2 * j * xi) * std::sinh(2 * j * eta);
    }
    const double sinh_eta = std::sinh(eta_p);
    const double sin_xi = std::sin(xi_p);
    const double cos_xi = std::cos(xi_p);
    const double tp = sin_xi / std::sqrt(sinh_eta * sinh_eta + cos_xi * cos_xi);
    double tau = tp;
    const double e2 = e * e;
    for (int iteration = 0; iteration < 20; ++iteration) {
      const double tpi = tau_prime(tau);
      const double delta = (tp - tpi) / std::sqrt(1 + tpi * tpi) * (1 + (1 - e2) * tau * tau) /
                           ((1 - e2) * std::sqrt(1 + tau * tau));
      tau += delta;
      if (std::fabs(delta) < 1e-13) break;
    }
    phi = std::atan(tau);
    lambda = std::atan2(sinh_eta, cos_xi);
  }
};

XY tm_forward(const TmParams& p, double lon, double lat) {
  const Krueger k(p.ellipsoid);
  double xi = 0, eta = 0;
  k.forward(rad(lat), rad(lon - p.lon0), xi, eta);
  double xi0 = 0, eta0 = 0;
  if (p.lat0 != 0.0) k.forward(rad(p.lat0), 0.0, xi0, eta0);
  return {p.false_easting + p.k0 * k.big_a * eta, p.false_northing + p.k0 * k.big_a * (xi - xi0)};
}

XY tm_inverse(const TmParams& p, double x, double y) {
  const Krueger k(p.ellipsoid);
  double xi0 = 0, eta0 = 0;
  if (p.lat0 != 0.0) k.forward(rad(p.lat0), 0.0, xi0, eta0);
  const double eta = (x - p.false_easting) / (p.k0 * k.big_a);
  const double xi = (y - p.false_northing) / (p.k0 * k.big_a) + xi0;
  double phi = 0, lambda = 0;
  k.inverse(xi, eta, phi, lambda);
  return {degs(lambda) + p.lon0, degs(phi)};
}

TmParams utm_params(const Crs& crs) {
  return TmParams{crs.datum == Datum::Wgs84 ? wgs84 : grs80, 0.9996, 0.0, -183.0 + 6.0 * crs.zone, 500000.0,
                  crs.north ? 0.0 : 10000000.0};
}

constexpr TmParams bng_params{airy1830, 0.9996012717, 49.0, -2.0, 400000.0, -100000.0};

// --------------------------------------------------------------------------- datum shift for OSGB36

struct Xyz {
  double x, y, z;
};

Xyz to_geocentric(const Ellipsoid& el, double lon, double lat, double h) {
  const double e2 = el.f * (2 - el.f);
  const double phi = rad(lat), lambda = rad(lon);
  const double n = el.a / std::sqrt(1 - e2 * std::sin(phi) * std::sin(phi));
  return {(n + h) * std::cos(phi) * std::cos(lambda), (n + h) * std::cos(phi) * std::sin(lambda),
          (n * (1 - e2) + h) * std::sin(phi)};
}

void from_geocentric(const Ellipsoid& el, const Xyz& p, double& lon, double& lat) {
  const double e2 = el.f * (2 - el.f);
  const double r = std::sqrt(p.x * p.x + p.y * p.y);
  double phi = std::atan2(p.z, r * (1 - e2));
  for (int i = 0; i < 10; ++i) {
    const double n = el.a / std::sqrt(1 - e2 * std::sin(phi) * std::sin(phi));
    const double next = std::atan2(p.z + e2 * n * std::sin(phi), r);
    if (std::fabs(next - phi) < 1e-14) {
      phi = next;
      break;
    }
    phi = next;
  }
  lat = degs(phi);
  lon = degs(std::atan2(p.y, p.x));
}

// Helmert transformation (position-vector convention, OS "A guide to coordinate systems in Great Britain").
Xyz helmert(const Xyz& p, double tx, double ty, double tz, double s_ppm, double rx_sec, double ry_sec, double rz_sec) {
  const double s = s_ppm * 1e-6;
  const double rx = rad(rx_sec / 3600.0), ry = rad(ry_sec / 3600.0), rz = rad(rz_sec / 3600.0);
  return {tx + (1 + s) * p.x - rz * p.y + ry * p.z, ty + rz * p.x + (1 + s) * p.y - rx * p.z,
          tz - ry * p.x + rx * p.y + (1 + s) * p.z};
}

void wgs84_to_osgb36(double& lon, double& lat) {
  const Xyz g = to_geocentric(wgs84, lon, lat, 0.0);
  const Xyz o = helmert(g, -446.448, 125.157, -542.060, 20.4894, -0.1502, -0.2470, -0.8421);
  from_geocentric(airy1830, o, lon, lat);
}

void osgb36_to_wgs84(double& lon, double& lat) {
  const Xyz o = to_geocentric(airy1830, lon, lat, 0.0);
  const Xyz g = helmert(o, 446.448, -125.157, 542.060, -20.4894, 0.1502, 0.2470, 0.8421);
  from_geocentric(wgs84, g, lon, lat);
}

// --------------------------------------------------------------------------- CRS text

[[nodiscard]] bool contains_ci(std::string_view text, std::string_view needle) {
  if (needle.size() > text.size()) return false;
  for (std::size_t i = 0; i + needle.size() <= text.size(); ++i) {
    if (util::iequals(text.substr(i, needle.size()), needle)) return true;
  }
  return false;
}

std::optional<Crs> crs_from_epsg(int code) {
  Crs crs;
  crs.epsg = code;
  switch (code) {
    case 4326:
    case 4258:  // ETRS89
    case 4269:  // NAD83
    case 4979:
    case 4937:
      crs.kind = CrsKind::Geographic;
      crs.epsg = 4326;
      return crs;
    case 3857:
    case 900913:
    case 3785:
    case 102100:
    case 102113:
    case 3587:
      crs.kind = CrsKind::WebMercator;
      crs.epsg = 3857;
      return crs;
    case 27700:
      crs.kind = CrsKind::BritishNationalGrid;
      return crs;
    default: break;
  }
  if (code >= 32601 && code <= 32660) {
    crs.kind = CrsKind::Utm;
    crs.zone = code - 32600;
    crs.north = true;
    return crs;
  }
  if (code >= 32701 && code <= 32760) {
    crs.kind = CrsKind::Utm;
    crs.zone = code - 32700;
    crs.north = false;
    return crs;
  }
  if (code >= 26901 && code <= 26923) {
    crs.kind = CrsKind::Utm;
    crs.datum = Datum::Nad83;
    crs.zone = code - 26900;
    return crs;
  }
  if (code >= 25828 && code <= 25838) {
    crs.kind = CrsKind::Utm;
    crs.datum = Datum::Etrs89;
    crs.zone = code - 25800;
    return crs;
  }
  return std::nullopt;
}

// Parses the number after `key` in WKT such as PARAMETER["central_meridian",-3].
std::optional<double> wkt_parameter(std::string_view wkt, std::string_view name) {
  std::size_t at = 0;
  while (true) {
    std::size_t found = std::string_view::npos;
    for (std::size_t i = at; i + name.size() <= wkt.size(); ++i) {
      if (util::iequals(wkt.substr(i, name.size()), name)) {
        found = i;
        break;
      }
    }
    if (found == std::string_view::npos) return std::nullopt;
    std::size_t pos = found + name.size();
    while (pos < wkt.size() && (wkt[pos] == '"' || wkt[pos] == ',' || wkt[pos] == ' ')) ++pos;
    std::size_t end = pos;
    while (end < wkt.size() && (util::is_ascii_digit(wkt[end]) || wkt[end] == '.' || wkt[end] == '-' ||
                                wkt[end] == '+' || wkt[end] == 'e' || wkt[end] == 'E')) {
      ++end;
    }
    if (end > pos) {
      if (auto v = util::parse_double(wkt.substr(pos, end - pos))) return v;
    }
    at = found + 1;
  }
}

// The last AUTHORITY["EPSG","nnnn"] / ID["EPSG",nnnn] at the outermost level.
std::optional<int> wkt_authority(std::string_view wkt) {
  // The outermost authority is the last one in the text for WKT1 and WKT2.
  std::optional<int> last;
  for (std::size_t i = 0; i + 6 < wkt.size(); ++i) {
    if (wkt[i] != '"' && wkt[i] != '\'') continue;
    if (!util::iequals(wkt.substr(i + 1, 4), "EPSG")) continue;
    std::size_t pos = i + 5;
    while (pos < wkt.size() && (wkt[pos] == '"' || wkt[pos] == '\'' || wkt[pos] == ',' || wkt[pos] == ' ')) ++pos;
    std::size_t end = pos;
    while (end < wkt.size() && util::is_ascii_digit(wkt[end])) ++end;
    if (end > pos && end - pos <= 7) {
      if (auto v = util::parse_int(wkt.substr(pos, end - pos))) last = static_cast<int>(*v);
    }
  }
  // Only trust it when it sits in the outermost element: check depth.
  return last;
}

std::optional<Crs> crs_from_wkt(std::string_view wkt) {
  const std::string_view head = util::trim(wkt);
  const bool projected = util::istarts_with(head, "PROJCS") || util::istarts_with(head, "PROJCRS") ||
                         util::istarts_with(head, "PROJECTEDCRS");
  const bool geographic = util::istarts_with(head, "GEOGCS") || util::istarts_with(head, "GEOGCRS") ||
                          util::istarts_with(head, "GEODCRS") || util::istarts_with(head, "GEOGRAPHICCRS");
  if (!projected && !geographic) return std::nullopt;
  // An outermost EPSG authority decides when present (the last one in the text).
  if (auto code = wkt_authority(head)) {
    // WKT1 puts the outermost AUTHORITY last; a GEOGCS nested in a PROJCS
    // has its own earlier, so the last one is the right one.
    if (auto crs = crs_from_epsg(*code)) {
      if (projected == (crs->kind != CrsKind::Geographic)) return crs;
    }
  }
  if (geographic) {
    if (contains_ci(head, "WGS") || contains_ci(head, "84") || contains_ci(head, "ETRS") ||
        contains_ci(head, "NAD83") || contains_ci(head, "North_American_1983") || contains_ci(head, "GRS")) {
      return crs_from_epsg(4326);
    }
    return std::nullopt;
  }
  // Projected without a usable authority: recognise by name and parameters.
  if (contains_ci(head, "Mercator_Auxiliary_Sphere") || contains_ci(head, "Pseudo-Mercator") ||
      contains_ci(head, "Pseudo_Mercator") || contains_ci(head, "Popular Visualisation")) {
    return crs_from_epsg(3857);
  }
  if (contains_ci(head, "British_National_Grid") || contains_ci(head, "British National Grid")) {
    return crs_from_epsg(27700);
  }
  if (contains_ci(head, "Transverse_Mercator") || contains_ci(head, "Transverse Mercator")) {
    const auto cm = wkt_parameter(head, "central_meridian");
    const auto k = wkt_parameter(head, "scale_factor");
    const auto fe = wkt_parameter(head, "false_easting");
    const auto fn = wkt_parameter(head, "false_northing");
    const auto lat0 = wkt_parameter(head, "latitude_of_origin");
    if (!cm || !k || !fe || !fn) return std::nullopt;
    if (std::fabs(*k - 0.9996) < 1e-9 && std::fabs(*fe - 500000) < 1e-6 && (!lat0 || *lat0 == 0.0)) {
      const double zone_f = (*cm + 183.0) / 6.0;
      const int zone = static_cast<int>(std::lround(zone_f));
      if (std::fabs(zone_f - zone) > 1e-9 || zone < 1 || zone > 60) return std::nullopt;
      const bool north = std::fabs(*fn) < 1e-6;
      if (!north && std::fabs(*fn - 10000000) > 1e-6) return std::nullopt;
      Crs crs;
      crs.kind = CrsKind::Utm;
      crs.zone = zone;
      crs.north = north;
      if (contains_ci(head, "ETRS")) {
        crs.datum = Datum::Etrs89;
        crs.epsg = 25800 + zone;
      } else if (contains_ci(head, "NAD83") || contains_ci(head, "North_American_1983")) {
        crs.datum = Datum::Nad83;
        crs.epsg = 26900 + zone;
      } else if (contains_ci(head, "WGS")) {
        crs.epsg = (north ? 32600 : 32700) + zone;
      } else {
        return std::nullopt;
      }
      if ((crs.datum != Datum::Wgs84) && !north) return std::nullopt;
      return crs;
    }
    if (std::fabs(*k - 0.9996012717) < 1e-9 && std::fabs(*fe - 400000) < 1e-6 && std::fabs(*fn + 100000) < 1e-6 &&
        std::fabs(*cm + 2) < 1e-9 && contains_ci(head, "Airy")) {
      return crs_from_epsg(27700);
    }
  }
  return std::nullopt;
}

}  // namespace

// --------------------------------------------------------------------------- CRS API

std::optional<int> epsg_code(std::string_view text) {
  std::string_view t = util::trim(text);
  std::string_view digits;
  if (util::istarts_with(t, "EPSG:")) {
    digits = t.substr(5);
    if (!digits.empty() && digits.front() == ':') digits.remove_prefix(1);  // "EPSG::4326"
  } else if (util::istarts_with(t, "urn:ogc:def:crs:EPSG:") || util::istarts_with(t, "urn:x-ogc:def:crs:EPSG:")) {
    const std::size_t colon = t.rfind(':');
    digits = t.substr(colon + 1);
  } else if (util::istarts_with(t, "http://www.opengis.net/def/crs/EPSG/") ||
             util::istarts_with(t, "https://www.opengis.net/def/crs/EPSG/")) {
    const std::size_t slash = t.rfind('/');
    digits = t.substr(slash + 1);
  } else if (util::istarts_with(t, "http://www.opengis.net/gml/srs/epsg.xml#")) {
    digits = t.substr(t.find('#') + 1);
  } else if (util::istarts_with(t, "ESRI:")) {
    digits = t.substr(5);
  } else {
    return std::nullopt;
  }
  if (digits.empty() || digits.size() > 7) return std::nullopt;
  for (char c : digits) {
    if (!util::is_ascii_digit(c)) return std::nullopt;
  }
  const auto value = util::parse_int(digits);
  if (!value) return std::nullopt;
  return static_cast<int>(*value);
}

std::optional<Crs> parse_crs(std::string_view text) {
  const std::string_view t = util::trim(text);
  if (t.empty()) return Crs{};
  if (util::iequals(t, "OGC:CRS84") || util::iequals(t, "CRS:84") || util::iequals(t, "CRS84") ||
      util::iequals(t, "urn:ogc:def:crs:OGC:1.3:CRS84") || util::iequals(t, "urn:ogc:def:crs:OGC::CRS84") ||
      util::iequals(t, "http://www.opengis.net/def/crs/OGC/1.3/CRS84") || util::iequals(t, "WGS84") ||
      util::iequals(t, "WGS 84")) {
    return Crs{};
  }
  if (auto code = epsg_code(t)) return crs_from_epsg(*code);
  if (auto value = util::parse_int(t)) return crs_from_epsg(static_cast<int>(*value));
  return crs_from_wkt(t);
}

bool is_wgs84(std::string_view crs_text) {
  const auto crs = parse_crs(crs_text);
  return crs && crs->kind == CrsKind::Geographic;
}

std::string crs_id(const Crs& crs) { return "EPSG:" + std::to_string(crs.epsg); }

namespace {

std::string esri_geogcs(Datum datum) {
  switch (datum) {
    case Datum::Wgs84:
      return R"(GEOGCS["GCS_WGS_1984",DATUM["D_WGS_1984",SPHEROID["WGS_1984",6378137.0,298.257223563]],PRIMEM["Greenwich",0.0],UNIT["Degree",0.0174532925199433]])";
    case Datum::Nad83:
      return R"(GEOGCS["GCS_North_American_1983",DATUM["D_North_American_1983",SPHEROID["GRS_1980",6378137.0,298.257222101]],PRIMEM["Greenwich",0.0],UNIT["Degree",0.0174532925199433]])";
    case Datum::Etrs89:
      return R"(GEOGCS["GCS_ETRS_1989",DATUM["D_ETRS_1989",SPHEROID["GRS_1980",6378137.0,298.257222101]],PRIMEM["Greenwich",0.0],UNIT["Degree",0.0174532925199433]])";
  }
  return {};
}

std::string utm_name(const Crs& crs) {
  const std::string zone = std::to_string(crs.zone) + (crs.north ? "N" : "S");
  switch (crs.datum) {
    case Datum::Wgs84: return "WGS_1984_UTM_Zone_" + zone;
    case Datum::Nad83: return "NAD_1983_UTM_Zone_" + zone;
    case Datum::Etrs89: return "ETRS_1989_UTM_Zone_" + zone;
  }
  return {};
}

}  // namespace

std::string crs_esri_wkt(const Crs& crs) {
  switch (crs.kind) {
    case CrsKind::Geographic: return esri_geogcs(Datum::Wgs84);
    case CrsKind::WebMercator:
      return R"(PROJCS["WGS_1984_Web_Mercator_Auxiliary_Sphere",)" + esri_geogcs(Datum::Wgs84) +
             R"(,PROJECTION["Mercator_Auxiliary_Sphere"],PARAMETER["False_Easting",0.0],PARAMETER["False_Northing",0.0],PARAMETER["Central_Meridian",0.0],PARAMETER["Standard_Parallel_1",0.0],PARAMETER["Auxiliary_Sphere_Type",0.0],UNIT["Meter",1.0]])";
    case CrsKind::Utm:
      return R"(PROJCS[")" + utm_name(crs) + R"(",)" + esri_geogcs(crs.datum) +
             R"(,PROJECTION["Transverse_Mercator"],PARAMETER["False_Easting",500000.0],PARAMETER["False_Northing",)" +
             std::string(crs.north ? "0.0" : "10000000.0") + R"(],PARAMETER["Central_Meridian",)" +
             format_number(-183.0 + 6.0 * crs.zone) +
             R"(.0],PARAMETER["Scale_Factor",0.9996],PARAMETER["Latitude_Of_Origin",0.0],UNIT["Meter",1.0]])";
    case CrsKind::BritishNationalGrid:
      return R"(PROJCS["British_National_Grid",GEOGCS["GCS_OSGB_1936",DATUM["D_OSGB_1936",SPHEROID["Airy_1830",6377563.396,299.3249646]],PRIMEM["Greenwich",0.0],UNIT["Degree",0.0174532925199433]],PROJECTION["Transverse_Mercator"],PARAMETER["False_Easting",400000.0],PARAMETER["False_Northing",-100000.0],PARAMETER["Central_Meridian",-2.0],PARAMETER["Scale_Factor",0.9996012717],PARAMETER["Latitude_Of_Origin",49.0],UNIT["Meter",1.0]])";
  }
  return {};
}

std::string crs_ogc_wkt(const Crs& crs) {
  const std::string wgs = R"(GEOGCS["WGS 84",DATUM["WGS_1984",SPHEROID["WGS 84",6378137,298.257223563,AUTHORITY["EPSG","7030"]],AUTHORITY["EPSG","6326"]],PRIMEM["Greenwich",0,AUTHORITY["EPSG","8901"]],UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9122"]],AUTHORITY["EPSG","4326"]])";
  const std::string authority = R"(AUTHORITY["EPSG",")" + std::to_string(crs.epsg) + R"("]])";
  switch (crs.kind) {
    case CrsKind::Geographic: return wgs;
    case CrsKind::WebMercator:
      return R"(PROJCS["WGS 84 / Pseudo-Mercator",)" + wgs +
             R"(,PROJECTION["Mercator_1SP"],PARAMETER["central_meridian",0],PARAMETER["scale_factor",1],PARAMETER["false_easting",0],PARAMETER["false_northing",0],UNIT["metre",1],AXIS["Easting",EAST],AXIS["Northing",NORTH],EXTENSION["PROJ4","+proj=merc +a=6378137 +b=6378137 +lat_ts=0 +lon_0=0 +x_0=0 +y_0=0 +k=1 +units=m +nadgrids=@null +wktext +no_defs"],)" +
             authority;
    case CrsKind::Utm:
    case CrsKind::BritishNationalGrid: {
      // ESRI and OGC WKT 1 agree on the parameters; add the authority.
      std::string esri = crs_esri_wkt(crs);
      esri.pop_back();
      return esri + "," + authority;
    }
  }
  return {};
}

Crs utm_crs_for(double lon, double lat, Datum datum) {
  int zone = static_cast<int>(std::floor((lon + 180.0) / 6.0)) + 1;
  zone = std::clamp(zone, 1, 60);
  if (lat >= 56.0 && lat < 64.0 && lon >= 3.0 && lon < 12.0) zone = 32;
  if (lat >= 72.0 && lat < 84.0 && lon >= 0.0) {
    if (lon < 9.0) zone = 31;
    else if (lon < 21.0) zone = 33;
    else if (lon < 33.0) zone = 35;
    else if (lon < 42.0) zone = 37;
  }
  Crs crs;
  crs.kind = CrsKind::Utm;
  crs.datum = datum;
  crs.zone = zone;
  crs.north = lat >= 0.0;
  crs.epsg = datum == Datum::Wgs84 ? (crs.north ? 32600 : 32700) + zone
                                   : (datum == Datum::Nad83 ? 26900 : 25800) + zone;
  return crs;
}

XY project(const Crs& crs, double lon, double lat) {
  switch (crs.kind) {
    case CrsKind::Geographic: return {lon, lat};
    case CrsKind::WebMercator: {
      const double clamped = std::clamp(lat, -85.05112877980659, 85.05112877980659);
      return {wgs84_a * rad(lon), wgs84_a * std::log(std::tan(pi / 4 + rad(clamped) / 2))};
    }
    case CrsKind::Utm: return tm_forward(utm_params(crs), lon, lat);
    case CrsKind::BritishNationalGrid: {
      double lo = lon, la = lat;
      wgs84_to_osgb36(lo, la);
      return tm_forward(bng_params, lo, la);
    }
  }
  return {lon, lat};
}

XY unproject(const Crs& crs, double x, double y) {
  switch (crs.kind) {
    case CrsKind::Geographic: return {x, y};
    case CrsKind::WebMercator: return {degs(x / wgs84_a), degs(2 * std::atan(std::exp(y / wgs84_a)) - pi / 2)};
    case CrsKind::Utm: return tm_inverse(utm_params(crs), x, y);
    case CrsKind::BritishNationalGrid: {
      XY ll = tm_inverse(bng_params, x, y);
      osgb36_to_wgs84(ll.x, ll.y);
      return ll;
    }
  }
  return {x, y};
}

Result<void> reproject(ir::Geo& geo, const Crs& target) {
  const auto source = parse_crs(geo.crs);
  if (!source) return fail(Status::InvalidArgument, "the coordinate reference system '{}' is not supported", geo.crs.substr(0, 80));
  if (*source == target || (source->kind == CrsKind::Geographic && target.kind == CrsKind::Geographic)) {
    geo.crs = target.kind == CrsKind::Geographic ? std::string() : crs_id(target);
    return {};
  }
  for_each_coord(geo, [&](ir::Coord& c) {
    XY ll = unproject(*source, c.lon, c.lat);
    XY out = project(target, ll.x, ll.y);
    c.lon = out.x;
    c.lat = out.y;
  });
  geo.crs = target.kind == CrsKind::Geographic ? std::string() : crs_id(target);
  return {};
}

// --------------------------------------------------------------------------- measurement

double geodesic_distance(double lon1, double lat1, double lon2, double lat2) {
  if (lon1 == lon2 && lat1 == lat2) return 0.0;
  const double a = wgs84_a, f = wgs84_f, b = a * (1 - f);
  const double l = rad(lon2 - lon1);
  const double u1 = std::atan((1 - f) * std::tan(rad(lat1)));
  const double u2 = std::atan((1 - f) * std::tan(rad(lat2)));
  const double sin_u1 = std::sin(u1), cos_u1 = std::cos(u1), sin_u2 = std::sin(u2), cos_u2 = std::cos(u2);
  double lambda = l;
  double sin_sigma = 0, cos_sigma = 0, sigma = 0, cos2_alpha = 0, cos_2sm = 0;
  bool converged = false;
  for (int i = 0; i < 200; ++i) {
    const double sin_l = std::sin(lambda), cos_l = std::cos(lambda);
    const double t1 = cos_u2 * sin_l, t2 = cos_u1 * sin_u2 - sin_u1 * cos_u2 * cos_l;
    sin_sigma = std::sqrt(t1 * t1 + t2 * t2);
    if (sin_sigma == 0) return 0.0;
    cos_sigma = sin_u1 * sin_u2 + cos_u1 * cos_u2 * cos_l;
    sigma = std::atan2(sin_sigma, cos_sigma);
    const double sin_alpha = cos_u1 * cos_u2 * sin_l / sin_sigma;
    cos2_alpha = 1 - sin_alpha * sin_alpha;
    cos_2sm = cos2_alpha != 0 ? cos_sigma - 2 * sin_u1 * sin_u2 / cos2_alpha : 0.0;
    const double c = f / 16 * cos2_alpha * (4 + f * (4 - 3 * cos2_alpha));
    const double previous = lambda;
    lambda = l + (1 - c) * f * sin_alpha * (sigma + c * sin_sigma * (cos_2sm + c * cos_sigma * (-1 + 2 * cos_2sm * cos_2sm)));
    if (std::fabs(lambda - previous) < 1e-12) {
      converged = true;
      break;
    }
  }
  if (!converged) {
    // Near-antipodal: fall back to a great circle on the mean sphere.
    const double p1 = rad(lat1), p2 = rad(lat2);
    const double h = std::sin((p2 - p1) / 2) * std::sin((p2 - p1) / 2) +
                     std::cos(p1) * std::cos(p2) * std::sin(l / 2) * std::sin(l / 2);
    return 2 * 6371008.8 * std::asin(std::min(1.0, std::sqrt(h)));
  }
  const double u_sq = cos2_alpha * (a * a - b * b) / (b * b);
  const double big_a = 1 + u_sq / 16384 * (4096 + u_sq * (-768 + u_sq * (320 - 175 * u_sq)));
  const double big_b = u_sq / 1024 * (256 + u_sq * (-128 + u_sq * (74 - 47 * u_sq)));
  const double delta_sigma =
      big_b * sin_sigma *
      (cos_2sm + big_b / 4 *
                     (cos_sigma * (-1 + 2 * cos_2sm * cos_2sm) -
                      big_b / 6 * cos_2sm * (-3 + 4 * sin_sigma * sin_sigma) * (-3 + 4 * cos_2sm * cos_2sm)));
  return b * big_a * (sigma - delta_sigma);
}

namespace {

double line_length(const std::vector<ir::Coord>& points) {
  double total = 0;
  for (std::size_t i = 1; i < points.size(); ++i) total += geodesic_distance(points[i - 1], points[i]);
  return total;
}

double polygon_area(const ir::Polygon& polygon) {
  double total = 0;
  for (std::size_t r = 0; r < polygon.rings.size(); ++r) {
    const double a = std::fabs(ring_area_m2(polygon.rings[r]));
    total += r == 0 ? a : -a;
  }
  return std::max(0.0, total);
}

}  // namespace

double ring_area_m2(const std::vector<ir::Coord>& ring) {
  // Spherical excess on the authalic sphere (Chamberlain & Duquette 2007).
  constexpr double radius = 6371007.180918475;
  const std::size_t n = ring.size();
  if (n < 4) return 0.0;
  double total = 0;
  for (std::size_t i = 0; i + 1 < n; ++i) {
    const ir::Coord& p1 = ring[i];
    const ir::Coord& p2 = ring[i + 1];
    double dl = p2.lon - p1.lon;
    if (dl > 180) dl -= 360;
    if (dl < -180) dl += 360;
    total += rad(dl) * (2 + std::sin(rad(p1.lat)) + std::sin(rad(p2.lat)));
  }
  return -total * radius * radius / 2;
}

double length_m(const ir::Geometry& geometry) {
  double total = 0;
  std::visit(
      [&](const auto& shape) {
        using T = std::decay_t<decltype(shape)>;
        if constexpr (std::is_same_v<T, ir::LineString>) {
          total += line_length(shape.points);
        } else if constexpr (std::is_same_v<T, ir::MultiLineString>) {
          for (const auto& line : shape.lines) total += line_length(line.points);
        } else if constexpr (std::is_same_v<T, ir::Polygon>) {
          for (const auto& ring : shape.rings) total += line_length(ring);
        } else if constexpr (std::is_same_v<T, ir::MultiPolygon>) {
          for (const auto& polygon : shape.polygons) {
            for (const auto& ring : polygon.rings) total += line_length(ring);
          }
        } else if constexpr (std::is_same_v<T, ir::GeometryCollection>) {
          for (const auto& g : shape.geometries) total += length_m(g);
        }
      },
      geometry.shape);
  return total;
}

double area_m2(const ir::Geometry& geometry) {
  double total = 0;
  std::visit(
      [&](const auto& shape) {
        using T = std::decay_t<decltype(shape)>;
        if constexpr (std::is_same_v<T, ir::Polygon>) {
          total += polygon_area(shape);
        } else if constexpr (std::is_same_v<T, ir::MultiPolygon>) {
          for (const auto& polygon : shape.polygons) total += polygon_area(polygon);
        } else if constexpr (std::is_same_v<T, ir::GeometryCollection>) {
          for (const auto& g : shape.geometries) total += area_m2(g);
        }
      },
      geometry.shape);
  return total;
}

void BoundingBox::add(const ir::Coord& c) {
  if (!std::isfinite(c.lon) || !std::isfinite(c.lat)) return;
  if (empty) {
    min_lon = max_lon = c.lon;
    min_lat = max_lat = c.lat;
    empty = false;
  } else {
    min_lon = std::min(min_lon, c.lon);
    max_lon = std::max(max_lon, c.lon);
    min_lat = std::min(min_lat, c.lat);
    max_lat = std::max(max_lat, c.lat);
  }
  if (c.ele) {
    min_ele = min_ele ? std::min(*min_ele, *c.ele) : *c.ele;
    max_ele = max_ele ? std::max(*max_ele, *c.ele) : *c.ele;
  }
}

void BoundingBox::add(const BoundingBox& other) {
  if (other.empty) return;
  ir::Coord lo;
  lo.lon = other.min_lon;
  lo.lat = other.min_lat;
  lo.ele = other.min_ele;
  ir::Coord hi;
  hi.lon = other.max_lon;
  hi.lat = other.max_lat;
  hi.ele = other.max_ele;
  add(lo);
  add(hi);
}

BoundingBox bounding_box(const ir::Geometry& geometry) {
  BoundingBox box;
  for_each_coord(geometry, [&](const ir::Coord& c) { box.add(c); });
  return box;
}

BoundingBox bounding_box(const ir::Geo& geo) {
  BoundingBox box;
  for (const ir::Feature& f : geo.features) box.add(bounding_box(f.geometry));
  return box;
}

namespace {

void collect_lines(const ir::Geometry& geometry, std::vector<const std::vector<ir::Coord>*>& out, int depth = 0) {
  if (depth > ir::max_geometry_depth) return;
  if (const auto* line = std::get_if<ir::LineString>(&geometry.shape)) {
    out.push_back(&line->points);
  } else if (const auto* multi = std::get_if<ir::MultiLineString>(&geometry.shape)) {
    for (const auto& l : multi->lines) out.push_back(&l.points);
  } else if (const auto* collection = std::get_if<ir::GeometryCollection>(&geometry.shape)) {
    for (const auto& g : collection->geometries) collect_lines(g, out, depth + 1);
  }
}

}  // namespace

TrackStats track_stats(const ir::Geometry& geometry, double elevation_threshold_m) {
  TrackStats stats;
  std::vector<const std::vector<ir::Coord>*> lines;
  collect_lines(geometry, lines);
  double moving = 0;
  bool any_time = false;
  double hr_sum = 0, cad_sum = 0, power_sum = 0;
  std::size_t hr_n = 0, cad_n = 0, power_n = 0;
  for (const auto* line : lines) {
    if (line->empty()) continue;
    ++stats.segments;
    std::optional<double> reference_ele;
    std::optional<std::int64_t> first_time, last_time;
    for (std::size_t i = 0; i < line->size(); ++i) {
      const ir::Coord& c = (*line)[i];
      ++stats.points;
      if (i > 0) {
        const ir::Coord& p = (*line)[i - 1];
        const double d = geodesic_distance(p, c);
        stats.distance_m += d;
        if (p.time_ms && c.time_ms && *c.time_ms > *p.time_ms) {
          const double speed = d / (static_cast<double>(*c.time_ms - *p.time_ms) / 1000.0);
          stats.max_speed_mps = stats.max_speed_mps ? std::max(*stats.max_speed_mps, speed) : speed;
        }
      }
      if (c.ele) {
        stats.min_ele = stats.min_ele ? std::min(*stats.min_ele, *c.ele) : *c.ele;
        stats.max_ele = stats.max_ele ? std::max(*stats.max_ele, *c.ele) : *c.ele;
        if (!reference_ele) {
          reference_ele = c.ele;
        } else {
          const double delta = *c.ele - *reference_ele;
          if (std::fabs(delta) >= elevation_threshold_m && delta != 0.0) {
            if (delta > 0) stats.elevation_gain_m += delta;
            else stats.elevation_loss_m -= delta;
            reference_ele = c.ele;
          }
        }
      }
      if (c.time_ms) {
        any_time = true;
        if (!first_time) first_time = c.time_ms;
        last_time = c.time_ms;
        stats.start_ms = stats.start_ms ? std::min(*stats.start_ms, *c.time_ms) : *c.time_ms;
        stats.end_ms = stats.end_ms ? std::max(*stats.end_ms, *c.time_ms) : *c.time_ms;
      }
      if (c.heart_rate) {
        hr_sum += *c.heart_rate;
        ++hr_n;
        stats.max_heart_rate = stats.max_heart_rate ? std::max(*stats.max_heart_rate, *c.heart_rate) : *c.heart_rate;
      }
      if (c.cadence) {
        cad_sum += *c.cadence;
        ++cad_n;
      }
      if (c.power) {
        power_sum += *c.power;
        ++power_n;
      }
    }
    if (first_time && last_time && *last_time > *first_time) {
      moving += static_cast<double>(*last_time - *first_time) / 1000.0;
    }
  }
  if (any_time && stats.start_ms && stats.end_ms) {
    stats.duration_s = static_cast<double>(*stats.end_ms - *stats.start_ms) / 1000.0;
    stats.moving_time_s = moving;
    if (moving > 0) stats.average_speed_mps = stats.distance_m / moving;
  }
  if (hr_n > 0) stats.average_heart_rate = hr_sum / static_cast<double>(hr_n);
  if (cad_n > 0) stats.average_cadence = cad_sum / static_cast<double>(cad_n);
  if (power_n > 0) stats.average_power = power_sum / static_cast<double>(power_n);
  return stats;
}

// --------------------------------------------------------------------------- simplification

namespace {

struct P2 {
  double x, y;
};

// Local planar coordinates in metres (equirectangular about the mean latitude).
std::vector<P2> planar(const std::vector<ir::Coord>& points, bool projected) {
  std::vector<P2> out;
  out.reserve(points.size());
  if (projected) {
    for (const ir::Coord& c : points) out.push_back({c.lon, c.lat});
    return out;
  }
  double lat_sum = 0;
  for (const ir::Coord& c : points) lat_sum += c.lat;
  const double mean_lat = points.empty() ? 0.0 : lat_sum / static_cast<double>(points.size());
  constexpr double r = 6371008.8;
  const double kx = r * deg * std::cos(rad(mean_lat));
  const double ky = r * deg;
  for (const ir::Coord& c : points) out.push_back({c.lon * kx, c.lat * ky});
  return out;
}

double segment_distance_sq(const P2& p, const P2& a, const P2& b) {
  const double dx = b.x - a.x, dy = b.y - a.y;
  const double len = dx * dx + dy * dy;
  double t = len > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len : 0.0;
  t = std::clamp(t, 0.0, 1.0);
  const double x = a.x + t * dx - p.x, y = a.y + t * dy - p.y;
  return x * x + y * y;
}

std::vector<std::size_t> douglas_peucker(const std::vector<P2>& pts, double tolerance) {
  const std::size_t n = pts.size();
  std::vector<char> keep(n, 0);
  if (n == 0) return {};
  keep[0] = keep[n - 1] = 1;
  const double tol_sq = tolerance * tolerance;
  std::vector<std::pair<std::size_t, std::size_t>> stack;
  if (n > 2) stack.emplace_back(0, n - 1);
  while (!stack.empty()) {
    const auto [first, last] = stack.back();
    stack.pop_back();
    double worst = -1;
    std::size_t index = first;
    for (std::size_t i = first + 1; i < last; ++i) {
      const double d = segment_distance_sq(pts[i], pts[first], pts[last]);
      if (d > worst) {
        worst = d;
        index = i;
      }
    }
    if (worst > tol_sq) {
      keep[index] = 1;
      if (index - first > 1) stack.emplace_back(first, index);
      if (last - index > 1) stack.emplace_back(index, last);
    }
  }
  std::vector<std::size_t> out;
  for (std::size_t i = 0; i < n; ++i) {
    if (keep[i]) out.push_back(i);
  }
  return out;
}

double triangle_area(const P2& a, const P2& b, const P2& c) {
  return std::fabs((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y)) / 2;
}

std::vector<std::size_t> visvalingam(const std::vector<P2>& pts, double tolerance) {
  const std::size_t n = pts.size();
  if (n <= 2) {
    std::vector<std::size_t> all(n);
    for (std::size_t i = 0; i < n; ++i) all[i] = i;
    return all;
  }
  const double threshold = tolerance * tolerance;
  std::vector<std::size_t> prev(n), next(n);
  std::vector<double> area(n, std::numeric_limits<double>::infinity());
  std::vector<char> removed(n, 0);
  for (std::size_t i = 0; i < n; ++i) {
    prev[i] = i == 0 ? 0 : i - 1;
    next[i] = i + 1 < n ? i + 1 : n - 1;
  }
  using Item = std::pair<double, std::size_t>;
  std::priority_queue<Item, std::vector<Item>, std::greater<>> heap;
  for (std::size_t i = 1; i + 1 < n; ++i) {
    area[i] = triangle_area(pts[i - 1], pts[i], pts[i + 1]);
    heap.emplace(area[i], i);
  }
  double max_removed = 0;
  while (!heap.empty()) {
    const auto [a, i] = heap.top();
    heap.pop();
    if (removed[i] || a != area[i]) continue;  // stale entry
    // Effective area never decreases (Visvalingam's rule).
    const double effective = std::max(a, max_removed);
    if (effective >= threshold) break;
    max_removed = effective;
    removed[i] = 1;
    const std::size_t p = prev[i], q = next[i];
    next[p] = q;
    prev[q] = p;
    if (p != 0) {
      area[p] = triangle_area(pts[prev[p]], pts[p], pts[q]);
      heap.emplace(area[p], p);
    }
    if (q != n - 1) {
      area[q] = triangle_area(pts[p], pts[q], pts[next[q]]);
      heap.emplace(area[q], q);
    }
  }
  std::vector<std::size_t> out;
  for (std::size_t i = 0; i < n; ++i) {
    if (!removed[i]) out.push_back(i);
  }
  return out;
}

bool segments_intersect(const P2& a, const P2& b, const P2& c, const P2& d) {
  const auto orient = [](const P2& p, const P2& q, const P2& r) {
    const double v = (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x);
    return v > 0 ? 1 : (v < 0 ? -1 : 0);
  };
  const auto on_segment = [](const P2& p, const P2& q, const P2& r) {
    return std::min(p.x, r.x) <= q.x && q.x <= std::max(p.x, r.x) && std::min(p.y, r.y) <= q.y &&
           q.y <= std::max(p.y, r.y);
  };
  const int o1 = orient(a, b, c), o2 = orient(a, b, d), o3 = orient(c, d, a), o4 = orient(c, d, b);
  if (o1 != o2 && o3 != o4) return true;
  if (o1 == 0 && on_segment(a, c, b)) return true;
  if (o2 == 0 && on_segment(a, d, b)) return true;
  if (o3 == 0 && on_segment(c, a, d)) return true;
  if (o4 == 0 && on_segment(c, b, d)) return true;
  return false;
}

bool planar_ring_self_intersects(const std::vector<P2>& ring) {
  const std::size_t n = ring.size();
  if (n < 4) return false;
  const std::size_t segments = n - 1;
  // Uniform grid over the bounding box so that the check is near-linear.
  double min_x = ring[0].x, max_x = ring[0].x, min_y = ring[0].y, max_y = ring[0].y;
  for (const P2& p : ring) {
    min_x = std::min(min_x, p.x);
    max_x = std::max(max_x, p.x);
    min_y = std::min(min_y, p.y);
    max_y = std::max(max_y, p.y);
  }
  const std::size_t cells_per_side =
      std::clamp<std::size_t>(static_cast<std::size_t>(std::sqrt(static_cast<double>(segments))), 1, 512);
  const double w = std::max(max_x - min_x, 1e-12) / static_cast<double>(cells_per_side);
  const double h = std::max(max_y - min_y, 1e-12) / static_cast<double>(cells_per_side);
  const auto cell = [&](double v, double lo, double size) {
    return std::min(cells_per_side - 1, static_cast<std::size_t>(std::max(0.0, (v - lo) / size)));
  };
  std::unordered_map<std::size_t, std::vector<std::size_t>> grid;
  for (std::size_t s = 0; s < segments; ++s) {
    const P2& a = ring[s];
    const P2& b = ring[s + 1];
    const std::size_t x0 = cell(std::min(a.x, b.x), min_x, w), x1 = cell(std::max(a.x, b.x), min_x, w);
    const std::size_t y0 = cell(std::min(a.y, b.y), min_y, h), y1 = cell(std::max(a.y, b.y), min_y, h);
    for (std::size_t y = y0; y <= y1; ++y) {
      for (std::size_t x = x0; x <= x1; ++x) {
        auto& bucket = grid[y * cells_per_side + x];
        for (std::size_t other : bucket) {
          const bool adjacent = other + 1 == s || s + 1 == other || (other == 0 && s == segments - 1) ||
                                (s == 0 && other == segments - 1);
          if (adjacent) {
            // Neighbours share a vertex; they only conflict when collinear and overlapping.
            continue;
          }
          if (segments_intersect(ring[other], ring[other + 1], a, b)) return true;
        }
        bucket.push_back(s);
      }
    }
  }
  return false;
}

std::vector<std::size_t> run_method(const std::vector<P2>& pts, double tolerance, SimplifyMethod method) {
  return method == SimplifyMethod::DouglasPeucker ? douglas_peucker(pts, tolerance) : visvalingam(pts, tolerance);
}

}  // namespace

bool ring_self_intersects(const std::vector<ir::Coord>& ring) {
  std::vector<P2> pts;
  pts.reserve(ring.size());
  for (const ir::Coord& c : ring) pts.push_back({c.lon, c.lat});
  return planar_ring_self_intersects(pts);
}

std::vector<std::size_t> simplify_indices(const std::vector<ir::Coord>& points, double tolerance,
                                          SimplifyMethod method, bool projected, bool ring) {
  std::vector<std::size_t> all(points.size());
  for (std::size_t i = 0; i < points.size(); ++i) all[i] = i;
  if (!(tolerance > 0) || points.size() < (ring ? 5u : 3u)) return all;
  const std::vector<P2> pts = planar(points, projected);
  if (!ring) return run_method(pts, tolerance, method);
  // Rings: split at the point farthest from the start so that both halves
  // have distinct end points, then make sure the result is a valid ring.
  std::size_t far = 0;
  double best = -1;
  for (std::size_t i = 1; i + 1 < pts.size(); ++i) {
    const double d = (pts[i].x - pts[0].x) * (pts[i].x - pts[0].x) + (pts[i].y - pts[0].y) * (pts[i].y - pts[0].y);
    if (d > best) {
      best = d;
      far = i;
    }
  }
  double t = tolerance;
  for (int attempt = 0; attempt < 8; ++attempt, t /= 2) {
    const std::vector<P2> first(pts.begin(), pts.begin() + static_cast<std::ptrdiff_t>(far) + 1);
    const std::vector<P2> second(pts.begin() + static_cast<std::ptrdiff_t>(far), pts.end());
    std::vector<std::size_t> kept = run_method(first, t, method);
    for (std::size_t i : run_method(second, t, method)) {
      if (i == 0) continue;
      kept.push_back(i + far);
    }
    if (kept.size() < 4) continue;
    std::vector<P2> candidate;
    candidate.reserve(kept.size());
    for (std::size_t i : kept) candidate.push_back(pts[i]);
    if (planar_ring_self_intersects(candidate)) continue;
    return kept;
  }
  return all;
}

namespace {

void simplify_sequence(std::vector<ir::Coord>& points, double tolerance, SimplifyMethod method, bool projected,
                       bool ring) {
  const std::vector<std::size_t> kept = simplify_indices(points, tolerance, method, projected, ring);
  if (kept.size() == points.size()) return;
  std::vector<ir::Coord> out;
  out.reserve(kept.size());
  for (std::size_t i : kept) out.push_back(std::move(points[i]));
  points = std::move(out);
}

void simplify_geometry(ir::Geometry& geometry, double tolerance, SimplifyMethod method, bool projected, int depth) {
  if (depth > ir::max_geometry_depth) return;
  std::visit(
      [&](auto& shape) {
        using T = std::decay_t<decltype(shape)>;
        if constexpr (std::is_same_v<T, ir::LineString>) {
          simplify_sequence(shape.points, tolerance, method, projected, false);
        } else if constexpr (std::is_same_v<T, ir::MultiLineString>) {
          for (auto& line : shape.lines) simplify_sequence(line.points, tolerance, method, projected, false);
        } else if constexpr (std::is_same_v<T, ir::Polygon>) {
          for (auto& ring : shape.rings) simplify_sequence(ring, tolerance, method, projected, true);
        } else if constexpr (std::is_same_v<T, ir::MultiPolygon>) {
          for (auto& polygon : shape.polygons) {
            for (auto& ring : polygon.rings) simplify_sequence(ring, tolerance, method, projected, true);
          }
        } else if constexpr (std::is_same_v<T, ir::GeometryCollection>) {
          for (auto& g : shape.geometries) simplify_geometry(g, tolerance, method, projected, depth + 1);
        }
      },
      geometry.shape);
}

double round_to(double value, int decimals) {
  if (!std::isfinite(value)) return value;
  const double scale = std::pow(10.0, decimals);
  const double scaled = value * scale;
  if (std::fabs(scaled) > 9e15) return value;
  return std::round(scaled) / scale;
}

}  // namespace

void simplify(ir::Geo& geo, double tolerance_m, SimplifyMethod method, bool projected) {
  if (!(tolerance_m > 0)) return;
  for (ir::Feature& f : geo.features) simplify_geometry(f.geometry, tolerance_m, method, projected, 0);
}

void round_coordinates(ir::Geo& geo, int decimals) {
  if (decimals < 0) return;
  const int ele_decimals = std::clamp(decimals - 4, 1, 3);
  for_each_coord(geo, [&](ir::Coord& c) {
    c.lon = round_to(c.lon, decimals);
    c.lat = round_to(c.lat, decimals);
    if (c.ele) c.ele = round_to(*c.ele, ele_decimals);
    if (c.m) c.m = round_to(*c.m, std::max(decimals, 3));
  });
}

double signed_area(const std::vector<ir::Coord>& ring) {
  double total = 0;
  for (std::size_t i = 0; i + 1 < ring.size(); ++i) {
    total += ring[i].lon * ring[i + 1].lat - ring[i + 1].lon * ring[i].lat;
  }
  if (!ring.empty() && !(ring.front().lon == ring.back().lon && ring.front().lat == ring.back().lat)) {
    total += ring.back().lon * ring.front().lat - ring.front().lon * ring.back().lat;
  }
  return total / 2;
}

namespace {

void orient_polygon(ir::Polygon& polygon, bool outer_ccw) {
  for (std::size_t r = 0; r < polygon.rings.size(); ++r) {
    auto& ring = polygon.rings[r];
    const double a = signed_area(ring);
    const bool want_ccw = (r == 0) == outer_ccw;
    if ((a > 0) != want_ccw && a != 0) std::reverse(ring.begin(), ring.end());
  }
}

}  // namespace

void orient_rings(ir::Geometry& geometry, bool outer_ccw) {
  if (auto* polygon = std::get_if<ir::Polygon>(&geometry.shape)) {
    orient_polygon(*polygon, outer_ccw);
  } else if (auto* multi = std::get_if<ir::MultiPolygon>(&geometry.shape)) {
    for (auto& p : multi->polygons) orient_polygon(p, outer_ccw);
  } else if (auto* collection = std::get_if<ir::GeometryCollection>(&geometry.shape)) {
    for (auto& g : collection->geometries) orient_rings(g, outer_ccw);
  }
}

namespace {

bool point_in_ring(const std::vector<ir::Coord>& ring, double x, double y) {
  bool in = false;
  for (std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
    const ir::Coord& a = ring[i];
    const ir::Coord& b = ring[j];
    if ((a.lat > y) != (b.lat > y) && x < (b.lon - a.lon) * (y - a.lat) / (b.lat - a.lat) + a.lon) in = !in;
  }
  return in;
}

}  // namespace

ir::Geometry assemble_rings(std::vector<std::vector<ir::Coord>> rings, bool outer_clockwise) {
  std::vector<ir::Polygon> polygons;
  std::vector<double> areas;
  std::vector<std::vector<ir::Coord>> holes;
  for (auto& ring : rings) {
    if (ring.empty()) continue;
    if (!(ring.front().lon == ring.back().lon && ring.front().lat == ring.back().lat)) ring.push_back(ring.front());
    const double a = signed_area(ring);
    const bool outer = outer_clockwise ? a <= 0 : a >= 0;
    if (outer) {
      areas.push_back(std::fabs(a));
      polygons.push_back(ir::Polygon{{std::move(ring)}});
    } else {
      holes.push_back(std::move(ring));
    }
  }
  for (auto& hole : holes) {
    std::optional<std::size_t> best;
    for (std::size_t i = 0; i < polygons.size(); ++i) {
      if (point_in_ring(polygons[i].rings[0], hole[0].lon, hole[0].lat) && (!best || areas[i] < areas[*best])) best = i;
    }
    if (best) {
      polygons[*best].rings.push_back(std::move(hole));
    } else {
      areas.push_back(std::fabs(signed_area(hole)));
      polygons.push_back(ir::Polygon{{std::move(hole)}});
    }
  }
  ir::Geometry g;
  if (polygons.size() == 1) {
    g.shape = std::move(polygons.front());
  } else {
    g.shape = ir::MultiPolygon{std::move(polygons)};
  }
  orient_rings(g, true);
  return g;
}

// --------------------------------------------------------------------------- antimeridian

namespace {

bool crosses(const std::vector<ir::Coord>& points) {
  for (std::size_t i = 1; i < points.size(); ++i) {
    if (std::fabs(points[i].lon - points[i - 1].lon) > 180.0) return true;
  }
  return false;
}

ir::Coord interpolate(const ir::Coord& a, const ir::Coord& b, double lon) {
  ir::Coord c = a;
  const double t = (b.lon == a.lon) ? 0.0 : (lon - a.lon) / (b.lon - a.lon);
  c.lon = lon;
  c.lat = a.lat + t * (b.lat - a.lat);
  if (a.ele && b.ele) c.ele = *a.ele + t * (*b.ele - *a.ele);
  if (a.time_ms && b.time_ms) {
    c.time_ms = *a.time_ms + static_cast<std::int64_t>(std::llround(t * static_cast<double>(*b.time_ms - *a.time_ms)));
  }
  return c;
}

std::vector<std::vector<ir::Coord>> split_line(const std::vector<ir::Coord>& points) {
  std::vector<std::vector<ir::Coord>> parts(1);
  for (std::size_t i = 0; i < points.size(); ++i) {
    if (i > 0 && std::fabs(points[i].lon - points[i - 1].lon) > 180.0) {
      const ir::Coord& a = points[i - 1];
      ir::Coord b = points[i];
      const double edge = a.lon > 0 ? 180.0 : -180.0;
      b.lon += a.lon > 0 ? 360.0 : -360.0;
      ir::Coord cut = interpolate(a, b, edge);
      parts.back().push_back(cut);
      parts.emplace_back();
      cut.lon = -edge;
      parts.back().push_back(cut);
    }
    parts.back().push_back(points[i]);
  }
  return parts;
}

// Unwrap longitudes so consecutive points never jump by more than 180 degrees.
std::vector<ir::Coord> unwrap(const std::vector<ir::Coord>& ring) {
  std::vector<ir::Coord> out = ring;
  for (std::size_t i = 1; i < out.size(); ++i) {
    while (out[i].lon - out[i - 1].lon > 180.0) out[i].lon -= 360.0;
    while (out[i].lon - out[i - 1].lon < -180.0) out[i].lon += 360.0;
  }
  return out;
}

// Sutherland-Hodgman against the half plane lon <= edge (keep_below) or lon >= edge.
std::vector<ir::Coord> clip(const std::vector<ir::Coord>& ring, double edge, bool keep_below) {
  std::vector<ir::Coord> out;
  if (ring.size() < 2) return out;
  const auto inside = [&](const ir::Coord& c) { return keep_below ? c.lon <= edge : c.lon >= edge; };
  for (std::size_t i = 0; i + 1 < ring.size(); ++i) {
    const ir::Coord& a = ring[i];
    const ir::Coord& b = ring[i + 1];
    if (inside(a)) out.push_back(a);
    if (inside(a) != inside(b)) out.push_back(interpolate(a, b, edge));
  }
  if (!out.empty()) out.push_back(out.front());
  if (out.size() < 4) out.clear();
  return out;
}

void normalise_lon(std::vector<ir::Coord>& ring) {
  for (ir::Coord& c : ring) {
    while (c.lon > 180.0) c.lon -= 360.0;
    while (c.lon < -180.0) c.lon += 360.0;
  }
}

std::vector<ir::Polygon> split_polygon(const ir::Polygon& polygon) {
  bool crossing = false;
  for (const auto& ring : polygon.rings) crossing = crossing || crosses(ring);
  if (!crossing || polygon.rings.empty()) return {polygon};
  std::vector<std::vector<ir::Coord>> rings;
  for (const auto& ring : polygon.rings) rings.push_back(unwrap(ring));
  // Which side of the meridian is the outer ring mostly on?
  const double start = rings[0].front().lon;
  const double edge = start >= 0 ? 180.0 : -180.0;
  std::vector<ir::Polygon> parts;
  for (bool below : {true, false}) {
    ir::Polygon part;
    for (std::size_t r = 0; r < rings.size(); ++r) {
      std::vector<ir::Coord> clipped = clip(rings[r], edge, below);
      if (clipped.empty()) {
        if (r == 0) break;
        continue;
      }
      // Bring the clipped piece back into -180..180, keeping the cut on the edge it belongs to.
      const bool beyond = below ? edge < 0 : edge > 0;  // the part on the far side of the edge
      if (beyond) {
        for (ir::Coord& c : clipped) c.lon += edge > 0 ? -360.0 : 360.0;
      }
      normalise_lon(clipped);
      part.rings.push_back(std::move(clipped));
    }
    if (!part.rings.empty()) parts.push_back(std::move(part));
  }
  return parts;
}

}  // namespace

ir::Geometry split_antimeridian(const ir::Geometry& geometry) {
  ir::Geometry out = geometry;
  if (const auto* line = std::get_if<ir::LineString>(&geometry.shape)) {
    if (!crosses(line->points)) return out;
    ir::MultiLineString multi;
    for (auto& part : split_line(line->points)) multi.lines.push_back(ir::LineString{std::move(part)});
    out.shape = std::move(multi);
  } else if (const auto* multi = std::get_if<ir::MultiLineString>(&geometry.shape)) {
    ir::MultiLineString result;
    for (const auto& l : multi->lines) {
      for (auto& part : split_line(l.points)) result.lines.push_back(ir::LineString{std::move(part)});
    }
    out.shape = std::move(result);
  } else if (const auto* polygon = std::get_if<ir::Polygon>(&geometry.shape)) {
    std::vector<ir::Polygon> parts = split_polygon(*polygon);
    if (parts.size() == 1) {
      out.shape = std::move(parts.front());
    } else {
      out.shape = ir::MultiPolygon{std::move(parts)};
    }
  } else if (const auto* mp = std::get_if<ir::MultiPolygon>(&geometry.shape)) {
    ir::MultiPolygon result;
    for (const auto& p : mp->polygons) {
      for (auto& part : split_polygon(p)) result.polygons.push_back(std::move(part));
    }
    out.shape = std::move(result);
  } else if (const auto* collection = std::get_if<ir::GeometryCollection>(&geometry.shape)) {
    ir::GeometryCollection result;
    for (const auto& g : collection->geometries) result.geometries.push_back(split_antimeridian(g));
    out.shape = std::move(result);
  }
  return out;
}

// --------------------------------------------------------------------------- tracks

namespace {

bool is_track(const ir::Feature& f) {
  return f.role == ir::FeatureRole::Track &&
         (std::holds_alternative<ir::MultiLineString>(f.geometry.shape) ||
          std::holds_alternative<ir::LineString>(f.geometry.shape));
}

std::vector<ir::LineString> track_lines(ir::Feature& f) {
  if (auto* multi = std::get_if<ir::MultiLineString>(&f.geometry.shape)) return std::move(multi->lines);
  return {std::move(std::get<ir::LineString>(f.geometry.shape))};
}

}  // namespace

void merge_tracks(ir::Geo& geo) {
  std::vector<ir::Feature> out;
  std::optional<std::size_t> first;
  for (ir::Feature& f : geo.features) {
    if (!is_track(f)) {
      out.push_back(std::move(f));
      continue;
    }
    std::vector<ir::LineString> lines = track_lines(f);
    if (!first) {
      first = out.size();
      ir::LineString joined;
      for (auto& l : lines) {
        for (auto& c : l.points) joined.points.push_back(std::move(c));
      }
      f.geometry.shape = ir::MultiLineString{{std::move(joined)}};
      out.push_back(std::move(f));
    } else {
      auto& joined = std::get<ir::MultiLineString>(out[*first].geometry.shape).lines.front();
      for (auto& l : lines) {
        for (auto& c : l.points) joined.points.push_back(std::move(c));
      }
    }
  }
  geo.features = std::move(out);
}

void split_tracks(ir::Geo& geo) {
  std::vector<ir::Feature> out;
  for (ir::Feature& f : geo.features) {
    auto* multi = std::get_if<ir::MultiLineString>(&f.geometry.shape);
    if (f.role != ir::FeatureRole::Track || multi == nullptr || multi->lines.size() < 2) {
      out.push_back(std::move(f));
      continue;
    }
    std::vector<ir::LineString> lines = std::move(multi->lines);
    const std::string base = feature_name(f);
    for (std::size_t i = 0; i < lines.size(); ++i) {
      ir::Feature part = f;
      part.geometry.shape = ir::MultiLineString{{std::move(lines[i])}};
      part.name = base.empty() ? "segment " + std::to_string(i + 1) : base + " (" + std::to_string(i + 1) + ")";
      if (part.properties.is_object() && part.properties.contains("name")) part.properties.set("name", part.name);
      out.push_back(std::move(part));
    }
  }
  geo.features = std::move(out);
}

}  // namespace cvt::geo
