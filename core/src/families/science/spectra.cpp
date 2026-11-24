// Convertible core - science family: spectra. JCAMP-DX (AFFN and the ASDF
// compressed forms SQZ/DIF/DUP; read + write) and mass-spectrometry runs in
// mzML and mzXML (read: base64 arrays, 32/64-bit floats, zlib).
//
// JCAMP Table: sheet "spectrum" (x, y) per data block, sheet "header"
// (label, value). mzML/mzXML Table: sheet "peaks" (scan, ms_level,
// retention_time_s, mz, intensity) and sheet "scans" (one row per spectrum).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <format>

#include "families/science/common.hpp"
#include "families/science/science.hpp"
#include "util/base64.hpp"
#include "util/deflate.hpp"
#include "util/text.hpp"
#include "util/xml.hpp"

namespace cvt::science {

namespace {

// --------------------------------------------------------------------------- JCAMP-DX

std::string jcamp_label(std::string_view label) {
  std::string out;
  for (const char c : label) {
    if (c == ' ' || c == '-' || c == '/' || c == '_') continue;
    out.push_back(util::ascii_upper(c));
  }
  return out;
}

struct AsdfValue {
  double value = 0;
  bool dif = false;
};

/// Decode one data line into numbers (AFFN or ASDF). DUP counts are expanded.
Result<std::vector<AsdfValue>> asdf_line(std::string_view line) {
  std::vector<AsdfValue> out;
  std::size_t i = 0;
  const auto read_digits = [&](std::string& text) {
    while (i < line.size() && (util::is_ascii_digit(line[i]) || line[i] == '.')) text.push_back(line[i++]);
    // An exponent needs an explicit sign ("1.5E+03"): a bare 'E' or 'e' after
    // digits is the next SQZ value in compressed (ASDF) data.
    if (i + 2 < line.size() && (line[i] == 'E' || line[i] == 'e') && (line[i + 1] == '+' || line[i + 1] == '-') &&
        util::is_ascii_digit(line[i + 2])) {
      text.push_back(line[i++]);
      text.push_back(line[i++]);
      while (i < line.size() && util::is_ascii_digit(line[i])) text.push_back(line[i++]);
    }
  };
  while (i < line.size()) {
    const char c = line[i];
    if (c == ' ' || c == ',' || c == '\t' || c == ';') {
      ++i;
      continue;
    }
    if (c == '$' && line.substr(i).starts_with("$$")) break;  // comment
    std::string text;
    bool dif = false;
    int dup = 0;
    if (util::is_ascii_digit(c) || c == '.' || c == '+' || c == '-') {
      if (c == '+' || c == '-') text.push_back(line[i++]);
      read_digits(text);
    } else if (c == '@' || (c >= 'A' && c <= 'I') || (c >= 'a' && c <= 'i')) {  // SQZ
      if (c == '@') text = "0";
      else if (c >= 'A' && c <= 'I') text = std::string(1, static_cast<char>('1' + (c - 'A')));
      else text = "-" + std::string(1, static_cast<char>('1' + (c - 'a')));
      ++i;
      read_digits(text);
    } else if (c == '%' || (c >= 'J' && c <= 'R') || (c >= 'j' && c <= 'r')) {  // DIF
      dif = true;
      if (c == '%') text = "0";
      else if (c >= 'J' && c <= 'R') text = std::string(1, static_cast<char>('1' + (c - 'J')));
      else text = "-" + std::string(1, static_cast<char>('1' + (c - 'j')));
      ++i;
      read_digits(text);
    } else if ((c >= 'S' && c <= 'Z') || c == 's') {  // DUP
      text = c == 's' ? "9" : std::string(1, static_cast<char>('1' + (c - 'S')));
      ++i;
      read_digits(text);
      dup = static_cast<int>(to_int(text).value_or(0));
      if (dup < 1 || out.empty()) return fail(Status::Decode, "bad JCAMP DUP count");
      if (dup > 1'000'000) return fail(Status::Limit, "JCAMP DUP count is too large");
      const AsdfValue last = out.back();
      for (int k = 1; k < dup; ++k) out.push_back(last);
      continue;
    } else if (c == '?') {
      out.push_back({std::numeric_limits<double>::quiet_NaN(), false});
      ++i;
      continue;
    } else {
      return fail(Status::Decode, "unexpected character '{}' in JCAMP data", c);
    }
    const auto value = to_double(text);
    if (!value) return fail(Status::Decode, "bad JCAMP number '{}'", text);
    out.push_back({*value, dif});
  }
  return out;
}

struct JcampBlock {
  std::vector<std::pair<std::string, std::string>> header;  // original label (without ##) -> value
  std::vector<double> x;
  std::vector<double> y;
};

double header_number(const JcampBlock& block, std::string_view label, double fallback) {
  for (const auto& [key, value] : block.header) {
    if (jcamp_label(key) == label) return to_double(value).value_or(fallback);
  }
  return fallback;
}

Result<void> decode_xydata(JcampBlock& block, const std::vector<std::string_view>& lines, const Context& ctx) {
  const double xfactor = header_number(block, "XFACTOR", 1.0);
  const double yfactor = header_number(block, "YFACTOR", 1.0);
  const double firstx = header_number(block, "FIRSTX", std::numeric_limits<double>::quiet_NaN());
  const double lastx = header_number(block, "LASTX", std::numeric_limits<double>::quiet_NaN());
  const double npoints = header_number(block, "NPOINTS", 0);
  std::vector<double> ys;
  std::vector<double> line_x;
  std::vector<std::size_t> line_start;
  bool previous_dif = false;
  for (const std::string_view line : lines) {
    CVT_TRY_ASSIGN(auto values, asdf_line(line));
    if (values.empty()) continue;
    line_x.push_back(values[0].value * xfactor);
    std::size_t k = 1;
    // Y check value: a DIF-ending line repeats its last Y at the next line's start.
    if (previous_dif && values.size() > 1 && !ys.empty()) k = 2;
    line_start.push_back(ys.size());
    double current = ys.empty() ? 0 : ys.back();
    for (; k < values.size(); ++k) {
      current = values[k].dif ? current + values[k].value : values[k].value;
      ys.push_back(current);
      if (ys.size() > ctx.limits.max_pixels) return fail(Status::Limit, "JCAMP spectrum has too many points");
    }
    previous_dif = values.size() > 1 && values.back().dif;
  }
  const std::size_t n = ys.size();
  block.y.reserve(n);
  for (const double y : ys) block.y.push_back(y * yfactor);
  block.x.resize(n);
  if (n == 0) return {};
  const bool have_range = std::isfinite(firstx) && std::isfinite(lastx) && n > 1;
  if (have_range) {
    for (std::size_t i = 0; i < n; ++i) {
      block.x[i] = firstx + (lastx - firstx) * static_cast<double>(i) / static_cast<double>(n - 1);
    }
  } else {
    // Interpolate between line abscissas.
    const double step = line_x.size() > 1 && line_start[1] > 0 ? (line_x[1] - line_x[0]) / static_cast<double>(line_start[1]) : xfactor;
    for (std::size_t i = 0; i < n; ++i) block.x[i] = line_x[0] + step * static_cast<double>(i);
  }
  if (npoints > 0 && static_cast<std::size_t>(npoints) != n) {
    ctx.warn(std::format("JCAMP-DX declares {} points but holds {}.", static_cast<std::uint64_t>(npoints), n));
  }
  return {};
}

Result<void> decode_xypoints(JcampBlock& block, const std::vector<std::string_view>& lines) {
  const double xfactor = header_number(block, "XFACTOR", 1.0);
  const double yfactor = header_number(block, "YFACTOR", 1.0);
  std::vector<double> values;
  for (const std::string_view line : lines) {
    CVT_TRY_ASSIGN(auto numbers, asdf_line(line));
    for (const auto& v : numbers) values.push_back(v.value);
  }
  for (std::size_t i = 0; i + 1 < values.size(); i += 2) {
    block.x.push_back(values[i] * xfactor);
    block.y.push_back(values[i + 1] * yfactor);
  }
  return {};
}

Result<std::vector<JcampBlock>> parse_jcamp(std::string_view text, const Context& ctx) {
  const auto lines = lines_of(text);
  std::vector<JcampBlock> blocks;
  JcampBlock current;
  bool open = false;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const std::string_view line = util::trim(lines[i]);
    if (!line.starts_with("##")) continue;
    const std::size_t eq = line.find('=');
    if (eq == std::string_view::npos) continue;
    const std::string label(util::trim(line.substr(2, eq - 2)));
    std::string value(util::trim(line.substr(eq + 1)));
    if (const std::size_t comment = value.find("$$"); comment != std::string::npos) value = std::string(util::trim(value.substr(0, comment)));
    const std::string key = jcamp_label(label);
    if (key == "TITLE") {
      if (open && (!current.y.empty() || !current.header.empty())) {
        blocks.push_back(std::move(current));
        current = JcampBlock{};
      }
      open = true;
    }
    if (key == "END") {
      if (open || !current.y.empty()) blocks.push_back(std::move(current));
      current = JcampBlock{};
      open = false;
      continue;
    }
    std::vector<std::string_view> data;
    std::size_t j = i + 1;
    while (j < lines.size() && !util::trim(lines[j]).starts_with("##")) data.push_back(lines[j++]);
    if (key == "XYDATA" || key == "XYPOINTS" || key == "PEAKTABLE") {
      current.header.emplace_back(label, value);
      if (key == "XYDATA" && value.find("++") != std::string::npos) {
        CVT_TRY(decode_xydata(current, data, ctx));
      } else {
        CVT_TRY(decode_xypoints(current, data));
      }
      i = j - 1;
      continue;
    }
    // Multi-line values (e.g. ##TITLE continued) are joined with newlines.
    for (const auto extra : data) {
      if (!util::trim(extra).empty()) value += "\n" + std::string(util::trim(extra));
    }
    i = j - 1;
    current.header.emplace_back(label, value);
  }
  if (open && (!current.y.empty() || !current.header.empty())) blocks.push_back(std::move(current));
  std::erase_if(blocks, [](const JcampBlock& b) { return b.y.empty(); });
  if (blocks.empty()) return fail(Status::Decode, "no JCAMP-DX spectrum data found");
  return blocks;
}

Result<AnyIR> read_jcamp(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const auto blocks, parse_jcamp(input_text(ctx.input), ctx));
  ir::Table table;
  for (std::size_t b = 0; b < blocks.size(); ++b) {
    const JcampBlock& block = blocks[b];
    std::string xname = "x";
    std::string yname = "y";
    for (const auto& [key, value] : block.header) {
      if (jcamp_label(key) == "XUNITS" && !value.empty()) xname = "x (" + util::to_lower(value) + ")";
      if (jcamp_label(key) == "YUNITS" && !value.empty()) yname = "y (" + util::to_lower(value) + ")";
    }
    ir::Sheet sheet = header_sheet(blocks.size() == 1 ? "spectrum" : "spectrum " + std::to_string(b + 1), {xname, yname});
    for (std::size_t i = 0; i < block.y.size(); ++i) {
      sheet.rows.push_back({number_cell(block.x[i]), number_cell(block.y[i])});
    }
    table.sheets.push_back(std::move(sheet));
  }
  ir::Sheet header = header_sheet("header", {"label", "value"});
  for (const auto& [key, value] : blocks.front().header) header.rows.push_back({ir::Cell::string(key), text_cell(value)});
  table.sheets.push_back(std::move(header));
  return table;
}

