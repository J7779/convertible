// Convertible core - pdf family: documents to PDF (layout engine).
//
// Lays out an ir::Document on pages: paragraphs with justified or ragged
// lines, headings kept with the following text, nested lists, block quotes,
// code blocks, tables with column sizing and page splitting (header rows
// repeated), images scaled to fit, links as annotations, page numbers and an
// outline from the headings. Text uses the bundled fonts (Inter, JetBrains
// Mono), embedded as subset TrueType CID fonts with ToUnicode maps.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <set>

#include "engine/standard_options.hpp"
#include "families/pdf/builder.hpp"
#include "families/pdf/embedded_fonts.hpp"
#include "families/pdf/font_program.hpp"
#include "families/pdf/pdf.hpp"
#include "families/pdf/pdf_image.hpp"
#include "families/pdf/syntax.hpp"
#include "families/pdf/write_common.hpp"
#include "util/text.hpp"

namespace cvt::pdf {

namespace {

// ------------------------------------------------------------------ fonts

struct Face {
  std::shared_ptr<const font::TrueType> tt;
  std::string resource;  // F0, F1, F2
  std::string ps_name;
  std::set<std::uint16_t> used;
  std::map<std::uint16_t, std::u32string> text;  // gid -> unicode
  double scale = 0.001;                          // font units -> em

  [[nodiscard]] std::optional<std::uint16_t> glyph(char32_t cp) const {
    if (!tt) return std::nullopt;
    return tt->glyph_for_unicode(cp);
  }
  [[nodiscard]] double advance(std::uint16_t gid) const { return tt ? tt->advance(gid) * scale : 0.5; }
};

enum FaceId { Regular = 0, Bold = 1, Mono = 2 };

struct Style {
  int face = Regular;
  bool italic = false;
  double size = 10.5;
  std::array<double, 3> color{0.1, 0.1, 0.12};
  std::string link;
  bool underline = false;
  bool strike = false;
  double rise = 0;
  bool operator==(const Style&) const = default;
};

struct Piece {
  Style style;
  int face = Regular;  // actual face after fallback
  std::vector<std::uint16_t> gids;
  double width = 0;
};

struct Word {
  std::vector<Piece> pieces;
  double width = 0;
  double space_after = 0;  // width of the following space
  bool hard_break = false; // a line break follows
  double max_size = 0;
};

struct LinkRect {
  Rect box;
  std::string url;
};

struct PageOut {
  std::string background;
  std::string content;
  std::vector<LinkRect> links;
  bool used = false;
};

struct OutlineItem {
  std::string title;
  int level = 1;
  std::size_t page = 0;
  double y = 0;  // PDF user space
};

struct Layout {
  // configuration
  Rect page;
  double margin = 56.7;
  double base_size = 10.5;
  bool justify = true;
  // state
  std::vector<PageOut> pages;
  double y = 0;  // distance from the top of the page
  std::array<Face, 3> faces;
  std::vector<OutlineItem> outline;
  std::map<std::string, Ref> images;  // resource name -> xobject
  int image_counter = 0;
  bool missing_glyphs = false;
  PdfBuilder* builder = nullptr;
  const WriteContext* ctx = nullptr;
  ImageEncoding image_encoding;

  [[nodiscard]] double content_width() const { return page.width() - 2 * margin; }
  [[nodiscard]] double bottom() const { return page.height() - margin; }
  [[nodiscard]] double remaining() const { return bottom() - y; }
  PageOut& current() {
    if (pages.empty()) new_page();
    return pages.back();
  }
  void new_page() {
    pages.emplace_back();
    y = margin;
  }
  void ensure(double height) {
    if (pages.empty()) new_page();
    if (y + height > bottom() && pages.back().used) new_page();
  }
  [[nodiscard]] double pdf_y(double top_y) const { return page.height() - top_y; }

  // Shape text into a piece with fallback faces.
  void shape(std::string_view utf8, const Style& style, std::vector<Piece>& out) {
    const std::u32string text = util::utf8_to_utf32(utf8);
    Piece current;
    current.style = style;
    current.face = style.face;
    for (char32_t cp : text) {
      if (cp == '\t') cp = ' ';
      if (cp == 0xAD || cp == 0x200B || cp == 0xFEFF) continue;
      if (cp < 0x20) continue;
      int face = style.face;
      auto gid = faces[static_cast<std::size_t>(face)].glyph(cp);
      if (!gid) {
        for (const int alt : {Regular, Mono, Bold}) {
          if (alt == face) continue;
          gid = faces[static_cast<std::size_t>(alt)].glyph(cp);
          if (gid) {
            face = alt;
            break;
          }
        }
      }
      if (!gid) {
        missing_glyphs = true;
        face = style.face;
        gid = faces[static_cast<std::size_t>(face)].glyph('?');
        if (!gid) continue;
      }
      if (face != current.face && !current.gids.empty()) {
        out.push_back(std::move(current));
        current = Piece{};
        current.style = style;
      }
      current.face = face;
      Face& f = faces[static_cast<std::size_t>(face)];
      f.used.insert(*gid);
      if (!f.text.contains(*gid)) f.text[*gid] = std::u32string(1, cp);
      current.gids.push_back(*gid);
      current.width += f.advance(*gid) * style.size;
    }
    if (!current.gids.empty()) out.push_back(std::move(current));
  }

