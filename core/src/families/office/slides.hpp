// Convertible core - office family: presentations as documents and back.
//
// A presentation read into an ir::Document becomes one section per slide:
// the slide title as a level 1 heading, the body blocks, the speaker notes
// as a BlockQuote with the class "speaker-notes", and a PageBreak between
// slides. Writers split a document back into slides: at page breaks when
// the document has them, otherwise at its top-level headings, and long
// sections continue on further slides.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <string_view>
#include <vector>

#include "engine/options.hpp"
#include "ir/document.hpp"

namespace cvt::office {

inline constexpr std::string_view speaker_notes_class = "speaker-notes";

struct Slide {
  ir::Inlines title;
  ir::Blocks body;
  ir::Blocks notes;
};

/// The blocks of one slide in a document (heading, body, notes).
[[nodiscard]] ir::Blocks make_slide(std::string_view title, ir::Blocks body, ir::Blocks notes,
                                    std::string_view name = {});
/// Split a document into slides (see the header comment).
[[nodiscard]] std::vector<Slide> split_slides(const ir::Document& document, const Options& options);
/// Writer options of the presentation writers.
[[nodiscard]] std::vector<OptionSpec> slide_write_options();

}  // namespace cvt::office
