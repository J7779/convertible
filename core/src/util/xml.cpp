// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
//
// Layout of this file:
//   1. storage: node / attribute records, the arena, the document block
//   2. character classes and small scanning helpers
//   3. tree primitives shared by the parser, the mutators and clone()
//   4. input preparation: encoding detection, transcoding, validation and
//      line-end normalisation, mapping of error offsets back to the input
//   5. the parser
//   6. the writer and deep_equal
//   7. the public handle API
#include "util/xml.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <new>
#include <utility>

#include "util/text.hpp"

namespace cvt::util {

// ===========================================================================
// 1. Storage
// ===========================================================================

namespace detail {

// Strings are (pointer, size) pairs that point either into the document's
// parse buffer or into its arena. They are not NUL terminated.
struct XmlAttributeData {  // 40 bytes on 64-bit targets
  XmlAttributeData* next = nullptr;
  XmlNodeData* owner = nullptr;
  const char* name = "";
  const char* value = "";
  std::uint32_t name_size = 0;
  std::uint32_t value_size = 0;
};

/// A node is either a container (Document, Element, Declaration: children
/// and attributes, no value) or a leaf (everything else: a value, no
/// children), so the two share storage. `type` never changes and says which
/// member of the union is alive; use the *_of() accessors below unless the
/// kind is known. 64 bytes on 64-bit targets.
struct XmlNodeData {
  struct Tree {
    XmlNodeData* first;
    XmlNodeData* last;
    XmlAttributeData* attributes;
  };
  struct Content {
    const char* data;
    std::size_t size;
  };

  XmlNodeData* parent = nullptr;
  XmlNodeData* prev = nullptr;
  XmlNodeData* next = nullptr;
  const char* name = "";
  union {
    Tree tree = {nullptr, nullptr, nullptr};
    Content content;
  };
  std::uint32_t name_size = 0;
  XmlNodeType type = XmlNodeType::Null;
  std::uint8_t flags = 0;  // parser bookkeeping, see kFlag* below
};

/// Bump allocator: memory is handed out from a few large blocks and released
/// all at once. Nothing stored in it needs a destructor.
class XmlArena {
 public:
  XmlArena() = default;
  XmlArena(const XmlArena&) = delete;
  XmlArena& operator=(const XmlArena&) = delete;
  ~XmlArena() {
    Block* block = head_;
    while (block != nullptr) {
      Block* const next = block->next;
      ::operator delete(block);
      block = next;
    }
  }

  /// `alignment` must be a power of two no larger than alignof(max_align_t).
  [[nodiscard]] void* allocate(std::size_t size, std::size_t alignment) {
    const std::size_t space = static_cast<std::size_t>(limit_ - cursor_);
    const std::size_t padding =
        (alignment - (reinterpret_cast<std::uintptr_t>(cursor_) & (alignment - 1))) & (alignment - 1);
    if (space >= padding && space - padding >= size) {
      char* const result = cursor_ + padding;
      cursor_ = result + size;
      return result;
    }
    return allocate_slow(size, alignment);
  }

  /// Copy `text` into the arena; the empty string needs no storage.
  [[nodiscard]] const char* store(std::string_view text) {
    if (text.empty()) return "";
    char* const copy = static_cast<char*>(allocate(text.size(), 1));
    std::memcpy(copy, text.data(), text.size());
    return copy;
  }

  /// Suggest the size of the next block (the parser knows the input size).
  void hint(std::size_t bytes) noexcept { hint_ = bytes; }
  [[nodiscard]] std::size_t reserved() const noexcept { return reserved_; }

 private:
  struct Block {
    Block* next;
  };
  static constexpr std::size_t kHeader =
      (sizeof(Block) + alignof(std::max_align_t) - 1) & ~(alignof(std::max_align_t) - 1);
  static constexpr std::size_t kMinBlock = 4096 - kHeader;
  static constexpr std::size_t kMaxBlock = (std::size_t{4} << 20) - kHeader;

  [[nodiscard]] static Block* new_block(std::size_t payload) {
    payload = std::min(payload, std::numeric_limits<std::size_t>::max() - kHeader);
    Block* const block = static_cast<Block*>(::operator new(kHeader + payload));
    block->next = nullptr;
    return block;
  }

  [[nodiscard]] void* allocate_slow(std::size_t size, std::size_t alignment) {
    // Blocks double the arena until they reach kMaxBlock.
    std::size_t payload = std::clamp(std::max(reserved_, hint_), kMinBlock, kMaxBlock);
    hint_ = 0;
    const std::size_t needed = size > std::numeric_limits<std::size_t>::max() - alignment ? size : size + alignment;
    if (head_ != nullptr && needed > payload / 4) {
      // A large request gets a block of its own; the current block keeps
      // serving small ones.
      Block* const block = new_block(needed);
      block->next = head_->next;
      head_->next = block;
      reserved_ += needed;
      return align_up(reinterpret_cast<char*>(block) + kHeader, alignment);
    }
    payload = std::max(payload, needed);
    Block* const block = new_block(payload);
    block->next = head_;
    head_ = block;
    reserved_ += payload;
    char* const result = align_up(reinterpret_cast<char*>(block) + kHeader, alignment);
    cursor_ = result + size;
    limit_ = reinterpret_cast<char*>(block) + kHeader + payload;
    return result;
  }

  [[nodiscard]] static char* align_up(char* p, std::size_t alignment) noexcept {
    return p + ((alignment - (reinterpret_cast<std::uintptr_t>(p) & (alignment - 1))) & (alignment - 1));
  }

  Block* head_ = nullptr;
  char* cursor_ = nullptr;
  char* limit_ = nullptr;
  std::size_t reserved_ = 0;
  std::size_t hint_ = 0;
};

struct XmlDocumentData;

/// The Document node additionally knows its document, which is how a handle
/// finds the arena: walk to the root, then follow this pointer.
struct XmlDocumentNode : XmlNodeData {
  XmlDocumentData* document = nullptr;
};

struct XmlDocumentData {
  XmlDocumentData() {
    root.type = XmlNodeType::Document;
    root.document = this;
  }
  XmlDocumentData(const XmlDocumentData&) = delete;
  XmlDocumentData& operator=(const XmlDocumentData&) = delete;