  [[nodiscard]] double space_width(const Style& style) {
    Face& f = faces[static_cast<std::size_t>(style.face)];
    const auto gid = f.glyph(' ');
    if (gid) {
      f.used.insert(*gid);
      f.text[*gid] = U" ";
    }
    return (gid ? f.advance(*gid) : 0.25) * style.size;
  }
};

// ------------------------------------------------------------------ inline flattening

void collect_words(Layout& L, const ir::Inlines& inlines, Style style, std::vector<Word>& words, bool& pending_space,
                   int depth) {
  if (depth > ir::max_document_depth) return;
  const auto add_text = [&](std::string_view text, const Style& st) {
    std::size_t i = 0;
    while (i < text.size()) {
      const char c = text[i];
      if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
        pending_space = true;
        ++i;
        continue;
      }
      std::size_t j = i;
      while (j < text.size() && text[j] != ' ' && text[j] != '\n' && text[j] != '\t' && text[j] != '\r') ++j;
      const std::string_view chunk = text.substr(i, j - i);
      if (words.empty() || pending_space || words.back().hard_break) {
        if (!words.empty() && pending_space) words.back().space_after = L.space_width(st);
        words.emplace_back();
      }
      pending_space = false;
      Word& w = words.back();
      std::vector<Piece> pieces;
      L.shape(chunk, st, pieces);
      for (Piece& p : pieces) {
        w.width += p.width;
        w.max_size = std::max(w.max_size, st.size);
        if (!w.pieces.empty() && w.pieces.back().style == p.style && w.pieces.back().face == p.face) {
          w.pieces.back().gids.insert(w.pieces.back().gids.end(), p.gids.begin(), p.gids.end());
          w.pieces.back().width += p.width;
        } else {
          w.pieces.push_back(std::move(p));
        }
      }
      i = j;
    }
  };
  for (const ir::Inline& node : inlines) {
    if (const auto* t = node.get_if<ir::Text>()) {
      add_text(t->text, style);
    } else if (const auto* c = node.get_if<ir::Code>()) {
      Style s = style;
      s.face = Mono;
      s.size = style.size * 0.92;
      add_text(c->text, s);
    } else if (const auto* e = node.get_if<ir::Emphasis>()) {
      Style s = style;
      s.italic = true;
      collect_words(L, e->children, s, words, pending_space, depth + 1);
    } else if (const auto* st = node.get_if<ir::Strong>()) {
      Style s = style;
      if (s.face != Mono) s.face = Bold;
      collect_words(L, st->children, s, words, pending_space, depth + 1);
    } else if (const auto* l = node.get_if<ir::Link>()) {
      Style s = style;
      s.link = l->url;
      s.color = {0.05, 0.3, 0.75};
      s.underline = true;
      collect_words(L, l->children, s, words, pending_space, depth + 1);
    } else if (const auto* x = node.get_if<ir::Strikethrough>()) {
      Style s = style;
      s.strike = true;
      collect_words(L, x->children, s, words, pending_space, depth + 1);
    } else if (const auto* u = node.get_if<ir::Underline>()) {
      Style s = style;
      s.underline = true;
      collect_words(L, u->children, s, words, pending_space, depth + 1);
    } else if (const auto* sup = node.get_if<ir::Superscript>()) {
      Style s = style;
      s.size = style.size * 0.7;
      s.rise = style.size * 0.35;
      collect_words(L, sup->children, s, words, pending_space, depth + 1);
    } else if (const auto* sub = node.get_if<ir::Subscript>()) {
      Style s = style;
      s.size = style.size * 0.7;
      s.rise = -style.size * 0.15;
      collect_words(L, sub->children, s, words, pending_space, depth + 1);
    } else if (node.is<ir::LineBreak>()) {
      if (!words.empty()) words.back().hard_break = true;
      pending_space = false;
    } else if (const auto* img = node.get_if<ir::InlineImage>()) {
      // Inline pictures are rare in flowing text: show the alt text.
      if (!img->alt.empty()) add_text("[" + img->alt + "]", style);
    }
  }
}

struct Line {
  std::size_t first = 0, last = 0;  // word range [first, last)
  double width = 0;                  // natural width incl. spaces
  double height = 0;
  bool hard = false;                 // ended by a forced break or end of paragraph
};

std::vector<Line> break_lines(std::vector<Word>& words, double width, double line_factor) {
  // Split words wider than the line into chunks.
  std::vector<Word> fitted;
  for (Word& w : words) {
    if (w.width <= width || w.pieces.empty()) {
      fitted.push_back(std::move(w));
      continue;
    }
    Word part;
    for (Piece& p : w.pieces) {
      Piece chunk;
      chunk.style = p.style;
      chunk.face = p.face;
      const double per_glyph = p.gids.empty() ? 0 : p.width / static_cast<double>(p.gids.size());
      for (const std::uint16_t g : p.gids) {
        if (part.width + chunk.width + per_glyph > width && (!chunk.gids.empty() || !part.pieces.empty())) {
          if (!chunk.gids.empty()) part.pieces.push_back(chunk);
          part.width += chunk.width;
          part.max_size = std::max(part.max_size, p.style.size);
          fitted.push_back(std::move(part));
          part = Word{};
          chunk.gids.clear();
          chunk.width = 0;
        }
        chunk.gids.push_back(g);
        chunk.width += per_glyph;
      }
      if (!chunk.gids.empty()) {
        part.width += chunk.width;
        part.max_size = std::max(part.max_size, p.style.size);
        part.pieces.push_back(std::move(chunk));
      }
    }
    part.space_after = w.space_after;
    part.hard_break = w.hard_break;
    fitted.push_back(std::move(part));
  }
  words = std::move(fitted);
  std::vector<Line> lines;
  std::size_t i = 0;
  while (i < words.size()) {
    Line line;
    line.first = i;
    double w = 0;
    double max_size = 0;
    while (i < words.size()) {
      const double add = (i > line.first ? words[i - 1].space_after : 0) + words[i].width;
      if (i > line.first && w + add > width + 0.01) break;
      w += add;
      max_size = std::max(max_size, words[i].max_size);
      ++i;
      if (words[i - 1].hard_break) {
        line.hard = true;
        break;
      }
    }
    line.last = i;
    line.width = w;
    line.height = std::max(max_size, 1.0) * line_factor;
    if (i >= words.size()) line.hard = true;
    lines.push_back(line);
  }
  return lines;
}

std::string hex_gids(const std::vector<std::uint16_t>& gids) {
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string s = "<";
  for (const std::uint16_t g : gids) {
    for (int k = 3; k >= 0; --k) s.push_back(hex[(g >> (4 * k)) & 15]);
  }
  s.push_back('>');
  return s;
}

std::string color_op(const std::array<double, 3>& c, bool fill) {
  return format_number(c[0]) + " " + format_number(c[1]) + " " + format_number(c[2]) + (fill ? " rg\n" : " RG\n");
}

enum class Align { Left, Justify, Center };

// Draw lines [from, to) of a broken paragraph at the current position.
void draw_lines(Layout& L, const std::vector<Word>& words, const std::vector<Line>& lines, std::size_t from,
                std::size_t to, double x, double width, Align align) {
  PageOut& page = L.current();
  page.used = true;
  for (std::size_t li = from; li < to; ++li) {
    const Line& line = lines[li];
    const double ascent = line.height / 1.3 * 0.82;
    const double baseline = L.y + (line.height - line.height / 1.3) / 2 + ascent;
    double extra = 0;
    const std::size_t gaps = line.last - line.first > 0 ? line.last - line.first - 1 : 0;
    if (align == Align::Justify && !line.hard && gaps > 0) extra = (width - line.width) / static_cast<double>(gaps);
    if (extra > 0.4 * L.base_size * 4) extra = 0;  // too sparse: leave ragged
    double cx = x;
    if (align == Align::Center) cx = x + std::max(0.0, (width - line.width) / 2);
    std::string& out = page.content;
    for (std::size_t wi = line.first; wi < line.last; ++wi) {
      const Word& w = words[wi];
      for (const Piece& p : w.pieces) {
        const Face& f = L.faces[static_cast<std::size_t>(p.face)];
        const double by = L.pdf_y(baseline) + p.style.rise;
        out += "BT\n" + color_op(p.style.color, true);
        out += "/" + f.resource + " " + format_number(p.style.size) + " Tf\n";
        out += "1 0 " + std::string(p.style.italic ? "0.2" : "0") + " 1 " + format_number(cx) + " " + format_number(by) + " Tm\n";
        if (p.style.face == Bold && p.face != Bold) out += "2 Tr 0.3 w " + color_op(p.style.color, false);
        out += hex_gids(p.gids) + " Tj\nET\n";
        if (p.style.underline || p.style.strike) {
          const double ly = p.style.underline ? by - p.style.size * 0.12 : by + p.style.size * 0.28;
          out += color_op(p.style.color, true);
          out += format_number(cx) + " " + format_number(ly) + " " + format_number(p.width) + " " +
                 format_number(std::max(0.5, p.style.size * 0.06)) + " re f\n";
        }
        if (!p.style.link.empty()) {
          const Rect box{cx, L.pdf_y(baseline) - p.style.size * 0.25, cx + p.width, L.pdf_y(baseline) + p.style.size * 0.85};
          if (!page.links.empty() && page.links.back().url == p.style.link &&
              std::abs(page.links.back().box.y0 - box.y0) < 0.5 && box.x0 - page.links.back().box.x1 < L.base_size) {
            page.links.back().box.x1 = box.x1;
          } else {
            page.links.push_back(LinkRect{box, p.style.link});
          }
        }
        cx += p.width;
      }
      if (wi + 1 < line.last) {
        const double gap = w.space_after + extra;
        // Underlined links continue under the space.
        cx += gap;
      }
    }
    L.y += line.height;
  }
}

void layout_inlines(Layout& L, const ir::Inlines& inlines, Style style, double x, double width, Align align,
                    double space_after, bool keep_with_next = false) {
  std::vector<Word> words;
  bool pending = false;
  collect_words(L, inlines, style, words, pending, 0);
  if (words.empty()) {
    L.y += style.size * 0.5;
    return;
  }
  const std::vector<Line> lines = break_lines(words, width, 1.3);
  std::size_t i = 0;
  if (keep_with_next) {
    double total = 0;
    for (const Line& l : lines) total += l.height;
    L.ensure(total + 3 * L.base_size * 1.3);
  }
  while (i < lines.size()) {
    if (L.pages.empty()) L.new_page();
    // How many lines fit on this page?
    std::size_t fit = 0;
    double h = 0;
    while (i + fit < lines.size() && L.y + h + lines[i + fit].height <= L.bottom() + 0.01) {
      h += lines[i + fit].height;
      ++fit;
    }
    const std::size_t left = lines.size() - i;
    if (fit < left) {
      // Orphan: a single first line at the bottom moves to the next page.
      if (fit == 1 && i == 0 && left > 1 && L.current().used) fit = 0;
      // Widow: leave at least two lines for the next page.
      if (fit >= 3 && left - fit == 1) --fit;
      if (fit == 0) {
        if (L.current().used) {
          L.new_page();
          continue;
        }
        fit = 1;  // a line taller than the page
      }
    }
    draw_lines(L, words, lines, i, i + fit, x, width, align);
    i += fit;
    if (i < lines.size()) L.new_page();
  }
  L.y += space_after;
}

// ------------------------------------------------------------------ blocks

struct DecodedPicture {
  bool ok = false;
  bool jpeg = false;
  Bytes jpeg_bytes;
  int components = 3;
  std::uint32_t width = 0, height = 0;
  double dpi = 96;
  ir::Frame frame;
};

DecodedPicture load_picture(const ir::ImageSource& source, const Context& ctx) {
  DecodedPicture pic;
  if (const auto* img = std::get_if<ir::Image>(&source)) {
    if (!img->valid()) return pic;
    pic.frame = img->frames[0];
    pic.width = pic.frame.width;
    pic.height = pic.frame.height;
    if (img->dpi_x > 0) pic.dpi = img->dpi_x;
    pic.ok = true;
    return pic;
  }
  const auto& enc = std::get<ir::EncodedImage>(source);
  const ByteView b = enc.bytes;
  const bool is_jpeg = b.size() > 4 && std::to_integer<int>(b[0]) == 0xFF && std::to_integer<int>(b[1]) == 0xD8;
  if (is_jpeg) {
    // Pass baseline/progressive JPEGs through: read the frame header.
    for (std::size_t i = 2; i + 9 < b.size();) {
      if (std::to_integer<int>(b[i]) != 0xFF) break;
      const int marker = std::to_integer<int>(b[i + 1]);
      if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
        i += 2;
        continue;
      }
      const std::size_t len = (std::to_integer<std::size_t>(b[i + 2]) << 8) | std::to_integer<std::size_t>(b[i + 3]);
      if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
        pic.height = (std::to_integer<std::uint32_t>(b[i + 5]) << 8) | std::to_integer<std::uint32_t>(b[i + 6]);
        pic.width = (std::to_integer<std::uint32_t>(b[i + 7]) << 8) | std::to_integer<std::uint32_t>(b[i + 8]);
        pic.components = std::to_integer<int>(b[i + 9]);
        break;
      }
      i += 2 + len;
    }
    if (pic.width > 0 && pic.height > 0 && (pic.components == 1 || pic.components == 3)) {
      pic.jpeg = true;
      pic.jpeg_bytes.assign(b.begin(), b.end());
      pic.ok = true;
      return pic;
    }
  }
  std::string format;
  const std::string mime = util::to_lower(enc.mime);
  if (mime.find("png") != std::string::npos) format = "png";
  else if (mime.find("jpeg") != std::string::npos || mime.find("jpg") != std::string::npos) format = "jpg";
  else if (mime.find("gif") != std::string::npos) format = "gif";
  else if (mime.find("webp") != std::string::npos) format = "webp";
  else if (mime.find("bmp") != std::string::npos) format = "bmp";
  else if (mime.find("tiff") != std::string::npos) format = "tiff";
  else if (mime.find("svg") != std::string::npos) format = "svg";
  else if (b.size() > 8 && as_chars(b).substr(1, 3) == "PNG") format = "png";
  if (format.empty()) return pic;
  auto decoded = decode_with_registry(&ctx, format, b);
  if (!decoded) return pic;
  pic.frame = decoded->frames[0];
  pic.width = pic.frame.width;
  pic.height = pic.frame.height;
  if (decoded->dpi_x > 0) pic.dpi = decoded->dpi_x;
  pic.ok = true;
  return pic;
}

