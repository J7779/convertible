// Convertible core - table family: CSV, TSV, PSV, semicolon CSV and
// fixed-width text.
//
// Reading follows RFC 4180 and tolerates what real files do: any line
// ending, a byte order mark or UTF-16 encoding, an Excel "sep=" hint line,
// stray quotes, unterminated quoted fields, ragged rows. The dialect
// (delimiter, quote, header row, encoding) is sniffed unless the caller
// fixes it with options.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/table/delimited.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/table/common.hpp"
#include "families/table/table.hpp"
#include "util/text.hpp"

namespace cvt::table {

using namespace std::string_view_literals;

// --------------------------------------------------------------------------- parsing

Result<void> parse_delimited(std::string_view text, const Dialect& dialect,
                             const std::function<Result<void>(std::vector<std::string>&&)>& on_record,
                             bool* unterminated) {
  std::vector<std::string> record;
  std::string field;
  std::size_t i = 0;
  if (dialect.skip_first_line) {
    while (i < text.size() && text[i] != '\n' && text[i] != '\r') ++i;
    if (i < text.size() && text[i] == '\r') ++i;
    if (i < text.size() && text[i] == '\n') ++i;
  }
  const char quote = dialect.quote;
  const char delimiter = dialect.delimiter;
  const char escape = dialect.escape;
  bool field_started = false;  // anything (even an empty quoted field) seen in this record
  const auto end_field = [&] {
    record.push_back(std::move(field));
    field.clear();
  };
  const auto end_record = [&]() -> Result<void> {
    if (!field_started && record.empty() && field.empty()) {
      return on_record({});  // blank line
    }
    end_field();
    Result<void> result = on_record(std::move(record));
    record.clear();
    field_started = false;
    return result;
  };
  while (i < text.size()) {
    char c = text[i];
    if (quote != 0 && c == quote && field.empty()) {
      // Quoted field.
      field_started = true;
      ++i;
      bool closed = false;
      while (i < text.size()) {
        c = text[i];
        if (escape != 0 && escape != quote && c == escape && i + 1 < text.size()) {
          field.push_back(text[i + 1]);
          i += 2;
        } else if (c == quote) {
          if (i + 1 < text.size() && text[i + 1] == quote) {
            field.push_back(quote);
            i += 2;
          } else {
            ++i;
            closed = true;
            break;
          }
        } else {
          field.push_back(c);
          ++i;
        }
      }
      if (!closed) {
        if (unterminated != nullptr) *unterminated = true;
        break;
      }
      // Anything up to the next delimiter or line end is kept literally ("a"b -> ab).
      while (i < text.size() && text[i] != delimiter && text[i] != '\n' && text[i] != '\r') {
        field.push_back(text[i]);
        ++i;
      }
      continue;
    }
    if (c == delimiter) {
      field_started = true;
      end_field();
      ++i;
    } else if (c == '\n' || c == '\r') {
      if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
      ++i;
      CVT_TRY(end_record());
    } else if (escape != 0 && escape != quote && c == escape && i + 1 < text.size()) {
      field_started = true;
      field.push_back(text[i + 1]);
      i += 2;
    } else {
      field_started = true;
      field.push_back(c);
      ++i;
    }
  }
  if (field_started || !field.empty() || !record.empty()) {
    end_field();
    CVT_TRY(on_record(std::move(record)));
  }
  return {};
}

Dialect sniff_dialect(std::string_view text, char preferred) {
  Dialect dialect;
  dialect.delimiter = preferred;
  // Excel's "sep=;" hint.
  {
    const std::size_t end = text.find_first_of("\r\n");
    const std::string_view first = text.substr(0, end);
    if (first.size() == 5 && util::istarts_with(first, "sep=")) {
      dialect.delimiter = first[4];
      dialect.skip_first_line = true;
      return dialect;
    }
  }
  std::string_view sample = text.substr(0, 64 * 1024);
  if (sample.size() < text.size()) {
    const std::size_t cut = sample.find_last_of('\n');
    if (cut != std::string_view::npos && cut > 0) sample = sample.substr(0, cut);
  }
  std::vector<char> candidates = {preferred};
  for (const char c : {',', '\t', ';', '|', ':', '^', '~'}) {
    if (c != preferred) candidates.push_back(c);
  }
  double best_score = 0.0;
  for (const char candidate : candidates) {
    Dialect trial;
    trial.delimiter = candidate;
    std::map<std::size_t, std::size_t> counts;
    std::size_t records = 0;
    (void)parse_delimited(sample, trial, [&](std::vector<std::string>&& record) -> Result<void> {
      if (record.empty()) return {};
      if (records >= 200) return fail(Status::Cancelled, "enough");
      ++counts[record.size()];
      ++records;
      return {};
    });
    if (records == 0) continue;
    std::size_t mode = 0;
    std::size_t mode_count = 0;
    for (const auto& [fields, count] : counts) {
      if (count > mode_count || (count == mode_count && fields > mode)) {
        mode = fields;
        mode_count = count;
      }
    }
    if (mode < 2) continue;
    // Consistency first; more fields as a small tie-breaker.
    const double score =
        static_cast<double>(mode_count) / static_cast<double>(records) + std::min<double>(static_cast<double>(mode), 50.0) * 1e-4;
    if (score > best_score + 1e-9) {
      best_score = score;
      dialect.delimiter = candidate;
    }
  }
  return dialect;
}

namespace {

// --------------------------------------------------------------------------- reading

char delimiter_from_option(std::string_view value, char fallback) {
  if (value.empty() || value == "auto") return 0;
  if (value == "tab" || value == "\\t") return '\t';
  if (value == "space") return ' ';
  if (value == "comma") return ',';
  if (value == "semicolon") return ';';
  if (value == "pipe") return '|';
  if (value.size() == 1) return value[0];
  return fallback;
}

char default_delimiter(std::string_view format_id) {
  if (format_id == "tsv") return '\t';
  if (format_id == "psv") return '|';
  if (format_id == "csv-semicolon") return ';';
  return ',';
}

std::string file_stem(std::string_view filename) {
  const std::size_t slash = filename.find_last_of("/\\");
  if (slash != std::string_view::npos) filename.remove_prefix(slash + 1);
  const std::size_t dot = filename.rfind('.');
  if (dot != std::string_view::npos && dot > 0) filename = filename.substr(0, dot);
  return std::string(filename);
}

Result<AnyIR> read_delimited(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const std::string text,
                 decode_source_text(ctx.input, ctx.options.get_string("source_encoding", "auto"), ctx));
  const char preferred = default_delimiter(ctx.format_id);
  Dialect dialect = sniff_dialect(text, preferred);
  if (const char fixed = delimiter_from_option(ctx.options.get_string("source_delimiter", "auto"), preferred);
      fixed != 0) {
    dialect.delimiter = fixed;
    dialect.skip_first_line = false;
    const std::string_view first = std::string_view(text).substr(0, text.find_first_of("\r\n"));
    if (first.size() == 5 && util::istarts_with(first, "sep=")) dialect.skip_first_line = true;
  }
  const std::string quote = ctx.options.get_enum("source_quote", "double", {"double", "single", "none"});
  dialect.quote = quote == "single" ? '\'' : quote == "none" ? '\0' : '"';
  if (ctx.options.get_enum("source_escape", "none", {"none", "backslash"}) == "backslash") dialect.escape = '\\';
  const InferOptions infer = infer_options_from(ctx.options);
  const std::string header = ctx.options.get_enum("header", "auto", {"auto", "yes", "no"});
  const std::string comment = ctx.options.get_string("comment_prefix", "");

