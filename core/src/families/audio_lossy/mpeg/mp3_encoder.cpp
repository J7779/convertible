// Convertible core - audio_lossy family: the in-house MPEG-1/2/2.5 Layer III
// encoder, written from ISO/IEC 11172-3 and 13818-3.
//
//   analysis polyphase filterbank -> MDCT (long/start/short/stop windows,
//   alias-reduction butterflies) -> psychoacoustic model (FFT, Bark-domain
//   spreading, tonality from spectral flatness, absolute threshold of
//   hearing, pre-echo limiting, attack detection for block switching) ->
//   per-band noise allocation (global gain + scale factors chosen so that
//   the quantisation noise of every band stays below its allowed level) ->
//   Huffman table and region selection -> frames with mid/side stereo
//   decided per frame, a bit reservoir and CBR, ABR or VBR rate control.
//
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "util/bytes.hpp"
#include "families/audio_lossy/mpeg/mp3_encoder.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <numbers>
#include <vector>

#include "families/audio_lossless/bits.hpp"
#include "families/audio_lossy/mpeg/mpeg_audio.hpp"
#include "families/audio_lossy/mpeg/mpeg_huffman.hpp"
#include "families/audio_lossy/mpeg/mpeg_psy.hpp"
#include "families/audio_lossy/mpeg/mpeg_tables.hpp"

namespace cvt::lossy::mpeg {

namespace {

template <class T>
constexpr std::size_t z(T v) noexcept {
  return static_cast<std::size_t>(v);
}

constexpr double pi = std::numbers::pi;
constexpr int max_ix = 8206;

// ------------------------------------------------------------------ Huffman

struct TableInfo {
  const HuffCode* codes = nullptr;
  int size = 0;     ///< values per dimension (x and y range 0..size-1)
  int linbits = 0;  ///< escape bits when |value| >= 15
  int max = 0;      ///< largest value the table codes
};

TableInfo table_info(int t) {
  TableInfo i;
  switch (t) {
    case 1: i = {huff_table1.data(), 2, 0, 1}; break;
    case 2: i = {huff_table2.data(), 3, 0, 2}; break;
    case 3: i = {huff_table3.data(), 3, 0, 2}; break;
    case 5: i = {huff_table5.data(), 4, 0, 3}; break;
    case 6: i = {huff_table6.data(), 4, 0, 3}; break;
    case 7: i = {huff_table7.data(), 6, 0, 5}; break;
    case 8: i = {huff_table8.data(), 6, 0, 5}; break;
    case 9: i = {huff_table9.data(), 6, 0, 5}; break;
    case 10: i = {huff_table10.data(), 8, 0, 7}; break;
    case 11: i = {huff_table11.data(), 8, 0, 7}; break;
    case 12: i = {huff_table12.data(), 8, 0, 7}; break;
    case 13: i = {huff_table13.data(), 16, 0, 15}; break;
    case 15: i = {huff_table15.data(), 16, 0, 15}; break;
    default:
      if (t >= 16 && t < 24) i = {huff_table16.data(), 16, linbits_table[t], 15 + (1 << linbits_table[t]) - 1};
      if (t >= 24 && t < 32) i = {huff_table24.data(), 16, linbits_table[t], 15 + (1 << linbits_table[t]) - 1};
      break;
  }
  return i;
}

constexpr std::array<int, 29> all_tables = {1,  2,  3,  5,  6,  7,  8,  9,  10, 11, 12, 13, 15, 16, 17,
                                            18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};

/// Bits of one pair with table `t` (|x|, |y| already within range).
int pair_bits(const TableInfo& t, int x, int y) {
  int bits = 0;
  int cx = x, cy = y;
  if (t.linbits != 0) {
    if (cx >= 15) {
      bits += t.linbits;
      cx = 15;
    }
    if (cy >= 15) {
      bits += t.linbits;
      cy = 15;
    }
  }
  bits += t.codes[z(cx * t.size + cy)].length;
  if (x != 0) ++bits;
  if (y != 0) ++bits;
  return bits;
}

int range_bits(const TableInfo& t, const int* ix, int from, int to) {
  int bits = 0;
  for (int i = from; i < to; i += 2) bits += pair_bits(t, std::abs(ix[i]), std::abs(ix[i + 1]));
  return bits;
}

int range_max(const int* ix, int from, int to) {
  int m = 0;
  for (int i = from; i < to; ++i) m = std::max(m, std::abs(ix[i]));
  return m;
}

/// Best table for a range: {table, bits}.
std::pair<int, int> best_table(const int* ix, int from, int to) {
  if (from >= to) return {0, 0};
  const int m = range_max(ix, from, to);
  if (m == 0) return {0, 0};
  int best = -1, best_bits = std::numeric_limits<int>::max();
  bool linbits_done16 = false, linbits_done24 = false;
  for (const int t : all_tables) {
    const TableInfo info = table_info(t);
    if (info.max < m) continue;
    if (t >= 16 && t < 24) {
      if (linbits_done16) continue;
      linbits_done16 = true;  // the smallest linbits that fits is the cheapest
    }
    if (t >= 24) {
      if (linbits_done24) continue;
      linbits_done24 = true;
    }
    const int bits = range_bits(info, ix, from, to);
    if (bits < best_bits) {
      best_bits = bits;
      best = t;
    }
  }
  return {best, best_bits};
}

void write_pair(audio::BitWriter& w, const TableInfo& t, int x, int y) {
  const int ax = std::abs(x), ay = std::abs(y);
  int cx = ax, cy = ay;
  if (t.linbits != 0) {
    cx = std::min(ax, 15);
    cy = std::min(ay, 15);
  }
  const HuffCode c = t.codes[z(cx * t.size + cy)];
  w.write(c.code, c.length);
  if (t.linbits != 0 && cx == 15) w.write(static_cast<std::uint32_t>(ax - 15), static_cast<unsigned>(t.linbits));
  if (ax != 0) w.write(x < 0 ? 1 : 0, 1);
  if (t.linbits != 0 && cy == 15) w.write(static_cast<std::uint32_t>(ay - 15), static_cast<unsigned>(t.linbits));
  if (ay != 0) w.write(y < 0 ? 1 : 0, 1);
}

int quad_bits(const int* ix, int from, int to, bool table_b) {
  int bits = 0;
  for (int i = from; i < to; i += 4) {
    const int v = (ix[i] != 0 ? 8 : 0) | (ix[i + 1] != 0 ? 4 : 0) | (ix[i + 2] != 0 ? 2 : 0) | (ix[i + 3] != 0 ? 1 : 0);
    bits += table_b ? 4 : count1_table_a[z(v)].length;
    for (int k = 0; k < 4; ++k) bits += ix[i + k] != 0 ? 1 : 0;
  }
  return bits;
}

struct Bands {
  std::array<int, 23> l{};
  std::array<int, 14> s{};
};

/// One channel of one granule, ready for quantisation.
struct GranuleChannel {
  int block_type = 0;
  std::array<double, 576> xr{};       ///< MDCT lines; short blocks in band/window order
  std::array<double, 576> xr34{};     ///< |xr|^(3/4)
  std::array<double, 39> energy{};    ///< per band (long: 22; short: 13 x 3)
  std::array<double, 39> allowed{};   ///< allowed noise per band before the quality offset
  int bands = 22;
};

/// Quantised result of one granule channel.
struct Coded {
  std::array<int, 576> ix{};
  int global_gain = 210;
  bool scalefac_scale = false;
  std::array<int, 39> sf{};  ///< per band (short: band * 3 + window)
  int scalefac_compress = 0;
  int part2_bits = 0;
  int big_values = 0;
  std::array<int, 3> tables{};
  int region0 = 0, region1 = 0;
  bool count1_b = false;
  int count1_end = 0;
  int part3_bits = 0;
  [[nodiscard]] int bits() const noexcept { return part2_bits + part3_bits; }
};

/// CRC-16/ARC as used by the LAME tag.
std::uint16_t crc16_arc(const std::uint8_t* data, std::size_t size, std::uint16_t crc = 0) {
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) crc = static_cast<std::uint16_t>((crc & 1) != 0 ? (crc >> 1) ^ 0xA001 : crc >> 1);
  }
  return crc;
}

}  // namespace

// =========================================================================== implementation

