// Convertible core - audio_lossy family: raw GSM 06.10 full-rate files
// (.gsm, 33-byte frames as written by libgsm/sox): the frames are repacked
// into Microsoft's 65-byte two-frame layout and decoded by the bit-exact
// GSM 06.10 decoder of the lossless family.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/audio_lossy/speech/speech.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "families/audio_lossless/common.hpp"
#include "families/audio_lossless/pcm.hpp"

namespace cvt::lossy::speech {

namespace {

constexpr std::array<int, 76> param_bits = [] {
  std::array<int, 76> b{};
  constexpr int lar[8] = {6, 6, 5, 5, 4, 4, 3, 3};
  std::size_t i = 0;
  for (const int v : lar) b[i++] = v;
  for (int s = 0; s < 4; ++s) {
    b[i++] = 7;
    b[i++] = 2;
    b[i++] = 2;
    b[i++] = 6;
    for (int k = 0; k < 13; ++k) b[i++] = 3;
  }
  return b;
}();

/// MSB-first parameters of a 33-byte frame (after the 0xD signature).
std::array<int, 76> unpack(const std::uint8_t* frame) {
  std::array<int, 76> p{};
  std::size_t bit = 4;
  for (std::size_t i = 0; i < 76; ++i) {
    int v = 0;
    for (int k = 0; k < param_bits[i]; ++k, ++bit) v = (v << 1) | ((frame[bit >> 3] >> (7 - (bit & 7))) & 1);
    p[i] = v;
  }
  return p;
}

/// Append parameters LSB-first (Microsoft GSM 6.10 bit order).
void pack_lsb(const std::array<int, 76>& p, std::array<std::uint8_t, 65>& out, std::size_t& bit) {
  for (std::size_t i = 0; i < 76; ++i) {
    for (int k = 0; k < param_bits[i]; ++k, ++bit) {
      if (((p[i] >> k) & 1) != 0) out[bit >> 3] = static_cast<std::uint8_t>(out[bit >> 3] | (1u << (bit & 7)));
    }
  }
}

}  // namespace

bool is_gsm(ByteView data) {
  const auto d = as_u8(data);
  if (d.size() < 33 || d.size() % 33 != 0) return false;
  for (std::size_t i = 0; i < d.size() && i < 33 * 16; i += 33) {
    if ((d[i] >> 4) != 0xD) return false;
  }
  return true;
}

Result<ir::Audio> read_gsm(ByteView data, const Context& ctx) {
  const auto d = as_u8(data);
  const std::size_t frames = d.size() / 33;
  if (frames == 0) return fail(Status::Decode, "the GSM file holds no complete frame");
  CVT_TRY(audio::check_sample_budget(ctx, std::uint64_t{frames} * 160));
  Bytes blocks;
  blocks.reserve((frames + 1) / 2 * 65);
  for (std::size_t f = 0; f < frames; f += 2) {
    if ((f & 1023) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(f) / static_cast<double>(frames), "Decoding"));
    if ((d[f * 33] >> 4) != 0xD) return fail(Status::Decode, "GSM frame {} has no 0xD signature", f);
    std::array<std::uint8_t, 65> block{};
    std::size_t bit = 0;
    pack_lsb(unpack(d.data() + f * 33), block, bit);
    if (f + 1 < frames) {
      if ((d[(f + 1) * 33] >> 4) != 0xD) return fail(Status::Decode, "GSM frame {} has no 0xD signature", f + 1);
      pack_lsb(unpack(d.data() + (f + 1) * 33), block, bit);
    } else {
      pack_lsb(std::array<int, 76>{}, block, bit);
    }
    const auto* p = reinterpret_cast<const std::byte*>(block.data());
    blocks.insert(blocks.end(), p, p + 65);
  }
  ir::Audio audio;
  audio.sample_rate = 8000;
  audio.channels = 1;
  CVT_TRY(audio::decode_gsm610_wav(blocks, audio.samples));
  audio.samples.resize(frames * 160);
  return audio;
}

}  // namespace cvt::lossy::speech
