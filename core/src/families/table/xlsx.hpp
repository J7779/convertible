// Convertible core - table family: Excel Open XML workbooks (internal).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <vector>

#include "engine/registry.hpp"
#include "ir/table.hpp"

namespace cvt::table {

/// Read an XLSX/XLSM/XLTX package.
[[nodiscard]] Result<ir::Table> read_xlsx_package(ByteView input, const ReadContext& ctx);
/// Write a workbook as XLSX (or the XLSM/XLTX flavour named by ctx.format_id).
[[nodiscard]] Result<Bytes> write_xlsx(const AnyIR& value, WriteContext& ctx);
[[nodiscard]] std::vector<OptionSpec> xlsx_write_options();
/// The XLSB (binary workbook) reader.
void register_xlsb(Registry& registry);

}  // namespace cvt::table
