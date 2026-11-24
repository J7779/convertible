// Convertible core - office family: shared building blocks.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/office/office_common.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <utility>

#include "engine/standard_options.hpp"
#include "families/document/document_api.hpp"
#include "families/image/image_api.hpp"
#include "families/image/image_ops.hpp"
#include "util/hash.hpp"
#include "util/text.hpp"

namespace cvt::office {

// =========================================================================== XML

Result<util::XmlDocument> parse_untrusted_xml(ByteView data, bool preserve_whitespace) {
  util::XmlParseOptions options;
  options.preserve_whitespace = preserve_whitespace;
  options.replace_invalid_text = true;
  options.keep_unknown_entities = true;
  options.max_depth = 512;
  options.max_entity_expansion = 256 * 1024;
  auto parsed = util::parse_xml(data, options);
  if (!parsed && parsed.error().code == Status::Decode) {
    // Unsupported declared encodings: retry as UTF-8 by blanking the label.
    const std::string_view text = as_chars(data);
    const std::size_t decl_end = text.substr(0, 256).find("?>");
    const std::size_t enc = text.substr(0, 256).find("encoding=");
    if (decl_end != std::string_view::npos && enc != std::string_view::npos && enc < decl_end) {
      std::string copy(text);
      copy.erase(0, decl_end + 2);
      return util::parse_xml(as_bytes(copy), options);
    }
  }
  return parsed;
}

std::string_view attr(util::XmlNode node, std::string_view local_name) {
  return node.attribute_local(local_name).value();
}

util::XmlNode child(util::XmlNode node, std::string_view local_name) { return node.child_local(local_name); }

std::vector<util::XmlNode> children(util::XmlNode node, std::string_view local_name) {
  std::vector<util::XmlNode> out;
  for (util::XmlNode c : node.children_local(local_name)) out.push_back(c);
  return out;
}

util::XmlNode descendant(util::XmlNode node, std::string_view local_name) {
  for (util::XmlNode d : node.descendants()) {
    if (d.is_element() && d.local_name() == local_name) return d;
  }
  return {};
}

std::string esc(std::string_view text) { return util::xml_escape(text, false); }
std::string esc_attr(std::string_view text) { return util::xml_escape(text, true); }

// =========================================================================== packages

Result<Package> Package::open(ByteView data, const Limits& limits) {
  CVT_TRY_ASSIGN(util::ZipReader zip, util::ZipReader::open(data, limits));
  Package package;
  package.zip_.emplace(std::move(zip));
  return package;
}

Package Package::from_parts(std::map<std::string, Bytes> parts) {
  Package package;
  package.parts_ = std::move(parts);
  return package;
}

std::optional<std::size_t> Package::index_of(std::string_view name) const {
  if (!zip_) return std::nullopt;
  if (name.starts_with('/')) name.remove_prefix(1);
  if (auto exact = zip_->find(name)) return exact;
  const auto& entries = zip_->entries();
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (util::iequals(entries[i].name, name)) return i;
  }
  return std::nullopt;
}

std::optional<std::string> Package::part_key(std::string_view name) const {
  if (name.starts_with('/')) name.remove_prefix(1);
  if (parts_.contains(std::string(name))) return std::string(name);
  for (const auto& [key, value] : parts_) {
    if (util::iequals(key, name)) return key;
  }
  return std::nullopt;
}

bool Package::has(std::string_view name) const { return zip_ ? index_of(name).has_value() : part_key(name).has_value(); }

std::vector<std::string> Package::names() const {
  std::vector<std::string> out;
  if (zip_) {
    for (const util::ZipEntry& e : zip_->entries()) {
      if (!e.is_directory) out.push_back(e.name);
    }
  } else {
    for (const auto& [key, value] : parts_) out.push_back(key);
  }
  return out;
}

Result<Bytes> Package::read(std::string_view name) {
  if (!zip_) {
    const auto key = part_key(name);
    if (!key) return fail(Status::Decode, "the package has no part '{}'", name);
    return parts_.at(*key);
  }
  const auto index = index_of(name);
  if (!index) return fail(Status::Decode, "the package has no part '{}'", name);
  if (zip_->entries()[*index].encrypted) return fail(Status::Decode, "the part '{}' is encrypted", name);
  return zip_->read(*index);
}

std::optional<Bytes> Package::try_read(std::string_view name) {
  auto bytes = read(name);
  if (!bytes) return std::nullopt;
  return std::move(*bytes);
}

Result<util::XmlDocument> Package::xml(std::string_view name, bool preserve_whitespace) {
  CVT_TRY_ASSIGN(const Bytes bytes, read(name));
  auto parsed = parse_untrusted_xml(bytes, preserve_whitespace);
  if (!parsed) return fail(parsed.error().code, "{}: {}", name, parsed.error().message);
  return parsed;
}

bool Package::encrypted_entries() const {
  if (!zip_) return false;
  return std::ranges::any_of(zip_->entries(), [](const util::ZipEntry& e) { return e.encrypted; });
}

