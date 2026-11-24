// Convertible core - table family: tables in text markup.
//
// Markdown pipe tables, reStructuredText grid and simple tables, AsciiDoc
// tables, Org tables, LaTeX tabular, MediaWiki tables, Jira tables and
// plain-text box drawings. Every reader finds all tables in a document
// (one sheet each) and infers cell types; every writer aligns columns by
// display width (East Asian wide characters count double) and escapes what
// the syntax needs.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/table/common.hpp"
#include "families/table/table.hpp"
#include "util/text.hpp"

namespace cvt::table {

namespace {

using Lines = std::vector<std::string_view>;

// --------------------------------------------------------------------------- shared

struct RawTable {
  std::string name;
  std::vector<std::vector<std::string>> rows;
  bool header = false;
};

Result<ir::Table> build_table(std::vector<RawTable> raw, const ReadContext& ctx) {
  const InferOptions infer = infer_options_from(ctx.options);
  CellBudget budget(ctx.limits);
  ir::Table table;
  for (std::size_t i = 0; i < raw.size(); ++i) {
    CVT_TRY(ctx.limits.check_archive_entries(i + 1));
    ir::Sheet sheet;
    sheet.name = raw[i].name.empty() ? std::format("Table {}", i + 1) : raw[i].name;
    sheet.has_header = raw[i].header && !raw[i].rows.empty();
    for (std::size_t r = 0; r < raw[i].rows.size(); ++r) {
      std::vector<ir::Cell> row;
      CVT_TRY(budget.take(raw[i].rows[r].size() + 1));
      for (std::string& text : raw[i].rows[r]) {
        if (r == 0 && sheet.has_header) {
          row.push_back(text.empty() ? ir::Cell{} : ir::Cell::string(std::move(text)));
        } else {
          row.push_back(infer_cell(std::move(text), infer));
        }
      }
      sheet.rows.push_back(std::move(row));
    }
    trim_sheet(sheet);
    table.sheets.push_back(std::move(sheet));
  }
  if (table.sheets.empty()) return fail(Status::Decode, "no table was found in the document");
  return table;
}

Result<std::string> read_text(const ReadContext& ctx) {
  return decode_source_text(ctx.input, ctx.options.get_string("source_encoding", "auto"), ctx);
}

// Split on `separator`, honouring backslash escapes ("\|") and `code spans`.
std::vector<std::string> split_cells(std::string_view line, char separator, bool backtick_spans) {
  std::vector<std::string> cells;
  std::string current;
  bool in_code = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (c == '\\' && i + 1 < line.size() && line[i + 1] == separator) {
      current.push_back(separator);
      ++i;
    } else if (backtick_spans && c == '`') {
      in_code = !in_code;
      current.push_back(c);
    } else if (c == separator && !in_code) {
      cells.push_back(std::move(current));
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  cells.push_back(std::move(current));
  return cells;
}

std::string trimmed(std::string_view text) { return std::string(util::trim(text)); }

// A grid of display texts for writers.
struct Grid {
  std::vector<std::vector<std::string>> rows;
  std::vector<bool> numeric;  // right-align the column
  bool header = false;
  std::size_t columns = 0;
  std::string name;
};

Grid make_grid(const ir::Sheet& sheet, const std::function<std::string(std::string_view)>& escape) {
  Grid grid;
  grid.header = sheet.has_header;
  grid.columns = sheet.column_count();
  grid.name = sheet.name;
  grid.numeric.assign(grid.columns, true);
  std::vector<bool> any(grid.columns, false);
  for (std::size_t r = 0; r < sheet.rows.size(); ++r) {
    std::vector<std::string> row;
    for (std::size_t c = 0; c < grid.columns; ++c) {
      const ir::Cell& cell = sheet.at(r, c);
      row.push_back(escape(cell.text));
      if (r == 0 && grid.header) continue;
      if (cell.empty()) continue;
      any[c] = true;
      if (cell.type != ir::CellType::Number) grid.numeric[c] = false;
    }
    grid.rows.push_back(std::move(row));
  }
  for (std::size_t c = 0; c < grid.columns; ++c) grid.numeric[c] = grid.numeric[c] && any[c];
  return grid;
}

std::string pad(std::string_view text, std::size_t width, bool right) {
  const std::size_t have = display_width(text);
  const std::size_t fill = width > have ? width - have : 0;
  std::string out;
  if (right) out.append(fill, ' ');
  out.append(text);
  if (!right) out.append(fill, ' ');
  return out;
}

std::vector<std::size_t> column_widths(const Grid& grid, std::size_t minimum) {
  std::vector<std::size_t> widths(grid.columns, minimum);
  for (const auto& row : grid.rows) {
    for (std::size_t c = 0; c < row.size(); ++c) {
      for (const std::string_view line : util::split(row[c], '\n')) widths[c] = std::max(widths[c], display_width(line));
    }
  }
  return widths;
}

std::string replace_newlines(std::string_view text, std::string_view with) {
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r') continue;
    if (text[i] == '\n') {
      out.append(with);
    } else {
      out.push_back(text[i]);
    }
  }
  return out;
}

Result<Bytes> finish_text(std::string text, const WriteContext& ctx) {
  CVT_TRY(ctx.limits.check_output(text.size()));
  return to_bytes(text);
}

// --------------------------------------------------------------------------- Markdown

bool markdown_delimiter_row(std::string_view line) {
  line = util::trim(line);
  if (line.find('-') == std::string_view::npos || line.find('|') == std::string_view::npos) {
    // A single-column table may be "---" without pipes only when the header had pipes; require one.
    return false;
  }
  for (const char c : line) {
    if (c != '|' && c != '-' && c != ':' && c != ' ' && c != '\t') return false;
  }
  return true;
}

std::vector<std::string> markdown_cells(std::string_view line) {
  line = util::trim(line);
  if (line.starts_with('|')) line.remove_prefix(1);
  if (line.ends_with('|') && !line.ends_with("\\|")) line.remove_suffix(1);
  std::vector<std::string> cells = split_cells(line, '|', true);
  for (std::string& cell : cells) {
    cell = trimmed(cell);
    cell = util::replace_all(cell, "<br>", "\n");
    cell = util::replace_all(cell, "<br/>", "\n");
    cell = util::replace_all(cell, "<br />", "\n");
  }
  return cells;
}

Result<AnyIR> read_markdown(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const std::string text, read_text(ctx));
  const Lines lines = util::split_lines(text);
  std::vector<RawTable> tables;
  std::string last_heading;
  for (std::size_t i = 0; i + 1 < lines.size(); ++i) {
    const std::string_view line = util::trim(lines[i]);
    if (line.starts_with('#')) {
      last_heading = trimmed(line.substr(line.find_first_not_of('#') == std::string_view::npos ? line.size() : line.find_first_not_of('#')));
      continue;
    }
    if (line.find('|') == std::string_view::npos || !markdown_delimiter_row(lines[i + 1])) continue;
    RawTable table;
    table.header = true;
    table.name = last_heading;
    table.rows.push_back(markdown_cells(line));
    const std::size_t columns = table.rows[0].size();
    std::size_t j = i + 2;
    for (; j < lines.size(); ++j) {
      const std::string_view row = util::trim(lines[j]);
      if (row.empty() || row.find('|') == std::string_view::npos) break;
      std::vector<std::string> cells = markdown_cells(row);
      cells.resize(columns);  // GFM: extra cells are dropped, missing ones are empty
      table.rows.push_back(std::move(cells));
    }
    tables.push_back(std::move(table));
    last_heading.clear();
    i = j - 1;
  }
  CVT_TRY_ASSIGN(ir::Table table, build_table(std::move(tables), ctx));
  return table;
}

Result<Bytes> write_markdown(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Sheet* sheet, select_sheet(value.get<ir::Table>(), ctx));
  const bool align = ctx.options.get_bool("align_columns", true);
  Grid grid = make_grid(*sheet, [](std::string_view text) {
    std::string out;
    for (const char c : text) {
      if (c == '|') {
        out += "\\|";
      } else if (c == '\n') {
        out += "<br>";
      } else if (c != '\r') {
        out.push_back(c);
      }
    }
    return trimmed(out);
  });
  if (grid.columns == 0) return to_bytes(std::string());
  const std::vector<std::size_t> widths = align ? column_widths(grid, 3) : std::vector<std::size_t>(grid.columns, 3);
  std::string out;
  const auto line = [&](const std::vector<std::string>& cells) {
    out += "|";
    for (std::size_t c = 0; c < grid.columns; ++c) {
      out += " " + (align ? pad(cells[c], widths[c], grid.numeric[c]) : cells[c]) + " |";
    }
    out += "\n";
  };
  // GFM requires a header row: without one, a blank header is written.
  std::size_t first = 0;
  if (grid.header && !grid.rows.empty()) {
    line(grid.rows[0]);
    first = 1;
  } else {
    line(std::vector<std::string>(grid.columns));
  }
  out += "|";
  for (std::size_t c = 0; c < grid.columns; ++c) {
    const std::size_t dashes = align ? widths[c] : 3;
    out += grid.numeric[c] ? " " + std::string(dashes - 1, '-') + ": |" : " " + std::string(dashes, '-') + " |";
  }
  out += "\n";
  for (std::size_t r = first; r < grid.rows.size(); ++r) line(grid.rows[r]);
  return finish_text(std::move(out), ctx);
}

