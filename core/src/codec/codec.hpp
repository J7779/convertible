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
//
// Packaging convention (what decoders receive in CodecParams::extradata and
// packets, what encoders emit, and what ir::Track carries between readers and
// writers; containers convert to and from their own storage form):
//
//   h264 / hevc  packets: NAL units with 4-byte big-endian length prefixes
//                (decoders also accept 1/2-byte prefixes as stated by the
//                record, and Annex-B start codes when extradata is empty);
//                extradata: the avcC / hvcC payload (AVCDecoderConfiguration-
//                Record / HEVCDecoderConfigurationRecord, without box header).
//   vp8 / vp9    packets: one frame (VP9: one superframe); extradata: the
//                body of the 'vpcC' FullBox (version 1 + 24-bit flags, then the
//                VPCodecConfigurationRecord: profile, level = 10 * X + Y,
//                bit depth/chroma/range, CICP colour, 16-bit init size 0) or
//                empty. MP4 stores it as the 'vpcC' box; Matroska/WebM must NOT
//                copy it to CodecPrivate (WebM leaves CodecPrivate empty).
//   av1          packets: one temporal unit of low-overhead (Section 5) OBUs
//                with obu_has_size_field set and no temporal delimiter;
//                extradata: the av1C payload (4 bytes + configOBUs).
//   aac          packets: raw access units (no ADTS header); extradata: the
//                AudioSpecificConfig.
//   opus         extradata: OpusHead; vorbis/theora: the three Xiph-laced
//                headers; flac: STREAMINFO (34 bytes, also "fLaC"+blocks).
//   mjpeg / png  one complete picture file per packet, no extradata.
//   rawvideo     extradata: the 4-byte layout tag (see video/builtin_codecs).
//   legacy codecs see families/codec_legacy/codec_notes.hpp (palettes as
//                B,G,R,0 entries, CodecParams::bits_per_sample = coded depth,
//                Packet::pts == INT64_MIN = no timestamp).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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

  /// Largest picture (width x height) the decoder may allocate; 0 = the
  /// decoder's own ceiling. Demuxers pass `Limits::max_pixels` so that a
  /// hostile bitstream cannot make a decoder allocate more than the caller allows.
  std::uint64_t max_pixels = 0;
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
  /// Non-fatal notes collected since the last call (concealed errors,
  /// unsupported features skipped); the caller forwards them to the user.
  [[nodiscard]] virtual std::vector<std::string> take_warnings() { return {}; }
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
  /// Mathematically lossless coding where the codec supports it (FFV1,
  /// HuffYUV, VP9/AV1/HEVC lossless modes); `quality` and `bitrate_kbps` are
  /// then ignored. Encoders without a lossless mode fail configure() with
  /// Status::InvalidArgument.
  bool lossless = false;
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
  /// Sample depth for PCM-like and lossless codecs (16, 24, 32 ...); 0 = the
  /// encoder's default (normally 16). Lossy codecs ignore it.
  std::uint16_t bits_per_sample = 0;
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
