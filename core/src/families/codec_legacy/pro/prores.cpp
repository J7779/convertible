// Convertible core - codec_legacy family: Apple ProRes (SMPTE RDD 36).
//
// Decoder: 4:2:2 (Proxy, LT, standard, HQ) and 4:4:4 (4444, 4444 XQ) frames,
// progressive and interlaced (two field pictures), custom quantisation
// matrices, 8- and 16-bit alpha. Output: I422P10, I444P10 without alpha,
// RGBA8 when the frame carries alpha (the IR has no YCbCr+alpha layout).
// Encoder: 4:2:2 10-bit (I422P10 native; other inputs converted) or, for
// RGBA8 input, 4:4:4 with 8-bit alpha. `quality` selects the quantiser
// (90 -> near-transparent, HQ-class data rates); the container labels the
// track by the profile it wants (apch/apcn/apcs/apco/ap4h): the bitstream
// syntax is the same for all of them.
//
// Coefficients: frame value = level * matrix * quantiser / 4 on the
// orthonormal DCT scale, samples = IDCT + 512, clipped to 4..1019.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "families/codec_legacy/common/bitstream.hpp"
#include "families/codec_legacy/common/codec_base.hpp"
#include "families/codec_legacy/common/dct.hpp"
#include "families/codec_legacy/pro/pro.hpp"

