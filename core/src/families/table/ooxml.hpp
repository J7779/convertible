// Convertible core - table family: Office Open XML package helpers shared by
// the XLSX reader and writer and the XLSB reader.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "engine/context.hpp"
#include "engine/registry.hpp"
#include "util/zip.hpp"

namespace cvt::table {

struct Relationship {
  std::string id;
  std::string type;    ///< full type URI
  std::string target;  ///< resolved part name, without a leading '/'
  bool external = false;
};

/// "xl/_rels/workbook.xml.rels" for "xl/workbook.xml".
[[nodiscard]] std::string rels_path_for(std::string_view part);
/// Resolve a relationship target against the folder of `source_part`.
[[nodiscard]] std::string resolve_part_name(std::string_view source_part, std::string_view target);
/// Relationships of a part (empty when it has none).
[[nodiscard]] Result<std::vector<Relationship>> read_relationships(util::ZipReader& zip, std::string_view source_part);
/// The main workbook part named by the package relationships, falling back
/// to `fallback` when the package has none.
[[nodiscard]] Result<std::string> main_part(util::ZipReader& zip, std::string_view fallback);
/// The value of a ZIP member read under the reader's limits, as UTF-8 XML text.
[[nodiscard]] Result<std::string> read_xml_part(util::ZipReader& zip, std::string_view name);

/// Which flavour an Excel ZIP package is, from its [Content_Types].xml:
/// "xlsx", "xlsm", "xltx", "xltm", "xlsb", or empty when it is not a workbook.
[[nodiscard]] std::string excel_package_kind(ByteView data);

}  // namespace cvt::table
