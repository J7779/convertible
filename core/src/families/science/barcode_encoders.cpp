// Convertible core - science family: barcode encoders. QR Code (ISO/IEC
// 18004, all versions and error-correction levels, numeric/alphanumeric/byte
// segments, best mask by the standard penalty rules), Data Matrix ECC 200
// (ISO/IEC 16022, square and rectangular symbols, ASCII encodation with ECI
// for UTF-8), Code 128 (sets A/B/C with switching) and EAN-13 / UPC-A.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/science/barcode.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string>

namespace cvt::science::barcode {

namespace {

// --------------------------------------------------------------------------- Reed-Solomon

struct Galois {
  std::array<std::uint8_t, 512> exp{};
  std::array<int, 256> log{};
  explicit Galois(int poly) {
    int x = 1;
    for (int i = 0; i < 255; ++i) {
      exp[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(x);
      log[static_cast<std::size_t>(x)] = i;
      x <<= 1;
      if (x & 0x100) x ^= poly;
    }
    for (int i = 255; i < 512; ++i) exp[static_cast<std::size_t>(i)] = exp[static_cast<std::size_t>(i - 255)];
  }
  [[nodiscard]] std::uint8_t mul(std::uint8_t a, std::uint8_t b) const {
    if (a == 0 || b == 0) return 0;
    return exp[static_cast<std::size_t>(log[a] + log[b])];
  }
};

/// Generator polynomial (monic, coefficients high to low, leading 1 omitted)
/// with roots alpha^first .. alpha^(first+degree-1).
std::vector<std::uint8_t> generator(const Galois& gf, int degree, int first) {
  std::vector<std::uint8_t> g = {1};
  for (int i = 0; i < degree; ++i) {
    const std::uint8_t root = gf.exp[static_cast<std::size_t>(i + first)];
    std::vector<std::uint8_t> next(g.size() + 1, 0);
    for (std::size_t j = 0; j < g.size(); ++j) {
      next[j] ^= g[j];
      next[j + 1] ^= gf.mul(g[j], root);
    }
    g = std::move(next);
  }
  g.erase(g.begin());
  return g;
}

std::vector<std::uint8_t> rs_remainder(const Galois& gf, const std::vector<std::uint8_t>& data, const std::vector<std::uint8_t>& gen) {
  std::vector<std::uint8_t> rem(gen.size(), 0);
  for (const std::uint8_t b : data) {
    const std::uint8_t factor = b ^ rem.front();
    rem.erase(rem.begin());
    rem.push_back(0);
    for (std::size_t i = 0; i < gen.size(); ++i) rem[i] ^= gf.mul(gen[i], factor);
  }
  return rem;
}

// --------------------------------------------------------------------------- QR

constexpr std::int8_t ecc_per_block[4][41] = {
    {-1, 7, 10, 15, 20, 26, 18, 20, 24, 30, 18, 20, 24, 26, 30, 22, 24, 28, 30, 28, 28, 28, 28, 30, 30, 26, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},
    {-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26, 26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28},
    {-1, 13, 22, 18, 26, 18, 24, 18, 22, 20, 24, 28, 26, 24, 20, 30, 24, 28, 28, 26, 30, 28, 30, 30, 30, 30, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},
    {-1, 17, 28, 22, 16, 22, 28, 26, 26, 24, 28, 24, 28, 22, 24, 24, 30, 28, 28, 26, 28, 30, 24, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30}};
constexpr std::int8_t num_blocks[4][41] = {
    {-1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4, 4, 4, 4, 4, 6, 6, 6, 6, 7, 8, 8, 9, 9, 10, 12, 12, 12, 13, 14, 15, 16, 17, 18, 19, 19, 20, 21, 22, 24, 25},
    {-1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5, 5, 8, 9, 9, 10, 10, 11, 13, 14, 16, 17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49},
    {-1, 1, 1, 2, 2, 4, 4, 6, 6, 8, 8, 8, 10, 12, 16, 12, 17, 16, 18, 21, 20, 23, 23, 25, 27, 29, 34, 34, 35, 38, 40, 43, 45, 48, 51, 53, 56, 59, 62, 65, 68},
    {-1, 1, 1, 2, 4, 4, 4, 5, 6, 8, 8, 11, 11, 16, 16, 18, 16, 19, 21, 25, 25, 25, 34, 30, 32, 35, 37, 40, 42, 45, 48, 51, 54, 57, 60, 63, 66, 70, 74, 77, 81}};

int raw_modules(int ver) {
  int result = (16 * ver + 128) * ver + 64;
  if (ver >= 2) {
    const int align = ver / 7 + 2;
    result -= (25 * align - 10) * align - 55;
    if (ver >= 7) result -= 36;
  }
  return result;
}

int data_codewords(int ver, int ecc) {
  return raw_modules(ver) / 8 - ecc_per_block[ecc][ver] * num_blocks[ecc][ver];
}

constexpr std::string_view alnum_chars = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";

struct BitBuffer {
  std::vector<bool> bits;
  void put(std::uint32_t value, int count) {
    for (int i = count - 1; i >= 0; --i) bits.push_back(((value >> i) & 1) != 0);
  }
};

enum class Mode { Numeric, Alnum, Byte };

int count_bits(Mode mode, int ver) {
  const int band = ver <= 9 ? 0 : ver <= 26 ? 1 : 2;
  switch (mode) {
    case Mode::Numeric: return std::array{10, 12, 14}[static_cast<std::size_t>(band)];
    case Mode::Alnum: return std::array{9, 11, 13}[static_cast<std::size_t>(band)];
    default: return std::array{8, 16, 16}[static_cast<std::size_t>(band)];
  }
}

struct QrBuilder {
  int ver;
  int size;
  std::vector<std::uint8_t> modules;
  std::vector<std::uint8_t> function;

