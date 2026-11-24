// Convertible core - office family: Compiled HTML Help (.chm) as a document.
//
// The topics are read in the order of the table of contents (.hhc; the
// keyword index .hhk or the file order when there is none), then every topic
// they link to that the contents leave out. Each topic goes through the
// document family's HTML reader in the code page of the help file's locale
// (#SYSTEM, unless the page declares its own charset); the contents'
// hierarchy gives the headings, links between topics become in-document
// anchors and pictures are embedded. Title and language come from #SYSTEM
// (or #WINDOWS). The format itself (FormatInfo) and the CHM -> Archive
// reader belong to the archive family; this file adds the Document reader.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/document/document_api.hpp"
#include "families/office/its.hpp"
#include "families/office/lcid.hpp"
#include "families/office/office.hpp"
#include "families/office/office_common.hpp"
#include "util/charsets.hpp"
#include "util/text.hpp"

namespace cvt::office {

namespace {

std::uint32_t le16(ByteView d, std::size_t at) {
  return std::to_integer<std::uint32_t>(d[at]) | (std::to_integer<std::uint32_t>(d[at + 1]) << 8);
}
std::uint32_t le32(ByteView d, std::size_t at) { return le16(d, at) | (le16(d, at + 2) << 16); }

std::string zstring(ByteView raw) {
  const std::string_view text = as_chars(raw);
  return std::string(text.substr(0, text.find('\0')));
}

bool alnum(char c) { return util::is_ascii_alpha(c) || util::is_ascii_digit(c); }

bool is_topic_name(std::string_view name) {
  for (const std::string_view ext : {".htm", ".html", ".xhtml", ".xhtm", ".shtml", ".stm"}) {
    if (util::iends_with(name, ext)) return true;
  }
  return false;
}

std::string stem_of(std::string_view path) {
  const std::size_t slash = path.rfind('/');
  std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
  const std::size_t dot = name.rfind('.');
  return std::string(dot == std::string_view::npos ? name : name.substr(0, dot));
}

// --------------------------------------------------------------------------- text

/// Turns the help file's bytes into UTF-8: valid UTF-8 stays as it is, other
/// text is read in the code page of the file's locale.
class TextDecoder {
 public:
  explicit TextDecoder(int code_page) : charset_(util::charset_for_code_page(code_page)) {
    if (charset_ == nullptr) charset_ = util::charset_for_code_page(1252);
  }
  [[nodiscard]] std::string operator()(std::string_view raw) const {
    if (util::is_valid_utf8(raw) || charset_ == nullptr) return std::string(raw);
    return util::charset_decode(as_bytes(raw), *charset_).text;
  }

