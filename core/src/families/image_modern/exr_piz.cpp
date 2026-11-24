// Convertible core - image_modern family: OpenEXR PIZ compression.
//
// PIZ maps the 16-bit words of a chunk through a lookup table of the values
// actually used, applies a 2D Haar-like wavelet per channel and codes the
// result with a canonical Huffman code that has a run-length escape. The
// Huffman coder is shared with DWA (AC coefficients).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <queue>
#include <utility>
#include <vector>

#include "families/image_modern/exr_internal.hpp"

namespace cvt::image_modern::exr {

namespace {

// --------------------------------------------------------------------------- Huffman

constexpr int kEncSize = 65537;  // 16-bit symbols plus the run-length symbol
constexpr int kDecBits = 14;
constexpr std::uint32_t kDecSize = 1u << kDecBits;
constexpr std::uint32_t kDecMask = kDecSize - 1;
constexpr int kMaxCodeLength = 58;
constexpr int kShortZeroRun = 59;
constexpr int kLongZeroRun = 63;
constexpr int kShortestLongRun = 2 + kLongZeroRun - kShortZeroRun;
constexpr int kLongestLongRun = 255 + kShortestLongRun;

/// Code lengths -> (code << 6 | length), the canonical code of OpenEXR:
/// longer codes are numerically lower; equal lengths count up with the symbol.
void canonical_codes(std::vector<std::uint64_t>& codes) {
  std::uint64_t n[kMaxCodeLength + 1] = {};
  for (const std::uint64_t length : codes) n[length] += 1;
  std::uint64_t c = 0;
  for (int i = kMaxCodeLength; i > 0; --i) {
    const std::uint64_t next = (c + n[i]) >> 1;
    n[i] = c;
    c = next;
  }
  for (std::uint64_t& code : codes) {
    const std::uint64_t length = code;
    if (length > 0) code = length | (n[length]++ << 6);
  }
}

[[nodiscard]] constexpr int code_length(std::uint64_t code) noexcept { return static_cast<int>(code & 63u); }
[[nodiscard]] constexpr std::uint64_t code_bits(std::uint64_t code) noexcept { return code >> 6; }

class BitWriter {
 public:
  explicit BitWriter(Bytes& out) : out_(out) {}
  void put(int count, std::uint64_t bits) {
    while (count > 32) {
      count -= 32;
      put_small(32, bits >> count);
    }
    put_small(count, bits);
  }
  /// Flush the last partial byte (zero padded); returns the number of bits written.
  std::uint64_t finish() {
    const std::uint64_t total = bytes_ * 8 + static_cast<std::uint64_t>(lc_);
    if (lc_ > 0) {
      out_.push_back(static_cast<std::byte>((c_ << (8 - lc_)) & 0xFFu));
      lc_ = 0;
    }
    return total;
  }

 private:
  void put_small(int count, std::uint64_t bits) {
    const std::uint64_t mask = count == 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << count) - 1);
    c_ = (c_ << count) | (bits & mask);
    lc_ += count;
    while (lc_ >= 8) {
      lc_ -= 8;
      out_.push_back(static_cast<std::byte>((c_ >> lc_) & 0xFFu));
      ++bytes_;
    }
  }
  Bytes& out_;
  std::uint64_t c_ = 0;
  int lc_ = 0;
  std::uint64_t bytes_ = 0;
};

