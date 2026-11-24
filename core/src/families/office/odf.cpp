// Convertible core - office family: OpenDocument text and presentations.
//
// Reader: ODT/OTT (package), FODT (flat XML), ODP/OTP and FODP. Paragraph
// and text styles (automatic and common, with parent chains) give headings,
// code, quotes and character formatting; list styles give bullets versus
// numbering per level; tables with spanned and covered cells; frames with
// pictures (package members or embedded binary data) and text boxes; notes;
// bookmarks; links. Presentations become one section per slide: the title
// frame as a heading, outline/text frames as lists and paragraphs, tables,
// pictures, optional speaker notes, page breaks between slides.
//
// Writers: ODT, FODT (flat, pictures embedded as base64) and ODP.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <map>
#include <utility>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/document/document_api.hpp"
#include "families/office/office.hpp"
#include "families/office/office_common.hpp"
#include "families/office/slides.hpp"
#include "util/base64.hpp"
#include "util/text.hpp"

namespace cvt::office {

namespace {

using util::XmlNode;

constexpr int max_nesting = 24;

// =========================================================================== reader

struct OdfStyle {
  std::string name;
  std::string display;
  std::string parent;
  std::string family;
  std::string list_style;
  int outline = 0;
  std::optional<bool> bold, italic, underline, strike, superscript, subscript, mono;
};

struct ListLevelKind {
  bool ordered = false;
  ir::ListNumbering numbering = ir::ListNumbering::Decimal;
  int start = 1;
};

class OdfReader {
 public:
  OdfReader(const ReadContext& ctx, ReadSettings settings) : ctx_(ctx), settings_(settings) {}
  Result<ir::Document> read_package(Package& package);
  Result<ir::Document> read_flat(const util::XmlDocument& xml);

 private:
  void load_styles(XmlNode container, bool automatic);
  void load_meta(XmlNode meta);
  Result<ir::Document> read_body(XmlNode body);
  void blocks(XmlNode node, FlowBuilder& flow, int depth, int list_level, const std::string& list_style,
              const std::string& list_id);
  void paragraph(XmlNode p, FlowBuilder& flow, int depth, bool heading_element);
  void list(XmlNode list, FlowBuilder& flow, int depth, int level, std::string style, const std::string& list_id);
  ir::Block table(XmlNode table, int depth);
  void spans(XmlNode node, InlineBuilder& out, RunFormat format, int depth, ir::Blocks& extra);
  void frame(XmlNode frame, InlineBuilder& out, const RunFormat& format, int depth, ir::Blocks& extra);
  std::optional<ir::EncodedImage> image(XmlNode image);
  RunFormat apply_style(std::string_view name, RunFormat format) const;
  const OdfStyle* style(std::string_view name) const;
  int outline_of(std::string_view name) const;
  std::string display_name(std::string_view name) const;
  std::string list_style_of(std::string_view paragraph_style) const;
  ListLevelKind list_kind(const std::string& style, int level) const;
  ir::Blocks slide_frames(XmlNode page, int depth);