/// ASDF DIFDUP encoding of integer Y values: one line per run of up to `per_line` values.
std::string asdf_encode(const std::vector<std::int64_t>& ys, std::size_t first, std::size_t count) {
  const auto sqz = [](std::int64_t v) {
    std::string digits = std::to_string(v < 0 ? -v : v);
    const int lead = digits[0] - '0';
    char c;
    if (v == 0) c = '@';
    else if (v > 0) c = static_cast<char>('A' + lead - 1);
    else c = static_cast<char>('a' + lead - 1);
    return std::string(1, c) + digits.substr(1);
  };
  const auto dif = [](std::int64_t v) {
    std::string digits = std::to_string(v < 0 ? -v : v);
    const int lead = digits[0] - '0';
    char c;
    if (v == 0) c = '%';
    else if (v > 0) c = static_cast<char>('J' + lead - 1);
    else c = static_cast<char>('j' + lead - 1);
    return std::string(1, c) + digits.substr(1);
  };
  const auto dup = [](std::size_t n) {
    std::string digits = std::to_string(n);
    const int lead = digits[0] - '0';
    const char c = lead == 9 ? 's' : static_cast<char>('S' + lead - 1);
    return std::string(1, c) + digits.substr(1);
  };
  std::string out = sqz(ys[first]);
  std::size_t i = first + 1;
  while (i < first + count) {
    const std::int64_t d = ys[i] - ys[i - 1];
    std::size_t run = 1;
    while (i + run < first + count && ys[i + run] - ys[i + run - 1] == d) ++run;
    out += dif(d);
    if (run > 1) out += dup(run);
    i += run;
  }
  return out;
}

