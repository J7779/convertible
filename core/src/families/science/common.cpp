// Convertible core - science family: shared helpers.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/science/common.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <system_error>

#include "util/crc.hpp"
#include "util/deflate.hpp"
#include "util/text.hpp"

namespace cvt::science {

void add_format(Registry& registry, FormatSpec spec) {
  FormatInfo info;
  info.id = std::move(spec.id);
  info.name = std::move(spec.name);
  info.description = std::move(spec.description);
  info.category = spec.category;
  info.extensions = std::move(spec.extensions);
  info.mime = std::move(spec.mime);
  info.popularity = spec.popularity;
  info.signatures = std::move(spec.signatures);
  if (spec.sniff) {
    info.sniff = [sniff = std::move(spec.sniff)](std::span<const std::byte> data, std::string_view name) {
      return sniff(data, name);
    };
  }
  registry.add_format(std::move(info));
}

void add_codec(Registry& registry, std::string format_id, Kind kind, ReadFn read, WriteFn write,
               std::vector<OptionSpec> read_options, std::vector<OptionSpec> write_options, int read_cost,
               int write_cost, bool lossy) {
  Codec codec;
  codec.format_id = std::move(format_id);
  codec.kind = kind;
  codec.read = std::move(read);
  codec.write = std::move(write);
  codec.read_options = std::move(read_options);
  codec.write_options = std::move(write_options);
  codec.read_cost = read_cost;
  codec.write_cost = write_cost;
  codec.lossy = lossy;
  registry.add_codec(std::move(codec));
}

std::string input_text(ByteView input) {
  const util::EncodingGuess guess = util::guess_encoding(input);
  if (guess.encoding == util::TextEncoding::Utf8) {
    return util::sanitize_utf8(as_chars(input.subspan(guess.bom_length)));
  }
  return util::decode_text(input, guess.encoding).text;
}

std::vector<std::string_view> lines_of(std::string_view text) {
  std::vector<std::string_view> lines;
  std::size_t start = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\n' || text[i] == '\r') {
      lines.push_back(text.substr(start, i - start));
      if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
      start = i + 1;
    }
  }
  if (start < text.size()) lines.push_back(text.substr(start));
  return lines;
}

std::vector<std::string_view> words(std::string_view text) {
  std::vector<std::string_view> out;
  std::size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && util::is_ascii_space(text[i])) ++i;
    const std::size_t start = i;
    while (i < text.size() && !util::is_ascii_space(text[i])) ++i;
    if (i > start) out.push_back(text.substr(start, i - start));
  }
  return out;
}

std::string_view head_text(ByteView data, std::size_t max) {
  std::string_view text = as_chars(data.first(std::min(data.size(), max)));
  if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
  return text;
}

bool has_extension(std::string_view name, std::string_view ext) {
  if (name.size() <= ext.size() + 1) return false;
  return name[name.size() - ext.size() - 1] == '.' && util::iends_with(name, ext);
}

std::string fmt_num(double value, int digits) {
  if (!std::isfinite(value)) return std::isnan(value) ? "nan" : (value > 0 ? "inf" : "-inf");
  if (digits <= 0) return util::format_double(value);
  std::array<char, 64> buffer{};
  const auto result =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::general, digits);
  if (result.ec != std::errc{}) return util::format_double(value);
  return std::string(buffer.data(), result.ptr);
}

std::string fmt_fixed(double value, int decimals) {
  if (!std::isfinite(value)) return fmt_num(value);
  std::array<char, 400> buffer{};
  const auto result =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::fixed, decimals);
  if (result.ec != std::errc{}) return util::format_double(value);
  std::string text(buffer.data(), result.ptr);
  if (text.starts_with("-") && text.find_first_not_of("-0.") == std::string::npos) text.erase(0, 1);
  return text;
}

std::optional<double> to_double(std::string_view text) {
  text = util::trim(text);
  if (text.empty()) return std::nullopt;
  if (text.front() == '+') text.remove_prefix(1);
  return util::parse_double(text);
}

std::optional<std::int64_t> to_int(std::string_view text) {
  text = util::trim(text);
  if (text.empty()) return std::nullopt;
  return util::parse_int(text);
}

ir::Cell text_cell(std::string_view text) {
  if (text.empty()) return {};
  return ir::Cell::string(std::string(text));
}

ir::Cell number_cell(double value) { return ir::Cell::numeric(value, fmt_num(value)); }

ir::Cell int_cell(std::int64_t value) { return ir::Cell::numeric(static_cast<double>(value), std::to_string(value)); }

