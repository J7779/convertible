// Convertible core - office family: WordprocessingML reader.
//
// One reader for the three XML dialects of Word: the OOXML package (DOCX,
// DOCM, DOTX, DOTM), the same parts as a single "flat OPC" XML file, and
// the Word 2003 XML format (w:wordDocument). Paragraph and character
// formatting is resolved through the style hierarchy (document defaults,
// paragraph style chain, character style chain, direct formatting).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/document/document_api.hpp"
#include "families/office/office.hpp"
#include "families/office/office_common.hpp"
#include "util/base64.hpp"
#include "util/text.hpp"

namespace cvt::office {

namespace {

using util::XmlNode;

constexpr int max_nesting = 24;

// --------------------------------------------------------------------------- properties

struct RunProps {
  std::optional<bool> bold, italic, underline, strike, superscript, subscript, hidden, mono, caps;
  std::optional<int> size;  // half points
  void merge(const RunProps& o) {
    if (o.bold) bold = o.bold;
    if (o.italic) italic = o.italic;
    if (o.underline) underline = o.underline;
    if (o.strike) strike = o.strike;
    if (o.superscript) superscript = o.superscript;
    if (o.subscript) subscript = o.subscript;
    if (o.hidden) hidden = o.hidden;
    if (o.mono) mono = o.mono;
    if (o.caps) caps = o.caps;
    if (o.size) size = o.size;
  }
};

struct ParaProps {
  std::optional<int> outline;  // 0-based outline level; 9 = body text
  std::optional<std::string> num_id;
  std::optional<int> ilvl;
  bool page_break_before = false;
};

struct Style {
  std::string id;
  std::string name;
  std::string based_on;
  std::string type;
  RunProps run;
  ParaProps para;
};

struct Level {
  bool ordered = false;
  ir::ListNumbering numbering = ir::ListNumbering::Decimal;
  int start = 1;
};

bool on_off(XmlNode node) {
  if (!node) return false;
  const std::string_view v = attr(node, "val");
  return v.empty() || !(v == "0" || v == "false" || v == "off" || v == "none");
}

bool mono_font(std::string_view font) {
  const std::string f = util::to_lower(font);
  static constexpr std::array<std::string_view, 14> names = {
      "courier", "consolas", "menlo", "monaco", "lucida console", "source code", "dejavu sans mono", "liberation mono",
      "andale mono", "inconsolata", "fira code", "fira mono", "roboto mono", "monospace"};
  return std::ranges::any_of(names, [&](std::string_view n) { return f.find(n) != std::string::npos; }) ||
         f.ends_with(" mono");
}

RunProps parse_rpr(XmlNode rpr) {
  RunProps p;
  if (!rpr) return p;
  for (XmlNode n : rpr.elements()) {
    const std::string_view name = n.local_name();
    if (name == "b") p.bold = on_off(n);
    else if (name == "i") p.italic = on_off(n);
    else if (name == "u") {
      const std::string_view v = attr(n, "val");
      p.underline = !(v == "none" || v == "0");
    } else if (name == "strike" || name == "dstrike") p.strike = on_off(n);
    else if (name == "vertAlign") {
      const std::string_view v = attr(n, "val");
      p.superscript = v == "superscript";
      p.subscript = v == "subscript";
    } else if (name == "vanish" || name == "webHidden") {
      if (name == "vanish") p.hidden = on_off(n);
    } else if (name == "position") {
      if (auto v = util::parse_int(attr(n, "val"))) {
        if (*v > 0) p.superscript = true;
        if (*v < 0) p.subscript = true;
      }
    } else if (name == "rFonts") {
      const std::string_view font = !attr(n, "ascii").empty() ? attr(n, "ascii") : attr(n, "hAnsi");
      if (!font.empty()) p.mono = mono_font(font);
    } else if (name == "sz") {
      if (auto v = util::parse_int(attr(n, "val"))) p.size = static_cast<int>(std::clamp<std::int64_t>(*v, 1, 4000));
    } else if (name == "caps") p.caps = on_off(n);
  }
  return p;
}

ParaProps parse_ppr(XmlNode ppr) {
  ParaProps p;
  if (!ppr) return p;
  if (XmlNode o = child(ppr, "outlineLvl")) {
    if (auto v = util::parse_int(attr(o, "val"))) p.outline = static_cast<int>(std::clamp<std::int64_t>(*v, 0, 9));
  }
  if (XmlNode num = child(ppr, "numPr")) {
    if (XmlNode id = child(num, "numId")) p.num_id = std::string(attr(id, "val"));
    if (XmlNode lvl = child(num, "ilvl")) {
      if (auto v = util::parse_int(attr(lvl, "val"))) p.ilvl = static_cast<int>(std::clamp<std::int64_t>(*v, 0, 8));
    }
  }
  if (XmlNode list = child(ppr, "listPr")) {  // Word 2003
    if (XmlNode id = child(list, "ilfo")) p.num_id = std::string(attr(id, "val"));
    if (XmlNode lvl = child(list, "ilvl")) {
      if (auto v = util::parse_int(attr(lvl, "val"))) p.ilvl = static_cast<int>(std::clamp<std::int64_t>(*v, 0, 8));
    }
  }
  p.page_break_before = on_off(child(ppr, "pageBreakBefore"));
  return p;
}

ir::ListNumbering numbering_from(std::string_view fmt) {
  if (fmt == "lowerLetter" || fmt == "4") return ir::ListNumbering::LowerAlpha;
  if (fmt == "upperLetter" || fmt == "3") return ir::ListNumbering::UpperAlpha;
  if (fmt == "lowerRoman" || fmt == "2") return ir::ListNumbering::LowerRoman;
  if (fmt == "upperRoman" || fmt == "1") return ir::ListNumbering::UpperRoman;
  return ir::ListNumbering::Decimal;
}

// The Symbol font maps Greek letters and a few signs to the private use area.
std::string symbol_char(std::string_view font, unsigned code) {
  if (code >= 0xF000) code -= 0xF000;
  const std::string f = util::to_lower(font);
  if (f == "symbol") {
    static constexpr std::string_view upper_greek = "ABXDEFGHIJKLMNOPQRSTUVWXYZ";
    static const char32_t greek_lower[26] = {U'α', U'β', U'χ', U'δ', U'ε', U'φ', U'γ', U'η', U'ι', U'ϕ', U'κ', U'λ', U'μ',
                                             U'ν', U'ο', U'π', U'θ', U'ρ', U'σ', U'τ', U'υ', U'ϖ', U'ω', U'ξ', U'ψ', U'ζ'};
    static const char32_t greek_upper[26] = {U'Α', U'Β', U'Χ', U'Δ', U'Ε', U'Φ', U'Γ', U'Η', U'Ι', U'ϑ', U'Κ', U'Λ', U'Μ',
                                             U'Ν', U'Ο', U'Π', U'Θ', U'Ρ', U'Σ', U'Τ', U'Υ', U'ς', U'Ω', U'Ξ', U'Ψ', U'Ζ'};
    (void)upper_greek;
    if (code >= 'a' && code <= 'z') return util::utf32_to_utf8(std::u32string(1, greek_lower[code - 'a']));
    if (code >= 'A' && code <= 'Z') return util::utf32_to_utf8(std::u32string(1, greek_upper[code - 'A']));
    if (code == 0xB7) return "\xE2\x80\xA2";
    if (code == 0xA5) return "\xE2\x88\x9E";
    if (code == 0xB4) return "\xC3\x97";
    if (code == 0xAE) return "\xE2\x86\x92";
  }
  if (f.starts_with("wingdings")) {
    if (code == 0xA7 || code == 0x6C || code == 0x9F) return "\xE2\x96\xAA";
    if (code == 0xFC) return "\xE2\x9C\x93";
    if (code == 0xD8) return "\xE2\x9E\xA2";
    return "\xE2\x80\xA2";
  }
  if (code < 0x20) return {};
  return util::utf32_to_utf8(std::u32string(1, static_cast<char32_t>(code)));
}

// A HYPERLINK field instruction: `HYPERLINK "url" \l "anchor" \o "tip"`.
std::string hyperlink_from_instr(std::string_view instr) {
  instr = util::trim(instr);
  if (!util::istarts_with(instr, "HYPERLINK")) return {};
  instr.remove_prefix(9);
  std::string url;
  std::string anchor;
  bool want_anchor = false;
  std::size_t i = 0;
  while (i < instr.size()) {
    if (instr[i] == ' ') {
      ++i;
      continue;
    }
    if (instr[i] == '\\' && i + 1 < instr.size()) {
      const char sw = instr[i + 1];
      want_anchor = sw == 'l';
      if (sw != 'l') want_anchor = false;
      i += 2;
      if (sw == 'o' || sw == 't') {
        // Skip the switch argument.
        while (i < instr.size() && instr[i] == ' ') ++i;
        if (i < instr.size() && instr[i] == '"') {
          const std::size_t close = instr.find('"', i + 1);
          i = close == std::string_view::npos ? instr.size() : close + 1;
        }
      }
      continue;
    }
    std::string token;
    if (instr[i] == '"') {
      const std::size_t close = instr.find('"', i + 1);
      token = std::string(instr.substr(i + 1, close == std::string_view::npos ? std::string_view::npos : close - i - 1));
      i = close == std::string_view::npos ? instr.size() : close + 1;
    } else {
      const std::size_t end = instr.find(' ', i);
      token = std::string(instr.substr(i, end == std::string_view::npos ? std::string_view::npos : end - i));
      i = end == std::string_view::npos ? instr.size() : end;
    }
    if (want_anchor) {
      anchor = token;
      want_anchor = false;
    } else if (url.empty()) {
      url = token;
    }
  }
  if (!anchor.empty()) url += "#" + anchor;
  return url;
}

// --------------------------------------------------------------------------- the reader

class WordReader {
 public:
  WordReader(const ReadContext& ctx, ReadSettings settings) : ctx_(ctx), settings_(settings) {}

