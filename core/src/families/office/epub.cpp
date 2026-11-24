// Convertible core - office family: EPUB 2 and 3 e-books.
//
// Reader: container.xml -> OPF; chapters in spine order through the
// document family's XHTML parser with pictures resolved inside the book;
// links between chapters rewritten to in-document anchors; the table of
// contents (EPUB 3 nav or EPUB 2 NCX) gives chapter titles where a chapter
// has no heading; cover picture; Dublin Core metadata. Books protected by
// DRM (Adobe ADEPT, Apple FairPlay, Readium LCP, any encrypted content
// document) are refused with a clear message - nothing is ever decrypted.
// Font obfuscation (IDPF, Adobe) is not DRM and is ignored.
//
// Writer: EPUB 3 with an EPUB 2 NCX for old readers; one XHTML file per
// top-level section, pictures as files, nav document from the headings,
// deterministic identifier derived from the content, minimal metadata.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/document/document_api.hpp"
#include "families/office/epub.hpp"
#include "families/office/office.hpp"
#include "families/office/office_common.hpp"
#include "util/hash.hpp"
#include "util/hex.hpp"
#include "util/text.hpp"

namespace cvt::office {

namespace {

using util::XmlNode;

std::string dirname(std::string_view path) {
  const std::size_t slash = path.rfind('/');
  return slash == std::string_view::npos ? std::string() : std::string(path.substr(0, slash + 1));
}

std::string stem(std::string_view path) {
  const std::size_t slash = path.rfind('/');
  std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
  const std::size_t dot = name.rfind('.');
  return std::string(dot == std::string_view::npos ? name : name.substr(0, dot));
}

// Visit every inline of a document (blocks, lists, tables, notes).
void for_each_inline(ir::Blocks& blocks, const std::function<void(ir::Inline&)>& fn, int depth = 0);

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

void for_each_inline(ir::Blocks& blocks, const std::function<void(ir::Inline&)>& fn, int depth) {
  if (depth > ir::max_document_depth + 4) return;
  for (ir::Block& block : blocks) {
    std::visit(
        [&](auto& node) {
          using T = std::remove_cvref_t<decltype(node)>;
          if constexpr (std::same_as<T, ir::Paragraph> || std::same_as<T, ir::Heading>) {
            for_each_inline(node.content, fn, depth + 1);
          } else if constexpr (std::same_as<T, ir::List>) {
            for (ir::Blocks& item : node.items) for_each_inline(item, fn, depth + 1);
          } else if constexpr (std::same_as<T, ir::BlockQuote>) {
            for_each_inline(node.blocks, fn, depth + 1);
          } else if constexpr (std::same_as<T, ir::TableBlock>) {
            for (ir::TableRow& row : node.rows) {
              for (ir::TableCell& cell : row.cells) for_each_inline(cell.content, fn, depth + 1);
            }
            for_each_inline(node.caption, fn, depth + 1);
          }
        },
        block.node);
  }
}

// Visit every picture source of a document.
void for_each_image(ir::Blocks& blocks, const std::function<void(ir::ImageSource&, std::string_view)>& fn, int depth = 0) {
  if (depth > ir::max_document_depth + 4) return;
  for (ir::Block& block : blocks) {
    if (auto* image = block.get_if<ir::ImageBlock>()) fn(image->source, image->alt);
    if (auto* list = block.get_if<ir::List>()) {
      for (ir::Blocks& item : list->items) for_each_image(item, fn, depth + 1);
    }
    if (auto* quote = block.get_if<ir::BlockQuote>()) for_each_image(quote->blocks, fn, depth + 1);
  }
  for_each_inline(blocks, [&](ir::Inline& item) {
    if (auto* image = item.get_if<ir::InlineImage>()) fn(image->source, image->alt);
  });
}


// --------------------------------------------------------------------------- CSS emphasis

// Books written by layout programs express emphasis and headings only
// through class selectors. A tiny CSS reader recovers the basics.
struct CssLook {
  bool bold = false, italic = false, underline = false, strike = false, mono = false, sup = false, sub = false;
  double size_pt = 0.0;  // absolute size; 0 = unknown
  double size_em = 0.0;  // relative size; 0 = unknown
};

void parse_declarations(std::string_view body, CssLook& look) {
  for (std::string_view decl : util::split(body, ';', false)) {
    const std::size_t colon = decl.find(':');
    if (colon == std::string_view::npos) continue;
    const std::string key = util::to_lower(util::trim(decl.substr(0, colon)));
    const std::string value = util::to_lower(util::trim(decl.substr(colon + 1)));
    if (key == "font-weight") look.bold = value == "bold" || value == "bolder" || value == "600" || value == "700" || value == "800" || value == "900";
    if (key == "font-style") look.italic = value == "italic" || value == "oblique";
    if (key == "text-decoration" || key == "text-decoration-line") {
      if (value.find("underline") != std::string::npos) look.underline = true;
      if (value.find("line-through") != std::string::npos) look.strike = true;
    }
    if (key == "font-family") {
      look.mono = value.find("mono") != std::string::npos || value.find("courier") != std::string::npos ||
                  value.find("consolas") != std::string::npos;
    }
    if (key == "vertical-align") {
      if (value.starts_with("super")) look.sup = true;
      if (value.starts_with("sub")) look.sub = true;
    }
    if (key == "top") {
      if (value.starts_with("-")) look.sup = true;
      else if (!value.starts_with("0") && !value.empty()) look.sub = true;
    }
    if (key == "font-size") {
      const auto number = util::parse_double(std::string_view(value).substr(0, value.find_first_not_of("0123456789.")));
      if (number) {
        if (value.ends_with("pt")) look.size_pt = *number;
        else if (value.ends_with("px")) look.size_pt = *number * 0.75;
        else if (value.ends_with("em") || value.ends_with("rem")) look.size_em = *number;
        else if (value.ends_with("%")) look.size_em = *number / 100.0;
      }
    }
  }
  if (look.size_em > 0 && look.size_em < 0.95 && !look.sup && !look.sub) look.size_em = look.size_em;  // smaller text
}

std::map<std::string, CssLook> parse_class_rules(std::string_view css) {
  std::map<std::string, CssLook> out;
  std::size_t i = 0;
  int guard = 0;
  while (i < css.size() && ++guard < 20000) {
    const std::size_t open = css.find('{', i);
    if (open == std::string_view::npos) break;
    const std::size_t close = css.find('}', open);
    if (close == std::string_view::npos) break;
    std::string_view selectors = css.substr(i, open - i);
    const std::string_view body = css.substr(open + 1, close - open - 1);
    i = close + 1;
    if (const std::size_t comment = selectors.rfind("*/"); comment != std::string_view::npos) selectors.remove_prefix(comment + 2);
    for (std::string_view selector : util::split(selectors, ',', false)) {
      selector = util::trim(selector);
      // Only simple class selectors: ".c" or "p.c" / "span.c".
      const std::size_t dot = selector.find('.');
      if (dot == std::string_view::npos || selector.find_first_of(" >+~:[") != std::string_view::npos) continue;
      const std::string name(selector.substr(dot + 1));
      CssLook& look = out[name];
      parse_declarations(body, look);
    }
  }
  return out;
}

std::string apply_css_semantics(std::string_view xhtml, const std::map<std::string, CssLook>& rules, bool headings) {
  if (rules.empty()) return std::string(xhtml);
  auto parsed = parse_untrusted_xml(as_bytes(xhtml), true);
  if (!parsed) return std::string(xhtml);
  const auto look_of = [&](XmlNode n) {
    CssLook look;
    for (std::string_view cls : util::split(attr(n, "class"), ' ', false)) {
      const auto it = rules.find(std::string(cls));
      if (it == rules.end()) continue;
      const CssLook& l = it->second;
      look.bold |= l.bold;
      look.italic |= l.italic;
      look.underline |= l.underline;
      look.strike |= l.strike;
      look.mono |= l.mono;
      look.sup |= l.sup;
      look.sub |= l.sub;
      if (l.size_pt > 0) look.size_pt = l.size_pt;
      if (l.size_em > 0) look.size_em = l.size_em;
    }
    return look;
  };
  std::vector<XmlNode> spans;
  std::vector<XmlNode> paragraphs;
  for (XmlNode n : parsed->root().descendants()) {
    if (!n.is_element()) continue;
    if (n.local_name() == "span" && n.attribute("class")) spans.push_back(n);
    if ((n.local_name() == "p" || n.local_name() == "div") && n.attribute("class")) paragraphs.push_back(n);
  }
  // Paragraphs that are one large bold run become headings.
  if (headings) {
    for (XmlNode p : paragraphs) {
      XmlNode only;
      int elements = 0;
      bool text_outside = false;
      for (XmlNode c = p.first_child(); c; c = c.next_sibling()) {
        if (c.is_element()) {
          ++elements;
          only = c;
        } else if (c.is_text() && !util::trim(c.value()).empty()) {
          text_outside = true;
        }
      }
      CssLook look = look_of(p);
      if (elements == 1 && !text_outside && only.local_name() == "span") {
        const CssLook inner = look_of(only);
        look.bold |= inner.bold;
        if (inner.size_pt > 0) look.size_pt = inner.size_pt;
        if (inner.size_em > 0) look.size_em = inner.size_em;
      } else if (elements > 0 || text_outside) {
        if (elements != 0) continue;
      }
      double pt = look.size_pt > 0 ? look.size_pt : look.size_em * 12.0;
      if (!look.bold || pt < 13.0 || util::trim(p.text()).size() > 150) continue;
      const int level = pt >= 20.0 ? 1 : pt >= 15.5 ? 2 : 3;
      p.set_name("h" + std::to_string(level));
      if (only) only.remove_attribute("class");
    }
  }
  for (XmlNode span : spans) {
    const CssLook look = look_of(span);
    if (look.sup) span.set_name("sup");
    else if (look.sub) span.set_name("sub");
    else if (look.mono) span.set_name("code");
    else if (look.bold) span.set_name("b");
    else if (look.italic) span.set_name("i");
    else if (look.underline && span.parent().local_name() != "a") span.set_name("u");
    else if (look.strike) span.set_name("s");
    else continue;
    // Keep a second kind of emphasis as a nested element.
    if (look.bold && look.italic && span.local_name() == "b") {
      std::string text = span.text();
      if (XmlNode first = span.first_child(); first && first.is_text() && !first.next_sibling()) {
        span.clear_children();
        span.append_child("i").append_text(text);
      }
    }
  }
  util::XmlWriteOptions options;
  options.indent.clear();
  return util::xml_to_string(parsed->node(), options);
}

// =========================================================================== reader

bool font_obfuscation(std::string_view algorithm) {
  return algorithm == "http://www.idpf.org/2008/embedding" || algorithm == "http://ns.adobe.com/pdf/enc#RC";
}

class EpubReader {
 public:
  EpubReader(const ReadContext& ctx, ReadSettings settings) : ctx_(ctx), settings_(settings) {}
  Result<ir::Document> read(Package& package);

