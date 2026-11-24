// Convertible core - office family: Microsoft Reader binary markup tokens.
//
// Element numbers of HTML pages follow the order of the MSHTML element
// identifiers (TAGID_*) offset by one. Attribute numbers were established
// from books written by an independent LIT producer (see the test fixtures'
// SOURCES.md); attributes missing here are skipped when reading.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/office/lit_tokens.hpp"

#include <array>

namespace cvt::office::lit {

namespace {

constexpr std::array<std::string_view, 109> html_tag_names = {
    "", "", "", "a", "acronym", "address", "applet", "area", "b", "base", "basefont", "bdo", "bgsound", "big", "blink",
    "blockquote", "body", "br", "button", "caption", "center", "cite", "code", "col", "colgroup", "", "", "dd", "del",
    "dfn", "dir", "div", "dl", "dt", "em", "embed", "fieldset", "font", "form", "frame", "frameset", "", "h1", "h2", "h3",
    "h4", "h5", "h6", "head", "hr", "html", "i", "iframe", "img", "input", "ins", "kbd", "label", "legend", "li", "link",
    "listing", "map", "marquee", "menu", "meta", "nextid", "nobr", "noembed", "noframes", "noscript", "object", "ol",
    "option", "p", "param", "plaintext", "pre", "q", "rp", "rt", "ruby", "s", "samp", "script", "select", "small", "span",
    "strike", "strong", "style", "sub", "sup", "table", "tbody", "tc", "td", "textarea", "tfoot", "th", "thead", "title",
    "tr", "tt", "u", "ul", "var", "wbr", "xmp"};

// Attributes shared by every element. "%" marks attributes that are skipped:
// class names only refer to the book's style sheets.
constexpr std::array html_global = {
    Token{0x8001, "name"},   Token{0x8006, "width"}, Token{0x8046, "title"}, Token{0x83EA, "%class"},
    Token{0x83EB, "id"},     Token{0x938E, "nowrap"}, Token{0x9392, "lang"}, Token{0x93FE, "dir"},
};

constexpr std::array a_attributes = {Token{0x0001, "href"}, Token{0x03EC, "target"}};
constexpr std::array img_attributes = {Token{0x03EB, "alt"}, Token{0x03EC, "src"}, Token{0x03ED, "border"},
                                       Token{0x07D9, "usemap"}};
constexpr std::array table_attributes = {Token{0x03EB, "border"}, Token{0x03EE, "cellspacing"},
                                         Token{0x03EF, "cellpadding"}};
constexpr std::array cell_attributes = {Token{0x07D2, "rowspan"}, Token{0x07D3, "colspan"}};
constexpr std::array ol_attributes = {Token{0x03EC, "start"}};
constexpr std::array li_attributes = {Token{0x03EA, "value"}};
constexpr std::array col_attributes = {Token{0x03EA, "span"}};
constexpr std::array meta_attributes = {Token{0x03EA, "http-equiv"}, Token{0x03EB, "content"}};
constexpr std::array link_attributes = {Token{0x03EE, "href"}, Token{0x03EF, "rel"}, Token{0x03F1, "type"}};

// Element numbers (see html_tag_names) with attributes of their own.
constexpr std::array html_local = {
    TagAttributes{3, a_attributes},       TagAttributes{53, img_attributes},  TagAttributes{93, table_attributes},
    TagAttributes{96, cell_attributes},   TagAttributes{99, cell_attributes}, TagAttributes{72, ol_attributes},
    TagAttributes{59, li_attributes},     TagAttributes{23, col_attributes},  TagAttributes{24, col_attributes},
    TagAttributes{65, meta_attributes},   TagAttributes{60, link_attributes},
};

// The OEB 1.0 package description: package structure and Dublin Core.
constexpr std::array<std::string_view, 42> opf_tag_names = {
    "", "package", "dc:Title", "dc:Creator", "", "", "", "", "", "", "", "", "", "", "", "", "manifest", "item",
    "spine", "itemref", "metadata", "dc-metadata", "dc:Subject", "dc:Description", "dc:Publisher", "dc:Contributor",
    "dc:Date", "dc:Type", "dc:Format", "dc:Identifier", "dc:Source", "dc:Language", "dc:Relation", "dc:Coverage",
    "dc:Rights", "x-metadata", "meta", "tours", "tour", "site", "guide", "reference"};

// "%"-prefixed attributes are internal to the format and skipped.
constexpr std::array opf_global = {
    Token{0x03, "%guid"},  Token{0x04, "%minimum_level"},  Token{0x05, "%attr5"},  Token{0x06, "id"},
    Token{0x07, "href"},   Token{0x08, "media-type"},      Token{0x0A, "idref"},   Token{0x0B, "xmlns:dc"},
    Token{0x0C, "xmlns:oebpackage"},                       Token{0x0D, "role"},    Token{0x0E, "file-as"},
    Token{0x10, "scheme"}, Token{0x11, "title"},           Token{0x12, "type"},    Token{0x13, "unique-identifier"},
    Token{0x14, "name"},   Token{0x15, "content"},
};
constexpr std::array<TagAttributes, 0> opf_local = {};

constexpr TokenMap html_map(html_tag_names, html_global, html_local);
constexpr TokenMap opf_map(opf_tag_names, opf_global, opf_local);

}  // namespace

const TokenMap& html_tokens() noexcept { return html_map; }
const TokenMap& opf_tokens() noexcept { return opf_map; }

}  // namespace cvt::office::lit
