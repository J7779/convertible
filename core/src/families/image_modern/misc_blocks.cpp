// Convertible core - image_modern family: BCn, ETC and EAC block decoders.
// References: the Direct3D 10/11 block compression specifications (BC1-BC5,
// BC7) and the Khronos Data Format Specification (ETC1, ETC2, EAC).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/image_modern/misc_blocks.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

#include "families/image_modern/misc_astc.hpp"

namespace cvt::image_modern {

namespace {

using u8 = std::uint8_t;

std::uint64_t be64(const u8* p) noexcept {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = v << 8 | p[i];
  return v;
}
u8 clamp8(int v) noexcept { return static_cast<u8>(std::clamp(v, 0, 255)); }

// --------------------------------------------------------------------------- BC1-BC5

/// Colour part of a BC1/2/3 block. `punch` enables BC1's 3-colour mode.
void bc1_colour(const u8* b, u8* out, bool punch) {
  const int c0 = b[0] | b[1] << 8;
  const int c1 = b[2] | b[3] << 8;
  int pal[4][4];
  const auto expand = [](int c, int* p) {
    const int r = (c >> 11) & 31;
    const int g = (c >> 5) & 63;
    const int bl = c & 31;
    p[0] = r << 3 | r >> 2;
    p[1] = g << 2 | g >> 4;
    p[2] = bl << 3 | bl >> 2;
    p[3] = 255;
  };
  expand(c0, pal[0]);
  expand(c1, pal[1]);
  if (c0 > c1 || !punch) {
    for (int c = 0; c < 3; ++c) {
      pal[2][c] = (2 * pal[0][c] + pal[1][c] + 1) / 3;
      pal[3][c] = (pal[0][c] + 2 * pal[1][c] + 1) / 3;
    }
    pal[2][3] = pal[3][3] = 255;
  } else {
    for (int c = 0; c < 3; ++c) {
      pal[2][c] = (pal[0][c] + pal[1][c]) / 2;
      pal[3][c] = 0;
    }
    pal[2][3] = 255;
    pal[3][3] = 0;
  }
  const std::uint32_t indices = b[4] | b[5] << 8 | b[6] << 16 | static_cast<std::uint32_t>(b[7]) << 24;
  for (int i = 0; i < 16; ++i) {
    const int* p = pal[(indices >> (2 * i)) & 3];
    for (int c = 0; c < 4; ++c) out[i * 4 + c] = static_cast<u8>(p[c]);
  }
}

/// An 8-byte BC4-style channel block into 16 values (stride `stride` bytes).
void bc4_channel(const u8* b, u8* out, int stride, bool is_signed) {
  int a0 = b[0];
  int a1 = b[1];
  int pal[8];
  if (is_signed) {
    a0 = std::max(-127, static_cast<int>(static_cast<std::int8_t>(b[0])));
    a1 = std::max(-127, static_cast<int>(static_cast<std::int8_t>(b[1])));
  }
  pal[0] = a0;
  pal[1] = a1;
  if (a0 > a1) {
    for (int i = 1; i < 7; ++i) pal[i + 1] = ((7 - i) * a0 + i * a1 + 3) / 7;
  } else {
    for (int i = 1; i < 5; ++i) pal[i + 1] = ((5 - i) * a0 + i * a1 + 2) / 5;
    pal[6] = is_signed ? -127 : 0;
    pal[7] = is_signed ? 127 : 255;
  }
  std::uint64_t bits = 0;
  for (int i = 7; i >= 2; --i) bits = bits << 8 | b[i];
  for (int i = 0; i < 16; ++i) {
    const int v = pal[(bits >> (3 * i)) & 7];
    out[i * stride] = is_signed ? static_cast<u8>((v + 127) * 255 / 254) : static_cast<u8>(v);
  }
}

// --------------------------------------------------------------------------- BC7

constexpr std::array<std::uint32_t, 64> bc7_partition2 = {
    0x50505050, 0x40404040, 0x54545454, 0x54505040, 0x50404000, 0x55545450, 0x55545040, 0x54504000,
    0x50400000, 0x55555450, 0x55544000, 0x54400000, 0x55555440, 0x55550000, 0x55555500, 0x55000000,
    0x55150100, 0x00004054, 0x15010000, 0x00405054, 0x00004050, 0x15050100, 0x05010000, 0x40505054,
    0x00404050, 0x05010100, 0x14141414, 0x05141450, 0x01155440, 0x00555500, 0x15014054, 0x05414150,
    0x44444444, 0x55005500, 0x11441144, 0x05055050, 0x05500550, 0x11114444, 0x41144114, 0x44111144,
    0x15055054, 0x01055040, 0x05041050, 0x05455150, 0x14414114, 0x50050550, 0x41411414, 0x00141400,
    0x00041504, 0x00105410, 0x10541000, 0x04150400, 0x50410514, 0x41051450, 0x05415014, 0x14054150,
    0x41050514, 0x41505014, 0x40011554, 0x54150140, 0x50505500, 0x00555050, 0x15151010, 0x54540404,
};
constexpr std::array<std::uint32_t, 64> bc7_partition3 = {
    0xAA685050, 0x6A5A5040, 0x5A5A4200, 0x5450A0A8, 0xA5A50000, 0xA0A05050, 0x5555A0A0, 0x5A5A5050,
    0xAA550000, 0xAA555500, 0xAAAA5500, 0x90909090, 0x94949494, 0xA4A4A4A4, 0xA9A59450, 0x2A0A4250,
    0xA5945040, 0x0A425054, 0xA5A5A500, 0x55A0A0A0, 0xA8A85454, 0x6A6A4040, 0xA4A45000, 0x1A1A0500,
    0x0050A4A4, 0xAAA59090, 0x14696914, 0x69691400, 0xA08585A0, 0xAA821414, 0x50A4A450, 0x6A5A0200,
    0xA9A58000, 0x5090A0A8, 0xA8A09050, 0x24242424, 0x00AA5500, 0x24924924, 0x24499224, 0x50A50A50,
    0x500AA550, 0xAAAA4444, 0x66660000, 0xA5A0A5A0, 0x50A050A0, 0x69286928, 0x44AAAA44, 0x66666600,
    0xAA444444, 0x54A854A8, 0x95809580, 0x96969600, 0xA85454A8, 0x80959580, 0xAA141414, 0x96960000,
    0xAAAA1414, 0xA05050A0, 0xA0A5A5A0, 0x96000000, 0x40804080, 0xA9A8A9A8, 0xAAAAAA44, 0x2A4A5254,
};
constexpr std::array<u8, 64> bc7_anchor2 = {15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
                                            15, 2,  8,  2,  2,  8,  8,  15, 2,  8,  2,  2,  8,  8,  2,  2,
                                            15, 15, 6,  8,  2,  8,  15, 15, 2,  8,  2,  2,  2,  15, 15, 6,
                                            6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2,  15};
constexpr std::array<u8, 64> bc7_anchor3_2 = {3,  3,  15, 15, 8,  3,  15, 15, 8,  8,  6,  6,  6,  5,  3,  3,
                                              3,  3,  8,  15, 3,  3,  6,  10, 5,  8,  8,  6,  8,  5,  15, 15,
                                              8,  15, 3,  5,  6,  10, 8,  15, 15, 3,  15, 5,  15, 15, 15, 15,
                                              3,  15, 5,  5,  5,  8,  5,  10, 5,  10, 8,  13, 15, 12, 3,  3};
constexpr std::array<u8, 64> bc7_anchor3_3 = {15, 8,  8,  3,  15, 15, 3,  8,  15, 15, 15, 15, 15, 15, 15, 8,
                                              15, 8,  15, 3,  15, 8,  15, 8,  3,  15, 6,  10, 15, 15, 10, 8,
                                              15, 3,  15, 10, 10, 8,  9,  10, 6,  15, 8,  15, 3,  6,  6,  8,
                                              15, 3,  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3,  15, 15, 8};
constexpr std::array<int, 4> bc7_w2 = {0, 21, 43, 64};
constexpr std::array<int, 8> bc7_w3 = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr std::array<int, 16> bc7_w4 = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

struct Bc7Mode {
  int subsets, partition_bits, rotation_bits, selector_bits, colour_bits, alpha_bits, endpoint_pbits, shared_pbits,
      index_bits, index2_bits;
};
constexpr std::array<Bc7Mode, 8> bc7_modes = {{
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0},
    {2, 6, 0, 0, 6, 0, 0, 1, 3, 0},
    {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
    {2, 6, 0, 0, 7, 0, 1, 0, 2, 0},
    {1, 0, 2, 1, 5, 6, 0, 0, 2, 3},
    {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
    {1, 0, 0, 0, 7, 7, 1, 0, 4, 0},
    {2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
}};

class LsbBits {
 public:
  explicit LsbBits(const u8* data) : data_(data) {}
  int get(int n) {
    int v = 0;
    for (int i = 0; i < n; ++i, ++pos_) v |= ((data_[pos_ >> 3] >> (pos_ & 7)) & 1) << i;
    return v;
  }

 private:
  const u8* data_;
  int pos_ = 0;
};

int bc7_subset(int subsets, int partition, int pixel) {
  if (subsets == 1) return 0;
  const std::uint32_t table = subsets == 2 ? bc7_partition2[static_cast<std::size_t>(partition)]
                                           : bc7_partition3[static_cast<std::size_t>(partition)];
  return static_cast<int>((table >> (2 * pixel)) & 3);
}

void bc7(const u8* b, u8* out) {
  int mode = 0;
  while (mode < 8 && ((b[0] >> mode) & 1) == 0) ++mode;
  if (mode == 8) {
    std::memset(out, 0, 64);  // reserved mode: transparent black
    return;
  }
  const Bc7Mode& m = bc7_modes[static_cast<std::size_t>(mode)];
  LsbBits bits(b);
  (void)bits.get(mode + 1);
  const int partition = bits.get(m.partition_bits);
  const int rotation = bits.get(m.rotation_bits);
  const int selector = bits.get(m.selector_bits);
  int ep[6][4] = {};
  const int endpoints = m.subsets * 2;
  for (int c = 0; c < 3; ++c) {
    for (int e = 0; e < endpoints; ++e) ep[e][c] = bits.get(m.colour_bits);
  }
  for (int e = 0; e < endpoints; ++e) ep[e][3] = m.alpha_bits > 0 ? bits.get(m.alpha_bits) : 255;
  int pbits[6] = {};
  if (m.endpoint_pbits) {
    for (int e = 0; e < endpoints; ++e) pbits[e] = bits.get(1);
  } else if (m.shared_pbits) {
    for (int s = 0; s < m.subsets; ++s) pbits[2 * s] = pbits[2 * s + 1] = bits.get(1);
  }
  const int pbit = m.endpoint_pbits || m.shared_pbits ? 1 : 0;
  for (int e = 0; e < endpoints; ++e) {
    for (int c = 0; c < 4; ++c) {
      int n = c < 3 ? m.colour_bits : m.alpha_bits;
      if (n == 0) {
        ep[e][c] = 255;
        continue;
      }
      int v = ep[e][c];
      if (pbit) {
        v = v << 1 | pbits[e];
        ++n;
      }
      ep[e][c] = (v << (8 - n)) | (v >> (2 * n - 8));
    }
  }
  const auto anchor = [&](int pixel) {
    if (pixel == 0) return true;
    if (m.subsets == 2) return pixel == bc7_anchor2[static_cast<std::size_t>(partition)];
    if (m.subsets == 3) {
      return pixel == bc7_anchor3_2[static_cast<std::size_t>(partition)] ||
             pixel == bc7_anchor3_3[static_cast<std::size_t>(partition)];
    }
    return false;
  };
  int index1[16];
  int index2[16] = {};
  for (int i = 0; i < 16; ++i) index1[i] = bits.get(anchor(i) ? m.index_bits - 1 : m.index_bits);
  if (m.index2_bits) {
    for (int i = 0; i < 16; ++i) index2[i] = bits.get(i == 0 ? m.index2_bits - 1 : m.index2_bits);
  }
  const auto weight = [](int nbits, int index) {
    return nbits == 2 ? bc7_w2[static_cast<std::size_t>(index)]
                      : nbits == 3 ? bc7_w3[static_cast<std::size_t>(index)] : bc7_w4[static_cast<std::size_t>(index)];
  };
  for (int i = 0; i < 16; ++i) {
    const int s = bc7_subset(m.subsets, partition, i);
    const int* e0 = ep[2 * s];
    const int* e1 = ep[2 * s + 1];
    int colour_w = weight(m.index_bits, index1[i]);
    int alpha_w = colour_w;
    if (m.index2_bits) {
      const int w2 = weight(m.index2_bits, index2[i]);
      if (selector) {
        alpha_w = colour_w;
        colour_w = w2;
      } else {
        alpha_w = w2;
      }
    }
    int px[4];
    for (int c = 0; c < 3; ++c) px[c] = ((64 - colour_w) * e0[c] + colour_w * e1[c] + 32) >> 6;
    px[3] = ((64 - alpha_w) * e0[3] + alpha_w * e1[3] + 32) >> 6;
    if (rotation > 0) std::swap(px[3], px[rotation - 1]);
    for (int c = 0; c < 4; ++c) out[i * 4 + c] = static_cast<u8>(px[c]);
  }
}

// --------------------------------------------------------------------------- ETC / EAC

constexpr int etc_modifiers[8][2] = {{2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}};
constexpr int etc_distances[8] = {3, 6, 11, 16, 23, 32, 41, 64};
constexpr int eac_modifiers[16][8] = {
    {-3, -6, -9, -15, 2, 5, 8, 14}, {-3, -7, -10, -13, 2, 6, 9, 12}, {-2, -5, -8, -13, 1, 4, 7, 12},
    {-2, -4, -6, -13, 1, 3, 5, 12}, {-3, -6, -8, -12, 2, 5, 7, 11},  {-3, -7, -9, -11, 2, 6, 8, 10},
    {-4, -7, -8, -11, 3, 6, 7, 10}, {-3, -5, -8, -11, 2, 4, 7, 10},  {-2, -6, -8, -10, 1, 5, 7, 9},
    {-2, -5, -8, -10, 1, 4, 7, 9},  {-2, -4, -8, -10, 1, 3, 7, 9},   {-2, -5, -7, -10, 1, 4, 6, 9},
    {-3, -4, -7, -10, 2, 3, 6, 9},  {-1, -2, -3, -10, 0, 1, 2, 9},   {-4, -6, -8, -9, 3, 5, 7, 8},
    {-3, -5, -7, -9, 2, 4, 6, 8},
};

int bits_of(std::uint64_t v, int high, int low) { return static_cast<int>((v >> low) & ((1ull << (high - low + 1)) - 1)); }
int ext4(int v) { return v << 4 | v; }
int ext5(int v) { return v << 3 | v >> 2; }

/// ETC1/ETC2 colour block. `etc2` enables the T/H/planar modes; `punch`
/// selects the punch-through alpha variant (bit 33 = opaque flag).
void etc_colour(const u8* b, u8* out, bool etc2, bool punch) {
  const std::uint64_t v = be64(b);
  const bool diff = punch || bits_of(v, 33, 33) != 0;
  const bool opaque = !punch || bits_of(v, 33, 33) != 0;
  const auto index = [&](int x, int y) {
    const int i = x * 4 + y;
    return (bits_of(v, 16 + i, 16 + i) << 1) | bits_of(v, i, i);
  };
  const auto put = [&](int x, int y, int r, int g, int bl, int a) {
    u8* p = out + (y * 4 + x) * 4;
    p[0] = clamp8(r);
    p[1] = clamp8(g);
    p[2] = clamp8(bl);
    p[3] = static_cast<u8>(a);
  };
  if (diff) {
    const int r = bits_of(v, 63, 59);
    const int g = bits_of(v, 55, 51);
    const int bl = bits_of(v, 47, 43);
    const auto signed3 = [](int d) { return d >= 4 ? d - 8 : d; };
    const int r2 = r + signed3(bits_of(v, 58, 56));
    const int g2 = g + signed3(bits_of(v, 50, 48));
    const int b2 = bl + signed3(bits_of(v, 42, 40));
    if (etc2 && (r2 < 0 || r2 > 31)) {
      // T mode.
      const int c1[3] = {ext4(bits_of(v, 60, 59) << 2 | bits_of(v, 57, 56)), ext4(bits_of(v, 55, 52)),
                         ext4(bits_of(v, 51, 48))};
      const int c2[3] = {ext4(bits_of(v, 47, 44)), ext4(bits_of(v, 43, 40)), ext4(bits_of(v, 39, 36))};
      const int d = etc_distances[bits_of(v, 35, 34) << 1 | bits_of(v, 32, 32)];
      const int paint[4][3] = {{c1[0], c1[1], c1[2]},
                               {c2[0] + d, c2[1] + d, c2[2] + d},
                               {c2[0], c2[1], c2[2]},
                               {c2[0] - d, c2[1] - d, c2[2] - d}};
      for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
          const int i = index(x, y);
          if (!opaque && i == 2) {
            put(x, y, 0, 0, 0, 0);
          } else {
            put(x, y, paint[i][0], paint[i][1], paint[i][2], 255);
          }
        }
      }
      return;
    }
    if (etc2 && (g2 < 0 || g2 > 31)) {
      // H mode.
      const int r1 = bits_of(v, 62, 59);
      const int g1 = bits_of(v, 58, 56) << 1 | bits_of(v, 52, 52);
      const int b1 = bits_of(v, 51, 51) << 3 | bits_of(v, 49, 47);
      const int rr2 = bits_of(v, 46, 43);
      const int gg2 = bits_of(v, 42, 39);
      const int bb2 = bits_of(v, 38, 35);
      const int k1 = r1 << 8 | g1 << 4 | b1;
      const int k2 = rr2 << 8 | gg2 << 4 | bb2;
      const int d = etc_distances[bits_of(v, 34, 34) << 2 | bits_of(v, 32, 32) << 1 | (k1 >= k2 ? 1 : 0)];
      const int c1[3] = {ext4(r1), ext4(g1), ext4(b1)};
      const int c2[3] = {ext4(rr2), ext4(gg2), ext4(bb2)};
      const int paint[4][3] = {{c1[0] + d, c1[1] + d, c1[2] + d},
                               {c1[0] - d, c1[1] - d, c1[2] - d},
                               {c2[0] + d, c2[1] + d, c2[2] + d},
                               {c2[0] - d, c2[1] - d, c2[2] - d}};
      for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
          const int i = index(x, y);
          if (!opaque && i == 2) {
            put(x, y, 0, 0, 0, 0);
          } else {
            put(x, y, paint[i][0], paint[i][1], paint[i][2], 255);
          }
        }
      }
      return;
    }
    if (etc2 && (b2 < 0 || b2 > 31)) {
      // Planar mode (always opaque).
      const auto e6 = [](int x) { return x << 2 | x >> 4; };
      const auto e7 = [](int x) { return x << 1 | x >> 6; };
      const int ro = e6(bits_of(v, 62, 57));
      const int go = e7(bits_of(v, 56, 56) << 6 | bits_of(v, 54, 49));
      const int bo = e6(bits_of(v, 48, 48) << 5 | bits_of(v, 44, 43) << 3 | bits_of(v, 41, 39));
      const int rh = e6(bits_of(v, 38, 34) << 1 | bits_of(v, 32, 32));
      const int gh = e7(bits_of(v, 31, 25));
      const int bh = e6(bits_of(v, 24, 19));
      const int rv = e6(bits_of(v, 18, 13));
      const int gv = e7(bits_of(v, 12, 6));
      const int bv = e6(bits_of(v, 5, 0));
      for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
          put(x, y, (x * (rh - ro) + y * (rv - ro) + 4 * ro + 2) >> 2, (x * (gh - go) + y * (gv - go) + 4 * go + 2) >> 2,
              (x * (bh - bo) + y * (bv - bo) + 4 * bo + 2) >> 2, 255);
        }
      }
      return;
    }
    (void)b2;
  }
  int base[2][3];
  if (diff) {
    const int r = bits_of(v, 63, 59);
    const int g = bits_of(v, 55, 51);
    const int bl = bits_of(v, 47, 43);
    const auto signed3 = [](int d) { return d >= 4 ? d - 8 : d; };
    base[0][0] = ext5(r);
    base[0][1] = ext5(g);
    base[0][2] = ext5(bl);
    base[1][0] = ext5((r + signed3(bits_of(v, 58, 56))) & 31);
    base[1][1] = ext5((g + signed3(bits_of(v, 50, 48))) & 31);
    base[1][2] = ext5((bl + signed3(bits_of(v, 42, 40))) & 31);
  } else {
    base[0][0] = ext4(bits_of(v, 63, 60));
    base[1][0] = ext4(bits_of(v, 59, 56));
    base[0][1] = ext4(bits_of(v, 55, 52));
    base[1][1] = ext4(bits_of(v, 51, 48));
    base[0][2] = ext4(bits_of(v, 47, 44));
    base[1][2] = ext4(bits_of(v, 43, 40));
  }
  const int table[2] = {bits_of(v, 39, 37), bits_of(v, 36, 34)};
  const bool flip = bits_of(v, 32, 32) != 0;
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      const int sub = flip ? (y >= 2 ? 1 : 0) : (x >= 2 ? 1 : 0);
      const int i = index(x, y);
      if (!opaque && i == 2) {
        put(x, y, 0, 0, 0, 0);
        continue;
      }
      const int* mod = etc_modifiers[table[sub]];
      int m = 0;
      switch (i) {
        case 0: m = opaque ? mod[0] : 0; break;
        case 1: m = mod[1]; break;
        case 2: m = -mod[0]; break;
        default: m = -mod[1]; break;
      }
      put(x, y, base[sub][0] + m, base[sub][1] + m, base[sub][2] + m, 255);
    }
  }
}