  XmlArena arena;
  std::string source;  ///< the parse buffer: the input as UTF-8, decoded in place
  XmlDocumentNode root;
};

std::string xml_format_signed(long long value) {
  std::array<char, 24> buffer{};
  const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  return ec == std::errc{} ? std::string(buffer.data(), end) : std::string("0");
}

std::string xml_format_unsigned(unsigned long long value) {
  std::array<char, 24> buffer{};
  const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  return ec == std::errc{} ? std::string(buffer.data(), end) : std::string("0");
}

std::string xml_format_float(float value) {
  if (std::isnan(value)) return "nan";
  if (std::isinf(value)) return value < 0 ? "-inf" : "inf";
  std::array<char, 64> buffer{};
  const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  return ec == std::errc{} ? std::string(buffer.data(), end) : std::string("0");
}

std::string xml_format_double(double value) { return format_double(value); }

}  // namespace detail

namespace {

using Node = detail::XmlNodeData;
using Attr = detail::XmlAttributeData;
using Arena = detail::XmlArena;
using DocumentData = detail::XmlDocumentData;

// Node::flags, used while parsing only.
constexpr std::uint8_t kFlagHasText = 1;   // the element directly contains non-white-space character data
constexpr std::uint8_t kFlagPreserve = 2;  // white space is significant here (xml:space or option)

// ===========================================================================
// 2. Character classes and scanning helpers
// ===========================================================================

enum : std::uint8_t {
  kSpace = 1,        // XML white space: space, tab, LF, CR
  kNameStart = 2,    // ASCII NameStartChar
  kNameChar = 4,     // ASCII NameChar
  kTextStop = 8,     // ends a run of plain character data: '<', '&', NUL
  kAttrStopDq = 16,  // ends a plain run inside "...": '"', '&', '<', tab, LF, NUL
  kAttrStopSq = 32,  // the same inside '...'
  kEscapeText = 64,  // needs escaping in element content
  kEscapeAttr = 128, // needs escaping in an attribute value
};

constexpr std::array<std::uint8_t, 256> make_class_table() {
  std::array<std::uint8_t, 256> table{};
  const auto add = [&](char c, std::uint8_t bits) {
    table[static_cast<unsigned char>(c)] = static_cast<std::uint8_t>(table[static_cast<unsigned char>(c)] | bits);
  };
  for (const char c : {' ', '\t', '\n', '\r'}) add(c, kSpace);
  for (char c = 'a'; c <= 'z'; ++c) add(c, kNameStart | kNameChar);
  for (char c = 'A'; c <= 'Z'; ++c) add(c, kNameStart | kNameChar);
  for (char c = '0'; c <= '9'; ++c) add(c, kNameChar);
  add('_', kNameStart | kNameChar);
  add(':', kNameStart | kNameChar);
  add('-', kNameChar);
  add('.', kNameChar);
  for (const char c : {'<', '&', '\0'}) add(c, kTextStop | kAttrStopDq | kAttrStopSq);
  for (const char c : {'\t', '\n'}) add(c, kAttrStopDq | kAttrStopSq);
  add('"', kAttrStopDq);
  add('\'', kAttrStopSq);
  for (const char c : {'&', '<', '>', '\r'}) add(c, kEscapeText | kEscapeAttr);
  for (const char c : {'"', '\t', '\n'}) add(c, kEscapeAttr);
  return table;
}
inline constexpr std::array<std::uint8_t, 256> kClass = make_class_table();

[[nodiscard]] constexpr std::uint8_t byte_of(char c) noexcept { return static_cast<std::uint8_t>(c); }
[[nodiscard]] constexpr bool is_space(char c) noexcept { return (kClass[byte_of(c)] & kSpace) != 0; }
[[nodiscard]] constexpr std::size_t distance(const char* from, const char* to) noexcept {
  return static_cast<std::size_t>(to - from);
}
[[nodiscard]] constexpr std::string_view view(const char* from, const char* to) noexcept {
  return {from, static_cast<std::size_t>(to - from)};
}

/// Names and attribute values store their size in 32 bits.
[[nodiscard]] constexpr bool fits_u32(std::size_t size) noexcept {
  return static_cast<std::uint64_t>(size) <= 0xFFFFFFFFull;
}

[[nodiscard]] constexpr std::string_view trim_xml_space(std::string_view text) noexcept {
  while (!text.empty() && is_space(text.front())) text.remove_prefix(1);
  while (!text.empty() && is_space(text.back())) text.remove_suffix(1);
  return text;
}

/// XML 1.0 `Char`.
[[nodiscard]] constexpr bool is_xml_char(char32_t c) noexcept {
  return c == 0x9 || c == 0xA || c == 0xD || (c >= 0x20 && c <= 0xD7FF) || (c >= 0xE000 && c <= 0xFFFD) ||
         (c >= 0x10000 && c <= 0x10FFFF);
}

/// XML 1.0 fifth edition `NameStartChar` / `NameChar`, non-ASCII part.
[[nodiscard]] constexpr bool is_name_start_high(char32_t c) noexcept {
  return (c >= 0xC0 && c <= 0xD6) || (c >= 0xD8 && c <= 0xF6) || (c >= 0xF8 && c <= 0x2FF) ||
         (c >= 0x370 && c <= 0x37D) || (c >= 0x37F && c <= 0x1FFF) || (c >= 0x200C && c <= 0x200D) ||
         (c >= 0x2070 && c <= 0x218F) || (c >= 0x2C00 && c <= 0x2FEF) || (c >= 0x3001 && c <= 0xD7FF) ||
         (c >= 0xF900 && c <= 0xFDCF) || (c >= 0xFDF0 && c <= 0xFFFD) || (c >= 0x10000 && c <= 0xEFFFF);
}
[[nodiscard]] constexpr bool is_name_char_high(char32_t c) noexcept {
  return is_name_start_high(c) || c == 0xB7 || (c >= 0x300 && c <= 0x36F) || (c >= 0x203F && c <= 0x2040);
}

struct Decoded {
  char32_t code_point;
  unsigned length;
};

/// Decode one scalar value from text that is known to be valid UTF-8.
[[nodiscard]] inline Decoded decode_valid(const char* p) noexcept {
  const char32_t b0 = byte_of(p[0]);
  if (b0 < 0x80) return {b0, 1};
  const char32_t b1 = byte_of(p[1]) & 0x3Fu;
  if (b0 < 0xE0) return {((b0 & 0x1Fu) << 6) | b1, 2};
  const char32_t b2 = byte_of(p[2]) & 0x3Fu;
  if (b0 < 0xF0) return {((b0 & 0x0Fu) << 12) | (b1 << 6) | b2, 3};
  const char32_t b3 = byte_of(p[3]) & 0x3Fu;
  return {((b0 & 0x07u) << 18) | (b1 << 12) | (b2 << 6) | b3, 4};
}

/// Write one scalar value as UTF-8 at `out`; returns the new end.
[[nodiscard]] inline char* put_utf8(char* out, char32_t cp) noexcept {
  if (cp < 0x80) {
    *out++ = static_cast<char>(cp);
  } else if (cp < 0x800) {
    *out++ = static_cast<char>(0xC0 | (cp >> 6));
    *out++ = static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    *out++ = static_cast<char>(0xE0 | (cp >> 12));
    *out++ = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    *out++ = static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    *out++ = static_cast<char>(0xF0 | (cp >> 18));
    *out++ = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    *out++ = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    *out++ = static_cast<char>(0x80 | (cp & 0x3F));
  }
  return out;
}

/// End of the XML Name starting at `p` (`p` itself when there is none).
/// The text must be valid UTF-8 and end with a NUL, which stops the scan.
template <class Pointer>
[[nodiscard]] inline Pointer scan_name(Pointer p) noexcept {
  const std::uint8_t first = byte_of(*p);
  if ((kClass[first] & kNameStart) != 0) {
    ++p;
  } else if (first >= 0x80) {
    const Decoded d = decode_valid(p);
    if (!is_name_start_high(d.code_point)) return p;
    p += d.length;
  } else {
    return p;
  }
  for (;;) {
    while ((kClass[byte_of(*p)] & kNameChar) != 0) ++p;
    if (byte_of(*p) < 0x80) return p;
    const Decoded d = decode_valid(p);
    if (!is_name_char_high(d.code_point)) return p;
    p += d.length;
  }
}

/// The same for a range of valid UTF-8 without a terminator.
[[nodiscard]] const char* scan_name(const char* p, const char* end) noexcept {
  if (p == end) return p;
  const std::uint8_t first = byte_of(*p);
  if ((kClass[first] & kNameStart) != 0) {
    ++p;
  } else if (first >= 0x80) {
    const Decoded d = decode_valid(p);
    if (!is_name_start_high(d.code_point)) return p;
    p += d.length;
  } else {
    return p;
  }
  while (p != end) {
    const std::uint8_t c = byte_of(*p);
    if ((kClass[c] & kNameChar) != 0) {
      ++p;
    } else if (c >= 0x80) {
      const Decoded d = decode_valid(p);
      if (!is_name_char_high(d.code_point)) return p;
      p += d.length;
    } else {
      return p;
    }
  }
  return p;
}

template <class Pointer>
[[nodiscard]] inline Pointer skip_space(Pointer p) noexcept {
  while (is_space(*p)) ++p;
  return p;
}

/// True when the text at `p` starts with `literal`. Compares byte by byte and
/// stops at the first difference, so it never reads past a NUL terminator.
template <std::size_t N>
[[nodiscard]] inline bool match(const char* p, const char (&literal)[N]) noexcept {
  for (std::size_t i = 0; i + 1 < N; ++i) {
    if (p[i] != literal[i]) return false;
  }
  return true;
}

/// First position in [p, end) that is not an ordinary character: malformed
/// UTF-8, a character XML forbids, or (with `stop_at_cr`) a carriage return.
/// Plain ASCII is skipped a machine word at a time.
[[nodiscard]] const char* find_special(const char* p, const char* end, bool stop_at_cr) noexcept {
  constexpr std::uint64_t kHigh = 0x8080808080808080ull;
  constexpr std::uint64_t kLow = 0x2020202020202020ull;
  while (p != end) {
    while (distance(p, end) >= 8) {
      std::uint64_t word = 0;
      std::memcpy(&word, p, 8);
      // Any byte >= 0x80, or any byte < 0x20?
      if (((word & kHigh) | ((word - kLow) & ~word & kHigh)) != 0) break;
      p += 8;
    }
    // Step over at least one word's worth of bytes one at a time.
    const char* const stretch = distance(p, end) >= 8 ? p + 8 : end;
    while (p < stretch) {
      const std::uint8_t c = byte_of(*p);
      if (c >= 0x20 && c < 0x80) {
        ++p;
      } else if (c == '\t' || c == '\n' || (c == '\r' && !stop_at_cr)) {
        ++p;
      } else if (c < 0x80) {
        return p;
      } else {
        const Utf8Decoded d = decode_utf8(view(p, end), 0);
        if (!d.ok || d.code_point == 0xFFFE || d.code_point == 0xFFFF) return p;
        p += d.length;
      }
    }
  }
  return end;
}

/// Human-readable description of the offending character at `p`.
[[nodiscard]] std::string describe_bad_character(const char* p, const char* end) {
  const Utf8Decoded d = decode_utf8(view(p, end), 0);
  if (!d.ok) return "malformed UTF-8 byte sequence";
  return std::format("character U+{:04X} is not allowed in XML", static_cast<std::uint32_t>(d.code_point));
}

/// `text` shortened for use in an error message (never splits a character).
[[nodiscard]] std::string clip(std::string_view text) {
  constexpr std::size_t kMax = 48;
  if (text.size() <= kMax) return std::string(text);
  std::size_t cut = kMax;
  while (cut > 0 && (byte_of(text[cut]) & 0xC0) == 0x80) --cut;
  return std::string(text.substr(0, cut)) + "...";
}

/// `VersionNum` of the XML declaration: "1." and digits.
[[nodiscard]] constexpr bool is_version_number(std::string_view text) noexcept {
  if (text.size() < 3 || !text.starts_with("1.")) return false;
  for (const char c : text.substr(2)) {
    if (!is_ascii_digit(c)) return false;
  }
  return true;
}

/// `EncName` of the XML declaration: a letter, then letters, digits, '.', '_' or '-'.
[[nodiscard]] constexpr bool is_encoding_name(std::string_view text) noexcept {
  if (text.empty() || !is_ascii_alpha(text.front())) return false;
  for (const char c : text) {
    if (!is_ascii_alpha(c) && !is_ascii_digit(c) && c != '.' && c != '_' && c != '-') return false;
  }
  return true;
}

[[nodiscard]] bool is_name(std::string_view text) noexcept {
  if (text.empty() || !fits_u32(text.size())) return false;
  std::size_t i = 0;
  while (i < text.size()) {
    const Utf8Decoded d = decode_utf8(text, i);
    if (!d.ok) return false;
    const char32_t c = d.code_point;
    const bool ok = c < 0x80 ? (kClass[c] & (i == 0 ? kNameStart : kNameChar)) != 0
                             : (i == 0 ? is_name_start_high(c) : is_name_char_high(c));
    if (!ok) return false;
    i += d.length;
  }
  return true;
}

// ===========================================================================
// 3. Tree primitives
// ===========================================================================

[[nodiscard]] constexpr bool has_tree(XmlNodeType type) noexcept {
  return type == XmlNodeType::Element || type == XmlNodeType::Document || type == XmlNodeType::Declaration;
}
[[nodiscard]] inline Node* first_child_of(const Node* node) noexcept {
  return has_tree(node->type) ? node->tree.first : nullptr;
}
[[nodiscard]] inline Node* last_child_of(const Node* node) noexcept {
  return has_tree(node->type) ? node->tree.last : nullptr;
}
[[nodiscard]] inline Attr* attributes_of(const Node* node) noexcept {
  return has_tree(node->type) ? node->tree.attributes : nullptr;
}
[[nodiscard]] inline std::string_view name_of(const Node* node) noexcept { return {node->name, node->name_size}; }
[[nodiscard]] inline std::string_view value_of(const Node* node) noexcept {
  return has_tree(node->type) ? std::string_view() : std::string_view(node->content.data, node->content.size);
}
[[nodiscard]] inline std::string_view name_of(const Attr* attr) noexcept { return {attr->name, attr->name_size}; }
[[nodiscard]] inline std::string_view value_of(const Attr* attr) noexcept { return {attr->value, attr->value_size}; }

inline void assign_name(Node* node, std::string_view name) noexcept {
  node->name = name.data();
  node->name_size = static_cast<std::uint32_t>(name.size());
}
/// Leaves only.
inline void assign_value(Node* node, std::string_view value) noexcept {
  node->content = {value.data(), value.size()};
}

[[nodiscard]] inline bool is_text_type(XmlNodeType type) noexcept {
  return type == XmlNodeType::Text || type == XmlNodeType::CData;
}

[[nodiscard]] inline Node* new_node(Arena& arena, XmlNodeType type) {
  Node* const node = ::new (arena.allocate(sizeof(Node), alignof(Node))) Node;
  node->type = type;
  if (!has_tree(type)) node->content = {"", 0};
  return node;
}

/// `name` and `value` must satisfy fits_u32().
[[nodiscard]] inline Attr* new_attribute(Arena& arena, Node* owner, std::string_view name, std::string_view value) {
  Attr* const attr = ::new (arena.allocate(sizeof(Attr), alignof(Attr))) Attr;
  attr->owner = owner;
  attr->name = name.data();
  attr->name_size = static_cast<std::uint32_t>(name.size());
  attr->value = value.data();
  attr->value_size = static_cast<std::uint32_t>(value.size());
  return attr;
}

/// Insert `node` under `parent` between the adjacent siblings `before` and
/// `after` (either may be null for the ends).
inline void link_between(Node* parent, Node* before, Node* after, Node* node) noexcept {
  node->parent = parent;
  node->prev = before;
  node->next = after;
  if (before != nullptr) {
    before->next = node;
  } else {
    parent->tree.first = node;
  }
  if (after != nullptr) {
    after->prev = node;
  } else {
    parent->tree.last = node;
  }
}

inline void link_last(Node* parent, Node* node) noexcept { link_between(parent, parent->tree.last, nullptr, node); }

inline void unlink(Node* node) noexcept {
  Node* const parent = node->parent;
  if (parent == nullptr) return;
  if (node->prev != nullptr) {
    node->prev->next = node->next;
  } else {
    parent->tree.first = node->next;
  }
  if (node->next != nullptr) {
    node->next->prev = node->prev;
  } else {
    parent->tree.last = node->prev;
  }
  node->parent = nullptr;
  node->prev = nullptr;
  node->next = nullptr;
}

inline void append_attribute(Node* owner, Attr* attr) noexcept {
  if (owner->tree.attributes == nullptr) {
    owner->tree.attributes = attr;
    return;
  }
  Attr* tail = owner->tree.attributes;
  while (tail->next != nullptr) tail = tail->next;
  tail->next = attr;
}

[[nodiscard]] Attr* find_attribute(const Node* node, std::string_view name) noexcept {
  for (Attr* a = attributes_of(node); a != nullptr; a = a->next) {
    if (name_of(a) == name) return a;
  }
  return nullptr;
}

/// The document that owns `node`, or null for null / detached nodes.
[[nodiscard]] DocumentData* document_of(Node* node) noexcept {
  if (node == nullptr) return nullptr;
  while (node->parent != nullptr) node = node->parent;
  if (node->type != XmlNodeType::Document) return nullptr;
  return static_cast<detail::XmlDocumentNode*>(node)->document;
}

/// Namespace bound to `prefix` at `element` ("" = default namespace).
[[nodiscard]] std::string_view resolve_namespace(const Node* element, std::string_view prefix) noexcept {
  if (prefix == "xml") return xml_namespace_uri;
  if (prefix == "xmlns") return xmlns_namespace_uri;
  for (const Node* n = element; n != nullptr && n->type == XmlNodeType::Element; n = n->parent) {
    for (const Attr* a = n->tree.attributes; a != nullptr; a = a->next) {
      const std::string_view name = name_of(a);
      if (prefix.empty()) {
        if (name == "xmlns") return value_of(a);
      } else if (name.size() == prefix.size() + 6 && name.starts_with("xmlns:") && name.substr(6) == prefix) {
        return value_of(a);
      }
    }
  }
  return {};
}

[[nodiscard]] std::string_view namespace_of(const Node* node) noexcept {
  if (node->type != XmlNodeType::Element) return {};
  return resolve_namespace(node, xml_prefix(name_of(node)));
}

[[nodiscard]] std::string_view namespace_of(const Attr* attr) noexcept {
  const std::string_view prefix = xml_prefix(name_of(attr));
  if (prefix.empty()) return {};
  return resolve_namespace(attr->owner, prefix);
}

/// Pre-order successor of `node` inside the subtree of `root`; null at the end.
[[nodiscard]] inline Node* next_in_subtree(const Node* root, Node* node) noexcept {
  if (Node* const child = first_child_of(node); child != nullptr) return child;
  while (node != root && node->next == nullptr) node = node->parent;
  return node == root ? nullptr : node->next;
}

/// Deep copy of the children of `source` (and their subtrees) below
/// `target`, allocating from `arena`. Iterative.
void copy_children(Arena& arena, const Node* source, Node* target) {
  const Node* s = first_child_of(source);
  Node* parent = target;
  while (s != nullptr) {
    Node* const copy = new_node(arena, s->type);
    assign_name(copy, std::string_view(arena.store(name_of(s)), s->name_size));
    if (!has_tree(s->type)) assign_value(copy, std::string_view(arena.store(value_of(s)), s->content.size));
    Attr* tail = nullptr;
    for (const Attr* a = attributes_of(s); a != nullptr; a = a->next) {
      Attr* const attr = new_attribute(arena, copy, std::string_view(arena.store(name_of(a)), a->name_size),
                                       std::string_view(arena.store(value_of(a)), a->value_size));
      (tail != nullptr ? tail->next : copy->tree.attributes) = attr;
      tail = attr;
    }
    link_last(parent, copy);
    if (first_child_of(s) != nullptr) {
      parent = copy;
      s = s->tree.first;
      continue;
    }
    while (s != source && s->next == nullptr) {
      s = s->parent;
      if (s != source) parent = parent->parent;
    }
    if (s == source) break;
    s = s->next;
  }
}

// ===========================================================================
// 4. Input preparation
// ===========================================================================

struct Source {
  enum class Kind { Utf8, Ascii, Other };
  Kind kind = Kind::Utf8;
  TextEncoding encoding = TextEncoding::Utf8;  // for Kind::Other
  std::size_t bom = 0;                         // bytes of byte order mark
};

/// The `encoding` label of an XML declaration at the start of
/// ASCII-compatible `text`, or "" when there is none. Lenient: the real
/// declaration parser reports syntax errors later.
[[nodiscard]] std::string_view sniff_declared_encoding(std::string_view text) noexcept {
  if (!text.starts_with("<?xml") || text.size() < 6 || !is_space(text[5])) return {};
  std::size_t i = 5;
  const auto skip = [&] {
    while (i < text.size() && is_space(text[i])) ++i;
  };
  for (;;) {
    skip();
    const std::size_t name_begin = i;
    while (i < text.size() && is_ascii_alpha(text[i])) ++i;
    const std::string_view name = text.substr(name_begin, i - name_begin);
    if (name.empty()) return {};
    skip();
    if (i >= text.size() || text[i] != '=') return {};
    ++i;
    skip();
    if (i >= text.size() || (text[i] != '"' && text[i] != '\'')) return {};
    const char quote = text[i++];
    const std::size_t value_begin = i;
    while (i < text.size() && text[i] != quote) ++i;
    if (i >= text.size()) return {};
    if (name == "encoding") return text.substr(value_begin, i - value_begin);
    ++i;
  }
}

[[nodiscard]] Result<Source> detect_source(ByteView data) {
  Source source;
  if (const auto bom = detect_bom(data)) {
    source.kind = bom->encoding == TextEncoding::Utf8 ? Source::Kind::Utf8 : Source::Kind::Other;
    source.encoding = bom->encoding;
    source.bom = bom->length;
    return source;
  }
  if (data.size() >= 4) {
    // No mark: UTF-16/32 shows as zero bytes around the first '<'.
    const auto b = [&](std::size_t i) { return std::to_integer<unsigned>(data[i]); };
    std::optional<TextEncoding> wide;
    if (b(0) == 0 && b(1) == 0 && b(2) == 0 && b(3) == '<') {
      wide = TextEncoding::Utf32BE;
    } else if (b(0) == '<' && b(1) == 0 && b(2) == 0 && b(3) == 0) {
      wide = TextEncoding::Utf32LE;
    } else if (b(0) == 0 && b(1) == '<' && b(2) == 0 && b(3) != 0) {
      wide = TextEncoding::Utf16BE;
    } else if (b(0) == '<' && b(1) == 0 && b(2) != 0 && b(3) == 0) {
      wide = TextEncoding::Utf16LE;
    }
    if (wide) {
      source.kind = Source::Kind::Other;
      source.encoding = *wide;
      return source;
    }
  }
  const std::string_view label = sniff_declared_encoding(as_chars(data));
  if (label.empty()) return source;
  if (!is_encoding_name(label)) {
    return fail(Status::Decode, "invalid XML: malformed encoding name in the XML declaration");
  }
  if (iequals(label, "us-ascii") || iequals(label, "ascii")) {
    source.kind = Source::Kind::Ascii;
    return source;
  }
  const std::optional<TextEncoding> encoding = parse_encoding(label);
  if (!encoding) return fail(Status::Decode, "invalid XML: unsupported encoding '{}'", clip(label));
  switch (*encoding) {
    case TextEncoding::Utf8: return source;
    case TextEncoding::Latin1:
    case TextEncoding::Windows1252:
      source.kind = Source::Kind::Other;
      source.encoding = *encoding;
      return source;
    case TextEncoding::Utf16LE:
    case TextEncoding::Utf16BE:
    case TextEncoding::Utf32LE:
    case TextEncoding::Utf32BE: break;
  }
  return fail(Status::Decode, "invalid XML: the declared encoding '{}' does not match the bytes of the document",
              clip(label));
}

/// The input as UTF-8 with every undecodable unit replaced by U+FFFD; for
/// UTF-8 input, the bytes after the mark as they are. This is the text that
/// line and column numbers refer to.
[[nodiscard]] std::string decode_lenient(ByteView data, const Source& source) {
  switch (source.kind) {
    case Source::Kind::Utf8: return std::string(as_chars(data.subspan(source.bom)));
    case Source::Kind::Ascii: {
      std::string out;
      out.reserve(data.size());
      for (const char c : as_chars(data)) {
        if (byte_of(c) < 0x80) {
          out.push_back(c);
        } else {
          out.append("\xEF\xBF\xBD");
        }
      }
      return out;
    }
    case Source::Kind::Other: break;
  }
  return decode_text(data, source.encoding).text;
}

/// Offset of the first unit of `data` that does not decode in the given
/// UTF-16 / UTF-32 encoding (data.size() when all of it does).
[[nodiscard]] std::size_t first_undecodable(ByteView data, const Source& source) noexcept {
  const auto b = [&](std::size_t i) { return std::to_integer<char32_t>(data[i]); };
  std::size_t i = source.bom;
  switch (source.encoding) {
    case TextEncoding::Utf16LE:
    case TextEncoding::Utf16BE: {
      const bool little = source.encoding == TextEncoding::Utf16LE;
      const auto unit = [&](std::size_t at) {
        return little ? (b(at) | (b(at + 1) << 8)) : ((b(at) << 8) | b(at + 1));
      };
      while (i + 1 < data.size()) {
        const char32_t u = unit(i);
        if (u >= 0xD800 && u <= 0xDBFF) {
          if (i + 3 >= data.size()) return i;
          const char32_t low = unit(i + 2);
          if (low < 0xDC00 || low > 0xDFFF) return i;
          i += 4;
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
          return i;
        } else {
          i += 2;
        }
      }
      return i;
    }
    case TextEncoding::Utf32LE:
    case TextEncoding::Utf32BE: {
      const bool little = source.encoding == TextEncoding::Utf32LE;
      while (i + 3 < data.size()) {
        const char32_t cp = little ? (b(i) | (b(i + 1) << 8) | (b(i + 2) << 16) | (b(i + 3) << 24))
                                   : ((b(i) << 24) | (b(i + 1) << 16) | (b(i + 2) << 8) | b(i + 3));
        if (!is_scalar_value(cp)) return i;
        i += 4;
      }
      return i;
    }
    case TextEncoding::Utf8:
    case TextEncoding::Latin1:
    case TextEncoding::Windows1252: break;
  }
  return data.size();
}

struct BadText {
  bool found = false;
  std::size_t offset = 0;  // in the normalised text
  std::string reason;
};

/// Prepare the parse buffer in place: CRLF and lone CR become LF, and the
/// text is checked to be valid UTF-8 made of legal XML characters. In strict
/// mode the text is cut at the first offence, which is described in `bad`
/// (the parser reports it when it gets there); otherwise every offence is
/// replaced by U+FFFD.
void normalise_source(std::string& text, bool replace, BadText& bad) {
  char* const begin = text.data();
  const char* const end = begin + text.size();
  char* out = begin;
  const char* p = begin;
  for (;;) {
    const char* const stop = find_special(p, end, true);
    const std::size_t plain = distance(p, stop);
    if (out != p) std::memmove(out, p, plain);
    out += plain;
    p = stop;
    if (p == end) break;
    if (*p == '\r') {
      *out++ = '\n';
      ++p;
      if (p != end && *p == '\n') ++p;
      continue;
    }
    if (!replace) {
      bad.found = true;
      bad.offset = distance(begin, out);
      bad.reason = describe_bad_character(p, end);
      break;
    }
    // Replacements can make the text longer, so finish in a new buffer.
    std::string rebuilt;
    rebuilt.reserve(text.size() + 64);
    rebuilt.append(begin, distance(begin, out));
    while (p != end) {
      const char* const next = find_special(p, end, true);
      rebuilt.append(p, distance(p, next));
      p = next;
      if (p == end) break;
      if (*p == '\r') {
        rebuilt.push_back('\n');
        ++p;
        if (p != end && *p == '\n') ++p;
        continue;
      }
      rebuilt.append("\xEF\xBF\xBD");
      p += decode_utf8(view(p, end), 0).length;
    }
    text = std::move(rebuilt);
    return;
  }
  text.resize(distance(begin, out));
}

/// Inverse of normalise_source for error reporting: the offset in `pristine`
/// (the text before normalisation) that corresponds to `offset` in the
/// normalised text.
[[nodiscard]] std::size_t map_offset(std::string_view pristine, std::size_t offset, bool replace) noexcept {
  std::size_t in = 0;
  std::size_t out = 0;
  while (out < offset && in < pristine.size()) {
    if (pristine[in] == '\r') {
      in += (in + 1 < pristine.size() && pristine[in + 1] == '\n') ? 2 : 1;
      out += 1;
      continue;
    }
    const Utf8Decoded d = decode_utf8(pristine, in);
    const bool legal = d.ok && is_xml_char(d.code_point) && d.code_point != 0xFFFE && d.code_point != 0xFFFF;
    if (legal) {
      out += d.length;
    } else if (replace) {
      out += 3;
    } else {
      break;  // strict mode never reads past the first offence
    }
    in += d.length;
  }
  return in;
}

struct Position {
  std::size_t line = 1;
  std::size_t column = 1;
};

/// Line and column (1-based, in characters) of the position just after `prefix`.
[[nodiscard]] Position position_after(std::string_view prefix) noexcept {
  Position position;
  std::size_t line_start = 0;
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    const char c = prefix[i];
    if (c == '\n' || (c == '\r' && !(i + 1 < prefix.size() && prefix[i + 1] == '\n'))) {
      ++position.line;
      line_start = i + 1;
    }
  }
  position.column = utf8_length(prefix.substr(line_start)) + 1;
  return position;
}

/// Size in bytes that `prefix` (decoded text) occupied in the original input.
[[nodiscard]] std::size_t original_size(const Source& source, std::string_view prefix) noexcept {
  if (source.kind == Source::Kind::Utf8) return source.bom + prefix.size();
  if (source.kind == Source::Kind::Ascii) return utf8_length(prefix);
  switch (source.encoding) {
    case TextEncoding::Utf16LE:
    case TextEncoding::Utf16BE: {
      std::size_t bytes = source.bom;
      for (std::size_t i = 0; i < prefix.size();) {
        const Utf8Decoded d = decode_utf8(prefix, i);
        bytes += d.code_point >= 0x10000 ? 4 : 2;
        i += d.length;
      }
      return bytes;
    }
    case TextEncoding::Utf32LE:
    case TextEncoding::Utf32BE: return source.bom + 4 * utf8_length(prefix);
    case TextEncoding::Utf8:
    case TextEncoding::Latin1:
    case TextEncoding::Windows1252: break;
  }
  return utf8_length(prefix);
}

[[nodiscard]] std::unexpected<Error> located_error(Status code, std::string_view prefix, std::size_t byte,
                                                   std::string_view reason) {
  const Position position = position_after(prefix);
  return fail(code, "invalid XML at line {}, column {} (byte {}): {}", position.line, position.column, byte, reason);
}

// ===========================================================================
// 5. Parser
// ===========================================================================

/// One `&...;` construct.
struct Reference {
  enum class Kind { Malformed, Character, Named };
  Kind kind = Kind::Malformed;
  bool numeric = false;      // Character: written as &#...; (not a predefined entity)
  bool at_end = false;       // Malformed: the input ended inside the reference
  char32_t code_point = 0;   // Character
  std::string_view name;     // Named
  const char* end = nullptr; // just after the ';'
};

/// Read the reference starting at `p` (which points at '&') within [p, limit).
[[nodiscard]] Reference read_reference(const char* p, const char* limit) noexcept {
  Reference ref;
  const char* q = p + 1;
  if (q != limit && *q == '#') {
    ++q;
    char32_t cp = 0;
    bool digits = false;
    const auto add = [&](char32_t base, char32_t digit) {
      if (cp < 0x110000) cp = cp * base + digit;  // saturates above the Unicode range
      digits = true;
    };
    if (q != limit && *q == 'x') {
      ++q;
      for (; q != limit; ++q) {
        const char c = *q;
        if (c >= '0' && c <= '9') {
          add(16, static_cast<char32_t>(c - '0'));
        } else if (c >= 'a' && c <= 'f') {
          add(16, static_cast<char32_t>(c - 'a' + 10));
        } else if (c >= 'A' && c <= 'F') {
          add(16, static_cast<char32_t>(c - 'A' + 10));
        } else {
          break;
        }
      }
    } else {
      for (; q != limit && *q >= '0' && *q <= '9'; ++q) add(10, static_cast<char32_t>(*q - '0'));
    }
    if (!digits || q == limit || *q != ';') {
      ref.at_end = q == limit;
      return ref;
    }
    ref.kind = Reference::Kind::Character;
    ref.numeric = true;
    ref.code_point = cp;
    ref.end = q + 1;
    return ref;
  }
  const char* const name_end = scan_name(q, limit);
  if (name_end == q || name_end == limit || *name_end != ';') {
    ref.at_end = name_end == limit;
    return ref;
  }
  const std::string_view name = view(q, name_end);
  ref.end = name_end + 1;
  ref.kind = Reference::Kind::Character;
  if (name == "lt") {
    ref.code_point = U'<';
  } else if (name == "gt") {
    ref.code_point = U'>';
  } else if (name == "amp") {
    ref.code_point = U'&';
  } else if (name == "apos") {
    ref.code_point = U'\'';
  } else if (name == "quot") {
    ref.code_point = U'"';
  } else {
    ref.kind = Reference::Kind::Named;
    ref.name = name;
  }
  return ref;
}

class Parser {
 public:
  Parser(DocumentData& document, const XmlParseOptions& options, const BadText& bad)
      : arena_(document.arena),
        root_(&document.root),
        options_(options),
        bad_(bad),
        begin_(document.source.data()),
        end_(document.source.data() + document.source.size()) {}

