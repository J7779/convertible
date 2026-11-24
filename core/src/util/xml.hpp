// Convertible core - XML: an in-house DOM, parser and writer.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
//
// Quick tour
// ----------
//   auto parsed = parse_xml(bytes);                 // Result<XmlDocument>
//   if (!parsed) return fail(std::move(parsed).error());
//   const XmlNode root = parsed->root();            // the document element
//   for (XmlNode row : root.child("sheetData").children("row")) {
//     const int index = row.attribute("r").as_int();
//     const std::string text = row.find("c/v").text();
//   }
//
//   XmlDocument out;
//   XmlNode svg = out.append_child("svg");
//   svg.set_attribute("xmlns", "http://www.w3.org/2000/svg");
//   svg.append_child("rect").set_attribute("width", 12.5);
//   std::string text = xml_to_string(out);
//
// Memory model
// ------------
// An XmlDocument owns everything: the nodes and attributes live in a few
// large arena blocks, and after parsing their strings point straight into a
// single UTF-8 copy of the input (decoded in place), so a parse makes a
// handful of heap allocations regardless of the document size. XmlNode and
// XmlAttribute are pointer-sized handles into the document; they are cheap
// to copy, and a default-constructed ("null") handle is safe to use: every
// accessor returns an empty or neutral value and every mutator does nothing,
// so chained lookups such as
//   doc.root().child("a").child("b").attribute("x").value()
// never crash. Handles and the string_views they return stay valid until the
// document is destroyed (moving the document keeps them valid) or the node /
// attribute they refer to is modified or removed. Removed nodes and replaced
// strings are not recycled: their memory is released when the document is
// destroyed (`clone()` produces a compact copy). Names and attribute values
// are limited to 4 GiB each; text nodes are not.
//
// A document may be read from several threads at once; mutation needs
// exclusive access. Nothing here touches the file system or network or
// depends on the C locale, and failures are reported through Result or a
// null handle, never by throwing (running out of memory aside).
//
// What the parser accepts
// -----------------------
// XML 1.0 (fifth edition) well-formed documents, checked strictly: matching
// tags, one root element, unique quoted attributes, legal names and
// characters, comments without "--". Deliberate deviations, all documented
// at the option or function they belong to:
//   * "]]>" in character data is accepted.
//   * Namespaces are resolved on demand and never validated: an undeclared
//     prefix is not an error (its namespace URI is empty), and names may
//     contain several colons.
//   * The DTD is never interpreted beyond internal general entities:
//     attribute defaults, attribute types and content models are ignored,
//     and external entities, external DTD subsets and parameter entities are
//     never read or expanded under any option.
#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/errors.hpp"
#include "util/bytes.hpp"