std::vector<Package::Relationship> Package::relationships(std::string_view part) {
  std::vector<Relationship> out;
  auto parsed = xml(rels_path_for(part), false);
  if (!parsed) return out;
  for (util::XmlNode rel : parsed->root().children_local("Relationship")) {
    Relationship r;
    r.id = std::string(rel.attribute("Id").value());
    r.type = std::string(rel.attribute("Type").value());
    r.external = rel.attribute("TargetMode").value() == "External";
    const std::string_view target = rel.attribute("Target").value();
    r.target = r.external ? std::string(target) : resolve_part(part, percent_decode(target));
    out.push_back(std::move(r));
  }
  return out;
}

std::string Package::content_type(std::string_view part) {
  if (!types_loaded_) {
    types_loaded_ = true;
    if (auto parsed = xml("[Content_Types].xml", false)) {
      for (util::XmlNode n : parsed->root().elements()) {
        if (n.local_name() == "Default") {
          type_defaults_[util::to_lower(n.attribute("Extension").value())] = std::string(n.attribute("ContentType").value());
        } else if (n.local_name() == "Override") {
          std::string_view name = n.attribute("PartName").value();
          if (name.starts_with('/')) name.remove_prefix(1);
          type_overrides_[util::to_lower(name)] = std::string(n.attribute("ContentType").value());
        }
      }
    }
  }
  if (part.starts_with('/')) part.remove_prefix(1);
  const std::string key = util::to_lower(part);
  if (auto it = type_overrides_.find(key); it != type_overrides_.end()) return it->second;
  const std::size_t dot = key.rfind('.');
  if (dot != std::string::npos) {
    if (auto it = type_defaults_.find(key.substr(dot + 1)); it != type_defaults_.end()) return it->second;
  }
  return {};
}

std::string rels_path_for(std::string_view part) {
  if (part.empty()) return "_rels/.rels";
  const std::size_t slash = part.rfind('/');
  if (slash == std::string_view::npos) return "_rels/" + std::string(part) + ".rels";
  return std::string(part.substr(0, slash)) + "/_rels/" + std::string(part.substr(slash + 1)) + ".rels";
}

std::string resolve_part(std::string_view source_part, std::string_view target) {
  if (const std::size_t hash = target.find('#'); hash != std::string_view::npos) target = target.substr(0, hash);
  std::string joined;
  if (target.starts_with('/')) {
    joined = std::string(target.substr(1));
  } else {
    const std::size_t slash = source_part.rfind('/');
    joined = (slash == std::string_view::npos ? std::string() : std::string(source_part.substr(0, slash + 1))) +
             std::string(target);
  }
  std::vector<std::string> segments;
  for (const std::string_view piece : util::split(joined, '/', false)) {
    if (piece == ".") continue;
    if (piece == "..") {
      if (!segments.empty()) segments.pop_back();
      continue;
    }
    segments.emplace_back(piece);
  }
  return util::join(segments, "/");
}

std::string percent_decode(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size()) {
      unsigned value = 0;
      const auto [ptr, ec] = std::from_chars(text.data() + i + 1, text.data() + i + 3, value, 16);
      if (ec == std::errc{} && ptr == text.data() + i + 3) {
        out.push_back(static_cast<char>(value));
        i += 2;
        continue;
      }
    }
    out.push_back(text[i]);
  }
  return out;
}

bool zip_mentions(ByteView data, std::string_view member, std::size_t window) {
  const std::string_view text = as_chars(data);
  if (!text.starts_with("PK\x03\x04")) return false;
  if (text.substr(0, window).find(member) != std::string_view::npos) return true;
  // The central directory sits at the end of the archive.
  const std::size_t tail = text.size() > window ? text.size() - window : 0;
  return text.substr(tail).find(member) != std::string_view::npos;
}

std::string ooxml_main_content_type(ByteView data, std::string_view main_part) {
  if (!as_chars(data).starts_with("PK\x03\x04")) return {};
  Limits limits;
  limits.max_archive_entries = 20000;
  limits.max_decompressed_bytes = 4 * 1024 * 1024;
  auto zip = util::ZipReader::open(data, limits);
  if (!zip) return {};
  const auto index = zip->find("[Content_Types].xml");
  if (!index || zip->entries()[*index].uncompressed_size > 1024 * 1024) return {};
  auto bytes = zip->read(*index);
  if (!bytes) return {};
  const std::string_view text = as_chars(*bytes);
  const std::string needle = "PartName=\"" + std::string(main_part) + "\"";
  std::size_t at = text.find(needle);
  if (at == std::string_view::npos) return {};
  const std::size_t start = text.rfind('<', at);
  const std::size_t end = text.find('>', at);
  if (start == std::string_view::npos || end == std::string_view::npos) return {};
  const std::string_view element = text.substr(start, end - start);
  const std::size_t type = element.find("ContentType=\"");
  if (type == std::string_view::npos) return {};
  const std::size_t close = element.find('"', type + 13);
  return close == std::string_view::npos ? std::string() : std::string(element.substr(type + 13, close - type - 13));
}