  /// Builds the tree; on failure returns false and leaves the error in
  /// `code`, `offset` (normalised text) and `message`.
  [[nodiscard]] bool run();

  Status code = Status::Ok;
  std::size_t offset = 0;
  std::string message;

 private:
  static constexpr int kMaxEntityDepth = 4;
  static constexpr std::size_t kLinearDuplicateCheck = 8;

  struct Entity {
    std::string_view name;
    std::string_view value;  // replacement text
    bool usable = false;     // internal, parsed, free of markup
  };
  /// A white-space-only run whose fate is decided when more of its parent
  /// has been seen.
  struct PendingSpace {
    Node* parent;
    Node* after;  // the sibling it follows (null: first child)
    std::string_view text;
  };
  enum class Decode { Done, Failed, Entity };

  // --- errors ---------------------------------------------------------------
  bool error(const char* at, std::string text) {
    code = Status::Decode;
    offset = distance(begin_, at);
    message = std::move(text);
    return false;
  }
  bool limit(const char* at, std::string text) {
    code = Status::Limit;
    offset = distance(begin_, at);
    message = std::move(text);
    return false;
  }
  /// The input ended where more was required. When the buffer was cut short
  /// at an illegal character, that character is the real problem.
  bool end_error(std::string text) { return error(end_, bad_.found ? bad_.reason : std::move(text)); }
  /// `error` at `at`, or `end_error` when `at` is the end of the input.
  bool error_or_end(const char* at, std::string text) {
    return at == end_ ? end_error("unexpected end of input: " + text) : error(at, std::move(text));
  }

  // --- tree building --------------------------------------------------------
  [[nodiscard]] Node* add_node(XmlNodeType type, Node* parent, const char* at) {
    if (node_count_ >= options_.max_nodes) {
      limit(at, std::format("more than {} nodes (max_nodes)", options_.max_nodes));
      return nullptr;
    }
    ++node_count_;
    Node* const node = new_node(arena_, type);
    link_last(parent, node);
    return node;
  }
  [[nodiscard]] bool add_text(Node* parent, std::string_view text, const char* at) {
    Node* const node = add_node(XmlNodeType::Text, parent, at);
    if (node == nullptr) return false;
    assign_value(node, text);
    return true;
  }
  [[nodiscard]] bool settle_pending(Node* parent, bool keep, const char* at);
  [[nodiscard]] bool mark_has_text(Node* element, const char* at) {
    if ((element->flags & kFlagHasText) != 0) return true;
    element->flags = static_cast<std::uint8_t>(element->flags | kFlagHasText);
    return settle_pending(element, true, at);
  }

