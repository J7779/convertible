// Convertible core - office family: presentations as documents and back.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/office/slides.hpp"

#include <algorithm>
#include <string>

#include "engine/standard_options.hpp"
#include "families/office/office_common.hpp"
#include "util/text.hpp"

namespace cvt::office {

ir::Blocks make_slide(std::string_view title, ir::Blocks body, ir::Blocks notes, std::string_view name) {
  ir::Blocks out;
  std::string heading(util::trim(title));
  (void)name;
  if (!heading.empty()) out.push_back(ir::Heading{1, {ir::Text{heading}}});
  for (ir::Block& b : body) out.push_back(std::move(b));
  if (!notes.empty()) {
    ir::Attributes attributes;
    attributes.add_class(speaker_notes_class);
    out.push_back(ir::Block(ir::BlockQuote{std::move(notes)}, std::move(attributes)));
  }
  return out;
}

namespace {

std::size_t weight(const ir::Block& block) {
  // A rough measure of how much room a block takes on a slide.
  if (block.is<ir::ImageBlock>()) return 400;
  if (const auto* table = block.get_if<ir::TableBlock>()) return 80 * table->rows.size();
  std::string text = ir::plain_text(ir::Blocks{block});
  std::size_t lines = static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
  return text.size() + 40 * lines;
}

void finish(std::vector<Slide>& slides, Slide& current) {
  if (current.title.empty() && current.body.empty() && current.notes.empty()) return;
  // Very long sections continue on further slides.
  constexpr std::size_t budget = 900;
  std::size_t used = 0;
  Slide part;
  part.title = current.title;
  int continuation = 0;
  for (ir::Block& block : current.body) {
    const std::size_t w = weight(block);
    if (used > 0 && used + w > budget) {
      slides.push_back(std::move(part));
      part = Slide{};
      part.title = current.title;
      if (!part.title.empty()) part.title.push_back(ir::Text{" (" + std::to_string(++continuation + 1) + ")"});
      used = 0;
    }
    used += w;
    part.body.push_back(std::move(block));
  }
  part.notes = std::move(current.notes);
  slides.push_back(std::move(part));
  current = Slide{};
}

}  // namespace

std::vector<Slide> split_slides(const ir::Document& document, const Options& options) {
  (void)options;
  std::vector<Slide> slides;
  const bool by_breaks = std::ranges::any_of(document.blocks, [](const ir::Block& b) { return b.is<ir::PageBreak>(); });
  // Split at the highest heading level that occurs more than once (a lone
  // top-level heading is the title slide).
  int counts[7] = {0, 0, 0, 0, 0, 0, 0};
  for (const ir::Block& b : document.blocks) {
    if (const auto* h = b.get_if<ir::Heading>()) ++counts[std::clamp(h->level, 1, 6)];
  }
  int top = 6;
  for (int level = 1; level <= 6; ++level) {
    if (counts[level] > 1) {
      top = level;
      break;
    }
    if (counts[level] == 1) top = level;
  }
  Slide current;
  bool title_slide_done = false;
  for (const ir::Block& b : document.blocks) {
    if (b.is<ir::PageBreak>()) {
      finish(slides, current);
      continue;
    }
    if (b.is<ir::BlockQuote>() && b.attr.has_class(speaker_notes_class)) {
      for (const ir::Block& n : b.get_if<ir::BlockQuote>()->blocks) current.notes.push_back(n);
      continue;
    }
    if (const auto* h = b.get_if<ir::Heading>()) {
      if (by_breaks && current.title.empty() && current.body.empty()) {
        current.title = h->content;
        continue;
      }
      if (h->level <= top) {
        finish(slides, current);
        current.title = h->content;
        continue;
      } else if (current.title.empty() && current.body.empty()) {
        current.title = h->content;
        continue;
      }
      // A sub-heading inside a slide: bold paragraph.
      current.body.push_back(ir::Paragraph{{ir::Strong{h->content}}});
      continue;
    }
    if (!by_breaks && slides.empty() && current.title.empty() && !title_slide_done) {
      title_slide_done = true;
      if (!document.metadata.title.empty()) current.title = {ir::Text{document.metadata.title}};
    }
    current.body.push_back(b);
  }
  finish(slides, current);
  if (slides.empty()) {
    Slide only;
    if (!document.metadata.title.empty()) only.title = {ir::Text{document.metadata.title}};
    slides.push_back(std::move(only));
  }
  return slides;
}

std::vector<OptionSpec> slide_write_options() {
  std::vector<OptionSpec> out = {
      OptionSpec::boolean("include_notes", "Include notes", true, "Write speaker notes into the notes pages."),
      OptionSpec::text("language", "Language", "", "Language of the text as a BCP 47 tag (empty: from the source).")
          .advanced(),
  };
  for (OptionSpec& spec : image_write_options()) out.push_back(std::move(spec));
  out.push_back(options::compression_level());
  out.push_back(options::strip_metadata());
  return out;
}

}  // namespace cvt::office