  ir::Sheet sheet;
  sheet.name = file_stem(ctx.filename);
  CellBudget budget(ctx.limits);
  std::vector<std::vector<std::string>> raw_first_row;  // the uninferred first record, for header handling
  const double total = static_cast<double>(text.size());
  std::size_t processed = 0;
  bool unterminated = false;
  CVT_TRY(parse_delimited(
      text, dialect,
      [&](std::vector<std::string>&& record) -> Result<void> {
        if ((++processed & 1023) == 0) CVT_TRY(ctx.checkpoint(std::min(0.99, static_cast<double>(processed) * 16 / total)));
        if (!comment.empty() && !record.empty() && record[0].starts_with(comment)) return {};
        CVT_TRY(budget.take(record.size() + 1));
        if (sheet.rows.empty()) raw_first_row.push_back(record);
        std::vector<ir::Cell> row;
        row.reserve(record.size());
        for (std::string& field : record) {
          row.push_back(infer.numbers || infer.booleans || infer.dates ? infer_cell(std::move(field), infer)
                                                                        : (field.empty() ? ir::Cell{}
                                                                                         : ir::Cell::string(std::move(field))));
        }
        sheet.rows.push_back(std::move(row));
        return {};
      },
      &unterminated));
  if (unterminated) ctx.warn("The last quoted field is not closed; it was read up to the end of the file.");
  trim_sheet(sheet);
  bool has_header = false;
  if (header == "yes") {
    has_header = !sheet.rows.empty();
  } else if (header == "auto") {
    has_header = guess_header(sheet);
  }
  if (has_header && !sheet.rows.empty() && !raw_first_row.empty()) {
    // Column titles stay text exactly as written ("2024" is a title, not a number).
    auto& first = sheet.rows[0];
    for (std::size_t c = 0; c < first.size() && c < raw_first_row[0].size(); ++c) {
      if (!raw_first_row[0][c].empty()) first[c] = ir::Cell::string(raw_first_row[0][c]);
    }
  }
  sheet.has_header = has_header;
  ir::Table table;
  table.sheets.push_back(std::move(sheet));
  return table;
}

// --------------------------------------------------------------------------- writing

// As Python's csv module and Excel do: only separators, quotes and line breaks need quoting.
bool needs_quotes(std::string_view text, char delimiter, char quote) {
  for (const char c : text) {
    if (c == delimiter || c == quote || c == '\n' || c == '\r') return true;
  }
  return false;
}

bool formula_like(std::string_view text) {
  return !text.empty() && (text.front() == '=' || text.front() == '+' || text.front() == '-' || text.front() == '@' ||
                           text.front() == '\t' || text.front() == '\r');
}

Result<Bytes> write_delimited(const AnyIR& value, WriteContext& ctx) {
  const ir::Table& table = value.get<ir::Table>();
  CVT_TRY_ASSIGN(const ir::Sheet* sheet, select_sheet(table, ctx));
  const char fallback = default_delimiter(ctx.format_id);
  char delimiter = delimiter_from_option(ctx.options.get_string("delimiter", ""), fallback);
  if (delimiter == 0) delimiter = fallback;
  if (delimiter == '"' || delimiter == '\n' || delimiter == '\r') {
    return fail(Status::InvalidArgument, "the delimiter cannot be a quote or a line break");
  }
  const std::string quoting = ctx.options.get_enum("quoting", "minimal", {"minimal", "all", "nonnumeric", "never"});
  const std::string_view newline = ctx.options.get_enum("line_ending", "crlf", {"crlf", "lf"}) == "lf" ? "\n"sv : "\r\n"sv;
  const bool guard = ctx.options.get_bool("formula_guard", false);
  const char quote = '"';
  const std::size_t columns = sheet->column_count();
  std::string out;
  std::size_t guarded = 0;
  bool ambiguous = false;
  for (std::size_t r = 0; r < sheet->rows.size(); ++r) {
    if ((r & 4095) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(r) / static_cast<double>(sheet->rows.size())));
    const auto& row = sheet->rows[r];
    for (std::size_t c = 0; c < columns; ++c) {
      if (c > 0) out.push_back(delimiter);
      if (c >= row.size() || row[c].empty()) {
        if (quoting == "all") out.append("\"\"");
        continue;
      }
      const ir::Cell& cell = row[c];
      std::string text = cell.text;
      if (guard && cell.type == ir::CellType::String && formula_like(text)) {
        text.insert(text.begin(), '\'');
        ++guarded;
      }
      bool quoted = false;
      if (quoting == "all") {
        quoted = true;
      } else if (quoting == "nonnumeric") {
        quoted = cell.type != ir::CellType::Number && cell.type != ir::CellType::Bool;
      } else if (quoting == "minimal") {
        quoted = needs_quotes(text, delimiter, quote);
      } else if (needs_quotes(text, delimiter, quote)) {
        ambiguous = true;
      }
      if (quoted) {
        out.push_back(quote);
        for (const char ch : text) {
          if (ch == quote) out.push_back(quote);
          out.push_back(ch);
        }
        out.push_back(quote);
      } else {
        out.append(text);
      }
    }
    out.append(newline);
    CVT_TRY(ctx.limits.check_output(out.size()));
  }
  if (guarded > 0) ctx.warn(std::format("{} cell(s) that looked like formulas were prefixed with an apostrophe.", guarded));
  if (ambiguous) {
    ctx.warn("Some fields contain the delimiter, quotes or line breaks but quoting is off; the file may not read back as it was.");
  }
  return encode_output_text(out, ctx.options.get_string("encoding", "utf-8"), ctx.options.get_bool("bom", false), ctx);
}

