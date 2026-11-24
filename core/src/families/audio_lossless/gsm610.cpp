// Convertible core - audio_lossless family: GSM 06.10 full-rate decoder
// (RPE-LTP, ETSI EN 300 961) for Microsoft "WAV49" GSM in WAVE files.
//
// The arithmetic follows the bit-exact fixed-point description of the
// standard: 16-bit saturating add/subtract, rounded fractional multiply.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "families/audio_lossless/pcm.hpp"

namespace cvt::audio {

namespace {

using word = std::int16_t;
constexpr int min_word = -32768;
constexpr int max_word = 32767;

word saturate(std::int32_t v) noexcept {
  return static_cast<word>(v < min_word ? min_word : v > max_word ? max_word : v);
}
word add(int a, int b) noexcept { return saturate(a + b); }
word sub(int a, int b) noexcept { return saturate(a - b); }
word mult_r(int a, int b) noexcept {
  if (a == min_word && b == min_word) return static_cast<word>(max_word);
  return static_cast<word>(static_cast<std::int16_t>(static_cast<std::uint16_t>(
      static_cast<std::uint32_t>((static_cast<std::int32_t>(a) * b + 16384) >> 15) & 0xFFFFu)));
}
int sasr(int x, int n) noexcept { return x >> n; }  // arithmetic shift of a non-negative count
word asr(int a, int n) noexcept;
word asl(int a, int n) noexcept {
  if (n >= 16) return 0;
  if (n <= -16) return static_cast<word>(-(a < 0));
  if (n < 0) return asr(a, -n);
  return static_cast<word>(a * (1 << n));
}
word asr(int a, int n) noexcept {
  if (n >= 16) return static_cast<word>(-(a < 0));
  if (n <= -16) return 0;
  if (n < 0) return static_cast<word>(a * (1 << -n));
  return static_cast<word>(a >> n);
}

constexpr std::array<int, 8> fac = {18431, 20479, 22527, 24575, 26623, 28671, 30719, 32767};
constexpr std::array<int, 4> qlb = {3277, 11469, 21299, 32767};

struct Frame {
  std::array<int, 8> larc{};
  std::array<int, 4> nc{}, bc{}, mc{}, xmaxc{};
  std::array<int, 52> xmc{};
};

class BitReader {
 public:
  explicit BitReader(const std::uint8_t* data) : data_(data) {}
  int take(int bits) {
    while (count_ < bits) {
      buffer_ |= static_cast<std::uint32_t>(data_[pos_++]) << count_;
      count_ += 8;
    }
    const int v = static_cast<int>(buffer_ & ((1u << bits) - 1u));
    buffer_ >>= bits;
    count_ -= bits;
    return v;
  }

 private:
  const std::uint8_t* data_;
  std::size_t pos_ = 0;
  std::uint32_t buffer_ = 0;
  int count_ = 0;
};

Frame unpack(BitReader& bits) {
  Frame f;
  static constexpr std::array<int, 8> lar_bits = {6, 6, 5, 5, 4, 4, 3, 3};
  for (std::size_t i = 0; i < 8; ++i) f.larc[i] = bits.take(lar_bits[i]);
  for (std::size_t j = 0; j < 4; ++j) {
    f.nc[j] = bits.take(7);
    f.bc[j] = bits.take(2);
    f.mc[j] = bits.take(2);
    f.xmaxc[j] = bits.take(6);
    for (std::size_t k = 0; k < 13; ++k) f.xmc[j * 13 + k] = bits.take(3);
  }
  return f;
}

class Decoder {
 public:
  void decode(const Frame& f, std::array<word, 160>& out) {
    std::array<word, 160> wt{};
    for (std::size_t j = 0; j < 4; ++j) {
      std::array<word, 40> erp{};
      rpe_decoding(f.xmaxc[j], f.mc[j], &f.xmc[j * 13], erp);
      long_term_synthesis(f.nc[j], f.bc[j], erp);
      for (std::size_t k = 0; k < 40; ++k) wt[j * 40 + k] = dp0_[120 + k];
    }
    short_term_synthesis(f.larc, wt, out);
    // Deemphasis, upscaling and truncation to 13 bits.
    for (word& s : out) {
      const word tmp = mult_r(msr_, 28180);
      msr_ = add(s, tmp);
      s = static_cast<word>(add(msr_, msr_) & 0xFFF8);
    }
  }

 private:
  static void rpe_decoding(int xmaxc, int mc, const int* xmc, std::array<word, 40>& erp) {
    int exp = 0;
    if (xmaxc > 15) exp = sasr(xmaxc, 3) - 1;
    int mant = xmaxc - (exp << 3);
    if (mant == 0) {
      exp = -4;
      mant = 7;
    } else {
      while (mant <= 7) {
        mant = mant << 1 | 1;
        --exp;
      }
      mant -= 8;
    }
    const int temp1 = fac[static_cast<std::size_t>(mant)];
    const word temp2 = sub(6, exp);
    const word temp3 = asl(1, sub(temp2, 1));
    std::array<word, 13> xmp{};
    for (std::size_t i = 0; i < 13; ++i) {
      int temp = (xmc[i] << 1) - 7;
      temp <<= 12;
      word t = mult_r(temp1, temp);
      t = add(t, temp3);
      xmp[i] = asr(t, temp2);
    }
    erp.fill(0);
    for (std::size_t i = 0; i < 13; ++i) erp[static_cast<std::size_t>(mc) + 3 * i] = xmp[i];
  }

