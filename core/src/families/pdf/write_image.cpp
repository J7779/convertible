// Convertible core - pdf family: images to PDF (one picture per page).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <format>
#include <unordered_set>

#include "engine/standard_options.hpp"
#include "families/pdf/builder.hpp"
#include "families/pdf/filters.hpp"
#include "families/pdf/pdf.hpp"
#include "families/pdf/pdf_image.hpp"
#include "families/pdf/syntax.hpp"
#include "families/pdf/write_common.hpp"

namespace cvt::pdf {

Rect paper_size(std::string_view name, bool landscape) {
  Rect r{0, 0, 595.276, 841.89};  // A4
  if (name == "letter") r = {0, 0, 612, 792};
  else if (name == "legal") r = {0, 0, 612, 1008};
  else if (name == "a5") r = {0, 0, 419.528, 595.276};
  else if (name == "a3") r = {0, 0, 841.89, 1190.551};
  if (landscape) std::swap(r.x1, r.y1);
  return r;
}

namespace {

bool is_gray(const ir::Frame& f) {
  for (std::size_t i = 0; i < f.rgba.size(); i += 4) {
    if (f.rgba[i] != f.rgba[i + 1] || f.rgba[i] != f.rgba[i + 2]) return false;
  }
  return true;
}

bool few_colors(const ir::Frame& f, std::size_t limit) {
  std::unordered_set<std::uint32_t> colors;
  for (std::size_t i = 0; i < f.rgba.size(); i += 4) {
    colors.insert((static_cast<std::uint32_t>(f.rgba[i]) << 16) | (static_cast<std::uint32_t>(f.rgba[i + 1]) << 8) | f.rgba[i + 2]);
    if (colors.size() > limit) return false;
  }
  return true;
}

}  // namespace

Result<Ref> embed_image(PdfBuilder& builder, const ir::Frame& frame, const ImageEncoding& enc, const Context& ctx) {
  const bool has_alpha = frame.has_transparency();
  const bool gray = is_gray(frame);
  const std::size_t pixels = static_cast<std::size_t>(frame.width) * frame.height;
  Bytes samples(pixels * (gray ? 1 : 3));
  for (std::size_t i = 0, o = 0; i < pixels; ++i) {
    if (gray) {
      samples[o++] = static_cast<std::byte>(frame.rgba[i * 4]);
    } else {
      samples[o++] = static_cast<std::byte>(frame.rgba[i * 4]);
      samples[o++] = static_cast<std::byte>(frame.rgba[i * 4 + 1]);
      samples[o++] = static_cast<std::byte>(frame.rgba[i * 4 + 2]);
    }
  }
  Dict dict;
  dict.set("Type", Object::name("XObject"));
  dict.set("Subtype", Object::name("Image"));
  dict.set("Width", static_cast<std::int64_t>(frame.width));
  dict.set("Height", static_cast<std::int64_t>(frame.height));
  dict.set("ColorSpace", Object::name(gray ? "DeviceGray" : "DeviceRGB"));
  dict.set("BitsPerComponent", 8);
  if (has_alpha) {
    Bytes alpha(pixels);
    for (std::size_t i = 0; i < pixels; ++i) alpha[i] = static_cast<std::byte>(frame.rgba[i * 4 + 3]);
    Dict md;
    md.set("Type", Object::name("XObject"));
    md.set("Subtype", Object::name("Image"));
    md.set("Width", static_cast<std::int64_t>(frame.width));
    md.set("Height", static_cast<std::int64_t>(frame.height));
    md.set("ColorSpace", Object::name("DeviceGray"));
    md.set("BitsPerComponent", 8);
    Dict parms;
    parms.set("Predictor", 15);
    parms.set("Colors", 1);
    parms.set("BitsPerComponent", 8);
    parms.set("Columns", static_cast<std::int64_t>(frame.width));
    md.set("DecodeParms", Object(std::move(parms)));
    md.set("Filter", Object::name("FlateDecode"));
    const Bytes predicted = png_predict(alpha, 1, 8, static_cast<int>(frame.width));
    dict.set("SMask", builder.add_stream_raw(std::move(md), flate_encode(predicted, enc.compression_level)));
  }
  // Lossy JPEG for photographs unless asked to stay lossless.
  bool use_jpeg = !enc.lossless && !few_colors(frame, 256) && pixels >= 64;
  if (use_jpeg) {
    ir::Image opaque;
    ir::Frame copy = frame;
    for (std::size_t i = 3; i < copy.rgba.size(); i += 4) copy.rgba[i] = 255;
    opaque.frames.push_back(std::move(copy));
    auto jpeg = encode_with_registry(&ctx, "jpg", opaque, std::format(R"({{"quality":{},"strip_metadata":true}})", enc.quality));
    if (jpeg) {
      dict.set("Filter", Object::name("DCTDecode"));
      if (gray) dict.set("ColorSpace", Object::name("DeviceGray"));
      // The JPEG encoder may write colour even for gray input; the colour
      // space must match its component count.
      const ByteView jb = *jpeg;
      int components = gray ? 1 : 3;
      for (std::size_t i = 2; i + 9 < jb.size();) {
        if (std::to_integer<int>(jb[i]) != 0xFF) break;
        const int marker = std::to_integer<int>(jb[i + 1]);
        const std::size_t len = (std::to_integer<std::size_t>(jb[i + 2]) << 8) | std::to_integer<std::size_t>(jb[i + 3]);
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
          components = std::to_integer<int>(jb[i + 9]);
          break;
        }
        i += 2 + len;
      }
      dict.set("ColorSpace", Object::name(components == 1 ? "DeviceGray" : "DeviceRGB"));
      return builder.add_stream_raw(std::move(dict), std::move(*jpeg));
    }
  }
  Dict parms;
  parms.set("Predictor", 15);
  parms.set("Colors", gray ? 1 : 3);
  parms.set("BitsPerComponent", 8);
  parms.set("Columns", static_cast<std::int64_t>(frame.width));
  dict.set("DecodeParms", Object(std::move(parms)));
  dict.set("Filter", Object::name("FlateDecode"));
  const Bytes predicted = png_predict(samples, gray ? 1 : 3, 8, static_cast<int>(frame.width));
  return builder.add_stream_raw(std::move(dict), flate_encode(predicted, enc.compression_level));
}