void layout_blocks(Layout& L, const ir::Blocks& blocks, double x, double width, int depth);

void layout_heading(Layout& L, const ir::Heading& h, double x, double width) {
  static constexpr double kScale[7] = {1, 1.9, 1.55, 1.3, 1.15, 1.05, 1.0};
  const int level = std::clamp(h.level, 1, 6);
  Style s;
  s.face = Bold;
  s.size = L.base_size * kScale[level];
  s.color = {0.06, 0.06, 0.08};
  if (!L.pages.empty() && L.current().used) L.y += s.size * (level <= 2 ? 0.9 : 0.6);
  L.ensure(s.size * 1.3 + 3 * L.base_size * 1.3);
  L.outline.push_back(OutlineItem{ir::plain_text(h.content), level, L.pages.empty() ? 0 : L.pages.size() - 1,
                                  L.pdf_y(L.y)});
  layout_inlines(L, h.content, s, x, width, Align::Left, s.size * 0.45, true);
  if (!L.outline.empty() && !L.pages.empty()) {
    // The heading may have moved to a new page while keeping with the next block.
  }
}

void layout_list(Layout& L, const ir::List& list, double x, double width, int depth) {
  const double indent = L.base_size * 1.6;
  int number = list.start;
  static constexpr const char* kBullets[] = {"•", "◦", "▪"};
  for (const ir::Blocks& item : list.items) {
    const std::string marker = list.ordered ? std::to_string(number++) + "." : kBullets[depth % 3];
    Style ms;
    ms.size = L.base_size;
    std::vector<Piece> pieces;
    L.shape(marker, ms, pieces);
    double mw = 0;
    for (const Piece& p : pieces) mw += p.width;
    L.ensure(L.base_size * 1.3);
    // Marker on the first line of the item.
    const double top = L.y;
    const std::size_t page_index = L.pages.size();
    layout_blocks(L, item, x + indent, width - indent, depth + 1);
    // Draw the marker at the item's first line (on the page where it started).
    PageOut& page = L.pages[page_index - 1];
    const double baseline = top + (L.base_size * 1.3 - L.base_size) / 2 + L.base_size * 0.82 * 1.0;
    double cx = x + indent - mw - L.base_size * 0.45;
    for (const Piece& p : pieces) {
      page.content += "BT\n" + color_op(ms.color, true) + "/" + L.faces[static_cast<std::size_t>(p.face)].resource + " " +
                      format_number(ms.size) + " Tf\n1 0 0 1 " + format_number(cx) + " " +
                      format_number(L.pdf_y(baseline)) + " Tm\n" + hex_gids(p.gids) + " Tj\nET\n";
      cx += p.width;
    }
    page.used = true;
  }
  L.y += L.base_size * 0.3;
}