  explicit QrBuilder(int v) : ver(v), size(v * 4 + 17), modules(static_cast<std::size_t>(size * size), 0), function(modules.size(), 0) {}
  void set(int x, int y, bool dark) {
    modules[static_cast<std::size_t>(y * size + x)] = dark ? 1 : 0;
    function[static_cast<std::size_t>(y * size + x)] = 1;
  }
  [[nodiscard]] bool get(int x, int y) const { return modules[static_cast<std::size_t>(y * size + x)] != 0; }
  [[nodiscard]] bool is_function(int x, int y) const { return function[static_cast<std::size_t>(y * size + x)] != 0; }

  std::vector<int> alignment_positions() const {
    if (ver == 1) return {};
    const int count = ver / 7 + 2;
    const int step = (ver * 8 + count * 3 + 5) / (count * 4 - 4) * 2;
    std::vector<int> out;
    for (int i = 0, pos = size - 7; i < count - 1; ++i, pos -= step) out.insert(out.begin(), pos);
    out.insert(out.begin(), 6);
    return out;
  }

  void finder(int cx, int cy) {
    for (int dy = -4; dy <= 4; ++dy) {
      for (int dx = -4; dx <= 4; ++dx) {
        const int x = cx + dx, y = cy + dy;
        if (x < 0 || y < 0 || x >= size || y >= size) continue;
        const int dist = std::max(std::abs(dx), std::abs(dy));
        set(x, y, dist != 2 && dist != 4);
      }
    }
  }

  void format_bits(int ecc_bits, int mask) {
    const int data = ecc_bits << 3 | mask;
    int rem = data;
    for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    const int bits = (data << 10 | rem) ^ 0x5412;
    const auto bit = [bits](int i) { return ((bits >> i) & 1) != 0; };
    for (int i = 0; i <= 5; ++i) set(8, i, bit(i));
    set(8, 7, bit(6));
    set(8, 8, bit(7));
    set(7, 8, bit(8));
    for (int i = 9; i < 15; ++i) set(14 - i, 8, bit(i));
    for (int i = 0; i < 8; ++i) set(size - 1 - i, 8, bit(i));
    for (int i = 8; i < 15; ++i) set(8, size - 15 + i, bit(i));
    set(8, size - 8, true);
  }

