// Convertible core - image family: JPEG decoder (ITU-T T.81).
//
// Baseline and extended sequential (8 and 12 bit), progressive, Huffman and
// arithmetic entropy coding, restart intervals, any sampling factors 1..4,
// greyscale, YCbCr, RGB, CMYK and YCCK. Chroma is upsampled with linear
// interpolation at centred sample positions (libjpeg's "fancy" upsampling
// for 2:1 ratios). Damaged entropy data is decoded as far as possible with a
// warning, like libjpeg does.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "families/image/common.hpp"
#include "families/image/image_ops.hpp"
#include "families/image/jpeg_codec.hpp"
#include "util/text.hpp"

namespace cvt::image::jpeg {

namespace {

// --------------------------------------------------------------------------- Huffman tables

struct HuffTable {
  bool defined = false;
  // 9-bit lookahead: (length << 8) | symbol, 0 when the code is longer.
  std::array<std::uint16_t, 512> lookup{};
  std::array<std::int32_t, 18> maxcode{};
  std::array<std::int32_t, 17> valoffset{};
  std::array<std::uint8_t, 256> symbols{};
};

Result<void> build_table(HuffTable& table, const std::array<std::uint8_t, 16>& counts,
                         std::span<const std::uint8_t> symbols) {
  std::size_t total = 0;
  for (const std::uint8_t c : counts) total += c;
  if (total > 256 || total > symbols.size()) return fail(Status::Decode, "JPEG Huffman table is invalid");
  table = HuffTable{};
  std::copy(symbols.begin(), symbols.begin() + static_cast<std::ptrdiff_t>(total), table.symbols.begin());
  std::uint32_t code = 0;
  std::size_t k = 0;
  for (int length = 1; length <= 16; ++length) {
    const std::uint32_t count = counts[static_cast<std::size_t>(length - 1)];
    table.valoffset[static_cast<std::size_t>(length)] = static_cast<std::int32_t>(k) - static_cast<std::int32_t>(code);
    for (std::uint32_t i = 0; i < count; ++i, ++k, ++code) {
      if (length <= 9) {
        const std::uint32_t shift = 9u - static_cast<std::uint32_t>(length);
        for (std::uint32_t fill = 0; fill < (1u << shift); ++fill) {
          table.lookup[(code << shift) | fill] =
              static_cast<std::uint16_t>((length << 8) | table.symbols[k]);
        }
      }
    }
    table.maxcode[static_cast<std::size_t>(length)] = count > 0 ? static_cast<std::int32_t>(code) - 1 : -1;
    if (code > (1u << length)) return fail(Status::Decode, "JPEG Huffman table is invalid");
    code <<= 1;
  }
  table.maxcode[17] = 0x7FFFFFFF;
  table.defined = true;
  return {};
}

// --------------------------------------------------------------------------- bit reader

class BitReader {
 public:
  BitReader() = default;
  BitReader(const std::uint8_t* data, std::size_t size, std::size_t position)
      : data_(data), size_(size), pos_(position) {}

  void fill() noexcept {
    while (bits_ <= 56) {
      std::uint32_t byte = 0;
      if (!marker_ && pos_ < size_) {
        byte = data_[pos_];
        if (byte == 0xFF) {
          const std::uint32_t next = pos_ + 1 < size_ ? data_[pos_ + 1] : 0xD9;
          if (next == 0) {
            pos_ += 2;
          } else if (next == 0xFF) {
            ++pos_;  // fill byte before a marker
            continue;
          } else {
            marker_ = true;
            byte = 0;
            ++padding_;
          }
        } else {
          ++pos_;
        }
      } else {
        ++padding_;
      }
      buffer_ |= static_cast<std::uint64_t>(byte) << (56 - bits_);
      bits_ += 8;
    }
  }
  [[nodiscard]] std::uint32_t peek(int count) noexcept {
    if (bits_ < count) fill();
    return static_cast<std::uint32_t>(buffer_ >> (64 - count));
  }
  void skip(int count) noexcept {
    buffer_ <<= count;
    bits_ -= count;
  }
  [[nodiscard]] std::uint32_t get(int count) noexcept {
    if (count == 0) return 0;
    const std::uint32_t value = peek(count);
    skip(count);
    return value;
  }
  [[nodiscard]] int decode(const HuffTable& table) noexcept {
    if (bits_ < 16) fill();
    const std::uint16_t entry = table.lookup[buffer_ >> 55];
    if (entry != 0) {
      skip(entry >> 8);
      return entry & 0xFF;
    }
    for (int length = 10; length <= 16; ++length) {
      const auto code = static_cast<std::int32_t>(buffer_ >> (64 - length));
      if (code <= table.maxcode[static_cast<std::size_t>(length)]) {
        skip(length);
        const std::int32_t index = table.valoffset[static_cast<std::size_t>(length)] + code;
        if (index < 0 || index > 255) break;
        return table.symbols[static_cast<std::size_t>(index)];
      }
    }
    corrupt_ = true;
    skip(16);
    return 0;
  }
  /// Read `s` bits and sign-extend per F.2.2.1.
  [[nodiscard]] int receive_extend(int s) noexcept {
    if (s <= 0) return 0;
    if (s > 16) {
      corrupt_ = true;
      return 0;
    }
    const auto v = static_cast<int>(get(s));
    return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v;
  }
  /// Discard buffered bits and skip to just after the next RSTn marker
  /// (resynchronising on damaged data). Returns false at the end of data.
  bool restart() noexcept {
    buffer_ = 0;
    bits_ = 0;
    marker_ = false;
    while (pos_ + 1 < size_) {
      if (data_[pos_] == 0xFF && data_[pos_ + 1] >= 0xD0 && data_[pos_ + 1] <= 0xD7) {
        pos_ += 2;
        return true;
      }
      if (data_[pos_] == 0xFF && data_[pos_ + 1] != 0 && data_[pos_ + 1] != 0xFF) {
        corrupt_ = true;  // another marker: the restart marker is missing
        return false;
      }
      ++pos_;
      corrupt_ = true;
    }
    return false;
  }
  /// Position after the scan's entropy data (at the next marker).
  [[nodiscard]] std::size_t end_position() const noexcept {
    std::size_t p = pos_;
    while (p + 1 < size_ && !(data_[p] == 0xFF && data_[p + 1] != 0 && !(data_[p + 1] >= 0xD0 && data_[p + 1] <= 0xD7))) {
      ++p;
    }
    return std::min(p, size_);
  }
  [[nodiscard]] bool corrupt() const noexcept { return corrupt_; }
  [[nodiscard]] bool overrun() const noexcept { return padding_ > 16; }
  void mark_corrupt() noexcept { corrupt_ = true; }

