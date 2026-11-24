// Convertible core - document family.
// Markup and text documents: HTML/XHTML, web archives, Markdown, the
// lightweight markup languages (reStructuredText, AsciiDoc, Org, wikis, ...),
// LaTeX, man pages, XML document formats (DocBook, FictionBook, JATS, TEI,
// OPML), Jupyter notebooks, and the bridges between documents and plain
// text, tables and data. Office and ebook containers and PDF live in their
// own families and use document_api.hpp.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/registry.hpp"
#include "families/document/formats.hpp"
#include "families/families.hpp"

namespace cvt {

void register_document_family(Registry& registry) {
  document::register_html(registry);
  document::register_web_archives(registry);
  document::register_markdown(registry);
  document::register_rst(registry);
  document::register_asciidoc(registry);
  document::register_org(registry);
  document::register_djot(registry);
  document::register_typst(registry);
  document::register_wikis(registry);
  document::register_textile(registry);
  document::register_bbcode(registry);
  document::register_txt2tags(registry);
  document::register_pod(registry);
  document::register_latex(registry);
  document::register_man(registry);
  document::register_texinfo(registry);
  document::register_xml_formats(registry);
  document::register_ipynb(registry);
  document::register_bridges(registry);
}

}  // namespace cvt