/// Huffman code lengths for the given frequencies (zero frequency -> length 0).
std::vector<std::uint64_t> code_lengths(const std::vector<std::uint64_t>& frequencies) {
  std::vector<std::uint64_t> lengths(frequencies.size(), 0);
  std::vector<std::uint32_t> leaf_symbol;
  for (std::size_t i = 0; i < frequencies.size(); ++i) {
    if (frequencies[i] != 0) leaf_symbol.push_back(static_cast<std::uint32_t>(i));
  }
  const std::size_t leaves = leaf_symbol.size();
  if (leaves == 1) {
    lengths[leaf_symbol[0]] = 1;
    return lengths;
  }
  std::vector<std::uint32_t> parent(2 * leaves, 0);
  using Item = std::pair<std::uint64_t, std::uint32_t>;
  std::priority_queue<Item, std::vector<Item>, std::greater<>> heap;
  for (std::uint32_t i = 0; i < leaves; ++i) heap.emplace(frequencies[leaf_symbol[i]], i);
  auto next = static_cast<std::uint32_t>(leaves);
  while (heap.size() > 1) {
    const Item a = heap.top();
    heap.pop();
    const Item b = heap.top();
    heap.pop();
    parent[a.second] = next;
    parent[b.second] = next;
    heap.emplace(a.first + b.first, next);
    ++next;
  }
  std::vector<std::uint32_t> depth(next, 0);
  for (std::uint32_t k = next - 1; k-- > 0;) depth[k] = depth[parent[k]] + 1;
  for (std::size_t i = 0; i < leaves; ++i) lengths[leaf_symbol[i]] = depth[i];
  return lengths;
}

}  // namespace

Bytes huf_compress(std::span<const std::uint16_t> values) {
  Bytes out;
  if (values.empty()) return out;
  std::vector<std::uint64_t> frequencies(kEncSize, 0);
  for (const std::uint16_t v : values) ++frequencies[v];
  std::uint32_t im = 0;
  while (frequencies[im] == 0) ++im;
  std::uint32_t iM = 0;
  for (std::uint32_t i = 0; i < kEncSize; ++i) {
    if (frequencies[i] != 0) iM = i;
  }
  ++iM;  // the run-length pseudo symbol
  frequencies[iM] = 1;
  std::vector<std::uint64_t> codes = code_lengths(frequencies);
  canonical_codes(codes);

  out.resize(20);
  BitWriter table(out);
  for (std::uint32_t i = im; i <= iM; ++i) {
    const int length = code_length(codes[i]);
    if (length == 0) {
      int run = 1;
      while (i < iM && run < kLongestLongRun && code_length(codes[i + 1]) == 0) {
        ++i;
        ++run;
      }
      if (run >= 2) {
        if (run >= kShortestLongRun) {
          table.put(6, kLongZeroRun);
          table.put(8, static_cast<std::uint64_t>(run - kShortestLongRun));
        } else {
          table.put(6, static_cast<std::uint64_t>(kShortZeroRun + run - 2));
        }
        continue;
      }
    }
    table.put(6, static_cast<std::uint64_t>(length));
  }
  (void)table.finish();
  const std::size_t table_length = out.size() - 20;

  BitWriter data(out);
  const std::uint64_t run_code = codes[iM];
  const auto send = [&](std::uint32_t symbol, int run) {
    const std::uint64_t code = codes[symbol];
    if (code_length(code) + code_length(run_code) + 8 < code_length(code) * run) {
      data.put(code_length(code), code_bits(code));
      data.put(code_length(run_code), code_bits(run_code));
      data.put(8, static_cast<std::uint64_t>(run));
    } else {
      for (int k = 0; k <= run; ++k) data.put(code_length(code), code_bits(code));
    }
  };
  std::uint32_t symbol = values[0];
  int run = 0;
  for (std::size_t i = 1; i < values.size(); ++i) {
    if (symbol == values[i] && run < 255) {
      ++run;
    } else {
      send(symbol, run);
      run = 0;
    }
    symbol = values[i];
  }
  send(symbol, run);
  const std::uint64_t bits = data.finish();

  store32(out.data(), im);
  store32(out.data() + 4, iM);
  store32(out.data() + 8, static_cast<std::uint32_t>(table_length));
  store32(out.data() + 12, static_cast<std::uint32_t>(bits));
  store32(out.data() + 16, 0);
  return out;
}

