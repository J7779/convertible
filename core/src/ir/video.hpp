// Convertible core - Video IR: a pull-based stream of frames, audio and
// (optionally) compressed packets.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "engine/errors.hpp"
#include "util/bytes.hpp"

namespace cvt::ir {

/// An exact fraction, used for frame rates (30000/1001 = 29.97 fps).
struct Rational {
  std::int64_t num = 0;
  std::int64_t den = 1;

  /// The value as a double; 0 when the denominator is 0.
  [[nodiscard]] constexpr double value() const noexcept {
    return den == 0 ? 0.0 : static_cast<double>(num) / static_cast<double>(den);
  }
  friend bool operator==(const Rational&, const Rational&) = default;
};

/// Stream-level facts known before any frame is decoded.
struct VideoInfo {
  /// Size of the decoded frames in pixels, before `rotation` is applied.
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  /// Nominal frame rate; 0/1 when unknown or variable (use frame timestamps).
  Rational fps;
  /// Total duration in milliseconds; 0 when unknown.
  std::int64_t duration_ms = 0;
  /// Number of video frames when the container states it.
  std::optional<std::uint64_t> frame_count;
  /// Clockwise rotation, in degrees (0, 90, 180 or 270), that a player must
  /// apply for correct display. Frames are delivered unrotated.
  int rotation = 0;
  /// Whether the stream carries audio, and its decoded format.
  bool has_audio = false;
  std::uint32_t audio_sample_rate = 0;  ///< sample frames per second
  std::uint16_t audio_channels = 0;     ///< interleaved channels per sample frame
  /// Container tags ("title", "artist", "creation_time", ...). Lower-case
  /// keys, UTF-8 values.
  std::map<std::string, std::string> metadata;
};

/// Memory layout of a decoded picture.
///
/// Planar YCbCr formats store one plane per component (Y, Cb, Cr); "420"
/// halves the chroma resolution in both directions, "422" horizontally only,
/// "444" not at all. NV12/P010 keep Cb and Cr interleaved in one plane.
/// The 10-bit formats hold one little-endian `uint16_t` per sample: values
/// 0..1023 for the I4xxP10 formats, value << 6 (MSB-aligned) for P010.
enum class PixelFormat {
  RGBA8,    ///< 1 plane: R, G, B, A bytes per pixel, sRGB, straight alpha
  I420,     ///< 3 planes: Y, Cb, Cr; chroma (w+1)/2 x (h+1)/2; 8 bit
  I422,     ///< 3 planes: Y, Cb, Cr; chroma (w+1)/2 x h; 8 bit
  I444,     ///< 3 planes: Y, Cb, Cr; chroma w x h; 8 bit
  NV12,     ///< 2 planes: Y, then interleaved CbCr at (w+1)/2 x (h+1)/2; 8 bit
  I420P10,  ///< as I420 with 10-bit samples in 16-bit words
  I422P10,  ///< as I422 with 10-bit samples in 16-bit words
  I444P10,  ///< as I444 with 10-bit samples in 16-bit words
  P010,     ///< as NV12 with 10-bit samples, MSB-aligned in 16-bit words
};

/// YCbCr <-> RGB matrix coefficients.
enum class ColorMatrix {
  BT601,   ///< SD video, JPEG/JFIF (Kr 0.299, Kb 0.114)
  BT709,   ///< HD video (Kr 0.2126, Kb 0.0722)
  BT2020,  ///< UHD video, non-constant luminance (Kr 0.2627, Kb 0.0593)
};

/// Quantisation range of YCbCr samples.
enum class ColorRange {
  Limited,  ///< "video" range: Y 16..235, CbCr 16..240 (scaled for 10 bit)
  Full,     ///< "PC/JPEG" range: 0..255 (0..1023)
};

/// Where subsampled chroma samples sit relative to the luma grid.
enum class ChromaSiting {
  Left,     ///< MPEG-2/H.264/HEVC default: co-sited with the left luma column, centred between rows
  Center,   ///< JPEG/MPEG-1: centred between luma samples in both directions
  TopLeft,  ///< BT.2020/HDR: co-sited with the top-left luma sample
};

/// How to interpret YCbCr samples. Ignored for RGBA8 frames.
struct ColorInfo {
  ColorMatrix matrix = ColorMatrix::BT709;
  ColorRange range = ColorRange::Limited;
  ChromaSiting siting = ChromaSiting::Left;
  /// Colour primaries and transfer characteristics as ITU-T H.273 (CICP)
  /// code points, carried through for encoders and containers that signal
  /// them (1 = BT.709, 9 = BT.2020, 16 = PQ, 18 = HLG ...); 2 = unspecified.
  /// The pixel conversions in codec/colorspace.hpp do not use them.
  std::uint8_t primaries = 2;
  std::uint8_t transfer = 2;
  friend bool operator==(const ColorInfo&, const ColorInfo&) = default;
};

/// Number of planes of a pixel format (1, 2 or 3).
[[nodiscard]] constexpr std::size_t plane_count(PixelFormat format) noexcept {
  switch (format) {
    case PixelFormat::RGBA8: return 1;
    case PixelFormat::NV12:
    case PixelFormat::P010: return 2;
    default: return 3;
  }
}
/// Bytes per stored sample: 1 for 8-bit formats, 2 for the 10-bit ones.
[[nodiscard]] constexpr std::size_t bytes_per_sample(PixelFormat format) noexcept {
  switch (format) {
    case PixelFormat::I420P10:
    case PixelFormat::I422P10:
    case PixelFormat::I444P10:
    case PixelFormat::P010: return 2;
    default: return 1;
  }
}
/// Significant bits per sample: 8 or 10.
[[nodiscard]] constexpr int bit_depth(PixelFormat format) noexcept { return bytes_per_sample(format) == 2 ? 10 : 8; }
[[nodiscard]] constexpr bool is_yuv(PixelFormat format) noexcept { return format != PixelFormat::RGBA8; }
/// log2 of the horizontal / vertical chroma subsampling factor (0 or 1).
[[nodiscard]] constexpr int chroma_shift_x(PixelFormat format) noexcept {
  switch (format) {
    case PixelFormat::RGBA8:
    case PixelFormat::I444:
    case PixelFormat::I444P10: return 0;
    default: return 1;
  }
}
[[nodiscard]] constexpr int chroma_shift_y(PixelFormat format) noexcept {
  switch (format) {
    case PixelFormat::I420:
    case PixelFormat::NV12:
    case PixelFormat::I420P10:
    case PixelFormat::P010: return 1;
    default: return 0;
  }
}
/// Width and height, in samples, of plane `plane` for a `width` x `height` frame.
[[nodiscard]] constexpr std::uint32_t plane_width(PixelFormat format, std::size_t plane, std::uint32_t width) noexcept {
  return plane == 0 ? width : (width + ((1u << chroma_shift_x(format)) - 1)) >> chroma_shift_x(format);
}
[[nodiscard]] constexpr std::uint32_t plane_height(PixelFormat format, std::size_t plane, std::uint32_t height) noexcept {
  return plane == 0 ? height : (height + ((1u << chroma_shift_y(format)) - 1)) >> chroma_shift_y(format);
}
/// Bytes one row of plane `plane` needs (the minimum stride).
[[nodiscard]] constexpr std::size_t plane_row_bytes(PixelFormat format, std::size_t plane, std::uint32_t width) noexcept {
  if (format == PixelFormat::RGBA8) return static_cast<std::size_t>(width) * 4;
  const std::size_t samples = plane_width(format, plane, width);
  const bool interleaved = plane == 1 && (format == PixelFormat::NV12 || format == PixelFormat::P010);
  return samples * (interleaved ? 2 : 1) * bytes_per_sample(format);
}

/// One plane of a picture.
struct Plane {
  /// At least `stride * rows` bytes, rows top to bottom.
  std::vector<std::uint8_t> data;
  /// Bytes from the start of one row to the start of the next; at least
  /// `plane_row_bytes()`, possibly larger (alignment padding).
  std::size_t stride = 0;
};

/// One decoded picture.
///
/// Decoders deliver frames in their native pixel format (usually I420) to
/// avoid needless conversions; `codec::to_rgba()` / `codec::from_rgba()` in
/// codec/colorspace.hpp convert when a consumer needs another layout.
struct VideoFrame {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  PixelFormat format = PixelFormat::RGBA8;
  /// Interpretation of YCbCr samples; ignored for RGBA8.
  ColorInfo color;
  /// `plane_count(format)` planes. Each needs `stride * (rows - 1)` bytes
  /// plus one final row of `plane_row_bytes()`; padding after the last row is
  /// optional and never read. For 10-bit formats, bits outside the 10
  /// significant ones are zero. For RGBA8, `planes[0]` holds R, G, B, A
  /// bytes (sRGB, straight alpha), the same layout as ir::Frame::rgba when
  /// the stride equals `width * 4`.
  std::vector<Plane> planes;
  /// Presentation time and display duration in milliseconds.
  std::int64_t pts_ms = 0;
  std::int64_t duration_ms = 0;
  /// Sample (pixel) aspect ratio from the bitstream, e.g. 16/11 for
  /// anamorphic PAL; 0/1 when unknown, which means square pixels.
  Rational sample_aspect{0, 1};