  void function_patterns() {
    for (int i = 0; i < size; ++i) {
      set(6, i, i % 2 == 0);
      set(i, 6, i % 2 == 0);
    }
    finder(3, 3);
    finder(size - 4, 3);
    finder(3, size - 4);
    const auto pos = alignment_positions();
    const auto n = pos.size();
    for (std::size_t i = 0; i < n; ++i) {
      for (std::size_t j = 0; j < n; ++j) {
        if ((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0)) continue;
        for (int dy = -2; dy <= 2; ++dy) {
          for (int dx = -2; dx <= 2; ++dx) set(pos[i] + dx, pos[j] + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
        }
      }
    }
    format_bits(0, 0);  // reserve
    if (ver >= 7) {
      int rem = ver;
      for (int i = 0; i < 12; ++i) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
      const long bits = static_cast<long>(ver) << 12 | rem;
      for (int i = 0; i < 18; ++i) {
        const bool b = ((bits >> i) & 1) != 0;
        const int a = size - 11 + i % 3, c = i / 3;
        set(a, c, b);
        set(c, a, b);
      }
    }
  }

  void place(const std::vector<std::uint8_t>& data) {
    std::size_t i = 0;
    for (int right = size - 1; right >= 1; right -= 2) {
      if (right == 6) right = 5;
      for (int vert = 0; vert < size; ++vert) {
        for (int j = 0; j < 2; ++j) {
          const int x = right - j;
          const bool upward = ((right + 1) & 2) == 0;
          const int y = upward ? size - 1 - vert : vert;
          if (!is_function(x, y) && i < data.size() * 8) {
            modules[static_cast<std::size_t>(y * size + x)] = ((data[i >> 3] >> (7 - (i & 7))) & 1) != 0 ? 1 : 0;
            ++i;
          }
        }
      }
    }
  }

  void apply_mask(int mask) {
    for (int y = 0; y < size; ++y) {
      for (int x = 0; x < size; ++x) {
        bool invert = false;
        switch (mask) {
          case 0: invert = (x + y) % 2 == 0; break;
          case 1: invert = y % 2 == 0; break;
          case 2: invert = x % 3 == 0; break;
          case 3: invert = (x + y) % 3 == 0; break;
          case 4: invert = (x / 3 + y / 2) % 2 == 0; break;
          case 5: invert = x * y % 2 + x * y % 3 == 0; break;
          case 6: invert = (x * y % 2 + x * y % 3) % 2 == 0; break;
          default: invert = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
        }
        if (invert && !is_function(x, y)) modules[static_cast<std::size_t>(y * size + x)] ^= 1;
      }
    }
  }

  [[nodiscard]] long penalty() const {
    long result = 0;
    // Rule 1 and the finder-like patterns of rule 3, rows then columns.
    for (int pass = 0; pass < 2; ++pass) {
      for (int a = 0; a < size; ++a) {
        const auto at = [&](int b) { return pass == 0 ? get(b, a) : get(a, b); };
        int run = 1;
        for (int b = 1; b <= size; ++b) {
          if (b < size && at(b) == at(b - 1)) {
            ++run;
            continue;
          }
          if (run >= 5) result += 3 + (run - 5);
          run = 1;
        }
        for (int b = -4; b < size; ++b) {
          // 1011101 with four light modules on one side (outside counts as light).
          const auto v = [&](int k) { return k >= 0 && k < size && at(k); };
          if (v(b + 4) && !v(b + 5) && v(b + 6) && v(b + 7) && v(b + 8) && !v(b + 9) && v(b + 10)) {
            const bool before = !v(b) && !v(b + 1) && !v(b + 2) && !v(b + 3);
            const bool after = !v(b + 11) && !v(b + 12) && !v(b + 13) && !v(b + 14);
            if (before) result += 40;
            if (after) result += 40;
          }
        }
      }
    }
    for (int y = 0; y + 1 < size; ++y) {
      for (int x = 0; x + 1 < size; ++x) {
        const bool c = get(x, y);
        if (c == get(x + 1, y) && c == get(x, y + 1) && c == get(x + 1, y + 1)) result += 3;
      }
    }
    long dark = 0;
    for (const auto m : modules) dark += m;
    const long total = static_cast<long>(size) * size;
    const long k = (std::abs(dark * 20 - total * 10) + total - 1) / total - 1;
    result += k * 10;
    return result;
  }
};

}  // namespace

Result<Matrix> qr_encode(std::string_view text, const QrOptions& options, int* version_out, int* mask_out) {
  Mode mode = Mode::Byte;
  if (!text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) {
    mode = Mode::Numeric;
  } else if (!text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return alnum_chars.find(c) != std::string_view::npos; })) {
    mode = Mode::Alnum;
  }
  int ecc = static_cast<int>(options.ecc);
  // QrEcc order L, M, Q, H; table rows are in the same order.
  const auto segment_bits = [&](int ver) -> long {
    const long n = static_cast<long>(text.size());
    long bits = 4 + count_bits(mode, ver);
    if (mode == Mode::Numeric) bits += n / 3 * 10 + (n % 3 == 0 ? 0 : n % 3 == 1 ? 4 : 7);
    else if (mode == Mode::Alnum) bits += n / 2 * 11 + (n % 2) * 6;
    else bits += n * 8;
    if (static_cast<long>(text.size()) >= (1L << count_bits(mode, ver))) return -1;
    return bits;
  };
  int ver = std::clamp(options.min_version, 1, 40);
  const int max_ver = std::clamp(options.max_version, ver, 40);
  for (;; ++ver) {
    const long bits = segment_bits(ver);
    if (bits >= 0 && bits <= data_codewords(ver, ecc) * 8L) break;
    if (ver >= max_ver) return fail(Status::Encode, "the text is too long for a QR code at this error-correction level ({} bytes)", text.size());
  }
  const long used = segment_bits(ver);
  if (options.boost_ecc) {
    for (int e = ecc + 1; e <= 3; ++e) {
      // Order of strength: L(0) < M(1) < Q(2) < H(3).
      if (used <= data_codewords(ver, e) * 8L) ecc = e;
    }
  }
  BitBuffer bb;
  bb.put(mode == Mode::Numeric ? 1 : mode == Mode::Alnum ? 2 : 4, 4);
  bb.put(static_cast<std::uint32_t>(text.size()), count_bits(mode, ver));
  if (mode == Mode::Numeric) {
    for (std::size_t i = 0; i < text.size(); i += 3) {
      const std::size_t n = std::min<std::size_t>(3, text.size() - i);
      std::uint32_t v = 0;
      for (std::size_t k = 0; k < n; ++k) v = v * 10 + static_cast<std::uint32_t>(text[i + k] - '0');
      bb.put(v, n == 3 ? 10 : n == 2 ? 7 : 4);
    }
  } else if (mode == Mode::Alnum) {
    for (std::size_t i = 0; i < text.size(); i += 2) {
      if (i + 1 < text.size()) {
        bb.put(static_cast<std::uint32_t>(alnum_chars.find(text[i]) * 45 + alnum_chars.find(text[i + 1])), 11);
      } else {
        bb.put(static_cast<std::uint32_t>(alnum_chars.find(text[i])), 6);
      }
    }
  } else {
    for (const char c : text) bb.put(static_cast<std::uint8_t>(c), 8);
  }
  const std::size_t capacity = static_cast<std::size_t>(data_codewords(ver, ecc)) * 8;
  bb.put(0, static_cast<int>(std::min<std::size_t>(4, capacity - bb.bits.size())));
  bb.put(0, static_cast<int>((8 - bb.bits.size() % 8) % 8));
  for (std::uint8_t pad = 0xEC; bb.bits.size() < capacity; pad ^= 0xEC ^ 0x11) bb.put(pad, 8);
  std::vector<std::uint8_t> data(bb.bits.size() / 8, 0);
  for (std::size_t i = 0; i < bb.bits.size(); ++i) {
    if (bb.bits[i]) data[i >> 3] |= static_cast<std::uint8_t>(1 << (7 - (i & 7)));
  }
  // Split into blocks, add error correction, interleave.
  const int blocks = num_blocks[ecc][ver];
  const int ecc_len = ecc_per_block[ecc][ver];
  const int raw = raw_modules(ver) / 8;
  const int short_blocks = blocks - raw % blocks;
  const int short_len = raw / blocks;
  const Galois gf(0x11D);
  const auto gen = generator(gf, ecc_len, 0);
  std::vector<std::vector<std::uint8_t>> all;
  std::size_t k = 0;
  for (int i = 0; i < blocks; ++i) {
    const std::size_t len = static_cast<std::size_t>(short_len - ecc_len + (i < short_blocks ? 0 : 1));
    std::vector<std::uint8_t> block(data.begin() + static_cast<std::ptrdiff_t>(k), data.begin() + static_cast<std::ptrdiff_t>(k + len));
    k += len;
    const auto ecc_bytes = rs_remainder(gf, block, gen);
    if (i < short_blocks) block.push_back(0);
    block.insert(block.end(), ecc_bytes.begin(), ecc_bytes.end());
    all.push_back(std::move(block));
  }
  std::vector<std::uint8_t> codewords;
  for (std::size_t i = 0; i < all[0].size(); ++i) {
    for (std::size_t j = 0; j < all.size(); ++j) {
      if (i != static_cast<std::size_t>(short_len - ecc_len) || j >= static_cast<std::size_t>(short_blocks)) codewords.push_back(all[j][i]);
    }
  }
  QrBuilder qr(ver);
  qr.function_patterns();
  qr.place(codewords);
  static constexpr int format_ecc[4] = {1, 0, 3, 2};
  int mask = options.mask;
  if (mask < 0 || mask > 7) {
    long best = -1;
    for (int m = 0; m < 8; ++m) {
      QrBuilder trial = qr;
      trial.apply_mask(m);
      trial.format_bits(format_ecc[ecc], m);
      const long p = trial.penalty();
      if (best < 0 || p < best) {
        best = p;
        mask = m;
      }
    }
  }
  qr.apply_mask(mask);
  qr.format_bits(format_ecc[ecc], mask);
  if (version_out != nullptr) *version_out = ver;
  if (mask_out != nullptr) *mask_out = mask;
  Matrix out;
  out.width = out.height = qr.size;
  out.modules = std::move(qr.modules);
  return out;
}

// --------------------------------------------------------------------------- Data Matrix

namespace {

struct DmSize {
  int rows, cols, region_rows, region_cols, data, ecc, blocks;
};

constexpr DmSize dm_sizes[] = {
    {10, 10, 8, 8, 3, 5, 1},       {12, 12, 10, 10, 5, 7, 1},     {14, 14, 12, 12, 8, 10, 1},    {16, 16, 14, 14, 12, 12, 1},
    {18, 18, 16, 16, 18, 14, 1},   {20, 20, 18, 18, 22, 18, 1},   {22, 22, 20, 20, 30, 20, 1},   {24, 24, 22, 22, 36, 24, 1},
    {26, 26, 24, 24, 44, 28, 1},   {32, 32, 14, 14, 62, 36, 1},   {36, 36, 16, 16, 86, 42, 1},   {40, 40, 18, 18, 114, 48, 1},
    {44, 44, 20, 20, 144, 56, 1},  {48, 48, 22, 22, 174, 68, 1},  {52, 52, 24, 24, 204, 84, 2},  {64, 64, 14, 14, 280, 112, 2},
    {72, 72, 16, 16, 368, 144, 4}, {80, 80, 18, 18, 456, 192, 4}, {88, 88, 20, 20, 576, 224, 4}, {96, 96, 22, 22, 696, 272, 4},
    {104, 104, 24, 24, 816, 336, 6}, {120, 120, 18, 18, 1050, 408, 6}, {132, 132, 20, 20, 1304, 496, 8}, {144, 144, 22, 22, 1558, 620, 10},
    // rectangular
    {8, 18, 6, 16, 5, 7, 1},       {8, 32, 6, 14, 10, 11, 1},     {12, 26, 10, 24, 16, 14, 1},   {12, 36, 10, 16, 22, 18, 1},
    {16, 36, 14, 16, 32, 24, 1},   {16, 48, 14, 22, 49, 28, 1},
};

class DmPlacement {
 public:
  DmPlacement(int rows, int cols, const std::vector<std::uint8_t>& codewords)
      : rows_(rows), cols_(cols), bits_(static_cast<std::size_t>(rows * cols), -1), cw_(codewords) {}

