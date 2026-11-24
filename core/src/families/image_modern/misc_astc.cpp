// Convertible core - image_modern family: ASTC block decoder.
//
// Implements the Khronos ASTC specification (KHR_texture_compression_astc_ldr)
// for 2D blocks: block modes, integer sequence encoding (bits, trits,
// quints), 1-4 partitions with the partition hash, dual-plane weights,
// weight-grid infill, the LDR colour endpoint modes and void-extent blocks.
// HDR endpoint modes decode to the error colour.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/image_modern/misc_astc.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

#include "families/image_modern/misc_pixel.hpp"

namespace cvt::image_modern {

namespace {

struct Quant {
  int trits;
  int quints;
  int bits;
};
// Index = quantisation level (ranges 2, 3, 4, 5, 6, 8, 10, 12, 16, 20, 24,
// 32, 40, 48, 64, 80, 96, 128, 160, 192, 256).
constexpr std::array<Quant, 21> quant_table = {{
    {0, 0, 1}, {1, 0, 0}, {0, 0, 2}, {0, 1, 0}, {1, 0, 1}, {0, 0, 3}, {0, 1, 1},
    {1, 0, 2}, {0, 0, 4}, {0, 1, 2}, {1, 0, 3}, {0, 0, 5}, {0, 1, 3}, {1, 0, 4},
    {0, 0, 6}, {0, 1, 4}, {1, 0, 5}, {0, 0, 7}, {0, 1, 5}, {1, 0, 6}, {0, 0, 8},
}};

int ise_bits(int count, int quant) {
  const Quant& q = quant_table[static_cast<std::size_t>(quant)];
  int bits = count * q.bits;
  if (q.trits) bits += (8 * count + 4) / 5;
  if (q.quints) bits += (7 * count + 2) / 3;
  return bits;
}

/// 128 bits, read LSB first from an arbitrary position.
struct Bits128 {
  std::array<std::uint8_t, 16> b{};
  [[nodiscard]] int bit(int pos) const noexcept {
    if (pos < 0 || pos >= 128) return 0;
    return (b[static_cast<std::size_t>(pos >> 3)] >> (pos & 7)) & 1;
  }
  [[nodiscard]] int get(int pos, int count) const noexcept {
    int v = 0;
    for (int i = 0; i < count; ++i) v |= bit(pos + i) << i;
    return v;
  }
};

/// Decode `count` ISE values at `quant` from bits [start, end) of `src`.
void ise_decode(const Bits128& src, int start, int end, int count, int quant, int* out) {
  const Quant& q = quant_table[static_cast<std::size_t>(quant)];
  int pos = start;
  const auto read = [&](int n) {
    int v = 0;
    for (int i = 0; i < n; ++i, ++pos) v |= (pos < end ? src.bit(pos) : 0) << i;
    return v;
  };
  const int n = q.bits;
  if (q.trits) {
    for (int g = 0; g < count; g += 5) {
      int m[5];
      int t = 0;
      m[0] = read(n);
      t |= read(2);
      m[1] = read(n);
      t |= read(2) << 2;
      m[2] = read(n);
      t |= read(1) << 4;
      m[3] = read(n);
      t |= read(2) << 5;
      m[4] = read(n);
      t |= read(1) << 7;
      int tr[5];
      int c = 0;
      if (((t >> 2) & 7) == 7) {
        c = ((t >> 5) & 7) << 2 | (t & 3);
        tr[4] = 2;
        tr[3] = 2;
      } else {
        c = t & 0x1F;
        if (((t >> 5) & 3) == 3) {
          tr[4] = 2;
          tr[3] = (t >> 7) & 1;
        } else {
          tr[4] = (t >> 7) & 1;
          tr[3] = (t >> 5) & 3;
        }
      }
      if ((c & 3) == 3) {
        tr[2] = 2;
        tr[1] = (c >> 4) & 1;
        tr[0] = ((c >> 3) & 1) << 1 | (((c >> 2) & 1) & ~((c >> 3) & 1));
      } else if (((c >> 2) & 3) == 3) {
        tr[2] = 2;
        tr[1] = 2;
        tr[0] = c & 3;
      } else {
        tr[2] = (c >> 4) & 1;
        tr[1] = (c >> 2) & 3;
        tr[0] = ((c >> 1) & 1) << 1 | ((c & 1) & ~((c >> 1) & 1));
      }
      for (int i = 0; i < 5 && g + i < count; ++i) out[g + i] = tr[i] << n | m[i];
    }
  } else if (q.quints) {
    for (int g = 0; g < count; g += 3) {
      int m[3];
      int v = 0;
      m[0] = read(n);
      v |= read(3);
      m[1] = read(n);
      v |= read(2) << 3;
      m[2] = read(n);
      v |= read(2) << 5;
      int qu[3];
      if (((v >> 1) & 3) == 3 && ((v >> 5) & 3) == 0) {
        qu[2] = (v & 1) << 2 | (((v >> 4) & 1) & ~(v & 1)) << 1 | (((v >> 3) & 1) & ~(v & 1));
        qu[1] = 4;
        qu[0] = 4;
      } else {
        int c = 0;
        if (((v >> 1) & 3) == 3) {
          qu[2] = 4;
          c = ((v >> 3) & 3) << 3 | ((~v >> 5) & 3) << 1 | (v & 1);
        } else {
          qu[2] = (v >> 5) & 3;
          c = v & 0x1F;
        }
        if ((c & 7) == 5) {
          qu[1] = 4;
          qu[0] = (c >> 3) & 3;
        } else {
          qu[1] = (c >> 3) & 3;
          qu[0] = c & 7;
        }
      }
      for (int i = 0; i < 3 && g + i < count; ++i) out[g + i] = qu[i] << n | m[i];
    }
  } else {
    for (int i = 0; i < count; ++i) out[i] = read(n);
  }
}

int replicate(int value, int from, int to) {
  if (from == 0) return 0;
  int r = 0;
  int filled = 0;
  while (filled < to) {
    r = r << from | value;
    filled += from;
  }
  return r >> (filled - to);
}

int unquant_colour(int quant, int value) {
  const Quant& q = quant_table[static_cast<std::size_t>(quant)];
  const int n = q.bits;
  if (!q.trits && !q.quints) return replicate(value, n, 8);
  const int m = value & ((1 << n) - 1);
  const int d = value >> n;
  const int a = (m & 1) ? 0x1FF : 0;
  const int b = (m >> 1) & 1;
  const int c = (m >> 2) & 1;
  const int dd = (m >> 3) & 1;
  const int e = (m >> 4) & 1;
  const int f = (m >> 5) & 1;
  int bb = 0;
  int cc = 0;
  if (q.trits) {
    switch (n) {
      case 1: cc = 204; break;
      case 2: bb = b << 8 | b << 4 | b << 2 | b << 1; cc = 93; break;
      case 3: bb = c << 8 | b << 7 | c << 3 | b << 2 | c << 1 | b; cc = 44; break;
      case 4: bb = dd << 8 | c << 7 | b << 6 | dd << 2 | c << 1 | b; cc = 22; break;
      case 5: bb = e << 8 | dd << 7 | c << 6 | b << 5 | e << 1 | dd; cc = 11; break;
      default: bb = f << 8 | e << 7 | dd << 6 | c << 5 | b << 4 | f; cc = 5; break;
    }
  } else {
    switch (n) {
      case 1: cc = 113; break;
      case 2: bb = b << 8 | b << 3 | b << 2; cc = 54; break;
      case 3: bb = c << 8 | b << 7 | c << 2 | b << 1 | c; cc = 26; break;
      case 4: bb = dd << 8 | c << 7 | b << 6 | dd << 1 | c; cc = 13; break;
      default: bb = e << 8 | dd << 7 | c << 6 | b << 5 | e; cc = 6; break;
    }
  }
  int t = d * cc + bb;
  t ^= a;
  return (a & 0x80) | (t >> 2);
}

int unquant_weight(int quant, int value) {
  const Quant& q = quant_table[static_cast<std::size_t>(quant)];
  const int n = q.bits;
  int v = 0;
  if (!q.trits && !q.quints) {
    v = replicate(value, n, 6);
  } else if (n == 0) {
    return q.trits ? value * 32 : value * 16;
  } else {
    const int m = value & ((1 << n) - 1);
    const int d = value >> n;
    const int a = (m & 1) ? 0x7F : 0;
    const int b = (m >> 1) & 1;
    const int c = (m >> 2) & 1;
    int bb = 0;
    int cc = 0;
    if (q.trits) {
      switch (n) {
        case 1: cc = 50; break;
        case 2: bb = b << 6 | b << 2 | b; cc = 23; break;
        default: bb = c << 6 | b << 5 | c << 1 | b; cc = 11; break;
      }
    } else {
      switch (n) {
        case 1: cc = 28; break;
        default: bb = b << 6 | b << 1; cc = 13; break;
      }
    }
    v = d * cc + bb;
    v ^= a;
    v = (a & 0x20) | (v >> 2);
  }
  if (v > 32) ++v;
  return v;
}

std::uint32_t hash52(std::uint32_t p) {
  p ^= p >> 15;
  p *= 0xEEDE0891u;
  p ^= p >> 5;
  p += p << 16;
  p ^= p >> 7;
  p ^= p >> 3;
  p ^= p << 6;
  p ^= p >> 17;
  return p;
}

int select_partition(int seed, int x, int y, int partitions, bool small) {
  if (small) {
    x <<= 1;
    y <<= 1;
  }
  seed += (partitions - 1) * 1024;
  const std::uint32_t r = hash52(static_cast<std::uint32_t>(seed));
  int s[12];
  s[0] = static_cast<int>(r & 0xF);
  s[1] = static_cast<int>((r >> 4) & 0xF);
  s[2] = static_cast<int>((r >> 8) & 0xF);
  s[3] = static_cast<int>((r >> 12) & 0xF);
  s[4] = static_cast<int>((r >> 16) & 0xF);
  s[5] = static_cast<int>((r >> 20) & 0xF);
  s[6] = static_cast<int>((r >> 24) & 0xF);
  s[7] = static_cast<int>((r >> 28) & 0xF);
  s[8] = static_cast<int>((r >> 18) & 0xF);
  s[9] = static_cast<int>((r >> 22) & 0xF);
  s[10] = static_cast<int>((r >> 26) & 0xF);
  s[11] = static_cast<int>(((r >> 30) | (r << 2)) & 0xF);
  for (int& v : s) v *= v;
  int sh1 = 0;
  int sh2 = 0;
  if (seed & 1) {
    sh1 = (seed & 2) ? 4 : 5;
    sh2 = partitions == 3 ? 6 : 5;
  } else {
    sh1 = partitions == 3 ? 6 : 5;
    sh2 = (seed & 2) ? 4 : 5;
  }
  const int sh3 = (seed & 0x10) ? sh1 : sh2;
  for (int i = 0; i < 8; ++i) s[i] >>= (i % 2 == 0) ? sh1 : sh2;
  for (int i = 8; i < 12; ++i) s[i] >>= sh3;
  int a = (s[0] * x + s[1] * y + static_cast<int>(r >> 14)) & 0x3F;
  int b = (s[2] * x + s[3] * y + static_cast<int>(r >> 10)) & 0x3F;
  int c = (s[4] * x + s[5] * y + static_cast<int>(r >> 6)) & 0x3F;
  int d = (s[6] * x + s[7] * y + static_cast<int>(r >> 2)) & 0x3F;
  if (partitions < 4) d = 0;
  if (partitions < 3) c = 0;
  if (a >= b && a >= c && a >= d) return 0;
  if (b >= c && b >= d) return 1;
  if (c >= d) return 2;
  return 3;
}

struct BlockMode {
  int grid_w = 0;
  int grid_h = 0;
  bool dual = false;
  int quant = 0;
  bool valid = false;
};

BlockMode decode_block_mode(int mode) {
  BlockMode m;
  int r = (mode >> 4) & 1;
  int high = (mode >> 9) & 1;
  int dual = (mode >> 10) & 1;
  const int a = (mode >> 5) & 3;
  int w = 0;
  int h = 0;
  if ((mode & 3) != 0) {
    r |= (mode & 3) << 1;
    int b = (mode >> 7) & 3;
    switch ((mode >> 2) & 3) {
      case 0: w = b + 4; h = a + 2; break;
      case 1: w = b + 8; h = a + 2; break;
      case 2: w = a + 2; h = b + 8; break;
      default:
        b &= 1;
        if (mode & 0x100) {
          w = b + 2;
          h = a + 2;
        } else {
          w = a + 2;
          h = b + 6;
        }
        break;
    }
  } else {
    r |= ((mode >> 2) & 3) << 1;
    if (((mode >> 2) & 3) == 0) return m;
    const int b = (mode >> 9) & 3;
    switch ((mode >> 7) & 3) {
      case 0: w = 12; h = a + 2; break;
      case 1: w = a + 2; h = 12; break;
      case 2:
        w = a + 6;
        h = b + 6;
        dual = 0;
        high = 0;
        break;
      default:
        switch ((mode >> 5) & 3) {
          case 0: w = 6; h = 10; break;
          case 1: w = 10; h = 6; break;
          default: return m;
        }
        break;
    }
  }
  const int weights = w * h * (dual + 1);
  const int quant = (r - 2) + 6 * high;
  if (quant < 0 || quant > 11 || weights > 64) return m;
  const int bits = ise_bits(weights, quant);
  if (bits < 24 || bits > 96) return m;
  m.grid_w = w;
  m.grid_h = h;
  m.dual = dual != 0;
  m.quant = quant;
  m.valid = true;
  return m;
}

void bit_transfer_signed(int& a, int& b) {
  b >>= 1;
  b |= a & 0x80;
  a >>= 1;
  a &= 0x3F;
  if (a & 0x20) a -= 0x40;
}

using Endpoint = std::array<int, 4>;

Endpoint blue_contract(int r, int g, int b, int a) { return {(r + b) >> 1, (g + b) >> 1, b, a}; }

Endpoint clamp_endpoint(Endpoint e) {
  for (int& v : e) v = std::clamp(v, 0, 255);
  return e;
}

/// Decode one LDR endpoint pair; false for HDR modes.
bool decode_endpoints(int cem, const int* v, Endpoint& e0, Endpoint& e1) {
  switch (cem) {
    case 0:
      e0 = {v[0], v[0], v[0], 255};
      e1 = {v[1], v[1], v[1], 255};
      return true;
    case 1: {
      const int l0 = (v[0] >> 2) | (v[1] & 0xC0);
      const int l1 = std::min(l0 + (v[1] & 0x3F), 255);
      e0 = {l0, l0, l0, 255};
      e1 = {l1, l1, l1, 255};
      return true;
    }
    case 4:
      e0 = {v[0], v[0], v[0], v[2]};
      e1 = {v[1], v[1], v[1], v[3]};
      return true;
    case 5: {
      int a0 = v[0];
      int b0 = v[1];
      int a2 = v[2];
      int b2 = v[3];
      bit_transfer_signed(b0, a0);
      bit_transfer_signed(b2, a2);
      e0 = {a0, a0, a0, a2};
      e1 = clamp_endpoint({a0 + b0, a0 + b0, a0 + b0, a2 + b2});
      return true;
    }
    case 6:
      e0 = {(v[0] * v[3]) >> 8, (v[1] * v[3]) >> 8, (v[2] * v[3]) >> 8, 255};
      e1 = {v[0], v[1], v[2], 255};
      return true;
    case 8:
    case 12: {
      const int a0 = cem == 12 ? v[6] : 255;
      const int a1 = cem == 12 ? v[7] : 255;
      if (v[1] + v[3] + v[5] >= v[0] + v[2] + v[4]) {
        e0 = {v[0], v[2], v[4], a0};
        e1 = {v[1], v[3], v[5], a1};
      } else {
        e0 = blue_contract(v[1], v[3], v[5], a1);
        e1 = blue_contract(v[0], v[2], v[4], a0);
      }
      return true;
    }
    case 9:
    case 13: {
      int x[8];
      for (int i = 0; i < 8; ++i) x[i] = v[i];
      bit_transfer_signed(x[1], x[0]);
      bit_transfer_signed(x[3], x[2]);
      bit_transfer_signed(x[5], x[4]);
      int a0 = 255;
      int a1 = 255;
      if (cem == 13) {
        bit_transfer_signed(x[7], x[6]);
        a0 = x[6];
        a1 = x[6] + x[7];
      }
      if (x[1] + x[3] + x[5] >= 0) {
        e0 = {x[0], x[2], x[4], a0};
        e1 = {x[0] + x[1], x[2] + x[3], x[4] + x[5], a1};
      } else {
        e0 = blue_contract(x[0] + x[1], x[2] + x[3], x[4] + x[5], a1);
        e1 = blue_contract(x[0], x[2], x[4], a0);
      }
      e0 = clamp_endpoint(e0);
      e1 = clamp_endpoint(e1);
      return true;
    }
    case 10:
      e0 = {(v[0] * v[3]) >> 8, (v[1] * v[3]) >> 8, (v[2] * v[3]) >> 8, v[4]};
      e1 = {v[0], v[1], v[2], v[5]};
      return true;
    default: return false;  // HDR modes 2, 3, 7, 11, 14, 15
  }
}

void fill_error(std::uint8_t* out, int texels) {
  for (int i = 0; i < texels; ++i) {
    out[i * 4] = 255;
    out[i * 4 + 1] = 0;
    out[i * 4 + 2] = 255;
    out[i * 4 + 3] = 255;
  }
}

}  // namespace

bool decode_astc_block(const std::uint8_t* block, int width, int height, std::uint8_t* out) noexcept {
  const int texels = width * height;
  Bits128 bits;
  for (std::size_t i = 0; i < 16; ++i) bits.b[i] = block[i];
  const int mode = bits.get(0, 11);

  // Void-extent block: one constant colour.
  if ((mode & 0x1FF) == 0x1FC) {
    const bool hdr = (mode >> 9) & 1;
    std::uint8_t colour[4];
    for (int c = 0; c < 4; ++c) {
      const int v = bits.get(64 + 16 * c, 16);
      if (hdr) {
        const float f = finite_or_zero(half_to_float(static_cast<std::uint16_t>(v)));
        colour[c] = static_cast<std::uint8_t>(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f);
      } else {
        colour[c] = static_cast<std::uint8_t>(v >> 8);
      }
    }
    for (int i = 0; i < texels; ++i) {
      for (int c = 0; c < 4; ++c) out[i * 4 + c] = colour[c];
    }
    return true;
  }

  const BlockMode bm = decode_block_mode(mode);
  if (!bm.valid || bm.grid_w > width || bm.grid_h > height) {
    fill_error(out, texels);
    return false;
  }
  const int partitions = bits.get(11, 2) + 1;
  if (partitions == 4 && bm.dual) {
    fill_error(out, texels);
    return false;
  }
  const int weight_count = bm.grid_w * bm.grid_h * (bm.dual ? 2 : 1);
  const int weight_bits = ise_bits(weight_count, bm.quant);
  int below_weights = 128 - weight_bits;

  std::array<int, 4> cem{};
  int colour_start = 17;
  int partition_index = 0;
  if (partitions == 1) {
    cem[0] = bits.get(13, 4);
  } else {
    partition_index = bits.get(13, 10);
    int encoded = bits.get(23, 6);
    colour_start = 29;
    if ((encoded & 3) == 0) {
      for (int p = 0; p < partitions; ++p) cem[static_cast<std::size_t>(p)] = (encoded >> 2) & 0xF;
    } else {
      const int extra = 3 * partitions - 4;
      below_weights -= extra;
      encoded |= bits.get(below_weights, extra) << 6;
      const int base_class = (encoded & 3) - 1;
      encoded >>= 2;
      int classes[4];
      for (int p = 0; p < partitions; ++p) {
        classes[p] = base_class + (encoded & 1);
        encoded >>= 1;
      }
      for (int p = 0; p < partitions; ++p) {
        cem[static_cast<std::size_t>(p)] = classes[p] << 2 | (encoded & 3);
        encoded >>= 2;
      }
    }
  }
  int ccs = -1;
  if (bm.dual) {
    below_weights -= 2;
    ccs = bits.get(below_weights, 2);
  }
  int value_count = 0;
  for (int p = 0; p < partitions; ++p) value_count += ((cem[static_cast<std::size_t>(p)] >> 2) + 1) * 2;
  const int colour_bits = below_weights - colour_start;
  if (value_count > 18 || colour_bits <= 0) {
    fill_error(out, texels);
    return false;
  }
  int colour_quant = -1;
  for (int q = 20; q >= 0; --q) {
    if (ise_bits(value_count, q) <= colour_bits) {
      colour_quant = q;
      break;
    }
  }
  if (colour_quant < 4) {
    fill_error(out, texels);
    return false;
  }
  int values[18];
  ise_decode(bits, colour_start, below_weights, value_count, colour_quant, values);
  std::array<Endpoint, 4> e0{};
  std::array<Endpoint, 4> e1{};
  for (int p = 0, k = 0; p < partitions; ++p) {
    int v[8] = {};
    const int n = ((cem[static_cast<std::size_t>(p)] >> 2) + 1) * 2;
    for (int i = 0; i < n; ++i) v[i] = unquant_colour(colour_quant, values[k + i]);
    k += n;
    if (!decode_endpoints(cem[static_cast<std::size_t>(p)], v, e0[static_cast<std::size_t>(p)],
                          e1[static_cast<std::size_t>(p)])) {
      fill_error(out, texels);
      return false;
    }
  }

  // Weights: stored bit-reversed from the top of the block.
  Bits128 reversed;
  for (int i = 0; i < 128; ++i) {
    if (bits.bit(127 - i)) reversed.b[static_cast<std::size_t>(i >> 3)] |= static_cast<std::uint8_t>(1 << (i & 7));
  }
  int raw[64];
  ise_decode(reversed, 0, weight_bits, weight_count, bm.quant, raw);
  int grid[2][64] = {};
  const int planes = bm.dual ? 2 : 1;
  for (int i = 0; i < bm.grid_w * bm.grid_h; ++i) {
    for (int p = 0; p < planes; ++p) grid[p][i] = unquant_weight(bm.quant, raw[i * planes + p]);
  }

  const int ds = (1024 + width / 2) / (width - 1);
  const int dt = (1024 + height / 2) / (height - 1);
  const bool small = texels < 31;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      int w[2] = {0, 0};
      const int gs = (ds * x * (bm.grid_w - 1) + 32) >> 6;
      const int gt = (dt * y * (bm.grid_h - 1) + 32) >> 6;
      const int js = gs >> 4;
      const int fs = gs & 0xF;
      const int jt = gt >> 4;
      const int ft = gt & 0xF;
      const int w11 = (fs * ft + 8) >> 4;
      const int w10 = ft - w11;
      const int w01 = fs - w11;
      const int w00 = 16 - fs - ft + w11;
      const int v0 = js + jt * bm.grid_w;
      const auto at = [&](int p, int index) {
        return index < bm.grid_w * bm.grid_h ? grid[p][index] : 0;
      };
      for (int p = 0; p < planes; ++p) {
        w[p] = (at(p, v0) * w00 + at(p, v0 + 1) * w01 + at(p, v0 + bm.grid_w) * w10 +
                at(p, v0 + bm.grid_w + 1) * w11 + 8) >>
               4;
      }
      const int part = partitions == 1 ? 0 : select_partition(partition_index, x, y, partitions, small);
      const Endpoint& a = e0[static_cast<std::size_t>(part)];
      const Endpoint& b = e1[static_cast<std::size_t>(part)];
      std::uint8_t* px = out + (y * width + x) * 4;
      for (int c = 0; c < 4; ++c) {
        const int weight = (bm.dual && c == ccs) ? w[1] : w[0];
        const int c0 = a[static_cast<std::size_t>(c)] * 257;
        const int c1 = b[static_cast<std::size_t>(c)] * 257;
        const int value = (c0 * (64 - weight) + c1 * weight + 32) >> 6;
        px[c] = static_cast<std::uint8_t>(value >> 8);
      }
    }
  }
  return true;
}

}  // namespace cvt::image_modern