Result<Bytes> write_jcamp(const AnyIR& value, WriteContext& ctx) {
  const ir::Table& table = value.get<ir::Table>();
  const ir::Sheet* data = nullptr;
  for (const auto& sheet : table.sheets) {
    if (!util::iequals(sheet.name, "header")) {
      data = &sheet;
      break;
    }
  }
  if (data == nullptr) return fail(Status::Encode, "the table has no spectrum sheet");
  std::vector<double> xs;
  std::vector<double> ys;
  for (std::size_t r = 0; r < data->rows.size(); ++r) {
    const auto& row = data->rows[r];
    if (row.size() < 2) continue;
    const auto x = row[0].type == ir::CellType::Number ? std::optional<double>(row[0].number) : to_double(row[0].text);
    const auto y = row[1].type == ir::CellType::Number ? std::optional<double>(row[1].number) : to_double(row[1].text);
    if (!x || !y) {
      if (r == 0) continue;  // header row
      return fail(Status::Encode, "row {} of the spectrum is not numeric", r);
    }
    xs.push_back(*x);
    ys.push_back(*y);
  }
  if (xs.empty()) return fail(Status::Encode, "the spectrum has no numeric (x, y) rows");
  const std::string form = ctx.options.get_string("jcamp_form", "asdf");
  std::string title = "spectrum";
  std::string xunits = "ARBITRARY UNITS";
  std::string yunits = "ARBITRARY UNITS";
  std::string data_type = "UNKNOWN";
  std::vector<std::pair<std::string, std::string>> extra;
  if (const ir::Sheet* header = find_sheet(table, "header")) {
    for (std::size_t r = 1; r < header->rows.size(); ++r) {
      const std::string key(cell_text(*header, r, 0));
      const std::string val(cell_text(*header, r, 1));
      const std::string norm = jcamp_label(key);
      if (norm == "TITLE") title = val;
      else if (norm == "XUNITS") xunits = val;
      else if (norm == "YUNITS") yunits = val;
      else if (norm == "DATATYPE") data_type = val;
      else if (norm == "JCAMPDX" || norm == "XYDATA" || norm == "XYPOINTS" || norm == "PEAKTABLE" || norm == "XFACTOR" ||
               norm == "YFACTOR" || norm == "FIRSTX" || norm == "LASTX" || norm == "NPOINTS" || norm == "FIRSTY" ||
               norm == "MAXY" || norm == "MINY" || norm == "MAXX" || norm == "MINX" || norm == "DELTAX" || norm == "END" ||
               norm == "OWNER" || norm == "ORIGIN" || norm == "DATE" || norm == "LONGDATE" || norm == "SOURCEREFERENCE") {
        continue;  // recomputed, or identifying fields dropped
      } else if (val.find('\n') == std::string::npos) {
        extra.emplace_back(key, val);
      }
    }
  }
  std::string out = "##TITLE=" + title + "\n##JCAMP-DX=4.24\n##DATA TYPE=" + data_type + "\n";
  out += "##ORIGIN=\n##OWNER=\n";
  for (const auto& [k, v] : extra) out += "##" + k + "=" + v + "\n";
  out += "##XUNITS=" + xunits + "\n##YUNITS=" + yunits + "\n";
  const std::size_t n = xs.size();
  bool uniform = n >= 2;
  const double step = n >= 2 ? (xs[n - 1] - xs[0]) / static_cast<double>(n - 1) : 0;
  for (std::size_t i = 1; i < n && uniform; ++i) {
    const double expected = xs[0] + step * static_cast<double>(i);
    uniform = std::fabs(xs[i] - expected) <= 1e-6 * std::max(1.0, std::fabs(step) * static_cast<double>(n));
  }
  const auto [min_y, max_y] = std::minmax_element(ys.begin(), ys.end());
  out += std::format("##FIRSTX={}\n##LASTX={}\n##NPOINTS={}\n##FIRSTY={}\n##MAXY={}\n##MINY={}\n", fmt_num(xs.front()),
                     fmt_num(xs.back()), n, fmt_num(ys.front()), fmt_num(*max_y), fmt_num(*min_y));
  if (!uniform || step == 0) {
    out += "##XFACTOR=1\n##YFACTOR=1\n##XYPOINTS=(XY..XY)\n";
    for (std::size_t i = 0; i < n; ++i) out += fmt_num(xs[i]) + ", " + fmt_num(ys[i]) + "\n";
  } else if (form == "affn") {
    out += "##XFACTOR=1\n##YFACTOR=1\n##XYDATA=(X++(Y..Y))\n";
    for (std::size_t i = 0; i < n; i += 8) {
      std::string line = fmt_num(xs[i]);
      for (std::size_t k = i; k < std::min(n, i + 8); ++k) line += " " + fmt_num(ys[k]);
      out += line + "\n";
    }
  } else {
    // Integer ordinates: the coarsest power-of-ten factor that represents
    // every value exactly (decimal data), at most 9 significant digits.
    const double largest = std::max(std::fabs(*max_y), std::fabs(*min_y));
    const int top = largest > 0 ? static_cast<int>(std::floor(std::log10(largest))) : 0;
    double yfactor = std::pow(10.0, top - 8);
    for (int k = top; k >= top - 8; --k) {
      const double f = std::pow(10.0, k);
      bool exact = true;
      for (const double y : ys) {
        const double scaled = y / f;
        if (std::fabs(scaled - std::round(scaled)) > 1e-6) {
          exact = false;
          break;
        }
      }
      if (exact) {
        yfactor = f;
        break;
      }
    }
    std::vector<std::int64_t> ints(n);
    for (std::size_t i = 0; i < n; ++i) ints[i] = static_cast<std::int64_t>(std::llround(ys[i] / yfactor));
    out += "##XFACTOR=1\n##YFACTOR=" + fmt_num(yfactor, 12) + "\n##XYDATA=(X++(Y..Y))\n";
    // Each line ends with the ordinate that the next line repeats as its
    // Y check value (DIF form), so consecutive lines overlap by one point.
    constexpr std::size_t per_line = 10;
    std::size_t start = 0;
    do {
      const std::size_t end = std::min(start + per_line, n - 1);
      out += fmt_num(xs[start]) + asdf_encode(ints, start, end - start + 1) + "\n";
      start = end;
    } while (start < n - 1);
  }
  out += "##END=\n";
  return to_bytes(out);
}

