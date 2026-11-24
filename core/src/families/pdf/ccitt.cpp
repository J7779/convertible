// Convertible core - pdf family: CCITT Group 3 / Group 4 fax decoding (T.4, T.6).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <unordered_map>

#include "families/pdf/bilevel.hpp"

namespace cvt::pdf::bilevel {

namespace {

struct Code {
  const char* bits;
  int value;
};

// T.4 run-length codes (terminating 0-63, make-up 64-1728, extended make-up 1792-2560).
constexpr Code kWhite[] = {
    {"00110101", 0}, {"000111", 1}, {"0111", 2}, {"1000", 3}, {"1011", 4}, {"1100", 5}, {"1110", 6}, {"1111", 7},
    {"10011", 8}, {"10100", 9}, {"00111", 10}, {"01000", 11}, {"001000", 12}, {"000011", 13}, {"110100", 14},
    {"110101", 15}, {"101010", 16}, {"101011", 17}, {"0100111", 18}, {"0001100", 19}, {"0001000", 20},
    {"0010111", 21}, {"0000011", 22}, {"0000100", 23}, {"0101000", 24}, {"0101011", 25}, {"0010011", 26},
    {"0100100", 27}, {"0011000", 28}, {"00000010", 29}, {"00000011", 30}, {"00011010", 31}, {"00011011", 32},
    {"00010010", 33}, {"00010011", 34}, {"00010100", 35}, {"00010101", 36}, {"00010110", 37}, {"00010111", 38},
    {"00101000", 39}, {"00101001", 40}, {"00101010", 41}, {"00101011", 42}, {"00101100", 43}, {"00101101", 44},
    {"00000100", 45}, {"00000101", 46}, {"00001010", 47}, {"00001011", 48}, {"01010010", 49}, {"01010011", 50},
    {"01010100", 51}, {"01010101", 52}, {"00100100", 53}, {"00100101", 54}, {"01011000", 55}, {"01011001", 56},
    {"01011010", 57}, {"01011011", 58}, {"01001010", 59}, {"01001011", 60}, {"00110010", 61}, {"00110011", 62},
    {"00110100", 63}, {"11011", 64}, {"10010", 128}, {"010111", 192}, {"0110111", 256}, {"00110110", 320},
    {"00110111", 384}, {"01100100", 448}, {"01100101", 512}, {"01101000", 576}, {"01100111", 640},
    {"011001100", 704}, {"011001101", 768}, {"011010010", 832}, {"011010011", 896}, {"011010100", 960},
    {"011010101", 1024}, {"011010110", 1088}, {"011010111", 1152}, {"011011000", 1216}, {"011011001", 1280},
    {"011011010", 1344}, {"011011011", 1408}, {"010011000", 1472}, {"010011001", 1536}, {"010011010", 1600},
    {"011000", 1664}, {"010011011", 1728}};

constexpr Code kBlack[] = {
    {"0000110111", 0}, {"010", 1}, {"11", 2}, {"10", 3}, {"011", 4}, {"0011", 5}, {"0010", 6}, {"00011", 7},
    {"000101", 8}, {"000100", 9}, {"0000100", 10}, {"0000101", 11}, {"0000111", 12}, {"00000100", 13},
    {"00000111", 14}, {"000011000", 15}, {"0000010111", 16}, {"0000011000", 17}, {"0000001000", 18},
    {"00001100111", 19}, {"00001101000", 20}, {"00001101100", 21}, {"00000110111", 22}, {"00000101000", 23},
    {"00000010111", 24}, {"00000011000", 25}, {"000011001010", 26}, {"000011001011", 27}, {"000011001100", 28},
    {"000011001101", 29}, {"000001101000", 30}, {"000001101001", 31}, {"000001101010", 32}, {"000001101011", 33},
    {"000011010010", 34}, {"000011010011", 35}, {"000011010100", 36}, {"000011010101", 37}, {"000011010110", 38},
    {"000011010111", 39}, {"000001101100", 40}, {"000001101101", 41}, {"000011011010", 42}, {"000011011011", 43},
    {"000001010100", 44}, {"000001010101", 45}, {"000001010110", 46}, {"000001010111", 47}, {"000001100100", 48},
    {"000001100101", 49}, {"000001010010", 50}, {"000001010011", 51}, {"000000100100", 52}, {"000000110111", 53},
    {"000000111000", 54}, {"000000100111", 55}, {"000000101000", 56}, {"000001011000", 57}, {"000001011001", 58},
    {"000000101011", 59}, {"000000101100", 60}, {"000001011010", 61}, {"000001100110", 62}, {"000001100111", 63},
    {"0000001111", 64}, {"000011001000", 128}, {"000011001001", 192}, {"000001011011", 256}, {"000000110011", 320},
    {"000000110100", 384}, {"000000110101", 448}, {"0000001101100", 512}, {"0000001101101", 576},
    {"0000001001010", 640}, {"0000001001011", 704}, {"0000001001100", 768}, {"0000001001101", 832},
    {"0000001110010", 896}, {"0000001110011", 960}, {"0000001110100", 1024}, {"0000001110101", 1088},
    {"0000001110110", 1152}, {"0000001110111", 1216}, {"0000001010010", 1280}, {"0000001010011", 1344},
    {"0000001010100", 1408}, {"0000001010101", 1472}, {"0000001011010", 1536}, {"0000001011011", 1600},
    {"0000001100100", 1664}, {"0000001100101", 1728}};

constexpr Code kExtended[] = {{"00000001000", 1792}, {"00000001100", 1856}, {"00000001101", 1920},
                              {"000000010010", 1984}, {"000000010011", 2048}, {"000000010100", 2112},
                              {"000000010101", 2176}, {"000000010110", 2240}, {"000000010111", 2304},
                              {"000000011100", 2368}, {"000000011101", 2432}, {"000000011110", 2496},
                              {"000000011111", 2560}};

using Table = std::unordered_map<std::uint32_t, int>;  // (length << 16 | code) -> value

std::uint32_t key_of(const char* bits) {
  std::uint32_t code = 0, len = 0;
  for (const char* p = bits; *p; ++p, ++len) code = (code << 1) | static_cast<std::uint32_t>(*p - '0');
  return (len << 16) | code;
}

Table make_table(const Code* codes, std::size_t n) {
  Table t;
  for (std::size_t i = 0; i < n; ++i) t[key_of(codes[i].bits)] = codes[i].value;
  for (const Code& c : kExtended) t[key_of(c.bits)] = c.value;
  return t;
}

class BitReader {
 public:
  explicit BitReader(ByteView data) : data_(data) {}
  [[nodiscard]] int bit() {
    if (pos_ >= data_.size() * 8) {
      ++overrun_;
      return 0;
    }
    const int b = (std::to_integer<int>(data_[pos_ / 8]) >> (7 - pos_ % 8)) & 1;
    ++pos_;
    return b;
  }
  [[nodiscard]] std::uint32_t peek(int n) const {
    std::uint32_t v = 0;
    for (int i = 0; i < n; ++i) {
      const std::size_t p = pos_ + static_cast<std::size_t>(i);
      const int b = p < data_.size() * 8 ? (std::to_integer<int>(data_[p / 8]) >> (7 - p % 8)) & 1 : 0;
      v = (v << 1) | static_cast<std::uint32_t>(b);
    }
    return v;
  }
  void skip(int n) { pos_ += static_cast<std::size_t>(n); }
  void align() { pos_ = (pos_ + 7) / 8 * 8; }
  [[nodiscard]] bool exhausted() const { return pos_ >= data_.size() * 8; }
  [[nodiscard]] bool overrun() const { return overrun_ > 32; }