 private:
  const util::CharsetInfo* charset_ = nullptr;
};

/// Character references in a sitemap value ("Caf&eacute;").
std::string unescape(std::string_view text) {
  if (text.find('&') == std::string_view::npos) return std::string(text);
  std::string markup = "<p>";
  for (const char c : text) markup += c == '<' ? std::string("&lt;") : std::string(1, c);
  markup += "</p>";
  document::HtmlReadOptions options;
  options.read_metadata = false;
  auto doc = document::parse_html_text(markup, options);
  if (!doc) return std::string(text);
  return std::string(util::trim(ir::plain_text(*doc)));
}

// --------------------------------------------------------------------------- sitemaps

struct SitemapEntry {
  int level = 1;
  std::string name;
  std::string local;
};

/// Value of attribute `key` inside a tag's text ("param name=Local value=x").
std::string tag_attribute(std::string_view tag, std::string_view key) {
  std::size_t i = 0;
  while (i < tag.size()) {
    while (i < tag.size() && !util::is_ascii_alpha(tag[i])) ++i;
    const std::size_t start = i;
    while (i < tag.size() && (alnum(tag[i]) || tag[i] == '-' || tag[i] == '_' || tag[i] == ':')) ++i;
    const std::string_view name = tag.substr(start, i - start);
    while (i < tag.size() && tag[i] == ' ') ++i;
    if (i >= tag.size() || tag[i] != '=') continue;
    ++i;
    while (i < tag.size() && (tag[i] == ' ' || tag[i] == '\t' || tag[i] == '\n' || tag[i] == '\r')) ++i;
    std::string_view value;
    if (i < tag.size() && (tag[i] == '"' || tag[i] == '\'')) {
      const char quote = tag[i++];
      const std::size_t close = tag.find(quote, i);
      value = tag.substr(i, close == std::string_view::npos ? std::string_view::npos : close - i);
      i = close == std::string_view::npos ? tag.size() : close + 1;
    } else {
      const std::size_t begin = i;
      while (i < tag.size() && tag[i] != ' ' && tag[i] != '\t' && tag[i] != '\n' && tag[i] != '\r' && tag[i] != '>') ++i;
      value = tag.substr(begin, i - begin);
    }
    if (util::iequals(name, key)) return std::string(value);
  }
  return {};
}

/// The <OBJECT type="text/sitemap"> entries of a .hhc / .hhk file with
/// their <UL> nesting depth.
std::vector<SitemapEntry> parse_sitemap(std::string_view text, std::size_t max_entries) {
  std::vector<SitemapEntry> out;
  int depth = 0;
  std::optional<SitemapEntry> current;
  std::size_t at = 0;
  while ((at = text.find('<', at)) != std::string_view::npos && out.size() < max_entries) {
    const std::size_t end = text.find('>', at);
    if (end == std::string_view::npos) break;
    const std::string_view tag = text.substr(at + 1, end - at - 1);
    at = end + 1;
    std::size_t n = 0;
    const bool closing = !tag.empty() && tag[0] == '/';
    if (closing) ++n;
    const std::size_t name_start = n;
    while (n < tag.size() && alnum(tag[n])) ++n;
    const std::string name = util::to_lower(tag.substr(name_start, n - name_start));
    if (name == "ul" || name == "ol") {
      depth = std::clamp(depth + (closing ? -1 : 1), 0, 64);
    } else if (name == "object") {
      if (!closing) {
        current.reset();
        if (util::iequals(tag_attribute(tag, "type"), "text/sitemap")) current = SitemapEntry{std::max(depth, 1), {}, {}};
      } else if (current) {
        if (!current->name.empty() || !current->local.empty()) out.push_back(std::move(*current));
        current.reset();
      }
    } else if (name == "param" && current) {
      const std::string key = tag_attribute(tag, "name");
      std::string value = tag_attribute(tag, "value");
      if (util::iequals(key, "Name") && current->name.empty()) current->name = std::move(value);
      if (util::iequals(key, "Local") && current->local.empty()) current->local = std::move(value);
    }
  }
  return out;
}

// --------------------------------------------------------------------------- tree helpers

void for_each_block(ir::Blocks& blocks, const std::function<void(ir::Block&)>& fn, int depth = 0) {
  if (depth > ir::max_document_depth + 4) return;
  for (ir::Block& block : blocks) {
    fn(block);
    if (auto* list = block.get_if<ir::List>()) {
      for (ir::Blocks& item : list->items) for_each_block(item, fn, depth + 1);
    } else if (auto* quote = block.get_if<ir::BlockQuote>()) {
      for_each_block(quote->blocks, fn, depth + 1);
    }
  }
}

void for_each_inline(ir::Inlines& inlines, const std::function<void(ir::Inline&)>& fn, int depth) {
  if (depth > ir::max_document_depth + 4) return;
  for (ir::Inline& item : inlines) {
    fn(item);
    std::visit(
        [&](auto& node) {
          if constexpr (requires { node.children; }) for_each_inline(node.children, fn, depth + 1);
        },
        item.node);
  }
}

void for_each_inline(ir::Blocks& blocks, const std::function<void(ir::Inline&)>& fn) {
  for_each_block(blocks, [&](ir::Block& block) {
    if (auto* p = block.get_if<ir::Paragraph>()) for_each_inline(p->content, fn, 0);
    if (auto* h = block.get_if<ir::Heading>()) for_each_inline(h->content, fn, 0);
    if (auto* t = block.get_if<ir::TableBlock>()) {
      for (ir::TableRow& row : t->rows) {
        for (ir::TableCell& cell : row.cells) for_each_inline(cell.content, fn, 0);
      }
      for_each_inline(t->caption, fn, 0);
    }
  });
}

/// `<a name="x">` targets are dropped by the HTML reader (only block ids are
/// kept), so give the enclosing block that id: the nearest preceding block
/// start tag without an id gets `id="x"`.
std::string promote_named_anchors(std::string_view html) {
  std::string out;
  out.reserve(html.size() + 64);
  std::optional<std::size_t> open_block;  // position in `out` just after the tag name
  std::size_t at = 0;
  while (at < html.size()) {
    const std::size_t lt = html.find('<', at);
    if (lt == std::string_view::npos) {
      out.append(html.substr(at));
      break;
    }
    out.append(html.substr(at, lt - at));
    const std::size_t gt = html.find('>', lt);
    if (gt == std::string_view::npos) {
      out.append(html.substr(lt));
      break;
    }
    const std::string_view tag = html.substr(lt + 1, gt - lt - 1);
    std::size_t n = 0;
    while (n < tag.size() && alnum(tag[n])) ++n;
    const std::string name = util::to_lower(tag.substr(0, n));
    const std::size_t tag_out = out.size();
    out.append(html.substr(lt, gt - lt + 1));
    at = gt + 1;
    static constexpr std::string_view blocks[] = {"h1", "h2", "h3", "h4", "h5", "h6", "p", "div", "li", "dt", "dd", "pre",
                                                  "blockquote", "table", "center"};
    if (std::ranges::find(blocks, name) != std::end(blocks)) {
      open_block = tag_attribute(tag, "id").empty() ? std::optional<std::size_t>(tag_out + 1 + n) : std::nullopt;
    } else if (name == "a" && open_block) {
      const std::string anchor = tag_attribute(tag, "name");
      if (!anchor.empty() && anchor.find('"') == std::string::npos) {
        out.insert(*open_block, " id=\"" + anchor + "\"");
        open_block.reset();
      }
    }
  }
  return out;
}

void shift_headings(ir::Blocks& blocks, int delta) {
  if (delta == 0) return;
  for_each_block(blocks, [delta](ir::Block& block) {
    if (auto* h = block.get_if<ir::Heading>()) h->level = std::clamp(h->level + delta, 1, 6);
  });
}

// --------------------------------------------------------------------------- the reader

constexpr std::string_view link_marker = "chm-topic:";

class ChmBook {
 public:
  ChmBook(ReadContext& ctx, its::Storage& storage) : ctx_(ctx), storage_(storage) {}