 private:
  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
  std::size_t pos_ = 0;
  std::uint64_t buffer_ = 0;
  int bits_ = 0;
  bool marker_ = false;
  bool corrupt_ = false;
  std::uint32_t padding_ = 0;
};

// --------------------------------------------------------------------------- arithmetic decoder (Annex D)

// (Qe << 16) | (Next_Index_MPS << 8) | (Switch_MPS << 7) | Next_Index_LPS
constexpr std::uint32_t qm(std::uint32_t qe, std::uint32_t nlps, std::uint32_t nmps, std::uint32_t sw) {
  return qe << 16 | nmps << 8 | sw << 7 | nlps;
}
constexpr std::array<std::uint32_t, 114> qm_table = {
    qm(0x5a1d, 1, 1, 1),     qm(0x2586, 14, 2, 0),    qm(0x1114, 16, 3, 0),    qm(0x080b, 18, 4, 0),
    qm(0x03d8, 20, 5, 0),    qm(0x01da, 23, 6, 0),    qm(0x00e5, 25, 7, 0),    qm(0x006f, 28, 8, 0),
    qm(0x0036, 30, 9, 0),    qm(0x001a, 33, 10, 0),   qm(0x000d, 35, 11, 0),   qm(0x0006, 9, 12, 0),
    qm(0x0003, 10, 13, 0),   qm(0x0001, 12, 13, 0),   qm(0x5a7f, 15, 15, 1),   qm(0x3f25, 36, 16, 0),
    qm(0x2cf2, 38, 17, 0),   qm(0x207c, 39, 18, 0),   qm(0x17b9, 40, 19, 0),   qm(0x1182, 42, 20, 0),
    qm(0x0cef, 43, 21, 0),   qm(0x09a1, 45, 22, 0),   qm(0x072f, 46, 23, 0),   qm(0x055c, 48, 24, 0),
    qm(0x0406, 49, 25, 0),   qm(0x0303, 51, 26, 0),   qm(0x0240, 52, 27, 0),   qm(0x01b1, 54, 28, 0),
    qm(0x0144, 56, 29, 0),   qm(0x00f5, 57, 30, 0),   qm(0x00b7, 59, 31, 0),   qm(0x008a, 60, 32, 0),
    qm(0x0068, 62, 33, 0),   qm(0x004e, 63, 34, 0),   qm(0x003b, 32, 35, 0),   qm(0x002c, 33, 9, 0),
    qm(0x5ae1, 37, 37, 1),   qm(0x484c, 64, 38, 0),   qm(0x3a0d, 65, 39, 0),   qm(0x2ef1, 67, 40, 0),
    qm(0x261f, 68, 41, 0),   qm(0x1f33, 69, 42, 0),   qm(0x19a8, 70, 43, 0),   qm(0x1518, 72, 44, 0),
    qm(0x1177, 73, 45, 0),   qm(0x0e74, 74, 46, 0),   qm(0x0bfb, 75, 47, 0),   qm(0x09f8, 77, 48, 0),
    qm(0x0861, 78, 49, 0),   qm(0x0706, 79, 50, 0),   qm(0x05cd, 48, 51, 0),   qm(0x04de, 50, 52, 0),
    qm(0x040f, 50, 53, 0),   qm(0x0363, 51, 54, 0),   qm(0x02d4, 52, 55, 0),   qm(0x025c, 53, 56, 0),
    qm(0x01f8, 54, 57, 0),   qm(0x01a4, 55, 58, 0),   qm(0x0160, 56, 59, 0),   qm(0x0125, 57, 60, 0),
    qm(0x00f6, 58, 61, 0),   qm(0x00cb, 59, 62, 0),   qm(0x00ab, 61, 63, 0),   qm(0x008f, 61, 32, 0),
    qm(0x5b12, 65, 65, 1),   qm(0x4d04, 80, 66, 0),   qm(0x412c, 81, 67, 0),   qm(0x37d8, 82, 68, 0),
    qm(0x2fe8, 83, 69, 0),   qm(0x293c, 84, 70, 0),   qm(0x2379, 86, 71, 0),   qm(0x1edf, 87, 72, 0),
    qm(0x1aa9, 87, 73, 0),   qm(0x174e, 72, 74, 0),   qm(0x1424, 72, 75, 0),   qm(0x119c, 74, 76, 0),
    qm(0x0f6b, 74, 77, 0),   qm(0x0d51, 75, 78, 0),   qm(0x0bb6, 77, 79, 0),   qm(0x0a40, 77, 48, 0),
    qm(0x5832, 80, 81, 1),   qm(0x4d1c, 88, 82, 0),   qm(0x438e, 89, 83, 0),   qm(0x3bdd, 90, 84, 0),
    qm(0x34ee, 91, 85, 0),   qm(0x2eae, 92, 86, 0),   qm(0x299a, 93, 87, 0),   qm(0x2516, 86, 71, 0),
    qm(0x5570, 88, 89, 1),   qm(0x4ca9, 95, 90, 0),   qm(0x44d9, 96, 91, 0),   qm(0x3e22, 97, 92, 0),
    qm(0x3824, 99, 93, 0),   qm(0x32b4, 99, 94, 0),   qm(0x2e17, 93, 86, 0),   qm(0x56a8, 95, 96, 1),
    qm(0x4f46, 101, 97, 0),  qm(0x47e5, 102, 98, 0),  qm(0x41cf, 103, 99, 0),  qm(0x3c3d, 104, 100, 0),
    qm(0x375e, 99, 93, 0),   qm(0x5231, 105, 102, 0), qm(0x4c0f, 106, 103, 0), qm(0x4639, 107, 104, 0),
    qm(0x415e, 103, 99, 0),  qm(0x5627, 105, 106, 1), qm(0x50e7, 108, 107, 0), qm(0x4b85, 109, 103, 0),
    qm(0x5597, 110, 109, 0), qm(0x504f, 111, 107, 0), qm(0x5a10, 110, 111, 1), qm(0x5522, 112, 109, 0),
    qm(0x59eb, 112, 111, 1), qm(0x5a1d, 113, 113, 0),
};

class ArithDecoder {
 public:
  ArithDecoder() = default;
  ArithDecoder(const std::uint8_t* data, std::size_t size, std::size_t position)
      : data_(data), size_(size), pos_(position) {}

  void reset() noexcept {
    c_ = 0;
    a_ = 0;
    ct_ = -16;
  }
  int decode(std::uint8_t& state) noexcept {
    while (a_ < 0x8000u) {
      if (--ct_ < 0) {
        std::uint32_t data = 0;
        if (!marker_ && pos_ < size_) {
          data = data_[pos_++];
          if (data == 0xFF) {
            while (pos_ < size_ && data_[pos_] == 0xFF) ++pos_;
            const std::uint32_t next = pos_ < size_ ? data_[pos_] : 0xD9;
            if (next == 0) {
              ++pos_;
              data = 0xFF;
            } else {
              marker_ = true;
              --pos_;  // leave the 0xFF so the marker can be found
              data = 0;
            }
          }
        } else {
          ++padding_;
        }
        c_ = (c_ << 8) | data;
        ct_ += 8;
        if (ct_ < 0) {
          if (++ct_ == 0) a_ = 0x8000u;
        }
      }
      a_ <<= 1;
    }
    std::uint32_t sv = state;
    std::uint32_t qe = qm_table[sv & 0x7F];
    const std::uint32_t nl = qe & 0xFF;
    qe >>= 8;
    const std::uint32_t nm = qe & 0xFF;
    qe >>= 8;
    std::uint32_t temp = a_ - qe;
    a_ = temp;
    temp <<= ct_;
    if (c_ >= temp) {
      c_ -= temp;
      if (a_ < qe) {
        a_ = qe;
        state = static_cast<std::uint8_t>((sv & 0x80) ^ nm);
      } else {
        a_ = qe;
        state = static_cast<std::uint8_t>((sv & 0x80) ^ nl);
        sv ^= 0x80;
      }
    } else if (a_ < 0x8000u) {
      if (a_ < qe) {
        state = static_cast<std::uint8_t>((sv & 0x80) ^ nl);
        sv ^= 0x80;
      } else {
        state = static_cast<std::uint8_t>((sv & 0x80) ^ nm);
      }
    }
    return static_cast<int>(sv >> 7);
  }
  bool restart() noexcept {
    marker_ = false;
    while (pos_ + 1 < size_) {
      if (data_[pos_] == 0xFF && data_[pos_ + 1] >= 0xD0 && data_[pos_ + 1] <= 0xD7) {
        pos_ += 2;
        reset();
        return true;
      }
      if (data_[pos_] == 0xFF && data_[pos_ + 1] != 0 && data_[pos_ + 1] != 0xFF) return false;
      ++pos_;
    }
    return false;
  }
  [[nodiscard]] std::size_t position() const noexcept { return pos_; }
  [[nodiscard]] bool overrun() const noexcept { return padding_ > 8; }