// --------------------------------------------------------------------------- fixed width

Result<AnyIR> read_fixed_width(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const std::string text,
                 decode_source_text(ctx.input, ctx.options.get_string("source_encoding", "auto"), ctx));
  std::vector<std::u32string> lines;
  for (const std::string_view line : util::split_lines(text)) {
    if (util::trim(line).empty()) continue;
    std::u32string wide = util::utf8_to_utf32(line);
    for (char32_t& c : wide) {
      if (c == U'\t') c = U' ';
    }
    lines.push_back(std::move(wide));
  }
  std::size_t width = 0;
  for (const auto& line : lines) width = std::max(width, line.size());
  CellBudget budget(ctx.limits);
  CVT_TRY(budget.take(width + lines.size()));
  // A column gutter is a position that is blank on every line.
  std::vector<bool> used(width, false);
  for (const auto& line : lines) {
    for (std::size_t i = 0; i < line.size(); ++i) {
      if (line[i] != U' ') used[i] = true;
    }
  }
  std::vector<std::pair<std::size_t, std::size_t>> spans;  // [begin, end)
  if (const auto explicit_widths = ctx.options.get_string("column_widths", ""); !explicit_widths.empty()) {
    std::size_t at = 0;
    for (const std::string_view piece : util::split(explicit_widths, ',', false)) {
      const auto n = util::parse_int(util::trim(piece));
      if (!n || *n <= 0 || *n > 100000) {
        return fail(Status::InvalidArgument, "option 'column_widths' must be a comma-separated list of positive numbers");
      }
      spans.emplace_back(at, at + static_cast<std::size_t>(*n));
      at += static_cast<std::size_t>(*n);
    }
  } else {
    std::size_t i = 0;
    while (i < width) {
      while (i < width && !used[i]) ++i;
      if (i >= width) break;
      const std::size_t begin = i;
      while (i < width && used[i]) ++i;
      spans.emplace_back(begin, i);
    }
  }
  const InferOptions infer = infer_options_from(ctx.options);
  ir::Sheet sheet;
  sheet.name = file_stem(ctx.filename);
  for (std::size_t r = 0; r < lines.size(); ++r) {
    const auto& line = lines[r];
    std::vector<ir::Cell> row;
    CVT_TRY(budget.take(spans.size()));
    for (const auto& [begin, end] : spans) {
      std::string field;
      if (begin < line.size()) field = util::utf32_to_utf8(line.substr(begin, std::min(end, line.size()) - begin));
      field = std::string(util::trim(field));
      row.push_back(infer_cell(std::move(field), infer));
    }
    sheet.rows.push_back(std::move(row));
  }
  trim_sheet(sheet);
  const std::string header = ctx.options.get_enum("header", "auto", {"auto", "yes", "no"});
  sheet.has_header = header == "yes" ? !sheet.rows.empty() : header == "auto" ? guess_header(sheet) : false;
  if (sheet.has_header) {
    for (ir::Cell& cell : sheet.rows[0]) {
      if (!cell.empty()) cell = ir::Cell::string(cell.text);
    }
  }
  ir::Table table;
  table.sheets.push_back(std::move(sheet));
  return table;
}

