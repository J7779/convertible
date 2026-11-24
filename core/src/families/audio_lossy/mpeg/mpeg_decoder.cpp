// Convertible core - audio_lossy family: MPEG-1/2/2.5 audio decoder for
// layers I, II and III, written from ISO/IEC 11172-3 and 13818-3: bit
// reservoir, MPEG-1 and LSF scale factors, Huffman decoding, requantisation,
// mid/side and intensity stereo (both flavours), short/mixed blocks, alias
// reduction, IMDCT with overlap-add and the polyphase synthesis filterbank.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <vector>

#include "families/audio_lossy/mpeg/mpeg_audio.hpp"
#include "families/audio_lossy/mpeg/mpeg_huffman.hpp"
#include "families/audio_lossy/mpeg/mpeg_tables.hpp"

namespace cvt::lossy::mpeg {

namespace {

constexpr std::size_t max_reservoir = 4096;

template <class T>
constexpr std::size_t z(T v) noexcept {
  return static_cast<std::size_t>(v);
}

/// MSB-first bit reader that reads zeros past the end.
class Bits {
 public:
  Bits() = default;
  Bits(const std::uint8_t* data, std::size_t size) : data_(data), size_bits_(std::uint64_t{size} * 8) {}
  std::uint32_t get(unsigned n) noexcept {
    std::uint32_t v = 0;
    for (unsigned i = 0; i < n; ++i) v = (v << 1) | bit();
    return v;
  }
  std::uint32_t bit() noexcept {
    const std::uint64_t p = pos_++;
    if (p >= size_bits_) return 0;
    return (data_[p >> 3] >> (7 - (p & 7))) & 1u;
  }
  [[nodiscard]] std::uint64_t pos() const noexcept { return pos_; }
  void seek(std::uint64_t p) noexcept { pos_ = p; }
  [[nodiscard]] bool overrun() const noexcept { return pos_ > size_bits_; }
  [[nodiscard]] std::uint64_t size() const noexcept { return size_bits_; }

 private:
  const std::uint8_t* data_ = nullptr;
  std::uint64_t size_bits_ = 0;
  std::uint64_t pos_ = 0;
};

/// Binary decoding tree of a Huffman table: node i has children at [2i]
/// and [2i+1]; a value > 0 is the next node, < 0 is -(symbol + 1), 0 is an
/// unused code.
struct HuffTree {
  std::vector<std::int32_t> child;
  template <std::size_t N>
  static HuffTree build(const std::array<HuffCode, N>& table) {
    HuffTree t;
    t.child.assign(2, 0);
    for (std::size_t s = 0; s < N; ++s) {
      const HuffCode c = table[s];
      if (c.length == 0) continue;
      std::size_t node = 0;
      for (int b = c.length - 1; b >= 0; --b) {
        const std::size_t slot = node * 2 + ((c.code >> b) & 1u);
        if (b == 0) {
          t.child[slot] = -static_cast<std::int32_t>(s) - 1;
        } else {
          if (t.child[slot] <= 0) {
            t.child[slot] = static_cast<std::int32_t>(t.child.size() / 2);
            t.child.resize(t.child.size() + 2, 0);
          }
          node = static_cast<std::size_t>(t.child[slot]);
        }
      }
    }
    return t;
  }
  /// Decode one symbol; -1 on an unused code.
  int decode(Bits& bits) const noexcept {
    std::size_t node = 0;
    for (int depth = 0; depth < 24; ++depth) {
      const std::int32_t c = child[node * 2 + bits.bit()];
      if (c < 0) return -c - 1;
      if (c == 0) return -1;
      node = static_cast<std::size_t>(c);
    }
    return -1;
  }
};

struct Granule {
  int part2_3_length = 0;
  int big_values = 0;
  int global_gain = 0;
  int scalefac_compress = 0;
  bool window_switching = false;
  int block_type = 0;
  bool mixed = false;
  std::array<int, 3> table_select{};
  std::array<int, 3> subblock_gain{};
  int region0_count = 0;
  int region1_count = 0;
  bool preflag = false;
  bool scalefac_scale = false;
  bool count1_table = false;
};

struct SideInfo {
  int main_data_begin = 0;
  std::array<std::array<int, 4>, 2> scfsi{};
  std::array<std::array<Granule, 2>, 2> gr{};  // [granule][channel]
};

struct Scalefactors {
  std::array<int, 22> l{};
  std::array<std::array<int, 3>, 13> s{};
  // Intensity stereo: the value marking an illegal position (-1 = none).
  std::array<int, 22> l_max{};
  std::array<int, 13> s_max{};
};

struct ChannelState {
  std::array<double, 576> overlap{};
  std::array<double, 1024> v{};
  std::size_t v_offset = 0;
};

/// Band edges (in lines) of a table row.
struct Bands {
  std::array<int, 23> l{};  // long: 22 bands
  std::array<int, 14> s{};  // short: 13 bands (lines of one window)
};

Bands make_bands(int slot) {
  Bands b;
  for (std::size_t i = 0; i < 22; ++i) b.l[i + 1] = b.l[i] + sfb_long_widths[slot][i];
  for (std::size_t i = 0; i < 13; ++i) b.s[i + 1] = b.s[i] + sfb_short_widths[slot][i];
  return b;
}

}  // namespace

struct FrameDecoder::State {
  std::array<HuffTree, 32> trees;
  HuffTree count1_a;
  std::array<double, 8207> pow43{};
  std::array<std::array<double, 32>, 64> synth_matrix{};
  std::array<double, 512> window{};
  std::array<std::array<double, 36>, 4> imdct_window{};
  std::array<std::array<double, 18>, 36> cos36{};
  std::array<std::array<double, 6>, 12> cos12{};
  std::array<double, 8> cs{}, ca{};
  std::array<double, 64> scale_l12{};
  std::array<ChannelState, 2> ch;
  std::vector<std::uint8_t> reservoir;
  std::array<std::array<Scalefactors, 2>, 2> sf{};  // [granule][channel]
  std::array<std::array<double, 576>, 2> xr{};
  std::array<std::array<int, 576>, 2> ix{};
  std::array<int, 2> nonzero{};  // lines up to and including the last non-zero one
  std::array<std::array<double, 18>, 32> hybrid_out{};