 private:
  Result<void> check_drm(Package& package);
  void read_toc(XmlNode nav_or_ncx, const std::string& base, bool ncx);

  const ReadContext& ctx_;
  ReadSettings settings_;
  std::map<std::string, std::string> toc_titles_;  // chapter path -> title
};

Result<void> EpubReader::check_drm(Package& package) {
  if (package.has("META-INF/rights.xml")) {
    return fail(Status::Decode, "this e-book is protected by DRM (Adobe ADEPT); it cannot be converted");
  }
  if (package.has("META-INF/sinf.xml")) {
    return fail(Status::Decode, "this e-book is protected by DRM (Apple FairPlay); it cannot be converted");
  }
  if (package.has("META-INF/license.lcpl")) {
    return fail(Status::Decode, "this e-book is protected by DRM (Readium LCP); it cannot be converted");
  }
  if (package.encrypted_entries()) {
    return fail(Status::Decode, "this e-book is encrypted; it cannot be converted");
  }
  if (auto enc = package.xml("META-INF/encryption.xml", false)) {
    for (XmlNode data : enc->root().children_local("EncryptedData")) {
      const std::string_view algorithm = attr(child(data, "EncryptionMethod"), "Algorithm");
      const std::string uri(attr(descendant(data, "CipherReference"), "URI"));
      if (font_obfuscation(algorithm)) continue;
      const std::string lower = util::to_lower(uri);
      const bool font = lower.ends_with(".otf") || lower.ends_with(".ttf") || lower.ends_with(".woff") ||
                        lower.ends_with(".woff2");
      if (!font || !algorithm.empty()) {
        return fail(Status::Decode, "this e-book is protected by DRM (encrypted content '{}'); it cannot be converted", uri);
      }
    }
  }
  return {};
}

void EpubReader::read_toc(XmlNode root, const std::string& base, bool ncx) {
  int guard = 0;
  for (XmlNode n : root.descendants()) {
    if (++guard > 100000) break;
    if (!n.is_element()) continue;
    std::string href;
    std::string label;
    if (ncx && n.local_name() == "navPoint") {
      label = std::string(util::trim(child(child(n, "navLabel"), "text").text()));
      href = std::string(attr(child(n, "content"), "src"));
    } else if (!ncx && n.local_name() == "a") {
      label = std::string(util::trim(n.text()));
      href = std::string(attr(n, "href"));
    } else {
      continue;
    }
    if (href.empty() || label.empty()) continue;
    if (href.find('#') != std::string::npos) continue;  // only whole chapters
    const std::string path = resolve_part(base, percent_decode(href));
    toc_titles_.emplace(path, label);
  }
}

Result<ir::Document> EpubReader::read(Package& package) {
  CVT_TRY(check_drm(package));
  std::string opf_path;
  if (auto container = package.xml("META-INF/container.xml", false)) {
    if (XmlNode rootfile = descendant(container->root(), "rootfile")) opf_path = std::string(attr(rootfile, "full-path"));
  }
  if (opf_path.empty()) {
    for (const std::string& name : package.names()) {
      if (util::iends_with(name, ".opf")) {
        opf_path = name;
        break;
      }
    }
  }
  if (opf_path.empty()) return fail(Status::Decode, "the e-book has no package document (OPF)");
  CVT_TRY_ASSIGN(util::XmlDocument opf, package.xml(opf_path, false));
  const XmlNode pkg = opf.root();
  const std::string base = dirname(opf_path);

  ir::Document doc;
  XmlNode metadata = child(pkg, "metadata");
  std::string cover_id;
  for (XmlNode m : metadata.elements()) {
    const std::string_view name = m.local_name();
    const std::string value(util::trim(m.text()));
    if (name == "title" && doc.metadata.title.empty()) doc.metadata.title = value;
    else if (name == "creator" && doc.metadata.author.empty()) doc.metadata.author = value;
    else if (name == "language" && doc.metadata.language.empty()) doc.metadata.language = value;
    else if (name == "date" && doc.metadata.date.empty()) doc.metadata.date = value;
    else if ((name == "publisher" || name == "description" || name == "subject" || name == "identifier" ||
              name == "rights") && !value.empty()) {
      doc.metadata.extra[std::string(name)] = value;
    } else if (name == "meta" && attr(m, "name") == "cover") {
      cover_id = std::string(attr(m, "content"));
    }
  }
  struct Item {
    std::string path;
    std::string media;
    std::string properties;
  };
  std::map<std::string, Item> manifest;
  std::string nav_path;
  std::string cover_path;
  for (XmlNode item : child(pkg, "manifest").children_local("item")) {
    Item it{resolve_part(base, percent_decode(attr(item, "href"))), std::string(attr(item, "media-type")),
            std::string(attr(item, "properties"))};
    const std::string id(attr(item, "id"));
    if (it.properties.find("nav") != std::string::npos) nav_path = it.path;
    if (it.properties.find("cover-image") != std::string::npos || id == cover_id) cover_path = it.path;
    manifest[id] = std::move(it);
  }
  CVT_TRY(ctx_.limits.check_archive_entries(manifest.size()));
  XmlNode spine = child(pkg, "spine");
  if (!nav_path.empty()) {
    if (auto nav = package.xml(nav_path, true)) read_toc(nav->root(), nav_path, false);
  } else if (auto toc = manifest.find(std::string(attr(spine, "toc"))); toc != manifest.end()) {
    if (auto ncx = package.xml(toc->second.path, false)) read_toc(ncx->root(), toc->second.path, true);
  }

  std::vector<std::string> chapters;
  for (XmlNode ref : spine.children_local("itemref")) {
    const auto it = manifest.find(std::string(attr(ref, "idref")));
    if (it == manifest.end()) continue;
    const std::string& media = it->second.media;
    if (media.find("html") == std::string::npos && media.find("xml") == std::string::npos) continue;
    chapters.push_back(it->second.path);
  }
  if (chapters.empty()) return fail(Status::Decode, "the e-book has no readable chapters");

  // Anchors for links between chapters.
  std::map<std::string, std::string> chapter_anchor;
  std::set<std::string> used;
  for (const std::string& path : chapters) chapter_anchor[path] = document::unique_slug("chapter-" + stem(path), used);

  const bool page_breaks = ctx_.options.get_bool("chapter_breaks", true);
  std::map<std::string, std::map<std::string, CssLook>> css_cache;
  bool cover_seen = false;
  for (std::size_t i = 0; i < chapters.size(); ++i) {
    CVT_TRY(ctx_.checkpoint(static_cast<double>(i) / static_cast<double>(chapters.size()), "Reading chapters"));
    const std::string& path = chapters[i];
    auto bytes = package.try_read(path);
    if (!bytes) {
      ctx_.warn("A chapter of the e-book is missing: " + path);
      continue;
    }
    document::HtmlReadOptions options;
    options.xhtml = true;
    options.read_metadata = false;
    std::set<std::string>* seen_images = nullptr;
    (void)seen_images;
    options.resolve = [&package, path, &cover_path, &cover_seen](std::string_view url) -> std::optional<ir::EncodedImage> {
      if (url.find("://") != std::string_view::npos || url.starts_with("data:")) return std::nullopt;
      const std::string target = resolve_part(path, percent_decode(url));
      if (target == cover_path) cover_seen = true;
      auto data = package.try_read(target);
      if (!data) return std::nullopt;
      ir::EncodedImage image;
      image.bytes = std::move(*data);
      image.mime = document::sniff_image_mime(image.bytes);
      if (image.mime.empty()) image.mime = mime_for_extension(target);
      return image;
    };
    std::vector<std::string> warnings;
    // Class-based emphasis from the book's style sheets.
    std::map<std::string, CssLook> rules;
    {
      const std::string_view text = as_chars(*bytes);
      std::size_t at = 0;
      int links = 0;
      while ((at = text.find("<link", at)) != std::string_view::npos && ++links < 16) {
        const std::size_t end = text.find('>', at);
        if (end == std::string_view::npos) break;
        const std::string_view tag = text.substr(at, end - at);
        at = end;
        if (tag.find("stylesheet") == std::string_view::npos) continue;
        const std::size_t href = tag.find("href=");
        if (href == std::string_view::npos || href + 6 >= tag.size()) continue;
        const char quote = tag[href + 5];
        const std::size_t close = tag.find(quote, href + 6);
        if (close == std::string_view::npos) continue;
        const std::string css_path = resolve_part(path, percent_decode(tag.substr(href + 6, close - href - 6)));
        if (!css_cache.contains(css_path)) {
          auto css = package.try_read(css_path);
          css_cache[css_path] = css ? parse_class_rules(as_chars(*css)) : std::map<std::string, CssLook>{};
        }
        for (const auto& [k, v] : css_cache[css_path]) rules[k] = v;
      }
      std::size_t style_at = 0;
      while ((style_at = text.find("<style", style_at)) != std::string_view::npos) {
        const std::size_t open = text.find('>', style_at);
        const std::size_t close = text.find("</style", style_at);
        if (open == std::string_view::npos || close == std::string_view::npos || close < open) break;
        for (const auto& [k, v] : parse_class_rules(text.substr(open + 1, close - open - 1))) rules[k] = v;
        style_at = close;
      }
    }
    const std::string styled = apply_css_semantics(as_chars(*bytes), rules, !settings_.strict_headings);
    CVT_TRY_ASSIGN(ir::Document chapter, document::parse_html(as_bytes(styled), options, &warnings));
    // Links: other chapter files become in-document anchors.
    for_each_inline(chapter.blocks, [&](ir::Inline& item) {
      auto* link = item.get_if<ir::Link>();
      if (!link || !link->note.empty() || link->url.empty() || link->url.find("://") != std::string::npos ||
          link->url.starts_with("mailto:") || link->url.starts_with('#')) {
        return;
      }
      const std::size_t hash = link->url.find('#');
      const std::string file = resolve_part(path, percent_decode(link->url.substr(0, hash)));
      if (hash != std::string::npos) {
        link->url = link->url.substr(hash);
      } else if (auto a = chapter_anchor.find(file); a != chapter_anchor.end()) {
        link->url = "#" + a->second;
      }
    });
    if (chapter.blocks.empty()) continue;
    // Chapter anchor on the first block; a title from the table of contents when the chapter has no heading.
    const bool has_heading = std::ranges::any_of(chapter.blocks, [](const ir::Block& b) { return b.is<ir::Heading>(); });
    if (!has_heading) {
      if (auto t = toc_titles_.find(path); t != toc_titles_.end() && settings_.strict_headings == false &&
                                            ctx_.options.get_bool("toc_titles", true)) {
        chapter.blocks.insert(chapter.blocks.begin(), ir::Heading{1, {ir::Text{t->second}}});
      }
    }
    if (chapter.blocks.front().attr.id.empty()) chapter.blocks.front().attr.id = chapter_anchor[path];
    // Footnotes: relabel to keep labels unique across chapters.
    const std::string prefix = chapters.size() > 1 ? std::to_string(i + 1) + "-" : std::string();
    if (!prefix.empty() && !chapter.footnotes.empty()) {
      for_each_inline(chapter.blocks, [&](ir::Inline& item) {
        if (auto* link = item.get_if<ir::Link>(); link && !link->note.empty()) {
          link->note = prefix + link->note;
          link->url = "#fn-" + link->note;
        }
      });
      for (ir::Footnote& note : chapter.footnotes) note.label = prefix + note.label;
    }
    if (!doc.blocks.empty() && page_breaks) doc.blocks.push_back(ir::PageBreak{});
    for (ir::Block& b : chapter.blocks) doc.blocks.push_back(std::move(b));
    for (ir::Footnote& n : chapter.footnotes) doc.footnotes.push_back(std::move(n));
  }
  if (!cover_seen && !cover_path.empty() && ctx_.options.get_bool("include_cover", true)) {
    if (auto data = package.try_read(cover_path)) {
      ir::EncodedImage image;
      image.bytes = std::move(*data);
      image.mime = document::sniff_image_mime(image.bytes);
      ir::Blocks cover;
      cover.push_back(ir::ImageBlock{std::move(image), "Cover", {}});
      if (page_breaks) cover.push_back(ir::PageBreak{});
      doc.blocks.insert(doc.blocks.begin(), cover.begin(), cover.end());
    }
  }
  if (!doc.footnotes.empty()) document::number_footnotes(doc);
  finish_metadata(doc, settings_);
  return doc;
}

Result<AnyIR> read_epub(ReadContext& ctx) {
  CVT_TRY_ASSIGN(Package package, Package::open(ctx.input, ctx.limits));
  EpubReader reader(ctx, read_settings(ctx.options));
  CVT_TRY_ASSIGN(ir::Document doc, reader.read(package));
  return AnyIR(std::move(doc));
}

// =========================================================================== writer

std::string uuid_from(ByteView data) {
  const std::string hex = util::sha1_hex(data);
  const int variant = util::hex_digit_value(hex[16]) & 3;
  return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-5" + hex.substr(13, 3) + "-" +
         std::string(1, "89ab"[variant]) + hex.substr(17, 3) + "-" + hex.substr(20, 12);
}

struct Chapter {
  ir::Document doc;
  std::string file;
  std::string title;
};

class EpubWriter {
 public:
  EpubWriter(ir::Document doc, WriteContext& ctx) : doc_(std::move(doc)), ctx_(ctx) { policy_ = image_policy(ctx.options); }
  Result<Bytes> write();

