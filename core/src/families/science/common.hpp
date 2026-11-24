// Convertible core - science family: shared helpers for registration, text
// input and table construction.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "ir/data.hpp"
#include "ir/document.hpp"
#include "ir/image.hpp"
#include "ir/table.hpp"
#include "util/bytes.hpp"
#include "util/json.hpp"

namespace cvt::science {

/// Recursion ceiling for every nested structure a science reader walks
/// (Newick trees, ASN.1, HDF5 groups, MAT cells, DICOM sequences, ...).
inline constexpr int max_nesting = 64;

/// One format description; `add_format` fills a FormatInfo from it.
struct FormatSpec {
  std::string id{};
  std::string name{};
  std::string description{};
  Category category = Category::Data;
  std::vector<std::string> extensions{};
  std::string mime{};
  int popularity = 10;
  std::vector<Signature> signatures{};
  std::function<int(ByteView data, std::string_view filename)> sniff{};
  bool output_only = false;  ///< generated from other content only (barcodes): see FormatInfo::output_only
};
void add_format(Registry& registry, FormatSpec spec);

using ReadFn = std::function<Result<AnyIR>(ReadContext& ctx)>;
using WriteFn = std::function<Result<Bytes>(const AnyIR& value, WriteContext& ctx)>;

/// Register one codec. Pass null functions for an unsupported direction.
void add_codec(Registry& registry, std::string format_id, Kind kind, ReadFn read, WriteFn write,
               std::vector<OptionSpec> read_options = {}, std::vector<OptionSpec> write_options = {},
               int read_cost = 10, int write_cost = 10, bool lossy = false);

// --------------------------------------------------------------------------- text

/// The input decoded to UTF-8 (BOM, UTF-16/32 handled; invalid bytes replaced).
[[nodiscard]] std::string input_text(ByteView input);
/// Lines without their terminators (LF, CRLF or CR).
[[nodiscard]] std::vector<std::string_view> lines_of(std::string_view text);
/// Split on runs of ASCII whitespace.
[[nodiscard]] std::vector<std::string_view> words(std::string_view text);
/// The first `max` bytes of `data` as text, for sniffers.
[[nodiscard]] std::string_view head_text(ByteView data, std::size_t max = 4096);
/// True when `name` ends with ".ext" (case-insensitive).
[[nodiscard]] bool has_extension(std::string_view name, std::string_view ext);
/// Locale-free number formatting with up to `digits` significant digits;
/// the shortest round-trip form when `digits` <= 0.
[[nodiscard]] std::string fmt_num(double value, int digits = 0);
/// Fixed-point formatting with `decimals` digits after the point.
[[nodiscard]] std::string fmt_fixed(double value, int decimals);
[[nodiscard]] std::optional<double> to_double(std::string_view text);
[[nodiscard]] std::optional<std::int64_t> to_int(std::string_view text);

// --------------------------------------------------------------------------- tables

[[nodiscard]] ir::Cell text_cell(std::string_view text);
[[nodiscard]] ir::Cell number_cell(double value);
[[nodiscard]] ir::Cell int_cell(std::int64_t value);
/// A number cell when the text is a plain number, else a string cell (empty -> Empty).
[[nodiscard]] ir::Cell auto_cell(std::string_view text);
/// A sheet whose row 0 is `header`.
[[nodiscard]] ir::Sheet header_sheet(std::string name, const std::vector<std::string>& header);
/// The sheet with this name (case-insensitive), or nullptr.
[[nodiscard]] const ir::Sheet* find_sheet(const ir::Table& table, std::string_view name);
/// Column index of a header title (case-insensitive) in row 0, or -1.
[[nodiscard]] int column_of(const ir::Sheet& sheet, std::string_view title);
/// The cell text at (row, column), "" when absent.
[[nodiscard]] std::string_view cell_text(const ir::Sheet& sheet, std::size_t row, int column);
/// Rows of a "header"/metadata sheet: column 0 of every row.
[[nodiscard]] std::vector<std::string> sheet_lines(const ir::Sheet& sheet);
/// A one-column sheet holding text lines (no header).
[[nodiscard]] ir::Sheet lines_sheet(std::string name, const std::vector<std::string>& lines);
/// Number of value cells of a Table, for limit checks.
[[nodiscard]] Result<void> check_cells(const Context& ctx, std::uint64_t cells);

// --------------------------------------------------------------------------- JSON helpers

/// A JSON number that keeps integers integral.
[[nodiscard]] Json json_number(double value);
/// Flatten a JSON object of scalars into a two-column (key, value) sheet.
[[nodiscard]] ir::Sheet key_value_sheet(std::string name, const Json& object);
/// Render a Data value as a document: headings for objects, tables for
/// arrays of flat objects, paragraphs for scalars.
[[nodiscard]] ir::Document data_document(std::string_view title, const Json& value);

// --------------------------------------------------------------------------- bytes

/// A minimal PNG encoder (8-bit RGBA or grey, zlib level 9), used by the
/// barcode and preview writers that must emit picture bytes themselves.
[[nodiscard]] Result<Bytes> encode_png(const ir::Frame& frame, bool grey);

}  // namespace cvt::science