std::string leading_mimetype(ByteView data) {
  util::ByteReader reader(data);
  if (reader.u32le() != 0x04034B50u) return {};
  reader.skip(4);
  const std::uint16_t method = reader.u16le();
  reader.skip(8);
  const std::uint32_t compressed = reader.u32le();
  reader.skip(4);
  const std::uint16_t name_length = reader.u16le();
  const std::uint16_t extra_length = reader.u16le();
  if (!reader.ok() || method != 0 || name_length != 8 || reader.chars(8) != "mimetype") return {};
  reader.skip(extra_length);
  if (compressed > 200) return {};
  const std::string_view value = reader.chars(compressed);
  if (!reader.ok()) return {};
  return std::string(util::trim(value));
}

// =========================================================================== inlines

void InlineBuilder::text(std::string_view utf8, const RunFormat& format) {
  if (utf8.empty()) return;
  if (!pieces_.empty() && !pieces_.back().node && pieces_.back().format == format) {
    pieces_.back().text.append(utf8);
    return;
  }
  pieces_.push_back(Piece{format, std::nullopt, std::string(utf8)});
}

void InlineBuilder::node(ir::Inline inline_node, const RunFormat& format) {
  pieces_.push_back(Piece{format, std::move(inline_node), {}});
}

bool InlineBuilder::blank() const {
  for (const Piece& p : pieces_) {
    if (p.node) return false;
    if (!util::trim(p.text).empty()) return false;
  }
  return true;
}

std::string InlineBuilder::plain() const {
  std::string out;
  for (const Piece& p : pieces_) {
    if (p.node) {
      out.append(ir::plain_text(ir::Inlines{*p.node}));
    } else {
      out.append(p.text);
    }
  }
  return out;
}

namespace {

enum class Attr : int { Link, Bold, Italic, Underline, Strike, Super, Sub, Code, End };

bool active(const RunFormat& f, Attr a) {
  switch (a) {
    case Attr::Link: return !f.link.empty();
    case Attr::Bold: return f.bold;
    case Attr::Italic: return f.italic;
    case Attr::Underline: return f.underline;
    case Attr::Strike: return f.strike;
    case Attr::Super: return f.superscript;
    case Attr::Sub: return f.subscript && !f.superscript;
    case Attr::Code: return f.code;
    case Attr::End: return false;
  }
  return false;
}

bool same(const RunFormat& a, const RunFormat& b, Attr attr) {
  if (attr == Attr::Link) return a.link == b.link;
  return active(a, attr) == active(b, attr);
}

template <class Pieces>
ir::Inlines build_range(const Pieces& pieces, std::size_t begin, std::size_t end, int level) {
  ir::Inlines out;
  if (level >= static_cast<int>(Attr::End)) {
    for (std::size_t i = begin; i < end; ++i) {
      if (pieces[i].node) {
        out.push_back(*pieces[i].node);
      } else if (!out.empty() && out.back().template is<ir::Text>()) {
        out.back().template get_if<ir::Text>()->text.append(pieces[i].text);
      } else {
        out.push_back(ir::Text{pieces[i].text});
      }
    }
    return out;
  }
  const Attr attr = static_cast<Attr>(level);
  std::size_t i = begin;
  while (i < end) {
    std::size_t j = i + 1;
    while (j < end && same(pieces[i].format, pieces[j].format, attr)) ++j;
    if (!active(pieces[i].format, attr)) {
      ir::Inlines inner = build_range(pieces, i, j, level + 1);
      for (ir::Inline& x : inner) out.push_back(std::move(x));
    } else if (attr == Attr::Code) {
      std::string code;
      for (std::size_t k = i; k < j; ++k) {
        if (pieces[k].node) {
          if (!code.empty()) out.push_back(ir::Code{std::exchange(code, {})});
          out.push_back(*pieces[k].node);
        } else {
          code.append(pieces[k].text);
        }
      }
      if (!code.empty()) out.push_back(ir::Code{std::move(code)});
    } else {
      ir::Inlines inner = build_range(pieces, i, j, level + 1);
      switch (attr) {
        case Attr::Link: out.push_back(ir::Link{pieces[i].format.link, {}, std::move(inner)}); break;
        case Attr::Bold: out.push_back(ir::Strong{std::move(inner)}); break;
        case Attr::Italic: out.push_back(ir::Emphasis{std::move(inner)}); break;
        case Attr::Underline: out.push_back(ir::Underline{std::move(inner)}); break;
        case Attr::Strike: out.push_back(ir::Strikethrough{std::move(inner)}); break;
        case Attr::Super: out.push_back(ir::Superscript{std::move(inner)}); break;
        case Attr::Sub: out.push_back(ir::Subscript{std::move(inner)}); break;
        default: break;
      }
    }
    i = j;
  }
  return out;
}

void trim_front(ir::Inlines& inlines, int depth) {
  while (!inlines.empty()) {
    ir::Inline& first = inlines.front();
    if (auto* t = first.get_if<ir::Text>()) {
      std::string_view view = t->text;
      std::size_t n = 0;
      while (n < view.size() && (view[n] == ' ' || view[n] == '\t' || view[n] == '\n' || view[n] == '\r')) ++n;
      t->text.erase(0, n);
      if (t->text.empty()) {
        inlines.erase(inlines.begin());
        continue;
      }
      return;
    }
    if (first.is<ir::LineBreak>()) {
      inlines.erase(inlines.begin());
      continue;
    }
    if (depth > 16) return;
    bool emptied = false;
    std::visit(
        [&](auto& node) {
          using T = std::remove_cvref_t<decltype(node)>;
          if constexpr (requires { node.children; }) {
            if constexpr (!std::same_as<T, ir::Link>) {
              trim_front(node.children, depth + 1);
              emptied = node.children.empty();
            }
          }
        },
        first.node);
    if (emptied) {
      inlines.erase(inlines.begin());
      continue;
    }
    return;
  }
}

void trim_back(ir::Inlines& inlines, int depth) {
  while (!inlines.empty()) {
    ir::Inline& last = inlines.back();
    if (auto* t = last.get_if<ir::Text>()) {
      while (!t->text.empty() &&
             (t->text.back() == ' ' || t->text.back() == '\t' || t->text.back() == '\n' || t->text.back() == '\r')) {
        t->text.pop_back();
      }
      if (t->text.empty()) {
        inlines.pop_back();
        continue;
      }
      return;
    }
    if (last.is<ir::LineBreak>()) {
      inlines.pop_back();
      continue;
    }
    if (depth > 16) return;
    bool emptied = false;
    std::visit(
        [&](auto& node) {
          using T = std::remove_cvref_t<decltype(node)>;
          if constexpr (requires { node.children; }) {
            if constexpr (!std::same_as<T, ir::Link>) {
              trim_back(node.children, depth + 1);
              emptied = node.children.empty();
            }
          }
        },
        last.node);
    if (emptied) {
      inlines.pop_back();
      continue;
    }
    return;
  }
}

}  // namespace