  Result<ir::Document> read() {
    read_system();
    const int code_page = lcid::code_page(lcid_);
    if (!lcid::html_charset(code_page).empty()) {
      charset_ = std::string(lcid::html_charset(code_page));
    } else {
      ctx_.warn("The help file's code page " + std::to_string(code_page) +
                " is not supported; pages without a declared charset are read as Windows-1252");
    }
    const TextDecoder decode(lcid::html_charset(code_page).empty() ? 1252 : code_page);
    breaks_ = ctx_.options.get_bool("chapter_breaks", false);
    toc_headings_ = ctx_.options.get_bool("toc_titles", true);

    // Every topic file, and its anchor.
    std::set<std::string> used;
    for (const auto& [name, entry] : storage_.entries()) {
      if (!name.starts_with('/') || name.starts_with("/#") || name.starts_with("/$") || !is_topic_name(name)) continue;
      const std::string path = name.substr(1);
      topics_[util::to_lower(path)] = path;
      anchor_[util::to_lower(path)] = document::unique_slug("topic-" + stem_of(path), used);
    }
    if (topics_.empty()) return fail(Status::Decode, "the help file contains no pages");

    // The order: table of contents, else keyword index, else file order.
    std::vector<SitemapEntry> order;
    for (const auto& [file, from_index] : {std::pair{contents_, false}, std::pair{index_, true}}) {
      if (!order.empty()) break;
      std::string path = file;
      if (path.empty()) path = first_with_extension(from_index ? ".hhk" : ".hhc");
      if (path.empty()) continue;
      auto raw = storage_.read("/" + path);
      if (!raw) {
        ctx_.warn("The help file's " + std::string(from_index ? "index" : "table of contents") + " cannot be read");
        continue;
      }
      std::vector<SitemapEntry> entries = parse_sitemap(decode(as_chars(*raw)), ctx_.limits.max_archive_entries);
      for (SitemapEntry& e : entries) {
        e.name = unescape(e.name);
        e.local = resolve(path, unescape(e.local));
        if (from_index) e.level = 1;
      }
      const bool useful = std::ranges::any_of(entries, [&](const SitemapEntry& e) { return topic_key(e.local).has_value(); });
      if (useful) order = std::move(entries);
      from_index_ = from_index && useful;
    }
    if (order.empty()) {
      for (const auto& [key, path] : topics_) order.push_back(SitemapEntry{1, {}, path});
      from_index_ = true;
    }

    std::size_t done = 0;
    for (const SitemapEntry& e : order) {
      CVT_TRY(ctx_.checkpoint(static_cast<double>(done++) / static_cast<double>(order.size() + 1), "Reading topics"));
      const auto key = topic_key(e.local);
      if (!key) {
        // A book node of the contents (no page of its own).
        if (!e.local.empty() || e.name.empty() || from_index_ || !toc_headings_) continue;
        add_break();
        doc_.blocks.push_back(ir::Heading{std::clamp(e.level, 1, 6), {ir::Text{e.name}}});
        continue;
      }
      if (emitted_.contains(*key)) continue;
      CVT_TRY(emit(*key, from_index_ ? 0 : std::clamp(e.level, 1, 6), from_index_ ? std::string() : e.name));
    }
    // Pages that are only reachable through links.
    for (std::size_t i = 0; i < pending_.size(); ++i) {
      const std::string key = pending_[i];
      if (emitted_.contains(key)) continue;
      CVT_TRY(ctx_.checkpoint(0.99, "Reading topics"));
      CVT_TRY(emit(key, 0, {}));
    }
    if (doc_.blocks.empty()) return fail(Status::Decode, "the help file's pages are empty");
    resolve_links();

    doc_.metadata.title = util::trim(decode(title_)).empty() ? std::string() : std::string(util::trim(decode(title_)));
    doc_.metadata.language = std::string(lcid::language_tag(lcid_));
    if (!compiler_.empty()) doc_.metadata.extra["generator"] = decode(compiler_);
    if (!doc_.footnotes.empty()) document::number_footnotes(doc_);
    finish_metadata(doc_, read_settings(ctx_.options));
    return std::move(doc_);
  }

