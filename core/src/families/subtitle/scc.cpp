// Convertible core - subtitle family: Scenarist SCC (CEA-608 line 21 captions).
//
// An SCC file lists time codes (29.97 fps; ';' before the frames marks drop
// frame) followed by CEA-608 byte pairs in hex, one pair per frame. Reading
// runs a CEA-608 decoder for one data channel (CC1 or CC2) with displayed
// and non-displayed memories and the three caption modes:
//   pop-on    RCL loads the hidden memory, EOC swaps it on screen, EDM clears
//   roll-up   RU2/RU3/RU4 write to the screen, CR scrolls the window up
//   paint-on  RDC writes straight to the screen
// and turns every change of the screen into a cue. PACs (row, indent,
// italics, underline), mid-row codes, tab offsets, backspace, delete to end
// of row, special and extended characters are honoured; parity is checked
// leniently and doubled control codes are executed once.
//
// Writing produces pop-on captions: each cue is preloaded into the hidden
// memory (ENM, RCL, PAC per row with centring indent and tab offset, text)
// shortly before its start, shown with EOC at the start frame and cleared
// with EDM at its end (unless the next cue replaces it). Up to four rows of
// 32 characters; italics through PAC and mid-row codes.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "families/subtitle/subtitle.hpp"
#include "util/text.hpp"