struct Layer3Encoder::Impl {
  // ---- configuration
  std::uint32_t rate = 44100;
  int nch = 2;
  int version = 0;  // 0 MPEG-1, 1 MPEG-2, 2 MPEG-2.5
  int rate_index = 0;
  int granules = 2;
  Mp3Settings settings;
  Bands bands;
  int lowpass_line = 576;
  double quality_db = 0.0;  // offset applied to the allowed noise (dB)
  int max_back = 511;
  std::size_t side_bytes = 32;
  int min_bitrate_index = 1;
  int cbr_index = 9;

  // ---- filterbank
  std::array<double, 512> analysis_window{};
  std::array<std::array<double, 64>, 32> analysis_matrix{};
  std::array<std::array<double, 512>, 2> fifo{};  // polyphase input history per channel
  std::array<std::array<double, 18>, 36> mdct_long{};   // [n][k]
  std::array<std::array<double, 6>, 12> mdct_short{};
  std::array<std::array<double, 36>, 4> windows{};
  std::array<double, 8> cs{}, ca{};

  // ---- psychoacoustics
  PsyBank psy_long, psy_short;
  std::array<std::array<double, 22>, 2> previous_threshold{};
  std::array<bool, 2> have_previous{};
  std::array<double, 2> hp_state{};
  std::array<std::vector<double>, 2> hp_energy_history;

  // ---- input
  std::array<std::vector<double>, 2> input;  // planar, from absolute sample `input_base`
  std::uint64_t input_base = 0;
  std::uint64_t input_total = 0;   // real samples pushed
  std::uint64_t padded_total = 0;  // including flush padding
  bool flushed = false;

  // ---- granule pipeline
  std::uint64_t next_subband_granule = 0;  // next granule whose subband samples are computed
  std::uint64_t next_coded_granule = 0;
  std::deque<std::array<std::array<std::array<double, 32>, 18>, 2>> subbands;  // from granule `subband_base`
  std::uint64_t subband_base = 0;
  std::deque<bool> attacks;  // attack flag per granule from `subband_base`
  int previous_type = 0;
  std::uint64_t total_granules = 0;  // known once flushed

  // ---- frame assembly
  std::vector<GranuleChannel> pending;  // granule channels of the frame being collected
  std::vector<int> pending_types;
  struct PendingFrame {
    Bytes head;              // header + side information
    std::uint64_t payload_start = 0;
    std::uint64_t payload_size = 0;
  };
  std::deque<PendingFrame> frames;
  std::vector<std::uint8_t> stream;  // payload bytes from `stream_base`
  std::uint64_t stream_base = 0;
  std::uint64_t payload_pos = 0;  // P: start of the next frame's payload
  std::uint64_t write_pos = 0;    // W: end of the main data written so far
  std::uint64_t padding_acc = 0;
  std::vector<Bytes> done;
  std::uint64_t abr_bits_target = 0;  // ABR/CBR: cumulative average budget
  std::uint64_t bits_spent = 0;
  std::uint32_t frames_written = 0;

  [[nodiscard]] bool lsf() const noexcept { return version != 0; }
  [[nodiscard]] std::uint32_t spf() const noexcept { return lsf() ? 576u : 1152u; }

  Result<void> configure(std::uint32_t sample_rate, int channels, const Mp3Settings& s);
  void analysis(int c, const double* samples, std::array<double, 32>& out);
  void compute_subbands();
  void mdct(int c, std::uint64_t g, int type, GranuleChannel& gc);
  bool detect_attack(std::uint64_t g);
  void psycho(int c, std::uint64_t g, GranuleChannel& gc);
  void quantize(const GranuleChannel& gc, double offset_db, Coded& out) const;
  void huffman_choose(const GranuleChannel& gc, Coded& out) const;
  void scalefactor_bits(const GranuleChannel& gc, Coded& out) const;
  Result<void> encode_frame(std::vector<GranuleChannel>& gcs, const std::vector<int>& types);
  void emit_frame(std::vector<Coded>& coded, const std::vector<GranuleChannel>& gcs, bool ms, int bitrate_index,
                  bool padding);
  void finalize_frames(bool all);
  double sample(int c, std::uint64_t n) const {
    if (n < input_base) return 0.0;
    const std::uint64_t i = n - input_base;
    if (i >= input[z(c)].size()) return 0.0;
    return input[z(c)][z(i)];
  }
  std::size_t frame_size(int bitrate_index, bool padding) const {
    const std::uint32_t kbps = bitrates[lsf() ? 1 : 0][2][bitrate_index];
    const std::uint64_t num = std::uint64_t{lsf() ? 72000u : 144000u} * kbps;
    return static_cast<std::size_t>(num / rate + (padding ? 1 : 0));
  }
  std::uint32_t encoder_delay() const noexcept { return 576 + 481 - 529; }
};

Result<void> Layer3Encoder::Impl::configure(std::uint32_t sample_rate, int channels, const Mp3Settings& s) {
  settings = s;
  rate = sample_rate;
  nch = channels;
  bool found = false;
  for (int v = 0; v < 3 && !found; ++v) {
    for (int r = 0; r < 3; ++r) {
      if (sample_rates[v][r] == sample_rate) {
        version = v;
        rate_index = r;
        found = true;
        break;
      }
    }
  }
  if (!found) return fail(Status::InvalidArgument, "MP3 does not support a sample rate of {} Hz", sample_rate);
  if (channels < 1 || channels > 2) return fail(Status::InvalidArgument, "MP3 holds one or two channels");
  granules = lsf() ? 1 : 2;
  max_back = lsf() ? 255 : 511;
  side_bytes = lsf() ? (nch == 1 ? 9 : 17) : (nch == 1 ? 17 : 32);
  const int slot = version * 3 + rate_index;
  for (std::size_t i = 0; i < 22; ++i) bands.l[i + 1] = bands.l[i] + sfb_long_widths[slot][i];
  for (std::size_t i = 0; i < 13; ++i) bands.s[i + 1] = bands.s[i] + sfb_short_widths[slot][i];

  const int q = std::clamp(s.quality, 1, 100);
  // Quality -> allowed-noise offset (dB above the masking threshold) and lowpass.
  const auto lerp = [q](std::initializer_list<std::pair<int, double>> pts) {
    auto it = pts.begin();
    auto prev = *it;
    for (; it != pts.end(); ++it) {
      if (q <= it->first) {
        if (it->first == prev.first) return it->second;
        const double t = static_cast<double>(q - prev.first) / static_cast<double>(it->first - prev.first);
        return prev.second + (it->second - prev.second) * t;
      }
      prev = *it;
    }
    return prev.second;
  };
  quality_db = lerp({{1, 12.0}, {10, 9.0}, {30, 4.0}, {50, 0.0}, {60, -2.0}, {75, -5.0}, {90, -9.0}, {100, -14.0}});
  double lowpass_hz = lerp({{1, 5500.0}, {10, 7000.0}, {30, 11000.0}, {50, 14500.0}, {60, 15500.0}, {75, 17000.0},
                            {90, 19500.0}, {100, 20500.0}});
  if (settings.mode != Mp3Settings::Mode::Vbr && settings.bitrate) {
    // Bit-rate driven: the lowpass follows the bit rate per channel.
    const double per_channel = static_cast<double>(*settings.bitrate) / nch;
    lowpass_hz = std::clamp(5000.0 + per_channel * 110.0, 5000.0, 20000.0);
    if (per_channel >= 128) lowpass_hz = 20000.0;
  }
  lowpass_hz = std::min(lowpass_hz, rate * 0.5 * 0.97);
  lowpass_line = std::clamp(static_cast<int>(lowpass_hz / (rate * 0.5) * 576.0), 8, 576);

  // CBR bit rate index.
  if (settings.bitrate) {
    int best = 1;
    for (int i = 1; i < 15; ++i) {
      if (bitrates[lsf() ? 1 : 0][2][i] <= *settings.bitrate) best = i;
    }
    cbr_index = best;
  }

  // Filterbank tables.
  for (std::size_t i = 0; i < 512; ++i) analysis_window[i] = synthesis_window[i] / 65536.0 / 32.0;
  for (std::size_t k = 0; k < 32; ++k) {
    for (std::size_t i = 0; i < 64; ++i) {
      analysis_matrix[k][i] = std::cos(static_cast<double>(2 * k + 1) * (static_cast<double>(i) - 16.0) * pi / 64.0);
    }
  }
  for (std::size_t n = 0; n < 36; ++n) {
    for (std::size_t k = 0; k < 18; ++k) {
      mdct_long[n][k] = std::cos(pi / 72.0 * static_cast<double>((2 * n + 1 + 18) * (2 * k + 1)));
    }
  }
  for (std::size_t n = 0; n < 12; ++n) {
    for (std::size_t k = 0; k < 6; ++k) {
      mdct_short[n][k] = std::cos(pi / 24.0 * static_cast<double>((2 * n + 1 + 6) * (2 * k + 1)));
    }
  }
  for (std::size_t i = 0; i < 36; ++i) {
    const double di = static_cast<double>(i);
    windows[0][i] = std::sin(pi / 36.0 * (di + 0.5));
    windows[1][i] = i < 18 ? std::sin(pi / 36.0 * (di + 0.5)) : i < 24 ? 1.0 : i < 30 ? std::sin(pi / 12.0 * (di - 18.0 + 0.5)) : 0.0;
    windows[3][i] = i < 6 ? 0.0 : i < 12 ? std::sin(pi / 12.0 * (di - 6.0 + 0.5)) : i < 18 ? 1.0 : std::sin(pi / 36.0 * (di + 0.5));
    windows[2][i] = i < 12 ? std::sin(pi / 12.0 * (di + 0.5)) : 0.0;
  }
  for (std::size_t i = 0; i < 8; ++i) {
    const double c = alias_coefficients[i];
    cs[i] = 1.0 / std::sqrt(1.0 + c * c);
    ca[i] = c / std::sqrt(1.0 + c * c);
  }
  const double ath_offset = lerp({{1, 6.0}, {50, 3.0}, {75, 0.0}, {90, -4.0}, {100, -8.0}});
  psy_long.init(1024, rate, ath_offset, 0.34);
  psy_short.init(256, rate, ath_offset, 0.8);
  return {};
}

