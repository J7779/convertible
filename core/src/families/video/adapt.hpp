// Convertible core - video family: packaging conversions between containers.
//
// A StreamAdapter re-packages the packets of one elementary stream for a
// destination (a muxer, or a decoder, which always wants the MP4 form; see
// bitstream.hpp). It learns missing configuration (parameter sets, AAC
// configuration, AV1 sequence header ...) from the packets themselves, so a
// stream from MPEG-TS can be written to MP4 without re-encoding and the other
// way round.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <string>

#include "engine/errors.hpp"
#include "families/video/bitstream.hpp"
#include "util/bytes.hpp"

namespace cvt::video {

/// How a destination wants a stream packaged.
struct Packaging {
  /// H.264/HEVC: true = length-prefixed NAL units + avcC/hvcC; false = Annex-B
  /// with parameter sets repeated before every key frame.
  bool length_prefixed = true;
  /// AAC: true = ADTS headers on every frame; false = raw + AudioSpecificConfig.
  bool adts = false;
  /// AV1: true = temporal units start with a temporal delimiter (IVF, .obu).
  bool av1_temporal_delimiters = false;
  /// H.264 in MPEG-TS: an access unit delimiter starts every access unit.
  bool access_unit_delimiters = false;
};

/// The MP4 packaging, which decoders receive.
inline constexpr Packaging decoder_packaging{};

class StreamAdapter {
 public:
  StreamAdapter() = default;
  StreamAdapter(std::string codec, Bytes extradata, Packaging target);

  /// Look at a packet without converting it, to learn configuration that is
  /// only carried in-band. Cheap; call it on the first packets before asking
  /// for `extradata()` when the configuration must be known up front.
  void learn(ByteView packet);
  /// Re-package one packet.
  [[nodiscard]] Result<Bytes> convert(ByteView packet, bool keyframe);
  /// The configuration record in the destination's form (may be empty).
  [[nodiscard]] Bytes extradata() const;
  /// True when `extradata()` is complete for codecs that need one.
  [[nodiscard]] bool configured() const;
  [[nodiscard]] const std::string& codec() const noexcept { return codec_; }
  /// AAC configuration learnt from ASC/ADTS (sample rate, channels), if any.
  [[nodiscard]] const bits::AacConfig* aac() const noexcept { return has_aac_ ? &aac_ : nullptr; }

 private:
  [[nodiscard]] bool is_nal_codec() const noexcept { return codec_ == "h264" || codec_ == "hevc"; }
  std::string codec_;
  Bytes original_extradata_;
  Packaging target_;
  // H.264 / HEVC
  bool source_length_prefixed_ = false;
  bits::ParameterSets sets_;
  // AAC
  bool has_aac_ = false;
  bits::AacConfig aac_;
  // learned configuration for other codecs (AV1 av1C, MPEG-4 VOL ...)
  Bytes learned_;
};

}  // namespace cvt::video