namespace cvt::subtitle {

namespace {

// ============================================================================ character sets

constexpr std::array<char32_t, 16> special_chars = {0xAE, 0xB0, 0xBD, 0xBF, 0x2122, 0xA2, 0xA3, 0x266A,
                                                    0xE0, 0xA0, 0xE8, 0xE2, 0xEA,   0xEE, 0xF4, 0xFB};
constexpr std::array<char32_t, 32> extended1 = {0xC1, 0xC9, 0xD3, 0xDA, 0xDC, 0xFC, 0x2018, 0xA1, '*',  0x2019, 0x2014,
                                                0xA9, 0x2120, 0x2022, 0x201C, 0x201D, 0xC0, 0xC2, 0xC7, 0xC8, 0xCA, 0xCB,
                                                0xEB, 0xCE, 0xCF, 0xEF, 0xD4, 0xD9, 0xF9, 0xDB, 0xAB, 0xBB};
constexpr std::array<char32_t, 32> extended2 = {0xC3, 0xE3, 0xCD, 0xCC, 0xEC, 0xD2, 0xF2, 0xD5, 0xF5, '{',    '}',
                                                '\\', '^',  '_',  '|',  '~',  0xC4, 0xE4, 0xD6, 0xF6, 0xDF, 0xA5,
                                                0xA4, 0xA6, 0xC5, 0xE5, 0xD8, 0xF8, 0x250C, 0x2510, 0x2514, 0x2518};

char32_t basic_char(std::uint8_t b) {
  switch (b) {
    case 0x2A: return 0xE1;
    case 0x5C: return 0xE9;
    case 0x5E: return 0xED;
    case 0x5F: return 0xF3;
    case 0x60: return 0xFA;
    case 0x7B: return 0xE7;
    case 0x7C: return 0xF7;
    case 0x7D: return 0xD1;
    case 0x7E: return 0xF1;
    case 0x7F: return 0x2588;
    default: return b;
  }
}

/// PAC row (0-based) from the first byte (channel bit removed) and bit 0x20 of the second.
int pac_row(std::uint8_t b1, std::uint8_t b2) {
  static constexpr std::array<int, 8> base = {10, 0, 2, 11, 13, 4, 6, 8};  // indexed by b1 & 7
  const int row = base[b1 & 7];
  if ((b1 & 7) == 0) return 10;  // row 11 has no second half
  return row + ((b2 & 0x20) != 0 ? 1 : 0);
}

/// PAC bytes for a 0-based row.
std::pair<std::uint8_t, std::uint8_t> pac_bytes(int row) {
  static constexpr std::array<std::pair<std::uint8_t, std::uint8_t>, 15> table = {{
      {0x11, 0x40}, {0x11, 0x60}, {0x12, 0x40}, {0x12, 0x60}, {0x15, 0x40}, {0x15, 0x60}, {0x16, 0x40}, {0x16, 0x60},
      {0x17, 0x40}, {0x17, 0x60}, {0x10, 0x40}, {0x13, 0x40}, {0x13, 0x60}, {0x14, 0x40}, {0x14, 0x60},
  }};
  return table[static_cast<std::size_t>(std::clamp(row, 0, 14))];
}

std::uint8_t with_parity(std::uint8_t b) {
  b &= 0x7F;
  return std::popcount(static_cast<unsigned>(b)) % 2 == 0 ? static_cast<std::uint8_t>(b | 0x80) : b;
}

// ============================================================================ time codes

constexpr double frame_ms = 1001.0 / 30.0;

std::optional<std::int64_t> parse_timecode(std::string_view text) {
  if (text.size() != 11) return std::nullopt;
  int fields[4] = {0, 0, 0, 0};
  for (int i = 0; i < 4; ++i) {
    const std::string_view part = text.substr(static_cast<std::size_t>(i) * 3, 2);
    if (!util::is_ascii_digit(part[0]) || !util::is_ascii_digit(part[1])) return std::nullopt;
    fields[i] = (part[0] - '0') * 10 + (part[1] - '0');
    if (i < 3 && text[static_cast<std::size_t>(i) * 3 + 2] != ':' && text[static_cast<std::size_t>(i) * 3 + 2] != ';' &&
        text[static_cast<std::size_t>(i) * 3 + 2] != '.') {
      return std::nullopt;
    }
  }
  const bool drop = text[8] == ';' || text[8] == '.';
  const std::int64_t minutes = fields[0] * 60 + fields[1];
  std::int64_t frames = (minutes * 60 + fields[2]) * 30 + fields[3];
  if (drop) frames -= 2 * (minutes - minutes / 10);
  return frames;
}

std::string drop_frame_timecode(std::int64_t frame) {
  frame = std::max<std::int64_t>(frame, 0);
  const std::int64_t d = frame / 17982;
  const std::int64_t m = frame % 17982;
  frame += 18 * d + (m > 1 ? 2 * ((m - 2) / 1798) : 0);
  return std::format("{:02}:{:02}:{:02};{:02}", frame / 108000, (frame / 1800) % 60, (frame / 30) % 60, frame % 30);
}

std::int64_t ms_to_frame(std::int64_t ms) { return std::llround(static_cast<double>(ms) / frame_ms); }
std::int64_t frame_to_ms(std::int64_t frame) { return std::llround(static_cast<double>(frame) * frame_ms); }

// ============================================================================ decoder

struct Cell {
  char32_t ch = 0;
  bool italic = false;
  bool underline = false;
};
using Screen = std::array<std::array<Cell, 32>, 15>;

std::string render(const Screen& screen) {
  std::vector<std::string> lines;
  for (const auto& row : screen) {
    int first = -1;
    int last = -1;
    for (int c = 0; c < 32; ++c) {
      if (row[static_cast<std::size_t>(c)].ch != 0 && row[static_cast<std::size_t>(c)].ch != ' ' &&
          row[static_cast<std::size_t>(c)].ch != 0xA0) {
        if (first < 0) first = c;
        last = c;
      }
    }
    if (first < 0) continue;
    std::string line;
    bool italic = false;
    bool underline = false;
    for (int c = first; c <= last; ++c) {
      const Cell& cell = row[static_cast<std::size_t>(c)];
      if (cell.underline != underline) {
        line += cell.underline ? "<u>" : "</u>";
        underline = cell.underline;
      }
      if (cell.italic != italic) {
        line += cell.italic ? "<i>" : "</i>";
        italic = cell.italic;
      }
      util::append_utf8(line, cell.ch == 0 || cell.ch == 0xA0 ? U' ' : cell.ch);
    }
    if (italic) line += "</i>";
    if (underline) line += "</u>";
    lines.push_back(std::move(line));
  }
  return balance_markup(util::join(lines, "\n"));
}

class Decoder {
 public:
  explicit Decoder(int channel) : channel_(channel) {}