Result<void> huf_decompress(ByteView data, std::span<std::uint16_t> out) {
  if (out.empty()) return {};
  if (data.size() < 20) return fail(Status::Decode, "Huffman data is truncated");
  const std::uint32_t im = load32(data.data());
  const std::uint32_t iM = load32(data.data() + 4);
  const std::uint32_t n_bits = load32(data.data() + 12);
  if (im >= kEncSize || iM >= kEncSize || im > iM) return fail(Status::Decode, "invalid Huffman table size");

  // Code-length table.
  const auto* in = reinterpret_cast<const std::uint8_t*>(data.data());
  std::size_t pos = 20;
  std::uint64_t c = 0;
  int lc = 0;
  const auto get_bits = [&](int count, std::uint32_t& value) -> bool {
    while (lc < count) {
      if (pos >= data.size()) return false;
      c = (c << 8) | in[pos++];
      lc += 8;
    }
    lc -= count;
    value = static_cast<std::uint32_t>((c >> lc) & ((1u << count) - 1));
    return true;
  };
  std::vector<std::uint64_t> codes(kEncSize, 0);
  for (std::uint32_t i = im; i <= iM; ++i) {
    std::uint32_t length = 0;
    if (!get_bits(6, length)) return fail(Status::Decode, "Huffman table is truncated");
    if (length == kLongZeroRun) {
      std::uint32_t extra = 0;
      if (!get_bits(8, extra)) return fail(Status::Decode, "Huffman table is truncated");
      const std::uint32_t run = extra + kShortestLongRun;
      if (std::uint64_t{i} + run > std::uint64_t{iM} + 1) return fail(Status::Decode, "corrupt Huffman table");
      i += run - 1;
    } else if (length >= kShortZeroRun) {
      const std::uint32_t run = length - kShortZeroRun + 2;
      if (std::uint64_t{i} + run > std::uint64_t{iM} + 1) return fail(Status::Decode, "corrupt Huffman table");
      i += run - 1;
    } else {
      codes[i] = length;
    }
  }
  // 58-bit codes cannot occur in data of any realistic size and would not
  // fit the 64-bit bit buffer below.
  for (std::uint32_t i = im; i <= iM; ++i) {
    if (codes[i] > 57) return fail(Status::Decode, "unsupported Huffman code length");
  }
  canonical_codes(codes);

  const std::size_t data_bytes = (std::size_t{n_bits} + 7) / 8;
  if (data_bytes > data.size() - pos) return fail(Status::Decode, "Huffman data is truncated");
  const std::uint8_t* bits_in = in + pos;

  // Decoding table: codes up to 14 bits fill all matching primary entries;
  // longer codes are listed under their 14-bit prefix.
  struct Entry {
    std::uint32_t length = 0;
    std::uint32_t symbol = 0;
    std::uint32_t long_begin = 0;
    std::uint32_t long_count = 0;
  };
  std::vector<Entry> table(kDecSize);
  std::vector<std::uint32_t> long_symbols;
  for (std::uint32_t i = im; i <= iM; ++i) {
    const int length = code_length(codes[i]);
    if (length == 0) continue;
    const std::uint64_t code = code_bits(codes[i]);
    if ((code >> length) != 0) return fail(Status::Decode, "invalid Huffman code");
    if (length <= kDecBits) {
      const std::uint64_t first = code << (kDecBits - length);
      const std::uint64_t count = std::uint64_t{1} << (kDecBits - length);
      for (std::uint64_t k = 0; k < count; ++k) {
        Entry& entry = table[first + k];
        if (entry.length != 0 || entry.long_count != 0) return fail(Status::Decode, "invalid Huffman code");
        entry.length = static_cast<std::uint32_t>(length);
        entry.symbol = i;
      }
    }
  }
  for (std::uint32_t i = im; i <= iM; ++i) {
    const int length = code_length(codes[i]);
    if (length <= kDecBits) continue;
    Entry& entry = table[code_bits(codes[i]) >> (length - kDecBits)];
    if (entry.length != 0) return fail(Status::Decode, "invalid Huffman code");
    ++entry.long_count;
  }
  std::uint32_t offset = 0;
  for (Entry& entry : table) {
    entry.long_begin = offset;
    offset += entry.long_count;
    entry.long_count = 0;
  }
  long_symbols.resize(offset);
  for (std::uint32_t i = im; i <= iM; ++i) {
    const int length = code_length(codes[i]);
    if (length <= kDecBits) continue;
    Entry& entry = table[code_bits(codes[i]) >> (length - kDecBits)];
    long_symbols[entry.long_begin + entry.long_count++] = i;
  }

  // Bit stream.
  const std::uint32_t rlc = iM;
  std::size_t o = 0;
  std::size_t p = 0;
  c = 0;
  lc = 0;
  bool ok = true;
  const auto emit = [&](std::uint32_t symbol) {
    if (symbol == rlc) {
      if (lc < 8) {
        if (p >= data_bytes) return false;
        c = (c << 8) | bits_in[p++];
        lc += 8;
      }
      lc -= 8;
      const auto count = static_cast<std::size_t>((c >> lc) & 0xFFu);
      if (o == 0 || count > out.size() - o) return false;
      std::fill_n(out.begin() + static_cast<std::ptrdiff_t>(o), count, out[o - 1]);
      o += count;
      return true;
    }
    if (o >= out.size()) return false;
    out[o++] = static_cast<std::uint16_t>(symbol);
    return true;
  };
  while (ok && p < data_bytes) {
    c = (c << 8) | bits_in[p++];
    lc += 8;
    while (ok && lc >= kDecBits) {
      const Entry& entry = table[(c >> (lc - kDecBits)) & kDecMask];
      if (entry.length != 0) {
        lc -= static_cast<int>(entry.length);
        ok = emit(entry.symbol);
        continue;
      }
      bool found = false;
      for (std::uint32_t k = 0; k < entry.long_count && !found; ++k) {
        const std::uint32_t symbol = long_symbols[entry.long_begin + k];
        const int length = code_length(codes[symbol]);
        while (lc < length && p < data_bytes) {
          c = (c << 8) | bits_in[p++];
          lc += 8;
        }
        if (lc >= length &&
            code_bits(codes[symbol]) == ((c >> (lc - length)) & ((std::uint64_t{1} << length) - 1))) {
          lc -= length;
          ok = emit(symbol);
          found = true;
        }
      }
      if (!found) ok = false;
    }
  }
  if (!ok) return fail(Status::Decode, "corrupt Huffman data");
  const int padding = static_cast<int>((8 - n_bits) & 7u);
  c >>= padding;
  lc -= padding;
  while (lc > 0) {
    const Entry& entry = table[(c << (kDecBits - lc)) & kDecMask];
    if (entry.length == 0 || static_cast<int>(entry.length) > lc) return fail(Status::Decode, "corrupt Huffman data");
    lc -= static_cast<int>(entry.length);
    if (!emit(entry.symbol)) return fail(Status::Decode, "corrupt Huffman data");
  }
  if (o != out.size()) return fail(Status::Decode, "Huffman data is truncated");
  return {};
}

