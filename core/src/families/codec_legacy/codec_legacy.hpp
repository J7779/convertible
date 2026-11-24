// Convertible core - codec_legacy family: codec ids and factories.
//
// The decoders and encoders of this family are registered in
// `Registry::media()` under their canonical ids (codec/ids.hpp). Codecs whose
// id is not (yet) canonical are listed below; they are registered as soon as
// their id appears in codec/ids.hpp and are reachable meanwhile through
// `make_video_decoder` / `make_video_encoder` of this header.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "codec/codec.hpp"
#include "codec/registry.hpp"

namespace cvt {
class Registry;
}

namespace cvt::legacy {

/// Codec ids used by this family that codec/ids.hpp does not define (yet).
/// Spelled like the FFmpeg codec names, which containers already map to.
namespace id {
inline constexpr std::string_view flv1 = "flv1";            ///< Sorenson Spark (H.263 variant in FLV)
inline constexpr std::string_view msmpeg4v3 = "msmpeg4v3";  ///< Microsoft MPEG-4 v3 / DivX ;-) 3
inline constexpr std::string_view dnxhd = "dnxhd";          ///< Avid DNxHD / DNxHR (SMPTE VC-3)
inline constexpr std::string_view dvvideo = "dvvideo";      ///< DV25/DV50 (IEC 61834, SMPTE 314M)
inline constexpr std::string_view huffyuv = "huffyuv";      ///< HuffYUV (HFYU)
inline constexpr std::string_view ffv1 = "ffv1";            ///< FFV1 (RFC 9043)
inline constexpr std::string_view utvideo = "utvideo";      ///< Ut Video (ULY0/ULY2/ULH0/ULH2/ULRG/ULRA)
inline constexpr std::string_view cinepak = "cinepak";      ///< Cinepak (cvid)
inline constexpr std::string_view msrle = "msrle";          ///< Microsoft RLE (BI_RLE8 / BI_RLE4)
inline constexpr std::string_view msvideo1 = "msvideo1";    ///< Microsoft Video 1 (CRAM)
inline constexpr std::string_view qtrle = "qtrle";          ///< QuickTime Animation ('rle ')
inline constexpr std::string_view rpza = "rpza";            ///< QuickTime Video ('rpza', "Road Pizza")
inline constexpr std::string_view smc = "smc";              ///< QuickTime Graphics ('smc ')
inline constexpr std::string_view uyvy = "uyvy";            ///< packed 4:2:2 8-bit U Y V Y ('2vuy', 'UYVY')
inline constexpr std::string_view yuy2 = "yuy2";            ///< packed 4:2:2 8-bit Y U Y V ('YUY2', 'yuvs')
inline constexpr std::string_view v210 = "v210";            ///< packed 4:2:2 10-bit ('v210')
inline constexpr std::string_view bgr24 = "bgr24";          ///< bottom-up or top-down 24-bit BGR (AVI BI_RGB)
inline constexpr std::string_view r210 = "r210";            ///< packed 10-bit RGB, big-endian words ('r210')

/// Every id above.
inline constexpr std::string_view all[] = {flv1, msmpeg4v3, dnxhd, dvvideo, huffyuv, ffv1, utvideo, cinepak, msrle,
                                           msvideo1, qtrle, rpza, smc, uyvy, yuy2, v210, bgr24, r210};
}  // namespace id

/// One codec implemented by this family; a missing direction is empty.
struct CodecEntry {
  std::string_view id;
  codec::VideoDecoderFactory decoder;
  codec::VideoEncoderFactory encoder;
};

/// Every codec of this family, in registration order.
[[nodiscard]] std::vector<CodecEntry> codec_entries();

/// A new decoder/encoder for `codec_id` (canonical or one of `id::*`), or
/// nullptr when this family has none.
[[nodiscard]] std::unique_ptr<codec::VideoDecoder> make_video_decoder(std::string_view codec_id);
[[nodiscard]] std::unique_ptr<codec::VideoEncoder> make_video_encoder(std::string_view codec_id);

/// Registers every codec whose id is canonical in `registry.media()`.
void register_codecs(Registry& registry);

}  // namespace cvt::legacy