/// EAC 8-bit alpha block into 16 values with `stride`.
void eac_alpha(const u8* b, u8* out, int stride) {
  const std::uint64_t v = be64(b);
  const int base = bits_of(v, 63, 56);
  const int mult = bits_of(v, 55, 52);
  const int* mod = eac_modifiers[bits_of(v, 51, 48)];
  for (int i = 0; i < 16; ++i) {
    const int x = i / 4;
    const int y = i % 4;
    const int idx = bits_of(v, 47 - 3 * i, 45 - 3 * i);
    out[(y * 4 + x) * stride] = clamp8(base + mod[idx] * mult);
  }
}

/// EAC 11-bit channel block into 8-bit values with `stride`.
void eac_r11(const u8* b, u8* out, int stride, bool is_signed) {
  const std::uint64_t v = be64(b);
  const int mult = bits_of(v, 55, 52);
  const int* mod = eac_modifiers[bits_of(v, 51, 48)];
  for (int i = 0; i < 16; ++i) {
    const int x = i / 4;
    const int y = i % 4;
    const int idx = bits_of(v, 47 - 3 * i, 45 - 3 * i);
    int value = 0;
    if (is_signed) {
      const int base = std::max(-127, static_cast<int>(static_cast<std::int8_t>(bits_of(v, 63, 56))));
      value = std::clamp(base * 8 + (mult == 0 ? mod[idx] : mod[idx] * mult * 8), -1023, 1023);
      out[(y * 4 + x) * stride] = static_cast<u8>((value + 1023) * 255 / 2046);
    } else {
      const int base = bits_of(v, 63, 56);
      value = std::clamp(base * 8 + 4 + (mult == 0 ? mod[idx] : mod[idx] * mult * 8), 0, 2047);
      out[(y * 4 + x) * stride] = static_cast<u8>((value * 255 + 1023) / 2047);
    }
  }
}

}  // namespace