ir::Inlines InlineBuilder::take() {
  ir::Inlines out = build_range(pieces_, 0, pieces_.size(), 0);
  pieces_.clear();
  return out;
}

void trim_inlines(ir::Inlines& inlines) {
  trim_front(inlines, 0);
  trim_back(inlines, 0);
}

// =========================================================================== lists

void FlowBuilder::close_to(std::size_t depth) {
  while (stack_.size() > depth) {
    ir::List list = std::move(stack_.back().list);
    stack_.pop_back();
    if (!stack_.empty()) {
      ir::List& parent = stack_.back().list;
      if (parent.items.empty()) parent.items.emplace_back();
      parent.items.back().push_back(std::move(list));
    } else {
      out_.push_back(std::move(list));
    }
  }
}

void FlowBuilder::block(ir::Block block) {
  close_to(0);
  out_.push_back(std::move(block));
}

void FlowBuilder::close_lists() { close_to(0); }

void FlowBuilder::continuation(ir::Block block) {
  if (stack_.empty()) {
    out_.push_back(std::move(block));
    return;
  }
  ir::List& list = stack_.back().list;
  if (list.items.empty()) list.items.emplace_back();
  list.items.back().push_back(std::move(block));
}

void FlowBuilder::list_item(const ListInfo& info, ir::Block block) {
  // Levels are compared with the levels of the open lists, so absolute
  // levels that start above 0 (or skip numbers) still nest correctly.
  const int level = std::clamp(info.level, 0, 64);
  if (!stack_.empty() && stack_.front().info.list_id != info.list_id && level <= stack_.front().info.level) close_to(0);
  while (!stack_.empty() && stack_.back().info.level > level) close_to(stack_.size() - 1);
  if (!stack_.empty() && stack_.back().info.level == level) {
    const ListInfo& top = stack_.back().info;
    if (top.ordered != info.ordered || (stack_.size() == 1 && top.list_id != info.list_id)) close_to(stack_.size() - 1);
  }
  if (stack_.empty() || stack_.back().info.level < level) {
    if (stack_.size() >= 9) {
      // Too deep: continue the deepest list.
      stack_.back().list.items.push_back(ir::Blocks{});
      stack_.back().list.items.back().push_back(std::move(block));
      return;
    }
    if (!stack_.empty() && stack_.back().list.items.empty()) stack_.back().list.items.emplace_back();
    Open open;
    open.info = info;
    open.info.level = level;
    open.list.ordered = info.ordered;
    open.list.numbering = info.numbering;
    open.list.start = info.start;
    stack_.push_back(std::move(open));
  }
  stack_.back().list.items.push_back(ir::Blocks{});
  stack_.back().list.items.back().push_back(std::move(block));
}