// --------------------------------------------------------------------------- reStructuredText

// Byte offsets of display columns in a line (for slicing by position).
std::vector<std::size_t> column_offsets(std::string_view line) {
  std::vector<std::size_t> offsets;  // offsets[display column] = byte offset
  std::size_t i = 0;
  while (i < line.size()) {
    const util::Utf8Decoded d = util::decode_utf8(line, i);
    const std::size_t length = d.length == 0 ? 1 : d.length;
    const std::size_t width = std::max<std::size_t>(1, display_width(line.substr(i, length)));
    for (std::size_t k = 0; k < width; ++k) offsets.push_back(i);
    i += length;
  }
  offsets.push_back(line.size());
  return offsets;
}

std::string slice_columns(std::string_view line, const std::vector<std::size_t>& offsets, std::size_t from, std::size_t to) {
  if (from >= offsets.size() - 1) return {};
  const std::size_t begin = offsets[from];
  const std::size_t end = to >= offsets.size() - 1 ? line.size() : offsets[to];
  return std::string(line.substr(begin, end > begin ? end - begin : 0));
}

bool is_grid_border(std::string_view line) {
  line = util::trim_right(line);
  if (line.size() < 3 || line.front() != '+' || line.back() != '+') return false;
  for (const char c : line) {
    if (c != '+' && c != '-' && c != '=' && c != ':') return false;
  }
  return true;
}

bool is_simple_border(std::string_view line) {
  line = util::trim_right(line);
  if (line.empty() || line.front() != '=') return false;
  for (const char c : line) {
    if (c != '=' && c != ' ') return false;
  }
  return true;
}

