// Convertible core - codec_vpx family: internal interface.
//
// VP8 and VP9 decoders and encoders built on the imported libvpx (generic C
// build). Other families never include this header: they reach the codecs
// through `registry.media()` under codec::id::vp8 / codec::id::vp9.
//
// Conventions of these codecs
//   Decoders  Output frames in the stream's own layout: I420 / I422 / I444
//             (8 bit) or I420P10 / I422P10 / I444P10 (VP9 profiles 2 and 3;
//             12-bit streams are rounded to 10 bit), RGBA8 for VP9 sRGB
//             streams; 4:4:0 streams are delivered as 4:4:4. Hidden
//             (alternate reference) frames, VP9 superframes and
//             show-existing frames are handled; every shown frame carries the
//             timestamp of the packet that displayed it, and a duration from
//             the packet or, failing that, from the next frame's timestamp.
//             `extradata` may be empty or a vpcC payload (used for the VP8
//             colour description only, since VP9 signals it in-band).
//   Encoders  Constant quality by default (`quality` 1..100 mapped onto the
//             quantiser scale; 100 = lossless VP9 / finest VP8 quantiser),
//             average bitrate when `bitrate_kbps` is set. Good-quality
//             deadline; the preset picks the effort (cpu-used). Single pass
//             without look-ahead, one thread, deterministic; one packet per
//             frame, in order (pts = dts). libvpx makes hidden alternate
//             reference frames only in two-pass mode, which a streaming
//             encoder cannot offer, so the output has none.
//             `extradata()` is a vpcC payload (version 1) for both codecs.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include "codec/codec.hpp"

namespace cvt::vpx {

enum class Codec { Vp8, Vp9 };

/// Largest decoded picture accepted, in pixels (8192 x 8192). Frames that
/// declare more are refused before libvpx allocates anything.
inline constexpr std::uint64_t max_frame_pixels = 8192ull * 8192ull;

[[nodiscard]] std::unique_ptr<codec::VideoDecoder> make_decoder(Codec codec);
[[nodiscard]] std::unique_ptr<codec::VideoEncoder> make_encoder(Codec codec);

/// The quantiser (libvpx "cq-level", 0..63) used for `quality` 1..100.
[[nodiscard]] int quantizer_for_quality(int quality) noexcept;
/// libvpx "cpu-used" (effort) for a preset; lower is slower and better.
[[nodiscard]] int cpu_used_for_preset(Codec codec, codec::EncodePreset preset) noexcept;
/// Keyframe distance used when the caller leaves it to the encoder.
[[nodiscard]] std::uint32_t default_keyframe_interval(double fps) noexcept;

/// "v1.16.0" - the libvpx version compiled in.
[[nodiscard]] std::string_view libvpx_version() noexcept;

}  // namespace cvt::vpx