 private:
  void read_system() {
    if (auto raw = storage_.read("/#SYSTEM"); raw && raw->size() >= 4) {
      const ByteView d = *raw;
      std::size_t pos = 4;
      while (pos + 4 <= d.size()) {
        const std::uint32_t code = le16(d, pos);
        const std::size_t length = le16(d, pos + 2);
        pos += 4;
        if (length > d.size() - pos) break;
        const ByteView value = d.subspan(pos, length);
        pos += length;
        switch (code) {
          case 0: contents_ = zstring(value); break;
          case 1: index_ = zstring(value); break;
          case 3: title_ = zstring(value); break;
          case 4:
            if (value.size() >= 4) lcid_ = le32(value, 0);
            break;
          case 9: compiler_ = zstring(value); break;
          default: break;
        }
      }
    }
    if (!title_.empty() && !contents_.empty() && !index_.empty()) return;
    // #WINDOWS: window definitions whose strings live in #STRINGS.
    auto windows = storage_.read("/#WINDOWS");
    auto strings = storage_.read("/#STRINGS");
    if (!windows || !strings || windows->size() < 8) return;
    const ByteView w = *windows;
    const std::uint32_t count = le32(w, 0);
    const std::uint32_t size = le32(w, 4);
    if (count == 0 || size < 0x70 || size > w.size() - 8) return;
    const ByteView entry = w.subspan(8, size);
    const auto string_at = [&](std::size_t field) -> std::string {
      const std::uint32_t offset = le32(entry, field);
      if (offset == 0 || offset >= strings->size()) return {};
      return zstring(ByteView(*strings).subspan(offset));
    };
    if (title_.empty()) title_ = string_at(0x14);
    if (contents_.empty()) contents_ = string_at(0x60);
    if (index_.empty()) index_ = string_at(0x64);
  }

  std::string first_with_extension(std::string_view ext) const {
    for (const auto& [name, entry] : storage_.entries()) {
      if (name.starts_with('/') && util::iends_with(name, ext)) return name.substr(1);
    }
    return {};
  }