// --------------------------------------------------------------------------- wavelet

namespace {

constexpr int kAOffset = 1 << 15;
constexpr int kModMask = (1 << 16) - 1;

inline void wenc14(std::uint16_t a, std::uint16_t b, std::uint16_t& l, std::uint16_t& h) noexcept {
  const auto as = static_cast<std::int16_t>(a);
  const auto bs = static_cast<std::int16_t>(b);
  const int ms = (as + bs) >> 1;
  const int ds = as - bs;
  l = static_cast<std::uint16_t>(ms);
  h = static_cast<std::uint16_t>(ds);
}

inline void wdec14(std::uint16_t l, std::uint16_t h, std::uint16_t& a, std::uint16_t& b) noexcept {
  const auto ls = static_cast<std::int16_t>(l);
  const auto hs = static_cast<std::int16_t>(h);
  const int hi = hs;
  const int ai = ls + (hi & 1) + (hi >> 1);
  a = static_cast<std::uint16_t>(ai);
  b = static_cast<std::uint16_t>(static_cast<std::int16_t>(ai) - hi);
}

inline void wenc16(std::uint16_t a, std::uint16_t b, std::uint16_t& l, std::uint16_t& h) noexcept {
  const int ao = (a + kAOffset) & kModMask;
  int m = (ao + b) >> 1;
  int d = ao - b;
  if (d < 0) m = (m + kAOffset) & kModMask;
  d &= kModMask;
  l = static_cast<std::uint16_t>(m);
  h = static_cast<std::uint16_t>(d);
}

inline void wdec16(std::uint16_t l, std::uint16_t h, std::uint16_t& a, std::uint16_t& b) noexcept {
  const int m = l;
  const int d = h;
  const int bb = (m - (d >> 1)) & kModMask;
  const int aa = (d + bb - kAOffset) & kModMask;
  b = static_cast<std::uint16_t>(bb);
  a = static_cast<std::uint16_t>(aa);
}

using Pair = void (*)(std::uint16_t, std::uint16_t, std::uint16_t&, std::uint16_t&) noexcept;

void wav2_encode(std::uint16_t* buf, std::ptrdiff_t nx, std::ptrdiff_t ox, std::ptrdiff_t ny, std::ptrdiff_t oy,
                 std::uint16_t mx) {
  const Pair enc = mx < (1 << 14) ? wenc14 : wenc16;
  const std::ptrdiff_t n = std::min(nx, ny);
  std::ptrdiff_t p = 1;
  std::ptrdiff_t p2 = 2;
  std::uint16_t i00 = 0, i01 = 0, i10 = 0, i11 = 0;
  while (p2 <= n) {
    const std::ptrdiff_t ey = oy * (ny - p2);
    const std::ptrdiff_t oy1 = oy * p, oy2 = oy * p2, ox1 = ox * p, ox2 = ox * p2;
    std::ptrdiff_t py = 0;
    for (; py <= ey; py += oy2) {
      std::ptrdiff_t px = py;
      const std::ptrdiff_t ex = py + ox * (nx - p2);
      for (; px <= ex; px += ox2) {
        const std::ptrdiff_t p01 = px + ox1, p10 = px + oy1, p11 = p10 + ox1;
        enc(buf[px], buf[p01], i00, i01);
        enc(buf[p10], buf[p11], i10, i11);
        enc(i00, i10, buf[px], buf[p10]);
        enc(i01, i11, buf[p01], buf[p11]);
      }
      if ((nx & p) != 0) {
        const std::ptrdiff_t p10 = px + oy1;
        enc(buf[px], buf[p10], i00, buf[p10]);
        buf[px] = i00;
      }
    }
    if ((ny & p) != 0) {
      std::ptrdiff_t px = py;
      const std::ptrdiff_t ex = py + ox * (nx - p2);
      for (; px <= ex; px += ox2) {
        const std::ptrdiff_t p01 = px + ox1;
        enc(buf[px], buf[p01], i00, buf[p01]);
        buf[px] = i00;
      }
    }
    p = p2;
    p2 <<= 1;
  }
}

void wav2_decode(std::uint16_t* buf, std::ptrdiff_t nx, std::ptrdiff_t ox, std::ptrdiff_t ny, std::ptrdiff_t oy,
                 std::uint16_t mx) {
  const Pair dec = mx < (1 << 14) ? wdec14 : wdec16;
  const std::ptrdiff_t n = std::min(nx, ny);
  std::ptrdiff_t p = 1;
  while (p <= n) p <<= 1;
  p >>= 1;
  std::ptrdiff_t p2 = p;
  p >>= 1;
  std::uint16_t i00 = 0, i01 = 0, i10 = 0, i11 = 0;
  while (p >= 1) {
    const std::ptrdiff_t ey = oy * (ny - p2);
    const std::ptrdiff_t oy1 = oy * p, oy2 = oy * p2, ox1 = ox * p, ox2 = ox * p2;
    std::ptrdiff_t py = 0;
    for (; py <= ey; py += oy2) {
      std::ptrdiff_t px = py;
      const std::ptrdiff_t ex = py + ox * (nx - p2);
      for (; px <= ex; px += ox2) {
        const std::ptrdiff_t p01 = px + ox1, p10 = px + oy1, p11 = p10 + ox1;
        dec(buf[px], buf[p10], i00, i10);
        dec(buf[p01], buf[p11], i01, i11);
        dec(i00, i01, buf[px], buf[p01]);
        dec(i10, i11, buf[p10], buf[p11]);
      }
      if ((nx & p) != 0) {
        const std::ptrdiff_t p10 = px + oy1;
        dec(buf[px], buf[p10], i00, buf[p10]);
        buf[px] = i00;
      }
    }
    if ((ny & p) != 0) {
      std::ptrdiff_t px = py;
      const std::ptrdiff_t ex = py + ox * (nx - p2);
      for (; px <= ex; px += ox2) {
        const std::ptrdiff_t p01 = px + ox1;
        dec(buf[px], buf[p01], i00, buf[p01]);
        buf[px] = i00;
      }
    }
    p2 = p;
    p >>= 1;
  }
}

// --------------------------------------------------------------------------- PIZ

constexpr std::size_t kBitmapSize = 8192;

struct Plane {
  std::size_t start = 0;  // in words
  std::ptrdiff_t nx = 0;
  std::ptrdiff_t ny = 0;
  std::ptrdiff_t size = 1;  // 16-bit words per sample
};

std::vector<Plane> planes_of(const ChunkLayout& layout, std::size_t& total_words) {
  std::vector<Plane> planes;
  total_words = 0;
  for (const ChunkChannel& channel : layout.channels) {
    Plane plane;
    plane.start = total_words;
    plane.nx = channel.width;
    plane.ny = channel.height;
    plane.size = static_cast<std::ptrdiff_t>(pixel_size(channel.type) / 2);
    total_words += static_cast<std::size_t>(plane.nx * plane.ny * plane.size);
    planes.push_back(plane);
  }
  return planes;
}

}  // namespace