  void word(std::uint8_t hi, std::uint8_t lo, std::int64_t frame) {
    const std::uint8_t b1 = hi & 0x7F;
    const std::uint8_t b2 = lo & 0x7F;
    frame_ = frame;
    if (b1 == 0 && b2 == 0) {
      last_control_ = 0;
      return;
    }
    if (b1 >= 0x10 && b1 <= 0x1F) {
      const auto word_value = static_cast<std::uint16_t>((b1 << 8) | b2);
      if (word_value == last_control_) {  // the redundant copy of a doubled code
        last_control_ = 0;
        return;
      }
      last_control_ = word_value;
      current_channel_ = (b1 & 0x08) != 0 ? 2 : 1;
      if (current_channel_ != channel_) return;
      control(static_cast<std::uint8_t>(b1 & 0x17), b2);
      return;
    }
    last_control_ = 0;
    if (current_channel_ != channel_) return;
    if (b1 >= 0x20) put(basic_char(b1));
    if (b2 >= 0x20) put(basic_char(b2));
  }

  /// End of an SCC line: publish screen changes of roll-up / paint-on text.
  void end_of_burst() {
    if (dirty_) {
      // The screen started changing with the first character of the burst.
      const std::int64_t now = frame_;
      frame_ = dirty_since_;
      snapshot();
      frame_ = now;
    }
  }

  std::vector<std::pair<std::int64_t, std::string>> take() {
    end_of_burst();
    return std::move(changes_);
  }

 private:
  enum class Mode { PopOn, RollUp, PaintOn };

  Screen& target() { return mode_ == Mode::PopOn ? hidden_ : shown_; }

  void mark_dirty() {
    if (!dirty_) dirty_since_ = frame_;
    dirty_ = true;
  }

  void snapshot() {
    dirty_ = false;
    std::string text = render(shown_);
    if (!changes_.empty() && changes_.back().second == text) return;
    if (!changes_.empty() && changes_.back().first == frame_) {
      changes_.back().second = std::move(text);
      return;
    }
    changes_.emplace_back(frame_, std::move(text));
  }

  void put(char32_t ch) {
    Screen& screen = target();
    if (column_ > 31) column_ = 31;
    screen[static_cast<std::size_t>(row_)][static_cast<std::size_t>(column_)] = {ch, italic_, underline_};
    ++column_;
    if (mode_ != Mode::PopOn) mark_dirty();
  }

  void control(std::uint8_t b1, std::uint8_t b2) {
    if ((b1 == 0x14 || b1 == 0x15) && b2 >= 0x20 && b2 <= 0x2F) {
      misc(b2);
    } else if (b1 == 0x17 && b2 >= 0x21 && b2 <= 0x23) {
      column_ = std::min(column_ + (b2 - 0x20), 31);  // tab offset
    } else if (b1 == 0x11 && b2 >= 0x20 && b2 <= 0x2F) {
      // Mid-row code: a space carrying the new style.
      italic_ = (b2 & 0x0E) == 0x0E;
      underline_ = (b2 & 1) != 0;
      put(U' ');
    } else if (b1 == 0x11 && b2 >= 0x30 && b2 <= 0x3F) {
      put(special_chars[b2 - 0x30]);
    } else if ((b1 == 0x12 || b1 == 0x13) && b2 >= 0x20 && b2 <= 0x3F) {
      // Extended characters replace the standard character sent before them.
      if (column_ > 0) --column_;
      put((b1 == 0x12 ? extended1 : extended2)[b2 - 0x20]);
    } else if (b2 >= 0x40) {
      pac(b1, b2);
    }
  }

  void pac(std::uint8_t b1, std::uint8_t b2) {
    const int row = pac_row(b1, b2);
    if (mode_ == Mode::RollUp) {
      // Moving the base row moves the whole roll-up window.
      if (row != base_row_) {
        Screen moved{};
        for (int r = 0; r < rollup_rows_; ++r) {
          const int from = base_row_ - r;
          const int to = row - r;
          if (from >= 0 && to >= 0) moved[static_cast<std::size_t>(to)] = shown_[static_cast<std::size_t>(from)];
        }
        shown_ = moved;
        base_row_ = row;
      }
      row_ = base_row_;
    } else {
      row_ = row;
    }
    const std::uint8_t attributes = b2 & 0x1F;
    underline_ = (attributes & 1) != 0;
    if (attributes >= 0x10) {
      column_ = ((attributes & 0x0E) >> 1) * 4;
      italic_ = false;
    } else {
      column_ = 0;
      italic_ = (attributes & 0x0E) == 0x0E;
    }
  }