Result<AnyIR> read_rst(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const std::string text, read_text(ctx));
  const Lines lines = util::split_lines(text);
  std::vector<RawTable> tables;
  std::size_t i = 0;
  while (i < lines.size()) {
    const std::string_view line = lines[i];
    if (is_grid_border(line)) {
      // Column boundaries are the '+' positions of the top border.
      const std::vector<std::size_t> top = column_offsets(line);
      std::vector<std::size_t> bounds;
      {
        std::size_t col = 0;
        std::size_t k = 0;
        while (k < line.size()) {
          const util::Utf8Decoded d = util::decode_utf8(line, k);
          if (line[k] == '+') bounds.push_back(col);
          col += 1;
          k += d.length == 0 ? 1 : d.length;
        }
      }
      RawTable table;
      std::vector<std::string> current(bounds.size() > 0 ? bounds.size() - 1 : 0);
      bool has_content = false;
      std::size_t j = i + 1;
      for (; j < lines.size(); ++j) {
        const std::string_view row = lines[j];
        if (is_grid_border(row)) {
          if (has_content) {
            for (std::string& cell : current) {
              while (!cell.empty() && cell.back() == '\n') cell.pop_back();
            }
            table.rows.push_back(current);
            if (row.find('=') != std::string_view::npos && table.rows.size() == 1) table.header = true;
          }
          current.assign(current.size(), std::string());
          has_content = false;
          continue;
        }
        if (!util::trim_left(row).starts_with('|')) break;
        // Writers pad by display width or by code points; splitting on the
        // separators works for both unless a cell itself contains '|'.
        std::vector<std::string> pieces;
        {
          std::string_view body = util::trim(row);
          body.remove_prefix(1);
          if (body.ends_with('|')) body.remove_suffix(1);
          pieces = split_cells(body, '|', false);
        }
        const bool by_separator = pieces.size() + 1 == bounds.size();
        const std::vector<std::size_t> offsets = column_offsets(row);
        for (std::size_t c = 0; c + 1 < bounds.size(); ++c) {
          std::string piece = by_separator ? trimmed(pieces[c]) : trimmed(slice_columns(row, offsets, bounds[c] + 1, bounds[c + 1]));
          if (!piece.empty() && piece.back() == '|') piece.pop_back();
          piece = trimmed(piece);
          if (!current[c].empty() || !piece.empty()) {
            if (!current[c].empty()) current[c] += "\n";
            current[c] += piece;
          }
          has_content = true;
        }
      }
      (void)top;
      if (!table.rows.empty()) tables.push_back(std::move(table));
      i = j;
      continue;
    }
    if (is_simple_border(line) && i + 1 < lines.size()) {
      // Simple table: column extents are the '=' runs.
      std::vector<std::pair<std::size_t, std::size_t>> spans;
      const std::string_view border = util::trim_right(line);
      std::size_t k = 0;
      while (k < border.size()) {
        while (k < border.size() && border[k] == ' ') ++k;
        const std::size_t begin = k;
        while (k < border.size() && border[k] == '=') ++k;
        if (k > begin) spans.emplace_back(begin, k);
      }
      if (spans.size() < 2) {
        ++i;
        continue;
      }
      RawTable table;
      std::size_t borders = 1;
      std::size_t j = i + 1;
      for (; j < lines.size(); ++j) {
        const std::string_view row = lines[j];
        if (is_simple_border(row)) {
          ++borders;
          if (borders == 2 && table.rows.size() == 1 && j + 1 < lines.size() && !is_simple_border(lines[j + 1]) &&
              !util::trim(lines[j + 1]).empty()) {
            table.header = true;
            continue;
          }
          ++j;
          break;
        }
        if (util::trim(row).empty()) continue;
        const std::vector<std::size_t> offsets = column_offsets(row);
        std::vector<std::string> cells;
        for (std::size_t c = 0; c < spans.size(); ++c) {
          const std::size_t end = c + 1 < spans.size() ? spans[c + 1].first : offsets.size();
          cells.push_back(trimmed(slice_columns(row, offsets, spans[c].first, end)));
        }
        table.rows.push_back(std::move(cells));
      }
      if (!table.rows.empty()) tables.push_back(std::move(table));
      i = j;
      continue;
    }
    ++i;
  }
  CVT_TRY_ASSIGN(ir::Table table, build_table(std::move(tables), ctx));
  return table;
}

Result<Bytes> write_rst(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Sheet* sheet, select_sheet(value.get<ir::Table>(), ctx));
  const Grid grid = make_grid(*sheet, [](std::string_view text) { return util::replace_all(text, "\r", ""); });
  if (grid.columns == 0) return to_bytes(std::string());
  const std::vector<std::size_t> widths = column_widths(grid, 1);
  const auto border = [&](char fill) {
    std::string out = "+";
    for (const std::size_t w : widths) out += std::string(w + 2, fill) + "+";
    return out + "\n";
  };
  std::string out = border('-');
  for (std::size_t r = 0; r < grid.rows.size(); ++r) {
    std::vector<std::vector<std::string_view>> parts;
    std::size_t height = 1;
    for (const std::string& cell : grid.rows[r]) {
      parts.push_back(util::split(cell, '\n'));
      height = std::max(height, parts.back().size());
    }
    for (std::size_t h = 0; h < height; ++h) {
      out += "|";
      for (std::size_t c = 0; c < grid.columns; ++c) {
        const std::string_view piece = h < parts[c].size() ? parts[c][h] : std::string_view();
        out += " " + pad(piece, widths[c], grid.numeric[c] && !(r == 0 && grid.header)) + " |";
      }
      out += "\n";
    }
    out += border(r == 0 && grid.header ? '=' : '-');
  }
  return finish_text(std::move(out), ctx);
}

// --------------------------------------------------------------------------- AsciiDoc

bool is_adoc_delimiter(std::string_view line) {
  if (line.size() < 4 || line[0] != '|') return false;
  return line.find_first_not_of('=', 1) == std::string_view::npos;
}

Result<AnyIR> read_asciidoc(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const std::string text, read_text(ctx));
  const Lines lines = util::split_lines(text);
  std::vector<RawTable> tables;
  std::string pending_title;
  bool pending_header_option = false;
  std::size_t pending_columns = 0;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const std::string_view line = util::trim(lines[i]);
    if (line.starts_with('.') && line.size() > 1 && line[1] != '.') {
      pending_title = trimmed(line.substr(1));
      continue;
    }
    if (line.starts_with('[') && line.ends_with(']')) {
      pending_header_option = line.find("header") != std::string_view::npos;
      if (const std::size_t cols = line.find("cols=\""); cols != std::string_view::npos) {
        const std::size_t end = line.find('"', cols + 6);
        const std::string_view spec = line.substr(cols + 6, end == std::string_view::npos ? 0 : end - cols - 6);
        if (const auto n = util::parse_int(spec); n && *n > 0 && *n < 10000) {
          pending_columns = static_cast<std::size_t>(*n);
        } else {
          pending_columns = util::split(spec, ',', false).size();
        }
      }
      continue;
    }
    if (!is_adoc_delimiter(line)) {
      if (!line.empty()) {
        pending_title.clear();
        pending_header_option = false;
        pending_columns = 0;
      }
      continue;
    }
    RawTable table;
    table.name = pending_title;
    std::vector<std::string> cells;
    std::size_t columns = pending_columns;
    bool implicit_header = false;
    std::size_t j = i + 1;
    std::size_t first_line_end_cells = 0;
    for (; j < lines.size(); ++j) {
      const std::string_view row = lines[j];
      if (is_adoc_delimiter(util::trim(row))) break;
      if (util::trim(row).empty()) {
        if (j == i + 2 && first_line_end_cells > 0) implicit_header = true;
        continue;
      }
      if (util::trim_left(row).starts_with('|')) {
        std::vector<std::string> pieces = split_cells(util::trim_left(row).substr(1), '|', false);
        for (std::string& piece : pieces) {
          // Drop cell specifiers such as "2+" or "a" that precede a '|'.
          cells.push_back(trimmed(piece));
        }
        if (j == i + 1) {
          first_line_end_cells = cells.size();
          if (columns == 0) columns = cells.size();
        }
      } else if (!cells.empty()) {
        // Continuation of the previous cell (" +" ends a hard line break);
        // further '|' on the line start new cells.
        std::vector<std::string> pieces = split_cells(row, '|', false);
        std::string& last = cells.back();
        if (last.ends_with(" +")) last.resize(last.size() - 2);
        last += "\n" + trimmed(pieces[0]);
        for (std::size_t k = 1; k < pieces.size(); ++k) cells.push_back(trimmed(pieces[k]));
      }
    }
    for (std::string& cell : cells) {
      if (cell.ends_with(" +")) cell.resize(cell.size() - 2);
      cell = util::replace_all(cell, " +\n", "\n");
    }
    if (columns == 0) columns = std::max<std::size_t>(1, cells.size());
    for (std::size_t k = 0; k < cells.size(); k += columns) {
      std::vector<std::string> row(cells.begin() + static_cast<std::ptrdiff_t>(k),
                                   cells.begin() + static_cast<std::ptrdiff_t>(std::min(cells.size(), k + columns)));
      row.resize(columns);
      table.rows.push_back(std::move(row));
    }
    table.header = pending_header_option || implicit_header;
    if (!table.rows.empty()) tables.push_back(std::move(table));
    pending_title.clear();
    pending_header_option = false;
    pending_columns = 0;
    i = j;
  }
  CVT_TRY_ASSIGN(ir::Table table, build_table(std::move(tables), ctx));
  return table;
}

