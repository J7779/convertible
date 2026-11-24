// Convertible core - video family: the muxer interface.
//
// A muxer receives track descriptions, then packets of all tracks in
// non-decreasing decode time (the pipeline interleaves them), and assembles
// the file in memory when finished, so header fields that depend on the
// whole stream (durations, indexes, cue points, fast-start sample tables,
// configuration records that encoders only produce late) are always exact.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "ir/video.hpp"
#include "util/bytes.hpp"

namespace cvt::video {

struct MuxTrack {
  ir::TrackType type = ir::TrackType::Video;
  std::string codec;
  /// Configuration record in any form the StreamAdapter understands.
  Bytes extradata;
  /// Ticks per second of the packet timestamps.
  std::uint32_t timescale = 1000;
  // video
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  ir::Rational fps;
  int rotation = 0;
  // audio
  std::uint32_t sample_rate = 0;
  std::uint16_t channels = 0;
  std::uint16_t bits_per_sample = 0;
  std::uint32_t block_align = 0;
  std::string language;
};

struct MuxSettings {
  std::string format_id;
  /// Container tags to write (already filtered by strip_metadata).
  std::map<std::string, std::string> metadata;
  Limits limits;
  Warnings warnings;
};

class Muxer {
 public:
  virtual ~Muxer() = default;
  /// Fails with Status::Encode when a track's codec cannot be stored.
  [[nodiscard]] virtual Result<void> begin(std::vector<MuxTrack> tracks, const MuxSettings& settings) = 0;
  /// Timestamps in the track's timescale; packets of one track in decode order.
  [[nodiscard]] virtual Result<void> write(std::size_t track, const ir::Packet& packet) = 0;
  /// Replace a track's configuration record (encoders that know it only late).
  virtual void set_extradata(std::size_t track, Bytes extradata) = 0;
  [[nodiscard]] virtual Result<Bytes> finish() = 0;
};

/// The muxer for a container format id, or nullptr.
[[nodiscard]] std::unique_ptr<Muxer> make_muxer(std::string_view format_id);

/// True when the muxer of `format_id` can store `codec` (of `type`) unchanged.
[[nodiscard]] bool container_accepts(std::string_view format_id, ir::TrackType type, std::string_view codec);

}  // namespace cvt::video
