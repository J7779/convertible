// Convertible core - office family: Microsoft Reader e-books (.lit).
//
// A LIT file is an "InfoTech Storage" container (see its.hpp): a manifest
// (/manifest) of the book's files, the package description (/meta) and each
// file's content (/data/<name>/content). The package description and the
// HTML pages are stored as "binary HTML": markup tokenised against fixed
// tables of element and attribute names (UTF-8 encoded numbers, text in
// between), which is turned back into markup here and then read by the
// document family's HTML reader. Books protected by Microsoft Reader DRM
// (bookplate or licence, or any content section with the DES transform) are
// refused; nothing is ever decrypted.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/document/document_api.hpp"
#include "families/office/epub.hpp"
#include "families/office/its.hpp"
#include "families/office/lit_tokens.hpp"
#include "families/office/office.hpp"
#include "families/office/office_common.hpp"
#include "util/text.hpp"

namespace cvt::office {

namespace {

using util::XmlNode;

std::uint32_t le32(ByteView d, std::size_t at) {
  return std::to_integer<std::uint32_t>(d[at]) | (std::to_integer<std::uint32_t>(d[at + 1]) << 8) |
         (std::to_integer<std::uint32_t>(d[at + 2]) << 16) | (std::to_integer<std::uint32_t>(d[at + 3]) << 24);
}

// --------------------------------------------------------------------------- characters

/// One "UTF-8" character as the format writes it: the lead byte's high bits
/// give the length (up to 6 bytes), continuation bytes carry 6 bits each.
/// Values are not range-checked (tag numbers such as 0x8000 and the 0xFFFF
/// marker are encoded this way).
std::optional<std::uint32_t> read_char(ByteView d, std::size_t& pos) {
  if (pos >= d.size()) return std::nullopt;
  const auto lead = std::to_integer<std::uint32_t>(d[pos]);
  if ((lead & 0x80u) == 0) {
    ++pos;
    return lead;
  }
  int length = 0;
  std::uint32_t mask = 0x80;
  while ((lead & mask) != 0 && mask != 0) {
    mask >>= 1;
    ++length;
  }
  if (length < 2 || length > 6 || d.size() - pos < static_cast<std::size_t>(length)) return std::nullopt;
  std::uint32_t value = lead & (mask - 1);
  for (int i = 1; i < length; ++i) {
    const auto b = std::to_integer<std::uint32_t>(d[pos + static_cast<std::size_t>(i)]);
    if ((b & 0xC0u) != 0x80u) return std::nullopt;
    value = (value << 6) | (b & 0x3Fu);
  }
  pos += static_cast<std::size_t>(length);
  return value;
}

void append_char(std::string& out, std::uint32_t c) {
  if (c == 0 || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) c = 0xFFFD;
  util::append_utf8(out, static_cast<char32_t>(c));
}

void append_escaped(std::string& out, std::uint32_t c, bool attribute) {
  switch (c) {
    case '<': out += "&lt;"; return;
    case '>': out += attribute ? ">" : "&gt;"; return;
    case '&': out += "&amp;"; return;
    case '"': out += attribute ? "&quot;" : "\""; return;
    case 0x0B: out += '\n'; return;
    default: append_char(out, c); return;
  }
}

/// A string whose length (in characters) is stored as one character first.
std::optional<std::string> sized_string(ByteView d, std::size_t& pos) {
  const auto length = read_char(d, pos);
  if (!length) return std::nullopt;
  std::string out;
  for (std::uint32_t i = 0; i < *length; ++i) {
    const auto c = read_char(d, pos);
    if (!c) return std::nullopt;
    append_char(out, *c);
  }
  return out;
}

// --------------------------------------------------------------------------- binary markup

constexpr std::uint32_t flag_opening = 1u << 0;
constexpr std::uint32_t flag_closing = 1u << 1;
constexpr std::uint32_t flag_atom = 1u << 4;

struct Atoms {
  std::map<std::uint32_t, std::string> tags;
  std::map<std::uint32_t, std::string> attributes;
};

/// Turns binary markup back into text markup. `link` maps the target of an
/// href/src (a manifest name, possibly with "#fragment") to the URL written.
class Unbinary {
 public:
  Unbinary(ByteView data, const lit::TokenMap& map, const Atoms& atoms, std::function<std::string(std::string_view)> link,
           std::uint64_t max_output)
      : data_(data), map_(map), atoms_(atoms), link_(std::move(link)), max_output_(max_output) {}