// --------------------------------------------------------------------------- filterbank

void Layer3Encoder::Impl::analysis(int c, const double* samples, std::array<double, 32>& out) {
  auto& x = fifo[z(c)];
  ::cvt::util::move_bytes(x.data() + 32, x.data(), sizeof(double) * 480);
  for (std::size_t i = 0; i < 32; ++i) x[i] = samples[31 - i];
  std::array<double, 64> y{};
  for (std::size_t i = 0; i < 64; ++i) {
    double sum = 0.0;
    for (std::size_t j = 0; j < 8; ++j) sum += analysis_window[i + 64 * j] * x[i + 64 * j];
    y[i] = sum;
  }
  for (std::size_t k = 0; k < 32; ++k) {
    double sum = 0.0;
    for (std::size_t i = 0; i < 64; ++i) sum += analysis_matrix[k][i] * y[i];
    out[k] = sum;
  }
}

void Layer3Encoder::Impl::compute_subbands() {
  // Subband samples of granule g need input up to (g + 1) * 576.
  const std::uint64_t available = flushed ? padded_total : input_total;
  while ((next_subband_granule + 1) * 576 <= available) {
    std::array<std::array<std::array<double, 32>, 18>, 2> g{};
    for (int c = 0; c < nch; ++c) {
      std::array<double, 32> block{};
      for (std::size_t t = 0; t < 18; ++t) {
        const std::uint64_t first = next_subband_granule * 576 + t * 32;
        for (std::size_t i = 0; i < 32; ++i) block[i] = sample(c, first + i);
        analysis(c, block.data(), g[z(c)][t]);
      }
    }
    subbands.push_back(g);
    attacks.push_back(detect_attack(next_subband_granule));
    ++next_subband_granule;
  }
}

void Layer3Encoder::Impl::mdct(int c, std::uint64_t g, int type, GranuleChannel& gc) {
  const auto& cur = subbands[z(g - subband_base)][z(c)];
  static const std::array<std::array<double, 32>, 18> zero{};
  const auto& prev = g > subband_base ? subbands[z(g - subband_base - 1)][z(c)] : zero;
  std::array<double, 576> lines{};
  std::array<double, 36> zin{};
  for (std::size_t sb = 0; sb < 32; ++sb) {
    for (std::size_t t = 0; t < 18; ++t) {
      zin[t] = prev[t][sb];
      zin[t + 18] = cur[t][sb];
    }
    if ((sb & 1) != 0) {
      for (std::size_t t = 1; t < 36; t += 2) zin[t] = -zin[t];
    }
    double* out = lines.data() + sb * 18;
    if (type != 2) {
      const auto& w = windows[z(type)];
      for (std::size_t k = 0; k < 18; ++k) {
        double sum = 0.0;
        for (std::size_t n = 0; n < 36; ++n) sum += zin[n] * w[n] * mdct_long[n][k];
        out[k] = sum / 9.0;
      }
    } else {
      for (std::size_t w = 0; w < 3; ++w) {
        for (std::size_t k = 0; k < 6; ++k) {
          double sum = 0.0;
          for (std::size_t n = 0; n < 12; ++n) sum += zin[6 + 6 * w + n] * windows[2][n] * mdct_short[n][k];
          out[3 * k + w] = sum / 3.0;
        }
      }
    }
  }
  if (type != 2) {
    // Encoder side of the alias-reduction butterflies (inverse rotation).
    for (std::size_t sb = 1; sb < 32; ++sb) {
      for (std::size_t i = 0; i < 8; ++i) {
        const std::size_t lo = sb * 18 - 1 - i, hi = sb * 18 + i;
        const double a = lines[lo], b = lines[hi];
        lines[lo] = a * cs[i] + b * ca[i];
        lines[hi] = b * cs[i] - a * ca[i];
      }
    }
  }
  gc.block_type = type;
  // Lowpass with a short raised-cosine transition.
  const int width = std::max(4, lowpass_line / 24);
  const auto gain = [&](int freq_line) {
    if (freq_line >= lowpass_line) return 0.0;
    if (freq_line < lowpass_line - width) return 1.0;
    const double t = static_cast<double>(lowpass_line - freq_line) / width;
    return 0.5 - 0.5 * std::cos(pi * t);
  };
  if (type != 2) {
    for (int i = 0; i < 576; ++i) gc.xr[z(i)] = lines[z(i)] * gain(i);
    gc.bands = 22;
  } else {
    // Reordered (line = 3 * f + w) -> band / window / frequency order.
    int pos = 0;
    for (std::size_t b = 0; b < 13; ++b) {
      const int width_b = bands.s[b + 1] - bands.s[b];
      for (int w = 0; w < 3; ++w) {
        for (int i = 0; i < width_b; ++i) {
          const int f = bands.s[b] + i;
          gc.xr[z(pos++)] = lines[z(3 * f + w)] * gain(f * 3);
        }
      }
    }
    gc.bands = 39;
  }
  for (std::size_t i = 0; i < 576; ++i) gc.xr34[i] = std::pow(std::abs(gc.xr[i]), 0.75);
}

bool Layer3Encoder::Impl::detect_attack(std::uint64_t g) {
  // High-passed energy in 192-sample segments over the span the short
  // windows of granule g would cover, against the recent average.
  const auto start = static_cast<std::int64_t>(g * 576) - 640;
  bool attack = false;
  for (int c = 0; c < nch; ++c) {
    auto& history = hp_energy_history[z(c)];
    for (int seg = 0; seg < 3; ++seg) {
      // Segments advance by one granule per call: only the newest three
      // of the four segments are new.
      double e = 0.0;
      const std::int64_t from = start + 192 + seg * 192;
      for (std::int64_t n = from; n < from + 192; ++n) {
        const double v = n < 0 ? 0.0 : sample(c, static_cast<std::uint64_t>(n));
        const double hp = v - hp_state[z(c)];
        hp_state[z(c)] = v;
        e += hp * hp;
      }
      double mean = 0.0;
      const std::size_t count = std::min<std::size_t>(history.size(), 6);
      for (std::size_t i = history.size() - count; i < history.size(); ++i) mean += history[i];
      mean = count > 0 ? mean / static_cast<double>(count) : e;
      if (e > 8.0 * mean && e > 192.0 * 1e-6) attack = true;
      history.push_back(e);
      if (history.size() > 16) history.erase(history.begin());
    }
  }
  return attack;
}

// --------------------------------------------------------------------------- psychoacoustics

