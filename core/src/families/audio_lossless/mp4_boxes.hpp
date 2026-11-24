// Convertible core - audio_lossless family: ISO base media box helpers and
// ALAC stream encoding shared by the M4A and CAF modules.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "families/audio_lossless/alac.hpp"
#include "ir/audio.hpp"
#include "util/bytes.hpp"

namespace cvt::audio {

struct Box {
  std::string type;
  ByteView body;
  std::size_t offset = 0;  ///< absolute offset of the body in the parsed buffer
  bool truncated = false;
};

/// The boxes directly inside `data` (`header_offset` = absolute offset of `data`).
[[nodiscard]] std::vector<Box> parse_boxes(ByteView data, std::size_t header_offset);
[[nodiscard]] const Box* find_box(const std::vector<Box>& boxes, std::string_view type);
/// Walk a path of nested boxes ("moov", "udta", "meta", ...); 'meta' full
/// box headers are skipped.
[[nodiscard]] std::optional<Box> find_path(ByteView data, std::initializer_list<std::string_view> path);
void write_box(util::ByteWriter& out, std::string_view type, ByteView body);
[[nodiscard]] Bytes box(std::string_view type, ByteView body);
[[nodiscard]] Bytes full_box(std::string_view type, std::uint8_t version, std::uint32_t flags, ByteView body);

/// Decode the first sound track of an MPEG-4/QuickTime file.
[[nodiscard]] Result<ir::Audio> read_mp4_audio(ByteView input, const Context& ctx);

struct AlacStream {
  alac::Config config;
  std::vector<Bytes> packets;
  std::uint64_t frames = 0;
};
/// Encode audio as ALAC packets (bit depth from `sample_format` / the
/// source, effort from `compression_level`).
[[nodiscard]] Result<AlacStream> encode_alac_stream(const ir::Audio& audio, const Context& ctx);

}  // namespace cvt::audio
