// Convertible core - audio_lossless family: FLAC file-level helpers shared
// by the .flac reader/writer, the media codecs and containers embedding FLAC.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "families/audio_lossless/flac_codec.hpp"
#include "families/audio_lossless/tags.hpp"
#include "ir/audio.hpp"
#include "util/bytes.hpp"

namespace cvt::audio {

/// Stream-level state while reading FLAC metadata and frames.
struct FlacStream {
  flac::StreamInfo info;
  bool have_info = false;
  std::uint32_t channel_mask = 0;
  bool md5_verified = false;

  /// Handle one metadata block (STREAMINFO, VORBIS_COMMENT; others ignored).
  [[nodiscard]] Result<void> add_metadata_block(std::uint8_t type, ByteView body, tags::TagMap& tags);
  /// Trim to the declared length, verify the MD5 and convert to float.
  [[nodiscard]] Result<ir::Audio> finish(const std::vector<std::int32_t>& samples, const Context& ctx, bool truncated);
};

/// Decode a complete native FLAC file.
[[nodiscard]] Result<ir::Audio> decode_flac_file(ByteView input, const Context& ctx);
/// Encode `audio` as a FLAC file with `bits`-bit samples at `level` (0..8).
/// Samples are quantised with the `dither` option of `ctx.options`.
[[nodiscard]] Result<Bytes> encode_flac_file(const ir::Audio& audio, int bits, int level, bool keep_tags,
                                             const Context& ctx);

}  // namespace cvt::audio