ir::Blocks FlowBuilder::take() {
  close_to(0);
  return std::exchange(out_, {});
}

// =========================================================================== pictures

std::optional<PixelSize> sniff_image_size(ByteView data) {
  const auto u8 = as_u8(data);
  const auto be16 = [&](std::size_t at) { return static_cast<std::uint32_t>((u8[at] << 8) | u8[at + 1]); };
  const auto be32 = [&](std::size_t at) {
    return (static_cast<std::uint32_t>(u8[at]) << 24) | (static_cast<std::uint32_t>(u8[at + 1]) << 16) |
           (static_cast<std::uint32_t>(u8[at + 2]) << 8) | u8[at + 3];
  };
  const auto le16 = [&](std::size_t at) { return static_cast<std::uint32_t>(u8[at] | (u8[at + 1] << 8)); };
  const auto le32 = [&](std::size_t at) {
    return static_cast<std::uint32_t>(u8[at]) | (static_cast<std::uint32_t>(u8[at + 1]) << 8) |
           (static_cast<std::uint32_t>(u8[at + 2]) << 16) | (static_cast<std::uint32_t>(u8[at + 3]) << 24);
  };
  const std::size_t n = u8.size();
  if (n >= 24 && u8[0] == 0x89 && u8[1] == 'P' && u8[2] == 'N' && u8[3] == 'G') {
    return PixelSize{be32(16), be32(20)};
  }
  if (n >= 10 && u8[0] == 'G' && u8[1] == 'I' && u8[2] == 'F') return PixelSize{le16(6), le16(8)};
  if (n >= 26 && u8[0] == 'B' && u8[1] == 'M') {
    const std::int32_t h = static_cast<std::int32_t>(le32(22));
    return PixelSize{le32(18), static_cast<std::uint32_t>(h < 0 ? -static_cast<std::int64_t>(h) : h)};
  }
  if (n >= 4 && u8[0] == 0xFF && u8[1] == 0xD8) {
    std::size_t at = 2;
    while (at + 9 < n) {
      if (u8[at] != 0xFF) {
        ++at;
        continue;
      }
      const std::uint8_t marker = u8[at + 1];
      if (marker == 0xFF) {
        ++at;
        continue;
      }
      if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
        at += 2;
        continue;
      }
      const std::uint32_t length = be16(at + 2);
      const bool sof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
      if (sof) return PixelSize{be16(at + 7), be16(at + 5)};
      if (length < 2) return std::nullopt;
      at += 2 + length;
    }
    return std::nullopt;
  }
  if (n >= 30 && std::memcmp(u8.data(), "RIFF", 4) == 0 && std::memcmp(u8.data() + 8, "WEBP", 4) == 0) {
    if (std::memcmp(u8.data() + 12, "VP8X", 4) == 0) {
      return PixelSize{1 + (le32(24) & 0xFFFFFF), 1 + (le32(27) & 0xFFFFFF)};
    }
    if (std::memcmp(u8.data() + 12, "VP8 ", 4) == 0) return PixelSize{le16(26) & 0x3FFF, le16(28) & 0x3FFF};
    if (std::memcmp(u8.data() + 12, "VP8L", 4) == 0) {
      const std::uint32_t bits = le32(21);
      return PixelSize{1 + (bits & 0x3FFF), 1 + ((bits >> 14) & 0x3FFF)};
    }
  }
  if (n >= 8 && ((u8[0] == 'I' && u8[1] == 'I' && u8[2] == 42) || (u8[0] == 'M' && u8[1] == 'M' && u8[3] == 42))) {
    const bool le = u8[0] == 'I';
    const auto r16 = [&](std::size_t at) { return le ? le16(at) : be16(at); };
    const auto r32 = [&](std::size_t at) { return le ? le32(at) : be32(at); };
    const std::uint64_t ifd = r32(4);
    if (ifd + 2 > n) return std::nullopt;
    const std::uint32_t count = r16(static_cast<std::size_t>(ifd));
    PixelSize size;
    for (std::uint32_t i = 0; i < count && i < 512; ++i) {
      const std::uint64_t entry = ifd + 2 + std::uint64_t{i} * 12;
      if (entry + 12 > n) break;
      const std::uint32_t tag = r16(static_cast<std::size_t>(entry));
      const std::uint32_t type = r16(static_cast<std::size_t>(entry + 2));
      const std::uint32_t value =
          type == 3 ? r16(static_cast<std::size_t>(entry + 8)) : r32(static_cast<std::size_t>(entry + 8));
      if (tag == 256) size.width = value;
      if (tag == 257) size.height = value;
    }
    if (size.width && size.height) return size;
  }
  return std::nullopt;
}

std::string extension_for_mime(std::string_view mime) {
  if (mime == "image/png") return "png";
  if (mime == "image/jpeg" || mime == "image/jpg") return "jpeg";
  if (mime == "image/gif") return "gif";
  if (mime == "image/bmp" || mime == "image/x-bmp") return "bmp";
  if (mime == "image/tiff") return "tiff";
  if (mime == "image/webp") return "webp";
  if (mime == "image/svg+xml") return "svg";
  if (mime == "image/x-emf" || mime == "image/emf") return "emf";
  if (mime == "image/x-wmf" || mime == "image/wmf") return "wmf";
  return "bin";
}

