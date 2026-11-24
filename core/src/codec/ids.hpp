// Convertible core - canonical codec identifiers.
//
// Containers (demuxers/muxers) and codecs (decoders/encoders) are written by
// different people and meet only through these strings: a demuxer labels a
// track with one of them (ir::Track::codec), and looks the decoder up under
// the same name in codec::Registry. Always use the constants; never invent a
// spelling. To add a codec, append it here (spelled like the FFmpeg codec
// name, which is what users and tools know).
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
inline constexpr std::string_view wmv1 = "wmv1";              ///< Windows Media Video 7
inline constexpr std::string_view wmv2 = "wmv2";              ///< Windows Media Video 8
inline constexpr std::string_view wmv3 = "wmv3";              ///< Windows Media Video 9 (VC-1 Simple/Main)
inline constexpr std::string_view vc1 = "vc1";                ///< SMPTE VC-1 Advanced profile (WVC1)
inline constexpr std::string_view msmpeg4v3 = "msmpeg4v3";    ///< Microsoft MPEG-4 v3 / DivX ;-) 3
inline constexpr std::string_view flv1 = "flv1";              ///< Sorenson Spark (H.263 flavour in FLV)
inline constexpr std::string_view vp6f = "vp6f";              ///< On2 VP6 in FLV
inline constexpr std::string_view rv10 = "rv10";              ///< RealVideo 1.0
inline constexpr std::string_view rv20 = "rv20";              ///< RealVideo 2.0
inline constexpr std::string_view rv30 = "rv30";              ///< RealVideo 3.0
inline constexpr std::string_view rv40 = "rv40";              ///< RealVideo 4.0
inline constexpr std::string_view huffyuv = "huffyuv";        ///< HuffYUV ('HFYU')
inline constexpr std::string_view ffv1 = "ffv1";              ///< FFV1 (RFC 9043)
inline constexpr std::string_view utvideo = "utvideo";        ///< Ut Video ('ULY0', 'ULRG', ...)
inline constexpr std::string_view cinepak = "cinepak";        ///< Cinepak ('cvid')
inline constexpr std::string_view msrle = "msrle";            ///< Microsoft RLE (BI_RLE8 / BI_RLE4)
inline constexpr std::string_view msvideo1 = "msvideo1";      ///< Microsoft Video 1 ('CRAM')
inline constexpr std::string_view qtrle = "qtrle";            ///< QuickTime Animation ('rle ')
inline constexpr std::string_view rpza = "rpza";              ///< QuickTime Video ('rpza')
inline constexpr std::string_view smc = "smc";                ///< QuickTime Graphics ('smc ')
inline constexpr std::string_view uyvy = "uyvy";              ///< packed 4:2:2 8-bit U Y V Y ('2vuy', 'UYVY')
inline constexpr std::string_view yuy2 = "yuy2";              ///< packed 4:2:2 8-bit Y U Y V ('YUY2', 'yuvs')
inline constexpr std::string_view v210 = "v210";              ///< packed 4:2:2 10-bit ('v210')
inline constexpr std::string_view bgr24 = "bgr24";            ///< 24-bit BGR rows (AVI BI_RGB), bottom-up by default
inline constexpr std::string_view r210 = "r210";              ///< packed 10-bit RGB, big-endian words ('r210')

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
inline constexpr std::string_view pcm_s24be = "pcm_s24be";
inline constexpr std::string_view pcm_s32le = "pcm_s32le";
inline constexpr std::string_view pcm_f64le = "pcm_f64le";
inline constexpr std::string_view pcm_s8 = "pcm_s8";
inline constexpr std::string_view tta = "tta";                ///< True Audio
inline constexpr std::string_view gsm_ms = "gsm_ms";          ///< GSM 06.10 in the Microsoft WAV packing (65-byte blocks)
inline constexpr std::string_view adpcm_ima_qt = "adpcm_ima_qt";  ///< Apple IMA4 (34-byte blocks per channel)
inline constexpr std::string_view wmav1 = "wmav1";
inline constexpr std::string_view wmav2 = "wmav2";
inline constexpr std::string_view wmapro = "wmapro";
inline constexpr std::string_view eac3 = "eac3";              ///< Dolby Digital Plus
inline constexpr std::string_view cook = "cook";              ///< RealAudio Cooker
inline constexpr std::string_view ra_144 = "ra_144";          ///< RealAudio 1.0 (14.4)

// ---- subtitles --------------------------------------------------------------
inline constexpr std::string_view subrip = "subrip";          ///< SRT text cues
inline constexpr std::string_view webvtt = "webvtt";
inline constexpr std::string_view mov_text = "mov_text";      ///< 3GPP timed text ('tx3g')
inline constexpr std::string_view ass = "ass";                ///< ASS/SSA events

/// Every canonical video codec id.
inline constexpr std::array video = {h264,    hevc,   vp8,      vp9,   av1,      mpeg1video, mpeg2video, mpeg4,
                                     h263,    theora, mjpeg,    prores, rawvideo, gif,       png,        wmv1,
                                     wmv2,    wmv3,   vc1,      msmpeg4v3, flv1, vp6f,      rv10,       rv20,
                                     rv30,    rv40,   huffyuv,  ffv1,  utvideo,  cinepak,   msrle,      msvideo1,
                                     qtrle,   rpza,   smc,      uyvy,  yuy2,     v210,      bgr24,      r210};
/// Every canonical audio codec id.
inline constexpr std::array audio = {aac,       mp3,       mp2,       opus,      vorbis,    flac,   alac,
                                     ac3,       pcm_s16le, pcm_s24le, pcm_f32le, pcm_u8,    pcm_s16be, pcm_alaw,
                                     pcm_mulaw, adpcm_ima, adpcm_ms,  amr_nb,    amr_wb,    speex,  wavpack,
                                     pcm_s24be, pcm_s32le, pcm_f64le, pcm_s8,    tta,       gsm_ms, adpcm_ima_qt,
                                     wmav1,     wmav2,     wmapro,    eac3,      cook,      ra_144};
/// Every canonical subtitle codec id.
inline constexpr std::array subtitle = {subrip, webvtt, mov_text, ass};

/// True when `name` is one of the canonical ids above.
[[nodiscard]] constexpr bool is_known(std::string_view name) noexcept {
  for (const std::string_view known : video) {
    if (known == name) return true;
  }
  for (const std::string_view known : audio) {
    if (known == name) return true;
  }
  for (const std::string_view known : subtitle) {
    if (known == name) return true;
  }
  return false;
}

}  // namespace cvt::codec::id