Result<Bytes> piz_compress(const ChunkLayout& layout, ByteView raw) {
  std::size_t total = 0;
  const std::vector<Plane> planes = planes_of(layout, total);
  if (total * 2 != raw.size()) return fail(Status::Internal, "PIZ chunk size mismatch");
  std::vector<std::uint16_t> words(total);
  {
    std::vector<std::size_t> cursor(planes.size());
    for (std::size_t c = 0; c < planes.size(); ++c) cursor[c] = planes[c].start;
    std::size_t in = 0;
    for (std::int32_t line = 0; line < layout.height; ++line) {
      for (std::size_t c = 0; c < planes.size(); ++c) {
        if (!ChunkLayout::has_line(layout.channels[c], std::int64_t{layout.y0} + line)) continue;
        const auto n = static_cast<std::size_t>(planes[c].nx * planes[c].size);
        for (std::size_t k = 0; k < n; ++k, in += 2) words[cursor[c]++] = load16(raw.data() + in);
      }
    }
  }
  std::vector<std::uint8_t> bitmap(kBitmapSize, 0);
  for (const std::uint16_t v : words) bitmap[v >> 3] = static_cast<std::uint8_t>(bitmap[v >> 3] | (1u << (v & 7)));
  bitmap[0] = static_cast<std::uint8_t>(bitmap[0] & ~1u);
  auto min_non_zero = static_cast<std::uint16_t>(kBitmapSize - 1);
  std::uint16_t max_non_zero = 0;
  for (std::size_t i = 0; i < kBitmapSize; ++i) {
    if (bitmap[i] != 0) {
      min_non_zero = std::min(min_non_zero, static_cast<std::uint16_t>(i));
      max_non_zero = std::max(max_non_zero, static_cast<std::uint16_t>(i));
    }
  }
  std::vector<std::uint16_t> lut(65536, 0);
  std::uint32_t k = 0;
  for (std::uint32_t i = 0; i < 65536; ++i) {
    if (i == 0 || (bitmap[i >> 3] & (1u << (i & 7))) != 0) lut[i] = static_cast<std::uint16_t>(k++);
  }
  const auto max_value = static_cast<std::uint16_t>(k - 1);
  for (std::uint16_t& v : words) v = lut[v];

  Bytes out;
  out.resize(4);
  store16(out.data(), min_non_zero);
  store16(out.data() + 2, max_non_zero);
  if (min_non_zero <= max_non_zero) {
    for (std::size_t i = min_non_zero; i <= max_non_zero; ++i) out.push_back(static_cast<std::byte>(bitmap[i]));
  }
  for (const Plane& plane : planes) {
    for (std::ptrdiff_t j = 0; j < plane.size; ++j) {
      wav2_encode(words.data() + plane.start + static_cast<std::size_t>(j), plane.nx, plane.size, plane.ny,
                  plane.nx * plane.size, max_value);
    }
  }
  const Bytes coded = huf_compress(words);
  const std::size_t length_at = out.size();
  out.resize(out.size() + 4);
  store32(out.data() + length_at, static_cast<std::uint32_t>(coded.size()));
  out.insert(out.end(), coded.begin(), coded.end());
  return out;
}

