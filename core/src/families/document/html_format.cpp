// Convertible core - document family: registration of HTML and XHTML.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <initializer_list>

#include "families/document/doc_common.hpp"
#include "families/document/formats.hpp"
#include "util/text.hpp"

namespace cvt::document {

std::string_view sniff_head(ByteView data, std::size_t limit) {
  std::string_view text = as_chars(data.first(std::min(data.size(), limit)));
  if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
  return text;
}

bool has_extension(std::string_view filename, std::initializer_list<std::string_view> extensions) {
  for (const std::string_view extension : extensions) {
    if (filename.size() > extension.size() && filename[filename.size() - extension.size() - 1] == '.' &&
        util::iends_with(filename, extension)) {
      return true;
    }
  }
  return false;
}

namespace {

int sniff_html(ByteView data, std::string_view filename) {
  const std::string_view head = util::trim_left(sniff_head(data, 2048));
  const std::string lowered = util::to_lower(head);
  if (lowered.starts_with("<!doctype html")) return 92;
  if (lowered.starts_with("<html") || lowered.find("<html") != std::string::npos) {
    return lowered.starts_with("<") ? 90 : 0;
  }
  if (lowered.starts_with("<") && (lowered.find("<head") != std::string::npos || lowered.find("<body") != std::string::npos)) {
    return 85;
  }
  // A fragment with several common tags.
  if (has_extension(filename, {"html", "htm", "shtml", "xhtml"})) return 80;
  int tags = 0;
  for (const std::string_view tag : {"<p>", "<div", "<br", "<table", "<span", "<a href", "<h1", "<h2", "<ul>", "<li>"}) {
    if (lowered.find(tag) != std::string::npos) ++tags;
  }
  return lowered.starts_with("<") && tags >= 3 ? 62 : 0;
}

int sniff_xhtml(ByteView data, std::string_view filename) {
  const std::string_view head = sniff_head(data, 2048);
  if (head.find("http://www.w3.org/1999/xhtml") != std::string_view::npos &&
      (head.starts_with("<?xml") || has_extension(filename, {"xhtml", "xht"}))) {
    return 93;
  }
  return 0;
}

HtmlReadOptions read_options_from(const Context& ctx) {
  HtmlReadOptions options;
  const std::string requested = ctx.options.get_string("source_encoding", "auto");
  if (requested != "auto") options.forced_encoding = requested;
  return options;
}

Result<ir::Document> read_html(ByteView data, ReadContext& ctx) {
  std::vector<std::string> warnings;
  HtmlReadOptions options = read_options_from(ctx);
  options.xhtml = ctx.format_id == "xhtml";
  auto document = parse_html(data, options, &warnings);
  for (std::string& w : warnings) ctx.warn(std::move(w));
  return document;
}

Result<std::string> write_html_codec(const ir::Document& document, WriteContext& ctx) {
  HtmlWriteOptions options;
  options.standalone = ctx.options.get_bool("standalone", true);
  options.stylesheet = ctx.options.get_bool("stylesheet", true);
  options.minify = ctx.options.get_bool("minify", false);
  options.xhtml = ctx.format_id == "xhtml";
  options.images = image_handling(ctx.options);
  return write_html(document, options);
}

std::vector<OptionSpec> html_write_options() {
  return {
      OptionSpec::boolean("stylesheet", "Stylesheet", true, "Embed a small stylesheet for comfortable reading."),
      OptionSpec::boolean("minify", "Minify", false, "Leave out optional line breaks to make the file smaller."),
  };
}

std::vector<OptionSpec> html_read_options() {
  return {OptionSpec::choice("source_encoding", "Source encoding", "auto",
                             {"auto", "utf-8", "utf-16le", "utf-16be", "windows-1252", "iso-8859-1", "iso-8859-2",
                              "iso-8859-15", "windows-1250", "windows-1251", "koi8-r", "macintosh"},
                             "Character encoding of the input; 'auto' honours the byte order mark and <meta charset>.")
              .advanced()};
}

}  // namespace

void register_html(Registry& registry) {
  MarkupFormat html;
  html.id = "html";
  html.name = "HTML page";
  html.description = "Web page; read with a tolerant HTML5 parser, written as clean standalone HTML5.";
  html.extensions = {"html", "htm", "shtml"};
  html.mime = "text/html";
  html.popularity = 90;
  html.sniff = sniff_html;
  html.read_bytes = read_html;
  html.write = write_html_codec;
  html.read_options = html_read_options();
  html.write_options = html_write_options();
  html.features = feature_standalone;
  register_markup(registry, std::move(html));

  MarkupFormat xhtml;
  xhtml.id = "xhtml";
  xhtml.name = "XHTML page";
  xhtml.description = "HTML in XML syntax (XHTML5 polyglot markup).";
  xhtml.extensions = {"xhtml", "xht"};
  xhtml.mime = "application/xhtml+xml";
  xhtml.popularity = 35;
  xhtml.sniff = sniff_xhtml;
  xhtml.read_bytes = read_html;
  xhtml.write = write_html_codec;
  xhtml.read_options = html_read_options();
  xhtml.write_options = html_write_options();
  xhtml.features = feature_standalone;
  register_markup(registry, std::move(xhtml));
}

}  // namespace cvt::document