int sniff_jcamp(ByteView data, std::string_view /*name*/) {
  const std::string_view head = head_text(data, 1024);
  if (!util::trim_left(head).starts_with("##")) return 0;
  return head.find("##JCAMP") != std::string_view::npos || head.find("##JCAMPDX") != std::string_view::npos ? 95 : 0;
}

// --------------------------------------------------------------------------- mzML / mzXML

Result<std::vector<double>> decode_array(std::string_view base64, bool zlib, int bits, bool integer, bool big_endian,
                                         const Context& ctx) {
  std::string compact;
  compact.reserve(base64.size());
  for (const char c : base64) {
    if (!util::is_ascii_space(c)) compact.push_back(c);
  }
  CVT_TRY_ASSIGN(Bytes raw, util::base64_decode(compact));
  if (zlib && !raw.empty()) {
    CVT_TRY_ASSIGN(raw, util::zlib_decompress(raw, ctx.limits.max_decompressed_bytes));
  }
  const std::size_t width = static_cast<std::size_t>(bits / 8);
  if (width == 0 || raw.size() % width != 0) return fail(Status::Decode, "binary array size is not a multiple of {}", width);
  std::vector<double> out;
  out.reserve(raw.size() / width);
  util::ByteReader reader(raw);
  const auto order = big_endian ? std::endian::big : std::endian::little;
  while (!reader.at_end()) {
    if (bits == 64) {
      const std::uint64_t v = reader.read<std::uint64_t>(order);
      out.push_back(integer ? static_cast<double>(static_cast<std::int64_t>(v)) : std::bit_cast<double>(v));
    } else {
      const std::uint32_t v = reader.read<std::uint32_t>(order);
      out.push_back(integer ? static_cast<double>(static_cast<std::int32_t>(v)) : static_cast<double>(std::bit_cast<float>(v)));
    }
  }
  return out;
}