  State() {
    trees[1] = HuffTree::build(huff_table1);
    trees[2] = HuffTree::build(huff_table2);
    trees[3] = HuffTree::build(huff_table3);
    trees[5] = HuffTree::build(huff_table5);
    trees[6] = HuffTree::build(huff_table6);
    trees[7] = HuffTree::build(huff_table7);
    trees[8] = HuffTree::build(huff_table8);
    trees[9] = HuffTree::build(huff_table9);
    trees[10] = HuffTree::build(huff_table10);
    trees[11] = HuffTree::build(huff_table11);
    trees[12] = HuffTree::build(huff_table12);
    trees[13] = HuffTree::build(huff_table13);
    trees[15] = HuffTree::build(huff_table15);
    trees[16] = HuffTree::build(huff_table16);
    for (std::size_t t = 17; t < 24; ++t) trees[t] = trees[16];
    trees[24] = HuffTree::build(huff_table24);
    for (std::size_t t = 25; t < 32; ++t) trees[t] = trees[24];
    count1_a = HuffTree::build(count1_table_a);
    for (std::size_t i = 0; i < pow43.size(); ++i) pow43[i] = std::pow(static_cast<double>(i), 4.0 / 3.0);
    for (std::size_t i = 0; i < 64; ++i) {
      for (std::size_t k = 0; k < 32; ++k) {
        synth_matrix[i][k] = std::cos(static_cast<double>((16 + i) * (2 * k + 1)) * std::numbers::pi / 64.0);
      }
    }
    for (std::size_t i = 0; i < 512; ++i) window[i] = synthesis_window[i] / 65536.0;
    const double pi = std::numbers::pi;
    for (std::size_t i = 0; i < 36; ++i) {
      const double di = static_cast<double>(i);
      imdct_window[0][i] = std::sin(pi / 36.0 * (di + 0.5));
      if (i < 18) imdct_window[1][i] = std::sin(pi / 36.0 * (di + 0.5));
      else if (i < 24) imdct_window[1][i] = 1.0;
      else if (i < 30) imdct_window[1][i] = std::sin(pi / 12.0 * (di - 18.0 + 0.5));
      else imdct_window[1][i] = 0.0;
      if (i < 6) imdct_window[3][i] = 0.0;
      else if (i < 12) imdct_window[3][i] = std::sin(pi / 12.0 * (di - 6.0 + 0.5));
      else if (i < 18) imdct_window[3][i] = 1.0;
      else imdct_window[3][i] = std::sin(pi / 36.0 * (di + 0.5));
      if (i < 12) imdct_window[2][i] = std::sin(pi / 12.0 * (di + 0.5));
    }
    for (std::size_t i = 0; i < 36; ++i) {
      for (std::size_t k = 0; k < 18; ++k) {
        cos36[i][k] = std::cos(pi / 72.0 * static_cast<double>((2 * i + 1 + 18) * (2 * k + 1)));
      }
    }
    for (std::size_t i = 0; i < 12; ++i) {
      for (std::size_t k = 0; k < 6; ++k) {
        cos12[i][k] = std::cos(pi / 24.0 * static_cast<double>((2 * i + 1 + 6) * (2 * k + 1)));
      }
    }
    for (std::size_t i = 0; i < 8; ++i) {
      const double c = alias_coefficients[i];
      cs[i] = 1.0 / std::sqrt(1.0 + c * c);
      ca[i] = c / std::sqrt(1.0 + c * c);
    }
    for (std::size_t i = 0; i < 64; ++i) scale_l12[i] = std::pow(2.0, 1.0 - static_cast<double>(i) / 3.0);
  }

  void reset() {
    for (ChannelState& c : ch) c = ChannelState{};
    reservoir.clear();
  }

