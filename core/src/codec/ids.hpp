// Convertible core - canonical codec identifiers.
//
// Containers (demuxers/muxers) and codecs (decoders/encoders) are written by
// different people and meet only through these strings: a demuxer labels a
// track with one of them (ir::Track::codec), and looks the decoder up under
// the same name in codec::Registry. Always use the constants; never invent a
// spelling. To add a codec, append it here.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <string_view>

namespace cvt::codec::id {

// ---- video ------------------------------------------------------------------
inline constexpr std::string_view h264 = "h264";              ///< H.264 / AVC
inline constexpr std::string_view hevc = "hevc";              ///< H.265 / HEVC
inline constexpr std::string_view vp8 = "vp8";
inline constexpr std::string_view vp9 = "vp9";
inline constexpr std::string_view av1 = "av1";
inline constexpr std::string_view mpeg1video = "mpeg1video";
inline constexpr std::string_view mpeg2video = "mpeg2video";
inline constexpr std::string_view mpeg4 = "mpeg4";            ///< MPEG-4 Part 2 (DivX/Xvid)
inline constexpr std::string_view h263 = "h263";
inline constexpr std::string_view theora = "theora";
inline constexpr std::string_view mjpeg = "mjpeg";            ///< Motion JPEG: one baseline JPEG per packet
inline constexpr std::string_view prores = "prores";
inline constexpr std::string_view rawvideo = "rawvideo";      ///< uncompressed frames
inline constexpr std::string_view gif = "gif";                ///< GIF frames (LZW, palette)
inline constexpr std::string_view png = "png";                ///< one PNG per packet (APNG, MOV PNG)

// ---- audio ------------------------------------------------------------------
inline constexpr std::string_view aac = "aac";
inline constexpr std::string_view mp3 = "mp3";
inline constexpr std::string_view mp2 = "mp2";
inline constexpr std::string_view opus = "opus";
inline constexpr std::string_view vorbis = "vorbis";
inline constexpr std::string_view flac = "flac";
inline constexpr std::string_view alac = "alac";
inline constexpr std::string_view ac3 = "ac3";
inline constexpr std::string_view pcm_s16le = "pcm_s16le";    ///< signed 16-bit little-endian PCM
inline constexpr std::string_view pcm_s24le = "pcm_s24le";
inline constexpr std::string_view pcm_f32le = "pcm_f32le";    ///< 32-bit float little-endian PCM
inline constexpr std::string_view pcm_u8 = "pcm_u8";
inline constexpr std::string_view pcm_s16be = "pcm_s16be";
inline constexpr std::string_view pcm_alaw = "pcm_alaw";      ///< G.711 A-law
inline constexpr std::string_view pcm_mulaw = "pcm_mulaw";    ///< G.711 mu-law
inline constexpr std::string_view adpcm_ima = "adpcm_ima";    ///< IMA/DVI ADPCM (WAV flavour)
inline constexpr std::string_view adpcm_ms = "adpcm_ms";      ///< Microsoft ADPCM
inline constexpr std::string_view amr_nb = "amr_nb";
inline constexpr std::string_view amr_wb = "amr_wb";
inline constexpr std::string_view speex = "speex";
inline constexpr std::string_view wavpack = "wavpack";

/// Every canonical video codec id.
inline constexpr std::array video = {h264,  hevc, vp8,    vp9,   av1,    mpeg1video, mpeg2video, mpeg4,
                                     h263,  theora, mjpeg, prores, rawvideo, gif,     png};
/// Every canonical audio codec id.
inline constexpr std::array audio = {aac,       mp3,       mp2,      opus,      vorbis,    flac,     alac,
                                     ac3,       pcm_s16le, pcm_s24le, pcm_f32le, pcm_u8,   pcm_s16be, pcm_alaw,
                                     pcm_mulaw, adpcm_ima, adpcm_ms, amr_nb,    amr_wb,    speex,    wavpack};

/// True when `name` is one of the canonical ids above.
[[nodiscard]] constexpr bool is_known(std::string_view name) noexcept {
  for (const std::string_view known : video) {
    if (known == name) return true;
  }
  for (const std::string_view known : audio) {
    if (known == name) return true;
  }
  return false;
}

}  // namespace cvt::codec::id