  // --- pieces of the grammar -----------------------------------------------
  [[nodiscard]] bool parse_declaration(char*& p);
  [[nodiscard]] bool parse_doctype(char*& p);
  [[nodiscard]] bool parse_internal_subset(char*& q);
  [[nodiscard]] bool parse_entity_declaration(char*& q);
  [[nodiscard]] bool skip_comment(char*& q, std::string_view* content);
  [[nodiscard]] bool skip_pi(char*& q, std::string_view* target, std::string_view* content);
  [[nodiscard]] bool skip_markup_declaration(char*& q);
  [[nodiscard]] bool parse_start_tag(char*& p);
  [[nodiscard]] bool parse_end_tag(char*& p);
  [[nodiscard]] bool parse_text(char*& p, char* start, std::string_view& value);
  [[nodiscard]] bool parse_attribute_value(char*& p, char quote, std::string_view& value);
  [[nodiscard]] bool check_duplicate_attributes(Node* element);

  // --- references -----------------------------------------------------------
  [[nodiscard]] const Entity* find_entity(std::string_view name) const noexcept;
  [[nodiscard]] Decode decode_reference(char*& p, char*& out);
  [[nodiscard]] bool expand_tail(const char* start, const char* decoded_end, char*& p, char quote,
                                 std::string_view& value);
  [[nodiscard]] bool append_decoded(std::string& out, const char* p, const char* end, bool attribute, int depth,
                                    const char* report_at);

  Arena& arena_;
  Node* const root_;
  const XmlParseOptions& options_;
  const BadText& bad_;
  char* const begin_;
  char* const end_;  // *end_ == '\0'

