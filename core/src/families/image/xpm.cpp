// Convertible core - image family: X PixMap (XPM3).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/image/common.hpp"
#include "families/image/image.hpp"
#include "families/image/image_ops.hpp"
#include "util/bytes.hpp"
#include "util/text.hpp"

namespace cvt::image {

namespace {

/// Every "..." string literal of the C source, in order (no escapes are
/// needed for XPM content; \" and \\ are still handled).
std::vector<std::string_view> string_literals(std::string_view text, std::size_t limit) {
  std::vector<std::string_view> out;
  std::size_t p = 0;
  while (p < text.size() && out.size() < limit) {
    if (text.compare(p, 2, "/*") == 0) {
      const std::size_t end = text.find("*/", p + 2);
      if (end == std::string_view::npos) break;
      p = end + 2;
      continue;
    }
    if (text[p] != '"') {
      ++p;
      continue;
    }
    const std::size_t start = ++p;
    while (p < text.size() && text[p] != '"') {
      if (text[p] == '\\') ++p;
      ++p;
    }
    if (p >= text.size()) break;
    out.push_back(text.substr(start, p - start));
    ++p;
  }
  return out;
}

std::optional<Rgba> named_colour(std::string_view name) {
  struct Named {
    std::string_view name;
    Rgba rgba;
  };
  static constexpr Named table[] = {
      {"none", {0, 0, 0, 0}},           {"black", {0, 0, 0, 255}},        {"white", {255, 255, 255, 255}},
      {"red", {255, 0, 0, 255}},        {"green", {0, 255, 0, 255}},      {"blue", {0, 0, 255, 255}},
      {"yellow", {255, 255, 0, 255}},   {"cyan", {0, 255, 255, 255}},     {"magenta", {255, 0, 255, 255}},
      {"gray", {190, 190, 190, 255}},   {"grey", {190, 190, 190, 255}},   {"orange", {255, 165, 0, 255}},
      {"purple", {160, 32, 240, 255}},  {"brown", {165, 42, 42, 255}},    {"pink", {255, 192, 203, 255}},
      {"navy", {0, 0, 128, 255}},       {"maroon", {176, 48, 96, 255}},   {"darkgray", {169, 169, 169, 255}},
      {"darkgrey", {169, 169, 169, 255}}, {"lightgray", {211, 211, 211, 255}}, {"lightgrey", {211, 211, 211, 255}},
      {"darkred", {139, 0, 0, 255}},    {"darkgreen", {0, 100, 0, 255}},  {"darkblue", {0, 0, 139, 255}},
      {"gold", {255, 215, 0, 255}},     {"silver", {192, 192, 192, 255}}, {"transparent", {0, 0, 0, 0}},
  };
  const std::string lower = util::to_lower(name);
  for (const Named& n : table) {
    if (lower == n.name) return n.rgba;
  }
  // grayN / greyN (0..100).
  for (const std::string_view prefix : {"gray", "grey"}) {
    if (lower.starts_with(prefix) && lower.size() > prefix.size()) {
      const auto v = util::parse_int(std::string_view(lower).substr(prefix.size()));
      if (v && *v >= 0 && *v <= 100) {
        const auto g = static_cast<std::uint8_t>((*v * 255 + 50) / 100);
        return Rgba{g, g, g, 255};
      }
    }
  }
  return std::nullopt;
}

std::optional<Rgba> parse_xpm_colour(std::string_view value) {
  if (value.starts_with('#')) {
    const std::string_view hex = value.substr(1);
    const auto parse = [&](std::size_t at, std::size_t n) -> std::optional<int> {
      const auto v = util::parse_int(hex.substr(at, n), 16);
      if (!v) return std::nullopt;
      // Scale n hex digits to 8 bits.
      const std::int64_t max = (std::int64_t{1} << (4 * n)) - 1;
      return static_cast<int>((*v * 255 + max / 2) / max);
    };
    std::size_t digits = 0;
    if (hex.size() == 3) digits = 1;
    if (hex.size() == 6) digits = 2;
    if (hex.size() == 9) digits = 3;
    if (hex.size() == 12) digits = 4;
    if (digits == 0) return std::nullopt;
    const auto r = parse(0, digits);
    const auto g = parse(digits, digits);
    const auto b = parse(2 * digits, digits);
    if (!r || !g || !b) return std::nullopt;
    return Rgba{static_cast<std::uint8_t>(*r), static_cast<std::uint8_t>(*g), static_cast<std::uint8_t>(*b), 255};
  }
  return named_colour(value);
}

Result<ir::Image> read_xpm(ReadContext& ctx) {
  const std::string_view text = as_chars(ctx.input);
  if (text.find("XPM") == std::string_view::npos) return fail(Status::Decode, "not an XPM file");
  const std::vector<std::string_view> header_only = string_literals(text, 1);
  if (header_only.empty()) return fail(Status::Decode, "XPM values are missing");
  const std::vector<std::string_view> head = util::split(header_only[0], ' ', false);
  std::vector<std::uint32_t> numbers;
  for (const std::string_view part : head) {
    const auto v = util::parse_int(util::trim(part));
    if (!v || *v < 0 || *v > 0x7FFFFFFF) return fail(Status::Decode, "XPM values are malformed");
    numbers.push_back(static_cast<std::uint32_t>(*v));
  }
  if (numbers.size() < 4) return fail(Status::Decode, "XPM values are malformed");
  const std::uint32_t width = numbers[0];
  const std::uint32_t height = numbers[1];
  const std::uint32_t colours = numbers[2];
  const std::uint32_t cpp = numbers[3];
  if (cpp == 0 || cpp > 8 || colours == 0 || colours > (1u << 20)) return fail(Status::Decode, "XPM colour table is invalid");
  CVT_TRY(check_size(ctx, width, height));
  CVT_TRY(check_plausible(std::uint64_t{width} * height, text.size(), 1));
  const std::vector<std::string_view> strings = string_literals(text, std::size_t{1} + colours + height);
  if (strings.size() < std::size_t{1} + colours) return fail(Status::Decode, "XPM colour table is truncated");
  std::map<std::string_view, Rgba> table;
  for (std::uint32_t i = 0; i < colours; ++i) {
    const std::string_view line = strings[1 + i];
    if (line.size() < cpp) return fail(Status::Decode, "XPM colour entry is malformed");
    const std::string_view key = line.substr(0, cpp);
    const std::vector<std::string_view> words = util::split(line.substr(cpp), ' ', false);
    std::optional<Rgba> colour;
    int best_rank = 99;
    for (std::size_t w = 0; w + 1 < words.size(); ++w) {
      const std::string_view k = words[w];
      const int rank = k == "c" ? 0 : k == "g" ? 1 : k == "g4" ? 2 : k == "m" ? 3 : 99;
      if (rank == 99) continue;
      // Colour names may contain spaces ("light gray"): join until the next key.
      std::string value(words[w + 1]);
      std::size_t next = w + 2;
      while (next < words.size() && words[next] != "c" && words[next] != "g" && words[next] != "g4" &&
             words[next] != "m" && words[next] != "s") {
        value += std::string(words[next++]);
      }
      if (rank < best_rank) {
        if (const auto parsed = parse_xpm_colour(value)) {
          colour = parsed;
          best_rank = rank;
        }
      }
    }
    table[key] = colour.value_or(Rgba{0, 0, 0, 255});
  }
  ir::Frame frame = ir::Frame::filled(width, height, {0, 0, 0, 0});
  bool damaged = false;
  for (std::uint32_t y = 0; y < height; ++y) {
    if (1 + colours + y >= strings.size()) {
      damaged = true;
      break;
    }
    const std::string_view row = strings[1 + colours + y];
    for (std::uint32_t x = 0; x < width; ++x) {
      if ((std::uint64_t{x} + 1) * cpp > row.size()) {
        damaged = true;
        break;
      }
      const auto it = table.find(row.substr(std::size_t{x} * cpp, cpp));
      if (it == table.end()) {
        damaged = true;
        continue;
      }
      std::memcpy(frame.rgba.data() + frame.offset(x, y), it->second.data(), 4);
    }
  }
  if (damaged) ctx.warn("The XPM data is incomplete or uses undefined colours.");
  ir::Image image;
  image.frames.push_back(std::move(frame));
  return image;
}

Result<Bytes> write_xpm(const ir::Image& source, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const WriteImage prepared, prepare_write(source, ctx));
  warn_if_animated(*prepared, ctx, "XPM");
  const ir::Frame& frame = prepared->frames.front();
  auto palette = exact_palette(frame, 256);
  if (!palette) {
    QuantizeOptions qo;
    qo.dither = resolved_dither(ctx.options);
    qo.keep_alpha = false;
    CVT_TRY_ASSIGN(Quantized q, quantize(frame, qo));
    palette = std::move(q);
    ctx.warn("The picture has more than 256 colours and was reduced to a palette.");
  }
  static constexpr std::string_view symbols =
      " .XoO+@#$%&*=-;:>,<1234567890qwertyuipasdfghjklzxcvbnmMNBVCZASDFGHJKLPIUYTREWQ!~^/()_`'][{}|";
  const std::size_t n = palette->palette.size();
  const std::size_t cpp = n <= symbols.size() ? 1 : 2;
  const auto code = [&](std::size_t i) {
    std::string c;
    if (cpp == 1) {
      c += symbols[i];
    } else {
      c += symbols[i % symbols.size()];
      c += symbols[i / symbols.size()];
    }
    return c;
  };
  std::string out = "/* XPM */\nstatic char *image[] = {\n";
  out += std::format("\"{} {} {} {}\",\n", frame.width, frame.height, n, cpp);
  for (std::size_t i = 0; i < n; ++i) {
    const Rgba c = palette->palette[i];
    if (c[3] < 128) {
      out += std::format("\"{} c None\",\n", code(i));
    } else {
      out += std::format("\"{} c #{:02X}{:02X}{:02X}\",\n", code(i), c[0], c[1], c[2]);
    }
  }
  for (std::uint32_t y = 0; y < frame.height; ++y) {
    out += '"';
    for (std::uint32_t x = 0; x < frame.width; ++x) out += code(palette->indices[static_cast<std::size_t>(y) * frame.width + x]);
    out += y + 1 < frame.height ? "\",\n" : "\"\n";
  }
  out += "};\n";
  return to_bytes(out);
}

int sniff_xpm(std::span<const std::byte> data, std::string_view /*filename*/) {
  const std::string_view text = as_chars(data.first(std::min<std::size_t>(data.size(), 256)));
  return text.starts_with("/* XPM */") ? 95 : 0;
}

}  // namespace

void register_xpm(Registry& registry) {
  FormatInfo info;
  info.id = "xpm";
  info.name = "X PixMap";
  info.description = "Colour X11 image stored as C source.";
  info.category = Category::Image;
  info.extensions = {"xpm"};
  info.mime = "image/x-xpixmap";
  info.popularity = 5;
  info.sniff = sniff_xpm;
  registry.add_format(std::move(info));

  ImageCodecSpec spec;
  spec.format_id = "xpm";
  spec.read = reader(read_xpm);
  spec.write = writer(write_xpm);
  spec.write_options = plain_writer_options();
  spec.write_options.push_back(dither_option());
  add_image_codec(registry, std::move(spec));
}

}  // namespace cvt::image