Result<Bytes> write_fixed_width(const AnyIR& value, WriteContext& ctx) {
  const ir::Table& table = value.get<ir::Table>();
  CVT_TRY_ASSIGN(const ir::Sheet* sheet, select_sheet(table, ctx));
  const std::size_t columns = sheet->column_count();
  const auto gap = static_cast<std::size_t>(ctx.options.get_int("column_gap", 2, 1, 16));
  bool flattened = false;
  const auto field_text = [&](const ir::Cell& cell) {
    std::string text = cell.text;
    if (text.find_first_of("\r\n\t") != std::string::npos) {
      flattened = true;
      for (char& c : text) {
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
      }
    }
    return text;
  };
  std::vector<std::size_t> widths(columns, 0);
  for (const auto& row : sheet->rows) {
    for (std::size_t c = 0; c < row.size(); ++c) widths[c] = std::max(widths[c], display_width(field_text(row[c])));
  }
  const ir::Cell empty_cell;
  std::string out;
  for (const auto& row : sheet->rows) {
    std::string line;
    for (std::size_t c = 0; c < columns; ++c) {
      const ir::Cell& cell = c < row.size() ? row[c] : empty_cell;
      const std::string text = field_text(cell);
      const std::size_t pad = widths[c] - display_width(text);
      if (c > 0) line.append(gap, ' ');
      const bool right = cell.type == ir::CellType::Number;
      if (right) line.append(pad, ' ');
      line.append(text);
      if (!right) line.append(pad, ' ');
    }
    while (!line.empty() && line.back() == ' ') line.pop_back();
    out.append(line);
    out.append(ctx.options.get_enum("line_ending", "lf", {"lf", "crlf"}) == "crlf" ? "\r\n" : "\n");
    CVT_TRY(ctx.limits.check_output(out.size()));
  }
  if (flattened) ctx.warn("Line breaks and tabs inside cells were replaced with spaces.");
  return encode_output_text(out, ctx.options.get_string("encoding", "utf-8"), ctx.options.get_bool("bom", false), ctx);
}