BlockShape block_shape(BlockFormat format, int astc_width, int astc_height) noexcept {
  switch (format) {
    case BlockFormat::BC1:
    case BlockFormat::BC4:
    case BlockFormat::BC4Signed:
    case BlockFormat::ETC1:
    case BlockFormat::ETC2:
    case BlockFormat::ETC2A1:
    case BlockFormat::EACR11:
    case BlockFormat::EACR11Signed: return {4, 4, 8};
    case BlockFormat::ASTC: return {astc_width, astc_height, 16};
    default: return {4, 4, 16};
  }
}

const char* block_format_name(BlockFormat format) noexcept {
  switch (format) {
    case BlockFormat::BC1: return "BC1";
    case BlockFormat::BC2: return "BC2";
    case BlockFormat::BC3: return "BC3";
    case BlockFormat::BC4: case BlockFormat::BC4Signed: return "BC4";
    case BlockFormat::BC5: case BlockFormat::BC5Signed: return "BC5";
    case BlockFormat::BC7: return "BC7";
    case BlockFormat::ETC1: return "ETC1";
    case BlockFormat::ETC2: return "ETC2";
    case BlockFormat::ETC2A1: return "ETC2 punch-through alpha";
    case BlockFormat::ETC2EAC: return "ETC2 + EAC";
    case BlockFormat::EACR11: case BlockFormat::EACR11Signed: return "EAC R11";
    case BlockFormat::EACRG11: case BlockFormat::EACRG11Signed: return "EAC RG11";
    case BlockFormat::ASTC: return "ASTC";
  }
  return "block";
}

