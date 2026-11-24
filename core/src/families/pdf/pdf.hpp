// Convertible core - pdf family: internal entry points.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "families/pdf/document.hpp"
#include "ir/document.hpp"
#include "ir/image.hpp"

namespace cvt {
class Registry;
}

namespace cvt::pdf {

/// Parse "1-3,5,8-" (1-based, inclusive; empty or "all" = every page) into
/// 0-based page indices. Out-of-range pages are dropped; an empty result or
/// malformed text is Status::InvalidArgument.
[[nodiscard]] Result<std::vector<int>> parse_page_ranges(std::string_view text, int page_count);

/// Open a PDF for reading with the conversion's password and limits.
[[nodiscard]] Result<std::shared_ptr<PdfFile>> open_for_read(const ReadContext& ctx);

struct ExtractOptions {
  std::vector<int> pages;
  bool images = true;
  bool remove_headers_footers = true;
};

/// Text extraction with layout analysis.
[[nodiscard]] Result<ir::Document> extract_document(PdfFile& file, const ExtractOptions& options, const Context& ctx);

struct RenderOptions {
  std::vector<int> pages;
  double dpi = 150;
  std::array<std::uint8_t, 4> background{255, 255, 255, 255};
  bool annotations = true;
  bool antialias = true;
};

/// Render pages to RGBA frames (one per page).
[[nodiscard]] Result<ir::Image> render_pages(PdfFile& file, const RenderOptions& options, const Context& ctx);
[[nodiscard]] Result<ir::Frame> render_page(PdfFile& file, int page_index, const RenderOptions& options,
                                            const Context& ctx);

/// Writers.
[[nodiscard]] Result<Bytes> write_document_pdf(const ir::Document& document, WriteContext& ctx);
[[nodiscard]] Result<Bytes> write_image_pdf(const ir::Image& image, WriteContext& ctx);
/// PDF -> PDF rewrite (compression, page extraction, decryption).
[[nodiscard]] Result<Bytes> rewrite_pdf(PdfFile& file, WriteContext& ctx);

void register_pdf(Registry& registry);
void register_xps(Registry& registry);
void register_fdf(Registry& registry);
void register_vector_pdf(Registry& registry);  // vector_pdf.cpp: vector scenes as true vector PDF

}  // namespace cvt::pdf