namespace cvt::util {

class XmlNode;
class XmlDocument;
struct XmlParseOptions;

namespace detail {

struct XmlNodeData;
struct XmlAttributeData;
struct XmlDocumentData;

/// Types `set_attribute` formats itself: bool, integers and floating point,
/// but not character types (pass text as a string).
template <class T>
concept XmlScalar = (std::integral<T> || std::floating_point<T>) && !std::same_as<T, char> &&
                    !std::same_as<T, wchar_t> && !std::same_as<T, char8_t> && !std::same_as<T, char16_t> &&
                    !std::same_as<T, char32_t>;

[[nodiscard]] std::string xml_format_signed(long long value);
[[nodiscard]] std::string xml_format_unsigned(unsigned long long value);
[[nodiscard]] std::string xml_format_float(float value);
[[nodiscard]] std::string xml_format_double(double value);

/// Locale-independent text form of a scalar: "true"/"false", decimal
/// integers, and the shortest decimal that reads back as the same floating
/// point value ("0.1", "1e+30", "nan", "inf", "-inf").
template <XmlScalar T>
[[nodiscard]] std::string xml_format(T value) {
  if constexpr (std::same_as<T, bool>) {
    return value ? "true" : "false";
  } else if constexpr (std::same_as<T, float>) {
    return xml_format_float(value);
  } else if constexpr (std::floating_point<T>) {
    return xml_format_double(static_cast<double>(value));
  } else if constexpr (std::signed_integral<T>) {
    return xml_format_signed(static_cast<long long>(value));
  } else {
    return xml_format_unsigned(static_cast<unsigned long long>(value));
  }
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Free helpers
// ---------------------------------------------------------------------------

/// Escape text for use in element content (`&`, `<`, `>`, and CR as `&#13;`)
/// or, with `attribute = true`, in a double-quoted attribute value (also `"`,
/// and tab/LF as character references). Characters XML 1.0 forbids are
/// dropped; malformed UTF-8 becomes U+FFFD.
[[nodiscard]] std::string xml_escape(std::string_view text, bool attribute = false);

/// "w:p" -> "p"; names without a prefix are returned unchanged.
[[nodiscard]] constexpr std::string_view xml_local_name(std::string_view qualified_name) noexcept {
  const std::size_t colon = qualified_name.rfind(':');
  return colon == std::string_view::npos ? qualified_name : qualified_name.substr(colon + 1);
}

/// "w:p" -> "w"; empty for names without a prefix.
[[nodiscard]] constexpr std::string_view xml_prefix(std::string_view qualified_name) noexcept {
  const std::size_t colon = qualified_name.rfind(':');
  return colon == std::string_view::npos ? std::string_view() : qualified_name.substr(0, colon);
}

/// True when `text` is a legal XML 1.0 (fifth edition) Name, i.e. usable as
/// an element, attribute or processing-instruction name: valid UTF-8, a
/// NameStartChar followed by NameChars. Use it before turning data (JSON
/// keys, column titles, ...) into element names.
[[nodiscard]] bool xml_is_name(std::string_view text) noexcept;

/// The namespaces bound implicitly to the prefixes `xml` and `xmlns`.
inline constexpr std::string_view xml_namespace_uri = "http://www.w3.org/XML/1998/namespace";
inline constexpr std::string_view xmlns_namespace_uri = "http://www.w3.org/2000/xmlns/";

// ---------------------------------------------------------------------------
// Attributes
// ---------------------------------------------------------------------------

/// Handle to one attribute of an element; null when absent.
class XmlAttribute {
 public:
  constexpr XmlAttribute() noexcept = default;
  constexpr explicit XmlAttribute(detail::XmlAttributeData* data) noexcept : data_(data) {}

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return data_ != nullptr; }
  [[nodiscard]] friend constexpr bool operator==(XmlAttribute a, XmlAttribute b) noexcept {
    return a.data_ == b.data_;
  }

  /// Qualified name as written ("r:id"); empty for a null handle.
  [[nodiscard]] std::string_view name() const noexcept;
  /// "r:id" -> "id".
  [[nodiscard]] std::string_view local_name() const noexcept;
  /// "r:id" -> "r"; empty when there is no prefix.
  [[nodiscard]] std::string_view prefix() const noexcept;
  /// Namespace bound to the prefix by the in-scope `xmlns:prefix`
  /// declarations. Attributes without a prefix have no namespace (""), as
  /// have attributes whose prefix is not declared. Same cost as
  /// XmlNode::namespace_uri().
  [[nodiscard]] std::string_view namespace_uri() const noexcept;
  /// The value with references decoded and white space normalised as XML
  /// prescribes (literal tabs and line breaks read as spaces).
  [[nodiscard]] std::string_view value() const noexcept;

  /// Typed readers. The value is trimmed of XML white space and must then be
  /// a complete number in range ("42", "-7", "+3"; for doubles also "1e-3",
  /// "inf", "nan"), otherwise `fallback` is returned. Never locale dependent.
  [[nodiscard]] int as_int(int fallback = 0) const noexcept;
  [[nodiscard]] unsigned int as_uint(unsigned int fallback = 0) const noexcept;
  [[nodiscard]] std::int64_t as_int64(std::int64_t fallback = 0) const noexcept;
  [[nodiscard]] std::uint64_t as_uint64(std::uint64_t fallback = 0) const noexcept;
  [[nodiscard]] double as_double(double fallback = 0.0) const noexcept;
  /// "true" / "1" -> true, "false" / "0" -> false, anything else -> fallback.
  [[nodiscard]] bool as_bool(bool fallback = false) const noexcept;

  /// The element this attribute belongs to.
  [[nodiscard]] XmlNode parent() const noexcept;
  /// The attribute written after this one on the same element.
  [[nodiscard]] XmlAttribute next_attribute() const noexcept;

  /// Replace the value (copied; sanitised like XmlNode::append_text).
  /// Returns false for a null handle or an attribute of a removed element.
  bool set_value(std::string_view value);
  template <detail::XmlScalar T>
  bool set_value(T number) {
    return set_value(std::string_view(detail::xml_format(number)));
  }

  [[nodiscard]] constexpr detail::XmlAttributeData* internal_data() const noexcept { return data_; }

 private:
  detail::XmlAttributeData* data_ = nullptr;
};

/// `for (XmlAttribute a : element.attributes())`
class XmlAttributeIterator {
 public:
  using value_type = XmlAttribute;
  using difference_type = std::ptrdiff_t;
  using iterator_category = std::forward_iterator_tag;