  Result<std::string> run() {
    std::size_t pos = 0;
    while (pos < data_.size()) {
      if (out_.size() > max_output_) return fail(Status::Limit, "the book's markup exceeds the size limit");
      const auto c = read_char(data_, pos);
      if (!c) return fail(Status::Decode, "the book's markup is damaged");
      if (*c != 0) {
        append_escaped(out_, *c, false);
        continue;
      }
      // A tag: flags, then the tag number (0 = none).
      const auto flags = read_char(data_, pos);
      if (!flags) return fail(Status::Decode, "the book's markup is truncated");
      if (*flags == 0) continue;
      const auto tag = read_char(data_, pos);
      if (!tag) return fail(Status::Decode, "the book's markup is truncated");
      if ((*flags & flag_opening) == 0) {
        if ((*flags & flag_closing) != 0) {
          if (open_.empty()) return fail(Status::Decode, "the book's markup closes more elements than it opens");
          out_ += "</" + open_.back() + ">";
          open_.pop_back();
        }
        continue;
      }
      if (*tag == 0) continue;
      CVT_TRY(element(*tag, *flags, pos));
    }
    while (!open_.empty()) {
      out_ += "</" + open_.back() + ">";
      open_.pop_back();
    }
    return std::move(out_);
  }

 private:
  Result<std::string> counted(std::size_t& pos, std::uint32_t count) {
    std::string text;
    for (std::uint32_t i = 0; i < count; ++i) {
      const auto c = read_char(data_, pos);
      if (!c) return fail(Status::Decode, "the book's markup is truncated");
      append_char(text, *c);
    }
    return text;
  }

  Result<std::uint32_t> length(std::size_t& pos) {
    const auto c = read_char(data_, pos);
    if (!c || *c == 0 || *c - 1 > data_.size() - pos) return fail(Status::Decode, "the book's markup has a bad length");
    return *c - 1;
  }

  Result<void> element(std::uint32_t tag, std::uint32_t flags, std::size_t& pos) {
    std::string name;
    std::span<const lit::Token> local;
    bool atom_attributes = false;
    if (tag == 0x8000) {
      CVT_TRY_ASSIGN(const std::uint32_t n, length(pos));
      CVT_TRY_ASSIGN(name, counted(pos, n));
    } else if ((flags & flag_atom) != 0) {
      const auto it = atoms_.tags.find(tag);
      if (it == atoms_.tags.end()) return fail(Status::Decode, "the book's markup uses an unknown element");
      name = it->second;
      atom_attributes = true;
    } else if (const std::string_view known = map_.tag(tag); !known.empty()) {
      name = std::string(known);
      local = map_.attributes_of(tag);
    } else {
      name = "x-tag-" + std::to_string(tag);
    }
    if (!valid_name(name)) name = "x-tag";
    out_ += "<" + name;
    while (true) {
      const auto a = read_char(data_, pos);
      if (!a) return fail(Status::Decode, "the book's markup is truncated");
      if (*a == 0) break;
      std::string attribute;
      if (*a == 0x8000) {
        CVT_TRY_ASSIGN(const std::uint32_t n, length(pos));
        CVT_TRY_ASSIGN(attribute, counted(pos, n));
      } else if (atom_attributes) {
        if (const auto it = atoms_.attributes.find(*a); it != atoms_.attributes.end()) attribute = it->second;
      } else {
        attribute = std::string(map_.attribute(*a, local));
      }
      CVT_TRY(value(attribute, pos));
    }
    if ((flags & flag_closing) != 0) {
      out_ += " />";
    } else {
      out_ += ">";
      if (open_.size() >= 1024) return fail(Status::Decode, "the book's markup is nested too deeply");
      open_.push_back(std::move(name));
    }
    return {};
  }

