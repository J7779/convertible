// Convertible core - abstract decoder and encoder interfaces.
//
// A container reader (demuxer) produces compressed ir::Packets and looks up
// a decoder for the track's codec id; a container writer (muxer) looks up an
// encoder and wraps the packets it emits. Neither side knows who implemented
// the other: they meet only through these classes and codec::Registry.
//
// Calling pattern (the same for all four interfaces):
//
//   decoder->configure(params);
//   for each packet:
//     decoder->send_packet(packet);
//     while (auto frame = decoder->receive_frame(); frame && *frame) use(**frame);
//   decoder->flush();                       // no more input
//   while (auto frame = decoder->receive_frame(); frame && *frame) use(**frame);
//
// `receive_*` returning an empty optional means "feed me more input" before
// `flush()`, and "completely drained" after it. Every call can fail; after an
// error the object must not be used further. Objects are single-threaded and
// hold no global state, so any number may exist at once.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "engine/errors.hpp"
#include "ir/video.hpp"
#include "util/bytes.hpp"

namespace cvt::codec {

using ir::AudioChunk;
using ir::ColorInfo;
using ir::Packet;
using ir::PixelFormat;
using ir::Rational;
using ir::VideoFrame;

/// What a demuxer knows about a compressed stream; given to decoders.
struct CodecParams {
  /// Canonical codec id (see codec/ids.hpp).
  std::string codec;
  /// Out-of-band configuration in the codec's native form (avcC/hvcC payload,
  /// AudioSpecificConfig, OpusHead, Vorbis headers, ...); may be empty.
  Bytes extradata;
  /// Ticks per second of the packet timestamps the decoder will be sent.
  std::uint32_t timescale = 1000;

  // -- video; zero when unknown (the decoder then relies on the bitstream) --
  std::uint32_t width = 0;
  std::uint32_t height = 0;