ir::Cell auto_cell(std::string_view text) {
  if (text.empty()) return {};
  const std::string_view trimmed = util::trim(text);
  bool numeric_shape = !trimmed.empty() && trimmed.size() == text.size();
  for (const char c : trimmed) {
    if (!(util::is_ascii_digit(c) || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E')) {
      numeric_shape = false;
      break;
    }
  }
  if (numeric_shape) {
    if (const auto value = util::parse_double(trimmed.front() == '+' ? trimmed.substr(1) : trimmed)) {
      return ir::Cell::numeric(*value, std::string(text));
    }
  }
  return ir::Cell::string(std::string(text));
}

ir::Sheet header_sheet(std::string name, const std::vector<std::string>& header) {
  ir::Sheet sheet;
  sheet.name = std::move(name);
  sheet.has_header = true;
  std::vector<ir::Cell> row;
  row.reserve(header.size());
  for (const std::string& title : header) row.push_back(ir::Cell::string(title));
  sheet.rows.push_back(std::move(row));
  return sheet;
}

const ir::Sheet* find_sheet(const ir::Table& table, std::string_view name) {
  for (const ir::Sheet& sheet : table.sheets) {
    if (util::iequals(sheet.name, name)) return &sheet;
  }
  return nullptr;
}

int column_of(const ir::Sheet& sheet, std::string_view title) {
  if (sheet.rows.empty()) return -1;
  const auto& header = sheet.rows.front();
  for (std::size_t i = 0; i < header.size(); ++i) {
    if (util::iequals(util::trim(header[i].text), title)) return static_cast<int>(i);
  }
  return -1;
}

std::string_view cell_text(const ir::Sheet& sheet, std::size_t row, int column) {
  if (column < 0 || row >= sheet.rows.size()) return {};
  const auto& cells = sheet.rows[row];
  if (static_cast<std::size_t>(column) >= cells.size()) return {};
  return cells[static_cast<std::size_t>(column)].text;
}

std::vector<std::string> sheet_lines(const ir::Sheet& sheet) {
  std::vector<std::string> lines;
  for (const auto& row : sheet.rows) lines.push_back(row.empty() ? std::string() : row.front().text);
  return lines;
}

ir::Sheet lines_sheet(std::string name, const std::vector<std::string>& lines) {
  ir::Sheet sheet;
  sheet.name = std::move(name);
  for (const std::string& line : lines) sheet.rows.push_back({ir::Cell::string(line)});
  return sheet;
}

Result<void> check_cells(const Context& ctx, std::uint64_t cells) {
  // A table cell costs far more than a pixel; reuse the pixel ceiling divided by 16.
  if (cells > ctx.limits.max_pixels / 16 + 1) {
    return fail(Status::Limit, "{} table cells exceed the limit", cells);
  }
  return {};
}

Json json_number(double value) {
  if (std::isfinite(value) && value == std::trunc(value) && std::fabs(value) < 9.0e15) {
    return Json(static_cast<std::int64_t>(value));
  }
  if (!std::isfinite(value)) return Json(fmt_num(value));
  return Json(value);
}

ir::Sheet key_value_sheet(std::string name, const Json& object) {
  ir::Sheet sheet = header_sheet(std::move(name), {"key", "value"});
  for (const auto& [key, value] : object.items()) {
    ir::Cell cell;
    if (value.is_number()) {
      cell = number_cell(value.as_double());
    } else if (value.is_string()) {
      cell = text_cell(value.as_string());
    } else if (value.is_bool()) {
      cell = ir::Cell::boolean(value.as_bool());
    } else if (!value.is_null()) {
      cell = text_cell(util::dump_json(value));
    }
    sheet.rows.push_back({ir::Cell::string(key), std::move(cell)});
  }
  return sheet;
}

namespace {

std::string scalar_text(const Json& value) {
  if (value.is_string()) return value.as_string();
  if (value.is_null()) return "";
  return util::dump_json(value);
}

bool flat_object(const Json& value) {
  if (!value.is_object()) return false;
  for (const auto& member : value.items()) {
    if (member.value().is_structured()) return false;
  }
  return true;
}

void render(ir::Blocks& out, std::string_view key, const Json& value, int depth) {
  using namespace ir::build;
  const int level = std::min(6, depth + 1);
  if (value.is_object()) {
    if (!key.empty()) out.push_back(ir::Heading{level, {text(std::string(key))}});
    ir::TableBlock scalars;
    for (const auto& [k, v] : value.items()) {
      if (v.is_structured()) continue;
      ir::TableRow row;
      row.cells.push_back(ir::TableCell{{text(k)}, 1, 1});
      row.cells.push_back(ir::TableCell{{text(scalar_text(v))}, 1, 1});
      scalars.rows.push_back(std::move(row));
    }
    if (!scalars.rows.empty()) out.push_back(std::move(scalars));
    for (const auto& [k, v] : value.items()) {
      if (!v.is_structured()) continue;
      if (depth >= 12) {
        out.push_back(ir::CodeBlock{"json", util::dump_json(v, 2)});
      } else {
        render(out, k, v, depth + 1);
      }
    }
    return;
  }
  if (value.is_array()) {
    if (!key.empty()) out.push_back(ir::Heading{level, {text(std::string(key))}});
    bool all_flat = !value.empty();
    for (const Json& element : value.elements()) all_flat = all_flat && flat_object(element);
    if (all_flat) {
      std::vector<std::string> columns;
      for (const Json& element : value.elements()) {
        for (const auto& member : element.items()) {
          if (std::find(columns.begin(), columns.end(), member.key()) == columns.end()) columns.push_back(member.key());
        }
      }
      ir::TableBlock table;
      ir::TableRow header;
      header.header = true;
      for (const auto& column : columns) header.cells.push_back(ir::TableCell{{text(column)}, 1, 1});
      table.rows.push_back(std::move(header));
      for (const Json& element : value.elements()) {
        ir::TableRow row;
        for (const auto& column : columns) {
          const Json* cell = element.find(column);
          row.cells.push_back(ir::TableCell{{text(cell != nullptr ? scalar_text(*cell) : std::string())}, 1, 1});
        }
        table.rows.push_back(std::move(row));
      }
      out.push_back(std::move(table));
      return;
    }
    bool scalars = true;
    for (const Json& element : value.elements()) scalars = scalars && !element.is_structured();
    if (scalars) {
      std::string joined;
      for (const Json& element : value.elements()) {
        if (!joined.empty()) joined += ", ";
        joined += scalar_text(element);
      }
      out.push_back(ir::Paragraph{{text(joined)}});
      return;
    }
    std::size_t index = 0;
    for (const Json& element : value.elements()) {
      const std::string name = std::string(key) + " [" + std::to_string(index++) + "]";
      if (depth >= 12) {
        out.push_back(ir::CodeBlock{"json", util::dump_json(element, 2)});
      } else {
        render(out, name, element, depth + 1);
      }
    }
    return;
  }
  out.push_back(ir::Paragraph{{text(std::string(key) + ": " + scalar_text(value))}});
}

}  // namespace

ir::Document data_document(std::string_view title, const Json& value) {
  ir::Document document;
  document.metadata.title = std::string(title);
  document.blocks.push_back(ir::Heading{1, {ir::Text{std::string(title)}}});
  render(document.blocks, {}, value, 1);
  return document;
}

Result<Bytes> encode_png(const ir::Frame& frame, bool grey) {
  if (!frame.valid()) return fail(Status::Encode, "the picture has no pixels");
  const std::size_t channels = grey ? 1 : 4;
  Bytes raw;
  raw.reserve((std::size_t{frame.width} * channels + 1) * frame.height);
  for (std::uint32_t y = 0; y < frame.height; ++y) {
    raw.push_back(std::byte{0});
    for (std::uint32_t x = 0; x < frame.width; ++x) {
      const std::size_t at = frame.offset(x, y);
      if (grey) {
        raw.push_back(std::byte{frame.rgba[at]});
      } else {
        for (std::size_t c = 0; c < 4; ++c) raw.push_back(std::byte{frame.rgba[at + c]});
      }
    }
  }
  CVT_TRY_ASSIGN(const Bytes compressed, util::zlib_compress(raw, 9));
  util::ByteWriter out(compressed.size() + 64);
  out.u8(0x89).text("PNG\r\n\x1A\n");
  const auto chunk = [&out](std::string_view type, ByteView data) {
    out.u32be(static_cast<std::uint32_t>(data.size()));
    const std::size_t start = out.size();
    out.text(type).bytes(data);
    out.u32be(util::crc32(out.view().subspan(start)));
  };
  util::ByteWriter header(13);
  header.u32be(frame.width).u32be(frame.height).u8(8).u8(grey ? 0 : 6).u8(0).u8(0).u8(0);
  chunk("IHDR", header.view());
  chunk("IDAT", compressed);
  chunk("IEND", {});
  return out.take();
}

}  // namespace cvt::science