  void synthesize(ChannelState& c, const double* subband, float* out, std::size_t stride);
  Result<void> decode_layer1(const FrameHeader& h, Bits& bits, std::vector<float>& out);
  Result<void> decode_layer2(const FrameHeader& h, Bits& bits, std::vector<float>& out);
  Result<void> decode_layer3(const FrameHeader& h, ByteView frame, std::vector<float>& out);
  bool read_side_info(const FrameHeader& h, Bits& bits, SideInfo& si);
  void read_scalefactors_mpeg1(Bits& bits, const Granule& g, const SideInfo& si, int gr, int c);
  bool read_scalefactors_lsf(Bits& bits, const Granule& g, int c, bool intensity_right);
  bool huffman(Bits& bits, const Granule& g, const Bands& bands, std::uint64_t end, int c);
  void requantize(const Granule& g, const Bands& bands, const Scalefactors& sf, bool lsf, int c);
  void stereo(const FrameHeader& h, const Granule& g, const Bands& bands, const Scalefactors& right_sf,
              int intensity_scale);
  void reorder(const Granule& g, const Bands& bands, bool lsf, int c);
  void hybrid(const Granule& g, const Bands& bands, bool lsf, int c);
};

// =========================================================================== synthesis

void FrameDecoder::State::synthesize(ChannelState& c, const double* subband, float* out, std::size_t stride) {
  c.v_offset = (c.v_offset + 1024 - 64) & 1023;
  double* v = c.v.data();
  for (std::size_t i = 0; i < 64; ++i) {
    double sum = 0.0;
    const auto& row = synth_matrix[i];
    for (std::size_t k = 0; k < 32; ++k) sum += row[k] * subband[k];
    v[(c.v_offset + i) & 1023] = sum;
  }
  for (std::size_t j = 0; j < 32; ++j) {
    double sum = 0.0;
    for (std::size_t i = 0; i < 8; ++i) {
      sum += window[64 * i + j] * v[(c.v_offset + 128 * i + j) & 1023];
      sum += window[64 * i + 32 + j] * v[(c.v_offset + 128 * i + 96 + j) & 1023];
    }
    out[j * stride] = static_cast<float>(sum);
  }
}

// =========================================================================== Layer I

Result<void> FrameDecoder::State::decode_layer1(const FrameHeader& h, Bits& bits, std::vector<float>& out) {
  const int nch = h.channels();
  const int bound = (h.mode == 1) ? (h.mode_ext + 1) * 4 : 32;
  std::array<std::array<int, 32>, 2> alloc{};
  std::array<std::array<int, 32>, 2> scf{};
  for (int sb = 0; sb < 32; ++sb) {
    for (int c = 0; c < (sb < bound ? nch : 1); ++c) alloc[z(c)][z(sb)] = static_cast<int>(bits.get(4));
    if (sb >= bound && nch == 2) alloc[1][z(sb)] = alloc[0][z(sb)];
  }
  for (int sb = 0; sb < 32; ++sb) {
    for (int c = 0; c < nch; ++c) {
      if (alloc[z(c)][z(sb)] == 15) return fail(Status::Decode, "invalid Layer I bit allocation");
      if (alloc[z(c)][z(sb)] != 0) scf[z(c)][z(sb)] = static_cast<int>(bits.get(6));
    }
  }
  const std::size_t base = out.size();
  out.resize(base + 384 * z(nch), 0.0f);
  std::array<std::array<double, 32>, 2> sample{};
  for (int s = 0; s < 12; ++s) {
    for (int sb = 0; sb < 32; ++sb) {
      for (int c = 0; c < nch; ++c) sample[z(c)][z(sb)] = 0.0;
      for (int c = 0; c < (sb < bound ? nch : 1); ++c) {
        const int a = alloc[z(c)][z(sb)];
        if (a == 0) continue;
        const int nb = a + 1;
        const double levels = static_cast<double>((1 << nb) - 1);
        const auto code = static_cast<double>(bits.get(static_cast<unsigned>(nb)));
        const double v = (2.0 * code - (levels - 1.0)) / levels;
        sample[z(c)][z(sb)] = v * scale_l12[z(scf[z(c)][z(sb)])];
        if (sb >= bound && nch == 2) sample[1][z(sb)] = v * scale_l12[z(scf[1][z(sb)])];
      }
    }
    for (int c = 0; c < nch; ++c) {
      synthesize(ch[z(c)], sample[z(c)].data(), out.data() + base + z(s) * 32 * z(nch) + z(c), z(nch));
    }
  }
  if (bits.overrun()) return fail(Status::Decode, "Layer I frame is truncated");
  return {};
}

// =========================================================================== Layer II

Result<void> FrameDecoder::State::decode_layer2(const FrameHeader& h, Bits& bits, std::vector<float>& out) {
  const int nch = h.channels();
  const AllocRow* rows = nullptr;
  std::size_t row_count = 0;
  int sblimit = 30;
  if (h.lsf()) {
    rows = alloc_lsf;
    row_count = 3;
    sblimit = 30;
  } else {
    std::uint32_t kbps = h.bitrate_kbps;
    if (kbps == 0) kbps = nch == 2 ? 384 : 192;  // free format: assume the large tables
    const std::uint32_t per_channel = nch == 2 ? kbps / 2 : kbps;
    if (per_channel <= 48) {
      rows = alloc_b2cd;
      row_count = 2;
      sblimit = h.sample_rate == 32000 ? 12 : 8;
    } else {
      rows = alloc_b2ab;
      row_count = 4;
      sblimit = (per_channel >= 96 && h.sample_rate != 48000) ? 30 : 27;
    }
  }
  const int bound = (h.mode == 1) ? std::min((h.mode_ext + 1) * 4, sblimit) : sblimit;
  std::array<const AllocRow*, 32> row_of{};
  {
    int sb = 0;
    for (std::size_t r = 0; r < row_count && sb < sblimit; ++r) {
      for (int i = 0; i < rows[r].subbands && sb < sblimit; ++i) row_of[z(sb++)] = &rows[r];
    }
  }
  std::array<std::array<int, 32>, 2> alloc{};
  for (int sb = 0; sb < sblimit; ++sb) {
    const AllocRow& row = *row_of[z(sb)];
    for (int c = 0; c < (sb < bound ? nch : 1); ++c) alloc[z(c)][z(sb)] = row.classes[bits.get(row.alloc_bits)];
    if (sb >= bound && nch == 2) alloc[1][z(sb)] = alloc[0][z(sb)];
  }
  std::array<std::array<int, 32>, 2> scfsi{};
  for (int sb = 0; sb < sblimit; ++sb) {
    for (int c = 0; c < nch; ++c) {
      if (alloc[z(c)][z(sb)] != 0) scfsi[z(c)][z(sb)] = static_cast<int>(bits.get(2));
    }
  }
  std::array<std::array<std::array<int, 3>, 32>, 2> scf{};
  for (int sb = 0; sb < sblimit; ++sb) {
    for (int c = 0; c < nch; ++c) {
      if (alloc[z(c)][z(sb)] == 0) continue;
      auto& s = scf[z(c)][z(sb)];
      switch (scfsi[z(c)][z(sb)]) {
        case 0:
          for (int& v : s) v = static_cast<int>(bits.get(6));
          break;
        case 1:
          s[0] = s[1] = static_cast<int>(bits.get(6));
          s[2] = static_cast<int>(bits.get(6));
          break;
        case 2:
          s[0] = s[1] = s[2] = static_cast<int>(bits.get(6));
          break;
        default:
          s[0] = static_cast<int>(bits.get(6));
          s[1] = s[2] = static_cast<int>(bits.get(6));
          break;
      }
    }
  }
  const std::size_t base = out.size();
  out.resize(base + 1152 * z(nch), 0.0f);
  std::array<std::array<std::array<double, 32>, 3>, 2> sample{};
  for (int gr = 0; gr < 12; ++gr) {
    const std::size_t part = z(gr / 4);
    for (auto& c : sample) {
      for (auto& slot : c) slot.fill(0.0);
    }
    for (int sb = 0; sb < sblimit; ++sb) {
      for (int c = 0; c < (sb < bound ? nch : 1); ++c) {
        const QuantClass& q = quant_classes[alloc[z(c)][z(sb)]];
        if (q.levels == 0) continue;
        std::array<double, 3> v{};
        const double levels = q.levels;
        if (q.grouped) {
          std::uint32_t code = bits.get(q.bits);
          for (double& x : v) {
            const std::uint32_t s = code % q.levels;
            code /= q.levels;
            x = (2.0 * s - (levels - 1.0)) / levels;
          }
        } else {
          for (double& x : v) x = (2.0 * bits.get(q.bits) - (levels - 1.0)) / levels;
        }
        for (std::size_t i = 0; i < 3; ++i) sample[z(c)][i][z(sb)] = v[i] * scale_l12[z(scf[z(c)][z(sb)][part])];
        if (sb >= bound && nch == 2) {
          for (std::size_t i = 0; i < 3; ++i) sample[1][i][z(sb)] = v[i] * scale_l12[z(scf[1][z(sb)][part])];
        }
      }
    }
    for (std::size_t i = 0; i < 3; ++i) {
      for (int c = 0; c < nch; ++c) {
        const std::size_t slot = z(gr) * 3 + i;
        synthesize(ch[z(c)], sample[z(c)][i].data(), out.data() + base + slot * 32 * z(nch) + z(c), z(nch));
      }
    }
  }
  if (bits.overrun()) return fail(Status::Decode, "Layer II frame is truncated");
  return {};
}

// =========================================================================== Layer III

bool FrameDecoder::State::read_side_info(const FrameHeader& h, Bits& bits, SideInfo& si) {
  const int nch = h.channels();
  const int granules = h.lsf() ? 1 : 2;
  if (h.lsf()) {
    si.main_data_begin = static_cast<int>(bits.get(8));
    bits.get(nch == 1 ? 1 : 2);
  } else {
    si.main_data_begin = static_cast<int>(bits.get(9));
    bits.get(nch == 1 ? 5 : 3);
    for (int c = 0; c < nch; ++c) {
      for (std::size_t b = 0; b < 4; ++b) si.scfsi[z(c)][b] = static_cast<int>(bits.get(1));
    }
  }
  for (int gr = 0; gr < granules; ++gr) {
    for (int c = 0; c < nch; ++c) {
      Granule& g = si.gr[z(gr)][z(c)];
      g.part2_3_length = static_cast<int>(bits.get(12));
      g.big_values = static_cast<int>(bits.get(9));
      if (g.big_values > 288) return false;
      g.global_gain = static_cast<int>(bits.get(8));
      g.scalefac_compress = static_cast<int>(bits.get(h.lsf() ? 9 : 4));
      g.window_switching = bits.get(1) != 0;
      if (g.window_switching) {
        g.block_type = static_cast<int>(bits.get(2));
        if (g.block_type == 0) return false;
        g.mixed = bits.get(1) != 0;
        g.table_select[0] = static_cast<int>(bits.get(5));
        g.table_select[1] = static_cast<int>(bits.get(5));
        g.table_select[2] = 0;
        for (std::size_t w = 0; w < 3; ++w) g.subblock_gain[w] = static_cast<int>(bits.get(3));
      } else {
        g.block_type = 0;
        g.mixed = false;
        for (std::size_t r = 0; r < 3; ++r) g.table_select[r] = static_cast<int>(bits.get(5));
        g.region0_count = static_cast<int>(bits.get(4));
        g.region1_count = static_cast<int>(bits.get(3));
      }
      g.preflag = h.lsf() ? false : bits.get(1) != 0;
      g.scalefac_scale = bits.get(1) != 0;
      g.count1_table = bits.get(1) != 0;
    }
  }
  return true;
}

void FrameDecoder::State::read_scalefactors_mpeg1(Bits& bits, const Granule& g, const SideInfo& si, int gr, int c) {
  Scalefactors& s = sf[z(gr)][z(c)];
  s.l_max.fill(7);
  s.s_max.fill(7);
  const auto slen1 = static_cast<unsigned>(slen_table[0][g.scalefac_compress]);
  const auto slen2 = static_cast<unsigned>(slen_table[1][g.scalefac_compress]);
  if (g.window_switching && g.block_type == 2) {
    int first = 0;
    if (g.mixed) {
      for (std::size_t b = 0; b < 8; ++b) s.l[b] = static_cast<int>(bits.get(slen1));
      first = 3;
    }
    for (int b = first; b < 12; ++b) {
      for (std::size_t w = 0; w < 3; ++w) s.s[z(b)][w] = static_cast<int>(bits.get(b < 6 ? slen1 : slen2));
    }
    s.s[12] = {0, 0, 0};
    return;
  }
  static constexpr int group_start[5] = {0, 6, 11, 16, 21};
  for (std::size_t grp = 0; grp < 4; ++grp) {
    const unsigned len = grp < 2 ? slen1 : slen2;
    const bool reuse = gr == 1 && si.scfsi[z(c)][grp] != 0;
    for (int b = group_start[grp]; b < group_start[grp + 1]; ++b) {
      s.l[z(b)] = reuse ? sf[0][z(c)].l[z(b)] : static_cast<int>(bits.get(len));
    }
  }
  s.l[21] = 0;
}

bool FrameDecoder::State::read_scalefactors_lsf(Bits& bits, const Granule& g, int c, bool intensity_right) {
  Scalefactors& s = sf[0][z(c)];
  std::array<int, 4> slen{};
  int table = 0;
  bool preflag = false;
  if (!intensity_right) {
    int v = g.scalefac_compress;
    if (v < 400) {
      slen = {(v >> 4) / 5, (v >> 4) % 5, (v & 15) >> 2, v & 3};
    } else if (v < 500) {
      v -= 400;
      slen = {(v >> 2) / 5, (v >> 2) % 5, v & 3, 0};
      table = 1;
    } else {
      v -= 500;
      slen = {v / 3, v % 3, 0, 0};
      table = 2;
      preflag = true;
    }
  } else {
    int v = g.scalefac_compress >> 1;
    if (v < 180) {
      slen = {v / 36, (v % 36) / 6, (v % 36) % 6, 0};
      table = 3;
    } else if (v < 244) {
      v -= 180;
      slen = {(v & 63) >> 4, (v & 15) >> 2, v & 3, 0};
      table = 4;
    } else {
      v -= 244;
      slen = {v / 3, v % 3, 0, 0};
      table = 5;
    }
  }
  const int block = (g.window_switching && g.block_type == 2) ? (g.mixed ? 2 : 1) : 0;
  std::array<int, 40> values{};
  std::array<int, 40> maxima{};
  std::size_t n = 0;
  for (std::size_t grp = 0; grp < 4; ++grp) {
    const int count = lsf_sfb_counts[table][block][grp];
    const int len = slen[grp];
    for (int i = 0; i < count && n < values.size(); ++i, ++n) {
      values[n] = static_cast<int>(bits.get(static_cast<unsigned>(len)));
      maxima[n] = (1 << len) - 1;
    }
  }
  s.l.fill(0);
  for (auto& w : s.s) w.fill(0);
  s.l_max.fill(-1);
  s.s_max.fill(-1);
  std::size_t k = 0;
  if (block == 0) {
    for (std::size_t b = 0; b < 21 && k < n; ++b, ++k) {
      s.l[b] = values[k];
      s.l_max[b] = maxima[k];
    }
    s.l_max[21] = s.l_max[20];
  } else {
    std::size_t first_short = 0;
    if (block == 2) {
      for (std::size_t b = 0; b < 6 && k < n; ++b, ++k) {
        s.l[b] = values[k];
        s.l_max[b] = maxima[k];
      }
      first_short = 3;
    }
    for (std::size_t b = first_short; b < 12; ++b) {
      for (std::size_t w = 0; w < 3 && k < n; ++w, ++k) {
        s.s[b][w] = values[k];
        s.s_max[b] = maxima[k];
      }
    }
    s.s_max[12] = s.s_max[11];
  }
  return preflag;
}

bool FrameDecoder::State::huffman(Bits& bits, const Granule& g, const Bands& bands, std::uint64_t end, int c) {
  auto& is = ix[z(c)];
  is.fill(0);
  int region1 = 0;
  int region2 = 576;
  if (g.window_switching) {
    region1 = g.block_type == 2 ? 3 * bands.s[3] : bands.l[8];
    region2 = 576;
  } else {
    region1 = bands.l[z(std::min(g.region0_count + 1, 22))];
    region2 = bands.l[z(std::min(g.region0_count + g.region1_count + 2, 22))];
  }
  const int big_end = std::min(g.big_values * 2, 576);
  region1 = std::min(region1, big_end);
  region2 = std::min(region2, big_end);
  int i = 0;
  int last = 0;
  for (int region = 0; region < 3; ++region) {
    const int stop = region == 0 ? region1 : region == 1 ? region2 : big_end;
    const int table = g.table_select[z(region)];
    if (table == 0 || table == 4 || table == 14) {
      if (table != 0 && i < stop) return false;  // tables 4 and 14 do not exist
      for (; i < stop; i += 2) is[z(i)] = is[z(i) + 1] = 0;
      continue;
    }
    const HuffTree& tree = trees[z(table)];
    const unsigned linbits = linbits_table[table];
    const int size = table >= 13 ? 16 : (table >= 10 ? 8 : (table >= 7 ? 6 : (table >= 5 ? 4 : (table >= 2 ? 3 : 2))));
    for (; i < stop; i += 2) {
      const int symbol = tree.decode(bits);
      if (symbol < 0) return false;
      int x = symbol / size;
      int y = symbol % size;
      if (linbits != 0 && x == 15) x += static_cast<int>(bits.get(linbits));
      if (x != 0 && bits.bit() != 0) x = -x;
      if (linbits != 0 && y == 15) y += static_cast<int>(bits.get(linbits));
      if (y != 0 && bits.bit() != 0) y = -y;
      is[z(i)] = x;
      is[z(i) + 1] = y;
      if (x != 0) last = i + 1;
      if (y != 0) last = i + 2;
    }
    if (bits.pos() > end) break;
  }
  // count1 region: quadruples until the part 3 data is used up.
  while (i + 4 <= 576 && bits.pos() < end) {
    int vwxy = 0;
    if (g.count1_table) {
      vwxy = 15 - static_cast<int>(bits.get(4));
    } else {
      vwxy = count1_a.decode(bits);
      if (vwxy < 0) return false;
    }
    std::array<int, 4> q = {(vwxy >> 3) & 1, (vwxy >> 2) & 1, (vwxy >> 1) & 1, vwxy & 1};
    for (int& v : q) {
      if (v != 0 && bits.bit() != 0) v = -1;
    }
    if (bits.pos() > end) break;  // the last quadruple overran: drop it
    for (std::size_t k = 0; k < 4; ++k) {
      is[z(i) + k] = q[k];
      if (q[k] != 0) last = i + static_cast<int>(k) + 1;
    }
    i += 4;
  }
  for (int k = i; k < 576; ++k) is[z(k)] = 0;
  if (last > i) last = i;
  nonzero[z(c)] = last;
  bits.seek(end);
  return true;
}

void FrameDecoder::State::requantize(const Granule& g, const Bands& bands, const Scalefactors& s, bool lsf, int c) {
  auto& out = xr[z(c)];
  const auto& in = ix[z(c)];
  out.fill(0.0);
  const double mult = g.scalefac_scale ? 1.0 : 0.5;
  const double base = 0.25 * (g.global_gain - 210);
  const int limit = nonzero[z(c)];
  const auto value = [&](int q, double gain) {
    const double m = pow43[z(std::min(std::abs(q), 8206))];
    return q < 0 ? -m * gain : m * gain;
  };
  if (!(g.window_switching && g.block_type == 2)) {
    for (std::size_t b = 0; b < 22; ++b) {
      if (bands.l[b] >= limit) break;
      const double e = base - mult * (s.l[b] + (g.preflag ? pretab[b] : 0));
      const double gain = std::exp2(e);
      for (int k = bands.l[b]; k < bands.l[b + 1] && k < limit; ++k) out[z(k)] = value(in[z(k)], gain);
    }
    return;
  }
  int line = 0;
  std::size_t first_short = 0;
  if (g.mixed) {
    const std::size_t long_bands = lsf ? 6 : 8;
    for (std::size_t b = 0; b < long_bands; ++b) {
      const double e = base - mult * (s.l[b] + (g.preflag ? pretab[b] : 0));
      const double gain = std::exp2(e);
      for (int k = bands.l[b]; k < bands.l[b + 1]; ++k) out[z(k)] = value(in[z(k)], gain);
    }
    line = bands.l[long_bands];
    first_short = 3;
  }
  for (std::size_t b = first_short; b < 13; ++b) {
    const int width = bands.s[b + 1] - bands.s[b];
    for (std::size_t w = 0; w < 3; ++w) {
      const double e = base - 2.0 * g.subblock_gain[w] - mult * s.s[b][w];
      const double gain = std::exp2(e);
      for (int k = 0; k < width; ++k, ++line) {
        if (line < 576) out[z(line)] = value(in[z(line)], gain);
      }
    }
  }
}

void FrameDecoder::State::stereo(const FrameHeader& h, const Granule& g, const Bands& bands,
                                 const Scalefactors& right, int intensity_scale) {
  const bool ms = (h.mode_ext & 2) != 0;
  const bool intensity = (h.mode_ext & 1) != 0;
  auto& l = xr[0];
  auto& r = xr[1];
  std::array<bool, 576> done{};
  if (intensity) {
    const auto apply = [&](int from, int to, int pos, int illegal) {
      if (pos == illegal) return;  // illegal position: plain or M/S stereo
      double kl = 1.0, kr = 1.0;
      if (!h.lsf()) {
        if (pos >= 7) return;
        if (pos == 6) {
          kl = 1.0;
          kr = 0.0;
        } else {
          const double t = std::tan(pos * std::numbers::pi / 12.0);
          kl = t / (1.0 + t);
          kr = 1.0 / (1.0 + t);
        }
      } else {
        const double io = intensity_scale != 0 ? std::numbers::sqrt2 / 2.0 : std::pow(2.0, -0.25);
        if (pos == 0) {
          kl = kr = 1.0;
        } else if ((pos & 1) != 0) {
          kl = std::pow(io, (pos + 1) / 2);
        } else {
          kr = std::pow(io, pos / 2);
        }
      }
      for (int k = from; k < to && k < 576; ++k) {
        const double v = l[z(k)];
        l[z(k)] = v * kl;
        r[z(k)] = v * kr;
        done[z(k)] = true;
      }
    };
    const auto mpeg1_illegal = h.lsf() ? -2 : 7;
    if (!(g.window_switching && g.block_type == 2)) {
      int start_band = 0;
      const int last = nonzero[1];
      if (last > 0) {
        while (start_band < 22 && bands.l[z(start_band) + 1] < last) ++start_band;
        ++start_band;
      }
      for (int b = start_band; b < 22; ++b) {
        const std::size_t src = z(b == 21 ? 20 : b);
        apply(bands.l[z(b)], bands.l[z(b) + 1], right.l[src], h.lsf() ? right.l_max[src] : mpeg1_illegal);
      }
    } else {
      // Short (or mixed) blocks: lines are stored window after window per band.
      const std::size_t first_short = g.mixed ? 3 : 0;
      const int short_start = g.mixed ? bands.l[h.lsf() ? 6 : 8] : 0;
      bool any_short = false;
      for (std::size_t w = 0; w < 3; ++w) {
        // Last non-zero band of this window in the right channel.
        int start_band = static_cast<int>(first_short);
        for (std::size_t b = first_short; b < 13; ++b) {
          const int width = bands.s[b + 1] - bands.s[b];
          const int from = short_start + 3 * (bands.s[b] - bands.s[first_short]) + static_cast<int>(w) * width;
          for (int k = from; k < from + width; ++k) {
            if (r[z(k)] != 0.0) {
              start_band = static_cast<int>(b) + 1;
              any_short = true;
              break;
            }
          }
        }
        for (std::size_t b = z(start_band); b < 13; ++b) {
          const int width = bands.s[b + 1] - bands.s[b];
          const int from = short_start + 3 * (bands.s[b] - bands.s[first_short]) + static_cast<int>(w) * width;
          const std::size_t src = b == 12 ? 11 : b;
          apply(from, from + width, right.s[src][w], h.lsf() ? right.s_max[src] : mpeg1_illegal);
        }
      }
      if (g.mixed && !any_short) {
        int start_band = 0;
        const int last = std::min(nonzero[1], short_start);
        if (last > 0) {
          while (start_band < 21 && bands.l[z(start_band) + 1] < last) ++start_band;
          ++start_band;
        }
        const int long_bands = h.lsf() ? 6 : 8;
        for (int b = start_band; b < long_bands; ++b) {
          apply(bands.l[z(b)], bands.l[z(b) + 1], right.l[z(b)], h.lsf() ? right.l_max[z(b)] : mpeg1_illegal);
        }
      }
    }
  }
  if (ms) {
    const int limit = std::max(nonzero[0], nonzero[1]);
    constexpr double s = std::numbers::sqrt2 / 2.0;
    for (int k = 0; k < 576; ++k) {
      if (done[z(k)]) continue;
      if (k >= limit && !intensity) break;
      const double m = l[z(k)];
      const double d = r[z(k)];
      l[z(k)] = (m + d) * s;
      r[z(k)] = (m - d) * s;
    }
  }
  nonzero[0] = nonzero[1] = 576;
}

void FrameDecoder::State::reorder(const Granule& g, const Bands& bands, bool lsf, int c) {
  if (!(g.window_switching && g.block_type == 2)) return;
  auto& x = xr[z(c)];
  std::array<double, 576> tmp{};
  const std::size_t first_short = g.mixed ? 3 : 0;
  const int start = g.mixed ? bands.l[lsf ? 6 : 8] : 0;
  for (int k = 0; k < start; ++k) tmp[z(k)] = x[z(k)];
  for (std::size_t b = first_short; b < 13; ++b) {
    const int width = bands.s[b + 1] - bands.s[b];
    const int from = start + 3 * (bands.s[b] - bands.s[first_short]);
    for (int w = 0; w < 3; ++w) {
      for (int k = 0; k < width; ++k) {
        const int dst = from + 3 * k + w;
        const int src = from + w * width + k;
        if (dst < 576 && src < 576) tmp[z(dst)] = x[z(src)];
      }
    }
  }
  x = tmp;
}

void FrameDecoder::State::hybrid(const Granule& g, const Bands& bands, bool lsf, int c) {
  auto& x = xr[z(c)];
  const bool short_blocks = g.window_switching && g.block_type == 2;
  const int long_subbands = !short_blocks ? 32 : (g.mixed ? bands.l[lsf ? 6 : 8] / 18 : 0);
  // Alias reduction between neighbouring long subbands.
  for (int sb = 1; sb < long_subbands; ++sb) {
    for (std::size_t i = 0; i < 8; ++i) {
      const std::size_t lo = z(sb) * 18 - 1 - i;
      const std::size_t hi = z(sb) * 18 + i;
      const double a = x[lo];
      const double b = x[hi];
      x[lo] = a * cs[i] - b * ca[i];
      x[hi] = b * cs[i] + a * ca[i];
    }
  }
  ChannelState& state = ch[z(c)];
  std::array<double, 36> y{};
  for (std::size_t sb = 0; sb < 32; ++sb) {
    const double* in = x.data() + sb * 18;
    double* overlap = state.overlap.data() + sb * 18;
    bool zero = true;
    for (std::size_t k = 0; k < 18; ++k) {
      if (in[k] != 0.0) {
        zero = false;
        break;
      }
    }
    if (zero) {
      for (std::size_t i = 0; i < 18; ++i) {
        hybrid_out[sb][i] = overlap[i];
        overlap[i] = 0.0;
      }
    } else if (static_cast<int>(sb) < long_subbands) {
      const int type = short_blocks ? 0 : g.block_type;
      const auto& win = imdct_window[z(type)];
      for (std::size_t i = 0; i < 36; ++i) {
        double sum = 0.0;
        for (std::size_t k = 0; k < 18; ++k) sum += in[k] * cos36[i][k];
        y[i] = sum * win[i];
      }
      for (std::size_t i = 0; i < 18; ++i) {
        hybrid_out[sb][i] = y[i] + overlap[i];
        overlap[i] = y[i + 18];
      }
    } else {
      y.fill(0.0);
      for (std::size_t w = 0; w < 3; ++w) {
        for (std::size_t i = 0; i < 12; ++i) {
          double sum = 0.0;
          for (std::size_t k = 0; k < 6; ++k) sum += in[3 * k + w] * cos12[i][k];
          y[6 + 6 * w + i] += sum * imdct_window[2][i];
        }
      }
      for (std::size_t i = 0; i < 18; ++i) {
        hybrid_out[sb][i] = y[i] + overlap[i];
        overlap[i] = y[i + 18];
      }
    }
    if ((sb & 1) != 0) {
      for (std::size_t i = 1; i < 18; i += 2) hybrid_out[sb][i] = -hybrid_out[sb][i];
    }
  }
}

Result<void> FrameDecoder::State::decode_layer3(const FrameHeader& h, ByteView frame, std::vector<float>& out) {
  const int nch = h.channels();
  const auto bytes = as_u8(frame);
  const std::size_t side_start = 4 + (h.crc ? 2 : 0);
  const std::size_t side_end = side_start + h.side_info_bytes();
  if (bytes.size() < side_end) return fail(Status::Decode, "MPEG audio frame is truncated");
  Bits side(bytes.data() + side_start, h.side_info_bytes());
  SideInfo si;
  if (!read_side_info(h, side, si)) return fail(Status::Decode, "invalid Layer III side information");
  const auto main = bytes.subspan(side_end);
  const std::size_t granules = h.lsf() ? 1 : 2;
  const std::size_t base = out.size();
  out.resize(base + granules * 576 * z(nch), 0.0f);
  const auto md = z(si.main_data_begin);
  const bool available = md <= reservoir.size();
  std::vector<std::uint8_t> buffer;
  if (available) {
    buffer.reserve(md + main.size());
    buffer.insert(buffer.end(), reservoir.end() - static_cast<std::ptrdiff_t>(md), reservoir.end());
    buffer.insert(buffer.end(), main.begin(), main.end());
  }
  reservoir.insert(reservoir.end(), main.begin(), main.end());
  if (reservoir.size() > max_reservoir) {
    reservoir.erase(reservoir.begin(), reservoir.end() - static_cast<std::ptrdiff_t>(max_reservoir));
  }
  const Bands bands = make_bands(h.rate_slot());
  Bits bits(buffer.data(), buffer.size());
  std::array<double, 32> subband{};
  for (std::size_t gr = 0; gr < granules; ++gr) {
    bool ok = available;
    std::array<Granule, 2> gi = {si.gr[gr][0], si.gr[gr][1]};
    for (int c = 0; c < nch && ok; ++c) {
      Granule& g = gi[z(c)];
      const std::uint64_t start = bits.pos();
      const std::uint64_t end = start + static_cast<std::uint64_t>(g.part2_3_length);
      if (end > bits.size()) {
        ok = false;
        break;
      }
      if (h.lsf()) {
        const bool intensity_right = c == 1 && h.mode == 1 && (h.mode_ext & 1) != 0;
        if (read_scalefactors_lsf(bits, g, c, intensity_right)) g.preflag = true;
      } else {
        read_scalefactors_mpeg1(bits, g, si, static_cast<int>(gr), c);
      }
      if (bits.pos() > end) {
        ok = false;
        break;
      }
      if (!huffman(bits, g, bands, end, c)) {
        ok = false;
        break;
      }
      const Scalefactors& s = h.lsf() ? sf[0][z(c)] : sf[gr][z(c)];
      requantize(g, bands, s, h.lsf(), c);
    }
    if (!ok) {
      // Unusable data (missing reservoir or damage): this granule is silent.
      for (int c = 0; c < nch; ++c) {
        xr[z(c)].fill(0.0);
        nonzero[z(c)] = 0;
      }
    } else if (nch == 2 && h.mode == 1 && h.mode_ext != 0) {
      const Scalefactors& right = h.lsf() ? sf[0][1] : sf[gr][1];
      stereo(h, gi[0], bands, right, gi[1].scalefac_compress & 1);
    }
    for (int c = 0; c < nch; ++c) {
      const Granule& g = gi[z(c)];
      reorder(g, bands, h.lsf(), c);
      hybrid(g, bands, h.lsf(), c);
      for (std::size_t t = 0; t < 18; ++t) {
        for (std::size_t sb = 0; sb < 32; ++sb) subband[sb] = hybrid_out[sb][t];
        synthesize(ch[z(c)], subband.data(), out.data() + base + (gr * 576 + t * 32) * z(nch) + z(c), z(nch));
      }
    }
  }
  return {};
}

// =========================================================================== public

FrameDecoder::FrameDecoder() : state_(std::make_unique<State>()) {}
FrameDecoder::~FrameDecoder() = default;

void FrameDecoder::reset() { state_->reset(); }

Result<void> FrameDecoder::decode(ByteView frame, std::vector<float>& out) {
  const auto header = parse_header(header_word(frame, 0));
  if (!header) return fail(Status::Decode, "invalid MPEG audio frame header");
  if (channels_ == 0) {
    channels_ = header->channels();
    sample_rate_ = header->sample_rate;
  } else if (channels_ != header->channels() || sample_rate_ != header->sample_rate) {
    return fail(Status::Decode, "the MPEG audio stream changes its format midway");
  }
  if (header->layer == 3) return state_->decode_layer3(*header, frame, out);
  const std::size_t skip = 4 + (header->crc ? 2 : 0);
  if (frame.size() < skip) return fail(Status::Decode, "MPEG audio frame is truncated");
  Bits bits(as_u8(frame).data() + skip, frame.size() - skip);
  return header->layer == 1 ? state_->decode_layer1(*header, bits, out) : state_->decode_layer2(*header, bits, out);
}

}  // namespace cvt::lossy::mpeg