  Node* current_ = nullptr;  // innermost open element, or the document
  std::size_t depth_ = 0;
  std::size_t node_count_ = 0;
  bool seen_root_ = false;
  bool seen_doctype_ = false;
  std::vector<PendingSpace> pending_;
  std::vector<Attr*> attribute_scratch_;
  std::vector<Entity> entities_;
  std::string scratch_;
  std::size_t expansion_used_ = 0;
};

bool Parser::run() {
  char* p = begin_;
  current_ = root_;
  if (options_.preserve_whitespace) root_->flags = kFlagPreserve;

  if (match(p, "<?xml") && (is_space(p[5]) || p[5] == '?')) {
    if (!parse_declaration(p)) return false;
  }

  for (;;) {
    if (current_ == root_) {
      p = skip_space(p);
      if (*p != '<') {
        if (p == end_) break;
        return error(p, "character data outside the root element");
      }
    } else {
      char* const start = p;
      p = skip_space(p);
      if (*p != '<') {
        if (p != end_) {
          std::string_view value;
          if (!parse_text(p, start, value)) return false;
          if (!mark_has_text(current_, start) || !add_text(current_, value, start)) return false;
        }
        if (*p != '<') {
          return end_error(std::format("unexpected end of input: <{}> is not closed", clip(name_of(current_))));
        }
      } else if (p != start) {
        const std::string_view space = view(start, p);
        if ((current_->flags & (kFlagHasText | kFlagPreserve)) != 0) {
          if (!add_text(current_, space, start)) return false;
        } else {
          pending_.push_back({current_, current_->tree.last, space});
        }
      }
    }

    // `p` is at '<'.
    const char next = p[1];
    if (next == '/') {
      if (!parse_end_tag(p)) return false;
    } else if (next == '?') {
      char* const at = p;
      std::string_view target;
      std::string_view content;
      if (!skip_pi(p, &target, &content)) return false;
      Node* const node = add_node(XmlNodeType::ProcessingInstruction, current_, at);
      if (node == nullptr) return false;
      assign_name(node, target);
      assign_value(node, content);
    } else if (next == '!') {
      char* const at = p;
      if (match(p, "<!--")) {
        std::string_view content;
        if (!skip_comment(p, &content)) return false;
        Node* const node = add_node(XmlNodeType::Comment, current_, at);
        if (node == nullptr) return false;
        assign_value(node, content);
      } else if (match(p, "<![CDATA[")) {
        if (current_ == root_) return error(p, "CDATA section outside the root element");
        const char* const content = p + 9;
        const char* q = content;
        for (;;) {
          q = static_cast<const char*>(std::memchr(q, ']', distance(q, end_)));
          if (q == nullptr) return end_error("unterminated CDATA section");
          if (q[1] == ']' && q[2] == '>') break;
          ++q;
        }
        if (!mark_has_text(current_, at)) return false;
        Node* const node = add_node(XmlNodeType::CData, current_, at);
        if (node == nullptr) return false;
        assign_value(node, view(content, q));
        p += distance(p, q) + 3;
      } else if (match(p, "<!DOCTYPE")) {
        if (current_ != root_ || seen_root_) return error(p, "DOCTYPE must come before the root element");
        if (seen_doctype_) return error(p, "more than one DOCTYPE");
        seen_doctype_ = true;
        if (!parse_doctype(p)) return false;
      } else {
        return error_or_end(p + 2, "expected a comment, CDATA section or DOCTYPE after '<!'");
      }
    } else {
      if (!parse_start_tag(p)) return false;
    }
  }

  if (!seen_root_) return end_error("no root element");
  if (bad_.found) return error(end_, bad_.reason);
  return true;
}

bool Parser::settle_pending(Node* parent, bool keep, const char* at) {
  while (!pending_.empty() && pending_.back().parent == parent) {
    const PendingSpace space = pending_.back();
    pending_.pop_back();
    if (!keep) continue;
    if (node_count_ >= options_.max_nodes) {
      return limit(at, std::format("more than {} nodes (max_nodes)", options_.max_nodes));
    }
    ++node_count_;
    Node* const node = new_node(arena_, XmlNodeType::Text);
    assign_value(node, space.text);
    link_between(parent, space.after, space.after != nullptr ? space.after->next : parent->tree.first, node);
  }
  return true;
}

bool Parser::parse_declaration(char*& p) {
  Node* const node = add_node(XmlNodeType::Declaration, root_, p);
  if (node == nullptr) return false;
  assign_name(node, view(p + 2, p + 5));
  char* q = p + 5;
  static constexpr std::array<std::string_view, 3> kNames = {"version", "encoding", "standalone"};
  std::size_t expected = 0;
  Attr* tail = nullptr;
  for (;;) {
    const bool spaced = is_space(*q);
    q = skip_space(q);
    if (q[0] == '?' && q[1] == '>') break;
    if (q == end_) return end_error("unterminated XML declaration");
    char* const name_begin = q;
    while (is_ascii_alpha(*q)) ++q;
    const std::string_view name = view(name_begin, q);
    while (expected < kNames.size() && kNames[expected] != name) {
      if (expected == 0) return error(name_begin, "the XML declaration must start with version=\"1.0\"");
      ++expected;
    }
    if (!spaced || expected == kNames.size()) {
      return error(name_begin, "malformed XML declaration: expected version, encoding, standalone in this order");
    }
    q = skip_space(q);
    if (*q != '=') return error_or_end(q, "malformed XML declaration: expected '='");
    q = skip_space(q + 1);
    const char quote = *q;
    if (quote != '"' && quote != '\'') return error_or_end(q, "malformed XML declaration: expected a quoted value");
    char* const value_begin = ++q;
    while (*q != quote && *q != '\0' && *q != '<' && *q != '?') ++q;
    if (*q != quote) return error_or_end(q, "malformed XML declaration: unterminated value");
    const std::string_view value = view(value_begin, q);
    ++q;
    if (expected == 0) {
      if (!is_version_number(value)) {
        return error(value_begin, std::format("unsupported XML version '{}'", clip(value)));
      }
    } else if (expected == 1) {
      if (!is_encoding_name(value)) return error(value_begin, "malformed encoding name in the XML declaration");
    } else if (value != "yes" && value != "no") {
      return error(value_begin, "standalone must be \"yes\" or \"no\"");
    }
    Attr* const attr = new_attribute(arena_, node, name, value);
    (tail != nullptr ? tail->next : node->tree.attributes) = attr;
    tail = attr;
    ++expected;
  }
  if (node->tree.attributes == nullptr) return error(q, "the XML declaration must start with version=\"1.0\"");
  p = q + 2;
  return true;
}

bool Parser::skip_comment(char*& q, std::string_view* content) {
  const char* const begin = q + 4;
  const char* c = begin;
  for (;;) {
    c = static_cast<const char*>(std::memchr(c, '-', distance(c, end_)));
    if (c == nullptr) return end_error("unterminated comment");
    if (c[1] == '-') {
      if (c[2] == '>') break;
      if (c + 2 == end_) return end_error("unterminated comment");
      return error(c, "'--' is not allowed inside a comment");
    }
    ++c;
  }
  if (content != nullptr) *content = view(begin, c);
  q += distance(q, c) + 3;
  return true;
}

bool Parser::skip_pi(char*& q, std::string_view* target, std::string_view* content) {
  char* const name_begin = q + 2;
  char* c = scan_name(name_begin);
  if (c == name_begin) return error_or_end(name_begin, "expected a processing instruction target after '<?'");
  const std::string_view name = view(name_begin, c);
  if (iequals(name, "xml")) {
    return error(q, name == "xml" ? "the XML declaration is only allowed at the very start of the document"
                                  : "the processing instruction target 'xml' is reserved");
  }
  const char* data = c;
  if (!(c[0] == '?' && c[1] == '>')) {
    if (!is_space(*c)) return error_or_end(c, "expected white space after the processing instruction target");
    c = skip_space(c);
    data = c;
    for (;;) {
      c = static_cast<char*>(std::memchr(c, '?', distance(c, end_)));
      if (c == nullptr) return end_error("unterminated processing instruction");
      if (c[1] == '>') break;
      ++c;
    }
  }
  if (target != nullptr) *target = name;
  if (content != nullptr) *content = view(data, c);
  q = c + 2;
  return true;
}

/// Skip `<!ELEMENT ...>`, `<!ATTLIST ...>` or `<!NOTATION ...>` (and the
/// tail of external entity declarations): everything up to the closing '>',
/// honouring quoted literals. `q` is somewhere inside the declaration.
bool Parser::skip_markup_declaration(char*& q) {
  for (;;) {
    const char c = *q;
    if (c == '>') {
      ++q;
      return true;
    }
    if (c == '"' || c == '\'') {
      char* const close = static_cast<char*>(std::memchr(q + 1, c, distance(q + 1, end_)));
      if (close == nullptr) return end_error("unterminated literal in DOCTYPE");
      q = close + 1;
    } else if (c == '<') {
      return error(q, "malformed declaration in DOCTYPE");
    } else if (q == end_) {
      return end_error("unterminated DOCTYPE");
    } else {
      ++q;
    }
  }
}

bool Parser::parse_entity_declaration(char*& q) {
  // `q` is just after "<!ENTITY".
  if (!is_space(*q)) return error_or_end(q, "expected white space after <!ENTITY");
  q = skip_space(q);
  bool parameter = false;
  if (*q == '%') {
    parameter = true;
    ++q;
    if (!is_space(*q)) return error_or_end(q, "expected white space after '%' in an entity declaration");
    q = skip_space(q);
  }
  char* const name_begin = q;
  q = scan_name(q);
  if (q == name_begin) return error_or_end(q, "expected an entity name");
  const std::string_view name = view(name_begin, q);
  if (!is_space(*q)) return error_or_end(q, "expected white space after the entity name");
  q = skip_space(q);

  Entity entity;
  entity.name = name;
  if (*q == '"' || *q == '\'') {
    const char quote = *q;
    const char* const value_begin = q + 1;
    const char* const value_end =
        static_cast<const char*>(std::memchr(value_begin, quote, distance(value_begin, end_)));
    if (value_end == nullptr) return end_error("unterminated entity value");
    std::string_view value = view(value_begin, value_end);
    entity.usable = !parameter && value.find('%') == std::string_view::npos;
    if (!parameter && value.find('&') != std::string_view::npos) {
      // Character references are resolved now; references to other entities
      // stay in the replacement text and are expanded when it is used.
      std::string processed;
      bool changed = false;
      for (const char* c = value_begin; c != value_end;) {
        if (*c != '&') {
          processed.push_back(*c++);
          continue;
        }
        const Reference ref = read_reference(c, value_end);
        if (ref.kind == Reference::Kind::Malformed) return error(c, "malformed reference in an entity value");
        if (ref.numeric) {
          // Always an error, even with replace_invalid_text: the declaration
          // is kept verbatim in the Doctype node and must stay well-formed.
          if (!is_xml_char(ref.code_point)) {
            return error(c, std::format("character reference to U+{:04X}, which is not allowed in XML",
                                        static_cast<std::uint32_t>(ref.code_point)));
          }
          append_utf8(processed, ref.code_point);
          changed = true;
        } else {
          processed.append(c, distance(c, ref.end));
        }
        c = ref.end;
      }
      if (changed) value = std::string_view(arena_.store(processed), processed.size());
    }
    if (value.find('<') != std::string_view::npos) entity.usable = false;
    entity.value = value;
    q += distance(q, value_end) + 1;
    q = skip_space(q);
    if (*q != '>') return error_or_end(q, "expected '>' at the end of the entity declaration");
    ++q;
  } else {
    // External (SYSTEM / PUBLIC) or unparsed entity: remembered as declared
    // but never read.
    if (!match(q, "SYSTEM") && !match(q, "PUBLIC")) return error_or_end(q, "malformed entity declaration");
    if (!skip_markup_declaration(q)) return false;
  }
  if (!parameter) entities_.push_back(entity);
  return true;
}

bool Parser::parse_internal_subset(char*& q) {
  // `q` is just after '['.
  for (;;) {
    q = skip_space(q);
    const char c = *q;
    if (c == ']') {
      ++q;
      return true;
    }
    if (c == '%') {
      // Parameter entity reference: recognised, never expanded.
      char* const name_begin = q + 1;
      char* const name_end = scan_name(name_begin);
      if (name_end == name_begin || *name_end != ';') {
        return error_or_end(name_end, "malformed parameter entity reference in DOCTYPE");
      }
      q = name_end + 1;
    } else if (match(q, "<!--")) {
      if (!skip_comment(q, nullptr)) return false;
    } else if (match(q, "<?")) {
      if (!skip_pi(q, nullptr, nullptr)) return false;
    } else if (match(q, "<!ENTITY")) {
      q += 8;
      if (!parse_entity_declaration(q)) return false;
    } else if (match(q, "<!ELEMENT") || match(q, "<!ATTLIST") || match(q, "<!NOTATION")) {
      q += 2;
      if (!skip_markup_declaration(q)) return false;
    } else if (q == end_) {
      return end_error("unterminated DOCTYPE");
    } else {
      return error(q, "unexpected content in the DOCTYPE internal subset");
    }
  }
}

bool Parser::parse_doctype(char*& p) {
  char* const at = p;
  char* q = p + 9;
  if (!is_space(*q)) return error_or_end(q, "expected white space after <!DOCTYPE");
  q = skip_space(q);
  char* const raw = q;
  char* const name_end = scan_name(q);
  if (name_end == q) return error_or_end(q, "expected the root element name in DOCTYPE");
  q = name_end;
  bool subset = false;
  for (;;) {
    const char c = *q;
    if (c == '>') break;
    if (c == '"' || c == '\'') {
      char* const close = static_cast<char*>(std::memchr(q + 1, c, distance(q + 1, end_)));
      if (close == nullptr) return end_error("unterminated literal in DOCTYPE");
      q = close + 1;
    } else if (c == '[') {
      if (subset) return error(q, "malformed DOCTYPE");
      subset = true;
      ++q;
      if (!parse_internal_subset(q)) return false;
    } else if (q == end_) {
      return end_error("unterminated DOCTYPE");
    } else if (c == '<' || c == ']' || (subset && !is_space(c))) {
      return error(q, "malformed DOCTYPE");
    } else {
      ++q;
    }
  }
  const char* raw_end = q;
  while (is_space(raw_end[-1])) --raw_end;
  Node* const node = add_node(XmlNodeType::Doctype, root_, at);
  if (node == nullptr) return false;
  assign_name(node, view(raw, name_end));
  assign_value(node, view(raw, raw_end));
  // The first declaration of a name wins; keep that order within equal names.
  std::stable_sort(entities_.begin(), entities_.end(),
                   [](const Entity& a, const Entity& b) { return a.name < b.name; });
  p = q + 1;
  return true;
}

const Parser::Entity* Parser::find_entity(std::string_view name) const noexcept {
  const auto it = std::lower_bound(entities_.begin(), entities_.end(), name,
                                   [](const Entity& entity, std::string_view key) { return entity.name < key; });
  return it != entities_.end() && it->name == name ? &*it : nullptr;
}

/// Decode the reference at `p` in place: the result is written at `out`
/// (never past `p`, since no reference is shorter than what it stands for)
/// and both cursors advance. Returns Decode::Entity, without consuming
/// anything, when the reference names an expandable general entity.
Parser::Decode Parser::decode_reference(char*& p, char*& out) {
  const Reference ref = read_reference(p, end_);
  switch (ref.kind) {
    case Reference::Kind::Malformed:
      if (ref.at_end) {
        end_error("unexpected end of input inside a reference");
      } else {
        error(p, "malformed reference: '&' must start &name; or &#number; (write &amp; for a literal '&')");
      }
      return Decode::Failed;
    case Reference::Kind::Character: {
      char32_t cp = ref.code_point;
      if (!is_xml_char(cp)) {
        if (!options_.replace_invalid_text) {
          error(p, std::format("character reference to U+{:04X}, which is not allowed in XML",
                               static_cast<std::uint32_t>(cp)));
          return Decode::Failed;
        }
        cp = replacement_character;
      }
      out = put_utf8(out, cp);
      p += distance(p, ref.end);
      return Decode::Done;
    }
    case Reference::Kind::Named: break;
  }
  const Entity* const entity = find_entity(ref.name);
  if (entity != nullptr && entity->usable) return Decode::Entity;
  if (!options_.keep_unknown_entities) {
    error(p, entity != nullptr
                 ? std::format("entity '&{};' is external or contains markup and cannot be expanded", clip(ref.name))
                 : std::format("undefined entity '&{};'", clip(ref.name)));
    return Decode::Failed;
  }
  const std::size_t size = distance(p, ref.end);
  std::memmove(out, p, size);
  out += size;
  p += size;
  return Decode::Done;
}

/// Append the decoded form of the raw text [p, end) to `out`: references are
/// resolved and internal entities expanded (recursively, within the limits).
bool Parser::append_decoded(std::string& out, const char* p, const char* end, bool attribute, int depth,
                            const char* report_at) {
  while (p != end) {
    const char* const plain = p;
    while (p != end && *p != '&' && !(attribute && (*p == '\t' || *p == '\n' || *p == '\r'))) ++p;
    out.append(plain, distance(plain, p));
    if (p == end) break;
    if (*p != '&') {
      out.push_back(' ');
      ++p;
      continue;
    }
    const char* const at = depth == 0 ? p : report_at;
    const Reference ref = read_reference(p, end);
    switch (ref.kind) {
      case Reference::Kind::Malformed:
        return error(at, depth == 0 ? "malformed reference: '&' must start &name; or &#number; (write &amp; for a "
                                      "literal '&')"
                                    : "malformed reference in the replacement text of an entity");
      case Reference::Kind::Character: {
        char32_t cp = ref.code_point;
        if (!is_xml_char(cp)) {
          if (!options_.replace_invalid_text) {
            return error(at, std::format("character reference to U+{:04X}, which is not allowed in XML",
                                         static_cast<std::uint32_t>(cp)));
          }
          cp = replacement_character;
        }
        append_utf8(out, cp);
        break;
      }
      case Reference::Kind::Named: {
        const Entity* const entity = find_entity(ref.name);
        if (entity != nullptr && entity->usable) {
          if (depth >= kMaxEntityDepth) {
            return limit(at, std::format("entity references nested more than {} levels deep", kMaxEntityDepth));
          }
          expansion_used_ += entity->value.size();
          if (expansion_used_ > options_.max_entity_expansion) {
            return limit(at, std::format("entity expansion exceeds {} bytes (max_entity_expansion)",
                                         options_.max_entity_expansion));
          }
          if (!append_decoded(out, entity->value.data(), entity->value.data() + entity->value.size(), attribute,
                              depth + 1, at)) {
            return false;
          }
        } else if (options_.keep_unknown_entities) {
          out.append(p, distance(p, ref.end));
        } else {
          return error(at, entity != nullptr ? std::format("entity '&{};' is external or contains markup and cannot "
                                                           "be expanded",
                                                           clip(ref.name))
                                             : std::format("undefined entity '&{};'", clip(ref.name)));
        }
        break;
      }
    }
    p = ref.end;
  }
  return true;
}

/// Slow path of text / attribute decoding, entered at the first reference to
/// an expandable entity: [start, decoded_end) is already decoded, the raw
/// rest starts at `p`. Expansion can outgrow the source text, so the value
/// is assembled in a scratch string and stored in the arena.
bool Parser::expand_tail(const char* start, const char* decoded_end, char*& p, char quote, std::string_view& value) {
  scratch_.assign(start, distance(start, decoded_end));
  char* end = p;
  if (quote == '\0') {
    while (*end != '<' && *end != '\0') ++end;
  } else {
    while (*end != quote && *end != '<' && *end != '\0') ++end;
    if (*end == '<') return error(end, "'<' is not allowed in an attribute value (write &lt;)");
    if (*end != quote) return end_error("unterminated attribute value");
  }
  if (!append_decoded(scratch_, p, end, quote != '\0', 0, p)) return false;
  value = std::string_view(arena_.store(scratch_), scratch_.size());
  p = quote == '\0' ? end : end + 1;
  return true;
}

/// Character data starting at `start` (`p` is at its first non-space
/// character). On return `p` is at the '<' that ends it, or at the end.
bool Parser::parse_text(char*& p, char* start, std::string_view& value) {
  while ((kClass[byte_of(*p)] & kTextStop) == 0) ++p;
  if (*p != '&') {
    value = view(start, p);
    return true;
  }
  char* out = p;
  for (;;) {
    switch (decode_reference(p, out)) {
      case Decode::Done: break;
      case Decode::Failed: return false;
      case Decode::Entity: return expand_tail(start, out, p, '\0', value);
    }
    char* const plain = p;
    while ((kClass[byte_of(*p)] & kTextStop) == 0) ++p;
    const std::size_t size = distance(plain, p);
    std::memmove(out, plain, size);
    out += size;
    if (*p != '&') break;
  }
  value = view(start, out);
  return true;
}

/// Attribute value; `p` is just after the opening quote and ends up just
/// after the closing one. Literal tabs and line breaks become spaces.
bool Parser::parse_attribute_value(char*& p, char quote, std::string_view& value) {
  const std::uint8_t stop = quote == '"' ? kAttrStopDq : kAttrStopSq;
  char* const start = p;
  char* out = nullptr;  // set once decoding has shortened the value
  for (;;) {
    char* const plain = p;
    while ((kClass[byte_of(*p)] & stop) == 0) ++p;
    if (out != nullptr) {
      const std::size_t size = distance(plain, p);
      std::memmove(out, plain, size);
      out += size;
    }
    const char c = *p;
    if (c == quote) break;
    if (c == '\t' || c == '\n') {
      if (out != nullptr) {
        *out++ = ' ';
      } else {
        *p = ' ';
      }
      ++p;
    } else if (c == '&') {
      if (out == nullptr) out = p;
      switch (decode_reference(p, out)) {
        case Decode::Done: break;
        case Decode::Failed: return false;
        case Decode::Entity: return expand_tail(start, out, p, quote, value);
      }
    } else if (c == '<') {
      return error(p, "'<' is not allowed in an attribute value (write &lt;)");
    } else {
      return end_error("unterminated attribute value");
    }
  }
  value = view(start, out != nullptr ? out : p);
  ++p;
  return true;
}

/// Uniqueness check for elements with many attributes: sort and compare
/// neighbours, which stays O(n log n) whatever the names are.
bool Parser::check_duplicate_attributes(Node* element) {
  attribute_scratch_.clear();
  for (Attr* a = element->tree.attributes; a != nullptr; a = a->next) attribute_scratch_.push_back(a);
  std::sort(attribute_scratch_.begin(), attribute_scratch_.end(), [](const Attr* a, const Attr* b) {
    const int order = name_of(a).compare(name_of(b));
    return order != 0 ? order < 0 : a->name < b->name;  // equal names: document order
  });
  const Attr* duplicate = nullptr;
  for (std::size_t i = 1; i < attribute_scratch_.size(); ++i) {
    const Attr* const a = attribute_scratch_[i];
    if (name_of(a) == name_of(attribute_scratch_[i - 1]) && (duplicate == nullptr || a->name < duplicate->name)) {
      duplicate = a;
    }
  }
  if (duplicate == nullptr) return true;
  return error(duplicate->name, std::format("duplicate attribute '{}'", clip(name_of(duplicate))));
}

bool Parser::parse_start_tag(char*& p) {
  char* const name_begin = p + 1;
  char* q = scan_name(name_begin);
  if (q == name_begin) return error_or_end(name_begin, "expected an element name after '<'");
  if (!fits_u32(distance(name_begin, q))) return limit(p, "element name too long");
  if (current_ == root_) {
    if (seen_root_) return error(p, "more than one root element");
    seen_root_ = true;
  }
  if (depth_ >= options_.max_depth) {
    return limit(p, std::format("elements nested more than {} levels deep (max_depth)", options_.max_depth));
  }
  Node* const element = add_node(XmlNodeType::Element, current_, p);
  if (element == nullptr) return false;
  assign_name(element, view(name_begin, q));
  element->flags = static_cast<std::uint8_t>(current_->flags & kFlagPreserve);

  Attr* tail = nullptr;
  std::size_t count = 0;
  bool open = true;
  for (;;) {
    bool spaced = false;
    if (is_space(*q)) {
      q = skip_space(q + 1);
      spaced = true;
    }
    const char c = *q;
    if (c == '>') {
      ++q;
      break;
    }
    if (c == '/') {
      if (q[1] != '>') return error_or_end(q + 1, "expected '>' after '/' in a start tag");
      q += 2;
      open = false;
      break;
    }
    if (q == end_) {
      return end_error(std::format("unexpected end of input in the start tag <{}>", clip(name_of(element))));
    }
    char* const attr_begin = q;
    q = scan_name(q);
    if (q == attr_begin) {
      return error(q, spaced ? "expected an attribute name, '>' or '/>'" : "invalid character in an element name");
    }
    if (!spaced) return error(attr_begin, "expected white space before the attribute name");
    const std::string_view name = view(attr_begin, q);
    if (!fits_u32(name.size())) return limit(attr_begin, "attribute name too long");
    q = skip_space(q);
    if (*q != '=') return error_or_end(q, std::format("expected '=' after the attribute name '{}'", clip(name)));
    q = skip_space(q + 1);
    const char quote = *q;
    if (quote != '"' && quote != '\'') {
      return error_or_end(q, std::format("the value of attribute '{}' must be quoted", clip(name)));
    }
    ++q;
    std::string_view value;
    if (!parse_attribute_value(q, quote, value)) return false;
    if (!fits_u32(value.size())) return limit(attr_begin, "attribute value too long");

    if (count >= options_.max_attribute_count_per_element) {
      return limit(attr_begin, std::format("more than {} attributes on one element (max_attribute_count_per_element)",
                                           options_.max_attribute_count_per_element));
    }
    if (count < kLinearDuplicateCheck) {
      for (const Attr* a = element->tree.attributes; a != nullptr; a = a->next) {
        if (name_of(a) == name) return error(attr_begin, std::format("duplicate attribute '{}'", clip(name)));
      }
    }
    Attr* const attr = new_attribute(arena_, element, name, value);
    (tail != nullptr ? tail->next : element->tree.attributes) = attr;
    tail = attr;
    ++count;
    if (name == "xml:space" && !options_.preserve_whitespace) {
      if (value == "preserve") {
        element->flags = static_cast<std::uint8_t>(element->flags | kFlagPreserve);
      } else if (value == "default") {
        element->flags = static_cast<std::uint8_t>(element->flags & ~kFlagPreserve);
      }
    }
  }
  if (count > kLinearDuplicateCheck && !check_duplicate_attributes(element)) return false;
  if (open) {
    current_ = element;
    ++depth_;
  }
  p = q;
  return true;
}

bool Parser::parse_end_tag(char*& p) {
  if (current_ == root_) return error(p, "closing tag without a matching opening tag");
  const std::string_view expected = name_of(current_);
  char* q = p + 2;
  std::size_t i = 0;
  while (i < expected.size() && q[i] == expected[i]) ++i;
  bool matches = i == expected.size();
  if (matches) {
    // The name in the tag must end here: "</ab>" does not close <a>.
    const std::uint8_t after = byte_of(q[i]);
    matches = after < 0x80 ? (kClass[after] & kNameChar) == 0 : scan_name(q) == q + i;
  }
  if (!matches) {
    char* const name_end = scan_name(q);
    if (name_end == q) return error_or_end(q, "expected an element name after '</'");
    if (name_end == end_) return end_error("unexpected end of input in a closing tag");
    return error(p, std::format("mismatched closing tag </{}>, expected </{}>", clip(view(q, name_end)),
                                clip(expected)));
  }
  q = skip_space(q + i);
  if (*q != '>') return error_or_end(q, "expected '>' at the end of the closing tag");
  ++q;

  // White-space-only runs that are still undecided: this element has no
  // other character data, so they go, unless one is its entire content.
  if (!pending_.empty() && pending_.back().parent == current_) {
    if (!settle_pending(current_, current_->tree.first == nullptr, p)) return false;
  }
  current_ = current_->parent;
  --depth_;
  p = q;
  return true;
}

// ===========================================================================
// 6. Writer and deep_equal
// ===========================================================================

constexpr std::size_t kMaxIndentLevels = 64;
constexpr std::string_view kDefaultDeclaration = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>";

/// Escape text that is already known to be legal (everything stored in a
/// tree is): only the markup characters need attention.
void append_escaped(std::string& out, std::string_view text, bool attribute, bool non_ascii) {
  const std::uint8_t mask = attribute ? kEscapeAttr : kEscapeText;
  const char* p = text.data();
  const char* const end = p + text.size();
  while (p != end) {
    const char* const plain = p;
    while (p != end && (kClass[byte_of(*p)] & mask) == 0 && !(non_ascii && byte_of(*p) >= 0x80)) ++p;
    out.append(plain, distance(plain, p));
    if (p == end) break;
    switch (*p) {
      case '&': out.append("&amp;"); break;
      case '<': out.append("&lt;"); break;
      case '>': out.append("&gt;"); break;
      case '"': out.append("&quot;"); break;
      case '\t': out.append("&#9;"); break;
      case '\n': out.append("&#10;"); break;
      case '\r': out.append("&#13;"); break;
      default: {
        const Utf8Decoded d = decode_utf8(view(p, end), 0);
        std::array<char, 16> buffer{};
        const auto [digits_end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                                                    static_cast<std::uint32_t>(d.code_point), 16);
        out.append("&#x");
        for (const char* c = buffer.data(); c != digits_end; ++c) out.push_back(ascii_upper(*c));
        out.push_back(';');
        (void)ec;
        p += d.length - 1;
        break;
      }
    }
    ++p;
  }
}

class Writer {
 public:
  Writer(std::string& out, const XmlWriteOptions& options) : out_(out), options_(options) {
    const auto blank = [](std::string_view text, std::string_view allowed) {
      return text.find_first_not_of(allowed) == std::string_view::npos;
    };
    indent_ = blank(options.indent, " \t") ? std::string_view(options.indent) : std::string_view("  ");
    newline_ = !options.newline.empty() && blank(options.newline, "\r\n") ? std::string_view(options.newline)
                                                                           : std::string_view("\n");
    pretty_ = !indent_.empty();
  }