  const ReadContext& ctx_;
  ReadSettings settings_;
  Package* package_ = nullptr;
  std::map<std::string, OdfStyle> styles_;       // common + automatic (automatic wins)
  std::map<std::string, std::map<int, ListLevelKind>> list_styles_;
  std::vector<ir::Footnote> footnotes_;
  ir::Document doc_;
  std::string bookmark_;
  int list_counter_ = 0;
};

const OdfStyle* OdfReader::style(std::string_view name) const {
  const auto it = styles_.find(std::string(name));
  return it == styles_.end() ? nullptr : &it->second;
}

int OdfReader::outline_of(std::string_view name) const {
  for (int guard = 0; guard < 16 && !name.empty(); ++guard) {
    const OdfStyle* s = style(name);
    if (!s) return heading_level_from_style_name(name);
    if (s->outline > 0) return s->outline;
    const int by_name = heading_level_from_style_name(s->display.empty() ? s->name : s->display);
    if (by_name > 0) return by_name;
    name = s->parent;
  }
  return 0;
}

std::string OdfReader::display_name(std::string_view name) const {
  std::string out;
  for (int guard = 0; guard < 16 && !name.empty(); ++guard) {
    const OdfStyle* s = style(name);
    const std::string d = s ? (s->display.empty() ? s->name : s->display) : std::string(name);
    out += util::to_lower(util::replace_all(d, "_20_", " ")) + "|";
    if (!s) break;
    name = s->parent;
  }
  return out;
}

std::string OdfReader::list_style_of(std::string_view name) const {
  for (int guard = 0; guard < 16 && !name.empty(); ++guard) {
    const OdfStyle* s = style(name);
    if (!s) return {};
    if (!s->list_style.empty()) return s->list_style;
    name = s->parent;
  }
  return {};
}

RunFormat OdfReader::apply_style(std::string_view name, RunFormat format) const {
  std::vector<const OdfStyle*> chain;
  for (int guard = 0; guard < 16 && !name.empty(); ++guard) {
    const OdfStyle* s = style(name);
    if (!s) break;
    chain.push_back(s);
    name = s->parent;
  }
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    const OdfStyle& s = **it;
    if (s.bold) format.bold = *s.bold;
    if (s.italic) format.italic = *s.italic;
    if (s.underline) format.underline = *s.underline;
    if (s.strike) format.strike = *s.strike;
    if (s.superscript) format.superscript = *s.superscript;
    if (s.subscript) format.subscript = *s.subscript;
    if (s.mono) format.code = *s.mono;
  }
  return format;
}

bool mono_name(std::string_view font) {
  const std::string f = util::to_lower(font);
  return f.find("courier") != std::string::npos || f.find("mono") != std::string::npos ||
         f.find("consolas") != std::string::npos || f.find("menlo") != std::string::npos ||
         f.find("monaco") != std::string::npos || f.find("source code") != std::string::npos;
}

void OdfReader::load_styles(XmlNode container, bool automatic) {
  for (XmlNode s : container.elements()) {
    const std::string_view kind = s.local_name();
    if (kind == "style" || kind == "default-style") {
      OdfStyle style;
      style.name = std::string(attr(s, "name"));
      style.display = std::string(attr(s, "display-name"));
      style.parent = std::string(attr(s, "parent-style-name"));
      style.family = std::string(attr(s, "family"));
      style.list_style = std::string(attr(s, "list-style-name"));
      if (auto lvl = util::parse_int(attr(s, "default-outline-level"))) style.outline = static_cast<int>(std::clamp<std::int64_t>(*lvl, 0, 6));
      if (XmlNode tp = child(s, "text-properties")) {
        const std::string_view weight = attr(tp, "font-weight");
        if (!weight.empty()) style.bold = weight == "bold" || weight == "600" || weight == "700" || weight == "800" || weight == "900";
        const std::string_view italic = attr(tp, "font-style");
        if (!italic.empty()) style.italic = italic == "italic" || italic == "oblique";
        const std::string_view underline = attr(tp, "text-underline-style");
        if (!underline.empty()) style.underline = underline != "none";
        const std::string_view strike = attr(tp, "text-line-through-style");
        if (!strike.empty()) style.strike = strike != "none";
        const std::string_view position = attr(tp, "text-position");
        if (!position.empty()) {
          style.superscript = position.starts_with("super") || (!position.starts_with("-") && !position.starts_with("0") &&
                                                                 !position.starts_with("sub") && util::is_ascii_digit(position.front()));
          style.subscript = position.starts_with("sub") || position.starts_with("-");
        }
        std::string_view font = attr(tp, "font-name");
        if (font.empty()) font = attr(tp, "font-family");
        if (!font.empty()) style.mono = mono_name(font);
      }
      if (!style.name.empty()) {
        if (!automatic && styles_.contains(style.name)) continue;
        styles_[style.name] = std::move(style);
      }
    } else if (kind == "list-style") {
      std::map<int, ListLevelKind>& levels = list_styles_[std::string(attr(s, "name"))];
      for (XmlNode level : s.elements()) {
        const auto lvl = util::parse_int(attr(level, "level"));
        if (!lvl) continue;
        ListLevelKind k;
        k.ordered = level.local_name() == "list-level-style-number";
        const std::string_view fmt = attr(level, "num-format");
        if (k.ordered) {
          if (fmt == "a") k.numbering = ir::ListNumbering::LowerAlpha;
          if (fmt == "A") k.numbering = ir::ListNumbering::UpperAlpha;
          if (fmt == "i") k.numbering = ir::ListNumbering::LowerRoman;
          if (fmt == "I") k.numbering = ir::ListNumbering::UpperRoman;
          if (fmt.empty()) k.ordered = false;
          if (auto start = util::parse_int(attr(level, "start-value"))) k.start = static_cast<int>(*start);
        }
        levels[static_cast<int>(*lvl)] = k;
      }
    }
  }
}

ListLevelKind OdfReader::list_kind(const std::string& style, int level) const {
  const auto it = list_styles_.find(style);
  if (it == list_styles_.end()) return {};
  const auto lv = it->second.find(level + 1);
  return lv == it->second.end() ? ListLevelKind{} : lv->second;
}

void OdfReader::load_meta(XmlNode meta) {
  for (XmlNode n : meta.elements()) {
    const std::string_view name = n.local_name();
    std::string value(util::trim(n.text()));
    if (value.empty()) continue;
    if (name == "title") doc_.metadata.title = value;
    else if (name == "language") doc_.metadata.language = value;
    else if (name == "initial-creator") doc_.metadata.author = value;
    else if (name == "creator" && doc_.metadata.author.empty()) doc_.metadata.author = value;
    else if (name == "creation-date") doc_.metadata.date = value;
    else if (name == "subject" || name == "description" || name == "keyword" || name == "date" || name == "generator") {
      doc_.metadata.extra[std::string(name)] = value;
    }
  }
}

std::optional<ir::EncodedImage> OdfReader::image(XmlNode node) {
  ir::EncodedImage out;
  if (XmlNode data = child(node, "binary-data")) {
    std::string b64;
    for (char c : data.text()) {
      if (!util::is_ascii_space(c)) b64.push_back(c);
    }
    auto decoded = util::base64_decode(b64);
    if (!decoded) return std::nullopt;
    out.bytes = std::move(*decoded);
  } else {
    const std::string href = percent_decode(attr(node, "href"));
    if (href.empty()) return std::nullopt;
    if (href.find("://") != std::string::npos) {
      out.url = href;
      return out;
    }
    if (!package_) return std::nullopt;
    std::string member = href;
    if (member.starts_with("./")) member.erase(0, 2);
    auto bytes = package_->try_read(member);
    if (!bytes) return std::nullopt;
    out.bytes = std::move(*bytes);
  }
  out.mime = document::sniff_image_mime(out.bytes);
  if (out.mime.empty()) out.mime = std::string(attr(node, "mime-type"));
  return out;
}

void OdfReader::frame(XmlNode frame, InlineBuilder& out, const RunFormat& format, int depth, ir::Blocks& extra) {
  if (depth > max_nesting) return;
  std::string alt(util::trim(child(frame, "desc").text()));
  if (alt.empty()) alt = std::string(util::trim(child(frame, "title").text()));
  for (XmlNode c : frame.elements()) {
    if (c.local_name() == "image") {
      // Several images in one frame are alternative renditions; take the first.
      if (auto picture = image(c)) {
        RunFormat plain;
        plain.link = format.link;
        out.node(ir::InlineImage{std::move(*picture), alt}, plain);
        // A caption inside the image (draw:image text) is rare; ignore.
        return;
      }
    } else if (c.local_name() == "text-box") {
      FlowBuilder flow;
      blocks(c, flow, depth + 1, -1, {}, {});
      for (ir::Block& b : flow.take()) extra.push_back(std::move(b));
      return;
    } else if (c.local_name() == "object") {
      // Embedded objects (charts, formulas): their replacement image, if any, follows.
      continue;
    }
  }
}

void OdfReader::spans(XmlNode node, InlineBuilder& out, RunFormat format, int depth, ir::Blocks& extra) {
  if (depth > max_nesting) return;
  for (XmlNode c = node.first_child(); c; c = c.next_sibling()) {
    if (c.is_text()) {
      // ODF collapses runs of white space; text:s carries significant spaces.
      std::string text;
      bool space = false;
      for (char ch : c.value()) {
        if (ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r') {
          if (!space) text.push_back(' ');
          space = true;
        } else {
          text.push_back(ch);
          space = false;
        }
      }
      out.text(text, format);
      continue;
    }
    if (!c.is_element()) continue;
    const std::string_view name = c.local_name();
    if (name == "span") {
      spans(c, out, apply_style(attr(c, "style-name"), format), depth + 1, extra);
    } else if (name == "a") {
      RunFormat linked = apply_style(attr(c, "style-name"), format);
      linked.link = std::string(attr(c, "href"));
      linked.underline = false;
      spans(c, out, linked, depth + 1, extra);
    } else if (name == "s") {
      const auto count = util::parse_int(attr(c, "c")).value_or(1);
      out.text(std::string(static_cast<std::size_t>(std::clamp<std::int64_t>(count, 1, 64)), ' '), format);
    } else if (name == "tab") {
      out.text("\t", format);
    } else if (name == "line-break") {
      out.line_break(format);
    } else if (name == "note") {
      if (!settings_.notes) continue;
      FlowBuilder flow;
      blocks(child(c, "note-body"), flow, depth + 1, -1, {}, {});
      const std::string label = std::to_string(footnotes_.size() + 1);
      footnotes_.push_back(ir::Footnote{label, flow.take()});
      ir::Link link;
      link.url = "#fn-" + label;
      link.note = label;
      link.children.push_back(ir::Text{label});
      out.node(std::move(link), RunFormat{});
    } else if (name == "frame") {
      frame(c, out, format, depth + 1, extra);
    } else if (name == "bookmark" || name == "bookmark-start" || name == "reference-mark" ||
               name == "reference-mark-start") {
      if (bookmark_.empty()) bookmark_ = std::string(attr(c, "name"));
    } else if (name == "annotation") {
      if (!settings_.comments) continue;
      FlowBuilder flow;
      blocks(c, flow, depth + 1, -1, {}, {});
      const std::string label = std::to_string(footnotes_.size() + 1);
      ir::Blocks body = flow.take();
      if (settings_.metadata) {
        const std::string author(util::trim(child(c, "creator").text()));
        if (!author.empty() && !body.empty()) {
          if (auto* para = body.front().get_if<ir::Paragraph>()) para->content.insert(para->content.begin(), ir::Text{author + ": "});
        }
      }
      footnotes_.push_back(ir::Footnote{label, std::move(body)});
      ir::Link link;
      link.url = "#fn-" + label;
      link.note = label;
      link.children.push_back(ir::Text{label});
      out.node(std::move(link), RunFormat{});
    } else if (name == "soft-page-break" || name == "annotation-end" || name == "bookmark-end" ||
               name == "change-start" || name == "change-end" || name == "change") {
      // Nothing visible.
    } else if (name == "ruby") {
      spans(child(c, "ruby-base"), out, format, depth + 1, extra);
    } else if (name == "meta" || name == "meta-field" || name == "date" || name == "time" || name == "page-number" ||
               name == "sequence" || name == "sequence-ref" || name == "bookmark-ref" || name == "reference-ref" ||
               name == "chapter" || name == "title" || name == "subject" || name == "author-name" ||
               name == "user-defined" || name == "variable-get" || name == "page-count" || name == "file-name" ||
               name == "number" || name == "hidden-text" || name == "conditional-text" || name == "text-input") {
      if (name == "hidden-text" && !settings_.hidden) continue;
      spans(c, out, format, depth + 1, extra);
    } else if (name == "custom-shape" || name == "rect" || name == "g") {
      for (XmlNode d : c.elements()) {
        if (d.local_name() == "p") spans(d, out, format, depth + 1, extra);
      }
    }
  }
}

void OdfReader::paragraph(XmlNode p, FlowBuilder& flow, int depth, bool heading_element) {
  const std::string_view style_name = attr(p, "style-name");
  const std::string names = display_name(style_name);
  int heading = 0;
  if (heading_element) {
    heading = static_cast<int>(std::clamp<std::int64_t>(util::parse_int(attr(p, "outline-level")).value_or(1), 1, 6));
  } else {
    heading = outline_of(style_name);
  }
  const bool code = names.find("preformatted") != std::string::npos || names.find("source") != std::string::npos ||
                    names.find("code") != std::string::npos;
  const bool quote = names.find("quotations") != std::string::npos || names.find("quote") != std::string::npos;
  const bool caption = names.find("caption") != std::string::npos || names.find("illustration") != std::string::npos;
  const bool title = names.starts_with("title|");

  RunFormat base;
  if (heading == 0 && !code && !quote && !caption && !title) base = apply_style(style_name, RunFormat{});
  base.link.clear();
  InlineBuilder out;
  ir::Blocks extra;
  bookmark_.clear();
  spans(p, out, base, depth, extra);
  const std::string plain = out.plain();
  ir::Inlines content = out.take();
  if (!code) trim_inlines(content);
  ir::Attributes attributes;
  if (!bookmark_.empty()) attributes.id = bookmark_;
  if (title && doc_.metadata.title.empty()) doc_.metadata.title = std::string(util::trim(plain));
  if (title && heading == 0) heading = 1;

  if (!content.empty()) {
    if (heading > 0) {
      flow.block(ir::Block(ir::Heading{heading, std::move(content)}, std::move(attributes)));
    } else if (code) {
      flow.block(ir::CodeBlock{{}, plain});
    } else if (quote) {
      flow.block(ir::BlockQuote{{ir::Paragraph{std::move(content)}}});
    } else if (content.size() == 1 && content.front().is<ir::InlineImage>()) {
      auto& picture = *content.front().get_if<ir::InlineImage>();
      flow.block(ir::ImageBlock{std::move(picture.source), picture.alt, {}});
    } else if (caption) {
      flow.block(ir::Block(ir::Paragraph{std::move(content)}, ir::Attributes{{}, {"caption"}, {}}));
    } else {
      flow.block(ir::Block(ir::Paragraph{std::move(content)}, std::move(attributes)));
    }
  }
  for (ir::Block& b : extra) flow.block(std::move(b));
}

void OdfReader::list(XmlNode node, FlowBuilder& flow, int depth, int level, std::string style, const std::string& list_id) {
  if (depth > max_nesting) return;
  if (!attr(node, "style-name").empty()) style = std::string(attr(node, "style-name"));
  // Consecutive lists read as one (presentations split an outline into one
  // text:list per paragraph); a paragraph between lists ends the list anyway.
  (void)list_counter_;
  const std::string id = level == 0 ? std::string("odf") : list_id;
  for (XmlNode item : node.elements()) {
    if (item.local_name() != "list-item" && item.local_name() != "list-header") continue;
    bool first = true;
    for (XmlNode c : item.elements()) {
      const std::string_view name = c.local_name();
      if (name == "list") {
        list(c, flow, depth + 1, level + 1, style, id);
        first = false;
      } else if (name == "p" || name == "h") {
        std::string item_style = style;
        if (item_style.empty()) item_style = list_style_of(attr(c, "style-name"));
        FlowBuilder one;
        paragraph(c, one, depth + 1, false);
        ir::Blocks blocks = one.take();
        if (blocks.empty()) blocks.push_back(ir::Paragraph{});
        for (ir::Block& b : blocks) {
          if (first && item.local_name() == "list-item") {
            const ListLevelKind kind = list_kind(item_style, level);
            FlowBuilder::ListInfo info;
            info.list_id = id;
            info.level = level;
            info.ordered = kind.ordered;
            info.numbering = kind.numbering;
            info.start = kind.start;
            if (auto start = util::parse_int(attr(item, "start-value"))) info.start = static_cast<int>(*start);
            if (b.is<ir::Heading>()) b = ir::Paragraph{std::move(b.get_if<ir::Heading>()->content)};
            flow.list_item(info, std::move(b));
            first = false;
          } else {
            flow.continuation(std::move(b));
          }
        }
      } else {
        FlowBuilder inner;
        blocks(c, inner, depth + 1, -1, {}, {});
        for (ir::Block& b : inner.take()) flow.continuation(std::move(b));
      }
    }
  }
}

ir::Block OdfReader::table(XmlNode node, int depth) {
  ir::TableBlock out;
  const auto add_rows = [&](XmlNode container, bool header, auto& self) -> void {
    for (XmlNode row : container.elements()) {
      const std::string_view name = row.local_name();
      if (name == "table-header-rows") {
        self(row, true, self);
        continue;
      }
      if (name == "table-rows" || name == "table-row-group") {
        self(row, header, self);
        continue;
      }
      if (name != "table-row") continue;
      const auto repeat = std::clamp<std::int64_t>(util::parse_int(attr(row, "number-rows-repeated")).value_or(1), 1, 64);
      ir::TableRow r;
      r.header = header;
      for (XmlNode cell : row.elements()) {
        if (cell.local_name() != "table-cell") continue;  // covered cells belong to a span
        ir::TableCell c;
        c.colspan = static_cast<int>(std::clamp<std::int64_t>(util::parse_int(attr(cell, "number-columns-spanned")).value_or(1), 1, 1000));
        c.rowspan = static_cast<int>(std::clamp<std::int64_t>(util::parse_int(attr(cell, "number-rows-spanned")).value_or(1), 1, 1000));
        FlowBuilder flow;
        blocks(cell, flow, depth + 1, -1, {}, {});
        for (const ir::Block& b : flow.take()) {
          ir::Inlines piece;
          if (const auto* para = b.get_if<ir::Paragraph>()) piece = para->content;
          else if (const auto* h = b.get_if<ir::Heading>()) piece = h->content;
          else if (const auto* img = b.get_if<ir::ImageBlock>()) piece.push_back(ir::InlineImage{img->source, img->alt});
          else {
            std::string text = ir::plain_text(ir::Blocks{b});
            while (!text.empty() && text.back() == '\n') text.pop_back();
            piece = text_with_breaks(text);
          }
          if (piece.empty()) continue;
          if (!c.content.empty()) c.content.push_back(ir::LineBreak{});
          for (ir::Inline& x : piece) c.content.push_back(std::move(x));
        }
        const auto cols = std::clamp<std::int64_t>(util::parse_int(attr(cell, "number-columns-repeated")).value_or(1), 1, 64);
        for (std::int64_t k = 0; k < cols; ++k) r.cells.push_back(c);
      }
      for (std::int64_t k = 0; k < repeat; ++k) out.rows.push_back(r);
    }
  };
  add_rows(node, false, add_rows);
  return out;
}

void OdfReader::blocks(XmlNode node, FlowBuilder& flow, int depth, int list_level, const std::string& list_style,
                       const std::string& list_id) {
  (void)list_level;
  (void)list_style;
  (void)list_id;
  if (depth > max_nesting) return;
  for (XmlNode c : node.elements()) {
    const std::string_view name = c.local_name();
    if (name == "p") {
      paragraph(c, flow, depth, false);
    } else if (name == "h") {
      paragraph(c, flow, depth, true);
    } else if (name == "list") {
      list(c, flow, depth + 1, 0, {}, {});
    } else if (name == "table") {
      flow.block(table(c, depth + 1));
    } else if (name == "section" || name == "index-body" || name == "table-of-content" ||
               name == "alphabetical-index" || name == "illustration-index" || name == "user-index" ||
               name == "bibliography" || name == "object-index" || name == "table-index" || name == "changed-region" ||
               name == "inserted" || name == "numbered-paragraph" || name == "text") {
      if (name == "numbered-paragraph") {
        FlowBuilder inner;
        blocks(c, inner, depth + 1, -1, {}, {});
        for (ir::Block& b : inner.take()) flow.block(std::move(b));
        continue;
      }
      blocks(c, flow, depth + 1, -1, {}, {});
    } else if (name == "frame") {
      InlineBuilder out;
      ir::Blocks extra;
      frame(c, out, RunFormat{}, depth + 1, extra);
      ir::Inlines content = out.take();
      if (content.size() == 1 && content.front().is<ir::InlineImage>()) {
        auto& picture = *content.front().get_if<ir::InlineImage>();
        flow.block(ir::ImageBlock{std::move(picture.source), picture.alt, {}});
      }
      for (ir::Block& b : extra) flow.block(std::move(b));
    } else if (name == "soft-page-break") {
      // Layout hint only.
    } else if (name == "deletion") {
      if (settings_.tracked_changes) blocks(c, flow, depth + 1, -1, {}, {});
    }
  }
}

ir::Blocks OdfReader::slide_frames(XmlNode page, int depth) {
  ir::Blocks title;
  ir::Blocks body;
  ir::Blocks notes;
  for (XmlNode c : page.elements()) {
    const std::string_view name = c.local_name();
    if (name == "frame" || name == "custom-shape" || name == "rect" || name == "g") {
      const std::string_view kind = attr(c, "class");
      FlowBuilder flow;
      if (XmlNode box = child(c, "text-box")) {
        blocks(box, flow, depth + 1, -1, {}, {});
      } else if (name == "custom-shape" || name == "rect") {
        blocks(c, flow, depth + 1, -1, {}, {});
      } else if (name == "g") {
        for (ir::Block& b : slide_frames(c, depth + 1)) flow.block(std::move(b));
      } else if (XmlNode tbl = child(c, "table")) {
        flow.block(table(tbl, depth + 1));
      } else {
        InlineBuilder out;
        ir::Blocks extra;
        frame(c, out, RunFormat{}, depth + 1, extra);
        ir::Inlines content = out.take();
        if (content.size() == 1 && content.front().is<ir::InlineImage>()) {
          auto& picture = *content.front().get_if<ir::InlineImage>();
          flow.block(ir::ImageBlock{std::move(picture.source), picture.alt, {}});
        }
        for (ir::Block& b : extra) flow.block(std::move(b));
      }
      ir::Blocks blocks_out = flow.take();
      if (kind == "title" && title.empty()) {
        for (ir::Block& b : blocks_out) title.push_back(std::move(b));
      } else if (kind == "page" || kind == "header" || kind == "footer" || kind == "date-time" || kind == "page-number") {
        continue;
      } else {
        for (ir::Block& b : blocks_out) body.push_back(std::move(b));
      }
    } else if (name == "notes" && settings_.notes) {
      for (XmlNode f : c.elements()) {
        if (attr(f, "class") != "notes") continue;
        FlowBuilder flow;
        blocks(child(f, "text-box"), flow, depth + 1, -1, {}, {});
        for (ir::Block& b : flow.take()) notes.push_back(std::move(b));
      }
    }
  }
  std::string title_text;
  for (const ir::Block& b : title) {
    std::string t = ir::plain_text(ir::Blocks{b});
    while (!t.empty() && t.back() == '\n') t.pop_back();
    if (!title_text.empty() && !t.empty()) title_text += " ";
    title_text += t;
  }
  return make_slide(title_text, std::move(body), std::move(notes), attr(page, "name"));
}

Result<ir::Document> OdfReader::read_body(XmlNode body) {
  if (XmlNode text = child(body, "text")) {
    FlowBuilder flow;
    blocks(text, flow, 0, -1, {}, {});
    doc_.blocks = flow.take();
  } else if (XmlNode pres = child(body, "presentation")) {
    std::vector<XmlNode> pages = children(pres, "page");
    for (std::size_t i = 0; i < pages.size(); ++i) {
      CVT_TRY(ctx_.checkpoint(static_cast<double>(i) / static_cast<double>(pages.size()), "Reading slides"));
      if (i > 0) doc_.blocks.push_back(ir::PageBreak{});
      for (ir::Block& b : slide_frames(pages[i], 0)) doc_.blocks.push_back(std::move(b));
    }
  } else if (XmlNode drawing = child(body, "drawing")) {
    std::vector<XmlNode> pages = children(drawing, "page");
    for (std::size_t i = 0; i < pages.size(); ++i) {
      if (i > 0) doc_.blocks.push_back(ir::PageBreak{});
      for (ir::Block& b : slide_frames(pages[i], 0)) doc_.blocks.push_back(std::move(b));
    }
  } else {
    return fail(Status::Decode, "the OpenDocument file has no text or presentation body");
  }
  // Merge consecutive code blocks and quotes; attach captions to pictures.
  ir::Blocks merged;
  for (ir::Block& b : doc_.blocks) {
    if (!merged.empty()) {
      if (auto* code = b.get_if<ir::CodeBlock>(); code && merged.back().is<ir::CodeBlock>()) {
        merged.back().get_if<ir::CodeBlock>()->code += "\n" + code->code;
        continue;
      }
      if (auto* quote = b.get_if<ir::BlockQuote>(); quote && merged.back().is<ir::BlockQuote>()) {
        for (ir::Block& inner : quote->blocks) merged.back().get_if<ir::BlockQuote>()->blocks.push_back(std::move(inner));
        continue;
      }
      if (auto* para = b.get_if<ir::Paragraph>(); para && b.attr.has_class("caption") && merged.back().is<ir::ImageBlock>()) {
        auto& picture = *merged.back().get_if<ir::ImageBlock>();
        if (picture.caption.empty()) {
          picture.caption = std::string(util::trim(ir::plain_text(para->content)));
          continue;
        }
      }
    }
    merged.push_back(std::move(b));
  }
  doc_.blocks = std::move(merged);
  doc_.footnotes = std::move(footnotes_);
  finish_metadata(doc_, settings_);
  return std::move(doc_);
}

Result<ir::Document> OdfReader::read_package(Package& package) {
  package_ = &package;
  if (package.has("META-INF/manifest.xml")) {
    if (auto manifest = package.xml("META-INF/manifest.xml", false)) {
      for (XmlNode entry : manifest->root().elements()) {
        if (child(entry, "encryption-data")) {
          return fail(Status::Decode, "the document is password-protected; it cannot be read");
        }
      }
    }
  }
  if (auto styles = package.xml("styles.xml", false)) {
    XmlNode root = styles->root();
    load_styles(child(root, "styles"), false);
    load_styles(child(root, "automatic-styles"), true);
  }
  if (auto meta = package.xml("meta.xml", false)) load_meta(child(meta->root(), "meta"));
  CVT_TRY_ASSIGN(util::XmlDocument content, package.xml("content.xml", true));
  XmlNode root = content.root();
  load_styles(child(root, "automatic-styles"), true);
  CVT_TRY(ctx_.checkpoint(0.2, "Reading styles"));
  return read_body(child(root, "body"));
}

Result<ir::Document> OdfReader::read_flat(const util::XmlDocument& xml) {
  XmlNode root = xml.root();
  if (root.local_name() != "document") return fail(Status::Decode, "not a flat OpenDocument file");
  load_styles(child(root, "styles"), false);
  load_styles(child(root, "automatic-styles"), true);
  load_meta(child(root, "meta"));
  return read_body(child(root, "body"));
}

Result<AnyIR> read_odf(ReadContext& ctx) {
  OdfReader reader(ctx, read_settings(ctx.options));
  if (as_chars(ctx.input).starts_with("PK")) {
    CVT_TRY_ASSIGN(Package package, Package::open(ctx.input, ctx.limits));
    CVT_TRY_ASSIGN(ir::Document doc, reader.read_package(package));
    return AnyIR(std::move(doc));
  }
  CVT_TRY_ASSIGN(util::XmlDocument xml, parse_untrusted_xml(ctx.input, true));
  CVT_TRY_ASSIGN(ir::Document doc, reader.read_flat(xml));
  return AnyIR(std::move(doc));
}

// =========================================================================== writers

constexpr std::string_view odf_namespaces =
    " xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\""
    " xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\""
    " xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\""
    " xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\""
    " xmlns:draw=\"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0\""
    " xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\""
    " xmlns:xlink=\"http://www.w3.org/1999/xlink\""
    " xmlns:dc=\"http://purl.org/dc/elements/1.1/\""
    " xmlns:meta=\"urn:oasis:names:tc:opendocument:xmlns:meta:1.0\""
    " xmlns:svg=\"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0\""
    " xmlns:presentation=\"urn:oasis:names:tc:opendocument:xmlns:presentation:1.0\""
    " xmlns:loext=\"urn:org:documentfoundation:names:experimental:office:xmlns:loext:1.0\"";

std::string cm(double twips) { return fmt_number(twips / 1440.0 * 2.54) + "cm"; }

class OdtWriter {
 public:
  OdtWriter(const ir::Document& doc, WriteContext& ctx, bool flat) : doc_(doc), ctx_(ctx), flat_(flat) {
    policy_ = image_policy(ctx.options);
    page_ = page_setup(ctx.options);
  }
  Result<Bytes> write();
  // Shared with the presentation writer.
  void inlines(const ir::Inlines& items, std::string& out, int depth);
  void blocks(const ir::Blocks& items, std::string& out, int quote, int depth);
  std::string image_frame(const ir::ImageSource& source, std::string_view alt, bool as_char, double max_width_cm);
  [[nodiscard]] const MediaStore& media() const { return media_; }
  /// A table as table:table XML (for presentation frames).
  std::string table_xml(const ir::TableBlock& table) {
    std::string out;
    this->table(table, out, 0, 1);
    return out;
  }
  /// Presentations cannot hold text:note: notes become superscript numbers
  /// and are collected here to be written at the end of the slide.
  void set_presentation(bool on) { presentation_ = on; }
  [[nodiscard]] std::vector<std::string> take_pending_notes() { return std::exchange(pending_notes_, {}); }
  std::string automatic_styles() const;