void layout_code(Layout& L, const ir::CodeBlock& code, double x, double width) {
  Style s;
  s.face = Mono;
  s.size = L.base_size * 0.88;
  s.color = {0.12, 0.12, 0.15};
  const double pad = L.base_size * 0.6;
  const double line_h = s.size * 1.35;
  std::vector<std::vector<Piece>> lines;
  for (const std::string_view raw : util::split_lines(code.code)) {
    // Expand tabs, keep leading spaces as glyphs (monospaced).
    std::string text;
    for (const char c : raw) {
      if (c == '\t') text.append(4 - text.size() % 4, ' ');
      else if (c != '\r') text.push_back(c);
    }
    std::vector<Piece> pieces;
    L.shape(text.empty() ? std::string(" ") : text, s, pieces);
    // Wrap at the box width by glyph count (monospace).
    double w = 0;
    std::vector<Piece> line;
    for (Piece& p : pieces) {
      Piece chunk;
      chunk.style = p.style;
      chunk.face = p.face;
      const double per = p.gids.empty() ? 0 : p.width / static_cast<double>(p.gids.size());
      for (const std::uint16_t g : p.gids) {
        if (w + per > width - 2 * pad && w > 0) {
          if (!chunk.gids.empty()) line.push_back(chunk);
          lines.push_back(std::move(line));
          line.clear();
          chunk.gids.clear();
          chunk.width = 0;
          w = 0;
        }
        chunk.gids.push_back(g);
        chunk.width += per;
        w += per;
      }
      if (!chunk.gids.empty()) line.push_back(std::move(chunk));
    }
    lines.push_back(std::move(line));
  }
  std::size_t i = 0;
  L.ensure(line_h * std::min<std::size_t>(lines.size(), 3) + 2 * pad);
  while (i < lines.size()) {
    PageOut& page = L.current();
    page.used = true;
    const double top = L.y;
    L.y += pad;
    std::size_t drawn = 0;
    while (i < lines.size() && (L.y + line_h + pad <= L.bottom() || drawn == 0)) {
      const double baseline = L.y + line_h * 0.78;
      double cx = x + pad;
      for (const Piece& p : lines[i]) {
        page.content += "BT\n" + color_op(s.color, true) + "/" + L.faces[static_cast<std::size_t>(p.face)].resource +
                        " " + format_number(s.size) + " Tf\n1 0 0 1 " + format_number(cx) + " " +
                        format_number(L.pdf_y(baseline)) + " Tm\n" + hex_gids(p.gids) + " Tj\nET\n";
        cx += p.width;
      }
      L.y += line_h;
      ++i;
      ++drawn;
    }
    L.y += pad;
    page.background += "0.95 0.95 0.96 rg\n" + format_number(x) + " " + format_number(L.pdf_y(L.y)) + " " +
                       format_number(width) + " " + format_number(L.y - top) + " re f\n";
    if (i < lines.size()) L.new_page();
  }
  L.y += L.base_size * 0.7;
}

