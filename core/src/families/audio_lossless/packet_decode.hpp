// Convertible core - audio_lossless family: decoding a list of compressed
// packets into ir::Audio, with the family's own ALAC decoder or any decoder
// registered in the media codec registry.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "ir/audio.hpp"
#include "util/bytes.hpp"

namespace cvt::audio {

struct PacketTrack {
  /// Canonical codec id (codec/ids.hpp).
  std::string codec;
  Bytes extradata;
  std::uint32_t sample_rate = 0;
  std::uint16_t channels = 0;
  std::uint16_t bits = 0;
  std::uint32_t channel_mask = 0;
  /// Packets in decode order (views into the input file).
  std::vector<ByteView> packets;
  /// Frames to drop at the start (encoder priming) and the total length to
  /// keep after that (0 = everything).
  std::uint64_t priming = 0;
  std::uint64_t valid_frames = 0;
};

/// Decode all packets. ALAC is decoded in-house (bit-exact integers);
/// other codecs go through `ctx.registry->media()` and fail with a clear
/// message when no decoder is registered.
[[nodiscard]] Result<ir::Audio> decode_track(const PacketTrack& track, const Context& ctx);

}  // namespace cvt::audio
