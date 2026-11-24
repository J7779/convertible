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
#include "codec/ids.hpp"
#include "codec/registry.hpp"

namespace cvt {
class Registry;
}

namespace cvt::legacy {

/// The codec ids of this family: canonical ones alias codec/ids.hpp; dnxhd
/// and dvvideo are reserved here until they are implemented.
namespace id {
inline constexpr std::string_view flv1 = codec::id::flv1;
inline constexpr std::string_view msmpeg4v3 = codec::id::msmpeg4v3;
inline constexpr std::string_view dnxhd = "dnxhd";          ///< Avid DNxHD / DNxHR (SMPTE VC-3)
inline constexpr std::string_view dvvideo = "dvvideo";      ///< DV25/DV50 (IEC 61834, SMPTE 314M)
inline constexpr std::string_view huffyuv = codec::id::huffyuv;
inline constexpr std::string_view ffv1 = codec::id::ffv1;
inline constexpr std::string_view utvideo = codec::id::utvideo;
inline constexpr std::string_view cinepak = codec::id::cinepak;
inline constexpr std::string_view msrle = codec::id::msrle;
inline constexpr std::string_view msvideo1 = codec::id::msvideo1;
inline constexpr std::string_view qtrle = codec::id::qtrle;
inline constexpr std::string_view rpza = codec::id::rpza;
inline constexpr std::string_view smc = codec::id::smc;
inline constexpr std::string_view uyvy = codec::id::uyvy;
inline constexpr std::string_view yuy2 = codec::id::yuy2;
inline constexpr std::string_view v210 = codec::id::v210;
inline constexpr std::string_view bgr24 = codec::id::bgr24;
inline constexpr std::string_view r210 = codec::id::r210;

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