  /// A reference resolved against `from` (a path without leading '/');
  /// "ms-its:" / "mk:@MSITStore:" references keep their in-file part.
  static std::string resolve(std::string_view from, std::string_view reference) {
    reference = util::trim(reference);
    std::string absolute;
    if (const std::size_t sep = reference.find("::"); sep != std::string_view::npos) {
      absolute = std::string(reference.substr(sep + 2));
      if (!absolute.starts_with('/')) absolute.insert(absolute.begin(), '/');
      reference = absolute;
    }
    if (reference.empty()) return {};
    std::string fragment;
    if (const std::size_t hash = reference.find('#'); hash != std::string_view::npos) {
      fragment = std::string(reference.substr(hash));
      reference = reference.substr(0, hash);
    }
    if (const std::size_t query = reference.find('?'); query != std::string_view::npos) reference = reference.substr(0, query);
    if (reference.empty()) return fragment.empty() ? std::string() : std::string(from) + fragment;
    return resolve_part(from, percent_decode(reference)) + fragment;
  }

  /// The topic a resolved reference points at (fragment ignored).
  std::optional<std::string> topic_key(std::string_view reference) const {
    const std::string path = std::string(reference.substr(0, reference.find('#')));
    std::string_view p = path;
    while (p.starts_with('/')) p.remove_prefix(1);
    const std::string key = util::to_lower(p);
    if (!topics_.contains(key)) return std::nullopt;
    return key;
  }

  void add_break() {
    if (breaks_ && !doc_.blocks.empty()) doc_.blocks.push_back(ir::PageBreak{});
  }

  /// Read one topic; `level` 0 = not in the contents (keep its own headings).
  Result<void> emit(const std::string& key, int level, const std::string& toc_name) {
    emitted_.insert(key);
    CVT_TRY(ctx_.limits.check_archive_entries(emitted_.size()));
    const std::string& path = topics_.at(key);
    auto raw = storage_.read("/" + path);
    if (!raw) {
      if (raw.error().code == Status::Limit) return std::unexpected(raw.error());
      ctx_.warn("A page of the help file cannot be read: " + path);
      return {};
    }
    const std::string html = promote_named_anchors(as_chars(*raw));
    document::HtmlReadOptions options;
    options.fallback_encoding = charset_;
    options.read_metadata = true;
    options.resolve = [this, &path](std::string_view url) -> std::optional<ir::EncodedImage> {
      if (url.find("://") != std::string_view::npos || url.starts_with("data:")) return std::nullopt;
      std::string target = resolve(path, url);
      target = target.substr(0, target.find('#'));
      while (target.starts_with('/')) target.erase(0, 1);
      if (target.empty()) return std::nullopt;
      auto bytes = storage_.read("/" + target);
      if (!bytes) return std::nullopt;
      ir::EncodedImage image;
      image.mime = document::sniff_image_mime(*bytes);
      if (image.mime.empty()) image.mime = mime_for_extension(target);
      image.bytes = std::move(*bytes);
      return image;
    };
    std::vector<std::string> warnings;
    CVT_TRY_ASSIGN(ir::Document topic, document::parse_html(as_bytes(html), options, &warnings));
    for (std::string& w : warnings) ctx_.warn(std::move(w));

    // Links: other topics become markers resolved once every anchor is known.
    for_each_inline(topic.blocks, [&](ir::Inline& item) {
      auto* link = item.get_if<ir::Link>();
      if (!link || !link->note.empty() || link->url.empty()) return;
      const std::string_view url = link->url;
      if (url.find("://") != std::string_view::npos && url.find("::") == std::string_view::npos) return;
      if (url.starts_with("mailto:") || url.starts_with("javascript:")) return;
      std::string target = url.starts_with('#') ? path + std::string(url) : resolve(path, url);
      while (target.starts_with('/')) target.erase(0, 1);
      const auto target_key = topic_key(target);
      if (!target_key) return;
      if (!emitted_.contains(*target_key)) pending_.push_back(*target_key);
      const std::size_t hash = target.find('#');
      link->url = std::string(link_marker) + *target_key + (hash == std::string::npos ? std::string() : target.substr(hash));
    });

    // Headings: the contents' hierarchy, the topic's own headings below it.
    const int topic_min = [&] {
      int m = 7;
      for_each_block(topic.blocks, [&](ir::Block& b) {
        if (const auto* h = b.get_if<ir::Heading>()) m = std::min(m, h->level);
      });
      return m;
    }();
    if (level > 0 && toc_headings_ && !toc_name.empty()) {
      ir::Heading* first = topic.blocks.empty() ? nullptr : topic.blocks.front().get_if<ir::Heading>();
      const bool same = first && util::iequals(util::trim(ir::plain_text(first->content)), util::trim(toc_name));
      if (same) {
        shift_headings(topic.blocks, level - first->level);
        first->level = level;
      } else {
        if (topic_min <= 6) shift_headings(topic.blocks, level + 1 - topic_min);
        topic.blocks.insert(topic.blocks.begin(), ir::Heading{level, {ir::Text{toc_name}}});
      }
    } else if (topic_min > 6 && !util::trim(topic.metadata.title).empty()) {
      topic.blocks.insert(topic.blocks.begin(), ir::Heading{toc_headings_ ? std::max(level, 1) : 1, {ir::Text{std::string(util::trim(topic.metadata.title))}}});
    }
    if (topic.blocks.empty()) return {};
    if (topic.blocks.front().attr.id.empty()) topic.blocks.front().attr.id = anchor_.at(key);
    topic_anchor_[key] = topic.blocks.front().attr.id;

    // Notes: labels unique across topics.
    if (!topic.footnotes.empty()) {
      const std::string prefix = std::to_string(emitted_.size()) + "-";
      for_each_inline(topic.blocks, [&](ir::Inline& item) {
        if (auto* link = item.get_if<ir::Link>(); link && !link->note.empty()) {
          link->note = prefix + link->note;
          link->url = "#fn-" + link->note;
        }
      });
      for (ir::Footnote& note : topic.footnotes) note.label = prefix + note.label;
    }
    add_break();
    for (ir::Block& b : topic.blocks) doc_.blocks.push_back(std::move(b));
    for (ir::Footnote& n : topic.footnotes) doc_.footnotes.push_back(std::move(n));
    return {};
  }