struct Scan {
  std::string id;
  int ms_level = 1;
  double rt = std::numeric_limits<double>::quiet_NaN();
  std::vector<double> mz;
  std::vector<double> intensity;
};

ir::Table scans_table(const std::vector<Scan>& scans) {
  ir::Sheet peaks = header_sheet("peaks", {"scan", "ms_level", "retention_time_s", "mz", "intensity"});
  ir::Sheet summary = header_sheet("scans", {"scan", "id", "ms_level", "retention_time_s", "peaks"});
  for (std::size_t s = 0; s < scans.size(); ++s) {
    const Scan& scan = scans[s];
    const ir::Cell rt = std::isnan(scan.rt) ? ir::Cell{} : number_cell(scan.rt);
    summary.rows.push_back({int_cell(static_cast<std::int64_t>(s + 1)), text_cell(scan.id), int_cell(scan.ms_level), rt,
                            int_cell(static_cast<std::int64_t>(scan.mz.size()))});
    for (std::size_t i = 0; i < scan.mz.size(); ++i) {
      peaks.rows.push_back({int_cell(static_cast<std::int64_t>(s + 1)), int_cell(scan.ms_level), rt, number_cell(scan.mz[i]),
                            number_cell(i < scan.intensity.size() ? scan.intensity[i] : 0.0)});
    }
  }
  ir::Table table;
  table.sheets.push_back(std::move(peaks));
  table.sheets.push_back(std::move(summary));
  return table;
}

