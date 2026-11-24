// Convertible core - video family: the transcoding pipeline.
//
//   demux -> decode -> rotate -> pixel format -> scale -> frame rate -> encode -> mux
//                   -> channel mix -> resample ----------------------> encode -/
//
// Every container writer of this family goes through `write_container`. Per
// media type it either COPIES the compressed packets (remux: lossless and
// near-instant, chosen automatically when the target can store the source
// codec and no option asks for re-encoding) or decodes, filters and encodes
// through codec::Registry. Streams are pulled one frame/chunk at a time and
// interleaved by decode time, so memory stays bounded by a few frames plus
// whatever the encoder buffers; the output file itself is assembled in memory
// as the engine requires.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "engine/context.hpp"
#include "engine/options.hpp"
#include "engine/registry.hpp"
#include "ir/video.hpp"

namespace cvt::video {

// --------------------------------------------------------------------------- options

/// Options of every container writer (codec choice, size, rate, bitrates, trim, streams ...).
[[nodiscard]] std::vector<OptionSpec> container_write_options(std::string_view format_id);
/// Options of the readers (`audio_track`, `raw_fps`).
[[nodiscard]] std::vector<OptionSpec> container_read_options(bool raw_stream);

/// Write `video` as `format_id` (a container with a policy in policy.hpp).
[[nodiscard]] Result<Bytes> write_container(const ir::Video& video, WriteContext& ctx, std::string_view format_id);

// --------------------------------------------------------------------------- frame filters

/// High-quality resize (separable Lanczos-3, widened when shrinking so that
/// it also low-pass filters) applied directly to each plane of any pixel
/// format; chroma planes are resized to the chroma size of the new frame.
[[nodiscard]] Result<ir::VideoFrame> scale_frame(const ir::VideoFrame& frame, std::uint32_t width, std::uint32_t height);
/// Rotate clockwise by 90, 180 or 270 degrees (4:2:2 input becomes 4:4:4).
[[nodiscard]] Result<ir::VideoFrame> rotate_frame(const ir::VideoFrame& frame, int degrees);

// --------------------------------------------------------------------------- audio

/// Sample-rate converter used by the pipeline.
///
/// SEAM: implemented with the lossless audio family's DSP toolbox
/// (families/audio_lossless/audio_dsp.hpp: polyphase Kaiser-windowed sinc,
/// exact output length, no delay); nothing else in the pipeline depends on it.
class AudioResampler {
 public:
  virtual ~AudioResampler() = default;
  /// Feed interleaved samples; returns the output produced so far. Pass
  /// `flush = true` once at the end to drain the filter tail.
  [[nodiscard]] virtual std::vector<float> process(std::span<const float> interleaved, bool flush) = 0;
};
[[nodiscard]] std::unique_ptr<AudioResampler> make_resampler(std::uint32_t in_rate, std::uint32_t out_rate,
                                                             std::uint16_t channels);
/// Re-map interleaved samples between channel layouts (WAVE order) with the
/// toolbox's ITU-R BS.775 matrices (centre and surrounds at -3 dB, LFE dropped,
/// normalised so that full scale cannot clip; mono goes to both fronts).
[[nodiscard]] std::vector<float> remix_channels(std::span<const float> interleaved, std::uint16_t from, std::uint16_t to);

}  // namespace cvt::video
