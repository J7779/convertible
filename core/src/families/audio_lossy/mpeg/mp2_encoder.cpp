// Convertible core - audio_lossy family: the in-house MPEG-1/2 Layer II
// encoder: polyphase analysis, scale factors with selection information,
// bit allocation driven by signal-to-mask ratios from the shared
// psychoacoustic model, and grouped/ungrouped sample quantisation.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numbers>
#include <vector>

#include "families/audio_lossless/bits.hpp"
#include "families/audio_lossy/mpeg/mpeg_files.hpp"
#include "families/audio_lossy/mpeg/mpeg_psy.hpp"
#include "families/audio_lossy/mpeg/mpeg_tables.hpp"

namespace cvt::lossy::mpeg {

namespace {

template <class T>
constexpr std::size_t z(T v) noexcept {
  return static_cast<std::size_t>(v);
}

// Signal-to-noise ratio of each quantisation class (ISO/IEC 11172-3 Table C.5).
constexpr double class_snr[18] = {0.0,   7.00,  11.00, 16.00, 20.84, 25.28, 31.59, 37.75, 43.84,
                                  49.89, 55.93, 61.96, 67.98, 74.01, 80.03, 86.05, 92.01, 98.01};

struct Analysis {
  std::array<double, 512> x{};
  std::array<double, 512> window{};
  std::array<std::array<double, 64>, 32> matrix{};
  Analysis() {
    for (std::size_t i = 0; i < 512; ++i) window[i] = synthesis_window[i] / 65536.0 / 32.0;
    for (std::size_t k = 0; k < 32; ++k) {
      for (std::size_t i = 0; i < 64; ++i) {
        matrix[k][i] = std::cos(static_cast<double>(2 * k + 1) * (static_cast<double>(i) - 16.0) * std::numbers::pi / 64.0);
      }
    }
  }
  void run(const double* samples, double* out) {
    std::memmove(x.data() + 32, x.data(), sizeof(double) * 480);
    for (std::size_t i = 0; i < 32; ++i) x[i] = samples[31 - i];
    std::array<double, 64> y{};
    for (std::size_t i = 0; i < 64; ++i) {
      double sum = 0.0;
      for (std::size_t j = 0; j < 8; ++j) sum += window[i + 64 * j] * x[i + 64 * j];
      y[i] = sum;
    }
    for (std::size_t k = 0; k < 32; ++k) {
      double sum = 0.0;
      for (std::size_t i = 0; i < 64; ++i) sum += matrix[k][i] * y[i];
      out[k] = sum;
    }
  }
};

bool legal_layer2_rate(bool lsf, int channels, std::uint32_t kbps) {
  for (int i = 1; i < 15; ++i) {
    if (bitrates[lsf ? 1 : 0][1][i] != kbps) continue;
    if (lsf) return true;
    if (channels == 1) return kbps <= 192;
    return kbps != 32 && kbps != 48 && kbps != 56 && kbps != 80;
  }
  return false;
}

}  // namespace

Result<Bytes> encode_mp2(const ir::Audio& audio, const Mp2Settings& settings, const Context& ctx) {
  int version = -1, rate_index = 0;
  for (int v = 0; v < 2 && version < 0; ++v) {
    for (int r = 0; r < 3; ++r) {
      if (sample_rates[v][r] == audio.sample_rate) {
        version = v;
        rate_index = r;
        break;
      }
    }
  }
  if (version < 0) return fail(Status::Encode, "MPEG Layer II does not support {} Hz", audio.sample_rate);
  const bool lsf = version == 1;
  const int nch = audio.channels;
  if (nch < 1 || nch > 2) return fail(Status::Encode, "MPEG Layer II holds one or two channels");
  std::uint32_t kbps = settings.bitrate;
  if (!legal_layer2_rate(lsf, nch, kbps)) {
    // Nearest legal rate.
    std::uint32_t best = 0;
    for (int i = 1; i < 15; ++i) {
      const std::uint32_t r = bitrates[lsf ? 1 : 0][1][i];
      if (!legal_layer2_rate(lsf, nch, r)) continue;
      if (best == 0 || (r <= kbps && r > best) || (best > kbps && r < best)) best = r;
    }
    kbps = best;
  }
  int bitrate_index = 1;
  for (int i = 1; i < 15; ++i) {
    if (bitrates[lsf ? 1 : 0][1][i] == kbps) bitrate_index = i;
  }
  // Allocation table.
  const AllocRow* rows = nullptr;
  std::size_t row_count = 0;
  int sblimit = 30;
  if (lsf) {
    rows = alloc_lsf;
    row_count = 3;
  } else {
    const std::uint32_t per_channel = nch == 2 ? kbps / 2 : kbps;
    if (per_channel <= 48) {
      rows = alloc_b2cd;
      row_count = 2;
      sblimit = audio.sample_rate == 32000 ? 12 : 8;
    } else {
      rows = alloc_b2ab;
      row_count = 4;
      sblimit = (per_channel >= 96 && audio.sample_rate != 48000) ? 30 : 27;
    }
  }
  std::array<const AllocRow*, 32> row_of{};
  {
    int sb = 0;
    for (std::size_t r = 0; r < row_count && sb < sblimit; ++r) {
      for (int i = 0; i < rows[r].subbands && sb < sblimit; ++i) row_of[z(sb++)] = &rows[r];
    }
  }
  std::array<double, 64> scale{};
  for (std::size_t i = 0; i < 64; ++i) scale[i] = std::pow(2.0, 1.0 - static_cast<double>(i) / 3.0);

  PsyBank psy;
  psy.init(1024, audio.sample_rate, 0.0, 0.34);
  std::array<Analysis, 2> analysis;
  const std::size_t frames_in = audio.frame_count();
  // Enough frames to flush the filterbank delay (481 samples).
  const std::size_t total_frames = (frames_in + 481 + 1151) / 1152;
  const auto sample = [&](int c, std::int64_t n) -> double {
    if (n < 0 || static_cast<std::size_t>(n) >= frames_in) return 0.0;
    return audio.samples[static_cast<std::size_t>(n) * z(nch) + z(c)];
  };
  Bytes out;
  std::uint64_t padding_acc = 0;
  const std::uint64_t slot_num = std::uint64_t{144000} * kbps;
  std::vector<double> buffer(1024), energy, threshold;
  for (std::size_t f = 0; f < total_frames; ++f) {
    if ((f & 31) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(f) / static_cast<double>(total_frames), "Encoding"));
    bool padding = false;
    if (audio.sample_rate % 1000 != 0) {
      padding_acc += slot_num % audio.sample_rate;
      if (padding_acc >= audio.sample_rate) {
        padding_acc -= audio.sample_rate;
        padding = true;
      }
    }
    const std::size_t frame_bytes = static_cast<std::size_t>(slot_num / audio.sample_rate) + (padding ? 1 : 0);
    // Subband samples: [ch][slot 0..35][sb].
    std::array<std::array<std::array<double, 32>, 36>, 2> sub{};
    std::array<double, 32> block{};
    for (int c = 0; c < nch; ++c) {
      for (std::size_t s = 0; s < 36; ++s) {
        const auto first = static_cast<std::int64_t>(f * 1152 + s * 32);
        for (std::size_t i = 0; i < 32; ++i) block[i] = sample(c, first + static_cast<std::int64_t>(i));
        analysis[z(c)].run(block.data(), sub[z(c)][s].data());
      }
    }
    // Scale factors and signal-to-mask ratios.
    std::array<std::array<std::array<int, 3>, 32>, 2> sf{};
    std::array<std::array<int, 32>, 2> scfsi{};
    std::array<std::array<double, 32>, 2> smr{};
    for (int c = 0; c < nch; ++c) {
      for (int sb = 0; sb < sblimit; ++sb) {
        for (std::size_t part = 0; part < 3; ++part) {
          double peak = 0.0;
          for (std::size_t s = part * 12; s < part * 12 + 12; ++s) peak = std::max(peak, std::abs(sub[z(c)][s][z(sb)]));
          int idx = 62;
          while (idx > 0 && scale[z(idx)] < peak) --idx;
          sf[z(c)][z(sb)][part] = idx;
        }
        auto& s = sf[z(c)][z(sb)];
        const int d1 = s[0] - s[1], d2 = s[1] - s[2];
        if (std::abs(d1) <= 1 && std::abs(d2) <= 1) {
          s[0] = s[1] = s[2] = std::min({s[0], s[1], s[2]});
          scfsi[z(c)][z(sb)] = 2;
        } else if (std::abs(d1) <= 1) {
          s[0] = s[1] = std::min(s[0], s[1]);
          scfsi[z(c)][z(sb)] = 1;
        } else if (std::abs(d2) <= 1) {
          s[1] = s[2] = std::min(s[1], s[2]);
          scfsi[z(c)][z(sb)] = 3;
        } else {
          scfsi[z(c)][z(sb)] = 0;
        }
      }
      // FFT around the frame (the filterbank delay is about 240 samples).
      const auto centre = static_cast<std::int64_t>(f * 1152 + 576) - 240;
      for (std::int64_t i = 0; i < 1024; ++i) buffer[z(i)] = sample(c, centre - 512 + i);
      psy.analyse(buffer.data(), energy, threshold);
      const double full = 256.0 * 256.0;
      for (int sb = 0; sb < sblimit; ++sb) {
        double e = 0.0, t = std::numeric_limits<double>::max();
        for (std::size_t k = z(sb) * 16; k < z(sb) * 16 + 16; ++k) {
          e = std::max(e, energy[k]);
          t = std::min(t, threshold[k] * 16.0);
        }
        (void)full;
        smr[z(c)][z(sb)] = e > 0.0 ? 10.0 * std::log10(std::max(e, 1e-30) / std::max(t, 1e-30)) : -100.0;
      }
    }
    // Greedy bit allocation: always improve the band with the worst
    // noise-to-mask ratio that still fits.
    std::array<std::array<int, 32>, 2> alloc{};  // index into the row's class list
    int bits_used = 32;                           // header
    for (int sb = 0; sb < sblimit; ++sb) bits_used += row_of[z(sb)]->alloc_bits * nch;
    const int available = static_cast<int>(frame_bytes) * 8;
    const auto sample_bits = [&](const QuantClass& q) { return q.grouped ? 12 * q.bits : 36 * q.bits; };
    const auto scf_bits = [&](int c, int sb) {
      const int pattern = scfsi[z(c)][z(sb)];
      return 2 + (pattern == 0 ? 18 : pattern == 2 ? 6 : 12);
    };
    while (true) {
      int best_c = -1, best_sb = -1;
      double worst = -1e9;
      int best_cost = 0;
      for (int c = 0; c < nch; ++c) {
        for (int sb = 0; sb < sblimit; ++sb) {
          const AllocRow& row = *row_of[z(sb)];
          const int a = alloc[z(c)][z(sb)];
          if (a + 1 >= (1 << row.alloc_bits)) continue;
          const QuantClass& now = quant_classes[row.classes[a]];
          const QuantClass& next = quant_classes[row.classes[a + 1]];
          if (next.levels == 0) continue;
          const double nmr = smr[z(c)][z(sb)] - class_snr[row.classes[a]];
          int cost = sample_bits(next) - (now.levels == 0 ? 0 : sample_bits(now));
          if (a == 0) cost += scf_bits(c, sb);
          if (bits_used + cost > available) continue;
          if (nmr > worst) {
            worst = nmr;
            best_c = c;
            best_sb = sb;
            best_cost = cost;
          }
        }
      }
      if (best_c < 0) break;
      ++alloc[z(best_c)][z(best_sb)];
      bits_used += best_cost;
    }
    // Write the frame.
    audio::BitWriter w;
    w.write(0xFFF, 12);
    w.write(lsf ? 0 : 1, 1);
    w.write(2, 2);  // layer II
    w.write(1, 1);  // no CRC
    w.write(static_cast<std::uint32_t>(bitrate_index), 4);
    w.write(static_cast<std::uint32_t>(rate_index), 2);
    w.write(padding ? 1 : 0, 1);
    w.write(0, 1);
    w.write(nch == 1 ? 3 : 0, 2);
    w.write(0, 2);
    w.write(0, 1);
    w.write(1, 1);
    w.write(0, 2);
    for (int sb = 0; sb < sblimit; ++sb) {
      for (int c = 0; c < nch; ++c) w.write(static_cast<std::uint32_t>(alloc[z(c)][z(sb)]), row_of[z(sb)]->alloc_bits);
    }
    for (int sb = 0; sb < sblimit; ++sb) {
      for (int c = 0; c < nch; ++c) {
        if (alloc[z(c)][z(sb)] != 0) w.write(static_cast<std::uint32_t>(scfsi[z(c)][z(sb)]), 2);
      }
    }
    for (int sb = 0; sb < sblimit; ++sb) {
      for (int c = 0; c < nch; ++c) {
        if (alloc[z(c)][z(sb)] == 0) continue;
        const auto& s = sf[z(c)][z(sb)];
        switch (scfsi[z(c)][z(sb)]) {
          case 0:
            for (const int v : s) w.write(static_cast<std::uint32_t>(v), 6);
            break;
          case 1:
            w.write(static_cast<std::uint32_t>(s[0]), 6);
            w.write(static_cast<std::uint32_t>(s[2]), 6);
            break;
          case 2:
            w.write(static_cast<std::uint32_t>(s[0]), 6);
            break;
          default:
            w.write(static_cast<std::uint32_t>(s[0]), 6);
            w.write(static_cast<std::uint32_t>(s[1]), 6);
            break;
        }
      }
    }
    for (int gr = 0; gr < 12; ++gr) {
      const std::size_t part = z(gr / 4);
      for (int sb = 0; sb < sblimit; ++sb) {
        for (int c = 0; c < nch; ++c) {
          const QuantClass& q = quant_classes[row_of[z(sb)]->classes[alloc[z(c)][z(sb)]]];
          if (q.levels == 0) continue;
          const double sc = scale[z(sf[z(c)][z(sb)][part])];
          std::array<std::uint32_t, 3> code{};
          for (std::size_t i = 0; i < 3; ++i) {
            const double v = std::clamp(sub[z(c)][z(gr) * 3 + i][z(sb)] / sc, -1.0, 1.0);
            const double cv = std::round((v * q.levels + (q.levels - 1)) / 2.0);
            code[i] = static_cast<std::uint32_t>(std::clamp(cv, 0.0, q.levels - 1.0));
          }
          if (q.grouped) {
            w.write(code[0] + q.levels * (code[1] + q.levels * code[2]), q.bits);
          } else {
            for (const std::uint32_t v : code) w.write(v, q.bits);
          }
        }
      }
    }
    while (w.bit_count() < static_cast<std::uint64_t>(available)) w.write(0, 1);
    const auto bytes = w.take();
    const auto* p = reinterpret_cast<const std::byte*>(bytes.data());
    out.insert(out.end(), p, p + bytes.size());
    CVT_TRY(ctx.limits.check_output(out.size()));
  }
  return out;
}

}  // namespace cvt::lossy::mpeg