 private:
  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
  std::size_t pos_ = 0;
  std::uint32_t c_ = 0;
  std::uint32_t a_ = 0;
  int ct_ = -16;
  bool marker_ = false;
  std::uint32_t padding_ = 0;
};

// --------------------------------------------------------------------------- IDCT

constexpr std::array<double, 8> aan_scale = {1.0,         1.387039845, 1.306562965, 1.175875602,
                                             1.0,         0.785694958, 0.541196100, 0.275899379};

/// Float AAN inverse DCT (as libjpeg's jidctflt). `q` holds the
/// quantisation table pre-multiplied by the AAN scale factors / 8.
/// Writes 8x8 samples (centre `center`, clamped to 0..max) with `stride`.
template <class Sample>
void idct_block(const std::int16_t* in, const float* q, Sample* out, std::size_t stride, float center, float max) {
  float ws[64];
  for (int c = 0; c < 8; ++c) {
    const std::int16_t* col = in + c;
    if (col[8] == 0 && col[16] == 0 && col[24] == 0 && col[32] == 0 && col[40] == 0 && col[48] == 0 && col[56] == 0) {
      const float dc = col[0] * q[c];
      for (int r = 0; r < 8; ++r) ws[r * 8 + c] = dc;
      continue;
    }
    float tmp0 = col[0] * q[c];
    float tmp1 = col[16] * q[16 + c];
    float tmp2 = col[32] * q[32 + c];
    float tmp3 = col[48] * q[48 + c];
    float tmp10 = tmp0 + tmp2;
    float tmp11 = tmp0 - tmp2;
    float tmp13 = tmp1 + tmp3;
    float tmp12 = (tmp1 - tmp3) * 1.414213562f - tmp13;
    tmp0 = tmp10 + tmp13;
    tmp3 = tmp10 - tmp13;
    tmp1 = tmp11 + tmp12;
    tmp2 = tmp11 - tmp12;
    float tmp4 = col[8] * q[8 + c];
    float tmp5 = col[24] * q[24 + c];
    float tmp6 = col[40] * q[40 + c];
    float tmp7 = col[56] * q[56 + c];
    const float z13 = tmp6 + tmp5;
    const float z10 = tmp6 - tmp5;
    const float z11 = tmp4 + tmp7;
    const float z12 = tmp4 - tmp7;
    tmp7 = z11 + z13;
    tmp11 = (z11 - z13) * 1.414213562f;
    const float z5 = (z10 + z12) * 1.847759065f;
    tmp10 = 1.082392200f * z12 - z5;
    tmp12 = -2.613125930f * z10 + z5;
    tmp6 = tmp12 - tmp7;
    tmp5 = tmp11 - tmp6;
    tmp4 = tmp10 + tmp5;
    ws[0 * 8 + c] = tmp0 + tmp7;
    ws[7 * 8 + c] = tmp0 - tmp7;
    ws[1 * 8 + c] = tmp1 + tmp6;
    ws[6 * 8 + c] = tmp1 - tmp6;
    ws[2 * 8 + c] = tmp2 + tmp5;
    ws[5 * 8 + c] = tmp2 - tmp5;
    ws[4 * 8 + c] = tmp3 + tmp4;
    ws[3 * 8 + c] = tmp3 - tmp4;
  }
  const auto put = [&](float v) {
    v = v * 0.125f + center;
    if (!(v > 0.0f)) return Sample{0};
    if (v >= max) return static_cast<Sample>(max);
    return static_cast<Sample>(v + 0.5f);
  };
  for (int r = 0; r < 8; ++r) {
    const float* row = ws + r * 8;
    Sample* o = out + static_cast<std::size_t>(r) * stride;
    float tmp10 = row[0] + row[4];
    float tmp11 = row[0] - row[4];
    float tmp13 = row[2] + row[6];
    float tmp12 = (row[2] - row[6]) * 1.414213562f - tmp13;
    const float tmp0 = tmp10 + tmp13;
    const float tmp3 = tmp10 - tmp13;
    const float tmp1 = tmp11 + tmp12;
    const float tmp2 = tmp11 - tmp12;
    const float z13 = row[5] + row[3];
    const float z10 = row[5] - row[3];
    const float z11 = row[1] + row[7];
    const float z12 = row[1] - row[7];
    const float tmp7 = z11 + z13;
    tmp11 = (z11 - z13) * 1.414213562f;
    const float z5 = (z10 + z12) * 1.847759065f;
    tmp10 = 1.082392200f * z12 - z5;
    tmp12 = -2.613125930f * z10 + z5;
    const float tmp6 = tmp12 - tmp7;
    const float tmp5 = tmp11 - tmp6;
    const float tmp4 = tmp10 + tmp5;
    o[0] = put(tmp0 + tmp7);
    o[7] = put(tmp0 - tmp7);
    o[1] = put(tmp1 + tmp6);
    o[6] = put(tmp1 - tmp6);
    o[2] = put(tmp2 + tmp5);
    o[5] = put(tmp2 - tmp5);
    o[4] = put(tmp3 + tmp4);
    o[3] = put(tmp3 - tmp4);
  }
}

// --------------------------------------------------------------------------- decoder state

struct Component {
  int id = 0;
  int h = 1;
  int v = 1;
  int tq = 0;
  std::uint32_t width = 0;        // sampled size
  std::uint32_t height = 0;
  std::uint32_t blocks_w = 0;     // MCU-padded block grid
  std::uint32_t blocks_h = 0;
  std::uint32_t real_blocks_w = 0;  // blocks that hold image data
  std::uint32_t real_blocks_h = 0;
  std::vector<std::int16_t> coefs;  // progressive / arithmetic: blocks_w * blocks_h * 64
  std::vector<std::uint8_t> plane;  // 8-bit: blocks_w*8 x blocks_h*8
  std::vector<std::uint16_t> plane12;
  int dc_table = 0;
  int ac_table = 0;
  int dc_pred = 0;
  // arithmetic
  int dc_context = 0;
};

struct Decoder {
  const Context& ctx;
  const std::uint8_t* data = nullptr;
  std::size_t size = 0;
  std::array<std::array<std::uint16_t, 64>, 4> quant{};
  std::array<bool, 4> quant_defined{};
  std::array<HuffTable, 4> dc_tables;
  std::array<HuffTable, 4> ac_tables;
  // arithmetic conditioning
  std::array<int, 4> arith_dc_l = {0, 0, 0, 0};
  std::array<int, 4> arith_dc_u = {1, 1, 1, 1};
  std::array<int, 4> arith_ac_k = {5, 5, 5, 5};
  std::uint32_t restart_interval = 0;
  bool frame_seen = false;
  bool progressive = false;
  bool arithmetic = false;
  bool stored_coefs = false;  // coefficients kept until the end
  int precision = 8;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  int max_h = 1;
  int max_v = 1;
  std::uint32_t mcus_x = 0;
  std::uint32_t mcus_y = 0;
  std::vector<Component> components;
  bool adobe = false;
  int adobe_transform = -1;
  bool jfif = false;
  std::vector<std::pair<int, Bytes>> icc_chunks;
  Extras extras;
  bool warned_corrupt = false;
  bool any_scan = false;
  bool eob_reached = false;
  bool default_huffman = true;

  explicit Decoder(const Context& c) : ctx(c) {}