  constexpr XmlAttributeIterator() noexcept = default;
  constexpr explicit XmlAttributeIterator(XmlAttribute current) noexcept : current_(current) {}
  [[nodiscard]] constexpr XmlAttribute operator*() const noexcept { return current_; }
  XmlAttributeIterator& operator++() noexcept {
    current_ = current_.next_attribute();
    return *this;
  }
  XmlAttributeIterator operator++(int) noexcept {
    const XmlAttributeIterator before = *this;
    ++*this;
    return before;
  }
  [[nodiscard]] friend constexpr bool operator==(XmlAttributeIterator a, XmlAttributeIterator b) noexcept {
    return a.current_ == b.current_;
  }

 private:
  XmlAttribute current_;
};

class XmlAttributeRange {
 public:
  constexpr XmlAttributeRange() noexcept = default;
  constexpr explicit XmlAttributeRange(XmlAttribute first) noexcept : first_(first) {}
  [[nodiscard]] constexpr XmlAttributeIterator begin() const noexcept { return XmlAttributeIterator(first_); }
  [[nodiscard]] constexpr XmlAttributeIterator end() const noexcept { return {}; }
  [[nodiscard]] constexpr bool empty() const noexcept { return !first_; }

 private:
  XmlAttribute first_;
};

// ---------------------------------------------------------------------------
// Nodes
// ---------------------------------------------------------------------------

enum class XmlNodeType : std::uint8_t {
  Null,                   ///< a null handle
  Document,               ///< the root of the tree; parent of the prolog nodes and the document element
  Element,                ///< `<name attr="...">children</name>`
  Text,                   ///< character data; `value()` is the decoded text
  CData,                  ///< `<![CDATA[...]]>`; `value()` is the content
  Comment,                ///< `<!--...-->`; `value()` is the text between the markers
  ProcessingInstruction,  ///< `<?name value?>`
  Declaration,            ///< `<?xml version="1.0" ...?>`; `name()` is "xml", the pseudo-attributes are attributes
  Doctype,                ///< `<!DOCTYPE value>`; `name()` is the declared root name, `value()` the raw text
};

class XmlNodeRange;
class XmlDescendantRange;

/// Handle to a node of an XmlDocument. See the notes at the top of the file
/// for lifetime and null-handle rules.
class XmlNode {
 public:
  constexpr XmlNode() noexcept = default;
  constexpr explicit XmlNode(detail::XmlNodeData* data) noexcept : data_(data) {}

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return data_ != nullptr; }
  [[nodiscard]] friend constexpr bool operator==(XmlNode a, XmlNode b) noexcept { return a.data_ == b.data_; }

  // --- what it is -----------------------------------------------------------

  [[nodiscard]] XmlNodeType type() const noexcept;
  [[nodiscard]] bool is_element() const noexcept { return type() == XmlNodeType::Element; }
  /// True for Text and CData nodes.
  [[nodiscard]] bool is_text() const noexcept {
    return type() == XmlNodeType::Text || type() == XmlNodeType::CData;
  }

