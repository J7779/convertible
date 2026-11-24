// Convertible core - office family: Word 97-2003 binary documents (.doc).
//
// The WordDocument stream of a compound file, read per [MS-DOC]: the FIB,
// the piece table (8-bit "compressed" and UTF-16 pieces), character runs
// (CHPX FKPs: bold, italic, underline, strike, super/subscript, hidden,
// monospaced fonts), paragraph runs (PAPX FKPs: style, outline level, list
// membership, table cell and row marks), the style sheet (built-in heading
// styles by identifier, names), the list tables (bullets versus numbering
// per level), fields (HYPERLINK), footnotes and endnotes, inline pictures
// from the Data stream (PNG, JPEG, DIB as BMP), and tables (cell and row
// marks with horizontal and vertical merges from the row definitions).
// Word 6 / Word 95 files: plain text extraction. There is no DOC writer:
// documents are written as DOCX, RTF or ODT instead.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "engine/registry.hpp"
#include "families/document/document_api.hpp"
#include "families/office/office.hpp"
#include "families/office/escher.hpp"
#include "families/office/office_common.hpp"
#include "families/table/cfb.hpp"
#include "families/table/codepages.hpp"
#include "util/text.hpp"

namespace cvt::office {

namespace {

std::uint32_t le32(ByteView d, std::size_t at) {
  util::ByteReader r(d);
  if (!r.seek(at)) return 0;
  return r.u32le();
}
std::uint16_t le16(ByteView d, std::size_t at) {
  util::ByteReader r(d);
  if (!r.seek(at)) return 0;
  return r.u16le();
}

struct Sprm {
  std::uint16_t code = 0;
  ByteView operand;
};

// Iterate the sprms of a grpprl.
std::vector<Sprm> parse_grpprl(ByteView grpprl) {
  std::vector<Sprm> out;
  util::ByteReader r(grpprl);
  while (r.remaining() >= 2 && out.size() < 4096) {
    const std::uint16_t code = r.u16le();
    const int spra = code >> 13;
    std::size_t size = 0;
    switch (spra) {
      case 0:
      case 1: size = 1; break;
      case 2:
      case 4:
      case 5: size = 2; break;
      case 3: size = 4; break;
      case 7: size = 3; break;
      case 6:
        if (code == 0xD608 || code == 0xD606) {  // sprmTDefTable / sprmTDefTable10: 2-byte length
          if (r.remaining() < 2) return out;
          size = static_cast<std::size_t>(le16(r.peek_rest(), 0)) - 1 + 2;
          // The length includes itself minus one byte; take the operand with its length.
        } else if (code == 0xC615) {  // sprmPChgTabs: special
          const std::uint8_t cb = r.peek_u8();
          size = cb == 255 ? r.remaining() : static_cast<std::size_t>(cb) + 1;
        } else {
          size = static_cast<std::size_t>(r.peek_u8()) + 1;
        }
        break;
      default: return out;
    }
    if (size > r.remaining()) return out;
    Sprm sprm;
    sprm.code = code;
    sprm.operand = r.bytes(size);
    out.push_back(sprm);
  }
  return out;
}

struct CharProps {
  int bold = -1, italic = -1, strike = -1, hidden = -1, special = -1;  // -1 = unset
  int underline = -1;
  int iss = -1;  // 1 super, 2 sub
  int font = -1;
  int size = -1;
  int istd = -1;
  std::uint32_t pic = 0xFFFFFFFFu;
  bool has_pic = false;
};

void apply_char_sprms(ByteView grpprl, CharProps& p) {
  for (const Sprm& s : parse_grpprl(grpprl)) {
    const auto b = s.operand.empty() ? 0 : std::to_integer<int>(s.operand[0]);
    const auto toggle = [&](int& field) {
      if (b == 0 || b == 1) field = b;
      else if (b == 0x81) field = field == 1 ? 0 : 1;
      // 0x80: same as the style (keep)
    };
    switch (s.code) {
      case 0x0835: toggle(p.bold); break;
      case 0x0836: toggle(p.italic); break;
      case 0x0837: toggle(p.strike); break;
      case 0x2A53: toggle(p.strike); break;
      case 0x083C: toggle(p.hidden); break;
      case 0x0855: p.special = b; break;
      case 0x2A3E: p.underline = b; break;
      case 0x2A48: p.iss = b; break;
      case 0x4A4F: p.font = le16(s.operand, 0); break;
      case 0x4A43: p.size = le16(s.operand, 0); break;
      case 0x4A30: p.istd = le16(s.operand, 0); break;
      case 0x4845: {  // sprmCHpsPos: raised or lowered text
        const auto pos = static_cast<std::int16_t>(le16(s.operand, 0));
        if (pos > 0) p.iss = 1;
        if (pos < 0) p.iss = 2;
        break;
      }
      case 0x6A03:
        p.pic = le32(s.operand, 0);
        p.has_pic = true;
        break;
      default: break;
    }
  }
}

struct ParaProps {
  int istd = 0;
  int outline = 9;
  int ilfo = 0;
  int ilvl = 0;
  bool in_table = false;
  bool row_end = false;
  int itap = 0;
  bool inner_cell = false;
  bool inner_row_end = false;
  // Row definition (on row-end paragraphs).
  struct Cell {
    bool first_merged = false, merged = false, vert_merge = false, vert_restart = false;
  };
  std::vector<Cell> cells;
  std::vector<int> centers;  // cell boundaries (twips)
  bool header_row = false;
};

void apply_para_sprms(ByteView grpprl, ParaProps& p) {
  for (const Sprm& s : parse_grpprl(grpprl)) {
    const int b = s.operand.empty() ? 0 : std::to_integer<int>(s.operand[0]);
    switch (s.code) {
      case 0x4600: p.istd = le16(s.operand, 0); break;
      case 0x2640: p.outline = b; break;
      case 0x460B: p.ilfo = static_cast<std::int16_t>(le16(s.operand, 0)); break;
      case 0x260A: p.ilvl = b; break;
      case 0x2416: p.in_table = b != 0; break;
      case 0x2417: p.row_end = b != 0; break;
      case 0x6649: p.itap = static_cast<int>(le32(s.operand, 0)); break;
      case 0x244B: p.inner_cell = b != 0; break;
      case 0x244C: p.inner_row_end = b != 0; break;
      case 0x3404: p.header_row = b != 0; break;  // sprmTTableHeader
      case 0xD608: {
        // sprmTDefTable: cb (u16), itcMac (u8), rgdxaCenter[itcMac+1] (i16), rgTc80[itcMac] (20 bytes each)
        util::ByteReader r(s.operand);
        r.skip(2);
        const std::uint8_t count = r.u8();
        p.centers.clear();
        for (std::size_t k = 0; k <= count && r.remaining() >= 2; ++k) p.centers.push_back(r.i16le());
        p.cells.clear();
        for (std::uint8_t i = 0; i < count && r.remaining() >= 2; ++i) {
          const std::uint16_t flags = r.u16le();
          r.skip(18);
          ParaProps::Cell cell;
          cell.first_merged = flags & 0x0001;
          cell.merged = flags & 0x0002;
          cell.vert_merge = flags & 0x0020;
          cell.vert_restart = flags & 0x0040;
          p.cells.push_back(cell);
        }
        break;
      }
      case 0xD62B: {
        // sprmTVertMerge: cb, itc, ucVertMerge (1 = merged with previous, 3 = start)
        if (s.operand.size() >= 3) {
          const std::size_t itc = std::to_integer<std::size_t>(s.operand[1]);
          const int merge = std::to_integer<int>(s.operand[2]);
          if (itc < 64) {
            if (p.cells.size() <= itc) p.cells.resize(itc + 1);
            p.cells[itc].vert_merge = merge != 0;
            p.cells[itc].vert_restart = merge == 3;
          }
        }
        break;
      }
      default: break;
    }
  }
}

struct Style {
  std::string name;
  int sti = 0;
  int base = 0x0FFF;
  int type = 1;  // 1 paragraph, 2 character
  Bytes para_grpprl;
  Bytes char_grpprl;
};

struct Run {
  std::uint32_t fc_start = 0, fc_end = 0;
  Bytes grpprl;
};
struct ParaRun {
  std::uint32_t fc_start = 0, fc_end = 0;
  int istd = 0;
  Bytes grpprl;
};

struct Piece {
  std::uint32_t cp_start = 0, cp_end = 0;
  std::uint32_t fc = 0;
  bool compressed = false;
  Bytes prm;
};

struct Level {
  bool ordered = false;
  ir::ListNumbering numbering = ir::ListNumbering::Decimal;
  int start = 1;
};

class DocReader {
 public:
  DocReader(const ReadContext& ctx, ReadSettings settings) : ctx_(ctx), settings_(settings) {}
  Result<ir::Document> read(const table::CfbReader& cfb);

