// Convertible core - office family: DOCX writer.
//
// A clean WordprocessingML package: real styles for headings, code, quotes,
// captions, footnotes and tables; numbering definitions for every list
// (ordered lists restart); tables with repeating header rows and merged
// cells; pictures embedded with their true aspect ratio; external and
// internal hyperlinks; footnotes; page size and margins from the options;
// core properties limited to the title unless metadata is kept.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "engine/standard_options.hpp"
#include "families/document/document_api.hpp"
#include "families/office/office.hpp"
#include "families/office/office_common.hpp"
#include "util/base64.hpp"
#include "util/text.hpp"
#include "util/zip.hpp"

namespace cvt::office {

namespace {

constexpr std::string_view ns_w = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
constexpr std::string_view ns_r = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
constexpr std::string_view rel_base = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";

std::string root_namespaces() {
  return std::string(" xmlns:w=\"") + std::string(ns_w) + "\" xmlns:r=\"" + std::string(ns_r) +
         "\" xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\""
         " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
         " xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"";
}

struct PartRels {
  std::vector<std::string> xml;
  int next = 1;
  std::string add(std::string_view type, std::string_view target, bool external) {
    const std::string id = "rId" + std::to_string(next++);
    std::string line = "<Relationship Id=\"" + id + "\" Type=\"" + std::string(rel_base) + std::string(type) +
                       "\" Target=\"" + esc_attr(target) + "\"";
    if (external) line += " TargetMode=\"External\"";
    line += "/>";
    xml.push_back(std::move(line));
    return id;
  }
  [[nodiscard]] std::string serialize() const {
    std::string out =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
    for (const std::string& line : xml) out += line;
    out += "</Relationships>";
    return out;
  }
};

struct RunStyle {
  bool bold = false, italic = false, underline = false, strike = false, sup = false, sub = false, code = false;
  bool link = false;
};

class DocxWriter {
 public:
  DocxWriter(const ir::Document& doc, WriteContext& ctx, bool word2003 = false)
      : doc_(doc), ctx_(ctx), w2003_(word2003) {
    policy_ = image_policy(ctx.options);
    page_ = page_setup(ctx.options);
  }
  Result<Bytes> write();
  Result<Bytes> write_word2003();

 private:
  struct Target {
    std::string* out;
    PartRels* rels;
    std::string media_prefix;  // relative path from the part to word/media
  };
  void blocks(const ir::Blocks& blocks, Target& t, int quote, int list_level, int depth);
  void block(const ir::Block& block, Target& t, int quote, int list_level, int depth);
  void paragraph_open(Target& t, std::string_view style, int quote, std::string_view extra_ppr = {});
  void inlines(const ir::Inlines& inlines, Target& t, RunStyle style, int depth);
  void text_run(std::string_view text, Target& t, const RunStyle& style);
  void image_run(const ir::ImageSource& source, std::string_view alt, Target& t, bool block);
  void list(const ir::List& list, Target& t, int quote, int level, int depth);
  void table(const ir::TableBlock& table, Target& t, int quote, int depth);
  std::string bookmark_name(std::string_view id);
  int ordered_num(const ir::List& list, int level);
  [[nodiscard]] std::string styles_xml() const;
  [[nodiscard]] std::string numbering_xml() const;
  [[nodiscard]] std::string footnotes_xml(PartRels& rels);
  [[nodiscard]] std::string section_xml() const;