void Layer3Encoder::Impl::psycho(int c, std::uint64_t g, GranuleChannel& gc) {
  std::vector<double> buffer, energy, threshold;
  if (gc.block_type != 2) {
    const std::int64_t centre = static_cast<std::int64_t>(g * 576) - 240;
    buffer.resize(1024);
    for (std::int64_t i = 0; i < 1024; ++i) {
      const std::int64_t n = centre - 512 + i;
      buffer[z(i)] = n < 0 ? 0.0 : sample(c, static_cast<std::uint64_t>(n));
    }
    psy_long.analyse(buffer.data(), energy, threshold);
    const double bin_per_line = 1024.0 / 1152.0;
    for (std::size_t b = 0; b < 22; ++b) {
      const int l0 = bands.l[b], l1 = bands.l[b + 1];
      double e = 0.0;
      for (int i = l0; i < l1; ++i) e += gc.xr[z(i)] * gc.xr[z(i)];
      gc.energy[b] = e;
      auto k0 = static_cast<std::size_t>(l0 * bin_per_line);
      auto k1 = static_cast<std::size_t>(std::ceil(l1 * bin_per_line));
      k1 = std::clamp<std::size_t>(k1, k0 + 1, 512);
      double fe = 0.0, ft = 0.0;
      for (std::size_t k = k0; k < k1 && k < 512; ++k) {
        fe += energy[k];
        ft += threshold[k];
      }
      // Pre-echo control: the threshold may not jump far above the last one.
      if (have_previous[z(c)]) ft = std::min(ft, 2.0 * previous_threshold[z(c)][b] + 1e-9 * fe);
      previous_threshold[z(c)][b] = ft;
      const double ratio = fe > 0.0 ? std::min(ft / fe, 64.0) : 64.0;  // above 1: masked, coded only at high quality
      gc.allowed[b] = e > 0.0 ? ratio * e : 0.0;
    }
    have_previous[z(c)] = true;
    return;
  }
  // Short blocks: three 256-point analyses centred on the windows.
  have_previous[z(c)] = false;
  buffer.resize(256);
  const double bin_per_line = 256.0 / 384.0;
  for (int w = 0; w < 3; ++w) {
    const std::int64_t centre = static_cast<std::int64_t>(g * 576) - 240 + (w - 1) * 192;
    for (std::int64_t i = 0; i < 256; ++i) {
      const std::int64_t n = centre - 128 + i;
      buffer[z(i)] = n < 0 ? 0.0 : sample(c, static_cast<std::uint64_t>(n));
    }
    psy_short.analyse(buffer.data(), energy, threshold);
    int pos = 0;
    for (std::size_t b = 0; b < 13; ++b) {
      const int width = bands.s[b + 1] - bands.s[b];
      const int from = 3 * bands.s[b] + w * width;
      double e = 0.0;
      for (int i = from; i < from + width; ++i) e += gc.xr[z(i)] * gc.xr[z(i)];
      (void)pos;
      auto k0 = static_cast<std::size_t>(bands.s[b] * bin_per_line);
      auto k1 = static_cast<std::size_t>(std::ceil(bands.s[b + 1] * bin_per_line));
      k1 = std::clamp<std::size_t>(k1, k0 + 1, 128);
      double fe = 0.0, ft = 0.0;
      for (std::size_t k = k0; k < k1 && k < 128; ++k) {
        fe += energy[k];
        ft += threshold[k];
      }
      const double ratio = fe > 0.0 ? std::min(ft / fe, 64.0) : 64.0;  // above 1: masked, coded only at high quality
      gc.energy[b * 3 + z(w)] = e;
      gc.allowed[b * 3 + z(w)] = e > 0.0 ? ratio * e : 1e-12;
    }
  }
}

// --------------------------------------------------------------------------- quantisation

namespace {

/// Quantise lines [from, to) with step exponent q (quarter steps); returns
/// the noise energy, or infinity when a value would overflow.
double band_noise(const GranuleChannel& gc, int from, int to, int q, int* ix) {
  const double inv = std::exp2(-0.1875 * q);  // (2^(q/4))^(-3/4)
  const double step = std::exp2(0.25 * q);
  double noise = 0.0;
  for (int i = from; i < to; ++i) {
    const double v = gc.xr34[z(i)] * inv + 0.4054;
    if (v >= max_ix + 1.0) return std::numeric_limits<double>::infinity();
    const int n = static_cast<int>(v);
    if (ix != nullptr) ix[i] = gc.xr[z(i)] < 0 ? -n : n;
    const double back = std::pow(static_cast<double>(n), 4.0 / 3.0) * step;
    const double d = std::abs(gc.xr[z(i)]) - back;
    noise += d * d;
  }
  return noise;
}

}  // namespace

void Layer3Encoder::Impl::quantize(const GranuleChannel& gc, double offset_db, Coded& out) const {
  const bool short_blocks = gc.block_type == 2;
  const int nb = short_blocks ? 39 : 22;
  const double factor = std::pow(10.0, offset_db / 10.0);
  std::array<int, 39> from{}, to{}, want{};
  std::array<bool, 39> silent{};
  for (int b = 0; b < nb; ++b) {
    if (!short_blocks) {
      from[z(b)] = bands.l[z(b)];
      to[z(b)] = bands.l[z(b) + 1];
    } else {
      const int band = b / 3, w = b % 3;
      const int width = bands.s[z(band) + 1] - bands.s[z(band)];
      from[z(b)] = 3 * bands.s[z(band)] + w * width;
      to[z(b)] = from[z(b)] + width;
    }
  }
  // Coarsest step per band whose noise stays within the allowance, and the
  // finest step each band can take without overflowing the quantiser.
  constexpr int none = -100000;
  std::array<int, 39> q_floor{};
  int qmax = none;
  int floor_all = -210;
  for (int b = 0; b < nb; ++b) {
    const double allowed = gc.allowed[z(b)] * factor;
    double peak = 0.0;
    for (int i = from[z(b)]; i < to[z(b)]; ++i) peak = std::max(peak, gc.xr34[z(i)]);
    q_floor[z(b)] = peak > 0.0 ? static_cast<int>(std::ceil(std::log2(peak / (max_ix + 0.5 - 0.4054)) / 0.1875)) : none;
    floor_all = std::max(floor_all, q_floor[z(b)]);
    if (peak == 0.0 || gc.energy[z(b)] <= allowed) {
      silent[z(b)] = true;
      continue;
    }
    int lo = std::max(q_floor[z(b)], -210), hi = 45;
    if (band_noise(gc, from[z(b)], to[z(b)], lo, nullptr) > allowed) {
      want[z(b)] = lo;
    } else {
      while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (band_noise(gc, from[z(b)], to[z(b)], mid, nullptr) <= allowed) lo = mid;
        else hi = mid - 1;
      }
      want[z(b)] = lo;
    }
    qmax = std::max(qmax, want[z(b)]);
  }
  out.ix.fill(0);
  out.sf.fill(0);
  out.scalefac_scale = false;
  if (qmax == none) {
    // Nothing audible: an all-zero granule.
    out.global_gain = 0;
    return;
  }
  // The highest band has no scale factor: it is coded with the global step.
  const auto limit_for = [&](int b) {
    if (!short_blocks) return b >= 21 ? 0 : (b < 11 ? 15 : 7);
    const int band = b / 3;
    if (band >= 12) return 0;
    return band < 6 ? 15 : 7;
  };
  int q_global = std::max(qmax, floor_all);
  bool scale = false;
  std::array<int, 39> sf{};
  for (int attempt = 0; attempt < 3; ++attempt) {
    scale = attempt >= 1;
    const int unit = scale ? 4 : 2;
    int needed_drop = 0;
    for (int b = 0; b < nb; ++b) {
      sf[z(b)] = 0;
      if (silent[z(b)]) continue;
      const int diff = q_global - want[z(b)];
      if (diff <= 0) continue;
      int s = (diff + unit - 1) / unit;
      const int limit = limit_for(b);
      if (s > limit) {
        needed_drop = std::max(needed_drop, diff - limit * unit);
        s = limit;
      }
      // Never refine a band into overflow.
      if (q_floor[z(b)] != none) s = std::min(s, std::max(0, (q_global - q_floor[z(b)]) / unit));
      sf[z(b)] = s;
    }
    if (needed_drop == 0) break;
    if (attempt == 1) q_global = std::max(floor_all, q_global - needed_drop);
  }
  q_global = std::clamp(q_global, -210, 45);
  out.global_gain = q_global + 210;
  out.scalefac_scale = scale;
  out.sf = sf;
  const int unit = scale ? 4 : 2;
  for (int b = 0; b < nb; ++b) {
    const int q = q_global - unit * sf[z(b)];
    if (band_noise(gc, from[z(b)], to[z(b)], q, out.ix.data()) == std::numeric_limits<double>::infinity()) {
      // Only reachable through the gain clamp: drop the band.
      for (int i = from[z(b)]; i < to[z(b)]; ++i) out.ix[z(i)] = 0;
    }
  }
}