  // The value of one attribute; unknown or hidden ("%"-prefixed) attributes are skipped.
  Result<void> value(const std::string& attribute, std::size_t& pos) {
    const auto first = read_char(data_, pos);
    if (!first) return fail(Status::Decode, "the book's markup is truncated");
    std::string text;
    if (*first == 0xFFFF) {
      const auto number = read_char(data_, pos);
      if (!number) return fail(Status::Decode, "the book's markup is truncated");
      text = std::to_string(static_cast<std::int64_t>(*number) - 1);
    } else {
      if (*first == 0 || *first - 1 > data_.size() - pos) return fail(Status::Decode, "the book's markup has a bad length");
      CVT_TRY_ASSIGN(text, counted(pos, *first - 1));
    }
    if (attribute.empty() || attribute.starts_with('%') || !valid_name(attribute)) return {};
    if ((attribute == "href" || attribute == "src") && *first != 0xFFFF && !text.empty()) {
      // The first character is a marker; the rest names a manifest item.
      text = link_(std::string_view(text).substr(util::decode_utf8(text, 0).length));
    }
    out_ += " " + attribute + "=\"";
    for (std::size_t i = 0; i < text.size();) {
      const util::Utf8Decoded d = util::decode_utf8(text, i);
      append_escaped(out_, d.code_point, true);
      i += std::max<std::size_t>(d.length, 1);
    }
    out_ += "\"";
    return {};
  }

  static bool valid_name(std::string_view name) {
    if (name.empty() || !util::is_ascii_alpha(name.front())) return false;
    return std::ranges::all_of(name, [](char c) {
      return util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '-' || c == '_' || c == ':' || c == '.';
    });
  }

  ByteView data_;
  const lit::TokenMap& map_;
  const Atoms& atoms_;
  std::function<std::string(std::string_view)> link_;
  std::uint64_t max_output_;
  std::string out_;
  std::vector<std::string> open_;
};

// --------------------------------------------------------------------------- the book

struct ManifestItem {
  std::string internal;
  std::string original;
  std::string mime;
  int group = 0;  // 0 spine, 1 other pages, 2 style sheets, 3 pictures
};

class LitBook {
 public:
  LitBook(ReadContext& ctx, its::Storage& storage) : ctx_(ctx), storage_(storage), settings_(read_settings(ctx.options)) {}

  Result<ir::Document> read() {
    CVT_TRY(check_drm());
    CVT_TRY(read_manifest());
    read_package();
    std::vector<const ManifestItem*> pages;
    for (const std::string& internal : spine_) {
      if (const auto it = by_internal_.find(internal); it != by_internal_.end()) pages.push_back(&items_[it->second]);
    }
    if (pages.empty()) {
      for (const ManifestItem& item : items_) {
        if (item.group == 0) pages.push_back(&item);
      }
    }
    if (pages.empty()) return fail(Status::Decode, "the book has no pages");

    std::set<std::string> used;
    for (const ManifestItem* page : pages) anchor_[page->internal] = document::unique_slug("chapter-" + stem(page->original), used);
    const bool breaks = ctx_.options.get_bool("chapter_breaks", true);
    for (std::size_t i = 0; i < pages.size(); ++i) {
      CVT_TRY(ctx_.checkpoint(static_cast<double>(i) / static_cast<double>(pages.size()), "Reading chapters"));
      CVT_TRY_ASSIGN(ir::Document chapter, read_page(*pages[i]));
      if (chapter.blocks.empty()) continue;
      // The chapter's anchor: its first block's id unless missing or already used.
      std::string& first_id = chapter.blocks.front().attr.id;
      if (first_id.empty() || ids_.contains(first_id)) first_id = anchor_[pages[i]->internal];
      anchor_[pages[i]->internal] = first_id;
      for_each_block(chapter.blocks, [&](const ir::Block& b) {
        if (!b.attr.id.empty()) ids_.insert(b.attr.id);
      });
      const std::string prefix = pages.size() > 1 ? std::to_string(i + 1) + "-" : std::string();
      if (!prefix.empty() && !chapter.footnotes.empty()) {
        for_each_link(chapter.blocks, [&](ir::Link& link) {
          if (!link.note.empty()) {
            link.note = prefix + link.note;
            link.url = "#fn-" + link.note;
          }
        });
        for (ir::Footnote& note : chapter.footnotes) note.label = prefix + note.label;
      }
      if (!doc_.blocks.empty() && breaks) doc_.blocks.push_back(ir::PageBreak{});
      for (ir::Block& b : chapter.blocks) doc_.blocks.push_back(std::move(b));
      for (ir::Footnote& n : chapter.footnotes) doc_.footnotes.push_back(std::move(n));
    }
    if (doc_.blocks.empty()) return fail(Status::Decode, "the book's pages are empty");
    // Links to other chapters: their anchors, or the fragment when it exists.
    for_each_link(doc_.blocks, [&](ir::Link& link) {
      if (!link.url.starts_with("lit-item:")) return;
      const std::string rest = link.url.substr(9);
      const std::size_t hash = rest.find('#');
      const std::string name = rest.substr(0, hash);
      const std::string fragment = hash == std::string::npos ? std::string() : rest.substr(hash + 1);
      if (!fragment.empty() && ids_.contains(fragment)) {
        link.url = "#" + fragment;
      } else if (const auto a = anchor_.find(name); a != anchor_.end() && ids_.contains(a->second)) {
        link.url = "#" + a->second;
      } else if (const auto it = by_internal_.find(name); it != by_internal_.end()) {
        link.url = items_[it->second].original + (fragment.empty() ? std::string() : "#" + fragment);
      }
    });
    if (!doc_.footnotes.empty()) document::number_footnotes(doc_);
    finish_metadata(doc_, settings_);
    return std::move(doc_);
  }