 private:
  void block(const ir::Block& b, std::string& out, int quote, int depth);
  void list(const ir::List& list, std::string& out, int quote, int depth, int level);
  void table(const ir::TableBlock& table, std::string& out, int quote, int depth);
  std::string text_style(bool bold, bool italic, bool underline, bool strike, bool sup, bool sub, bool code);
  std::string list_style(const ir::List& list);

  const ir::Document& doc_;
  WriteContext& ctx_;
  bool flat_;
  ImagePolicy policy_;
  PageSetup page_;
  MediaStore media_;
  std::map<std::string, std::string> text_styles_;  // key -> name
  std::vector<std::string> list_styles_;            // XML of automatic list styles
  int tables_ = 0;
  int frames_ = 0;
  int dropped_ = 0;
  std::set<std::string> written_notes_;
  bool presentation_ = false;
  std::vector<std::string> pending_notes_;

  struct Format {
    bool bold = false, italic = false, underline = false, strike = false, sup = false, sub = false, code = false;
  };
  void runs(const ir::Inlines& items, std::string& out, Format format, int depth);
  void text(std::string_view value, std::string& out, const Format& format);
};

std::string OdtWriter::text_style(bool bold, bool italic, bool underline, bool strike, bool sup, bool sub, bool code) {
  std::string key;
  key += bold ? 'b' : '-';
  key += italic ? 'i' : '-';
  key += underline ? 'u' : '-';
  key += strike ? 's' : '-';
  key += sup ? 'p' : sub ? 'd' : '-';
  key += code ? 'c' : '-';
  if (key == "------") return {};
  if (auto it = text_styles_.find(key); it != text_styles_.end()) return it->second;
  const std::string name = "T" + std::to_string(text_styles_.size() + 1);
  text_styles_[key] = name;
  return name;
}

std::string OdtWriter::automatic_styles() const {
  std::string out;
  for (const auto& [key, name] : text_styles_) {
    out += "<style:style style:name=\"" + name + "\" style:family=\"text\"><style:text-properties";
    if (key[0] == 'b') out += " fo:font-weight=\"bold\" style:font-weight-asian=\"bold\" style:font-weight-complex=\"bold\"";
    if (key[1] == 'i') out += " fo:font-style=\"italic\" style:font-style-asian=\"italic\" style:font-style-complex=\"italic\"";
    if (key[2] == 'u') out += " style:text-underline-style=\"solid\" style:text-underline-width=\"auto\" style:text-underline-color=\"font-color\"";
    if (key[3] == 's') out += " style:text-line-through-style=\"solid\" style:text-line-through-type=\"single\"";
    if (key[4] == 'p') out += " style:text-position=\"super 58%\"";
    if (key[4] == 'd') out += " style:text-position=\"sub 58%\"";
    if (key[5] == 'c') out += " style:font-name=\"Liberation Mono\" fo:font-family=\"'Liberation Mono'\" style:font-family-generic=\"modern\" style:font-pitch=\"fixed\"";
    out += "/></style:style>";
  }
  for (const std::string& list : list_styles_) out += list;
  return out;
}

std::string OdtWriter::list_style(const ir::List& list) {
  const std::string name = "L" + std::to_string(list_styles_.size() + 1);
  std::string xml = "<text:list-style style:name=\"" + name + "\">";
  static const std::string_view bullets[3] = {"\xE2\x80\xA2", "\xE2\x97\xA6", "\xE2\x96\xAA"};
  for (int level = 1; level <= 10; ++level) {
    const std::string indent = fmt_number(0.635 * level) + "cm";
    const std::string props =
        "<style:list-level-properties text:list-level-position-and-space-mode=\"label-alignment\">"
        "<style:list-level-label-alignment text:label-followed-by=\"listtab\" text:list-tab-stop-position=\"" +
        indent + "\" fo:text-indent=\"-0.635cm\" fo:margin-left=\"" + indent + "\"/></style:list-level-properties>";
    if (list.ordered) {
      std::string fmt = "1";
      if (list.numbering == ir::ListNumbering::LowerAlpha) fmt = "a";
      if (list.numbering == ir::ListNumbering::UpperAlpha) fmt = "A";
      if (list.numbering == ir::ListNumbering::LowerRoman) fmt = "i";
      if (list.numbering == ir::ListNumbering::UpperRoman) fmt = "I";
      xml += "<text:list-level-style-number text:level=\"" + std::to_string(level) +
             "\" style:num-suffix=\".\" style:num-format=\"" + fmt + "\"";
      if (level == 1 && list.start != 1) xml += " text:start-value=\"" + std::to_string(std::max(0, list.start)) + "\"";
      xml += ">" + props + "</text:list-level-style-number>";
    } else {
      xml += "<text:list-level-style-bullet text:level=\"" + std::to_string(level) + "\" text:bullet-char=\"" +
             std::string(bullets[(level - 1) % 3]) + "\">" + props + "</text:list-level-style-bullet>";
    }
  }
  xml += "</text:list-style>";
  list_styles_.push_back(std::move(xml));
  return name;
}

void OdtWriter::text(std::string_view value, std::string& out, const Format& f) {
  if (value.empty()) return;
  const std::string style = text_style(f.bold, f.italic, f.underline, f.strike, f.sup, f.sub, f.code);
  if (!style.empty()) out += "<text:span text:style-name=\"" + style + "\">";
  // Spaces: a run of n > 1 spaces keeps one literal space and text:s for the rest.
  std::size_t i = 0;
  while (i < value.size()) {
    const char c = value[i];
    if (c == '\t') {
      out += "<text:tab/>";
      ++i;
    } else if (c == '\n') {
      out += "<text:line-break/>";
      ++i;
    } else if (c == ' ') {
      std::size_t j = i;
      while (j < value.size() && value[j] == ' ') ++j;
      const std::size_t count = j - i;
      if (i == 0 || count > 1) {
        const std::size_t literal = i == 0 ? 0 : 1;
        if (literal) out += " ";
        const std::size_t rest = count - literal;
        if (rest > 0) out += rest == 1 ? "<text:s/>" : "<text:s text:c=\"" + std::to_string(rest) + "\"/>";
      } else {
        out += " ";
      }
      i = j;
    } else {
      std::size_t j = i;
      while (j < value.size() && value[j] != ' ' && value[j] != '\t' && value[j] != '\n') ++j;
      out += esc(value.substr(i, j - i));
      i = j;
    }
  }
  if (!style.empty()) out += "</text:span>";
}

std::string OdtWriter::image_frame(const ir::ImageSource& source, std::string_view alt, bool as_char,
                                   double max_width_cm) {
  auto prepared = prepare_image(source, policy_, ctx_);
  if (prepared && !is_web_safe(prepared->extension)) prepared = convert_to_png(*prepared, ctx_);
  if (!prepared) {
    if (policy_.mode == ImageMode::Drop) ++dropped_;
    return {};
  }
  PixelSize px = prepared->size;
  if (px.width == 0 || px.height == 0) px = PixelSize{300, 200};
  double w = px.width / 96.0 * 2.54;
  double h = px.height / 96.0 * 2.54;
  if (w > max_width_cm) {
    h *= max_width_cm / w;
    w = max_width_cm;
  }
  const std::string mime = prepared->mime;
  const std::size_t index = media_.add(std::move(*prepared));
  const auto& item = media_.items()[index];
  const int id = ++frames_;
  std::string out = "<draw:frame draw:style-name=\"fr1\" draw:name=\"Image" + std::to_string(id) +
                    "\" text:anchor-type=\"" + (as_char ? "as-char" : "paragraph") + "\" svg:width=\"" +
                    fmt_number(w) + "cm\" svg:height=\"" + fmt_number(h) + "cm\" draw:z-index=\"" + std::to_string(id) + "\">";
  if (flat_) {
    out += "<draw:image draw:mime-type=\"" + mime + "\"><office:binary-data>" + util::base64_encode(item.image.bytes) +
           "</office:binary-data></draw:image>";
  } else {
    out += "<draw:image xlink:href=\"Pictures/" + item.name +
           "\" xlink:type=\"simple\" xlink:show=\"embed\" xlink:actuate=\"onLoad\" draw:mime-type=\"" + mime + "\"/>";
  }
  if (!alt.empty()) out += "<svg:desc>" + esc(alt) + "</svg:desc>";
  out += "</draw:frame>";
  return out;
}

void OdtWriter::runs(const ir::Inlines& items, std::string& out, Format f, int depth) {
  if (depth > ir::max_document_depth) return;
  for (const ir::Inline& item : items) {
    std::visit(
        [&](const auto& node) {
          using T = std::remove_cvref_t<decltype(node)>;
          if constexpr (std::same_as<T, ir::Text>) {
            text(node.text, out, f);
          } else if constexpr (std::same_as<T, ir::Code>) {
            Format g = f;
            g.code = true;
            text(node.text, out, g);
          } else if constexpr (std::same_as<T, ir::Emphasis>) {
            Format g = f;
            g.italic = true;
            runs(node.children, out, g, depth + 1);
          } else if constexpr (std::same_as<T, ir::Strong>) {
            Format g = f;
            g.bold = true;
            runs(node.children, out, g, depth + 1);
          } else if constexpr (std::same_as<T, ir::Underline>) {
            Format g = f;
            g.underline = true;
            runs(node.children, out, g, depth + 1);
          } else if constexpr (std::same_as<T, ir::Strikethrough>) {
            Format g = f;
            g.strike = true;
            runs(node.children, out, g, depth + 1);
          } else if constexpr (std::same_as<T, ir::Superscript>) {
            Format g = f;
            g.sup = true;
            runs(node.children, out, g, depth + 1);
          } else if constexpr (std::same_as<T, ir::Subscript>) {
            Format g = f;
            g.sub = true;
            runs(node.children, out, g, depth + 1);
          } else if constexpr (std::same_as<T, ir::LineBreak>) {
            out += "<text:line-break/>";
          } else if constexpr (std::same_as<T, ir::InlineImage>) {
            const std::string frame = image_frame(node.source, node.alt, true, 16.0);
            if (frame.empty()) {
              text(node.alt, out, f);
            } else {
              out += frame;
            }
          } else if constexpr (std::same_as<T, ir::Link>) {
            if (!node.note.empty() && presentation_) {
              Format g = f;
              g.sup = true;
              text(node.note, out, g);
              pending_notes_.push_back(node.note);
              return;
            }
            if (!node.note.empty()) {
              for (const ir::Footnote& note : doc_.footnotes) {
                if (note.label != node.note || depth > 8) continue;
                out += "<text:note text:id=\"ftn" + std::to_string(written_notes_.size()) +
                       "\" text:note-class=\"footnote\"><text:note-citation>" + esc(note.label) +
                       "</text:note-citation><text:note-body>";
                written_notes_.insert(note.label);
                std::string body;
                blocks(note.blocks, body, 0, depth + 4);
                if (body.empty()) body = "<text:p text:style-name=\"Footnote\"/>";
                out += body + "</text:note-body></text:note>";
                return;
              }
            }
            if (node.url.empty()) {
              runs(node.children, out, f, depth + 1);
              return;
            }
            out += "<text:a xlink:type=\"simple\" xlink:href=\"" + esc_attr(node.url) + "\">";
            runs(node.children, out, f, depth + 1);
            out += "</text:a>";
          }
        },
        item.node);
  }
}

void OdtWriter::inlines(const ir::Inlines& items, std::string& out, int depth) { runs(items, out, Format{}, depth); }

void OdtWriter::list(const ir::List& list, std::string& out, int quote, int depth, int level) {
  const std::string style = level == 0 ? list_style(list) : std::string();
  out += "<text:list";
  if (!style.empty()) out += " text:style-name=\"" + style + "\"";
  out += ">";
  for (std::size_t i = 0; i < list.items.size(); ++i) {
    out += "<text:list-item>";
    bool any = false;
    for (std::size_t b = 0; b < list.items[i].size(); ++b) {
      const ir::Block& inner = list.items[i][b];
      if (const auto* nested = inner.get_if<ir::List>()) {
        if (!any) out += "<text:p text:style-name=\"List_20_Paragraph\"/>";
        this->list(*nested, out, quote, depth + 1, level + 1);
      } else if (const auto* para = inner.get_if<ir::Paragraph>()) {
        out += "<text:p text:style-name=\"List_20_Paragraph\">";
        if (b == 0 && list.task(i) == ir::TaskState::Open) out += "\xE2\x98\x90 ";
        if (b == 0 && list.task(i) == ir::TaskState::Done) out += "\xE2\x98\x92 ";
        Format f;
        f.bold = list.definition && b == 0;
        runs(para->content, out, f, depth + 1);
        out += "</text:p>";
      } else {
        std::string inner_xml;
        block(inner, inner_xml, 0, depth + 1);
        if (inner.is<ir::TableBlock>() || inner_xml.empty()) {
          out += "<text:p text:style-name=\"List_20_Paragraph\">" +
                 esc(util::trim(ir::plain_text(ir::Blocks{inner}))) + "</text:p>";
        } else {
          out += inner_xml;
        }
      }
      any = true;
    }
    if (!any) out += "<text:p text:style-name=\"List_20_Paragraph\"/>";
    out += "</text:list-item>";
  }
  out += "</text:list>";
}

void OdtWriter::table(const ir::TableBlock& table, std::string& out, int quote, int depth) {
  const document::TableGrid grid = document::table_grid(table);
  if (grid.columns == 0) return;
  (void)quote;
  if (!table.caption.empty()) {
    out += "<text:p text:style-name=\"Table\">";
    inlines(table.caption, out, depth + 1);
    out += "</text:p>";
  }
  const std::string name = "Table" + std::to_string(++tables_);
  out += "<table:table table:name=\"" + name + "\" table:style-name=\"Tbl\"><table:table-column table:style-name=\"TblCol\" "
         "table:number-columns-repeated=\"" + std::to_string(grid.columns) + "\"/>";
  bool header_open = false;
  for (std::size_t r = 0; r < grid.cells.size(); ++r) {
    const bool header = r < grid.header_rows;
    if (header && !header_open && !presentation_) {
      out += "<table:table-header-rows>";
      header_open = true;
    }
    if (!header && header_open) {
      out += "</table:table-header-rows>";
      header_open = false;
    }
    out += "<table:table-row>";
    for (std::size_t c = 0; c < grid.columns; ++c) {
      const ir::TableCell* cell = grid.cells[r][c];
      if (!grid.origin[r][c] && cell) {
        out += "<table:covered-table-cell/>";
        continue;
      }
      out += "<table:table-cell table:style-name=\"TblCell\" office:value-type=\"string\"";
      if (cell && cell->colspan > 1) out += " table:number-columns-spanned=\"" + std::to_string(cell->colspan) + "\"";
      if (cell && cell->rowspan > 1) out += " table:number-rows-spanned=\"" + std::to_string(cell->rowspan) + "\"";
      out += "><text:p text:style-name=\"" + std::string(header ? "Table_20_Heading" : "Table_20_Contents") + "\">";
      if (cell) inlines(cell->content, out, depth + 1);
      out += "</text:p></table:table-cell>";
    }
    out += "</table:table-row>";
  }
  if (header_open) out += "</table:table-header-rows>";
  out += "</table:table>";
}

void OdtWriter::block(const ir::Block& b, std::string& out, int quote, int depth) {
  if (depth > ir::max_document_depth) return;
  std::visit(
      [&](const auto& node) {
        using T = std::remove_cvref_t<decltype(node)>;
        if constexpr (std::same_as<T, ir::Paragraph>) {
          const std::string style = b.attr.has_class("caption") ? "Caption" : quote > 0 ? "Quotations" : "Text_20_body";
          out += "<text:p text:style-name=\"" + style + "\">";
          if (!b.attr.id.empty()) out += "<text:bookmark text:name=\"" + esc_attr(b.attr.id) + "\"/>";
          inlines(node.content, out, depth + 1);
          out += "</text:p>";
        } else if constexpr (std::same_as<T, ir::Heading>) {
          const int level = std::clamp(node.level, 1, 6);
          out += "<text:h text:style-name=\"Heading_20_" + std::to_string(level) + "\" text:outline-level=\"" +
                 std::to_string(level) + "\">";
          if (!b.attr.id.empty()) out += "<text:bookmark text:name=\"" + esc_attr(b.attr.id) + "\"/>";
          inlines(node.content, out, depth + 1);
          out += "</text:h>";
        } else if constexpr (std::same_as<T, ir::List>) {
          list(node, out, quote, depth + 1, 0);
        } else if constexpr (std::same_as<T, ir::CodeBlock>) {
          std::string_view code = node.code;
          while (code.ends_with('\n')) code.remove_suffix(1);
          for (std::string_view line : util::split(code, '\n')) {
            if (line.ends_with('\r')) line.remove_suffix(1);
            out += "<text:p text:style-name=\"Preformatted_20_Text\">";
            text(line, out, Format{});
            out += "</text:p>";
          }
        } else if constexpr (std::same_as<T, ir::BlockQuote>) {
          blocks(node.blocks, out, quote + 1, depth + 1);
        } else if constexpr (std::same_as<T, ir::TableBlock>) {
          table(node, out, quote, depth + 1);
        } else if constexpr (std::same_as<T, ir::ImageBlock>) {
          const double width = (page_.width - 2.0 * page_.margin) / 1440.0 * 2.54;
          const std::string frame = image_frame(node.source, node.alt, true, width);
          out += "<text:p text:style-name=\"Figure\">";
          out += frame.empty() ? esc(node.alt) : frame;
          out += "</text:p>";
          if (!node.caption.empty()) out += "<text:p text:style-name=\"Caption\">" + esc(node.caption) + "</text:p>";
        } else if constexpr (std::same_as<T, ir::HorizontalRule>) {
          out += "<text:p text:style-name=\"Horizontal_20_Line\"/>";
        } else if constexpr (std::same_as<T, ir::PageBreak>) {
          out += "<text:p text:style-name=\"PageBreak\"/>";
        }
      },
      b.node);
}

void OdtWriter::blocks(const ir::Blocks& items, std::string& out, int quote, int depth) {
  for (const ir::Block& b : items) block(b, out, quote, depth);
}

std::string common_text_styles(const std::string& language) {
  std::string lang_attrs;
  if (!language.empty()) {
    const std::size_t dash = language.find('-');
    lang_attrs = " fo:language=\"" + esc_attr(language.substr(0, dash)) + "\"";
    if (dash != std::string::npos) lang_attrs += " fo:country=\"" + esc_attr(language.substr(dash + 1)) + "\"";
  }
  std::string out =
      "<style:default-style style:family=\"paragraph\"><style:paragraph-properties fo:margin-top=\"0cm\" "
      "fo:margin-bottom=\"0.212cm\"/><style:text-properties style:font-name=\"Liberation Serif\" fo:font-size=\"11pt\"" +
      lang_attrs +
      "/></style:default-style>"
      "<style:style style:name=\"Standard\" style:family=\"paragraph\" style:class=\"text\"/>"
      "<style:style style:name=\"Text_20_body\" style:display-name=\"Text body\" style:family=\"paragraph\" "
      "style:parent-style-name=\"Standard\" style:class=\"text\"><style:paragraph-properties fo:margin-top=\"0cm\" "
      "fo:margin-bottom=\"0.247cm\"/></style:style>"
      "<style:style style:name=\"Heading\" style:family=\"paragraph\" style:parent-style-name=\"Standard\" "
      "style:next-style-name=\"Text_20_body\" style:class=\"text\"><style:paragraph-properties fo:margin-top=\"0.423cm\" "
      "fo:margin-bottom=\"0.212cm\" fo:keep-with-next=\"always\"/><style:text-properties style:font-name=\"Liberation Sans\" "
      "fo:font-weight=\"bold\"/></style:style>";
  static constexpr const char* sizes[6] = {"18pt", "16pt", "14pt", "13pt", "12pt", "11pt"};
  for (int level = 1; level <= 6; ++level) {
    out += "<style:style style:name=\"Heading_20_" + std::to_string(level) + "\" style:display-name=\"Heading " +
           std::to_string(level) +
           "\" style:family=\"paragraph\" style:parent-style-name=\"Heading\" style:next-style-name=\"Text_20_body\" "
           "style:default-outline-level=\"" +
           std::to_string(level) + "\" style:class=\"text\"><style:text-properties fo:font-size=\"" + sizes[level - 1] +
           "\" fo:font-weight=\"bold\"/></style:style>";
  }
  out +=
      "<style:style style:name=\"Quotations\" style:family=\"paragraph\" style:parent-style-name=\"Standard\" "
      "style:class=\"html\"><style:paragraph-properties fo:margin-left=\"1cm\" fo:margin-right=\"1cm\"/>"
      "<style:text-properties fo:font-style=\"italic\"/></style:style>"
      "<style:style style:name=\"Preformatted_20_Text\" style:display-name=\"Preformatted Text\" style:family=\"paragraph\" "
      "style:parent-style-name=\"Standard\" style:class=\"html\"><style:paragraph-properties fo:margin-top=\"0cm\" "
      "fo:margin-bottom=\"0cm\" fo:background-color=\"#f3f3f3\"/><style:text-properties style:font-name=\"Liberation Mono\" "
      "fo:font-family=\"'Liberation Mono'\" style:font-family-generic=\"modern\" style:font-pitch=\"fixed\" "
      "fo:font-size=\"10pt\"/></style:style>"
      "<style:style style:name=\"List_20_Paragraph\" style:display-name=\"List Paragraph\" style:family=\"paragraph\" "
      "style:parent-style-name=\"Text_20_body\" style:class=\"list\"><style:paragraph-properties fo:margin-top=\"0cm\" "
      "fo:margin-bottom=\"0.1cm\"/></style:style>"
      "<style:style style:name=\"Table_20_Contents\" style:display-name=\"Table Contents\" style:family=\"paragraph\" "
      "style:parent-style-name=\"Standard\" style:class=\"extra\"/>"
      "<style:style style:name=\"Table_20_Heading\" style:display-name=\"Table Heading\" style:family=\"paragraph\" "
      "style:parent-style-name=\"Table_20_Contents\" style:class=\"extra\"><style:text-properties fo:font-weight=\"bold\"/>"
      "</style:style>"
      "<style:style style:name=\"Caption\" style:family=\"paragraph\" style:parent-style-name=\"Standard\" "
      "style:class=\"extra\"><style:paragraph-properties fo:text-align=\"center\"/><style:text-properties "
      "fo:font-size=\"10pt\" fo:font-style=\"italic\"/></style:style>"
      "<style:style style:name=\"Table\" style:family=\"paragraph\" style:parent-style-name=\"Caption\" style:class=\"extra\"/>"
      "<style:style style:name=\"Figure\" style:family=\"paragraph\" style:parent-style-name=\"Standard\" "
      "style:class=\"extra\"><style:paragraph-properties fo:text-align=\"center\"/></style:style>"
      "<style:style style:name=\"Footnote\" style:family=\"paragraph\" style:parent-style-name=\"Standard\" "
      "style:class=\"extra\"><style:paragraph-properties fo:margin-left=\"0.6cm\" fo:text-indent=\"-0.6cm\"/>"
      "<style:text-properties fo:font-size=\"10pt\"/></style:style>"
      "<style:style style:name=\"Horizontal_20_Line\" style:display-name=\"Horizontal Line\" style:family=\"paragraph\" "
      "style:parent-style-name=\"Standard\" style:class=\"html\"><style:paragraph-properties fo:margin-bottom=\"0.5cm\" "
      "fo:border-bottom=\"0.06pt solid #808080\" fo:padding=\"0cm\"/></style:style>";
  return out;
}

Result<Bytes> OdtWriter::write() {
  std::string body;
  const std::size_t total = doc_.blocks.size();
  for (std::size_t i = 0; i < total; ++i) {
    if ((i & 63) == 0) {
      CVT_TRY(ctx_.checkpoint(static_cast<double>(i) / static_cast<double>(std::max<std::size_t>(total, 1)), "Writing"));
    }
    block(doc_.blocks[i], body, 0, 0);
  }
  if (dropped_ > 0) ctx_.warn(std::to_string(dropped_) + " picture(s) were left out as requested.");
  std::string language = ctx_.options.get_string("language", "");
  if (language.empty()) language = doc_.metadata.language;
  const bool strip = options::resolved_strip_metadata(ctx_.options);
  const bool landscape = page_.width > page_.height;

  const std::string automatic =
      "<style:style style:name=\"PageBreak\" style:family=\"paragraph\" style:parent-style-name=\"Standard\">"
      "<style:paragraph-properties fo:break-before=\"page\"/></style:style>"
      "<style:style style:name=\"Tbl\" style:family=\"table\"><style:table-properties style:width=\"" +
      cm(page_.width - 2.0 * page_.margin) +
      "\" table:align=\"margins\"/></style:style>"
      "<style:style style:name=\"TblCol\" style:family=\"table-column\"/>"
      "<style:style style:name=\"TblCell\" style:family=\"table-cell\"><style:table-cell-properties fo:padding=\"0.097cm\" "
      "fo:border=\"0.5pt solid #000000\"/></style:style>"
      "<style:style style:name=\"fr1\" style:family=\"graphic\"><style:graphic-properties style:vertical-pos=\"top\" "
      "style:vertical-rel=\"baseline\" style:horizontal-pos=\"center\" style:horizontal-rel=\"paragraph\"/></style:style>" +
      automatic_styles();

  const std::string meta_inner = [&] {
    std::string m;
    if (!doc_.metadata.title.empty()) m += "<dc:title>" + esc(doc_.metadata.title) + "</dc:title>";
    if (!language.empty()) m += "<dc:language>" + esc(language) + "</dc:language>";
    if (!strip && !doc_.metadata.author.empty()) m += "<meta:initial-creator>" + esc(doc_.metadata.author) + "</meta:initial-creator>";
    if (!strip && !doc_.metadata.date.empty()) m += "<meta:creation-date>" + esc(doc_.metadata.date) + "</meta:creation-date>";
    return m;
  }();
  const std::string styles_inner = common_text_styles(language);
  const std::string page_layout =
      "<style:page-layout style:name=\"pm1\"><style:page-layout-properties fo:page-width=\"" + cm(page_.width) +
      "\" fo:page-height=\"" + cm(page_.height) + "\" style:print-orientation=\"" + (landscape ? "landscape" : "portrait") +
      "\" fo:margin-top=\"" + cm(page_.margin) + "\" fo:margin-bottom=\"" + cm(page_.margin) + "\" fo:margin-left=\"" +
      cm(page_.margin) + "\" fo:margin-right=\"" + cm(page_.margin) + "\"/></style:page-layout>";
  const std::string master =
      "<style:master-page style:name=\"Standard\" style:page-layout-name=\"pm1\"/>";
  const std::string fonts =
      "<office:font-face-decls><style:font-face style:name=\"Liberation Serif\" svg:font-family=\"'Liberation Serif'\" "
      "style:font-family-generic=\"roman\" style:font-pitch=\"variable\"/><style:font-face style:name=\"Liberation Sans\" "
      "svg:font-family=\"'Liberation Sans'\" style:font-family-generic=\"swiss\" style:font-pitch=\"variable\"/>"
      "<style:font-face style:name=\"Liberation Mono\" svg:font-family=\"'Liberation Mono'\" "
      "style:font-family-generic=\"modern\" style:font-pitch=\"fixed\"/></office:font-face-decls>";

  if (flat_) {
    std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document" + std::string(odf_namespaces) +
                      " office:version=\"1.3\" office:mimetype=\"application/vnd.oasis.opendocument.text\">";
    if (!meta_inner.empty()) out += "<office:meta>" + meta_inner + "</office:meta>";
    out += fonts + "<office:styles>" + styles_inner + "</office:styles><office:automatic-styles>" + automatic +
           page_layout + "</office:automatic-styles><office:master-styles>" + master +
           "</office:master-styles><office:body><office:text>" + body + "</office:text></office:body></office:document>\n";
    return to_bytes(out);
  }

  const std::string content = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-content" +
                              std::string(odf_namespaces) + " office:version=\"1.3\">" + fonts +
                              "<office:automatic-styles>" + automatic + "</office:automatic-styles><office:body><office:text>" +
                              body + "</office:text></office:body></office:document-content>\n";
  const std::string styles = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-styles" +
                             std::string(odf_namespaces) + " office:version=\"1.3\">" + fonts + "<office:styles>" +
                             styles_inner + "</office:styles><office:automatic-styles>" + page_layout +
                             "</office:automatic-styles><office:master-styles>" + master +
                             "</office:master-styles></office:document-styles>\n";
  const std::string meta = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-meta" + std::string(odf_namespaces) +
                           " office:version=\"1.3\"><office:meta>" + meta_inner + "</office:meta></office:document-meta>\n";
  std::string manifest =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<manifest:manifest "
      "xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.3\">"
      "<manifest:file-entry manifest:full-path=\"/\" manifest:version=\"1.3\" "
      "manifest:media-type=\"application/vnd.oasis.opendocument.text\"/>"
      "<manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>"
      "<manifest:file-entry manifest:full-path=\"styles.xml\" manifest:media-type=\"text/xml\"/>"
      "<manifest:file-entry manifest:full-path=\"meta.xml\" manifest:media-type=\"text/xml\"/>";
  for (const auto& item : media_.items()) {
    manifest += "<manifest:file-entry manifest:full-path=\"Pictures/" + item.name + "\" manifest:media-type=\"" +
                item.image.mime + "\"/>";
  }
  manifest += "</manifest:manifest>\n";

  int level = static_cast<int>(ctx_.options.get_int("compression_level", 9, 0, 9));
  if (!ctx_.options.was_set("compression_level")) level = 9;
  util::ZipEntryOptions stored;
  stored.level = 0;
  util::ZipEntryOptions entry;
  entry.level = level;
  util::ZipWriter zip;
  CVT_TRY(zip.add("mimetype", "application/vnd.oasis.opendocument.text", stored));
  CVT_TRY(zip.add("content.xml", content, entry));
  CVT_TRY(zip.add("styles.xml", styles, entry));
  CVT_TRY(zip.add("meta.xml", meta, entry));
  for (const auto& item : media_.items()) CVT_TRY(zip.add("Pictures/" + item.name, item.image.bytes, stored));
  CVT_TRY(zip.add("META-INF/manifest.xml", manifest, entry));
  return zip.finish();
}

Result<Bytes> write_odt(const AnyIR& value, WriteContext& ctx) {
  OdtWriter writer(value.get<ir::Document>(), ctx, false);
  return writer.write();
}

Result<Bytes> write_fodt(const AnyIR& value, WriteContext& ctx) {
  OdtWriter writer(value.get<ir::Document>(), ctx, true);
  return writer.write();
}

// --------------------------------------------------------------------------- ODP writer

Result<Bytes> write_odp(const AnyIR& value, WriteContext& ctx) {
  const ir::Document& doc = value.get<ir::Document>();
  const std::vector<Slide> slides = split_slides(doc, ctx.options);
  OdtWriter text(doc, ctx, false);
  text.set_presentation(true);
  std::string pages;
  const bool notes = ctx.options.get_bool("include_notes", true);
  for (std::size_t i = 0; i < slides.size(); ++i) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(std::max<std::size_t>(slides.size(), 1)), "Writing slides"));
    const Slide& slide = slides[i];
    pages += "<draw:page draw:name=\"page" + std::to_string(i + 1) +
             "\" draw:style-name=\"dp1\" draw:master-page-name=\"Default\" presentation:presentation-page-layout-name=\"AL1T1\">";
    pages += "<draw:frame presentation:style-name=\"pr1\" draw:layer=\"layout\" svg:width=\"25.199cm\" svg:height=\"2.629cm\" "
             "svg:x=\"1.4cm\" svg:y=\"0.837cm\" presentation:class=\"title\"><draw:text-box><text:p>";
    text.inlines(slide.title, pages, 1);
    pages += "</text:p></draw:text-box></draw:frame>";
    std::string body;
    std::vector<const ir::ImageBlock*> pictures;
    std::vector<const ir::TableBlock*> tables;
    ir::Blocks text_blocks;
    for (const ir::Block& b : slide.body) {
      if (const auto* t = b.get_if<ir::TableBlock>()) {
        tables.push_back(t);
      } else if (const auto* picture = b.get_if<ir::ImageBlock>()) {
        pictures.push_back(picture);
      } else {
        text_blocks.push_back(b);
      }
    }
    const double body_width = pictures.empty() && tables.empty() ? 25.199 : 14.0;
    if (!text_blocks.empty()) {
      text.blocks(text_blocks, body, 0, 1);
      for (const std::string& label : text.take_pending_notes()) {
        for (const ir::Footnote& note : doc.footnotes) {
          if (note.label != label) continue;
          body += "<text:p text:style-name=\"Footnote\">" + esc(label) + ". " +
                  esc(util::trim(ir::plain_text(note.blocks))) + "</text:p>";
        }
      }
      pages += "<draw:frame presentation:style-name=\"pr2\" draw:layer=\"layout\" svg:width=\"" + fmt_number(body_width) +
               "cm\" svg:height=\"10.5cm\" svg:x=\"1.4cm\" svg:y=\"3.9cm\" presentation:class=\"outline\"><draw:text-box>" +
               body + "</draw:text-box></draw:frame>";
    }
    double y = 3.9;
    for (const ir::TableBlock* t : tables) {
      // Impress shows tables only as frames of their own.
      const document::TableGrid grid = document::table_grid(*t);
      const double x = text_blocks.empty() ? 1.4 : 16.0;
      const double w = text_blocks.empty() ? 25.199 : 10.6;
      const double h = 0.8 * static_cast<double>(std::max<std::size_t>(1, grid.cells.size()));
      pages += "<draw:frame draw:style-name=\"fr1\" draw:layer=\"layout\" svg:width=\"" + fmt_number(w) + "cm\" svg:height=\"" +
               fmt_number(h) + "cm\" svg:x=\"" + fmt_number(x) + "cm\" svg:y=\"" + fmt_number(y) + "cm\">" + text.table_xml(*t) +
               "</draw:frame>";
      y += h + 0.5;
    }
    for (const ir::ImageBlock* picture : pictures) {
      std::string frame = text.image_frame(picture->source, picture->alt, false, pictures.size() == 1 ? 10.0 : 8.0);
      if (frame.empty()) continue;
      // Place the picture on the right of the text (or centred without text).
      const std::string anchor = "text:anchor-type=\"paragraph\"";
      if (const std::size_t at = frame.find(anchor); at != std::string::npos) frame.erase(at, anchor.size());
      const std::string pos = " svg:x=\"" + fmt_number(text_blocks.empty() ? 8.0 : 16.0) + "cm\" svg:y=\"" + fmt_number(y) + "cm\"";
      frame.insert(frame.find(' '), pos);
      pages += frame;
      y += 5.5;
    }
    if (notes && !slide.notes.empty()) {
      std::string note_xml;
      text.blocks(slide.notes, note_xml, 0, 1);
      pages += "<presentation:notes draw:style-name=\"dp1\"><draw:frame presentation:style-name=\"pr3\" draw:layer=\"layout\" "
               "svg:width=\"17.271cm\" svg:height=\"12.322cm\" svg:x=\"2.159cm\" svg:y=\"14.107cm\" "
               "presentation:class=\"notes\"><draw:text-box>" + note_xml + "</draw:text-box></draw:frame></presentation:notes>";
    }
    pages += "</draw:page>";
  }
  std::string language = ctx.options.get_string("language", "");
  if (language.empty()) language = doc.metadata.language;
  const bool strip = options::resolved_strip_metadata(ctx.options);
  const std::string content =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-content" + std::string(odf_namespaces) +
      " office:version=\"1.3\"><office:automatic-styles>"
      "<style:style style:name=\"dp1\" style:family=\"drawing-page\"/>"
      "<style:style style:name=\"pr1\" style:family=\"presentation\" style:parent-style-name=\"Default-title\"/>"
      "<style:style style:name=\"pr2\" style:family=\"presentation\" style:parent-style-name=\"Default-outline1\"/>"
      "<style:style style:name=\"pr3\" style:family=\"presentation\" style:parent-style-name=\"Default-notes\"/>"
      "<style:style style:name=\"fr1\" style:family=\"graphic\"/>"
      "<style:style style:name=\"Tbl\" style:family=\"table\"/><style:style style:name=\"TblCol\" style:family=\"table-column\"/>"
      "<style:style style:name=\"TblCell\" style:family=\"table-cell\"/>"
      "<style:style style:name=\"PageBreak\" style:family=\"paragraph\"/>" +
      text.automatic_styles() + "</office:automatic-styles><office:body><office:presentation>" + pages +
      "</office:presentation></office:body></office:document-content>\n";
  const std::string styles =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-styles" + std::string(odf_namespaces) +
      " office:version=\"1.3\"><office:styles>"
      "<style:default-style style:family=\"graphic\"><style:text-properties fo:font-size=\"18pt\"" +
      (language.empty() ? std::string() : " fo:language=\"" + esc_attr(language.substr(0, language.find('-'))) + "\"") +
      "/></style:default-style>" + common_text_styles(language) +
      "</office:styles><office:automatic-styles>"
      "<style:page-layout style:name=\"PM1\"><style:page-layout-properties fo:margin-top=\"0cm\" fo:margin-bottom=\"0cm\" "
      "fo:margin-left=\"0cm\" fo:margin-right=\"0cm\" fo:page-width=\"28cm\" fo:page-height=\"15.75cm\" "
      "style:print-orientation=\"landscape\"/></style:page-layout>"
      "<style:style style:name=\"Mdp1\" style:family=\"drawing-page\"><style:drawing-page-properties draw:fill=\"solid\" "
      "draw:fill-color=\"#ffffff\"/></style:style>"
      "</office:automatic-styles><office:master-styles><draw:layer-set><draw:layer draw:name=\"layout\"/>"
      "<draw:layer draw:name=\"background\"/><draw:layer draw:name=\"backgroundobjects\"/></draw:layer-set>"
      "<style:master-page style:name=\"Default\" style:page-layout-name=\"PM1\" draw:style-name=\"Mdp1\"/>"
      "</office:master-styles></office:document-styles>\n";
  std::string meta_inner;
  if (!doc.metadata.title.empty()) meta_inner += "<dc:title>" + esc(doc.metadata.title) + "</dc:title>";
  if (!language.empty()) meta_inner += "<dc:language>" + esc(language) + "</dc:language>";
  if (!strip && !doc.metadata.author.empty()) meta_inner += "<meta:initial-creator>" + esc(doc.metadata.author) + "</meta:initial-creator>";
  const std::string meta = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-meta" + std::string(odf_namespaces) +
                           " office:version=\"1.3\"><office:meta>" + meta_inner + "</office:meta></office:document-meta>\n";
  std::string manifest =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<manifest:manifest "
      "xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.3\">"
      "<manifest:file-entry manifest:full-path=\"/\" manifest:version=\"1.3\" "
      "manifest:media-type=\"application/vnd.oasis.opendocument.presentation\"/>"
      "<manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>"
      "<manifest:file-entry manifest:full-path=\"styles.xml\" manifest:media-type=\"text/xml\"/>"
      "<manifest:file-entry manifest:full-path=\"meta.xml\" manifest:media-type=\"text/xml\"/>";
  for (const auto& item : text.media().items()) {
    manifest += "<manifest:file-entry manifest:full-path=\"Pictures/" + item.name + "\" manifest:media-type=\"" +
                item.image.mime + "\"/>";
  }
  manifest += "</manifest:manifest>\n";
  int level = static_cast<int>(ctx.options.get_int("compression_level", 9, 0, 9));
  if (!ctx.options.was_set("compression_level")) level = 9;
  util::ZipEntryOptions stored;
  stored.level = 0;
  util::ZipEntryOptions entry;
  entry.level = level;
  util::ZipWriter zip;
  CVT_TRY(zip.add("mimetype", "application/vnd.oasis.opendocument.presentation", stored));
  CVT_TRY(zip.add("content.xml", content, entry));
  CVT_TRY(zip.add("styles.xml", styles, entry));
  CVT_TRY(zip.add("meta.xml", meta, entry));
  for (const auto& item : text.media().items()) CVT_TRY(zip.add("Pictures/" + item.name, item.image.bytes, stored));
  CVT_TRY(zip.add("META-INF/manifest.xml", manifest, entry));
  return zip.finish();
}