void layout_table(Layout& L, const ir::TableBlock& table, double x, double width) {
  std::size_t cols = 0;
  for (const ir::TableRow& row : table.rows) {
    std::size_t n = 0;
    for (const ir::TableCell& c : row.cells) n += static_cast<std::size_t>(std::max(1, c.colspan));
    cols = std::max(cols, n);
  }
  if (cols == 0) return;
  const double pad = 4;
  const double size = L.base_size * 0.92;
  // Column widths from content: minimum (longest word) and preferred (whole text).
  std::vector<double> min_w(cols, 20), pref_w(cols, 20);
  for (const ir::TableRow& row : table.rows) {
    std::size_t c = 0;
    for (const ir::TableCell& cell : row.cells) {
      if (c >= cols) break;
      Style st;
      st.size = size;
      st.face = row.header ? Bold : Regular;
      std::vector<Word> words;
      bool pending = false;
      collect_words(L, cell.content, st, words, pending, 0);
      double total = 0, longest = 0;
      for (const Word& w : words) {
        total += w.width + w.space_after;
        longest = std::max(longest, w.width);
      }
      if (cell.colspan <= 1) {
        min_w[c] = std::max(min_w[c], longest + 2 * pad);
        pref_w[c] = std::max(pref_w[c], total + 2 * pad);
      }
      c += static_cast<std::size_t>(std::max(1, cell.colspan));
    }
  }
  std::vector<double> col_w(cols);
  double pref_total = 0, min_total = 0;
  for (std::size_t c = 0; c < cols; ++c) {
    pref_total += pref_w[c];
    min_total += min_w[c];
  }
  if (pref_total <= width) {
    col_w = pref_w;
    // Stretch to the full width when the table is nearly as wide anyway.
    if (pref_total > 0.6 * width) {
      for (double& w : col_w) w *= width / pref_total;
    }
  } else if (min_total >= width) {
    for (std::size_t c = 0; c < cols; ++c) col_w[c] = width * min_w[c] / min_total;
  } else {
    const double spare = width - min_total, wanted = pref_total - min_total;
    for (std::size_t c = 0; c < cols; ++c) col_w[c] = min_w[c] + spare * (pref_w[c] - min_w[c]) / wanted;
  }
  double table_w = 0;
  for (const double w : col_w) table_w += w;
  struct LaidCell {
    std::vector<Word> words;
    std::vector<Line> lines;
    double x = 0, w = 0, h = 0;
  };
  const auto lay_row = [&](const ir::TableRow& row) {
    std::vector<LaidCell> cells;
    std::size_t c = 0;
    double cx = x;
    for (const ir::TableCell& cell : row.cells) {
      if (c >= cols) break;
      const auto span = std::min<std::size_t>(static_cast<std::size_t>(std::max(1, cell.colspan)), cols - c);
      double w = 0;
      for (std::size_t k = c; k < c + span; ++k) w += col_w[k];
      LaidCell lc;
      lc.x = cx;
      lc.w = w;
      Style st;
      st.size = size;
      st.face = row.header ? Bold : Regular;
      bool pending = false;
      collect_words(L, cell.content, st, lc.words, pending, 0);
      lc.lines = break_lines(lc.words, std::max(4.0, w - 2 * pad), 1.3);
      for (const Line& l : lc.lines) lc.h += l.height;
      lc.h = std::max(lc.h, size * 1.3);
      cells.push_back(std::move(lc));
      cx += w;
      c += span;
    }
    return cells;
  };
  const auto draw_row = [&](std::vector<LaidCell>& cells, bool header) {
    double h = 0;
    for (const LaidCell& c : cells) h = std::max(h, c.h);
    h += 2 * pad;
    PageOut& page = L.current();
    page.used = true;
    const double top = L.y;
    if (header) {
      page.background += "0.93 0.94 0.96 rg\n" + format_number(x) + " " + format_number(L.pdf_y(top + h)) + " " +
                         format_number(table_w) + " " + format_number(h) + " re f\n";
    }
    for (LaidCell& c : cells) {
      L.y = top + pad;
      draw_lines(L, c.words, c.lines, 0, c.lines.size(), c.x + pad, c.w - 2 * pad, Align::Left);
    }
    // Borders.
    page.background += "0.6 0.62 0.66 RG 0.5 w\n";
    page.background += format_number(x) + " " + format_number(L.pdf_y(top)) + " " + format_number(table_w) + " " +
                       format_number(-h) + " re S\n";
    double cx = x;
    for (std::size_t k = 0; k + 1 < cols; ++k) {
      cx += col_w[k];
      page.background += format_number(cx) + " " + format_number(L.pdf_y(top)) + " m " + format_number(cx) + " " +
                         format_number(L.pdf_y(top + h)) + " l S\n";
    }
    L.y = top + h;
  };
  std::vector<std::vector<LaidCell>> header_rows;
  bool first_row = true;
  for (const ir::TableRow& row : table.rows) {
    auto cells = lay_row(row);
    double h = 0;
    for (const LaidCell& c : cells) h = std::max(h, c.h);
    h += 2 * pad;
    if (L.pages.empty()) L.new_page();
    if (L.y + h > L.bottom() && L.current().used) {
      L.new_page();
      if (!row.header) {
        for (const auto& hr : header_rows) {
          auto copy = hr;
          draw_row(copy, true);
        }
      }
    }
    if (row.header && (first_row || !header_rows.empty())) header_rows.push_back(cells);
    first_row = false;
    draw_row(cells, row.header);
  }
  L.y += L.base_size * 0.8;
}