 private:
  static void for_each_block(const ir::Blocks& blocks, const std::function<void(const ir::Block&)>& fn, int depth = 0) {
    if (depth > ir::max_document_depth + 4) return;
    for (const ir::Block& block : blocks) {
      fn(block);
      if (const auto* list = block.get_if<ir::List>()) {
        for (const ir::Blocks& item : list->items) for_each_block(item, fn, depth + 1);
      } else if (const auto* quote = block.get_if<ir::BlockQuote>()) {
        for_each_block(quote->blocks, fn, depth + 1);
      }
    }
  }

  static std::string stem(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    return std::string(name.substr(0, name.rfind('.')));
  }

  static void for_each_link(ir::Blocks& blocks, const std::function<void(ir::Link&)>& fn, int depth = 0) {
    if (depth > ir::max_document_depth + 4) return;
    const std::function<void(ir::Inlines&, int)> inlines = [&](ir::Inlines& list, int d) {
      if (d > ir::max_document_depth + 4) return;
      for (ir::Inline& item : list) {
        if (auto* link = item.get_if<ir::Link>()) fn(*link);
        std::visit(
            [&](auto& node) {
              if constexpr (requires { node.children; }) inlines(node.children, d + 1);
            },
            item.node);
      }
    };
    for (ir::Block& block : blocks) {
      if (auto* p = block.get_if<ir::Paragraph>()) inlines(p->content, 0);
      if (auto* h = block.get_if<ir::Heading>()) inlines(h->content, 0);
      if (auto* t = block.get_if<ir::TableBlock>()) {
        for (ir::TableRow& row : t->rows) {
          for (ir::TableCell& cell : row.cells) inlines(cell.content, 0);
        }
      }
      if (auto* l = block.get_if<ir::List>()) {
        for (ir::Blocks& item : l->items) for_each_link(item, fn, depth + 1);
      }
      if (auto* q = block.get_if<ir::BlockQuote>()) for_each_link(q->blocks, fn, depth + 1);
    }
  }

  Result<void> check_drm() const {
    if (storage_.has("/DRMStorage/Licenses/EUL")) {
      return fail(Status::Decode, "the book is protected by Microsoft Reader DRM (owner-exclusive licence); "
                                  "protected books cannot be converted");
    }
    if (storage_.has("/DRMStorage/DRMBookplate")) {
      return fail(Status::Decode, "the book is protected by Microsoft Reader DRM (inscribed bookplate); "
                                  "protected books cannot be converted");
    }
    // "Sealed" books (level 1) are readable when their content is not encrypted;
    // a section with the DES transform is refused when it is read.
    return {};
  }