  /// Elements: the qualified name as written ("w:p"). Processing
  /// instructions: the target. Declaration: "xml". Doctype: the root name.
  /// Empty for every other kind.
  [[nodiscard]] std::string_view name() const noexcept;
  /// "w:p" -> "p".
  [[nodiscard]] std::string_view local_name() const noexcept;
  /// "w:p" -> "w"; empty when there is no prefix.
  [[nodiscard]] std::string_view prefix() const noexcept;
  /// Elements: the namespace the name is in, found by walking up to the
  /// nearest `xmlns` / `xmlns:prefix` declaration; "" when the element is in
  /// no namespace or its prefix is not declared. The cost is proportional to
  /// the depth of the element times the attributes on the way up, so resolve
  /// once (or compare prefixes / local names) inside hot loops.
  [[nodiscard]] std::string_view namespace_uri() const noexcept;
  /// The namespace URI bound to `prefix` at this element ("" = the default
  /// namespace); "" when it is not declared. `xml` and `xmlns` are built in.
  [[nodiscard]] std::string_view lookup_namespace(std::string_view prefix) const noexcept;
  /// Text, CData, Comment: the content. ProcessingInstruction: the data
  /// after the target. Doctype: the raw text between `<!DOCTYPE` and `>`.
  /// Empty for elements (see `text()`), documents and declarations.
  [[nodiscard]] std::string_view value() const noexcept;

  // --- navigation -----------------------------------------------------------

  [[nodiscard]] XmlNode parent() const noexcept;
  [[nodiscard]] XmlNode first_child() const noexcept;
  [[nodiscard]] XmlNode last_child() const noexcept;
  [[nodiscard]] XmlNode next_sibling() const noexcept;
  [[nodiscard]] XmlNode previous_sibling() const noexcept;

  /// First child ELEMENT with this qualified name ("w:p"); linear in the
  /// number of children.
  [[nodiscard]] XmlNode child(std::string_view name) const noexcept;
  /// First child element with this local name, whatever its prefix.
  [[nodiscard]] XmlNode child_local(std::string_view local_name) const noexcept;
  /// First child element with this namespace URI and local name.
  [[nodiscard]] XmlNode child(std::string_view namespace_uri, std::string_view local_name) const noexcept;
  /// The same three lookups among the siblings after this node.
  [[nodiscard]] XmlNode next_sibling(std::string_view name) const noexcept;
  [[nodiscard]] XmlNode next_sibling_local(std::string_view local_name) const noexcept;
  [[nodiscard]] XmlNode next_sibling(std::string_view namespace_uri, std::string_view local_name) const noexcept;

  /// Ranges for `for (XmlNode c : node.children())`. The filtered forms
  /// yield elements only; the name passed in must outlive the loop.
  [[nodiscard]] XmlNodeRange children() const noexcept;
  [[nodiscard]] XmlNodeRange children(std::string_view name) const noexcept;
  [[nodiscard]] XmlNodeRange children_local(std::string_view local_name) const noexcept;
  [[nodiscard]] XmlNodeRange children(std::string_view namespace_uri, std::string_view local_name) const noexcept;
  /// Child elements only (no text, comments, ...).
  [[nodiscard]] XmlNodeRange elements() const noexcept;
  /// Every node below this one, depth first in document order (the node
  /// itself is not included). Iterative: safe on arbitrarily deep trees.
  [[nodiscard]] XmlDescendantRange descendants() const noexcept;

  /// Tiny path lookup: child steps by qualified name separated by '/', with
  /// `*` matching any element: `find("w:body/w:p")`, `find("*/title")`. No
  /// XPath: no predicates, axes, attributes or absolute paths; an empty path
  /// or an empty step matches nothing. `find` returns the first match in
  /// document order (it backtracks over earlier siblings that lack the later
  /// steps), `find_all` every match in document order.
  [[nodiscard]] XmlNode find(std::string_view path) const noexcept;
  [[nodiscard]] std::vector<XmlNode> find_all(std::string_view path) const;

  // --- attributes -----------------------------------------------------------

  /// Attribute with this qualified name; linear in the element's attributes.
  [[nodiscard]] XmlAttribute attribute(std::string_view name) const noexcept;
  /// First attribute with this local name, whatever its prefix.
  [[nodiscard]] XmlAttribute attribute_local(std::string_view local_name) const noexcept;
  /// Attribute with this namespace URI and local name ("" = no namespace,
  /// i.e. an attribute without a prefix).
  [[nodiscard]] XmlAttribute attribute(std::string_view namespace_uri, std::string_view local_name) const noexcept;
  [[nodiscard]] XmlAttribute first_attribute() const noexcept;
  /// In document order. Declaration nodes expose `version`, `encoding` and
  /// `standalone` this way.
  [[nodiscard]] XmlAttributeRange attributes() const noexcept;