// --------------------------------------------------------------------------- detection

// A delimited file: text whose first lines split into a consistent number
// (at least two) of fields with the given delimiter.
int sniff_delimited(ByteView data, char delimiter, int confidence) {
  std::string_view head = as_chars(data).substr(0, 16 * 1024);
  if (head.starts_with("\xEF\xBB\xBF")) head.remove_prefix(3);
  if (head.empty() || !util::looks_like_text(as_bytes(head))) return 0;
  const std::string_view trimmed = util::trim_left(head);
  if (trimmed.starts_with("<") || trimmed.starts_with("{") || trimmed.starts_with("[")) return 0;
  if (head.size() == 16 * 1024) {
    const std::size_t cut = head.find_last_of('\n');
    if (cut != std::string_view::npos) head = head.substr(0, cut);
  }
  if (util::split_lines(head).size() < 2) return 0;
  const Dialect sniffed = sniff_dialect(head, delimiter);
  if (sniffed.delimiter != delimiter) return 0;
  std::size_t lines = 0;
  std::size_t consistent = 0;
  std::size_t first = 0;
  Dialect dialect = sniffed;
  (void)parse_delimited(head, dialect, [&](std::vector<std::string>&& record) -> Result<void> {
    if (record.empty()) return {};
    if (lines == 0) first = record.size();
    ++lines;
    if (record.size() == first) ++consistent;
    return {};
  });
  if (first < 2 || lines < 2) return 0;
  // Three or more consistent records are convincing; two are a weaker hint.
  if (lines < 3) confidence -= 15;
  if (delimiter == '|') {
    // Markdown, Org and other pipe tables start and end lines with '|'.
    const std::string_view line = util::trim(head.substr(0, head.find('\n')));
    if (line.starts_with('|') && line.ends_with('|')) return 0;
  }
  return consistent * 10 >= lines * 9 ? confidence : 0;
}