Result<Bytes> write_asciidoc(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Sheet* sheet, select_sheet(value.get<ir::Table>(), ctx));
  const Grid grid = make_grid(*sheet, [](std::string_view text) {
    return replace_newlines(util::replace_all(text, "|", "\\|"), " +\n");
  });
  if (grid.columns == 0) return to_bytes(std::string());
  std::string out;
  if (!grid.name.empty()) out += "." + replace_newlines(grid.name, " ") + "\n";
  out += std::format("[cols=\"{}\"", grid.columns);
  if (grid.header) out += ", options=\"header\"";
  out += "]\n|===\n";
  for (std::size_t r = 0; r < grid.rows.size(); ++r) {
    for (std::size_t c = 0; c < grid.columns; ++c) out += (c == 0 ? "|" : " |") + std::string(grid.rows[r][c].empty() ? "" : " ") + grid.rows[r][c];
    out += "\n";
    if (r == 0 && grid.header) out += "\n";
  }
  out += "|===\n";
  return finish_text(std::move(out), ctx);
}

// --------------------------------------------------------------------------- Org

Result<AnyIR> read_org(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const std::string text, read_text(ctx));
  const Lines lines = util::split_lines(text);
  std::vector<RawTable> tables;
  std::string name;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const std::string_view line = util::trim(lines[i]);
    if (util::istarts_with(line, "#+name:") || util::istarts_with(line, "#+caption:")) {
      name = trimmed(line.substr(line.find(':') + 1));
      continue;
    }
    if (!line.starts_with('|')) continue;
    RawTable table;
    table.name = name;
    std::size_t j = i;
    bool separator_seen = false;
    for (; j < lines.size(); ++j) {
      const std::string_view row = util::trim(lines[j]);
      if (!row.starts_with('|')) break;
      if (row.starts_with("|-") || row.starts_with("|+")) {
        if (!separator_seen && table.rows.size() == 1) table.header = true;
        separator_seen = true;
        continue;
      }
      std::string_view body = row.substr(1);
      if (body.ends_with('|')) body.remove_suffix(1);
      std::vector<std::string> cells = split_cells(body, '|', false);
      for (std::string& cell : cells) cell = util::replace_all(trimmed(cell), "\\vert{}", "|");
      table.rows.push_back(std::move(cells));
    }
    if (!table.rows.empty()) tables.push_back(std::move(table));
    name.clear();
    i = j - 1;
  }
  CVT_TRY_ASSIGN(ir::Table table, build_table(std::move(tables), ctx));
  return table;
}

Result<Bytes> write_org(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Sheet* sheet, select_sheet(value.get<ir::Table>(), ctx));
  const Grid grid = make_grid(*sheet, [](std::string_view text) {
    return replace_newlines(util::replace_all(text, "|", "\\vert{}"), " ");
  });
  if (grid.columns == 0) return to_bytes(std::string());
  const std::vector<std::size_t> widths = column_widths(grid, 1);
  std::string out;
  if (!grid.name.empty()) out += "#+NAME: " + replace_newlines(grid.name, " ") + "\n";
  for (std::size_t r = 0; r < grid.rows.size(); ++r) {
    out += "|";
    for (std::size_t c = 0; c < grid.columns; ++c) out += " " + pad(grid.rows[r][c], widths[c], grid.numeric[c] && !(r == 0 && grid.header)) + " |";
    out += "\n";
    if (r == 0 && grid.header && grid.rows.size() > 1) {
      out += "|";
      for (std::size_t c = 0; c < grid.columns; ++c) out += std::string(widths[c] + 2, '-') + (c + 1 < grid.columns ? "+" : "|");
      out += "\n";
    }
  }
  return finish_text(std::move(out), ctx);
}

// --------------------------------------------------------------------------- LaTeX

std::string latex_escape(std::string_view text) {
  std::string out;
  for (const char c : text) {
    switch (c) {
      case '\\': out += "\\textbackslash{}"; break;
      case '&': out += "\\&"; break;
      case '%': out += "\\%"; break;
      case '$': out += "\\$"; break;
      case '#': out += "\\#"; break;
      case '_': out += "\\_"; break;
      case '{': out += "\\{"; break;
      case '}': out += "\\}"; break;
      case '~': out += "\\textasciitilde{}"; break;
      case '^': out += "\\textasciicircum{}"; break;
      case '\n': out += " "; break;
      case '\r': break;
      default: out.push_back(c);
    }
  }
  return out;
}

// Strip simple commands: \textbf{x} -> x, \& -> &, \textbackslash{} -> \ ...
std::string latex_unescape(std::string_view text) {
  std::string out;
  std::size_t i = 0;
  while (i < text.size()) {
    const char c = text[i];
    if (c == '\\' && i + 1 < text.size()) {
      const char next = text[i + 1];
      if (std::string_view("&%$#_{} ").find(next) != std::string_view::npos) {
        out.push_back(next);
        i += 2;
        continue;
      }
      std::size_t j = i + 1;
      while (j < text.size() && util::is_ascii_alpha(text[j])) ++j;
      const std::string_view command = text.substr(i + 1, j - i - 1);
      if (command == "textbackslash") {
        out.push_back('\\');
      } else if (command == "textasciitilde") {
        out.push_back('~');
      } else if (command == "textasciicircum") {
        out.push_back('^');
      } else if (command == "multicolumn" || command == "multirow") {
        // Skip the first two arguments; the third is the content.
        for (int arg = 0; arg < 2 && j < text.size(); ++arg) {
          while (j < text.size() && text[j] != '{') ++j;
          int depth = 0;
          for (; j < text.size(); ++j) {
            if (text[j] == '{') ++depth;
            if (text[j] == '}' && --depth == 0) {
              ++j;
              break;
            }
          }
        }
      }
      // Other commands are dropped; their braced argument stays as text.
      if (j < text.size() && text[j] == '{' && j + 1 < text.size() && text[j + 1] == '}') j += 2;
      i = j;
      continue;
    }
    if (c == '{' || c == '}') {
      ++i;
      continue;
    }
    if (c == '~') {
      out.push_back(' ');
      ++i;
      continue;
    }
    out.push_back(c);
    ++i;
  }
  return trimmed(out);
}

