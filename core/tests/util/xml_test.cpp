// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "support/fixtures.hpp"
#include "support/test_support.hpp"
#include "util/text.hpp"
#include "util/xml.hpp"

using namespace cvt;
using namespace cvt::util;

namespace {

// --- helpers -----------------------------------------------------------------

XmlParseOptions keep_whitespace() {
  XmlParseOptions options;
  options.preserve_whitespace = true;
  return options;
}

XmlParseOptions lenient_text() {
  XmlParseOptions options;
  options.replace_invalid_text = true;
  return options;
}

XmlParseOptions keep_entities() {
  XmlParseOptions options;
  options.keep_unknown_entities = true;
  return options;
}

/// No indentation, no declaration: exactly the tree.
XmlWriteOptions compact() {
  XmlWriteOptions options;
  options.indent.clear();
  options.declaration = XmlDeclaration::Never;
  return options;
}

XmlWriteOptions pretty_without_declaration() {
  XmlWriteOptions options;
  options.declaration = XmlDeclaration::Never;
  return options;
}

std::string visible(std::string_view text) {
  std::string out;
  for (const char c : text) {
    if (c == '\n') {
      out += "\\n";
    } else if (c == '\t') {
      out += "\\t";
    } else if (c == '\r') {
      out += "\\r";
    } else {
      out += c;
    }
  }
  return out;
}

/// Compact, writer-independent picture of a (shallow) tree:
///   element      name[a=1 b=2](child,child)
///   text         'text'           CDATA   cdata'text'
///   comment      #'text'          PI      ?target'data'
///   doctype      !name            declaration   decl
void shape_into(std::string& out, XmlNode node) {
  const auto children = [&](std::string_view open, std::string_view close) {
    bool first = true;
    for (const XmlNode child : node.children()) {
      out += first ? open : std::string_view(",");
      first = false;
      shape_into(out, child);
    }
    if (!first) out += close;
  };
  switch (node.type()) {
    case XmlNodeType::Document: children("", ""); break;
    case XmlNodeType::Element: {
      out += node.name();
      bool first = true;
      for (const XmlAttribute attribute : node.attributes()) {
        out += first ? '[' : ' ';
        first = false;
        out += attribute.name();
        out += '=';
        out += visible(attribute.value());
      }
      if (!first) out += ']';
      children("(", ")");
      break;
    }
    case XmlNodeType::Text: out += "'" + visible(node.value()) + "'"; break;
    case XmlNodeType::CData: out += "cdata'" + visible(node.value()) + "'"; break;
    case XmlNodeType::Comment: out += "#'" + visible(node.value()) + "'"; break;
    case XmlNodeType::ProcessingInstruction:
      out += "?" + std::string(node.name()) + "'" + visible(node.value()) + "'";
      break;
    case XmlNodeType::Doctype: out += "!" + std::string(node.name()); break;
    case XmlNodeType::Declaration: out += "decl"; break;
    case XmlNodeType::Null: out += "null"; break;
  }
}

std::string shape(XmlNode node) {
  std::string out;
  shape_into(out, node);
  return out;
}

/// shape() of the parsed document, or "ERROR: <message>".
std::string parsed_shape(std::string_view xml, const XmlParseOptions& options = {}) {
  const auto parsed = parse_xml(xml, options);
  return parsed ? shape(*parsed) : "ERROR: " + parsed.error().message;
}

/// The error message for `xml`, or "" when it parses.
std::string error_of(std::string_view xml, const XmlParseOptions& options = {}) {
  const auto parsed = parse_xml(xml, options);
  return parsed ? std::string() : parsed.error().message;
}
std::string error_of(ByteView xml, const XmlParseOptions& options = {}) {
  const auto parsed = parse_xml(xml, options);
  return parsed ? std::string() : parsed.error().message;
}

Status status_of(std::string_view xml, const XmlParseOptions& options = {}) {
  const auto parsed = parse_xml(xml, options);
  return parsed ? Status::Ok : parsed.error().code;
}

/// Text of the document element, or "ERROR: <message>".
std::string root_text(std::string_view xml, const XmlParseOptions& options = {}) {
  const auto parsed = parse_xml(xml, options);
  return parsed ? parsed->root().text() : "ERROR: " + parsed.error().message;
}
std::string root_text(ByteView xml, const XmlParseOptions& options = {}) {
  const auto parsed = parse_xml(xml, options);
  return parsed ? parsed->root().text() : "ERROR: " + parsed.error().message;
}

/// The full message parse_xml produces for a problem at a 0-based byte
/// offset on the first line of pure-ASCII input.
std::string at(std::size_t byte, std::string_view reason) {
  return std::format("invalid XML at line 1, column {} (byte {}): {}", byte + 1, byte, reason);
}

Bytes utf16(std::u16string_view text, bool big_endian, bool with_mark) {
  Bytes out;
  const auto put = [&](char16_t unit) {
    const auto low = static_cast<std::byte>(unit & 0xFF);
    const auto high = static_cast<std::byte>(unit >> 8);
    out.push_back(big_endian ? high : low);
    out.push_back(big_endian ? low : high);
  };
  if (with_mark) put(u'\xFEFF');
  for (const char16_t unit : text) put(unit);
  return out;
}

Bytes utf32(std::u32string_view text, bool big_endian, bool with_mark) {
  Bytes out;
  const auto put = [&](char32_t unit) {
    for (int i = 0; i < 4; ++i) {
      const int shift = big_endian ? 8 * (3 - i) : 8 * i;
      out.push_back(static_cast<std::byte>((unit >> shift) & 0xFF));
    }
  };
  if (with_mark) put(U'\xFEFF');
  for (const char32_t unit : text) put(unit);
  return out;
}

/// Writes `document` both ways the writer guarantees to round-trip and
/// reports whether the re-parsed trees are equal to it.
bool round_trips(const XmlDocument& document) {
  const auto exact = parse_xml(xml_to_string(document, compact()), keep_whitespace());
  if (!exact || !deep_equal(document, *exact)) return false;
  XmlWriteOptions ascii;
  ascii.indent.clear();
  ascii.escape_non_ascii = true;
  ascii.self_close_empty = false;
  const auto escaped = parse_xml(xml_to_string(document, ascii), keep_whitespace());
  return escaped && deep_equal(document, *escaped);
}

/// A worksheet-like document of roughly `bytes` bytes.
std::string make_sheet(std::size_t bytes) {
  std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<worksheet xmlns=\"urn:example:sheet\"><sheetData>";
  std::uint32_t state = 7;
  std::size_t row = 1;
  while (out.size() < bytes) {
    out += "<row r=\"" + std::to_string(row) + "\">";
    for (int column = 0; column < 12; ++column) {
      state = state * 1664525u + 1013904223u;
      out += "<c r=\"";
      out.push_back(static_cast<char>('A' + column));
      out += std::to_string(row) + "\" s=\"" + std::to_string(state >> 28) + "\"><v>" + std::to_string(state >> 12) +
             "</v></c>";
    }
    out += "</row>\n";
    ++row;
  }
  out += "</sheetData></worksheet>";
  return out;
}

double seconds_to_parse(std::string_view xml, const XmlParseOptions& options = {}) {
  const auto start = std::chrono::steady_clock::now();
  const auto parsed = parse_xml(xml, options);
  const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
  return parsed || !parsed.error().message.empty() ? elapsed.count() : 0.0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

TEST_CASE("xml: parse, navigate and serialise") {
  const auto parsed = parse_xml(
      R"(<?xml version="1.0"?><w:doc xmlns:w="urn:x"><w:p a="1">Hello <b>big</b> world</w:p><w:p/></w:doc>)");
  REQUIRE(parsed);
  const XmlDocument& document = *parsed;
  const XmlNode root = document.root();
  CHECK(root.name() == "w:doc");
  CHECK(root.local_name() == "doc");
  CHECK(xml_local_name(root.name()) == "doc");
  const XmlNode paragraph = root.child_local("p");
  REQUIRE(paragraph);
  CHECK(paragraph.attribute("a").as_int() == 1);
  CHECK(paragraph.text() == "Hello big world");
  CHECK(root.text() == "Hello big world");
  CHECK(document.node().text() == "Hello big world");
  CHECK_FALSE(root.child_local("missing"));
  CHECK(XmlNode().text().empty());

  XmlWriteOptions flat;
  flat.indent.clear();
  const std::string with_declaration = xml_to_string(document, flat);
  CHECK(with_declaration ==
        R"(<?xml version="1.0"?><w:doc xmlns:w="urn:x"><w:p a="1">Hello <b>big</b> world</w:p><w:p/></w:doc>)");
  CHECK(xml_to_string(paragraph, compact()) == "<w:p a=\"1\">Hello <b>big</b> world</w:p>");
  XmlWriteOptions always = flat;
  always.declaration = XmlDeclaration::Always;
  CHECK(xml_to_string(paragraph.child("b"), always) == "<?xml version=\"1.0\" encoding=\"UTF-8\"?><b>big</b>");

  XmlDocument built;
  XmlNode node = built.append_child("root");
  node.set_attribute("id", "a&b");
  node.append_child("item").append_text("1 < 2");
  CHECK(xml_to_string(built) ==
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<root id=\"a&amp;b\">\n  <item>1 &lt; 2</item>\n</root>\n");
  CHECK(xml_to_string(built, compact()) == "<root id=\"a&amp;b\"><item>1 &lt; 2</item></root>");
}

TEST_CASE("xml: every node kind") {
  const auto parsed = parse_xml("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                                "<!DOCTYPE root SYSTEM \"root.dtd\">\n"
                                "<!-- prolog -->\n"
                                "<?style href=\"a.css\" ?>\n"
                                "<root id=\"r\">text<![CDATA[<raw> & ]]]><!--inner--><?pi?><child/></root>\n"
                                "<!-- epilog -->\n");
  REQUIRE(parsed);
  const XmlNode document = parsed->node();
  CHECK(document.type() == XmlNodeType::Document);
  CHECK(document.name().empty());
  CHECK(document.value().empty());
  CHECK_FALSE(document.parent());
  CHECK(shape(document) == "decl,!root,#' prolog ',?style'href=\"a.css\" ',"
                           "root[id=r]('text',cdata'<raw> & ]',#'inner',?pi'',child),#' epilog '");

  const XmlNode declaration = parsed->declaration();
  CHECK(declaration.type() == XmlNodeType::Declaration);
  CHECK(declaration == document.first_child());
  CHECK(declaration.name() == "xml");
  CHECK(declaration.value().empty());
  CHECK(declaration.attribute("version").value() == "1.0");
  CHECK(declaration.attribute("encoding").value() == "UTF-8");
  CHECK(declaration.attribute("standalone").value() == "yes");
  CHECK_FALSE(declaration.first_child());

  const XmlNode doctype = parsed->doctype();
  CHECK(doctype.type() == XmlNodeType::Doctype);
  CHECK(doctype.name() == "root");
  CHECK(doctype.value() == "root SYSTEM \"root.dtd\"");
  CHECK_FALSE(doctype.first_child());
  CHECK_FALSE(doctype.first_attribute());

  const XmlNode root = parsed->root();
  CHECK(root.type() == XmlNodeType::Element);
  CHECK(root.is_element());
  CHECK_FALSE(root.is_text());
  CHECK(root.value().empty());
  CHECK(root.parent() == document);

  XmlNode node = root.first_child();
  CHECK(node.type() == XmlNodeType::Text);
  CHECK(node.is_text());
  CHECK(node.name().empty());
  CHECK(node.value() == "text");
  CHECK(node.text() == "text");
  node = node.next_sibling();
  CHECK(node.type() == XmlNodeType::CData);
  CHECK(node.is_text());
  CHECK(node.value() == "<raw> & ]");
  node = node.next_sibling();
  CHECK(node.type() == XmlNodeType::Comment);
  CHECK(node.value() == "inner");
  CHECK(node.text().empty());
  node = node.next_sibling();
  CHECK(node.type() == XmlNodeType::ProcessingInstruction);
  CHECK(node.name() == "pi");
  CHECK(node.value().empty());
  node = node.next_sibling();
  CHECK(node.type() == XmlNodeType::Element);
  CHECK(node == root.last_child());
  CHECK_FALSE(node.next_sibling());

  CHECK(document.last_child().type() == XmlNodeType::Comment);
  CHECK(document.last_child().value() == " epilog ");
  CHECK(root.text() == "text<raw> & ]");
  CHECK(XmlNode().type() == XmlNodeType::Null);

  // A document without declaration or doctype has neither node.
  const auto bare = parse_xml("<a/>");
  REQUIRE(bare);
  CHECK_FALSE(bare->declaration());
  CHECK_FALSE(bare->doctype());
  CHECK(bare->root() == bare->node().first_child());
}

TEST_CASE("xml: null handles are safe everywhere") {
  const XmlDocument empty;
  CHECK_FALSE(empty.root());
  CHECK(empty.node().type() == XmlNodeType::Document);

  XmlNode none;
  CHECK_FALSE(none);
  CHECK(none == XmlNode());
  CHECK(none.type() == XmlNodeType::Null);
  CHECK(none.name().empty());
  CHECK(none.local_name().empty());
  CHECK(none.prefix().empty());
  CHECK(none.namespace_uri().empty());
  CHECK(none.lookup_namespace("x").empty());
  CHECK(none.value().empty());
  CHECK_FALSE(none.parent());
  CHECK_FALSE(none.first_child());
  CHECK_FALSE(none.last_child());
  CHECK_FALSE(none.next_sibling());
  CHECK_FALSE(none.previous_sibling());
  CHECK_FALSE(none.child("a"));
  CHECK_FALSE(none.child_local("a"));
  CHECK_FALSE(none.child("urn:x", "a"));
  CHECK_FALSE(none.next_sibling("a"));
  CHECK_FALSE(none.next_sibling_local("a"));
  CHECK_FALSE(none.next_sibling("urn:x", "a"));
  CHECK_FALSE(none.find("a/b"));
  CHECK(none.find_all("a/b").empty());
  CHECK_FALSE(none.attribute("a"));
  CHECK_FALSE(none.attribute_local("a"));
  CHECK_FALSE(none.attribute("urn:x", "a"));
  CHECK_FALSE(none.first_attribute());
  CHECK(none.text().empty());
  CHECK(none.child_text("a").empty());
  CHECK(none.children().empty());
  CHECK(none.children("a").empty());
  CHECK(none.children_local("a").empty());
  CHECK(none.children("urn:x", "a").empty());
  CHECK(none.elements().empty());
  CHECK(none.attributes().empty());
  CHECK(none.descendants().empty());
  int visited = 0;
  for (const XmlNode child : none.children()) visited += child ? 1 : 0;
  for (const XmlNode child : none.descendants()) visited += child ? 1 : 0;
  for (const XmlAttribute attribute : none.attributes()) visited += attribute ? 1 : 0;
  CHECK(visited == 0);

  // The classic chained lookup through nodes that do not exist.
  CHECK(empty.root().child("a").child("b").attribute("x").value().empty());
  CHECK(empty.root().child("a").child("b").attribute("x").as_int(7) == 7);
  CHECK(empty.root().find("a/b/c").first_child().text().empty());

  XmlAttribute nothing;
  CHECK_FALSE(nothing);
  CHECK(nothing == XmlAttribute());
  CHECK(nothing.name().empty());
  CHECK(nothing.local_name().empty());
  CHECK(nothing.prefix().empty());
  CHECK(nothing.namespace_uri().empty());
  CHECK(nothing.value().empty());
  CHECK(nothing.as_int(-1) == -1);
  CHECK(nothing.as_uint(2u) == 2u);
  CHECK(nothing.as_int64(-3) == -3);
  CHECK(nothing.as_uint64(4u) == 4u);
  CHECK(nothing.as_double(1.5) == 1.5);
  CHECK(nothing.as_bool(true));
  CHECK_FALSE(nothing.as_bool());
  CHECK_FALSE(nothing.parent());
  CHECK_FALSE(nothing.next_attribute());
  CHECK_FALSE(nothing.set_value("x"));
  CHECK_FALSE(nothing.set_value(3));

  // Mutators do nothing on a null handle.
  CHECK_FALSE(none.append_child("a"));
  CHECK_FALSE(none.prepend_child("a"));
  CHECK_FALSE(none.insert_child_before("a", XmlNode()));
  CHECK_FALSE(none.insert_child_after("a", XmlNode()));
  CHECK_FALSE(none.append_text("t"));
  CHECK_FALSE(none.append_cdata("t"));
  CHECK_FALSE(none.append_comment("t"));
  CHECK_FALSE(none.append_pi("t", "d"));
  CHECK_FALSE(none.set_name("a"));
  CHECK_FALSE(none.set_value("v"));
  CHECK_FALSE(none.set_text("v"));
  CHECK_FALSE(none.set_attribute("a", "v"));
  CHECK_FALSE(none.set_attribute("a", 1));
  CHECK_FALSE(none.remove_attribute("a"));
  CHECK_FALSE(none.remove_attribute(XmlAttribute()));
  CHECK_FALSE(none.remove_child(XmlNode()));
  none.clear_children();

  CHECK(xml_to_string(none).empty());
  CHECK(deep_equal(XmlNode(), XmlNode()));
  CHECK_FALSE(deep_equal(XmlNode(), empty.node()));
  CHECK_FALSE(deep_equal(empty.node(), XmlNode()));
}

TEST_CASE("xml: navigation and lookup") {
  const auto parsed = parse_xml(
      R"(<r xmlns:p="urn:p" xmlns:q="urn:q"><a i="1"/>text<p:a i="2"/><b/><q:a i="3"/><a i="4"/><!--c--></r>)");
  REQUIRE(parsed);
  const XmlNode r = parsed->root();

  SUBCASE("siblings and parents") {
    const XmlNode first = r.first_child();
    const XmlNode last = r.last_child();
    CHECK(first.attribute("i").value() == "1");
    CHECK(last.type() == XmlNodeType::Comment);
    CHECK_FALSE(first.previous_sibling());
    CHECK_FALSE(last.next_sibling());
    CHECK(first.next_sibling().value() == "text");
    CHECK(first.next_sibling().previous_sibling() == first);
    CHECK(last.previous_sibling().attribute("i").value() == "4");
    CHECK(first.parent() == r);
    CHECK(r.parent() == parsed->node());
    std::string backwards;
    for (XmlNode n = last; n; n = n.previous_sibling()) backwards += std::to_string(static_cast<int>(n.type()));
    CHECK(backwards == "5222232");  // comment, a, q:a, b, p:a, text, a
  }

  SUBCASE("children by qualified name, local name and namespace") {
    CHECK(r.child("a").attribute("i").value() == "1");
    CHECK(r.child("p:a").attribute("i").value() == "2");
    CHECK(r.child("q:a").attribute("i").value() == "3");
    CHECK_FALSE(r.child("x:a"));
    CHECK_FALSE(r.child("text"));
    CHECK(r.child_local("a").attribute("i").value() == "1");
    CHECK(r.child_local("b").name() == "b");
    CHECK(r.child("urn:q", "a").attribute("i").value() == "3");
    CHECK(r.child("urn:p", "a").attribute("i").value() == "2");
    CHECK(r.child("", "a").attribute("i").value() == "1");
    CHECK_FALSE(r.child("urn:none", "a"));

    const XmlNode a1 = r.child("a");
    CHECK(a1.next_sibling("a").attribute("i").value() == "4");
    CHECK_FALSE(a1.next_sibling("a").next_sibling("a"));
    CHECK(a1.next_sibling_local("a").attribute("i").value() == "2");
    CHECK(a1.next_sibling("urn:q", "a").attribute("i").value() == "3");
    CHECK(a1.next_sibling("b").name() == "b");
  }

  SUBCASE("ranges") {
    const auto ids = [](XmlNodeRange range) {
      std::string out;
      for (const XmlNode n : range) out += n.is_element() ? std::string(n.attribute("i").value()) + ";" : "?;";
      return out;
    };
    CHECK(ids(r.children()) == "1;?;2;;3;4;?;");
    CHECK(ids(r.elements()) == "1;2;;3;4;");
    CHECK(ids(r.children("a")) == "1;4;");
    CHECK(ids(r.children_local("a")) == "1;2;3;4;");
    CHECK(ids(r.children("urn:p", "a")) == "2;");
    CHECK(ids(r.children("", "a")) == "1;4;");
    CHECK(ids(r.children("nope")).empty());
    CHECK(r.children("nope").empty());
    CHECK_FALSE(r.children().empty());
    CHECK(r.child("b").children().empty());

    XmlNodeIterator it = r.children("a").begin();
    const XmlNodeIterator before = it++;
    CHECK((*before).attribute("i").value() == "1");
    CHECK((*it).attribute("i").value() == "4");
    CHECK(++it == r.children("a").end());
  }

  SUBCASE("descendants are visited in document order") {
    const auto deep = parse_xml("<a><b><c/>t<d><e/></d></b><f/><!--g--><h><i>u</i></h></a>");
    REQUIRE(deep);
    std::string order;
    for (const XmlNode n : deep->root().descendants()) {
      order += n.is_element() ? std::string(n.name()) : n.is_text() ? "'" + std::string(n.value()) + "'" : "#";
    }
    CHECK(order == "bc't'def#hi'u'");
    order.clear();
    for (const XmlNode n : deep->root().child("b").descendants()) order += n.is_element() ? n.name() : "'";
    CHECK(order == "c'de");  // never leaves the subtree
    order.clear();
    for (const XmlNode n : deep->node().descendants()) order += n.is_element() ? n.name() : "";
    CHECK(order == "abcdefhi");
    CHECK(deep->root().child("f").descendants().empty());
    XmlDescendantIterator it = deep->root().descendants().begin();
    const XmlDescendantIterator old = it++;
    CHECK((*old).name() == "b");
    CHECK((*it).name() == "c");
  }
}

TEST_CASE("xml: find and find_all") {
  const auto parsed = parse_xml(
      R"(<r><a><x/></a><a><b id="1"/><b id="2"><c/></b></a><c><b id="3"/></c><p:a xmlns:p="u"><b id="4"/></p:a>t</r>)");
  REQUIRE(parsed);
  const XmlNode r = parsed->root();
  const auto ids = [](const std::vector<XmlNode>& nodes) {
    std::string out;
    for (const XmlNode n : nodes) out += std::string(n.attribute("id").value()) + ";";
    return out;
  };
  CHECK(r.find("a").first_child().name() == "x");
  CHECK(r.find("a/b").attribute("id").value() == "1");  // the first <a> has no <b>: backtracks
  CHECK(r.find("a/b/c").parent().attribute("id").value() == "2");
  CHECK(r.find("c/b").attribute("id").value() == "3");
  CHECK(r.find("p:a/b").attribute("id").value() == "4");
  CHECK(r.find("*/b").attribute("id").value() == "1");
  CHECK(r.find("*").first_child().name() == "x");
  CHECK(r.find("*/*/c").name() == "c");
  CHECK(ids(r.find_all("a/b")) == "1;2;");
  CHECK(ids(r.find_all("*/b")) == "1;2;3;4;");
  CHECK(ids(r.find_all("*/*")) == ";1;2;3;4;");
  CHECK(r.find_all("a").size() == 2u);
  CHECK(r.find_all("*").size() == 4u);  // elements only: the text is not matched
  CHECK(parsed->node().find("r/c/b").attribute("id").value() == "3");

  CHECK_FALSE(r.find(""));
  CHECK_FALSE(r.find("/a"));
  CHECK_FALSE(r.find("a/"));
  CHECK_FALSE(r.find("a//b"));
  CHECK_FALSE(r.find("nope"));
  CHECK_FALSE(r.find("a/nope"));
  CHECK_FALSE(r.find("a/b/c/d"));
  CHECK_FALSE(r.find("r"));  // steps start below the node
  CHECK(r.find_all("").empty());
  CHECK(r.find_all("a/x/y").empty());
  CHECK_FALSE(r.find("a").first_child().find("anything"));
}

TEST_CASE("xml: attributes and typed getters") {
  const auto parsed = parse_xml(R"(<e xmlns:n="urn:n" a="1" n:b="two" c="" d=' 42 ' n:a="ns"/>)");
  REQUIRE(parsed);
  const XmlNode e = parsed->root();

  std::string order;
  for (const XmlAttribute a : e.attributes()) order += std::string(a.name()) + "=" + std::string(a.value()) + ";";
  CHECK(order == "xmlns:n=urn:n;a=1;n:b=two;c=;d= 42 ;n:a=ns;");
  CHECK(e.first_attribute().name() == "xmlns:n");
  CHECK(e.first_attribute().next_attribute().name() == "a");
  CHECK(e.first_attribute().parent() == e);

  CHECK(e.attribute("a").value() == "1");
  CHECK(e.attribute("n:b").value() == "two");
  CHECK(e.attribute("n:b").local_name() == "b");
  CHECK(e.attribute("n:b").prefix() == "n");
  CHECK(e.attribute("a").prefix().empty());
  CHECK(e.attribute("c"));
  CHECK(e.attribute("c").value().empty());
  CHECK_FALSE(e.attribute("b"));
  CHECK_FALSE(e.attribute("missing"));
  CHECK(e.attribute_local("b").value() == "two");
  CHECK(e.attribute_local("a").value() == "1");  // first one with that local name
  CHECK(e.attribute("urn:n", "a").value() == "ns");
  CHECK(e.attribute("", "a").value() == "1");
  CHECK_FALSE(e.attribute("urn:other", "a"));
  CHECK_FALSE(e.attribute("urn:n", "c"));

  const auto numbers = parse_xml(R"(<n i="-17" plus="+5" big="9223372036854775807" huge="9223372036854775808"
      umax="18446744073709551615" over="18446744073709551616" imax="2147483647" iover="2147483648"
      imin="-2147483648" uover="4294967296" f="2.5" e="1e3" neg="-0.125" comma="1,5" text="abc" trail="12px"
      space="  7  " empty="" hex="0x10" t="true" f0="false" one="1" zero="0" yes="yes" T="TRUE" inf="inf"/>)");
  REQUIRE(numbers);
  const XmlNode n = numbers->root();
  CHECK(n.attribute("i").as_int() == -17);
  CHECK(n.attribute("plus").as_int() == 5);
  CHECK(n.attribute("imax").as_int() == 2147483647);
  CHECK(n.attribute("iover").as_int(-1) == -1);
  CHECK(n.attribute("imin").as_int() == std::numeric_limits<int>::min());
  CHECK(n.attribute("iover").as_int64() == 2147483648LL);
  CHECK(n.attribute("big").as_int64() == std::numeric_limits<std::int64_t>::max());
  CHECK(n.attribute("huge").as_int64(-5) == -5);
  CHECK(n.attribute("huge").as_uint64() == 9223372036854775808ULL);
  CHECK(n.attribute("umax").as_uint64() == std::numeric_limits<std::uint64_t>::max());
  CHECK(n.attribute("over").as_uint64(9u) == 9u);
  CHECK(n.attribute("i").as_uint(3u) == 3u);
  CHECK(n.attribute("i").as_uint64(3u) == 3u);
  CHECK(n.attribute("plus").as_uint() == 5u);
  CHECK(n.attribute("uover").as_uint(1u) == 1u);
  CHECK(n.attribute("uover").as_uint64() == 4294967296ULL);
  CHECK(n.attribute("space").as_int() == 7);
  CHECK(n.attribute("space").as_uint() == 7u);
  CHECK(n.attribute("space").as_double() == 7.0);
  CHECK(n.attribute("text").as_int(-1) == -1);
  CHECK(n.attribute("trail").as_int(-1) == -1);
  CHECK(n.attribute("empty").as_int(-1) == -1);
  CHECK(n.attribute("hex").as_int(-1) == -1);
  CHECK(n.attribute("f").as_int(-1) == -1);
  CHECK(n.attribute("f").as_double() == 2.5);
  CHECK(n.attribute("e").as_double() == 1000.0);
  CHECK(n.attribute("neg").as_double() == -0.125);
  CHECK(n.attribute("i").as_double() == -17.0);
  CHECK(n.attribute("comma").as_double(-1.0) == -1.0);  // never locale dependent
  CHECK(n.attribute("text").as_double(-1.0) == -1.0);
  CHECK(n.attribute("empty").as_double(-1.0) == -1.0);
  CHECK(std::isinf(n.attribute("inf").as_double()));
  CHECK(n.attribute("t").as_bool());
  CHECK_FALSE(n.attribute("f0").as_bool(true));
  CHECK(n.attribute("one").as_bool());
  CHECK_FALSE(n.attribute("zero").as_bool(true));
  CHECK(n.attribute("yes").as_bool(true));
  CHECK_FALSE(n.attribute("yes").as_bool(false));
  CHECK_FALSE(n.attribute("T").as_bool(false));
  CHECK(n.attribute("space").as_bool(true));
  CHECK_FALSE(n.attribute("empty").as_bool());
}

TEST_CASE("xml: namespaces are resolved through the in-scope declarations") {
  const auto parsed = parse_xml(R"(<root xmlns="urn:default" xmlns:a="urn:a" a:attr="1" plain="2">
    <child/>
    <a:child a:x="3" y="4"/>
    <inner xmlns="urn:inner" xmlns:a="urn:shadow"><a:leaf a:z="5"/><leaf/></inner>
    <none xmlns=""><leaf/></none>
    <u:undeclared u:attr="6" xml:lang="en"/>
  </root>)");
  REQUIRE(parsed);
  const XmlNode root = parsed->root();
  CHECK(root.namespace_uri() == "urn:default");
  CHECK(root.prefix().empty());
  CHECK(root.local_name() == "root");
  CHECK(root.attribute("a:attr").namespace_uri() == "urn:a");
  CHECK(root.attribute("plain").namespace_uri().empty());  // no prefix: no namespace, even with a default one
  CHECK(root.attribute("xmlns").namespace_uri().empty());
  CHECK(root.attribute("xmlns:a").namespace_uri() == xmlns_namespace_uri);

  CHECK(root.child("child").namespace_uri() == "urn:default");  // inherited default namespace
  const XmlNode prefixed = root.child("a:child");
  CHECK(prefixed.namespace_uri() == "urn:a");
  CHECK(prefixed.prefix() == "a");
  CHECK(prefixed.local_name() == "child");
  CHECK(prefixed.attribute("a:x").namespace_uri() == "urn:a");
  CHECK(prefixed.attribute("y").namespace_uri().empty());

  const XmlNode inner = root.child("inner");
  CHECK(inner.namespace_uri() == "urn:inner");
  CHECK(inner.child("a:leaf").namespace_uri() == "urn:shadow");  // the nearer declaration wins
  CHECK(inner.child("a:leaf").attribute("a:z").namespace_uri() == "urn:shadow");
  CHECK(inner.child("leaf").namespace_uri() == "urn:inner");
  CHECK(prefixed.namespace_uri() == "urn:a");  // shadowing ended with <inner>

  const XmlNode none = root.child("none");
  CHECK(none.namespace_uri().empty());  // xmlns="" removes the default namespace
  CHECK(none.child("leaf").namespace_uri().empty());

  // An undeclared prefix is not an error: the name simply has no namespace.
  const XmlNode undeclared = root.child("u:undeclared");
  REQUIRE(undeclared);
  CHECK(undeclared.namespace_uri().empty());
  CHECK(undeclared.prefix() == "u");
  CHECK(undeclared.attribute("u:attr").namespace_uri().empty());
  CHECK(undeclared.attribute("xml:lang").namespace_uri() == xml_namespace_uri);  // built in

  CHECK(root.lookup_namespace("") == "urn:default");
  CHECK(root.lookup_namespace("a") == "urn:a");
  CHECK(root.lookup_namespace("zzz").empty());
  CHECK(root.lookup_namespace("xml") == "http://www.w3.org/XML/1998/namespace");
  CHECK(root.lookup_namespace("xmlns") == "http://www.w3.org/2000/xmlns/");
  CHECK(inner.child("leaf").lookup_namespace("a") == "urn:shadow");
  CHECK(inner.child("leaf").lookup_namespace("") == "urn:inner");
  CHECK(none.lookup_namespace("").empty());
  CHECK(none.lookup_namespace("a") == "urn:a");
  CHECK(inner.child("leaf").parent().parent().first_child().lookup_namespace("a") == "urn:a");

  // Lookups by namespace.
  CHECK(root.child("urn:default", "child") == root.child("child"));
  CHECK(root.child("urn:a", "child") == prefixed);
  CHECK(inner.child("urn:shadow", "leaf") == inner.child("a:leaf"));
  CHECK(inner.child("urn:inner", "leaf") == inner.child("leaf"));
  CHECK(none.child("", "leaf") == none.child("leaf"));
  CHECK(root.child("", "undeclared") == undeclared);
  CHECK(prefixed.attribute("urn:a", "x").value() == "3");
  CHECK(prefixed.attribute("", "y").value() == "4");
  CHECK_FALSE(prefixed.attribute("", "x"));

  // Text and other nodes have no namespace.
  CHECK(root.find("inner/leaf").namespace_uri() == "urn:inner");
  CHECK(parsed->node().namespace_uri().empty());

  // Names may contain several colons; the prefix ends at the last one.
  const auto odd = parse_xml("<a:b:c xmlns:a:b='urn:odd' :x='1'/>");
  REQUIRE(odd);
  CHECK(odd->root().prefix() == "a:b");
  CHECK(odd->root().local_name() == "c");
  CHECK(odd->root().namespace_uri() == "urn:odd");
  CHECK(odd->root().attribute(":x").local_name() == "x");
}

TEST_CASE("xml: attribute values are normalised") {
  // Literal tab, LF, CR and CRLF each become one space; references are kept.
  const auto parsed = parse_xml("<a t='a\tb' n='a\nb' r='a\rb' rn='a\r\nb' refs='a&#9;b&#10;c&#13;d&#32;e'\n"
                                "   pad='  x  y  ' quote=\"it's\" other='say \"hi\"' "
                                "ents='&lt;&amp;&gt;&quot;&apos;'/>");
  REQUIRE(parsed);
  const XmlNode a = parsed->root();
  CHECK(a.attribute("t").value() == "a b");
  CHECK(a.attribute("n").value() == "a b");
  CHECK(a.attribute("r").value() == "a b");
  CHECK(a.attribute("rn").value() == "a b");
  CHECK(a.attribute("refs").value() == "a\tb\nc\rd e");
  CHECK(a.attribute("pad").value() == "  x  y  ");  // CDATA attributes: spaces are not collapsed
  CHECK(a.attribute("quote").value() == "it's");
  CHECK(a.attribute("other").value() == "say \"hi\"");
  CHECK(a.attribute("ents").value() == "<&>\"'");

  // Normalisation also happens after decoding has started shortening the value.
  const auto mixed = parse_xml("<a v='&amp;\t\n&lt;\r\n&gt;\t'/>");
  REQUIRE(mixed);
  CHECK(mixed->root().attribute("v").value() == "&  < > ");

  // White space from an entity's replacement text is normalised as well;
  // character references inside it are not.
  const auto entity = parse_xml("<!DOCTYPE a [<!ENTITY e 'x\ty\nz'><!ENTITY f '&#38;#10;'>]><a v='&e;|&f;'>&e;</a>");
  REQUIRE(entity);
  CHECK(entity->root().attribute("v").value() == "x y z|\n");
  CHECK(entity->root().text() == "x\ty\nz");
}

TEST_CASE("xml: line endings are normalised to LF") {
  const auto parsed = parse_xml("<a>one\r\ntwo\rthree\nfour\r\n\r\nfive\r</a>", keep_whitespace());
  REQUIRE(parsed);
  CHECK(parsed->root().text() == "one\ntwo\nthree\nfour\n\nfive\n");

  const auto kinds = parse_xml("<a><![CDATA[c\r\nd\re]]><!--f\r\ng--><?pi h\r\ni\rj?></a>\r\n");
  REQUIRE(kinds);
  CHECK(shape(*kinds) == "a(cdata'c\\nd\\ne',#'f\\ng',?pi'h\\ni\\nj')");

  // A carriage return written as a character reference is data and survives.
  CHECK(root_text("<a>x&#13;&#xD;\r\ny</a>") == "x\r\r\ny");
  CHECK(root_text("<a>&#13;</a>") == "\r");

  const auto doctype = parse_xml("<!DOCTYPE a [\r\n<!ELEMENT a ANY>\r]>\r\n<a/>");
  REQUIRE(doctype);
  CHECK(doctype->doctype().value() == "a [\n<!ELEMENT a ANY>\n]");
}

TEST_CASE("xml: white-space-only text") {
  SUBCASE("default: dropped between elements") {
    CHECK(parsed_shape("<a>\n  <b/>\n  <c/>\n</a>") == "a(b,c)");
    CHECK(parsed_shape("<a> <b> <c/> </b> </a>") == "a(b(c))");
    CHECK(parsed_shape("<a>\t<!-- c -->\r\n<?pi?> </a>") == "a(#' c ',?pi'')");
    CHECK(parsed_shape("<a><b/> </a>") == "a(b)");
    CHECK(parsed_shape("<a> <b/></a>") == "a(b)");
  }
  SUBCASE("default: kept when it is the entire content of an element") {
    CHECK(parsed_shape("<a> </a>") == "a(' ')");
    CHECK(parsed_shape("<a>\n\t </a>") == "a('\\n\\t ')");
    CHECK(parsed_shape("<a><t> </t><t>\n</t></a>") == "a(t(' '),t('\\n'))");
    CHECK(parsed_shape("<a></a>") == "a");
  }
  SUBCASE("default: kept in mixed content") {
    CHECK(parsed_shape("<p>a <b>b</b> <i>c</i></p>") == "p('a ',b('b'),' ',i('c'))");
    // The decision covers the whole element, also runs seen before the text...
    CHECK(parsed_shape("<p> <b/> <i/> text</p>") == "p(' ',b,' ',i,' text')");
    CHECK(parsed_shape("<p> <b/> <i/>text <u/> </p>") == "p(' ',b,' ',i,'text ',u,' ')");
    // ...but not other elements: <q> below has no text of its own.
    CHECK(parsed_shape("<p>x <q> <b/> </q> </p>") == "p('x ',q(b),' ')");
    CHECK(parsed_shape("<p> <q>x</q> </p>") == "p(q('x'))");
  }
  SUBCASE("default: CDATA sections and references count as character data") {
    CHECK(parsed_shape("<a> <![CDATA[x]]> </a>") == "a(' ',cdata'x',' ')");
    CHECK(parsed_shape("<a> <![CDATA[]]> <b/> </a>") == "a(' ',cdata'',' ',b,' ')");
    CHECK(parsed_shape("<a> <b/>&#32;<c/> </a>") == "a(' ',b,' ',c,' ')");
    CHECK(parsed_shape("<a>&#x20;</a>") == "a(' ')");
    CHECK(parsed_shape("<a> &#10;<b/></a>") == "a(' \\n',b)");
  }
  SUBCASE("default: xml:space") {
    CHECK(parsed_shape("<a xml:space='preserve'> <b/> </a>") == "a[xml:space=preserve](' ',b,' ')");
    // Inherited by descendants...
    CHECK(parsed_shape("<a xml:space='preserve'><b> <c/> </b></a>") == "a[xml:space=preserve](b(' ',c,' '))");
    // ...until an element says "default" again.
    CHECK(parsed_shape("<a xml:space='preserve'> <b xml:space='default'> <c/> </b> </a>") ==
          "a[xml:space=preserve](' ',b[xml:space=default](c),' ')");
    CHECK(parsed_shape("<a><b xml:space='preserve'> <c/> </b> <d> <e/> </d></a>") ==
          "a(b[xml:space=preserve](' ',c,' '),d(e))");
    // Any other value changes nothing.
    CHECK(parsed_shape("<a xml:space='keep'> <b/> </a>") == "a[xml:space=keep](b)");
    CHECK(parsed_shape("<a space='preserve'> <b/> </a>") == "a[space=preserve](b)");
  }
  SUBCASE("preserve_whitespace keeps every run") {
    const XmlParseOptions keep = keep_whitespace();
    CHECK(parsed_shape("<a>\n  <b/>\n  <c/>\n</a>", keep) == "a('\\n  ',b,'\\n  ',c,'\\n')");
    CHECK(parsed_shape("<a> <b> <c/> </b> </a>", keep) == "a(' ',b(' ',c,' '),' ')");
    CHECK(parsed_shape("<a xml:space='default'> <b/> </a>", keep) == "a[xml:space=default](' ',b,' ')");
    CHECK(parsed_shape("<a><b> </b></a>", keep) == "a(b(' '))");
  }
  SUBCASE("never a node outside the document element") {
    for (const XmlParseOptions& options : {XmlParseOptions(), keep_whitespace()}) {
      CHECK(parsed_shape(" \n<?xml-like?>\n <!--c-->\n\t<a/> \r\n <!--d--> \n", options) == "?xml-like'',#'c',a,#'d'");
      CHECK(parsed_shape("<?xml version='1.0'?>\n\n<a/>\n", options) == "decl,a");
    }
  }
}

TEST_CASE("xml: character and predefined entity references") {
  CHECK(root_text("<a>&lt;&gt;&amp;&apos;&quot;</a>") == "<>&'\"");
  CHECK(root_text("<a>&#65;&#x42;&#x63;&#0100;&#x00065;</a>") == "ABcde");
  CHECK(root_text("<a>&#xe9;&#xE9;&#233;</a>") == "\xC3\xA9\xC3\xA9\xC3\xA9");
  CHECK(root_text("<a>&#x20AC;&#x1F600;&#x10FFFF;</a>") == "\xE2\x82\xAC\xF0\x9F\x98\x80\xF4\x8F\xBF\xBF");
  CHECK(root_text("<a>&#x9;&#xA;&#xD;&#x20;&#xD7FF;&#xE000;&#xFFFD;&#x10000;</a>") ==
        "\t\n\r \xED\x9F\xBF\xEE\x80\x80\xEF\xBF\xBD\xF0\x90\x80\x80");
  CHECK(root_text("<a>a&amp;b&amp;&amp;c&lt;d</a>") == "a&b&&c<d");
  CHECK(root_text("<a>&amp;lt;&#38;amp;</a>") == "&lt;&amp;");  // decoded once, not twice
  CHECK(root_text("<a>x&#x41;</a>") == "xA");
  CHECK(root_text("<a>&#x41;x</a>") == "Ax");
  // Text around references in one node, positions preserved.
  CHECK(parsed_shape("<a>one &amp; two<b/>&lt;three&gt;</a>") == "a('one & two',b,'<three>')");

  SUBCASE("references to characters XML forbids") {
    for (const std::string_view bad : {"&#0;", "&#x0;", "&#1;", "&#8;", "&#xB;", "&#xC;", "&#x1F;", "&#xD800;",
                                       "&#xDFFF;", "&#xFFFE;", "&#xFFFF;", "&#x110000;", "&#1114112;",
                                       "&#99999999999999999999;", "&#xFFFFFFFFFFFFFFFFFFFF;"}) {
      CAPTURE(bad);
      const std::string text = "<a>" + std::string(bad) + "</a>";
      CHECK(status_of(text) == Status::Decode);
      CHECK(error_of(text).starts_with("invalid XML at line 1, column 4 (byte 3): character reference to U+"));
      CHECK(root_text(text, lenient_text()) == "\xEF\xBF\xBD");
      const std::string attribute = "<a v='" + std::string(bad) + "'/>";
      CHECK(status_of(attribute) == Status::Decode);
      const auto lenient = parse_xml(attribute, lenient_text());
      REQUIRE(lenient);
      CHECK(lenient->root().attribute("v").value() == "\xEF\xBF\xBD");
    }
    CHECK(error_of("<a>&#0;</a>") == at(3, "character reference to U+0000, which is not allowed in XML"));
    CHECK(error_of("<a>&#xD800;</a>") == at(3, "character reference to U+D800, which is not allowed in XML"));
  }

  SUBCASE("malformed references") {
    const std::string reason =
        "malformed reference: '&' must start &name; or &#number; (write &amp; for a literal '&')";
    for (const std::string_view bad : {"&", "& ", "&;", "&amp", "&amp ;", "& amp;", "&#;", "&#x;", "&#xG;", "&#12a;",
                                       "&#-1;", "&# 65;", "&#X41;", "&1a;", "&&amp;", "&#65", "&lt&gt;"}) {
      CAPTURE(bad);
      CHECK(error_of("<a>" + std::string(bad) + "<b/></a>") == at(3, reason));
      CHECK(error_of("<a v=\"" + std::string(bad) + "\"/>") == at(6, reason));
      // Not a matter of unknown names or bad characters: no option accepts it.
      CHECK(status_of("<a>" + std::string(bad) + "<b/></a>", keep_entities()) == Status::Decode);
      CHECK(status_of("<a>" + std::string(bad) + "<b/></a>", lenient_text()) == Status::Decode);
    }
    CHECK(error_of("<a>ok &amp; then & alone</a>") == at(17, reason));
    CHECK(error_of("<a>&amp") == at(7, "unexpected end of input inside a reference"));
    CHECK(error_of("<a>&#x41") == at(8, "unexpected end of input inside a reference"));
  }

  SUBCASE("unknown entities") {
    CHECK(error_of("<a>&nbsp;</a>") == at(3, "undefined entity '&nbsp;'"));
    CHECK(error_of("<a b='x&copy;'/>") == at(7, "undefined entity '&copy;'"));
    CHECK(root_text("<a>a&nbsp;b &copy; &amp;</a>", keep_entities()) == "a&nbsp;b &copy; &");
    const auto kept = parse_xml("<a b='&x.y-z;&lt;'/>", keep_entities());
    REQUIRE(kept);
    CHECK(kept->root().attribute("b").value() == "&x.y-z;<");
    // Kept text is literal text: writing escapes its ampersand.
    CHECK(xml_to_string(*kept, compact()) == "<a b=\"&amp;x.y-z;&lt;\"/>");
  }
}

TEST_CASE("xml: internal entities are expanded within strict limits") {
  SUBCASE("simple replacement text") {
    const auto parsed = parse_xml("<!DOCTYPE d [<!ENTITY who \"World\"><!ENTITY empty ''>\n"
                                  "  <!ENTITY quoted 'say \"hi\"'>]>\n"
                                  "<d a=\"Hello &who;!\" b='&empty;' c='&who;&who;'>Hello &who;&empty;! &quoted; &amp; "
                                  "&who;</d>");
    REQUIRE(parsed);
    const XmlNode d = parsed->root();
    CHECK(d.attribute("a").value() == "Hello World!");
    CHECK(d.attribute("b").value().empty());
    CHECK(d.attribute("c").value() == "WorldWorld");
    CHECK(d.text() == "Hello World! say \"hi\" & World");
    CHECK(shape(d) == "d[a=Hello World! b= c=WorldWorld]('Hello World! say \"hi\" & World')");
    // The declarations stay in the doctype; the text is written expanded.
    CHECK(xml_to_string(d, compact()) ==
          "<d a=\"Hello World!\" b=\"\" c=\"WorldWorld\">Hello World! say \"hi\" &amp; World</d>");
    CHECK(round_trips(*parsed));
  }
  SUBCASE("the existing bomb test: declared entities expand, but only this much") {
    const auto parsed = parse_xml("<!DOCTYPE d [<!ENTITY a \"aaaaaaaaaa\">]><d>&a;&a;</d>");
    REQUIRE(parsed);
    CHECK(parsed->root().text() == "aaaaaaaaaaaaaaaaaaaa");
  }
  SUBCASE("character references and nested entities in replacement text") {
    // &#60; in a declaration would become markup; &#38;#60; becomes a literal '<'.
    CHECK(root_text("<!DOCTYPE d [<!ENTITY lt2 '&#38;#60;'><!ENTITY amp2 '&#38;#38;'><!ENTITY e '&#233;&#x20AC;'>]>"
                    "<d>&lt2;&amp2;&e;</d>") == "<&\xC3\xA9\xE2\x82\xAC");
    CHECK(root_text("<!DOCTYPE d [<!ENTITY a 'A'><!ENTITY b '&a;&a;'><!ENTITY c '[&b;&lt;&b;]'>]><d>&c;</d>") ==
          "[AA<AA]");
    // Entities may be used before the entity they refer to is declared.
    CHECK(root_text("<!DOCTYPE d [<!ENTITY outer '&inner;!'><!ENTITY inner 'in'>]><d>&outer;</d>") == "in!");
    // The first declaration of a name is the binding one.
    CHECK(root_text("<!DOCTYPE d [<!ENTITY e 'first'><!ENTITY e 'second'>]><d>&e;</d>") == "first");
    // The predefined entities cannot be redefined.
    CHECK(root_text("<!DOCTYPE d [<!ENTITY lt 'less'>]><d>&lt;</d>") == "<");
    // Unused declarations cost nothing, whatever they contain.
    CHECK(root_text("<!DOCTYPE d [<!ENTITY loop '&loop;'><!ENTITY m '<b/>'><!ENTITY u '&undefined;'>]><d>x</d>") ==
          "x");
  }
  SUBCASE("nesting depth") {
    const std::string four = "<!DOCTYPE d [<!ENTITY e1 'x'><!ENTITY e2 '&e1;'><!ENTITY e3 '&e2;'><!ENTITY e4 '&e3;'>"
                             "<!ENTITY e5 '&e4;'>]>";
    CHECK(root_text(four + "<d>&e4;</d>") == "x");
    CHECK(status_of(four + "<d>&e5;</d>") == Status::Limit);
    CHECK(error_of(four + "<d>&e5;</d>") ==
          at(four.size() + 3, "entity references nested more than 4 levels deep"));
    CHECK(status_of(four + "<d a='&e5;'/>") == Status::Limit);
    CHECK(status_of("<!DOCTYPE d [<!ENTITY e '&e;'>]><d>&e;</d>") == Status::Limit);
    CHECK(status_of("<!DOCTYPE d [<!ENTITY a '&b;'><!ENTITY b '&a;'>]><d a='&a;'/>") == Status::Limit);
  }
  SUBCASE("expansion budget") {
    const std::string prolog = "<!DOCTYPE d [<!ENTITY e '0123456789'>]>";
    XmlParseOptions options;
    options.max_entity_expansion = 30;
    CHECK(root_text(prolog + "<d>&e;&e;&e;</d>", options) == "012345678901234567890123456789");
    CHECK(status_of(prolog + "<d>&e;&e;&e;&e;</d>", options) == Status::Limit);
    CHECK(error_of(prolog + "<d>&e;&e;&e;&e;</d>", options) ==
          at(prolog.size() + 12, "entity expansion exceeds 30 bytes (max_entity_expansion)"));
    // The budget is for the whole document, attributes included.
    CHECK(status_of(prolog + "<d a='&e;&e;'><b c='&e;'/>&e;</d>", options) == Status::Limit);
    CHECK(status_of(prolog + "<d a='&e;&e;'><b c='&e;'/></d>", options) == Status::Ok);
    options.max_entity_expansion = 0;
    CHECK(status_of(prolog + "<d>&e;</d>", options) == Status::Limit);
    CHECK(status_of(prolog + "<d>&amp;&#65;</d>", options) == Status::Ok);  // not entity expansion
  }
  SUBCASE("billion laughs and quadratic blow-up are refused quickly") {
    std::string laughs = "<?xml version=\"1.0\"?><!DOCTYPE lolz [<!ENTITY lol \"lol\">";
    for (int level = 1; level < 10; ++level) {
      laughs += "<!ENTITY lol" + std::to_string(level) + " \"";
      for (int i = 0; i < 10; ++i) laughs += level == 1 ? "&lol;" : "&lol" + std::to_string(level - 1) + ";";
      laughs += "\">";
    }
    CHECK(status_of(laughs + "]><lolz>&lol9;</lolz>") == Status::Limit);
    CHECK(status_of(laughs + "]><lolz a='&lol9;'/>") == Status::Limit);
    CHECK(status_of(laughs + "]><lolz>&lol9;</lolz>", keep_entities()) == Status::Limit);
    // Three levels are fine (1000 x "lol"), as is the unused rest.
    CHECK(root_text(laughs + "]><lolz>&lol3;</lolz>").size() == 3000u);

    // One large entity referenced very often: 100 KB x 20000 would be 2 GB.
    std::string quadratic = "<!DOCTYPE q [<!ENTITY big \"" + std::string(100'000, 'A') + "\">]><q>";
    for (int i = 0; i < 20'000; ++i) quadratic += "&big;";
    quadratic += "</q>";
    CHECK(status_of(quadratic) == Status::Limit);
    CHECK(error_of(quadratic).ends_with("entity expansion exceeds 1048576 bytes (max_entity_expansion)"));
    // Ten uses are exactly the default budget (1 MiB is 1048576 bytes): refused at the eleventh.
    const std::string ten = "<!DOCTYPE q [<!ENTITY big \"" + std::string(104'857, 'A') + "\">]><q>" +
                            "&big;&big;&big;&big;&big;&big;&big;&big;&big;&big;";
    CHECK(root_text(ten + "</q>").size() == 1'048'570u);
    CHECK(status_of(ten + "&big;</q>") == Status::Limit);
  }
  SUBCASE("entities that are never expanded") {
    const std::string cannot = "is external or contains markup and cannot be expanded";
    // Replacement text with markup.
    const std::string markup = "<!DOCTYPE d [<!ENTITY m '<b>bold</b>'><!ENTITY lt1 '&#60;'><!ENTITY n '&m;'>]>";
    CHECK(error_of(markup + "<d>&m;</d>") == at(markup.size() + 3, "entity '&m;' " + cannot));
    CHECK(error_of(markup + "<d>&lt1;</d>") == at(markup.size() + 3, "entity '&lt1;' " + cannot));
    CHECK(error_of(markup + "<d>&n;</d>") == at(markup.size() + 3, "entity '&m;' " + cannot));
    CHECK(root_text(markup + "<d>&m;|&n;</d>", keep_entities()) == "&m;|&m;");
    // External and unparsed entities: declared, never fetched.
    const std::string external = "<!DOCTYPE d [<!ENTITY sys SYSTEM \"file:///etc/passwd\">"
                                 "<!ENTITY pub PUBLIC \"-//X//Y\" \"http://example.invalid/x\">"
                                 "<!NOTATION gif SYSTEM \"viewer\"><!ENTITY pic SYSTEM \"pic.gif\" NDATA gif>]>";
    for (const std::string_view name : {"sys", "pub", "pic"}) {
      CAPTURE(name);
      const std::string text = external + "<d>&" + std::string(name) + ";</d>";
      CHECK(error_of(text) == at(external.size() + 3, "entity '&" + std::string(name) + ";' " + cannot));
      CHECK(root_text(text, keep_entities()) == "&" + std::string(name) + ";");
      CHECK(status_of(external + "<d a='&" + std::string(name) + ";'/>") == Status::Decode);
    }
    // Parameter entities are recognised and skipped, never expanded; what
    // they would have declared stays undefined.
    const std::string parameter = "<!DOCTYPE d [<!ENTITY % pe '<!ENTITY hidden \"x\">'> %pe; <!ENTITY shown 'y'>]>";
    CHECK(root_text(parameter + "<d>&shown;</d>") == "y");
    CHECK(error_of(parameter + "<d>&hidden;</d>") == at(parameter.size() + 3, "undefined entity '&hidden;'"));
    CHECK(error_of(parameter + "<d>&pe;</d>") == at(parameter.size() + 3, "undefined entity '&pe;'"));
    // An external subset is not read either.
    const std::string external_subset = "<!DOCTYPE d SYSTEM \"http://example.invalid/d.dtd\">";
    CHECK(error_of(external_subset + "<d>&fromdtd;</d>") ==
          at(external_subset.size() + 3, "undefined entity '&fromdtd;'"));
  }
  SUBCASE("broken replacement text is reported where it is used") {
    const std::string prolog = "<!DOCTYPE d [<!ENTITY bad 'AT&#38;T'><!ENTITY ctl '&#38;#1;'>]>";
    CHECK(error_of(prolog + "<d>&bad;</d>") ==
          at(prolog.size() + 3, "malformed reference in the replacement text of an entity"));
    CHECK(error_of(prolog + "<d>&ctl;</d>") ==
          at(prolog.size() + 3, "character reference to U+0001, which is not allowed in XML"));
    CHECK(root_text(prolog + "<d>&ctl;</d>", lenient_text()) == "\xEF\xBF\xBD");
    // In the declaration itself, bad references are errors under every option.
    for (const XmlParseOptions& options : {XmlParseOptions(), lenient_text(), keep_entities()}) {
      const std::string_view bare = "<!DOCTYPE d [<!ENTITY e 'a & b'>]><d/>";
      CHECK(error_of(bare, options) == at(bare.find('&'), "malformed reference in an entity value"));
      const std::string_view control = "<!DOCTYPE d [<!ENTITY e '&#1;'>]><d/>";
      CHECK(error_of(control, options) ==
            at(control.find('&'), "character reference to U+0001, which is not allowed in XML"));
    }
  }
}

TEST_CASE("xml: the doctype is kept as an opaque node") {
  const std::string_view subset = "<!-- a ] and a > in a comment -->\n"
                                  "  <?pi ]> ?>\n"
                                  "  <!ELEMENT note (#PCDATA | b)*>\n"
                                  "  <!ATTLIST note kind (a|b) \"a\" text CDATA \"]>\">\n"
                                  "  <!ENTITY e \"]]>\">\n"
                                  "  <!ENTITY % p '>'>\n"
                                  "  %p;\n"
                                  "  <!NOTATION n PUBLIC \"]\">\n";
  const std::string text = "<!DOCTYPE note PUBLIC \"-//X//DTD > ]//EN\" 'note.dtd' [\n  " + std::string(subset) + "]  "
      ">\n<note/>";
  const auto parsed = parse_xml(text);
  REQUIRE(parsed);
  const XmlNode doctype = parsed->doctype();
  REQUIRE(doctype);
  CHECK(doctype.name() == "note");
  CHECK(doctype.value() == "note PUBLIC \"-//X//DTD > ]//EN\" 'note.dtd' [\n  " + std::string(subset) + "]");
  CHECK(shape(*parsed) == "!note,note");
  // Attribute defaults declared in the DTD are not applied.
  CHECK_FALSE(parsed->root().first_attribute());
  // Written back verbatim.
  CHECK(xml_to_string(*parsed, compact()) == "<!DOCTYPE " + std::string(doctype.value()) + "><note/>");
  CHECK(round_trips(*parsed));

  CHECK(parsed_shape("<!DOCTYPE html><html/>") == "!html,html");
  CHECK(parsed_shape("<!DOCTYPE\n\ta:b\n><a:b/>") == "!a:b,a:b");
  CHECK(parsed_shape("<!--c--><!DOCTYPE a []><?p?><a/>") == "#'c',!a,?p'',a");
  const auto html = parse_xml("<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Strict//EN\"\n"
                              "  \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd\"><html/>");
  REQUIRE(html);
  CHECK(html->doctype().value() == "html PUBLIC \"-//W3C//DTD XHTML 1.0 Strict//EN\"\n"
                                   "  \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd\"");

  // Malformed doctypes.
  CHECK(error_of("<!DOCTYPE><a/>") == at(9, "expected white space after <!DOCTYPE"));
  CHECK(error_of("<!DOCTYPE ><a/>") == at(10, "expected the root element name in DOCTYPE"));
  CHECK(error_of("<!DOCTYPE a [") == at(13, "unterminated DOCTYPE"));
  CHECK(error_of("<!DOCTYPE a") == at(11, "unterminated DOCTYPE"));
  CHECK(error_of("<!DOCTYPE a \"x><a/>") == at(19, "unterminated literal in DOCTYPE"));
  CHECK(error_of("<!DOCTYPE a [<a/>]><a/>") == at(13, "unexpected content in the DOCTYPE internal subset"));
  CHECK(error_of("<!DOCTYPE a [text]><a/>") == at(13, "unexpected content in the DOCTYPE internal subset"));
  CHECK(error_of("<!DOCTYPE a [<![INCLUDE[ ]]>]><a/>") == at(13, "unexpected content in the DOCTYPE internal subset"));
  CHECK(error_of("<!DOCTYPE a [] x><a/>") == at(15, "malformed DOCTYPE"));
  CHECK(error_of("<!DOCTYPE a [] []><a/>") == at(15, "malformed DOCTYPE"));
  CHECK(error_of("<!DOCTYPE a [<!ENTITY>]><a/>") == at(21, "expected white space after <!ENTITY"));
  CHECK(error_of("<!DOCTYPE a [<!ENTITY e>]><a/>") == at(23, "expected white space after the entity name"));
  CHECK(error_of("<!DOCTYPE a [<!ENTITY e x>]><a/>") == at(24, "malformed entity declaration"));
  CHECK(error_of("<!DOCTYPE a [<!ENTITY e 'v' x>]><a/>") ==
        at(28, "expected '>' at the end of the entity declaration"));
  CHECK(error_of("<!DOCTYPE a [<!ENTITY e 'v>]><a/>") == at(33, "unterminated entity value"));
  CHECK(error_of("<!DOCTYPE a [%bad]><a/>") == at(17, "malformed parameter entity reference in DOCTYPE"));
  CHECK(error_of("<!DOCTYPE a [<!-- -- -->]><a/>") == at(18, "'--' is not allowed inside a comment"));
  CHECK(error_of("<!DOCTYPE a [<!ELEMENT a (b <c>]><a/>") == at(28, "malformed declaration in DOCTYPE"));
  CHECK(error_of("<!doctype a><a/>") == at(2, "expected a comment, CDATA section or DOCTYPE after '<!'"));
}

TEST_CASE("xml: encodings are detected and transcoded to UTF-8") {
  const std::string euro = "\xE2\x82\xAC";
  const std::string grin = "\xF0\x9F\x98\x80";

  SUBCASE("UTF-8 with and without byte order mark") {
    CHECK(root_text("<a>" + euro + grin + "</a>") == euro + grin);
    CHECK(root_text("\xEF\xBB\xBF<a>" + euro + "</a>") == euro);
    CHECK(root_text("\xEF\xBB\xBF<?xml version='1.0' encoding='UTF-8'?><a>" + euro + "</a>") == euro);
    CHECK(root_text("<?xml version='1.0' encoding='utf-8'?><a>" + euro + "</a>") == euro);
    CHECK(root_text("<?xml version='1.0' encoding=\"UTF8\"?><a>" + euro + "</a>") == euro);
    // U+FEFF inside the document is an ordinary character.
    CHECK(root_text("<a>\xEF\xBB\xBF</a>") == "\xEF\xBB\xBF");
  }
  SUBCASE("UTF-16 and UTF-32 with byte order marks") {
    const std::u16string wide = u"<?xml version=\"1.0\" encoding=\"UTF-16\"?><a b=\"é\">€\U0001F600</a>";
    for (const bool big : {false, true}) {
      const auto parsed = parse_xml(ByteView(utf16(wide, big, true)));
      REQUIRE(parsed);
      CHECK(parsed->root().text() == euro + grin);
      CHECK(parsed->root().attribute("b").value() == "\xC3\xA9");
      // The declaration keeps the label of the file; the tree is UTF-8.
      CHECK(parsed->declaration().attribute("encoding").value() == "UTF-16");
      CHECK(xml_to_string(*parsed, compact()) == "<a b=\"\xC3\xA9\">" + euro + grin + "</a>");
    }
    const std::u32string wider = U"<a>€\U0001F600</a>";
    for (const bool big : {false, true}) {
      CHECK(root_text(ByteView(utf32(wider, big, true))) == euro + grin);
    }
    // The original test: UTF-16LE with mark, no declaration.
    CHECK(root_text(ByteView(utf16(u"<a>€</a>", false, true))) == euro);
  }
  SUBCASE("UTF-16 and UTF-32 without a mark are recognised by their zero bytes") {
    const std::u16string declared = u"<?xml version='1.0' encoding='UTF-16'?><a>€</a>";
    CHECK(root_text(ByteView(utf16(declared, false, false))) == euro);
    CHECK(root_text(ByteView(utf16(declared, true, false))) == euro);
    CHECK(root_text(ByteView(utf16(u"<a>€</a>", false, false))) == euro);
    CHECK(root_text(ByteView(utf16(u"<a>€</a>", true, false))) == euro);
    CHECK(root_text(ByteView(utf32(U"<a>\U0001F600</a>", false, false))) == grin);
    CHECK(root_text(ByteView(utf32(U"<a>\U0001F600</a>", true, false))) == grin);
  }
  SUBCASE("single-byte encodings named by the declaration") {
    CHECK(root_text("<?xml version='1.0' encoding='ISO-8859-1'?><a>caf\xE9 \xA4 \x85</a>") ==
          "caf\xC3\xA9 \xC2\xA4 \xC2\x85");
    CHECK(root_text("<?xml version=\"1.0\" encoding=\"iso-8859-1\" standalone='yes'?><a>\xFF</a>") == "\xC3\xBF");
    CHECK(root_text("<?xml version='1.0' encoding='latin1'?><a>\xE9</a>") == "\xC3\xA9");
    // Windows-1252 differs from Latin-1 in 0x80..0x9F: 0x80 is the euro sign.
    CHECK(root_text("<?xml version='1.0' encoding='windows-1252'?><a>\x80 \x99 \xE9</a>") ==
          euro + " \xE2\x84\xA2 \xC3\xA9");
    CHECK(root_text("<?xml version='1.0' encoding='Windows-1252'?><a>\x93q\x94</a>") == "\xE2\x80\x9Cq\xE2\x80\x9D");
    CHECK(root_text("<?xml version='1.0' encoding='US-ASCII'?><a>plain &#233;</a>") == "plain \xC3\xA9");
    CHECK(root_text("<?xml version='1.0' encoding='ascii'?><a>plain</a>") == "plain");
    // Names can use the encoding too.
    const auto named = parse_xml("<?xml version='1.0' encoding='ISO-8859-1'?><caf\xE9 cr\xE8me='br\xFBl\xE9""e'/>");
    REQUIRE(named);
    CHECK(named->root().name() == "caf\xC3\xA9");
    CHECK(named->root().attribute("cr\xC3\xA8me").value() == "br\xC3\xBBl\xC3\xA9""e");
  }
  SUBCASE("a byte order mark wins over the declared encoding") {
    CHECK(root_text("\xEF\xBB\xBF<?xml version='1.0' encoding='ISO-8859-1'?><a>\xC3\xA9</a>") == "\xC3\xA9");
    CHECK(root_text(ByteView(utf16(u"<?xml version='1.0' encoding='UTF-8'?><a>é</a>", false, true))) == "\xC3\xA9");
    CHECK(root_text(ByteView(utf16(u"<?xml version='1.0' encoding='Shift_JIS'?><a>x</a>", true, true))) == "x");
  }
  SUBCASE("unsupported and contradictory encodings") {
    for (const std::string_view label : {"Shift_JIS", "ISO-8859-2", "ISO-8859-15", "KOI8-R", "GB2312", "EUC-JP",
                                         "windows-1251", "x-unknown"}) {
      CAPTURE(label);
      const std::string text = "<?xml version='1.0' encoding='" + std::string(label) + "'?><a/>";
      CHECK(status_of(text) == Status::Decode);
      CHECK(error_of(text) == "invalid XML: unsupported encoding '" + std::string(label) + "'");
      CHECK(status_of(text, lenient_text()) == Status::Decode);
    }
    // Declared as UTF-16 but the bytes are not.
    CHECK(error_of("<?xml version='1.0' encoding='UTF-16'?><a/>") ==
          "invalid XML: the declared encoding 'UTF-16' does not match the bytes of the document");
    CHECK(status_of("<?xml version='1.0' encoding='UTF-32LE'?><a/>") == Status::Decode);
    CHECK(error_of("<?xml version='1.0' encoding='bad name'?><a/>") ==
          "invalid XML: malformed encoding name in the XML declaration");
    CHECK(error_of("<?xml version='1.0' encoding='\xFF\xFE'?><a/>") ==
          "invalid XML: malformed encoding name in the XML declaration");
    CHECK(error_of("<?xml version='1.0' encoding=''?><a/>") ==
          at(30, "malformed encoding name in the XML declaration"));
  }
  SUBCASE("invalid byte sequences: an error, or U+FFFD on request") {
    const std::string fffd = "\xEF\xBF\xBD";
    // UTF-8: stray continuation byte, truncated sequence, overlong form, surrogate, beyond U+10FFFF.
    for (const std::string_view bad :
         {"\x80", "\xC3", "\xE2\x82", "\xC0\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xFF"}) {
      const std::string text = "<a>x" + std::string(bad) + "y</a>";
      CHECK(error_of(text) == at(4, "malformed UTF-8 byte sequence"));
      CHECK(status_of(text) == Status::Decode);
      const std::string replaced = root_text(text, lenient_text());
      CHECK(is_valid_utf8(replaced));
      CHECK(replaced.starts_with("x" + fffd));
      CHECK(replaced.ends_with("y"));
    }
    CHECK(root_text("<a>x\xFFy\x80z\xC3</a>", lenient_text()) == "x" + fffd + "y" + fffd + "z" + fffd);
    {
      const auto attribute = parse_xml("<a v='\xFF'/>", lenient_text());
      REQUIRE(attribute);
      CHECK(attribute->root().attribute("v").value() == fffd);
    }

    // UTF-16: a lone surrogate (here the high half without its partner).
    Bytes lone = utf16(u"<a>x", false, true);
    lone.push_back(std::byte{0x3D});
    lone.push_back(std::byte{0xD8});  // U+D83D
    append(lone, ByteView(utf16(u"y</a>", false, false)));
    CHECK(error_of(ByteView(lone)) == "invalid XML at line 1, column 5 (byte 10): malformed UTF-16LE sequence");
    CHECK(root_text(ByteView(lone), lenient_text()) == "x" + fffd + "y");
    // UTF-16 cut in the middle of a unit.
    Bytes odd = utf16(u"<a>x</a>", true, true);
    odd.push_back(std::byte{0x00});
    CHECK(error_of(ByteView(odd)) == "invalid XML at line 1, column 9 (byte 18): malformed UTF-16BE sequence");
    // UTF-32: a value that is not a Unicode scalar.
    Bytes scalar = utf32(U"<a>", false, true);
    append(scalar, ByteView(utf32(std::u32string(1, static_cast<char32_t>(0x110000)), false, false)));
    append(scalar, ByteView(utf32(U"</a>", false, false)));
    CHECK(error_of(ByteView(scalar)) == "invalid XML at line 1, column 4 (byte 16): malformed UTF-32LE sequence");
    CHECK(root_text(ByteView(scalar), lenient_text()) == fffd);

    // US-ASCII: any byte above 0x7F.
    const std::string declaration = "<?xml version='1.0' encoding='US-ASCII'?>";
    CHECK(error_of(declaration + "<a>caf\xC3\xA9</a>") ==
          at(declaration.size() + 6, "byte 0xC3 is not US-ASCII, the declared encoding"));
    CHECK(root_text(declaration + "<a>caf\xC3\xA9</a>", lenient_text()) == "caf" + fffd + fffd);
  }
  SUBCASE("characters XML forbids: an error, or U+FFFD on request") {
    const std::string fffd = "\xEF\xBF\xBD";
    using namespace std::string_view_literals;
    for (const std::string_view bad :
         {"\x00"sv, "\x01"sv, "\x08"sv, "\x0B"sv, "\x0C"sv, "\x1F"sv, "\xEF\xBF\xBE"sv, "\xEF\xBF\xBF"sv}) {
      const std::string text = "<a>" + std::string(bad) + "</a>";
      CHECK(status_of(text) == Status::Decode);
      CHECK(error_of(text).starts_with("invalid XML at line 1, column 4 (byte 3): character U+"));
      CHECK(root_text(text, lenient_text()) == fffd);
      CHECK(status_of("<a v='" + std::string(bad) + "'/>") == Status::Decode);
      CHECK(status_of("<a><!--" + std::string(bad) + "--></a>") == Status::Decode);
      CHECK(status_of("<a><![CDATA[" + std::string(bad) + "]]></a>") == Status::Decode);
      CHECK(status_of("<a><?pi " + std::string(bad) + "?></a>") == Status::Decode);
      CHECK(status_of("<a/>" + std::string(bad)) == Status::Decode);
    }
    CHECK(error_of("<a>\x01</a>") == at(3, "character U+0001 is not allowed in XML"));
    CHECK(error_of(std::string_view("<a>\0</a>", 8)) == at(3, "character U+0000 is not allowed in XML"));
    CHECK(error_of("<a>\xEF\xBF\xBF</a>") == at(3, "character U+FFFF is not allowed in XML"));
    // Tab, LF, CR, DEL, C1 controls and non-characters other than U+FFFE/U+FFFF are legal.
    CHECK(root_text("<a>\t\x7F\xC2\x80\xC2\x9F\xEF\xB7\x90\xF0\x9F\xBF\xBE</a>") ==
          "\t\x7F\xC2\x80\xC2\x9F\xEF\xB7\x90\xF0\x9F\xBF\xBE");
    // An earlier well-formedness error is reported first; a later one is not reached.
    CHECK(error_of("<a></b>\x01") == at(3, "mismatched closing tag </b>, expected </a>"));
    CHECK(error_of("<a>\x01</b>") == at(3, "character U+0001 is not allowed in XML"));
    // Replaced characters still have to form well-formed markup. (U+FFFD
    // itself is a legal name character, so a damaged name stays a name.)
    CHECK(error_of("<a>\x01</b>", lenient_text()) ==
          "invalid XML at line 1, column 5 (byte 4): mismatched closing tag </b>, expected </a>");
    CHECK(parsed_shape("<a\x01/>", lenient_text()) == "a\xEF\xBF\xBD");
  }
  SUBCASE("everything handed out is valid UTF-8") {
    const auto parsed = parse_xml("<a b='\xFF\xFE'>\xC3<![CDATA[\x80]]><!--\xF0\x9F--><?p \xED\xA0\x80?>"
        "</a>", lenient_text());
    REQUIRE(parsed);
    for (const XmlNode node : parsed->node().descendants()) {
      CHECK(is_valid_utf8(node.name()));
      CHECK(is_valid_utf8(node.value()));
      for (const XmlAttribute attribute : node.attributes()) CHECK(is_valid_utf8(attribute.value()));
    }
    CHECK(is_valid_utf8(xml_to_string(*parsed)));
    CHECK(round_trips(*parsed));
  }
}

TEST_CASE("xml: the XML declaration") {
  const auto full = parse_xml("<?xml version = \"1.0\"  encoding='UTF-8'\n standalone=\"no\" ?><a/>");
  REQUIRE(full);
  std::string attributes;
  for (const XmlAttribute a : full->declaration().attributes()) {
    attributes += std::string(a.name()) + "=" + std::string(a.value()) + ";";
  }
  CHECK(attributes == "version=1.0;encoding=UTF-8;standalone=no;");
  CHECK(parsed_shape("<?xml version='1.0'?><a/>") == "decl,a");
  CHECK(parsed_shape("<?xml version='1.1'?><a/>") == "decl,a");  // read as XML 1.0
  CHECK(parsed_shape("<?xml version='1.0' standalone='yes'?><a/>") == "decl,a");

  const std::string_view order = "malformed XML declaration: expected version, encoding, standalone in this order";
  CHECK(error_of("<?xml?><a/>") == at(5, "the XML declaration must start with version=\"1.0\""));
  CHECK(error_of("<?xml ?><a/>") == at(6, "the XML declaration must start with version=\"1.0\""));
  CHECK(error_of("<?xml encoding='UTF-8'?><a/>") == at(6, "the XML declaration must start with version=\"1.0\""));
  CHECK(error_of("<?xml version='2.0'?><a/>") == at(15, "unsupported XML version '2.0'"));
  CHECK(error_of("<?xml version='1'?><a/>") == at(15, "unsupported XML version '1'"));
  CHECK(error_of("<?xml version=''?><a/>") == at(15, "unsupported XML version ''"));
  CHECK(error_of("<?xml version='1.0' standalone='maybe'?><a/>") == at(32, "standalone must be \"yes\" or \"no\""));
  CHECK(error_of("<?xml version='1.0' standalone='yes' encoding='UTF-8'?><a/>") == at(37, order));
  CHECK(error_of("<?xml version='1.0' version='1.0'?><a/>") == at(20, order));
  CHECK(error_of("<?xml version='1.0' bogus='1'?><a/>") == at(20, order));
  CHECK(error_of("<?xml version='1.0'standalone='yes'?><a/>") == at(19, order));
  CHECK(error_of("<?xml version=1.0?><a/>") == at(14, "malformed XML declaration: expected a quoted value"));
  CHECK(error_of("<?xml version?><a/>") == at(13, "malformed XML declaration: expected '='"));
  CHECK(error_of("<?xml version='1.0\"?><a/>") == at(19, "malformed XML declaration: unterminated value"));
  CHECK(error_of("<?xml version='1.0'><a/>") == at(19, order));
  CHECK(error_of("<?xml version='1.0'") == at(19, "unterminated XML declaration"));
  // Only at the very start, and only once.
  const std::string_view misplaced = "the XML declaration is only allowed at the very start of the document";
  CHECK(error_of(" <?xml version='1.0'?><a/>") == at(1, misplaced));
  CHECK(error_of("<?xml version='1.0'?><?xml version='1.0'?><a/>") == at(21, misplaced));
  CHECK(error_of("<a><?xml version='1.0'?></a>") == at(3, misplaced));
  CHECK(error_of("<a/><?xml version='1.0'?>") == at(4, misplaced));
  CHECK(error_of("<?XML version='1.0'?><a/>") == at(0, "the processing instruction target 'xml' is reserved"));
  CHECK(error_of("<a><?xMl?></a>") == at(3, "the processing instruction target 'xml' is reserved"));
  // Names that merely start with "xml" are ordinary processing instructions.
  CHECK(parsed_shape("<?xml-stylesheet href='a.xsl'?><a><?xmlfoo?></a>") ==
        "?xml-stylesheet'href='a.xsl'',a(?xmlfoo'')");
}

TEST_CASE("xml: well-formedness errors carry line, column and byte offset") {
  SUBCASE("structure") {
    CHECK(error_of("") == at(0, "no root element"));
    CHECK(error_of("   \n") == "invalid XML at line 2, column 1 (byte 4): no root element");
    CHECK(error_of("<!-- only a comment -->") == at(23, "no root element"));
    CHECK(error_of("plain text") == at(0, "character data outside the root element"));
    CHECK(error_of("<a/>trailing") == at(4, "character data outside the root element"));
    CHECK(error_of("<a/>&amp;") == at(4, "character data outside the root element"));
    CHECK(error_of("<a/><b/>") == at(4, "more than one root element"));
    CHECK(error_of("<a></a><a></a>") == at(7, "more than one root element"));
    CHECK(error_of("<a>") == at(3, "unexpected end of input: <a> is not closed"));
    CHECK(error_of("<a><b></b>") == at(10, "unexpected end of input: <a> is not closed"));
    CHECK(error_of("<a><b>text") == at(10, "unexpected end of input: <b> is not closed"));
    CHECK(error_of("<a></b>") == at(3, "mismatched closing tag </b>, expected </a>"));
    CHECK(error_of("<a><b></a></b>") == at(6, "mismatched closing tag </a>, expected </b>"));
    CHECK(error_of("<a></ab>") == at(3, "mismatched closing tag </ab>, expected </a>"));
    CHECK(error_of("<ab></a>") == at(4, "mismatched closing tag </a>, expected </ab>"));
    CHECK(error_of("<a></A>") == at(3, "mismatched closing tag </A>, expected </a>"));
    CHECK(error_of("<p:a></q:a>") == at(5, "mismatched closing tag </q:a>, expected </p:a>"));
    CHECK(error_of("</a>") == at(0, "closing tag without a matching opening tag"));
    CHECK(error_of("<a/></a>") == at(4, "closing tag without a matching opening tag"));
    CHECK(error_of("<a></a") == at(6, "unexpected end of input: expected '>' at the end of the closing tag"));
    CHECK(error_of("<a></a b='1'>") == at(7, "expected '>' at the end of the closing tag"));
    CHECK(error_of("<a></>") == at(5, "expected an element name after '</'"));
    CHECK(error_of("<a></ a>") == at(5, "expected an element name after '</'"));
    CHECK(error_of("<![CDATA[x]]><a/>") == at(0, "CDATA section outside the root element"));
    CHECK(error_of("<a/><![CDATA[x]]>") == at(4, "CDATA section outside the root element"));
    CHECK(error_of("<!DOCTYPE a><!DOCTYPE a><a/>") == at(12, "more than one DOCTYPE"));
    CHECK(error_of("<a/><!DOCTYPE a>") == at(4, "DOCTYPE must come before the root element"));
    CHECK(error_of("<a><!DOCTYPE a></a>") == at(3, "DOCTYPE must come before the root element"));
  }
  SUBCASE("tags and attributes") {
    CHECK(error_of("<") == at(1, "unexpected end of input: expected an element name after '<'"));
    CHECK(error_of("<>") == at(1, "expected an element name after '<'"));
    CHECK(error_of("< a/>") == at(1, "expected an element name after '<'"));
    CHECK(error_of("<1a/>") == at(1, "expected an element name after '<'"));
    CHECK(error_of("<-a/>") == at(1, "expected an element name after '<'"));
    CHECK(error_of("<a><.b/></a>") == at(4, "expected an element name after '<'"));
    CHECK(error_of("<a>1 < 2</a>") == at(6, "expected an element name after '<'"));
    CHECK(error_of("<a") == at(2, "unexpected end of input in the start tag <a>"));
    CHECK(error_of("<a b='1' ") == at(9, "unexpected end of input in the start tag <a>"));
    CHECK(error_of("<a,b/>") == at(2, "invalid character in an element name"));
    CHECK(error_of("<a\xC3\x97/>") == at(2, "invalid character in an element name"));  // U+00D7 is not a name character
    CHECK(error_of("<a/ >") == at(3, "expected '>' after '/' in a start tag"));
    CHECK(error_of("<a/") == at(3, "unexpected end of input: expected '>' after '/' in a start tag"));
    CHECK(error_of("<a b/>") == at(4, "expected '=' after the attribute name 'b'"));
    CHECK(error_of("<a b>") == at(4, "expected '=' after the attribute name 'b'"));
    CHECK(error_of("<a b c='1'/>") == at(5, "expected '=' after the attribute name 'b'"));
    CHECK(error_of("<a attr=></a>") == at(8, "the value of attribute 'attr' must be quoted"));
    CHECK(error_of("<a b=c/>") == at(5, "the value of attribute 'b' must be quoted"));
    CHECK(error_of("<a b=") == at(5, "unexpected end of input: the value of attribute 'b' must be quoted"));
    CHECK(error_of("<a b=\"c/>") == at(9, "unterminated attribute value"));
    CHECK(error_of("<a b='c\"/>") == at(10, "unterminated attribute value"));
    CHECK(error_of("<a b=\"x<y\"/>") == at(7, "'<' is not allowed in an attribute value (write &lt;)"));
    CHECK(error_of("<a b='1'c='2'/>") == at(8, "expected white space before the attribute name"));
    CHECK(error_of("<a b='1' 2c='2'/>") == at(9, "expected an attribute name, '>' or '/>'"));
    CHECK(error_of("<a ='1'/>") == at(3, "expected an attribute name, '>' or '/>'"));
    CHECK(error_of("<a b='1' b='2'/>") == at(9, "duplicate attribute 'b'"));
    CHECK(error_of("<a b='1' c='2' d='3' c=\"4\"/>") == at(21, "duplicate attribute 'c'"));
    CHECK(error_of("<a p:b='1' p:b='2'/>") == at(11, "duplicate attribute 'p:b'"));
    CHECK(status_of("<a b='1' B='2' p:b='3' q:b='4'/>") == Status::Ok);  // different names
  }
  SUBCASE("comments, CDATA sections and processing instructions") {
    CHECK(error_of("<a><!-- x -- y --></a>") == at(10, "'--' is not allowed inside a comment"));
    CHECK(error_of("<a><!-- x ---></a>") == at(10, "'--' is not allowed inside a comment"));
    CHECK(error_of("<!---><a/>") == at(10, "unterminated comment"));  // "<!--" followed by "-><a/>"
    CHECK(error_of("<a><!-- x") == at(9, "unterminated comment"));
    CHECK(error_of("<a><!-- x --") == at(12, "unterminated comment"));
    CHECK(error_of("<a><!-- x -></a>") == at(16, "unterminated comment"));
    CHECK(error_of("<a><!- x --></a>") == at(5, "expected a comment, CDATA section or DOCTYPE after '<!'"));
    CHECK(error_of("<a><!x></a>") == at(5, "expected a comment, CDATA section or DOCTYPE after '<!'"));
    CHECK(error_of("<a><![cdata[x]]></a>") == at(5, "expected a comment, CDATA section or DOCTYPE after '<!'"));
    CHECK(error_of("<a><![CDATA[x</a>") == at(17, "unterminated CDATA section"));
    CHECK(error_of("<a><![CDATA[x]]</a>") == at(19, "unterminated CDATA section"));
    CHECK(error_of("<a><?pi x</a>") == at(13, "unterminated processing instruction"));
    CHECK(error_of("<a><?pi x? ></a>") == at(16, "unterminated processing instruction"));
    CHECK(error_of("<a><?" "?></a>") == at(5, "expected a processing instruction target after '<?'"));
    CHECK(error_of("<a><? pi?></a>") == at(5, "expected a processing instruction target after '<?'"));
    CHECK(error_of("<a><?pi+x?></a>") == at(7, "expected white space after the processing instruction target"));
    // "]]>" in character data is tolerated (a documented leniency).
    CHECK(root_text("<a>x]]>y]]></a>") == "x]]>y]]>");
  }
  SUBCASE("lines and columns") {
    CHECK(error_of("<a>\n  <b>\n    text\n  </c>\n</a>") ==
          "invalid XML at line 4, column 3 (byte 21): mismatched closing tag </c>, expected </b>");
    // CRLF and lone CR count as one line break each; offsets are in the original bytes.
    CHECK(error_of("<a>\r\n<b>\r\n</c></a>") ==
          "invalid XML at line 3, column 1 (byte 10): mismatched closing tag </c>, expected </b>");
    CHECK(error_of("<a>\r<b>\r</c></a>") ==
          "invalid XML at line 3, column 1 (byte 8): mismatched closing tag </c>, expected </b>");
    CHECK(error_of("<a>\r\n\r\n\r\n  &bogus;</a>") ==
          "invalid XML at line 4, column 3 (byte 11): undefined entity '&bogus;'");
    // Columns count characters, byte offsets count bytes.
    CHECK(error_of("<a>\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80</b>") ==
          "invalid XML at line 1, column 7 (byte 12): mismatched closing tag </b>, expected </a>");
    CHECK(error_of("<a>\n\xE4\xB8\xAD\xE6\x96\x87 &x;</a>") ==
          "invalid XML at line 2, column 4 (byte 11): undefined entity '&x;'");
    // In-place decoding before the error does not shift the reported position.
    CHECK(error_of("<a b='&lt;&amp;&#x41;'>&amp;&amp;\r\n&lt;</b>") ==
          "invalid XML at line 2, column 5 (byte 39): mismatched closing tag </b>, expected </a>");
    // A byte order mark is counted in the byte offset but is not a column.
    CHECK(error_of("\xEF\xBB\xBF<a></b>") ==
          "invalid XML at line 1, column 4 (byte 6): mismatched closing tag </b>, expected </a>");
    // Transcoded input: the offset is in the original bytes.
    CHECK(error_of(ByteView(utf16(u"<a>\n€\U0001F600</b>", false, true))) ==
          "invalid XML at line 2, column 3 (byte 16): mismatched closing tag </b>, expected </a>");
    CHECK(error_of(ByteView(utf16(u"<a></b>", true, false))) ==
          "invalid XML at line 1, column 4 (byte 6): mismatched closing tag </b>, expected </a>");
    CHECK(error_of(ByteView(utf32(U"<a>\U0001F600</b>", true, true))) ==
          "invalid XML at line 1, column 5 (byte 20): mismatched closing tag </b>, expected </a>");
    CHECK(error_of("<?xml version='1.0' encoding='ISO-8859-1'?>\n<a>\xE9\xE9</b>") ==
          "invalid XML at line 2, column 6 (byte 49): mismatched closing tag </b>, expected </a>");
    // Lenient decoding lengthens the text internally; positions still refer to the input.
    CHECK(error_of("<a>\xFF\xFF\x01</b>", lenient_text()) ==
          "invalid XML at line 1, column 7 (byte 6): mismatched closing tag </b>, expected </a>");
    // Long names are clipped in messages.
    const std::string longer(200, 'n');
    CHECK(error_of("<a></" + longer + ">") ==
          at(3, "mismatched closing tag </" + std::string(48, 'n') + "...>, expected </a>"));
  }
  SUBCASE("the original rejection list: nothing half-parsed is returned") {
    for (const std::string_view bad : {"", "<a>", "<a></b>", "plain text", "<a attr=></a>"}) {
      const auto result = parse_xml(bad);
      CHECK_MESSAGE(!result, "should reject: ", bad);
      if (!result) CHECK(result.error().code == Status::Decode);
    }
  }
}

TEST_CASE("xml: names follow the fifth edition of XML 1.0") {
  CHECK(xml_is_name("a"));
  CHECK(xml_is_name("_a-b.c1"));
  CHECK(xml_is_name(":a"));
  CHECK(xml_is_name("w:p"));
  CHECK(xml_is_name("A9"));
  CHECK(xml_is_name("caf\xC3\xA9"));
  CHECK(xml_is_name("\xE4\xB8\xAD\xE6\x96\x87"));
  CHECK(xml_is_name("a\xC2\xB7""b"));          // U+00B7 middle dot: name character
  CHECK(xml_is_name("a\xCC\x81"));             // U+0301 combining mark: name character
  CHECK(xml_is_name("\xE2\x85\xA0"));          // U+2160: a name start character since the fifth edition
  CHECK(xml_is_name("\xF0\x90\x80\x80"));      // U+10000
  CHECK(xml_is_name("\xF3\xAF\xBF\xBF"));      // U+EFFFF
  CHECK_FALSE(xml_is_name(""));
  CHECK_FALSE(xml_is_name("1a"));
  CHECK_FALSE(xml_is_name("-a"));
  CHECK_FALSE(xml_is_name(".a"));
  CHECK_FALSE(xml_is_name("a b"));
  CHECK_FALSE(xml_is_name("a<"));
  CHECK_FALSE(xml_is_name("a\t"));
  CHECK_FALSE(xml_is_name("\xC2\xB7""a"));     // may not start a name
  CHECK_FALSE(xml_is_name("\xCC\x81""a"));
  CHECK_FALSE(xml_is_name("a\xC3\x97"));       // U+00D7 multiplication sign
  CHECK_FALSE(xml_is_name("a\xC3\xB7"));       // U+00F7 division sign
  CHECK_FALSE(xml_is_name("a\xE2\x80\x8B"));   // U+200B
  CHECK_FALSE(xml_is_name("a\xCD\xBE"));       // U+037E Greek question mark
  CHECK_FALSE(xml_is_name("\xF3\xB0\x80\x80")); // U+F0000
  CHECK_FALSE(xml_is_name("a\xFF"));           // not UTF-8
  CHECK_FALSE(xml_is_name(std::string_view("a\0b", 3)));

  // The parser applies the same rules to elements, attributes and targets.
  CHECK(parsed_shape("<\xE2\x85\xA0 \xCE\xB1\xCC\x81='1'><?\xE4\xB8\xAD x?></\xE2\x85\xA0>") ==
        "\xE2\x85\xA0[\xCE\xB1\xCC\x81=1](?\xE4\xB8\xAD'x')");
  CHECK(status_of("<a\xC2\xB7""b/>") == Status::Ok);
  CHECK(status_of("<\xC2\xB7""a/>") == Status::Decode);
  CHECK(status_of("<a \xC3\x97='1'/>") == Status::Decode);
  CHECK(status_of("<a><?\xCC\x81 x?></a>") == Status::Decode);

  CHECK(xml_local_name("w:p") == "p");
  CHECK(xml_local_name("p").size() == 1u);
  CHECK(xml_local_name("a:b:c") == "c");
  CHECK(xml_local_name("").empty());
  CHECK(xml_prefix("w:p") == "w");
  CHECK(xml_prefix("p").empty());
  CHECK(xml_prefix("a:b:c") == "a:b");
  static_assert(xml_local_name("svg:rect") == "rect");
  static_assert(xml_prefix("svg:rect") == "svg");
}

TEST_CASE("xml: limits") {
  SUBCASE("max_depth") {
    XmlParseOptions options;
    options.max_depth = 3;
    CHECK(status_of("<a><b><c/></b></a>", options) == Status::Ok);
    CHECK(status_of("<a><b><c>x</c><c><!--3--></c></b><b/></a>", options) == Status::Ok);
    CHECK(status_of("<a><b><c><d/></c></b></a>", options) == Status::Limit);
    CHECK(error_of("<a><b><c><d/></c></b></a>", options) ==
          at(9, "elements nested more than 3 levels deep (max_depth)"));
    options.max_depth = 0;
    CHECK(status_of("<a/>", options) == Status::Limit);

    // The default is 1024 levels.
    const auto nested = [](int levels) {
      std::string text;
      for (int i = 0; i < levels; ++i) text += "<n>";
      text += "x";
      for (int i = 0; i < levels; ++i) text += "</n>";
      return text;
    };
    CHECK(root_text(nested(1024)) == "x");
    CHECK(status_of(nested(1025)) == Status::Limit);
    CHECK(error_of(nested(1025)) == at(3 * 1024, "elements nested more than 1024 levels deep (max_depth)"));
  }
  SUBCASE("max_nodes") {
    XmlParseOptions options;
    options.max_nodes = 5;
    CHECK(status_of("<a><b/>text<!--c--><?pi?></a>", options) == Status::Ok);  // exactly five
    CHECK(status_of("<a><b/>text<!--c--><?pi?><![CDATA[x]]></a>", options) == Status::Limit);
    CHECK(error_of("<a><b/><b/><b/><b/><b/></a>", options) == at(19, "more than 5 nodes (max_nodes)"));
    // Declaration and doctype are nodes too; attributes are not.
    CHECK(status_of("<?xml version='1.0'?><!DOCTYPE a><a x='1' y='2'><b/><b/></a>", options) == Status::Ok);
    CHECK(status_of("<?xml version='1.0'?><!DOCTYPE a><a><b/><b/><b/></a>", options) == Status::Limit);
    // Dropped white space does not count, kept white space does.
    CHECK(status_of("<a> <b/> <b/> <b/> <b/> </a>", options) == Status::Ok);
    XmlParseOptions keep = options;
    keep.preserve_whitespace = true;
    CHECK(status_of("<a> <b/> <b/> <b/> <b/> </a>", keep) == Status::Limit);
    CHECK(status_of("<a> <b/> <b/>x</a>", options) == Status::Limit);  // the text makes the earlier runs significant
    options.max_nodes = 0;
    CHECK(status_of("<a/>", options) == Status::Limit);
  }
  SUBCASE("max_attribute_count_per_element") {
    XmlParseOptions options;
    options.max_attribute_count_per_element = 3;
    CHECK(status_of("<a x='1' y='2' z='3'><b p='1' q='2' r='3'/></a>", options) == Status::Ok);
    CHECK(status_of("<a x='1' y='2' z='3' w='4'/>", options) == Status::Limit);
    CHECK(error_of("<a x='1' y='2' z='3' w='4'/>", options) ==
          at(21, "more than 3 attributes on one element (max_attribute_count_per_element)"));
    options.max_attribute_count_per_element = 0;
    CHECK(status_of("<a/>", options) == Status::Ok);
    CHECK(status_of("<a x='1'/>", options) == Status::Limit);
  }
  SUBCASE("limit errors are Status::Limit, everything else Status::Decode") {
    CHECK(status_of("<a>") == Status::Decode);
    CHECK(status_of("<a>&undefined;</a>") == Status::Decode);
    CHECK(status_of("<?xml version='1.0' encoding='EBCDIC'?><a/>") == Status::Decode);
  }
}

TEST_CASE("xml: many attributes on one element") {
  const auto element = [](int count, std::string_view extra = {}) {
    std::string text = "<a";
    for (int i = 0; i < count; ++i) text += " n" + std::to_string(i) + "=\"" + std::to_string(i) + "\"";
    text += std::string(extra) + "/>";
    return text;
  };
  // Just inside and just outside the default limit of 100000.
  {
    const auto parsed = parse_xml(element(100'000));
    REQUIRE(parsed);
    CHECK(parsed->root().attribute("n0").as_int(-1) == 0);
    CHECK(parsed->root().attribute("n99999").as_int() == 99999);
    std::size_t count = 0;
    for (const XmlAttribute a : parsed->root().attributes()) count += a ? 1 : 0;
    CHECK(count == 100'000u);
  }
  CHECK(status_of(element(100'001)) == Status::Limit);
  CHECK(error_of(element(100'001))
            .ends_with("more than 100000 attributes on one element (max_attribute_count_per_element)"));

  // Duplicates are found wherever they are, and the later occurrence is reported.
  const std::string many = element(50'000);
  CHECK(error_of(element(50'000, " n77=\"again\"")) == at(many.size() - 2 + 1, "duplicate attribute 'n77'"));
  CHECK(error_of(element(50'000, " n49999=''")) == at(many.size() - 2 + 1, "duplicate attribute 'n49999'"));
  CHECK(error_of(element(9, " n0=''")) == at(element(9).size() - 2 + 1, "duplicate attribute 'n0'"));
  CHECK(error_of(element(10, " n9=''")) == at(element(10).size() - 2 + 1, "duplicate attribute 'n9'"));
  {
    // Two duplicated names: the one that repeats first in the document is reported.
    std::string text = "<a";
    for (int i = 0; i < 30; ++i) text += " n" + std::to_string(i) + "='1'";
    const std::size_t first_repeat = text.size() + 1;
    text += " n20='x' n3='y'/>";
    CHECK(error_of(text) == at(first_repeat, "duplicate attribute 'n20'"));
  }
  {
    // 100000 copies of the same attribute: rejected at the second one.
    std::string text = "<a";
    for (int i = 0; i < 100'000; ++i) text += " b='v'";
    text += "/>";
    CHECK(error_of(text) == at(9, "duplicate attribute 'b'"));
  }
}

TEST_CASE("xml: deeply nested documents never recurse") {
  const int levels = 200'000;
  std::string deep;
  deep.reserve(static_cast<std::size_t>(levels) * 7 + 1);
  for (int i = 0; i < levels; ++i) deep += "<n>";
  deep += "x";
  for (int i = 0; i < levels; ++i) deep += "</n>";

  // The original expectation was 20000 levels with default options; nesting
  // is now limited by default and unlimited on request.
  CHECK(status_of(deep) == Status::Limit);
  XmlParseOptions options;
  options.max_depth = static_cast<std::size_t>(levels);
  auto parsed = parse_xml(deep, options);
  REQUIRE(parsed);
  XmlNode root = parsed->root();
  CHECK(root.text() == "x");
  CHECK(parsed->node().text() == "x");
  CHECK(xml_to_string(*parsed, compact()).size() == deep.size());
  CHECK(xml_to_string(*parsed, compact()) == deep);

  // Indentation stops growing at 64 levels, so pretty printing stays linear.
  const std::string pretty = xml_to_string(*parsed, pretty_without_declaration());
  CHECK(pretty.size() < deep.size() + static_cast<std::size_t>(levels) * 2 * (64 * 2 + 1) + 16);
  const auto reparsed = parse_xml(pretty, options);
  REQUIRE(reparsed);
  CHECK(deep_equal(*parsed, *reparsed));

  std::size_t count = 0;
  XmlNode innermost;
  for (const XmlNode node : parsed->node().descendants()) {
    ++count;
    innermost = node;
  }
  CHECK(count == static_cast<std::size_t>(levels) + 1);
  CHECK(innermost.value() == "x");
  std::size_t depth = 0;
  for (XmlNode node = innermost; node; node = node.parent()) ++depth;
  CHECK(depth == static_cast<std::size_t>(levels) + 2);
  CHECK(root.find("n/n/n/n"));
  CHECK(innermost.parent().lookup_namespace("p").empty());

  XmlDocument copy = parsed->clone();
  CHECK(deep_equal(copy, *parsed));
  CHECK(copy.root().text() == "x");
  // Building, mutating and destroying deep trees is iterative as well.
  XmlNode tip = innermost.parent();
  for (int i = 0; i < 100; ++i) tip = tip.append_child("m");
  CHECK_FALSE(deep_equal(copy, *parsed));
  root.clear_children();
  CHECK(xml_to_string(*parsed, compact()) == "<n/>");
  parsed = fail(Status::Internal, "dropped");  // destroys the document
  CHECK(copy.root().text() == "x");

  // One million unclosed elements with default options: refused at level 1025.
  std::string unclosed;
  for (int i = 0; i < 1'000'000; ++i) unclosed += "<n>";
  CHECK(error_of(unclosed) == at(3 * 1024, "elements nested more than 1024 levels deep (max_depth)"));
}

// ---------------------------------------------------------------------------
// Building and changing
// ---------------------------------------------------------------------------

TEST_CASE("xml: building a document") {
  XmlDocument document;
  CHECK_FALSE(document.root());
  XmlNode root = document.append_child("library");
  REQUIRE(root);
  CHECK(document.root() == root);
  CHECK(root.parent() == document.node());

  XmlNode second = root.append_child("book");
  XmlNode first = root.prepend_child("book");
  XmlNode third = root.insert_child_after("book", second);
  XmlNode between = root.insert_child_before("shelf", second);
  XmlNode last = root.insert_child_after("end", third);
  XmlNode zeroth = root.insert_child_before("start", first);
  first.set_attribute("n", 1);
  second.set_attribute("n", 2);
  third.set_attribute("n", 3);
  CHECK(shape(root) == "library(start,book[n=1],shelf,book[n=2],book[n=3],end)");
  CHECK(zeroth == root.first_child());
  CHECK(last == root.last_child());
  CHECK(between.next_sibling() == second);
  CHECK(second.previous_sibling() == between);

  first.append_text("Plain & <simple>");
  second.append_cdata("x < y && y > z");
  second.append_comment(" note ");
  second.append_pi("render", "fast");
  second.append_pi("flag");
  third.append_text("a");
  third.append_child("i").append_text("b");
  third.append_text("c");
  CHECK(shape(root) == "library(start,book[n=1]('Plain & <simple>'),shelf,"
                       "book[n=2](cdata'x < y && y > z',#' note "
                       "',?render'fast',?flag''),book[n=3]('a',i('b'),'c'),end)");
  CHECK(xml_to_string(document) == "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                   "<library>\n"
                                   "  <start/>\n"
                                   "  <book n=\"1\">Plain &amp; &lt;simple&gt;</book>\n"
                                   "  <shelf/>\n"
                                   "  <book n=\"2\"><![CDATA[x < y && y > z]]><!-- note --><?render fast?><?flag?>"
                                   "</book>\n"
                                   "  <book n=\"3\">a<i>b</i>c</book>\n"
                                   "  <end/>\n"
                                   "</library>\n");
  CHECK(round_trips(document));

  SUBCASE("renaming and replacing values") {
    CHECK(between.set_name("case"));
    CHECK(between.name() == "case");
    CHECK(between.set_name("a-much-longer-name:than.before"));
    CHECK(between.name() == "a-much-longer-name:than.before");
    CHECK(between.set_name("s"));
    CHECK(between.name() == "s");
    XmlNode text = first.first_child();
    CHECK(text.set_value("short"));
    CHECK(text.value() == "short");
    CHECK(text.set_value("a considerably longer piece of text than the node held before"));
    CHECK(first.text() == "a considerably longer piece of text than the node held before");
    CHECK(text.set_value(text.value().substr(2, 12)));  // a view into the value being replaced
    CHECK(text.value() == "considerably");
    CHECK(text.set_value(""));
    CHECK(text.value().empty());
    XmlNode cdata = second.first_child();
    CHECK(cdata.set_value("]]>"));
    CHECK(cdata.value() == "]]>");
    XmlNode comment = cdata.next_sibling();
    CHECK(comment.set_value("changed"));
    XmlNode pi = comment.next_sibling();
    CHECK(pi.set_value("slow"));
    CHECK(pi.set_name("draw"));
    CHECK(shape(second) == "book[n=2](cdata']]>',#'changed',?draw'slow',?flag'')");
    // Elements and documents have no value of their own.
    CHECK_FALSE(root.set_value("x"));
    CHECK_FALSE(document.node().set_value("x"));
    CHECK_FALSE(text.set_name("x"));
    CHECK_FALSE(document.node().set_name("x"));
  }
  SUBCASE("set_text replaces the content of an element") {
    CHECK(third.set_text("only"));
    CHECK(shape(third) == "book[n=3]('only')");
    CHECK(third.set_text(third.first_child().value().substr(1)));  // aliasing the child being replaced
    CHECK(shape(third) == "book[n=3]('nly')");
    CHECK(third.set_text(""));
    CHECK(shape(third) == "book[n=3]");
    CHECK(zeroth.set_text("fresh"));
    CHECK(zeroth.text() == "fresh");
    CHECK_FALSE(first.first_child().set_text("x"));
  }
  SUBCASE("removing") {
    CHECK(root.remove_child(between));
    CHECK_FALSE(root.remove_child(between));  // no longer a child
    CHECK_FALSE(root.remove_child(third.first_child()));  // a grandchild
    CHECK_FALSE(root.remove_child(root));
    CHECK(shape(root).starts_with("library(start,book[n=1]('Plain & <simple>'),book[n=2]"));
    // The removed node stays readable but is cut off from the document.
    CHECK(between.name() == "shelf");
    CHECK_FALSE(between.parent());
    CHECK_FALSE(between.next_sibling());
    CHECK_FALSE(between.append_child("x"));
    CHECK_FALSE(between.set_attribute("a", "b"));
    CHECK_FALSE(between.set_name("y"));
    CHECK_FALSE(root.insert_child_before("z", between));
    CHECK(root.remove_child(root.first_child()));
    CHECK(root.remove_child(root.last_child()));
    CHECK(shape(root).starts_with("library(book[n=1]"));
    CHECK(shape(root).ends_with("book[n=3]('a',i('b'),'c'))"));
    third.clear_children();
    CHECK(shape(third) == "book[n=3]");
    CHECK_FALSE(third.first_child());
    CHECK_FALSE(third.last_child());
    third.clear_children();
    third.append_text("again");
    CHECK(shape(third) == "book[n=3]('again')");
    root.clear_children();
    CHECK(xml_to_string(document, compact()) == "<library/>");
    CHECK(round_trips(document));
  }
}

TEST_CASE("xml: setting attributes") {
  XmlDocument document;
  XmlNode e = document.append_child("e");
  CHECK(e.set_attribute("text", "plain").value() == "plain");
  CHECK(e.set_attribute("string", std::string("owned")).value() == "owned");
  CHECK(e.set_attribute("view", std::string_view("viewed")).value() == "viewed");
  CHECK(e.set_attribute("empty", "").value().empty());
  CHECK(e.set_attribute("i", -42).value() == "-42");
  CHECK(e.set_attribute("u", 4000000000u).value() == "4000000000");
  CHECK(e.set_attribute("short", static_cast<short>(-7)).value() == "-7");
  CHECK(e.set_attribute("byte", static_cast<unsigned char>(200)).value() == "200");
  CHECK(e.set_attribute("size", std::size_t{7}).value() == "7");
  CHECK(e.set_attribute("min", std::numeric_limits<std::int64_t>::min()).value() == "-9223372036854775808");
  CHECK(e.set_attribute("max", std::numeric_limits<std::uint64_t>::max()).value() == "18446744073709551615");
  CHECK(e.set_attribute("d", 0.1).value() == "0.1");
  CHECK(e.set_attribute("third", 1.0 / 3.0).value() == "0.3333333333333333");
  CHECK(e.set_attribute("whole", 100.0).value() == "100");
  CHECK(e.set_attribute("large", 1e30).value() == "1e+30");
  CHECK(e.set_attribute("small", -2.5e-7).value() == "-2.5e-07");
  CHECK(e.set_attribute("f", 0.1f).value() == "0.1");
  CHECK(e.set_attribute("half", 2.5f).value() == "2.5");
  CHECK(e.set_attribute("nan", std::numeric_limits<double>::quiet_NaN()).value() == "nan");
  CHECK(e.set_attribute("inf", -std::numeric_limits<double>::infinity()).value() == "-inf");
  CHECK(e.set_attribute("yes", true).value() == "true");
  CHECK(e.set_attribute("no", false).value() == "false");
  // What was written reads back exactly.
  CHECK(e.attribute("third").as_double() == 1.0 / 3.0);
  CHECK(e.attribute("min").as_int64() == std::numeric_limits<std::int64_t>::min());
  CHECK(e.attribute("max").as_uint64() == std::numeric_limits<std::uint64_t>::max());
  CHECK(e.attribute("small").as_double() == -2.5e-7);
  CHECK(std::isnan(e.attribute("nan").as_double()));
  CHECK(e.attribute("yes").as_bool());
  CHECK_FALSE(e.attribute("no").as_bool(true));

  XmlDocument other;
  XmlNode o = other.append_child("o");
  o.set_attribute("b", "1");
  o.set_attribute("a", "2");
  o.set_attribute("c", "3");
  // Replacing keeps the position; new attributes go last.
  const XmlAttribute a = o.attribute("a");
  CHECK(o.set_attribute("a", "replaced") == a);
  CHECK(o.set_attribute("a", "x") == a);
  CHECK(o.set_attribute("a", "a longer value than before") == a);
  o.set_attribute("d", 4);
  CHECK(shape(o) == "o[b=1 a=a longer value than before c=3 d=4]");
  CHECK(o.attribute("c").set_value("three"));
  CHECK(o.attribute("c").set_value(3.5));
  CHECK(o.attribute("d").set_value(false));
  CHECK(o.attribute("b").set_value(o.attribute("a").value().substr(2, 6)));
  CHECK(shape(o) == "o[b=longer a=a longer value than before c=3.5 d=false]");

  CHECK(o.remove_attribute("a"));
  CHECK_FALSE(o.remove_attribute("a"));
  CHECK_FALSE(a.parent());  // the removed handle is detached
  CHECK_FALSE(XmlAttribute(a).set_value("x"));
  CHECK(shape(o) == "o[b=longer c=3.5 d=false]");
  CHECK(o.remove_attribute(o.attribute("d")));
  CHECK(o.remove_attribute(o.first_attribute()));
  CHECK(shape(o) == "o[c=3.5]");
  CHECK_FALSE(o.remove_attribute(e.attribute("i")));  // an attribute of another element
  CHECK(o.remove_attribute("c"));
  CHECK(shape(o) == "o");
  CHECK_FALSE(o.first_attribute());
  o.set_attribute("z", "back");
  CHECK(xml_to_string(o, compact()) == "<o z=\"back\"/>");

  // Namespace declarations are ordinary attributes.
  o.set_attribute("xmlns:x", "urn:x");
  o.set_attribute("x:id", "7");
  CHECK(o.attribute("x:id").namespace_uri() == "urn:x");
  CHECK(o.append_child("x:kid").namespace_uri() == "urn:x");
  o.set_attribute("xmlns:x", "urn:changed");
  CHECK(o.child("x:kid").namespace_uri() == "urn:changed");  // always resolved against the current tree
  o.remove_attribute("xmlns:x");
  CHECK(o.child("x:kid").namespace_uri().empty());

  // Only elements have settable attributes.
  CHECK_FALSE(o.append_text("t").set_attribute("a", "b"));
  CHECK_FALSE(other.node().set_attribute("a", "b"));
  CHECK_FALSE(other.set_declaration().set_attribute("version", "1.1"));
  CHECK_FALSE(other.declaration().attribute("version").set_value("1.1"));
}

TEST_CASE("xml: mutators validate what they are given") {
  XmlDocument document;
  XmlNode root = document.append_child("root");

  SUBCASE("names") {
    for (const std::string_view bad : {"", "1a", "a b", "<a>", "a>", "a=\"b\"", "-a", "a\xFF", "a/b", "a&b"}) {
      CAPTURE(bad);
      CHECK_FALSE(root.append_child(bad));
      CHECK_FALSE(root.prepend_child(bad));
      CHECK_FALSE(root.set_attribute(bad, "v"));
      CHECK_FALSE(root.append_pi(bad, "d"));
      CHECK_FALSE(root.set_name(bad));
    }
    CHECK_FALSE(root.append_pi("xml", "version='1.0'"));
    CHECK_FALSE(root.append_pi("XmL"));
    CHECK(root.append_pi("xml-stylesheet", "href='a'"));
    CHECK_FALSE(root.first_child().set_name("xml"));
    CHECK(shape(root) == "root(?xml-stylesheet'href='a'')");
    CHECK(root.append_child("caf\xC3\xA9").set_attribute("\xE4\xB8\xAD", "ok"));
    // Replacing the value of an existing attribute does not re-validate its name.
    CHECK(root.set_attribute("ok", "1"));
    CHECK(root.set_attribute("ok", "2").value() == "2");
  }
  SUBCASE("text is sanitised like xml_escape does") {
    using namespace std::string_view_literals;
    CHECK(root.append_text("bad\xFFutf8 and \xC3").value() == "bad\xEF\xBF\xBDutf8 and \xEF\xBF\xBD");
    CHECK(root.append_text("nul\0and\x01""bell\x0B\x0C\x1F."sv).value() == "nulandbell.");
    CHECK(root.append_text("\xEF\xBF\xBE|\xEF\xBF\xBF|\xED\xA0\x80").value() ==
          "||\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");
    CHECK(root.append_text("kept: \t\n\r \x7F \xC2\x85").value() == "kept: \t\n\r \x7F \xC2\x85");
    CHECK(root.append_cdata("c\x01""d\xFF").value() == "cd\xEF\xBF\xBD");
    CHECK(root.set_attribute("a", "v\x02\xFE").value() == "v\xEF\xBF\xBD");
    CHECK(root.attribute("a").set_value("\x03"));
    CHECK(root.attribute("a").value().empty());
    XmlNode text = root.first_child();
    CHECK(text.set_value("\x01\x02\x03"));
    CHECK(text.value().empty());
    for (const XmlNode node : root.children()) CHECK(is_valid_utf8(node.value()));
    CHECK(round_trips(document));
  }
  SUBCASE("comments cannot contain two hyphens") {
    CHECK(root.append_comment("a--b").value() == "a- -b");
    CHECK(root.append_comment("-----").value() == "- - - - - ");
    CHECK(root.append_comment("ends with -").value() == "ends with - ");
    CHECK(root.append_comment("-").value() == "- ");
    CHECK(root.append_comment("--> <evil/> <!--").value() == "- -> <evil/> <!- - ");
    CHECK(root.append_comment("fine - as - is").value() == "fine - as - is");
    CHECK(root.append_comment("").value().empty());
    CHECK(root.append_comment("x\x01y").value() == "xy");
    XmlNode comment = root.first_child();
    CHECK(comment.set_value("--"));
    CHECK(comment.value() == "- - ");
    CHECK(xml_to_string(root, compact()) ==
          "<root><!--- - --><!--- - - - - --><!--ends with - --><!--- --><!--- -> <evil/> <!- - --><!--fine - as - "
          "is-->"
          "<!----><!--xy--></root>");
    CHECK(round_trips(document));
  }
  SUBCASE("processing instruction data cannot contain ?>") {
    CHECK(root.append_pi("a", "x?>y").value() == "x? >y");
    CHECK(root.append_pi("b", "?>?>").value() == "? >? >");
    CHECK(root.append_pi("c", "  \t leading space is dropped ").value() == "leading space is dropped ");
    CHECK(root.append_pi("d", "? > is fine ?").value() == "? > is fine ?");
    CHECK(root.append_pi("e", "   ").value().empty());
    XmlNode pi = root.first_child();
    CHECK(pi.set_value(" ?>"));
    CHECK(pi.value() == "? >");
    CHECK(xml_to_string(root, compact()) ==
          "<root><?a ? >?><?b ? >? >?><?c leading space is dropped ?><?d ? > is fine ?" "?><?e?></root>");
    CHECK(round_trips(document));
  }
  SUBCASE("structure of a document") {
    // One document element; no text, CDATA, or second root at the top.
    CHECK_FALSE(document.append_child("second"));
    CHECK_FALSE(document.node().prepend_child("second"));
    CHECK_FALSE(document.node().insert_child_before("second", root));
    CHECK_FALSE(document.node().append_text("text"));
    CHECK_FALSE(document.node().append_cdata("text"));
    CHECK(document.append_comment("after"));
    CHECK(document.append_pi("after"));
    CHECK(shape(document) == "root,#'after',?after''");
    // Nothing goes before the declaration; the root stays behind the doctype.
    const XmlNode declaration = document.set_declaration();
    const XmlNode doctype = document.set_doctype("root");
    CHECK(shape(document) == "decl,!root,root,#'after',?after''");
    document.node().remove_child(root);
    CHECK_FALSE(document.node().insert_child_before("early", declaration));
    CHECK_FALSE(document.node().insert_child_before("early", doctype));
    CHECK_FALSE(document.node().prepend_child("early"));  // would precede the doctype
    CHECK(shape(document) == "decl,!root,#'after',?after''");
    CHECK(document.node().insert_child_after("root", doctype));
    CHECK(shape(document) == "decl,!root,root,#'after',?after''");
    XmlWriteOptions flat;
    flat.indent.clear();
    CHECK(xml_to_string(document, flat) ==
          "<?xml version=\"1.0\" encoding=\"UTF-8\"?><!DOCTYPE root><root/><!--after--><?after?>");
    CHECK(parse_xml(xml_to_string(document)).has_value());
  }
  SUBCASE("children only where they belong") {
    XmlNode text = root.append_text("t");
    XmlNode comment = root.append_comment("c");
    XmlNode pi = root.append_pi("p");
    XmlNode cdata = root.append_cdata("d");
    for (XmlNode leaf : {text, comment, pi, cdata}) {
      CHECK_FALSE(leaf.append_child("x"));
      CHECK_FALSE(leaf.append_text("x"));
      CHECK_FALSE(leaf.append_comment("x"));
      CHECK_FALSE(leaf.first_child());
      leaf.clear_children();
    }
    // The reference node must be a child of the node it is used on.
    XmlNode inner = root.append_child("inner");
    XmlNode leaf = inner.append_child("leaf");
    CHECK_FALSE(root.insert_child_before("x", leaf));
    CHECK_FALSE(root.insert_child_after("x", leaf));
    CHECK_FALSE(root.insert_child_before("x", XmlNode()));
    CHECK_FALSE(root.insert_child_after("x", root));
    CHECK(inner.insert_child_before("x", leaf));
    CHECK(shape(root) == "root('t',#'c',?p'',cdata'd',inner(x,leaf))");
  }
}

TEST_CASE("xml: declaration and doctype of a built document") {
  XmlWriteOptions flat;
  flat.indent.clear();
  XmlDocument document;
  document.append_child("html");

  SUBCASE("set_declaration") {
    XmlNode declaration = document.set_declaration();
    CHECK(declaration == document.declaration());
    CHECK(declaration == document.node().first_child());
    CHECK(xml_to_string(document, flat) == "<?xml version=\"1.0\" encoding=\"UTF-8\"?><html/>");
    document.set_declaration("1.0", "", true);
    CHECK(xml_to_string(document, flat) == "<?xml version=\"1.0\" standalone=\"yes\"?><html/>");
    document.set_declaration("1.1", "UTF-8", false);
    CHECK(xml_to_string(document, flat) == "<?xml version=\"1.1\" encoding=\"UTF-8\" standalone=\"no\"?><html/>");
    CHECK(document.declaration() == declaration);  // replaced in place, still first
    CHECK(shape(document) == "decl,html");
    // The stored label is informative; the output is UTF-8 and says so.
    document.set_declaration("1.0", "ISO-8859-1");
    CHECK(document.declaration().attribute("encoding").value() == "ISO-8859-1");
    CHECK(xml_to_string(document, flat) == "<?xml version=\"1.0\" encoding=\"UTF-8\"?><html/>");
    // Unusable arguments fall back to something well-formed.
    document.set_declaration("2.0\"?><evil/>", "bad name\"", std::nullopt);
    CHECK(xml_to_string(document, flat) == "<?xml version=\"1.0\"?><html/>");
    CHECK(document.declaration().attribute("version").value() == "1.0");
    CHECK_FALSE(document.declaration().attribute("encoding"));
    // A parsed declaration is written back with its own version and standalone flag.
    const auto parsed = parse_xml("<?xml version='1.0' encoding='ISO-8859-1' standalone='yes'?><a>\xE9</a>");
    REQUIRE(parsed);
    CHECK(xml_to_string(*parsed, flat) ==
          "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?><a>\xC3\xA9</a>");
    CHECK(round_trips(*parsed));
  }
  SUBCASE("set_doctype") {
    XmlNode doctype = document.set_doctype("html");
    REQUIRE(doctype);
    CHECK(doctype == document.doctype());
    CHECK(doctype.name() == "html");
    CHECK(shape(document) == "!html,html");
    CHECK(xml_to_string(document, flat) == "<?xml version=\"1.0\" encoding=\"UTF-8\"?><!DOCTYPE html><html/>");
    const std::string_view strict = "html PUBLIC \"-//W3C//DTD XHTML 1.0 Strict//EN\" "
        "\"http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd\"";
    CHECK(document.set_doctype(strict) == doctype);  // replaced in place
    CHECK(doctype.value() == strict);
    CHECK(document.set_doctype("  note [\r\n<!ENTITY e \"v\">\r\n]  ").value() == "note [\n<!ENTITY e \"v\">\n]");
    CHECK(doctype.name() == "note");
    // Anything that is not exactly one doctype body is refused and changes nothing.
    for (const std::string_view bad : {"", " ", "1bad", "a><evil/><!--", "a><!--x--", "a><?x ?", "a><x/><!--", "a [",
                                       "a [<!ENTITY e 'v>]", "a \"unterminated", "a [] junk", "a><x/", "a>\n<x y='"}) {
      CAPTURE(bad);
      CHECK_FALSE(document.set_doctype(bad));
      CHECK(doctype.value() == "note [\n<!ENTITY e \"v\">\n]");
    }
    document.set_declaration();
    CHECK(shape(document) == "decl,!note,html");
    CHECK(round_trips(document));
    // On an empty document the doctype comes first and the root after it.
    XmlDocument fresh;
    CHECK(fresh.set_doctype("plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\""));
    fresh.append_comment("c");
    fresh.append_child("plist").set_attribute("version", "1.0");
    CHECK(xml_to_string(fresh) == "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                  "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                                  "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                                  "<!--c-->\n"
                                  "<plist version=\"1.0\"/>\n");
    // With a root and leading comments present, it goes right before the root.
    XmlDocument late;
    late.append_comment("first");
    late.append_child("r");
    late.append_comment("last");
    CHECK(late.set_doctype("r"));
    CHECK(shape(late) == "#'first',!r,r,#'last'");
  }
}

TEST_CASE("xml: clone, move and clear") {
  auto parsed = parse_xml("<?xml version='1.0'?><!DOCTYPE a><a x='1'>text<b y='2'><![CDATA[c]]></b><!--d--><?e f?>"
      "</a>");
  REQUIRE(parsed);
  XmlDocument original = std::move(*parsed);
  const XmlNode root = original.root();
  const std::string_view name = root.name();
  const std::string before = xml_to_string(original);

  // Moving a document keeps handles and string views valid.
  XmlDocument moved = std::move(original);
  CHECK(moved.root() == root);
  CHECK(name == "a");
  CHECK(root.attribute("x").value() == "1");
  CHECK(xml_to_string(moved) == before);
  // The moved-from document is empty and can be reused.
  CHECK_FALSE(original.root());  // NOLINT(bugprone-use-after-move)
  CHECK_FALSE(original.node());
  CHECK(original.memory_usage() == 0);
  CHECK(xml_to_string(original).empty());
  CHECK(original.clone().node().first_child() == XmlNode());
  CHECK(original.append_child("again"));
  CHECK(xml_to_string(original, compact()) == "<again/>");
  original = std::move(moved);
  CHECK(original.root() == root);
  CHECK(xml_to_string(original) == before);

  // A clone is equal, independent, and owns its strings.
  XmlDocument copy = original.clone();
  CHECK(deep_equal(copy, original));
  CHECK(xml_to_string(copy) == before);
  CHECK(copy.root() != root);
  CHECK(copy.root().name().data() != name.data());
  CHECK(copy.declaration().attribute("version").value() == "1.0");
  CHECK(copy.doctype().value() == "a");
  copy.root().set_attribute("x", "changed");
  copy.root().child("b").append_child("new");
  CHECK_FALSE(deep_equal(copy, original));
  CHECK(xml_to_string(original) == before);
  original.root().clear_children();
  CHECK(copy.root().text() == "textc");
  { const XmlDocument dropped = std::move(original); }
  CHECK(copy.root().attribute("x").value() == "changed");  // the copy outlives its source
  CHECK(copy.memory_usage() > 0);
  CHECK(XmlDocument().clone().node().type() == XmlNodeType::Document);

  copy.clear();
  CHECK_FALSE(copy.root());
  CHECK_FALSE(copy.node().first_child());
  CHECK(copy.append_child("fresh"));
  CHECK(xml_to_string(copy, compact()) == "<fresh/>");
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

TEST_CASE("xml: writer options, one by one") {
  const auto parsed = parse_xml("<doc a='1'><empty/><full>text</full><nested><leaf b='2'/></nested></doc>");
  REQUIRE(parsed);
  const XmlDocument& document = *parsed;

  SUBCASE("defaults: two spaces, LF, declaration, self-closing") {
    CHECK(xml_to_string(document) == "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                     "<doc a=\"1\">\n"
                                     "  <empty/>\n"
                                     "  <full>text</full>\n"
                                     "  <nested>\n"
                                     "    <leaf b=\"2\"/>\n"
                                     "  </nested>\n"
                                     "</doc>\n");
  }
  SUBCASE("indent") {
    XmlWriteOptions options;
    options.declaration = XmlDeclaration::Never;
    options.indent = "\t";
    CHECK(xml_to_string(document, options) ==
          "<doc a=\"1\">\n\t<empty/>\n\t<full>text</full>\n\t<nested>\n\t\t<leaf b=\"2\"/>\n\t</nested>\n</doc>\n");
    options.indent = "    ";
    CHECK(xml_to_string(document.root().child("nested"), options) == "<nested>\n    <leaf b=\"2\"/>\n</nested>\n");
    options.indent = "";
    CHECK(xml_to_string(document, options) ==
          "<doc a=\"1\"><empty/><full>text</full><nested><leaf b=\"2\"/></nested></doc>");
    // Anything but spaces and tabs would change the document: the default is used instead.
    options.indent = "--";
    CHECK(xml_to_string(document.root().child("nested"), options) == "<nested>\n  <leaf b=\"2\"/>\n</nested>\n");
  }
  SUBCASE("newline") {
    XmlWriteOptions options;
    options.newline = "\r\n";
    CHECK(xml_to_string(document.root().child("nested"), options) == "<nested>\r\n  <leaf b=\"2\"/>\r\n</nested>\r\n");
    CHECK(xml_to_string(document, options).starts_with("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n<doc "
        "a=\"1\">\r\n  <empty/>\r\n"));
    options.newline = "<br/>";  // not a line break: the default is used
    CHECK(xml_to_string(document.root().child("nested"), options) == "<nested>\n  <leaf b=\"2\"/>\n</nested>\n");
    options.newline = "";
    CHECK(xml_to_string(document.root().child("nested"), options) == "<nested>\n  <leaf b=\"2\"/>\n</nested>\n");
    // Without indentation no line break is ever written.
    options.newline = "\r\n";
    options.indent = "";
    CHECK(xml_to_string(document, options) ==
          "<?xml version=\"1.0\" encoding=\"UTF-8\"?><doc a=\"1\"><empty/><full>text</full><nested><leaf b=\"2\"/>"
          "</nested></doc>");
  }
  SUBCASE("declaration") {
    XmlWriteOptions options;
    options.indent = "";
    const XmlNode leaf = document.root().find("nested/leaf");
    const std::string standard = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>";
    // Auto: documents get one, subtrees do not.
    CHECK(xml_to_string(document, options).starts_with(standard + "<doc"));
    CHECK(xml_to_string(leaf, options) == "<leaf b=\"2\"/>");
    options.declaration = XmlDeclaration::Always;
    CHECK(xml_to_string(document, options).starts_with(standard + "<doc"));
    CHECK(xml_to_string(leaf, options) == standard + "<leaf b=\"2\"/>");
    options.declaration = XmlDeclaration::Never;
    CHECK(xml_to_string(document, options).starts_with("<doc"));
    CHECK(xml_to_string(leaf, options) == "<leaf b=\"2\"/>");

    // A document that has a declaration keeps it (Auto, Always) or loses it (Never).
    const auto declared = parse_xml("<?xml version='1.1' standalone='no'?><a/>");
    REQUIRE(declared);
    options.declaration = XmlDeclaration::Auto;
    CHECK(xml_to_string(*declared, options) == "<?xml version=\"1.1\" standalone=\"no\"?><a/>");
    options.declaration = XmlDeclaration::Always;
    CHECK(xml_to_string(*declared, options) == "<?xml version=\"1.1\" standalone=\"no\"?><a/>");
    CHECK(xml_to_string(declared->root(), options) == standard + "<a/>");
    CHECK(xml_to_string(declared->declaration(), options) == "<?xml version=\"1.1\" standalone=\"no\"?>");
    options.declaration = XmlDeclaration::Never;
    CHECK(xml_to_string(*declared, options) == "<a/>");
    // Pretty printing puts the declaration on its own line.
    options.declaration = XmlDeclaration::Always;
    options.indent = "  ";
    CHECK(xml_to_string(leaf, options) == standard + "\n<leaf b=\"2\"/>\n");
    // An empty document is just its declaration.
    CHECK(xml_to_string(XmlDocument(), options) == standard + "\n");
    options.declaration = XmlDeclaration::Never;
    CHECK(xml_to_string(XmlDocument(), options).empty());
  }
  SUBCASE("self_close_empty") {
    XmlWriteOptions options = compact();
    options.self_close_empty = false;
    CHECK(xml_to_string(document, options) ==
          "<doc a=\"1\"><empty></empty><full>text</full><nested><leaf b=\"2\"></leaf></nested></doc>");
    options.indent = "  ";
    CHECK(xml_to_string(document.root().child("nested"), options) == "<nested>\n  <leaf b=\"2\"></leaf>\n</nested>\n");
    // An element holding an empty text node is not "empty" for the writer, but reads back the same.
    XmlDocument built;
    built.append_child("e").append_text("");
    CHECK(xml_to_string(built, compact()) == "<e></e>");
    CHECK(round_trips(built));
  }
  SUBCASE("escape_non_ascii") {
    const auto wide = parse_xml("<caf\xC3\xA9 \xC3\xA0=\"\xC3\xA9\xE2\x82\xAC\">\xF0\x9F\x98\x80 \xC2\x85"
        "<!--\xC3\xA9--><![CDATA[\xC3\xA9]]>"
                                "<?p \xC3\xA9?>\x7F</caf\xC3\xA9>");
    REQUIRE(wide);
    XmlWriteOptions options = compact();
    CHECK(xml_to_string(*wide, options) ==
          "<caf\xC3\xA9 \xC3\xA0=\"\xC3\xA9\xE2\x82\xAC\">\xF0\x9F\x98\x80 \xC2\x85<!--\xC3\xA9--><![CDATA[\xC3\xA9]]>"
          "<?p \xC3\xA9?>\x7F</caf\xC3\xA9>");
    options.escape_non_ascii = true;
    // Text and attribute values become references; names, comments, CDATA and PIs cannot.
    CHECK(xml_to_string(*wide, options) ==
          "<caf\xC3\xA9 \xC3\xA0=\"&#xE9;&#x20AC;\">&#x1F600; &#x85;<!--\xC3\xA9--><![CDATA[\xC3\xA9]]><?p "
          "\xC3\xA9?>\x7F</caf\xC3\xA9>");
    const auto back = parse_xml(xml_to_string(*wide, options));
    REQUIRE(back);
    CHECK(deep_equal(*wide, *back));
    const auto ascii = parse_xml("<a b='\xC3\xA9'>\xE4\xB8\xAD</a>");
    REQUIRE(ascii);
    const std::string out = xml_to_string(*ascii, options);
    CHECK(out == "<a b=\"&#xE9;\">&#x4E2D;</a>");
    for (const char c : out) CHECK(static_cast<unsigned char>(c) < 0x80);
  }
}

TEST_CASE("xml: pretty printing never changes character data") {
  const auto print = [](std::string_view xml, const XmlParseOptions& options = {}) {
    const auto parsed = parse_xml(xml, options);
    return parsed ? xml_to_string(*parsed, pretty_without_declaration()) : "ERROR: " + parsed.error().message;
  };

  SUBCASE("elements with text are written on one line, exactly") {
    CHECK(print("<doc><p>Hello <b>big</b> world</p><list><item/><item>x</item></list><!--c--><?pi d?></doc>") ==
          "<doc>\n"
          "  <p>Hello <b>big</b> world</p>\n"
          "  <list>\n"
          "    <item/>\n"
          "    <item>x</item>\n"
          "  </list>\n"
          "  <!--c-->\n"
          "  <?pi d?>\n"
          "</doc>\n");
    // Everything below a mixed element stays inline, even text-free elements.
    CHECK(print("<a><p>x<b><i><u/></i></b><c><d/><e/></c></p><q><r/></q></a>") ==
          "<a>\n"
          "  <p>x<b><i><u/></i></b><c><d/><e/></c></p>\n"
          "  <q>\n"
          "    <r/>\n"
          "  </q>\n"
          "</a>\n");
    CHECK(print("<a>text</a>") == "<a>text</a>\n");
    CHECK(print("<a><![CDATA[x]]><b/></a>") == "<a><![CDATA[x]]><b/></a>\n");
    CHECK(print("<a/>") == "<a/>\n");
    CHECK(print("<a><b/></a>") == "<a>\n  <b/>\n</a>\n");
    CHECK(print("<a><!--only--></a>") == "<a>\n  <!--only-->\n</a>\n");
  }
  SUBCASE("white space that is content is left alone") {
    CHECK(print("<a> </a>") == "<a> </a>\n");
    CHECK(print("<p>a <b>b</b> <i>c</i>\n</p>") == "<p>a <b>b</b> <i>c</i>\n</p>\n");
    // A tree that kept all its white space is reproduced exactly.
    const std::string_view spaced = "<a>\n\t<b>  <c/>\n</b>\n    <d/> </a>";
    CHECK(print(spaced, keep_whitespace()) == std::string(spaced) + "\n");
    CHECK(print(" \n<a>\n  <b/>\n</a>\n", keep_whitespace()) == "<a>\n  <b/>\n</a>\n");
  }
  SUBCASE("xml:space=\"preserve\" subtrees are never indented") {
    CHECK(print("<a><b xml:space='preserve'><c/><d><e/></d></b><f><g/></f></a>") ==
          "<a>\n"
          "  <b xml:space=\"preserve\"><c/><d><e/></d></b>\n"
          "  <f>\n"
          "    <g/>\n"
          "  </f>\n"
          "</a>\n");
    CHECK(print("<a xml:space='preserve'><b><c/></b></a>") == "<a xml:space=\"preserve\"><b><c/></b></a>\n");
    CHECK(print("<a xml:space='default'><b/></a>") == "<a xml:space=\"default\">\n  <b/>\n</a>\n");
  }
  SUBCASE("document-level nodes each get a line") {
    CHECK(print("<?xml version='1.0'?><!--a--><!DOCTYPE r [<!ENTITY e 'v'>]><?p q?><r><s/></r><!--z-->") ==
          "<!--a-->\n<!DOCTYPE r [<!ENTITY e 'v'>]>\n<?p q?>\n<r>\n  <s/>\n</r>\n<!--z-->\n");
  }
  SUBCASE("indentation stops growing after 64 levels") {
    std::string nested;
    for (int i = 0; i < 70; ++i) nested += "<e" + std::to_string(i) + ">";
    nested += "<leaf/>";
    for (int i = 69; i >= 0; --i) nested += "</e" + std::to_string(i) + ">";
    const std::string out = print(nested);
    CHECK(out.find("\n" + std::string(2 * 10, ' ') + "<e10>\n") != std::string::npos);
    CHECK(out.find("\n" + std::string(2 * 63, ' ') + "<e63>\n") != std::string::npos);
    CHECK(out.find("\n" + std::string(2 * 64, ' ') + "<e64>\n") != std::string::npos);
    CHECK(out.find("\n" + std::string(2 * 64, ' ') + "<e69>\n") != std::string::npos);
    CHECK(out.find("\n" + std::string(2 * 64, ' ') + "<leaf/>\n") != std::string::npos);
    CHECK(out.find("\n" + std::string(2 * 64, ' ') + "</e69>\n") != std::string::npos);
    CHECK(out.find(std::string(2 * 64 + 1, ' ')) == std::string::npos);
    CHECK(out.ends_with("\n</e0>\n"));
  }
  SUBCASE("single nodes other than elements get no line break") {
    const auto parsed = parse_xml("<a>t<!--c--><?p d?><![CDATA[e]]></a>");
    REQUIRE(parsed);
    std::string all;
    for (const XmlNode child : parsed->root().children()) all += xml_to_string(child) + "|";
    CHECK(all == "t|<!--c-->|<?p d?>|<![CDATA[e]]>|");
  }
}

TEST_CASE("xml: writer escaping") {
  XmlDocument document;
  XmlNode root = document.append_child("r");
  root.set_attribute("a", "& < > \" ' \t \n \r end");
  root.append_text("& < > \" ' \t \n \r ]]> end");
  CHECK(xml_to_string(document, compact()) ==
        "<r a=\"&amp; &lt; &gt; &quot; ' &#9; &#10; &#13; end\">&amp; &lt; &gt; \" ' \t \n &#13; ]]&gt; end</r>");
  // ...which is exactly what reads back.
  const auto back = parse_xml(xml_to_string(document, compact()));
  REQUIRE(back);
  CHECK(back->root().attribute("a").value() == "& < > \" ' \t \n \r end");
  CHECK(back->root().text() == "& < > \" ' \t \n \r ]]> end");
  CHECK(round_trips(document));

  SUBCASE("CDATA sections are split at ]]>") {
    XmlDocument cdata;
    XmlNode c = cdata.append_child("c");
    c.append_cdata("a]]>b");
    CHECK(xml_to_string(cdata, compact()) == "<c><![CDATA[a]]]]><![CDATA[>b]]></c>");
    c.append_cdata("]]>]]>");
    c.append_cdata("]]");
    c.append_cdata(">");
    c.append_cdata("");
    c.append_cdata("<&>\r\n");
    CHECK(xml_to_string(cdata, compact()) ==
          "<c><![CDATA[a]]]]><![CDATA[>b]]><![CDATA[]]]]><![CDATA[>]]]]><![CDATA[>]]><![CDATA[]]]]><![CDATA[>]]>"
          "<![CDATA[]]>"
          "<![CDATA[<&>\r\n]]></c>");
    const auto reparsed = parse_xml(xml_to_string(cdata, compact()), keep_whitespace());
    REQUIRE(reparsed);
    // The CR inside CDATA cannot be escaped and reads back as LF: the one
    // value a CDATA section does not preserve.
    CHECK(reparsed->root().text() == "a]]>b]]>]]>]]><&>\n");
    c.last_child().set_value("<&>\n");
    CHECK(round_trips(cdata));
  }
  SUBCASE("every legal character survives a round trip") {
    XmlDocument all;
    XmlNode e = all.append_child("e");
    std::string text;
    for (char32_t cp = 0x20; cp < 0x3000; ++cp) append_utf8(text, cp);
    for (const char32_t cp : {U'\t', U'\n', U'\r', U'\xD7FF', U'\xE000', U'\xFFFD', U'\x10000', U'\x10FFFF'}) {
      append_utf8(text, cp);
    }
    e.set_attribute("v", text);
    e.append_text(text);
    CHECK(e.attribute("v").value() == text);
    CHECK(e.text() == text);
    for (const bool ascii : {false, true}) {
      XmlWriteOptions options = compact();
      options.escape_non_ascii = ascii;
      const auto reparsed = parse_xml(xml_to_string(all, options));
      REQUIRE(reparsed);
      CHECK(reparsed->root().attribute("v").value() == text);
      CHECK(reparsed->root().text() == text);
      CHECK(deep_equal(all, *reparsed));
    }
  }
}

TEST_CASE("xml: escaping") {
  CHECK(xml_escape("a < b && c > d") == "a &lt; b &amp;&amp; c &gt; d");
  CHECK(xml_escape("say \"hi\"\n") == "say \"hi\"\n");
  CHECK(xml_escape("say \"hi\"\n\t", true) == "say &quot;hi&quot;&#10;&#9;");
  CHECK(xml_escape("cr\r") == "cr&#13;");
  CHECK(xml_escape("cr\r", true) == "cr&#13;");
  CHECK(xml_escape(std::string_view("nul\0and\x01" "bell", 12)) == "nulandbell");  // forbidden characters vanish
  CHECK(xml_escape("bad\xFFutf8") == "bad\xEF\xBF\xBDutf8");
  CHECK(xml_escape("\xE2\x82\xAC") == "\xE2\x82\xAC");
  CHECK(xml_escape("\xEF\xBF\xBE\xEF\xBF\xBFx") == "x");
  CHECK(xml_escape("it's") == "it's");
  CHECK(xml_escape("").empty());
  // xml_escape and the tree agree on what a value looks like after sanitising.
  XmlDocument document;
  XmlNode e = document.append_child("e");
  for (const std::string_view sample : {"plain", "a\x01""b\xFF", "<&>\"'", "\t\r\n", "\xED\xA0\x80"}) {
    e.set_text(sample);
    e.set_attribute("v", sample);
    CHECK(xml_to_string(e, compact()) ==
          "<e v=\"" + xml_escape(sample, true) + "\">" + xml_escape(sample) + "</e>");
  }
}

TEST_CASE("xml: deep_equal") {
  const auto same = [](std::string_view a, std::string_view b, const XmlParseOptions& options = {}) {
    const auto first = parse_xml(a, options);
    const auto second = parse_xml(b, options);
    return first && second && deep_equal(*first, *second) && deep_equal(*second, *first);
  };
  CHECK(same("<a/>", "<a></a>"));
  CHECK(same("<a b='1' c='2'><d>t</d></a>", "<a  b=\"1\"\n c=\"2\" ><d >t</d ></a >"));
  // Attribute order does not matter; names and values do.
  CHECK(same("<a b='1' c='2' d='3'/>", "<a d='3' b='1' c='2'/>"));
  CHECK_FALSE(same("<a b='1' c='2'/>", "<a b='1' c='3'/>"));
  CHECK_FALSE(same("<a b='1' c='2'/>", "<a b='1' d='2'/>"));
  CHECK_FALSE(same("<a b='1'/>", "<a b='1' c='2'/>"));
  CHECK_FALSE(same("<a b='1' c='2'/>", "<a b='1'/>"));
  CHECK_FALSE(same("<a b='1' c='1'/>", "<a b='1' b2='1'/>"));
  // Declarations are ignored; everything else at the top counts.
  CHECK(same("<?xml version='1.0' encoding='UTF-8'?><a/>", "<a/>"));
  CHECK(same("<?xml version='1.0'?><a/>", "<?xml version='1.1' standalone='yes'?><a/>"));
  CHECK_FALSE(same("<!DOCTYPE a><a/>", "<a/>"));
  CHECK_FALSE(same("<!DOCTYPE a><a/>", "<!DOCTYPE a []><a/>"));
  CHECK_FALSE(same("<!--c--><a/>", "<a/>"));
  CHECK_FALSE(same("<a/><?p?>", "<a/>"));
  // Structure and content.
  CHECK_FALSE(same("<a/>", "<b/>"));
  CHECK_FALSE(same("<a><b/></a>", "<a><b/><b/></a>"));
  CHECK_FALSE(same("<a><b/><c/></a>", "<a><c/><b/></a>"));
  CHECK_FALSE(same("<a><b><c/></b></a>", "<a><b/><c/></a>"));
  CHECK_FALSE(same("<a>x</a>", "<a>y</a>"));
  CHECK_FALSE(same("<a>x</a>", "<a/>"));
  CHECK_FALSE(same("<a>x</a>", "<a>x<b/></a>"));
  CHECK_FALSE(same("<a>x</a>", "<a><![CDATA[x]]></a>"));  // text and CDATA are different kinds
  CHECK_FALSE(same("<a><!--x--></a>", "<a><!--y--></a>"));
  CHECK_FALSE(same("<a><?p x?></a>", "<a><?p y?></a>"));
  CHECK_FALSE(same("<a><?p x?></a>", "<a><?q x?></a>"));
  CHECK_FALSE(same("<a><!--x--></a>", "<a><?x?></a>"));
  CHECK(same("<a>x&amp;y</a>", "<a>x&#38;y</a>"));
  CHECK(same("<a><![CDATA[x]]></a>", "<a><![CDATA[x]]></a>"));
  // Prefixes are compared as written, not by namespace.
  CHECK_FALSE(same("<p:a xmlns:p='u'/>", "<q:a xmlns:q='u'/>"));
  // White space that the parser kept is content.
  CHECK(same("<a> <b/> </a>", "<a><b/></a>"));
  CHECK_FALSE(same("<a> <b/> </a>", "<a><b/></a>", keep_whitespace()));

  SUBCASE("adjacent text nodes count as one piece of text") {
    XmlDocument split;
    XmlNode s = split.append_child("a");
    s.append_text("he");
    s.append_text("");
    s.append_text("llo");
    s.append_child("b");
    s.append_text("");
    s.append_cdata("x]]");
    s.append_cdata(">y");
    s.append_text("");
    XmlDocument joined;
    XmlNode j = joined.append_child("a");
    j.append_text("hello");
    j.append_child("b");
    j.append_cdata("x]]>y");
    CHECK(deep_equal(split, joined));
    CHECK(deep_equal(joined, split));
    CHECK(round_trips(split));
    j.last_child().set_value("x]]>z");
    CHECK_FALSE(deep_equal(split, joined));
    j.last_child().set_value("x]]>y!");
    CHECK_FALSE(deep_equal(split, joined));
    CHECK_FALSE(deep_equal(joined, split));
    j.last_child().set_value("x]]>");
    CHECK_FALSE(deep_equal(split, joined));
    CHECK_FALSE(deep_equal(joined, split));
    // Empty text nodes are as good as absent, also as the only child.
    XmlDocument hollow;
    hollow.append_child("e").append_text("");
    XmlDocument bare;
    bare.append_child("e");
    CHECK(deep_equal(hollow, bare));
    CHECK(deep_equal(bare, hollow));
    // An empty CDATA section is a node of its own.
    bare.root().append_cdata("");
    CHECK_FALSE(deep_equal(hollow, bare));
  }
  SUBCASE("subtrees and single nodes") {
    const auto parsed = parse_xml("<r><a x='1'>t<b/></a><a x='1'>t<b/></a><a x='2'>t<b/></a>t<!--c--><!--c--></r>");
    REQUIRE(parsed);
    const XmlNode r = parsed->root();
    const XmlNode first = r.first_child();
    CHECK(deep_equal(first, first));
    CHECK(deep_equal(first, first.next_sibling()));
    CHECK_FALSE(deep_equal(first, first.next_sibling().next_sibling()));
    CHECK_FALSE(deep_equal(first, r));
    CHECK(deep_equal(first.first_child(), r.child("a").next_sibling().next_sibling().next_sibling()));  // 't' and 't'
    CHECK(deep_equal(r.last_child(), r.last_child().previous_sibling()));
    CHECK_FALSE(deep_equal(r.last_child(), first.first_child()));
    CHECK_FALSE(deep_equal(parsed->node(), r));
  }
}

TEST_CASE("xml: documents round-trip through the writer") {
  const std::vector<std::string_view> samples = {
      "<a/>",
      "<?xml version='1.0' encoding='UTF-8' standalone='yes'?>\n<!-- c -->\n<!DOCTYPE a [<!ENTITY e 'v'>]>\n<a>&e;"
      "</a>\n<?end?>",
      "<a b='1' c=\"two\" d='&lt;&amp;&gt;&quot;&apos;' e='tab&#9;lf&#10;cr&#13;'>text &amp; <b>bold</b> tail</a>",
      "<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink'>\n"
      "  <defs><path id='p' d='M 0 0 L 10 10 Z'/></defs>\n  <use xlink:href='#p' x='5'/>\n"
      "  <text xml:space='preserve'>  two  <tspan> </tspan>  </text>\n</svg>",
      "<a>  leading and trailing  </a>",
      "<a><b> </b><c>\n</c><d>\t</d></a>",
      "<p>mixed <i>content</i> <b>with</b> spaces <br/> and\nline breaks\r\nof all kinds\r</p>",
      "<a><![CDATA[<raw> & ]]]]><![CDATA[>]]>after<!-- comment - with - dashes --><?pi with ? and > ?></a>",
      "<a>&#x1F600;&#xE9;&#x4E2D;\xF0\x9F\x98\x80</a>",
      "<r><e1/><e2></e2><e3> </e3><e4><!----></e4><e5><?p?></e5><e6><![CDATA[]]></e6></r>",
      "<a>\xC2\x85\xE2\x80\xA8 &#13; &#xD;&#xA; ]]&gt; ]]></a>",
      "<w:document xmlns:w='urn:w'><w:body><w:p><w:r><w:t xml:space='preserve'> a </w:t></w:r></w:p></w:body>"
      "</w:document>",
  };
  for (const std::string_view sample : samples) {
    CAPTURE(sample);
    for (const bool keep : {false, true}) {
      XmlParseOptions options;
      options.preserve_whitespace = keep;
      const auto parsed = parse_xml(sample, options);
      REQUIRE(parsed);
      // Compact output, all white space kept on the way back: always equal.
      CHECK(round_trips(*parsed));
      // Any indentation, default parse: equal for trees that came from a default parse.
      if (!keep) {
        for (const char* indent : {"  ", "\t", ""}) {
          XmlWriteOptions write;
          write.indent = indent;
          write.newline = "\r\n";
          const auto back = parse_xml(xml_to_string(*parsed, write));
          REQUIRE(back);
          CHECK(deep_equal(*parsed, *back));
        }
      }
      // Writing is stable: the output of the output is the same text.
      const std::string once = xml_to_string(*parsed);
      const auto again = parse_xml(once, options);
      REQUIRE(again);
      CHECK(xml_to_string(*again) == once);
      CHECK(xml_to_string(parsed->clone()) == once);
    }
  }
}

// ---------------------------------------------------------------------------
// Files written by other software
// ---------------------------------------------------------------------------

TEST_CASE("xml: fixtures written by other tools") {
  SUBCASE("ElementTree, default and prefixed namespaces") {
    const Bytes data = test::fixture("util/xml/elementtree_namespaces.xml");
    const auto parsed = parse_xml(ByteView(data));
    REQUIRE(parsed);
    CHECK(parsed->declaration().attribute("encoding").value() == "utf-8");
    const XmlNode catalog = parsed->root();
    CHECK(catalog.name() == "catalog");
    CHECK(catalog.namespace_uri() == "urn:example:catalog");
    CHECK(catalog.attribute("version").as_int() == 3);
    CHECK(catalog.attribute("urn:example:meta", "checked").as_bool());
    CHECK(shape(catalog) ==
          "catalog[xmlns=urn:example:catalog xmlns:m=urn:example:meta version=3 m:checked=true]("
          "item[id=a1 price=12.50]('Blue <widget> & \"gadget\"'),"
          "item[id=a2 price=0.75 m:note=tab\\there, quote \" and <angle>]('Mixed ',b('bold'),' tail text'),m:empty)");
    const std::vector<XmlNode> items = catalog.find_all("item");
    REQUIRE(items.size() == 2u);
    CHECK(items[0].attribute("price").as_double() == 12.5);
    CHECK(items[1].attribute("price").as_double() == 0.75);
    CHECK(items[1].text() == "Mixed bold tail text");
    CHECK(items[1].child("b").namespace_uri() == "urn:example:catalog");
    CHECK(items[1].attribute("m:note").namespace_uri() == "urn:example:meta");
    CHECK(items[1].attribute("id").namespace_uri().empty());
    CHECK(catalog.child("urn:example:meta", "empty").name() == "m:empty");
    CHECK_FALSE(catalog.children("urn:example:catalog", "item").empty());
    CHECK(round_trips(*parsed));
  }
  SUBCASE("minidom, pretty printed with comment, PI and CDATA") {
    const std::string text = test::fixture_text("util/xml/minidom_pretty.xml");
    const auto parsed = parse_xml(text);
    REQUIRE(parsed);
    CHECK(shape(*parsed) ==
          "decl,#' synthetic sample ',?render'mode=\"draft\"',report[title=Q&A <summary>]("
          "section[n=1](para('alpha & beta < gamma'),code(cdata'if (a < b && c > d) { run(); }'),break),#' end of "
          "report ')");
    // Our pretty printer reproduces minidom's layout byte for byte, except
    // for the lower-case encoding label.
    std::string expected = text;
    const std::size_t label = expected.find("utf-8");
    REQUIRE(label != std::string::npos);
    expected.replace(label, 5, "UTF-8");
    CHECK(xml_to_string(*parsed) == expected);
    CHECK(round_trips(*parsed));
    // With all white space kept, the compact output is the file itself.
    const auto kept = parse_xml(text, keep_whitespace());
    REQUIRE(kept);
    XmlWriteOptions flat;
    flat.indent.clear();
    std::string joined = expected;
    // (the line breaks between the prolog nodes are outside the root and not part of the tree)
    for (const std::string_view prolog_end : {"?>\n<!--", "-->\n<?render", "?>\n<report"}) {
      const std::size_t at_break = joined.find(prolog_end);
      REQUIRE(at_break != std::string::npos);
      joined.erase(at_break + prolog_end.find('\n'), 1);
    }
    joined.pop_back();  // the final line break
    CHECK(xml_to_string(*kept, flat) == joined);
  }
  SUBCASE("ElementTree, UTF-16 with byte order mark") {
    const Bytes data = test::fixture("util/xml/utf16_bom_elementtree.xml");
    REQUIRE(data.size() > 2);
    CHECK(data[0] == std::byte{0xFF});
    CHECK(data[1] == std::byte{0xFE});
    const auto parsed = parse_xml(ByteView(data));
    REQUIRE(parsed);
    CHECK(parsed->declaration().attribute("encoding").value() == "utf-16");
    const XmlNode greetings = parsed->root();
    CHECK(greetings.attribute("count").as_int() == 3);
    std::string all;
    for (const XmlNode greeting : greetings.children("greeting")) {
      all += std::string(greeting.attribute("lang").value()) + "=" + greeting.text() + ";";
    }
    CHECK(all == "en=Hello;ja=\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF;emoji=\xE2\x82\xAC 5 "
        "\xF0\x9F\x98\x80;");
    CHECK(xml_to_string(*parsed).starts_with("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<greetings count=\"3\">\n  "
        "<greeting lang=\"en\">Hello</greeting>\n"));
    CHECK(round_trips(*parsed));
  }
  SUBCASE("ElementTree, ISO-8859-1") {
    const Bytes data = test::fixture("util/xml/latin1_elementtree.xml");
    CHECK_FALSE(is_valid_utf8(to_string(ByteView(data))));  // really Latin-1 on disk
    const auto parsed = parse_xml(ByteView(data));
    REQUIRE(parsed);
    CHECK(shape(*parsed) == "decl,menu[caf\xC3\xA9=cr\xC3\xA8me br\xC3\xBBl\xC3\xA9""e]("
                            "plat('\xC3\x86""bleskiver \xC3\xA0 la carte, se\xC3\xB1or'),prix('4 \xE2\x82\xAC'))");
    CHECK(round_trips(*parsed));
  }
  SUBCASE("DOCTYPE with an internal subset and entities") {
    const std::string text = test::fixture_text("util/xml/doctype_internal_subset.xml");
    const auto parsed = parse_xml(text);
    REQUIRE(parsed);
    const XmlNode doctype = parsed->doctype();
    CHECK(doctype.name() == "note");
    CHECK(doctype.value().starts_with("note [\n  <!-- internal subset: a ] and a > inside a comment -->\n"));
    CHECK(doctype.value().ends_with("<?subset-pi ]> ?>\n]"));
    const XmlNode note = parsed->root();
    CHECK(shape(note) ==
          "note[lang=en title=Sample \xC2\xA9](to('Hello from Sample'),body('Line one\\nline two & <three>'))");
    // Entities that are not declared stay errors.
    std::string broken = text;
    broken.replace(broken.find("&greeting;"), 10, "&farewell;");
    CHECK(error_of(broken) == std::format("invalid XML at line 12, column 7 (byte {}): undefined entity '&farewell;'",
                                          broken.find("&farewell;")));
    CHECK(round_trips(*parsed));
    // The doctype is written back verbatim, the entity uses expanded.
    const std::string out = xml_to_string(*parsed);
    CHECK(out.find("<!DOCTYPE " + std::string(doctype.value()) + ">\n<note lang=\"en\" title=\"Sample "
        "\xC2\xA9\">\n") != std::string::npos);
    CHECK(out.ends_with("  <to>Hello from Sample</to>\n  <body>Line one\nline two &amp; &lt;three&gt;</body>\n"
        "</note>\n"));
  }
  SUBCASE("a document shaped like WordprocessingML") {
    const Bytes data = test::fixture("util/xml/ooxml_like_document.xml");
    const auto parsed = parse_xml(ByteView(data));
    REQUIRE(parsed);
    const std::string_view w = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
    const std::string_view r = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
    const XmlNode document = parsed->root();
    CHECK(document.name() == "w:document");
    CHECK(document.namespace_uri() == w);
    const XmlNode body = document.child(w, "body");
    REQUIRE(body);
    std::vector<std::string> paragraphs;
    for (const XmlNode paragraph : body.children(w, "p")) paragraphs.push_back(paragraph.text());
    REQUIRE(paragraphs.size() == 3u);
    // xml:space="preserve" keeps the significant blanks; indentation is dropped.
    CHECK(paragraphs[0] == "Lorem ipsum dolor sit amet.");
    CHECK(paragraphs[1] == "  two leading spaces");
    CHECK(paragraphs[2] == "link");
    CHECK(body.find("w:p/w:r/w:rPr/w:b"));
    CHECK(body.find("w:p/w:r/w:rPr").parent().child_text("w:t") == "dolor");
    CHECK(body.find("w:p/w:hyperlink").attribute(r, "id").value() == "rId4");
    CHECK(body.find("w:p").attribute(w, "rsidR").value() == "00A00001");
    const XmlNode size = body.find("w:sectPr/w:pgSz");
    CHECK(size.attribute("w:w").as_int() == 11906);
    CHECK(size.attribute(w, "h").as_int() == 16838);
    CHECK(shape(body.child_local("p")) ==
          "w:p[w:rsidR=00A00001](w:r(w:t[xml:space=preserve]('Lorem ipsum ')),"
          "w:r(w:rPr(w:b),w:t[xml:space=preserve]('dolor')),w:r(w:t[xml:space=preserve](' sit amet.')))");
    std::size_t elements = 0;
    std::size_t texts = 0;
    for (const XmlNode node : parsed->node().descendants()) {
      elements += node.is_element() ? 1 : 0;
      texts += node.is_text() ? 1 : 0;
    }
    CHECK(elements == 20u);
    CHECK(texts == 5u);
    CHECK(round_trips(*parsed));
    // Pretty printing keeps the preserved text intact.
    const auto back = parse_xml(xml_to_string(*parsed));
    REQUIRE(back);
    CHECK(deep_equal(*parsed, *back));
    CHECK(xml_to_string(*parsed).find("<w:t xml:space=\"preserve\">  two leading spaces</w:t>") != std::string::npos);
  }
}

// ---------------------------------------------------------------------------
// Hostile input
// ---------------------------------------------------------------------------

TEST_CASE("xml: truncated and corrupted input is rejected or parsed, never worse") {
  std::vector<std::string> corpus;
  for (const std::string_view name :
       {"util/xml/elementtree_namespaces.xml", "util/xml/minidom_pretty.xml", "util/xml/utf16_bom_elementtree.xml",
        "util/xml/latin1_elementtree.xml", "util/xml/doctype_internal_subset.xml",
        "util/xml/ooxml_like_document.xml"}) {
    corpus.push_back(test::fixture_text(name));
  }
  corpus.emplace_back("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
                      "<!DOCTYPE r [<!ENTITY a \"x&#38;#60;y\"><!ENTITY b '&a;&a;'><!-- ] --><?p ]> ?>]>\r\n"
                      "<r xmlns:p='urn:p' a=\"1 &amp; 2\" p:b='&b;\t&#9;'><![CDATA[c]]>d&b;&#x1F600;<!--e--><?f g?>"
                      "<p:h xml:space=\"preserve\"> <i/> </p:h>\xC3\xA9\xE4\xB8\xAD</r>\r\n<!-- end -->");

  std::size_t inputs = 0;
  std::size_t accepted = 0;
  const auto attempt = [&](std::string_view input) {
    ++inputs;
    for (int combination = 0; combination < 4; ++combination) {
      XmlParseOptions options;
      options.preserve_whitespace = (combination & 1) != 0;
      options.replace_invalid_text = (combination & 2) != 0;
      options.keep_unknown_entities = (combination & 2) != 0;
      const auto parsed = parse_xml(input, options);
      if (!parsed) {
        // Failing is fine; failing vaguely is not.
        if (parsed.error().code != Status::Decode && parsed.error().code != Status::Limit) return false;
        if (!parsed.error().message.starts_with("invalid XML") || !is_valid_utf8(parsed.error().message)) return false;
        continue;
      }
      ++accepted;
      // Whatever was accepted is a consistent tree: valid UTF-8 that survives the writer.
      for (const XmlNode node : parsed->node().descendants()) {
        if (!is_valid_utf8(node.name()) || !is_valid_utf8(node.value())) return false;
        for (const XmlAttribute attribute : node.attributes()) {
          if (!is_valid_utf8(attribute.name()) || !is_valid_utf8(attribute.value())) return false;
        }
      }
      if (!round_trips(*parsed)) return false;
      if (!options.preserve_whitespace) {
        const auto back = parse_xml(xml_to_string(*parsed));
        if (!back || !deep_equal(*parsed, *back)) return false;
      }
    }
    return true;
  };

  for (std::size_t index = 0; index < corpus.size(); ++index) {
    const std::string& document = corpus[index];
    CAPTURE(index);
    REQUIRE_FALSE(document.empty());
    CHECK(attempt(document));
    // Every prefix.
    std::size_t bad_prefix = document.size();
    for (std::size_t length = 0; length < document.size(); ++length) {
      if (!attempt(std::string_view(document).substr(0, length))) {
        bad_prefix = length;
        break;
      }
    }
    CHECK(bad_prefix == document.size());
    // Deterministic corruptions: overwritten bytes, deletions, insertions of markup characters.
    const std::string_view markup = "<>&\"'/=![]-?;# \r\n\t:x\0";
    std::size_t bad_seed = 0;
    for (std::uint32_t seed = 1; seed <= 400 && bad_seed == 0; ++seed) {
      const Bytes noise = test::random_bytes(16, seed * 31u + static_cast<std::uint32_t>(index));
      std::string mutated = document;
      for (std::size_t edit = 0; edit < 3 && !mutated.empty(); ++edit) {
        const std::size_t position = (std::to_integer<std::size_t>(noise[edit * 4]) * 257u +
                                      std::to_integer<std::size_t>(noise[edit * 4 + 1])) % mutated.size();
        const auto value = std::to_integer<unsigned>(noise[edit * 4 + 2]);
        switch (std::to_integer<unsigned>(noise[edit * 4 + 3]) % 5) {
          case 0: mutated[position] = static_cast<char>(value); break;
          case 1: mutated[position] = markup[value % markup.size()]; break;
          case 2: mutated.erase(position, 1 + value % 4); break;
          case 3: mutated.insert(position, 1, markup[value % markup.size()]); break;
          default: mutated[position] = static_cast<char>(mutated[position] ^ (1u << (value % 8))); break;
        }
      }
      if (!attempt(mutated)) bad_seed = seed;
    }
    CHECK(bad_seed == 0);
  }
  // Pure noise.
  for (std::uint32_t seed = 1; seed <= 50; ++seed) {
    const Bytes noise = test::random_bytes(seed * 13, seed);
    CHECK(attempt(to_string(ByteView(noise))));
    CHECK(attempt("<a>" + to_string(ByteView(noise)) + "</a>"));
  }
  CHECK(inputs > 5000u);
  CHECK(accepted > 100u);
}

TEST_CASE("xml: large and adversarial inputs are handled in linear time") {
  // Generous bounds: they hold under sanitizers on slow machines, yet any
  // quadratic behaviour on these inputs would exceed them many times over.
  const double generous = 20.0;

  SUBCASE("a 20 MB document") {
    const std::string small = make_sheet(1'250'000);
    const std::string large = make_sheet(20'000'000);
    const double small_time = seconds_to_parse(small);
    const auto start = std::chrono::steady_clock::now();
    const auto parsed = parse_xml(large);
    const std::chrono::duration<double> large_time = std::chrono::steady_clock::now() - start;
    REQUIRE(parsed);
    // 16 times the input must not cost much more than 16 times the time.
    CHECK(large_time.count() < 64.0 * small_time + 1.0);
    CHECK(large_time.count() < generous);
    // A handful of large blocks rather than one allocation per node.
    CHECK(parsed->memory_usage() < 12 * large.size());
    const XmlNode data = parsed->root().child("sheetData");
    std::size_t rows = 0;
    std::size_t cells = 0;
    for (const XmlNode row : data.children("row")) {
      ++rows;
      for (const XmlNode cell : row.children("c")) cells += cell.child("v") ? 1 : 0;
    }
    CHECK(rows > 30'000u);
    CHECK(cells == rows * 12);
    CHECK(data.last_child().attribute("r").as_uint64() == rows);
    CHECK(data.first_child().find("c/v").namespace_uri() == "urn:example:sheet");
    const std::string written = xml_to_string(*parsed, compact());
    CHECK(written.size() + 39 + rows == large.size());  // declaration and the line breaks between rows
  }
  SUBCASE("inputs built to provoke quadratic work") {
    XmlParseOptions wide;
    wide.max_attribute_count_per_element = 10'000'000;
    std::string attributes = "<a";
    for (int i = 0; i < 200'000; ++i) attributes += " a" + std::to_string(i) + "=\"\"";
    attributes += "/>";
    CHECK(seconds_to_parse(attributes, wide) < generous);
    CHECK(status_of(attributes, wide) == Status::Ok);

    std::string siblings = "<a>";
    for (int i = 0; i < 500'000; ++i) siblings += "<b/> ";
    CHECK(seconds_to_parse(siblings + "</a>") < generous);
    CHECK(seconds_to_parse(siblings + "x</a>") < generous);  // a million pending blanks become nodes at once

    std::string references = "<a b='";
    for (int i = 0; i < 400'000; ++i) references += "&amp;&#x41;\t";
    references += "'>";
    for (int i = 0; i < 400'000; ++i) references += "&lt;&#65;\r\n";
    references += "</a>";
    CHECK(seconds_to_parse(references) < generous);
    const auto decoded = parse_xml(references);
    REQUIRE(decoded);
    CHECK(decoded->root().attribute("b").value().size() == 1'200'000u);
    CHECK(decoded->root().text().size() == 1'200'000u);

    std::string entities = "<!DOCTYPE a [";
    for (int i = 0; i < 100'000; ++i) entities += "<!ENTITY e" + std::to_string(i) + " 'v'>";
    entities += "]><a>";
    for (int i = 0; i < 100'000; ++i) entities += "&e" + std::to_string(i) + ";";
    entities += "</a>";
    CHECK(seconds_to_parse(entities) < generous);
    CHECK(root_text(entities).size() == 100'000u);

    const std::string garbage = "<a>" + std::string(4'000'000, '\x01') + "</a>";
    CHECK(seconds_to_parse(garbage) < generous);
    CHECK(seconds_to_parse(garbage, lenient_text()) < generous);
    CHECK(root_text(garbage, lenient_text()).size() == 12'000'000u);

    const std::string text(12'000'000, 'x');
    CHECK(seconds_to_parse("<a>" + text + "</a>") < generous);
    CHECK(seconds_to_parse("<a b='" + text + "'/>") < generous);
    CHECK(seconds_to_parse("<a><!--" + text + "--><![CDATA[" + text + "]]></a>") < generous);
  }
}