void Layer3Encoder::Impl::scalefactor_bits(const GranuleChannel& gc, Coded& out) const {
  const bool short_blocks = gc.block_type == 2;
  if (!lsf()) {
    int max1 = 0, max2 = 0;
    if (!short_blocks) {
      for (int b = 0; b < 11; ++b) max1 = std::max(max1, out.sf[z(b)]);
      for (int b = 11; b < 21; ++b) max2 = std::max(max2, out.sf[z(b)]);
    } else {
      for (int b = 0; b < 18; ++b) max1 = std::max(max1, out.sf[z(b)]);
      for (int b = 18; b < 36; ++b) max2 = std::max(max2, out.sf[z(b)]);
    }
    int best = 15, best_bits = std::numeric_limits<int>::max();
    for (int c = 0; c < 16; ++c) {
      const int s1 = slen_table[0][c], s2 = slen_table[1][c];
      if ((1 << s1) - 1 < max1 || (1 << s2) - 1 < max2) continue;
      const int bits = short_blocks ? 18 * s1 + 18 * s2 : 11 * s1 + 10 * s2;
      if (bits < best_bits) {
        best_bits = bits;
        best = c;
      }
    }
    out.scalefac_compress = best;
    out.part2_bits = best_bits;
    return;
  }
  // LSF, table 0: four groups (long 6/5/5/5 bands, short 9/9/9/9 values).
  std::array<int, 4> maxv{};
  if (!short_blocks) {
    static constexpr int edges[5] = {0, 6, 11, 16, 21};
    for (std::size_t grp = 0; grp < 4; ++grp) {
      for (int b = edges[grp]; b < edges[grp + 1]; ++b) maxv[grp] = std::max(maxv[grp], out.sf[z(b)]);
    }
  } else {
    for (std::size_t grp = 0; grp < 4; ++grp) {
      for (int b = static_cast<int>(grp) * 9; b < static_cast<int>(grp) * 9 + 9; ++b) maxv[grp] = std::max(maxv[grp], out.sf[z(b)]);
    }
  }
  std::array<int, 4> slen{};
  for (std::size_t grp = 0; grp < 4; ++grp) {
    int s = 0;
    while ((1 << s) - 1 < maxv[grp]) ++s;
    slen[grp] = s;
  }
  slen[0] = std::min(slen[0], 4);
  slen[1] = std::min(slen[1], 4);
  slen[2] = std::min(slen[2], 3);
  slen[3] = std::min(slen[3], 3);
  out.scalefac_compress = ((slen[0] * 5 + slen[1]) << 4) + (slen[2] << 2) + slen[3];
  static constexpr int counts_long[4] = {6, 5, 5, 5};
  int bits = 0;
  for (std::size_t grp = 0; grp < 4; ++grp) bits += slen[grp] * (short_blocks ? 9 : counts_long[grp]);
  out.part2_bits = bits;
}

void Layer3Encoder::Impl::huffman_choose(const GranuleChannel& gc, Coded& out) const {
  const int* ix = out.ix.data();
  int rz = 576;
  while (rz > 0 && ix[rz - 1] == 0) --rz;
  int i = rz + (rz & 1);
  const int count1_end = std::min(i, 576);
  // Extend the count1 region downwards over quadruples of values <= 1.
  while (i >= 4 && std::abs(ix[i - 1]) <= 1 && std::abs(ix[i - 2]) <= 1 && std::abs(ix[i - 3]) <= 1 &&
         std::abs(ix[i - 4]) <= 1) {
    i -= 4;
  }
  // count1 must hold whole quadruples up to count1_end.
  const int big_end = i;
  int c1_end = count1_end;
  if ((c1_end - big_end) % 4 != 0) c1_end = std::min(576, big_end + ((c1_end - big_end + 3) / 4) * 4);
  out.big_values = big_end / 2;
  out.count1_end = c1_end;
  const int qa = quad_bits(ix, big_end, c1_end, false);
  const int qb = quad_bits(ix, big_end, c1_end, true);
  out.count1_b = qb < qa;
  const int count1_bits = std::min(qa, qb);
  int best_bits = std::numeric_limits<int>::max();
  if (gc.block_type != 0) {
    const int r1 = std::min(gc.block_type == 2 ? 3 * bands.s[3] : bands.l[8], big_end);
    const auto a = best_table(ix, 0, r1);
    const auto b = best_table(ix, r1, big_end);
    out.tables = {a.first, b.first, 0};
    best_bits = a.second + b.second;
  } else {
    // Per-band costs for every table, then the best split into regions.
    std::array<int, 23> edge{};
    int nbands = 0;
    for (int b = 0; b <= 22; ++b) {
      edge[z(b)] = std::min(bands.l[z(b)], big_end);
      if (bands.l[z(b)] < big_end) nbands = b + 1;
    }
    if (big_end == 0) {
      out.tables = {0, 0, 0};
      out.region0 = 0;
      out.region1 = 0;
      best_bits = 0;
    } else {
      // prefix[t][b] = bits of bands [0, b) with table t; -1 when impossible.
      constexpr int inf = std::numeric_limits<int>::max() / 4;
      std::array<std::array<int, 23>, 32> prefix{};
      std::array<int, 23> band_max{};
      for (int b = 0; b < nbands; ++b) band_max[z(b)] = range_max(ix, edge[z(b)], edge[z(b) + 1]);
      std::array<int, 32> tables_used{};
      int used = 0;
      tables_used[z(used++)] = 0;
      for (const int t : all_tables) tables_used[z(used++)] = t;
      for (int u = 0; u < used; ++u) {
        const int t = tables_used[z(u)];
        const TableInfo info = table_info(t);
        prefix[z(t)][0] = 0;
        for (int b = 0; b < nbands; ++b) {
          int cost;
          if (t == 0) cost = band_max[z(b)] == 0 ? 0 : inf;
          else cost = band_max[z(b)] > info.max ? inf : range_bits(info, ix, edge[z(b)], edge[z(b) + 1]);
          prefix[z(t)][z(b) + 1] = std::min(inf, prefix[z(t)][z(b)] + cost);
        }
      }
      const auto region_cost = [&](int from_band, int to_band, int& table) {
        int best = inf;
        table = 0;
        if (from_band >= to_band) return 0;
        for (int u = 0; u < used; ++u) {
          const int t = tables_used[z(u)];
          const int a = prefix[z(t)][z(from_band)], b = prefix[z(t)][z(to_band)];
          if (a >= inf || b >= inf) continue;
          if (b - a < best) {
            best = b - a;
            table = t;
          }
        }
        return best;
      };
      for (int r0 = 0; r0 < 16; ++r0) {
        const int e0 = std::min(r0 + 1, nbands);
        int t0 = 0;
        const int c0 = region_cost(0, e0, t0);
        if (c0 >= inf) continue;
        for (int r1 = 0; r1 < 8; ++r1) {
          const int e1 = std::min(r0 + r1 + 2, nbands);
          int t1 = 0, t2 = 0;
          const int c1 = region_cost(e0, e1, t1);
          const int c2 = region_cost(e1, nbands, t2);
          if (c1 >= inf || c2 >= inf) continue;
          if (c0 + c1 + c2 < best_bits) {
            best_bits = c0 + c1 + c2;
            out.tables = {t0, t1, t2};
            out.region0 = r0;
            out.region1 = r1;
          }
          if (e1 >= nbands) break;
        }
        if (e0 >= nbands) break;
      }
    }
  }
  out.part3_bits = best_bits + count1_bits;
}

// --------------------------------------------------------------------------- frames