  const ir::Document& doc_;
  WriteContext& ctx_;
  bool w2003_ = false;
  ImagePolicy policy_;
  PageSetup page_;
  MediaStore media_;
  std::map<std::string, int> footnote_ids_;
  std::map<std::string, std::string> bookmarks_;
  std::set<std::string> used_bookmarks_;
  struct OrderedDef {
    int level;
    ir::ListNumbering numbering;
    int start;
  };
  std::vector<OrderedDef> ordered_;  // numId 2.. ; numId 1 = bullets
  int bookmark_counter_ = 0;
  int drawing_id_ = 0;
  bool used_bullets_ = false;
  int dropped_images_ = 0;
};

std::string DocxWriter::bookmark_name(std::string_view id) {
  if (auto it = bookmarks_.find(std::string(id)); it != bookmarks_.end()) return it->second;
  std::string name;
  for (char c : id) {
    const bool ok = util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '_';
    name.push_back(ok ? c : '_');
  }
  if (name.empty() || !util::is_ascii_alpha(name.front())) name = "h" + name;
  if (name.size() > 36) name.resize(36);
  std::string unique = name;
  for (int n = 1; used_bookmarks_.contains(unique); ++n) unique = name.substr(0, 32) + std::to_string(n);
  used_bookmarks_.insert(unique);
  bookmarks_[std::string(id)] = unique;
  return unique;
}

int DocxWriter::ordered_num(const ir::List& list, int level) {
  ordered_.push_back(OrderedDef{level, list.numbering, list.start});
  return static_cast<int>(ordered_.size()) + 1;
}

void DocxWriter::text_run(std::string_view text, Target& t, const RunStyle& style) {
  if (text.empty()) return;
  std::string rpr;
  if (style.link) rpr += "<w:rStyle w:val=\"Hyperlink\"/>";
  else if (style.code) rpr += "<w:rStyle w:val=\"VerbatimChar\"/>";
  if (style.bold) rpr += "<w:b/><w:bCs/>";
  if (style.italic) rpr += "<w:i/><w:iCs/>";
  if (style.strike) rpr += "<w:strike/>";
  if (style.underline) rpr += "<w:u w:val=\"single\"/>";
  if (style.sup) rpr += "<w:vertAlign w:val=\"superscript\"/>";
  else if (style.sub) rpr += "<w:vertAlign w:val=\"subscript\"/>";
  std::string& out = *t.out;
  out += "<w:r>";
  if (!rpr.empty()) out += "<w:rPr>" + rpr + "</w:rPr>";
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t stop = text.find_first_of("\t\n", start);
    const std::string_view piece = text.substr(start, stop == std::string_view::npos ? std::string_view::npos : stop - start);
    if (!piece.empty()) out += "<w:t xml:space=\"preserve\">" + esc(piece) + "</w:t>";
    if (stop == std::string_view::npos) break;
    out += text[stop] == '\t' ? "<w:tab/>" : "<w:t xml:space=\"preserve\"> </w:t>";
    start = stop + 1;
  }
  out += "</w:r>";
}

void DocxWriter::image_run(const ir::ImageSource& source, std::string_view alt, Target& t, bool block) {
  auto prepared = prepare_image(source, policy_, ctx_);
  if (!prepared) {
    if (policy_.mode != ImageMode::Drop) {
      if (const auto* encoded = std::get_if<ir::EncodedImage>(&source); encoded && !encoded->url.empty()) {
        // An external picture: a link to it.
        RunStyle link;
        link.link = true;
        if (w2003_) {
          *t.out += "<w:hlink w:dest=\"" + esc_attr(encoded->url) + "\">";
          text_run(alt.empty() ? std::string_view(encoded->url) : alt, t, link);
          *t.out += "</w:hlink>";
          return;
        }
        const std::string id = t.rels->add("hyperlink", encoded->url, true);
        *t.out += "<w:hyperlink r:id=\"" + id + "\">";
        text_run(alt.empty() ? std::string_view(encoded->url) : alt, t, link);
        *t.out += "</w:hyperlink>";
        return;
      }
    } else {
      ++dropped_images_;
    }
    if (!alt.empty()) text_run(alt, t, RunStyle{});
    return;
  }
  if (!is_web_safe(prepared->extension)) {
    // Formats Word cannot show (WebP, SVG ...): PNG through the registry when possible.
    auto png = convert_to_png(*prepared, ctx_);
    if (!png) {
      if (!alt.empty()) text_run(alt, t, RunStyle{});
      ctx_.warn("A picture in a format Word cannot display was left out.");
      return;
    }
    prepared = std::move(png);
  }
  PixelSize px = prepared->size;
  if (px.width == 0 || px.height == 0) px = PixelSize{300, 200};
  if (w2003_) {
    const std::size_t index = media_.add(std::move(*prepared));
    const auto& item = media_.items()[index];
    double w = px.width * 0.75;
    double h = px.height * 0.75;
    const double max_w = (page_.width - 2 * page_.margin) / 20.0;
    if (w > max_w) {
      h *= max_w / w;
      w = max_w;
    }
    const std::string src = "wordml://" + item.name;
    *t.out += "<w:r><w:pict><w:binData w:name=\"" + src + "\">" + util::base64_encode(item.image.bytes) +
              "</w:binData><v:shape id=\"_x0000_i" + std::to_string(++drawing_id_) +
              "\" type=\"#_x0000_t75\" style=\"width:" + fmt_number(w) + "pt;height:" + fmt_number(h) +
              "pt\"><v:imagedata src=\"" + src + "\" o:title=\"" + esc_attr(alt) + "\"/></v:shape></w:pict></w:r>";
    return;
  }
  const std::size_t index = media_.add(std::move(*prepared));
  const std::string& name = media_.items()[index].name;
  const std::string rid = t.rels->add("image", t.media_prefix + "media/" + name, false);
  // 96 dpi; fit into the text area.
  double cx = static_cast<double>(px.width) * 9525.0;
  double cy = static_cast<double>(px.height) * 9525.0;
  const double max_cx = static_cast<double>(page_.width - 2 * page_.margin) * 635.0;
  const double max_cy = static_cast<double>(page_.height - 2 * page_.margin) * 635.0 * 0.9;
  const double scale = std::min({1.0, max_cx / cx, max_cy / cy});
  const auto ecx = static_cast<std::int64_t>(std::max(1.0, std::round(cx * scale)));
  const auto ecy = static_cast<std::int64_t>(std::max(1.0, std::round(cy * scale)));
  const int id = ++drawing_id_;
  const std::string extent = "cx=\"" + std::to_string(ecx) + "\" cy=\"" + std::to_string(ecy) + "\"";
  std::string& out = *t.out;
  (void)block;
  out += "<w:r><w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\"><wp:extent " + extent +
         "/><wp:effectExtent l=\"0\" t=\"0\" r=\"0\" b=\"0\"/><wp:docPr id=\"" + std::to_string(id) +
         "\" name=\"Picture " + std::to_string(id) + "\" descr=\"" + esc_attr(alt) +
         "\"/><wp:cNvGraphicFramePr><a:graphicFrameLocks noChangeAspect=\"1\"/></wp:cNvGraphicFramePr>"
         "<a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"><pic:pic>"
         "<pic:nvPicPr><pic:cNvPr id=\"" +
         std::to_string(id) + "\" name=\"" + esc_attr(name) + "\" descr=\"" + esc_attr(alt) +
         "\"/><pic:cNvPicPr><a:picLocks noChangeAspect=\"1\" noChangeArrowheads=\"1\"/></pic:cNvPicPr></pic:nvPicPr>"
         "<pic:blipFill><a:blip r:embed=\"" +
         rid + "\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill><pic:spPr bwMode=\"auto\"><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext " +
         extent + "/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr></pic:pic></a:graphicData></a:graphic>"
                  "</wp:inline></w:drawing></w:r>";
}

void DocxWriter::inlines(const ir::Inlines& items, Target& t, RunStyle style, int depth) {
  if (depth > ir::max_document_depth) return;
  for (const ir::Inline& item : items) {
    std::visit(
        [&](const auto& node) {
          using T = std::remove_cvref_t<decltype(node)>;
          if constexpr (std::same_as<T, ir::Text>) {
            text_run(node.text, t, style);
          } else if constexpr (std::same_as<T, ir::Code>) {
            RunStyle s = style;
            s.code = true;
            text_run(node.text, t, s);
          } else if constexpr (std::same_as<T, ir::Emphasis>) {
            RunStyle s = style;
            s.italic = true;
            inlines(node.children, t, s, depth + 1);
          } else if constexpr (std::same_as<T, ir::Strong>) {
            RunStyle s = style;
            s.bold = true;
            inlines(node.children, t, s, depth + 1);
          } else if constexpr (std::same_as<T, ir::Underline>) {
            RunStyle s = style;
            s.underline = true;
            inlines(node.children, t, s, depth + 1);
          } else if constexpr (std::same_as<T, ir::Strikethrough>) {
            RunStyle s = style;
            s.strike = true;
            inlines(node.children, t, s, depth + 1);
          } else if constexpr (std::same_as<T, ir::Superscript>) {
            RunStyle s = style;
            s.sup = true;
            inlines(node.children, t, s, depth + 1);
          } else if constexpr (std::same_as<T, ir::Subscript>) {
            RunStyle s = style;
            s.sub = true;
            inlines(node.children, t, s, depth + 1);
          } else if constexpr (std::same_as<T, ir::LineBreak>) {
            *t.out += "<w:r><w:br/></w:r>";
          } else if constexpr (std::same_as<T, ir::InlineImage>) {
            image_run(node.source, node.alt, t, false);
          } else if constexpr (std::same_as<T, ir::Link>) {
            if (!node.note.empty() && w2003_) {
              for (const ir::Footnote& note : doc_.footnotes) {
                if (note.label != node.note || depth > 8) continue;
                *t.out += "<w:r><w:rPr><w:rStyle w:val=\"FootnoteReference\"/></w:rPr><w:footnote>";
                bool any = false;
                for (const ir::Block& b : note.blocks) {
                  if (const auto* para = b.get_if<ir::Paragraph>()) {
                    *t.out += "<w:p><w:pPr><w:pStyle w:val=\"FootnoteText\"/></w:pPr>";
                    inlines(para->content, t, RunStyle{}, depth + 4);
                    *t.out += "</w:p>";
                    any = true;
                  }
                }
                if (!any) *t.out += "<w:p/>";
                *t.out += "</w:footnote></w:r>";
                return;
              }
            }
            if (!node.note.empty()) {
              if (auto it = footnote_ids_.find(node.note); it != footnote_ids_.end()) {
                *t.out += "<w:r><w:rPr><w:rStyle w:val=\"FootnoteReference\"/></w:rPr><w:footnoteReference w:id=\"" +
                          std::to_string(it->second) + "\"/></w:r>";
                return;
              }
            }
            if (style.link || node.url.empty()) {
              inlines(node.children, t, style, depth + 1);
              return;
            }
            RunStyle s = style;
            s.link = true;
            if (w2003_) {
              if (node.url.starts_with('#')) {
                *t.out += "<w:hlink w:bookmark=\"" + esc_attr(bookmark_name(std::string_view(node.url).substr(1))) + "\">";
              } else {
                *t.out += "<w:hlink w:dest=\"" + esc_attr(node.url) + "\">";
              }
              inlines(node.children, t, s, depth + 1);
              *t.out += "</w:hlink>";
              return;
            }
            if (node.url.starts_with('#')) {
              *t.out += "<w:hyperlink w:anchor=\"" + esc_attr(bookmark_name(std::string_view(node.url).substr(1))) + "\">";
            } else {
              const std::string id = t.rels->add("hyperlink", node.url, true);
              *t.out += "<w:hyperlink r:id=\"" + id + "\">";
            }
            inlines(node.children, t, s, depth + 1);
            *t.out += "</w:hyperlink>";
          }
        },
        item.node);
  }
}

void DocxWriter::paragraph_open(Target& t, std::string_view style, int quote, std::string_view extra_ppr) {
  std::string& out = *t.out;
  out += "<w:p>";
  std::string ppr;
  if (!style.empty()) ppr += "<w:pStyle w:val=\"" + std::string(style) + "\"/>";
  ppr += extra_ppr;
  if (quote > 0 && extra_ppr.find("<w:ind") == std::string_view::npos && style != "Quote") {
    ppr += "<w:ind w:left=\"" + std::to_string(720 * quote) + "\"/>";
  }
  if (!ppr.empty()) out += "<w:pPr>" + ppr + "</w:pPr>";
}

void DocxWriter::list(const ir::List& list, Target& t, int quote, int level, int depth) {
  const int lvl = std::clamp(level, 0, 8);
  int num_id = 1;
  if (list.ordered) {
    num_id = ordered_num(list, lvl);
  } else {
    used_bullets_ = true;
  }
  for (std::size_t i = 0; i < list.items.size(); ++i) {
    const ir::Blocks& item = list.items[i];
    bool first = true;
    const std::string numpr =
        w2003_ ? "<w:listPr><w:ilvl w:val=\"" + std::to_string(lvl) + "\"/><w:ilfo w:val=\"" + std::to_string(num_id) +
                     "\"/></w:listPr>"
               : "<w:numPr><w:ilvl w:val=\"" + std::to_string(lvl) + "\"/><w:numId w:val=\"" + std::to_string(num_id) +
                     "\"/></w:numPr>";
    const std::string indent = "<w:ind w:left=\"" + std::to_string(720 * (lvl + 1) + 720 * quote) + "\"/>";
    std::string marker;
    if (list.task(i) == ir::TaskState::Open) marker = "\xE2\x98\x90 ";
    if (list.task(i) == ir::TaskState::Done) marker = "\xE2\x98\x92 ";
    for (std::size_t b = 0; b < item.size(); ++b) {
      const ir::Block& block_item = item[b];
      const auto* para = block_item.get_if<ir::Paragraph>();
      if (para || block_item.is<ir::Heading>()) {
        const ir::Inlines& content = para ? para->content : block_item.get_if<ir::Heading>()->content;
        const bool term = list.definition && b == 0;
        if (list.definition) {
          paragraph_open(t, "ListParagraph", 0, term ? std::string() : indent);
        } else {
          paragraph_open(t, "ListParagraph", 0, first ? numpr : indent);
        }
        if (first && !marker.empty()) text_run(marker, t, RunStyle{});
        RunStyle style;
        style.bold = term;
        inlines(content, t, style, depth + 1);
        *t.out += "</w:p>";
        first = false;
      } else if (const auto* nested = block_item.get_if<ir::List>()) {
        if (first) {
          paragraph_open(t, "ListParagraph", 0, numpr);
          *t.out += "</w:p>";
          first = false;
        }
        this->list(*nested, t, quote, level + 1, depth + 1);
      } else {
        if (first) {
          paragraph_open(t, "ListParagraph", 0, numpr);
          *t.out += "</w:p>";
          first = false;
        }
        block(block_item, t, quote + level + 1, level + 1, depth + 1);
      }
    }
    if (first) {
      paragraph_open(t, "ListParagraph", 0, numpr);
      *t.out += "</w:p>";
    }
  }
}

void DocxWriter::table(const ir::TableBlock& table, Target& t, int quote, int depth) {
  const document::TableGrid grid = document::table_grid(table);
  if (grid.columns == 0) return;
  std::string& out = *t.out;
  if (!table.caption.empty()) {
    paragraph_open(t, "Caption", quote);
    inlines(table.caption, t, RunStyle{}, depth + 1);
    out += "</w:p>";
  }
  const int text_width = page_.width - 2 * page_.margin - 720 * quote;
  const int column_width = std::max(200, text_width / static_cast<int>(grid.columns));
  out += std::string("<w:tbl><w:tblPr>") +
         (w2003_ ? "<w:tblBorders><w:top w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/><w:left "
                   "w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/><w:bottom w:val=\"single\" w:sz=\"4\" "
                   "w:space=\"0\" w:color=\"auto\"/><w:right w:val=\"single\" w:sz=\"4\" w:space=\"0\" "
                   "w:color=\"auto\"/><w:insideH w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/>"
                   "<w:insideV w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/></w:tblBorders>"
                 : "<w:tblStyle w:val=\"TableGrid\"/>") +
         "<w:tblW w:w=\"" +
         std::to_string(column_width * static_cast<int>(grid.columns)) + "\" w:type=\"dxa\"/>";
  if (quote > 0) out += "<w:tblInd w:w=\"" + std::to_string(720 * quote) + "\" w:type=\"dxa\"/>";
  if (!w2003_) out += "<w:tblLook w:val=\"04A0\" w:firstRow=\"1\" w:lastRow=\"0\" w:firstColumn=\"0\" w:lastColumn=\"0\" "
         "w:noHBand=\"0\" w:noVBand=\"1\"/>";
  out += "</w:tblPr><w:tblGrid>";
  for (std::size_t c = 0; c < grid.columns; ++c) out += "<w:gridCol w:w=\"" + std::to_string(column_width) + "\"/>";
  out += "</w:tblGrid>";
  for (std::size_t r = 0; r < grid.cells.size(); ++r) {
    const bool header = r < grid.header_rows;
    out += "<w:tr>";
    if (header) out += "<w:trPr><w:tblHeader/></w:trPr>";
    std::size_t c = 0;
    while (c < grid.columns) {
      const ir::TableCell* cell = grid.cells[r][c];
      // Width of this cell in grid columns (a colspan within the row).
      std::size_t span = 1;
      while (c + span < grid.columns && grid.cells[r][c + span] == cell && cell != nullptr) ++span;
      const bool origin = grid.origin[r][c];
      const bool vertical_continue = cell != nullptr && !origin && r > 0 && grid.cells[r - 1][c] == cell;
      const bool vertical_start = cell != nullptr && origin && cell->rowspan > 1;
      out += "<w:tc><w:tcPr><w:tcW w:w=\"" + std::to_string(column_width * static_cast<int>(span)) +
             "\" w:type=\"dxa\"/>";
      if (span > 1) out += "<w:gridSpan w:val=\"" + std::to_string(span) + "\"/>";
      if (vertical_start) out += w2003_ ? "<w:vmerge w:val=\"restart\"/>" : "<w:vMerge w:val=\"restart\"/>";
      if (vertical_continue) out += w2003_ ? "<w:vmerge/>" : "<w:vMerge/>";
      out += "</w:tcPr>";
      std::string jc;
      const ir::Alignment align =
          cell && cell->align != ir::Alignment::Default ? cell->align : table.column_alignment(c);
      if (align == ir::Alignment::Center) jc = "<w:jc w:val=\"center\"/>";
      if (align == ir::Alignment::Right) jc = "<w:jc w:val=\"right\"/>";
      paragraph_open(t, "Compact", 0, jc);
      if (cell && origin) {
        RunStyle style;
        style.bold = header;
        inlines(cell->content, t, style, depth + 1);
      }
      out += "</w:p></w:tc>";
      c += span;
    }
    out += "</w:tr>";
  }
  out += "</w:tbl>";
  // Word needs a paragraph between two consecutive tables.
  out += "<w:p><w:pPr><w:pStyle w:val=\"Compact\"/></w:pPr></w:p>";
}

void DocxWriter::block(const ir::Block& b, Target& t, int quote, int list_level, int depth) {
  if (depth > ir::max_document_depth) return;
  std::string& out = *t.out;
  std::visit(
      [&](const auto& node) {
        using T = std::remove_cvref_t<decltype(node)>;
        if constexpr (std::same_as<T, ir::Paragraph>) {
          const bool caption = b.attr.has_class("caption");
          paragraph_open(t, caption ? "Caption" : (quote > 0 ? "Quote" : ""), quote,
                         quote > 1 ? "<w:ind w:left=\"" + std::to_string(720 * quote) + "\"/>" : std::string());
          std::string bookmark;
          if (!b.attr.id.empty() && !w2003_) {
            bookmark = bookmark_name(b.attr.id);
            out += "<w:bookmarkStart w:id=\"" + std::to_string(bookmark_counter_) + "\" w:name=\"" + bookmark + "\"/>";
          }
          inlines(node.content, t, RunStyle{}, depth + 1);
          if (!bookmark.empty()) out += "<w:bookmarkEnd w:id=\"" + std::to_string(bookmark_counter_++) + "\"/>";
          out += "</w:p>";
        } else if constexpr (std::same_as<T, ir::Heading>) {
          const int level = std::clamp(node.level, 1, 6);
          paragraph_open(t, "Heading" + std::to_string(level), 0);
          std::string bookmark;
          if (!b.attr.id.empty() && !w2003_) {
            bookmark = bookmark_name(b.attr.id);
            out += "<w:bookmarkStart w:id=\"" + std::to_string(bookmark_counter_) + "\" w:name=\"" + bookmark + "\"/>";
          }
          inlines(node.content, t, RunStyle{}, depth + 1);
          if (!bookmark.empty()) out += "<w:bookmarkEnd w:id=\"" + std::to_string(bookmark_counter_++) + "\"/>";
          out += "</w:p>";
        } else if constexpr (std::same_as<T, ir::List>) {
          list(node, t, quote, list_level, depth + 1);
        } else if constexpr (std::same_as<T, ir::CodeBlock>) {
          std::string_view code = node.code;
          while (code.ends_with('\n')) code.remove_suffix(1);
          for (std::string_view line : util::split(code, '\n')) {
            if (line.ends_with('\r')) line.remove_suffix(1);
            paragraph_open(t, "SourceCode", 0,
                           quote > 0 ? "<w:ind w:left=\"" + std::to_string(720 * quote) + "\"/>" : std::string());
            text_run(line, t, RunStyle{});
            out += "</w:p>";
          }
        } else if constexpr (std::same_as<T, ir::BlockQuote>) {
          blocks(node.blocks, t, quote + 1, list_level, depth + 1);
        } else if constexpr (std::same_as<T, ir::TableBlock>) {
          table(node, t, quote, depth + 1);
        } else if constexpr (std::same_as<T, ir::ImageBlock>) {
          paragraph_open(t, "Figure", quote);
          image_run(node.source, node.alt, t, true);
          out += "</w:p>";
          if (!node.caption.empty()) {
            paragraph_open(t, "Caption", quote);
            text_run(node.caption, t, RunStyle{});
            out += "</w:p>";
          }
        } else if constexpr (std::same_as<T, ir::HorizontalRule>) {
          out += "<w:p><w:pPr><w:pBdr><w:bottom w:val=\"single\" w:sz=\"6\" w:space=\"1\" w:color=\"auto\"/></w:pBdr>"
                 "</w:pPr></w:p>";
        } else if constexpr (std::same_as<T, ir::PageBreak>) {
          out += "<w:p><w:r><w:br w:type=\"page\"/></w:r></w:p>";
        }
      },
      b.node);
}

void DocxWriter::blocks(const ir::Blocks& items, Target& t, int quote, int list_level, int depth) {
  for (const ir::Block& b : items) block(b, t, quote, list_level, depth);
}

std::string DocxWriter::styles_xml() const {
  std::string lang = ctx_.options.get_string("language", "");
  if (lang.empty()) lang = doc_.metadata.language;
  if (lang.empty()) lang = "en-US";
  std::string out =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:styles" + root_namespaces() +
      "><w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:asciiTheme=\"minorHAnsi\" w:eastAsiaTheme=\"minorEastAsia\" "
      "w:hAnsiTheme=\"minorHAnsi\" w:cstheme=\"minorBidi\"/><w:sz w:val=\"22\"/><w:szCs w:val=\"22\"/><w:lang w:val=\"" +
      esc_attr(lang) +
      "\" w:eastAsia=\"en-US\" w:bidi=\"ar-SA\"/></w:rPr></w:rPrDefault><w:pPrDefault><w:pPr><w:spacing w:after=\"160\" "
      "w:line=\"264\" w:lineRule=\"auto\"/></w:pPr></w:pPrDefault></w:docDefaults>"
      "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/><w:qFormat/></w:style>"
      "<w:style w:type=\"character\" w:default=\"1\" w:styleId=\"DefaultParagraphFont\"><w:name w:val=\"Default Paragraph Font\"/>"
      "<w:uiPriority w:val=\"1\"/><w:semiHidden/><w:unhideWhenUsed/></w:style>"
      "<w:style w:type=\"table\" w:default=\"1\" w:styleId=\"TableNormal\"><w:name w:val=\"Normal Table\"/><w:semiHidden/>"
      "<w:unhideWhenUsed/><w:tblPr><w:tblInd w:w=\"0\" w:type=\"dxa\"/><w:tblCellMar><w:top w:w=\"0\" w:type=\"dxa\"/>"
      "<w:left w:w=\"108\" w:type=\"dxa\"/><w:bottom w:w=\"0\" w:type=\"dxa\"/><w:right w:w=\"108\" w:type=\"dxa\"/>"
      "</w:tblCellMar></w:tblPr></w:style>"
      "<w:style w:type=\"numbering\" w:default=\"1\" w:styleId=\"NoList\"><w:name w:val=\"No List\"/><w:semiHidden/>"
      "<w:unhideWhenUsed/></w:style>";
  static constexpr int sizes[6] = {36, 30, 26, 24, 22, 22};
  for (int level = 1; level <= 6; ++level) {
    out += "<w:style w:type=\"paragraph\" w:styleId=\"Heading" + std::to_string(level) + "\"><w:name w:val=\"heading " +
           std::to_string(level) +
           "\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:uiPriority w:val=\"9\"/><w:qFormat/><w:pPr>"
           "<w:keepNext/><w:keepLines/><w:spacing w:before=\"" +
           std::to_string(level <= 2 ? 360 : 240) + "\" w:after=\"120\"/><w:outlineLvl w:val=\"" +
           std::to_string(level - 1) + "\"/></w:pPr><w:rPr><w:b/><w:bCs/>" + (level >= 5 ? "<w:i/>" : "") +
           "<w:sz w:val=\"" + std::to_string(sizes[level - 1]) + "\"/><w:szCs w:val=\"" +
           std::to_string(sizes[level - 1]) + "\"/></w:rPr></w:style>";
  }
  out +=
      "<w:style w:type=\"paragraph\" w:styleId=\"Title\"><w:name w:val=\"Title\"/><w:basedOn w:val=\"Normal\"/>"
      "<w:next w:val=\"Normal\"/><w:qFormat/><w:pPr><w:spacing w:after=\"240\"/></w:pPr><w:rPr><w:sz w:val=\"52\"/>"
      "</w:rPr></w:style>"
      "<w:style w:type=\"paragraph\" w:styleId=\"Quote\"><w:name w:val=\"Quote\"/><w:basedOn w:val=\"Normal\"/>"
      "<w:next w:val=\"Normal\"/><w:qFormat/><w:pPr><w:ind w:left=\"720\" w:right=\"720\"/></w:pPr><w:rPr><w:i/>"
      "<w:color w:val=\"404040\"/></w:rPr></w:style>"
      "<w:style w:type=\"paragraph\" w:styleId=\"SourceCode\"><w:name w:val=\"Source Code\"/><w:basedOn w:val=\"Normal\"/>"
      "<w:qFormat/><w:pPr><w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"F3F3F3\"/><w:spacing w:after=\"0\" "
      "w:line=\"240\" w:lineRule=\"auto\"/></w:pPr><w:rPr><w:rFonts w:ascii=\"Courier New\" w:hAnsi=\"Courier New\" "
      "w:cs=\"Courier New\"/><w:sz w:val=\"20\"/></w:rPr></w:style>"
      "<w:style w:type=\"character\" w:styleId=\"VerbatimChar\"><w:name w:val=\"Verbatim Char\"/><w:rPr><w:rFonts "
      "w:ascii=\"Courier New\" w:hAnsi=\"Courier New\" w:cs=\"Courier New\"/><w:sz w:val=\"20\"/></w:rPr></w:style>"
      "<w:style w:type=\"character\" w:styleId=\"Hyperlink\"><w:name w:val=\"Hyperlink\"/><w:uiPriority w:val=\"99\"/>"
      "<w:unhideWhenUsed/><w:rPr><w:color w:val=\"0563C1\"/><w:u w:val=\"single\"/></w:rPr></w:style>"
      "<w:style w:type=\"paragraph\" w:styleId=\"Caption\"><w:name w:val=\"caption\"/><w:basedOn w:val=\"Normal\"/>"
      "<w:next w:val=\"Normal\"/><w:qFormat/><w:pPr><w:spacing w:after=\"200\"/></w:pPr><w:rPr><w:i/>"
      "<w:sz w:val=\"18\"/></w:rPr></w:style>"
      "<w:style w:type=\"paragraph\" w:styleId=\"Figure\"><w:name w:val=\"Figure\"/><w:basedOn w:val=\"Normal\"/>"
      "<w:next w:val=\"Caption\"/><w:pPr><w:keepNext/><w:jc w:val=\"center\"/></w:pPr></w:style>"
      "<w:style w:type=\"paragraph\" w:styleId=\"Compact\"><w:name w:val=\"Compact\"/><w:basedOn w:val=\"Normal\"/>"
      "<w:pPr><w:spacing w:before=\"36\" w:after=\"36\"/></w:pPr></w:style>"
      "<w:style w:type=\"paragraph\" w:styleId=\"ListParagraph\"><w:name w:val=\"List Paragraph\"/>"
      "<w:basedOn w:val=\"Normal\"/><w:uiPriority w:val=\"34\"/><w:qFormat/><w:pPr><w:spacing w:after=\"60\"/>"
      "<w:ind w:left=\"720\"/><w:contextualSpacing/></w:pPr></w:style>"
      "<w:style w:type=\"paragraph\" w:styleId=\"FootnoteText\"><w:name w:val=\"footnote text\"/>"
      "<w:basedOn w:val=\"Normal\"/><w:uiPriority w:val=\"99\"/><w:unhideWhenUsed/><w:pPr><w:spacing w:after=\"0\" "
      "w:line=\"240\" w:lineRule=\"auto\"/></w:pPr><w:rPr><w:sz w:val=\"20\"/></w:rPr></w:style>"
      "<w:style w:type=\"character\" w:styleId=\"FootnoteReference\"><w:name w:val=\"footnote reference\"/>"
      "<w:uiPriority w:val=\"99\"/><w:unhideWhenUsed/><w:rPr><w:vertAlign w:val=\"superscript\"/></w:rPr></w:style>"
      "<w:style w:type=\"table\" w:styleId=\"TableGrid\"><w:name w:val=\"Table Grid\"/><w:basedOn w:val=\"TableNormal\"/>"
      "<w:uiPriority w:val=\"39\"/><w:pPr><w:spacing w:after=\"0\" w:line=\"240\" w:lineRule=\"auto\"/></w:pPr>"
      "<w:tblPr><w:tblBorders><w:top w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/><w:left w:val=\"single\" "
      "w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/><w:bottom w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/>"
      "<w:right w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/><w:insideH w:val=\"single\" w:sz=\"4\" "
      "w:space=\"0\" w:color=\"auto\"/><w:insideV w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/>"
      "</w:tblBorders></w:tblPr></w:style>"
      "</w:styles>";
  return out;
}

std::string DocxWriter::numbering_xml() const {
  std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:numbering" + root_namespaces() + ">";
  static const std::string_view bullets[3] = {"\xE2\x80\xA2", "\xE2\x97\xA6", "\xE2\x96\xAA"};
  const auto level_xml = [](int i, std::string_view fmt, std::string_view text, int start) {
    return "<w:lvl w:ilvl=\"" + std::to_string(i) + "\"><w:start w:val=\"" + std::to_string(start) +
           "\"/><w:numFmt w:val=\"" + std::string(fmt) + "\"/><w:lvlText w:val=\"" + std::string(text) +
           "\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"" + std::to_string(720 * (i + 1)) +
           "\" w:hanging=\"360\"/></w:pPr></w:lvl>";
  };
  out += "<w:abstractNum w:abstractNumId=\"0\"><w:multiLevelType w:val=\"hybridMultilevel\"/>";
  for (int i = 0; i < 9; ++i) out += level_xml(i, "bullet", bullets[i % 3], 1);
  out += "</w:abstractNum>";
  const auto fmt_of = [](ir::ListNumbering n) -> std::string_view {
    switch (n) {
      case ir::ListNumbering::LowerAlpha: return "lowerLetter";
      case ir::ListNumbering::UpperAlpha: return "upperLetter";
      case ir::ListNumbering::LowerRoman: return "lowerRoman";
      case ir::ListNumbering::UpperRoman: return "upperRoman";
      case ir::ListNumbering::Decimal: return "decimal";
    }
    return "decimal";
  };
  static const ir::ListNumbering cycle[3] = {ir::ListNumbering::Decimal, ir::ListNumbering::LowerAlpha,
                                             ir::ListNumbering::LowerRoman};
  for (std::size_t n = 0; n < ordered_.size(); ++n) {
    const OrderedDef& def = ordered_[n];
    out += "<w:abstractNum w:abstractNumId=\"" + std::to_string(n + 1) + "\"><w:multiLevelType w:val=\"hybridMultilevel\"/>";
    for (int i = 0; i < 9; ++i) {
      const bool mine = i == def.level;
      out += level_xml(i, fmt_of(mine ? def.numbering : cycle[i % 3]), "%" + std::to_string(i + 1) + ".",
                       mine ? std::max(0, def.start) : 1);
    }
    out += "</w:abstractNum>";
  }
  out += "<w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>";
  for (std::size_t n = 0; n < ordered_.size(); ++n) {
    out += "<w:num w:numId=\"" + std::to_string(n + 2) + "\"><w:abstractNumId w:val=\"" + std::to_string(n + 1) +
           "\"/></w:num>";
  }
  out += "</w:numbering>";
  return out;
}

std::string DocxWriter::footnotes_xml(PartRels& rels) {
  std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:footnotes" + root_namespaces() +
                    "><w:footnote w:type=\"separator\" w:id=\"-1\"><w:p><w:pPr><w:spacing w:after=\"0\" w:line=\"240\" "
                    "w:lineRule=\"auto\"/></w:pPr><w:r><w:separator/></w:r></w:p></w:footnote>"
                    "<w:footnote w:type=\"continuationSeparator\" w:id=\"0\"><w:p><w:pPr><w:spacing w:after=\"0\" "
                    "w:line=\"240\" w:lineRule=\"auto\"/></w:pPr><w:r><w:continuationSeparator/></w:r></w:p></w:footnote>";
  for (const ir::Footnote& note : doc_.footnotes) {
    const auto id = footnote_ids_.find(note.label);
    if (id == footnote_ids_.end()) continue;
    out += "<w:footnote w:id=\"" + std::to_string(id->second) + "\">";
    std::string body;
    Target t{&body, &rels, ""};
    bool first = true;
    for (const ir::Block& b : note.blocks) {
      const auto* para = b.get_if<ir::Paragraph>();
      if (!para) {
        block(b, t, 0, 0, 1);
        continue;
      }
      body += "<w:p><w:pPr><w:pStyle w:val=\"FootnoteText\"/></w:pPr>";
      if (first) {
        body += "<w:r><w:rPr><w:rStyle w:val=\"FootnoteReference\"/></w:rPr><w:footnoteRef/></w:r>"
                "<w:r><w:t xml:space=\"preserve\"> </w:t></w:r>";
        first = false;
      }
      inlines(para->content, t, RunStyle{}, 1);
      body += "</w:p>";
    }
    if (first) {
      body += "<w:p><w:pPr><w:pStyle w:val=\"FootnoteText\"/></w:pPr><w:r><w:rPr><w:rStyle "
              "w:val=\"FootnoteReference\"/></w:rPr><w:footnoteRef/></w:r></w:p>";
    }
    out += body + "</w:footnote>";
  }
  out += "</w:footnotes>";
  return out;
}

std::string DocxWriter::section_xml() const {
  const bool landscape = page_.width > page_.height;
  return "<w:sectPr><w:pgSz w:w=\"" + std::to_string(page_.width) + "\" w:h=\"" + std::to_string(page_.height) + "\"" +
         (landscape ? " w:orient=\"landscape\"" : "") + "/><w:pgMar w:top=\"" + std::to_string(page_.margin) +
         "\" w:right=\"" + std::to_string(page_.margin) + "\" w:bottom=\"" + std::to_string(page_.margin) +
         "\" w:left=\"" + std::to_string(page_.margin) +
         "\" w:header=\"708\" w:footer=\"708\" w:gutter=\"0\"/><w:cols w:space=\"708\"/></w:sectPr>";
}

Result<Bytes> DocxWriter::write() {
  // Footnote ids in order of the notes.
  int next_note = 1;
  for (const ir::Footnote& note : doc_.footnotes) {
    if (!footnote_ids_.contains(note.label)) footnote_ids_[note.label] = next_note++;
  }
  PartRels doc_rels;
  doc_rels.add("styles", "styles.xml", false);
  doc_rels.add("settings", "settings.xml", false);
  doc_rels.add("numbering", "numbering.xml", false);
  const bool has_notes = !footnote_ids_.empty();
  if (has_notes) doc_rels.add("footnotes", "footnotes.xml", false);

  std::string body;
  Target target{&body, &doc_rels, ""};
  const std::size_t total = doc_.blocks.size();
  for (std::size_t i = 0; i < total; ++i) {
    if ((i & 63) == 0) CVT_TRY(ctx_.checkpoint(static_cast<double>(i) / static_cast<double>(std::max<std::size_t>(total, 1)) * 0.8, "Writing"));
    block(doc_.blocks[i], target, 0, 0, 0);
  }
  PartRels note_rels;
  std::string footnotes;
  if (has_notes) footnotes = footnotes_xml(note_rels);
  if (dropped_images_ > 0) ctx_.warn(std::to_string(dropped_images_) + " picture(s) were left out as requested.");

  const std::string document = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:document" +
                               root_namespaces() + "><w:body>" + body + section_xml() + "</w:body></w:document>";

  std::string types =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
      "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
      "<Default Extension=\"xml\" ContentType=\"application/xml\"/>";
  std::set<std::string> extensions;
  for (const auto& item : media_.items()) extensions.insert(item.image.extension);
  for (const std::string& ext : extensions) {
    const std::string mime = ext == "jpg" ? "image/jpeg" : mime_for_extension("x." + ext);
    types += "<Default Extension=\"" + ext + "\" ContentType=\"" + mime + "\"/>";
  }
  types +=
      "<Override PartName=\"/word/document.xml\" "
      "ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
      "<Override PartName=\"/word/styles.xml\" "
      "ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
      "<Override PartName=\"/word/settings.xml\" "
      "ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.settings+xml\"/>"
      "<Override PartName=\"/word/numbering.xml\" "
      "ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml\"/>"
      "<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/>"
      "<Override PartName=\"/docProps/app.xml\" "
      "ContentType=\"application/vnd.openxmlformats-officedocument.extended-properties+xml\"/>";
  if (has_notes) {
    types +=
        "<Override PartName=\"/word/footnotes.xml\" "
        "ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.footnotes+xml\"/>";
  }
  types += "</Types>";

  const std::string package_rels =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
      "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" "
      "Target=\"word/document.xml\"/>"
      "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" "
      "Target=\"docProps/core.xml\"/>"
      "<Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties\" "
      "Target=\"docProps/app.xml\"/></Relationships>";

  const bool strip = options::resolved_strip_metadata(ctx_.options);
  std::string core =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
      "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:dcterms=\"http://purl.org/dc/terms/\" "
      "xmlns:dcmitype=\"http://purl.org/dc/dcmitype/\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">";
  if (!doc_.metadata.title.empty()) core += "<dc:title>" + esc(doc_.metadata.title) + "</dc:title>";
  std::string language = ctx_.options.get_string("language", "");
  if (language.empty()) language = doc_.metadata.language;
  if (!language.empty()) core += "<dc:language>" + esc(language) + "</dc:language>";
  if (!strip) {
    if (!doc_.metadata.author.empty()) core += "<dc:creator>" + esc(doc_.metadata.author) + "</dc:creator>";
    for (const auto& [key, value] : doc_.metadata.extra) {
      if (key == "subject" || key == "description") core += "<dc:" + key + ">" + esc(value) + "</dc:" + key + ">";
      if (key == "keywords") core += "<cp:keywords>" + esc(value) + "</cp:keywords>";
    }
    if (doc_.metadata.date.size() >= 10) {
      std::string date = doc_.metadata.date;
      if (date.size() == 10) date += "T00:00:00Z";
      core += "<dcterms:created xsi:type=\"dcterms:W3CDTF\">" + esc(date) + "</dcterms:created>";
    }
  }
  core += "</cp:coreProperties>";
  const std::string app =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/extended-properties\" "
      "xmlns:vt=\"http://schemas.openxmlformats.org/officeDocument/2006/docPropsVTypes\"><DocSecurity>0</DocSecurity>"
      "</Properties>";
  std::string settings = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:settings" + root_namespaces() +
                         "><w:defaultTabStop w:val=\"720\"/><w:characterSpacingControl w:val=\"doNotCompress\"/>";
  if (has_notes) settings += "<w:footnotePr><w:footnote w:id=\"-1\"/><w:footnote w:id=\"0\"/></w:footnotePr>";
  settings +=
      "<w:compat><w:compatSetting w:name=\"compatibilityMode\" w:uri=\"http://schemas.microsoft.com/office/word\" "
      "w:val=\"15\"/></w:compat></w:settings>";

  int level = static_cast<int>(ctx_.options.get_int("compression_level", 9, 0, 9));
  if (!ctx_.options.was_set("compression_level")) level = 9;
  util::ZipEntryOptions entry;
  entry.level = level;
  util::ZipWriter zip;
  CVT_TRY(zip.add("[Content_Types].xml", types, entry));
  CVT_TRY(zip.add("_rels/.rels", package_rels, entry));
  CVT_TRY(zip.add("docProps/core.xml", core, entry));
  CVT_TRY(zip.add("docProps/app.xml", app, entry));
  CVT_TRY(zip.add("word/document.xml", document, entry));
  CVT_TRY(zip.add("word/_rels/document.xml.rels", doc_rels.serialize(), entry));
  CVT_TRY(zip.add("word/styles.xml", styles_xml(), entry));
  CVT_TRY(zip.add("word/settings.xml", settings, entry));
  CVT_TRY(zip.add("word/numbering.xml", numbering_xml(), entry));
  if (has_notes) {
    CVT_TRY(zip.add("word/footnotes.xml", footnotes, entry));
    if (!note_rels.xml.empty()) CVT_TRY(zip.add("word/_rels/footnotes.xml.rels", note_rels.serialize(), entry));
  }
  util::ZipEntryOptions stored = entry;
  for (const auto& item : media_.items()) {
    const bool compressed = item.image.extension == "png" || item.image.extension == "jpg" || item.image.extension == "gif";
    stored.level = compressed ? 0 : level;
    CVT_TRY(zip.add("word/media/" + item.name, item.image.bytes, stored));
  }
  (void)used_bullets_;
  return zip.finish();
}

Result<Bytes> DocxWriter::write_word2003() {
  std::string body;
  PartRels unused;
  Target target{&body, &unused, ""};
  const std::size_t total = doc_.blocks.size();
  for (std::size_t i = 0; i < total; ++i) {
    if ((i & 63) == 0) {
      CVT_TRY(ctx_.checkpoint(static_cast<double>(i) / static_cast<double>(std::max<std::size_t>(total, 1)), "Writing"));
    }
    block(doc_.blocks[i], target, 0, 0, 0);
  }
  const bool strip = options::resolved_strip_metadata(ctx_.options);
  std::string lang = ctx_.options.get_string("language", "");
  if (lang.empty()) lang = doc_.metadata.language;
  std::string out =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<?mso-application progid=\"Word.Document\"?>\n"
      "<w:wordDocument xmlns:w=\"http://schemas.microsoft.com/office/word/2003/wordml\" "
      "xmlns:wx=\"http://schemas.microsoft.com/office/word/2003/auxHint\" xmlns:v=\"urn:schemas-microsoft-com:vml\" "
      "xmlns:o=\"urn:schemas-microsoft-com:office:office\" xml:space=\"preserve\">";
  if (!doc_.metadata.title.empty() || (!strip && !doc_.metadata.author.empty())) {
    out += "<o:DocumentProperties>";
    if (!doc_.metadata.title.empty()) out += "<o:Title>" + esc(doc_.metadata.title) + "</o:Title>";
    if (!strip && !doc_.metadata.author.empty()) out += "<o:Author>" + esc(doc_.metadata.author) + "</o:Author>";
    out += "</o:DocumentProperties>";
  }
  out += "<w:styles><w:style w:type=\"paragraph\" w:default=\"on\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/>"
         "<w:rPr><w:sz w:val=\"22\"/>";
  if (!lang.empty()) out += "<w:lang w:val=\"" + esc_attr(lang) + "\"/>";
  out += "</w:rPr></w:style>";
  static constexpr int sizes[6] = {36, 30, 26, 24, 22, 22};
  for (int level = 1; level <= 6; ++level) {
    out += "<w:style w:type=\"paragraph\" w:styleId=\"Heading" + std::to_string(level) + "\"><w:name w:val=\"heading " +
           std::to_string(level) +
           "\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:pPr><w:keepNext/>"
           "<w:spacing w:before=\"240\" w:after=\"120\"/><w:outlineLvl w:val=\"" +
           std::to_string(level - 1) + "\"/></w:pPr><w:rPr><w:b/><w:sz w:val=\"" + std::to_string(sizes[level - 1]) +
           "\"/></w:rPr></w:style>";
  }
  out += "<w:style w:type=\"paragraph\" w:styleId=\"Quote\"><w:name w:val=\"Quote\"/><w:basedOn w:val=\"Normal\"/>"
         "<w:pPr><w:ind w:left=\"720\" w:right=\"720\"/></w:pPr><w:rPr><w:i/></w:rPr></w:style>"
         "<w:style w:type=\"paragraph\" w:styleId=\"SourceCode\"><w:name w:val=\"Source Code\"/>"
         "<w:basedOn w:val=\"Normal\"/><w:pPr><w:spacing w:after=\"0\"/></w:pPr><w:rPr><w:rFonts w:ascii=\"Courier New\" "
         "w:h-ansi=\"Courier New\"/><w:sz w:val=\"20\"/></w:rPr></w:style>"
         "<w:style w:type=\"character\" w:styleId=\"VerbatimChar\"><w:name w:val=\"Verbatim Char\"/><w:rPr><w:rFonts "
         "w:ascii=\"Courier New\" w:h-ansi=\"Courier New\"/></w:rPr></w:style>"
         "<w:style w:type=\"character\" w:styleId=\"Hyperlink\"><w:name w:val=\"Hyperlink\"/><w:rPr>"
         "<w:color w:val=\"0563C1\"/><w:u w:val=\"single\"/></w:rPr></w:style>"
         "<w:style w:type=\"paragraph\" w:styleId=\"Caption\"><w:name w:val=\"caption\"/><w:basedOn w:val=\"Normal\"/>"
         "<w:rPr><w:i/><w:sz w:val=\"18\"/></w:rPr></w:style>"
         "<w:style w:type=\"paragraph\" w:styleId=\"Figure\"><w:name w:val=\"Figure\"/><w:basedOn w:val=\"Normal\"/>"
         "<w:pPr><w:jc w:val=\"center\"/></w:pPr></w:style>"
         "<w:style w:type=\"paragraph\" w:styleId=\"Compact\"><w:name w:val=\"Compact\"/><w:basedOn w:val=\"Normal\"/>"
         "</w:style><w:style w:type=\"paragraph\" w:styleId=\"ListParagraph\"><w:name w:val=\"List Paragraph\"/>"
         "<w:basedOn w:val=\"Normal\"/><w:pPr><w:ind w:left=\"720\"/></w:pPr></w:style>"
         "<w:style w:type=\"paragraph\" w:styleId=\"FootnoteText\"><w:name w:val=\"footnote text\"/>"
         "<w:basedOn w:val=\"Normal\"/><w:rPr><w:sz w:val=\"20\"/></w:rPr></w:style>"
         "<w:style w:type=\"character\" w:styleId=\"FootnoteReference\"><w:name w:val=\"footnote reference\"/>"
         "<w:rPr><w:vertAlign w:val=\"superscript\"/></w:rPr></w:style></w:styles>";
  // Lists: listDef 0 = bullets, one definition per ordered list.
  const auto level_xml = [](int i, int nfc, std::string_view text, int start) {
    return "<w:lvl w:ilvl=\"" + std::to_string(i) + "\"><w:start w:val=\"" + std::to_string(start) +
           "\"/><w:nfc w:val=\"" + std::to_string(nfc) + "\"/><w:lvlText w:val=\"" + std::string(text) +
           "\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"" + std::to_string(720 * (i + 1)) +
           "\" w:hanging=\"360\"/></w:pPr></w:lvl>";
  };
  const auto nfc_of = [](ir::ListNumbering n) {
    switch (n) {
      case ir::ListNumbering::UpperRoman: return 1;
      case ir::ListNumbering::LowerRoman: return 2;
      case ir::ListNumbering::UpperAlpha: return 3;
      case ir::ListNumbering::LowerAlpha: return 4;
      case ir::ListNumbering::Decimal: return 0;
    }
    return 0;
  };
  out += "<w:lists><w:listDef w:listDefId=\"0\">";
  for (int i = 0; i < 9; ++i) out += level_xml(i, 23, "\xE2\x80\xA2", 1);
  out += "</w:listDef>";
  for (std::size_t n = 0; n < ordered_.size(); ++n) {
    out += "<w:listDef w:listDefId=\"" + std::to_string(n + 1) + "\">";
    for (int i = 0; i < 9; ++i) {
      const bool mine = i == ordered_[n].level;
      out += level_xml(i, mine ? nfc_of(ordered_[n].numbering) : 0, "%" + std::to_string(i + 1) + ".",
                       mine ? std::max(0, ordered_[n].start) : 1);
    }
    out += "</w:listDef>";
  }
  out += "<w:list w:ilfo=\"1\"><w:ilst w:val=\"0\"/></w:list>";
  for (std::size_t n = 0; n < ordered_.size(); ++n) {
    out += "<w:list w:ilfo=\"" + std::to_string(n + 2) + "\"><w:ilst w:val=\"" + std::to_string(n + 1) + "\"/></w:list>";
  }
  out += "</w:lists><w:body><wx:sect>" + body + section_xml() + "</wx:sect></w:body></w:wordDocument>";
  return to_bytes(out);
}

Result<Bytes> write_word2003(const AnyIR& value, WriteContext& ctx) {
  const ir::Document& doc = value.get<ir::Document>();
  DocxWriter writer(doc, ctx, true);
  return writer.write_word2003();
}

Result<Bytes> write_docx(const AnyIR& value, WriteContext& ctx) {
  const ir::Document& doc = value.get<ir::Document>();
  DocxWriter writer(doc, ctx);
  return writer.write();
}

}  // namespace

WriteFn docx_writer() { return write_docx; }
WriteFn word2003_writer() { return write_word2003; }

}  // namespace cvt::office