  void warn_corrupt() {
    if (!warned_corrupt) ctx.warn("The JPEG data is damaged; parts of the picture may be wrong.");
    warned_corrupt = true;
  }
};

Result<void> read_dqt(Decoder& d, util::ByteReader seg) {
  while (seg.remaining() > 0) {
    const std::uint8_t pq_tq = seg.u8();
    const int pq = pq_tq >> 4;
    const int tq = pq_tq & 15;
    if (tq > 3 || pq > 1) return fail(Status::Decode, "JPEG quantisation table is invalid");
    for (std::size_t k = 0; k < 64; ++k) {
      const std::uint16_t value = pq ? seg.u16be() : seg.u8();
      d.quant[static_cast<std::size_t>(tq)][zigzag[k]] = value;
    }
    if (!seg.ok()) return fail(Status::Decode, "JPEG quantisation table is truncated");
    d.quant_defined[static_cast<std::size_t>(tq)] = true;
  }
  return {};
}

Result<void> define_huffman(Decoder& d, int tc, int th, const std::array<std::uint8_t, 16>& counts,
                            std::span<const std::uint8_t> symbols) {
  if (tc > 1 || th > 3) return fail(Status::Decode, "JPEG Huffman table is invalid");
  HuffTable& table = tc == 0 ? d.dc_tables[static_cast<std::size_t>(th)] : d.ac_tables[static_cast<std::size_t>(th)];
  return build_table(table, counts, symbols);
}

Result<void> read_dht(Decoder& d, util::ByteReader seg) {
  while (seg.remaining() > 0) {
    const std::uint8_t tc_th = seg.u8();
    std::array<std::uint8_t, 16> counts{};
    std::size_t total = 0;
    for (auto& c : counts) {
      c = seg.u8();
      total += c;
    }
    if (!seg.ok() || total > 256) return fail(Status::Decode, "JPEG Huffman table is invalid");
    const std::span<const std::uint8_t> symbols = as_u8(seg.bytes(total));
    if (!seg.ok()) return fail(Status::Decode, "JPEG Huffman table is truncated");
    CVT_TRY(define_huffman(d, tc_th >> 4, tc_th & 15, counts, symbols));
  }
  return {};
}

Result<void> read_dac(Decoder& d, util::ByteReader seg) {
  while (seg.remaining() >= 2) {
    const std::uint8_t tc_tb = seg.u8();
    const std::uint8_t value = seg.u8();
    const int tc = tc_tb >> 4;
    const int tb = tc_tb & 15;
    if (tb > 3) return fail(Status::Decode, "JPEG arithmetic conditioning is invalid");
    if (tc == 0) {
      d.arith_dc_l[static_cast<std::size_t>(tb)] = value & 15;
      d.arith_dc_u[static_cast<std::size_t>(tb)] = value >> 4;
      if (d.arith_dc_l[static_cast<std::size_t>(tb)] > d.arith_dc_u[static_cast<std::size_t>(tb)]) {
        return fail(Status::Decode, "JPEG arithmetic conditioning is invalid");
      }
    } else {
      if (value < 1 || value > 63) return fail(Status::Decode, "JPEG arithmetic conditioning is invalid");
      d.arith_ac_k[static_cast<std::size_t>(tb)] = value;
    }
  }
  return {};
}

Result<void> read_sof(Decoder& d, util::ByteReader seg, int marker) {
  if (d.frame_seen) return fail(Status::Decode, "JPEG file has more than one frame header");
  d.frame_seen = true;
  d.progressive = marker == 0xC2 || marker == 0xCA;
  d.arithmetic = marker >= 0xC9;
  d.precision = seg.u8();
  d.height = seg.u16be();
  d.width = seg.u16be();
  const int count = seg.u8();
  if (!seg.ok()) return fail(Status::Decode, "JPEG frame header is truncated");
  if (d.precision != 8 && d.precision != 12) {
    return fail(Status::Decode, "{}-bit JPEG files are not supported", d.precision);
  }
  if (d.height == 0) return fail(Status::Decode, "JPEG files that define their height later (DNL) are not supported");
  if (d.width == 0) return fail(Status::Decode, "JPEG image has no pixels");
  if (count != 1 && count != 3 && count != 4) {
    return fail(Status::Decode, "JPEG files with {} components are not supported", count);
  }
  CVT_TRY(check_size(d.ctx, d.width, d.height));
  for (int i = 0; i < count; ++i) {
    Component c;
    c.id = seg.u8();
    const std::uint8_t hv = seg.u8();
    c.h = hv >> 4;
    c.v = hv & 15;
    c.tq = seg.u8();
    if (!seg.ok()) return fail(Status::Decode, "JPEG frame header is truncated");
    if (c.h < 1 || c.h > 4 || c.v < 1 || c.v > 4 || c.tq > 3) {
      return fail(Status::Decode, "JPEG component has invalid sampling factors");
    }
    for (const Component& other : d.components) {
      if (other.id == c.id) return fail(Status::Decode, "JPEG component ids are not unique");
    }
    d.max_h = std::max(d.max_h, c.h);
    d.max_v = std::max(d.max_v, c.v);
    d.components.push_back(std::move(c));
  }
  d.mcus_x = (d.width + 8u * static_cast<std::uint32_t>(d.max_h) - 1) / (8u * static_cast<std::uint32_t>(d.max_h));
  d.mcus_y = (d.height + 8u * static_cast<std::uint32_t>(d.max_v) - 1) / (8u * static_cast<std::uint32_t>(d.max_v));
  d.stored_coefs = d.progressive || d.arithmetic;
  std::uint64_t total_samples = 0;
  for (Component& c : d.components) {
    c.width = static_cast<std::uint32_t>((std::uint64_t{d.width} * static_cast<std::uint32_t>(c.h) +
                                          static_cast<std::uint32_t>(d.max_h) - 1) /
                                         static_cast<std::uint32_t>(d.max_h));
    c.height = static_cast<std::uint32_t>((std::uint64_t{d.height} * static_cast<std::uint32_t>(c.v) +
                                           static_cast<std::uint32_t>(d.max_v) - 1) /
                                          static_cast<std::uint32_t>(d.max_v));
    c.blocks_w = d.mcus_x * static_cast<std::uint32_t>(c.h);
    c.blocks_h = d.mcus_y * static_cast<std::uint32_t>(c.v);
    c.real_blocks_w = (c.width + 7) / 8;
    c.real_blocks_h = (c.height + 7) / 8;
    total_samples += std::uint64_t{c.blocks_w} * c.blocks_h * 64;
  }
  // Planes (+ coefficient buffers) are bounded by the pixel limit times the component count.
  CVT_TRY(d.ctx.limits.check_pixels(total_samples, 1));
  // Huffman coding needs at least one bit per block; arithmetic coding can
  // go far below that, but not below a few hundredths of a bit.
  CVT_TRY(check_plausible(total_samples, d.size, d.arithmetic ? 64u * 512u : 64u * 8u));
  if (total_samples * (d.stored_coefs ? 4 : 2) > static_cast<std::uint64_t>(SIZE_MAX / 2)) {
    return fail(Status::Limit, "JPEG image is too large for this device");
  }
  for (Component& c : d.components) {
    const std::size_t samples = static_cast<std::size_t>(c.blocks_w) * c.blocks_h * 64;
    if (d.precision == 8) {
      c.plane.assign(samples, 128);
    } else {
      c.plane12.assign(samples, 2048);
    }
    if (d.stored_coefs) c.coefs.assign(samples, 0);
  }
  return {};
}

/// Dequantisation table pre-scaled for the float IDCT.
std::array<float, 64> idct_table(const std::array<std::uint16_t, 64>& q) {
  std::array<float, 64> out{};
  for (std::size_t r = 0; r < 8; ++r) {
    for (std::size_t c = 0; c < 8; ++c) {
      out[r * 8 + c] = static_cast<float>(q[r * 8 + c] * aan_scale[r] * aan_scale[c]);
    }
  }
  return out;
}

void idct_into_plane(Decoder& d, Component& c, const std::int16_t* coefs, const std::array<float, 64>& table,
                     std::uint32_t bx, std::uint32_t by) {
  const std::size_t stride = static_cast<std::size_t>(c.blocks_w) * 8;
  const std::size_t offset = static_cast<std::size_t>(by) * 8 * stride + static_cast<std::size_t>(bx) * 8;
  if (d.precision == 8) {
    idct_block<std::uint8_t>(coefs, table.data(), c.plane.data() + offset, stride, 128.0f, 255.0f);
  } else {
    idct_block<std::uint16_t>(coefs, table.data(), c.plane12.data() + offset, stride, 2048.0f, 4095.0f);
  }
}

inline std::int16_t clamp16(int v) noexcept { return static_cast<std::int16_t>(std::clamp(v, -32768, 32767)); }

struct Scan {
  std::vector<int> comps;  // indices into d.components
  int ss = 0, se = 63, ah = 0, al = 0;
};

// ---- Huffman block decoders ----

void decode_block_sequential(BitReader& bits, Decoder& d, Component& c, std::int16_t* block) {
  const HuffTable& dc = d.dc_tables[static_cast<std::size_t>(c.dc_table)];
  const HuffTable& ac = d.ac_tables[static_cast<std::size_t>(c.ac_table)];
  const int s = bits.decode(dc);
  c.dc_pred += bits.receive_extend(s);
  c.dc_pred = std::clamp(c.dc_pred, -65536, 65535);
  block[0] = clamp16(c.dc_pred);
  for (int k = 1; k < 64;) {
    const int rs = bits.decode(ac);
    const int r = rs >> 4;
    const int size = rs & 15;
    if (size != 0) {
      k += r;
      if (k > 63) {
        bits.mark_corrupt();
        return;
      }
      block[zigzag[static_cast<std::size_t>(k)]] = clamp16(bits.receive_extend(size));
      ++k;
    } else {
      if (r != 15) return;
      k += 16;
    }
  }
}

struct ProgressiveState {
  std::uint32_t eobrun = 0;
};

void decode_dc_first(BitReader& bits, Decoder& d, Component& c, std::int16_t* block, int al) {
  const int s = bits.decode(d.dc_tables[static_cast<std::size_t>(c.dc_table)]);
  c.dc_pred += bits.receive_extend(s);
  c.dc_pred = std::clamp(c.dc_pred, -65536, 65535);
  block[0] = clamp16(c.dc_pred * (1 << al));
}

void decode_dc_refine(BitReader& bits, std::int16_t* block, int al) {
  if (bits.get(1)) block[0] = static_cast<std::int16_t>(block[0] | (1 << al));
}

void decode_ac_first(BitReader& bits, Decoder& d, Component& c, std::int16_t* block, const Scan& scan,
                     ProgressiveState& ps) {
  if (ps.eobrun > 0) {
    --ps.eobrun;
    return;
  }
  const HuffTable& ac = d.ac_tables[static_cast<std::size_t>(c.ac_table)];
  for (int k = scan.ss; k <= scan.se; ++k) {
    const int rs = bits.decode(ac);
    const int r = rs >> 4;
    const int s = rs & 15;
    if (s != 0) {
      k += r;
      if (k > scan.se) {
        bits.mark_corrupt();
        return;
      }
      block[zigzag[static_cast<std::size_t>(k)]] = clamp16(bits.receive_extend(s) * (1 << scan.al));
    } else if (r == 15) {
      k += 15;
    } else {
      ps.eobrun = 1u << r;
      if (r != 0) ps.eobrun += bits.get(r);
      --ps.eobrun;
      return;
    }
  }
}

void decode_ac_refine(BitReader& bits, Decoder& d, Component& c, std::int16_t* block, const Scan& scan,
                      ProgressiveState& ps) {
  const int p1 = 1 << scan.al;
  const int m1 = -1 * (1 << scan.al);
  const HuffTable& ac = d.ac_tables[static_cast<std::size_t>(c.ac_table)];
  int k = scan.ss;
  const auto refine = [&](std::int16_t& coef) {
    if (bits.get(1)) {
      if ((coef & p1) == 0) coef = clamp16(coef >= 0 ? coef + p1 : coef + m1);
    }
  };
  if (ps.eobrun == 0) {
    for (; k <= scan.se; ++k) {
      const int rs = bits.decode(ac);
      int r = rs >> 4;
      int s = rs & 15;
      if (s != 0) {
        if (s != 1) bits.mark_corrupt();
        s = bits.get(1) ? p1 : m1;
      } else if (r != 15) {
        ps.eobrun = 1u << r;
        if (r != 0) ps.eobrun += bits.get(r);
        break;
      }
      while (k <= scan.se) {
        std::int16_t& coef = block[zigzag[static_cast<std::size_t>(k)]];
        if (coef != 0) {
          refine(coef);
        } else {
          if (--r < 0) break;
        }
        ++k;
      }
      if (s != 0 && k <= scan.se) block[zigzag[static_cast<std::size_t>(k)]] = static_cast<std::int16_t>(s);
    }
  }
  if (ps.eobrun > 0) {
    for (; k <= scan.se; ++k) {
      std::int16_t& coef = block[zigzag[static_cast<std::size_t>(k)]];
      if (coef != 0) refine(coef);
    }
    --ps.eobrun;
  }
}

// ---- Arithmetic block decoders ----

struct ArithStats {
  std::array<std::array<std::uint8_t, 64>, 4> dc{};
  std::array<std::array<std::uint8_t, 256>, 4> ac{};
  std::uint8_t fixed = 113;
  void reset() {
    for (auto& t : dc) t.fill(0);
    for (auto& t : ac) t.fill(0);
    fixed = 113;
  }
};

bool arith_dc(ArithDecoder& ad, Decoder& d, ArithStats& stats, Component& c, std::int16_t* block, int al) {
  const auto tbl = static_cast<std::size_t>(c.dc_table);
  std::uint8_t* st = stats.dc[tbl].data() + c.dc_context;
  if (ad.decode(*st) == 0) {
    c.dc_context = 0;
  } else {
    const int sign = ad.decode(st[1]);
    st += 2 + sign;
    int m = ad.decode(*st);
    if (m != 0) {
      st = stats.dc[tbl].data() + 20;
      while (ad.decode(*st)) {
        if ((m <<= 1) == 0x8000) return false;
        ++st;
      }
    }
    if (m < ((1 << d.arith_dc_l[tbl]) >> 1)) {
      c.dc_context = 0;
    } else if (m > ((1 << d.arith_dc_u[tbl]) >> 1)) {
      c.dc_context = 12 + sign * 4;
    } else {
      c.dc_context = 4 + sign * 4;
    }
    int v = m;
    st += 14;
    while (m >>= 1) {
      if (ad.decode(*st)) v |= m;
    }
    v += 1;
    if (sign) v = -v;
    c.dc_pred = std::clamp(c.dc_pred + v, -65536, 65535);
  }
  block[0] = clamp16(c.dc_pred * (1 << al));
  return true;
}

bool arith_ac(ArithDecoder& ad, Decoder& d, ArithStats& stats, Component& c, std::int16_t* block, int ss, int se,
              int al) {
  const auto tbl = static_cast<std::size_t>(c.ac_table);
  for (int k = ss; k <= se; ++k) {
    std::uint8_t* st = stats.ac[tbl].data() + 3 * (k - 1);
    if (ad.decode(*st)) break;  // EOB
    while (ad.decode(st[1]) == 0) {
      st += 3;
      if (++k > se) return false;
    }
    const int sign = ad.decode(stats.fixed);
    st += 2;
    int m = ad.decode(*st);
    if (m != 0) {
      if (ad.decode(*st)) {
        m <<= 1;
        st = stats.ac[tbl].data() + (k <= d.arith_ac_k[tbl] ? 189 : 217);
        while (ad.decode(*st)) {
          if ((m <<= 1) == 0x8000) return false;
          ++st;
        }
      }
    }
    int v = m;
    st += 14;
    while (m >>= 1) {
      if (ad.decode(*st)) v |= m;
    }
    v += 1;
    if (sign) v = -v;
    block[zigzag[static_cast<std::size_t>(k)]] = clamp16(v * (1 << al));
  }
  return true;
}

bool arith_ac_refine(ArithDecoder& ad, ArithStats& stats, Component& c, std::int16_t* block, int ss, int se, int al) {
  const auto tbl = static_cast<std::size_t>(c.ac_table);
  const int p1 = 1 << al;
  const int m1 = -1 * (1 << al);
  int kex = se;
  for (; kex > 0; --kex) {
    if (block[zigzag[static_cast<std::size_t>(kex)]] != 0) break;
  }
  for (int k = ss; k <= se; ++k) {
    std::uint8_t* st = stats.ac[tbl].data() + 3 * (k - 1);
    if (k > kex && ad.decode(*st)) break;
    while (true) {
      std::int16_t& coef = block[zigzag[static_cast<std::size_t>(k)]];
      if (coef != 0) {
        if (ad.decode(st[2])) coef = clamp16(coef < 0 ? coef + m1 : coef + p1);
        break;
      }
      if (ad.decode(st[1])) {
        coef = static_cast<std::int16_t>(ad.decode(stats.fixed) ? m1 : p1);
        break;
      }
      st += 3;
      if (++k > se) return false;
    }
  }
  return true;
}

// ---- scan driver ----

Result<std::size_t> decode_scan(Decoder& d, const Scan& scan, std::size_t start) {
  const bool single = scan.comps.size() == 1;
  std::uint32_t units_x = d.mcus_x;
  std::uint32_t units_y = d.mcus_y;
  if (single) {
    const Component& c = d.components[static_cast<std::size_t>(scan.comps[0])];
    units_x = c.real_blocks_w;
    units_y = c.real_blocks_h;
  }
  std::array<std::array<float, 64>, 4> tables{};
  for (std::size_t i = 0; i < 4; ++i) tables[i] = idct_table(d.quant[i]);
  for (const int ci : scan.comps) d.components[static_cast<std::size_t>(ci)].dc_pred = 0;
  for (const int ci : scan.comps) d.components[static_cast<std::size_t>(ci)].dc_context = 0;

  BitReader bits(d.data, d.size, start);
  ArithDecoder ad(d.data, d.size, start);
  ArithStats stats;
  if (d.arithmetic) {
    stats.reset();
    ad.reset();
  }
  ProgressiveState ps;
  std::uint32_t until_restart = d.restart_interval;
  alignas(16) std::int16_t local[64];
  bool arith_failed = false;

  const auto decode_unit = [&](Component& c, std::uint32_t bx, std::uint32_t by) {
    std::int16_t* block = local;
    if (d.stored_coefs) {
      block = c.coefs.data() + (static_cast<std::size_t>(by) * c.blocks_w + bx) * 64;
    } else {
      std::memset(local, 0, sizeof(local));
    }
    if (d.arithmetic) {
      if (arith_failed) return;
      bool ok = true;
      if (!d.progressive) {
        ok = arith_dc(ad, d, stats, c, block, 0) && arith_ac(ad, d, stats, c, block, 1, 63, 0);
      } else if (scan.ss == 0) {
        if (scan.ah == 0) {
          ok = arith_dc(ad, d, stats, c, block, scan.al);
        } else if (ad.decode(stats.fixed)) {
          block[0] = static_cast<std::int16_t>(block[0] | (1 << scan.al));
        }
      } else if (scan.ah == 0) {
        ok = arith_ac(ad, d, stats, c, block, scan.ss, scan.se, scan.al);
      } else {
        ok = arith_ac_refine(ad, stats, c, block, scan.ss, scan.se, scan.al);
      }
      if (!ok) {
        arith_failed = true;
        d.warn_corrupt();
      }
      return;
    }
    if (!d.progressive) {
      decode_block_sequential(bits, d, c, block);
      idct_into_plane(d, c, block, tables[static_cast<std::size_t>(c.tq)], bx, by);
    } else if (scan.ss == 0) {
      if (scan.ah == 0) {
        decode_dc_first(bits, d, c, block, scan.al);
      } else {
        decode_dc_refine(bits, block, scan.al);
      }
    } else if (scan.ah == 0) {
      decode_ac_first(bits, d, c, block, scan, ps);
    } else {
      decode_ac_refine(bits, d, c, block, scan, ps);
    }
  };

  for (std::uint32_t my = 0; my < units_y; ++my) {
    CVT_TRY(d.ctx.checkpoint(static_cast<double>(my) / units_y, "Decoding"));
    for (std::uint32_t mx = 0; mx < units_x; ++mx) {
      if (d.restart_interval != 0) {
        if (until_restart == 0) {
          const bool found = d.arithmetic ? ad.restart() : bits.restart();
          if (!found) {
            d.warn_corrupt();
            return d.arithmetic ? ad.position() : bits.end_position();
          }
          until_restart = d.restart_interval;
          ps.eobrun = 0;
          for (const int ci : scan.comps) {
            d.components[static_cast<std::size_t>(ci)].dc_pred = 0;
            d.components[static_cast<std::size_t>(ci)].dc_context = 0;
          }
          if (d.arithmetic) {
            stats.reset();
            arith_failed = false;
          }
        }
        --until_restart;
      }
      if (single) {
        decode_unit(d.components[static_cast<std::size_t>(scan.comps[0])], mx, my);
      } else {
        for (const int ci : scan.comps) {
          Component& c = d.components[static_cast<std::size_t>(ci)];
          for (int v = 0; v < c.v; ++v) {
            for (int h = 0; h < c.h; ++h) {
              decode_unit(c, mx * static_cast<std::uint32_t>(c.h) + static_cast<std::uint32_t>(h),
                          my * static_cast<std::uint32_t>(c.v) + static_cast<std::uint32_t>(v));
            }
          }
        }
      }
    }
    if (!d.arithmetic && bits.overrun()) {
      // The data ran out: the rest of the picture stays grey.
      d.ctx.warn("The JPEG file is truncated; the bottom of the picture is missing.");
      d.warned_corrupt = true;
      return bits.end_position();
    }
    if (d.arithmetic && ad.overrun()) {
      d.ctx.warn("The JPEG file is truncated; the bottom of the picture is missing.");
      d.warned_corrupt = true;
      return ad.position();
    }
  }
  if (!d.arithmetic && bits.corrupt()) d.warn_corrupt();
  return d.arithmetic ? ad.position() : bits.end_position();
}

Result<std::size_t> read_sos(Decoder& d, util::ByteReader seg, std::size_t data_start) {
  if (!d.frame_seen) return fail(Status::Decode, "JPEG scan before the frame header");
  const int count = seg.u8();
  if (count < 1 || count > 4) return fail(Status::Decode, "JPEG scan header is invalid");
  Scan scan;
  for (int i = 0; i < count; ++i) {
    const int id = seg.u8();
    const std::uint8_t tables = seg.u8();
    int index = -1;
    for (std::size_t k = 0; k < d.components.size(); ++k) {
      if (d.components[k].id == id) index = static_cast<int>(k);
    }
    if (index < 0) return fail(Status::Decode, "JPEG scan refers to an unknown component");
    if (std::find(scan.comps.begin(), scan.comps.end(), index) != scan.comps.end()) {
      return fail(Status::Decode, "JPEG scan lists a component twice");
    }
    Component& c = d.components[static_cast<std::size_t>(index)];
    c.dc_table = tables >> 4;
    c.ac_table = tables & 15;
    if (c.dc_table > 3 || c.ac_table > 3) return fail(Status::Decode, "JPEG scan header is invalid");
    scan.comps.push_back(index);
  }
  scan.ss = seg.u8();
  scan.se = seg.u8();
  const std::uint8_t a = seg.u8();
  scan.ah = a >> 4;
  scan.al = a & 15;
  if (!seg.ok()) return fail(Status::Decode, "JPEG scan header is truncated");
  if (d.progressive) {
    if (scan.ss > scan.se || scan.se > 63 || (scan.ss == 0 && scan.se != 0) || (scan.ss > 0 && count != 1) ||
        scan.al > 13 || scan.ah > 13) {
      return fail(Status::Decode, "JPEG progressive scan parameters are invalid");
    }
  } else {
    scan.ss = 0;
    scan.se = 63;
    scan.ah = scan.al = 0;
  }
  std::uint64_t blocks = 0;
  for (const int ci : scan.comps) {
    const Component& c = d.components[static_cast<std::size_t>(ci)];
    blocks += static_cast<std::uint64_t>(c.h) * static_cast<std::uint64_t>(c.v);
  }
  if (count > 1 && blocks > 10) return fail(Status::Decode, "JPEG scan has too many blocks per MCU");
  // Check the needed tables.
  for (const int ci : scan.comps) {
    const Component& c = d.components[static_cast<std::size_t>(ci)];
    if (!d.quant_defined[static_cast<std::size_t>(c.tq)] && !d.stored_coefs) {
      return fail(Status::Decode, "JPEG quantisation table {} is missing", c.tq);
    }
    if (d.arithmetic) continue;
    const bool need_dc = scan.ss == 0 && scan.ah == 0;
    const bool need_ac = scan.se > 0 || !d.progressive;
    if (need_dc && !d.dc_tables[static_cast<std::size_t>(c.dc_table)].defined) {
      if (!d.default_huffman) return fail(Status::Decode, "JPEG Huffman table is missing");
      const HuffmanSpec& spec = c.dc_table == 0 ? standard_dc_luma() : standard_dc_chroma();
      CVT_TRY(define_huffman(d, 0, c.dc_table, spec.counts, spec.symbols));
    }
    if (need_ac && !d.ac_tables[static_cast<std::size_t>(c.ac_table)].defined) {
      if (!d.default_huffman) return fail(Status::Decode, "JPEG Huffman table is missing");
      const HuffmanSpec& spec = c.ac_table == 0 ? standard_ac_luma() : standard_ac_chroma();
      CVT_TRY(define_huffman(d, 1, c.ac_table, spec.counts, spec.symbols));
    }
  }
  d.any_scan = true;
  return decode_scan(d, scan, data_start);
}

void read_app(Decoder& d, int marker, ByteView body) {
  const std::string_view text = as_chars(body);
  if (marker == 0xE0 && text.starts_with(std::string_view("JFIF\0", 5)) && body.size() >= 12) {
    d.jfif = true;
    const auto units = std::to_integer<int>(body[7]);
    const auto x = static_cast<double>(std::to_integer<int>(body[8]) << 8 | std::to_integer<int>(body[9]));
    const auto y = static_cast<double>(std::to_integer<int>(body[10]) << 8 | std::to_integer<int>(body[11]));
    if (units == 1 && x > 0 && y > 0) {
      d.extras.dpi_x = x;
      d.extras.dpi_y = y;
    } else if (units == 2 && x > 0 && y > 0) {
      d.extras.dpi_x = x * 2.54;
      d.extras.dpi_y = y * 2.54;
    }
  } else if (marker == 0xE1 && text.starts_with(exif_app1_prefix)) {
    if (d.extras.exif.empty()) d.extras.exif = to_bytes(body.subspan(6));
  } else if (marker == 0xE1 && text.starts_with(std::string_view("http://ns.adobe.com/xap/1.0/\0", 29))) {
    d.extras.xmp = util::sanitize_utf8(text.substr(29));
  } else if (marker == 0xE2 && text.starts_with(std::string_view("ICC_PROFILE\0", 12)) && body.size() >= 14) {
    const int seq = std::to_integer<int>(body[12]);
    if (d.icc_chunks.size() < 256) d.icc_chunks.emplace_back(seq, to_bytes(body.subspan(14)));
  } else if (marker == 0xE2 && text.starts_with(std::string_view("MPF\0", 4))) {
    d.extras.has_mpf = true;
  } else if (marker == 0xEE && text.starts_with("Adobe") && body.size() >= 12) {
    d.adobe = true;
    d.adobe_transform = std::to_integer<int>(body[11]);
  }
}

Result<void> parse(Decoder& d, ByteView input, bool is_tables) {
  d.data = as_u8(input).data();
  d.size = input.size();
  util::ByteReader reader(input);
  if (reader.u8() != 0xFF || reader.u8() != 0xD8) return fail(Status::Decode, "not a JPEG file");
  while (true) {
    // Find the next marker, skipping fill bytes and stray data.
    std::uint8_t byte = reader.u8();
    if (!reader.ok()) break;
    if (byte != 0xFF) continue;
    std::uint8_t marker = reader.u8();
    while (marker == 0xFF && reader.ok()) marker = reader.u8();
    if (!reader.ok()) break;
    if (marker == 0x00 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) continue;
    if (marker == 0xD9) {
      d.eob_reached = true;
      break;
    }
    if (marker == 0xD8) continue;
    const std::uint16_t length = reader.u16be();
    if (!reader.ok() || length < 2 || length - 2u > reader.remaining()) {
      if (d.any_scan) {
        d.ctx.warn("The JPEG file is truncated.");
        break;
      }
      return fail(Status::Decode, "JPEG segment is truncated");
    }
    const ByteView body = reader.bytes(length - 2u);
    util::ByteReader seg(body);
    switch (marker) {
      case 0xC0:
      case 0xC1:
      case 0xC2:
      case 0xC9:
      case 0xCA: CVT_TRY(read_sof(d, seg, marker)); break;
      case 0xC3:
      case 0xC5:
      case 0xC6:
      case 0xC7:
      case 0xCB:
      case 0xCD:
      case 0xCE:
      case 0xCF: return fail(Status::Decode, "lossless and hierarchical JPEG files are not supported");
      case 0xC4: CVT_TRY(read_dht(d, seg)); break;
      case 0xCC: CVT_TRY(read_dac(d, seg)); break;
      case 0xDB: CVT_TRY(read_dqt(d, seg)); break;
      case 0xDD:
        d.restart_interval = seg.u16be();
        break;
      case 0xDA: {
        if (is_tables) return fail(Status::Decode, "JPEG table data contains a scan");
        CVT_TRY_ASSIGN(const std::size_t end, read_sos(d, seg, reader.position()));
        reader.seek(std::min(end, d.size));
        break;
      }
      case 0xFE:
        if (d.extras.comment.empty()) d.extras.comment = util::sanitize_utf8(as_chars(body));
        break;
      default:
        if (marker >= 0xE0 && marker <= 0xEF) read_app(d, marker, body);
        break;
    }
  }
  return {};
}

}  // namespace

Result<Planes> decode_planes(ByteView data, const Context& ctx, const DecodeOptions& options, Extras* extras) {
  Decoder d(ctx);
  d.default_huffman = options.default_huffman;
  if (!options.tables.empty()) {
    CVT_TRY(parse(d, options.tables, true));
    d.eob_reached = false;
  }
  CVT_TRY(parse(d, data, false));
  if (!d.frame_seen) return fail(Status::Decode, "JPEG file has no frame header");
  if (!d.any_scan) return fail(Status::Decode, "JPEG file has no image data");
  if (d.stored_coefs) {
    // Dequantise and transform every stored block.
    std::array<std::array<float, 64>, 4> tables{};
    for (std::size_t i = 0; i < 4; ++i) tables[i] = idct_table(d.quant[i]);
    for (Component& c : d.components) {
      for (std::uint32_t by = 0; by < c.blocks_h; ++by) {
        if ((by & 31) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(by) / c.blocks_h, "Transforming"));
        for (std::uint32_t bx = 0; bx < c.blocks_w; ++bx) {
          idct_into_plane(d, c, c.coefs.data() + (static_cast<std::size_t>(by) * c.blocks_w + bx) * 64,
                          tables[static_cast<std::size_t>(c.tq)], bx, by);
        }
      }
      c.coefs = {};
    }
  }
  Planes planes;
  planes.width = d.width;
  planes.height = d.height;
  planes.precision = d.precision;
  planes.max_h = d.max_h;
  planes.max_v = d.max_v;
  const std::size_t n = d.components.size();
  if (n == 1) {
    planes.model = ColorModel::Gray;
  } else if (n == 3) {
    const bool rgb_ids = d.components[0].id == 'R' && d.components[1].id == 'G' && d.components[2].id == 'B';
    planes.model = (d.adobe && d.adobe_transform == 0) || (!d.jfif && !d.adobe && rgb_ids) ? ColorModel::Rgb
                                                                                              : ColorModel::YCbCr;
  } else {
    planes.model = d.adobe && d.adobe_transform == 2 ? ColorModel::Ycck : ColorModel::Cmyk;
    planes.adobe_inverted = d.adobe;
  }
  for (Component& c : d.components) {
    Plane p;
    p.width = c.width;
    p.height = c.height;
    p.h = c.h;
    p.v = c.v;
    const std::size_t stride = static_cast<std::size_t>(c.blocks_w) * 8;
    if (d.precision == 8) {
      p.samples.resize(static_cast<std::size_t>(c.width) * c.height);
      for (std::uint32_t y = 0; y < c.height; ++y) {
        std::memcpy(p.samples.data() + static_cast<std::size_t>(y) * c.width, c.plane.data() + y * stride, c.width);
      }
    } else {
      p.samples12.resize(static_cast<std::size_t>(c.width) * c.height);
      for (std::uint32_t y = 0; y < c.height; ++y) {
        std::memcpy(p.samples12.data() + static_cast<std::size_t>(y) * c.width, c.plane12.data() + y * stride,
                    static_cast<std::size_t>(c.width) * 2);
      }
    }
    c.plane = {};
    c.plane12 = {};
    planes.components.push_back(std::move(p));
  }
  if (extras != nullptr) {
    *extras = std::move(d.extras);
    extras->progressive = d.progressive;
    extras->arithmetic = d.arithmetic;
    std::sort(d.icc_chunks.begin(), d.icc_chunks.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [seq, chunk] : d.icc_chunks) append(extras->icc, chunk);
  }
  return planes;
}