  Result<void> read_manifest() {
    CVT_TRY_ASSIGN(const Bytes raw, storage_.read("/manifest"));
    const ByteView d = raw;
    std::size_t pos = 0;
    while (pos < d.size()) {
      const auto root_length = std::to_integer<std::size_t>(d[pos++]);
      if (root_length == 0) break;
      if (root_length > d.size() - pos) return fail(Status::Decode, "the book's manifest is truncated");
      pos += root_length;
      for (int group = 0; group < 4; ++group) {
        if (d.size() - pos < 4) return fail(Status::Decode, "the book's manifest is truncated");
        const std::uint32_t count = le32(d, pos);
        pos += 4;
        for (std::uint32_t i = 0; i < count; ++i) {
          if (d.size() - pos < 5) return fail(Status::Decode, "the book's manifest is truncated");
          pos += 4;  // offset of the item in its section (not needed)
          ManifestItem item;
          item.group = group;
          auto internal = sized_string(d, pos);
          auto original = sized_string(d, pos);
          auto mime = sized_string(d, pos);
          if (!internal || !original || !mime) return fail(Status::Decode, "the book's manifest is damaged");
          if (pos < d.size() && d[pos] == std::byte{0}) ++pos;
          item.internal = std::move(*internal);
          item.original = percent_decode(*original);
          item.mime = util::to_lower(*mime);
          CVT_TRY(ctx_.limits.check_archive_entries(items_.size() + 1));
          by_internal_[item.internal] = items_.size();
          by_original_[util::to_lower(item.original)] = items_.size();
          items_.push_back(std::move(item));
        }
      }
    }
    if (items_.empty()) return fail(Status::Decode, "the book's manifest is empty");
    return {};
  }

  /// The URL for a reference to a manifest item from binary markup.
  std::string link_target(std::string_view reference) const {
    const std::size_t hash = reference.find('#');
    const std::string name(reference.substr(0, hash));
    const std::string fragment = hash == std::string_view::npos ? std::string() : std::string(reference.substr(hash));
    if (name.empty()) return fragment;
    if (const auto it = by_internal_.find(name); it != by_internal_.end()) {
      return "lit-item:" + name + fragment;
    }
    return std::string(reference);
  }

  void read_package() {
    auto raw = storage_.read("/meta");
    if (!raw) {
      ctx_.warn("The book's package description cannot be read");
      return;
    }
    Unbinary unbinary(*raw, lit::opf_tokens(), Atoms{}, [](std::string_view s) { return std::string(s); }, raw->size() * 8 + 4096);
    auto text = unbinary.run();
    if (!text) {
      ctx_.warn("The book's package description is damaged");
      return;
    }
    auto xml = parse_untrusted_xml(as_bytes(*text), false);
    if (!xml) {
      ctx_.warn("The book's package description is damaged");
      return;
    }
    const XmlNode package = xml->root();
    std::map<std::string, std::string> ids;  // OPF manifest id -> href
    for (XmlNode item : descendant(package, "manifest").children_local("item")) {
      ids[std::string(attr(item, "id"))] = percent_decode(attr(item, "href"));
    }
    for (XmlNode ref : descendant(package, "spine").children_local("itemref")) {
      const auto href = ids.find(std::string(attr(ref, "idref")));
      if (href == ids.end()) continue;
      if (by_internal_.contains(href->second)) {
        spine_.push_back(href->second);
      } else if (const auto it = by_original_.find(util::to_lower(href->second)); it != by_original_.end()) {
        spine_.push_back(items_[it->second].internal);
      }
    }
    for (XmlNode n : package.descendants()) {
      std::string name = util::to_lower(n.local_name());
      if (const std::size_t colon = name.find(':'); colon != std::string::npos) name = name.substr(colon + 1);
      const std::string value(util::trim(n.text()));
      if (value.empty()) continue;
      ir::DocumentMetadata& m = doc_.metadata;
      if (name == "title" && m.title.empty()) m.title = value;
      else if (name == "creator" && m.author.empty()) m.author = value;
      else if (name == "language" && m.language.empty()) m.language = value;
      else if (name == "date" && m.date.empty()) m.date = value;
      else if (name == "publisher") m.extra.try_emplace("publisher", value);
      else if (name == "description") m.extra.try_emplace("description", value);
      else if (name == "subject") m.extra.try_emplace("subject", value);
      else if (name == "rights") m.extra.try_emplace("rights", value);
    }
  }

  Atoms read_atoms(const std::string& internal) const {
    Atoms atoms;
    auto raw = storage_.read("/data/" + internal + "/atom");
    if (!raw) return atoms;
    const ByteView d = *raw;
    if (d.size() < 4) return atoms;
    std::size_t pos = 4;
    const std::uint32_t tags = le32(d, 0);
    for (std::uint32_t i = 1; i <= tags && pos < d.size(); ++i) {
      const auto size = std::to_integer<std::size_t>(d[pos++]);
      if (size == 0 || size > d.size() - pos) return atoms;
      atoms.tags[i] = std::string(as_chars(d.subspan(pos, size)));
      pos += size;
    }
    if (d.size() - pos < 4) return atoms;
    const std::uint32_t attributes = le32(d, pos);
    pos += 4;
    for (std::uint32_t i = 1; i <= attributes && d.size() - pos > 4; ++i) {
      const std::uint32_t size = le32(d, pos);
      pos += 4;
      if (size == 0 || size > d.size() - pos) return atoms;
      atoms.attributes[i] = std::string(as_chars(d.subspan(pos, size)));
      pos += size;
    }
    return atoms;
  }