  // --- text -----------------------------------------------------------------

  /// The concatenated values of all Text and CData nodes below this node (or
  /// of the node itself when it is one), in document order.
  [[nodiscard]] std::string text() const;
  /// `child(name).text()`.
  [[nodiscard]] std::string child_text(std::string_view name) const;

  // --- building and changing -----------------------------------------------
  //
  // All strings passed in are copied into the document. To keep every tree
  // serialisable, mutators validate what they are given:
  //   * names must satisfy xml_is_name(); otherwise nothing changes and a
  //     null handle / false is returned;
  //   * text and attribute values are sanitised the way xml_escape() does:
  //     malformed UTF-8 becomes U+FFFD and characters XML forbids are dropped;
  //   * a Document accepts one Element, plus comments and processing
  //     instructions after its declaration; an Element accepts anything
  //     except Declaration and Doctype (see XmlDocument for those two).
  // Mutators find the owning document by walking to the root (cost
  // proportional to the depth) and do nothing on null handles and on nodes
  // that have been removed from their document. They are non-const members:
  // a `const XmlNode` is a read-only view.

  /// Add a new element as the last / first child, or next to `reference`
  /// (which must be a child of this node). Null on failure.
  XmlNode append_child(std::string_view name);
  XmlNode prepend_child(std::string_view name);
  XmlNode insert_child_before(std::string_view name, XmlNode reference);
  XmlNode insert_child_after(std::string_view name, XmlNode reference);

  /// Append character data. `append_text` values are escaped on output;
  /// `append_cdata` values are written as CDATA sections.
  XmlNode append_text(std::string_view text);
  XmlNode append_cdata(std::string_view text);
  /// Append `<!--text-->`. Because "--" may not occur in a comment, "--" is
  /// stored as "- -" and a trailing "-" gains a space.
  XmlNode append_comment(std::string_view text);
  /// Append `<?target data?>`. The target must be a name other than "xml" (in
  /// any letter case); leading white space of `data` is dropped and "?>" in
  /// it is stored as "? >".
  XmlNode append_pi(std::string_view target, std::string_view data = {});

  /// Rename an element or the target of a processing instruction.
  bool set_name(std::string_view name);
  /// Replace the content of a Text, CData, Comment or ProcessingInstruction
  /// node (same sanitising as the matching append_* function).
  bool set_value(std::string_view value);
  /// Elements: replace all children with one Text node (none when `text` is
  /// empty).
  bool set_text(std::string_view text);

  /// Add an attribute or replace its value; new attributes go last. Null on
  /// failure (not an element, invalid name).
  XmlAttribute set_attribute(std::string_view name, std::string_view value);
  /// Numbers and booleans: `set_attribute("width", 12.5)`, `("count", 3)`,
  /// `("hidden", true)` write "12.5", "3", "true" (shortest round-trip
  /// decimal for floating point; locale independent).
  template <detail::XmlScalar T>
  XmlAttribute set_attribute(std::string_view attribute_name, T number) {
    return set_attribute(attribute_name, std::string_view(detail::xml_format(number)));
  }
  bool remove_attribute(std::string_view name);
  bool remove_attribute(XmlAttribute attribute);

  /// Detach `child` (with its subtree) from this node. The handle stays
  /// readable but can no longer be modified or re-inserted.
  bool remove_child(XmlNode child);
  /// Detach all children.
  void clear_children();

  [[nodiscard]] constexpr detail::XmlNodeData* internal_data() const noexcept { return data_; }

 private:
  detail::XmlNodeData* data_ = nullptr;
};

/// Forward iterator over siblings, optionally filtered to elements with a
/// given name. Obtained from XmlNode::children() and friends.
class XmlNodeIterator {
 public:
  using value_type = XmlNode;
  using difference_type = std::ptrdiff_t;
  using iterator_category = std::forward_iterator_tag;

  enum class Filter : std::uint8_t { All, Elements, Name, LocalName, Namespace };

  constexpr XmlNodeIterator() noexcept = default;
  /// Positions on the first node at or after `start` that passes the filter.
  XmlNodeIterator(XmlNode start, Filter filter, std::string_view name, std::string_view namespace_uri) noexcept;