namespace {

/// Interpolation taps for upsampling a component of `source` samples to
/// `target` samples with centred siting.
struct Taps {
  std::vector<std::uint32_t> i0;
  std::vector<std::uint32_t> i1;
  std::vector<float> w;  // weight of i1
};

Taps make_taps(std::uint32_t source, std::uint32_t target, int factor, int max_factor) {
  Taps t;
  t.i0.resize(target);
  t.i1.resize(target);
  t.w.resize(target);
  const double ratio = static_cast<double>(factor) / max_factor;
  for (std::uint32_t x = 0; x < target; ++x) {
    double s = (x + 0.5) * ratio - 0.5;
    s = std::clamp(s, 0.0, static_cast<double>(source - 1));
    const auto base = static_cast<std::uint32_t>(std::floor(s));
    t.i0[x] = base;
    t.i1[x] = std::min(base + 1, source - 1);
    t.w[x] = static_cast<float>(s - base);
  }
  return t;
}

}  // namespace

namespace {

/// Calls `emit(y, rgb)` for every output row with R, G, B as floats in
/// sample units (0..255 or 0..4095).
template <class Emit>
Result<void> convert_rows(const Planes& planes, const Context& ctx, Emit&& emit) {
  const std::uint32_t width = planes.width;
  const std::uint32_t height = planes.height;
  const std::size_t n = planes.components.size();
  const float max_value = planes.precision == 8 ? 255.0f : 4095.0f;
  const float half = planes.precision == 8 ? 128.0f : 2048.0f;
  std::vector<Taps> htaps(n);
  std::vector<Taps> vtaps(n);
  std::vector<std::vector<float>> rows(n, std::vector<float>(width));
  std::vector<float> temp;
  for (std::size_t c = 0; c < n; ++c) {
    const Plane& p = planes.components[c];
    htaps[c] = make_taps(p.width, width, p.h, planes.max_h);
    vtaps[c] = make_taps(p.height, height, p.v, planes.max_v);
  }
  const auto sample = [&](const Plane& p, std::size_t index) -> float {
    return planes.precision == 8 ? static_cast<float>(p.samples[index]) : static_cast<float>(p.samples12[index]);
  };
  std::vector<float> rgb(static_cast<std::size_t>(width) * 3);
  for (std::uint32_t y = 0; y < height; ++y) {
    if ((y & 63) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(y) / height, "Converting colours"));
    for (std::size_t c = 0; c < n; ++c) {
      const Plane& p = planes.components[c];
      std::vector<float>& row = rows[c];
      const std::uint32_t y0 = vtaps[c].i0[y];
      const std::uint32_t y1 = vtaps[c].i1[y];
      const float wy = vtaps[c].w[y];
      const std::size_t base0 = static_cast<std::size_t>(y0) * p.width;
      const std::size_t base1 = static_cast<std::size_t>(y1) * p.width;
      if (p.width == width) {
        for (std::uint32_t x = 0; x < width; ++x) {
          const float a = sample(p, base0 + x);
          row[x] = wy == 0.0f ? a : a + (sample(p, base1 + x) - a) * wy;
        }
        continue;
      }
      temp.resize(p.width);
      for (std::uint32_t x = 0; x < p.width; ++x) {
        const float a = sample(p, base0 + x);
        temp[x] = wy == 0.0f ? a : a + (sample(p, base1 + x) - a) * wy;
      }
      const Taps& t = htaps[c];
      for (std::uint32_t x = 0; x < width; ++x) {
        const float a = temp[t.i0[x]];
        row[x] = a + (temp[t.i1[x]] - a) * t.w[x];
      }
    }
    float* out = rgb.data();
    switch (planes.model) {
      case ColorModel::Gray:
        for (std::uint32_t x = 0; x < width; ++x) out[x * 3] = out[x * 3 + 1] = out[x * 3 + 2] = rows[0][x];
        break;
      case ColorModel::Rgb:
        for (std::uint32_t x = 0; x < width; ++x) {
          out[x * 3] = rows[0][x];
          out[x * 3 + 1] = rows[1][x];
          out[x * 3 + 2] = rows[2][x];
        }
        break;
      case ColorModel::YCbCr:
        for (std::uint32_t x = 0; x < width; ++x) {
          const float yy = rows[0][x];
          const float cb = rows[1][x] - half;
          const float cr = rows[2][x] - half;
          out[x * 3] = yy + 1.402f * cr;
          out[x * 3 + 1] = yy - 0.344136286f * cb - 0.714136286f * cr;
          out[x * 3 + 2] = yy + 1.772f * cb;
        }
        break;
      case ColorModel::Cmyk:
      case ColorModel::Ycck:
        for (std::uint32_t x = 0; x < width; ++x) {
          float c = rows[0][x], m = rows[1][x], ye = rows[2][x];
          const float k = rows[3][x];
          if (planes.model == ColorModel::Ycck) {
            const float yy = c;
            const float cb = m - half;
            const float cr = ye - half;
            // YCC -> R'G'B', and C = max - R' (libjpeg's YCCK -> CMYK).
            c = max_value - std::clamp(yy + 1.402f * cr, 0.0f, max_value);
            m = max_value - std::clamp(yy - 0.344136286f * cb - 0.714136286f * cr, 0.0f, max_value);
            ye = max_value - std::clamp(yy + 1.772f * cb, 0.0f, max_value);
          }
          if (planes.adobe_inverted) {
            // Adobe stores 255 - ink: R = stored C * stored K / max.
            out[x * 3] = c * k / max_value;
            out[x * 3 + 1] = m * k / max_value;
            out[x * 3 + 2] = ye * k / max_value;
          } else {
            out[x * 3] = (max_value - c) * (max_value - k) / max_value;
            out[x * 3 + 1] = (max_value - m) * (max_value - k) / max_value;
            out[x * 3 + 2] = (max_value - ye) * (max_value - k) / max_value;
          }
        }
        break;
    }
    emit(y, rgb.data());
  }
  return {};
}