  [[nodiscard]] bool pretty() const noexcept { return pretty_; }
  void newline() { out_.append(newline_); }

  void declaration(const Node* node) {
    if (node == nullptr) {
      out_.append(kDefaultDeclaration);
      return;
    }
    const Attr* const version = find_attribute(node, "version");
    const Attr* const standalone = find_attribute(node, "standalone");
    out_.append("<?xml version=\"");
    out_.append(version != nullptr ? value_of(version) : std::string_view("1.0"));
    out_.push_back('"');
    if (find_attribute(node, "encoding") != nullptr) out_.append(" encoding=\"UTF-8\"");
    if (standalone != nullptr) {
      out_.append(" standalone=\"");
      out_.append(value_of(standalone));
      out_.push_back('"');
    }
    out_.append("?>");
  }

  /// Write `top` and everything below it. Iterative.
  void subtree(const Node* top) {
    const Node* node = top;
    std::size_t depth = 0;
    const Node* inline_root = nullptr;  // outermost element being written on one line
    for (;;) {
      if (node != top && pretty_ && inline_root == nullptr) line_break(depth);
      bool descend = false;
      switch (node->type) {
        case XmlNodeType::Element: {
          out_.push_back('<');
          out_.append(name_of(node));
          for (const Attr* a = node->tree.attributes; a != nullptr; a = a->next) {
            out_.push_back(' ');
            out_.append(name_of(a));
            out_.append("=\"");
            append_escaped(out_, value_of(a), true, options_.escape_non_ascii);
            out_.push_back('"');
          }
          if (node->tree.first == nullptr) {
            if (options_.self_close_empty) {
              out_.append("/>");
            } else {
              out_.append("></");
              out_.append(name_of(node));
              out_.push_back('>');
            }
          } else {
            out_.push_back('>');
            if (pretty_ && inline_root == nullptr && is_inline(node)) inline_root = node;
            descend = true;
          }
          break;
        }
        case XmlNodeType::Text: append_escaped(out_, value_of(node), false, options_.escape_non_ascii); break;
        case XmlNodeType::CData: cdata(value_of(node)); break;
        case XmlNodeType::Comment:
          out_.append("<!--");
          out_.append(value_of(node));
          out_.append("-->");
          break;
        case XmlNodeType::ProcessingInstruction:
          out_.append("<?");
          out_.append(name_of(node));
          if (node->content.size != 0) {
            out_.push_back(' ');
            out_.append(value_of(node));
          }
          out_.append("?>");
          break;
        case XmlNodeType::Declaration: declaration(node); break;
        case XmlNodeType::Doctype:
          out_.append("<!DOCTYPE ");
          out_.append(value_of(node));
          out_.push_back('>');
          break;
        case XmlNodeType::Document:
        case XmlNodeType::Null: break;
      }
      if (descend) {
        node = node->tree.first;
        ++depth;
        continue;
      }
      for (;;) {
        if (node == top) return;
        if (node->next != nullptr) {
          node = node->next;
          break;
        }
        node = node->parent;
        --depth;
        if (pretty_ && inline_root == nullptr) line_break(depth);
        out_.append("</");
        out_.append(name_of(node));
        out_.push_back('>');
        if (node == inline_root) inline_root = nullptr;
      }
    }
  }

 private:
  /// True when pretty printing must leave the content of `element` alone.
  [[nodiscard]] static bool is_inline(const Node* element) noexcept {
    const Attr* const space = find_attribute(element, "xml:space");
    if (space != nullptr && value_of(space) == "preserve") return true;
    for (const Node* child = element->tree.first; child != nullptr; child = child->next) {
      if (is_text_type(child->type)) return true;
    }
    return false;
  }

  void line_break(std::size_t depth) {
    out_.append(newline_);
    for (std::size_t i = std::min(depth, kMaxIndentLevels); i > 0; --i) out_.append(indent_);
  }

  void cdata(std::string_view text) {
    out_.append("<![CDATA[");
    for (;;) {
      const std::size_t at = text.find("]]>");
      if (at == std::string_view::npos) break;
      // "]]>" cannot occur inside a section: end one after "]]" and start
      // the next with ">".
      out_.append(text.substr(0, at + 2));
      out_.append("]]><![CDATA[");
      text.remove_prefix(at + 2);
    }
    out_.append(text);
    out_.append("]]>");
  }

  std::string& out_;
  const XmlWriteOptions& options_;
  std::string_view indent_;
  std::string_view newline_;
  bool pretty_ = false;
};

// --- deep_equal --------------------------------------------------------------

/// First node at or after `node` (among its siblings) that takes part in a
/// comparison: declarations and empty text nodes do not.
[[nodiscard]] const Node* significant(const Node* node) noexcept {
  while (node != nullptr &&
         (node->type == XmlNodeType::Declaration || (node->type == XmlNodeType::Text && node->content.size == 0))) {
    node = node->next;
  }
  return node;
}

/// Compare the runs of adjacent same-kind text nodes starting at `a` and `b`
/// by their concatenated values; on success both are moved past their run.
[[nodiscard]] bool equal_text_runs(const Node*& a, const Node*& b) noexcept {
  const XmlNodeType type = a->type;
  std::string_view left = value_of(a);
  std::string_view right = value_of(b);
  const auto advance = [type](const Node*& node, std::string_view& rest) {
    // Moves to the next fragment of the run; returns false at its end.
    const Node* const next = significant(node->next);
    if (next == nullptr || next->type != type) return false;
    node = next;
    rest = value_of(next);
    return true;
  };
  for (;;) {
    const std::size_t common = std::min(left.size(), right.size());
    if (left.substr(0, common) != right.substr(0, common)) return false;
    left.remove_prefix(common);
    right.remove_prefix(common);
    const bool more_left = !left.empty() || advance(a, left);
    const bool more_right = !right.empty() || advance(b, right);
    if (!more_left || !more_right) {
      // One run is exhausted: the other must hold nothing but empty fragments.
      if (more_left) {
        while (left.empty()) {
          if (!advance(a, left)) break;
        }
        if (!left.empty()) return false;
      }
      if (more_right) {
        while (right.empty()) {
          if (!advance(b, right)) break;
        }
        if (!right.empty()) return false;
      }
      a = significant(a->next);
      b = significant(b->next);
      return true;
    }
  }
}

[[nodiscard]] bool equal_attributes(const Node* a, const Node* b) {
  const Attr* x = a->tree.attributes;
  const Attr* y = b->tree.attributes;
  while (x != nullptr && y != nullptr && name_of(x) == name_of(y) && value_of(x) == value_of(y)) {
    x = x->next;
    y = y->next;
  }
  if (x == nullptr && y == nullptr) return true;
  if (x == nullptr || y == nullptr) return false;
  // Same attributes in a different order? Compare the remainders sorted.
  using Pair = std::pair<std::string_view, std::string_view>;
  std::vector<Pair> left;
  std::vector<Pair> right;
  for (; x != nullptr; x = x->next) left.emplace_back(name_of(x), value_of(x));
  for (; y != nullptr; y = y->next) right.emplace_back(name_of(y), value_of(y));
  if (left.size() != right.size()) return false;
  std::sort(left.begin(), left.end());
  std::sort(right.begin(), right.end());
  return left == right;
}

/// Kind, name, value and attributes of two non-text nodes.
[[nodiscard]] bool equal_shallow(const Node* a, const Node* b) {
  if (a->type != b->type || name_of(a) != name_of(b)) return false;
  if (a->type == XmlNodeType::Element) return equal_attributes(a, b);
  return value_of(a) == value_of(b);
}

// ===========================================================================
// 7. Helpers of the public API
// ===========================================================================

/// Sanitise `text` like xml_escape does (malformed UTF-8 -> U+FFFD, illegal
/// characters dropped). Returns `text` itself when it is clean, otherwise a
/// view of `storage`.
[[nodiscard]] std::string_view sanitised(std::string_view text, std::string& storage) {
  const char* const begin = text.data();
  const char* const end = begin + text.size();
  const char* p = find_special(begin, end, false);
  if (p == end) return text;
  storage.assign(begin, distance(begin, p));
  while (p != end) {
    const Utf8Decoded d = decode_utf8(view(p, end), 0);
    if (!d.ok) storage.append("\xEF\xBF\xBD");
    p += d.length;
    const char* const next = find_special(p, end, false);
    storage.append(p, distance(p, next));
    p = next;
  }
  return storage;
}

[[nodiscard]] std::string_view sanitised_comment(std::string_view text, std::string& storage) {
  std::string_view clean = sanitised(text, storage);
  if (clean.find("--") == std::string_view::npos && !clean.ends_with('-')) return clean;
  std::string fixed;
  fixed.reserve(clean.size() + 8);
  for (const char c : clean) {
    if (c == '-' && !fixed.empty() && fixed.back() == '-') fixed.push_back(' ');
    fixed.push_back(c);
  }
  if (fixed.ends_with('-')) fixed.push_back(' ');
  storage = std::move(fixed);
  return storage;
}

[[nodiscard]] std::string_view sanitised_pi_data(std::string_view text, std::string& storage) {
  std::string_view clean = sanitised(text, storage);
  while (!clean.empty() && is_space(clean.front())) clean.remove_prefix(1);
  if (clean.find("?>") == std::string_view::npos) return clean;
  std::string fixed;
  fixed.reserve(clean.size() + 8);
  for (const char c : clean) {
    if (c == '>' && !fixed.empty() && fixed.back() == '?') fixed.push_back(' ');
    fixed.push_back(c);
  }
  storage = std::move(fixed);
  return storage;
}

/// Sanitised form of `value` for a node of the given kind.
[[nodiscard]] std::string_view sanitised_for(XmlNodeType type, std::string_view value, std::string& storage) {
  switch (type) {
    case XmlNodeType::Comment: return sanitised_comment(value, storage);
    case XmlNodeType::ProcessingInstruction: return sanitised_pi_data(value, storage);
    default: return sanitised(value, storage);
  }
}

/// Store `text` as the new content of a string slot, reusing the old storage
/// when it is large enough (`text` may alias it).
void replace_string(Arena& arena, const char*& slot, std::size_t old_size, std::string_view text) {
  if (text.empty()) {
    slot = "";
  } else if (text.size() <= old_size) {
    std::memmove(const_cast<char*>(slot), text.data(), text.size());
  } else {
    slot = arena.store(text);
  }
}

enum class Where { First, Last, Before, After };

/// Create a node and insert it under `parent`, enforcing the structural
/// rules described in the header. Returns null when they are violated.
[[nodiscard]] Node* insert_node(Node* parent, XmlNodeType type, std::string_view name, std::string_view value,
                                Where where, Node* reference) {
  DocumentData* const document = document_of(parent);
  if (document == nullptr) return nullptr;
  const bool in_document = parent->type == XmlNodeType::Document;
  if (!in_document && parent->type != XmlNodeType::Element) return nullptr;
  if (in_document && type != XmlNodeType::Element && type != XmlNodeType::Comment &&
      type != XmlNodeType::ProcessingInstruction) {
    return nullptr;
  }
  if (type == XmlNodeType::Element || type == XmlNodeType::ProcessingInstruction) {
    if (!is_name(name)) return nullptr;
    if (type == XmlNodeType::ProcessingInstruction && iequals(name, "xml")) return nullptr;
  }

  Node* before = nullptr;
  Node* after = nullptr;
  switch (where) {
    case Where::Last: before = parent->tree.last; break;
    case Where::First:
      after = parent->tree.first;
      if (after != nullptr && after->type == XmlNodeType::Declaration) {
        before = after;
        after = after->next;
      }
      break;
    case Where::Before:
      if (reference == nullptr || reference->parent != parent) return nullptr;
      if (reference->type == XmlNodeType::Declaration) return nullptr;
      before = reference->prev;
      after = reference;
      break;
    case Where::After:
      if (reference == nullptr || reference->parent != parent) return nullptr;
      before = reference;
      after = reference->next;
      break;
  }
  if (in_document && type == XmlNodeType::Element) {
    // One document element, and it follows the doctype.
    for (const Node* n = parent->tree.first; n != nullptr; n = n->next) {
      if (n->type == XmlNodeType::Element) return nullptr;
    }
    for (const Node* n = after; n != nullptr; n = n->next) {
      if (n->type == XmlNodeType::Doctype) return nullptr;
    }
  }

  std::string storage;
  const std::string_view clean = sanitised_for(type, value, storage);
  Node* const node = new_node(document->arena, type);
  assign_name(node, std::string_view(document->arena.store(name), name.size()));
  if (!has_tree(type)) assign_value(node, std::string_view(document->arena.store(clean), clean.size()));
  link_between(parent, before, after, node);
  return node;
}

[[nodiscard]] bool matches_step(const Node* node, std::string_view step) noexcept {
  return node->type == XmlNodeType::Element && (step == "*" || name_of(node) == step);
}

[[nodiscard]] Node* first_match(Node* node, std::string_view step) noexcept {
  while (node != nullptr && !matches_step(node, step)) node = node->next;
  return node;
}

/// Step `index` of a '/'-separated path.
[[nodiscard]] std::string_view path_step(std::string_view path, std::size_t index) noexcept {
  std::size_t begin = 0;
  for (; index > 0; --index) begin = path.find('/', begin) + 1;
  const std::size_t end = path.find('/', begin);
  return path.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
}

/// Calls `visit(Node*)` for every node reached from `start` by `path`, in
/// document order, until it returns false. Iterative with backtracking.
template <class Visit>
void walk_path(Node* start, std::string_view path, Visit&& visit) {
  if (start == nullptr || path.empty()) return;
  const std::size_t steps = static_cast<std::size_t>(std::count(path.begin(), path.end(), '/')) + 1;
  for (std::size_t i = 0; i < steps; ++i) {
    if (path_step(path, i).empty()) return;
  }
  std::size_t level = 0;
  Node* node = first_match(first_child_of(start), path_step(path, 0));
  while (node != nullptr) {
    if (level + 1 == steps) {
      if (!visit(node)) return;
    } else if (Node* const child = first_match(node->tree.first, path_step(path, level + 1)); child != nullptr) {
      node = child;
      ++level;
      continue;
    }
    // Next candidate: a later sibling at this level, else one level up.
    for (;;) {
      if (Node* const sibling = first_match(node->next, path_step(path, level)); sibling != nullptr) {
        node = sibling;
        break;
      }
      if (level == 0) return;
      node = node->parent;
      --level;
    }
  }
}

[[nodiscard]] std::optional<std::uint64_t> parse_unsigned(std::string_view text) noexcept {
  if (!text.empty() && text.front() == '+') text.remove_prefix(1);
  if (text.empty()) return std::nullopt;
  std::uint64_t value = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end != text.data() + text.size()) return std::nullopt;
  return value;
}

}  // namespace