  void run() {
    int pos = 0, row = 4, col = 0;
    do {
      if (row == rows_ && col == 0) corner1(pos++);
      if (row == rows_ - 2 && col == 0 && cols_ % 4 != 0) corner2(pos++);
      if (row == rows_ - 2 && col == 0 && cols_ % 8 == 4) corner3(pos++);
      if (row == rows_ + 4 && col == 2 && cols_ % 8 == 0) corner4(pos++);
      do {
        if (row < rows_ && col >= 0 && !has(col, row)) utah(row, col, pos++);
        row -= 2;
        col += 2;
      } while (row >= 0 && col < cols_);
      ++row;
      col += 3;
      do {
        if (row >= 0 && col < cols_ && !has(col, row)) utah(row, col, pos++);
        row += 2;
        col -= 2;
      } while (row < rows_ && col >= 0);
      row += 3;
      ++col;
    } while (row < rows_ || col < cols_);
    if (!has(cols_ - 1, rows_ - 1)) {
      set(cols_ - 1, rows_ - 1, true);
      set(cols_ - 2, rows_ - 2, true);
    }
  }
  [[nodiscard]] bool bit(int col, int row) const { return bits_[static_cast<std::size_t>(row * cols_ + col)] == 1; }

 private:
  bool has(int col, int row) const { return bits_[static_cast<std::size_t>(row * cols_ + col)] >= 0; }
  void set(int col, int row, bool v) { bits_[static_cast<std::size_t>(row * cols_ + col)] = v ? 1 : 0; }
  void module(int row, int col, int pos, int bit) {
    if (row < 0) {
      row += rows_;
      col += 4 - ((rows_ + 4) % 8);
    }
    if (col < 0) {
      col += cols_;
      row += 4 - ((cols_ + 4) % 8);
    }
    const std::uint8_t v = pos < static_cast<int>(cw_.size()) ? cw_[static_cast<std::size_t>(pos)] : 0;
    set(col, row, ((v >> (8 - bit)) & 1) != 0);
  }
  void utah(int row, int col, int pos) {
    module(row - 2, col - 2, pos, 1);
    module(row - 2, col - 1, pos, 2);
    module(row - 1, col - 2, pos, 3);
    module(row - 1, col - 1, pos, 4);
    module(row - 1, col, pos, 5);
    module(row, col - 2, pos, 6);
    module(row, col - 1, pos, 7);
    module(row, col, pos, 8);
  }
  void corner1(int pos) {
    module(rows_ - 1, 0, pos, 1);
    module(rows_ - 1, 1, pos, 2);
    module(rows_ - 1, 2, pos, 3);
    module(0, cols_ - 2, pos, 4);
    module(0, cols_ - 1, pos, 5);
    module(1, cols_ - 1, pos, 6);
    module(2, cols_ - 1, pos, 7);
    module(3, cols_ - 1, pos, 8);
  }
  void corner2(int pos) {
    module(rows_ - 3, 0, pos, 1);
    module(rows_ - 2, 0, pos, 2);
    module(rows_ - 1, 0, pos, 3);
    module(0, cols_ - 4, pos, 4);
    module(0, cols_ - 3, pos, 5);
    module(0, cols_ - 2, pos, 6);
    module(0, cols_ - 1, pos, 7);
    module(1, cols_ - 1, pos, 8);
  }
  void corner3(int pos) {
    module(rows_ - 3, 0, pos, 1);
    module(rows_ - 2, 0, pos, 2);
    module(rows_ - 1, 0, pos, 3);
    module(0, cols_ - 2, pos, 4);
    module(0, cols_ - 1, pos, 5);
    module(1, cols_ - 1, pos, 6);
    module(2, cols_ - 1, pos, 7);
    module(3, cols_ - 1, pos, 8);
  }
  void corner4(int pos) {
    module(rows_ - 1, 0, pos, 1);
    module(rows_ - 1, cols_ - 1, pos, 2);
    module(0, cols_ - 3, pos, 3);
    module(0, cols_ - 2, pos, 4);
    module(0, cols_ - 1, pos, 5);
    module(1, cols_ - 3, pos, 6);
    module(1, cols_ - 2, pos, 7);
    module(1, cols_ - 1, pos, 8);
  }
  int rows_, cols_;
  std::vector<int> bits_;
  const std::vector<std::uint8_t>& cw_;
};

}  // namespace

Result<Matrix> datamatrix_encode(std::string_view text, bool rectangular) {
  std::vector<std::uint8_t> cw;
  const bool non_ascii = std::any_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) > 127; });
  if (non_ascii) {
    cw.push_back(241);  // ECI
    cw.push_back(27);   // 26 (UTF-8) + 1
  }
  for (std::size_t i = 0; i < text.size(); ++i) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c >= '0' && c <= '9' && i + 1 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '9') {
      cw.push_back(static_cast<std::uint8_t>(130 + (c - '0') * 10 + (text[i + 1] - '0')));
      ++i;
    } else if (c > 127) {
      cw.push_back(235);
      cw.push_back(static_cast<std::uint8_t>(c - 127));
    } else {
      cw.push_back(static_cast<std::uint8_t>(c + 1));
    }
  }
  const DmSize* size = nullptr;
  for (const DmSize& s : dm_sizes) {
    const bool rect = s.rows != s.cols;
    if (rect != rectangular) continue;
    if (static_cast<int>(cw.size()) <= s.data) {
      size = &s;
      break;
    }
  }
  if (size == nullptr && rectangular) return datamatrix_encode(text, false);
  if (size == nullptr) return fail(Status::Encode, "the text is too long for a Data Matrix symbol ({} codewords)", cw.size());
  // Pad.
  if (static_cast<int>(cw.size()) < size->data) cw.push_back(129);
  while (static_cast<int>(cw.size()) < size->data) {
    const int pos = static_cast<int>(cw.size()) + 1;
    int v = 129 + ((149 * pos) % 253) + 1;
    if (v > 254) v -= 254;
    cw.push_back(static_cast<std::uint8_t>(v));
  }
  // Error correction, interleaved over the blocks.
  const Galois gf(0x12D);
  const int blocks = size->blocks;
  const int ecc_per = size->ecc / blocks;
  const auto gen = generator(gf, ecc_per, 1);
  std::vector<std::uint8_t> all(cw);
  all.resize(static_cast<std::size_t>(size->data + size->ecc), 0);
  for (int b = 0; b < blocks; ++b) {
    std::vector<std::uint8_t> block;
    for (int i = b; i < size->data; i += blocks) block.push_back(cw[static_cast<std::size_t>(i)]);
    const auto ecc = rs_remainder(gf, block, gen);
    for (int j = 0; j < ecc_per; ++j) all[static_cast<std::size_t>(size->data + j * blocks + b)] = ecc[static_cast<std::size_t>(j)];
  }
  const int regions_v = size->rows / (size->region_rows + 2);
  const int regions_h = size->cols / (size->region_cols + 2);
  const int map_rows = size->region_rows * regions_v;
  const int map_cols = size->region_cols * regions_h;
  DmPlacement placement(map_rows, map_cols, all);
  placement.run();
  Matrix m;
  m.width = size->cols;
  m.height = size->rows;
  m.modules.assign(static_cast<std::size_t>(m.width * m.height), 0);
  const int rr = size->region_rows + 2, rc = size->region_cols + 2;
  for (int y = 0; y < m.height; ++y) {
    for (int x = 0; x < m.width; ++x) {
      const int ry = y % rr, rx = x % rc;
      bool dark;
      if (ry == rr - 1) dark = true;             // bottom finder edge
      else if (rx == 0) dark = true;             // left finder edge
      else if (ry == 0) dark = x % 2 == 0;       // top clock track
      else if (rx == rc - 1) dark = y % 2 == 1;  // right clock track
      else dark = placement.bit((x / rc) * size->region_cols + rx - 1, (y / rr) * size->region_rows + ry - 1);
      m.set(x, y, dark);
    }
  }
  return m;
}