// Above the 60 of plain text, so that tabular text is recognised as such.
// CSV also covers the semicolon dialect that Excel writes in locales with a
// decimal comma (the reader finds the delimiter), and a text file named .csv
// is taken at its word.
int sniff_csv(ByteView data, std::string_view filename) {
  int score = sniff_delimited(data, ',', 72);
  if (score == 0) {
    // Statements ending in ';' (SQL, C-like code) are not a semicolon table.
    const std::string_view head = as_chars(data).substr(0, 4096);
    std::size_t lines = 0;
    std::size_t terminated = 0;
    for (const std::string_view line : util::split_lines(head)) {
      const std::string_view t = util::trim(line);
      if (t.empty()) continue;
      ++lines;
      if (t.ends_with(';')) ++terminated;
    }
    if (terminated * 2 < lines) score = sniff_delimited(data, ';', 68);
  }
  if (score == 0 && util::iends_with(filename, ".csv") && !data.empty() &&
      util::looks_like_text(data.first(std::min<std::size_t>(data.size(), 4096)))) {
    score = 79;
  }
  return score;
}
int sniff_tsv(ByteView data, std::string_view) { return sniff_delimited(data, '\t', 72); }
int sniff_psv(ByteView data, std::string_view) { return sniff_delimited(data, '|', 66); }

// --------------------------------------------------------------------------- registration

std::vector<OptionSpec> delimited_read_options() {
  std::vector<OptionSpec> specs = {
      OptionSpec::text("source_delimiter", "Source delimiter", "auto",
                       "Field separator of the input: auto, a single character, or tab, comma, semicolon, pipe, space."),
      OptionSpec::choice("header", "Header row", "auto", {"auto", "yes", "no"},
                         "Whether the first row holds column titles; auto guesses from the content."),
      OptionSpec::choice("source_quote", "Source quote character", "double", {"double", "single", "none"},
                         "Character that encloses fields containing separators or line breaks.")
          .advanced(),
      OptionSpec::choice("source_escape", "Source escape character", "none", {"none", "backslash"},
                         "Whether a backslash escapes the next character (MySQL-style exports).")
          .advanced(),
      OptionSpec::text("comment_prefix", "Comment prefix", "",
                       "Lines starting with this text are skipped (for example #). Empty keeps every line.")
          .advanced(),
      source_encoding_option(),
  };
  for (OptionSpec& spec : infer_option_specs()) specs.push_back(std::move(spec));
  return specs;
}

std::vector<OptionSpec> output_encoding_options(bool for_csv) {
  std::vector<std::string> encodings = {"utf-8", "utf-16le", "utf-16be"};
  for (const auto& page : {"windows-1252", "windows-1250", "windows-1251", "iso-8859-1", "iso-8859-15", "mac-roman",
                           "cp437", "cp850"}) {
    encodings.emplace_back(page);
  }
  return {
      OptionSpec::choice("encoding", "Encoding", "utf-8", std::move(encodings),
                         "Character encoding of the output. Excel's \"Unicode text\" is utf-16le with a BOM.")
          .advanced(),
      OptionSpec::boolean("bom", "Byte order mark", false,
                          for_csv ? "Start the file with a byte order mark so that Excel recognises UTF-8."
                                  : "Start the file with a byte order mark.")
          .advanced(),
  };
}

std::vector<OptionSpec> delimited_write_options(std::string_view default_delimiter_name) {
  std::vector<OptionSpec> specs = {
      OptionSpec::text("delimiter", "Delimiter", std::string(default_delimiter_name),
                       "Field separator: a single character, or tab, comma, semicolon, pipe, space."),
      OptionSpec::choice("quoting", "Quoting", "minimal", {"minimal", "all", "nonnumeric", "never"},
                         "Which fields are enclosed in quotes: only where needed, all, all but numbers, or none."),
      OptionSpec::choice("line_ending", "Line ending", "crlf", {"crlf", "lf"},
                         "Record terminator; RFC 4180 and Excel use CRLF.")
          .advanced(),
      OptionSpec::boolean("formula_guard", "Guard against formula injection", false,
                          "Prefix text cells that start with = + - @ with an apostrophe so spreadsheet apps do not "
                          "run them as formulas."),
      sheet_option(),
  };
  for (OptionSpec& spec : output_encoding_options(true)) specs.push_back(std::move(spec));
  return specs;
}