  void misc(std::uint8_t code) {
    switch (code) {
      case 0x20:  // RCL resume caption loading
        mode_ = Mode::PopOn;
        break;
      case 0x21:  // BS backspace
        if (column_ > 0) {
          --column_;
          target()[static_cast<std::size_t>(row_)][static_cast<std::size_t>(column_)] = {};
          if (mode_ != Mode::PopOn) mark_dirty();
        }
        break;
      case 0x24:  // DER delete to end of row
        for (int c = column_; c < 32; ++c) target()[static_cast<std::size_t>(row_)][static_cast<std::size_t>(c)] = {};
        if (mode_ != Mode::PopOn) mark_dirty();
        break;
      case 0x25:
      case 0x26:
      case 0x27: {  // RU2 RU3 RU4
        if (mode_ != Mode::RollUp) {
          shown_ = {};
          hidden_ = {};
          mark_dirty();
          base_row_ = 14;
        }
        mode_ = Mode::RollUp;
        rollup_rows_ = code - 0x23;
        row_ = base_row_;
        column_ = 0;
        break;
      }
      case 0x29:  // RDC resume direct captioning
        mode_ = Mode::PaintOn;
        break;
      case 0x2C:  // EDM erase displayed memory
        shown_ = {};
        snapshot();
        break;
      case 0x2D:  // CR carriage return
        if (mode_ == Mode::RollUp) {
          end_of_burst();
          const int top = std::max(base_row_ - rollup_rows_ + 1, 0);
          for (int r = top; r < base_row_; ++r) shown_[static_cast<std::size_t>(r)] = shown_[static_cast<std::size_t>(r + 1)];
          if (base_row_ - rollup_rows_ >= 0) shown_[static_cast<std::size_t>(base_row_ - rollup_rows_)] = {};
          shown_[static_cast<std::size_t>(base_row_)] = {};
          column_ = 0;
          row_ = base_row_;
        } else if (row_ < 14) {
          ++row_;
          column_ = 0;
        }
        break;
      case 0x2E:  // ENM erase non-displayed memory
        hidden_ = {};
        break;
      case 0x2F:  // EOC end of caption: swap memories
        std::swap(shown_, hidden_);
        mode_ = Mode::PopOn;
        snapshot();
        break;
      default:  // FON, TR, RTD, AOF, AON: no effect on caption text
        break;
    }
  }