Result<AnyIR> read_mzml(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const util::XmlDocument doc, util::parse_xml(ctx.input));
  std::vector<Scan> scans;
  std::uint64_t points = 0;
  for (const util::XmlNode spectrum : doc.root().descendants()) {
    if (!spectrum.is_element() || spectrum.local_name() != "spectrum") continue;
    Scan scan;
    scan.id = std::string(spectrum.attribute("id").value());
    for (const util::XmlNode node : spectrum.descendants()) {
      if (!node.is_element() || node.local_name() != "cvParam") continue;
      const std::string_view accession = node.attribute("accession").value();
      if (accession == "MS:1000511") scan.ms_level = node.attribute("value").as_int(1);
      if (accession == "MS:1000016") {
        double rt = node.attribute("value").as_double();
        const std::string_view unit = node.attribute("unitName").value();
        if (unit == "minute" || node.attribute("unitAccession").value() == "UO:0000031") rt *= 60.0;
        scan.rt = rt;
      }
    }
    for (const util::XmlNode array : spectrum.descendants()) {
      if (!array.is_element() || array.local_name() != "binaryDataArray") continue;
      int bits = 64;
      bool integer = false;
      bool zlib = false;
      int kind = 0;  // 1 = m/z, 2 = intensity
      for (const util::XmlNode p : array.children_local("cvParam")) {
        const std::string_view a = p.attribute("accession").value();
        if (a == "MS:1000523") bits = 64, integer = false;
        if (a == "MS:1000521") bits = 32, integer = false;
        if (a == "MS:1000522") bits = 64, integer = true;
        if (a == "MS:1000519") bits = 32, integer = true;
        if (a == "MS:1000574") zlib = true;
        if (a == "MS:1000514") kind = 1;
        if (a == "MS:1000515") kind = 2;
        if (a == "MS:1001478" || a == "MS:1002312" || a == "MS:1002313" || a == "MS:1002314" || a == "MS:1002746") {
          return fail(Status::Decode, "MS-Numpress compressed arrays are not supported");
        }
      }
      if (kind == 0) continue;
      const util::XmlNode binary = array.child_local("binary");
      CVT_TRY_ASSIGN(auto values, decode_array(binary ? binary.text() : std::string(), zlib, bits, integer, false, ctx));
      points += values.size();
      CVT_TRY(check_cells(ctx, points * 5));
      (kind == 1 ? scan.mz : scan.intensity) = std::move(values);
    }
    scans.push_back(std::move(scan));
    CVT_TRY(ctx.checkpoint(0.5));
  }
  if (scans.empty()) return fail(Status::Decode, "no <spectrum> elements in the mzML file");
  return scans_table(scans);
}