std::string mime_for_extension(std::string_view name) {
  const std::size_t dot = name.rfind('.');
  const std::string ext = util::to_lower(dot == std::string_view::npos ? name : name.substr(dot + 1));
  if (ext == "png") return "image/png";
  if (ext == "jpg" || ext == "jpeg" || ext == "jpe") return "image/jpeg";
  if (ext == "gif") return "image/gif";
  if (ext == "bmp" || ext == "dib") return "image/bmp";
  if (ext == "tif" || ext == "tiff") return "image/tiff";
  if (ext == "webp") return "image/webp";
  if (ext == "svg") return "image/svg+xml";
  if (ext == "emf") return "image/x-emf";
  if (ext == "wmf") return "image/x-wmf";
  if (ext == "xhtml" || ext == "html" || ext == "htm") return "application/xhtml+xml";
  if (ext == "css") return "text/css";
  if (ext == "ncx") return "application/x-dtbncx+xml";
  return {};
}

ImagePolicy image_policy(const Options& options) {
  ImagePolicy policy;
  const std::string mode = options.get_enum("images", "optimize", {"keep", "optimize", "drop"});
  policy.mode = mode == "keep" ? ImageMode::Keep : mode == "drop" ? ImageMode::Drop : ImageMode::Optimize;
  policy.quality = options::resolved_quality(options);
  policy.max_size = static_cast<std::uint32_t>(options.get_int("max_image_size", 0, 0, 20000));
  if (policy.max_size == 0 && !options.was_set("max_image_size")) {
    // Presets below "high" also cap the picture size.
    const auto preset = options::resolved_preset(options);
    if (preset == options::Preset::Balanced) policy.max_size = 2048;
    if (preset == options::Preset::Small) policy.max_size = 1280;
  }
  return policy;
}

namespace {

std::string image_format_for_mime(std::string_view mime) {
  if (mime == "image/png") return "png";
  if (mime == "image/jpeg") return "jpg";
  if (mime == "image/gif") return "gif";
  if (mime == "image/bmp") return "bmp";
  if (mime == "image/tiff") return "tiff";
  if (mime == "image/webp") return "webp";
  return {};
}

bool is_jpeg(std::string_view mime) { return mime == "image/jpeg"; }

}  // namespace

std::optional<PreparedImage> prepare_image(const ir::ImageSource& source, const ImagePolicy& policy,
                                           const Context& ctx) {
  if (policy.mode == ImageMode::Drop) return std::nullopt;
  PreparedImage out;
  std::optional<ir::Image> decoded;
  if (const auto* encoded = std::get_if<ir::EncodedImage>(&source)) {
    if (encoded->bytes.empty()) return std::nullopt;
    out.bytes = encoded->bytes;
    out.mime = encoded->mime.empty() ? document::sniff_image_mime(encoded->bytes) : encoded->mime;
    if (out.mime == "image/jpg") out.mime = "image/jpeg";
  } else {
    decoded = std::get<ir::Image>(source);
    if (!decoded->valid()) return std::nullopt;
    auto png = image::encode("png", *decoded, Options{}, ctx);
    if (!png) return std::nullopt;
    out.bytes = std::move(*png);
    out.mime = "image/png";
  }
  if (const auto size = sniff_image_size(out.bytes)) out.size = *size;

  if (policy.mode == ImageMode::Optimize) {
    const std::string format = image_format_for_mime(out.mime);
    const bool lossy_ok = policy.quality < 90 || is_jpeg(out.mime);
    const bool wants_resize =
        policy.max_size > 0 && (out.size.width > policy.max_size || out.size.height > policy.max_size);
    // Only touch pictures when something can be gained: a resize, a quality
    // below the default, or a format that compresses badly (BMP, TIFF).
    const bool convertible = out.mime == "image/bmp" || out.mime == "image/tiff";
    if (!format.empty() && image::can_decode(format, ctx) &&
        (wants_resize || policy.quality < 90 || convertible)) {
      if (!decoded) {
        auto d = image::decode(format, out.bytes, ctx);
        if (d) decoded = std::move(*d);
      }
      if (decoded && decoded->valid()) {
        ir::Image picture = std::move(*decoded);
        if (picture.frames.size() > 1 && out.mime == "image/gif") {
          // Animated pictures are kept as they are.
        } else {
          if (wants_resize) {
            const std::uint32_t w = picture.width();
            const std::uint32_t h = picture.height();
            const double scale = static_cast<double>(policy.max_size) / std::max(w, h);
            const auto nw = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(w * scale)));
            const auto nh = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(h * scale)));
            if (auto r = image::resize(picture, nw, nh); !r) return out;
          }
          const bool opaque = std::ranges::all_of(picture.frames, [](const ir::Frame& f) { return image::is_opaque(f); });
          const bool to_jpeg = is_jpeg(out.mime) || (lossy_ok && opaque && policy.quality < 90 && out.mime != "image/gif");
          Options encode_options;
          std::string target = "png";
          if (to_jpeg && image::can_encode("jpg", ctx)) {
            target = "jpg";
            encode_options.set("quality", Json(static_cast<std::int64_t>(policy.quality)));
          } else {
            encode_options.set("compression_level", Json(std::int64_t{9}));
          }
          encode_options.set("strip_metadata", Json(true));
          auto encoded = image::encode(target, picture, encode_options, ctx);
          if (encoded && (encoded->size() < out.bytes.size() || wants_resize || convertible)) {
            out.bytes = std::move(*encoded);
            out.mime = target == "jpg" ? "image/jpeg" : "image/png";
            out.size = PixelSize{picture.width(), picture.height()};
          }
        }
      }
    }
  }
  out.extension = extension_for_mime(out.mime);
  if (out.extension == "jpeg") out.extension = "jpg";
  return out;
}

