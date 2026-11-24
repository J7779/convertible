// Convertible core - image family: CCITT T.4 (Group 3, 1D and 2D) and T.6
// (Group 4) bilevel decoding, as used by TIFF compressions 2, 3 and 4.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/image/tiff_ccitt.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace cvt::image::ccitt {

namespace {

struct Code {
  std::uint16_t run;
  std::string_view bits;
};

// T.4 Table 2/T.4 and 3/T.4: terminating and make-up codes.
constexpr Code white_codes[] = {
    {0, "00110101"},    {1, "000111"},      {2, "0111"},        {3, "1000"},        {4, "1011"},
    {5, "1100"},        {6, "1110"},        {7, "1111"},        {8, "10011"},       {9, "10100"},
    {10, "00111"},      {11, "01000"},      {12, "001000"},     {13, "000011"},     {14, "110100"},
    {15, "110101"},     {16, "101010"},     {17, "101011"},     {18, "0100111"},    {19, "0001100"},
    {20, "0001000"},    {21, "0010111"},    {22, "0000011"},    {23, "0000100"},    {24, "0101000"},
    {25, "0101011"},    {26, "0010011"},    {27, "0100100"},    {28, "0011000"},    {29, "00000010"},
    {30, "00000011"},   {31, "00011010"},   {32, "00011011"},   {33, "00010010"},   {34, "00010011"},
    {35, "00010100"},   {36, "00010101"},   {37, "00010110"},   {38, "00010111"},   {39, "00101000"},
    {40, "00101001"},   {41, "00101010"},   {42, "00101011"},   {43, "00101100"},   {44, "00101101"},
    {45, "00000100"},   {46, "00000101"},   {47, "00001010"},   {48, "00001011"},   {49, "01010010"},
    {50, "01010011"},   {51, "01010100"},   {52, "01010101"},   {53, "00100100"},   {54, "00100101"},
    {55, "01011000"},   {56, "01011001"},   {57, "01011010"},   {58, "01011011"},   {59, "01001010"},
    {60, "01001011"},   {61, "00110010"},   {62, "00110011"},   {63, "00110100"},   {64, "11011"},
    {128, "10010"},     {192, "010111"},    {256, "0110111"},   {320, "00110110"},  {384, "00110111"},
    {448, "01100100"},  {512, "01100101"},  {576, "01101000"},  {640, "01100111"},  {704, "011001100"},
    {768, "011001101"}, {832, "011010010"}, {896, "011010011"}, {960, "011010100"}, {1024, "011010101"},
    {1088, "011010110"}, {1152, "011010111"}, {1216, "011011000"}, {1280, "011011001"}, {1344, "011011010"},
    {1408, "011011011"}, {1472, "010011000"}, {1536, "010011001"}, {1600, "010011010"}, {1664, "011000"},
    {1728, "010011011"},
};

constexpr Code black_codes[] = {
    {0, "0000110111"},     {1, "010"},            {2, "11"},             {3, "10"},             {4, "011"},
    {5, "0011"},           {6, "0010"},           {7, "00011"},          {8, "000101"},         {9, "000100"},
    {10, "0000100"},       {11, "0000101"},       {12, "0000111"},       {13, "00000100"},      {14, "00000111"},
    {15, "000011000"},     {16, "0000010111"},    {17, "0000011000"},    {18, "0000001000"},    {19, "00001100111"},
    {20, "00001101000"},   {21, "00001101100"},   {22, "00000110111"},   {23, "00000101000"},   {24, "00000010111"},
    {25, "00000011000"},   {26, "000011001010"},  {27, "000011001011"},  {28, "000011001100"},  {29, "000011001101"},
    {30, "000001101000"},  {31, "000001101001"},  {32, "000001101010"},  {33, "000001101011"},  {34, "000011010010"},
    {35, "000011010011"},  {36, "000011010100"},  {37, "000011010101"},  {38, "000011010110"},  {39, "000011010111"},
    {40, "000001101100"},  {41, "000001101101"},  {42, "000011011010"},  {43, "000011011011"},  {44, "000001010100"},
    {45, "000001010101"},  {46, "000001010110"},  {47, "000001010111"},  {48, "000001100100"},  {49, "000001100101"},
    {50, "000001010010"},  {51, "000001010011"},  {52, "000000100100"},  {53, "000000110111"},  {54, "000000111000"},
    {55, "000000100111"},  {56, "000000101000"},  {57, "000001011000"},  {58, "000001011001"},  {59, "000000101011"},
    {60, "000000101100"},  {61, "000001011010"},  {62, "000001100110"},  {63, "000001100111"},  {64, "0000001111"},
    {128, "000011001000"}, {192, "000011001001"}, {256, "000001011011"}, {320, "000000110011"}, {384, "000000110100"},
    {448, "000000110101"}, {512, "0000001101100"}, {576, "0000001101101"}, {640, "0000001001010"},
    {704, "0000001001011"}, {768, "0000001001100"}, {832, "0000001001101"}, {896, "0000001110010"},
    {960, "0000001110011"}, {1024, "0000001110100"}, {1088, "0000001110101"}, {1152, "0000001110110"},
    {1216, "0000001110111"}, {1280, "0000001010010"}, {1344, "0000001010011"}, {1408, "0000001010100"},
    {1472, "0000001010101"}, {1536, "0000001011010"}, {1600, "0000001011011"}, {1664, "0000001100100"},
    {1728, "0000001100101"},
};

// Extended make-up codes, shared by both colours.
constexpr Code extended_codes[] = {
    {1792, "00000001000"},  {1856, "00000001100"},  {1920, "00000001101"},  {1984, "000000010010"},
    {2048, "000000010011"}, {2112, "000000010100"}, {2176, "000000010101"}, {2240, "000000010110"},
    {2304, "000000010111"}, {2368, "000000011100"}, {2432, "000000011101"}, {2496, "000000011110"},
    {2560, "000000011111"},
};

constexpr int table_bits = 13;

// entry = length << 12 | run (run < 4096); 0 = invalid code.
struct RunTable {
  std::array<std::uint32_t, 1u << table_bits> entries{};
};

void add_code(RunTable& table, const Code& code) {
  std::uint32_t value = 0;
  for (const char c : code.bits) value = (value << 1) | (c == '1' ? 1u : 0u);
  const auto length = static_cast<std::uint32_t>(code.bits.size());
  const std::uint32_t shift = table_bits - length;
  for (std::uint32_t fill = 0; fill < (1u << shift); ++fill) {
    table.entries[(value << shift) | fill] = length << 12 | code.run;
  }
}

struct Tables {
  RunTable white;
  RunTable black;
  Tables() {
    for (const Code& c : white_codes) add_code(white, c);
    for (const Code& c : black_codes) add_code(black, c);
    for (const Code& c : extended_codes) {
      add_code(white, c);
      add_code(black, c);
    }
  }
};

const Tables& tables() {
  static const Tables instance;
  return instance;
}

class Bits {
 public:
  explicit Bits(ByteView data) : data_(data) {}
  [[nodiscard]] std::uint32_t peek(int count) const noexcept {
    std::uint32_t value = 0;
    for (int i = 0; i < count; ++i) {
      const std::size_t bit = position_ + static_cast<std::size_t>(i);
      const std::size_t byte = bit >> 3;
      const std::uint32_t b = byte < data_.size() ? (std::to_integer<std::uint32_t>(data_[byte]) >> (7 - (bit & 7))) & 1u : 0u;
      value = (value << 1) | b;
    }
    return value;
  }
  void skip(int count) noexcept { position_ += static_cast<std::size_t>(count); }
  [[nodiscard]] bool exhausted() const noexcept { return position_ >= data_.size() * 8; }
  void align() noexcept { position_ = (position_ + 7) & ~std::size_t{7}; }
  /// Skip an EOL (eleven or more zeros then a one), if present.
  bool skip_eol() noexcept {
    std::size_t p = position_;
    int zeros = 0;
    while (p < data_.size() * 8) {
      const std::size_t byte = p >> 3;
      const std::uint32_t b = (std::to_integer<std::uint32_t>(data_[byte]) >> (7 - (p & 7))) & 1u;
      ++p;
      if (b == 0) {
        ++zeros;
        continue;
      }
      if (zeros >= 11) {
        position_ = p;
        return true;
      }
      return false;
    }
    return false;
  }