Result<void> Layer3Encoder::Impl::encode_frame(std::vector<GranuleChannel>& gcs, const std::vector<int>& types) {
  (void)types;
  const std::size_t count = gcs.size();  // granules * nch
  // Mid/side decision for the frame (joint stereo).
  bool ms = false;
  if (nch == 2 && settings.stereo != Mp3Settings::Stereo::Stereo) {
    double pe_lr = 0.0, pe_ms = 0.0;
    for (std::size_t g = 0; g < static_cast<std::size_t>(granules); ++g) {
      const GranuleChannel& l = gcs[g * 2];
      const GranuleChannel& r = gcs[g * 2 + 1];
      for (int b = 0; b < l.bands; ++b) {
        int from = 0, to = 0;
        if (l.block_type != 2) {
          from = bands.l[z(b)];
          to = bands.l[z(b) + 1];
        } else {
          const int band = b / 3, w = b % 3;
          const int width = bands.s[z(band) + 1] - bands.s[z(band)];
          from = 3 * bands.s[z(band)] + w * width;
          to = from + width;
        }
        double em = 0.0, es = 0.0;
        for (int i = from; i < to; ++i) {
          const double m = (l.xr[z(i)] + r.xr[z(i)]) * std::numbers::sqrt2 / 2.0;
          const double s = (l.xr[z(i)] - r.xr[z(i)]) * std::numbers::sqrt2 / 2.0;
          em += m * m;
          es += s * s;
        }
        const double width = to - from;
        const double al = std::max(l.allowed[z(b)], 1e-20), ar = std::max(r.allowed[z(b)], 1e-20);
        const double am = std::min(al, ar);
        pe_lr += width * (std::log2(1.0 + l.energy[z(b)] / al) + std::log2(1.0 + r.energy[z(b)] / ar));
        pe_ms += width * (std::log2(1.0 + em / am) + std::log2(1.0 + es / am));
      }
    }
    ms = pe_ms < pe_lr * 0.98 || settings.stereo == Mp3Settings::Stereo::Joint;
    if (settings.stereo == Mp3Settings::Stereo::Joint) ms = pe_ms < pe_lr;
  }
  if (ms) {
    for (std::size_t g = 0; g < static_cast<std::size_t>(granules); ++g) {
      GranuleChannel& l = gcs[g * 2];
      GranuleChannel& r = gcs[g * 2 + 1];
      for (std::size_t i = 0; i < 576; ++i) {
        const double m = (l.xr[i] + r.xr[i]) * std::numbers::sqrt2 / 2.0;
        const double s = (l.xr[i] - r.xr[i]) * std::numbers::sqrt2 / 2.0;
        l.xr[i] = m;
        r.xr[i] = s;
        l.xr34[i] = std::pow(std::abs(m), 0.75);
        r.xr34[i] = std::pow(std::abs(s), 0.75);
      }
      for (int b = 0; b < l.bands; ++b) {
        const double a = std::min(l.allowed[z(b)], r.allowed[z(b)]);
        l.allowed[z(b)] = r.allowed[z(b)] = a;
        // Energies of the new channels (needed by the quantiser).
        int from = 0, to = 0;
        if (l.block_type != 2) {
          from = bands.l[z(b)];
          to = bands.l[z(b) + 1];
        } else {
          const int band = b / 3, w = b % 3;
          const int width = bands.s[z(band) + 1] - bands.s[z(band)];
          from = 3 * bands.s[z(band)] + w * width;
          to = from + width;
        }
        double em = 0.0, es = 0.0;
        for (int i = from; i < to; ++i) {
          em += l.xr[z(i)] * l.xr[z(i)];
          es += r.xr[z(i)] * r.xr[z(i)];
        }
        l.energy[z(b)] = em;
        r.energy[z(b)] = es;
      }
    }
  }

  // Space: payload that this frame's main data may use.
  const std::uint64_t start = std::max(write_pos, payload_pos > static_cast<std::uint64_t>(max_back) ? payload_pos - static_cast<std::uint64_t>(max_back) : 0);
  const std::size_t header_bytes = 4 + side_bytes;
  const auto capacity = [&](int index, bool pad) -> std::int64_t {
    return static_cast<std::int64_t>(payload_pos + frame_size(index, pad) - header_bytes) - static_cast<std::int64_t>(start);
  };
  std::vector<Coded> coded(count);
  const auto evaluate = [&](double offset, std::vector<Coded>& out) {
    std::int64_t total = 0;
    bool fits_granule = true;
    for (std::size_t i = 0; i < count; ++i) {
      quantize(gcs[i], offset, out[i]);
      scalefactor_bits(gcs[i], out[i]);
      huffman_choose(gcs[i], out[i]);
      if (out[i].bits() > 4095) fits_granule = false;
      total += out[i].bits();
    }
    return fits_granule ? total : std::numeric_limits<std::int64_t>::max() / 2;
  };
  const bool vbr = settings.mode == Mp3Settings::Mode::Vbr || !settings.bitrate;
  const int top = 14;
  bool padding = false;
  int index = cbr_index;
  if (settings.mode == Mp3Settings::Mode::Cbr && settings.bitrate) {
    const std::uint32_t kbps = bitrates[lsf() ? 1 : 0][2][cbr_index];
    const std::uint64_t num = std::uint64_t{lsf() ? 72000u : 144000u} * kbps;
    padding_acc += num % rate;
    if (padding_acc >= rate) {
      padding_acc -= rate;
      padding = true;
    }
  }
  const std::int64_t max_bits = (vbr || settings.mode == Mp3Settings::Mode::Abr) ? capacity(top, false) * 8
                                                                                   : capacity(index, padding) * 8;
  std::int64_t target = max_bits;
  if (!vbr) {
    // CBR / ABR: average budget per frame, using the reservoir moderately.
    const double mean = static_cast<double>(*settings.bitrate) * 1000.0 * spf() / rate;
    abr_bits_target += static_cast<std::uint64_t>(mean);
    const double debt = static_cast<double>(bits_spent) - static_cast<double>(abr_bits_target);
    double want = mean - 0.5 * debt;
    if (settings.mode == Mp3Settings::Mode::Cbr) {
      const double reservoir = static_cast<double>(payload_pos - start) * 8.0;
      want = static_cast<double>(capacity(index, padding)) * 8.0 - reservoir + 0.6 * reservoir - static_cast<double>(header_bytes) * 0.0;
      want = std::max(want, mean * 0.5);
    }
    target = std::min<std::int64_t>(max_bits, static_cast<std::int64_t>(want));
    target = std::max<std::int64_t>(target, 0);
  }
  double offset = quality_db;
  std::int64_t bits = evaluate(offset, coded);
  if (vbr) {
    if (bits > max_bits) {
      double lo = offset, hi = offset + 40.0;
      for (int it = 0; it < 10; ++it) {
        const double mid = (lo + hi) / 2;
        if (evaluate(mid, coded) <= max_bits) hi = mid;
        else lo = mid;
      }
      offset = hi;
      bits = evaluate(offset, coded);
    }
  } else {
    // Best quality (lowest offset) whose bits fit the target.
    double lo = -30.0, hi = 50.0;
    if (evaluate(lo, coded) <= target) {
      offset = lo;
    } else {
      for (int it = 0; it < 9; ++it) {
        const double mid = (lo + hi) / 2;
        if (evaluate(mid, coded) <= target) hi = mid;
        else lo = mid;
      }
      offset = hi;
    }
    bits = evaluate(offset, coded);
    if (bits > max_bits) {
      double l2 = offset, h2 = offset + 60.0;
      for (int it = 0; it < 10; ++it) {
        const double mid = (l2 + h2) / 2;
        if (evaluate(mid, coded) <= max_bits) h2 = mid;
        else l2 = mid;
      }
      bits = evaluate(h2, coded);
    }
  }
  if (bits > max_bits) return fail(Status::Encode, "the MP3 encoder could not fit a frame into its bit budget");
  // Frame size: CBR fixed; VBR/ABR the smallest bit rate that holds the data.
  if (vbr || settings.mode == Mp3Settings::Mode::Abr) {
    index = top;
    for (int i = std::max(1, min_bitrate_index); i <= top; ++i) {
      if (capacity(i, false) * 8 >= bits) {
        index = i;
        break;
      }
    }
    padding = false;
  }
  bits_spent += static_cast<std::uint64_t>(frame_size(index, padding)) * 8;
  emit_frame(coded, gcs, ms, index, padding);
  return {};
}