bool is_web_safe(std::string_view extension) {
  return extension == "png" || extension == "jpg" || extension == "jpeg" || extension == "gif" || extension == "bmp" ||
         extension == "tiff";
}

std::optional<PreparedImage> convert_to_png(const PreparedImage& image, const Context& ctx) {
  const std::string format = image_format_for_mime(image.mime);
  if (format.empty() || !image::can_decode(format, ctx)) return std::nullopt;
  auto decoded = image::decode(format, image.bytes, ctx);
  if (!decoded || !decoded->valid()) return std::nullopt;
  auto png = image::encode("png", *decoded, Options{}, ctx);
  if (!png) return std::nullopt;
  PreparedImage out;
  out.bytes = std::move(*png);
  out.mime = "image/png";
  out.extension = "png";
  out.size = PixelSize{decoded->width(), decoded->height()};
  return out;
}

std::size_t MediaStore::add(PreparedImage image, std::string_view prefix) {
  for (std::size_t i = 0; i < items_.size(); ++i) {
    if (items_[i].image.bytes == image.bytes) return i;
  }
  Item item;
  item.name = std::string(prefix) + std::to_string(items_.size() + 1) + "." + image.extension;
  item.image = std::move(image);
  items_.push_back(std::move(item));
  return items_.size() - 1;
}

// =========================================================================== options

std::vector<OptionSpec> document_read_options() {
  return {
      OptionSpec::boolean("include_comments", "Include comments", false,
                          "Append reviewer comments as notes at the place they refer to."),
      OptionSpec::boolean("include_headers", "Include headers and footers", false,
                          "Add the text of page headers and footers at the start and end of the document."),
      OptionSpec::boolean("include_notes", "Include notes", true,
                          "Keep footnotes and endnotes (and speaker notes of presentations)."),
      OptionSpec::boolean("include_hidden", "Include hidden text", false, "Keep text formatted as hidden.")
          .advanced(),
      OptionSpec::boolean("show_changes", "Show tracked changes", false,
                          "Show deleted text struck through and inserted text underlined instead of accepting "
                          "all changes.")
          .advanced(),
      OptionSpec::boolean("strict_headings", "Strict heading detection", false,
                          "Recognise headings only from outline levels and heading styles, never from "
                          "formatting such as large bold lines.")
          .advanced(),
      options::strip_metadata(),
  };
}

std::vector<OptionSpec> image_write_options() {
  return {
      OptionSpec::choice("images", "Pictures", "optimize", {"keep", "optimize", "drop"},
                         "Keep pictures as they are, optimise them (downscale and re-encode when it makes the "
                         "file smaller), or leave them out."),
      options::quality(),
      options::preset(),
      OptionSpec::integer("max_image_size", "Largest picture side", 0, 0, 20000,
                          "Downscale pictures whose longer side exceeds this many pixels (0 = no limit).")
          .advanced(),
  };
}

std::vector<OptionSpec> document_write_options(bool pages) {
  std::vector<OptionSpec> out;
  if (pages) {
    out.push_back(OptionSpec::choice("page_size", "Page size", "a4", {"a4", "letter", "legal", "a5", "a3"},
                                     "Paper size of the pages."));
    out.push_back(OptionSpec::choice("orientation", "Orientation", "portrait", {"portrait", "landscape"},
                                     "Page orientation."));
    out.push_back(OptionSpec::number("margin_mm", "Margins (mm)", 25.4, 0.0, 100.0,
                                     "Width of the page margins in millimetres."));
  }
  out.push_back(OptionSpec::text("language", "Language", "",
                                 "Language of the text as a BCP 47 tag such as en-GB (empty: from the source).")
                    .advanced());
  for (OptionSpec& spec : image_write_options()) out.push_back(std::move(spec));
  out.push_back(options::compression_level());
  out.push_back(options::strip_metadata());
  return out;
}

