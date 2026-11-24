// Convertible core - audio_lossless family: minimal Ogg page demultiplexer
// (one logical stream, CRC-checked pages, packet reassembly).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "util/bytes.hpp"

namespace cvt::audio {

struct OggStream {
  std::vector<Bytes> packets;
  /// Pages with a bad checksum or a sequence gap were dropped.
  bool damaged = false;
};

/// The packets of the first logical stream whose first packet starts with
/// `magic`. The total packet bytes count against the decompression limit.
[[nodiscard]] Result<OggStream> read_ogg_stream(ByteView input, std::string_view magic, const Context& ctx);

/// The Ogg CRC-32 (polynomial 0x04C11DB7, not reflected) of a page, with
/// the checksum field counted as zero.
[[nodiscard]] std::uint32_t ogg_crc(ByteView page) noexcept;

}  // namespace cvt::audio
