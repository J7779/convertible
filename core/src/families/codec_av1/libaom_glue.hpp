// Convertible core - codec_av1 family: the only code that talks to the
// imported libaom. Everything else in the family uses these two classes,
// which expose no libaom types.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>

#include "codec/codec.hpp"
#include "engine/errors.hpp"
#include "families/codec_av1/av1_api.hpp"
#include "util/bytes.hpp"

namespace cvt::av1::detail {

/// A libaom decoder instance fed with Section 5 temporal units.
class DecoderCore {
 public:
  DecoderCore();
  ~DecoderCore();
  DecoderCore(const DecoderCore&) = delete;
  DecoderCore& operator=(const DecoderCore&) = delete;

  /// Creates the libaom decoder.
  [[nodiscard]] Result<void> open(bool apply_film_grain);
  /// Decodes one temporal unit (Section 5 OBUs). Shown pictures become
  /// available through `next_picture`.
  [[nodiscard]] Result<void> decode(ByteView temporal_unit);
  /// Releases any frame still held by the decoder.
  [[nodiscard]] Result<void> flush();
  /// The next shown picture, converted to a VideoFrame (timestamps zero).
  [[nodiscard]] std::optional<Picture> next_picture();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// How libaom should be driven.
enum class Usage { GoodQuality, Realtime, AllIntra };

struct EncoderSettings {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  /// I420, I422, I444 or their P10 forms.
  ir::PixelFormat format = ir::PixelFormat::I420;
  bool monochrome = false;
  Usage usage = Usage::GoodQuality;
  int cpu_used = 6;
  /// 0..63; ignored when lossless or with a bitrate.
  int cq_level = 24;
  bool lossless = false;
  std::optional<std::uint32_t> bitrate_kbps;
  /// 0 = libaom default.
  std::uint32_t keyframe_interval = 0;
  /// Frames of look-ahead (0 = none).
  int lag_in_frames = 19;
  /// A single still picture (reduced still-picture header).
  bool still_picture = false;
  PictureColor color;
};

/// One compressed temporal unit (Section 5, including its temporal delimiter).
struct EncodedUnit {
  Bytes data;
  std::int64_t pts_ms = 0;
  std::int64_t duration_ms = 0;
  bool keyframe = false;
};

/// A libaom encoder instance.
class EncoderCore {
 public:
  EncoderCore();
  ~EncoderCore();
  EncoderCore(const EncoderCore&) = delete;
  EncoderCore& operator=(const EncoderCore&) = delete;

  [[nodiscard]] Result<void> open(const EncoderSettings& settings);
  /// The sequence header OBU libaom will use (Section 5 form).
  [[nodiscard]] Bytes sequence_header() const;
  /// Encodes one picture already in `settings.format` with the configured
  /// size; pts/duration in milliseconds, pts strictly increasing.
  [[nodiscard]] Result<void> encode(const ir::VideoFrame& frame, std::int64_t pts_ms, std::int64_t duration_ms);
  /// Signals the end of input; call until it returns false, collecting units.
  [[nodiscard]] Result<bool> flush_step();
  [[nodiscard]] std::optional<EncodedUnit> next_unit();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Converts the CICP matrix code to the ColorInfo matrix used for YCbCr <->
/// RGB conversion (unspecified: BT.709 for HD sizes, BT.601 otherwise).
[[nodiscard]] ir::ColorInfo color_info_from_cicp(const PictureColor& color, std::uint32_t width, std::uint32_t height);

}  // namespace cvt::av1::detail