// --------------------------------------------------------------------------- registration

int sniff_flat(ByteView data, std::string_view mime) {
  // The namespace declarations of LibreOffice push the mimetype attribute far down.
  std::string_view head = as_chars(data).substr(0, 65536);
  if (head.starts_with("\xEF\xBB\xBF")) head.remove_prefix(3);
  if (!util::trim_left(head).starts_with("<")) return 0;
  const std::size_t root = head.find("<office:document ");
  if (root == std::string_view::npos) return 0;
  const std::size_t end = head.find('>', root);
  const std::string_view tag = head.substr(root, end == std::string_view::npos ? std::string_view::npos : end - root);
  return tag.find("office:mimetype=\"" + std::string(mime) + "\"") != std::string_view::npos ? 96 : 0;
}

void add_odf(Registry& registry, std::string id, std::string name, std::string description, std::string mime,
             Category category, int popularity, WriteFn writer, bool flat, std::vector<OptionSpec> write_options) {
  FormatInfo info;
  info.id = id;
  info.name = std::move(name);
  info.description = std::move(description);
  info.category = category;
  info.extensions = {id};
  info.mime = mime;
  info.popularity = popularity;
  if (flat) {
    info.sniff = [mime](ByteView data, std::string_view) { return sniff_flat(data, mime); };
  } else {
    info.sniff = [mime](ByteView data, std::string_view) {
      const std::string found = leading_mimetype(data);
      if (found == mime) return 96;
      // Writer/Web documents are text documents too.
      return mime == "application/vnd.oasis.opendocument.text" && found == mime + "-web" ? 90 : 0;
    };
  }
  registry.add_format(std::move(info));
  Codec reader;
  reader.format_id = id;
  reader.kind = Kind::Document;
  reader.read = read_odf;
  reader.read_options = document_read_options();
  registry.add_codec(std::move(reader));
  if (writer) {
    Codec codec;
    codec.format_id = id;
    codec.kind = Kind::Document;
    codec.write = std::move(writer);
    codec.write_options = std::move(write_options);
    registry.add_codec(std::move(codec));
  }
}

}  // namespace