double xml_duration_seconds(std::string_view text) {
  // xs:duration such as "PT12.5S" or "PT1M2S".
  if (!text.starts_with("PT")) return to_double(text).value_or(std::numeric_limits<double>::quiet_NaN());
  double total = 0;
  std::string number;
  for (const char c : text.substr(2)) {
    if (util::is_ascii_digit(c) || c == '.') {
      number.push_back(c);
      continue;
    }
    const double v = to_double(number).value_or(0);
    number.clear();
    if (c == 'H') total += v * 3600;
    if (c == 'M') total += v * 60;
    if (c == 'S') total += v;
  }
  return total;
}

Result<AnyIR> read_mzxml(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const util::XmlDocument doc, util::parse_xml(ctx.input));
  std::vector<Scan> scans;
  std::uint64_t points = 0;
  for (const util::XmlNode node : doc.root().descendants()) {
    if (!node.is_element() || node.local_name() != "scan") continue;
    Scan scan;
    scan.id = std::string(node.attribute("num").value());
    scan.ms_level = node.attribute("msLevel").as_int(1);
    scan.rt = xml_duration_seconds(node.attribute("retentionTime").value());
    const util::XmlNode peaks = node.child_local("peaks");
    if (peaks) {
      const int bits = peaks.attribute("precision").as_int(32);
      if (bits != 32 && bits != 64) return fail(Status::Decode, "unsupported mzXML precision {}", bits);
      const bool zlib = peaks.attribute("compressionType").value() == "zlib";
      const std::string_view order = peaks.attribute("byteOrder").value();
      const std::string_view content = peaks.attribute("contentType").value();
      if (!content.empty() && content != "m/z-int") return fail(Status::Decode, "unsupported mzXML contentType {}", content);
      CVT_TRY_ASSIGN(auto values, decode_array(peaks.text(), zlib, bits, false, order != "little", ctx));
      points += values.size();
      CVT_TRY(check_cells(ctx, points * 3));
      for (std::size_t i = 0; i + 1 < values.size(); i += 2) {
        scan.mz.push_back(values[i]);
        scan.intensity.push_back(values[i + 1]);
      }
    }
    scans.push_back(std::move(scan));
    CVT_TRY(ctx.checkpoint(0.5));
  }
  if (scans.empty()) return fail(Status::Decode, "no <scan> elements in the mzXML file");
  return scans_table(scans);
}

int sniff_xml_root(ByteView data, std::string_view root) {
  const std::string_view head = head_text(data, 4096);
  if (head.find(std::string("<") + std::string(root)) == std::string_view::npos) return 0;
  return head.find("<?xml") != std::string_view::npos || head.starts_with("<") ? 93 : 0;
}

}  // namespace

void register_spectra(Registry& registry) {
  add_format(registry, {.id = "jcamp",
                        .name = "JCAMP-DX spectrum",
                        .description = "IUPAC JCAMP-DX spectrum (IR, UV-Vis, NMR, MS ...).",
                        .extensions = {"jdx", "dx", "jcamp", "jcm"},
                        .mime = "chemical/x-jcamp-dx",
                        .popularity = 15,
                        .sniff = sniff_jcamp});
  add_codec(registry, "jcamp", Kind::Table, read_jcamp, write_jcamp, {},
            {OptionSpec::choice("jcamp_form", "Data form", "asdf", {"asdf", "affn"},
                                "ASDF (DIFDUP) is compact; AFFN writes plain numbers.")});
  add_format(registry, {.id = "mzml",
                        .name = "mzML mass spectra",
                        .description = "HUPO-PSI mzML mass-spectrometry run.",
                        .extensions = {"mzml"},
                        .mime = "application/x-mzml",
                        .popularity = 15,
                        .sniff = [](ByteView d, std::string_view) { return sniff_xml_root(d, "mzML") ? 95 : sniff_xml_root(d, "indexedmzML"); }});
  add_codec(registry, "mzml", Kind::Table, read_mzml, nullptr);
  add_format(registry, {.id = "mzxml",
                        .name = "mzXML mass spectra",
                        .description = "mzXML mass-spectrometry run.",
                        .extensions = {"mzxml"},
                        .mime = "application/x-mzxml",
                        .popularity = 10,
                        .sniff = [](ByteView d, std::string_view) { return sniff_xml_root(d, "mzXML"); }});
  add_codec(registry, "mzxml", Kind::Table, read_mzxml, nullptr);
}

}  // namespace cvt::science
