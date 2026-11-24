// Convertible core - science family: colour palettes and swatch exchange.
// Adobe ACO / ASE / ACT, GIMP GPL, JASC-PAL (Paint Shop Pro / Paint.NET),
// RIFF PAL, CSS / SCSS / LESS variables and design-token JSON, on one model:
//
//   Data  {"name","colors":[{"name","hex","rgb":[r,g,b],"alpha","space",
//          "values":[original components],"group"}]}
//   Table name, hex, r, g, b, alpha, space, values
//   Image a swatch sheet (reading); a picture's dominant colours (writing,
//         median cut).
// Lab swatches are converted with a D50 white by default (Adobe), D65 on request.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <map>

#include "families/science/colour.hpp"
#include "families/science/common.hpp"
#include "families/science/science.hpp"
#include "util/text.hpp"

namespace cvt::science {

namespace {

using colour::Vec3;

struct Swatch {
  std::string name;
  Vec3 rgb{0, 0, 0};  // sRGB 0..1
  double alpha = 1.0;
  std::string space = "RGB";
  std::vector<double> values;
  std::string group;
};

struct Palette {
  std::string name;
  std::vector<Swatch> colors;
};

std::uint8_t to_byte(double v) { return static_cast<std::uint8_t>(std::clamp(std::lround(v * 255.0), 0L, 255L)); }

std::string hex_of(const Swatch& s, bool with_alpha = false) {
  std::string out = std::format("#{:02x}{:02x}{:02x}", to_byte(s.rgb[0]), to_byte(s.rgb[1]), to_byte(s.rgb[2]));
  if (with_alpha && s.alpha < 1.0) out += std::format("{:02x}", to_byte(s.alpha));
  return out;
}

Swatch rgb_swatch(std::string name, double r, double g, double b) {
  Swatch s;
  s.name = std::move(name);
  s.rgb = {r, g, b};
  s.values = {r * 255, g * 255, b * 255};
  return s;
}

colour::Illuminant lab_white(const Options& options) {
  return options.get_string("lab_white", "d50") == "d65" ? colour::Illuminant::D65 : colour::Illuminant::D50;
}

// --------------------------------------------------------------------------- colour strings

std::optional<Swatch> parse_colour(std::string_view text) {
  text = util::trim(text);
  if (text.starts_with('#')) {
    std::string_view h = text.substr(1);
    std::string full;
    if (h.size() == 3 || h.size() == 4) {
      for (const char c : h) full += std::string(2, c);
    } else if (h.size() == 6 || h.size() == 8) {
      full = std::string(h);
    } else {
      return std::nullopt;
    }
    const auto v = util::parse_int(full, 16);
    if (!v) return std::nullopt;
    const auto value = static_cast<std::uint64_t>(*v);
    Swatch s;
    if (full.size() == 8) {
      s = rgb_swatch("", ((value >> 24) & 255) / 255.0, ((value >> 16) & 255) / 255.0, ((value >> 8) & 255) / 255.0);
      s.alpha = (value & 255) / 255.0;
    } else {
      s = rgb_swatch("", ((value >> 16) & 255) / 255.0, ((value >> 8) & 255) / 255.0, (value & 255) / 255.0);
    }
    return s;
  }
  const std::size_t open = text.find('(');
  if (open == std::string_view::npos || !text.ends_with(')')) return std::nullopt;
  const std::string fn = util::to_lower(util::trim(text.substr(0, open)));
  std::string inner(text.substr(open + 1, text.size() - open - 2));
  std::replace(inner.begin(), inner.end(), ',', ' ');
  std::replace(inner.begin(), inner.end(), '/', ' ');
  std::vector<double> parts;
  std::vector<bool> percent;
  for (const auto w : words(inner)) {
    std::string_view v = w;
    const bool pct = v.ends_with('%');
    if (pct) v.remove_suffix(1);
    if (v.ends_with("deg")) v.remove_suffix(3);
    const auto d = to_double(v);
    if (!d) return std::nullopt;
    parts.push_back(*d);
    percent.push_back(pct);
  }
  if (parts.size() < 3) return std::nullopt;
  Swatch s;
  if (fn == "rgb" || fn == "rgba") {
    for (std::size_t i = 0; i < 3; ++i) s.rgb[i] = percent[i] ? parts[i] / 100.0 : parts[i] / 255.0;
    s.values = {s.rgb[0] * 255, s.rgb[1] * 255, s.rgb[2] * 255};
  } else if (fn == "hsl" || fn == "hsla") {
    s.rgb = colour::hsl_to_rgb({parts[0], parts[1] / 100.0, parts[2] / 100.0});
    s.space = "HSL";
    s.values = {parts[0], parts[1], parts[2]};
  } else {
    return std::nullopt;
  }
  if (parts.size() >= 4) s.alpha = percent[3] ? parts[3] / 100.0 : parts[3];
  return s;
}

// --------------------------------------------------------------------------- model <-> Data/Table

Json palette_json(const Palette& p) {
  Json colors = Json::array();
  for (const Swatch& s : p.colors) {
    Json c = Json::object();
    c["name"] = s.name;
    c["hex"] = hex_of(s);
    c["rgb"] = Json::array({to_byte(s.rgb[0]), to_byte(s.rgb[1]), to_byte(s.rgb[2])});
    if (s.alpha < 1.0) c["alpha"] = json_number(std::round(s.alpha * 1000) / 1000);
    c["space"] = s.space;
    Json values = Json::array();
    for (const double v : s.values) values.push_back(json_number(std::round(v * 10000) / 10000));
    c["values"] = std::move(values);
    if (!s.group.empty()) c["group"] = s.group;
    colors.push_back(std::move(c));
  }
  return Json::object({{"name", p.name}, {"colors", std::move(colors)}});
}

void collect_tokens(const Json& node, const std::string& prefix, Palette& p, int depth) {
  if (depth > 32 || p.colors.size() > 65536) return;
  if (node.is_string()) {
    if (auto s = parse_colour(node.as_string())) {
      s->name = prefix;
      p.colors.push_back(std::move(*s));
    }
    return;
  }
  if (node.is_object()) {
    if (const Json* value = node.find("$value"); value != nullptr && value->is_string()) {
      collect_tokens(*value, prefix, p, depth + 1);
      return;
    }
    for (const auto& [key, child] : node.items()) {
      if (key.starts_with('$')) continue;
      collect_tokens(child, prefix.empty() ? key : prefix + "." + key, p, depth + 1);
    }
    return;
  }
  if (node.is_array()) {
    std::size_t i = 0;
    for (const Json& child : node.elements()) collect_tokens(child, prefix.empty() ? std::to_string(++i) : prefix + "." + std::to_string(++i), p, depth + 1);
  }
}

Result<Palette> json_palette(const Json& value) {
  Palette p;
  p.name = value["name"].as_string();
  if (const Json* colors = value.find("colors"); colors != nullptr && colors->is_array()) {
    for (const Json& c : colors->elements()) {
      Swatch s;
      if (c.is_string()) {
        auto parsed = parse_colour(c.as_string());
        if (!parsed) continue;
        s = *parsed;
      } else if (const Json* rgb = c.find("rgb"); rgb != nullptr && rgb->size() >= 3) {
        s = rgb_swatch("", (*rgb)[0].as_double() / 255, (*rgb)[1].as_double() / 255, (*rgb)[2].as_double() / 255);
      } else if (auto parsed = parse_colour(c["hex"].as_string())) {
        s = *parsed;
      } else {
        continue;
      }
      if (c.is_object()) {
        s.name = c["name"].as_string();
        if (const Json* a = c.find("alpha")) s.alpha = a->as_double(1.0);
        if (const Json* space = c.find("space"); space != nullptr && space->is_string()) s.space = space->as_string();
        if (const Json* values = c.find("values"); values != nullptr && values->is_array()) {
          s.values.clear();
          for (const Json& v : values->elements()) s.values.push_back(v.as_double());
        }
        s.group = c["group"].as_string();
      }
      p.colors.push_back(std::move(s));
    }
  } else {
    collect_tokens(value, "", p, 0);
  }
  if (p.colors.empty()) return fail(Status::Encode, "the data holds no colours");
  return p;
}

ir::Table palette_table(const Palette& p) {
  ir::Sheet sheet = header_sheet(p.name.empty() ? "palette" : p.name, {"name", "hex", "r", "g", "b", "alpha", "space", "values"});
  for (const Swatch& s : p.colors) {
    std::string values;
    for (const double v : s.values) values += (values.empty() ? "" : " ") + fmt_num(std::round(v * 10000) / 10000);
    sheet.rows.push_back({text_cell(s.name), ir::Cell::string(hex_of(s)), int_cell(to_byte(s.rgb[0])), int_cell(to_byte(s.rgb[1])),
                          int_cell(to_byte(s.rgb[2])), number_cell(std::round(s.alpha * 1000) / 1000), ir::Cell::string(s.space),
                          text_cell(values)});
  }
  ir::Table t;
  t.sheets.push_back(std::move(sheet));
  return t;
}

Result<Palette> table_palette(const ir::Table& t) {
  if (t.sheets.empty()) return fail(Status::Encode, "the table has no sheets");
  const ir::Sheet& sheet = t.sheets.front();
  Palette p;
  p.name = sheet.name;
  const int name = column_of(sheet, "name");
  const int hex = std::max(column_of(sheet, "hex"), column_of(sheet, "color"));
  const int r = column_of(sheet, "r"), g = column_of(sheet, "g"), b = column_of(sheet, "b");
  const int alpha = column_of(sheet, "alpha");
  for (std::size_t row = 1; row < sheet.rows.size(); ++row) {
    std::optional<Swatch> s;
    if (r >= 0 && g >= 0 && b >= 0 && !cell_text(sheet, row, r).empty()) {
      s = rgb_swatch("", to_double(cell_text(sheet, row, r)).value_or(0) / 255, to_double(cell_text(sheet, row, g)).value_or(0) / 255,
                     to_double(cell_text(sheet, row, b)).value_or(0) / 255);
    } else if (hex >= 0) {
      s = parse_colour(cell_text(sheet, row, hex));
    }
    if (!s) continue;
    s->name = std::string(cell_text(sheet, row, name));
    if (alpha >= 0 && !cell_text(sheet, row, alpha).empty()) s->alpha = to_double(cell_text(sheet, row, alpha)).value_or(1);
    p.colors.push_back(std::move(*s));
  }
  if (p.colors.empty()) return fail(Status::Encode, "the table has no colour rows (columns hex or r, g, b)");
  return p;
}

// --------------------------------------------------------------------------- images

ir::Image swatch_sheet(const Palette& p, const Options& options) {
  const auto size = static_cast<std::uint32_t>(options.get_int("swatch_size", 32));
  const std::size_t n = std::max<std::size_t>(1, p.colors.size());
  auto columns = static_cast<std::size_t>(options.get_int("swatch_columns", 0));
  if (columns == 0) columns = std::min<std::size_t>(n, 16);
  const std::size_t rows = (n + columns - 1) / columns;
  ir::Frame f = ir::Frame::filled(static_cast<std::uint32_t>(columns * size), static_cast<std::uint32_t>(rows * size), {0, 0, 0, 0});
  for (std::size_t i = 0; i < p.colors.size(); ++i) {
    const Swatch& s = p.colors[i];
    const std::uint32_t x0 = static_cast<std::uint32_t>((i % columns) * size), y0 = static_cast<std::uint32_t>((i / columns) * size);
    for (std::uint32_t y = y0; y < y0 + size; ++y) {
      for (std::uint32_t x = x0; x < x0 + size; ++x) {
        const std::size_t at = f.offset(x, y);
        f.rgba[at] = to_byte(s.rgb[0]);
        f.rgba[at + 1] = to_byte(s.rgb[1]);
        f.rgba[at + 2] = to_byte(s.rgb[2]);
        f.rgba[at + 3] = to_byte(s.alpha);
      }
    }
  }
  ir::Image image;
  image.frames.push_back(std::move(f));
  return image;
}

/// Median-cut quantisation of an image's opaque pixels.
Palette palette_from_image(const ir::Image& image, std::size_t wanted, const Context& ctx) {
  std::map<std::uint32_t, std::uint64_t> counts;
  const ir::Frame& f = image.frames.front();
  for (std::size_t i = 0; i < f.rgba.size(); i += 4) {
    if (f.rgba[i + 3] < 128) continue;
    ++counts[(std::uint32_t{f.rgba[i]} << 16) | (std::uint32_t{f.rgba[i + 1]} << 8) | f.rgba[i + 2]];
  }
  struct Entry {
    std::array<int, 3> c;
    std::uint64_t n;
  };
  std::vector<Entry> all;
  for (const auto& [rgb, n] : counts) all.push_back({{static_cast<int>(rgb >> 16), static_cast<int>((rgb >> 8) & 255), static_cast<int>(rgb & 255)}, n});
  std::vector<std::vector<Entry>> boxes;
  if (!all.empty()) boxes.push_back(std::move(all));
  while (boxes.size() < wanted) {
    // Split the box with the widest channel range (weighted by population).
    std::size_t best = boxes.size();
    int best_range = 0;
    int best_channel = 0;
    for (std::size_t b = 0; b < boxes.size(); ++b) {
      if (boxes[b].size() < 2) continue;
      for (int c = 0; c < 3; ++c) {
        const auto [lo, hi] = std::minmax_element(boxes[b].begin(), boxes[b].end(), [c](const Entry& x, const Entry& y) {
          return x.c[static_cast<std::size_t>(c)] < y.c[static_cast<std::size_t>(c)];
        });
        const int range = hi->c[static_cast<std::size_t>(c)] - lo->c[static_cast<std::size_t>(c)];
        if (range > best_range) {
          best_range = range;
          best = b;
          best_channel = c;
        }
      }
    }
    if (best == boxes.size()) break;
    auto& box = boxes[best];
    std::sort(box.begin(), box.end(), [best_channel](const Entry& x, const Entry& y) {
      return x.c[static_cast<std::size_t>(best_channel)] < y.c[static_cast<std::size_t>(best_channel)];
    });
    std::uint64_t total = 0;
    for (const auto& e : box) total += e.n;
    std::uint64_t acc = 0;
    std::size_t cut = 1;
    for (std::size_t i = 0; i + 1 < box.size(); ++i) {
      acc += box[i].n;
      cut = i + 1;
      if (acc * 2 >= total) break;
    }
    std::vector<Entry> upper(box.begin() + static_cast<std::ptrdiff_t>(cut), box.end());
    box.resize(cut);
    boxes.push_back(std::move(upper));
  }
  // Most populous first.
  std::vector<std::pair<std::uint64_t, Vec3>> averaged;
  for (const auto& box : boxes) {
    std::uint64_t n = 0;
    Vec3 sum{0, 0, 0};
    for (const auto& e : box) {
      n += e.n;
      for (int c = 0; c < 3; ++c) sum[static_cast<std::size_t>(c)] += static_cast<double>(e.c[static_cast<std::size_t>(c)]) * static_cast<double>(e.n);
    }
    if (n > 0) averaged.emplace_back(n, Vec3{std::round(sum[0] / n) / 255, std::round(sum[1] / n) / 255, std::round(sum[2] / n) / 255});
  }
  std::stable_sort(averaged.begin(), averaged.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  Palette p;
  p.name = "image colours";
  for (std::size_t i = 0; i < averaged.size(); ++i) {
    Swatch s = rgb_swatch("", averaged[i].second[0], averaged[i].second[1], averaged[i].second[2]);
    s.name = hex_of(s);
    p.colors.push_back(std::move(s));
  }
  if (image.animated()) ctx.warn("Only the first frame was used for the palette.");
  return p;
}

// --------------------------------------------------------------------------- formats

Result<Palette> read_gpl(ByteView input, const Context& /*ctx*/) {
  const std::string text = input_text(input);
  const auto lines = lines_of(text);
  if (lines.empty() || !util::trim(lines[0]).starts_with("GIMP Palette")) return fail(Status::Decode, "not a GIMP palette");
  Palette p;
  for (std::size_t i = 1; i < lines.size(); ++i) {
    const std::string_view line = util::trim(lines[i]);
    if (line.empty() || line.starts_with('#')) continue;
    if (line.starts_with("Name:")) {
      p.name = std::string(util::trim(line.substr(5)));
      continue;
    }
    if (line.starts_with("Columns:")) continue;
    const auto parts = words(line);
    if (parts.size() < 3) return fail(Status::Decode, "bad GIMP palette line {}", i + 1);
    const auto r = to_int(parts[0]), g = to_int(parts[1]), b = to_int(parts[2]);
    if (!r || !g || !b) return fail(Status::Decode, "bad GIMP palette line {}", i + 1);
    Swatch s = rgb_swatch("", static_cast<double>(*r) / 255, static_cast<double>(*g) / 255, static_cast<double>(*b) / 255);
    const std::size_t name_at = static_cast<std::size_t>(parts[2].data() + parts[2].size() - line.data());
    s.name = std::string(util::trim(line.substr(name_at)));
    p.colors.push_back(std::move(s));
    if (p.colors.size() > 100000) return fail(Status::Limit, "too many palette entries");
  }
  return p;
}

std::string write_gpl(const Palette& p) {
  std::string out = "GIMP Palette\nName: " + (p.name.empty() ? std::string("Palette") : p.name) + "\nColumns: 0\n#\n";
  for (const Swatch& s : p.colors) {
    out += std::format("{:>3} {:>3} {:>3}\t{}\n", to_byte(s.rgb[0]), to_byte(s.rgb[1]), to_byte(s.rgb[2]),
                       s.name.empty() ? hex_of(s) : s.name);
  }
  return out;
}

Result<Palette> read_jasc(ByteView input, const Context& /*ctx*/) {
  const std::string text = input_text(input);
  const auto lines = lines_of(text);
  if (lines.size() < 3 || util::trim(lines[0]) != "JASC-PAL") return fail(Status::Decode, "not a JASC palette");
  const auto count = to_int(lines[2]);
  if (!count || *count < 0 || static_cast<std::uint64_t>(*count) + 3 > lines.size()) return fail(Status::Decode, "bad JASC palette count");
  Palette p;
  for (std::int64_t i = 0; i < *count; ++i) {
    const auto parts = words(lines[3 + static_cast<std::size_t>(i)]);
    if (parts.size() < 3) return fail(Status::Decode, "bad JASC palette entry");
    p.colors.push_back(rgb_swatch("", to_double(parts[0]).value_or(0) / 255, to_double(parts[1]).value_or(0) / 255,
                                  to_double(parts[2]).value_or(0) / 255));
  }
  return p;
}

std::string write_jasc(const Palette& p) {
  std::string out = "JASC-PAL\r\n0100\r\n" + std::to_string(p.colors.size()) + "\r\n";
  for (const Swatch& s : p.colors) out += std::format("{} {} {}\r\n", to_byte(s.rgb[0]), to_byte(s.rgb[1]), to_byte(s.rgb[2]));
  return out;
}

Result<Palette> read_riff(ByteView input, const Context& /*ctx*/) {
  util::ByteReader r(input);
  if (!r.consume("RIFF")) return fail(Status::Decode, "not a RIFF palette");
  (void)r.u32le();
  if (!r.consume("PAL ")) return fail(Status::Decode, "not a RIFF palette");
  while (r.remaining() >= 8) {
    const std::string_view id = r.chars(4);
    const std::uint32_t size = r.u32le();
    if (size > r.remaining()) return fail(Status::Decode, "RIFF palette chunk is truncated");
    util::ByteReader chunk = r.sub(size);
    if (size % 2 == 1 && r.remaining() > 0) (void)r.u8();
    if (id != "data") continue;
    (void)chunk.u16le();
    const std::uint16_t count = chunk.u16le();
    if (std::size_t{count} * 4 > chunk.remaining()) return fail(Status::Decode, "RIFF palette data is truncated");
    Palette p;
    for (std::uint16_t i = 0; i < count; ++i) {
      const double red = chunk.u8() / 255.0, green = chunk.u8() / 255.0, blue = chunk.u8() / 255.0;
      (void)chunk.u8();
      p.colors.push_back(rgb_swatch("", red, green, blue));
    }
    return p;
  }
  return fail(Status::Decode, "RIFF palette has no data chunk");
}

Bytes write_riff(const Palette& p) {
  const std::size_t n = std::min<std::size_t>(p.colors.size(), 65535);
  util::ByteWriter out(24 + n * 4);
  out.text("RIFF").u32le(static_cast<std::uint32_t>(16 + n * 4)).text("PAL ").text("data").u32le(static_cast<std::uint32_t>(4 + n * 4));
  out.u16le(0x0300).u16le(static_cast<std::uint16_t>(n));
  for (std::size_t i = 0; i < n; ++i) {
    const Swatch& s = p.colors[i];
    out.u8(to_byte(s.rgb[0])).u8(to_byte(s.rgb[1])).u8(to_byte(s.rgb[2])).u8(0);
  }
  return out.take();
}

Result<Palette> read_act(ByteView input, const Context& /*ctx*/) {
  if (input.size() != 768 && input.size() != 772) return fail(Status::Decode, "an ACT colour table is 768 or 772 bytes");
  std::size_t count = 256;
  std::optional<std::size_t> transparent;
  if (input.size() == 772) {
    util::ByteReader r(input.subspan(768));
    count = std::min<std::size_t>(256, r.u16be());
    const std::uint16_t t = r.u16be();
    if (t < 256) transparent = t;
  }
  Palette p;
  for (std::size_t i = 0; i < count; ++i) {
    Swatch s = rgb_swatch("", static_cast<std::uint8_t>(input[i * 3]) / 255.0, static_cast<std::uint8_t>(input[i * 3 + 1]) / 255.0,
                          static_cast<std::uint8_t>(input[i * 3 + 2]) / 255.0);
    if (transparent && *transparent == i) s.alpha = 0;
    p.colors.push_back(std::move(s));
  }
  return p;
}

Bytes write_act(const Palette& p, const Context& ctx) {
  if (p.colors.size() > 256) ctx.warn("ACT tables hold 256 colours; the rest were dropped.");
  util::ByteWriter out(772);
  std::optional<std::size_t> transparent;
  for (std::size_t i = 0; i < 256; ++i) {
    if (i < p.colors.size()) {
      const Swatch& s = p.colors[i];
      out.u8(to_byte(s.rgb[0])).u8(to_byte(s.rgb[1])).u8(to_byte(s.rgb[2]));
      if (s.alpha == 0 && !transparent) transparent = i;
    } else {
      out.u8(0).u8(0).u8(0);
    }
  }
  out.u16be(static_cast<std::uint16_t>(std::min<std::size_t>(256, p.colors.size())));
  out.u16be(transparent ? static_cast<std::uint16_t>(*transparent) : 0xFFFF);
  return out.take();
}

/// One ACO colour record -> swatch.
Swatch aco_swatch(std::uint16_t space, std::array<std::uint16_t, 4> w, colour::Illuminant white) {
  Swatch s;
  switch (space) {
    case 0:
      s.space = "RGB";
      s.rgb = {w[0] / 65535.0, w[1] / 65535.0, w[2] / 65535.0};
      s.values = {w[0] / 257.0, w[1] / 257.0, w[2] / 257.0};
      break;
    case 1: {
      s.space = "HSB";
      const Vec3 hsv = {w[0] / 65535.0 * 360.0, w[1] / 65535.0, w[2] / 65535.0};
      s.rgb = colour::hsv_to_rgb(hsv);
      s.values = {hsv[0], hsv[1] * 100, hsv[2] * 100};
      break;
    }
    case 2: {
      s.space = "CMYK";
      const std::array<double, 4> cmyk = {1 - w[0] / 65535.0, 1 - w[1] / 65535.0, 1 - w[2] / 65535.0, 1 - w[3] / 65535.0};
      s.rgb = colour::cmyk_to_rgb(cmyk);
      s.values = {cmyk[0] * 100, cmyk[1] * 100, cmyk[2] * 100, cmyk[3] * 100};
      break;
    }
    case 7: {
      s.space = "LAB";
      const Vec3 lab = {w[0] / 100.0, static_cast<std::int16_t>(w[1]) / 100.0, static_cast<std::int16_t>(w[2]) / 100.0};
      s.rgb = colour::lab_to_srgb(lab, white);
      s.values = {lab[0], lab[1], lab[2]};
      break;
    }
    case 8:
      s.space = "GRAY";
      s.rgb = {1 - w[0] / 10000.0, 1 - w[0] / 10000.0, 1 - w[0] / 10000.0};
      s.values = {w[0] / 100.0};
      break;
    case 9: {
      s.space = "CMYK";
      const std::array<double, 4> cmyk = {w[0] / 10000.0, w[1] / 10000.0, w[2] / 10000.0, w[3] / 10000.0};
      s.rgb = colour::cmyk_to_rgb(cmyk);
      s.values = {cmyk[0] * 100, cmyk[1] * 100, cmyk[2] * 100, cmyk[3] * 100};
      break;
    }
    default:
      s.space = "UNKNOWN";
      break;
  }
  for (double& c : s.rgb) c = std::clamp(c, 0.0, 1.0);
  return s;
}

Result<Palette> read_aco(ByteView input, const Options& options) {
  util::ByteReader r(input);
  Palette p;
  const colour::Illuminant white = lab_white(options);
  std::uint16_t version = r.u16be();
  if (version != 1 && version != 2) return fail(Status::Decode, "not an ACO swatch file");
  std::uint16_t count = r.u16be();
  if (std::size_t{count} * 10 > r.remaining()) return fail(Status::Decode, "ACO swatch file is truncated");
  for (std::uint16_t i = 0; i < count; ++i) {
    const std::uint16_t space = r.u16be();
    const std::array<std::uint16_t, 4> w = {r.u16be(), r.u16be(), r.u16be(), r.u16be()};
    std::string name;
    if (version == 2) {
      (void)r.u16be();
      const std::uint32_t length = r.u32be();
      if (std::uint64_t{length} * 2 > r.remaining()) return fail(Status::Decode, "ACO name is truncated");
      std::u16string units;
      for (std::uint32_t k = 0; k < length; ++k) units.push_back(r.u16be());
      while (!units.empty() && units.back() == 0) units.pop_back();
      name = util::utf16_to_utf8(units);
    }
    Swatch s = aco_swatch(space, w, white);
    s.name = name;
    p.colors.push_back(std::move(s));
  }
  // A version 1 section is usually followed by a version 2 section with names.
  if (version == 1 && r.remaining() >= 4 && r.peek_u8() == 0 && r.peek_rest()[1] == std::byte{2}) {
    auto v2 = read_aco(r.peek_rest(), options);
    if (v2 && v2->colors.size() == p.colors.size()) return v2;
  }
  if (!r.ok()) return fail(Status::Decode, "ACO swatch file is truncated");
  return p;
}

Bytes write_aco(const Palette& p, const Options& options) {
  const colour::Illuminant white = lab_white(options);
  util::ByteWriter out(64 + p.colors.size() * 64);
  const auto record = [&](const Swatch& s) {
    const auto clamp16 = [](double v) { return static_cast<std::uint16_t>(std::clamp(std::lround(v), 0L, 65535L)); };
    if (s.space == "CMYK" && s.values.size() == 4) {
      out.u16be(2);
      for (int i = 0; i < 4; ++i) out.u16be(clamp16(65535.0 * (1 - s.values[static_cast<std::size_t>(i)] / 100.0)));
    } else if (s.space == "LAB" && s.values.size() == 3) {
      out.u16be(7).u16be(clamp16(s.values[0] * 100));
      out.i16be(static_cast<std::int16_t>(std::clamp(std::lround(s.values[1] * 100), -32768L, 32767L)));
      out.i16be(static_cast<std::int16_t>(std::clamp(std::lround(s.values[2] * 100), -32768L, 32767L)));
      out.u16be(0);
      (void)white;
    } else if (s.space == "GRAY" && s.values.size() == 1) {
      out.u16be(8).u16be(clamp16(s.values[0] * 100)).u16be(0).u16be(0).u16be(0);
    } else {
      out.u16be(0);
      for (int i = 0; i < 3; ++i) out.u16be(static_cast<std::uint16_t>(to_byte(s.rgb[static_cast<std::size_t>(i)]) * 257));
      out.u16be(0);
    }
  };
  const auto n = static_cast<std::uint16_t>(std::min<std::size_t>(p.colors.size(), 65535));
  out.u16be(1).u16be(n);
  for (std::size_t i = 0; i < n; ++i) record(p.colors[i]);
  out.u16be(2).u16be(n);
  for (std::size_t i = 0; i < n; ++i) {
    record(p.colors[i]);
    const std::u16string name = util::utf8_to_utf16(p.colors[i].name.empty() ? hex_of(p.colors[i]) : p.colors[i].name);
    out.u16be(0).u32be(static_cast<std::uint32_t>(name.size() + 1));
    for (const char16_t c : name) out.u16be(static_cast<std::uint16_t>(c));
    out.u16be(0);
  }
  return out.take();
}

Result<Palette> read_ase(ByteView input, const Options& options) {
  util::ByteReader r(input);
  if (!r.consume("ASEF")) return fail(Status::Decode, "not an Adobe Swatch Exchange file");
  (void)r.u16be();
  (void)r.u16be();
  const std::uint32_t blocks = r.u32be();
  const colour::Illuminant white = lab_white(options);
  Palette p;
  std::string group;
  for (std::uint32_t b = 0; b < blocks && r.remaining() >= 6; ++b) {
    const std::uint16_t type = r.u16be();
    const std::uint32_t length = r.u32be();
    if (length > r.remaining()) return fail(Status::Decode, "ASE block is truncated");
    util::ByteReader block = r.sub(length);
    if (type == 0xC002) {
      group.clear();
      continue;
    }
    std::string name;
    if (block.remaining() >= 2) {
      const std::uint16_t chars = block.u16be();
      std::u16string units;
      for (std::uint16_t k = 0; k < chars && block.remaining() >= 2; ++k) units.push_back(block.u16be());
      while (!units.empty() && units.back() == 0) units.pop_back();
      name = util::utf16_to_utf8(units);
    }
    if (type == 0xC001) {
      group = name;
      continue;
    }
    if (type != 0x0001) continue;
    const std::string_view model = block.chars(4);
    Swatch s;
    s.name = name;
    s.group = group;
    const auto f = [&block] { return static_cast<double>(block.f32be()); };
    if (model == "RGB ") {
      const double red = f(), green = f(), blue = f();
      s.rgb = {red, green, blue};
      s.values = {red * 255, green * 255, blue * 255};
    } else if (model == "CMYK") {
      const std::array<double, 4> cmyk = {f(), f(), f(), f()};
      s.space = "CMYK";
      s.rgb = colour::cmyk_to_rgb(cmyk);
      s.values = {cmyk[0] * 100, cmyk[1] * 100, cmyk[2] * 100, cmyk[3] * 100};
    } else if (model == "LAB ") {
      const Vec3 lab = {f() * 100.0, f(), f()};
      s.space = "LAB";
      s.rgb = colour::lab_to_srgb(lab, white);
      s.values = {lab[0], lab[1], lab[2]};
    } else if (model == "Gray") {
      const double g = f();
      s.space = "GRAY";
      s.rgb = {g, g, g};
      s.values = {g * 100};
    } else {
      return fail(Status::Decode, "unknown ASE colour model '{}'", model);
    }
    if (!block.ok()) return fail(Status::Decode, "ASE colour entry is truncated");
    for (double& c : s.rgb) c = std::clamp(c, 0.0, 1.0);
    p.colors.push_back(std::move(s));
    if (p.colors.size() > 100000) return fail(Status::Limit, "too many swatches");
  }
  return p;
}

Bytes write_ase(const Palette& p) {
  util::ByteWriter out(32 + p.colors.size() * 64);
  // Group blocks when swatches carry group names.
  std::vector<std::pair<std::string, std::vector<const Swatch*>>> groups;
  for (const Swatch& s : p.colors) {
    if (groups.empty() || groups.back().first != s.group) groups.push_back({s.group, {}});
    groups.back().second.push_back(&s);
  }
  std::uint32_t blocks = 0;
  util::ByteWriter body(32 + p.colors.size() * 64);
  const auto put_name = [](util::ByteWriter& w, const std::string& name) {
    const std::u16string units = util::utf8_to_utf16(name);
    w.u16be(static_cast<std::uint16_t>(units.size() + 1));
    for (const char16_t c : units) w.u16be(static_cast<std::uint16_t>(c));
    w.u16be(0);
  };
  for (const auto& [group, members] : groups) {
    if (!group.empty()) {
      util::ByteWriter g(64);
      put_name(g, group);
      body.u16be(0xC001).u32be(static_cast<std::uint32_t>(g.size())).bytes(g.view());
      ++blocks;
    }
    for (const Swatch* s : members) {
      util::ByteWriter e(64);
      put_name(e, s->name.empty() ? hex_of(*s) : s->name);
      if (s->space == "CMYK" && s->values.size() == 4) {
        e.text("CMYK");
        for (const double v : s->values) e.f32be(static_cast<float>(v / 100.0));
      } else if (s->space == "LAB" && s->values.size() == 3) {
        e.text("LAB ").f32be(static_cast<float>(s->values[0] / 100.0)).f32be(static_cast<float>(s->values[1])).f32be(static_cast<float>(s->values[2]));
      } else if (s->space == "GRAY" && s->values.size() == 1) {
        e.text("Gray").f32be(static_cast<float>(s->values[0] / 100.0));
      } else {
        e.text("RGB ");
        for (int c = 0; c < 3; ++c) e.f32be(static_cast<float>(to_byte(s->rgb[static_cast<std::size_t>(c)]) / 255.0));
      }
      e.u16be(2);  // normal
      body.u16be(0x0001).u32be(static_cast<std::uint32_t>(e.size())).bytes(e.view());
      ++blocks;
    }
    if (!group.empty()) {
      body.u16be(0xC002).u32be(0);
      ++blocks;
    }
  }
  out.text("ASEF").u16be(1).u16be(0).u32be(blocks).bytes(body.view());
  return out.take();
}

std::string css_name(const Swatch& s, std::size_t index) {
  std::string name;
  for (const char c : s.name.empty() ? "color-" + std::to_string(index + 1) : s.name) {
    if (util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '-' || c == '_') {
      name.push_back(util::ascii_lower(c));
    } else if (!name.empty() && name.back() != '-') {
      name.push_back('-');
    }
  }
  while (!name.empty() && name.back() == '-') name.pop_back();
  if (name.empty() || util::is_ascii_digit(name[0])) name = "color-" + name;
  return name;
}

Result<Palette> read_css_vars(ByteView input, char sigil) {
  const std::string text = input_text(input);
  Palette p;
  std::size_t at = 0;
  while ((at = text.find(sigil == '-' ? "--" : std::string(1, sigil), at)) != std::string::npos) {
    const std::size_t start = at + (sigil == '-' ? 2 : 1);
    std::size_t end = start;
    while (end < text.size() && (util::is_ascii_alpha(text[end]) || util::is_ascii_digit(text[end]) || text[end] == '-' || text[end] == '_')) ++end;
    const std::size_t colon = text.find_first_not_of(" \t", end);
    at = end;
    if (end == start || colon == std::string::npos || text[colon] != ':') continue;
    const std::size_t semicolon = text.find_first_of(";}\n", colon);
    const std::string_view value = util::trim(std::string_view(text).substr(colon + 1, semicolon == std::string::npos ? std::string::npos : semicolon - colon - 1));
    if (auto s = parse_colour(value)) {
      s->name = text.substr(start, end - start);
      p.colors.push_back(std::move(*s));
    }
    if (p.colors.size() > 100000) return fail(Status::Limit, "too many colours");
  }
  if (p.colors.empty()) return fail(Status::Decode, "no colour variables found");
  return p;
}

std::string write_css_vars(const Palette& p, char sigil) {
  std::string out = sigil == '-' ? ":root {\n" : "";
  for (std::size_t i = 0; i < p.colors.size(); ++i) {
    const Swatch& s = p.colors[i];
    const std::string value = s.alpha < 1.0 ? std::format("rgba({}, {}, {}, {})", to_byte(s.rgb[0]), to_byte(s.rgb[1]), to_byte(s.rgb[2]),
                                                          fmt_num(std::round(s.alpha * 1000) / 1000))
                                            : hex_of(s);
    if (sigil == '-') {
      out += "  --" + css_name(s, i) + ": " + value + ";\n";
    } else {
      out += std::string(1, sigil) + css_name(s, i) + ": " + value + ";\n";
    }
  }
  if (sigil == '-') out += "}\n";
  return out;
}

int sniff_css_vars(ByteView data, std::string_view needle) {
  const std::string_view head = head_text(data, 4096);
  std::size_t hits = 0;
  for (std::size_t at = head.find(needle); at != std::string_view::npos && hits < 3; at = head.find(needle, at + 1)) {
    const std::size_t colon = head.find(':', at);
    if (colon == std::string_view::npos) break;
    const std::string_view value = util::trim(head.substr(colon + 1, 30));
    if (value.starts_with('#') || value.starts_with("rgb") || value.starts_with("hsl")) ++hits;
  }
  return hits >= 2 ? 30 : 0;  // below a plain extension match: these are style sheets first
}

// --------------------------------------------------------------------------- registration

struct PaletteFormat {
  std::string id;
  std::function<Result<Palette>(ByteView, const Context&)> read;
  std::function<Result<Bytes>(const Palette&, const Context&)> write;
};

std::vector<OptionSpec> swatch_options() {
  return {OptionSpec::integer("swatch_size", "Swatch size", 32, 4, 512, "Edge length of each colour square, in pixels."),
          OptionSpec::integer("swatch_columns", "Columns", 0, 0, 256, "Swatches per row; 0 = up to 16.")};
}

void register_palette(Registry& registry, PaletteFormat f) {
  const std::vector<OptionSpec> lab = {OptionSpec::choice("lab_white", "Lab white point", "d50", {"d50", "d65"},
                                                          "Reference white of Lab swatches (Adobe uses D50).")};
  ReadFn read_data, read_table, read_image;
  WriteFn write_data, write_table, write_image;
  if (f.read) {
    read_data = [r = f.read](ReadContext& ctx) -> Result<AnyIR> {
      CVT_TRY_ASSIGN(const Palette p, r(ctx.input, ctx));
      return ir::Data{palette_json(p)};
    };
    read_table = [r = f.read](ReadContext& ctx) -> Result<AnyIR> {
      CVT_TRY_ASSIGN(const Palette p, r(ctx.input, ctx));
      return palette_table(p);
    };
    read_image = [r = f.read](ReadContext& ctx) -> Result<AnyIR> {
      CVT_TRY_ASSIGN(const Palette p, r(ctx.input, ctx));
      if (p.colors.empty()) return fail(Status::Decode, "the palette is empty");
      return swatch_sheet(p, ctx.options);
    };
  }
  if (f.write) {
    write_data = [w = f.write](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
      CVT_TRY_ASSIGN(const Palette p, json_palette(value.get<ir::Data>().value));
      return w(p, ctx);
    };
    write_table = [w = f.write](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
      CVT_TRY_ASSIGN(const Palette p, table_palette(value.get<ir::Table>()));
      return w(p, ctx);
    };
    write_image = [w = f.write](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
      const ir::Image& image = value.get<ir::Image>();
      if (!image.valid()) return fail(Status::Encode, "the image has no pixels");
      const Palette p = palette_from_image(image, static_cast<std::size_t>(ctx.options.get_int("palette_colors", 16)), ctx);
      if (p.colors.empty()) return fail(Status::Encode, "the image has no opaque pixels");
      return w(p, ctx);
    };
  }
  std::vector<OptionSpec> write_opts = lab;
  add_codec(registry, f.id, Kind::Data, read_data, write_data, lab, write_opts, 10, 10);
  add_codec(registry, f.id, Kind::Table, read_table, write_table, lab, write_opts, 12, 12);
  std::vector<OptionSpec> image_read = swatch_options();
  image_read.insert(image_read.end(), lab.begin(), lab.end());
  add_codec(registry, f.id, Kind::Image, read_image, write_image, image_read,
            {OptionSpec::integer("palette_colors", "Colours", 16, 1, 256, "Number of dominant colours taken from a picture.")},
            30, 40, true);
}

}  // namespace

void register_palettes(Registry& registry) {
  add_format(registry, {.id = "gpl", .name = "GIMP palette", .description = "GIMP/Inkscape/Krita colour palette.",
                        .category = Category::Image, .extensions = {"gpl"}, .mime = "text/x-gimp-palette", .popularity = 20,
                        .sniff = [](ByteView d, std::string_view) { return head_text(d, 16).starts_with("GIMP Palette") ? 97 : 0; }});
  register_palette(registry, {"gpl", read_gpl, [](const Palette& p, const Context&) -> Result<Bytes> { return to_bytes(write_gpl(p)); }});

  add_format(registry, {.id = "jasc-pal", .name = "JASC palette", .description = "Paint Shop Pro / Paint.NET style JASC-PAL palette.",
                        .category = Category::Image, .extensions = {"pal"}, .mime = "text/x-jasc-palette", .popularity = 14,
                        .sniff = [](ByteView d, std::string_view) { return head_text(d, 16).starts_with("JASC-PAL") ? 97 : 0; }});
  register_palette(registry, {"jasc-pal", read_jasc, [](const Palette& p, const Context&) -> Result<Bytes> { return to_bytes(write_jasc(p)); }});

  add_format(registry, {.id = "riff-pal", .name = "RIFF palette", .description = "Microsoft RIFF PAL colour palette.",
                        .category = Category::Image, .extensions = {"pal"}, .mime = "application/x-riff-palette", .popularity = 10,
                        .signatures = {Signature::at(0, "'RIFF' ?? ?? ?? ?? 'PAL '")}});
  register_palette(registry, {"riff-pal", read_riff, [](const Palette& p, const Context&) -> Result<Bytes> { return write_riff(p); }});

  add_format(registry, {.id = "act", .name = "Adobe Color Table", .description = "Photoshop indexed-colour table (256 RGB entries).",
                        .category = Category::Image, .extensions = {"act"}, .mime = "application/x-adobe-act", .popularity = 12,
                        .sniff = [](ByteView d, std::string_view name) {
                          return (d.size() == 768 || d.size() == 772) && has_extension(name, "act") ? 85 : 0;
                        }});
  register_palette(registry, {"act", read_act, [](const Palette& p, const Context& ctx) -> Result<Bytes> { return write_act(p, ctx); }});

  add_format(registry, {.id = "aco", .name = "Adobe Color Swatch", .description = "Photoshop colour swatches (.aco).",
                        .category = Category::Image, .extensions = {"aco"}, .mime = "application/x-adobe-aco", .popularity = 15,
                        .sniff = [](ByteView d, std::string_view name) {
                          return d.size() >= 4 && d[0] == std::byte{0} && (d[1] == std::byte{1} || d[1] == std::byte{2}) &&
                                         has_extension(name, "aco")
                                     ? 90
                                     : 0;
                        }});
  register_palette(registry, {"aco", [](ByteView in, const Context& ctx) { return read_aco(in, ctx.options); },
                              [](const Palette& p, const Context& ctx) -> Result<Bytes> { return write_aco(p, ctx.options); }});

  add_format(registry, {.id = "ase", .name = "Adobe Swatch Exchange", .description = "Swatches shared between Adobe applications.",
                        .category = Category::Image, .extensions = {"ase"}, .mime = "application/x-adobe-ase", .popularity = 18,
                        .signatures = {Signature::at(0, "'ASEF'")}});
  register_palette(registry, {"ase", [](ByteView in, const Context& ctx) { return read_ase(in, ctx.options); },
                              [](const Palette& p, const Context&) -> Result<Bytes> { return write_ase(p); }});

  add_format(registry, {.id = "palette-css", .name = "CSS colour variables", .description = "Colours as CSS custom properties (:root { --name: #hex; }).",
                        .category = Category::Code, .extensions = {"css"}, .mime = "text/css", .popularity = 5,
                        .sniff = [](ByteView d, std::string_view) { return sniff_css_vars(d, "--"); }});
  register_palette(registry, {"palette-css", [](ByteView in, const Context&) { return read_css_vars(in, '-'); },
                              [](const Palette& p, const Context&) -> Result<Bytes> { return to_bytes(write_css_vars(p, '-')); }});
  add_format(registry, {.id = "palette-scss", .name = "SCSS colour variables", .description = "Colours as Sass variables ($name: #hex;).",
                        .category = Category::Code, .extensions = {"scss"}, .mime = "text/x-scss", .popularity = 5,
                        .sniff = [](ByteView d, std::string_view) { return sniff_css_vars(d, "$"); }});
  register_palette(registry, {"palette-scss", [](ByteView in, const Context&) { return read_css_vars(in, '$'); },
                              [](const Palette& p, const Context&) -> Result<Bytes> { return to_bytes(write_css_vars(p, '$')); }});
  add_format(registry, {.id = "palette-less", .name = "LESS colour variables", .description = "Colours as LESS variables (@name: #hex;).",
                        .category = Category::Code, .extensions = {"less"}, .mime = "text/x-less", .popularity = 5,
                        .sniff = [](ByteView d, std::string_view) { return sniff_css_vars(d, "@"); }});
  register_palette(registry, {"palette-less", [](ByteView in, const Context&) { return read_css_vars(in, '@'); },
                              [](const Palette& p, const Context&) -> Result<Bytes> { return to_bytes(write_css_vars(p, '@')); }});

  add_format(registry, {.id = "color-tokens", .name = "Colour design tokens", .description = "Design-token JSON ({\"name\": {\"$type\": \"color\", \"$value\": \"#hex\"}}).",
                        .category = Category::Data, .extensions = {"tokens.json", "tokens"}, .mime = "application/design-tokens+json", .popularity = 8,
                        .sniff = [](ByteView d, std::string_view) {
                          const std::string_view h = head_text(d, 4096);
                          return h.find("\"$value\"") != std::string_view::npos && h.find("\"color\"") != std::string_view::npos ? 70 : 0;
                        }});
  register_palette(registry, {"color-tokens",
                              [](ByteView in, const Context&) -> Result<Palette> {
                                CVT_TRY_ASSIGN(const Json json, util::parse_json(input_text(in)));
                                auto p = json_palette(json);
                                if (!p) return fail(Status::Decode, "no colours in the token file");
                                return p;
                              },
                              [](const Palette& p, const Context&) -> Result<Bytes> {
                                Json root = Json::object();
                                for (std::size_t i = 0; i < p.colors.size(); ++i) {
                                  const Swatch& s = p.colors[i];
                                  root[css_name(s, i)] = Json::object({{"$type", "color"}, {"$value", hex_of(s, true)}});
                                }
                                return to_bytes(util::dump_json(root, 2) + "\n");
                              }});
}

}  // namespace cvt::science