// ===========================================================================
// Public API
// ===========================================================================

bool xml_is_name(std::string_view text) noexcept { return is_name(text); }

std::string xml_escape(std::string_view text, bool attribute) {
  std::string out;
  out.reserve(text.size() + text.size() / 8);
  std::size_t i = 0;
  while (i < text.size()) {
    const Utf8Decoded d = decode_utf8(text, i);
    const char32_t cp = d.code_point;
    const std::string_view raw = text.substr(i, d.length);
    i += d.length;
    switch (cp) {
      case U'&': out.append("&amp;"); continue;
      case U'<': out.append("&lt;"); continue;
      case U'>': out.append("&gt;"); continue;
      case U'"':
        if (attribute) {
          out.append("&quot;");
          continue;
        }
        break;
      case U'\t':
        if (attribute) {
          out.append("&#9;");
          continue;
        }
        break;
      case U'\n':
        if (attribute) {
          out.append("&#10;");
          continue;
        }
        break;
      case U'\r': out.append("&#13;"); continue;
      default: break;
    }
    if (!is_xml_char(cp) || cp == 0xFFFE || cp == 0xFFFF) continue;
    if (d.ok) {
      out.append(raw);
    } else {
      append_utf8(out, replacement_character);
    }
  }
  return out;
}

// --- XmlAttribute ------------------------------------------------------------

std::string_view XmlAttribute::name() const noexcept { return data_ != nullptr ? name_of(data_) : std::string_view(); }
std::string_view XmlAttribute::local_name() const noexcept { return xml_local_name(name()); }
std::string_view XmlAttribute::prefix() const noexcept { return xml_prefix(name()); }
std::string_view XmlAttribute::namespace_uri() const noexcept {
  return data_ != nullptr ? namespace_of(data_) : std::string_view();
}
std::string_view XmlAttribute::value() const noexcept {
  return data_ != nullptr ? value_of(data_) : std::string_view();
}

std::int64_t XmlAttribute::as_int64(std::int64_t fallback) const noexcept {
  return parse_int(trim_xml_space(value())).value_or(fallback);
}

int XmlAttribute::as_int(int fallback) const noexcept {
  const std::optional<std::int64_t> parsed = parse_int(trim_xml_space(value()));
  if (!parsed || *parsed < std::numeric_limits<int>::min() || *parsed > std::numeric_limits<int>::max()) {
    return fallback;
  }
  return static_cast<int>(*parsed);
}

std::uint64_t XmlAttribute::as_uint64(std::uint64_t fallback) const noexcept {
  return parse_unsigned(trim_xml_space(value())).value_or(fallback);
}

unsigned int XmlAttribute::as_uint(unsigned int fallback) const noexcept {
  const std::optional<std::uint64_t> parsed = parse_unsigned(trim_xml_space(value()));
  if (!parsed || *parsed > std::numeric_limits<unsigned int>::max()) return fallback;
  return static_cast<unsigned int>(*parsed);
}

double XmlAttribute::as_double(double fallback) const noexcept {
  return parse_double(trim_xml_space(value())).value_or(fallback);
}

bool XmlAttribute::as_bool(bool fallback) const noexcept {
  const std::string_view text = trim_xml_space(value());
  if (text == "true" || text == "1") return true;
  if (text == "false" || text == "0") return false;
  return fallback;
}

XmlNode XmlAttribute::parent() const noexcept { return XmlNode(data_ != nullptr ? data_->owner : nullptr); }
XmlAttribute XmlAttribute::next_attribute() const noexcept {
  return XmlAttribute(data_ != nullptr ? data_->next : nullptr);
}

bool XmlAttribute::set_value(std::string_view new_value) {
  if (data_ == nullptr || data_->owner == nullptr || data_->owner->type != XmlNodeType::Element) return false;
  DocumentData* const document = document_of(data_->owner);
  if (document == nullptr) return false;
  std::string storage;
  const std::string_view clean = sanitised(new_value, storage);
  if (!fits_u32(clean.size())) return false;
  replace_string(document->arena, data_->value, data_->value_size, clean);
  data_->value_size = static_cast<std::uint32_t>(clean.size());
  return true;
}

// --- XmlNode: reading ----------------------------------------------------------

XmlNodeType XmlNode::type() const noexcept { return data_ != nullptr ? data_->type : XmlNodeType::Null; }
std::string_view XmlNode::name() const noexcept { return data_ != nullptr ? name_of(data_) : std::string_view(); }
std::string_view XmlNode::local_name() const noexcept { return xml_local_name(name()); }
std::string_view XmlNode::prefix() const noexcept { return xml_prefix(name()); }
std::string_view XmlNode::namespace_uri() const noexcept {
  return data_ != nullptr ? namespace_of(data_) : std::string_view();
}
std::string_view XmlNode::lookup_namespace(std::string_view wanted_prefix) const noexcept {
  return data_ != nullptr ? resolve_namespace(data_, wanted_prefix) : std::string_view();
}
std::string_view XmlNode::value() const noexcept { return data_ != nullptr ? value_of(data_) : std::string_view(); }

XmlNode XmlNode::parent() const noexcept { return XmlNode(data_ != nullptr ? data_->parent : nullptr); }
XmlNode XmlNode::first_child() const noexcept { return XmlNode(data_ != nullptr ? first_child_of(data_) : nullptr); }
XmlNode XmlNode::last_child() const noexcept { return XmlNode(data_ != nullptr ? last_child_of(data_) : nullptr); }
XmlNode XmlNode::next_sibling() const noexcept { return XmlNode(data_ != nullptr ? data_->next : nullptr); }
XmlNode XmlNode::previous_sibling() const noexcept { return XmlNode(data_ != nullptr ? data_->prev : nullptr); }

XmlNode XmlNode::child(std::string_view qualified_name) const noexcept {
  return *XmlNodeIterator(first_child(), XmlNodeIterator::Filter::Name, qualified_name, {});
}
XmlNode XmlNode::child_local(std::string_view local) const noexcept {
  return *XmlNodeIterator(first_child(), XmlNodeIterator::Filter::LocalName, local, {});
}
XmlNode XmlNode::child(std::string_view uri, std::string_view local) const noexcept {
  return *XmlNodeIterator(first_child(), XmlNodeIterator::Filter::Namespace, local, uri);
}
XmlNode XmlNode::next_sibling(std::string_view qualified_name) const noexcept {
  return *XmlNodeIterator(next_sibling(), XmlNodeIterator::Filter::Name, qualified_name, {});
}
XmlNode XmlNode::next_sibling_local(std::string_view local) const noexcept {
  return *XmlNodeIterator(next_sibling(), XmlNodeIterator::Filter::LocalName, local, {});
}
XmlNode XmlNode::next_sibling(std::string_view uri, std::string_view local) const noexcept {
  return *XmlNodeIterator(next_sibling(), XmlNodeIterator::Filter::Namespace, local, uri);
}

XmlNodeRange XmlNode::children() const noexcept { return {first_child(), XmlNodeIterator::Filter::All}; }
XmlNodeRange XmlNode::children(std::string_view qualified_name) const noexcept {
  return {first_child(), XmlNodeIterator::Filter::Name, qualified_name};
}
XmlNodeRange XmlNode::children_local(std::string_view local) const noexcept {
  return {first_child(), XmlNodeIterator::Filter::LocalName, local};
}
XmlNodeRange XmlNode::children(std::string_view uri, std::string_view local) const noexcept {
  return {first_child(), XmlNodeIterator::Filter::Namespace, local, uri};
}
XmlNodeRange XmlNode::elements() const noexcept { return {first_child(), XmlNodeIterator::Filter::Elements}; }
XmlDescendantRange XmlNode::descendants() const noexcept { return XmlDescendantRange(*this); }

XmlNode XmlNode::find(std::string_view path) const noexcept {
  Node* found = nullptr;
  walk_path(data_, path, [&](Node* node) {
    found = node;
    return false;
  });
  return XmlNode(found);
}

std::vector<XmlNode> XmlNode::find_all(std::string_view path) const {
  std::vector<XmlNode> found;
  walk_path(data_, path, [&](Node* node) {
    found.emplace_back(node);
    return true;
  });
  return found;
}

XmlAttribute XmlNode::attribute(std::string_view qualified_name) const noexcept {
  return XmlAttribute(data_ != nullptr ? find_attribute(data_, qualified_name) : nullptr);
}

XmlAttribute XmlNode::attribute_local(std::string_view local) const noexcept {
  if (data_ == nullptr) return {};
  for (Attr* a = attributes_of(data_); a != nullptr; a = a->next) {
    if (xml_local_name(name_of(a)) == local) return XmlAttribute(a);
  }
  return {};
}

XmlAttribute XmlNode::attribute(std::string_view uri, std::string_view local) const noexcept {
  if (data_ == nullptr) return {};
  for (Attr* a = attributes_of(data_); a != nullptr; a = a->next) {
    if (xml_local_name(name_of(a)) == local && namespace_of(a) == uri) return XmlAttribute(a);
  }
  return {};
}

XmlAttribute XmlNode::first_attribute() const noexcept {
  return XmlAttribute(data_ != nullptr ? attributes_of(data_) : nullptr);
}
XmlAttributeRange XmlNode::attributes() const noexcept { return XmlAttributeRange(first_attribute()); }

std::string XmlNode::text() const {
  std::string out;
  if (data_ == nullptr) return out;
  if (is_text_type(data_->type)) return std::string(value_of(data_));
  for (const Node* n = first_child_of(data_); n != nullptr; n = next_in_subtree(data_, const_cast<Node*>(n))) {
    if (is_text_type(n->type)) out.append(value_of(n));
  }
  return out;
}

std::string XmlNode::child_text(std::string_view qualified_name) const { return child(qualified_name).text(); }

// --- XmlNode: mutation ---------------------------------------------------------

XmlNode XmlNode::append_child(std::string_view qualified_name) {
  return XmlNode(insert_node(data_, XmlNodeType::Element, qualified_name, {}, Where::Last, nullptr));
}
XmlNode XmlNode::prepend_child(std::string_view qualified_name) {
  return XmlNode(insert_node(data_, XmlNodeType::Element, qualified_name, {}, Where::First, nullptr));
}
XmlNode XmlNode::insert_child_before(std::string_view qualified_name, XmlNode reference) {
  return XmlNode(insert_node(data_, XmlNodeType::Element, qualified_name, {}, Where::Before, reference.data_));
}
XmlNode XmlNode::insert_child_after(std::string_view qualified_name, XmlNode reference) {
  return XmlNode(insert_node(data_, XmlNodeType::Element, qualified_name, {}, Where::After, reference.data_));
}
XmlNode XmlNode::append_text(std::string_view content) {
  return XmlNode(insert_node(data_, XmlNodeType::Text, {}, content, Where::Last, nullptr));
}
XmlNode XmlNode::append_cdata(std::string_view content) {
  return XmlNode(insert_node(data_, XmlNodeType::CData, {}, content, Where::Last, nullptr));
}
XmlNode XmlNode::append_comment(std::string_view content) {
  return XmlNode(insert_node(data_, XmlNodeType::Comment, {}, content, Where::Last, nullptr));
}
XmlNode XmlNode::append_pi(std::string_view target, std::string_view data) {
  return XmlNode(insert_node(data_, XmlNodeType::ProcessingInstruction, target, data, Where::Last, nullptr));
}

