// Convertible core - video family: the demuxed-file model shared by every
// container reader and the helpers that turn it into an ir::Video.
//
// Every demuxer parses the container once, up front, into an index: per track
// a list of PacketRef (timestamps, key flag) whose payload is a list of Spans.
// A span is a byte range of the input (memory-mapped by the engine, alive
// until the writer returns) or, for the rare payloads that do not exist
// contiguously in the file (descrambled or reassembled data), a range of the
// demuxer's own `owned` buffer. Packet payloads are therefore never copied
// until a consumer pulls them, and the index costs a few dozen bytes per
// packet however large the file is.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "ir/video.hpp"
#include "util/bytes.hpp"

namespace cvt::video {

/// Codec ids used by containers of this family that codec/ids.hpp lacks.
/// They label tracks so that messages are precise and so that a codec family
/// can register a decoder under the same spelling later (proposed for
/// codec/ids.hpp in the family report).
namespace cid {
inline constexpr std::string_view wmv1 = "wmv1";
inline constexpr std::string_view wmv2 = "wmv2";
inline constexpr std::string_view wmv3 = "wmv3";
inline constexpr std::string_view vc1 = "vc1";
inline constexpr std::string_view msmpeg4v3 = "msmpeg4v3";
inline constexpr std::string_view flv1 = "flv1";  ///< Sorenson Spark (FLV H.263 flavour)
inline constexpr std::string_view vp6f = "vp6f";
inline constexpr std::string_view rv10 = "rv10";
inline constexpr std::string_view rv20 = "rv20";
inline constexpr std::string_view rv30 = "rv30";
inline constexpr std::string_view rv40 = "rv40";
inline constexpr std::string_view wmav1 = "wmav1";
inline constexpr std::string_view wmav2 = "wmav2";
inline constexpr std::string_view wmapro = "wmapro";
inline constexpr std::string_view eac3 = "eac3";
inline constexpr std::string_view cook = "cook";
inline constexpr std::string_view ra_144 = "ra_144";
inline constexpr std::string_view subrip = "subrip";
inline constexpr std::string_view webvtt = "webvtt";
inline constexpr std::string_view mov_text = "mov_text";
inline constexpr std::string_view ass = "ass";
}  // namespace cid

/// A byte range of the input, or of `Demuxed::owned` when `owned` is set.
struct Span {
  std::uint64_t offset = 0;
  std::uint32_t size = 0;
  bool owned = false;
};

/// One compressed packet in the index.
struct PacketRef {
  std::int64_t pts = 0;
  std::int64_t dts = 0;
  std::int64_t duration = 0;
  std::uint32_t first_span = 0;
  std::uint32_t span_count = 0;
  std::uint32_t size = 0;
  bool keyframe = false;
};

/// Everything a container says about one elementary stream.
struct TrackInfo {
  ir::TrackType type = ir::TrackType::Video;
  /// Canonical codec id; empty when the container's tag is not recognised.
  std::string codec;
  /// The container's own codec label (FourCC, Matroska codec id ...), for messages.
  std::string tag;
  Bytes extradata;
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
  /// Decoder delay in sample frames that a player drops (edit list, Opus pre-skip, CodecDelay).
  std::int64_t priming = 0;
  // general
  std::string language;
  std::string title;
  bool is_default = true;
  std::uint32_t id = 0;
  /// Track duration in timescale units; 0 when unknown.
  std::int64_t duration = 0;
  std::vector<PacketRef> packets;
};

class Demuxed;

/// Collects the spans of one packet while a demuxer assembles it.
class PacketBuilder {
 public:
  explicit PacketBuilder(Demuxed& owner);
  /// Append `size` bytes of the input at `offset` (adjacent ranges merge).
  void input(std::uint64_t offset, std::uint64_t size);
  /// Append bytes that are not in the input as they are.
  void owned(ByteView data);
  [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  /// Finish: a PacketRef pointing at the collected spans.
  [[nodiscard]] PacketRef finish(std::int64_t pts, std::int64_t dts, std::int64_t duration, bool keyframe);

 private:
  Demuxed& owner_;
  std::vector<Span> spans_;
  std::uint64_t size_ = 0;
};

/// A fully indexed container file.
class Demuxed {
 public:
  ByteView input;
  Bytes owned;
  std::vector<Span> spans;
  std::vector<TrackInfo> tracks;
  std::map<std::string, std::string> metadata;
  std::int64_t duration_ms = 0;
  std::string format_id;