namespace cvt::legacy::pro {

namespace {

// Scan orders (RDD 36): progressive frames, and field pictures.
constexpr std::uint8_t kProgressiveScan[64] = {
    0,  1,  8,  9,  2,  3,  10, 11, 16, 17, 24, 25, 18, 19, 26, 27, 4,  5,  12, 20, 13, 6,
    7,  14, 21, 28, 29, 22, 15, 23, 30, 31, 32, 33, 40, 48, 41, 34, 35, 42, 49, 56, 57, 50,
    43, 36, 37, 44, 51, 58, 59, 52, 45, 38, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
constexpr std::uint8_t kInterlacedScan[64] = {
    0,  8,  1,  9,  16, 24, 17, 25, 2,  10, 3,  11, 18, 26, 19, 27, 32, 40, 33, 34, 41, 48,
    56, 49, 42, 35, 43, 50, 57, 58, 51, 59, 4,  12, 5,  6,  13, 20, 28, 21, 14, 7,  15, 22,
    29, 36, 44, 37, 30, 23, 31, 38, 45, 52, 60, 53, 46, 39, 47, 54, 61, 62, 55, 63};

// Codebook descriptors: rice order (bits 7..5), exp-Golomb order (4..2),
// switch (1..0).
constexpr std::uint8_t kFirstDcCodebook = 0xB8;
constexpr std::uint8_t kDcCodebook[7] = {0x04, 0x28, 0x28, 0x4D, 0x4D, 0x70, 0x70};
constexpr std::uint8_t kRunCodebook[16] = {0x06, 0x06, 0x05, 0x05, 0x04, 0x29, 0x29, 0x29,
                                           0x29, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x4C};
constexpr std::uint8_t kLevelCodebook[10] = {0x04, 0x0A, 0x05, 0x06, 0x04, 0x28, 0x28, 0x28, 0x28, 0x4C};

/// Reads one adaptive Rice / exp-Golomb code word.
bool read_codeword(BitReader& br, std::uint8_t codebook, std::uint32_t& value) {
  const int switch_bits = codebook & 3;
  const int rice = codebook >> 5;
  const int expo = (codebook >> 2) & 7;
  int q = 0;
  while (br.peek(1) == 0) {
    if (++q > 31 || br.bits_left() == 0) return false;
    br.skip(1);
  }
  if (q > switch_bits) {
    const int bits = expo - switch_bits + (q << 1);
    // The leading zeros are part of the exp-Golomb word: rewind them.
    br.seek(br.position() - static_cast<std::uint64_t>(q));
    if (bits > 32 || bits <= 0) return false;
    const std::uint32_t raw = bits == 32 ? br.bits(32) : br.bits(bits);
    value = raw - (1u << expo) + (static_cast<std::uint32_t>(switch_bits + 1) << rice);
    return true;
  }
  br.skip(1);  // the terminating one
  value = (static_cast<std::uint32_t>(q) << rice) + (rice > 0 ? br.bits(rice) : 0u);
  return true;
}

void write_codeword(BitWriter& w, std::uint8_t codebook, std::uint32_t value) {
  const int switch_bits = (codebook & 3) + 1;
  const int rice = codebook >> 5;
  const int expo = (codebook >> 2) & 7;
  const std::uint32_t switch_value = static_cast<std::uint32_t>(switch_bits) << rice;
  if (value >= switch_value) {
    const std::uint32_t v = value - switch_value + (1u << expo);
    int exponent = 0;
    while ((v >> (exponent + 1)) != 0) ++exponent;
    w.put(exponent - expo + switch_bits, 0);
    w.put(exponent + 1, v);
  } else {
    const std::uint32_t q = value >> rice;
    if (q > 0) w.put(static_cast<int>(q), 0);
    w.put(1, 1);
    if (rice > 0) w.put(rice, value & ((1u << rice) - 1u));
  }
}

[[nodiscard]] constexpr int to_signed(std::uint32_t x) noexcept { return static_cast<int>(x >> 1) ^ -static_cast<int>(x & 1); }
[[nodiscard]] constexpr std::uint32_t from_signed(int x) noexcept {
  return static_cast<std::uint32_t>(x < 0 ? -2 * x - 1 : 2 * x);
}

struct FrameHeader {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  int chroma = 2;     // 2 = 4:2:2, 3 = 4:4:4
  int interlace = 0;  // 0 progressive, 1 top field first, 2 bottom field first
  int primaries = 0;
  int transfer = 0;
  int matrix = 0;
  int alpha = 0;      // 0 none, 1 8-bit, 2 16-bit
  std::array<std::uint8_t, 64> luma_q{};
  std::array<std::uint8_t, 64> chroma_q{};
};

class ProresDecoder final : public SimpleDecoder {
 protected:
  Result<VideoFrame> decode(const Packet& packet) override {
    const auto data = as_u8(ByteView(packet.bytes));
    if (data.size() < 28 || std::memcmp(data.data() + 4, "icpf", 4) != 0) return fail(Status::Decode, "not a ProRes frame");
    const std::uint32_t frame_size = static_cast<std::uint32_t>(data[0]) << 24 | static_cast<std::uint32_t>(data[1]) << 16 |
                                     static_cast<std::uint32_t>(data[2]) << 8 | data[3];
    const auto frame = data.subspan(8, std::min<std::size_t>(data.size(), std::max<std::size_t>(frame_size, 8)) - 8);
    if (frame.size() < 20) return fail(Status::Decode, "truncated ProRes frame header");
    FrameHeader h;
    const std::size_t header_size = static_cast<std::size_t>(frame[0]) << 8 | frame[1];
    if (header_size < 20 || header_size > frame.size()) return fail(Status::Decode, "invalid ProRes frame header size");
    h.width = static_cast<std::uint32_t>(frame[8]) << 8 | frame[9];
    h.height = static_cast<std::uint32_t>(frame[10]) << 8 | frame[11];
    CVT_TRY(check_dimensions(h.width, h.height));
    h.chroma = frame[12] >> 6;
    h.interlace = (frame[12] >> 2) & 3;
    h.primaries = frame[14];
    h.transfer = frame[15];
    h.matrix = frame[16];
    h.alpha = frame[17] & 15;
    if (h.chroma != 2 && h.chroma != 3) return fail(Status::Decode, "unsupported ProRes chroma format {}", h.chroma);
    if (h.interlace == 3 || h.alpha > 2) return fail(Status::Decode, "invalid ProRes frame header");
    const std::uint8_t flags = frame[19];
    std::size_t at = 20;
    h.luma_q.fill(4);
    if ((flags & 2) != 0) {
      if (at + 64 > header_size) return fail(Status::Decode, "truncated ProRes matrices");
      std::copy_n(frame.begin() + static_cast<std::ptrdiff_t>(at), 64, h.luma_q.begin());
      at += 64;
    }
    h.chroma_q = h.luma_q;
    if ((flags & 1) != 0) {
      if (at + 64 > header_size) return fail(Status::Decode, "truncated ProRes matrices");
      std::copy_n(frame.begin() + static_cast<std::ptrdiff_t>(at), 64, h.chroma_q.begin());
    }
    // Planes at 10 bits (+ alpha).
    const std::uint32_t cw = h.chroma == 3 ? h.width : (h.width + 1) / 2;
    const std::uint32_t mbw = (h.width + 15) / 16;
    const std::uint32_t aligned_w = mbw * 16;
    const std::uint32_t mbh_frame = (h.height + 15) / 16;
    const std::uint32_t aligned_h = (h.interlace != 0 ? (h.height + 31) / 32 * 32 : mbh_frame * 16);
    Plane10 y(aligned_w, aligned_h);
    Plane10 u(h.chroma == 3 ? aligned_w : aligned_w / 2, aligned_h);
    Plane10 v(h.chroma == 3 ? aligned_w : aligned_w / 2, aligned_h);
    Plane10 a(h.alpha != 0 ? aligned_w : 0, h.alpha != 0 ? aligned_h : 0);
    std::size_t pos = header_size;
    const int pictures = h.interlace != 0 ? 2 : 1;
    for (int p = 0; p < pictures; ++p) {
      // Field order: the first picture is the top field unless bottom-first.
      const int field = h.interlace == 0 ? -1 : (h.interlace == 1 ? p : 1 - p);
      CVT_TRY_ASSIGN(const std::size_t used, decode_picture(frame.subspan(pos), h, field, y, u, v, a));
      pos += used;
    }
    (void)cw;
    return make_frame(h, y, u, v, a);
  }

 private:
  struct Plane10 {
    std::vector<std::uint16_t> s;
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    Plane10(std::uint32_t width, std::uint32_t height) : s(static_cast<std::size_t>(width) * height, 512), w(width), h(height) {}
  };

  Result<std::size_t> decode_picture(std::span<const std::uint8_t> pic, const FrameHeader& h, int field, Plane10& y, Plane10& u,
                                     Plane10& v, Plane10& a) {
    if (pic.size() < 8) return fail(Status::Decode, "truncated ProRes picture header");
    const std::size_t header = pic[0] >> 3;
    const std::uint32_t size = static_cast<std::uint32_t>(pic[1]) << 24 | static_cast<std::uint32_t>(pic[2]) << 16 |
                               static_cast<std::uint32_t>(pic[3]) << 8 | pic[4];
    const int log2_slice = pic[7] >> 4;
    if (header < 8 || size < header || size > pic.size() || log2_slice > 3) return fail(Status::Decode, "invalid ProRes picture header");
    const std::uint32_t mbw = (h.width + 15) / 16;
    const std::uint32_t mbh = field >= 0 ? (h.height + 31) / 32 : (h.height + 15) / 16;
    // Slices per row: as many full slices as fit, then halving sizes.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> row_slices;  // (mb_x, mb_count)
    {
      std::uint32_t x = 0;
      std::uint32_t n = 1u << log2_slice;
      while (x < mbw) {
        while (mbw - x < n) n >>= 1;
        row_slices.emplace_back(x, n);
        x += n;
      }
    }
    const std::size_t slice_count = row_slices.size() * mbh;
    const std::size_t index_at = header;
    std::size_t data_at = index_at + slice_count * 2;
    if (data_at > size) return fail(Status::Decode, "truncated ProRes slice table");
    const std::uint8_t* scan = field >= 0 ? kInterlacedScan : kProgressiveScan;
    for (std::size_t i = 0; i < slice_count; ++i) {
      const std::size_t slice_size = static_cast<std::size_t>(pic[index_at + 2 * i]) << 8 | pic[index_at + 2 * i + 1];
      if (data_at + slice_size > size) return fail(Status::Decode, "truncated ProRes slice");
      const auto [mb_x, mb_count] = row_slices[i % row_slices.size()];
      const std::uint32_t mb_y = static_cast<std::uint32_t>(i / row_slices.size());
      CVT_TRY(decode_slice(pic.subspan(data_at, slice_size), h, field, scan, mb_x, mb_y, mb_count, y, u, v, a));
      data_at += slice_size;
    }
    return std::size_t{size};
  }

  Result<void> decode_slice(std::span<const std::uint8_t> s, const FrameHeader& h, int field, const std::uint8_t* scan, std::uint32_t mb_x,
                            std::uint32_t mb_y, std::uint32_t mb_count, Plane10& y, Plane10& u, Plane10& v, Plane10& a) {
    if (s.size() < 6) return fail(Status::Decode, "truncated ProRes slice header");
    const std::size_t header = s[0] >> 3;
    int qscale = std::clamp<int>(s[1], 1, 224);
    if (qscale > 128) qscale = (qscale - 96) << 2;
    const std::size_t y_size = static_cast<std::size_t>(s[2]) << 8 | s[3];
    const std::size_t u_size = static_cast<std::size_t>(s[4]) << 8 | s[5];
    if (header < 6 || header > s.size()) return fail(Status::Decode, "invalid ProRes slice header");
    std::size_t v_size = s.size() - header - std::min(s.size() - header, y_size + u_size);
    if (header > 7) {
      if (s.size() < 8) return fail(Status::Decode, "truncated ProRes slice header");
      v_size = static_cast<std::size_t>(s[6]) << 8 | s[7];
    }
    if (header + y_size + u_size + v_size > s.size()) return fail(Status::Decode, "invalid ProRes slice sizes");
    const std::size_t a_size = s.size() - header - y_size - u_size - v_size;
    std::size_t at = header;
    const int luma_blocks = static_cast<int>(mb_count * 4);
    const int chroma_blocks = static_cast<int>(mb_count * (h.chroma == 3 ? 4 : 2));
    std::vector<std::int32_t> blocks;
    // Luma: blocks TL, TR, BL, BR per macroblock.
    CVT_TRY(decode_blocks(s.subspan(at, y_size), luma_blocks, scan, blocks));
    put_blocks(blocks, h.luma_q, qscale, y, field, mb_x * 16, mb_y * 16, mb_count, true);
    at += y_size;
    CVT_TRY(decode_blocks(s.subspan(at, u_size), chroma_blocks, scan, blocks));
    put_blocks(blocks, h.chroma_q, qscale, u, field, mb_x * (h.chroma == 3 ? 16 : 8), mb_y * 16, mb_count, false, h.chroma == 3);
    at += u_size;
    CVT_TRY(decode_blocks(s.subspan(at, v_size), chroma_blocks, scan, blocks));
    put_blocks(blocks, h.chroma_q, qscale, v, field, mb_x * (h.chroma == 3 ? 16 : 8), mb_y * 16, mb_count, false, h.chroma == 3);
    at += v_size;
    if (h.alpha != 0 && a_size > 0) decode_alpha(s.subspan(at, a_size), h.alpha == 2 ? 16 : 8, a, field, mb_x * 16, mb_y * 16, mb_count);
    return {};
  }

  /// DC and AC coefficients of `count` blocks (count is a power of two).
  static Result<void> decode_blocks(std::span<const std::uint8_t> data, int count, const std::uint8_t* scan, std::vector<std::int32_t>& blocks) {
    blocks.assign(static_cast<std::size_t>(count) * 64, 0);
    BitReader br(data);
    std::uint32_t code = 0;
    if (!read_codeword(br, kFirstDcCodebook, code)) return fail(Status::Decode, "damaged ProRes DC coefficients");
    int prev = to_signed(code);
    blocks[0] = prev;
    code = 5;
    int sign = 0;
    for (int b = 1; b < count; ++b) {
      if (!read_codeword(br, kDcCodebook[std::min<std::uint32_t>(code, 6)], code)) return fail(Status::Decode, "damaged ProRes DC coefficients");
      if (code != 0) {
        sign ^= -static_cast<int>(code & 1);
      } else {
        sign = 0;
      }
      prev += ((static_cast<int>((code + 1) >> 1)) ^ sign) - sign;
      blocks[static_cast<std::size_t>(b) * 64] = prev;
    }
    int log2_count = 0;
    while ((1 << log2_count) < count) ++log2_count;
    const std::uint32_t max = 64u << log2_count;
    const std::uint32_t mask = static_cast<std::uint32_t>(count) - 1;
    std::uint32_t run = 4;
    std::uint32_t level = 2;
    std::uint32_t pos = mask;
    while (true) {
      const std::uint64_t left = br.bits_left();
      if (left == 0) break;
      if (left < 32 && br.peek(static_cast<int>(left)) == 0) break;
      if (!read_codeword(br, kRunCodebook[std::min<std::uint32_t>(run, 15)], run)) return fail(Status::Decode, "damaged ProRes AC coefficients");
      pos += run + 1;
      if (pos >= max) return fail(Status::Decode, "damaged ProRes AC coefficients");
      if (!read_codeword(br, kLevelCodebook[std::min<std::uint32_t>(level, 9)], level)) return fail(Status::Decode, "damaged ProRes AC coefficients");
      level += 1;
      const bool negative = br.bit();
      const int value = negative ? -static_cast<int>(level) : static_cast<int>(level);
      blocks[((pos & mask) << 6) + scan[pos >> log2_count]] = value;
    }
    return {};
  }

  /// Dequantises, transforms and stores the blocks of one component.
  /// Luma: TL, TR, BL, BR per macroblock. 4:2:2 chroma: top, bottom.
  /// 4:4:4 chroma: TL, BL, TR, BR.
  static void put_blocks(const std::vector<std::int32_t>& blocks, const std::array<std::uint8_t, 64>& matrix, int qscale, Plane10& plane,
                         int field, std::uint32_t x0, std::uint32_t y0, std::uint32_t mb_count, bool luma, bool full_chroma = false) {
    const int per_mb = luma || full_chroma ? 4 : 2;
    const int block_w = luma || full_chroma ? 16 : 8;
    std::array<std::int32_t, 64> coeff{};
    std::array<std::int32_t, 64> out{};
    for (std::uint32_t m = 0; m < mb_count; ++m) {
      for (int b = 0; b < per_mb; ++b) {
        const std::int32_t* src = blocks.data() + (static_cast<std::size_t>(m) * static_cast<std::size_t>(per_mb) + static_cast<std::size_t>(b)) * 64;
        for (int i = 0; i < 64; ++i) coeff[static_cast<std::size_t>(i)] = src[i] * matrix[static_cast<std::size_t>(i)] * qscale;
        idct8x8_scaled(coeff.data(), out.data(), 2);
        int bx;
        int by;
        if (luma) {
          bx = (b & 1) * 8;
          by = (b >> 1) * 8;
        } else if (full_chroma) {
          bx = (b >> 1) * 8;
          by = (b & 1) * 8;
        } else {
          bx = 0;
          by = b * 8;
        }
        const std::uint32_t px = x0 + m * static_cast<std::uint32_t>(block_w) + static_cast<std::uint32_t>(bx);
        for (int r = 0; r < 8; ++r) {
          std::uint32_t row = y0 + static_cast<std::uint32_t>(by + r);
          if (field >= 0) row = row * 2 + static_cast<std::uint32_t>(field);
          if (row >= plane.h) continue;
          for (int c = 0; c < 8; ++c) {
            if (px + static_cast<std::uint32_t>(c) >= plane.w) continue;
            plane.s[static_cast<std::size_t>(row) * plane.w + px + static_cast<std::uint32_t>(c)] =
                static_cast<std::uint16_t>(std::clamp(out[static_cast<std::size_t>(r * 8 + c)] + 512, 4, 1019));
          }
        }
      }
    }
  }

  static void decode_alpha(std::span<const std::uint8_t> data, int bits, Plane10& a, int field, std::uint32_t x0, std::uint32_t y0,
                           std::uint32_t mb_count) {
    BitReader br(data);
    const std::uint32_t mask = (1u << bits) - 1u;
    const std::size_t total = static_cast<std::size_t>(mb_count) * 256;
    std::vector<std::uint16_t> values(total, 0);
    std::size_t idx = 0;
    std::uint32_t alpha = mask;
    const auto store = [&](std::uint32_t v) { return static_cast<std::uint16_t>(bits == 16 ? v >> 6 : (v << 2) | (v >> 6)); };
    while (idx < total && !br.overrun()) {
      do {
        int val;
        if (br.bit()) {
          val = static_cast<int>(br.bits(bits));
        } else {
          val = static_cast<int>(br.bits(bits == 16 ? 7 : 4));
          const bool negative = (val & 1) != 0;
          val = (val + 2) >> 1;
          if (negative) val = -val;
        }
        alpha = static_cast<std::uint32_t>(static_cast<int>(alpha) + val) & mask;
        values[idx++] = store(alpha);
        if (idx >= total) break;
      } while (br.bits_left() > 0 && br.bit());
      if (idx >= total) break;
      std::uint32_t run = br.bits(4);
      if (run == 0) run = br.bits(11);
      run = static_cast<std::uint32_t>(std::min<std::size_t>(run, total - idx));
      for (std::uint32_t k = 0; k < run; ++k) values[idx++] = store(alpha);
    }
    const std::uint32_t width = mb_count * 16;
    for (std::uint32_t r = 0; r < 16; ++r) {
      std::uint32_t row = y0 + r;
      if (field >= 0) row = row * 2 + static_cast<std::uint32_t>(field);
      if (row >= a.h) continue;
      for (std::uint32_t c = 0; c < width && x0 + c < a.w; ++c) a.s[static_cast<std::size_t>(row) * a.w + x0 + c] = values[r * width + c];
    }
  }

  VideoFrame make_frame(const FrameHeader& h, const Plane10& y, const Plane10& u, const Plane10& v, const Plane10& a) const {
    ColorInfo color;
    color.range = ir::ColorRange::Limited;
    color.siting = ir::ChromaSiting::Left;
    color.matrix = h.matrix == 1 ? ir::ColorMatrix::BT709
                   : (h.matrix == 6 || h.matrix == 5) ? ir::ColorMatrix::BT601
                   : h.matrix == 9 ? ir::ColorMatrix::BT2020
                   : (h.height >= 720 ? ir::ColorMatrix::BT709 : ir::ColorMatrix::BT601);
    const PixelFormat format = h.chroma == 3 ? PixelFormat::I444P10 : PixelFormat::I422P10;
    VideoFrame f = VideoFrame::allocate(format, h.width, h.height);
    f.color = color;
    const Plane10* planes[3] = {&y, &u, &v};
    for (std::size_t p = 0; p < 3; ++p) {
      const std::uint32_t pw = ir::plane_width(format, p, h.width);
      for (std::uint32_t r = 0; r < h.height; ++r) {
        std::uint8_t* dst = f.planes[p].data.data() + static_cast<std::size_t>(r) * f.planes[p].stride;
        const std::uint16_t* src = planes[p]->s.data() + static_cast<std::size_t>(r) * planes[p]->w;
        for (std::uint32_t c = 0; c < pw; ++c) {
          dst[c * 2] = static_cast<std::uint8_t>(src[c] & 0xFF);
          dst[c * 2 + 1] = static_cast<std::uint8_t>(src[c] >> 8);
        }
      }
    }
    if (h.alpha == 0) return f;
    auto rgba = codec::to_rgba(f);
    if (!rgba) return f;
    VideoFrame out = std::move(*rgba);
    for (std::uint32_t r = 0; r < h.height; ++r) {
      std::uint8_t* row = out.planes[0].data.data() + static_cast<std::size_t>(r) * out.planes[0].stride;
      const std::uint16_t* src = a.s.data() + static_cast<std::size_t>(r) * a.w;
      for (std::uint32_t c = 0; c < h.width; ++c) row[c * 4 + 3] = static_cast<std::uint8_t>((src[c] * 255 + 511) / 1023);
    }
    return out;
  }
};

/// ProRes encoder (see the file comment).
class ProresEncoder final : public SimpleEncoder {
 protected:
  Result<void> on_configure() override {
    if (params_.width > 16384 || params_.height > 16384) return fail(Status::InvalidArgument, "picture too large for ProRes");
    alpha_ = params_.pixel_format == PixelFormat::RGBA8;
    full_chroma_ = alpha_ || params_.pixel_format == PixelFormat::I444 || params_.pixel_format == PixelFormat::I444P10;
    const int q = std::clamp(params_.quality, 1, 100);
    // Quantiser: 1 at quality 100, 4 at 90, about 16 at 50, up to 128.
    qscale_ = std::clamp(static_cast<int>(std::lround(quality_to_scale(q, 1.0, 4.0, 128.0))), 1, 128);
    if (params_.bitrate_kbps) {
      // A rough constant-rate model: about 0.35 bits per sample at quantiser 16.
      const double samples = static_cast<double>(params_.width) * params_.height * (full_chroma_ ? 3.0 : 2.0);
      const double fps = params_.fps.value() > 0 ? params_.fps.value() : 25.0;
      const double bits = static_cast<double>(*params_.bitrate_kbps) * 1000.0 / fps;
      qscale_ = std::clamp(static_cast<int>(std::lround(16.0 * 0.35 * samples / std::max(1.0, bits))), 1, 128);
    }
    // Matrices: flat for luma, a gentle slope for chroma at lower qualities.
    luma_q_.fill(4);
    for (int i = 0; i < 64; ++i) {
      const int u = i % 8;
      const int v = i / 8;
      chroma_q_[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(q >= 90 ? 4 : 4 + (u + v) / 4);
    }
    return {};
  }

  Result<Bytes> encode(const VideoFrame& frame, bool& keyframe) override {
    keyframe = true;
    ColorInfo color = params_.color;
    color.range = ir::ColorRange::Limited;
    color.siting = ir::ChromaSiting::Left;
    const PixelFormat format = full_chroma_ ? PixelFormat::I444P10 : PixelFormat::I422P10;
    CVT_TRY_ASSIGN(const VideoFrame f, to_format(frame, format, color));
    std::vector<std::uint8_t> alpha;
    if (alpha_) {
      CVT_TRY_ASSIGN(const VideoFrame rgba, to_format(frame, PixelFormat::RGBA8, {}));
      alpha.resize(static_cast<std::size_t>(params_.width) * params_.height);
      for (std::uint32_t r = 0; r < params_.height; ++r) {
        for (std::uint32_t c = 0; c < params_.width; ++c) alpha[static_cast<std::size_t>(r) * params_.width + c] = rgba.planes[0].data[r * rgba.planes[0].stride + c * 4 + 3];
      }
    }
    const std::uint32_t w = params_.width;
    const std::uint32_t h = params_.height;
    util::ByteWriter out;
    out.u32be(0);  // frame size, patched
    out.text("icpf");
    // Frame header (148 bytes with both matrices).
    out.u16be(148);
    out.u8(0);
    out.u8(full_chroma_ ? 1 : 0);  // bitstream version
    out.text("cvtc");
    out.u16be(static_cast<std::uint16_t>(w));
    out.u16be(static_cast<std::uint16_t>(h));
    out.u8(static_cast<std::uint8_t>((full_chroma_ ? 3 : 2) << 6));  // progressive
    out.u8(0);  // aspect ratio / frame rate unknown
    const std::uint8_t mc = color.matrix == ir::ColorMatrix::BT709 ? 1 : (color.matrix == ir::ColorMatrix::BT2020 ? 9 : 6);
    out.u8(mc == 6 ? 6 : mc);  // primaries
    out.u8(1);  // transfer characteristic: BT.709 (also the BT.601 curve)
    out.u8(mc);
    out.u8(alpha_ ? 1 : 0);  // 8-bit alpha
    out.u8(0);
    out.u8(3);  // both matrices follow
    out.bytes(std::span<const std::uint8_t>(luma_q_));
    out.bytes(std::span<const std::uint8_t>(chroma_q_));
    // One progressive picture.
    const std::uint32_t mbw = (w + 15) / 16;
    const std::uint32_t mbh = (h + 15) / 16;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> row_slices;
    for (std::uint32_t x = 0, n = 8; x < mbw;) {
      while (mbw - x < n) n >>= 1;
      row_slices.emplace_back(x, n);
      x += n;
    }
    const std::size_t picture_at = out.size();
    const std::size_t slice_count = row_slices.size() * mbh;
    out.u8(8 << 3);
    out.u32be(0);  // picture size, patched
    out.u16be(static_cast<std::uint16_t>(slice_count));
    out.u8(3 << 4);  // log2 of 8 macroblocks per slice
    const std::size_t table_at = out.size();
    out.zeros(slice_count * 2);
    Bytes bytes;
    std::vector<std::vector<std::uint8_t>> slices;
    for (std::uint32_t my = 0; my < mbh; ++my) {
      for (const auto& [mx, count] : row_slices) slices.push_back(encode_slice(f, alpha, mx, my, count));
    }
    for (std::size_t i = 0; i < slices.size(); ++i) {
      if (slices[i].size() > 65535) return fail(Status::Encode, "ProRes slice too large; lower the quality");
      out.bytes(std::span<const std::uint8_t>(slices[i]));
    }
    bytes = out.take();
    for (std::size_t i = 0; i < slices.size(); ++i) {
      bytes[table_at + 2 * i] = static_cast<std::byte>(slices[i].size() >> 8);
      bytes[table_at + 2 * i + 1] = static_cast<std::byte>(slices[i].size() & 0xFF);
    }
    const std::uint32_t picture_size = static_cast<std::uint32_t>(bytes.size() - picture_at);
    for (int k = 0; k < 4; ++k) bytes[picture_at + 1 + static_cast<std::size_t>(k)] = static_cast<std::byte>(picture_size >> (24 - 8 * k));
    const std::uint32_t total = static_cast<std::uint32_t>(bytes.size());
    for (int k = 0; k < 4; ++k) bytes[static_cast<std::size_t>(k)] = static_cast<std::byte>(total >> (24 - 8 * k));
    return bytes;
  }

 private:
  /// Samples of an 8x8 block (edge replicated), minus 512.
  static void gather(const VideoFrame& f, std::size_t plane, std::uint32_t x0, std::uint32_t y0, std::int32_t* out) {
    const std::uint32_t pw = ir::plane_width(f.format, plane, f.width);
    const std::uint32_t ph = ir::plane_height(f.format, plane, f.height);
    const auto& p = f.planes[plane];
    for (std::uint32_t r = 0; r < 8; ++r) {
      const std::uint32_t row = std::min(y0 + r, ph - 1);
      for (std::uint32_t c = 0; c < 8; ++c) {
        const std::uint32_t col = std::min(x0 + c, pw - 1);
        const std::uint8_t* s = p.data.data() + static_cast<std::size_t>(row) * p.stride + col * 2;
        out[r * 8 + c] = static_cast<std::int32_t>((s[0] | s[1] << 8) & 0x3FF) - 512;
      }
    }
  }

  std::vector<std::uint8_t> encode_component(const VideoFrame& f, std::size_t plane, std::uint32_t mx, std::uint32_t my, std::uint32_t count,
                                             bool luma) const {
    const bool four = luma || full_chroma_;
    const int per_mb = four ? 4 : 2;
    const int blocks_n = static_cast<int>(count) * per_mb;
    const auto& matrix = luma ? luma_q_ : chroma_q_;
    std::vector<std::int32_t> levels(static_cast<std::size_t>(blocks_n) * 64, 0);
    std::array<std::int32_t, 64> samples{};
    std::array<std::int32_t, 64> coeff{};
    const std::uint32_t mb_w = luma || full_chroma_ ? 16 : 8;
    for (std::uint32_t m = 0; m < count; ++m) {
      for (int b = 0; b < per_mb; ++b) {
        std::uint32_t bx;
        std::uint32_t by;
        if (luma) {
          bx = static_cast<std::uint32_t>(b & 1) * 8;
          by = static_cast<std::uint32_t>(b >> 1) * 8;
        } else if (full_chroma_) {
          bx = static_cast<std::uint32_t>(b >> 1) * 8;
          by = static_cast<std::uint32_t>(b & 1) * 8;
        } else {
          bx = 0;
          by = static_cast<std::uint32_t>(b) * 8;
        }
        gather(f, plane, (mx + m) * mb_w + bx, my * 16 + by, samples.data());
        fdct8x8(samples.data(), coeff.data());
        std::int32_t* lv = levels.data() + (static_cast<std::size_t>(m) * static_cast<std::size_t>(per_mb) + static_cast<std::size_t>(b)) * 64;
        for (int i = 0; i < 64; ++i) {
          // level = F * 4 / (matrix * q), rounded, with a small dead zone for AC.
          const std::int64_t step = static_cast<std::int64_t>(matrix[static_cast<std::size_t>(i)]) * qscale_;
          const std::int64_t num = static_cast<std::int64_t>(coeff[static_cast<std::size_t>(i)]) * 4;
          const std::int64_t mag = (std::llabs(num) * 2 + (i == 0 ? step : step * 2 / 3)) / (2 * step);
          lv[i] = static_cast<std::int32_t>(num < 0 ? -mag : mag);
        }
      }
    }
    BitWriter w;
    // DC coefficients.
    int prev = levels[0];
    write_codeword(w, kFirstDcCodebook, from_signed(prev));
    int sign = 0;
    std::uint32_t codebook = 5;
    for (int b = 1; b < blocks_n; ++b) {
      const int dc = levels[static_cast<std::size_t>(b) * 64];
      int delta = dc - prev;
      const int new_sign = delta < 0 ? -1 : 0;
      delta = (delta ^ sign) - sign;
      const std::uint32_t code = from_signed(delta);
      write_codeword(w, kDcCodebook[std::min<std::uint32_t>(codebook, 6)], code);
      codebook = code;
      sign = new_sign;
      prev = dc;
    }
    // AC coefficients, interleaved over the blocks in scan order.
    std::uint32_t run = 0;
    std::uint32_t run_cb = 4;
    std::uint32_t level_cb = 2;
    for (int i = 1; i < 64; ++i) {
      for (int b = 0; b < blocks_n; ++b) {
        const int value = levels[static_cast<std::size_t>(b) * 64 + kProgressiveScan[i]];
        if (value == 0) {
          ++run;
          continue;
        }
        const std::uint32_t magnitude = static_cast<std::uint32_t>(value < 0 ? -value : value);
        write_codeword(w, kRunCodebook[std::min<std::uint32_t>(run_cb, 15)], run);
        write_codeword(w, kLevelCodebook[std::min<std::uint32_t>(level_cb, 9)], magnitude - 1);
        w.put(1, value < 0 ? 1 : 0);
        run_cb = run;
        level_cb = magnitude;
        run = 0;
      }
    }
    return w.take();
  }

  std::vector<std::uint8_t> encode_alpha(const std::vector<std::uint8_t>& alpha, std::uint32_t mx, std::uint32_t my, std::uint32_t count) const {
    BitWriter w;
    const std::uint32_t width = count * 16;
    std::vector<std::uint32_t> values;
    values.reserve(static_cast<std::size_t>(width) * 16);
    for (std::uint32_t r = 0; r < 16; ++r) {
      const std::uint32_t row = std::min(my * 16 + r, params_.height - 1);
      for (std::uint32_t c = 0; c < width; ++c) {
        const std::uint32_t col = std::min(mx * 16 + c, params_.width - 1);
        values.push_back(alpha[static_cast<std::size_t>(row) * params_.width + col]);
      }
    }
    // Mirror of the decoder's loop: groups of differences (each followed by a
    // "more" flag unless it is the last sample), then a run of repeats.
    std::uint32_t current = 255;
    std::size_t i = 0;
    while (i < values.size()) {
      while (true) {
        const std::uint32_t diff = (values[i] - current) & 0xFF;
        const int small = diff > 127 ? static_cast<int>(diff) - 256 : static_cast<int>(diff);
        if (small != 0 && small >= -8 && small <= 8) {
          w.put(1, 0);
          w.put(4, static_cast<std::uint32_t>(small > 0 ? 2 * small - 2 : -2 * small - 1));
        } else {
          w.put(1, 1);
          w.put(8, diff);
        }
        current = values[i++];
        if (i >= values.size()) break;
        if (values[i] != current) {
          w.put(1, 1);
          continue;
        }
        w.put(1, 0);
        break;
      }
      std::uint32_t run = 0;
      while (i + run < values.size() && values[i + run] == current && run < 2047) ++run;
      if (run > 0 && run < 16) {
        w.put(4, run);
      } else {
        w.put(4, 0);
        w.put(11, run);
      }
      i += run;
    }
    return w.take();
  }

  std::vector<std::uint8_t> encode_slice(const VideoFrame& f, const std::vector<std::uint8_t>& alpha, std::uint32_t mx, std::uint32_t my,
                                         std::uint32_t count) const {
    const std::vector<std::uint8_t> y = encode_component(f, 0, mx, my, count, true);
    const std::vector<std::uint8_t> u = encode_component(f, 1, mx, my, count, false);
    const std::vector<std::uint8_t> v = encode_component(f, 2, mx, my, count, false);
    std::vector<std::uint8_t> out;
    const int header = alpha_ ? 8 : 6;
    out.push_back(static_cast<std::uint8_t>(header << 3));
    out.push_back(static_cast<std::uint8_t>(qscale_));
    out.push_back(static_cast<std::uint8_t>(y.size() >> 8));
    out.push_back(static_cast<std::uint8_t>(y.size() & 0xFF));
    out.push_back(static_cast<std::uint8_t>(u.size() >> 8));
    out.push_back(static_cast<std::uint8_t>(u.size() & 0xFF));
    if (alpha_) {
      out.push_back(static_cast<std::uint8_t>(v.size() >> 8));
      out.push_back(static_cast<std::uint8_t>(v.size() & 0xFF));
    }
    out.insert(out.end(), y.begin(), y.end());
    out.insert(out.end(), u.begin(), u.end());
    out.insert(out.end(), v.begin(), v.end());
    if (alpha_) {
      const std::vector<std::uint8_t> a = encode_alpha(alpha, mx, my, count);
      out.insert(out.end(), a.begin(), a.end());
    }
    return out;
  }

  bool alpha_ = false;
  bool full_chroma_ = false;
  int qscale_ = 4;
  std::array<std::uint8_t, 64> luma_q_{};
  std::array<std::uint8_t, 64> chroma_q_{};
};

}  // namespace

std::unique_ptr<codec::VideoDecoder> make_prores_decoder() { return std::make_unique<ProresDecoder>(); }
std::unique_ptr<codec::VideoEncoder> make_prores_encoder() { return std::make_unique<ProresEncoder>(); }

}  // namespace cvt::legacy::pro
