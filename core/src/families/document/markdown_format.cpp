// Convertible core - document family: registration of Markdown.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/document/doc_common.hpp"
#include "families/document/formats.hpp"
#include "util/text.hpp"

namespace cvt::document {

namespace {

MarkdownFlavor flavor_from(std::string_view name, MarkdownFlavor fallback) {
  if (name == "commonmark") return MarkdownFlavor::CommonMark;
  if (name == "gfm") return MarkdownFlavor::Gfm;
  if (name == "extended") return MarkdownFlavor::Extended;
  return fallback;
}

int sniff_markdown(ByteView data, std::string_view filename) {
  if (has_extension(filename, {"md", "markdown", "mdown", "mkd", "mkdn", "mdwn", "mdtxt", "mdtext", "rmd", "qmd"})) {
    return 80;
  }
  const std::string_view head = sniff_head(data, 8192);
  if (head.empty() || head.front() == '<' || head.front() == '{' || head.front() == '\\') return 0;
  int score = 0;
  std::size_t lines = 0;
  for (const std::string_view line : util::split_lines(head)) {
    if (++lines > 200) break;
    if (line.starts_with("# ") || line.starts_with("## ") || line.starts_with("### ")) score += 3;
    if (line.starts_with("```")) score += 3;
    if (line.starts_with("- [ ] ") || line.starts_with("- [x] ")) score += 3;
    if (line.starts_with("|") && line.find("---") != std::string_view::npos) score += 3;
    if (line.find("](") != std::string_view::npos && line.find('[') != std::string_view::npos) score += 2;
    if (line.starts_with("> ")) score += 1;
    if (line.starts_with("- ") || line.starts_with("* ")) score += 1;
    if (line.find("**") != std::string_view::npos) score += 1;
  }
  return score >= 9 ? 65 : 0;
}

Result<ir::Document> read_md(std::string_view text, ReadContext& ctx) {
  MarkdownReadOptions options;
  options.flavor = flavor_from(ctx.options.get_string("input_flavor", "extended"), MarkdownFlavor::Extended);
  std::vector<std::string> warnings;
  auto document = parse_markdown(text, options, &warnings);
  for (std::string& w : warnings) ctx.warn(std::move(w));
  return document;
}

Result<std::string> write_md(const ir::Document& document, WriteContext& ctx) {
  MarkdownWriteOptions options;
  options.flavor = flavor_from(ctx.options.get_string("flavor", "gfm"), MarkdownFlavor::Gfm);
  options.wrap_width = static_cast<int>(ctx.options.get_int("wrap_width", 0, 0, 1000));
  options.reference_links = ctx.options.get_bool("reference_links", false);
  options.setext_headings = ctx.options.get_bool("setext_headings", false);
  options.front_matter = ctx.options.get_bool("front_matter", true);
  options.images = image_handling(ctx.options);
  const std::string bullet = ctx.options.get_string("bullet", "-");
  options.bullet = bullet.empty() ? '-' : bullet.front();
  if (options.flavor == MarkdownFlavor::CommonMark && !document.footnotes.empty()) {
    ctx.warn("CommonMark has no footnotes; they were written as end notes.");
    ir::Document copy = document;
    footnotes_to_endnotes(copy);
    return write_markdown(copy, options);
  }
  return write_markdown(document, options);
}

}  // namespace

void register_markdown(Registry& registry) {
  MarkupFormat md;
  md.id = "md";
  md.name = "Markdown";
  md.description = "Lightweight markup (CommonMark, GitHub Flavored Markdown and common extensions).";
  md.extensions = {"md", "markdown", "mdown", "mkd", "mkdn", "mdwn", "mdtxt", "mdtext"};
  md.mime = "text/markdown";
  md.popularity = 85;
  md.sniff = sniff_markdown;
  md.read = read_md;
  md.write = write_md;
  md.read_options = {OptionSpec::choice("input_flavor", "Input flavour", "extended", {"commonmark", "gfm", "extended"},
                                        "Markdown dialect of the input; 'extended' adds footnotes, definition lists, "
                                        "math and front matter to GitHub Flavored Markdown.")
                         .advanced()};
  md.write_options = {
      OptionSpec::choice("flavor", "Flavour", "gfm", {"commonmark", "gfm", "extended"},
                         "Markdown dialect to write: plain CommonMark, GitHub Flavored Markdown, or GFM with "
                         "definition lists, math and front matter."),
      OptionSpec::boolean("reference_links", "Reference links", false,
                          "Write links as [text][1] with the addresses collected at the end."),
      OptionSpec::boolean("setext_headings", "Underlined headings", false,
                          "Underline level 1 and 2 headings (=== / ---) instead of using # marks.")
          .advanced(),
      OptionSpec::boolean("front_matter", "Front matter", true, "Write the document metadata as YAML front matter.")
          .advanced(),
      OptionSpec::choice("bullet", "Bullet", "-", {"-", "*", "+"}, "Marker of bulleted list items.").advanced(),
  };
  md.features = feature_wrap;
  md.default_wrap = 0;
  register_markup(registry, std::move(md));
}

}  // namespace cvt::document
