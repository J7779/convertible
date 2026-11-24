// Convertible core - audio_lossy family: the psychoacoustic model shared by
// the MPEG audio encoders: Hann-windowed FFT, Bark partitions, Schroeder
// spreading, tonality from spectral flatness (Johnston's offsets) and the
// absolute threshold of hearing.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <vector>

namespace cvt::lossy::mpeg {

namespace psy_detail {
template <class T>
constexpr std::size_t z(T v) noexcept {
  return static_cast<std::size_t>(v);
}
}  // namespace psy_detail


inline void fft(std::vector<std::complex<double>>& a) {
  const std::size_t n = a.size();
  for (std::size_t i = 1, j = 0; i < n; ++i) {
    std::size_t bit = n >> 1;
    for (; (j & bit) != 0; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (std::size_t len = 2; len <= n; len <<= 1) {
    const double angle = -2.0 * std::numbers::pi / static_cast<double>(len);
    const std::complex<double> wl(std::cos(angle), std::sin(angle));
    for (std::size_t i = 0; i < n; i += len) {
      std::complex<double> w(1.0, 0.0);
      for (std::size_t k = 0; k < len / 2; ++k) {
        const std::complex<double> u = a[i + k];
        const std::complex<double> v = a[i + k + len / 2] * w;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}

inline double bark(double hz) { return 13.0 * std::atan(0.00076 * hz) + 3.5 * std::atan((hz / 7500.0) * (hz / 7500.0)); }

/// Absolute threshold of hearing in dB SPL (Terhardt).
inline double ath_db(double hz) {
  const double f = std::clamp(hz, 20.0, 20000.0) / 1000.0;
  return 3.64 * std::pow(f, -0.8) - 6.5 * std::exp(-0.6 * (f - 3.3) * (f - 3.3)) + 0.001 * f * f * f * f;
}

/// Spreading function (Schroeder), dz = maskee - masker in Bark; linear power.
inline double spreading(double dz) {
  const double t = dz + 0.474;
  const double db = 15.81 + 7.5 * t - 17.5 * std::sqrt(1.0 + t * t);
  return db < -60.0 ? 0.0 : std::pow(10.0, db / 10.0);
}

/// Masking model over one FFT size.
struct PsyBank {
  std::size_t n = 0;                  ///< FFT size
  std::vector<double> window;         ///< Hann
  std::vector<int> partition_of;      ///< bin -> partition
  std::vector<int> partition_first;   ///< first bin of each partition
  std::vector<double> partition_bark;
  std::vector<std::vector<double>> spread;  ///< [maskee][masker], normalised per maskee
  std::vector<double> ath;            ///< per bin, energy units of this FFT

  void init(std::size_t size, double rate, double ath_offset_db, double width_bark) {
    n = size;
    window.resize(n);
    for (std::size_t i = 0; i < n; ++i) window[i] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * (static_cast<double>(i) + 0.5) / static_cast<double>(n));
    const std::size_t bins = n / 2;
    partition_of.assign(bins, 0);
    partition_first.clear();
    partition_bark.clear();
    double start_bark = -1.0;
    for (std::size_t k = 0; k < bins; ++k) {
      const double b = bark((static_cast<double>(k) + 0.5) * rate / static_cast<double>(n));
      if (partition_first.empty() || b - start_bark >= width_bark) {
        partition_first.push_back(static_cast<int>(k));
        start_bark = b;
        partition_bark.push_back(b);
      }
      partition_of[k] = static_cast<int>(partition_first.size()) - 1;
    }
    const std::size_t parts = partition_first.size();
    spread.assign(parts, std::vector<double>(parts, 0.0));
    for (std::size_t p = 0; p < parts; ++p) {
      double sum = 0.0;
      for (std::size_t q = 0; q < parts; ++q) {
        spread[p][q] = spreading(partition_bark[p] - partition_bark[q]);
        sum += spread[p][q];
      }
      for (std::size_t q = 0; q < parts; ++q) spread[p][q] /= sum;
    }
    // A full-scale sine has a peak bin energy of about (n/4)^2 with this
    // window; it is taken as 96 dB SPL.
    const double full = (static_cast<double>(n) / 4.0) * (static_cast<double>(n) / 4.0);
    ath.resize(bins);
    for (std::size_t k = 0; k < bins; ++k) {
      const double hz = (static_cast<double>(k) + 0.5) * rate / static_cast<double>(n);
      ath[k] = full * std::pow(10.0, (ath_db(hz) - 96.0 + ath_offset_db) / 10.0);
    }
  }

  /// Energies and masking thresholds per bin of `samples` (n values).
  void analyse(const double* samples, std::vector<double>& energy, std::vector<double>& threshold) const {
    std::vector<std::complex<double>> a(n);
    for (std::size_t i = 0; i < n; ++i) a[i] = {samples[i] * window[i], 0.0};
    fft(a);
    const std::size_t bins = n / 2;
    energy.resize(bins);
    for (std::size_t k = 0; k < bins; ++k) energy[k] = std::norm(a[k]);
    const std::size_t parts = partition_first.size();
    std::vector<double> e(parts, 0.0), offset(parts, 0.0);
    for (std::size_t p = 0; p < parts; ++p) {
      const std::size_t first = psy_detail::z(partition_first[p]);
      const std::size_t last = p + 1 < parts ? psy_detail::z(partition_first[p + 1]) : bins;
      double sum = 0.0, log_sum = 0.0;
      for (std::size_t k = first; k < last; ++k) {
        sum += energy[k];
        log_sum += std::log(energy[k] + 1e-12);
      }
      e[p] = sum;
      const auto count = static_cast<double>(last - first);
      double tonality = 0.5;
      if (count >= 2 && sum > 1e-9) {
        const double geometric = std::exp(log_sum / count);
        const double sfm_db = 10.0 * std::log10(geometric / (sum / count) + 1e-12);
        tonality = std::clamp(sfm_db / -25.0, 0.0, 1.0);
      }
      // Tonal maskers mask less than noise (Johnston's offsets).
      const double offset_db = tonality * (14.5 + partition_bark[p]) + (1.0 - tonality) * 5.5;
      offset[p] = std::pow(10.0, -offset_db / 10.0);
    }
    std::vector<double> thr(parts, 0.0);
    for (std::size_t p = 0; p < parts; ++p) {
      double sum = 0.0;
      for (std::size_t q = 0; q < parts; ++q) sum += spread[p][q] * e[q] * offset[q];
      thr[p] = sum;
    }
    threshold.resize(bins);
    for (std::size_t p = 0; p < parts; ++p) {
      const std::size_t first = psy_detail::z(partition_first[p]);
      const std::size_t last = p + 1 < parts ? psy_detail::z(partition_first[p + 1]) : bins;
      const double per_bin = thr[p] / static_cast<double>(last - first);
      for (std::size_t k = first; k < last; ++k) threshold[k] = std::max(per_bin, ath[k]);
    }
  }
};

}  // namespace cvt::lossy::mpeg