void add_delimited(Registry& registry, std::string_view id, std::string_view name, std::string_view description,
                   std::vector<std::string> extensions, std::string_view mime, int popularity,
                   std::function<int(ByteView, std::string_view)> sniff, std::string_view delimiter_name,
                   bool output_only = false) {
  FormatInfo info;
  info.id = std::string(id);
  info.name = std::string(name);
  info.description = std::string(description);
  info.category = Category::Spreadsheet;
  info.extensions = std::move(extensions);
  info.mime = std::string(mime);
  info.popularity = popularity;
  info.sniff = std::move(sniff);
  info.output_only = output_only;
  registry.add_format(std::move(info));

  Codec codec;
  codec.format_id = std::string(id);
  codec.kind = Kind::Table;
  if (!output_only) codec.read = read_delimited;
  codec.write = write_delimited;
  codec.read_options = delimited_read_options();
  codec.write_options = delimited_write_options(delimiter_name);
  registry.add_codec(std::move(codec));
}

}  // namespace

void register_delimited(Registry& registry) {
  add_delimited(registry, "csv", "Comma-separated values", "Plain-text table with one record per line.", {"csv"},
                "text/csv", 92, sniff_csv, ",");
  add_delimited(registry, "tsv", "Tab-separated values", "Plain-text table with tab-separated fields.",
                {"tsv", "tab"}, "text/tab-separated-values", 55, sniff_tsv, "tab");
  add_delimited(registry, "psv", "Pipe-separated values", "Plain-text table with fields separated by '|'.", {"psv"},
                "text/plain", 20, sniff_psv, "|");
  add_delimited(registry, "csv-semicolon", "CSV (semicolon)",
                "Comma-separated values with ';' between fields, as Excel writes them in locales with a decimal comma.",
                {"csv"}, "text/csv", 40, nullptr, ";", /*output_only=*/true);  // read as "csv" (the reader finds ';')

  FormatInfo fixed;
  fixed.id = "fwf";
  fixed.name = "Fixed-width text table";
  fixed.description = "Columns aligned with spaces, as printed reports and mainframe exports are laid out.";
  fixed.category = Category::Spreadsheet;
  fixed.extensions = {"fwf", "prn"};
  fixed.mime = "text/plain";
  fixed.popularity = 15;
  registry.add_format(std::move(fixed));

  Codec codec;
  codec.format_id = "fwf";
  codec.kind = Kind::Table;
  codec.read = read_fixed_width;
  codec.write = write_fixed_width;
  codec.read_cost = 20;
  codec.read_options = {
      OptionSpec::choice("header", "Header row", "auto", {"auto", "yes", "no"},
                         "Whether the first row holds column titles; auto guesses from the content."),
      OptionSpec::text("column_widths", "Column widths", "",
                       "Comma-separated widths in characters (e.g. 10,5,8); empty detects the columns from blank "
                       "gutters.")
          .advanced(),
      source_encoding_option(),
  };
  for (OptionSpec& spec : infer_option_specs()) codec.read_options.push_back(std::move(spec));
  codec.write_options = {
      OptionSpec::integer("column_gap", "Column gap", 2, 1, 16, "Spaces between columns."),
      OptionSpec::choice("line_ending", "Line ending", "lf", {"lf", "crlf"}, "Line terminator.").advanced(),
      sheet_option(),
  };
  for (OptionSpec& spec : output_encoding_options(false)) codec.write_options.push_back(std::move(spec));
  registry.add_codec(std::move(codec));
}

}  // namespace cvt::table
