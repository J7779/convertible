// Convertible core - table family: what a sheet becomes as an SQL table
// (shared by the SQLite and SQL dump writers).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "ir/table.hpp"

namespace cvt::table {

struct SqlColumns {
  std::vector<std::string> names;  ///< unique (case-insensitively), never empty
  /// Declared types from the content: "INTEGER", "REAL", "TEXT", "BOOLEAN",
  /// "DATETIME", or "" for columns mixing kinds.
  std::vector<std::string> types;
  std::size_t first_data_row = 0;  ///< 1 when row 0 provided the names
};

[[nodiscard]] SqlColumns sql_columns(const ir::Sheet& sheet, bool use_header);

}  // namespace cvt::table