Result<AnyIR> read_latex(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const std::string text, read_text(ctx));
  // Drop comments.
  std::string source;
  for (const std::string_view line : util::split_lines(text)) {
    std::size_t cut = line.size();
    for (std::size_t k = 0; k < line.size(); ++k) {
      if (line[k] == '%' && (k == 0 || line[k - 1] != '\\')) {
        cut = k;
        break;
      }
    }
    source.append(line.substr(0, cut));
    source.push_back('\n');
  }
  std::vector<RawTable> tables;
  std::size_t at = 0;
  while (true) {
    std::size_t begin = std::string::npos;
    std::string environment;
    for (const char* env : {"tabular*", "tabularx", "longtable", "tabular"}) {
      const std::string needle = std::string("\\begin{") + env + "}";
      const std::size_t found = source.find(needle, at);
      if (found < begin) {
        begin = found;
        environment = env;
      }
    }
    if (begin == std::string::npos) break;
    std::size_t pos = begin + 8 + environment.size();
    // Skip arguments: {width} for tabular*/tabularx, then the column spec, plus an optional [pos].
    const int arguments = environment == "tabular*" || environment == "tabularx" ? 2 : 1;
    for (int arg = 0; arg < arguments; ++arg) {
      while (pos < source.size() && (util::is_ascii_space(source[pos]) || source[pos] == '[')) {
        if (source[pos] == '[') {
          const std::size_t close = source.find(']', pos);
          pos = close == std::string::npos ? source.size() : close + 1;
        } else {
          ++pos;
        }
      }
      if (pos >= source.size() || source[pos] != '{') break;
      int depth = 0;
      for (; pos < source.size(); ++pos) {
        if (source[pos] == '{') ++depth;
        if (source[pos] == '}' && --depth == 0) {
          ++pos;
          break;
        }
      }
    }
    const std::string end_marker = "\\end{" + environment + "}";
    const std::size_t end = source.find(end_marker, pos);
    if (end == std::string::npos) break;
    std::string body = source.substr(pos, end - pos);
    at = end + end_marker.size();
    RawTable table;
    // Rules after the first row mark it as a header.
    for (const char* rule : {"\\toprule", "\\bottomrule", "\\endhead", "\\endfirsthead"}) body = util::replace_all(body, rule, "");
    body = util::replace_all(body, "\\midrule", "\\hline");
    std::size_t start = 0;
    bool previous_rule = false;
    std::size_t rows_before_rule = 0;
    bool header_rule = false;
    while (start < body.size()) {
      std::size_t split = std::string::npos;
      for (std::size_t k = start; k + 1 < body.size(); ++k) {
        if (body[k] == '\\' && body[k + 1] == '\\') {
          split = k;
          break;
        }
        if (body[k] == '\\') ++k;  // skip escaped characters
      }
      std::string row = body.substr(start, split == std::string::npos ? std::string::npos : split - start);
      start = split == std::string::npos ? body.size() : split + 2;
      // Line rules before the row content.
      bool rule = false;
      for (const char* line_rule : {"\\hline", "\\cline"}) {
        std::size_t p = 0;
        while ((p = row.find(line_rule, p)) != std::string::npos) {
          std::size_t e = p + std::string_view(line_rule).size();
          if (row[p + 1] == 'c') {
            const std::size_t close = row.find('}', e);
            e = close == std::string::npos ? row.size() : close + 1;
          }
          row.erase(p, e - p);
          rule = true;
        }
      }
      if (rule && table.rows.size() == 1 && rows_before_rule == 0) header_rule = true;
      if (rule) rows_before_rule = table.rows.size();
      previous_rule = rule;
      if (util::trim(row).empty()) continue;
      std::vector<std::string> cells;
      std::string current;
      for (std::size_t k = 0; k < row.size(); ++k) {
        if (row[k] == '\\' && k + 1 < row.size()) {
          current.push_back(row[k]);
          current.push_back(row[k + 1]);
          ++k;
        } else if (row[k] == '&') {
          cells.push_back(latex_unescape(current));
          current.clear();
        } else {
          current.push_back(row[k]);
        }
      }
      cells.push_back(latex_unescape(current));
      table.rows.push_back(std::move(cells));
    }
    (void)previous_rule;
    table.header = header_rule && table.rows.size() > 1;
    if (!table.rows.empty()) tables.push_back(std::move(table));
  }
  CVT_TRY_ASSIGN(ir::Table table, build_table(std::move(tables), ctx));
  return table;
}

Result<Bytes> write_latex(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Sheet* sheet, select_sheet(value.get<ir::Table>(), ctx));
  const Grid grid = make_grid(*sheet, latex_escape);
  if (grid.columns == 0) return to_bytes(std::string());
  const bool booktabs = ctx.options.get_bool("booktabs", false);
  std::string spec;
  for (std::size_t c = 0; c < grid.columns; ++c) spec.push_back(grid.numeric[c] ? 'r' : 'l');
  const std::vector<std::size_t> widths = column_widths(grid, 1);
  std::string out = "\\begin{tabular}{" + spec + "}\n" + (booktabs ? "\\toprule\n" : "\\hline\n");
  for (std::size_t r = 0; r < grid.rows.size(); ++r) {
    for (std::size_t c = 0; c < grid.columns; ++c) {
      std::string cell = grid.rows[r][c];
      if (r == 0 && grid.header && !cell.empty()) cell = "\\textbf{" + cell + "}";
      out += (c == 0 ? "" : " & ") + pad(cell, widths[c] + (r == 0 && grid.header ? 9 : 0), false);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    out += " \\\\\n";
    if (r == 0 && grid.header) out += booktabs ? "\\midrule\n" : "\\hline\n";
  }
  out += booktabs ? "\\bottomrule\n" : "\\hline\n";
  out += "\\end{tabular}\n";
  return finish_text(std::move(out), ctx);
}