  void long_term_synthesis(int ncr, int bcr, const std::array<word, 40>& erp) {
    const int nr = (ncr < 40 || ncr > 120) ? nrp_ : ncr;
    nrp_ = nr;
    const int brp = qlb[static_cast<std::size_t>(bcr)];
    word* drp = dp0_.data() + 120;
    for (int k = 0; k < 40; ++k) {
      const word drpp = mult_r(brp, drp[k - nr]);
      drp[k] = add(erp[static_cast<std::size_t>(k)], drpp);
    }
    for (std::size_t k = 0; k < 120; ++k) dp0_[k] = dp0_[k + 40];
  }

  static void decode_lars(const std::array<int, 8>& larc, std::array<word, 8>& larpp) {
    static constexpr std::array<int, 8> b = {0, 0, 2048, -2560, 94, -1792, -341, -1144};
    static constexpr std::array<int, 8> mic = {-32, -32, -16, -16, -8, -8, -4, -4};
    static constexpr std::array<int, 8> inva = {13107, 13107, 13107, 13107, 19223, 17476, 31454, 29708};
    for (std::size_t i = 0; i < 8; ++i) {
      word temp1 = static_cast<word>(add(larc[i], mic[i]) * 1024);
      temp1 = sub(temp1, b[i] * 2);
      temp1 = mult_r(inva[i], temp1);
      larpp[i] = add(temp1, temp1);
    }
  }

  static void larp_to_rp(std::array<word, 8>& larp) {
    for (word& v : larp) {
      if (v < 0) {
        const int temp = v == min_word ? max_word : -v;
        v = static_cast<word>(-((temp < 11059) ? temp << 1 : ((temp < 20070) ? temp + 11059 : add(temp >> 2, 26112))));
      } else {
        const int temp = v;
        v = static_cast<word>((temp < 11059) ? temp << 1 : ((temp < 20070) ? temp + 11059 : add(temp >> 2, 26112)));
      }
    }
  }

  void synthesis_filter(const std::array<word, 8>& rrp, int count, const word* wt, word* sr) {
    for (int k = 0; k < count; ++k) {
      word sri = wt[k];
      for (int i = 7; i >= 0; --i) {
        const auto ui = static_cast<std::size_t>(i);
        const word tmp1 = rrp[ui];
        const word tmp2 = mult_r(tmp1, v_[ui]);
        sri = sub(sri, tmp2);
        const word tmp3 = mult_r(tmp1, sri);
        v_[ui + 1] = add(v_[ui], tmp3);
      }
      sr[k] = v_[0] = sri;
    }
  }

  void short_term_synthesis(const std::array<int, 8>& larc, const std::array<word, 160>& wt,
                            std::array<word, 160>& s) {
    std::array<word, 8>& larpp_j = larpp_[j_];
    j_ ^= 1;
    const std::array<word, 8>& larpp_j1 = larpp_[j_];
    decode_lars(larc, larpp_j);
    std::array<word, 8> larp{};
    for (std::size_t i = 0; i < 8; ++i) {
      larp[i] = add(sasr(larpp_j1[i], 2), sasr(larpp_j[i], 2));
      larp[i] = add(larp[i], sasr(larpp_j1[i], 1));
    }
    larp_to_rp(larp);
    synthesis_filter(larp, 13, wt.data(), s.data());
    for (std::size_t i = 0; i < 8; ++i) larp[i] = add(sasr(larpp_j1[i], 1), sasr(larpp_j[i], 1));
    larp_to_rp(larp);
    synthesis_filter(larp, 14, wt.data() + 13, s.data() + 13);
    for (std::size_t i = 0; i < 8; ++i) {
      larp[i] = add(sasr(larpp_j1[i], 2), sasr(larpp_j[i], 2));
      larp[i] = add(larp[i], sasr(larpp_j[i], 1));
    }
    larp_to_rp(larp);
    synthesis_filter(larp, 13, wt.data() + 27, s.data() + 27);
    larp = larpp_j;
    larp_to_rp(larp);
    synthesis_filter(larp, 120, wt.data() + 40, s.data() + 40);
  }

  std::array<word, 280> dp0_{};
  std::array<std::array<word, 8>, 2> larpp_{};
  std::size_t j_ = 0;
  int nrp_ = 40;
  std::array<word, 9> v_{};
  word msr_ = 0;
};

}  // namespace

Result<void> decode_gsm610_wav(ByteView data, std::vector<float>& out) {
  const std::span<const std::uint8_t> bytes = as_u8(data);
  Decoder decoder;
  std::array<word, 160> pcm{};
  for (std::size_t at = 0; at + 65 <= bytes.size(); at += 65) {
    BitReader bits(bytes.data() + at);
    for (int half = 0; half < 2; ++half) {
      const Frame frame = unpack(bits);
      decoder.decode(frame, pcm);
      for (const word s : pcm) out.push_back(static_cast<float>(s) / 32768.0f);
    }
  }
  return {};
}

}  // namespace cvt::audio