  /// Index a packet made of one contiguous input range; returns false (and
  /// adds nothing) when the range is not entirely inside the input.
  bool add_input_packet(TrackInfo& track, std::uint64_t offset, std::uint64_t size, std::int64_t pts,
                        std::int64_t dts, std::int64_t duration, bool keyframe);
  /// The payload of a packet, copied out.
  [[nodiscard]] Bytes packet_bytes(const PacketRef& packet) const;
  /// The payload of a packet when it is one contiguous range (no copy); empty otherwise.
  [[nodiscard]] ByteView packet_view(const PacketRef& packet) const;
  /// Total number of packets over all tracks.
  [[nodiscard]] std::uint64_t packet_count() const noexcept;

  friend class PacketBuilder;
};

/// Reader-side knobs shared by every demuxer.
struct ReadSettings {
  Limits limits;
  Progress progress;
  Warnings warnings;
};

/// Timestamps read from files are untrusted: arithmetic on them saturates
/// instead of overflowing, and the index keeps them within +-2^53.
inline constexpr std::int64_t max_timestamp = std::int64_t{1} << 53;
[[nodiscard]] constexpr std::int64_t clamp_timestamp(std::int64_t v) noexcept {
  return v > max_timestamp ? max_timestamp : v < -max_timestamp ? -max_timestamp : v;
}
[[nodiscard]] constexpr std::int64_t sat_add(std::int64_t a, std::int64_t b) noexcept {
  if (b > 0 && a > INT64_MAX - b) return INT64_MAX;
  if (b < 0 && a < INT64_MIN - b) return INT64_MIN;
  return a + b;
}
[[nodiscard]] constexpr std::int64_t sat_mul(std::int64_t a, std::int64_t b) noexcept {
  if (a == 0 || b == 0) return 0;
  const bool negative = (a < 0) != (b < 0);
  const std::uint64_t ua = a < 0 ? 0 - static_cast<std::uint64_t>(a) : static_cast<std::uint64_t>(a);
  const std::uint64_t ub = b < 0 ? 0 - static_cast<std::uint64_t>(b) : static_cast<std::uint64_t>(b);
  if (ua > static_cast<std::uint64_t>(INT64_MAX) / ub) return negative ? INT64_MIN : INT64_MAX;
  const auto product = static_cast<std::int64_t>(ua * ub);
  return negative ? -product : product;
}

/// Convert a timestamp between timescales with rounding to nearest
/// (exact 128-bit-free arithmetic for the ranges that occur in practice).
[[nodiscard]] std::int64_t rescale(std::int64_t value, std::int64_t from, std::int64_t to) noexcept;

/// Build the streaming IR from an index: offers every track for remuxing and
/// decodes the first (default) video and audio tracks on demand through the
/// codec registry. `audio_track` picks among the audio tracks (0 = default).
[[nodiscard]] Result<ir::Video> make_video(std::shared_ptr<Demuxed> demuxed, const Context& ctx);

/// Signature shared by the container parsers.
using DemuxFunction = Result<std::shared_ptr<Demuxed>> (*)(ByteView input, const ReadSettings& settings);

/// Sort helper: tracks with no packets are dropped and a container without any
/// usable track becomes a Decode error naming the format.
[[nodiscard]] Result<void> finish_demux(Demuxed& demuxed, std::string_view format_name);

/// Human label of a track for messages: "video track 1 (h264)".
[[nodiscard]] std::string track_label(const TrackInfo& track, std::size_t index);

}  // namespace cvt::video
