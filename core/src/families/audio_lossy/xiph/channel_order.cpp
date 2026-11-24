// Convertible core - audio_lossy family: Vorbis/Opus channel order <-> WAVE order.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

#include "families/audio_lossy/xiph/xiph.hpp"

namespace cvt::lossy::xiph {

std::array<int, 8> vorbis_to_wave_map(int channels) noexcept {
  // Vorbis order (I.5 of the Vorbis spec):
  //   3: L C R   5: FL C FR RL RR   6: FL C FR RL RR LFE
  //   7: FL C FR SL SR RC LFE   8: FL C FR SL SR RL RR LFE
  // WAVE order: FL FR FC LFE BL BR (BC) SL SR.
  switch (channels) {
    case 3: return {0, 2, 1, 3, 4, 5, 6, 7};
    case 5: return {0, 2, 1, 3, 4, 5, 6, 7};
    case 6: return {0, 2, 1, 5, 3, 4, 6, 7};
    case 7: return {0, 2, 1, 6, 5, 3, 4, 7};
    case 8: return {0, 2, 1, 7, 5, 6, 3, 4};
    default: return {0, 1, 2, 3, 4, 5, 6, 7};
  }
}

void vorbis_to_wave(std::span<float> samples, int channels) {
  if (channels < 3 || channels > 8) return;
  const auto map = vorbis_to_wave_map(channels);
  const auto n = static_cast<std::size_t>(channels);
  std::array<float, 8> tmp{};
  for (std::size_t f = 0; f + n <= samples.size(); f += n) {
    for (std::size_t c = 0; c < n; ++c) tmp[c] = samples[f + static_cast<std::size_t>(map[c])];
    std::copy(tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(n), samples.begin() + static_cast<std::ptrdiff_t>(f));
  }
}

void wave_to_vorbis(std::span<float> samples, int channels) {
  if (channels < 3 || channels > 8) return;
  const auto map = vorbis_to_wave_map(channels);
  const auto n = static_cast<std::size_t>(channels);
  std::array<float, 8> tmp{};
  for (std::size_t f = 0; f + n <= samples.size(); f += n) {
    for (std::size_t c = 0; c < n; ++c) tmp[static_cast<std::size_t>(map[c])] = samples[f + c];
    std::copy(tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(n), samples.begin() + static_cast<std::ptrdiff_t>(f));
  }
}

}  // namespace cvt::lossy::xiph