  std::optional<Bytes> item_bytes(const ManifestItem& item) const {
    for (const std::string& name : {"/data/" + item.internal + "/content", "/data/" + item.internal}) {
      if (auto bytes = storage_.read(name)) return std::move(*bytes);
    }
    return std::nullopt;
  }

  Result<ir::Document> read_page(const ManifestItem& page) {
    auto raw = storage_.read("/data/" + page.internal + "/content");
    if (!raw) {
      if (raw.error().code == Status::Limit || raw.error().message.find("DRM") != std::string::npos) {
        return std::unexpected(raw.error());
      }
      ctx_.warn("A page of the book cannot be read: " + page.original);
      return ir::Document{};
    }
    const Atoms atoms = read_atoms(page.internal);
    Unbinary unbinary(*raw, lit::html_tokens(), atoms, [this](std::string_view s) { return link_target(s); },
                      std::max<std::uint64_t>(raw->size() * 16 + 4096, 1u << 20));
    CVT_TRY_ASSIGN(const std::string markup, unbinary.run());
    document::HtmlReadOptions options;
    options.read_metadata = false;
    options.resolve = [this](std::string_view url) -> std::optional<ir::EncodedImage> {
      std::string_view name = url;
      if (!name.starts_with("lit-item:")) return std::nullopt;
      name.remove_prefix(9);
      name = name.substr(0, name.find('#'));
      const auto it = by_internal_.find(std::string(name));
      if (it == by_internal_.end()) return std::nullopt;
      auto bytes = item_bytes(items_[it->second]);
      if (!bytes) return std::nullopt;
      ir::EncodedImage image;
      image.mime = document::sniff_image_mime(*bytes);
      if (image.mime.empty()) image.mime = items_[it->second].mime;
      image.bytes = std::move(*bytes);
      return image;
    };
    std::vector<std::string> warnings;
    CVT_TRY_ASSIGN(ir::Document chapter, document::parse_html_text(markup, options, &warnings));
    for (std::string& w : warnings) ctx_.warn(std::move(w));
    return chapter;
  }

  ReadContext& ctx_;
  its::Storage& storage_;
  ReadSettings settings_;
  ir::Document doc_;
  std::vector<ManifestItem> items_;
  std::map<std::string, std::size_t> by_internal_;
  std::map<std::string, std::size_t> by_original_;
  std::vector<std::string> spine_;
  std::map<std::string, std::string> anchor_;  // manifest name -> chapter anchor
  std::set<std::string> ids_;                 // block ids of the chapters read so far
};

Result<AnyIR> read_lit(ReadContext& ctx) {
  CVT_TRY_ASSIGN(its::Storage storage, its::Storage::open_lit(ctx.input, ctx.limits));
  LitBook book(ctx, storage);
  CVT_TRY_ASSIGN(ir::Document doc, book.read());
  return AnyIR(std::move(doc));
}

}  // namespace

void register_lit(Registry& registry) {
  FormatInfo info;
  info.id = "lit";
  info.name = "Microsoft Reader e-book";
  info.description = "Microsoft LIT e-book (books without DRM). Read only.";
  info.category = Category::Ebook;
  info.extensions = {"lit"};
  info.mime = "application/x-ms-reader";
  info.popularity = 5;
  info.signatures = {Signature::at(0, "'ITOLITLS' 01 00 00 00")};
  registry.add_format(std::move(info));
  Codec codec;
  codec.format_id = "lit";
  codec.kind = Kind::Document;
  codec.read = read_lit;
  codec.read_cost = 20;
  codec.lossy = true;
  codec.read_options = {
      OptionSpec::boolean("chapter_breaks", "Page break between chapters", true,
                          "Start every chapter on a new page in paginated output."),
      options::strip_metadata(),
  };
  registry.add_codec(std::move(codec));
}

}  // namespace cvt::office
