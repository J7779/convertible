// Convertible core - vector family: PostScript-based formats (eps, ps, ai).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

#include "engine/registry.hpp"
#include "families/vector/ps_io.hpp"
#include "families/vector/vector.hpp"
#include "families/vector/vector_codecs.hpp"
#include "util/text.hpp"

namespace cvt::vector {

namespace {

std::string_view head_text(std::span<const std::byte> data, std::size_t n) {
  return as_chars(data.subspan(0, std::min(n, data.size())));
}

bool is_dos_eps(std::span<const std::byte> data) {
  return data.size() >= 30 && data[0] == std::byte{0xC5} && data[1] == std::byte{0xD0} &&
         data[2] == std::byte{0xD3} && data[3] == std::byte{0xC6};
}

/// The text of the PostScript part (DOS EPS aware), first `n` bytes.
std::string_view ps_head(std::span<const std::byte> data, std::size_t n) {
  const ByteView section = postscript_section(data);
  return head_text(section, n);
}

bool looks_illustrator(std::string_view head) {
  return head.find("%AI5_") != std::string_view::npos || head.find("%AI3_") != std::string_view::npos ||
         head.find("%AI7_") != std::string_view::npos || head.find("%AI8_") != std::string_view::npos ||
         head.find("%%Creator: Adobe Illustrator") != std::string_view::npos;
}

int sniff_eps(std::span<const std::byte> data, std::string_view filename) {
  if (is_dos_eps(data)) return 95;
  std::string_view h = ps_head(data, 4096);
  if (h.starts_with("\x04")) h.remove_prefix(1);
  if (h.starts_with("%!PS-Adobe-") && h.substr(0, 64).find("EPSF") != std::string_view::npos) {
    return util::iends_with(filename, ".ps") ? 80 : 95;
  }
  if (h.starts_with("%!") && (util::iends_with(filename, ".eps") || util::iends_with(filename, ".epsf") ||
                              util::iends_with(filename, ".epsi"))) {
    return 80;
  }
  return 0;
}

int sniff_ps(std::span<const std::byte> data, std::string_view filename) {
  std::string_view h = head_text(data, 4096);
  if (h.starts_with("\x04")) h.remove_prefix(1);
  if (h.starts_with("%!PS-Adobe-") && h.substr(0, 64).find("EPSF") != std::string_view::npos) {
    return util::iends_with(filename, ".ps") ? 92 : 60;
  }
  if (h.starts_with("%!PS")) return 90;
  if (h.starts_with("%!")) return util::iends_with(filename, ".ps") ? 85 : 60;
  if (h.starts_with("\x1B%-12345X") && h.find("%!PS") != std::string_view::npos) return 70;
  return 0;
}

int sniff_ai(std::span<const std::byte> data, std::string_view filename) {
  const bool ai_name = util::iends_with(filename, ".ai");
  const std::string_view h = ps_head(data, 8192);
  if (h.starts_with("%PDF")) {
    // PDF-based Illustrator files belong to the PDF reader; claim them only
    // weakly so that a clear message results when nothing else reads PDF.
    return ai_name ? 20 : 0;
  }
  if (!h.starts_with("%!")) return 0;
  if (looks_illustrator(h)) {
    const bool epsf = h.substr(0, 64).find("EPSF") != std::string_view::npos;
    if (ai_name) return 97;
    return epsf ? 80 : 96;
  }
  return ai_name ? 70 : 0;
}

std::vector<OptionSpec> reader_options() {
  return {OptionSpec::integer("max_operations", "Operation limit", 30'000'000, 1000, 2'000'000'000,
                              "Largest number of PostScript operators run before the file is rejected.")
              .advanced()};
}

std::vector<OptionSpec> writer_options() {
  return {
      OptionSpec::choice("level", "Language level", "2", {"2", "3"},
                         "PostScript language level: 2 runs everywhere (gradients use LanguageLevel 3 smooth "
                         "shading where available), 3 also compresses pictures with Flate."),
      OptionSpec::choice("ps_text", "Text", "outlines", {"outlines", "text"},
                         "outlines draws text as shapes (looks identical everywhere); text writes real text with "
                         "the standard PostScript fonts so it stays searchable."),
      OptionSpec::integer("precision", "Precision", 3, 0, 6, "Decimals written for coordinates (in points).")
          .advanced(),
      OptionSpec::integer("raster_dpi", "Raster resolution", 150, 36, 1200,
                          "Resolution of parts that must be rasterised (transparency, masks, filters).")
          .advanced(),
  };
}

PsWriteOptions write_options_from(const Options& o, bool eps) {
  PsWriteOptions w;
  w.eps = eps;
  w.level = o.get_string("level", "2") == "3" ? 3 : 2;
  w.text_as_text = o.get_string("ps_text", "outlines") == "text";
  w.precision = static_cast<int>(o.get_int("precision", 3, 0, 6));
  w.raster_dpi = static_cast<double>(o.get_int("raster_dpi", 150, 36, 1200));
  return w;
}

}  // namespace

void register_postscript(Registry& registry) {
  // ---- EPS
  {
    VectorFormat f;
    f.info.id = "eps";
    f.info.name = "Encapsulated PostScript";
    f.info.description = "Single-page PostScript drawing with a bounding box, for placing in other documents.";
    f.info.category = Category::Vector;
    f.info.extensions = {"eps", "epsf", "epsi"};
    f.info.mime = "application/postscript";
    f.info.popularity = 45;
    f.info.signatures = {Signature::at(0, "C5 D0 D3 C6")};
    f.info.sniff = sniff_eps;
    f.read = [](ReadContext& ctx) { return read_postscript(ctx.input, ctx, PsKind::EPS); };
    f.write = [](const Scene& scene, WriteContext& ctx) {
      return write_postscript(scene, ctx, write_options_from(ctx.options, true));
    };
    f.read_options = reader_options();
    f.write_options = writer_options();
    f.lossy_read = true;
    f.lossy_write = true;
    f.read_cost = 25;
    f.write_cost = 15;
    register_vector_format(registry, std::move(f));
  }
  // ---- PS
  {
    VectorFormat f;
    f.info.id = "ps";
    f.info.name = "PostScript";
    f.info.description = "PostScript page description document (one or more pages).";
    f.info.category = Category::Document;
    f.info.extensions = {"ps"};
    f.info.mime = "application/postscript";
    f.info.popularity = 35;
    f.info.sniff = sniff_ps;
    f.read = [](ReadContext& ctx) { return read_postscript(ctx.input, ctx, PsKind::PS); };
    f.write = [](const Scene& scene, WriteContext& ctx) {
      return write_postscript(scene, ctx, write_options_from(ctx.options, false));
    };
    f.read_options = reader_options();
    f.write_options = writer_options();
    f.lossy_read = true;
    f.lossy_write = true;
    f.read_cost = 25;
    f.write_cost = 15;
    register_vector_format(registry, std::move(f));
  }
  // ---- AI (legacy, PostScript-based Illustrator <= 8)
  {
    VectorFormat f;
    f.info.id = "ai";
    f.info.name = "Adobe Illustrator (legacy)";
    f.info.description =
        "Adobe Illustrator drawing in the PostScript-based format of Illustrator 8 and earlier.";
    f.info.category = Category::Vector;
    f.info.extensions = {"ai"};
    f.info.mime = "application/illustrator";
    f.info.popularity = 30;
    f.info.sniff = sniff_ai;
    f.read = [](ReadContext& ctx) { return read_postscript(ctx.input, ctx, PsKind::AI); };
    f.read_options = reader_options();
    f.lossy_read = true;
    f.image_writer = false;
    f.read_cost = 25;
    register_vector_format(registry, std::move(f));
  }
}

}  // namespace cvt::vector