// --------------------------------------------------------------------------- Code 128

namespace {

constexpr std::array<std::string_view, 107> code128_patterns = {
    "212222", "222122", "222221", "121223", "121322", "131222", "122213", "122312", "132212", "221213", "221312", "231212",
    "112232", "122132", "122231", "113222", "123122", "123221", "223211", "221132", "221231", "213212", "223112", "312131",
    "311222", "321122", "321221", "312212", "322112", "322211", "212123", "212321", "232121", "111323", "131123", "131321",
    "112313", "132113", "132311", "211313", "231113", "231311", "112133", "112331", "132131", "113123", "113321", "133121",
    "313121", "211331", "231131", "213113", "213311", "213131", "311123", "311321", "331121", "312113", "312311", "332111",
    "314111", "221411", "431111", "111224", "111422", "121124", "121421", "141122", "141221", "112214", "112412", "122114",
    "122411", "142112", "142211", "241211", "221114", "413111", "241112", "134111", "111242", "121142", "121241", "114212",
    "124112", "124211", "411212", "421112", "421211", "212141", "214121", "412121", "111143", "111341", "131141", "114113",
    "114311", "411113", "411311", "113141", "114131", "311141", "411131", "211412", "211214", "211232", "2331112"};

void put_pattern(std::vector<bool>& out, std::string_view widths) {
  bool bar = true;
  for (const char w : widths) {
    for (int i = 0; i < w - '0'; ++i) out.push_back(bar);
    bar = !bar;
  }
}

std::size_t digit_run(std::string_view s, std::size_t from) {
  std::size_t n = 0;
  while (from + n < s.size() && s[from + n] >= '0' && s[from + n] <= '9') ++n;
  return n;
}

}  // namespace