// --------------------------------------------------------------------------- MediaWiki

std::string strip_comments(std::string_view text) {
  std::string out;
  std::size_t i = 0;
  while (i < text.size()) {
    const std::size_t open = text.find("<!--", i);
    if (open == std::string_view::npos) {
      out.append(text.substr(i));
      break;
    }
    out.append(text.substr(i, open - i));
    const std::size_t close = text.find("-->", open + 4);
    i = close == std::string_view::npos ? text.size() : close + 3;
  }
  return out;
}

std::string wiki_cell_content(std::string_view raw) {
  const std::string without_comments = strip_comments(raw);
  std::string_view cell = without_comments;
  // "style=... | content": the attributes end at the first single '|' outside links.
  int link_depth = 0;
  for (std::size_t k = 0; k < cell.size(); ++k) {
    if (cell.substr(k).starts_with("[[") || cell.substr(k).starts_with("{{")) ++link_depth;
    if (cell.substr(k).starts_with("]]") || cell.substr(k).starts_with("}}")) --link_depth;
    if (cell[k] == '|' && link_depth <= 0) {
      cell = cell.substr(k + 1);
      break;
    }
  }
  std::string out = trimmed(cell);
  for (const char* br : {"<br>", "<br/>", "<br />"}) out = util::replace_all(out, br, "\n");
  out = util::replace_all(out, "&#124;", "|");
  out = util::replace_all(out, "'''", "");
  out = util::replace_all(out, "''", "");
  return out;
}

Result<AnyIR> read_mediawiki(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const std::string text, read_text(ctx));
  const Lines lines = util::split_lines(text);
  std::vector<RawTable> tables;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (!util::trim(lines[i]).starts_with("{|")) continue;
    RawTable table;
    std::vector<std::string> row;
    bool row_all_header = true;
    bool first_row = true;
    int nesting = 0;
    const auto end_row = [&] {
      if (row.empty()) return;
      if (first_row && row_all_header) table.header = true;
      first_row = false;
      table.rows.push_back(std::move(row));
      row.clear();
      row_all_header = true;
    };
    std::size_t j = i + 1;
    for (; j < lines.size(); ++j) {
      const std::string_view line = util::trim(lines[j]);
      if (line.starts_with("{|")) {
        ++nesting;  // nested tables are skipped
        continue;
      }
      if (line.starts_with("|}")) {
        if (nesting > 0) {
          --nesting;
          continue;
        }
        break;
      }
      if (nesting > 0) continue;
      if (line.starts_with("|+")) {
        table.name = wiki_cell_content(line.substr(2));
      } else if (line.starts_with("|-")) {
        end_row();
      } else if (line.starts_with('!')) {
        std::string body(line.substr(1));
        body = util::replace_all(body, "||", "!!");
        size_t p = 0;
        std::string_view rest = body;
        while (true) {
          p = rest.find("!!");
          row.push_back(wiki_cell_content(rest.substr(0, p)));
          if (p == std::string_view::npos) break;
          rest.remove_prefix(p + 2);
        }
      } else if (line.starts_with('|')) {
        std::string_view rest = line.substr(1);
        while (true) {
          const std::size_t p = rest.find("||");
          row.push_back(wiki_cell_content(rest.substr(0, p)));
          row_all_header = false;
          if (p == std::string_view::npos) break;
          rest.remove_prefix(p + 2);
        }
      } else if (!row.empty() && !line.empty()) {
        row.back() += "\n" + std::string(line);
      }
    }
    end_row();
    if (!table.rows.empty()) tables.push_back(std::move(table));
    i = j;
  }
  CVT_TRY_ASSIGN(ir::Table table, build_table(std::move(tables), ctx));
  return table;
}

Result<Bytes> write_mediawiki(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Sheet* sheet, select_sheet(value.get<ir::Table>(), ctx));
  const Grid grid = make_grid(*sheet, [](std::string_view text) {
    return replace_newlines(util::replace_all(util::replace_all(text, "|", "&#124;"), "!", "&#33;"), "<br />");
  });
  std::string out = "{| class=\"wikitable\"\n";
  if (!grid.name.empty()) out += "|+ " + replace_newlines(grid.name, " ") + "\n";
  for (std::size_t r = 0; r < grid.rows.size(); ++r) {
    if (r > 0) out += "|-\n";
    const bool header = r == 0 && grid.header;
    out += header ? "! " : "| ";
    for (std::size_t c = 0; c < grid.columns; ++c) {
      if (c > 0) out += header ? " !! " : " || ";
      if (!header && grid.numeric[c] && !grid.rows[r][c].empty()) out += "style=\"text-align:right\" | ";
      out += grid.rows[r][c];
    }
    out += "\n";
  }
  out += "|}\n";
  return finish_text(std::move(out), ctx);
}

// --------------------------------------------------------------------------- Jira

Result<AnyIR> read_jira(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const std::string text, read_text(ctx));
  const Lines lines = util::split_lines(text);
  std::vector<RawTable> tables;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (!util::trim(lines[i]).starts_with('|')) continue;
    RawTable table;
    std::size_t j = i;
    for (; j < lines.size(); ++j) {
      std::string_view line = util::trim(lines[j]);
      if (!line.starts_with('|')) break;
      const bool header = line.starts_with("||");
      std::string body(line);
      if (header) body = util::replace_all(body, "||", "|");
      std::string_view view = body;
      view.remove_prefix(1);
      if (view.ends_with('|') && !view.ends_with("\\|")) view.remove_suffix(1);
      std::vector<std::string> cells = split_cells(view, '|', false);
      for (std::string& cell : cells) {
        cell = util::replace_all(trimmed(cell), "\\\\", "\n");
      }
      if (header && table.rows.empty()) table.header = true;
      table.rows.push_back(std::move(cells));
    }
    if (!table.rows.empty()) tables.push_back(std::move(table));
    i = j - 1;
  }
  CVT_TRY_ASSIGN(ir::Table table, build_table(std::move(tables), ctx));
  return table;
}

Result<Bytes> write_jira(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Sheet* sheet, select_sheet(value.get<ir::Table>(), ctx));
  const Grid grid = make_grid(*sheet, [](std::string_view text) {
    std::string out = replace_newlines(util::replace_all(text, "|", "\\|"), "\\\\");
    return out.empty() ? std::string(" ") : out;
  });
  std::string out;
  for (std::size_t r = 0; r < grid.rows.size(); ++r) {
    const std::string_view separator = r == 0 && grid.header ? "||" : "|";
    out += separator;
    for (std::size_t c = 0; c < grid.columns; ++c) out += grid.rows[r][c] + std::string(separator);
    out += "\n";
  }
  return finish_text(std::move(out), ctx);
}