  [[nodiscard]] constexpr XmlNode operator*() const noexcept { return current_; }
  XmlNodeIterator& operator++() noexcept;
  XmlNodeIterator operator++(int) noexcept {
    const XmlNodeIterator before = *this;
    ++*this;
    return before;
  }
  [[nodiscard]] friend constexpr bool operator==(const XmlNodeIterator& a, const XmlNodeIterator& b) noexcept {
    return a.current_ == b.current_;
  }

 private:
  void settle() noexcept;

  XmlNode current_;
  Filter filter_ = Filter::All;
  std::string_view name_;
  std::string_view namespace_uri_;
};

class XmlNodeRange {
 public:
  constexpr XmlNodeRange() noexcept = default;
  XmlNodeRange(XmlNode first, XmlNodeIterator::Filter filter, std::string_view name = {},
               std::string_view namespace_uri = {}) noexcept
      : begin_(first, filter, name, namespace_uri) {}
  [[nodiscard]] XmlNodeIterator begin() const noexcept { return begin_; }
  [[nodiscard]] constexpr XmlNodeIterator end() const noexcept { return {}; }
  [[nodiscard]] bool empty() const noexcept { return begin_ == XmlNodeIterator(); }

 private:
  XmlNodeIterator begin_;
};

/// Depth-first (document order) iterator over the nodes below a root.
class XmlDescendantIterator {
 public:
  using value_type = XmlNode;
  using difference_type = std::ptrdiff_t;
  using iterator_category = std::forward_iterator_tag;

  constexpr XmlDescendantIterator() noexcept = default;
  XmlDescendantIterator(XmlNode root, XmlNode current) noexcept : root_(root), current_(current) {}

  [[nodiscard]] constexpr XmlNode operator*() const noexcept { return current_; }
  XmlDescendantIterator& operator++() noexcept;
  XmlDescendantIterator operator++(int) noexcept {
    const XmlDescendantIterator before = *this;
    ++*this;
    return before;
  }
  [[nodiscard]] friend constexpr bool operator==(const XmlDescendantIterator& a,
                                                 const XmlDescendantIterator& b) noexcept {
    return a.current_ == b.current_;
  }

 private:
  XmlNode root_;
  XmlNode current_;
};

class XmlDescendantRange {
 public:
  constexpr XmlDescendantRange() noexcept = default;
  explicit XmlDescendantRange(XmlNode root) noexcept : root_(root) {}
  [[nodiscard]] XmlDescendantIterator begin() const noexcept { return {root_, root_.first_child()}; }
  [[nodiscard]] constexpr XmlDescendantIterator end() const noexcept { return {}; }
  [[nodiscard]] bool empty() const noexcept { return !root_.first_child(); }

 private:
  XmlNode root_;
};

// ---------------------------------------------------------------------------
// Documents
// ---------------------------------------------------------------------------

/// Owns an XML tree. Movable (handles stay valid), not copyable: use
/// `clone()` for an explicit deep copy. A moved-from document is empty.
class XmlDocument {
 public:
  XmlDocument();
  ~XmlDocument();
  XmlDocument(XmlDocument&& other) noexcept;
  XmlDocument& operator=(XmlDocument&& other) noexcept;
  XmlDocument(const XmlDocument&) = delete;
  XmlDocument& operator=(const XmlDocument&) = delete;

  /// Deep copy with compact storage; independent of this document.
  [[nodiscard]] XmlDocument clone() const;

  /// The Document node: parent of the declaration, doctype, top-level
  /// comments / processing instructions and the document element. A document
  /// converts implicitly to it, so it can be passed wherever a node is
  /// expected (`xml_to_string(document)`).
  [[nodiscard]] XmlNode node() const noexcept;
  operator XmlNode() const noexcept { return node(); }  // NOLINT(google-explicit-constructor)

  /// The document element; null while the document has none.
  [[nodiscard]] XmlNode root() const noexcept;
  /// The `<?xml ...?>` declaration node, or null.
  [[nodiscard]] XmlNode declaration() const noexcept;
  /// The `<!DOCTYPE ...>` node, or null.
  [[nodiscard]] XmlNode doctype() const noexcept;

