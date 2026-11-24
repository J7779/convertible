// Convertible core - archive family: internals shared by the ISO 9660 reader (iso.cpp), the UDF reader
// (udf_read.cpp) and the ISO 9660 writer (iso_write.cpp).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "families/archive/archive_common.hpp"
#include "ir/archive.hpp"
#include "util/bytes.hpp"

namespace cvt::archive::iso {

inline constexpr std::size_t sector = 2048;

/// True when the volume recognition sequence (sectors 16..) names a UDF
/// volume (NSR02 / NSR03).
[[nodiscard]] bool has_udf(ByteView image) noexcept;

/// Read the UDF file system of `image` into `sink`.
[[nodiscard]] Result<void> read_udf(ByteView image, EntrySink& sink, const Context& ctx);

/// Write an ISO 9660 level 3 image with Rock Ridge and Joliet.
[[nodiscard]] Result<Bytes> write_iso(const ir::Archive& archive, WriteContext& ctx);

/// Days since 1970-01-01 of a proleptic Gregorian date.
[[nodiscard]] constexpr std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept {
  y -= m <= 2 ? 1 : 0;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

struct CivilTime {
  std::int64_t year = 1970;
  unsigned month = 1;
  unsigned day = 1;
  unsigned hour = 0;
  unsigned minute = 0;
  unsigned second = 0;
};

/// UTC calendar time of a Unix timestamp.
[[nodiscard]] constexpr CivilTime civil_from_unix(std::int64_t t) noexcept {
  std::int64_t days = t / 86400;
  std::int64_t rem = t % 86400;
  if (rem < 0) {
    rem += 86400;
    --days;
  }
  const std::int64_t z = days + 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const auto doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  CivilTime c;
  c.day = doy - (153 * mp + 2) / 5 + 1;
  c.month = mp < 10 ? mp + 3 : mp - 9;
  c.year = static_cast<std::int64_t>(yoe) + era * 400 + (c.month <= 2 ? 1 : 0);
  c.hour = static_cast<unsigned>(rem / 3600);
  c.minute = static_cast<unsigned>(rem / 60 % 60);
  c.second = static_cast<unsigned>(rem % 60);
  return c;
}

/// Unix time of a calendar time given in a zone `offset_minutes` east of UTC.
[[nodiscard]] constexpr std::int64_t unix_time(std::int64_t year, unsigned month, unsigned day, unsigned hour,
                                               unsigned minute, unsigned second, int offset_minutes) noexcept {
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) return 0;
  return days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second -
         static_cast<std::int64_t>(offset_minutes) * 60;
}

}  // namespace cvt::archive::iso
