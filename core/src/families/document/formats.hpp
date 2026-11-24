// Convertible core - document family: registration entry points of the
// format groups (one per source file).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <string_view>

#include "util/bytes.hpp"

namespace cvt {
class Registry;
}  // namespace cvt

namespace cvt::document {

void register_html(Registry& registry);           // html, xhtml
void register_web_archives(Registry& registry);   // mhtml, webarchive
void register_markdown(Registry& registry);       // md (+ gfm, commonmark variants)
void register_bridges(Registry& registry);        // Document <-> PlainText, Table, Data
void register_rst(Registry& registry);
void register_asciidoc(Registry& registry);
void register_org(Registry& registry);
void register_wikis(Registry& registry);          // mediawiki, dokuwiki, creole, jira
void register_textile(Registry& registry);
void register_bbcode(Registry& registry);
void register_txt2tags(Registry& registry);
void register_pod(Registry& registry);
void register_djot(Registry& registry);
void register_typst(Registry& registry);
void register_latex(Registry& registry);
void register_man(Registry& registry);
void register_texinfo(Registry& registry);
void register_xml_formats(Registry& registry);    // docbook, fb2, jats, tei, opml, enex
void register_ipynb(Registry& registry);

/// Helpers for sniffers: the first `limit` bytes as text, and whether the
/// file name ends in one of the extensions (case-insensitive).
[[nodiscard]] std::string_view sniff_head(ByteView data, std::size_t limit = 4096);
[[nodiscard]] bool has_extension(std::string_view filename, std::initializer_list<std::string_view> extensions);

}  // namespace cvt::document