void layout_image(Layout& L, const ir::ImageBlock& block, double x, double width) {
  const DecodedPicture pic = load_picture(block.source, *L.ctx);
  if (!pic.ok || pic.width == 0 || pic.height == 0) {
    if (!block.alt.empty() || !block.caption.empty()) {
      Style s;
      s.italic = true;
      s.size = L.base_size;
      layout_inlines(L, {ir::Text{"[" + (block.alt.empty() ? block.caption : block.alt) + "]"}}, s, x, width,
                     Align::Center, L.base_size * 0.6);
    }
    L.ctx->warn("A picture could not be decoded and was left out of the PDF.");
    return;
  }
  double w = pic.width * 72.0 / pic.dpi, h = pic.height * 72.0 / pic.dpi;
  const double max_h = L.page.height() - 2 * L.margin - L.base_size * 3;
  const double s = std::min({1.0, width / w, max_h / h});
  w *= s;
  h *= s;
  L.ensure(h);
  Ref ref;
  if (pic.jpeg) {
    Dict dict;
    dict.set("Type", Object::name("XObject"));
    dict.set("Subtype", Object::name("Image"));
    dict.set("Width", static_cast<std::int64_t>(pic.width));
    dict.set("Height", static_cast<std::int64_t>(pic.height));
    dict.set("ColorSpace", Object::name(pic.components == 1 ? "DeviceGray" : "DeviceRGB"));
    dict.set("BitsPerComponent", 8);
    dict.set("Filter", Object::name("DCTDecode"));
    ref = L.builder->add_stream_raw(std::move(dict), pic.jpeg_bytes);
  } else {
    auto embedded = embed_image(*L.builder, pic.frame, L.image_encoding, *L.ctx);
    if (!embedded) return;
    ref = *embedded;
  }
  const std::string name = "Im" + std::to_string(L.image_counter++);
  L.images[name] = ref;
  PageOut& page = L.current();
  page.used = true;
  const double ix = x + (width - w) / 2;
  page.content += "q " + format_number(w) + " 0 0 " + format_number(h) + " " + format_number(ix) + " " +
                  format_number(L.pdf_y(L.y + h)) + " cm /" + name + " Do Q\n";
  L.y += h + L.base_size * 0.4;
  if (!block.caption.empty()) {
    Style cs;
    cs.italic = true;
    cs.size = L.base_size * 0.9;
    cs.color = {0.3, 0.3, 0.35};
    layout_inlines(L, {ir::Text{block.caption}}, cs, x, width, Align::Center, L.base_size * 0.6);
  } else {
    L.y += L.base_size * 0.5;
  }
}

void layout_blocks(Layout& L, const ir::Blocks& blocks, double x, double width, int depth) {
  if (depth > ir::max_document_depth) return;
  for (const ir::Block& block : blocks) {
    if (L.pages.size() > 100000) return;
    if (const auto* p = block.get_if<ir::Paragraph>()) {
      Style s;
      s.size = L.base_size;
      layout_inlines(L, p->content, s, x, width, L.justify ? Align::Justify : Align::Left, L.base_size * 0.65);
    } else if (const auto* h = block.get_if<ir::Heading>()) {
      layout_heading(L, *h, x, width);
    } else if (const auto* list = block.get_if<ir::List>()) {
      layout_list(L, *list, x, width, depth);
    } else if (const auto* code = block.get_if<ir::CodeBlock>()) {
      layout_code(L, *code, x, width);
    } else if (const auto* quote = block.get_if<ir::BlockQuote>()) {
      const std::size_t first_page = L.pages.empty() ? 0 : L.pages.size() - 1;
      L.ensure(L.base_size * 1.3);
      const double top = L.y;
      layout_blocks(L, quote->blocks, x + L.base_size * 1.4, width - L.base_size * 1.4, depth + 1);
      // Bar on every page the quote spans.
      for (std::size_t pi = first_page; pi < L.pages.size(); ++pi) {
        const double y0 = pi == first_page ? top : L.margin;
        const double y1 = pi + 1 == L.pages.size() ? L.y - L.base_size * 0.65 : L.bottom();
        if (y1 <= y0) continue;
        L.pages[pi].background += "0.75 0.77 0.8 rg\n" + format_number(x + 2) + " " + format_number(L.pdf_y(y1)) +
                                  " 2.5 " + format_number(y1 - y0) + " re f\n";
      }
    } else if (const auto* table = block.get_if<ir::TableBlock>()) {
      layout_table(L, *table, x, width);
    } else if (const auto* image = block.get_if<ir::ImageBlock>()) {
      layout_image(L, *image, x, width);
    } else if (block.is<ir::HorizontalRule>()) {
      L.ensure(L.base_size * 1.5);
      PageOut& page = L.current();
      page.used = true;
      L.y += L.base_size * 0.6;
      page.background += "0.7 0.72 0.75 RG 0.75 w\n" + format_number(x) + " " + format_number(L.pdf_y(L.y)) + " m " +
                         format_number(x + width) + " " + format_number(L.pdf_y(L.y)) + " l S\n";
      L.y += L.base_size * 0.9;
    } else if (block.is<ir::PageBreak>()) {
      if (!L.pages.empty() && L.current().used) L.new_page();
    }
  }
}

std::string subset_tag(const std::set<std::uint16_t>& glyphs, int salt) {
  std::uint32_t h = 2166136261u + static_cast<std::uint32_t>(salt);
  for (const std::uint16_t g : glyphs) h = (h ^ g) * 16777619u;
  std::string tag;
  for (int i = 0; i < 6; ++i) {
    tag.push_back(static_cast<char>('A' + h % 26));
    h /= 26;
    if (h == 0) h = 7919;
  }
  return tag;
}