  // -- audio; zero when unknown ------------------------------------------------
  std::uint32_t sample_rate = 0;
  std::uint16_t channels = 0;
  /// Bits per coded sample, for PCM and ADPCM flavours.
  std::uint16_t bits_per_sample = 0;
  /// Bytes per coded block, for block-based codecs (ADPCM, some PCM).
  std::uint32_t block_align = 0;
};

/// Turns compressed video packets into pictures.
class VideoDecoder {
 public:
  virtual ~VideoDecoder() = default;
  /// Prepare for a stream. Called exactly once, before anything else.
  [[nodiscard]] virtual Result<void> configure(const CodecParams& params) = 0;
  /// Feed one packet (decode order). Its `pts` is carried to the frame.
  [[nodiscard]] virtual Result<void> send_packet(const Packet& packet) = 0;
  /// The next picture in presentation order, in the decoder's native pixel
  /// format, with `pts_ms`/`duration_ms` derived from the packet timestamps;
  /// empty when more input is needed (or, after `flush`, when done).
  [[nodiscard]] virtual Result<std::optional<VideoFrame>> receive_frame() = 0;
  /// Signal the end of the stream so that buffered pictures are released.
  [[nodiscard]] virtual Result<void> flush() = 0;
};

/// Speed/size trade-off for encoders.
enum class EncodePreset {
  Fast,      ///< favour encoding speed
  Balanced,  ///< sensible default
  Small,     ///< favour compression efficiency, slower
};

/// What a muxer asks of a video encoder.
struct VideoEncodeParams {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  /// Nominal frame rate; 0/1 = variable (frame timestamps are authoritative).
  Rational fps;
  /// 1..100, higher is better and larger; 90 is "visually transparent".
  /// Ignored when `bitrate_kbps` is set.
  int quality = 90;
  /// Target average bitrate; nullopt = quality-driven (constant quality).
  std::optional<std::uint32_t> bitrate_kbps;
  /// Maximum distance between keyframes in frames; 0 = encoder default.
  std::uint32_t keyframe_interval = 0;
  EncodePreset preset = EncodePreset::Balanced;
  /// Layout of the frames that will be sent. Encoders must accept I420 and
  /// may accept more; callers convert with codec/colorspace.hpp otherwise.
  PixelFormat pixel_format = PixelFormat::I420;
  ColorInfo color;
  /// Ticks per second for the timestamps of the packets produced.
  std::uint32_t timescale = 90000;
};

/// Turns pictures into compressed video packets.
class VideoEncoder {
 public:
  virtual ~VideoEncoder() = default;
  /// Prepare the encoder. Called exactly once, before anything else.
  /// Fails with Status::InvalidArgument for unsupported parameters.
  [[nodiscard]] virtual Result<void> configure(const VideoEncodeParams& params) = 0;
  /// Feed one picture (presentation order) in the configured pixel format.
  [[nodiscard]] virtual Result<void> send_frame(const VideoFrame& frame) = 0;
  /// The next packet in decode order; empty when more input is needed (or,
  /// after `flush`, when done).
  [[nodiscard]] virtual Result<std::optional<Packet>> receive_packet() = 0;
  /// Out-of-band configuration for the container (same form as
  /// CodecParams::extradata). Valid after `configure`; some encoders only
  /// know it after the first packet, so muxers should read it late.
  [[nodiscard]] virtual Bytes extradata() const = 0;
  /// Signal the end of the stream so that delayed packets are released.
  [[nodiscard]] virtual Result<void> flush() = 0;
};

/// Turns compressed audio packets into float samples.
class AudioDecoder {
 public:
  virtual ~AudioDecoder() = default;
  [[nodiscard]] virtual Result<void> configure(const CodecParams& params) = 0;
  [[nodiscard]] virtual Result<void> send_packet(const Packet& packet) = 0;
  /// The next run of interleaved float32 samples (-1..+1); empty when more
  /// input is needed (or, after `flush`, when done). Decoders drop their own
  /// priming samples, so output starts at the first real sample.
  [[nodiscard]] virtual Result<std::optional<AudioChunk>> receive_chunk() = 0;
  [[nodiscard]] virtual Result<void> flush() = 0;
  /// Output format. Known after `configure` when the parameters or extradata
  /// state it, otherwise after the first decoded chunk; 0 until then.
  [[nodiscard]] virtual std::uint32_t sample_rate() const = 0;
  [[nodiscard]] virtual std::uint16_t channels() const = 0;
};

/// What a muxer asks of an audio encoder.
struct AudioEncodeParams {
  std::uint32_t sample_rate = 0;
  std::uint16_t channels = 0;
  /// 1..100, higher is better and larger; ignored when `bitrate_kbps` is set
  /// and by lossless codecs.
  int quality = 90;
  /// Target bitrate for all channels together; nullopt = quality-driven.
  std::optional<std::uint32_t> bitrate_kbps;
  /// 0..9 effort for lossless codecs (0 fastest, 9 smallest).
  int compression_level = 6;
  /// Ticks per second for the timestamps of the packets produced; 0 = use
  /// the sample rate.
  std::uint32_t timescale = 0;
};

/// Turns float samples into compressed audio packets.
class AudioEncoder {
 public:
  virtual ~AudioEncoder() = default;
  /// Fails with Status::InvalidArgument for unsupported rates or layouts.
  [[nodiscard]] virtual Result<void> configure(const AudioEncodeParams& params) = 0;
  /// Feed interleaved float32 samples; any chunk length is accepted (the
  /// encoder buffers to its own frame size).
  [[nodiscard]] virtual Result<void> send_chunk(const AudioChunk& chunk) = 0;
  [[nodiscard]] virtual Result<std::optional<Packet>> receive_packet() = 0;
  [[nodiscard]] virtual Bytes extradata() const = 0;
  /// Signal the end of the stream; the final partial frame is padded and emitted.
  [[nodiscard]] virtual Result<void> flush() = 0;
  /// Sample frames per packet; 0 when it varies.
  [[nodiscard]] virtual std::uint32_t frame_size() const = 0;
  /// Encoder delay ("priming"): sample frames at the start of the decoded
  /// stream that a player must discard. Muxers record it (edit list, Opus
  /// pre-skip, LAME header...).
  [[nodiscard]] virtual std::uint32_t delay() const = 0;
};

}  // namespace cvt::codec