int sniff_jira(ByteView data, std::string_view) {
  const std::string_view head = as_chars(data).substr(0, 4096);
  const Lines lines = util::split_lines(head);
  if (lines.size() < 2 || !util::trim(lines[0]).starts_with("||") || !util::trim(lines[0]).ends_with("||")) return 0;
  return util::trim(lines[1]).starts_with('|') && !util::trim(lines[1]).starts_with("|-") ? 72 : 0;
}

// --------------------------------------------------------------------------- box drawings

bool is_box_border(std::string_view line) {
  line = util::trim(line);
  if (line.empty()) return false;
  std::size_t i = 0;
  bool any_rule = false;
  while (i < line.size()) {
    const util::Utf8Decoded d = util::decode_utf8(line, i);
    i += d.length == 0 ? 1 : d.length;
    const char32_t c = d.code_point;
    if (c == U'-' || c == U'=' || c == U'─' || c == U'━' || c == U'═') {
      any_rule = true;
      continue;
    }
    if (c == U'+' || c == U'|' || c == U':' || c == U' ' || (c >= 0x2500 && c <= 0x257F)) continue;
    return false;
  }
  return any_rule;
}

bool box_separator(char32_t c) { return c == U'|' || c == U'│' || c == U'┃' || c == U'║'; }

std::vector<std::string> box_cells(std::string_view line) {
  // A line drawn with box-drawing characters keeps a literal '|' as text.
  const bool unicode = line.find("\u2502") != std::string_view::npos || line.find("\u2503") != std::string_view::npos ||
                       line.find("\u2551") != std::string_view::npos;
  const auto separator = [&](char32_t c) { return unicode ? (c != U'|' && box_separator(c)) : c == U'|'; };
  std::vector<std::string> cells;
  std::string current;
  std::size_t i = 0;
  bool started = false;
  const std::string_view trimmed_line = util::trim(line);
  bool leading = true;
  while (i < trimmed_line.size()) {
    const util::Utf8Decoded d = util::decode_utf8(trimmed_line, i);
    const std::size_t length = d.length == 0 ? 1 : d.length;
    if (separator(d.code_point)) {
      if (!(leading && current.empty())) cells.push_back(trimmed(current));
      current.clear();
      started = true;
    } else {
      current.append(trimmed_line.substr(i, length));
    }
    leading = false;
    i += length;
  }
  if (!util::trim(current).empty() || !started) cells.push_back(trimmed(current));
  return cells;
}

Result<AnyIR> read_box(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const std::string text, read_text(ctx));
  const Lines lines = util::split_lines(text);
  std::vector<RawTable> tables;
  // Content lines grouped by the border lines between them.
  std::vector<std::vector<std::vector<std::string>>> groups;
  std::vector<std::vector<std::string>> group;
  bool in_table = false;
  const auto end_group = [&] {
    if (!group.empty()) groups.push_back(std::move(group));
    group.clear();
  };
  const auto close = [&] {
    end_group();
    if (!groups.empty()) {
      RawTable table;
      // Rows drawn on several lines are separated by rules: then every
      // group is one row. Otherwise every line is a row, and a rule after
      // the first line marks the header.
      bool multi_line = groups.size() > 2;
      for (const auto& g : groups) multi_line = multi_line && !g.empty();
      bool any_tall = false;
      for (const auto& g : groups) any_tall = any_tall || g.size() > 1;
      if (multi_line && groups.front().size() == 1 && (any_tall || groups.size() > 2)) {
        for (const auto& g : groups) {
          std::vector<std::string> row;
          for (const auto& line : g) {
            if (row.size() < line.size()) row.resize(line.size());
            for (std::size_t c = 0; c < line.size(); ++c) {
              if (line[c].empty()) continue;
              if (!row[c].empty()) row[c] += "\n";
              row[c] += line[c];
            }
          }
          table.rows.push_back(std::move(row));
        }
        table.header = table.rows.size() > 1;
      } else {
        for (auto& g : groups) {
          for (auto& line : g) table.rows.push_back(std::move(line));
        }
        table.header = groups.size() >= 2 && groups.front().size() == 1 && table.rows.size() > 1;
      }
      tables.push_back(std::move(table));
    }
    groups.clear();
    in_table = false;
  };
  for (const std::string_view line : lines) {
    if (util::trim(line).empty()) {
      close();
      continue;
    }
    if (is_box_border(line)) {
      in_table = true;
      end_group();
      continue;
    }
    bool has_separator = false;
    for (std::size_t i = 0; i < line.size();) {
      const util::Utf8Decoded d = util::decode_utf8(line, i);
      has_separator = has_separator || box_separator(d.code_point);
      i += d.length == 0 ? 1 : d.length;
    }
    if (!has_separator) {
      if (in_table) close();
      continue;
    }
    in_table = true;
    group.push_back(box_cells(line));
  }
  close();
  CVT_TRY_ASSIGN(ir::Table table, build_table(std::move(tables), ctx));
  return table;
}