  Result<ir::Document> read_package(Package& package);
  Result<ir::Document> read_word2003(const util::XmlDocument& xml);

 private:
  // Content parsing.
  ir::Blocks parse_container(XmlNode container, int depth);
  void walk_blocks(XmlNode node, FlowBuilder& flow, int depth);
  void paragraph(XmlNode p, FlowBuilder& flow, int depth);
  ir::Block table(XmlNode tbl, int depth);
  void runs(XmlNode node, InlineBuilder& out, RunFormat format, const RunProps& para_run, int depth,
            ir::Blocks& extra, bool& page_break);
  void run(XmlNode r, InlineBuilder& out, const RunFormat& base, const RunProps& para_run, int depth,
           ir::Blocks& extra, bool& page_break);
  void drawing(XmlNode node, InlineBuilder& out, const RunFormat& format, int depth, ir::Blocks& extra);
  std::optional<ir::EncodedImage> image_part(std::string_view rel_id);
  std::optional<ir::EncodedImage> image_from_vml(XmlNode node);
  ir::Inline note_ref(std::string_view kind, std::string_view id);

  // Styles and numbering.
  void load_styles(XmlNode root);
  void load_numbering(XmlNode root);
  void load_word2003_lists(XmlNode lists);
  const Style* style(std::string_view id) const;
  RunProps style_run(std::string_view id, int guard = 0) const;
  ParaProps style_para(std::string_view id, int guard = 0) const;
  std::string style_name(std::string_view id) const;
  std::optional<Level> list_level(const std::string& num_id, int ilvl) const;

  void load_notes(XmlNode root, std::string_view kind);
  void load_comments(XmlNode root);
  void read_core_properties(Package& package);

  const ReadContext& ctx_;
  ReadSettings settings_;
  Package* package_ = nullptr;
  std::string main_part_;
  std::map<std::string, Package::Relationship> rels_;
  std::map<std::string, Style> styles_;
  std::string default_para_style_;
  RunProps defaults_;
  std::map<std::string, std::vector<Level>> abstract_;
  std::map<std::string, std::string> num_to_abstract_;
  std::map<std::string, std::map<int, int>> start_overrides_;
  std::map<std::string, ir::Blocks> notes_;       // "f12", "e3", "c4" -> body
  std::map<std::string, Bytes> bin_data_;         // Word 2003 embedded pictures
  std::vector<ir::Footnote> footnotes_;
  std::set<std::string> used_notes_;
  int note_counter_ = 0;
  bool word2003_ = false;
  ir::Document doc_;