PageSetup page_setup(const Options& options) {
  PageSetup setup;
  const std::string size = options.get_enum("page_size", "a4", {"a4", "letter", "legal", "a5", "a3"});
  if (size == "letter") {
    setup.width = 12240;
    setup.height = 15840;
  } else if (size == "legal") {
    setup.width = 12240;
    setup.height = 20160;
  } else if (size == "a5") {
    setup.width = 8391;
    setup.height = 11906;
  } else if (size == "a3") {
    setup.width = 16838;
    setup.height = 23811;
  }
  if (options.get_enum("orientation", "portrait", {"portrait", "landscape"}) == "landscape") {
    std::swap(setup.width, setup.height);
  }
  const double margin_mm = options.get_float("margin_mm", 25.4, 0.0, 100.0);
  setup.margin = static_cast<int>(std::lround(margin_mm / 25.4 * 1440.0));
  setup.margin = std::min(setup.margin, std::min(setup.width, setup.height) / 3);
  return setup;
}

ReadSettings read_settings(const Options& options) {
  ReadSettings s;
  s.comments = options.get_bool("include_comments", false);
  s.headers = options.get_bool("include_headers", false);
  s.notes = options.get_bool("include_notes", true);
  s.hidden = options.get_bool("include_hidden", false);
  s.tracked_changes = options.get_bool("show_changes", false);
  s.strict_headings = options.get_bool("strict_headings", false);
  s.metadata = !options::resolved_strip_metadata(options);
  return s;
}

int heading_level_from_style_name(std::string_view name) {
  std::string key;
  const std::string spaced = util::replace_all(name, "_20_", " ");
  for (char c : spaced) {
    if (c == ' ' || c == '-' || c == '_') continue;
    key.push_back(util::ascii_lower(c));
  }
  if (key == "title") return 1;
  static constexpr std::array<std::string_view, 9> prefixes = {
      "heading", "\xC3\xBC" "berschrift", "titre", "t\xC3\xADtulo", "titolo", "kop", "rubrik", "otsikko", "nag\xC5\x82\xC3\xB3wek"};
  for (std::string_view prefix : prefixes) {
    // Compare ASCII-lowered; the non-ASCII prefixes are stored lower-case already.
    if (key.size() > prefix.size() && std::string_view(key).starts_with(prefix)) {
      const auto n = util::parse_int(std::string_view(key).substr(prefix.size()));
      if (n && *n >= 1 && *n <= 9) return static_cast<int>(std::min<std::int64_t>(*n, 6));
    }
  }
  return 0;
}

void finish_metadata(ir::Document& document, const ReadSettings& settings) {
  if (!settings.metadata) document::strip_metadata(document);
  document::enforce_depth(document);
}

void unwrap_header_emphasis(ir::TableBlock& table) {
  for (ir::TableRow& row : table.rows) {
    if (!row.header) continue;
    for (ir::TableCell& cell : row.cells) {
      if (cell.content.size() == 1 && cell.content.front().is<ir::Strong>()) {
        ir::Inlines inner = std::move(cell.content.front().get_if<ir::Strong>()->children);
        cell.content = std::move(inner);
      }
    }
  }
}

void merge_adjacent_blocks(ir::Blocks& blocks) {
  ir::Blocks merged;
  merged.reserve(blocks.size());
  for (ir::Block& b : blocks) {
    if (!merged.empty()) {
      if (auto* code = b.get_if<ir::CodeBlock>(); code && merged.back().is<ir::CodeBlock>() && b.attr.empty()) {
        merged.back().get_if<ir::CodeBlock>()->code += "\n" + code->code;
        continue;
      }
      if (auto* quote = b.get_if<ir::BlockQuote>(); quote && merged.back().is<ir::BlockQuote>() && b.attr.empty() &&
                                                    merged.back().attr.empty()) {
        for (ir::Block& inner : quote->blocks) merged.back().get_if<ir::BlockQuote>()->blocks.push_back(std::move(inner));
        continue;
      }
    }
    merged.push_back(std::move(b));
  }
  blocks = std::move(merged);
}

ir::Inlines text_with_breaks(std::string_view text) {
  ir::Inlines out;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t nl = text.find('\n', start);
    const std::string_view piece = text.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
    if (!piece.empty()) out.push_back(ir::Text{std::string(piece)});
    if (nl == std::string_view::npos) break;
    out.push_back(ir::LineBreak{});
    start = nl + 1;
  }
  return out;
}

std::string fmt_number(double value) {
  const double rounded = std::round(value * 1000.0) / 1000.0;
  std::array<char, 64> buffer{};
  const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), rounded, std::chars_format::fixed, 3);
  if (ec != std::errc{}) return "0";
  std::string out(buffer.data(), end);
  while (!out.empty() && out.back() == '0') out.pop_back();
  if (!out.empty() && out.back() == '.') out.pop_back();
  if (out == "-0") out = "0";
  return out;
}

}  // namespace cvt::office
