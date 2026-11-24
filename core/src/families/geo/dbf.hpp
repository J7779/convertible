// Convertible core - geo family: a minimal dBase (.dbf) table reader and
// writer for shapefile and MapInfo attributes.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "util/bytes.hpp"
#include "util/json.hpp"
#include "util/text.hpp"

namespace cvt::geo::dbf {

struct Field {
  std::string name;
  char type = 'C';            ///< C, N, F, L, D, M, I, O, ...
  std::uint8_t length = 0;
  std::uint8_t decimals = 0;
};

struct Table {
  std::vector<Field> fields;
  /// One JSON object per record (deleted records are skipped), with typed
  /// values: C -> string, N/F -> integer or double, L -> bool, D ->
  /// "YYYY-MM-DD", empty values -> null.
  std::vector<Json> records;
};

/// The text encoding named by a shapefile .cpg file ("UTF-8", "1252",
/// "ISO-8859-1", ...); nullopt when unknown.
[[nodiscard]] std::optional<util::TextEncoding> encoding_from_cpg(std::string_view cpg);

/// Parses a .dbf file. `encoding` (from the .cpg) wins; otherwise the
/// language driver byte decides, and text that is valid UTF-8 is read as such.
[[nodiscard]] Result<Table> read(ByteView data, std::optional<util::TextEncoding> encoding, const Context& ctx);

/// Writes a dBase III table (UTF-8 text) whose fields are inferred from the
/// property objects: booleans -> L, whole numbers -> N, other numbers -> N
/// with decimals, everything else -> C (up to 254 bytes, with a warning
/// when text had to be cut). Field names are shortened to 10 bytes and made
/// unique; `names` receives the property key of every field.
[[nodiscard]] Result<Bytes> write(const std::vector<const Json*>& rows, const Context& ctx,
                                  std::vector<std::string>* names = nullptr);

}  // namespace cvt::geo::dbf