inline std::uint8_t to8(float v) noexcept {
  if (!(v > 0.0f)) return 0;
  if (v >= 255.0f) return 255;
  return static_cast<std::uint8_t>(v + 0.5f);
}

}  // namespace

Result<ir::Frame> planes_to_frame(const Planes& planes, const Context& ctx) {
  CVT_TRY(check_size(ctx, planes.width, planes.height));
  ir::Frame frame;
  frame.width = planes.width;
  frame.height = planes.height;
  frame.rgba.resize(static_cast<std::size_t>(planes.width) * planes.height * 4);
  const float scale = planes.precision == 8 ? 1.0f : 255.0f / 4095.0f;
  CVT_TRY(convert_rows(planes, ctx, [&](std::uint32_t y, const float* rgb) {
    std::uint8_t* out = frame.rgba.data() + static_cast<std::size_t>(y) * planes.width * 4;
    for (std::uint32_t x = 0; x < planes.width; ++x) {
      out[x * 4] = to8(rgb[x * 3] * scale);
      out[x * 4 + 1] = to8(rgb[x * 3 + 1] * scale);
      out[x * 4 + 2] = to8(rgb[x * 3 + 2] * scale);
      out[x * 4 + 3] = 255;
    }
  }));
  return frame;
}

Result<ir::Image> decode(ByteView data, const Context& ctx, const DecodeOptions& options) {
  Extras extras;
  CVT_TRY_ASSIGN(const Planes planes, decode_planes(data, ctx, options, &extras));
  ir::Image image;
  if (planes.precision == 8) {
    CVT_TRY_ASSIGN(ir::Frame frame, planes_to_frame(planes, ctx));
    image.frames.push_back(std::move(frame));
  } else {
    CVT_TRY(check_size(ctx, planes.width, planes.height));
    ir::Frame frame;
    frame.width = planes.width;
    frame.height = planes.height;
    frame.rgba.resize(static_cast<std::size_t>(planes.width) * planes.height * 4);
    ir::HdrPixels hdr;
    hdr.width = planes.width;
    hdr.height = planes.height;
    hdr.rgba.resize(frame.rgba.size());
    CVT_TRY(convert_rows(planes, ctx, [&](std::uint32_t y, const float* rgb) {
      const std::size_t base = static_cast<std::size_t>(y) * planes.width * 4;
      for (std::uint32_t x = 0; x < planes.width; ++x) {
        for (std::size_t c = 0; c < 3; ++c) {
          const float v = std::clamp(rgb[x * 3 + c], 0.0f, 4095.0f);
          frame.rgba[base + x * 4 + c] = to8(v * (255.0f / 4095.0f));
          hdr.rgba[base + x * 4 + c] = srgb_to_linear(v / 4095.0f);
        }
        frame.rgba[base + x * 4 + 3] = 255;
        hdr.rgba[base + x * 4 + 3] = 1.0f;
      }
    }));
    image.frames.push_back(std::move(frame));
    image.hdr = std::move(hdr);
  }
  image.source_bit_depth = static_cast<std::uint8_t>(planes.precision);
  image.source_grayscale = planes.model == ColorModel::Gray;
  image.dpi_x = extras.dpi_x;
  image.dpi_y = extras.dpi_y;
  image.icc_profile = std::move(extras.icc);
  image.xmp = std::move(extras.xmp);
  if (!extras.comment.empty()) image.metadata["Comment"] = extras.comment;
  int orientation = 1;
  if (!extras.exif.empty()) orientation = attach_exif(image, extras.exif);
  // Keep the file itself for writers that can embed JPEG unchanged (PDF):
  // only when the pixels are exactly what the bytes show, and bounded.
  constexpr std::size_t max_kept = 64u << 20;
  const bool embeddable_model = planes.model == ColorModel::Gray || planes.model == ColorModel::YCbCr || planes.model == ColorModel::Rgb;
  if (orientation == 1 && planes.precision == 8 && embeddable_model && data.size() <= max_kept) {
    image.original_bytes.assign(data.begin(), data.end());
    image.original_mime = "image/jpeg";
  }
  return image;
}

}  // namespace cvt::image::jpeg