  /// Create the document element (null when one exists or the name is bad).
  XmlNode append_child(std::string_view name);
  XmlNode append_comment(std::string_view text);
  XmlNode append_pi(std::string_view target, std::string_view data = {});

  /// Create or replace the XML declaration. `version` must look like "1.0"
  /// (else "1.0" is used); an `encoding` that is not a legal encoding name
  /// is left out. The label is stored for information only: xml_to_string()
  /// always produces UTF-8 and writes `encoding="UTF-8"` whenever the
  /// declaration has an encoding. Returns the declaration node.
  XmlNode set_declaration(std::string_view version = "1.0", std::string_view encoding = "UTF-8",
                          std::optional<bool> standalone = std::nullopt);
  /// Create or replace the document type declaration from the text that goes
  /// between `<!DOCTYPE ` and `>`, e.g.
  ///   set_doctype("html")
  ///   set_doctype(R"(plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd")")
  /// It is placed before the document element. Returns null (and changes
  /// nothing) when the text is not a well-formed doctype body.
  XmlNode set_doctype(std::string_view text);

  /// Drop everything; all handles into the document become invalid.
  void clear();

  /// Bytes of heap memory currently held (arena blocks plus the parse
  /// buffer), for limits and diagnostics.
  [[nodiscard]] std::size_t memory_usage() const noexcept;

 private:
  detail::XmlDocumentData& ensure();

  detail::XmlDocumentData* data_ = nullptr;

  friend Result<XmlDocument> parse_xml(ByteView data, const XmlParseOptions& options);
};

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

struct XmlParseOptions {
  /// White-space-only text. With `false` (the default) a run of character
  /// data consisting only of literal spaces, tabs and line breaks is DROPPED
  /// unless one of these holds for the element E that directly contains it:
  ///   1. `xml:space="preserve"` is in effect for E (set on E or on its
  ///      nearest ancestor carrying an `xml:space` attribute);
  ///   2. E directly contains other character data: a Text node with a
  ///      non-white-space character or a reference, or a CDATA section
  ///      (mixed content such as `<p>a <b>b</b> <i>c</i></p>`);
  ///   3. the run is the entire content of E (`<t> </t>` keeps its space).
  /// With `true` every such run becomes a Text node. White space outside the
  /// document element never produces nodes. Formats whose inline markup
  /// relies on spaces between elements (XHTML, FB2, TTML, ...) should pass
  /// `true`.
  bool preserve_whitespace = false;

  /// Lenient text decoding: malformed byte sequences, characters XML forbids
  /// (control characters, U+FFFE, U+FFFF) and character references to such
  /// characters become U+FFFD instead of failing with Status::Decode. (Inside
  /// an entity declaration of the DOCTYPE such a reference still fails: the
  /// doctype is kept verbatim and must stay well-formed.)
  bool replace_invalid_text = false;

  /// References to general entities other than the five predefined ones are
  /// expanded only when the entity is declared in the DOCTYPE internal
  /// subset with literal replacement text that contains no markup (see
  /// `max_entity_expansion`). Any other reference - undeclared, external,
  /// unparsed or containing markup - fails with Status::Decode, or with this
  /// option stays in the text verbatim as `&name;`.
  bool keep_unknown_entities = false;

