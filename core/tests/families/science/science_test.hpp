// Shared helpers for the science family tests.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <string>
#include <string_view>

#include "engine/detect.hpp"
#include "engine/registry.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

namespace cvt::test::science {

/// The format detection picks for these bytes and file name.
[[nodiscard]] inline std::string detected(ByteView data, std::string_view filename = {}) {
  const auto result = detect_format(Registry::instance(), data, filename);
  if (!result) return "";
  return result->format == nullptr ? "" : result->format->id;
}

/// Text of a cell of sheet 0.
[[nodiscard]] inline std::string cell(const ir::Table& table, std::size_t row, std::size_t column,
                                      std::size_t sheet = 0) {
  if (sheet >= table.sheets.size()) return "<no sheet>";
  const auto& rows = table.sheets[sheet].rows;
  if (row >= rows.size() || column >= rows[row].size()) return "<none>";
  return rows[row][column].text;
}

/// True when a writer for `format` and some reader for `source` exist, i.e.
/// a conversion that needs another family can run in this build.
[[nodiscard]] inline bool has_writer(std::string_view format) { return Registry::instance().has_writer(format); }

}  // namespace cvt::test::science