bool decode_block(BlockFormat format, const std::uint8_t* b, std::uint8_t* out, int astc_width,
                  int astc_height) noexcept {
  switch (format) {
    case BlockFormat::BC1: bc1_colour(b, out, true); return true;
    case BlockFormat::BC2:
      bc1_colour(b + 8, out, false);
      for (int i = 0; i < 16; ++i) out[i * 4 + 3] = static_cast<u8>(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
      return true;
    case BlockFormat::BC3:
      bc1_colour(b + 8, out, false);
      bc4_channel(b, out + 3, 4, false);
      return true;
    case BlockFormat::BC4:
    case BlockFormat::BC4Signed:
      bc4_channel(b, out, 4, format == BlockFormat::BC4Signed);
      for (int i = 0; i < 16; ++i) {
        out[i * 4 + 1] = out[i * 4 + 2] = out[i * 4];
        out[i * 4 + 3] = 255;
      }
      return true;
    case BlockFormat::BC5:
    case BlockFormat::BC5Signed:
      bc4_channel(b, out, 4, format == BlockFormat::BC5Signed);
      bc4_channel(b + 8, out + 1, 4, format == BlockFormat::BC5Signed);
      for (int i = 0; i < 16; ++i) {
        out[i * 4 + 2] = 0;
        out[i * 4 + 3] = 255;
      }
      return true;
    case BlockFormat::BC7: bc7(b, out); return true;
    case BlockFormat::ETC1: etc_colour(b, out, false, false); return true;
    case BlockFormat::ETC2: etc_colour(b, out, true, false); return true;
    case BlockFormat::ETC2A1: etc_colour(b, out, true, true); return true;
    case BlockFormat::ETC2EAC:
      etc_colour(b + 8, out, true, false);
      eac_alpha(b, out + 3, 4);
      return true;
    case BlockFormat::EACR11:
    case BlockFormat::EACR11Signed:
      eac_r11(b, out, 4, format == BlockFormat::EACR11Signed);
      for (int i = 0; i < 16; ++i) {
        out[i * 4 + 1] = out[i * 4 + 2] = out[i * 4];
        out[i * 4 + 3] = 255;
      }
      return true;
    case BlockFormat::EACRG11:
    case BlockFormat::EACRG11Signed:
      eac_r11(b, out, 4, format == BlockFormat::EACRG11Signed);
      eac_r11(b + 8, out + 1, 4, format == BlockFormat::EACRG11Signed);
      for (int i = 0; i < 16; ++i) {
        out[i * 4 + 2] = 0;
        out[i * 4 + 3] = 255;
      }
      return true;
    case BlockFormat::ASTC: return decode_astc_block(b, astc_width, astc_height, out);
  }
  return false;
}

std::uint64_t surface_bytes(BlockFormat format, std::uint32_t width, std::uint32_t height, int astc_width,
                            int astc_height) noexcept {
  const BlockShape shape = block_shape(format, astc_width, astc_height);
  if (shape.width <= 0 || shape.height <= 0) return 0;
  const std::uint64_t bx = (std::uint64_t{width} + static_cast<std::uint64_t>(shape.width) - 1) /
                           static_cast<std::uint64_t>(shape.width);
  const std::uint64_t by = (std::uint64_t{height} + static_cast<std::uint64_t>(shape.height) - 1) /
                           static_cast<std::uint64_t>(shape.height);
  return bx * by * static_cast<std::uint64_t>(shape.bytes);
}

Result<ir::Frame> decode_surface(BlockFormat format, ByteView data, std::uint32_t width, std::uint32_t height,
                                 const Context& ctx, int astc_width, int astc_height) {
  const BlockShape shape = block_shape(format, astc_width, astc_height);
  const std::uint64_t needed = surface_bytes(format, width, height, astc_width, astc_height);
  if (needed == 0 || data.size() < needed) {
    return fail(Status::Decode, "the {} texture data is truncated", block_format_name(format));
  }
  CVT_TRY(ctx.limits.check_pixels(width, height));
  ir::Frame frame = ir::Frame::filled(width, height);
  const std::uint32_t blocks_x = (width + static_cast<std::uint32_t>(shape.width) - 1) / static_cast<std::uint32_t>(shape.width);
  const std::uint32_t blocks_y = (height + static_cast<std::uint32_t>(shape.height) - 1) / static_cast<std::uint32_t>(shape.height);
  const auto* src = reinterpret_cast<const std::uint8_t*>(data.data());
  std::array<std::uint8_t, 12 * 12 * 4> pixels{};
  bool invalid = false;
  for (std::uint32_t by = 0; by < blocks_y; ++by) {
    if ((by & 63) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(by) / blocks_y));
    for (std::uint32_t bx = 0; bx < blocks_x; ++bx) {
      const std::uint8_t* block = src + (std::size_t{by} * blocks_x + bx) * static_cast<std::size_t>(shape.bytes);
      if (!decode_block(format, block, pixels.data(), astc_width, astc_height)) invalid = true;
      for (int y = 0; y < shape.height; ++y) {
        const std::uint32_t py = by * static_cast<std::uint32_t>(shape.height) + static_cast<std::uint32_t>(y);
        if (py >= height) break;
        for (int x = 0; x < shape.width; ++x) {
          const std::uint32_t px = bx * static_cast<std::uint32_t>(shape.width) + static_cast<std::uint32_t>(x);
          if (px >= width) break;
          std::memcpy(frame.rgba.data() + frame.offset(px, py), pixels.data() + (y * shape.width + x) * 4, 4);
        }
      }
    }
  }
  if (invalid) ctx.warn("Some texture blocks are invalid and were shown in magenta.");
  return frame;
}

}  // namespace cvt::image_modern
