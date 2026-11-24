// Convertible core - pdf family: vector drawings as true vector PDF.
//
// svg, eps/ps, emf/wmf, dxf ... are read by the vector family into a scene,
// carried as `Data` in the tagged "vector-scene" envelope (Codec::ir_tag).
// This writer takes that envelope and writes every page as PDF content
// operators through vector::pdf::page_content (families/vector/vector_api.hpp):
// paths stay paths, text becomes glyph outlines, pictures stay image
// XObjects - nothing is rasterised except filter effects.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cstddef>
#include <format>
#include <string>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/pdf/pdf.hpp"
#include "families/vector/vector_api.hpp"
#include "ir/data.hpp"
#include "util/deflate.hpp"
#include "util/text.hpp"

namespace cvt::pdf {

namespace {

// Writes numbered objects and the cross-reference table.
class PdfAssembler {
 public:
  PdfAssembler() { append("%PDF-1.7\n%\xE2\xE3\xCF\xD3\n"); }

  [[nodiscard]] std::size_t reserve() {
    offsets_.push_back(0);
    return offsets_.size();  // object numbers start at 1
  }

  void object(std::size_t number, std::string_view dict, const Bytes* stream = nullptr) {
    offsets_[number - 1] = out_.size();
    append(std::format("{} 0 obj\n", number));
    append(dict);
    if (stream != nullptr) {
      append("\nstream\n");
      out_.insert(out_.end(), stream->begin(), stream->end());
      append("\nendstream");
    }
    append("\nendobj\n");
  }

  [[nodiscard]] Bytes finish(std::size_t root) {
    const std::size_t xref = out_.size();
    append(std::format("xref\n0 {}\n0000000000 65535 f \n", offsets_.size() + 1));
    for (const std::size_t offset : offsets_) append(std::format("{:010} 00000 n \n", offset));
    append(std::format("trailer\n<< /Size {} /Root {} 0 R >>\nstartxref\n{}\n%%EOF\n", offsets_.size() + 1, root, xref));
    return std::move(out_);
  }

 private:
  void append(std::string_view text) {
    const auto* p = reinterpret_cast<const std::byte*>(text.data());
    out_.insert(out_.end(), p, p + text.size());
  }

  Bytes out_;
  std::vector<std::size_t> offsets_;
};

// "<< ... >>" with /Length (and the stream's own /Filter already inside).
std::string with_length(std::string dict, std::size_t length) {
  const std::size_t close = dict.rfind(">>");
  if (close == std::string::npos) return dict;
  dict.insert(close, std::format(" /Length {} ", length));
  return dict;
}

Result<Bytes> write_vector_pdf(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(vector::Scene scene, vector::scene_from_json(value.get<ir::Data>().value, ctx.limits.max_pixels));
  if (scene.pages.empty()) return fail(Status::Encode, "the drawing has no pages");
  const int level = options::resolved_compression_level(ctx.options, 9);
  vector::pdf::PdfContentOptions content_options;
  content_options.compression_level = level;

  PdfAssembler pdf;
  const std::size_t catalog = pdf.reserve();
  const std::size_t pages = pdf.reserve();
  std::vector<std::size_t> kids;
  for (std::size_t p = 0; p < scene.pages.size(); ++p) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(p) / static_cast<double>(scene.pages.size()), "Writing"));
    CVT_TRY_ASSIGN(const vector::pdf::PdfPageContent page, vector::pdf::page_content(scene, p, ctx, content_options));
    std::vector<std::size_t> numbers;
    for (std::size_t i = 0; i < page.objects.size(); ++i) numbers.push_back(pdf.reserve());
    const auto ref = [&](std::size_t i) { return std::format("{} 0 R", numbers.at(i)); };
    const std::string resources = vector::pdf::resolve(page.resources_dict(), ref, "");
    for (std::size_t i = 0; i < page.objects.size(); ++i) {
      const vector::pdf::PdfObject& object = page.objects[i];
      std::string dict = vector::pdf::resolve(object.dict, ref, resources);
      if (object.stream) {
        pdf.object(numbers[i], with_length(std::move(dict), object.stream->size()), &*object.stream);
      } else {
        pdf.object(numbers[i], dict);
      }
    }
    CVT_TRY_ASSIGN(const Bytes content, util::zlib_compress(page.content, level));
    const std::size_t content_number = pdf.reserve();
    pdf.object(content_number, std::format("<< /Filter /FlateDecode /Length {} >>", content.size()), &content);
    const std::size_t page_number = pdf.reserve();
    pdf.object(page_number, std::format("<< /Type /Page /Parent {} 0 R /MediaBox [0 0 {} {}] /Resources {} /Contents {} 0 R >>",
                                        pages, util::format_double(page.width_pt), util::format_double(page.height_pt),
                                        resources, content_number));
    kids.push_back(page_number);
  }
  std::string kid_list;
  for (const std::size_t kid : kids) kid_list += std::format("{} 0 R ", kid);
  pdf.object(pages, std::format("<< /Type /Pages /Kids [{}] /Count {} >>", kid_list, kids.size()));
  pdf.object(catalog, std::format("<< /Type /Catalog /Pages {} 0 R >>", pages));
  return pdf.finish(catalog);
}

}  // namespace

void register_vector_pdf(Registry& registry) {
  Codec codec;
  codec.format_id = "pdf";
  codec.kind = Kind::Data;
  codec.ir_tag = "vector-scene";
  codec.write = write_vector_pdf;
  codec.write_cost = 10;
  codec.write_options = {options::compression_level(9)};
  registry.add_codec(std::move(codec));
}

}  // namespace cvt::pdf