 private:
  ByteView data_;
  std::size_t pos_ = 0;
  int overrun_ = 0;
};

// Decode one Huffman value; -1 on failure.
int read_code(BitReader& r, const Table& table) {
  std::uint32_t code = 0;
  for (std::uint32_t len = 1; len <= 13; ++len) {
    code = (code << 1) | static_cast<std::uint32_t>(r.bit());
    const auto it = table.find((len << 16) | code);
    if (it != table.end()) return it->second;
  }
  return -1;
}

int read_run(BitReader& r, const Table& table) {
  int total = 0;
  for (int guard = 0; guard < 64; ++guard) {
    const int v = read_code(r, table);
    if (v < 0) return -1;
    total += v;
    if (v < 64) return total;
  }
  return -1;
}

enum class Mode { Pass, Horizontal, V0, VR1, VR2, VR3, VL1, VL2, VL3, Ext, Eol, Error };

Mode read_mode(BitReader& r) {
  if (r.bit()) return Mode::V0;
  if (r.bit()) return r.bit() ? Mode::VR1 : Mode::VL1;  // 011 / 010
  if (r.bit()) return Mode::Horizontal;                 // 001
  if (r.bit()) return Mode::Pass;                       // 0001
  if (r.bit()) return r.bit() ? Mode::VR2 : Mode::VL2;  // 000011 / 000010
  if (r.bit()) return r.bit() ? Mode::VR3 : Mode::VL3;  // 0000011 / 0000010
  if (r.bit()) return Mode::Ext;                        // 0000001
  // 0000000...: EOL if followed by more zeros and a 1
  for (int i = 0; i < 16; ++i) {
    if (r.bit()) return Mode::Eol;
  }
  return Mode::Error;
}

// Skip fill bits and an EOL code if present. Returns true when an EOL was consumed.
bool skip_eol(BitReader& r) {
  if (r.peek(12) == 1) {
    r.skip(12);
    return true;
  }
  // Fill: zeros before the EOL's final 1.
  int zeros = 0;
  while (zeros < 64 && r.peek(zeros + 1) == 0 && !r.exhausted()) ++zeros;
  if (zeros >= 11 && r.peek(zeros + 1) == 1) {
    r.skip(zeros + 1);
    return true;
  }
  return false;
}

}  // namespace

Result<Bytes> ccitt_decode(ByteView data, const CcittParams& params, std::uint64_t max_output) {
  const Table white = make_table(kWhite, std::size(kWhite));
  const Table black = make_table(kBlack, std::size(kBlack));
  const int columns = static_cast<int>(std::clamp<std::uint32_t>(params.columns, 1, 1u << 20));
  const std::size_t row_bytes = (static_cast<std::size_t>(columns) + 7) / 8;
  const std::size_t max_rows = params.rows > 0 ? params.rows : static_cast<std::size_t>(max_output / row_bytes);
  if (params.rows > 0 && static_cast<std::uint64_t>(params.rows) * row_bytes > max_output) {
    return fail(Status::Limit, "CCITT image exceeds the decompression limit");
  }
  BitReader r(data);
  Bytes out;
  std::vector<int> ref{columns, columns};
  std::vector<int> cur;
  std::vector<std::uint8_t> row(row_bytes);
  const std::uint8_t white_byte = params.black_is_1 ? 0x00 : 0xFF;
  std::size_t decoded_rows = 0;
  bool damaged = false;
  while (decoded_rows < max_rows) {
    if (r.exhausted() || r.overrun()) break;
    bool two_d = params.k < 0;
    if (params.k >= 0) {
      // Optional EOLs before each row (always present when EndOfLine is set).
      int eols = 0;
      while (skip_eol(r)) {
        if (++eols >= 2) break;  // RTC
      }
      if (eols >= 2 && decoded_rows > 0) break;
      if (params.encoded_byte_align && eols == 0) r.align();
      if (params.k > 0) two_d = r.bit() == 0;
    } else if (params.encoded_byte_align) {
      r.align();
    }
    if (r.exhausted()) break;
    cur.clear();
    bool ok = true;
    if (!two_d) {
      int pos = 0;
      int color = 0;
      while (pos < columns) {
        const int run = read_run(r, color ? black : white);
        if (run < 0) {
          ok = false;
          break;
        }
        pos = std::min(columns, pos + run);
        cur.push_back(pos);
        color ^= 1;
      }
    } else {
      int a0 = -1;
      int color = 0;
      std::size_t guard = 0;
      while (a0 < columns && ++guard < static_cast<std::size_t>(columns) * 4 + 16) {
        std::size_t i = 0;
        while (i < ref.size() && (ref[i] <= a0 || static_cast<int>(i & 1) != color)) ++i;
        const int b1 = i < ref.size() ? ref[i] : columns;
        const int b2 = i + 1 < ref.size() ? ref[i + 1] : columns;
        const Mode mode = read_mode(r);
        const auto push = [&](int v) {
          v = std::clamp(v, 0, columns);
          if (!cur.empty() && v < cur.back()) v = cur.back();
          cur.push_back(v);
        };
        switch (mode) {
          case Mode::Pass: a0 = b2; break;
          case Mode::Horizontal: {
            const int start = std::max(a0, 0);
            const int r1 = read_run(r, color ? black : white);
            const int r2 = read_run(r, color ? white : black);
            if (r1 < 0 || r2 < 0) {
              ok = false;
              break;
            }
            push(start + r1);
            push(start + r1 + r2);
            a0 = std::min(columns, start + r1 + r2);
            break;
          }
          case Mode::V0: case Mode::VR1: case Mode::VR2: case Mode::VR3: case Mode::VL1: case Mode::VL2:
          case Mode::VL3: {
            static constexpr int kDelta[] = {0, 0, 0, 1, 2, 3, -1, -2, -3};
            const int d = kDelta[static_cast<int>(mode)];
            const int a1 = std::clamp(b1 + d, 0, columns);
            push(a1);
            a0 = a1;
            color ^= 1;
            break;
          }
          case Mode::Eol:
            // EOFB (G4) or an EOL in the middle of a row.
            if (cur.empty() && a0 < 0) {
              decoded_rows = max_rows;  // end of data
            }
            ok = false;
            break;
          default: ok = false; break;
        }
        if (!ok) break;
      }
    }
    if (!ok && cur.empty() && decoded_rows >= max_rows) break;
    if (!ok) {
      damaged = true;
      if (decoded_rows == 0 && cur.empty()) {
        if (out.empty()) return fail(Status::Decode, "damaged CCITT fax data");
      }
      if (params.rows == 0) break;
    }
    // Paint the row.
    std::fill(row.begin(), row.end(), white_byte);
    int pos = 0;
    int color = 0;
    for (const int change : cur) {
      if (color == 1) {
        for (int x = pos; x < change && x < columns; ++x) {
          const std::size_t byte = static_cast<std::size_t>(x) / 8;
          const auto mask = static_cast<std::uint8_t>(0x80u >> (x % 8));
          if (params.black_is_1) row[byte] |= mask;
          else row[byte] &= static_cast<std::uint8_t>(~mask);
        }
      }
      pos = change;
      color ^= 1;
    }
    out.insert(out.end(), reinterpret_cast<const std::byte*>(row.data()),
               reinterpret_cast<const std::byte*>(row.data()) + row.size());
    ++decoded_rows;
    if (out.size() > max_output) return fail(Status::Limit, "CCITT image exceeds the decompression limit");
    ref = cur;
    ref.push_back(columns);
    ref.push_back(columns);
    if (!ok) break;
    if (params.k < 0 && r.peek(24) == 0x001001) break;  // EOFB
  }
  if (out.empty()) return fail(Status::Decode, "damaged CCITT fax data");
  // Rows that could not be decoded stay white.
  if (params.rows > 0) {
    while (out.size() < static_cast<std::size_t>(params.rows) * row_bytes) out.push_back(static_cast<std::byte>(white_byte));
  }
  (void)damaged;
  return out;
}

}  // namespace cvt::pdf::bilevel
