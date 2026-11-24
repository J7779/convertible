// Convertible core - office family: EPUB entry points shared with the
// other e-book formats (comics, MOBI, FB2 wrappers write through EPUB's
// building blocks).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "engine/options.hpp"
#include "families/office/office.hpp"
#include "ir/document.hpp"

namespace cvt::office {

[[nodiscard]] ReadFn epub_reader();
[[nodiscard]] Result<Bytes> write_epub_document(const ir::Document& doc, WriteContext& ctx);
[[nodiscard]] std::vector<OptionSpec> epub_read_options();

}  // namespace cvt::office