Result<Ref> embed_face(PdfBuilder& builder, Face& face, int index) {
  std::vector<std::uint16_t> glyphs(face.used.begin(), face.used.end());
  CVT_TRY_ASSIGN(font::SubsetResult subset, font::subset_truetype(*face.tt, glyphs));
  const std::string base = subset_tag(face.used, index) + "+" + face.ps_name;
  // CIDs are the original glyph ids; CIDToGIDMap maps them to the subset.
  std::uint16_t max_cid = 0;
  for (const std::uint16_t g : face.used) max_cid = std::max(max_cid, g);
  Bytes map(static_cast<std::size_t>(max_cid + 1) * 2, std::byte{0});
  for (const auto& [old_gid, new_gid] : subset.new_gid) {
    if (old_gid > max_cid) continue;
    map[old_gid * 2u] = static_cast<std::byte>(new_gid >> 8);
    map[old_gid * 2u + 1] = static_cast<std::byte>(new_gid & 0xFF);
  }
  const Ref map_ref = builder.add_stream(Dict{}, map);
  Dict ff;
  ff.set("Length1", static_cast<std::int64_t>(subset.font.size()));
  const Ref font_file = builder.add_stream(std::move(ff), subset.font);
  const double s = 1000.0 / face.tt->units_per_em();
  Dict fd;
  fd.set("Type", Object::name("FontDescriptor"));
  fd.set("FontName", Object::name(base));
  fd.set("Flags", static_cast<std::int64_t>(32 | (face.tt->fixed_pitch() ? 1 : 0)));
  const Rect bb = face.tt->bbox();
  fd.set("FontBBox", Array{Object(std::round(bb.x0 * s)), Object(std::round(bb.y0 * s)), Object(std::round(bb.x1 * s)),
                           Object(std::round(bb.y1 * s))});
  fd.set("ItalicAngle", 0);
  fd.set("Ascent", Object(std::round(face.tt->ascent() * s)));
  fd.set("Descent", Object(std::round(face.tt->descent() * s)));
  fd.set("CapHeight", Object(std::round(face.tt->cap_height() * s)));
  fd.set("StemV", face.tt->weight_class() >= 600 ? 120 : 80);
  fd.set("FontFile2", font_file);
  const Ref fd_ref = builder.add(Object(std::move(fd)));
  Array widths;
  for (const std::uint16_t g : face.used) {
    widths.push_back(static_cast<std::int64_t>(g));
    widths.push_back(Array{Object(std::round(face.tt->advance(g) * s))});
  }
  Dict cid;
  cid.set("Type", Object::name("Font"));
  cid.set("Subtype", Object::name("CIDFontType2"));
  cid.set("BaseFont", Object::name(base));
  Dict info;
  info.set("Registry", Object::string("Adobe"));
  info.set("Ordering", Object::string("Identity"));
  info.set("Supplement", 0);
  cid.set("CIDSystemInfo", Object(std::move(info)));
  cid.set("FontDescriptor", fd_ref);
  cid.set("W", Object(std::move(widths)));
  cid.set("CIDToGIDMap", map_ref);
  const Ref cid_ref = builder.add(Object(std::move(cid)));
  // ToUnicode
  std::string cmap =
      "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n/CIDSystemInfo << /Registry (Adobe) /Ordering "
      "(UCS) /Supplement 0 >> def\n/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n1 begincodespacerange\n<0000> "
      "<FFFF>\nendcodespacerange\n";
  std::vector<std::pair<std::uint16_t, std::u32string>> entries(face.text.begin(), face.text.end());
  for (std::size_t i = 0; i < entries.size(); i += 100) {
    const std::size_t n = std::min<std::size_t>(100, entries.size() - i);
    cmap += std::to_string(n) + " beginbfchar\n";
    for (std::size_t k = i; k < i + n; ++k) {
      cmap += std::format("<{:04X}> <", entries[k].first);
      const std::u16string u16 = util::utf8_to_utf16(util::utf32_to_utf8(entries[k].second));
      for (const char16_t u : u16) cmap += std::format("{:04X}", static_cast<unsigned>(u));
      cmap += ">\n";
    }
    cmap += "endbfchar\n";
  }
  cmap += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
  const Ref tu = builder.add_stream(Dict{}, as_bytes(cmap));
  Dict type0;
  type0.set("Type", Object::name("Font"));
  type0.set("Subtype", Object::name("Type0"));
  type0.set("BaseFont", Object::name(base));
  type0.set("Encoding", Object::name("Identity-H"));
  type0.set("DescendantFonts", Array{Object(cid_ref)});
  type0.set("ToUnicode", tu);
  return builder.add(Object(std::move(type0)));
}

}  // namespace

