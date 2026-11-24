// Convertible core - office family: internal declarations.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "ir/any.hpp"
#include "util/bytes.hpp"

namespace cvt {
class Registry;
}

namespace cvt::office {

using ReadFn = std::function<Result<AnyIR>(ReadContext&)>;
using WriteFn = std::function<Result<Bytes>(const AnyIR&, WriteContext&)>;

// Word processing.
[[nodiscard]] ReadFn docx_reader();
[[nodiscard]] ReadFn flat_word_reader();  // flat OPC and Word 2003 XML
[[nodiscard]] WriteFn docx_writer();
[[nodiscard]] WriteFn word2003_writer();
void register_word(Registry& registry);
void register_doc(Registry& registry);
void register_rtf(Registry& registry);
void register_odf(Registry& registry);
void register_misc_text(Registry& registry);  // abw, zabw, hwpx, hwp, wpd, pages

// Presentations.
void register_pptx(Registry& registry);
void register_ppt(Registry& registry);

// E-books.
void register_epub(Registry& registry);
void register_mobi(Registry& registry);
void register_chm(Registry& registry);
void register_comics(Registry& registry);
void register_ebook_wrappers(Registry& registry);  // fb2.zip / fbz, Palm text, TCR

// Containers.
void register_xps(Registry& registry);
void register_mail(Registry& registry);  // msg, tnef
void register_vsdx(Registry& registry);

/// Parse RTF text (used for the bodies of Outlook messages).
[[nodiscard]] Result<ir::Document> rtf_to_document(std::string_view rtf, const ReadContext& ctx);
/// Decompress an Outlook compressed-RTF ("LZFu") body.
[[nodiscard]] Result<std::string> decompress_compressed_rtf(ByteView data, std::uint64_t max_out);

/// True when the bytes are a compound file whose directory names `stream`
/// (a cheap test for sniffers).
[[nodiscard]] bool compound_file_has(ByteView data, std::u16string_view stream);

// Extract embedded pictures as an Archive (second reader kind of a format).
void register_image_extraction(Registry& registry);

}  // namespace cvt::office