void register_odf(Registry& registry) {
  add_odf(registry, "odt", "OpenDocument text", "Word-processing document of LibreOffice and OpenOffice.",
          "application/vnd.oasis.opendocument.text", Category::Document, 60, write_odt, false,
          document_write_options(true));
  add_odf(registry, "ott", "OpenDocument text template", "Template for OpenDocument text documents.",
          "application/vnd.oasis.opendocument.text-template", Category::Document, 15, nullptr, false, {});
  add_odf(registry, "fodt", "Flat OpenDocument text", "OpenDocument text as a single XML file.",
          "application/vnd.oasis.opendocument.text", Category::Document, 15, write_fodt, true,
          document_write_options(true));
  add_odf(registry, "odp", "OpenDocument presentation", "Slide presentation of LibreOffice Impress and OpenOffice.",
          "application/vnd.oasis.opendocument.presentation", Category::Presentation, 40, write_odp, false,
          slide_write_options());
  add_odf(registry, "otp", "OpenDocument presentation template", "Template for OpenDocument presentations.",
          "application/vnd.oasis.opendocument.presentation-template", Category::Presentation, 10, nullptr, false, {});
  add_odf(registry, "fodp", "Flat OpenDocument presentation", "OpenDocument presentation as a single XML file.",
          "application/vnd.oasis.opendocument.presentation", Category::Presentation, 10, nullptr, true, {});
}

}  // namespace cvt::office