  // Complex field state (fields may span runs and paragraphs).
  struct Field {
    std::string instr;
    bool result = false;
    std::string link;
  };
  std::vector<Field> fields_;
  std::string pending_bookmark_;
};

const Style* WordReader::style(std::string_view id) const {
  const auto it = styles_.find(std::string(id));
  return it == styles_.end() ? nullptr : &it->second;
}

RunProps WordReader::style_run(std::string_view id, int guard) const {
  const Style* s = style(id);
  if (!s || guard > 16) return {};
  RunProps out = s->based_on.empty() ? RunProps{} : style_run(s->based_on, guard + 1);
  out.merge(s->run);
  return out;
}

ParaProps WordReader::style_para(std::string_view id, int guard) const {
  const Style* s = style(id);
  if (!s || guard > 16) return {};
  ParaProps out = s->based_on.empty() ? ParaProps{} : style_para(s->based_on, guard + 1);
  if (s->para.outline) out.outline = s->para.outline;
  if (s->para.num_id) out.num_id = s->para.num_id;
  if (s->para.ilvl) out.ilvl = s->para.ilvl;
  if (s->para.page_break_before) out.page_break_before = true;
  return out;
}

std::string WordReader::style_name(std::string_view id) const {
  const Style* s = style(id);
  return s ? (s->name.empty() ? s->id : s->name) : std::string(id);
}

void WordReader::load_styles(XmlNode root) {
  if (XmlNode defaults = child(root, "docDefaults")) {
    defaults_ = parse_rpr(child(child(defaults, "rPrDefault"), "rPr"));
  }
  for (XmlNode s : root.children_local("style")) {
    Style style;
    style.id = std::string(attr(s, "styleId"));
    style.type = std::string(attr(s, "type"));
    style.name = std::string(attr(child(s, "name"), "val"));
    style.based_on = std::string(attr(child(s, "basedOn"), "val"));
    style.run = parse_rpr(child(s, "rPr"));
    style.para = parse_ppr(child(s, "pPr"));
    if (on_off(s.attribute_local("default") ? s : XmlNode{}) && style.type == "paragraph") {
      const std::string_view def = attr(s, "default");
      if (def == "1" || def == "true" || def == "on") default_para_style_ = style.id;
    }
    if (!style.id.empty()) styles_[style.id] = std::move(style);
  }
}

void WordReader::load_numbering(XmlNode root) {
  for (XmlNode a : root.children_local("abstractNum")) {
    std::vector<Level> levels(9);
    for (XmlNode lvl : a.children_local("lvl")) {
      const auto index = util::parse_int(attr(lvl, "ilvl"));
      if (!index || *index < 0 || *index > 8) continue;
      Level level;
      const std::string_view fmt = attr(child(lvl, "numFmt"), "val");
      level.ordered = !(fmt == "bullet" || fmt == "none" || fmt.empty());
      level.numbering = numbering_from(fmt);
      if (auto start = util::parse_int(attr(child(lvl, "start"), "val"))) {
        level.start = static_cast<int>(std::clamp<std::int64_t>(*start, 0, 1000000));
      }
      levels[static_cast<std::size_t>(*index)] = level;
    }
    abstract_[std::string(attr(a, "abstractNumId"))] = std::move(levels);
  }
  for (XmlNode n : root.children_local("num")) {
    const std::string id(attr(n, "numId"));
    num_to_abstract_[id] = std::string(attr(child(n, "abstractNumId"), "val"));
    for (XmlNode o : n.children_local("lvlOverride")) {
      const auto index = util::parse_int(attr(o, "ilvl"));
      const auto start = util::parse_int(attr(child(o, "startOverride"), "val"));
      if (index && start) start_overrides_[id][static_cast<int>(*index)] = static_cast<int>(*start);
    }
  }
}

void WordReader::load_word2003_lists(XmlNode lists) {
  for (XmlNode def : lists.children_local("listDef")) {
    std::vector<Level> levels(9);
    for (XmlNode lvl : def.children_local("lvl")) {
      const auto index = util::parse_int(attr(lvl, "ilvl"));
      if (!index || *index < 0 || *index > 8) continue;
      Level level;
      const std::string_view nfc = attr(child(lvl, "nfc"), "val");
      level.ordered = !(nfc == "23" || nfc == "255");
      level.numbering = numbering_from(nfc);
      if (auto start = util::parse_int(attr(child(lvl, "start"), "val"))) {
        level.start = static_cast<int>(std::clamp<std::int64_t>(*start, 0, 1000000));
      }
      levels[static_cast<std::size_t>(*index)] = level;
    }
    abstract_[std::string(attr(def, "listDefId"))] = std::move(levels);
  }
  for (XmlNode list : lists.children_local("list")) {
    num_to_abstract_[std::string(attr(list, "ilfo"))] = std::string(attr(child(list, "ilst"), "val"));
  }
}

std::optional<Level> WordReader::list_level(const std::string& num_id, int ilvl) const {
  if (num_id.empty() || num_id == "0") return std::nullopt;
  const auto a = num_to_abstract_.find(num_id);
  if (a == num_to_abstract_.end()) return std::nullopt;
  const auto levels = abstract_.find(a->second);
  if (levels == abstract_.end()) return std::nullopt;
  Level level = levels->second[static_cast<std::size_t>(std::clamp(ilvl, 0, 8))];
  if (auto o = start_overrides_.find(num_id); o != start_overrides_.end()) {
    if (auto s = o->second.find(ilvl); s != o->second.end()) level.start = s->second;
  }
  return level;
}

std::optional<ir::EncodedImage> WordReader::image_part(std::string_view rel_id) {
  if (word2003_) {
    const auto it = bin_data_.find(std::string(rel_id));
    if (it == bin_data_.end()) return std::nullopt;
    ir::EncodedImage image;
    image.bytes = it->second;
    image.mime = document::sniff_image_mime(image.bytes);
    if (image.mime.empty()) image.mime = mime_for_extension(rel_id);
    return image;
  }
  const auto it = rels_.find(std::string(rel_id));
  if (it == rels_.end() || !package_) return std::nullopt;
  ir::EncodedImage image;
  if (it->second.external) {
    image.url = it->second.target;
    return image;
  }
  auto bytes = package_->try_read(it->second.target);
  if (!bytes) return std::nullopt;
  image.bytes = std::move(*bytes);
  image.mime = package_->content_type(it->second.target);
  if (image.mime.empty() || !image.mime.starts_with("image/")) {
    const std::string sniffed = document::sniff_image_mime(image.bytes);
    image.mime = sniffed.empty() ? mime_for_extension(it->second.target) : sniffed;
  }
  return image;
}

std::optional<ir::EncodedImage> WordReader::image_from_vml(XmlNode node) {
  for (XmlNode d : node.descendants()) {
    if (!d.is_element()) continue;
    if (d.local_name() == "binData") {
      std::string data;
      for (char c : d.text()) {
        if (!util::is_ascii_space(c)) data.push_back(c);
      }
      if (auto decoded = util::base64_decode(data)) bin_data_[std::string(attr(d, "name"))] = std::move(*decoded);
    }
  }
  for (XmlNode d : node.descendants()) {
    if (!d.is_element() || d.local_name() != "imagedata") continue;
    std::string_view id = d.attribute("r:id").value();
    if (id.empty()) id = attr(d, "id");
    if (!id.empty() && !word2003_) return image_part(id);
    const std::string_view src = attr(d, "src");
    if (!src.empty()) return image_part(src);
  }
  return std::nullopt;
}

ir::Inline WordReader::note_ref(std::string_view kind, std::string_view id) {
  const std::string key = std::string(kind) + std::string(id);
  const auto it = notes_.find(key);
  if (it == notes_.end()) return ir::Text{};
  std::string label = std::to_string(++note_counter_);
  if (!used_notes_.contains(key)) {
    used_notes_.insert(key);
    footnotes_.push_back(ir::Footnote{label, it->second});
  }
  ir::Link link;
  link.url = "#fn-" + label;
  link.note = label;
  link.children.push_back(ir::Text{label});
  return link;
}

void WordReader::drawing(XmlNode node, InlineBuilder& out, const RunFormat& format, int depth, ir::Blocks& extra) {
  // Text boxes: their paragraphs follow the paragraph that anchors them.
  for (XmlNode d : node.descendants()) {
    if (d.is_element() && d.local_name() == "txbxContent") {
      ir::Blocks inner = parse_container(d, depth + 1);
      for (ir::Block& b : inner) extra.push_back(std::move(b));
      break;
    }
  }
  XmlNode blip = descendant(node, "blip");
  if (!blip) return;
  std::string_view rel = blip.attribute("r:embed").value();
  if (rel.empty()) rel = attr(blip, "embed");
  if (rel.empty()) rel = attr(blip, "link");
  auto image = image_part(rel);
  if (!image) return;
  XmlNode doc_pr = descendant(node, "docPr");
  std::string alt(attr(doc_pr, "descr"));
  if (alt.empty()) alt = std::string(attr(doc_pr, "title"));
  ir::InlineImage picture{std::move(*image), alt};
  picture.title = std::string(attr(doc_pr, "title"));
  RunFormat plain;
  plain.link = format.link;
  out.node(std::move(picture), plain);
}

void WordReader::run(XmlNode r, InlineBuilder& out, const RunFormat& base, const RunProps& para_run, int depth,
                     ir::Blocks& extra, bool& page_break) {
  RunProps props = para_run;
  XmlNode rpr = child(r, "rPr");
  if (XmlNode rs = child(rpr, "rStyle")) props.merge(style_run(attr(rs, "val")));
  const RunProps direct = parse_rpr(rpr);
  props.merge(direct);
  if (props.hidden.value_or(false) && !settings_.hidden) return;
  RunFormat format = base;
  format.bold = format.bold || props.bold.value_or(false);
  format.italic = format.italic || props.italic.value_or(false);
  format.underline = format.underline || props.underline.value_or(false);
  format.strike = format.strike || props.strike.value_or(false);
  format.superscript = props.superscript.value_or(false);
  format.subscript = props.subscript.value_or(false);
  format.code = props.mono.value_or(false);
  if (rpr) {
    const std::string_view rstyle = attr(child(rpr, "rStyle"), "val");
    const std::string sname = util::to_lower(style_name(rstyle));
    if (sname.find("verbatim") != std::string::npos || sname.find("code") != std::string::npos) format.code = true;
    if (sname.find("hyperlink") != std::string::npos) format.underline = false;
  }
  if (!fields_.empty() && !fields_.back().link.empty() && fields_.back().result) {
    format.link = fields_.back().link;
    format.underline = false;
  }
  if (!format.link.empty()) format.underline = false;

  for (XmlNode c : r.elements()) {
    const std::string_view name = c.local_name();
    const bool in_instr = !fields_.empty() && !fields_.back().result;
    if (name == "fldChar") {
      const std::string_view type = attr(c, "fldCharType");
      if (type == "begin") {
        if (fields_.size() < 32) fields_.push_back(Field{});
      } else if (type == "separate") {
        if (!fields_.empty()) {
          fields_.back().result = true;
          fields_.back().link = hyperlink_from_instr(fields_.back().instr);
          if (!fields_.back().link.empty()) {
            format.link = fields_.back().link;
            format.underline = false;
          }
        }
      } else if (type == "end") {
        if (!fields_.empty()) fields_.pop_back();
        format.link = base.link;
      }
      continue;
    }
    if (name == "instrText") {
      if (!fields_.empty()) fields_.back().instr += c.text();
      continue;
    }
    if (in_instr) continue;
    if (name == "t") {
      std::string text = c.text();
      if (props.caps.value_or(false)) text = util::to_upper(text);
      out.text(text, format);
    } else if (name == "delText") {
      if (settings_.tracked_changes) out.text(c.text(), format);
    } else if (name == "tab" || name == "ptab") {
      out.text("\t", format);
    } else if (name == "br" || name == "cr") {
      const std::string_view type = attr(c, "type");
      if (type == "page") {
        page_break = true;
      } else if (type != "column") {
        out.line_break(format);
      }
    } else if (name == "noBreakHyphen") {
      out.text("\xE2\x80\x91", format);
    } else if (name == "sym") {
      if (auto code = util::parse_int(attr(c, "char"), 16)) {
        out.text(symbol_char(attr(c, "font"), static_cast<unsigned>(*code)), format);
      }
    } else if (name == "drawing") {
      drawing(c, out, format, depth, extra);
    } else if (name == "pict" || name == "object") {
      for (XmlNode d : c.descendants()) {
        if (d.is_element() && d.local_name() == "txbxContent") {
          for (ir::Block& b : parse_container(d, depth + 1)) extra.push_back(std::move(b));
          break;
        }
      }
      if (auto image = image_from_vml(c)) {
        std::string alt;
        if (XmlNode shape = descendant(c, "shape")) alt = std::string(attr(shape, "alt"));
        if (XmlNode data = descendant(c, "imagedata"); alt.empty() && data) alt = std::string(attr(data, "title"));
        RunFormat plain;
        plain.link = format.link;
        out.node(ir::InlineImage{std::move(*image), alt}, plain);
      }
    } else if (name == "footnoteReference" || name == "endnoteReference") {
      if (settings_.notes) {
        const ir::Inline ref = note_ref(name == "footnoteReference" ? "f" : "e", attr(c, "id"));
        if (!ref.is<ir::Text>()) out.node(ref, RunFormat{});
      }
    } else if (name == "footnote" || name == "endnote") {  // Word 2003: inline note bodies
      if (settings_.notes) {
        const std::string key = "inline" + std::to_string(notes_.size());
        notes_[key] = parse_container(c, depth + 1);
        const ir::Inline ref = note_ref("", key);
        if (!ref.is<ir::Text>()) out.node(ref, RunFormat{});
      }
    } else if (name == "commentReference") {
      if (settings_.comments) {
        const ir::Inline ref = note_ref("c", attr(c, "id"));
        if (!ref.is<ir::Text>()) out.node(ref, RunFormat{});
      }
    } else if (name == "AlternateContent") {
      XmlNode choice = child(c, "Choice");
      XmlNode chosen = choice ? choice : child(c, "Fallback");
      XmlNode fake = chosen;
      // Treat the chosen branch as the content of this run.
      ir::Blocks more;
      for (XmlNode d : fake.elements()) {
        if (d.local_name() == "drawing") drawing(d, out, format, depth, extra);
        if (d.local_name() == "pict") {
          if (auto image = image_from_vml(d)) out.node(ir::InlineImage{std::move(*image), {}}, RunFormat{});
        }
      }
    } else if (name == "ruby") {
      if (XmlNode base_text = child(c, "rubyBase")) runs(base_text, out, format, para_run, depth + 1, extra, page_break);
    } else if (name == "softHyphen" || name == "lastRenderedPageBreak" || name == "annotationRef" ||
               name == "footnoteRef" || name == "endnoteRef" || name == "separator" ||
               name == "continuationSeparator" || name == "rPr") {
      // Nothing to show.
    }
  }
}

void WordReader::runs(XmlNode node, InlineBuilder& out, RunFormat format, const RunProps& para_run, int depth,
                      ir::Blocks& extra, bool& page_break) {
  if (depth > max_nesting) return;
  for (XmlNode c : node.elements()) {
    const std::string_view name = c.local_name();
    if (name == "r") {
      run(c, out, format, para_run, depth, extra, page_break);
    } else if (name == "hyperlink" || name == "hlink") {
      RunFormat linked = format;
      std::string_view rel = c.attribute("r:id").value();
      if (rel.empty()) rel = attr(c, "id");
      std::string target;
      if (!rel.empty()) {
        if (auto it = rels_.find(std::string(rel)); it != rels_.end()) target = it->second.target;
      }
      if (target.empty()) target = std::string(attr(c, "dest"));
      std::string_view anchor = attr(c, "anchor");
      if (anchor.empty()) anchor = attr(c, "bookmark");
      if (!anchor.empty()) target += "#" + std::string(anchor);
      linked.link = target;
      linked.underline = false;
      runs(c, out, linked, para_run, depth + 1, extra, page_break);
    } else if (name == "fldSimple") {
      RunFormat linked = format;
      const std::string link = hyperlink_from_instr(attr(c, "instr"));
      if (!link.empty()) linked.link = link;
      runs(c, out, linked, para_run, depth + 1, extra, page_break);
    } else if (name == "ins" || name == "moveTo") {
      RunFormat changed = format;
      if (settings_.tracked_changes) changed.underline = true;
      runs(c, out, changed, para_run, depth + 1, extra, page_break);
    } else if (name == "del" || name == "moveFrom") {
      if (settings_.tracked_changes) {
        RunFormat changed = format;
        changed.strike = true;
        runs(c, out, changed, para_run, depth + 1, extra, page_break);
      }
    } else if (name == "smartTag" || name == "customXml" || name == "sdtContent" || name == "dir" || name == "bdo" ||
               name == "Choice") {
      runs(c, out, format, para_run, depth + 1, extra, page_break);
    } else if (name == "sdt") {
      runs(child(c, "sdtContent"), out, format, para_run, depth + 1, extra, page_break);
    } else if (name == "AlternateContent") {
      XmlNode chosen = child(c, "Choice") ? child(c, "Choice") : child(c, "Fallback");
      runs(chosen, out, format, para_run, depth + 1, extra, page_break);
    } else if (name == "bookmarkStart") {
      const std::string_view bookmark = attr(c, "name");
      if (pending_bookmark_.empty() && !bookmark.empty() && bookmark != "_GoBack") {
        pending_bookmark_ = std::string(bookmark);
      }
    } else if (name == "oMath" || name == "oMathPara") {
      // Office math: keep the text of the formula.
      std::string text;
      for (XmlNode t : c.descendants()) {
        if (t.is_element() && t.local_name() == "t") text += t.text();
      }
      if (!text.empty()) out.node(ir::Code{text, ir::MathMode::None}, RunFormat{});
    }
  }
}

void WordReader::paragraph(XmlNode p, FlowBuilder& flow, int depth) {
  XmlNode ppr = child(p, "pPr");
  std::string style_id(attr(child(ppr, "pStyle"), "val"));
  if (style_id.empty()) style_id = default_para_style_;
  ParaProps props = style_para(style_id);
  const ParaProps direct = parse_ppr(ppr);
  if (direct.outline) props.outline = direct.outline;
  if (direct.num_id) props.num_id = direct.num_id;
  if (direct.ilvl) props.ilvl = direct.ilvl;
  if (direct.page_break_before) props.page_break_before = true;

  const std::string sname = style_name(style_id);
  const std::string lname = util::to_lower(sname);

  // Heading level: outline level, then style name, then (lenient) formatting.
  int heading = 0;
  if (props.outline && *props.outline < 9) heading = std::min(*props.outline + 1, 6);
  if (heading == 0) heading = heading_level_from_style_name(sname);
  if (heading == 0) heading = heading_level_from_style_name(style_id);
  const bool is_code_style = lname.find("code") != std::string::npos || lname.find("preformatted") != std::string::npos ||
                             lname.find("source text") != std::string::npos || lname.find("verbatim") != std::string::npos ||
                             lname == "plain text";
  const bool is_quote = lname == "quote" || lname == "intense quote" || lname == "block text" ||
                        lname == "block quote" || lname == "blockquote" || lname == "quotations";
  const bool is_caption = lname.find("caption") != std::string::npos;
  const bool is_title = lname == "title";

  RunProps para_run = defaults_;
  para_run.merge(style_run(style_id));
  if (heading != 0 || is_quote || is_caption || is_code_style) {
    // The look of these paragraph styles is structure, not emphasis.
    para_run.bold.reset();
    para_run.italic.reset();
    para_run.underline.reset();
    para_run.mono.reset();
    para_run.caps.reset();
  }

  if (props.page_break_before) flow.block(ir::PageBreak{});

  InlineBuilder inlines;
  ir::Blocks extra;
  bool page_break = false;
  pending_bookmark_.clear();
  runs(p, inlines, RunFormat{}, para_run, depth, extra, page_break);
  ir::Attributes attributes;
  if (!pending_bookmark_.empty()) attributes.id = pending_bookmark_;
  const std::string plain = inlines.plain();
  if (is_title && doc_.metadata.title.empty()) doc_.metadata.title = std::string(util::trim(plain));

  std::optional<Level> level;
  if (props.num_id) level = list_level(*props.num_id, props.ilvl.value_or(0));
  if (heading != 0 && level && heading_level_from_style_name(sname) == 0 && !props.outline) level.reset();

  ir::Inlines content = inlines.take();
  if (!is_code_style) trim_inlines(content);
  if (heading == 0 && !settings_.strict_headings && !level && !is_code_style && !plain.empty() &&
      plain.size() < 120 && plain.find('\n') == std::string::npos && content.size() == 1 &&
      content.front().is<ir::Strong>()) {
    // A short, entirely bold line in a large font reads as a heading.
    int size = para_run.size.value_or(22);
    for (XmlNode r : p.children_local("r")) {
      if (auto sz = parse_rpr(child(r, "rPr")).size) size = std::max(size, *sz);
    }
    if (size >= 32) {
      heading = size >= 40 ? 1 : 2;
      ir::Inlines inner = std::move(content.front().get_if<ir::Strong>()->children);
      content = std::move(inner);
    }
  }
  if (heading != 0) {
    if (!content.empty()) flow.block(ir::Block(ir::Heading{heading, std::move(content)}, std::move(attributes)));
  } else if (level) {
    FlowBuilder::ListInfo info;
    info.list_id = *props.num_id;
    info.level = props.ilvl.value_or(0);
    info.ordered = level->ordered;
    info.numbering = level->numbering;
    info.start = level->start;
    flow.list_item(info, ir::Paragraph{std::move(content)});
  } else if (is_code_style) {
    // Consecutive code paragraphs are merged by parse_container.
    flow.block(ir::CodeBlock{{}, plain});
  } else if (is_quote) {
    if (!content.empty()) flow.block(ir::BlockQuote{{ir::Paragraph{std::move(content)}}});
  } else if (!content.empty()) {
    if (content.size() == 1 && content.front().is<ir::InlineImage>()) {
      auto& image = *content.front().get_if<ir::InlineImage>();
      flow.block(ir::ImageBlock{std::move(image.source), image.alt, {}, image.title});
    } else if (is_caption) {
      flow.block(ir::Block(ir::Paragraph{std::move(content)}, ir::Attributes{{}, {"caption"}, {}}));
    } else {
      flow.block(ir::Block(ir::Paragraph{std::move(content)}, std::move(attributes)));
    }
  }
  for (ir::Block& b : extra) flow.block(std::move(b));
  if (page_break) flow.block(ir::PageBreak{});
}

ir::Block WordReader::table(XmlNode tbl, int depth) {
  ir::TableBlock out;
  struct Pending {
    std::size_t row;
    std::size_t cell;
  };
  std::map<std::size_t, Pending> open_merges;  // grid column -> cell starting a vertical merge
  bool header_rows = true;
  for (XmlNode tr : tbl.children_local("tr")) {
    ir::TableRow row;
    XmlNode trpr = child(tr, "trPr");
    row.header = header_rows && on_off(child(trpr, "tblHeader"));
    if (!row.header) header_rows = false;
    std::size_t column = 0;
    if (XmlNode before = child(trpr, "gridBefore")) {
      column += static_cast<std::size_t>(std::clamp<std::int64_t>(util::parse_int(attr(before, "val")).value_or(0), 0, 64));
    }
    std::vector<XmlNode> cells;
    for (XmlNode c : tr.elements()) {
      if (c.local_name() == "tc") {
        cells.push_back(c);
      } else if (c.local_name() == "sdt" || c.local_name() == "customXml") {
        for (XmlNode d : child(c, "sdtContent") ? child(c, "sdtContent").elements() : c.elements()) {
          if (d.local_name() == "tc") cells.push_back(d);
        }
      }
    }
    for (XmlNode tc : cells) {
      XmlNode tcpr = child(tc, "tcPr");
      const int span = static_cast<int>(std::clamp<std::int64_t>(
          util::parse_int(attr(child(tcpr, "gridSpan"), "val")).value_or(1), 1, 1000));
      XmlNode vmerge = child(tcpr, "vMerge");
      if (!vmerge) vmerge = child(tcpr, "vmerge");
      const std::string_view merge = attr(vmerge, "val");
      if (vmerge && merge != "restart") {
        // Continuation of the cell above.
        if (auto it = open_merges.find(column); it != open_merges.end()) {
          out.rows[it->second.row].cells[it->second.cell].rowspan += 1;
        }
        column += static_cast<std::size_t>(span);
        continue;
      }
      ir::TableCell cell;
      cell.colspan = span;
      const ir::Blocks blocks = parse_container(tc, depth + 1);
      for (const ir::Block& b : blocks) {
        ir::Inlines piece;
        if (const auto* para = b.get_if<ir::Paragraph>()) {
          piece = para->content;
        } else if (const auto* heading = b.get_if<ir::Heading>()) {
          piece = heading->content;
        } else if (const auto* image = b.get_if<ir::ImageBlock>()) {
          piece.push_back(ir::InlineImage{image->source, image->alt});
        } else {
          // Nested tables, lists ...: their text, line by line.
          std::string text = ir::plain_text(ir::Blocks{b});
          while (!text.empty() && text.back() == '\n') text.pop_back();
          piece = text_with_breaks(text);
        }
        if (piece.empty()) continue;
        if (!cell.content.empty()) cell.content.push_back(ir::LineBreak{});
        for (ir::Inline& x : piece) cell.content.push_back(std::move(x));
      }
      if (vmerge && merge == "restart") {
        open_merges[column] = Pending{out.rows.size(), row.cells.size()};
      } else {
        open_merges.erase(column);
      }
      row.cells.push_back(std::move(cell));
      column += static_cast<std::size_t>(span);
    }
    out.rows.push_back(std::move(row));
  }
  unwrap_header_emphasis(out);
  return out;
}

void WordReader::walk_blocks(XmlNode node, FlowBuilder& flow, int depth) {
  if (depth > max_nesting) return;
  for (XmlNode c : node.elements()) {
    const std::string_view name = c.local_name();
    if (name == "p") {
      paragraph(c, flow, depth);
    } else if (name == "tbl") {
      flow.block(table(c, depth + 1));
    } else if (name == "sdt") {
      walk_blocks(child(c, "sdtContent"), flow, depth + 1);
    } else if (name == "customXml" || name == "sect" || name == "sub-section" || name == "ins" || name == "moveTo" ||
               name == "smartTag" || name == "sdtContent") {
      walk_blocks(c, flow, depth + 1);
    } else if (name == "AlternateContent") {
      XmlNode chosen = child(c, "Choice") ? child(c, "Choice") : child(c, "Fallback");
      walk_blocks(chosen, flow, depth + 1);
    } else if (name == "del" || name == "moveFrom") {
      if (settings_.tracked_changes) walk_blocks(c, flow, depth + 1);
    }
  }
}

ir::Blocks WordReader::parse_container(XmlNode container, int depth) {
  FlowBuilder flow;
  if (depth > max_nesting) return {};
  walk_blocks(container, flow, depth);
  ir::Blocks blocks = flow.take();
  // Merge consecutive code blocks and quotes.
  ir::Blocks merged;
  for (ir::Block& b : blocks) {
    if (!merged.empty()) {
      if (auto* code = b.get_if<ir::CodeBlock>(); code && merged.back().is<ir::CodeBlock>()) {
        auto& prev = *merged.back().get_if<ir::CodeBlock>();
        prev.code += "\n" + code->code;
        continue;
      }
      if (auto* para = b.get_if<ir::Paragraph>(); para && b.attr.has_class("caption") && merged.back().is<ir::ImageBlock>()) {
        auto& image = *merged.back().get_if<ir::ImageBlock>();
        if (image.caption.empty()) {
          image.caption = std::string(util::trim(ir::plain_text(para->content)));
          continue;
        }
      }
      if (auto* quote = b.get_if<ir::BlockQuote>(); quote && merged.back().is<ir::BlockQuote>()) {
        auto& prev = *merged.back().get_if<ir::BlockQuote>();
        for (ir::Block& inner : quote->blocks) prev.blocks.push_back(std::move(inner));
        continue;
      }
    }
    merged.push_back(std::move(b));
  }
  return merged;
}

void WordReader::load_notes(XmlNode root, std::string_view kind) {
  for (XmlNode n : root.elements()) {
    if (n.local_name() != "footnote" && n.local_name() != "endnote") continue;
    const std::string_view type = attr(n, "type");
    if (type == "separator" || type == "continuationSeparator" || type == "continuationNotice") continue;
    notes_[std::string(kind) + std::string(attr(n, "id"))] = parse_container(n, 1);
  }
}

void WordReader::load_comments(XmlNode root) {
  for (XmlNode n : root.children_local("comment")) {
    ir::Blocks body = parse_container(n, 1);
    if (settings_.metadata) {
      const std::string author(attr(n, "author"));
      if (!author.empty() && !body.empty()) {
        if (auto* para = body.front().get_if<ir::Paragraph>()) {
          para->content.insert(para->content.begin(), ir::Text{author + ": "});
        }
      }
    }
    notes_["c" + std::string(attr(n, "id"))] = std::move(body);
  }
}

void WordReader::read_core_properties(Package& package) {
  std::string core = "docProps/core.xml";
  for (const auto& rel : package.relationships("")) {
    if (rel.type.ends_with("/core-properties")) core = rel.target;
  }
  auto xml = package.xml(core, false);
  if (!xml) return;
  for (XmlNode n : xml->root().elements()) {
    const std::string_view name = n.local_name();
    std::string value(util::trim(n.text()));
    if (value.empty()) continue;
    if (name == "title") doc_.metadata.title = value;
    else if (name == "creator") doc_.metadata.author = value;
    else if (name == "language") doc_.metadata.language = value;
    else if (name == "created") doc_.metadata.date = value;
    else if (name == "subject" || name == "description" || name == "keywords" || name == "category" ||
             name == "lastModifiedBy" || name == "modified" || name == "revision")
      doc_.metadata.extra[std::string(name)] = value;
  }
}

Result<ir::Document> WordReader::read_package(Package& package) {
  package_ = &package;
  main_part_ = "word/document.xml";
  for (const auto& rel : package.relationships("")) {
    if (rel.type.ends_with("/officeDocument") && !rel.external) main_part_ = rel.target;
  }
  if (!package.has(main_part_)) {
    for (const std::string& name : package.names()) {
      if (name.ends_with("document.xml") || name.ends_with("document2.xml")) main_part_ = name;
    }
  }
  CVT_TRY_ASSIGN(util::XmlDocument main, package.xml(main_part_));
  XmlNode body = child(main.root(), "body");
  if (main.root().local_name() != "document" || !body) {
    return fail(Status::Decode, "the package has no Word document body");
  }
  for (auto& rel : package.relationships(main_part_)) {
    const std::string type = rel.type.substr(rel.type.rfind('/') + 1);
    if (type == "styles") {
      if (auto x = package.xml(rel.target, false)) load_styles(x->root());
    } else if (type == "numbering") {
      if (auto x = package.xml(rel.target, false)) load_numbering(x->root());
    }
    rels_[rel.id] = std::move(rel);
  }
  CVT_TRY(ctx_.checkpoint(0.2, "Reading styles"));
  // A copy: note bodies temporarily add their own relationships to rels_ and
  // restore it afterwards, which must not happen under a live iterator (the
  // map assignment reuses and reorders nodes: an endless loop with libstdc++).
  const auto main_rels = rels_;
  for (const auto& [id, rel] : main_rels) {
    const std::string type = rel.type.substr(rel.type.rfind('/') + 1);
    if (type == "footnotes" || type == "endnotes") {
      // Note bodies may reference pictures through their own relationships.
      const auto saved = rels_;
      for (auto& r : package.relationships(rel.target)) rels_[r.id] = std::move(r);
      if (auto x = package.xml(rel.target)) load_notes(x->root(), type == "footnotes" ? "f" : "e");
      rels_ = saved;
    } else if (type == "comments" && settings_.comments) {
      if (auto x = package.xml(rel.target)) load_comments(x->root());
    }
  }
  read_core_properties(package);

  ir::Blocks headers;
  ir::Blocks footers;
  if (settings_.headers) {
    std::set<std::string> seen;
    for (XmlNode sect : main.root().find_all("w:body/w:sectPr")) {
      for (XmlNode ref : sect.elements()) {
        const bool header = ref.local_name() == "headerReference";
        if (!header && ref.local_name() != "footerReference") continue;
        std::string_view id = ref.attribute("r:id").value();
        if (id.empty()) id = attr(ref, "id");
        const auto it = rels_.find(std::string(id));
        if (it == rels_.end() || !seen.insert(it->second.target).second) continue;
        if (auto x = package.xml(it->second.target)) {
          for (ir::Block& b : parse_container(x->root(), 1)) (header ? headers : footers).push_back(std::move(b));
        }
      }
    }
  }

  doc_.blocks = parse_container(body, 0);
  CVT_TRY(ctx_.checkpoint(0.9, "Reading text"));
  if (!headers.empty()) doc_.blocks.insert(doc_.blocks.begin(), headers.begin(), headers.end());
  for (ir::Block& b : footers) doc_.blocks.push_back(std::move(b));
  doc_.footnotes = std::move(footnotes_);
  finish_metadata(doc_, settings_);
  return std::move(doc_);
}

Result<ir::Document> WordReader::read_word2003(const util::XmlDocument& xml) {
  word2003_ = true;
  XmlNode root = xml.root();
  if (XmlNode styles = child(root, "styles")) load_styles(styles);
  if (XmlNode lists = child(root, "lists")) load_word2003_lists(lists);
  if (XmlNode props = child(root, "DocumentProperties")) {
    doc_.metadata.title = std::string(util::trim(child(props, "Title").text()));
    doc_.metadata.author = std::string(util::trim(child(props, "Author").text()));
    const std::string created(util::trim(child(props, "Created").text()));
    if (!created.empty()) doc_.metadata.date = created;
  }
  // Pictures are stored inline as w:binData; collect them first.
  for (XmlNode d : root.descendants()) {
    if (d.is_element() && d.local_name() == "binData") {
      std::string data;
      for (char c : d.text()) {
        if (!util::is_ascii_space(c)) data.push_back(c);
      }
      if (auto decoded = util::base64_decode(data)) bin_data_[std::string(attr(d, "name"))] = std::move(*decoded);
    }
  }
  XmlNode body = child(root, "body");
  if (!body) return fail(Status::Decode, "the Word XML document has no body");
  doc_.blocks = parse_container(body, 0);
  doc_.footnotes = std::move(footnotes_);
  finish_metadata(doc_, settings_);
  return std::move(doc_);
}

// --------------------------------------------------------------------------- codecs

Result<AnyIR> read_docx(ReadContext& ctx) {
  const std::string_view head = as_chars(ctx.input).substr(0, 8);
  if (head.starts_with("\xD0\xCF\x11\xE0")) {
    return fail(Status::Decode, "the document is password-protected (encrypted OOXML); it cannot be read");
  }
  CVT_TRY_ASSIGN(Package package, Package::open(ctx.input, ctx.limits));
  WordReader reader(ctx, read_settings(ctx.options));
  CVT_TRY_ASSIGN(ir::Document doc, reader.read_package(package));
  return AnyIR(std::move(doc));
}

// Flat OPC: <pkg:package><pkg:part pkg:name="/word/document.xml"><pkg:xmlData>...</pkg:xmlData></pkg:part>...
Result<AnyIR> read_flat_word(ReadContext& ctx) {
  CVT_TRY_ASSIGN(util::XmlDocument xml, parse_untrusted_xml(ctx.input));
  const XmlNode root = xml.root();
  if (root.local_name() == "wordDocument") {
    WordReader reader(ctx, read_settings(ctx.options));
    CVT_TRY_ASSIGN(ir::Document doc, reader.read_word2003(xml));
    return AnyIR(std::move(doc));
  }
  if (root.local_name() != "package") return fail(Status::Decode, "not a Word XML document");
  std::map<std::string, Bytes> parts;
  std::uint64_t total = 0;
  for (XmlNode part : root.children_local("part")) {
    std::string name(attr(part, "name"));
    if (name.starts_with('/')) name.erase(0, 1);
    Bytes bytes;
    if (XmlNode data = child(part, "xmlData")) {
      util::XmlWriteOptions options;
      options.indent.clear();
      std::string text;
      for (XmlNode c : data.elements()) text += util::xml_to_string(c, options);
      bytes = to_bytes(text);
    } else if (XmlNode bin = child(part, "binaryData")) {
      std::string data;
      for (char c : bin.text()) {
        if (!util::is_ascii_space(c)) data.push_back(c);
      }
      auto decoded = util::base64_decode(data);
      if (!decoded) continue;
      bytes = std::move(*decoded);
    }
    total += bytes.size();
    CVT_TRY(ctx.limits.check_decompressed(total));
    parts[name] = std::move(bytes);
  }
  Package package = Package::from_parts(std::move(parts));
  WordReader reader(ctx, read_settings(ctx.options));
  CVT_TRY_ASSIGN(ir::Document doc, reader.read_package(package));
  return AnyIR(std::move(doc));
}

}  // namespace

ReadFn docx_reader() { return read_docx; }
ReadFn flat_word_reader() { return read_flat_word; }

}  // namespace cvt::office