Result<std::vector<bool>> code128_encode(std::string_view text) {
  if (text.empty()) return fail(Status::Encode, "Code 128 needs at least one character");
  for (const char c : text) {
    if (static_cast<unsigned char>(c) > 127) return fail(Status::Encode, "Code 128 encodes ASCII only");
  }
  std::vector<int> values;
  int set = 0;  // 'A', 'B', 'C'
  std::size_t i = 0;
  const auto start = [&](int s) {
    set = s;
    values.push_back(s == 'A' ? 103 : s == 'B' ? 104 : 105);
  };
  const auto needs_a = [&](std::size_t from) {
    for (std::size_t k = from; k < text.size(); ++k) {
      const auto c = static_cast<unsigned char>(text[k]);
      if (c < 32) return true;
      if (c >= 96) return false;
    }
    return false;
  };
  const std::size_t lead = digit_run(text, 0);
  if (lead >= 4 || (lead == text.size() && lead >= 2 && lead % 2 == 0)) start('C');
  else start(needs_a(0) ? 'A' : 'B');
  while (i < text.size()) {
    const std::size_t run = digit_run(text, i);
    if (set == 'C') {
      if (run >= 2) {
        values.push_back((text[i] - '0') * 10 + (text[i + 1] - '0'));
        i += 2;
        continue;
      }
      set = needs_a(i) ? 'A' : 'B';
      values.push_back(set == 'A' ? 101 : 100);
      continue;
    }
    // Switch to C for runs of at least 4 digits (6 in the middle), even length.
    if (run >= 4 && (run >= 6 || i + run == text.size())) {
      if (run % 2 == 1) {
        values.push_back(text[i] - ' ');
        ++i;
      }
      set = 'C';
      values.push_back(99);
      continue;
    }
    const auto c = static_cast<unsigned char>(text[i]);
    if (set == 'B' && c < 32) {
      set = 'A';
      values.push_back(101);
    } else if (set == 'A' && c >= 96) {
      set = 'B';
      values.push_back(100);
    }
    values.push_back(set == 'A' ? (c < 32 ? c + 64 : c - 32) : c - 32);
    ++i;
  }
  int checksum = values[0];
  for (std::size_t k = 1; k < values.size(); ++k) checksum += values[k] * static_cast<int>(k);
  values.push_back(checksum % 103);
  values.push_back(106);
  std::vector<bool> out;
  for (const int v : values) put_pattern(out, code128_patterns[static_cast<std::size_t>(v)]);
  return out;
}