 private:
  Result<void> load_pieces(ByteView table, std::uint32_t fc_clx, std::uint32_t lcb_clx);
  void load_chpx(ByteView table, std::uint32_t fc, std::uint32_t lcb);
  void load_papx(ByteView table, std::uint32_t fc, std::uint32_t lcb);
  void load_styles(ByteView table, std::uint32_t fc, std::uint32_t lcb);
  void load_lists(ByteView table, std::uint32_t fc_lst, std::uint32_t lcb_lst, std::uint32_t fc_lfo, std::uint32_t lcb_lfo);
  std::vector<std::uint32_t> plc_cps(ByteView table, std::uint32_t fc, std::uint32_t lcb, std::size_t data_size);

  // Characters of the CP range with their stream positions.
  struct Char {
    char32_t c;
    std::uint32_t fc;
    std::uint32_t cp;
  };
  std::vector<Char> characters(std::uint32_t cp_start, std::uint32_t cp_end);
  CharProps char_props_at(std::uint32_t fc, int para_istd);
  ParaProps para_props_at(std::uint32_t fc);
  void style_char_props(int istd, CharProps& out, int guard = 0);
  void style_para_props(int istd, ParaProps& out, int guard = 0);
  ir::Blocks story(std::uint32_t cp_start, std::uint32_t cp_end, bool main);
  std::optional<ir::EncodedImage> picture(std::uint32_t offset);