  void resolve_links() {
    std::set<std::string> ids;
    for_each_block(doc_.blocks, [&](ir::Block& b) {
      if (!b.attr.id.empty()) ids.insert(b.attr.id);
    });
    for_each_inline(doc_.blocks, [&](ir::Inline& item) {
      auto* link = item.get_if<ir::Link>();
      if (!link || !link->url.starts_with(link_marker)) return;
      const std::string rest = link->url.substr(link_marker.size());
      const std::size_t hash = rest.find('#');
      const std::string key = rest.substr(0, hash);
      const std::string fragment = hash == std::string::npos ? std::string() : rest.substr(hash + 1);
      if (!fragment.empty() && ids.contains(fragment)) {
        link->url = "#" + fragment;
      } else if (const auto a = topic_anchor_.find(key); a != topic_anchor_.end()) {
        link->url = "#" + a->second;
      } else {
        link->url = topics_.contains(key) ? topics_.at(key) : key;
      }
    });
  }

  ReadContext& ctx_;
  its::Storage& storage_;
  ir::Document doc_;
  std::string contents_, index_, title_, compiler_;
  std::uint32_t lcid_ = 0;
  std::string charset_ = "windows-1252";
  bool breaks_ = false;
  bool toc_headings_ = true;
  bool from_index_ = false;
  std::map<std::string, std::string> topics_;        // lower-case path -> stored path
  std::map<std::string, std::string> anchor_;        // lower-case path -> planned anchor
  std::map<std::string, std::string> topic_anchor_;  // lower-case path -> anchor in the document
  std::set<std::string> emitted_;
  std::vector<std::string> pending_;
};

Result<AnyIR> read_chm(ReadContext& ctx) {
  CVT_TRY_ASSIGN(its::Storage storage, its::Storage::open_chm(ctx.input, ctx.limits));
  ChmBook book(ctx, storage);
  CVT_TRY_ASSIGN(ir::Document doc, book.read());
  return AnyIR(std::move(doc));
}

}  // namespace

void register_chm_reader(Registry& registry) {
  // The archive family describes the format (FormatInfo) and reads it as an
  // Archive of files; this is the Document reader on the same id.
  Codec codec;
  codec.format_id = "chm";
  codec.kind = Kind::Document;
  codec.read = read_chm;
  codec.read_cost = 20;
  codec.lossy = true;
  codec.read_options = {
      OptionSpec::boolean("chapter_breaks", "Page break between topics", false,
                          "Start every help topic on a new page in paginated output."),
      OptionSpec::boolean("toc_titles", "Headings from the table of contents", true,
                          "Give every topic the heading and level it has in the help file's table of contents."),
      options::strip_metadata(),
  };
  registry.add_codec(std::move(codec));
}

}  // namespace cvt::office