Result<Bytes> write_image_pdf(const ir::Image& image, WriteContext& ctx) {
  if (!image.valid()) return fail(Status::Encode, "the image has no pixels");
  PdfBuilder builder(options::resolved_compression_level(ctx.options, 9) == 0 ? 0 : 9);
  ImageEncoding enc;
  enc.quality = options::resolved_quality(ctx.options);
  enc.lossless = ctx.options.get_bool("lossless", false);
  enc.compression_level = 9;
  const std::string page_size = ctx.options.get_enum("page_size", "fit", {"fit", "a4", "letter", "legal", "a5", "a3"});
  const std::string orientation = ctx.options.get_enum("orientation", "auto", {"auto", "portrait", "landscape"});
  const double margin = ctx.options.get_float("margin_mm", 0, 0, 100) * 72.0 / 25.4;
  const double dpi_x = image.dpi_x > 0 ? image.dpi_x : 96.0;
  const double dpi_y = image.dpi_y > 0 ? image.dpi_y : dpi_x;
  const Ref pages_ref = builder.reserve();
  Array kids;
  for (std::size_t i = 0; i < image.frames.size(); ++i) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(image.frames.size()), "Writing pages"));
    const ir::Frame& frame = image.frames[i];
    const double natural_w = frame.width * 72.0 / dpi_x, natural_h = frame.height * 72.0 / dpi_y;
    Rect page{0, 0, natural_w + 2 * margin, natural_h + 2 * margin};
    double w = natural_w, h = natural_h;
    if (page_size != "fit") {
      const bool landscape = orientation == "landscape" || (orientation == "auto" && natural_w > natural_h);
      page = paper_size(page_size, landscape);
      const double avail_w = std::max(1.0, page.width() - 2 * margin), avail_h = std::max(1.0, page.height() - 2 * margin);
      const double s = std::min(avail_w / natural_w, avail_h / natural_h);
      w = natural_w * s;
      h = natural_h * s;
    }
    CVT_TRY_ASSIGN(const Ref xobj, embed_image(builder, frame, enc, ctx));
    const double x = (page.width() - w) / 2, y = (page.height() - h) / 2;
    const std::string content = "q " + format_number(w) + " 0 0 " + format_number(h) + " " + format_number(x) + " " +
                                format_number(y) + " cm /Im0 Do Q\n";
    const Ref content_ref = builder.add_stream(Dict{}, as_bytes(content));
    Dict xobjects;
    xobjects.set("Im0", xobj);
    Dict resources;
    resources.set("XObject", Object(std::move(xobjects)));
    Dict pd;
    pd.set("Type", Object::name("Page"));
    pd.set("Parent", pages_ref);
    pd.set("MediaBox", Array{Object(0), Object(0), Object(page.width()), Object(page.height())});
    pd.set("Resources", Object(std::move(resources)));
    pd.set("Contents", content_ref);
    kids.push_back(builder.add(Object(std::move(pd))));
  }
  Dict pages;
  pages.set("Type", Object::name("Pages"));
  pages.set("Count", static_cast<std::int64_t>(kids.size()));
  pages.set("Kids", Object(std::move(kids)));
  builder.set(pages_ref, Object(std::move(pages)));
  Dict catalog;
  catalog.set("Type", Object::name("Catalog"));
  catalog.set("Pages", pages_ref);
  const Ref root = builder.add(Object(std::move(catalog)));
  Dict info;
  info.set("Producer", Object::string("Convertible"));
  if (!options::resolved_strip_metadata(ctx.options)) {
    if (const auto it = image.metadata.find("Title"); it != image.metadata.end()) {
      info.set("Title", Object::string(encode_text_string(it->second)));
    }
  }
  const Ref info_ref = builder.add(Object(std::move(info)));
  return builder.finish(root, info_ref, "1.7");
}

}  // namespace cvt::pdf