 private:
  ByteView data_;
  std::size_t position_ = 0;
};

/// Read one run (sum of make-up codes and a terminating code). -1 on error.
int read_run(Bits& bits, bool black) {
  const RunTable& table = black ? tables().black : tables().white;
  int total = 0;
  for (int guard = 0; guard < 64; ++guard) {
    const std::uint32_t entry = table.entries[bits.peek(table_bits)];
    if (entry == 0) return -1;
    bits.skip(static_cast<int>(entry >> 12));
    const int run = static_cast<int>(entry & 0xFFF);
    total += run;
    if (run < 64) return total;
    if (total > (1 << 24)) return -1;
  }
  return -1;
}

/// Changing elements of one line: positions where the colour changes,
/// starting with a white-to-black change.
using Changes = std::vector<std::uint32_t>;

void fill_row(std::uint8_t* row, const Changes& changes, std::uint32_t width) {
  // `row` receives 1 for black, 0 for white, one byte per pixel.
  std::fill(row, row + width, std::uint8_t{0});
  for (std::size_t i = 0; i + 1 <= changes.size(); i += 2) {
    const std::uint32_t start = std::min(changes[i], width);
    const std::uint32_t end = i + 1 < changes.size() ? std::min(changes[i + 1], width) : width;
    for (std::uint32_t x = start; x < end; ++x) row[x] = 1;
  }
}

bool decode_1d(Bits& bits, std::uint32_t width, Changes& changes) {
  changes.clear();
  std::uint32_t a0 = 0;
  bool black = false;
  while (a0 < width) {
    const int run = read_run(bits, black);
    if (run < 0) return false;
    a0 += static_cast<std::uint32_t>(run);
    if (a0 > width) a0 = width;
    changes.push_back(a0);
    black = !black;
  }
  return true;
}

bool decode_2d(Bits& bits, std::uint32_t width, const Changes& reference, Changes& changes) {
  changes.clear();
  std::int64_t a0 = -1;
  bool black = false;
  std::size_t ref_index = 0;
  const auto ref_at = [&](std::size_t i) -> std::int64_t { return i < reference.size() ? reference[i] : width; };
  for (int guard = 0; a0 < static_cast<std::int64_t>(width); ++guard) {
    if (guard > static_cast<int>(width) * 4 + 16) return false;
    // b1: first changing element on the reference line to the right of a0
    // whose colour change is to the opposite of the current colour.
    while (ref_index > 0 && ref_at(ref_index - 1) > a0) --ref_index;
    while (ref_at(ref_index) <= a0 || (ref_index % 2 == 1) != black) {
      if (ref_index >= reference.size() + 2) break;
      ++ref_index;
    }
    const std::int64_t b1 = ref_at(ref_index);
    const std::int64_t b2 = ref_at(ref_index + 1);
    const std::uint32_t code = bits.peek(7);
    if (code >> 6 == 1) {  // V0: 1
      bits.skip(1);
      const std::int64_t a1 = std::min<std::int64_t>(b1, width);
      changes.push_back(static_cast<std::uint32_t>(a1));
      a0 = a1;
      black = !black;
    } else if (code >> 3 == 1) {  // pass: 0001
      bits.skip(4);
      a0 = std::min<std::int64_t>(b2, width);
    } else if (code >> 4 == 0b011 || code >> 4 == 0b010) {  // VR1 011 / VL1 010
      bits.skip(3);
      const std::int64_t a1 = b1 + ((code >> 4) == 0b011 ? 1 : -1);
      if (a1 < 0 || a1 > width) return false;
      changes.push_back(static_cast<std::uint32_t>(a1));
      a0 = a1;
      black = !black;
    } else if (code >> 4 == 0b001) {  // horizontal: 001
      bits.skip(3);
      const int r1 = read_run(bits, black);
      const int r2 = read_run(bits, !black);
      if (r1 < 0 || r2 < 0) return false;
      const std::int64_t start = std::max<std::int64_t>(a0, 0);
      const std::int64_t a1 = std::min<std::int64_t>(start + r1, width);
      const std::int64_t a2 = std::min<std::int64_t>(a1 + r2, width);
      changes.push_back(static_cast<std::uint32_t>(a1));
      changes.push_back(static_cast<std::uint32_t>(a2));
      a0 = a2;
    } else if (code >> 1 == 0b000011 || code >> 1 == 0b000010) {  // VR2 / VL2
      bits.skip(6);
      const std::int64_t a1 = b1 + ((code >> 1) == 0b000011 ? 2 : -2);
      if (a1 < 0 || a1 > width) return false;
      changes.push_back(static_cast<std::uint32_t>(a1));
      a0 = a1;
      black = !black;
    } else if (code == 0b0000011 || code == 0b0000010) {  // VR3 / VL3
      bits.skip(7);
      const std::int64_t a1 = b1 + (code == 0b0000011 ? 3 : -3);
      if (a1 < 0 || a1 > width) return false;
      changes.push_back(static_cast<std::uint32_t>(a1));
      a0 = a1;
      black = !black;
    } else {
      return false;  // EOL, extensions (uncompressed mode) or garbage
    }
    if (!changes.empty() && changes.size() >= 2 && changes[changes.size() - 1] < changes[changes.size() - 2]) {
      return false;
    }
  }
  return true;
}

}  // namespace