bool XmlNode::set_name(std::string_view qualified_name) {
  if (data_ == nullptr) return false;
  const XmlNodeType kind = data_->type;
  if (kind != XmlNodeType::Element && kind != XmlNodeType::ProcessingInstruction) return false;
  if (!is_name(qualified_name)) return false;
  if (kind == XmlNodeType::ProcessingInstruction && iequals(qualified_name, "xml")) return false;
  DocumentData* const document = document_of(data_);
  if (document == nullptr) return false;
  replace_string(document->arena, data_->name, data_->name_size, qualified_name);
  data_->name_size = static_cast<std::uint32_t>(qualified_name.size());
  return true;
}

bool XmlNode::set_value(std::string_view new_value) {
  if (data_ == nullptr) return false;
  const XmlNodeType kind = data_->type;
  if (!is_text_type(kind) && kind != XmlNodeType::Comment && kind != XmlNodeType::ProcessingInstruction) return false;
  DocumentData* const document = document_of(data_);
  if (document == nullptr) return false;
  std::string storage;
  const std::string_view clean = sanitised_for(kind, new_value, storage);
  replace_string(document->arena, data_->content.data, data_->content.size, clean);
  data_->content.size = clean.size();
  return true;
}

bool XmlNode::set_text(std::string_view content) {
  if (data_ == nullptr || data_->type != XmlNodeType::Element || document_of(data_) == nullptr) return false;
  // Copy first: `content` may point into a child that is about to be detached
  // (detaching does not free it, but be explicit about the order).
  Node* const replacement =
      content.empty() ? nullptr : insert_node(data_, XmlNodeType::Text, {}, content, Where::Last, nullptr);
  while (data_->tree.first != nullptr && data_->tree.first != replacement) unlink(data_->tree.first);
  return true;
}

XmlAttribute XmlNode::set_attribute(std::string_view qualified_name, std::string_view new_value) {
  if (data_ == nullptr || data_->type != XmlNodeType::Element) return {};
  DocumentData* const document = document_of(data_);
  if (document == nullptr) return {};
  if (Attr* const existing = find_attribute(data_, qualified_name); existing != nullptr) {
    return XmlAttribute(existing).set_value(new_value) ? XmlAttribute(existing) : XmlAttribute();
  }
  if (!is_name(qualified_name)) return {};
  std::string storage;
  const std::string_view clean = sanitised(new_value, storage);
  if (!fits_u32(clean.size())) return {};
  const std::string_view stored_name(document->arena.store(qualified_name), qualified_name.size());
  const std::string_view stored_value(document->arena.store(clean), clean.size());
  Attr* const attr = new_attribute(document->arena, data_, stored_name, stored_value);
  append_attribute(data_, attr);
  return XmlAttribute(attr);
}

bool XmlNode::remove_attribute(std::string_view qualified_name) {
  return data_ != nullptr && remove_attribute(XmlAttribute(find_attribute(data_, qualified_name)));
}

bool XmlNode::remove_attribute(XmlAttribute handle) {
  Attr* const attr = handle.internal_data();
  if (data_ == nullptr || attr == nullptr || attr->owner != data_ || data_->type != XmlNodeType::Element) return false;
  if (data_->tree.attributes == attr) {
    data_->tree.attributes = attr->next;
  } else {
    Attr* before = data_->tree.attributes;
    while (before != nullptr && before->next != attr) before = before->next;
    if (before == nullptr) return false;
    before->next = attr->next;
  }
  attr->next = nullptr;
  attr->owner = nullptr;
  return true;
}

bool XmlNode::remove_child(XmlNode removed) {
  Node* const node = removed.data_;
  if (data_ == nullptr || node == nullptr || node->parent != data_) return false;
  unlink(node);
  return true;
}

void XmlNode::clear_children() {
  if (data_ == nullptr) return;
  while (Node* const first = first_child_of(data_)) unlink(first);
}

// --- iterators -----------------------------------------------------------------

XmlNodeIterator::XmlNodeIterator(XmlNode start, Filter filter, std::string_view name,
                                 std::string_view namespace_uri) noexcept
    : current_(start), filter_(filter), name_(name), namespace_uri_(namespace_uri) {
  settle();
}

XmlNodeIterator& XmlNodeIterator::operator++() noexcept {
  current_ = current_.next_sibling();
  settle();
  return *this;
}

void XmlNodeIterator::settle() noexcept {
  if (filter_ == Filter::All) return;
  const Node* node = current_.internal_data();
  for (; node != nullptr; node = node->next) {
    if (node->type != XmlNodeType::Element) continue;
    if (filter_ == Filter::Elements) break;
    if (filter_ == Filter::Name) {
      if (name_of(node) == name_) break;
    } else if (xml_local_name(name_of(node)) == name_) {
      if (filter_ == Filter::LocalName || namespace_of(node) == namespace_uri_) break;
    }
  }
  current_ = XmlNode(const_cast<Node*>(node));
}

XmlDescendantIterator& XmlDescendantIterator::operator++() noexcept {
  Node* const node = current_.internal_data();
  current_ = XmlNode(node != nullptr ? next_in_subtree(root_.internal_data(), node) : nullptr);
  return *this;
}

// --- XmlDocument ---------------------------------------------------------------

XmlDocument::XmlDocument() : data_(new DocumentData) {}
XmlDocument::~XmlDocument() { delete data_; }
XmlDocument::XmlDocument(XmlDocument&& other) noexcept : data_(std::exchange(other.data_, nullptr)) {}
XmlDocument& XmlDocument::operator=(XmlDocument&& other) noexcept {
  if (this != &other) {
    delete data_;
    data_ = std::exchange(other.data_, nullptr);
  }
  return *this;
}

DocumentData& XmlDocument::ensure() {
  if (data_ == nullptr) data_ = new DocumentData;
  return *data_;
}

XmlDocument XmlDocument::clone() const {
  XmlDocument copy;
  if (data_ != nullptr) copy_children(copy.data_->arena, &data_->root, &copy.data_->root);
  return copy;
}

XmlNode XmlDocument::node() const noexcept { return XmlNode(data_ != nullptr ? &data_->root : nullptr); }

XmlNode XmlDocument::root() const noexcept {
  return *XmlNodeIterator(node().first_child(), XmlNodeIterator::Filter::Elements, {}, {});
}

XmlNode XmlDocument::declaration() const noexcept {
  const XmlNode first = node().first_child();
  return first.type() == XmlNodeType::Declaration ? first : XmlNode();
}

XmlNode XmlDocument::doctype() const noexcept {
  for (const XmlNode child : node().children()) {
    if (child.type() == XmlNodeType::Doctype) return child;
  }
  return {};
}

XmlNode XmlDocument::append_child(std::string_view name) {
  ensure();
  return node().append_child(name);
}
XmlNode XmlDocument::append_comment(std::string_view text) {
  ensure();
  return node().append_comment(text);
}
XmlNode XmlDocument::append_pi(std::string_view target, std::string_view data) {
  ensure();
  return node().append_pi(target, data);
}

XmlNode XmlDocument::set_declaration(std::string_view version, std::string_view encoding,
                                     std::optional<bool> standalone) {
  DocumentData& data = ensure();
  Node* target = data.root.tree.first;
  if (target == nullptr || target->type != XmlNodeType::Declaration) {
    target = new_node(data.arena, XmlNodeType::Declaration);
    assign_name(target, "xml");
    link_between(&data.root, nullptr, data.root.tree.first, target);
  }
  target->tree.attributes = nullptr;
  const auto add = [&](std::string_view name, std::string_view value) {
    const std::string_view stored(data.arena.store(value), value.size());
    append_attribute(target, new_attribute(data.arena, target, name, stored));
  };
  add("version", is_version_number(version) ? version : std::string_view("1.0"));
  if (is_encoding_name(encoding)) add("encoding", encoding);
  if (standalone) add("standalone", *standalone ? std::string_view("yes") : std::string_view("no"));
  return XmlNode(target);
}

XmlNode XmlDocument::set_doctype(std::string_view text) {
  // Validate by parsing a minimal document around the text: it must come
  // back as exactly this doctype followed by the probe element.
  std::string probe = "<!DOCTYPE ";
  probe.append(text);
  probe.append("><x/>");
  const Result<XmlDocument> parsed = parse_xml(std::string_view(probe));
  if (!parsed) return {};
  const XmlNode probed = parsed->node().first_child();
  const XmlNode element = probed.next_sibling();
  if (probed.type() != XmlNodeType::Doctype || !element.is_element() || element.name() != "x" ||
      element.next_sibling() || element.first_child() || element.first_attribute() ||
      probed.value() != trim_xml_space(convert_line_endings(text, LineEnding::Lf))) {
    return {};
  }

  DocumentData& data = ensure();
  Node* target = doctype().internal_data();
  if (target == nullptr) {
    target = new_node(data.arena, XmlNodeType::Doctype);
    Node* after = data.root.tree.first;
    while (after != nullptr && after->type != XmlNodeType::Element) after = after->next;
    link_between(&data.root, after != nullptr ? after->prev : data.root.tree.last, after, target);
  }
  const std::string_view value(data.arena.store(probed.value()), probed.value().size());
  assign_value(target, value);
  assign_name(target, value.substr(0, probed.name().size()));
  return XmlNode(target);
}

void XmlDocument::clear() {
  delete data_;
  data_ = nullptr;
  data_ = new DocumentData;
}

std::size_t XmlDocument::memory_usage() const noexcept {
  if (data_ == nullptr) return 0;
  return sizeof(DocumentData) + data_->arena.reserved() + data_->source.capacity();
}

// --- parse_xml -------------------------------------------------------------------

Result<XmlDocument> parse_xml(ByteView data, const XmlParseOptions& options) {
  const Result<Source> detected = detect_source(data);
  if (!detected) return fail(detected.error());
  const Source& source = *detected;
  const bool replace = options.replace_invalid_text;

  XmlDocument document;
  DocumentData& impl = *document.data_;
  switch (source.kind) {
    case Source::Kind::Utf8: impl.source.assign(as_chars(data.subspan(source.bom))); break;
    case Source::Kind::Ascii: {
      const std::string_view chars = as_chars(data);
      const auto high = std::find_if(chars.begin(), chars.end(), [](char c) { return byte_of(c) >= 0x80; });
      if (high == chars.end()) {
        impl.source.assign(chars);
      } else if (replace) {
        impl.source = decode_lenient(data, source);
      } else {
        const std::size_t at = static_cast<std::size_t>(high - chars.begin());
        return located_error(Status::Decode, chars.substr(0, at), at,
                             std::format("byte 0x{:02X} is not US-ASCII, the declared encoding", byte_of(*high)));
      }
      break;
    }
    case Source::Kind::Other: {
      DecodedText decoded = decode_text(data, source.encoding);
      if (decoded.replaced != 0 && !replace) {
        const std::size_t at = first_undecodable(data, source);
        return located_error(Status::Decode, decode_text(data.first(at), source.encoding).text, at,
                             std::format("malformed {} sequence", to_upper(encoding_name(source.encoding))));
      }
      impl.source = std::move(decoded.text);
      break;
    }
  }

  BadText bad;
  normalise_source(impl.source, replace, bad);
  impl.arena.hint(impl.source.size());

  Parser parser(impl, options, bad);
  if (!parser.run()) {
    // Cold path: recover the position in the original input.
    std::string decoded;
    std::string_view pristine;
    if (source.kind == Source::Kind::Utf8) {
      pristine = as_chars(data.subspan(source.bom));
    } else {
      decoded = decode_lenient(data, source);
      pristine = decoded;
    }
    const std::string_view prefix = pristine.substr(0, map_offset(pristine, parser.offset, replace));
    return located_error(parser.code, prefix, original_size(source, prefix), parser.message);
  }
  return document;
}

// --- xml_to_string / deep_equal ----------------------------------------------------

std::string xml_to_string(const XmlNode& node, const XmlWriteOptions& options) {
  std::string out;
  const Node* const top = node.internal_data();
  if (top == nullptr) return out;
  Writer writer(out, options);
  if (top->type == XmlNodeType::Document) {
    out.reserve(static_cast<const detail::XmlDocumentNode*>(top)->document->source.size() + 64);
    const Node* const first = top->tree.first;
    if (options.declaration != XmlDeclaration::Never) {
      writer.declaration(first != nullptr && first->type == XmlNodeType::Declaration ? first : nullptr);
      if (writer.pretty()) writer.newline();
    }
    for (const Node* child = first; child != nullptr; child = child->next) {
      if (child->type == XmlNodeType::Declaration) continue;
      writer.subtree(child);
      if (writer.pretty()) writer.newline();
    }
    return out;
  }
  if (options.declaration == XmlDeclaration::Always && top->type != XmlNodeType::Declaration) {
    writer.declaration(nullptr);
    if (writer.pretty()) writer.newline();
  }
  writer.subtree(top);
  if (writer.pretty() && top->type == XmlNodeType::Element) writer.newline();
  return out;
}

bool deep_equal(XmlNode first, XmlNode second) {
  const Node* const top_a = first.internal_data();
  const Node* const top_b = second.internal_data();
  if (top_a == nullptr || top_b == nullptr) return top_a == top_b;
  if (top_a->type != top_b->type) return false;
  if (is_text_type(top_a->type)) return value_of(top_a) == value_of(top_b);
  if (top_a->type != XmlNodeType::Document && !equal_shallow(top_a, top_b)) return false;

  const Node* parent_a = top_a;
  const Node* parent_b = top_b;
  const Node* a = significant(first_child_of(top_a));
  const Node* b = significant(first_child_of(top_b));
  for (;;) {
    if (a == nullptr || b == nullptr) {
      if (a != b) return false;  // one side has more children
      if (parent_a == top_a) return true;
      a = significant(parent_a->next);
      b = significant(parent_b->next);
      parent_a = parent_a->parent;
      parent_b = parent_b->parent;
      continue;
    }
    if (a->type != b->type) return false;
    if (is_text_type(a->type)) {
      if (!equal_text_runs(a, b)) return false;
      continue;
    }
    if (!equal_shallow(a, b)) return false;
    if (first_child_of(a) != nullptr || first_child_of(b) != nullptr) {
      parent_a = a;
      parent_b = b;
      a = significant(first_child_of(a));
      b = significant(first_child_of(b));
      continue;
    }
    a = significant(a->next);
    b = significant(b->next);
  }
}

}  // namespace cvt::util