void Layer3Encoder::Impl::emit_frame(std::vector<Coded>& coded, const std::vector<GranuleChannel>& gcs, bool ms,
                                     int bitrate_index, bool padding) {
  // Main data.
  audio::BitWriter main;
  for (std::size_t i = 0; i < coded.size(); ++i) {
    const Coded& c = coded[i];
    const GranuleChannel& gc = gcs[i];
    const bool short_blocks = gc.block_type == 2;
    // Scale factors.
    if (!lsf()) {
      const int s1 = slen_table[0][c.scalefac_compress], s2 = slen_table[1][c.scalefac_compress];
      if (!short_blocks) {
        for (int b = 0; b < 11; ++b) main.write(static_cast<std::uint32_t>(c.sf[z(b)]), static_cast<unsigned>(s1));
        for (int b = 11; b < 21; ++b) main.write(static_cast<std::uint32_t>(c.sf[z(b)]), static_cast<unsigned>(s2));
      } else {
        for (int b = 0; b < 18; ++b) main.write(static_cast<std::uint32_t>(c.sf[z(b)]), static_cast<unsigned>(s1));
        for (int b = 18; b < 36; ++b) main.write(static_cast<std::uint32_t>(c.sf[z(b)]), static_cast<unsigned>(s2));
      }
    } else {
      const int v = c.scalefac_compress;
      const std::array<int, 4> slen = {(v >> 4) / 5, (v >> 4) % 5, (v & 15) >> 2, v & 3};
      static constexpr int counts_long[4] = {6, 5, 5, 5};
      int b = 0;
      for (std::size_t grp = 0; grp < 4; ++grp) {
        const int n = short_blocks ? 9 : counts_long[grp];
        for (int k = 0; k < n; ++k, ++b) main.write(static_cast<std::uint32_t>(c.sf[z(b)]), static_cast<unsigned>(slen[grp]));
      }
    }
    // Huffman data.
    const int big_end = c.big_values * 2;
    std::array<int, 3> region_end{};
    if (gc.block_type != 0) {
      const int r1 = std::min(short_blocks ? 3 * bands.s[3] : bands.l[8], big_end);
      region_end = {r1, big_end, big_end};
    } else {
      region_end = {std::min(bands.l[z(c.region0) + 1], big_end),
                    std::min(bands.l[z(std::min(c.region0 + c.region1 + 2, 22))], big_end), big_end};
    }
    int line = 0;
    for (std::size_t r = 0; r < 3; ++r) {
      const TableInfo t = table_info(c.tables[r]);
      for (; line < region_end[r]; line += 2) {
        if (c.tables[r] == 0) continue;
        write_pair(main, t, c.ix[z(line)], c.ix[z(line) + 1]);
      }
    }
    for (int q = big_end; q < c.count1_end; q += 4) {
      const int v = (c.ix[z(q)] != 0 ? 8 : 0) | (c.ix[z(q) + 1] != 0 ? 4 : 0) | (c.ix[z(q) + 2] != 0 ? 2 : 0) |
                    (c.ix[z(q) + 3] != 0 ? 1 : 0);
      if (c.count1_b) main.write(static_cast<std::uint32_t>(15 - v), 4);
      else main.write(count1_table_a[z(v)].code, count1_table_a[z(v)].length);
      for (int k = 0; k < 4; ++k) {
        if (c.ix[z(q + k)] != 0) main.write(c.ix[z(q + k)] < 0 ? 1 : 0, 1);
      }
    }
  }
  const std::uint64_t data_bytes = (main.bit_count() + 7) / 8;
  const std::uint64_t start = std::max(write_pos, payload_pos > static_cast<std::uint64_t>(max_back) ? payload_pos - static_cast<std::uint64_t>(max_back) : 0);
  const std::uint64_t begin = payload_pos - start;

  // Header and side information.
  audio::BitWriter head;
  head.write(0x7FF, 11);
  head.write(version == 0 ? 3 : version == 1 ? 2 : 0, 2);
  head.write(1, 2);  // layer III
  head.write(1, 1);  // no CRC
  head.write(static_cast<std::uint32_t>(bitrate_index), 4);
  head.write(static_cast<std::uint32_t>(rate_index), 2);
  head.write(padding ? 1 : 0, 1);
  head.write(0, 1);
  const int mode = nch == 1 ? 3 : (ms ? 1 : (settings.stereo == Mp3Settings::Stereo::Stereo ? 0 : 1));
  head.write(static_cast<std::uint32_t>(mode), 2);
  head.write(ms ? 2 : 0, 2);
  head.write(0, 1);  // copyright
  head.write(1, 1);  // original
  head.write(0, 2);  // emphasis
  if (!lsf()) {
    head.write(static_cast<std::uint32_t>(begin), 9);
    head.write(0, nch == 1 ? 5 : 3);
    for (int c = 0; c < nch; ++c) head.write(0, 4);  // scfsi
  } else {
    head.write(static_cast<std::uint32_t>(begin), 8);
    head.write(0, nch == 1 ? 1 : 2);
  }
  for (std::size_t i = 0; i < coded.size(); ++i) {
    const Coded& c = coded[i];
    const GranuleChannel& gc = gcs[i];
    head.write(static_cast<std::uint32_t>(c.bits()), 12);
    head.write(static_cast<std::uint32_t>(c.big_values), 9);
    head.write(static_cast<std::uint32_t>(c.global_gain), 8);
    head.write(static_cast<std::uint32_t>(c.scalefac_compress), lsf() ? 9 : 4);
    if (gc.block_type != 0) {
      head.write(1, 1);
      head.write(static_cast<std::uint32_t>(gc.block_type), 2);
      head.write(0, 1);  // mixed
      head.write(static_cast<std::uint32_t>(c.tables[0]), 5);
      head.write(static_cast<std::uint32_t>(c.tables[1]), 5);
      for (int w = 0; w < 3; ++w) head.write(0, 3);  // subblock gain
    } else {
      head.write(0, 1);
      for (std::size_t r = 0; r < 3; ++r) head.write(static_cast<std::uint32_t>(c.tables[r]), 5);
      head.write(static_cast<std::uint32_t>(c.region0), 4);
      head.write(static_cast<std::uint32_t>(c.region1), 3);
    }
    if (!lsf()) head.write(0, 1);  // preflag
    head.write(c.scalefac_scale ? 1 : 0, 1);
    head.write(c.count1_b ? 1 : 0, 1);
  }
  PendingFrame frame;
  frame.head = to_bytes(std::span<const std::uint8_t>(head.take()));
  frame.payload_start = payload_pos;
  frame.payload_size = frame_size(bitrate_index, padding) - 4 - side_bytes;
  // Write the main data into the payload stream (zero stuffing before it).
  const std::uint64_t end = start + data_bytes;
  const std::uint64_t needed = std::max(end, payload_pos + frame.payload_size);
  if (needed > stream_base + stream.size()) stream.resize(static_cast<std::size_t>(needed - stream_base), 0);
  const auto bytes = main.take();
  std::copy(bytes.begin(), bytes.end(), stream.begin() + static_cast<std::ptrdiff_t>(start - stream_base));
  write_pos = end;
  payload_pos += frame.payload_size;
  frames.push_back(std::move(frame));
  ++frames_written;
  finalize_frames(false);
}

void Layer3Encoder::Impl::finalize_frames(bool all) {
  // A frame is final once no later frame can reach back into its payload.
  while (!frames.empty()) {
    const PendingFrame& f = frames.front();
    const std::uint64_t end = f.payload_start + f.payload_size;
    const bool reachable = payload_pos < static_cast<std::uint64_t>(max_back) || end > payload_pos - static_cast<std::uint64_t>(max_back);
    if (!all && (reachable || end > write_pos)) break;
    Bytes out = f.head;
    const std::size_t at = static_cast<std::size_t>(f.payload_start - stream_base);
    const auto* p = reinterpret_cast<const std::byte*>(stream.data() + at);
    out.insert(out.end(), p, p + f.payload_size);
    done.push_back(std::move(out));
    // Drop consumed payload bytes.
    const std::uint64_t consumed = end - stream_base;
    stream.erase(stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(consumed));
    stream_base = end;
    frames.pop_front();
  }
}

// =========================================================================== public

Layer3Encoder::Layer3Encoder() : impl_(std::make_unique<Impl>()) {}
Layer3Encoder::~Layer3Encoder() = default;