// --------------------------------------------------------------------------- EAN-13 / UPC-A

namespace {

constexpr std::array<std::string_view, 10> ean_l = {"0001101", "0011001", "0010011", "0111101", "0100011",
                                                    "0110001", "0101111", "0111011", "0110111", "0001011"};
constexpr std::array<std::string_view, 10> ean_parity = {"LLLLLL", "LLGLGG", "LLGGLG", "LLGGGL", "LGLLGG",
                                                         "LGGLLG", "LGGGLL", "LGLGLG", "LGLGGL", "LGGLGL"};

int ean_check(std::string_view twelve) {
  int sum = 0;
  for (std::size_t i = 0; i < 12; ++i) sum += (twelve[i] - '0') * (i % 2 == 0 ? 1 : 3);
  return (10 - sum % 10) % 10;
}

}  // namespace

Result<std::vector<bool>> ean13_encode(std::string_view digits, std::string* full_out) {
  std::string d;
  for (const char c : digits) {
    if (c == ' ' || c == '-') continue;
    if (c < '0' || c > '9') return fail(Status::Encode, "EAN-13 takes digits only");
    d.push_back(c);
  }
  if (d.size() == 12) {
    d.push_back(static_cast<char>('0' + ean_check(d)));
  } else if (d.size() == 13) {
    if (ean_check(d) != d[12] - '0') return fail(Status::Encode, "EAN-13 check digit {} is wrong (expected {})", d[12], ean_check(d));
  } else {
    return fail(Status::Encode, "EAN-13 needs 12 digits (or 13 with the check digit)");
  }
  std::vector<bool> out;
  const auto put = [&out](std::string_view bits) {
    for (const char b : bits) out.push_back(b == '1');
  };
  put("101");
  const std::string_view parity = ean_parity[static_cast<std::size_t>(d[0] - '0')];
  for (std::size_t i = 1; i <= 6; ++i) {
    std::string bits(ean_l[static_cast<std::size_t>(d[i] - '0')]);
    if (parity[i - 1] == 'G') {
      for (char& b : bits) b = b == '1' ? '0' : '1';
      std::reverse(bits.begin(), bits.end());
    }
    put(bits);
  }
  put("01010");
  for (std::size_t i = 7; i <= 12; ++i) {
    std::string bits(ean_l[static_cast<std::size_t>(d[i] - '0')]);
    for (char& b : bits) b = b == '1' ? '0' : '1';
    put(bits);
  }
  put("101");
  if (full_out != nullptr) *full_out = d;
  return out;
}

Result<std::vector<bool>> upca_encode(std::string_view digits, std::string* full_out) {
  std::string d;
  for (const char c : digits) {
    if (c != ' ' && c != '-') d.push_back(c);
  }
  if (d.size() != 11 && d.size() != 12) return fail(Status::Encode, "UPC-A needs 11 digits (or 12 with the check digit)");
  std::string ean = "0" + d;
  auto out = ean13_encode(ean, full_out);
  if (out && full_out != nullptr) full_out->erase(0, 1);
  return out;
}

}  // namespace cvt::science::barcode