Result<std::vector<std::uint8_t>> decode(ByteView data, std::uint32_t width, std::uint32_t rows, Mode mode,
                                         bool two_d, bool byte_aligned_eol) {
  std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * rows, 0);
  Bits bits(data);
  Changes reference = {width, width};
  Changes changes;
  changes.reserve(width + 2);
  for (std::uint32_t y = 0; y < rows; ++y) {
    bool ok = false;
    if (mode == Mode::Huffman) {
      ok = decode_1d(bits, width, changes);
      bits.align();
    } else if (mode == Mode::Group3) {
      // Fill bits before a byte-aligned EOL are zeros, which skip_eol absorbs.
      (void)byte_aligned_eol;
      bits.skip_eol();
      bool line_1d = true;
      if (two_d) {
        line_1d = bits.peek(1) == 1;
        bits.skip(1);
      }
      ok = line_1d ? decode_1d(bits, width, changes) : decode_2d(bits, width, reference, changes);
    } else {
      ok = decode_2d(bits, width, reference, changes);
    }
    if (!ok) {
      if (y == 0) return fail(Status::Decode, "CCITT fax data is corrupt");
      break;  // keep what was decoded
    }
    fill_row(out.data() + static_cast<std::size_t>(y) * width, changes, width);
    reference = changes;
    reference.push_back(width);
    reference.push_back(width);
    if (bits.exhausted() && y + 1 < rows) break;
  }
  return out;
}

}  // namespace cvt::image::ccitt