 private:
  void split();
  void externalise_images();
  std::string nav_xhtml(const std::vector<document::OutlineEntry>& outline);
  std::string ncx(const std::vector<document::OutlineEntry>& outline, const std::string& uid);

  ir::Document doc_;
  WriteContext& ctx_;
  ImagePolicy policy_;
  MediaStore media_;
  std::vector<Chapter> chapters_;
  std::map<std::string, std::string> id_file_;  // anchor -> chapter file
  std::string cover_name_;
};

void EpubWriter::externalise_images() {
  int dropped = 0;
  for (Chapter& chapter : chapters_) {
    for_each_image(chapter.doc.blocks, [&](ir::ImageSource& source, std::string_view) {
      auto prepared = prepare_image(source, policy_, ctx_);
      if (prepared && !is_web_safe(prepared->extension) && prepared->extension != "svg" && prepared->extension != "webp") {
        prepared = convert_to_png(*prepared, ctx_);
      }
      ir::EncodedImage ref;
      if (!prepared) {
        if (policy_.mode == ImageMode::Drop) ++dropped;
        if (const auto* e = std::get_if<ir::EncodedImage>(&source); e && !e->url.empty()) ref.url = e->url;
        source = std::move(ref);
        return;
      }
      const std::size_t index = media_.add(std::move(*prepared));
      ref.url = "images/" + media_.items()[index].name;
      source = std::move(ref);
    });
  }
  if (dropped > 0) ctx_.warn(std::to_string(dropped) + " picture(s) were left out as requested.");
  // Pictures that are not inside the book (missing files, web addresses)
  // become their alt text, linked to the address when there is one.
  const auto fallback = [](const ir::ImageSource& source, const std::string& alt) -> ir::Inlines {
    const auto* e = std::get_if<ir::EncodedImage>(&source);
    const std::string url = e ? e->url : std::string();
    const bool remote = url.starts_with("http://") || url.starts_with("https://");
    const std::string text = alt.empty() ? (remote ? url : std::string()) : alt;
    if (text.empty()) return {};
    if (remote) return {ir::Link{url, {}, {ir::Text{text}}}};
    return {ir::Text{text}};
  };
  const auto inside = [](const ir::ImageSource& source) {
    const auto* e = std::get_if<ir::EncodedImage>(&source);
    return e && e->url.starts_with("images/");
  };
  for (Chapter& chapter : chapters_) {
    std::function<void(ir::Blocks&, int)> fix = [&](ir::Blocks& blocks, int depth) {
      if (depth > ir::max_document_depth) return;
      for (ir::Block& b : blocks) {
        if (auto* image = b.get_if<ir::ImageBlock>(); image && !inside(image->source)) {
          b = ir::Block(ir::Paragraph{fallback(image->source, image->alt)});
        } else if (auto* list = b.get_if<ir::List>()) {
          for (ir::Blocks& item : list->items) fix(item, depth + 1);
        } else if (auto* quote = b.get_if<ir::BlockQuote>()) {
          fix(quote->blocks, depth + 1);
        }
      }
    };
    fix(chapter.doc.blocks, 0);
    std::function<void(ir::Inlines&, int)> fix_inlines = [&](ir::Inlines& inlines, int depth) {
      if (depth > ir::max_document_depth) return;
      ir::Inlines out;
      for (ir::Inline& item : inlines) {
        if (auto* image = item.get_if<ir::InlineImage>(); image && !inside(image->source)) {
          for (ir::Inline& x : fallback(image->source, image->alt)) out.push_back(std::move(x));
          continue;
        }
        std::visit(
            [&](auto& node) {
              if constexpr (requires { node.children; }) fix_inlines(node.children, depth + 1);
            },
            item.node);
        out.push_back(std::move(item));
      }
      inlines = std::move(out);
    };
    std::function<void(ir::Blocks&, int)> walk = [&](ir::Blocks& blocks, int depth) {
      if (depth > ir::max_document_depth) return;
      for (ir::Block& b : blocks) {
        if (auto* para = b.get_if<ir::Paragraph>()) fix_inlines(para->content, 0);
        if (auto* heading = b.get_if<ir::Heading>()) fix_inlines(heading->content, 0);
        if (auto* list = b.get_if<ir::List>()) {
          for (ir::Blocks& item : list->items) walk(item, depth + 1);
        }
        if (auto* quote = b.get_if<ir::BlockQuote>()) walk(quote->blocks, depth + 1);
        if (auto* table = b.get_if<ir::TableBlock>()) {
          for (ir::TableRow& row : table->rows) {
            for (ir::TableCell& cell : row.cells) fix_inlines(cell.content, 0);
          }
        }
      }
    };
    walk(chapter.doc.blocks, 0);
    for (ir::Footnote& note : chapter.doc.footnotes) walk(note.blocks, 0);
  }
}

void EpubWriter::split() {
  document::assign_heading_ids(doc_);
  int counts[7] = {0, 0, 0, 0, 0, 0, 0};
  for (const ir::Block& b : doc_.blocks) {
    if (const auto* h = b.get_if<ir::Heading>()) ++counts[std::clamp(h->level, 1, 6)];
  }
  int split_level = 0;
  for (int level = 1; level <= 6; ++level) {
    if (counts[level] > 0) {
      split_level = level;
      if (counts[level] > 1) break;
    }
  }
  Chapter current;
  const auto flush = [&] {
    if (current.doc.blocks.empty()) return;
    const std::size_t n = chapters_.size() + 1;
    std::string number = std::to_string(n);
    while (number.size() < 3) number.insert(number.begin(), '0');
    current.file = "chapter-" + number + ".xhtml";
    current.doc.metadata.language = doc_.metadata.language;
    if (current.title.empty()) current.title = doc_.metadata.title.empty() ? "Chapter " + std::to_string(n) : doc_.metadata.title;
    chapters_.push_back(std::move(current));
    current = Chapter{};
  };
  for (ir::Block& b : doc_.blocks) {
    if (const auto* h = b.get_if<ir::Heading>(); h && split_level > 0 && h->level <= split_level) {
      flush();
      current.title = std::string(util::trim(ir::plain_text(h->content)));
    }
    if (b.is<ir::PageBreak>()) continue;
    current.doc.blocks.push_back(std::move(b));
  }
  flush();
  if (chapters_.empty()) {
    Chapter only;
    only.file = "chapter-001.xhtml";
    only.title = doc_.metadata.title.empty() ? "Untitled" : doc_.metadata.title;
    only.doc.blocks.push_back(ir::Paragraph{});
    chapters_.push_back(std::move(only));
  }
  // Record where every anchor lives, then distribute footnotes and fix links.
  for (Chapter& chapter : chapters_) {
    std::function<void(const ir::Blocks&, int)> collect = [&](const ir::Blocks& blocks, int depth) {
      if (depth > ir::max_document_depth) return;
      for (const ir::Block& b : blocks) {
        if (!b.attr.id.empty()) id_file_.emplace(b.attr.id, chapter.file);
        if (const auto* q = b.get_if<ir::BlockQuote>()) collect(q->blocks, depth + 1);
        if (const auto* l = b.get_if<ir::List>()) {
          for (const ir::Blocks& item : l->items) collect(item, depth + 1);
        }
      }
    };
    collect(chapter.doc.blocks, 0);
  }
  for (Chapter& chapter : chapters_) {
    std::set<std::string> notes;
    for_each_inline(chapter.doc.blocks, [&](ir::Inline& item) {
      auto* link = item.get_if<ir::Link>();
      if (!link) return;
      if (!link->note.empty()) {
        notes.insert(link->note);
        return;
      }
      if (link->url.starts_with('#')) {
        const auto it = id_file_.find(link->url.substr(1));
        if (it != id_file_.end() && it->second != chapter.file) link->url = it->second + link->url;
      }
    });
    for (const ir::Footnote& note : doc_.footnotes) {
      if (notes.contains(note.label)) chapter.doc.footnotes.push_back(note);
    }
    chapter.doc.metadata.title = chapter.title;
  }
}

void nav_list(std::string& out, const std::vector<document::OutlineEntry>& outline, std::size_t& i, int level,
              const std::map<std::string, std::string>& id_file, int max_level) {
  out += "<ol>";
  while (i < outline.size()) {
    const document::OutlineEntry& e = outline[i];
    if (e.level < level) break;
    if (e.level > max_level) {
      ++i;
      continue;
    }
    if (e.level > level) {
      // Deeper entry without a parent at this level: nest it in a list item of its own.
      out += "<li><span>&#160;</span>";
      nav_list(out, outline, i, e.level, id_file, max_level);
      out += "</li>";
      continue;
    }
    const auto file = id_file.find(e.id);
    const std::string href = (file == id_file.end() ? std::string("chapter-001.xhtml") : file->second) + "#" + e.id;
    out += "<li><a href=\"" + esc_attr(href) + "\">" + esc(e.text.empty() ? std::string("Section") : e.text) + "</a>";
    ++i;
    if (i < outline.size() && outline[i].level > level && outline[i].level <= max_level) {
      nav_list(out, outline, i, level + 1, id_file, max_level);
    }
    out += "</li>";
  }
  out += "</ol>";
}

std::string EpubWriter::nav_xhtml(const std::vector<document::OutlineEntry>& outline) {
  std::string out =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE html>\n<html xmlns=\"http://www.w3.org/1999/xhtml\" "
      "xmlns:epub=\"http://www.idpf.org/2007/ops\"";
  if (!doc_.metadata.language.empty()) {
    out += " lang=\"" + esc_attr(doc_.metadata.language) + "\" xml:lang=\"" + esc_attr(doc_.metadata.language) + "\"";
  }
  out += ">\n<head>\n<meta charset=\"utf-8\" />\n<title>Contents</title>\n</head>\n<body>\n<nav epub:type=\"toc\" id=\"toc\">\n<h1>Contents</h1>\n";
  if (outline.empty()) {
    out += "<ol>";
    for (const Chapter& c : chapters_) out += "<li><a href=\"" + c.file + "\">" + esc(c.title) + "</a></li>";
    out += "</ol>";
  } else {
    std::size_t i = 0;
    int min_level = 6;
    for (const auto& e : outline) min_level = std::min(min_level, e.level);
    nav_list(out, outline, i, min_level, id_file_, min_level + 2);
  }
  out += "\n</nav>\n</body>\n</html>\n";
  return out;
}

std::string EpubWriter::ncx(const std::vector<document::OutlineEntry>& outline, const std::string& uid) {
  std::string points;
  int order = 0;
  if (outline.empty()) {
    for (const Chapter& c : chapters_) {
      ++order;
      points += "<navPoint id=\"np" + std::to_string(order) + "\" playOrder=\"" + std::to_string(order) +
                "\"><navLabel><text>" + esc(c.title) + "</text></navLabel><content src=\"" + c.file + "\"/></navPoint>";
    }
  } else {
    int min_level = 6;
    for (const auto& e : outline) min_level = std::min(min_level, e.level);
    for (const auto& e : outline) {
      if (e.level != min_level) continue;
      ++order;
      const auto file = id_file_.find(e.id);
      const std::string href = (file == id_file_.end() ? std::string("chapter-001.xhtml") : file->second) + "#" + e.id;
      points += "<navPoint id=\"np" + std::to_string(order) + "\" playOrder=\"" + std::to_string(order) +
                "\"><navLabel><text>" + esc(e.text.empty() ? std::string("Section") : e.text) +
                "</text></navLabel><content src=\"" + esc_attr(href) + "\"/></navPoint>";
    }
  }
  return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\">"
         "<head><meta name=\"dtb:uid\" content=\"" + esc_attr(uid) + "\"/><meta name=\"dtb:depth\" content=\"1\"/>"
         "<meta name=\"dtb:totalPageCount\" content=\"0\"/><meta name=\"dtb:maxPageNumber\" content=\"0\"/></head>"
         "<docTitle><text>" + esc(doc_.metadata.title.empty() ? std::string("Untitled") : doc_.metadata.title) +
         "</text></docTitle><navMap>" + points + "</navMap></ncx>\n";
}

constexpr std::string_view epub_css =
    "body{font-family:serif;line-height:1.4;margin:0 4%}h1,h2,h3,h4,h5,h6{font-family:sans-serif;line-height:1.2}"
    "pre,code{font-family:monospace}pre{white-space:pre-wrap;background:#f3f3f3;padding:.5em}"
    "blockquote{margin:1em 2em;font-style:italic}table{border-collapse:collapse}td,th{border:1px solid #888;padding:.2em .4em}"
    "img{max-width:100%;height:auto}.footnotes{font-size:.9em}\n";

Result<Bytes> EpubWriter::write() {
  std::string language = ctx_.options.get_string("language", "");
  if (!language.empty()) doc_.metadata.language = language;
  if (doc_.metadata.language.empty()) doc_.metadata.language = "en";
  const bool strip = options::resolved_strip_metadata(ctx_.options);
  if (strip) document::strip_metadata(doc_);
  if (doc_.metadata.title.empty()) {
    for (const ir::Block& b : doc_.blocks) {
      if (const auto* h = b.get_if<ir::Heading>()) {
        doc_.metadata.title = std::string(util::trim(ir::plain_text(h->content)));
        break;
      }
    }
  }
  if (doc_.metadata.title.empty()) doc_.metadata.title = "Untitled";
  const std::vector<document::OutlineEntry> outline = [&] {
    document::assign_heading_ids(doc_);
    return document::heading_outline(doc_);
  }();
  split();
  // A cover: the first picture when the book starts with one.
  const bool want_cover = ctx_.options.get_bool("cover", true);
  externalise_images();
  if (want_cover && !chapters_.empty() && !chapters_.front().doc.blocks.empty()) {
    if (const auto* image = chapters_.front().doc.blocks.front().get_if<ir::ImageBlock>()) {
      if (const auto* e = std::get_if<ir::EncodedImage>(&image->source); e && e->url.starts_with("images/")) {
        cover_name_ = e->url.substr(7);
      }
    }
  }

  // Chapters as XHTML.
  std::vector<std::string> xhtml;
  std::string fingerprint;
  for (std::size_t i = 0; i < chapters_.size(); ++i) {
    CVT_TRY(ctx_.checkpoint(static_cast<double>(i) / static_cast<double>(chapters_.size()), "Writing chapters"));
    document::HtmlWriteOptions options;
    options.standalone = true;
    options.xhtml = true;
    options.stylesheet = false;
    options.metadata = false;
    options.images = document::ImageHandling::Link;
    std::string page = document::write_html(chapters_[i].doc, options);
    const std::string head_end = "</head>";
    if (const std::size_t at = page.find(head_end); at != std::string::npos) {
      page.insert(at, "<link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\" />\n");
    }
    fingerprint += page;
    xhtml.push_back(std::move(page));
  }
  for (const auto& item : media_.items()) fingerprint += util::sha1_hex(item.image.bytes);
  const std::string uid = "urn:uuid:" + uuid_from(as_bytes(fingerprint));

  std::string date = "2000-01-01T00:00:00Z";
  std::string opf =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" "
      "unique-identifier=\"uid\" xml:lang=\"" + esc_attr(doc_.metadata.language) +
      "\">\n<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n<dc:identifier id=\"uid\">" + uid +
      "</dc:identifier>\n<dc:title>" + esc(doc_.metadata.title) + "</dc:title>\n<dc:language>" +
      esc(doc_.metadata.language) + "</dc:language>\n";
  if (!strip && !doc_.metadata.author.empty()) opf += "<dc:creator>" + esc(doc_.metadata.author) + "</dc:creator>\n";
  if (!strip && !doc_.metadata.date.empty()) opf += "<dc:date>" + esc(doc_.metadata.date) + "</dc:date>\n";
  opf += "<meta property=\"dcterms:modified\">" + date + "</meta>\n";
  if (!cover_name_.empty()) opf += "<meta name=\"cover\" content=\"cover-image\"/>\n";
  opf += "</metadata>\n<manifest>\n<item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>\n"
         "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n"
         "<item id=\"css\" href=\"style.css\" media-type=\"text/css\"/>\n";
  for (std::size_t i = 0; i < chapters_.size(); ++i) {
    opf += "<item id=\"c" + std::to_string(i + 1) + "\" href=\"" + chapters_[i].file +
           "\" media-type=\"application/xhtml+xml\"";
    if (xhtml[i].find("<svg") != std::string::npos) opf += " properties=\"svg\"";
    opf += "/>\n";
  }
  for (std::size_t i = 0; i < media_.items().size(); ++i) {
    const auto& item = media_.items()[i];
    const bool cover = item.name == cover_name_;
    opf += "<item id=\"" + std::string(cover ? "cover-image" : "img" + std::to_string(i + 1)) + "\" href=\"images/" +
           item.name + "\" media-type=\"" + item.image.mime + "\"" + (cover ? " properties=\"cover-image\"" : "") + "/>\n";
  }
  opf += "</manifest>\n<spine toc=\"ncx\">\n";
  for (std::size_t i = 0; i < chapters_.size(); ++i) opf += "<itemref idref=\"c" + std::to_string(i + 1) + "\"/>\n";
  opf += "</spine>\n</package>\n";

  const std::string container =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<container version=\"1.0\" "
      "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles><rootfile full-path=\"EPUB/content.opf\" "
      "media-type=\"application/oebps-package+xml\"/></rootfiles></container>\n";

  int level = static_cast<int>(ctx_.options.get_int("compression_level", 9, 0, 9));
  if (!ctx_.options.was_set("compression_level")) level = 9;
  util::ZipEntryOptions stored;
  stored.level = 0;
  util::ZipEntryOptions entry;
  entry.level = level;
  util::ZipWriter zip;
  CVT_TRY(zip.add("mimetype", "application/epub+zip", stored));
  CVT_TRY(zip.add("META-INF/container.xml", container, entry));
  CVT_TRY(zip.add("EPUB/content.opf", opf, entry));
  CVT_TRY(zip.add("EPUB/nav.xhtml", nav_xhtml(outline), entry));
  CVT_TRY(zip.add("EPUB/toc.ncx", ncx(outline, uid), entry));
  CVT_TRY(zip.add("EPUB/style.css", epub_css, entry));
  for (std::size_t i = 0; i < chapters_.size(); ++i) CVT_TRY(zip.add("EPUB/" + chapters_[i].file, xhtml[i], entry));
  for (const auto& item : media_.items()) CVT_TRY(zip.add("EPUB/images/" + item.name, item.image.bytes, stored));
  return zip.finish();
}

Result<Bytes> write_epub(const AnyIR& value, WriteContext& ctx) {
  EpubWriter writer(value.get<ir::Document>(), ctx);
  return writer.write();
}

}  // namespace

ReadFn epub_reader() { return read_epub; }

Result<Bytes> write_epub_document(const ir::Document& doc, WriteContext& ctx) {
  EpubWriter writer(doc, ctx);
  return writer.write();
}

std::vector<OptionSpec> epub_read_options() {
  std::vector<OptionSpec> out = {
      OptionSpec::boolean("chapter_breaks", "Page break between chapters", true,
                          "Start every chapter on a new page in paginated output."),
      OptionSpec::boolean("include_cover", "Include the cover", true, "Put the cover picture at the start."),
      OptionSpec::boolean("toc_titles", "Titles from the table of contents", true,
                          "Give chapters without a heading the title listed in the table of contents.")
          .advanced(),
      options::strip_metadata(),
  };
  return out;
}

void register_epub(Registry& registry) {
  FormatInfo info;
  info.id = "epub";
  info.name = "EPUB e-book";
  info.description = "Reflowable e-book (EPUB 2 and 3).";
  info.category = Category::Ebook;
  info.extensions = {"epub"};
  info.mime = "application/epub+zip";
  info.popularity = 80;
  info.signatures = {Signature::at(30, "'mimetypeapplication/epub+zip'")};
  info.sniff = [](ByteView data, std::string_view) {
    if (leading_mimetype(data) == "application/epub+zip") return 96;
    return zip_mentions(data, "META-INF/container.xml") && zip_mentions(data, ".opf") ? 80 : 0;
  };
  registry.add_format(std::move(info));

  Codec codec;
  codec.format_id = "epub";
  codec.kind = Kind::Document;
  codec.read = read_epub;
  codec.write = write_epub;
  codec.read_options = epub_read_options();
  std::vector<OptionSpec> write_options = {
      OptionSpec::boolean("cover", "Cover picture", true, "Use a picture at the very start of the book as its cover."),
      OptionSpec::text("language", "Language", "", "Language of the book as a BCP 47 tag (empty: from the source).")
          .advanced(),
  };
  for (OptionSpec& spec : image_write_options()) write_options.push_back(std::move(spec));
  write_options.push_back(options::compression_level());
  write_options.push_back(options::strip_metadata());
  codec.write_options = std::move(write_options);
  registry.add_codec(std::move(codec));
}

}  // namespace cvt::office
