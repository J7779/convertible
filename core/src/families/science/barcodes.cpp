// Convertible core - science family: barcode generation from text.
//
// Formats qr, datamatrix, code128, ean13, upca. Two ways in:
//   * text -> <symbology>: a PlainText writer that emits the symbol as PNG
//     (default), SVG or PBM bytes, chosen with `barcode_output`;
//   * <symbology> -> any picture format: an Image reader that treats the
//     input file as the text to encode (convert with the source format set
//     to "qr" etc.), so every image writer of the registry can store it.
// The formats are never detected from content (they have no content of their
// own); they are targets, or explicit sources.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <format>

#include "families/science/barcode.hpp"
#include "families/science/common.hpp"
#include "families/science/science.hpp"
#include "ir/plain_text.hpp"
#include "util/text.hpp"

namespace cvt::science {

namespace {

using barcode::Matrix;

Result<std::array<std::uint8_t, 3>> colour_option(const Options& options, std::string_view key, std::string_view fallback) {
  std::string text = options.get_string(key, fallback);
  if (text.starts_with('#')) text.erase(0, 1);
  const auto v = util::parse_int(text, 16);
  if (text.size() != 6 || !v) return fail(Status::InvalidArgument, "option '{}' must be six hexadecimal digits (RRGGBB)", key);
  return std::array<std::uint8_t, 3>{static_cast<std::uint8_t>(*v >> 16), static_cast<std::uint8_t>(*v >> 8), static_cast<std::uint8_t>(*v)};
}

/// Build the module matrix for a symbology (linear symbols get one row).
Result<Matrix> symbol(std::string_view id, std::string_view text, const Options& options, std::string* note) {
  if (id == "qr") {
    barcode::QrOptions o;
    const std::string ecc = options.get_string("qr_ecc", "M");
    o.ecc = ecc == "L" ? barcode::QrEcc::L : ecc == "Q" ? barcode::QrEcc::Q : ecc == "H" ? barcode::QrEcc::H : barcode::QrEcc::M;
    const int version = static_cast<int>(options.get_int("qr_version", 0));
    if (version > 0) o.min_version = version;
    o.mask = static_cast<int>(options.get_int("qr_mask", -1));
    o.boost_ecc = options.get_bool("qr_boost_ecc", true);
    int v = 0, m = 0;
    CVT_TRY_ASSIGN(Matrix matrix, barcode::qr_encode(text, o, &v, &m));
    if (version > 0 && v != version) return fail(Status::Encode, "the text does not fit QR version {}", version);
    if (note != nullptr) *note = std::format("QR version {}, mask {}", v, m);
    return matrix;
  }
  if (id == "datamatrix") return barcode::datamatrix_encode(text, options.get_string("dm_shape", "square") == "rectangle");
  std::vector<bool> bars;
  if (id == "code128") {
    CVT_TRY_ASSIGN(bars, barcode::code128_encode(text));
  } else if (id == "ean13") {
    CVT_TRY_ASSIGN(bars, barcode::ean13_encode(text, note));
  } else {
    CVT_TRY_ASSIGN(bars, barcode::upca_encode(text, note));
  }
  Matrix m;
  m.width = static_cast<int>(bars.size());
  m.height = 1;
  for (const bool b : bars) m.modules.push_back(b ? 1 : 0);
  return m;
}

bool linear(std::string_view id) { return id == "code128" || id == "ean13" || id == "upca"; }

struct Layout {
  int module = 8;
  int quiet = 4;
  int bar_height = 1;  // modules of height for linear symbols
};

Layout layout(std::string_view id, const Options& options) {
  Layout l;
  l.module = static_cast<int>(options.get_int("module_size", linear(id) ? 3 : 8));
  const int default_quiet = id == "qr" ? 4 : id == "datamatrix" ? 2 : 10;
  l.quiet = static_cast<int>(options.get_int("quiet_zone", -1));
  if (l.quiet < 0) l.quiet = default_quiet;
  l.bar_height = linear(id) ? static_cast<int>(options.get_int("bar_height", 50)) : 1;
  return l;
}

Result<ir::Frame> render(const Matrix& m, const Layout& l, std::array<std::uint8_t, 3> fg, std::array<std::uint8_t, 3> bg,
                         const Context& ctx) {
  const std::uint64_t rows = static_cast<std::uint64_t>(m.height) * static_cast<std::uint64_t>(l.bar_height);
  const std::uint64_t w = (static_cast<std::uint64_t>(m.width) + 2ull * static_cast<std::uint64_t>(l.quiet)) * static_cast<std::uint64_t>(l.module);
  const std::uint64_t h = (rows + 2ull * static_cast<std::uint64_t>(l.quiet)) * static_cast<std::uint64_t>(l.module);
  CVT_TRY(ctx.limits.check_pixels(w, h));
  ir::Frame f = ir::Frame::filled(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), {bg[0], bg[1], bg[2], 255});
  for (std::uint64_t y = 0; y < h; ++y) {
    const std::int64_t my = static_cast<std::int64_t>(y / static_cast<std::uint64_t>(l.module)) - l.quiet;
    if (my < 0 || my >= static_cast<std::int64_t>(rows)) continue;
    for (std::uint64_t x = 0; x < w; ++x) {
      const std::int64_t mx = static_cast<std::int64_t>(x / static_cast<std::uint64_t>(l.module)) - l.quiet;
      if (mx < 0 || mx >= m.width) continue;
      if (m.get(static_cast<int>(mx), static_cast<int>(my / l.bar_height))) {
        const std::size_t at = f.offset(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
        f.rgba[at] = fg[0];
        f.rgba[at + 1] = fg[1];
        f.rgba[at + 2] = fg[2];
      }
    }
  }
  return f;
}

std::string svg(const Matrix& m, const Layout& l, std::array<std::uint8_t, 3> fg, std::array<std::uint8_t, 3> bg) {
  const int w = m.width + 2 * l.quiet;
  const int h = m.height * l.bar_height + 2 * l.quiet;
  std::string path;
  for (int y = 0; y < m.height; ++y) {
    for (int x = 0; x < m.width;) {
      if (!m.get(x, y)) {
        ++x;
        continue;
      }
      int run = 1;
      while (x + run < m.width && m.get(x + run, y)) ++run;
      path += std::format("M{} {}h{}v{}h-{}z", x + l.quiet, y * l.bar_height + l.quiet, run, l.bar_height, run);
      x += run;
    }
  }
  return std::format(
      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"{}\" height=\"{}\" viewBox=\"0 0 {} {}\" shape-rendering=\"crispEdges\">"
      "<rect width=\"{}\" height=\"{}\" fill=\"#{:02x}{:02x}{:02x}\"/><path fill=\"#{:02x}{:02x}{:02x}\" d=\"{}\"/></svg>\n",
      w * l.module, h * l.module, w, h, w, h, bg[0], bg[1], bg[2], fg[0], fg[1], fg[2], path);
}

Bytes pbm(const ir::Frame& f) {
  util::ByteWriter out(f.width * f.height / 8 + 32);
  out.text(std::format("P4\n{} {}\n", f.width, f.height));
  for (std::uint32_t y = 0; y < f.height; ++y) {
    std::uint8_t byte = 0;
    int bits = 0;
    for (std::uint32_t x = 0; x < f.width; ++x) {
      byte = static_cast<std::uint8_t>(byte << 1 | (f.rgba[f.offset(x, y)] < 128 ? 1 : 0));
      if (++bits == 8) {
        out.u8(byte);
        byte = 0;
        bits = 0;
      }
    }
    if (bits) out.u8(static_cast<std::uint8_t>(byte << (8 - bits)));
  }
  return out.take();
}

std::string payload(std::string_view text) {
  // A trailing line break from a text file is not part of the message.
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.remove_suffix(1);
  return std::string(text);
}

std::vector<OptionSpec> barcode_options(std::string_view id) {
  std::vector<OptionSpec> o = {
      OptionSpec::integer("module_size", "Module size", linear(id) ? 3 : 8, 1, 100, "Pixels per module (narrowest bar or square)."),
      OptionSpec::integer("quiet_zone", "Quiet zone", -1, -1, 100, "Blank modules around the symbol; -1 uses the standard margin."),
      OptionSpec::text("foreground", "Foreground", "000000", "Colour of the dark modules (RRGGBB)."),
      OptionSpec::text("background", "Background", "ffffff", "Colour of the light modules (RRGGBB)."),
  };
  if (id == "qr") {
    o.push_back(OptionSpec::choice("qr_ecc", "Error correction", "M", {"L", "M", "Q", "H"},
                                   "Recovery capacity: L 7 %, M 15 %, Q 25 %, H 30 % of the symbol."));
    o.push_back(OptionSpec::integer("qr_version", "Version", 0, 0, 40, "Symbol size 1..40 (21 to 177 modules); 0 = smallest that fits.").advanced());
    o.push_back(OptionSpec::integer("qr_mask", "Mask", -1, -1, 7, "Data mask pattern; -1 picks the best one.").advanced());
    o.push_back(OptionSpec::boolean("qr_boost_ecc", "Boost error correction", true,
                                    "Use a stronger level when it fits in the same symbol size.").advanced());
  } else if (id == "datamatrix") {
    o.push_back(OptionSpec::choice("dm_shape", "Shape", "square", {"square", "rectangle"}, "Square or rectangular symbol."));
  } else {
    o.push_back(OptionSpec::integer("bar_height", "Bar height", 50, 1, 1000, "Height of the bars in modules."));
  }
  return o;
}

void register_symbology(Registry& registry, std::string id, std::string name, std::string description, int popularity) {
  add_format(registry, {.id = id,
                        .name = std::move(name),
                        .description = std::move(description),
                        .category = Category::Image,
                        .extensions = {"png"},
                        .mime = "image/png",
                        .popularity = popularity,
                        .output_only = true});
  std::vector<OptionSpec> write_options = barcode_options(id);
  write_options.push_back(OptionSpec::choice("barcode_output", "Output", "png", {"png", "svg", "pbm"},
                                             "Picture format written for the symbol."));
  add_codec(
      registry, id, Kind::PlainText, nullptr,
      [id](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
        const std::string text = payload(value.get<ir::PlainText>().text);
        std::string note;
        CVT_TRY_ASSIGN(const Matrix m, symbol(id, text, ctx.options, &note));
        if (!note.empty()) ctx.warn(note);
        const Layout l = layout(id, ctx.options);
        CVT_TRY_ASSIGN(const auto fg, colour_option(ctx.options, "foreground", "000000"));
        CVT_TRY_ASSIGN(const auto bg, colour_option(ctx.options, "background", "ffffff"));
        const std::string output = ctx.options.get_string("barcode_output", "png");
        if (output == "svg") return to_bytes(svg(m, l, fg, bg));
        CVT_TRY_ASSIGN(const ir::Frame f, render(m, l, fg, bg, ctx));
        if (output == "pbm") return pbm(f);
        const bool grey = fg[0] == fg[1] && fg[1] == fg[2] && bg[0] == bg[1] && bg[1] == bg[2];
        return encode_png(f, grey);
      },
      {}, write_options, 10, 10);
  add_codec(
      registry, id, Kind::Image,
      [id](ReadContext& ctx) -> Result<AnyIR> {
        const std::string text = payload(input_text(ctx.input));
        CVT_TRY_ASSIGN(const Matrix m, symbol(id, text, ctx.options, nullptr));
        CVT_TRY_ASSIGN(const auto fg, colour_option(ctx.options, "foreground", "000000"));
        CVT_TRY_ASSIGN(const auto bg, colour_option(ctx.options, "background", "ffffff"));
        CVT_TRY_ASSIGN(ir::Frame f, render(m, layout(id, ctx.options), fg, bg, ctx));
        ir::Image image;
        image.frames.push_back(std::move(f));
        return image;
      },
      nullptr, barcode_options(id), {}, 10, 10);
}

}  // namespace

void register_barcodes(Registry& registry) {
  register_symbology(registry, "qr", "QR Code", "QR Code symbol generated from text.", 40);
  register_symbology(registry, "datamatrix", "Data Matrix", "Data Matrix ECC 200 symbol generated from text.", 15);
  register_symbology(registry, "code128", "Code 128 barcode", "Code 128 linear barcode generated from text.", 20);
  register_symbology(registry, "ean13", "EAN-13 barcode", "EAN-13 retail barcode from 12 or 13 digits.", 18);
  register_symbology(registry, "upca", "UPC-A barcode", "UPC-A retail barcode from 11 or 12 digits.", 12);
}

}  // namespace cvt::science
