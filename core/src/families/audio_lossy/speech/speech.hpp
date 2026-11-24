// Convertible core - audio_lossy family: speech codecs (AMR-NB, AMR-WB,
// GSM 06.10).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <memory>

#include "codec/codec.hpp"
#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "ir/audio.hpp"
#include "util/bytes.hpp"

namespace cvt::lossy::speech {

/// True for the RFC 4867 storage format; `wide` = AMR-WB.
[[nodiscard]] bool is_amr(ByteView data, bool& wide);
[[nodiscard]] Result<ir::Audio> read_amr(ByteView data, const Context& ctx);
/// `audio` mono at 8 kHz (narrowband) or 16 kHz (wideband); the highest
/// mode not above `bitrate` bit/s is used.
[[nodiscard]] Result<Bytes> write_amr(const ir::Audio& audio, bool wide, std::uint32_t bitrate, bool dtx,
                                      const Context& ctx);
[[nodiscard]] std::unique_ptr<codec::AudioDecoder> make_amr_decoder(bool wide);

[[nodiscard]] bool is_gsm(ByteView data);
[[nodiscard]] Result<ir::Audio> read_gsm(ByteView data, const Context& ctx);

}  // namespace cvt::lossy::speech