  /// Deepest allowed element nesting (the document element is level 1);
  /// deeper input fails with Status::Limit. Parsing, destruction, traversal
  /// and serialisation never recurse, so this guards memory, not the stack.
  std::size_t max_depth = 1024;
  /// Most nodes (elements, text, comments, ...) the tree may have; more
  /// fails with Status::Limit. A node costs 64 bytes and an attribute 40 on
  /// 64-bit targets, plus their text.
  std::size_t max_nodes = 50'000'000;
  /// Most attributes on one element; more fails with Status::Limit.
  std::size_t max_attribute_count_per_element = 100'000;
  /// Budget for internal entity expansion: the total size in bytes of all
  /// replacement texts included while parsing the document (nested
  /// inclusions count at every level). Exceeding it, or nesting entity
  /// references more than 4 levels deep, fails with Status::Limit, which
  /// defuses "billion laughs" and quadratic-blow-up documents.
  std::size_t max_entity_expansion = 1024 * 1024;
};

/// Parse an XML document.
///
/// Encoding: a byte order mark (UTF-8, UTF-16LE/BE, UTF-32LE/BE) decides;
/// without one, UTF-16/32 is recognised by the zero bytes around the first
/// `<`, and otherwise the `encoding` of the XML declaration is used: UTF-8
/// (the default), US-ASCII, ISO-8859-1 or Windows-1252. Any other declared
/// encoding fails with Status::Decode "unsupported encoding '...'". The tree
/// is always UTF-8.
///
/// Normalisation follows the specification: CRLF and lone CR read as LF;
/// literal tabs and line breaks in attribute values read as spaces;
/// character references and the entities lt, gt, amp, apos, quot are decoded.
///
/// Failures are Status::Decode (malformed input) or Status::Limit (a limit
/// of XmlParseOptions was exceeded) with a message of the form
///   "invalid XML at line 3, column 14 (byte 57): mismatched closing tag </b>, expected </a>"
/// Lines and columns are 1-based, columns count characters, and the byte
/// offset refers to the original input.
///
/// Time and memory are linear in the input size. No option makes the parser
/// read anything but `data`.
[[nodiscard]] Result<XmlDocument> parse_xml(ByteView data, const XmlParseOptions& options = {});
[[nodiscard]] inline Result<XmlDocument> parse_xml(std::string_view text, const XmlParseOptions& options = {}) {
  return parse_xml(as_bytes(text), options);
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

enum class XmlDeclaration : std::uint8_t {
  Auto,    ///< documents start with their declaration, or the default one when they have none; subtrees get none
  Always,  ///< as Auto, and subtrees are prefixed with the default declaration too
  Never,   ///< no declaration, even when the document has one
};

struct XmlWriteOptions {
  /// Indentation per nesting level (spaces and tabs only); empty = no pretty
  /// printing at all, the output has exactly the white space of the tree.
  ///
  /// Pretty printing never changes character data: line breaks and
  /// indentation are added only between the children of elements that have
  /// no Text or CData child at all. An element with any text child, or with
  /// `xml:space="preserve"`, is written on one line exactly as stored,
  /// together with everything below it. Indentation stops growing after 64
  /// levels so that deep documents cannot blow up the output.
  std::string indent = "  ";
  /// Line break used by pretty printing ("\n" or "\r\n").
  std::string newline = "\n";
  /// See XmlDeclaration. The default declaration is
  /// `<?xml version="1.0" encoding="UTF-8"?>`.
  XmlDeclaration declaration = XmlDeclaration::Auto;
  /// Write `<a/>` for elements without children (otherwise `<a></a>`).
  bool self_close_empty = true;
  /// Write non-ASCII characters of text and attribute values as numeric
  /// references (`&#xE9;`). Names, comments, processing instructions and
  /// CDATA sections cannot contain references and stay UTF-8.
  bool escape_non_ascii = false;
};

/// Serialise a document or any subtree as UTF-8.
///
/// The output is always well-formed: text escapes `&`, `<`, `>` and CR,
/// attribute values also `"`, tab and LF; CDATA sections containing "]]>" are
/// split into two sections at that point. It reads back as the same tree:
///   * written with an empty `indent` and parsed with `preserve_whitespace`,
///     the result is `deep_equal` to `node` - always;
///   * written with any `indent` and parsed with default options, the result
///     is `deep_equal` to `node` provided the tree holds no white-space-only
///     text that the default parse rule drops (true for every tree that came
///     from parse_xml with default options, and for trees built without such
///     text nodes).
/// With a non-empty `indent` the output ends with a line break when `node` is
/// a document or an element. A null handle gives an empty string.
[[nodiscard]] std::string xml_to_string(const XmlNode& node, const XmlWriteOptions& options = {});

/// Structural equality of two subtrees (of the same or different documents):
/// same node kinds, names, values and attributes, recursively, where
///   * attribute order is ignored;
///   * Declaration nodes are ignored (they describe one serialisation, not
///     the content);
///   * adjacent Text nodes count as one piece of text, empty Text nodes do
///     not count, and adjacent CData nodes count as one CDATA section -
///     exactly what survives writing and re-parsing.
/// Namespace prefixes are compared literally, not resolved.
[[nodiscard]] bool deep_equal(XmlNode a, XmlNode b);

}  // namespace cvt::util