Result<void> Layer3Encoder::configure(std::uint32_t sample_rate, int channels, const Mp3Settings& settings) {
  return impl_->configure(sample_rate, channels, settings);
}

void Layer3Encoder::push(const float* samples, std::size_t frames) {
  Impl& m = *impl_;
  for (int c = 0; c < m.nch; ++c) {
    auto& in = m.input[z(c)];
    in.reserve(in.size() + frames);
    for (std::size_t i = 0; i < frames; ++i) in.push_back(static_cast<double>(samples[i * z(m.nch) + z(c)]));
  }
  m.input_total += frames;
}

Result<void> Layer3Encoder::encode(bool final) {
  Impl& m = *impl_;
  if (final && !m.flushed) {
    m.flushed = true;
    // Enough granules that the decoded stream covers delay + 529 + input.
    const std::uint64_t needed = std::uint64_t{m.encoder_delay()} + 529 + m.input_total;
    std::uint64_t granules = (needed + 575) / 576;
    granules = (granules + static_cast<std::uint64_t>(m.granules) - 1) / static_cast<std::uint64_t>(m.granules) *
               static_cast<std::uint64_t>(m.granules);
    m.total_granules = granules;
    // Subband samples one granule beyond the last coded one (lookahead).
    m.padded_total = (granules + 1) * 576 + 1024;
  }
  m.compute_subbands();
  // Granule g can be coded when the subband samples (and the attack flag)
  // of granule g + 1 exist.
  while (m.next_coded_granule + 1 < m.next_subband_granule && (!m.flushed || m.next_coded_granule < m.total_granules)) {
    const std::uint64_t g = m.next_coded_granule;
    const bool attack = m.attacks[z(g - m.subband_base)];
    const bool attack_next = m.attacks[z(g + 1 - m.subband_base)];
    int type = 0;
    if (attack) type = 2;
    else if (attack_next) type = m.previous_type == 2 ? 2 : 1;
    else if (m.previous_type == 2) type = 3;
    else if (m.previous_type == 1) type = 2;
    m.previous_type = type;
    for (int c = 0; c < m.nch; ++c) {
      GranuleChannel gc;
      m.mdct(c, g, type, gc);
      m.psycho(c, g, gc);
      m.pending.push_back(std::move(gc));
    }
    m.pending_types.push_back(type);
    ++m.next_coded_granule;
    if (static_cast<int>(m.pending_types.size()) == m.granules) {
      CVT_TRY(m.encode_frame(m.pending, m.pending_types));
      m.pending.clear();
      m.pending_types.clear();
    }
    // Drop subband granules and input no longer needed.
    while (m.subband_base + 2 < m.next_coded_granule) {
      m.subbands.pop_front();
      m.attacks.pop_front();
      ++m.subband_base;
    }
    const std::uint64_t keep_from = m.next_coded_granule * 576 > 2048 ? m.next_coded_granule * 576 - 2048 : 0;
    if (keep_from > m.input_base + 65536) {
      const std::uint64_t drop = keep_from - m.input_base;
      for (int c = 0; c < m.nch; ++c) {
        auto& in = m.input[z(c)];
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(drop, in.size()));
        in.erase(in.begin(), in.begin() + static_cast<std::ptrdiff_t>(n));
      }
      m.input_base += drop;
    }
  }
  if (m.flushed && m.next_coded_granule >= m.total_granules) m.finalize_frames(true);
  return {};
}

std::vector<Bytes> Layer3Encoder::take_frames() { return std::exchange(impl_->done, {}); }

std::uint32_t Layer3Encoder::encoder_delay() const noexcept { return impl_->encoder_delay(); }
std::uint64_t Layer3Encoder::input_frames() const noexcept { return impl_->input_total; }
std::uint32_t Layer3Encoder::samples_per_frame() const noexcept { return impl_->spf(); }

Bytes Layer3Encoder::info_frame(std::uint32_t frames, std::uint64_t bytes, bool vbr) const {
  const Impl& m = *impl_;
  // Smallest frame that holds the tag (at least the stream's CBR rate).
  const std::size_t tag_bytes = 4 + m.side_bytes + 8 + 4 + 4 + 100 + 4 + 36;
  int index = vbr ? 1 : m.cbr_index;
  while (index < 14 && m.frame_size(index, false) < tag_bytes) ++index;
  const std::size_t size = m.frame_size(index, false);
  std::vector<std::uint8_t> f(size, 0);
  const std::uint32_t header = 0xFFE00000u | (static_cast<std::uint32_t>(m.version == 0 ? 3 : m.version == 1 ? 2 : 0) << 19) |
                               (1u << 17) | (1u << 16) | (static_cast<std::uint32_t>(index) << 12) |
                               (static_cast<std::uint32_t>(m.rate_index) << 10) |
                               (static_cast<std::uint32_t>(m.nch == 1 ? 3 : 1) << 6) | (1u << 2);
  f[0] = static_cast<std::uint8_t>(header >> 24);
  f[1] = static_cast<std::uint8_t>(header >> 16);
  f[2] = static_cast<std::uint8_t>(header >> 8);
  f[3] = static_cast<std::uint8_t>(header);
  std::size_t p = 4 + m.side_bytes;
  const auto put32 = [&](std::uint32_t v) {
    f[p++] = static_cast<std::uint8_t>(v >> 24);
    f[p++] = static_cast<std::uint8_t>(v >> 16);
    f[p++] = static_cast<std::uint8_t>(v >> 8);
    f[p++] = static_cast<std::uint8_t>(v);
  };
  ::cvt::util::copy_bytes(f.data() + p, vbr ? "Xing" : "Info", 4);
  p += 4;
  put32(0x0F);
  put32(frames);
  const std::uint64_t total = bytes + size;
  put32(static_cast<std::uint32_t>(std::min<std::uint64_t>(total, 0xFFFFFFFFu)));
  // Seek table: linear in time (approximate for VBR, exact for CBR).
  for (int i = 0; i < 100; ++i) f[p++] = static_cast<std::uint8_t>(std::min(255, i * 256 / 100));
  put32(static_cast<std::uint32_t>(std::clamp(100 - m.settings.quality, 0, 100)));
  const std::size_t lame = p;
  ::cvt::util::copy_bytes(f.data() + p, "LAME     ", 9);  // tag layout of the LAME header (no encoder version claimed)
  p += 9;
  const int method = vbr ? 4 : (m.settings.mode == Mp3Settings::Mode::Abr ? 2 : 1);
  f[p++] = static_cast<std::uint8_t>(method);
  f[p++] = static_cast<std::uint8_t>(std::min(255, static_cast<int>(m.lowpass_line * (m.rate / 2.0) / 576.0 / 100.0)));
  p += 4 + 2 + 2;  // replay gain fields
  f[p++] = 0;      // flags / ATH type
  f[p++] = static_cast<std::uint8_t>(std::min<std::uint32_t>(255, m.settings.bitrate.value_or(0)));
  const std::uint32_t delay = m.encoder_delay();
  const std::uint64_t coded = std::uint64_t{frames} * m.spf();
  const std::uint64_t pad64 = coded > delay + m.input_total ? coded - delay - m.input_total : 0;
  const std::uint32_t pad = static_cast<std::uint32_t>(std::min<std::uint64_t>(pad64, 4095));
  f[p++] = static_cast<std::uint8_t>(delay >> 4);
  f[p++] = static_cast<std::uint8_t>(((delay & 15) << 4) | (pad >> 8));
  f[p++] = static_cast<std::uint8_t>(pad & 0xFF);
  const int source = m.rate <= 32000 ? 0 : m.rate == 44100 ? 1 : m.rate == 48000 ? 2 : 3;
  f[p++] = static_cast<std::uint8_t>(source << 6);
  f[p++] = 0;      // mp3 gain
  p += 2;          // preset / surround
  put32(static_cast<std::uint32_t>(std::min<std::uint64_t>(total, 0xFFFFFFFFu)));
  p += 2;  // music CRC (not used by players)
  const std::uint16_t crc = crc16_arc(f.data(), lame + 34);
  f[p++] = static_cast<std::uint8_t>(crc >> 8);
  f[p++] = static_cast<std::uint8_t>(crc);
  return to_bytes(std::span<const std::uint8_t>(f));
}

}  // namespace cvt::lossy::mpeg