  int channel_;
  int current_channel_ = 1;
  std::uint16_t last_control_ = 0;
  Mode mode_ = Mode::PopOn;
  Screen shown_{};
  Screen hidden_{};
  int row_ = 14;
  int column_ = 0;
  int base_row_ = 14;
  int rollup_rows_ = 2;
  bool italic_ = false;
  bool underline_ = false;
  bool dirty_ = false;
  std::int64_t dirty_since_ = 0;
  std::int64_t frame_ = 0;
  std::vector<std::pair<std::int64_t, std::string>> changes_;
};

Result<ir::Subtitles> parse_scc(std::string_view text, const ReadContext& ctx) {
  const std::vector<std::string_view> lines = text_lines(text);
  if (lines.empty() || !util::trim(lines[0]).starts_with("Scenarist_SCC")) {
    return fail(Status::Decode, "not a Scenarist SCC file (missing the 'Scenarist_SCC V1.0' header)");
  }
  const int channel = static_cast<int>(ctx.options.get_int("scc_channel", 1, 1, 2));
  Decoder decoder(channel);
  std::int64_t last_frame = 0;
  for (std::size_t i = 1; i < lines.size(); ++i) {
    if ((i & 1023) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(lines.size())));
    const std::string_view line = util::trim(lines[i]);
    if (line.empty()) continue;
    const std::size_t tab = line.find_first_of(" \t");
    if (tab == std::string_view::npos) continue;
    const auto start = parse_timecode(line.substr(0, tab));
    if (!start) continue;
    // Time codes never run backwards; a late burst continues where the last ended.
    std::int64_t frame = std::max(*start, last_frame);
    for (const std::string_view token : util::split(line.substr(tab + 1), ' ', false)) {
      const std::string_view hex = util::trim(token);
      if (hex.size() != 4) continue;
      const auto value = util::parse_int(hex, 16);
      if (!value) continue;
      decoder.word(static_cast<std::uint8_t>(*value >> 8), static_cast<std::uint8_t>(*value & 0xFF), frame);
      ++frame;
    }
    decoder.end_of_burst();
    last_frame = frame;
  }
  ir::Subtitles subs;
  const std::vector<std::pair<std::int64_t, std::string>> changes = decoder.take();
  for (std::size_t i = 0; i < changes.size(); ++i) {
    if (is_blank_text(changes[i].second)) continue;
    ir::Cue cue;
    cue.start_ms = frame_to_ms(changes[i].first);
    cue.end_ms = i + 1 < changes.size() ? frame_to_ms(changes[i + 1].first) : cue.start_ms + 5000;
    cue.text = changes[i].second;
    subs.cues.push_back(std::move(cue));
  }
  return subs;
}

// ============================================================================ encoder

struct Encoded {
  std::vector<std::uint16_t> words;
  std::vector<std::uint8_t> pending;  // characters waiting to fill a word
};

void flush_chars(Encoded& out) {
  if (out.pending.empty()) return;
  const std::uint8_t a = with_parity(out.pending[0]);
  const std::uint8_t b = out.pending.size() > 1 ? with_parity(out.pending[1]) : with_parity(0);
  out.words.push_back(static_cast<std::uint16_t>((a << 8) | b));
  out.pending.clear();
}

void add_char(Encoded& out, std::uint8_t c) {
  out.pending.push_back(c);
  if (out.pending.size() == 2) flush_chars(out);
}

void add_control(Encoded& out, std::uint8_t b1, std::uint8_t b2, bool doubled = true) {
  flush_chars(out);
  const auto word = static_cast<std::uint16_t>((with_parity(b1) << 8) | with_parity(b2));
  out.words.push_back(word);
  if (doubled) out.words.push_back(word);
}

/// ASCII stand-ins sent before an extended character.
std::uint8_t fallback(char32_t c) {
  switch (c) {
    case 0xC1: case 0xC0: case 0xC2: case 0xC3: case 0xC4: case 0xC5: return 'A';
    case 0xE3: case 0xE4: case 0xE5: return 'a';
    case 0xC9: case 0xC8: case 0xCA: case 0xCB: return 'E';
    case 0xEB: return 'e';
    case 0xD3: case 0xD2: case 0xD4: case 0xD5: case 0xD6: case 0xD8: return 'O';
    case 0xF2: case 0xF5: case 0xF6: case 0xF8: return 'o';
    case 0xDA: case 0xD9: case 0xDB: case 0xDC: return 'U';
    case 0xFC: case 0xF9: return 'u';
    case 0xCD: case 0xCC: case 0xCE: case 0xCF: return 'I';
    case 0xEC: case 0xEF: return 'i';
    case 0xC7: return 'C';
    case 0xDF: return 's';
    case 0x201C: case 0x201D: case 0xAB: case 0xBB: return '"';
    case 0x2018: case 0x2019: return '\'';
    case 0xA1: return '!';
    case 0x2014: return '-';
    default: return ' ';
  }
}

/// Append one character in the CEA-608 repertoire; false when it has none.
bool encode_char(Encoded& out, char32_t c) {
  if (c >= 0x20 && c < 0x7F && c != '*' && c != '\\' && c != '^' && c != '_' && c != '`' && c != '{' && c != '|' &&
      c != '}' && c != '~') {
    add_char(out, static_cast<std::uint8_t>(c));
    return true;
  }
  for (std::uint8_t b = 0x20; b < 0x80; ++b) {
    if (b != c && basic_char(b) == c) {
      add_char(out, b);
      return true;
    }
  }
  for (std::size_t i = 0; i < special_chars.size(); ++i) {
    if (special_chars[i] == c && c != 0xA0) {
      add_control(out, 0x11, static_cast<std::uint8_t>(0x30 + i));
      return true;
    }
  }
  for (std::size_t set = 0; set < 2; ++set) {
    const auto& table = set == 0 ? extended1 : extended2;
    for (std::size_t i = 0; i < table.size(); ++i) {
      if (table[i] != c) continue;
      add_char(out, fallback(c));
      add_control(out, set == 0 ? 0x12 : 0x13, static_cast<std::uint8_t>(0x20 + i));
      return true;
    }
  }
  if (c == '`') {
    add_char(out, '\'');
    return true;
  }
  return false;
}

struct Line {
  std::vector<std::pair<char32_t, bool>> chars;  // character, italic
};

/// Wrap a cue's text into lines of at most 32 characters.
std::vector<Line> layout(std::string_view ir_text, bool& truncated) {
  std::vector<Line> lines;
  std::vector<std::vector<std::pair<char32_t, bool>>> source(1);
  bool italic = false;
  for (const MarkupToken& token : tokenize_markup(ir_text)) {
    if (token.type == MarkupToken::Type::LineBreak) source.emplace_back();
    if ((token.type == MarkupToken::Type::Open || token.type == MarkupToken::Type::Close) && token.text == "i") {
      italic = token.type == MarkupToken::Type::Open;
    }
    if (token.type == MarkupToken::Type::Text) {
      for (const char32_t c : util::utf8_to_utf32(token.text)) source.back().emplace_back(c, italic);
    }
  }
  for (auto& chars : source) {
    // Greedy word wrap at 32 columns.
    while (!chars.empty() && chars.front().first == U' ') chars.erase(chars.begin());
    while (!chars.empty() && chars.back().first == U' ') chars.pop_back();
    while (chars.size() > 32) {
      std::size_t cut = 32;
      while (cut > 0 && chars[cut].first != U' ') --cut;
      if (cut == 0) cut = 32;
      Line line;
      line.chars.assign(chars.begin(), chars.begin() + static_cast<std::ptrdiff_t>(cut));
      while (!line.chars.empty() && line.chars.back().first == U' ') line.chars.pop_back();
      lines.push_back(std::move(line));
      chars.erase(chars.begin(), chars.begin() + static_cast<std::ptrdiff_t>(cut));
      while (!chars.empty() && chars.front().first == U' ') chars.erase(chars.begin());
    }
    if (!chars.empty()) lines.push_back(Line{chars});
  }
  if (lines.size() > 4) {
    truncated = true;
    lines.resize(4);
  }
  return lines;
}

Result<std::string> write_scc(const ir::Subtitles& subs, const WriteContext& ctx) {
  const int channel = static_cast<int>(ctx.options.get_int("scc_channel", 1, 1, 2));
  const std::uint8_t ch = channel == 2 ? 0x08 : 0x00;
  bool truncated = false;
  bool unmappable = false;
  std::vector<std::pair<std::int64_t, std::vector<std::uint16_t>>> bursts;  // frame, words

  const auto preload = [&](const ir::Cue& cue) {
    Encoded out;
    add_control(out, 0x14 | ch, 0x2E);  // ENM
    add_control(out, 0x14 | ch, 0x20);  // RCL
    const std::vector<Line> lines = layout(cue.text, truncated);
    const int alignment = cue.position ? std::clamp(cue.position->alignment, 1, 9) : 2;
    const bool top = alignment >= 7;
    const int first_row = top ? 1 : 15 - static_cast<int>(lines.size());
    for (std::size_t n = 0; n < lines.size(); ++n) {
      const Line& line = lines[n];
      const int length = static_cast<int>(line.chars.size());
      const int column_kind = (alignment - 1) % 3;
      int column = column_kind == 0 ? 0 : column_kind == 2 ? 32 - length : (32 - length) / 2;
      const bool all_italic = std::ranges::all_of(line.chars, [](const auto& c) { return c.second; });
      auto [b1, b2] = pac_bytes(first_row + static_cast<int>(n));
      b1 = static_cast<std::uint8_t>(b1 | ch);
      bool italic = false;
      if (all_italic && column == 0) {
        add_control(out, b1, static_cast<std::uint8_t>(b2 | 0x0E));  // italics PAC at column 0
        italic = true;
      } else {
        // An indent PAC is never italic: an italic line starts with a mid-row
        // code, which takes one column.
        if (all_italic) --column;
        add_control(out, b1, static_cast<std::uint8_t>(b2 | 0x10 | ((column / 4) << 1)));
        if (column % 4 != 0) add_control(out, 0x17 | ch, static_cast<std::uint8_t>(0x20 + column % 4));
      }
      for (const auto& [c, it] : line.chars) {
        if (it != italic) {
          add_control(out, 0x11 | ch, it ? 0x2E : 0x20);  // mid-row italics / plain white
          italic = it;
        }
        if (!encode_char(out, c)) {
          unmappable = true;
          add_char(out, '?');
        }
      }
    }
    flush_chars(out);
    return out.words;
  };

  // Each cue: preload (hidden memory, any time after the previous EOC),
  // EOC at the start frame, EDM at the end unless the next cue replaces it.
  // A preload may go before the previous cue's EDM when it would otherwise
  // delay the next cue.
  std::int64_t next_frame = 0;
  std::optional<std::int64_t> pending_edm;
  const auto emit_edm = [&] {
    if (!pending_edm) return;
    const std::int64_t at = std::max(next_frame, *pending_edm);
    Encoded edm;
    add_control(edm, 0x14 | ch, 0x2C);
    bursts.emplace_back(at, edm.words);
    next_frame = at + 2;
    pending_edm.reset();
  };
  for (std::size_t i = 0; i < subs.cues.size(); ++i) {
    if ((i & 1023) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(subs.cues.size())));
    const ir::Cue& cue = subs.cues[i];
    std::vector<std::uint16_t> words = preload(cue);
    const auto length = static_cast<std::int64_t>(words.size());
    const std::int64_t start = ms_to_frame(cue.start_ms);
    const std::int64_t end = std::max(ms_to_frame(cue.end_ms), start + 1);
    std::int64_t load_at = std::max(next_frame, start - length);
    if (pending_edm) {
      const std::int64_t early = std::max(next_frame, std::min(start - length, *pending_edm - length));
      if (early + length <= *pending_edm) {
        load_at = early;  // fits before the previous cue is cleared
      } else {
        emit_edm();
        load_at = std::max(next_frame, start - length);
      }
    }
    bursts.emplace_back(load_at, std::move(words));
    next_frame = load_at + length;
    emit_edm();
    const std::int64_t eoc_at = std::max(next_frame, start);
    Encoded eoc;
    add_control(eoc, 0x14 | ch, 0x2F);
    bursts.emplace_back(eoc_at, eoc.words);
    next_frame = eoc_at + 2;
    const bool replaced = i + 1 < subs.cues.size() && ms_to_frame(subs.cues[i + 1].start_ms) <= end;
    if (!replaced) pending_edm = end;
  }
  emit_edm();
  std::string out = "Scenarist_SCC V1.0\n";
  for (const auto& [frame, words] : bursts) {
    out += "\n" + drop_frame_timecode(frame) + "\t";
    for (std::size_t w = 0; w < words.size(); ++w) out += std::format("{}{:04x}", w == 0 ? "" : " ", words[w]);
    out += "\n";
  }
  if (truncated) ctx.warn("CEA-608 captions hold four rows of 32 characters; longer cues were cut.");
  if (unmappable) ctx.warn("Some characters have no CEA-608 equivalent and were written as '?'.");
  return out;
}

int sniff_scc(ByteView data, std::string_view /*filename*/) {
  return sniff_prefix(data, 64).starts_with("Scenarist_SCC") ? 98 : 0;
}

}  // namespace

void register_scc(Registry& registry) {
  FormatInfo info = subtitle_format("scc", "Scenarist closed captions", "CEA-608 line-21 captions in Scenarist SCC files.",
                                    {"scc"}, "text/x-scc", 30);
  info.sniff = sniff_scc;
  info.signatures = {Signature::at(0, "'Scenarist_SCC'")};
  registry.add_format(std::move(info));
  const OptionSpec channel = OptionSpec::integer("scc_channel", "Caption channel", 1, 1, 2,
                                                 "CEA-608 data channel: 1 = CC1 (primary), 2 = CC2.");
  registry.add_codec(make_text_codec("scc", parse_scc, write_scc, {channel}, {channel}, "crlf"));
}

}  // namespace cvt::subtitle