  /// A zero-filled frame with tightly packed planes (stride = row bytes).
  [[nodiscard]] static VideoFrame allocate(PixelFormat format, std::uint32_t width, std::uint32_t height) {
    VideoFrame frame;
    frame.width = width;
    frame.height = height;
    frame.format = format;
    frame.planes.resize(plane_count(format));
    for (std::size_t p = 0; p < frame.planes.size(); ++p) {
      frame.planes[p].stride = plane_row_bytes(format, p, width);
      frame.planes[p].data.resize(frame.planes[p].stride * plane_height(format, p, height));
    }
    return frame;
  }
  /// True when the dimensions are non-zero, the plane count matches the
  /// format and every plane is large enough for its stride and row count.
  [[nodiscard]] bool valid() const noexcept {
    if (width == 0 || height == 0 || planes.size() != plane_count(format)) return false;
    if (width > 0x40000000u || height > 0x40000000u) return false;  // keeps the plane arithmetic below exact
    for (std::size_t p = 0; p < planes.size(); ++p) {
      const std::size_t rows = plane_height(format, p, height);
      const std::size_t row_bytes = plane_row_bytes(format, p, width);
      const std::size_t stride = planes[p].stride;
      const std::size_t size = planes[p].data.size();
      // Overflow-safe form of: size >= stride * (rows - 1) + row_bytes.
      if (stride < row_bytes || size < row_bytes) return false;
      if (rows > 1 && (size - row_bytes) / (rows - 1) < stride) return false;
    }
    return true;
  }
};

/// A run of decoded audio.
struct AudioChunk {
  /// Interleaved float samples, nominal range -1..+1, `VideoInfo::audio_channels`
  /// per sample frame at `VideoInfo::audio_sample_rate`.
  std::vector<float> samples;
  /// Presentation time of the first sample frame in milliseconds.
  std::int64_t pts_ms = 0;
};

enum class TrackType { Video, Audio, Subtitle };

/// One compressed access unit of a track.
struct Packet {
  /// The coded data exactly as stored in the container (one video frame, one
  /// or more audio frames, one subtitle cue).
  Bytes bytes;
  /// Presentation and decode timestamps and duration, in units of
  /// `1 / Track::timescale` seconds.
  std::int64_t pts = 0;
  std::int64_t dts = 0;
  std::int64_t duration = 0;
  /// True when decoding can start at this packet.
  bool keyframe = false;
};

/// A compressed elementary stream offered for pass-through, so that a muxer
/// can re-wrap it into another container without decoding and re-encoding.
struct Track {
  TrackType type = TrackType::Video;
  /// Lower-case codec identifier: "h264", "hevc", "av1", "vp8", "vp9",
  /// "mjpeg", "aac", "mp3", "opus", "vorbis", "flac", "pcm_s16le", "srt", ...
  std::string codec;
  /// Codec configuration in the codec's native out-of-band form (avcC/hvcC
  /// box payload, AudioSpecificConfig, OpusHead, ...); may be empty.
  Bytes extradata;
  /// Ticks per second for `Packet` timestamps (e.g. 90000, or the sample rate).
  std::uint32_t timescale = 1000;
  /// Returns the next packet in decode order, `nullopt` at the end of the
  /// track, or an error. Null when the track cannot be read.
  std::function<Result<std::optional<Packet>>()> next_packet;
};

/// A video, delivered as a stream.
///
/// Whole decoded videos never fit in memory, so this IR does not hold the
/// content: it holds functions that produce it piece by piece, on demand.
/// The writer pulls; the reader's functions decode just enough to answer.
///
/// Rules
///   - `next_frame` yields frames in presentation order and `nullopt` after
///     the last one; `next_audio` likewise for audio chunks. Either may be
///     null when that part is absent (no audio) or cannot be decoded.
///     After `nullopt` or an error a function must not be called again.
///   - `tracks` optionally offers the same content in compressed form.
///     A consumer uses EITHER the decoded functions OR the packets of a
///     track, never both for the same media type.
///   - A Video is single-pass and has exactly one consumer, hence MOVE-ONLY.
///   - The functions may refer to the conversion's input bytes
///     (`ReadContext::input`), which the engine keeps alive until the writer
///     returns. They must not refer to the ReadContext itself; copy what is
///     needed (`ctx.progress`, `ctx.limits` and `ctx.warnings` are cheap,
///     shareable handles). Shared decoder state belongs in a
///     `std::shared_ptr` captured by the functions.
///   - Functions are called from the thread running the conversion.
struct Video {
  VideoInfo info;
  std::function<Result<std::optional<VideoFrame>>()> next_frame;
  std::function<Result<std::optional<AudioChunk>>()> next_audio;
  std::vector<Track> tracks;

  Video() = default;
  Video(Video&&) noexcept = default;
  Video& operator=(Video&&) noexcept = default;
  Video(const Video&) = delete;
  Video& operator=(const Video&) = delete;
};

}  // namespace cvt::ir
