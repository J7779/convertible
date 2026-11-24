// Convertible core - pdf family.
// PDF reading (text extraction, rendering), writing (documents, images) and
// rewriting/compression, plus XPS/OXPS and FDF/XFDF reading.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <charconv>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/families.hpp"
#include "families/pdf/pdf.hpp"
#include "families/pdf/syntax.hpp"
#include "ir/blob.hpp"
#include "util/text.hpp"

namespace cvt {

namespace pdf {

Result<std::vector<int>> parse_page_ranges(std::string_view text, int page_count) {
  std::vector<int> pages;
  const std::string_view trimmed = util::trim(text);
  if (trimmed.empty() || util::iequals(trimmed, "all")) {
    for (int i = 0; i < page_count; ++i) pages.push_back(i);
    if (pages.empty()) return fail(Status::Decode, "the PDF has no pages");
    return pages;
  }
  for (const std::string_view part_raw : util::split(trimmed, ',', false)) {
    const std::string_view part = util::trim(part_raw);
    if (part.empty()) continue;
    const std::size_t dash = part.find('-');
    const auto number = [](std::string_view s) -> std::optional<int> {
      s = util::trim(s);
      const auto v = util::parse_int(s);
      if (!v || *v < 1 || *v > 1'000'000) return std::nullopt;
      return static_cast<int>(*v);
    };
    int lo = 0, hi = 0;
    if (dash == std::string_view::npos) {
      const auto v = number(part);
      if (!v) return fail(Status::InvalidArgument, "option 'pages': '{}' is not a page number", part);
      lo = hi = *v;
    } else {
      const std::string_view a = util::trim(part.substr(0, dash)), b = util::trim(part.substr(dash + 1));
      const auto va = a.empty() ? std::optional<int>(1) : number(a);
      const auto vb = b.empty() ? std::optional<int>(std::max(page_count, 1)) : number(b);
      if (!va || !vb) return fail(Status::InvalidArgument, "option 'pages': '{}' is not a page range", part);
      lo = *va;
      hi = *vb;
      if (hi < lo) std::swap(lo, hi);
    }
    for (int p = lo; p <= hi && p <= page_count; ++p) {
      if (pages.size() >= 100'000) break;
      pages.push_back(p - 1);
    }
  }
  if (pages.empty()) return fail(Status::InvalidArgument, "option 'pages' selects no page of this {}-page PDF", page_count);
  return pages;
}

Result<std::shared_ptr<PdfFile>> open_for_read(const ReadContext& ctx) {
  OpenOptions options;
  options.password = ctx.options.get_string("password", "");
  options.limits = ctx.limits;
  options.max_pages = std::min<std::uint64_t>(options.max_pages, std::max<std::uint64_t>(ctx.limits.max_archive_entries, 1));
  CVT_TRY_ASSIGN(auto file, PdfFile::open(ctx.input, options));
  if (file->pages().empty()) return fail(Status::Decode, "the PDF has no pages");
  return file;
}

namespace {

OptionSpec password_option() {
  return OptionSpec::text("password", "Password", "",
                          "Password of an encrypted PDF (tried as user and as owner password).")
      .advanced();
}
OptionSpec pages_option() {
  return OptionSpec::text("pages", "Pages", "",
                          "Pages to convert, e.g. \"1-3,5\" (1-based); empty for all pages.");
}

std::array<std::uint8_t, 4> parse_background(const std::string& text, bool& ok) {
  ok = true;
  if (text.empty() || util::iequals(text, "white")) return {255, 255, 255, 255};
  if (util::iequals(text, "transparent") || util::iequals(text, "none")) return {255, 255, 255, 0};
  if (util::iequals(text, "black")) return {0, 0, 0, 255};
  std::string_view hex = text;
  if (!hex.empty() && hex.front() == '#') hex.remove_prefix(1);
  std::uint32_t v = 0;
  const auto [end, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), v, 16);
  if ((hex.size() != 6 && hex.size() != 8) || ec != std::errc{} || end != hex.data() + hex.size()) {
    ok = false;
    return {255, 255, 255, 255};
  }
  if (hex.size() == 6) v = (v << 8) | 0xFF;
  return {static_cast<std::uint8_t>(v >> 24), static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 8),
          static_cast<std::uint8_t>(v)};
}

Result<AnyIR> read_document(ReadContext& ctx) {
  CVT_TRY_ASSIGN(auto file, open_for_read(ctx));
  ExtractOptions options;
  CVT_TRY_ASSIGN(options.pages, parse_page_ranges(ctx.options.get_string("pages", ""), static_cast<int>(file->pages().size())));
  options.images = ctx.options.get_bool("include_images", true);
  options.remove_headers_footers = ctx.options.get_bool("remove_headers_footers", true);
  CVT_TRY_ASSIGN(ir::Document doc, extract_document(*file, options, ctx));
  return doc;
}

Result<AnyIR> read_image(ReadContext& ctx) {
  CVT_TRY_ASSIGN(auto file, open_for_read(ctx));
  RenderOptions options;
  CVT_TRY_ASSIGN(options.pages, parse_page_ranges(ctx.options.get_string("pages", ""), static_cast<int>(file->pages().size())));
  options.dpi = ctx.options.get_float("dpi", 150, 10, 1200);
  bool ok = true;
  options.background = parse_background(ctx.options.get_string("background", "white"), ok);
  if (!ok) return fail(Status::InvalidArgument, "option 'background' must be white, transparent or a hex colour RRGGBB");
  options.annotations = ctx.options.get_bool("annotations", true);
  CVT_TRY_ASSIGN(ir::Image image, render_pages(*file, options, ctx));
  return image;
}

Result<AnyIR> read_blob(ReadContext& ctx) {
  ir::Blob blob;
  blob.bytes.assign(ctx.input.begin(), ctx.input.end());
  blob.filename = ctx.filename;
  blob.format_id = "pdf";
  blob.mime = "application/pdf";
  return blob;
}

Result<Bytes> write_blob(const AnyIR& value, WriteContext& ctx) {
  const ir::Blob& blob = value.get<ir::Blob>();
  const std::string_view head = as_chars(ByteView(blob.bytes)).substr(0, 1024);
  if (head.find("%PDF-") == std::string_view::npos) {
    return fail(Status::UnsupportedConversion,
                "only PDF files can be rewritten as PDF this way; convert documents or images to PDF instead");
  }
  OpenOptions options;
  options.password = ctx.options.get_string("password", "");
  options.limits = ctx.limits;
  CVT_TRY_ASSIGN(auto file, PdfFile::open(blob.bytes, options));
  return rewrite_pdf(*file, ctx);
}

Result<Bytes> write_document(const AnyIR& value, WriteContext& ctx) {
  return write_document_pdf(value.get<ir::Document>(), ctx);
}

Result<Bytes> write_image(const AnyIR& value, WriteContext& ctx) { return write_image_pdf(value.get<ir::Image>(), ctx); }

std::vector<OptionSpec> page_layout_options() {
  return {
      OptionSpec::choice("page_size", "Page size", "a4", {"a4", "letter", "legal", "a5", "a3"},
                         "Paper size of the pages."),
      OptionSpec::choice("orientation", "Orientation", "portrait", {"portrait", "landscape"}, "Page orientation."),
      OptionSpec::number("margin_mm", "Margins (mm)", 20, 0, 100, "White space around the content, in millimetres."),
  };
}

}  // namespace

void register_pdf(Registry& registry) {
  FormatInfo info;
  info.id = "pdf";
  info.name = "PDF document";
  info.description = "Portable Document Format: fixed-layout pages with text, graphics and images.";
  info.category = Category::Document;
  info.extensions = {"pdf"};
  info.mime = "application/pdf";
  info.popularity = 98;
  info.signatures = {Signature::at(0, "'%PDF-'")};
  info.sniff = [](std::span<const std::byte> data, std::string_view) -> int {
    const std::string_view head = as_chars(data.first(std::min<std::size_t>(data.size(), 1024)));
    const std::size_t at = head.find("%PDF-");
    if (at == std::string_view::npos) return 0;
    return at == 0 ? 95 : 85;
  };
  registry.add_format(std::move(info));

  // Text extraction with layout analysis (PDF -> TXT/Markdown/HTML/DOCX ...).
  Codec doc_reader;
  doc_reader.format_id = "pdf";
  doc_reader.kind = Kind::Document;
  doc_reader.read = read_document;
  doc_reader.read_cost = 20;
  doc_reader.lossy = true;
  doc_reader.read_options = {
      password_option(),
      pages_option(),
      OptionSpec::boolean("include_images", "Include images", true, "Keep the pictures of the PDF in the document."),
      OptionSpec::boolean("remove_headers_footers", "Remove headers and footers", true,
                          "Drop page numbers and running headers/footers repeated on most pages.")
          .advanced(),
  };
  registry.add_codec(std::move(doc_reader));

  // Rendering (PDF -> PNG/JPEG/TIFF ...).
  Codec image_reader;
  image_reader.format_id = "pdf";
  image_reader.kind = Kind::Image;
  image_reader.read = read_image;
  image_reader.read_cost = 15;
  image_reader.lossy = true;
  image_reader.read_options = {
      password_option(),
      pages_option(),
      OptionSpec::number("dpi", "Resolution (dpi)", 150, 10, 1200, "Rendering resolution in dots per inch."),
      OptionSpec::text("background", "Background", "white",
                       "Page background: white, transparent, or a hex colour such as ffffff."),
      OptionSpec::boolean("annotations", "Draw annotations", true, "Render form fields, stamps and other annotations.")
          .advanced(),
  };
  registry.add_codec(std::move(image_reader));

  // Document -> PDF (layout engine).
  Codec doc_writer;
  doc_writer.format_id = "pdf";
  doc_writer.kind = Kind::Document;
  doc_writer.write = write_document;
  doc_writer.write_cost = 20;
  doc_writer.write_options = page_layout_options();
  doc_writer.write_options.push_back(OptionSpec::number("font_size", "Font size", 10.5, 6, 24, "Body text size in points."));
  doc_writer.write_options.push_back(
      OptionSpec::boolean("justify", "Justify text", true, "Align paragraphs to both margins."));
  doc_writer.write_options.push_back(
      OptionSpec::boolean("page_numbers", "Page numbers", true, "Print page numbers at the bottom of each page."));
  doc_writer.write_options.push_back(options::quality());
  doc_writer.write_options.push_back(options::preset());
  doc_writer.write_options.push_back(options::strip_metadata());
  registry.add_codec(std::move(doc_writer));

  // Image -> PDF (one picture per page).
  Codec image_writer;
  image_writer.format_id = "pdf";
  image_writer.kind = Kind::Image;
  image_writer.write = write_image;
  image_writer.write_cost = 10;
  image_writer.write_options = {
      OptionSpec::choice("page_size", "Page size", "fit", {"fit", "a4", "letter", "legal", "a5", "a3"},
                         "\"fit\" makes each page exactly the size of its picture."),
      OptionSpec::choice("orientation", "Orientation", "auto", {"auto", "portrait", "landscape"},
                         "Page orientation; auto follows the picture."),
      OptionSpec::number("margin_mm", "Margins (mm)", 0, 0, 100, "White space around the picture (fixed page sizes)."),
      options::quality(),
      options::preset(),
      options::lossless(),
      options::strip_metadata(),
  };
  registry.add_codec(std::move(image_writer));

  // PDF -> PDF rewrite / compression (same-format route).
  Codec blob_reader;
  blob_reader.format_id = "pdf";
  blob_reader.kind = Kind::Blob;
  blob_reader.read = read_blob;
  blob_reader.read_cost = 12;
  registry.add_codec(std::move(blob_reader));

  Codec blob_writer;
  blob_writer.format_id = "pdf";
  blob_writer.kind = Kind::Blob;
  blob_writer.write = write_blob;
  blob_writer.write_cost = 8;
  blob_writer.write_options = {
      password_option(),
      pages_option(),
      options::quality(),
      options::preset(),
      options::compression_level(),
      options::strip_metadata(),
      OptionSpec::integer("max_image_dpi", "Maximum image resolution (dpi)", 0, 0, 2400,
                          "Downsample pictures above this resolution; 0 chooses from the preset (high keeps them)."),
      OptionSpec::boolean("recompress_images", "Recompress images", true,
                          "Re-encode pictures when that makes them smaller (lossy for photos below high quality).")
          .advanced(),
  };
  registry.add_codec(std::move(blob_writer));
}

}  // namespace pdf

void register_pdf_family(Registry& registry) {
  pdf::register_pdf(registry);
  pdf::register_xps(registry);
  pdf::register_fdf(registry);
}

}  // namespace cvt
