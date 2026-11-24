// Convertible core - audio_lossy family: MPEG audio files (.mp3/.mp2/.mp1)
// and the encoders behind their writers.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "codec/codec.hpp"
#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "ir/audio.hpp"
#include "util/bytes.hpp"

namespace cvt::lossy::mpeg {

/// Tags found around an MPEG audio stream and where the stream lies.
struct TagScan {
  std::map<std::string, std::string> tags;
  std::size_t start = 0;  ///< first byte after leading ID3v2 tags
  std::size_t end = 0;    ///< first byte of trailing APE/Lyrics3/ID3v1 tags
};
[[nodiscard]] TagScan scan_tags(ByteView data);

/// Decode a whole MPEG audio file (any layer) with gapless trimming.
[[nodiscard]] Result<ir::Audio> read_mpeg_file(ByteView data, const Context& ctx);

/// Settings of the Layer III encoder.
struct Mp3Settings {
  int quality = 90;                      ///< 1..100
  std::optional<std::uint32_t> bitrate;  ///< kbit/s; with `mode` cbr/abr
  enum class Mode { Vbr, Cbr, Abr } mode = Mode::Vbr;
  enum class Stereo { Auto, Joint, Stereo, Mono } stereo = Stereo::Auto;
  bool write_info_tag = true;            ///< Xing/Info frame with encoder delay/padding
};

/// Encode `audio` (rate already one MP3 supports) as a complete MP3 stream
/// (without ID3 tags).
[[nodiscard]] Result<Bytes> encode_mp3(const ir::Audio& audio, const Mp3Settings& settings, const Context& ctx);

/// Sample rates the Layer III encoder accepts (MPEG-1, 2 and 2.5).
[[nodiscard]] bool mp3_rate_supported(std::uint32_t rate) noexcept;
/// The supported rate closest to `rate`.
[[nodiscard]] std::uint32_t nearest_mp3_rate(std::uint32_t rate) noexcept;

/// Settings of the Layer II encoder.
struct Mp2Settings {
  std::uint32_t bitrate = 192;  ///< kbit/s, a legal Layer II rate
  bool joint_stereo = true;
};
[[nodiscard]] Result<Bytes> encode_mp2(const ir::Audio& audio, const Mp2Settings& settings, const Context& ctx);

/// Codec-registry objects (packets = whole frames).
[[nodiscard]] std::unique_ptr<codec::AudioDecoder> make_mpeg_decoder();
[[nodiscard]] std::unique_ptr<codec::AudioEncoder> make_mp3_encoder();
[[nodiscard]] std::unique_ptr<codec::AudioEncoder> make_mp2_encoder();

}  // namespace cvt::lossy::mpeg
