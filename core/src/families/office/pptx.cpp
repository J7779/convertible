// Convertible core - office family: PowerPoint Open XML presentations.
//
// Reader (PPTX, PPTM, PPSX, PPSM, POTX, POTM): slides in presentation
// order; shapes in reading order (top to bottom, left to right) with
// positions inherited from layout and master placeholders; the title
// placeholder as the slide title; body placeholders as bulleted (or
// auto-numbered) lists with levels, text boxes as paragraphs unless they
// carry bullets; group shapes; tables (grid and row spans, merged cells);
// pictures with alt text; hyperlinks; speaker notes (option).
//
// Writer (PPTX): a clean 16:9 deck with a neutral theme, one master, a
// "Title Slide" and a "Title and Content" layout; each slide gets a title
// and a body placeholder; tables and pictures become their own shapes
// placed beside or below the text; speaker notes go to notes pages.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <map>
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
#include "util/text.hpp"
#include "util/zip.hpp"

namespace cvt::office {

namespace {

using util::XmlNode;

constexpr int max_nesting = 16;

// =========================================================================== reader

struct Position {
  std::int64_t x = 0;
  std::int64_t y = 0;
  bool known = false;
};

Position position_of(XmlNode sp_pr) {
  Position p;
  XmlNode off = child(child(sp_pr, "xfrm"), "off");
  if (off) {
    p.x = util::parse_int(attr(off, "x")).value_or(0);
    p.y = util::parse_int(attr(off, "y")).value_or(0);
    p.known = true;
  }
  return p;
}

struct Placeholder {
  std::string type;  // "title", "body", ... ("" = body with an index)
  std::string idx;
};

std::optional<Placeholder> placeholder_of(XmlNode shape) {
  XmlNode nv;
  for (XmlNode c : shape.elements()) {
    if (c.local_name().starts_with("nv")) nv = c;
  }
  XmlNode ph = child(child(nv, "nvPr"), "ph");
  if (!ph) return std::nullopt;
  return Placeholder{std::string(attr(ph, "type")), std::string(attr(ph, "idx"))};
}

struct PlaceholderInfo {
  Position position;
  bool bullets = true;
};

class PptxReader {
 public:
  PptxReader(const ReadContext& ctx, ReadSettings settings) : ctx_(ctx), settings_(settings) {}
  Result<ir::Document> read(Package& package);

 private:
  struct Shape {
    Position position;
    ir::Blocks blocks;
    bool title = false;
    std::string title_text;
  };
  void shapes(XmlNode tree, const std::string& part, std::vector<Shape>& out, int depth);
  ir::Blocks text_body(XmlNode body, const std::string& part, bool bullets_default, bool title);
  ir::Inlines paragraph_runs(XmlNode p, const std::string& part);
  ir::Block table(XmlNode tbl, const std::string& part);
  std::optional<ir::EncodedImage> picture(XmlNode pic, const std::string& part);
  void load_placeholders(const std::string& part, std::map<std::string, PlaceholderInfo>& out);
  std::string rel_target(const std::string& part, std::string_view id, bool* external = nullptr);