  const ReadContext& ctx_;
  ReadSettings settings_;
  ByteView word_;
  Bytes data_stream_;
  std::vector<Piece> pieces_;
  std::vector<Run> chpx_;
  std::vector<ParaRun> papx_;
  std::vector<Style> styles_;
  std::map<std::string, std::vector<std::string>> fonts_;
  std::vector<bool> mono_fonts_;
  std::map<std::int32_t, std::vector<Level>> lists_;  // lsid -> levels
  std::vector<std::int32_t> lfo_;                     // ilfo-1 -> lsid
  std::uint32_t ccp_text_ = 0, ccp_ftn_ = 0, ccp_hdd_ = 0, ccp_atn_ = 0, ccp_edn_ = 0;
  std::map<std::uint32_t, std::pair<char, std::size_t>> note_refs_;  // cp of reference -> (kind, index)
  std::vector<std::pair<std::uint32_t, std::uint32_t>> footnote_ranges_, endnote_ranges_;
  std::vector<ir::Footnote> footnotes_;
  int depth_ = 0;
};

std::vector<std::uint32_t> DocReader::plc_cps(ByteView table, std::uint32_t fc, std::uint32_t lcb, std::size_t data_size) {
  std::vector<std::uint32_t> out;
  if (lcb < 4 || std::uint64_t{fc} + lcb > table.size()) return out;
  const std::size_t n = (lcb - 4) / (4 + data_size);
  for (std::size_t i = 0; i <= n; ++i) out.push_back(le32(table, fc + 4 * i));
  return out;
}

Result<void> DocReader::load_pieces(ByteView table, std::uint32_t fc_clx, std::uint32_t lcb_clx) {
  if (std::uint64_t{fc_clx} + lcb_clx > table.size()) return fail(Status::Decode, "the piece table lies outside the table stream");
  util::ByteReader r(table.subspan(fc_clx, lcb_clx));
  while (!r.at_end()) {
    const std::uint8_t clxt = r.u8();
    if (clxt == 1) {
      const std::uint16_t cb = r.u16le();
      r.skip(cb);
    } else if (clxt == 2) {
      const std::uint32_t lcb = r.u32le();
      if (lcb < 4 || lcb > r.remaining()) return fail(Status::Decode, "bad piece table");
      const ByteView plc = r.bytes(lcb);
      const std::size_t n = (lcb - 4) / 12;
      CVT_TRY(ctx_.limits.check_archive_entries(n));
      for (std::size_t i = 0; i < n; ++i) {
        Piece p;
        p.cp_start = le32(plc, 4 * i);
        p.cp_end = le32(plc, 4 * (i + 1));
        const std::size_t pcd = 4 * (n + 1) + 8 * i;
        const std::uint32_t fc = le32(plc, pcd + 2);
        p.compressed = (fc & 0x40000000u) != 0;
        p.fc = p.compressed ? (fc & 0x3FFFFFFFu) / 2 : fc;
        if (p.cp_end < p.cp_start) return fail(Status::Decode, "bad piece table");
        pieces_.push_back(p);
      }
      return {};
    } else {
      return fail(Status::Decode, "bad piece table");
    }
    if (!r.ok()) return fail(Status::Decode, "bad piece table");
  }
  return fail(Status::Decode, "the document has no piece table");
}

void DocReader::load_chpx(ByteView table, std::uint32_t fc, std::uint32_t lcb) {
  if (lcb < 8 || std::uint64_t{fc} + lcb > table.size()) return;
  const std::size_t n = (lcb - 4) / 8;
  std::set<std::uint32_t> pages;
  for (std::size_t i = 0; i < n; ++i) pages.insert(le32(table, fc + 4 * (n + 1) + 4 * i) & 0x3FFFFF);
  for (std::uint32_t pn : pages) {
    const std::uint64_t at = std::uint64_t{pn} * 512;
    if (at + 512 > word_.size()) continue;
    const ByteView page = word_.subspan(static_cast<std::size_t>(at), 512);
    const std::uint8_t crun = std::to_integer<std::uint8_t>(page[511]);
    if (4 * (std::size_t{crun} + 1) + crun > 511) continue;
    for (std::uint8_t k = 0; k < crun; ++k) {
      Run run;
      run.fc_start = le32(page, 4 * k);
      run.fc_end = le32(page, 4 * (k + 1));
      const std::size_t offset = std::to_integer<std::size_t>(page[4 * (std::size_t{crun} + 1) + k]) * 2;
      if (offset != 0 && offset < 511) {
        const std::size_t cb = std::to_integer<std::size_t>(page[offset]);
        if (offset + 1 + cb <= 511) run.grpprl = to_bytes(page.subspan(offset + 1, cb));
      }
      chpx_.push_back(std::move(run));
    }
  }
  std::ranges::sort(chpx_, [](const Run& a, const Run& b) { return a.fc_start < b.fc_start; });
}

void DocReader::load_papx(ByteView table, std::uint32_t fc, std::uint32_t lcb) {
  if (lcb < 8 || std::uint64_t{fc} + lcb > table.size()) return;
  const std::size_t n = (lcb - 4) / 8;
  std::set<std::uint32_t> pages;
  for (std::size_t i = 0; i < n; ++i) pages.insert(le32(table, fc + 4 * (n + 1) + 4 * i) & 0x3FFFFF);
  for (std::uint32_t pn : pages) {
    const std::uint64_t at = std::uint64_t{pn} * 512;
    if (at + 512 > word_.size()) continue;
    const ByteView page = word_.subspan(static_cast<std::size_t>(at), 512);
    const std::uint8_t cpara = std::to_integer<std::uint8_t>(page[511]);
    if (4 * (std::size_t{cpara} + 1) + 13 * std::size_t{cpara} > 511) continue;
    for (std::uint8_t k = 0; k < cpara; ++k) {
      ParaRun run;
      run.fc_start = le32(page, 4 * k);
      run.fc_end = le32(page, 4 * (k + 1));
      const std::size_t offset = std::to_integer<std::size_t>(page[4 * (std::size_t{cpara} + 1) + 13 * k]) * 2;
      if (offset != 0 && offset < 511) {
        std::size_t cb = std::to_integer<std::size_t>(page[offset]);
        std::size_t start = offset + 1;
        std::size_t size = 0;
        if (cb == 0) {
          if (offset + 1 < 511) {
            cb = std::to_integer<std::size_t>(page[offset + 1]);
            start = offset + 2;
            size = 2 * cb;
          }
        } else {
          size = 2 * cb - 1;
        }
        if (size >= 2 && start + size <= 511) {
          run.istd = le16(page, start);
          run.grpprl = to_bytes(page.subspan(start + 2, size - 2));
        }
      }
      papx_.push_back(std::move(run));
    }
  }
  std::ranges::sort(papx_, [](const ParaRun& a, const ParaRun& b) { return a.fc_start < b.fc_start; });
}

void DocReader::load_styles(ByteView table, std::uint32_t fc, std::uint32_t lcb) {
  if (lcb < 4 || std::uint64_t{fc} + lcb > table.size()) return;
  const ByteView stsh = table.subspan(fc, lcb);
  util::ByteReader r(stsh);
  const std::uint16_t cb_stshi = r.u16le();
  const ByteView stshi = r.bytes(cb_stshi);
  if (!r.ok() || stshi.size() < 4) return;
  const std::uint16_t cstd = le16(stshi, 0);
  const std::uint16_t cb_base = le16(stshi, 2);
  for (std::uint16_t i = 0; i < cstd && r.ok() && styles_.size() < 4096; ++i) {
    const std::uint16_t cb = r.u16le();
    Style style;
    if (cb == 0) {
      styles_.push_back(std::move(style));
      continue;
    }
    const ByteView std_data = r.bytes(cb);
    if (!r.ok() || std_data.size() < cb_base || cb_base < 10) {
      styles_.push_back(std::move(style));
      continue;
    }
    style.sti = le16(std_data, 0) & 0x0FFF;
    const std::uint16_t w2 = le16(std_data, 2);
    style.type = w2 & 0x000F;
    style.base = w2 >> 4;
    util::ByteReader s(std_data);
    s.seek(cb_base);
    const std::uint16_t chars = s.u16le();
    if (chars < 256 && s.remaining() >= std::size_t{chars} * 2 + 2) {
      std::u16string name;
      for (std::uint16_t k = 0; k < chars; ++k) name.push_back(static_cast<char16_t>(s.u16le()));
      style.name = util::utf16_to_utf8(name);
      s.skip(2);
      // UPXs (padded to even offsets).
      const auto upx = [&]() -> ByteView {
        if (s.position() % 2) s.skip(1);
        const std::uint16_t size = s.u16le();
        return s.bytes(size);
      };
      if (style.type == 1) {
        const ByteView papx = upx();
        if (papx.size() >= 2) style.para_grpprl = to_bytes(papx.subspan(2));
        style.char_grpprl = to_bytes(upx());
      } else if (style.type == 2) {
        style.char_grpprl = to_bytes(upx());
      }
    }
    styles_.push_back(std::move(style));
  }
}

void DocReader::load_lists(ByteView table, std::uint32_t fc_lst, std::uint32_t lcb_lst, std::uint32_t fc_lfo,
                           std::uint32_t lcb_lfo) {
  if (lcb_lst >= 2 && std::uint64_t{fc_lst} + lcb_lst <= table.size()) {
    util::ByteReader r(table.subspan(fc_lst, lcb_lst));
    const std::uint16_t count = r.u16le();
    struct Lstf {
      std::int32_t lsid;
      bool simple;
    };
    std::vector<Lstf> lstf;
    for (std::uint16_t i = 0; i < count && r.remaining() >= 28; ++i) {
      const std::int32_t lsid = r.i32le();
      r.skip(4 + 18);
      const std::uint8_t flags = r.u8();
      r.skip(1);
      lstf.push_back(Lstf{lsid, (flags & 1) != 0});
    }
    // The LVL records follow the PlfLst in the table stream.
    util::ByteReader lv(table);
    lv.seek(static_cast<std::size_t>(fc_lst) + lcb_lst);
    for (const Lstf& l : lstf) {
      std::vector<Level> levels;
      const int n = l.simple ? 1 : 9;
      for (int k = 0; k < n && lv.remaining() >= 28; ++k) {
        Level level;
        level.start = static_cast<int>(std::clamp<std::int32_t>(lv.i32le(), 0, 1000000));
        const std::uint8_t nfc = lv.u8();
        level.ordered = nfc != 23 && nfc != 255;
        level.numbering = nfc == 1   ? ir::ListNumbering::UpperRoman
                          : nfc == 2 ? ir::ListNumbering::LowerRoman
                          : nfc == 3 ? ir::ListNumbering::UpperAlpha
                          : nfc == 4 ? ir::ListNumbering::LowerAlpha
                                     : ir::ListNumbering::Decimal;
        lv.skip(1 + 9 + 1 + 4 + 4);
        const std::uint8_t cb_chpx = lv.u8();
        const std::uint8_t cb_papx = lv.u8();
        lv.skip(2);
        lv.skip(std::size_t{cb_papx} + cb_chpx);
        const std::uint16_t xst = lv.u16le();
        lv.skip(std::size_t{xst} * 2);
        levels.push_back(level);
      }
      lists_[l.lsid] = std::move(levels);
    }
  }
  if (lcb_lfo >= 4 && std::uint64_t{fc_lfo} + lcb_lfo <= table.size()) {
    util::ByteReader r(table.subspan(fc_lfo, lcb_lfo));
    const std::uint32_t count = r.u32le();
    for (std::uint32_t i = 0; i < count && r.remaining() >= 16 && i < 65536; ++i) {
      lfo_.push_back(r.i32le());
      r.skip(12);
    }
  }
}

std::vector<DocReader::Char> DocReader::characters(std::uint32_t cp_start, std::uint32_t cp_end) {
  std::vector<Char> out;
  const table::CodePageInfo* cp1252 = table::find_code_page(1252);
  for (const Piece& p : pieces_) {
    const std::uint32_t from = std::max(cp_start, p.cp_start);
    const std::uint32_t to = std::min(cp_end, p.cp_end);
    if (from >= to) continue;
    for (std::uint32_t cp = from; cp < to; ++cp) {
      const std::uint64_t offset = cp - p.cp_start;
      if (p.compressed) {
        const std::uint64_t fc = p.fc + offset;
        if (fc >= word_.size()) break;
        const std::uint8_t b = std::to_integer<std::uint8_t>(word_[static_cast<std::size_t>(fc)]);
        char32_t c = b;
        if (b >= 0x80 && cp1252) c = (*cp1252->upper)[b - 0x80];
        out.push_back(Char{c, static_cast<std::uint32_t>(fc), cp});
      } else {
        const std::uint64_t fc = p.fc + offset * 2;
        if (fc + 1 >= word_.size()) break;
        char32_t c = le16(word_, static_cast<std::size_t>(fc));
        out.push_back(Char{c, static_cast<std::uint32_t>(fc), cp});
      }
    }
    if (out.size() > 64 * 1024 * 1024) break;
  }
  // Surrogate pairs.
  std::vector<Char> merged;
  merged.reserve(out.size());
  for (std::size_t i = 0; i < out.size(); ++i) {
    if (out[i].c >= 0xD800 && out[i].c <= 0xDBFF && i + 1 < out.size() && out[i + 1].c >= 0xDC00 && out[i + 1].c <= 0xDFFF) {
      Char c = out[i];
      c.c = 0x10000 + ((out[i].c - 0xD800) << 10) + (out[i + 1].c - 0xDC00);
      merged.push_back(c);
      ++i;
    } else {
      merged.push_back(out[i]);
    }
  }
  return merged;
}

void DocReader::style_char_props(int istd, CharProps& out, int guard) {
  if (istd < 0 || static_cast<std::size_t>(istd) >= styles_.size() || guard > 16) return;
  const Style& s = styles_[static_cast<std::size_t>(istd)];
  if (s.base != 0x0FFF) style_char_props(s.base, out, guard + 1);
  apply_char_sprms(s.char_grpprl, out);
}

void DocReader::style_para_props(int istd, ParaProps& out, int guard) {
  if (istd < 0 || static_cast<std::size_t>(istd) >= styles_.size() || guard > 16) return;
  const Style& s = styles_[static_cast<std::size_t>(istd)];
  if (s.base != 0x0FFF) style_para_props(s.base, out, guard + 1);
  apply_para_sprms(s.para_grpprl, out);
}

CharProps DocReader::char_props_at(std::uint32_t fc, int para_istd) {
  CharProps props;
  // Paragraph style character formatting is not emphasis for headings.
  style_char_props(para_istd, props);
  const auto it = std::upper_bound(chpx_.begin(), chpx_.end(), fc, [](std::uint32_t v, const Run& r) { return v < r.fc_start; });
  if (it != chpx_.begin()) {
    const Run& run = *std::prev(it);
    if (fc >= run.fc_start && fc < run.fc_end) {
      CharProps direct;
      apply_char_sprms(run.grpprl, direct);
      if (direct.istd >= 0) style_char_props(direct.istd, props);
      apply_char_sprms(run.grpprl, props);
    }
  }
  return props;
}

ParaProps DocReader::para_props_at(std::uint32_t fc) {
  ParaProps props;
  const auto it = std::upper_bound(papx_.begin(), papx_.end(), fc, [](std::uint32_t v, const ParaRun& r) { return v < r.fc_start; });
  if (it != papx_.begin()) {
    const ParaRun& run = *std::prev(it);
    if (fc >= run.fc_start && fc < run.fc_end) {
      props.istd = run.istd;
      style_para_props(run.istd, props);
      props.istd = run.istd;
      apply_para_sprms(run.grpprl, props);
    }
  }
  return props;
}

std::optional<ir::EncodedImage> DocReader::picture(std::uint32_t offset) {
  if (offset >= data_stream_.size()) return std::nullopt;
  const ByteView data = data_stream_;
  const std::uint32_t lcb = le32(data, offset);
  const std::uint16_t cb_header = le16(data, offset + 4);
  if (lcb < cb_header || std::uint64_t{offset} + lcb > data.size() || cb_header < 0x44) return std::nullopt;
  const std::uint16_t mm = le16(data, offset + 6);
  std::size_t at = offset + cb_header;
  const std::size_t end = offset + lcb;
  if (mm == 0x66 && at < end) at += 1 + std::to_integer<std::size_t>(data[at]);
  return find_blip(data, at, end);
}

ir::Blocks DocReader::story(std::uint32_t cp_start, std::uint32_t cp_end, bool main) {
  if (++depth_ > 4) {
    --depth_;
    return {};
  }
  const std::vector<Char> chars = characters(cp_start, cp_end);
  FlowBuilder flow;
  InlineBuilder inlines;
  struct Field {
    std::string instr;
    bool result = false;
    std::string link;
  };
  std::vector<Field> fields;
  // Table state.
  ir::TableBlock table;
  ir::TableRow row;
  ir::Inlines cell;
  bool in_table = false;
  std::vector<int> open_vertical;  // per grid column: row index of a restart cell
  ir::Blocks pending_extra;

  std::vector<std::vector<std::pair<int, int>>> row_bounds;
  const auto flush_table = [&] {
    if (!in_table) return;
    // Cells wider than the narrowest columns span several of them.
    if (row_bounds.size() == table.rows.size()) {
      std::vector<int> edges;
      bool usable = true;
      for (const auto& row_b : row_bounds) {
        if (row_b.empty()) usable = false;
        for (const auto& [l, r] : row_b) edges.push_back(r);
      }
      std::ranges::sort(edges);
      edges.erase(std::unique(edges.begin(), edges.end(), [](int a, int b) { return std::abs(a - b) < 20; }), edges.end());
      if (usable && edges.size() <= 64) {
        for (std::size_t r = 0; r < table.rows.size(); ++r) {
          if (row_bounds[r].size() != table.rows[r].cells.size()) continue;
          for (std::size_t c = 0; c < table.rows[r].cells.size(); ++c) {
            const auto [left, right] = row_bounds[r][c];
            int span = 0;
            for (int e : edges) {
              if (e > left + 10 && e <= right + 10) ++span;
            }
            if (span > table.rows[r].cells[c].colspan) table.rows[r].cells[c].colspan = span;
          }
        }
      }
    }
    row_bounds.clear();
    if (!table.rows.empty()) flow.block(std::move(table));
    table = ir::TableBlock{};
    in_table = false;
  };

  std::size_t i = 0;
  std::size_t para_begin = 0;
  while (i < chars.size()) {
    // Find the end of the paragraph (0x0D, or 0x07 cell mark).
    std::size_t end = i;
    while (end < chars.size() && chars[end].c != 0x0D && chars[end].c != 0x07) ++end;
    const bool cell_mark = end < chars.size() && chars[end].c == 0x07;
    const std::uint32_t mark_fc = end < chars.size() ? chars[end].fc : (chars.empty() ? 0 : chars.back().fc);
    const ParaProps para = para_props_at(mark_fc);
    para_begin = i;
    (void)para_begin;
    const Style* style = para.istd >= 0 && static_cast<std::size_t>(para.istd) < styles_.size() ? &styles_[static_cast<std::size_t>(para.istd)] : nullptr;
    int heading = 0;
    if (para.outline < 9) heading = std::min(para.outline + 1, 6);
    if (heading == 0 && style && style->sti >= 1 && style->sti <= 9) heading = std::min(style->sti, 6);
    if (heading == 0 && style) heading = heading_level_from_style_name(style->name);
    const std::string style_name = style ? util::to_lower(style->name) : std::string();
    const bool structural = heading != 0 || style_name.find("quote") != std::string::npos;

    for (std::size_t k = i; k < end; ++k) {
      const Char& ch = chars[k];
      const CharProps props = char_props_at(ch.fc, structural ? -1 : para.istd);
      if (props.hidden == 1 && !settings_.hidden) continue;
      RunFormat f;
      f.bold = props.bold == 1;
      f.italic = props.italic == 1;
      f.strike = props.strike == 1;
      f.underline = props.underline > 0;
      f.superscript = props.iss == 1;
      f.subscript = props.iss == 2;
      if (props.font >= 0 && static_cast<std::size_t>(props.font) < mono_fonts_.size()) f.code = mono_fonts_[static_cast<std::size_t>(props.font)];
      const bool in_instr = !fields.empty() && !fields.back().result;
      if (!fields.empty() && fields.back().result && !fields.back().link.empty()) {
        f.link = fields.back().link;
        f.underline = false;
      }
      switch (ch.c) {
        case 0x13:
          if (fields.size() < 16) fields.push_back(Field{});
          continue;
        case 0x14:
          if (!fields.empty()) {
            fields.back().result = true;
            std::string_view instr = util::trim(fields.back().instr);
            if (util::istarts_with(instr, "HYPERLINK")) {
              instr = util::trim(instr.substr(9));
              std::string url;
              bool anchor = false;
              while (!instr.empty()) {
                if (instr.starts_with("\\l")) {
                  anchor = true;
                  instr = util::trim(instr.substr(2));
                  continue;
                }
                if (instr.starts_with('\\')) {
                  const std::size_t sp = instr.find(' ');
                  instr = sp == std::string_view::npos ? std::string_view() : util::trim(instr.substr(sp));
                  continue;
                }
                std::string token;
                if (instr.starts_with('"')) {
                  const std::size_t close = instr.find('"', 1);
                  token = std::string(instr.substr(1, close == std::string_view::npos ? std::string_view::npos : close - 1));
                  instr = close == std::string_view::npos ? std::string_view() : util::trim(instr.substr(close + 1));
                } else {
                  const std::size_t sp = instr.find(' ');
                  token = std::string(instr.substr(0, sp));
                  instr = sp == std::string_view::npos ? std::string_view() : util::trim(instr.substr(sp));
                }
                if (anchor) url += "#" + token;
                else if (url.empty()) url = token;
                anchor = false;
              }
              fields.back().link = url;
            }
          }
          continue;
        case 0x15:
          if (!fields.empty()) fields.pop_back();
          continue;
        default: break;
      }
      if (in_instr) {
        if (ch.c >= 0x20) fields.back().instr += util::utf32_to_utf8(std::u32string(1, ch.c));
        continue;
      }
      if (props.special == 1) {
        if (ch.c == 0x01 && props.has_pic) {
          if (auto image = picture(props.pic)) inlines.node(ir::InlineImage{std::move(*image), {}}, RunFormat{});
          continue;
        }
        if (ch.c == 0x02 && main) {
          if (const auto ref = note_refs_.find(ch.cp); ref != note_refs_.end() && settings_.notes) {
            const auto& ranges = ref->second.first == 'f' ? footnote_ranges_ : endnote_ranges_;
            if (ref->second.second < ranges.size()) {
              const auto [s, e] = ranges[ref->second.second];
              const std::string label = std::to_string(footnotes_.size() + 1);
              footnotes_.push_back(ir::Footnote{label, {}});
              const std::size_t slot = footnotes_.size() - 1;
              ir::Blocks body = story(s, e, false);
              footnotes_[slot].blocks = std::move(body);
              ir::Link link;
              link.url = "#fn-" + label;
              link.note = label;
              link.children.push_back(ir::Text{label});
              inlines.node(std::move(link), RunFormat{});
            }
          }
          continue;
        }
        if (ch.c < 0x20) continue;  // other special characters (anchors, drawn objects)
      }
      switch (ch.c) {
        case 0x09: inlines.text("\t", f); break;
        case 0x0B: inlines.line_break(f); break;
        case 0x0C:
          // Page or section break inside a paragraph.
          break;
        case 0x1E: inlines.text("\xE2\x80\x91", f); break;
        case 0x1F: break;
        case 0x08:
        case 0x01:
        case 0x02:
        case 0x05: break;
        default:
          if (ch.c >= 0x20) inlines.text(util::utf32_to_utf8(std::u32string(1, ch.c)), f);
      }
    }
    const bool page_break = std::any_of(chars.begin() + static_cast<std::ptrdiff_t>(i), chars.begin() + static_cast<std::ptrdiff_t>(end),
                                        [](const Char& c) { return c.c == 0x0C; });
    ir::Inlines content = inlines.take();
    trim_inlines(content);
    i = end + 1;

    if (para.in_table && para.itap <= 1) {
      if (!in_table) {
        flow.close_lists();
        in_table = true;
      }
      if (para.row_end) {
        // Row end: apply merges from the row definition.
        ir::TableRow finished;
        finished.header = para.header_row && (table.rows.empty() || table.rows.back().header);
        const std::size_t row_index = table.rows.size();
        if (open_vertical.size() < row.cells.size()) open_vertical.resize(row.cells.size(), -1);
        std::size_t column = 0;
        std::vector<std::pair<int, int>> bounds;
        const auto bound = [&](std::size_t c) {
          return c + 1 < para.centers.size() ? std::pair<int, int>{para.centers[c], para.centers[c + 1]} : std::pair<int, int>{0, 0};
        };
        for (std::size_t c = 0; c < row.cells.size(); ++c) {
          const ParaProps::Cell def = c < para.cells.size() ? para.cells[c] : ParaProps::Cell{};
          if (def.merged && !finished.cells.empty()) {
            finished.cells.back().colspan += 1;
            if (!bounds.empty()) bounds.back().second = bound(c).second;
            ++column;
            continue;
          }
          if (def.vert_merge && !def.vert_restart && column < open_vertical.size() && open_vertical[column] >= 0) {
            // Find the restart cell in its row and extend it.
            ir::TableRow& above = table.rows[static_cast<std::size_t>(open_vertical[column])];
            std::size_t col = 0;
            for (ir::TableCell& c2 : above.cells) {
              if (col == column) {
                c2.rowspan += 1;
                break;
              }
              col += static_cast<std::size_t>(c2.colspan);
            }
            ++column;
            continue;
          }
          if (column >= open_vertical.size()) open_vertical.resize(column + 1, -1);
          open_vertical[column] = def.vert_restart ? static_cast<int>(row_index) : -1;
          finished.cells.push_back(std::move(row.cells[c]));
          bounds.push_back(bound(c));
          ++column;
        }
        row = ir::TableRow{};
        if (!finished.cells.empty()) {
          table.rows.push_back(std::move(finished));
          const bool valid = para.centers.size() > 1 && std::ranges::all_of(bounds, [](const auto& b) { return b.second > b.first; });
          row_bounds.push_back(valid ? bounds : std::vector<std::pair<int, int>>{});
        }
        continue;
      }
      if (!cell.empty() && !content.empty()) cell.push_back(ir::LineBreak{});
      for (ir::Inline& x : content) cell.push_back(std::move(x));
      if (cell_mark) {
        ir::TableCell c;
        c.content = std::move(cell);
        cell.clear();
        row.cells.push_back(std::move(c));
      }
      continue;
    }
    if (para.in_table && para.itap > 1) {
      // Nested tables: their text joins the enclosing cell.
      if (!content.empty()) {
        if (!cell.empty()) cell.push_back(cell_mark ? ir::Inline(ir::Text{" "}) : ir::Inline(ir::LineBreak{}));
        for (ir::Inline& x : content) cell.push_back(std::move(x));
      }
      continue;
    }
    flush_table();
    open_vertical.clear();
    if (content.empty()) {
      if (page_break) flow.block(ir::PageBreak{});
      continue;
    }
    if (heading != 0) {
      if (content.size() == 1 && content.front().is<ir::Strong>()) {
        ir::Inlines inner = std::move(content.front().get_if<ir::Strong>()->children);
        content = std::move(inner);
      }
      flow.block(ir::Heading{heading, std::move(content)});
    } else if (para.ilfo > 0 && static_cast<std::size_t>(para.ilfo) <= lfo_.size()) {
      const auto list = lists_.find(lfo_[static_cast<std::size_t>(para.ilfo) - 1]);
      FlowBuilder::ListInfo info;
      info.list_id = std::to_string(para.ilfo);
      info.level = para.ilvl;
      if (list != lists_.end() && !list->second.empty()) {
        const Level& level = list->second[std::min<std::size_t>(static_cast<std::size_t>(std::max(0, para.ilvl)), list->second.size() - 1)];
        info.ordered = level.ordered;
        info.numbering = level.numbering;
        info.start = level.start;
      }
      flow.list_item(info, ir::Paragraph{std::move(content)});
    } else if (style_name.find("quote") != std::string::npos) {
      flow.block(ir::BlockQuote{{ir::Paragraph{std::move(content)}}});
    } else if (content.size() == 1 && content.front().is<ir::InlineImage>()) {
      auto& image = *content.front().get_if<ir::InlineImage>();
      flow.block(ir::ImageBlock{std::move(image.source), image.alt, {}});
    } else {
      bool all_code = true;
      for (const ir::Inline& x : content) {
        if (!x.is<ir::Code>() && !x.is<ir::LineBreak>()) all_code = false;
      }
      if (all_code) {
        flow.block(ir::CodeBlock{{}, ir::plain_text(content)});
      } else {
        flow.block(ir::Paragraph{std::move(content)});
      }
    }
    if (page_break) flow.block(ir::PageBreak{});
  }
  flush_table();
  --depth_;
  ir::Blocks blocks = flow.take();
  // Merge consecutive code blocks.
  ir::Blocks merged;
  for (ir::Block& b : blocks) {
    if (!merged.empty() && b.is<ir::CodeBlock>() && merged.back().is<ir::CodeBlock>()) {
      merged.back().get_if<ir::CodeBlock>()->code += "\n" + b.get_if<ir::CodeBlock>()->code;
      continue;
    }
    merged.push_back(std::move(b));
  }
  return merged;
}

Result<ir::Document> DocReader::read(const table::CfbReader& cfb) {
  CVT_TRY_ASSIGN(const Bytes word, cfb.read("WordDocument"));
  word_ = word;
  if (word_.size() < 0x200) return fail(Status::Decode, "the WordDocument stream is truncated");
  if (le16(word_, 0) != 0xA5EC && le16(word_, 0) != 0xA5DC) return fail(Status::Decode, "not a Word document");
  const std::uint16_t nfib = le16(word_, 2);
  const std::uint16_t flags = le16(word_, 0x0A);
  if (flags & 0x0100) return fail(Status::Decode, "the document is password-protected; it cannot be read");
  ir::Document doc;
  if (nfib < 101) {
    // Word 6 / Word 95: text between fcMin and fcMac in the WordDocument stream.
    const std::uint32_t fc_min = le32(word_, 0x18);
    const std::uint32_t fc_mac = le32(word_, 0x1C);
    if (fc_min >= fc_mac || fc_mac > word_.size()) return fail(Status::Decode, "the Word 6/95 text range is invalid");
    if (flags & 0x0004) ctx_.warn("Fast-saved Word 6/95 document: the text may be out of order.");
    const table::CodePageInfo* page = table::find_code_page(1252);
    std::string text = table::decode_code_page(word_.subspan(fc_min, fc_mac - fc_min), *page);
    for (char& c : text) {
      if (c == '\r') c = '\n';
      if (c == 0x07 || c == 0x0B || c == 0x0C) c = '\n';
    }
    std::string clean;
    for (char c : text) {
      if (static_cast<unsigned char>(c) >= 0x20 || c == '\n' || c == '\t') clean.push_back(c);
    }
    doc = document::infer_document_from_text(clean);
    ctx_.warn("Word 6/95 document: only the text was read.");
    finish_metadata(doc, settings_);
    return doc;
  }
  const bool table1 = (flags & 0x0200) != 0;
  auto table_stream = cfb.read(table1 ? "1Table" : "0Table");
  if (!table_stream) return fail(Status::Decode, "the document's table stream is missing");
  const ByteView table = *table_stream;
  if (cfb.contains("Data")) {
    if (auto data = cfb.read("Data")) data_stream_ = std::move(*data);
  }
  // FIB: fibBase (32) | csw | fibRgW | cslw | fibRgLw | cbRgFcLcb | fibRgFcLcb
  util::ByteReader fib(word_);
  fib.seek(32);
  const std::uint16_t csw = fib.u16le();
  fib.skip(std::size_t{csw} * 2);
  const std::uint16_t cslw = fib.u16le();
  const std::size_t rg_lw = fib.position();
  fib.skip(std::size_t{cslw} * 4);
  const std::uint16_t cb_fclcb = fib.u16le();
  const std::size_t rg_fclcb = fib.position();
  if (!fib.ok() || cslw < 11 || cb_fclcb < 75) return fail(Status::Decode, "the FIB is truncated");
  ccp_text_ = le32(word_, rg_lw + 4 * 3);
  ccp_ftn_ = le32(word_, rg_lw + 4 * 4);
  ccp_hdd_ = le32(word_, rg_lw + 4 * 5);
  ccp_atn_ = le32(word_, rg_lw + 4 * 7);
  ccp_edn_ = le32(word_, rg_lw + 4 * 8);
  const auto pair = [&](std::size_t index) {
    return std::pair<std::uint32_t, std::uint32_t>{le32(word_, rg_fclcb + 8 * index), le32(word_, rg_fclcb + 8 * index + 4)};
  };
  const auto [fc_clx, lcb_clx] = pair(33);
  CVT_TRY(load_pieces(table, fc_clx, lcb_clx));
  CVT_TRY(ctx_.checkpoint(0.1, "Reading formatting"));
  {
    const auto [fc, lcb] = pair(1);
    load_styles(table, fc, lcb);
  }
  {
    const auto [fc, lcb] = pair(12);
    load_chpx(table, fc, lcb);
  }
  {
    const auto [fc, lcb] = pair(13);
    load_papx(table, fc, lcb);
  }
  {
    const auto [fc_lst, lcb_lst] = pair(73);
    const auto [fc_lfo, lcb_lfo] = pair(74);
    load_lists(table, fc_lst, lcb_lst, fc_lfo, lcb_lfo);
  }
  {
    // Font table: names to spot monospaced fonts.
    const auto [fc, lcb] = pair(15);
    if (lcb >= 4 && std::uint64_t{fc} + lcb <= table.size()) {
      util::ByteReader r(table.subspan(fc, lcb));
      const std::uint16_t count = r.u16le();
      r.skip(2);
      for (std::uint16_t i = 0; i < count && r.remaining() > 40; ++i) {
        const std::uint8_t cb = r.u8();
        const ByteView ffn = r.bytes(cb);
        std::u16string name;
        for (std::size_t k = 39; k + 1 < ffn.size(); k += 2) {
          const char16_t c = static_cast<char16_t>(le16(ffn, k));
          if (c == 0) break;
          name.push_back(c);
        }
        const std::string lower = util::to_lower(util::utf16_to_utf8(name));
        mono_fonts_.push_back(lower.find("courier") != std::string::npos || lower.find("mono") != std::string::npos ||
                              lower.find("consolas") != std::string::npos);
      }
    }
  }
  // Notes.
  const auto load_notes = [&](std::size_t ref_index, std::size_t txt_index, char kind, std::uint32_t story_start,
                              std::vector<std::pair<std::uint32_t, std::uint32_t>>& ranges) {
    const auto [fc_ref, lcb_ref] = pair(ref_index);
    const auto [fc_txt, lcb_txt] = pair(txt_index);
    const std::vector<std::uint32_t> refs = plc_cps(table, fc_ref, lcb_ref, 2);
    const std::vector<std::uint32_t> txt = plc_cps(table, fc_txt, lcb_txt, 0);
    for (std::size_t k = 0; k + 1 < refs.size(); ++k) note_refs_[refs[k]] = {kind, ranges.size() + k};
    for (std::size_t k = 0; k + 1 < txt.size(); ++k) ranges.emplace_back(story_start + txt[k], story_start + txt[k + 1]);
  };
  load_notes(2, 3, 'f', ccp_text_, footnote_ranges_);
  const std::uint32_t edn_start = ccp_text_ + ccp_ftn_ + ccp_hdd_ + ccp_atn_;
  {
    const std::size_t before = endnote_ranges_.size();
    (void)before;
    const auto [fc_ref, lcb_ref] = pair(46);
    const auto [fc_txt, lcb_txt] = pair(47);
    const std::vector<std::uint32_t> refs = plc_cps(table, fc_ref, lcb_ref, 2);
    const std::vector<std::uint32_t> txt = plc_cps(table, fc_txt, lcb_txt, 0);
    for (std::size_t k = 0; k + 1 < refs.size(); ++k) note_refs_[refs[k]] = {'e', k};
    for (std::size_t k = 0; k + 1 < txt.size(); ++k) endnote_ranges_.emplace_back(edn_start + txt[k], edn_start + txt[k + 1]);
  }
  CVT_TRY(ctx_.checkpoint(0.3, "Reading text"));
  doc.blocks = story(0, ccp_text_, true);
  doc.footnotes = std::move(footnotes_);
  // Document properties (SummaryInformation): the title only unless metadata is kept.
  if (auto summary = cfb.read("\x05SummaryInformation")) {
    const ByteView s = *summary;
    const std::uint32_t section = le32(s, 44);
    if (section < s.size()) {
      const std::uint32_t count = le32(s, section + 4);
      for (std::uint32_t k = 0; k < count && k < 256; ++k) {
        const std::uint32_t id = le32(s, section + 8 + 8 * k);
        const std::uint32_t offset = le32(s, section + 12 + 8 * k);
        const std::size_t at = static_cast<std::size_t>(section) + offset;
        if (at + 8 > s.size() || le32(s, at) != 30) continue;  // VT_LPSTR
        const std::uint32_t length = le32(s, at + 4);
        if (length == 0 || at + 8 + length > s.size()) continue;
        std::string value(as_chars(s.subspan(at + 8, length)));
        value = value.substr(0, value.find('\0'));
        value = table::decode_code_page(as_bytes(value), *table::find_code_page(1252));
        if (id == 2) doc.metadata.title = value;
        if (id == 4) doc.metadata.author = value;
        if (id == 3) doc.metadata.extra["subject"] = value;
      }
    }
  }
  finish_metadata(doc, settings_);
  return doc;
}

Result<AnyIR> read_doc(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const table::CfbReader cfb, table::CfbReader::open(ctx.input, ctx.limits));
  if (cfb.contains("EncryptedPackage")) {
    return fail(Status::Decode, "the document is password-protected (encrypted OOXML); it cannot be read");
  }
  if (!cfb.contains("WordDocument")) return fail(Status::Decode, "the compound file is not a Word document");
  DocReader reader(ctx, read_settings(ctx.options));
  CVT_TRY_ASSIGN(ir::Document doc, reader.read(cfb));
  return AnyIR(std::move(doc));
}

// The directory of a compound file names its streams in UTF-16: look for
// "WordDocument" there without parsing the whole file.
bool cfb_mentions(ByteView data, std::u16string_view name) {
  if (data.size() < 512 || !as_chars(data).starts_with("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1")) return false;
  // A real directory lookup (bounded: the reader follows chains with cycle
  // checks); a damaged directory falls back to a name search near the start.
  Limits limits;
  limits.max_archive_entries = 20000;
  limits.max_decompressed_bytes = 1024 * 1024;
  if (auto cfb = table::CfbReader::open(data, limits)) {
    const std::string utf8 = util::utf16_to_utf8(name);
    for (const table::CfbEntry& e : cfb->entries()) {
      if (util::iequals(e.name, utf8)) return true;
    }
    return false;
  }
  std::string needle;
  for (char16_t c : name) {
    needle.push_back(static_cast<char>(c & 0xFF));
    needle.push_back(static_cast<char>(c >> 8));
  }
  return as_chars(data).substr(0, 1024 * 1024).find(needle) != std::string_view::npos;
}

}  // namespace