Result<Bytes> write_document_pdf(const ir::Document& document, WriteContext& ctx) {
  Layout L;
  const std::string size_name = ctx.options.get_enum("page_size", "a4", {"a4", "letter", "legal", "a5", "a3"});
  const bool landscape = ctx.options.get_enum("orientation", "portrait", {"portrait", "landscape"}) == "landscape";
  L.page = paper_size(size_name, landscape);
  L.margin = std::min(ctx.options.get_float("margin_mm", 20, 0, 100) * 72.0 / 25.4,
                      std::min(L.page.width(), L.page.height()) / 3);
  L.base_size = ctx.options.get_float("font_size", 10.5, 6, 24);
  L.justify = ctx.options.get_bool("justify", true);
  L.ctx = &ctx;
  L.image_encoding.quality = options::resolved_quality(ctx.options);
  PdfBuilder builder(9);
  L.builder = &builder;
  const ByteView sources[3] = {fonts::inter_regular(), fonts::inter_semibold(), fonts::mono_regular()};
  for (int i = 0; i < 3; ++i) {
    auto tt = font::TrueType::parse(sources[i]);
    if (!tt) return fail(Status::Internal, "the bundled fonts are missing from this build");
    Face& face = L.faces[static_cast<std::size_t>(i)];
    face.tt = std::make_shared<const font::TrueType>(std::move(*tt));
    face.resource = "F" + std::to_string(i);
    face.scale = 1.0 / face.tt->units_per_em();
    face.ps_name = face.tt->postscript_name().empty() ? "Font" + std::to_string(i) : face.tt->postscript_name();
  }
  CVT_TRY(ctx.checkpoint(0.05, "Layout"));
  layout_blocks(L, document.blocks, L.margin, L.content_width(), 0);
  if (L.pages.empty()) L.new_page();
  if (L.missing_glyphs) ctx.warn("Some characters are not covered by the bundled fonts and were replaced by '?'.");
  // Page numbers.
  const bool numbers = ctx.options.get_bool("page_numbers", true);
  if (numbers) {
    Style ns;
    ns.size = L.base_size * 0.85;
    ns.color = {0.4, 0.4, 0.45};
    for (std::size_t i = 0; i < L.pages.size(); ++i) {
      std::vector<Piece> pieces;
      L.shape(std::to_string(i + 1), ns, pieces);
      double w = 0;
      for (const Piece& p : pieces) w += p.width;
      double cx = (L.page.width() - w) / 2;
      const double by = std::max(L.margin * 0.45, 12.0);
      for (const Piece& p : pieces) {
        L.pages[i].content += "BT\n" + color_op(ns.color, true) + "/" + L.faces[static_cast<std::size_t>(p.face)].resource +
                              " " + format_number(ns.size) + " Tf\n1 0 0 1 " + format_number(cx) + " " +
                              format_number(by) + " Tm\n" + hex_gids(p.gids) + " Tj\nET\n";
        cx += p.width;
      }
    }
  }
  CVT_TRY(ctx.checkpoint(0.7, "Fonts"));
  Dict font_dict;
  for (int i = 0; i < 3; ++i) {
    Face& face = L.faces[static_cast<std::size_t>(i)];
    if (face.used.empty()) continue;
    CVT_TRY_ASSIGN(const Ref ref, embed_face(builder, face, i));
    font_dict.set(face.resource, ref);
  }
  Dict xobjects;
  for (const auto& [name, ref] : L.images) xobjects.set(name, ref);
  Dict resources;
  resources.set("Font", Object(std::move(font_dict)));
  if (!xobjects.empty()) resources.set("XObject", Object(std::move(xobjects)));
  const Ref resources_ref = builder.add(Object(std::move(resources)));
  const Ref pages_ref = builder.reserve();
  Array kids;
  std::vector<Ref> page_refs;
  for (std::size_t i = 0; i < L.pages.size(); ++i) page_refs.push_back(builder.reserve());
  for (std::size_t i = 0; i < L.pages.size(); ++i) {
    PageOut& p = L.pages[i];
    const std::string content = "q\n" + p.background + "Q\n" + p.content;
    const Ref content_ref = builder.add_stream(Dict{}, as_bytes(content));
    Dict pd;
    pd.set("Type", Object::name("Page"));
    pd.set("Parent", pages_ref);
    pd.set("Resources", resources_ref);
    pd.set("Contents", content_ref);
    if (!p.links.empty()) {
      Array annots;
      for (const LinkRect& l : p.links) {
        Dict a;
        a.set("Type", Object::name("Annot"));
        a.set("Subtype", Object::name("Link"));
        a.set("Rect", Array{Object(l.box.x0), Object(l.box.y0), Object(l.box.x1), Object(l.box.y1)});
        a.set("Border", Array{Object(0), Object(0), Object(0)});
        Dict action;
        action.set("S", Object::name("URI"));
        action.set("URI", Object::string(l.url));
        a.set("A", Object(std::move(action)));
        annots.push_back(builder.add(Object(std::move(a))));
      }
      pd.set("Annots", Object(std::move(annots)));
    }
    builder.set(page_refs[i], Object(std::move(pd)));
    kids.push_back(page_refs[i]);
  }
  Dict pages;
  pages.set("Type", Object::name("Pages"));
  pages.set("Count", static_cast<std::int64_t>(kids.size()));
  pages.set("Kids", Object(std::move(kids)));
  pages.set("MediaBox", Array{Object(0), Object(0), Object(L.page.width()), Object(L.page.height())});
  builder.set(pages_ref, Object(std::move(pages)));
  Dict catalog;
  catalog.set("Type", Object::name("Catalog"));
  catalog.set("Pages", pages_ref);
  if (!document.metadata.language.empty()) catalog.set("Lang", Object::string(encode_text_string(document.metadata.language)));
  // Outline from the headings.
  if (!L.outline.empty() && L.outline.size() < 20000) {
    const Ref root = builder.reserve();
    struct Node {
      Ref ref;
      std::size_t index;
      std::vector<std::size_t> children;
      int level;
    };
    std::vector<Node> nodes;
    std::vector<std::size_t> top;
    std::vector<std::size_t> stack;
    for (std::size_t i = 0; i < L.outline.size(); ++i) {
      nodes.push_back(Node{builder.reserve(), i, {}, L.outline[i].level});
      while (!stack.empty() && nodes[stack.back()].level >= L.outline[i].level) stack.pop_back();
      if (stack.empty()) top.push_back(i);
      else nodes[stack.back()].children.push_back(i);
      stack.push_back(i);
    }
    std::function<std::int64_t(const std::vector<std::size_t>&, Ref)> emit = [&](const std::vector<std::size_t>& list,
                                                                                 Ref parent) -> std::int64_t {
      std::int64_t count = 0;
      for (std::size_t k = 0; k < list.size(); ++k) {
        Node& n = nodes[list[k]];
        const OutlineItem& item = L.outline[n.index];
        Dict d;
        d.set("Title", Object::string(encode_text_string(item.title)));
        d.set("Parent", parent);
        if (k > 0) d.set("Prev", nodes[list[k - 1]].ref);
        if (k + 1 < list.size()) d.set("Next", nodes[list[k + 1]].ref);
        const std::size_t page = std::min(item.page, page_refs.size() - 1);
        d.set("Dest", Array{Object(page_refs[page]), Object::name("XYZ"), Object(0), Object(item.y), Object{}});
        if (!n.children.empty()) {
          d.set("First", nodes[n.children.front()].ref);
          d.set("Last", nodes[n.children.back()].ref);
          const std::int64_t inner = emit(n.children, n.ref);
          d.set("Count", inner);
        }
        builder.set(n.ref, Object(std::move(d)));
        count += 1;
      }
      return count;
    };
    Dict od;
    od.set("Type", Object::name("Outlines"));
    od.set("First", nodes[top.front()].ref);
    od.set("Last", nodes[top.back()].ref);
    od.set("Count", emit(top, root));
    builder.set(root, Object(std::move(od)));
    catalog.set("Outlines", root);
    catalog.set("PageMode", Object::name("UseOutlines"));
  }
  const Ref catalog_ref = builder.add(Object(std::move(catalog)));
  Dict info;
  info.set("Producer", Object::string("Convertible"));
  if (!document.metadata.title.empty()) info.set("Title", Object::string(encode_text_string(document.metadata.title)));
  if (!options::resolved_strip_metadata(ctx.options)) {
    if (!document.metadata.author.empty()) info.set("Author", Object::string(encode_text_string(document.metadata.author)));
    for (const auto& [key, value] : document.metadata.extra) {
      if (key == "subject") info.set("Subject", Object::string(encode_text_string(value)));
      if (key == "keywords") info.set("Keywords", Object::string(encode_text_string(value)));
    }
  }
  const Ref info_ref = builder.add(Object(std::move(info)));
  CVT_TRY(ctx.checkpoint(0.9, "Writing"));
  CVT_TRY_ASSIGN(Bytes out, builder.finish(catalog_ref, info_ref, "1.7"));
  CVT_TRY(ctx.limits.check_output(out.size()));
  return out;
}

}  // namespace cvt::pdf