  const ReadContext& ctx_;
  ReadSettings settings_;
  Package* package_ = nullptr;
  std::map<std::string, std::vector<Package::Relationship>> rels_cache_;
  std::map<std::string, PlaceholderInfo> layout_placeholders_;
  std::map<std::string, PlaceholderInfo> master_placeholders_;
};

std::string PptxReader::rel_target(const std::string& part, std::string_view id, bool* external) {
  auto it = rels_cache_.find(part);
  if (it == rels_cache_.end()) it = rels_cache_.emplace(part, package_->relationships(part)).first;
  for (const auto& rel : it->second) {
    if (rel.id == id) {
      if (external) *external = rel.external;
      return rel.target;
    }
  }
  return {};
}

void PptxReader::load_placeholders(const std::string& part, std::map<std::string, PlaceholderInfo>& out) {
  out.clear();
  auto xml = package_->xml(part, false);
  if (!xml) return;
  XmlNode tree = child(child(xml->root(), "cSld"), "spTree");
  for (XmlNode sp : tree.elements()) {
    const auto ph = placeholder_of(sp);
    if (!ph) continue;
    PlaceholderInfo info;
    info.position = position_of(child(sp, "spPr"));
    if (XmlNode lvl = descendant(child(sp, "txBody"), "lvl1pPr")) {
      if (child(lvl, "buNone")) info.bullets = false;
    }
    if (!ph->idx.empty()) out["idx:" + ph->idx] = info;
    out["type:" + (ph->type.empty() ? std::string("body") : ph->type)] = info;
  }
}

std::optional<ir::EncodedImage> PptxReader::picture(XmlNode pic, const std::string& part) {
  XmlNode blip = descendant(pic, "blip");
  std::string_view id = blip.attribute("r:embed").value();
  if (id.empty()) id = attr(blip, "embed");
  bool external = false;
  std::string target = rel_target(part, id, &external);
  if (target.empty()) {
    id = blip.attribute("r:link").value();
    target = rel_target(part, id, &external);
  }
  if (target.empty()) return std::nullopt;
  ir::EncodedImage image;
  if (external) {
    image.url = target;
    return image;
  }
  auto bytes = package_->try_read(target);
  if (!bytes) return std::nullopt;
  image.bytes = std::move(*bytes);
  image.mime = document::sniff_image_mime(image.bytes);
  if (image.mime.empty()) image.mime = mime_for_extension(target);
  return image;
}

ir::Inlines PptxReader::paragraph_runs(XmlNode p, const std::string& part) {
  InlineBuilder out;
  for (XmlNode c : p.elements()) {
    const std::string_view name = c.local_name();
    if (name == "r" || name == "fld") {
      XmlNode rpr = child(c, "rPr");
      RunFormat f;
      const auto flag = [&](std::string_view key) {
        const std::string_view v = attr(rpr, key);
        return v == "1" || v == "true";
      };
      f.bold = flag("b");
      f.italic = flag("i");
      const std::string_view u = attr(rpr, "u");
      f.underline = !u.empty() && u != "none";
      const std::string_view strike = attr(rpr, "strike");
      f.strike = !strike.empty() && strike != "noStrike";
      if (auto baseline = util::parse_int(attr(rpr, "baseline"))) {
        f.superscript = *baseline > 0;
        f.subscript = *baseline < 0;
      }
      if (XmlNode latin = child(rpr, "latin")) {
        const std::string font = util::to_lower(attr(latin, "typeface"));
        f.code = font.find("courier") != std::string::npos || font.find("mono") != std::string::npos ||
                 font.find("consolas") != std::string::npos;
      }
      if (XmlNode click = child(rpr, "hlinkClick")) {
        std::string_view id = click.attribute("r:id").value();
        if (id.empty()) id = attr(click, "id");
        bool external = false;
        const std::string target = rel_target(part, id, &external);
        if (external) f.link = target;  // jumps to other slides are not links in a document
        f.underline = false;
      }
      out.text(child(c, "t").text(), f);
    } else if (name == "br") {
      out.line_break(RunFormat{});
    }
  }
  ir::Inlines inlines = out.take();
  trim_inlines(inlines);
  return inlines;
}

ir::Blocks PptxReader::text_body(XmlNode body, const std::string& part, bool bullets_default, bool title) {
  FlowBuilder flow;
  for (XmlNode p : body.children_local("p")) {
    ir::Inlines content = paragraph_runs(p, part);
    if (content.empty()) continue;
    if (title) {
      flow.block(ir::Paragraph{std::move(content)});
      continue;
    }
    XmlNode ppr = child(p, "pPr");
    const int level = static_cast<int>(std::clamp<std::int64_t>(util::parse_int(attr(ppr, "lvl")).value_or(0), 0, 8));
    bool bullets = bullets_default;
    bool ordered = false;
    if (child(ppr, "buNone")) bullets = false;
    if (child(ppr, "buChar") || child(ppr, "buBlip")) bullets = true;
    if (XmlNode num = child(ppr, "buAutoNum")) {
      bullets = true;
      ordered = true;
      (void)num;
    }
    if (bullets) {
      FlowBuilder::ListInfo info;
      info.list_id = "body";
      info.level = level;
      info.ordered = ordered;
      if (XmlNode num = child(ppr, "buAutoNum")) {
        const std::string_view type = attr(num, "type");
        if (type.starts_with("alphaLc")) info.numbering = ir::ListNumbering::LowerAlpha;
        if (type.starts_with("alphaUc")) info.numbering = ir::ListNumbering::UpperAlpha;
        if (type.starts_with("romanLc")) info.numbering = ir::ListNumbering::LowerRoman;
        if (type.starts_with("romanUc")) info.numbering = ir::ListNumbering::UpperRoman;
        if (auto start = util::parse_int(attr(num, "startAt"))) info.start = static_cast<int>(*start);
      }
      flow.list_item(info, ir::Paragraph{std::move(content)});
    } else {
      flow.block(ir::Paragraph{std::move(content)});
    }
  }
  return flow.take();
}

ir::Block PptxReader::table(XmlNode tbl, const std::string& part) {
  ir::TableBlock out;
  const bool first_row_header = attr(child(tbl, "tblPr"), "firstRow") == "1";
  for (XmlNode tr : tbl.children_local("tr")) {
    ir::TableRow row;
    row.header = first_row_header && out.rows.empty();
    for (XmlNode tc : tr.children_local("tc")) {
      if (attr(tc, "hMerge") == "1" || attr(tc, "vMerge") == "1") continue;
      ir::TableCell cell;
      cell.colspan = static_cast<int>(std::clamp<std::int64_t>(util::parse_int(attr(tc, "gridSpan")).value_or(1), 1, 1000));
      cell.rowspan = static_cast<int>(std::clamp<std::int64_t>(util::parse_int(attr(tc, "rowSpan")).value_or(1), 1, 1000));
      for (XmlNode p : child(tc, "txBody").children_local("p")) {
        ir::Inlines content = paragraph_runs(p, part);
        if (content.empty()) continue;
        if (!cell.content.empty()) cell.content.push_back(ir::LineBreak{});
        for (ir::Inline& x : content) cell.content.push_back(std::move(x));
      }
      row.cells.push_back(std::move(cell));
    }
    out.rows.push_back(std::move(row));
  }
  unwrap_header_emphasis(out);
  return out;
}

void PptxReader::shapes(XmlNode tree, const std::string& part, std::vector<Shape>& out, int depth) {
  if (depth > max_nesting) return;
  for (XmlNode sp : tree.elements()) {
    const std::string_view name = sp.local_name();
    Shape shape;
    if (name == "sp") {
      const auto ph = placeholder_of(sp);
      shape.position = position_of(child(sp, "spPr"));
      bool bullets = false;
      if (ph) {
        const std::string type = ph->type.empty() ? "body" : ph->type;
        if (type == "dt" || type == "ftr" || type == "sldNum" || type == "hdr" || type == "sldImg") continue;
        const PlaceholderInfo* inherited = nullptr;
        for (const auto* map : {&layout_placeholders_, &master_placeholders_}) {
          if (inherited) break;
          if (!ph->idx.empty()) {
            if (auto it = map->find("idx:" + ph->idx); it != map->end()) inherited = &it->second;
          }
          if (!inherited) {
            if (auto it = map->find("type:" + type); it != map->end()) inherited = &it->second;
          }
        }
        if (!shape.position.known && inherited) shape.position = inherited->position;
        shape.title = type == "title" || type == "ctrTitle";
        bullets = (type == "body" || type == "obj") && (!inherited || inherited->bullets);
      }
      XmlNode body = child(sp, "txBody");
      shape.blocks = text_body(body, part, bullets, shape.title);
      if (shape.title) {
        for (const ir::Block& b : shape.blocks) {
          std::string t = ir::plain_text(ir::Blocks{b});
          while (!t.empty() && t.back() == '\n') t.pop_back();
          if (!shape.title_text.empty() && !t.empty()) shape.title_text += " ";
          shape.title_text += t;
        }
      }
    } else if (name == "grpSp") {
      std::vector<Shape> inner;
      shapes(sp, part, inner, depth + 1);
      shape.position = position_of(child(sp, "grpSpPr"));
      for (Shape& s : inner) {
        for (ir::Block& b : s.blocks) shape.blocks.push_back(std::move(b));
      }
    } else if (name == "graphicFrame") {
      XmlNode xfrm = child(sp, "xfrm");
      if (XmlNode off = child(xfrm, "off")) {
        shape.position = Position{util::parse_int(attr(off, "x")).value_or(0), util::parse_int(attr(off, "y")).value_or(0), true};
      }
      if (XmlNode tbl = descendant(sp, "tbl")) shape.blocks.push_back(table(tbl, part));
    } else if (name == "pic") {
      shape.position = position_of(child(sp, "spPr"));
      if (auto image = picture(sp, part)) {
        XmlNode cnv = descendant(sp, "cNvPr");
        std::string alt(attr(cnv, "descr"));
        if (alt.empty()) alt = std::string(attr(cnv, "title"));
        shape.blocks.push_back(ir::ImageBlock{std::move(*image), alt, {}});
      }
    } else if (name == "AlternateContent") {
      XmlNode chosen = child(sp, "Choice") ? child(sp, "Choice") : child(sp, "Fallback");
      std::vector<Shape> inner;
      shapes(chosen, part, inner, depth + 1);
      for (Shape& s : inner) out.push_back(std::move(s));
      continue;
    } else if (name == "cxnSp") {
      continue;
    } else {
      continue;
    }
    if (!shape.blocks.empty() || shape.title) out.push_back(std::move(shape));
  }
}

Result<ir::Document> PptxReader::read(Package& package) {
  package_ = &package;
  std::string main = "ppt/presentation.xml";
  for (const auto& rel : package.relationships("")) {
    if (rel.type.ends_with("/officeDocument") && !rel.external) main = rel.target;
  }
  CVT_TRY_ASSIGN(util::XmlDocument pres, package.xml(main, false));
  std::vector<std::string> slide_parts;
  for (XmlNode id : child(pres.root(), "sldIdLst").children_local("sldId")) {
    std::string_view rid = id.attribute("r:id").value();
    if (rid.empty()) rid = attr(id, "id");
    const std::string target = rel_target(main, rid);
    if (!target.empty()) slide_parts.push_back(target);
  }
  CVT_TRY(ctx_.limits.check_archive_entries(slide_parts.size()));
  ir::Document doc;
  if (auto core = package.xml("docProps/core.xml", false)) {
    for (XmlNode n : core->root().elements()) {
      const std::string value(util::trim(n.text()));
      if (value.empty()) continue;
      if (n.local_name() == "title") doc.metadata.title = value;
      if (n.local_name() == "creator") doc.metadata.author = value;
      if (n.local_name() == "language") doc.metadata.language = value;
      if (n.local_name() == "created") doc.metadata.date = value;
    }
  }
  std::string loaded_layout;
  std::string loaded_master;
  for (std::size_t i = 0; i < slide_parts.size(); ++i) {
    CVT_TRY(ctx_.checkpoint(static_cast<double>(i) / static_cast<double>(slide_parts.size()), "Reading slides"));
    const std::string& part = slide_parts[i];
    auto xml = package.xml(part, true);
    if (!xml) continue;
    if (attr(xml->root(), "show") == "0" && !settings_.hidden) continue;
    // Layout and master placeholders for positions and bullet defaults.
    std::string layout;
    std::string notes_part;
    auto it = rels_cache_.find(part);
    if (it == rels_cache_.end()) it = rels_cache_.emplace(part, package.relationships(part)).first;
    for (const auto& rel : it->second) {
      if (rel.type.ends_with("/slideLayout")) layout = rel.target;
      if (rel.type.ends_with("/notesSlide")) notes_part = rel.target;
    }
    if (layout != loaded_layout) {
      load_placeholders(layout, layout_placeholders_);
      std::string master;
      for (const auto& rel : package.relationships(layout)) {
        if (rel.type.ends_with("/slideMaster")) master = rel.target;
      }
      if (master != loaded_master) {
        load_placeholders(master, master_placeholders_);
        loaded_master = master;
      }
      loaded_layout = layout;
    }
    std::vector<Shape> list;
    shapes(child(child(xml->root(), "cSld"), "spTree"), part, list, 0);
    std::stable_sort(list.begin(), list.end(), [](const Shape& a, const Shape& b) {
      if (a.title != b.title) return a.title;
      if (a.position.y != b.position.y) return a.position.y < b.position.y;
      return a.position.x < b.position.x;
    });
    std::string title;
    ir::Blocks body;
    for (Shape& s : list) {
      if (s.title && title.empty()) {
        title = s.title_text;
        continue;
      }
      for (ir::Block& b : s.blocks) body.push_back(std::move(b));
    }
    ir::Blocks notes;
    if (settings_.notes && !notes_part.empty()) {
      if (auto notes_xml = package.xml(notes_part, true)) {
        for (XmlNode sp : child(child(notes_xml->root(), "cSld"), "spTree").elements()) {
          const auto ph = placeholder_of(sp);
          if (!ph || ph->type != "body") continue;
          for (ir::Block& b : text_body(child(sp, "txBody"), notes_part, false, false)) notes.push_back(std::move(b));
        }
      }
    }
    if (!doc.blocks.empty()) doc.blocks.push_back(ir::PageBreak{});
    for (ir::Block& b : make_slide(title, std::move(body), std::move(notes))) doc.blocks.push_back(std::move(b));
  }
  finish_metadata(doc, settings_);
  return doc;
}

Result<AnyIR> read_pptx(ReadContext& ctx) {
  if (as_chars(ctx.input).starts_with("\xD0\xCF\x11\xE0")) {
    return fail(Status::Decode, "the presentation is password-protected (encrypted OOXML); it cannot be read");
  }
  CVT_TRY_ASSIGN(Package package, Package::open(ctx.input, ctx.limits));
  PptxReader reader(ctx, read_settings(ctx.options));
  CVT_TRY_ASSIGN(ir::Document doc, reader.read(package));
  return AnyIR(std::move(doc));
}

// =========================================================================== writer

constexpr std::string_view ns_decl =
    " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
    " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
    " xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"";
constexpr std::string_view rel_ns = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
constexpr std::int64_t slide_w = 12192000;
constexpr std::int64_t slide_h = 6858000;

std::string xml_head() { return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"; }

std::string theme_xml() {
  std::string fills = "<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>";
  std::string lines;
  for (int w : {6350, 12700, 19050}) {
    lines += "<a:ln w=\"" + std::to_string(w) +
             "\" cap=\"flat\" cmpd=\"sng\" algn=\"ctr\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
             "<a:prstDash val=\"solid\"/><a:miter lim=\"800000\"/></a:ln>";
  }
  return xml_head() +
         "<a:theme xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" name=\"Neutral\"><a:themeElements>"
         "<a:clrScheme name=\"Neutral\"><a:dk1><a:srgbClr val=\"000000\"/></a:dk1><a:lt1><a:srgbClr val=\"FFFFFF\"/></a:lt1>"
         "<a:dk2><a:srgbClr val=\"333333\"/></a:dk2><a:lt2><a:srgbClr val=\"EEEEEE\"/></a:lt2>"
         "<a:accent1><a:srgbClr val=\"3366CC\"/></a:accent1><a:accent2><a:srgbClr val=\"CC6633\"/></a:accent2>"
         "<a:accent3><a:srgbClr val=\"669933\"/></a:accent3><a:accent4><a:srgbClr val=\"996699\"/></a:accent4>"
         "<a:accent5><a:srgbClr val=\"339999\"/></a:accent5><a:accent6><a:srgbClr val=\"CC9933\"/></a:accent6>"
         "<a:hlink><a:srgbClr val=\"0563C1\"/></a:hlink><a:folHlink><a:srgbClr val=\"954F72\"/></a:folHlink></a:clrScheme>"
         "<a:fontScheme name=\"Neutral\"><a:majorFont><a:latin typeface=\"Arial\"/><a:ea typeface=\"\"/><a:cs typeface=\"\"/>"
         "</a:majorFont><a:minorFont><a:latin typeface=\"Arial\"/><a:ea typeface=\"\"/><a:cs typeface=\"\"/></a:minorFont>"
         "</a:fontScheme><a:fmtScheme name=\"Neutral\"><a:fillStyleLst>" +
         fills + fills + fills + "</a:fillStyleLst><a:lnStyleLst>" + lines +
         "</a:lnStyleLst><a:effectStyleLst><a:effectStyle><a:effectLst/></a:effectStyle><a:effectStyle><a:effectLst/>"
         "</a:effectStyle><a:effectStyle><a:effectLst/></a:effectStyle></a:effectStyleLst><a:bgFillStyleLst>" +
         fills + fills + fills +
         "</a:bgFillStyleLst></a:fmtScheme></a:themeElements><a:objectDefaults/><a:extraClrSchemeLst/></a:theme>";
}

std::string text_styles() {
  std::string body;
  for (int i = 1; i <= 9; ++i) {
    const int indent = 228600 * 2 * i - 228600;
    body += "<a:lvl" + std::to_string(i) + "pPr marL=\"" + std::to_string(indent) +
            "\" indent=\"-228600\" algn=\"l\"><a:spcBef><a:spcPts val=\"600\"/></a:spcBef><a:buFont typeface=\"Arial\"/>"
            "<a:buChar char=\"" + std::string(i % 2 == 1 ? "\xE2\x80\xA2" : "\xE2\x80\x93") +
            "\"/><a:defRPr sz=\"" + std::to_string(i == 1 ? 2400 : i == 2 ? 2000 : 1800) +
            "\" kern=\"1200\"><a:solidFill><a:schemeClr val=\"tx1\"/></a:solidFill><a:latin typeface=\"+mn-lt\"/>"
            "</a:defRPr></a:lvl" + std::to_string(i) + "pPr>";
  }
  return "<p:txStyles><p:titleStyle><a:lvl1pPr algn=\"l\"><a:defRPr sz=\"3600\" b=\"1\" kern=\"1200\"><a:solidFill>"
         "<a:schemeClr val=\"tx1\"/></a:solidFill><a:latin typeface=\"+mj-lt\"/></a:defRPr></a:lvl1pPr></p:titleStyle>"
         "<p:bodyStyle>" + body + "</p:bodyStyle><p:otherStyle><a:defPPr><a:defRPr lang=\"en-US\"/></a:defPPr>"
         "</p:otherStyle></p:txStyles>";
}

std::string sp_placeholder(int id, std::string_view name, std::string_view ph, std::int64_t x, std::int64_t y,
                            std::int64_t cx, std::int64_t cy, std::string_view body, bool with_xfrm = true) {
  std::string out = "<p:sp><p:nvSpPr><p:cNvPr id=\"" + std::to_string(id) + "\" name=\"" + std::string(name) +
                    "\"/><p:cNvSpPr><a:spLocks noGrp=\"1\"/></p:cNvSpPr><p:nvPr>" + std::string(ph) + "</p:nvPr></p:nvSpPr><p:spPr>";
  if (with_xfrm) {
    out += "<a:xfrm><a:off x=\"" + std::to_string(x) + "\" y=\"" + std::to_string(y) + "\"/><a:ext cx=\"" +
           std::to_string(cx) + "\" cy=\"" + std::to_string(cy) + "\"/></a:xfrm>";
  }
  out += "</p:spPr><p:txBody><a:bodyPr><a:normAutofit/></a:bodyPr><a:lstStyle/>" + std::string(body) + "</p:txBody></p:sp>";
  return out;
}

std::string tree_head() {
  return "<p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr>"
         "<a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/><a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/>"
         "</a:xfrm></p:grpSpPr>";
}

struct Rels {
  std::vector<std::string> lines;
  int next = 1;
  std::string add(std::string_view type, std::string_view target, bool external = false) {
    const std::string id = "rId" + std::to_string(next++);
    lines.push_back("<Relationship Id=\"" + id + "\" Type=\"" + std::string(rel_ns) + std::string(type) + "\" Target=\"" +
                    esc_attr(target) + "\"" + (external ? " TargetMode=\"External\"" : "") + "/>");
    return id;
  }
  [[nodiscard]] std::string xml() const {
    std::string out = xml_head() + "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
    for (const std::string& l : lines) out += l;
    return out + "</Relationships>";
  }
};

class PptxWriter {
 public:
  PptxWriter(const ir::Document& doc, WriteContext& ctx) : doc_(doc), ctx_(ctx) { policy_ = image_policy(ctx.options); }
  Result<Bytes> write();

 private:
  struct Run {
    bool bold = false, italic = false, underline = false, strike = false, sup = false, sub = false, code = false;
  };
  void runs(const ir::Inlines& items, std::string& out, Run f, Rels& rels, int depth);
  void text_run(std::string_view text, std::string& out, const Run& f, std::string_view link_id = {});
  void paragraphs(const ir::Blocks& blocks, std::string& out, Rels& rels, int level, int depth);
  std::string table_frame(const ir::TableBlock& table, int id, std::int64_t x, std::int64_t y, std::int64_t cx, Rels& rels);
  std::string picture(const ir::ImageBlock& image, int id, std::int64_t x, std::int64_t y, std::int64_t cx,
                      std::int64_t cy, Rels& rels);

  const ir::Document& doc_;
  WriteContext& ctx_;
  ImagePolicy policy_;
  MediaStore media_;
  std::vector<std::string> pending_notes_;
  std::string link_target_;
};

void PptxWriter::text_run(std::string_view text, std::string& out, const Run& f, std::string_view link_id) {
  if (text.empty()) return;
  std::string rpr = "<a:rPr lang=\"en-US\" dirty=\"0\"";
  if (f.bold) rpr += " b=\"1\"";
  if (f.italic) rpr += " i=\"1\"";
  if (f.underline) rpr += " u=\"sng\"";
  if (f.strike) rpr += " strike=\"sngStrike\"";
  if (f.sup) rpr += " baseline=\"30000\"";
  if (f.sub) rpr += " baseline=\"-25000\"";
  rpr += ">";
  if (f.code) rpr += "<a:latin typeface=\"Courier New\"/>";
  if (!link_id.empty()) rpr += "<a:hlinkClick r:id=\"" + std::string(link_id) + "\"/>";
  rpr += "</a:rPr>";
  std::string clean;
  for (char c : text) clean.push_back(c == '\n' || c == '\t' ? ' ' : c);
  out += "<a:r>" + rpr + "<a:t>" + esc(clean) + "</a:t></a:r>";
}

void PptxWriter::runs(const ir::Inlines& items, std::string& out, Run f, Rels& rels, int depth) {
  if (depth > ir::max_document_depth) return;
  for (const ir::Inline& item : items) {
    std::visit(
        [&](const auto& node) {
          using T = std::remove_cvref_t<decltype(node)>;
          if constexpr (std::same_as<T, ir::Text>) {
            text_run(node.text, out, f, link_target_);
          } else if constexpr (std::same_as<T, ir::Code>) {
            Run g = f;
            g.code = true;
            text_run(node.text, out, g, link_target_);
          } else if constexpr (std::same_as<T, ir::LineBreak>) {
            out += "<a:br><a:rPr lang=\"en-US\"/></a:br>";
          } else if constexpr (std::same_as<T, ir::InlineImage>) {
            text_run(node.alt, out, f);
          } else if constexpr (std::same_as<T, ir::Link>) {
            if (!node.note.empty()) {
              Run g = f;
              g.sup = true;
              text_run(node.note, out, g);
              pending_notes_.push_back(node.note);
              return;
            }
            const std::string saved = link_target_;
            if (!node.url.empty() && !node.url.starts_with('#') && link_target_.empty()) {
              link_target_ = rels.add("hyperlink", node.url, true);
            }
            runs(node.children, out, f, rels, depth + 1);
            link_target_ = saved;
          } else {
            Run g = f;
            if constexpr (std::same_as<T, ir::Emphasis>) g.italic = true;
            if constexpr (std::same_as<T, ir::Strong>) g.bold = true;
            if constexpr (std::same_as<T, ir::Underline>) g.underline = true;
            if constexpr (std::same_as<T, ir::Strikethrough>) g.strike = true;
            if constexpr (std::same_as<T, ir::Superscript>) g.sup = true;
            if constexpr (std::same_as<T, ir::Subscript>) g.sub = true;
            runs(node.children, out, g, rels, depth + 1);
          }
        },
        item.node);
  }
}

void PptxWriter::paragraphs(const ir::Blocks& blocks, std::string& out, Rels& rels, int level, int depth) {
  if (depth > ir::max_document_depth) return;
  const auto plain_para = [&](const ir::Inlines& content, Run f, std::string_view extra_ppr = {}) {
    out += "<a:p><a:pPr marL=\"0\" indent=\"0\"" + std::string(extra_ppr) + "><a:buNone/></a:pPr>";
    runs(content, out, f, rels, depth + 1);
    out += "</a:p>";
  };
  for (const ir::Block& b : blocks) {
    if (const auto* para = b.get_if<ir::Paragraph>()) {
      plain_para(para->content, Run{});
    } else if (const auto* heading = b.get_if<ir::Heading>()) {
      Run f;
      f.bold = true;
      plain_para(heading->content, f);
    } else if (const auto* list = b.get_if<ir::List>()) {
      const int lvl = std::clamp(level, 0, 8);
      for (std::size_t i = 0; i < list->items.size(); ++i) {
        bool first = true;
        for (const ir::Block& inner : list->items[i]) {
          if (const auto* nested = inner.get_if<ir::List>()) {
            paragraphs(ir::Blocks{inner}, out, rels, level + 1, depth + 1);
            (void)nested;
            continue;
          }
          const ir::Inlines* content = nullptr;
          if (const auto* p = inner.get_if<ir::Paragraph>()) content = &p->content;
          if (const auto* h = inner.get_if<ir::Heading>()) content = &h->content;
          if (!content) {
            paragraphs(ir::Blocks{inner}, out, rels, level + 1, depth + 1);
            continue;
          }
          out += "<a:p><a:pPr lvl=\"" + std::to_string(lvl) + "\"";
          if (!first) out += " marL=\"" + std::to_string(457200 * (lvl + 1) - 228600) + "\" indent=\"0\"><a:buNone/>";
          else if (list->ordered) {
            std::string scheme = "arabicPeriod";
            if (list->numbering == ir::ListNumbering::LowerAlpha) scheme = "alphaLcPeriod";
            if (list->numbering == ir::ListNumbering::UpperAlpha) scheme = "alphaUcPeriod";
            if (list->numbering == ir::ListNumbering::LowerRoman) scheme = "romanLcPeriod";
            if (list->numbering == ir::ListNumbering::UpperRoman) scheme = "romanUcPeriod";
            out += "><a:buFont typeface=\"+mj-lt\"/><a:buAutoNum type=\"" + scheme + "\"";
            if (list->start != 1) out += " startAt=\"" + std::to_string(std::max(1, list->start)) + "\"";
            out += "/>";
          } else {
            out += ">";
          }
          out += "</a:pPr>";
          if (first && list->task(i) == ir::TaskState::Open) text_run("\xE2\x98\x90 ", out, Run{});
          if (first && list->task(i) == ir::TaskState::Done) text_run("\xE2\x98\x92 ", out, Run{});
          Run f;
          f.bold = list->definition && first;
          runs(*content, out, f, rels, depth + 1);
          out += "</a:p>";
          first = false;
        }
      }
    } else if (const auto* code = b.get_if<ir::CodeBlock>()) {
      std::string_view text = code->code;
      while (text.ends_with('\n')) text.remove_suffix(1);
      for (std::string_view line : util::split(text, '\n')) {
        if (line.ends_with('\r')) line.remove_suffix(1);
        Run f;
        f.code = true;
        out += "<a:p><a:pPr marL=\"0\" indent=\"0\"><a:buNone/></a:pPr>";
        text_run(line.empty() ? std::string_view(" ") : line, out, f);
        out += "</a:p>";
      }
    } else if (const auto* quote = b.get_if<ir::BlockQuote>()) {
      std::string inner;
      std::swap(inner, out);
      paragraphs(quote->blocks, out, rels, level, depth + 1);
      // Italicise the quoted runs.
      out = util::replace_all(out, "<a:rPr lang=\"en-US\" dirty=\"0\"", "<a:rPr lang=\"en-US\" dirty=\"0\" i=\"1\"");
      out = util::replace_all(out, "i=\"1\" i=\"1\"", "i=\"1\"");
      inner += out;
      out = std::move(inner);
    } else if (b.is<ir::HorizontalRule>() || b.is<ir::PageBreak>()) {
      continue;
    } else if (const auto* image = b.get_if<ir::ImageBlock>()) {
      plain_para({ir::Text{image->alt}}, Run{});
    } else if (const auto* table = b.get_if<ir::TableBlock>()) {
      for (const ir::TableRow& row : table->rows) {
        std::string line;
        for (std::size_t c = 0; c < row.cells.size(); ++c) {
          if (c) line += " | ";
          line += ir::plain_text(row.cells[c].content);
        }
        plain_para({ir::Text{line}}, Run{});
      }
    }
  }
}

std::string PptxWriter::table_frame(const ir::TableBlock& table, int id, std::int64_t x, std::int64_t y, std::int64_t cx,
                                    Rels& rels) {
  const document::TableGrid grid = document::table_grid(table);
  if (grid.columns == 0) return {};
  const std::int64_t col = cx / static_cast<std::int64_t>(grid.columns);
  const std::int64_t row_h = 370840;
  std::string out = "<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id=\"" + std::to_string(id) + "\" name=\"Table " +
                    std::to_string(id) + "\"/><p:cNvGraphicFramePr><a:graphicFrameLocks noGrp=\"1\"/></p:cNvGraphicFramePr>"
                    "<p:nvPr/></p:nvGraphicFramePr><p:xfrm><a:off x=\"" + std::to_string(x) + "\" y=\"" + std::to_string(y) +
                    "\"/><a:ext cx=\"" + std::to_string(col * static_cast<std::int64_t>(grid.columns)) + "\" cy=\"" +
                    std::to_string(row_h * static_cast<std::int64_t>(grid.cells.size())) +
                    "\"/></p:xfrm><a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/table\">"
                    "<a:tbl><a:tblPr firstRow=\"" + std::string(grid.header_rows > 0 ? "1" : "0") + "\" bandRow=\"1\"/><a:tblGrid>";
  for (std::size_t c = 0; c < grid.columns; ++c) out += "<a:gridCol w=\"" + std::to_string(col) + "\"/>";
  out += "</a:tblGrid>";
  for (std::size_t r = 0; r < grid.cells.size(); ++r) {
    out += "<a:tr h=\"" + std::to_string(row_h) + "\">";
    for (std::size_t c = 0; c < grid.columns; ++c) {
      const ir::TableCell* cell = grid.cells[r][c];
      out += "<a:tc";
      if (cell && grid.origin[r][c]) {
        if (cell->colspan > 1) out += " gridSpan=\"" + std::to_string(cell->colspan) + "\"";
        if (cell->rowspan > 1) out += " rowSpan=\"" + std::to_string(cell->rowspan) + "\"";
      } else if (cell) {
        const bool left = c > 0 && grid.cells[r][c - 1] == cell;
        out += left ? " hMerge=\"1\"" : " vMerge=\"1\"";
      }
      out += "><a:txBody><a:bodyPr/><a:lstStyle/><a:p>";
      if (cell && grid.origin[r][c]) {
        Run f;
        f.bold = r < grid.header_rows;
        runs(cell->content, out, f, rels, 1);
      }
      out += "<a:endParaRPr lang=\"en-US\" sz=\"1400\"/></a:p></a:txBody><a:tcPr><a:lnL w=\"6350\"><a:solidFill>"
             "<a:srgbClr val=\"808080\"/></a:solidFill></a:lnL><a:lnR w=\"6350\"><a:solidFill><a:srgbClr val=\"808080\"/>"
             "</a:solidFill></a:lnR><a:lnT w=\"6350\"><a:solidFill><a:srgbClr val=\"808080\"/></a:solidFill></a:lnT>"
             "<a:lnB w=\"6350\"><a:solidFill><a:srgbClr val=\"808080\"/></a:solidFill></a:lnB></a:tcPr></a:tc>";
    }
    out += "</a:tr>";
  }
  out += "</a:tbl></a:graphicData></a:graphic></p:graphicFrame>";
  return out;
}

std::string PptxWriter::picture(const ir::ImageBlock& image, int id, std::int64_t x, std::int64_t y, std::int64_t cx,
                                std::int64_t cy, Rels& rels) {
  auto prepared = prepare_image(image.source, policy_, ctx_);
  if (prepared && !is_web_safe(prepared->extension)) prepared = convert_to_png(*prepared, ctx_);
  if (!prepared) return {};
  PixelSize px = prepared->size;
  if (px.width == 0 || px.height == 0) px = PixelSize{4, 3};
  // Fit into the box keeping the aspect ratio.
  const double scale = std::min(static_cast<double>(cx) / px.width, static_cast<double>(cy) / px.height);
  const std::int64_t w = std::max<std::int64_t>(1, static_cast<std::int64_t>(px.width * scale));
  const std::int64_t h = std::max<std::int64_t>(1, static_cast<std::int64_t>(px.height * scale));
  const std::size_t index = media_.add(std::move(*prepared));
  const std::string rid = rels.add("image", "../media/" + media_.items()[index].name);
  return "<p:pic><p:nvPicPr><p:cNvPr id=\"" + std::to_string(id) + "\" name=\"Picture " + std::to_string(id) +
         "\" descr=\"" + esc_attr(image.alt) + "\"/><p:cNvPicPr><a:picLocks noChangeAspect=\"1\"/></p:cNvPicPr><p:nvPr/>"
         "</p:nvPicPr><p:blipFill><a:blip r:embed=\"" + rid + "\"/><a:stretch><a:fillRect/></a:stretch></p:blipFill>"
         "<p:spPr><a:xfrm><a:off x=\"" + std::to_string(x + (cx - w) / 2) + "\" y=\"" + std::to_string(y) +
         "\"/><a:ext cx=\"" + std::to_string(w) + "\" cy=\"" + std::to_string(h) +
         "\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr></p:pic>";
}

Result<Bytes> PptxWriter::write() {
  const std::vector<Slide> slides = split_slides(doc_, ctx_.options);
  const bool with_notes = ctx_.options.get_bool("include_notes", true);
  int level = static_cast<int>(ctx_.options.get_int("compression_level", 9, 0, 9));
  if (!ctx_.options.was_set("compression_level")) level = 9;
  util::ZipEntryOptions entry;
  entry.level = level;
  util::ZipWriter zip;

  std::vector<std::string> slide_xml;
  std::vector<Rels> slide_rels;
  std::vector<std::string> notes_xml;
  bool any_notes = false;
  for (std::size_t s = 0; s < slides.size(); ++s) {
    CVT_TRY(ctx_.checkpoint(static_cast<double>(s) / static_cast<double>(slides.size()), "Writing slides"));
    const Slide& slide = slides[s];
    Rels rels;
    const bool title_only = s == 0 && slide.body.empty();
    rels.add("slideLayout", title_only ? "../slideLayouts/slideLayout1.xml" : "../slideLayouts/slideLayout2.xml");
    std::string title_body = "<a:p>";
    runs(slide.title, title_body, Run{}, rels, 0);
    title_body += "<a:endParaRPr lang=\"en-US\"/></a:p>";
    pending_notes_.clear();
    std::string xml = xml_head() + "<p:sld" + std::string(ns_decl) + ">" + tree_head();
    xml += sp_placeholder(2, "Title 1", title_only ? "<p:ph type=\"ctrTitle\"/>" : "<p:ph type=\"title\"/>", 0, 0, 0, 0,
                          title_body, false);
    ir::Blocks text;
    std::vector<const ir::TableBlock*> tables;
    std::vector<const ir::ImageBlock*> pictures;
    for (const ir::Block& b : slide.body) {
      if (const auto* t = b.get_if<ir::TableBlock>()) tables.push_back(t);
      else if (const auto* p = b.get_if<ir::ImageBlock>()) pictures.push_back(p);
      else text.push_back(b);
    }
    std::string body;
    paragraphs(text, body, rels, 0, 0);
    for (const std::string& label : pending_notes_) {
      for (const ir::Footnote& note : doc_.footnotes) {
        if (note.label != label) continue;
        body += "<a:p><a:pPr marL=\"0\" indent=\"0\"><a:buNone/></a:pPr>";
        text_run(label + ". " + std::string(util::trim(ir::plain_text(note.blocks))), body, Run{});
        body += "</a:p>";
      }
    }
    const bool visuals = !tables.empty() || !pictures.empty();
    const std::int64_t left = 838200;
    const std::int64_t top = 1825625;
    const std::int64_t full_w = 10515600;
    const std::int64_t full_h = 4351338;
    int id = 3;
    if (!body.empty()) {
      const std::int64_t w = visuals ? full_w / 2 - 100000 : full_w;
      xml += sp_placeholder(id++, "Content 2", title_only ? "<p:ph type=\"subTitle\" idx=\"1\"/>" : "<p:ph idx=\"1\"/>", left,
                            top, w, full_h, body, visuals);
    }
    const std::int64_t vx = body.empty() ? left : left + full_w / 2 + 100000;
    const std::int64_t vw = body.empty() ? full_w : full_w / 2 - 100000;
    std::int64_t vy = top;
    const std::int64_t vh = full_h / static_cast<std::int64_t>(std::max<std::size_t>(1, tables.size() + pictures.size()));
    for (const ir::TableBlock* t : tables) {
      xml += table_frame(*t, id++, vx, vy, vw, rels);
      vy += vh;
    }
    for (const ir::ImageBlock* p : pictures) {
      xml += picture(*p, id++, vx, vy, vw, vh - 50000, rels);
      vy += vh;
    }
    xml += "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sld>";
    std::string notes;
    if (with_notes && !slide.notes.empty()) {
      any_notes = true;
      rels.add("notesSlide", "../notesSlides/notesSlide" + std::to_string(s + 1) + ".xml");
      std::string note_body;
      Rels unused;
      paragraphs(slide.notes, note_body, unused, 0, 0);
      notes = xml_head() + "<p:notes" + std::string(ns_decl) + ">" + tree_head() +
              "<p:sp><p:nvSpPr><p:cNvPr id=\"2\" name=\"Slide Image 1\"/><p:cNvSpPr><a:spLocks noGrp=\"1\" noRot=\"1\" "
              "noChangeAspect=\"1\"/></p:cNvSpPr><p:nvPr><p:ph type=\"sldImg\"/></p:nvPr></p:nvSpPr><p:spPr/></p:sp>" +
              sp_placeholder(3, "Notes 2", "<p:ph type=\"body\" idx=\"1\"/>", 0, 0, 0, 0, note_body, false) +
              "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:notes>";
    }
    slide_xml.push_back(std::move(xml));
    slide_rels.push_back(std::move(rels));
    notes_xml.push_back(std::move(notes));
  }

  // Package parts.
  std::string types = xml_head() +
                      "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                      "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
                      "<Default Extension=\"xml\" ContentType=\"application/xml\"/>";
  std::set<std::string> extensions;
  for (const auto& item : media_.items()) extensions.insert(item.image.extension);
  for (const std::string& ext : extensions) {
    types += "<Default Extension=\"" + ext + "\" ContentType=\"" + (ext == "jpg" ? std::string("image/jpeg") : mime_for_extension("x." + ext)) + "\"/>";
  }
  const std::string pml = "application/vnd.openxmlformats-officedocument.presentationml.";
  types += "<Override PartName=\"/ppt/presentation.xml\" ContentType=\"" + pml + "presentation.main+xml\"/>"
           "<Override PartName=\"/ppt/slideMasters/slideMaster1.xml\" ContentType=\"" + pml + "slideMaster+xml\"/>"
           "<Override PartName=\"/ppt/slideLayouts/slideLayout1.xml\" ContentType=\"" + pml + "slideLayout+xml\"/>"
           "<Override PartName=\"/ppt/slideLayouts/slideLayout2.xml\" ContentType=\"" + pml + "slideLayout+xml\"/>"
           "<Override PartName=\"/ppt/theme/theme1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.theme+xml\"/>"
           "<Override PartName=\"/ppt/presProps.xml\" ContentType=\"" + pml + "presProps+xml\"/>"
           "<Override PartName=\"/ppt/viewProps.xml\" ContentType=\"" + pml + "viewProps+xml\"/>"
           "<Override PartName=\"/ppt/tableStyles.xml\" ContentType=\"" + pml + "tableStyles+xml\"/>"
           "<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/>"
           "<Override PartName=\"/docProps/app.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.extended-properties+xml\"/>";
  if (any_notes) {
    types += "<Override PartName=\"/ppt/notesMasters/notesMaster1.xml\" ContentType=\"" + pml + "notesMaster+xml\"/>"
             "<Override PartName=\"/ppt/theme/theme2.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.theme+xml\"/>";
  }
  for (std::size_t s = 0; s < slides.size(); ++s) {
    types += "<Override PartName=\"/ppt/slides/slide" + std::to_string(s + 1) + ".xml\" ContentType=\"" + pml + "slide+xml\"/>";
    if (!notes_xml[s].empty()) {
      types += "<Override PartName=\"/ppt/notesSlides/notesSlide" + std::to_string(s + 1) + ".xml\" ContentType=\"" + pml +
               "notesSlide+xml\"/>";
    }
  }
  types += "</Types>";

  Rels pres_rels;
  const std::string master_rid = pres_rels.add("slideMaster", "slideMasters/slideMaster1.xml");
  std::string notes_master_rid;
  if (any_notes) notes_master_rid = pres_rels.add("notesMaster", "notesMasters/notesMaster1.xml");
  std::string slide_ids;
  for (std::size_t s = 0; s < slides.size(); ++s) {
    const std::string rid = pres_rels.add("slide", "slides/slide" + std::to_string(s + 1) + ".xml");
    slide_ids += "<p:sldId id=\"" + std::to_string(256 + s) + "\" r:id=\"" + rid + "\"/>";
  }
  pres_rels.add("theme", "theme/theme1.xml");
  pres_rels.add("presProps", "presProps.xml");
  pres_rels.add("viewProps", "viewProps.xml");
  pres_rels.add("tableStyles", "tableStyles.xml");
  std::string presentation = xml_head() + "<p:presentation" + std::string(ns_decl) +
                             " saveSubsetFonts=\"1\"><p:sldMasterIdLst><p:sldMasterId id=\"2147483648\" r:id=\"" + master_rid +
                             "\"/></p:sldMasterIdLst>";
  if (any_notes) presentation += "<p:notesMasterIdLst><p:notesMasterId r:id=\"" + notes_master_rid + "\"/></p:notesMasterIdLst>";
  presentation += "<p:sldIdLst>" + slide_ids + "</p:sldIdLst><p:sldSz cx=\"" + std::to_string(slide_w) + "\" cy=\"" +
                  std::to_string(slide_h) + "\"/><p:notesSz cx=\"6858000\" cy=\"9144000\"/><p:defaultTextStyle>"
                  "<a:defPPr><a:defRPr lang=\"en-US\"/></a:defPPr></p:defaultTextStyle></p:presentation>";

  const std::string master =
      xml_head() + "<p:sldMaster" + std::string(ns_decl) + "><p:cSld><p:bg><p:bgRef idx=\"1001\"><a:schemeClr val=\"bg1\"/>"
      "</p:bgRef></p:bg>" + tree_head().substr(8) +
      sp_placeholder(2, "Title Placeholder 1", "<p:ph type=\"title\"/>", 838200, 365125, 10515600, 1325563,
                     "<a:p><a:r><a:rPr lang=\"en-US\"/><a:t>Title</a:t></a:r></a:p>") +
      sp_placeholder(3, "Text Placeholder 2", "<p:ph type=\"body\" idx=\"1\"/>", 838200, 1825625, 10515600, 4351338,
                     "<a:p><a:pPr lvl=\"0\"/><a:r><a:rPr lang=\"en-US\"/><a:t>Text</a:t></a:r></a:p>") +
      "</p:spTree></p:cSld><p:clrMap bg1=\"lt1\" tx1=\"dk1\" bg2=\"lt2\" tx2=\"dk2\" accent1=\"accent1\" accent2=\"accent2\" "
      "accent3=\"accent3\" accent4=\"accent4\" accent5=\"accent5\" accent6=\"accent6\" hlink=\"hlink\" folHlink=\"folHlink\"/>"
      "<p:sldLayoutIdLst><p:sldLayoutId id=\"2147483649\" r:id=\"rId1\"/><p:sldLayoutId id=\"2147483650\" r:id=\"rId2\"/>"
      "</p:sldLayoutIdLst>" + text_styles() + "</p:sldMaster>";
  Rels master_rels;
  master_rels.add("slideLayout", "../slideLayouts/slideLayout1.xml");
  master_rels.add("slideLayout", "../slideLayouts/slideLayout2.xml");
  master_rels.add("theme", "../theme/theme1.xml");
  const std::string layout1 =
      xml_head() + "<p:sldLayout" + std::string(ns_decl) + " type=\"title\" preserve=\"1\"><p:cSld name=\"Title Slide\">" +
      tree_head().substr(8) +
      sp_placeholder(2, "Title 1", "<p:ph type=\"ctrTitle\"/>", 1524000, 1122363, 9144000, 2387600,
                     "<a:lstStyle/><a:p><a:pPr algn=\"ctr\"/><a:r><a:rPr lang=\"en-US\"/><a:t>Title</a:t></a:r></a:p>") +
      sp_placeholder(3, "Subtitle 2", "<p:ph type=\"subTitle\" idx=\"1\"/>", 1524000, 3602038, 9144000, 1655762,
                     "<a:p><a:pPr marL=\"0\" indent=\"0\" algn=\"ctr\"><a:buNone/></a:pPr><a:r><a:rPr lang=\"en-US\"/>"
                     "<a:t>Subtitle</a:t></a:r></a:p>") +
      "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sldLayout>";
  const std::string layout2 =
      xml_head() + "<p:sldLayout" + std::string(ns_decl) + " type=\"obj\" preserve=\"1\"><p:cSld name=\"Title and Content\">" +
      tree_head().substr(8) +
      sp_placeholder(2, "Title 1", "<p:ph type=\"title\"/>", 0, 0, 0, 0,
                     "<a:p><a:r><a:rPr lang=\"en-US\"/><a:t>Title</a:t></a:r></a:p>", false) +
      sp_placeholder(3, "Content Placeholder 2", "<p:ph idx=\"1\"/>", 0, 0, 0, 0,
                     "<a:p><a:pPr lvl=\"0\"/><a:r><a:rPr lang=\"en-US\"/><a:t>Text</a:t></a:r></a:p>", false) +
      "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sldLayout>";
  Rels layout_rels;
  layout_rels.add("slideMaster", "../slideMasters/slideMaster1.xml");

  const std::string package_rels =
      xml_head() + "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                   "<Relationship Id=\"rId1\" Type=\"" + std::string(rel_ns) + "officeDocument\" Target=\"ppt/presentation.xml\"/>"
                   "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" "
                   "Target=\"docProps/core.xml\"/><Relationship Id=\"rId3\" Type=\"" + std::string(rel_ns) +
      "extended-properties\" Target=\"docProps/app.xml\"/></Relationships>";
  std::string core = xml_head() +
                     "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
                     "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:dcterms=\"http://purl.org/dc/terms/\" "
                     "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">";
  if (!doc_.metadata.title.empty()) core += "<dc:title>" + esc(doc_.metadata.title) + "</dc:title>";
  if (!options::resolved_strip_metadata(ctx_.options) && !doc_.metadata.author.empty()) {
    core += "<dc:creator>" + esc(doc_.metadata.author) + "</dc:creator>";
  }
  core += "</cp:coreProperties>";
  const std::string app = xml_head() +
                          "<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/extended-properties\">"
                          "<Slides>" + std::to_string(slides.size()) + "</Slides></Properties>";

  CVT_TRY(zip.add("[Content_Types].xml", types, entry));
  CVT_TRY(zip.add("_rels/.rels", package_rels, entry));
  CVT_TRY(zip.add("docProps/core.xml", core, entry));
  CVT_TRY(zip.add("docProps/app.xml", app, entry));
  CVT_TRY(zip.add("ppt/presentation.xml", presentation, entry));
  CVT_TRY(zip.add("ppt/_rels/presentation.xml.rels", pres_rels.xml(), entry));
  CVT_TRY(zip.add("ppt/presProps.xml", xml_head() + "<p:presentationPr" + std::string(ns_decl) + "/>", entry));
  CVT_TRY(zip.add("ppt/viewProps.xml", xml_head() + "<p:viewPr" + std::string(ns_decl) +
                                           "><p:normalViewPr><p:restoredLeft sz=\"15620\"/><p:restoredTop sz=\"94660\"/>"
                                           "</p:normalViewPr><p:gridSpacing cx=\"76200\" cy=\"76200\"/></p:viewPr>",
                  entry));
  CVT_TRY(zip.add("ppt/tableStyles.xml",
                  xml_head() + "<a:tblStyleLst xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
                               "def=\"{5C22544A-7EE6-4342-B048-85BDC9FD1C3A}\"/>",
                  entry));
  CVT_TRY(zip.add("ppt/slideMasters/slideMaster1.xml", master, entry));
  CVT_TRY(zip.add("ppt/slideMasters/_rels/slideMaster1.xml.rels", master_rels.xml(), entry));
  CVT_TRY(zip.add("ppt/slideLayouts/slideLayout1.xml", layout1, entry));
  CVT_TRY(zip.add("ppt/slideLayouts/_rels/slideLayout1.xml.rels", layout_rels.xml(), entry));
  CVT_TRY(zip.add("ppt/slideLayouts/slideLayout2.xml", layout2, entry));
  CVT_TRY(zip.add("ppt/slideLayouts/_rels/slideLayout2.xml.rels", layout_rels.xml(), entry));
  CVT_TRY(zip.add("ppt/theme/theme1.xml", theme_xml(), entry));
  if (any_notes) {
    const std::string notes_master =
        xml_head() + "<p:notesMaster" + std::string(ns_decl) + "><p:cSld><p:bg><p:bgRef idx=\"1001\"><a:schemeClr val=\"bg1\"/>"
        "</p:bgRef></p:bg>" + tree_head().substr(8) +
        sp_placeholder(2, "Notes Placeholder 1", "<p:ph type=\"body\" idx=\"1\"/>", 685800, 4400550, 5486400, 3600450,
                       "<a:p><a:endParaRPr lang=\"en-US\"/></a:p>") +
        "</p:spTree></p:cSld><p:clrMap bg1=\"lt1\" tx1=\"dk1\" bg2=\"lt2\" tx2=\"dk2\" accent1=\"accent1\" "
        "accent2=\"accent2\" accent3=\"accent3\" accent4=\"accent4\" accent5=\"accent5\" accent6=\"accent6\" "
        "hlink=\"hlink\" folHlink=\"folHlink\"/></p:notesMaster>";
    Rels nm_rels;
    nm_rels.add("theme", "../theme/theme2.xml");
    CVT_TRY(zip.add("ppt/notesMasters/notesMaster1.xml", notes_master, entry));
    CVT_TRY(zip.add("ppt/notesMasters/_rels/notesMaster1.xml.rels", nm_rels.xml(), entry));
    CVT_TRY(zip.add("ppt/theme/theme2.xml", theme_xml(), entry));
  }
  for (std::size_t s = 0; s < slides.size(); ++s) {
    const std::string n = std::to_string(s + 1);
    CVT_TRY(zip.add("ppt/slides/slide" + n + ".xml", slide_xml[s], entry));
    CVT_TRY(zip.add("ppt/slides/_rels/slide" + n + ".xml.rels", slide_rels[s].xml(), entry));
    if (!notes_xml[s].empty()) {
      Rels nr;
      nr.add("notesMaster", "../notesMasters/notesMaster1.xml");
      nr.add("slide", "../slides/slide" + n + ".xml");
      CVT_TRY(zip.add("ppt/notesSlides/notesSlide" + n + ".xml", notes_xml[s], entry));
      CVT_TRY(zip.add("ppt/notesSlides/_rels/notesSlide" + n + ".xml.rels", nr.xml(), entry));
    }
  }
  util::ZipEntryOptions stored = entry;
  stored.level = 0;
  for (const auto& item : media_.items()) CVT_TRY(zip.add("ppt/media/" + item.name, item.image.bytes, stored));
  return zip.finish();
}

Result<Bytes> write_pptx(const AnyIR& value, WriteContext& ctx) {
  PptxWriter writer(value.get<ir::Document>(), ctx);
  return writer.write();
}

std::string pptx_kind(ByteView data, std::string_view filename) {
  if (!zip_mentions(data, "ppt/presentation.xml")) return {};
  const std::size_t dot = filename.rfind('.');
  const std::string ext = dot == std::string_view::npos ? std::string() : util::to_lower(filename.substr(dot + 1));
  const std::string type = ooxml_main_content_type(data, "/ppt/presentation.xml");
  // Slide shows differ from presentations only in how they open: the name decides.
  if (type.find("template.macroEnabled") != std::string::npos) return "potm";
  if (type.find("slideshow.macroEnabled") != std::string::npos) return ext == "ppsm" ? "ppsm" : "pptm";
  if (type.find("macroEnabled") != std::string::npos) return "pptm";
  if (type.find("template.main") != std::string::npos) return "potx";
  if (!type.empty()) return ext == "ppsx" ? "ppsx" : "pptx";
  if (ext == "pptm" || ext == "ppsx" || ext == "ppsm" || ext == "potx" || ext == "potm") return ext;
  return zip_mentions(data, "ppt/vbaProject.bin") ? "pptm" : "pptx";
}

void add_pptx(Registry& registry, std::string id, std::string name, std::string mime, int popularity, bool writer) {
  FormatInfo info;
  info.id = id;
  info.name = std::move(name);
  info.description = "Microsoft PowerPoint presentation (Office Open XML).";
  info.category = Category::Presentation;
  info.extensions = {id};
  info.mime = std::move(mime);
  info.popularity = popularity;
  info.sniff = [id](ByteView data, std::string_view filename) {
    const std::string kind = pptx_kind(data, filename);
    if (kind.empty()) return 0;
    if (kind == id) return 96;
    return id == "pptx" ? 90 : 0;
  };
  registry.add_format(std::move(info));
  Codec reader;
  reader.format_id = id;
  reader.kind = Kind::Document;
  reader.read = read_pptx;
  reader.read_options = document_read_options();
  registry.add_codec(std::move(reader));
  if (writer) {
    Codec codec;
    codec.format_id = id;
    codec.kind = Kind::Document;
    codec.write = write_pptx;
    codec.write_options = slide_write_options();
    registry.add_codec(std::move(codec));
  }
}

}  // namespace

void register_pptx(Registry& registry) {
  add_pptx(registry, "pptx", "PowerPoint presentation",
           "application/vnd.openxmlformats-officedocument.presentationml.presentation", 85, true);
  add_pptx(registry, "pptm", "PowerPoint macro-enabled presentation",
           "application/vnd.ms-powerpoint.presentation.macroEnabled.12", 20, false);
  add_pptx(registry, "ppsx", "PowerPoint slide show",
           "application/vnd.openxmlformats-officedocument.presentationml.slideshow", 30, false);
  add_pptx(registry, "ppsm", "PowerPoint macro-enabled slide show",
           "application/vnd.ms-powerpoint.slideshow.macroEnabled.12", 10, false);
  add_pptx(registry, "potx", "PowerPoint template",
           "application/vnd.openxmlformats-officedocument.presentationml.template", 20, false);
  add_pptx(registry, "potm", "PowerPoint macro-enabled template",
           "application/vnd.ms-powerpoint.template.macroEnabled.12", 10, false);
}

}  // namespace cvt::office