Result<Bytes> write_box(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Sheet* sheet, select_sheet(value.get<ir::Table>(), ctx));
  const Grid grid = make_grid(*sheet, [](std::string_view text) { return util::replace_all(text, "\r", ""); });
  if (grid.columns == 0) return to_bytes(std::string());
  const std::string style = ctx.options.get_enum("box_style", "unicode", {"unicode", "ascii", "rounded", "double"});
  struct Glyphs {
    std::string_view h, v, tl, tm, tr, ml, mm, mr, bl, bm, br, hh, hl, hm, hr;
  };
  Glyphs g{"─", "│", "┌", "┬", "┐", "├", "┼", "┤", "└", "┴", "┘", "─", "├", "┼", "┤"};
  if (style == "ascii") g = {"-", "|", "+", "+", "+", "+", "+", "+", "+", "+", "+", "=", "+", "+", "+"};
  if (style == "rounded") g = {"─", "│", "╭", "┬", "╮", "├", "┼", "┤", "╰", "┴", "╯", "─", "├", "┼", "┤"};
  if (style == "double") g = {"═", "║", "╔", "╦", "╗", "╠", "╬", "╣", "╚", "╩", "╝", "═", "╠", "╬", "╣"};
  if (style == "unicode") {
    g.hh = "═";
    g.hl = "╞";
    g.hm = "╪";
    g.hr = "╡";
  }
  const std::vector<std::size_t> widths = column_widths(grid, 1);
  bool tall = false;
  for (const auto& row : grid.rows) {
    for (const std::string& cell : row) tall = tall || cell.find('\n') != std::string::npos;
  }
  const auto rule = [&](std::string_view left, std::string_view middle, std::string_view right, std::string_view fill) {
    std::string out(left);
    for (std::size_t c = 0; c < grid.columns; ++c) {
      for (std::size_t k = 0; k < widths[c] + 2; ++k) out += fill;
      out += c + 1 < grid.columns ? middle : right;
    }
    return out + "\n";
  };
  std::string out = rule(g.tl, g.tm, g.tr, g.h);
  for (std::size_t r = 0; r < grid.rows.size(); ++r) {
    std::vector<std::vector<std::string_view>> parts;
    std::size_t height = 1;
    for (const std::string& cell : grid.rows[r]) {
      parts.push_back(util::split(cell, '\n'));
      height = std::max(height, parts.back().size());
    }
    for (std::size_t h = 0; h < height; ++h) {
      out += g.v;
      for (std::size_t c = 0; c < grid.columns; ++c) {
        const std::string_view piece = h < parts[c].size() ? parts[c][h] : std::string_view();
        out += " " + pad(piece, widths[c], grid.numeric[c] && !(r == 0 && grid.header)) + " " + std::string(g.v);
      }
      out += "\n";
    }
    if (r == 0 && grid.header && grid.rows.size() > 1) {
      out += rule(g.hl, g.hm, g.hr, g.hh);
    } else if (tall && r + 1 < grid.rows.size()) {
      out += rule(g.ml, g.mm, g.mr, g.h);  // rows of several lines need rules between them
    }
  }
  out += rule(g.bl, g.bm, g.br, g.h);
  return finish_text(std::move(out), ctx);
}

int sniff_box(ByteView data, std::string_view) {
  const std::string_view head = as_chars(data).substr(0, 4096);
  const Lines lines = util::split_lines(head);
  if (lines.size() < 3) return 0;
  const std::string_view first = util::trim(lines[0]);
  const bool ascii = first.starts_with("+-") && first.ends_with('+');
  const bool unicode = first.starts_with("┌") || first.starts_with("╔") || first.starts_with("╭");
  if (!ascii && !unicode) return 0;
  if (!is_box_border(first)) return 0;
  const std::string_view second = util::trim(lines[1]);
  return second.starts_with('|') || second.starts_with("│") || second.starts_with("║") ? 70 : 0;
}

// --------------------------------------------------------------------------- registration

struct TextFormat {
  std::string_view id;
  std::string_view name;
  std::string_view description;
  std::vector<std::string> extensions;
  std::string_view mime;
  int popularity;
  Result<AnyIR> (*read)(ReadContext&);
  Result<Bytes> (*write)(const AnyIR&, WriteContext&);
  std::function<int(ByteView, std::string_view)> sniff;  // only for formats no other family owns
  std::vector<OptionSpec> write_options;
};

}  // namespace

void register_text_tables(Registry& registry) {
  std::vector<TextFormat> formats;
  formats.push_back({"md", "Markdown", "Markdown text; tables use the GitHub pipe syntax.", {"md", "markdown"},
                     "text/markdown", 85, read_markdown, write_markdown, nullptr,
                     {OptionSpec::boolean("align_columns", "Align columns", true,
                                          "Pad cells so the columns line up in plain text.")}});
  formats.push_back({"rst", "reStructuredText", "reStructuredText markup; tables are written as grid tables.", {"rst"},
                     "text/x-rst", 30, read_rst, write_rst, nullptr, {}});
  formats.push_back({"adoc", "AsciiDoc", "AsciiDoc markup.", {"adoc", "asciidoc"}, "text/asciidoc", 25, read_asciidoc,
                     write_asciidoc, nullptr, {}});
  formats.push_back({"org", "Org mode", "Emacs Org mode document.", {"org"}, "text/org", 20, read_org, write_org, nullptr, {}});
  formats.push_back({"tex", "LaTeX source", "LaTeX document; tables are tabular environments.", {"tex", "latex"},
                     "application/x-tex", 40, read_latex, write_latex, nullptr,
                     {OptionSpec::boolean("booktabs", "Booktabs rules", false,
                                          "Use \\toprule, \\midrule and \\bottomrule (needs the booktabs package).")}});
  formats.push_back({"mediawiki", "MediaWiki markup", "Wiki text as used by Wikipedia.", {"wiki", "mediawiki"},
                     "text/x-wiki", 20, read_mediawiki, write_mediawiki, nullptr, {}});
  formats.push_back({"jira", "Jira wiki table", "Table in the wiki markup of Jira and Confluence.", {"jira"},
                     "text/plain", 15, read_jira, write_jira, sniff_jira, {}});
  formats.push_back({"text-table", "Text table", "Table drawn with box-drawing characters for terminals and plain text.",
                     {"txt"}, "text/plain", 20, read_box, write_box, sniff_box,
                     {OptionSpec::choice("box_style", "Border style", "unicode", {"unicode", "ascii", "rounded", "double"},
                                         "Characters used for the borders.")}});
  for (TextFormat& format : formats) {
    FormatInfo info;
    info.id = std::string(format.id);
    info.name = std::string(format.name);
    info.description = std::string(format.description);
    info.category = format.sniff ? Category::Spreadsheet : Category::Document;
    info.extensions = format.extensions;
    info.mime = std::string(format.mime);
    info.popularity = format.popularity;
    if (format.sniff) {
      info.sniff = format.sniff;
      registry.add_format(std::move(info));
    } else {
      // Markup formats belong to the document family, which describes and
      // detects them; this description only fills gaps.
      registry.add_known_format(std::move(info));
    }

    // Reading keeps only the tables of a document, so it is lossy and more
    // expensive than a document reader for the same format.
    Codec reader;
    reader.format_id = std::string(format.id);
    reader.kind = Kind::Table;
    reader.read = format.read;
    reader.read_cost = 30;
    reader.lossy = true;
    reader.read_options = infer_option_specs();
    reader.read_options.push_back(source_encoding_option());
    registry.add_codec(std::move(reader));

    Codec writer;
    writer.format_id = std::string(format.id);
    writer.kind = Kind::Table;
    writer.write = format.write;
    writer.write_options = std::move(format.write_options);
    writer.write_options.push_back(sheet_option());
    registry.add_codec(std::move(writer));
  }
}

}  // namespace cvt::table
