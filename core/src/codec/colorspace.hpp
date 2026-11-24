// Convertible core - conversion between RGBA and the YCbCr pixel formats.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include "codec/codec.hpp"
#include "engine/errors.hpp"
#include "ir/video.hpp"

namespace cvt::codec {

// Conventions shared by the three functions below
//
//   Colour     The matrices relate gamma-encoded R'G'B' to Y'CbCr; no transfer
//              function is applied or removed. RGBA8 holds that same R'G'B'
//              (8 bit, full range). Kr/Kb: BT.601 0.299/0.114, BT.709
//              0.2126/0.0722, BT.2020 non-constant luminance 0.2627/0.0593.
//   Range      n-bit Limited: Y' = 16..235 and CbCr = 16..240, times 2^(n-8),
//              neutral chroma 128 * 2^(n-8). n-bit Full: Y' = 0..2^n-1, CbCr =
//              2^(n-1) +/- (2^n-1)/2 (JFIF for 8 bit, ITU-T H.273 in general).
//   Siting     Left: chroma on the even luma columns, midway between rows.
//              Center: midway in both directions. TopLeft: on the even
//              columns and even rows. 4:2:2 formats use the horizontal rule
//              only; 4:4:4 formats ignore the siting.
//   Rounding   Arithmetic is double precision; stored values are rounded to
//              nearest with ties away from zero (for Cb/Cr: away from neutral
//              chroma) and clamped to the representable sample range, 0..255
//              or 0..1023 - not to the nominal limited range, so super-white
//              and sub-black survive wherever the target can hold them.
//   10 bit     Samples are read and written byte-wise as little-endian words.
//              I4xxP10 values above 1023 are treated as 1023; the six low
//              bits of P010 words are ignored on input and zero on output.
//   Input      Any stride >= the row size is accepted and padding is never
//              read. Besides `VideoFrame::valid()` the functions verify the
//              plane sizes without overflow; frames that fail either check
//              are refused with Status::InvalidArgument, frames of more than
//              2^31 pixels (or whose RGBA size does not fit the address
//              space) with Status::Limit.
//   Output     Tightly packed planes (`VideoFrame::allocate`), `pts_ms` and
//              `duration_ms` copied from the input. `color` is the requested
//              ColorInfo for YCbCr results and the input's ColorInfo,
//              unchanged and for information only, for RGBA8 results.

/// Convert any frame to PixelFormat::RGBA8 (tightly packed, stride =
/// width * 4, alpha 255).
///
/// Uses `frame.color` for the matrix and range, reconstructs subsampled
/// chroma by bilinear interpolation at the positions given by
/// `frame.color.siting` (edge samples replicate, so flat chroma stays exactly
/// flat) and rounds to nearest. 10-bit input is converted at full precision
/// before being reduced to 8 bits. An RGBA8 input is returned as a tightly
/// packed copy (alpha included). Timestamps are preserved.
/// Fails with Status::InvalidArgument when `frame.valid()` is false.
[[nodiscard]] Result<VideoFrame> to_rgba(const VideoFrame& frame);

/// Convert an RGBA8 frame to `format` (tightly packed planes) using the given
/// matrix, range and chroma siting. Alpha is ignored (not premultiplied in).
/// Every pixel is converted to Y'CbCr first; chroma is then downsampled with
/// a filter matched to `color.siting` (not by dropping samples): a box over
/// the two (4:2:2) or four (4:2:0) covered pixels where chroma is centred,
/// [1 2 1] / 4 around the even column or row where it is co-sited. Odd
/// dimensions are handled by edge replication, and results are rounded to
/// nearest and clamped to the sample range.
/// `format == RGBA8` yields a tightly packed copy. Timestamps are preserved.
/// Fails with Status::InvalidArgument when `rgba` is not a valid RGBA8 frame.
[[nodiscard]] Result<VideoFrame> from_rgba(const VideoFrame& rgba, PixelFormat format, const ColorInfo& color = {});

/// Convert between any two formats without an 8-bit round trip.
///
///   - Identical format and colour info: a tightly packed copy.
///   - RGBA8 on either side: `from_rgba(frame, format, color)` or
///     `to_rgba(frame)` (`color` is then ignored).
///   - YCbCr to YCbCr with the same matrix: each plane is rescaled directly
///     for range and bit depth. Limited range scales by exactly 2^(n-8), so
///     8 -> 10 bit is `v * 4`; full range scales by (2^n-1) ratios, so 8 -> 10
///     bit luma is round(v * 1023 / 255). Chroma is resampled only along an
///     axis whose subsampling or sample position changes; I420 <-> NV12,
///     8 <-> 10 bit and limited <-> full therefore keep every chroma sample in
///     place, and 8 -> 10 -> 8 bit is lossless.
///   - YCbCr to YCbCr with different matrices: through full-precision,
///     unclamped R'G'B' - chroma interpolated to the luma grid as in
///     `to_rgba`, then filtered down as in `from_rgba`. Out-of-gamut values
///     are clamped only where the target cannot store them.
[[nodiscard]] Result<VideoFrame> convert_frame(const VideoFrame& frame, PixelFormat format, const ColorInfo& color);

}  // namespace cvt::codec