bool compound_file_has(ByteView data, std::u16string_view stream) { return cfb_mentions(data, stream); }

void register_doc(Registry& registry) {
  FormatInfo info;
  info.id = "doc";
  info.name = "Word 97-2003 document";
  info.description = "Microsoft Word binary document (Word 97 to 2003; text of Word 6 and 95).";
  info.category = Category::Document;
  info.extensions = {"doc"};
  info.mime = "application/msword";
  info.popularity = 60;
  info.sniff = [](ByteView data, std::string_view) {
    return cfb_mentions(data, u"WordDocument") ? 97 : 0;
  };
  registry.add_format(std::move(info));
  Codec codec;
  codec.format_id = "doc";
  codec.kind = Kind::Document;
  codec.read = read_doc;
  codec.read_options = document_read_options();
  registry.add_codec(codec);

  FormatInfo dot;
  dot.id = "dot";
  dot.name = "Word 97-2003 template";
  dot.description = "Microsoft Word binary template (Word 97 to 2003).";
  dot.category = Category::Document;
  dot.extensions = {"dot"};
  dot.mime = "application/msword";
  dot.popularity = 15;
  dot.sniff = [](ByteView data, std::string_view filename) {
    return util::iends_with(filename, ".dot") && cfb_mentions(data, u"WordDocument") ? 98 : 0;
  };
  registry.add_format(std::move(dot));
  codec.format_id = "dot";
  registry.add_codec(std::move(codec));
}

}  // namespace cvt::office