Result<Bytes> piz_decompress(const ChunkLayout& layout, ByteView packed) {
  std::size_t total = 0;
  const std::vector<Plane> planes = planes_of(layout, total);
  if (total * 2 != layout.raw_size) return fail(Status::Internal, "PIZ chunk size mismatch");
  if (packed.size() < 4) return fail(Status::Decode, "PIZ data is truncated");
  const std::uint16_t min_non_zero = load16(packed.data());
  const std::uint16_t max_non_zero = load16(packed.data() + 2);
  if (max_non_zero >= kBitmapSize) return fail(Status::Decode, "invalid PIZ bitmap");
  std::size_t pos = 4;
  std::vector<std::uint8_t> bitmap(kBitmapSize, 0);
  if (min_non_zero <= max_non_zero) {
    const std::size_t count = std::size_t{max_non_zero} - min_non_zero + 1;
    if (count > packed.size() - pos) return fail(Status::Decode, "PIZ data is truncated");
    for (std::size_t i = 0; i < count; ++i) bitmap[min_non_zero + i] = std::to_integer<std::uint8_t>(packed[pos + i]);
    pos += count;
  }
  std::vector<std::uint16_t> lut(65536, 0);
  std::uint32_t k = 0;
  for (std::uint32_t i = 0; i < 65536; ++i) {
    if (i == 0 || (bitmap[i >> 3] & (1u << (i & 7))) != 0) lut[k++] = static_cast<std::uint16_t>(i);
  }
  const auto max_value = static_cast<std::uint16_t>(k - 1);
  if (packed.size() - pos < 4) return fail(Status::Decode, "PIZ data is truncated");
  const std::uint32_t length = load32(packed.data() + pos);
  pos += 4;
  if (length > packed.size() - pos) return fail(Status::Decode, "PIZ data is truncated");
  std::vector<std::uint16_t> words(total);
  CVT_TRY(huf_decompress(packed.subspan(pos, length), words));
  for (const Plane& plane : planes) {
    for (std::ptrdiff_t j = 0; j < plane.size; ++j) {
      wav2_decode(words.data() + plane.start + static_cast<std::size_t>(j), plane.nx, plane.size, plane.ny,
                  plane.nx * plane.size, max_value);
    }
  }
  for (std::uint16_t& v : words) v = lut[v];
  Bytes out(static_cast<std::size_t>(layout.raw_size));
  std::vector<std::size_t> cursor(planes.size());
  for (std::size_t c = 0; c < planes.size(); ++c) cursor[c] = planes[c].start;
  std::size_t write = 0;
  for (std::int32_t line = 0; line < layout.height; ++line) {
    for (std::size_t c = 0; c < planes.size(); ++c) {
      if (!ChunkLayout::has_line(layout.channels[c], std::int64_t{layout.y0} + line)) continue;
      const auto n = static_cast<std::size_t>(planes[c].nx * planes[c].size);
      for (std::size_t i = 0; i < n; ++i, write += 2) store16(out.data() + write, words[cursor[c]++]);
    }
  }
  return out;
}

}  // namespace cvt::image_modern::exr
