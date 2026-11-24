// Convertible core - audio_lossless family: internal declarations shared by
// the format modules of this family.
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

namespace cvt {
class Registry;
}

namespace cvt::audio {

// Registration of the modules (each in its own .cpp).
void register_wav(Registry& registry);
void register_aiff(Registry& registry);
void register_au(Registry& registry);
void register_caf(Registry& registry);
void register_raw(Registry& registry);
void register_voc(Registry& registry);
void register_svx(Registry& registry);
void register_sphere(Registry& registry);
void register_flac(Registry& registry);
void register_m4a(Registry& registry);
void register_wavpack(Registry& registry);
void register_tta(Registry& registry);
void register_media_codecs(Registry& registry);
void register_bridges(Registry& registry);

/// Refuse to decode more than the limits allow: `samples` float samples are
/// about to be produced (counted against max_decompressed_bytes at 4 bytes
/// each).
[[nodiscard]] Result<void> check_sample_budget(const Context& ctx, std::uint64_t samples);

/// Pre-sized empty audio with the source description filled in.
[[nodiscard]] ir::Audio make_audio(std::uint32_t sample_rate, std::uint16_t channels, std::uint16_t bits,
                                   bool float_samples = false, std::uint32_t channel_mask = 0);

/// Validate the basic stream parameters a header declared.
[[nodiscard]] Result<void> check_stream_params(std::uint32_t sample_rate, std::uint32_t channels,
                                               std::string_view what);

/// Truncate/pad `audio.samples` to exactly `frames` frames when the container
/// states a length (padding never happens for well-formed input).
void set_frame_count(ir::Audio& audio, std::uint64_t frames);

/// IEEE 754 80-bit extended (AIFF sample rate) <-> double.
[[nodiscard]] double read_extended(ByteView ten_bytes) noexcept;
void write_extended(util::ByteWriter& out, double value);

}  // namespace cvt::audio
