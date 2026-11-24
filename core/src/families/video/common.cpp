// Convertible core - video family: the demuxed-file model and the streaming IR built from it.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/video/common.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <utility>

#include "codec/registry.hpp"
#include "engine/registry.hpp"
#include "families/video/adapt.hpp"

namespace cvt::video {

// --------------------------------------------------------------------------- index

PacketBuilder::PacketBuilder(Demuxed& owner) : owner_(owner) {}

void PacketBuilder::input(std::uint64_t offset, std::uint64_t size) {
  while (size > 0) {
    const auto piece = static_cast<std::uint32_t>(std::min<std::uint64_t>(size, 0x7FFFFFFFu));
    if (!spans_.empty() && !spans_.back().owned && spans_.back().offset + spans_.back().size == offset &&
        std::uint64_t{spans_.back().size} + piece <= 0x7FFFFFFFu) {
      spans_.back().size += piece;
    } else {
      spans_.push_back(Span{offset, piece, false});
    }
    size_ += piece;
    offset += piece;
    size -= piece;
  }
}

void PacketBuilder::owned(ByteView data) {
  if (data.empty()) return;
  const std::uint64_t at = owner_.owned.size();
  owner_.owned.insert(owner_.owned.end(), data.begin(), data.end());
  spans_.push_back(Span{at, static_cast<std::uint32_t>(data.size()), true});
  size_ += data.size();
}

PacketRef PacketBuilder::finish(std::int64_t pts, std::int64_t dts, std::int64_t duration, bool keyframe) {
  PacketRef ref;
  ref.pts = clamp_timestamp(pts);
  ref.dts = clamp_timestamp(dts);
  ref.duration = clamp_timestamp(duration);
  ref.keyframe = keyframe;
  ref.first_span = static_cast<std::uint32_t>(owner_.spans.size());
  ref.span_count = static_cast<std::uint32_t>(spans_.size());
  ref.size = static_cast<std::uint32_t>(std::min<std::uint64_t>(size_, 0xFFFFFFFFu));
  owner_.spans.insert(owner_.spans.end(), spans_.begin(), spans_.end());
  spans_.clear();
  size_ = 0;
  return ref;
}

bool Demuxed::add_input_packet(TrackInfo& track, std::uint64_t offset, std::uint64_t size, std::int64_t pts,
                               std::int64_t dts, std::int64_t duration, bool keyframe) {
  if (offset > input.size() || size > input.size() - offset || size > 0x7FFFFFFFu) return false;
  PacketBuilder builder(*this);
  builder.input(offset, size);
  track.packets.push_back(builder.finish(pts, dts, duration, keyframe));
  return true;
}

Bytes Demuxed::packet_bytes(const PacketRef& packet) const {
  Bytes out;
  out.reserve(packet.size);
  for (std::uint32_t i = 0; i < packet.span_count; ++i) {
    const Span& span = spans[packet.first_span + i];
    const ByteView source = span.owned ? ByteView(owned) : input;
    if (span.offset > source.size() || span.size > source.size() - span.offset) continue;
    const ByteView piece = source.subspan(static_cast<std::size_t>(span.offset), span.size);
    out.insert(out.end(), piece.begin(), piece.end());
  }
  return out;
}

ByteView Demuxed::packet_view(const PacketRef& packet) const {
  if (packet.span_count != 1) return {};
  const Span& span = spans[packet.first_span];
  const ByteView source = span.owned ? ByteView(owned) : input;
  if (span.offset > source.size() || span.size > source.size() - span.offset) return {};
  return source.subspan(static_cast<std::size_t>(span.offset), span.size);
}

std::uint64_t Demuxed::packet_count() const noexcept {
  std::uint64_t n = 0;
  for (const TrackInfo& t : tracks) n += t.packets.size();
  return n;
}

std::int64_t rescale(std::int64_t value, std::int64_t from, std::int64_t to) noexcept {
  if (from <= 0 || to <= 0 || from == to) return value;
  // value * to / from, rounded to nearest, without overflow for |value| < 2^62.
  const std::int64_t q = value / from;
  const std::int64_t r = value % from;
  const long double frac = static_cast<long double>(r) * static_cast<long double>(to) / static_cast<long double>(from);
  const long double whole = static_cast<long double>(q) * static_cast<long double>(to);
  const long double result = whole + frac;
  if (result >= 9.2e18L) return std::numeric_limits<std::int64_t>::max();
  if (result <= -9.2e18L) return std::numeric_limits<std::int64_t>::min();
  const long double rounded = result >= 0 ? result + 0.5L : result - 0.5L;
  return static_cast<std::int64_t>(rounded);
}

std::string track_label(const TrackInfo& track, std::size_t index) {
  const char* kind = track.type == ir::TrackType::Video ? "video" : track.type == ir::TrackType::Audio ? "audio" : "subtitle";
  return std::format("{} track {} ({})", kind, index + 1, track.codec.empty() ? track.tag : track.codec);
}

Result<void> finish_demux(Demuxed& demuxed, std::string_view format_name) {
  std::erase_if(demuxed.tracks, [](const TrackInfo& t) { return t.packets.empty(); });
  if (demuxed.tracks.empty()) return fail(Status::Decode, "the {} file contains no readable audio or video", format_name);
  for (TrackInfo& t : demuxed.tracks) {
    // Fill missing durations from the next packet's decode time (or the previous duration).
    for (std::size_t i = 0; i < t.packets.size(); ++i) {
      if (t.packets[i].duration > 0) continue;
      if (i + 1 < t.packets.size() && t.packets[i + 1].dts > t.packets[i].dts) {
        t.packets[i].duration = t.packets[i + 1].dts - t.packets[i].dts;
      } else if (i > 0) {
        t.packets[i].duration = t.packets[i - 1].duration;
      }
    }
    if (t.duration == 0 && !t.packets.empty()) {
      std::int64_t lo = std::numeric_limits<std::int64_t>::max();
      std::int64_t hi = std::numeric_limits<std::int64_t>::min();
      for (const PacketRef& p : t.packets) {
        lo = std::min(lo, p.pts);
        hi = std::max(hi, p.pts + p.duration);
      }
      t.duration = std::max<std::int64_t>(0, hi - std::max<std::int64_t>(lo, 0));
    }
    if (t.type == ir::TrackType::Video && t.fps.num == 0 && t.packets.size() > 1) {
      // Constant frame rate when every frame lasts the same.
      const std::int64_t d = t.packets.front().duration;
      const bool constant = d > 0 && std::ranges::all_of(t.packets, [d](const PacketRef& p) { return p.duration == d; });
      if (constant) t.fps = ir::Rational{t.timescale, d};
    }
    if (t.type == ir::TrackType::Video && t.fps.num != 0 && t.fps.den != 0) {
      // Reduce the fraction.
      std::int64_t a = t.fps.num;
      std::int64_t b = t.fps.den;
      while (b != 0) {
        const std::int64_t c = a % b;
        a = b;
        b = c;
      }
      if (a > 1) {
        t.fps.num /= a;
        t.fps.den /= a;
      }
    }
  }
  if (demuxed.duration_ms == 0) {
    for (const TrackInfo& t : demuxed.tracks) {
      demuxed.duration_ms = std::max(demuxed.duration_ms, rescale(t.duration, t.timescale, 1000));
    }
  }
  return {};
}

// --------------------------------------------------------------------------- decoding on demand

namespace {

struct TrackCursor {
  std::shared_ptr<Demuxed> demuxed;
  std::size_t track = 0;
  std::size_t next = 0;
  std::optional<StreamAdapter> adapter;  // re-packages to the canonical form
};

codec::CodecParams params_for(const TrackInfo& t, const StreamAdapter& adapter) {
  codec::CodecParams params;
  params.codec = t.codec;
  params.extradata = adapter.extradata();
  if (params.extradata.empty()) params.extradata = t.extradata;
  params.timescale = t.timescale;
  params.width = t.width;
  params.height = t.height;
  params.sample_rate = t.sample_rate;
  params.channels = t.channels;
  params.bits_per_sample = t.bits_per_sample;
  params.block_align = t.block_align;
  if (const bits::AacConfig* aac = adapter.aac()) {
    if (params.sample_rate == 0) params.sample_rate = aac->sample_rate;
    if (params.channels == 0) params.channels = static_cast<std::uint16_t>(aac->channel_config);
  }
  return params;
}

std::string missing_decoder(const TrackInfo& t, std::size_t index, std::string_view format_id, bool video) {
  if (t.codec.empty()) {
    return std::format("the {} codec '{}' of {} in this {} file is not supported", video ? "video" : "audio", t.tag,
                       track_label(t, index), format_id);
  }
  return std::format(
      "no {} decoder for '{}' is available in this build ({} of this {} file); converting to a container that can "
      "hold {} unchanged still works",
      video ? "video" : "audio", t.codec, track_label(t, index), format_id, t.codec);
}

// Shared state of one decoded stream.
template <class Decoder>
struct DecodeState {
  std::shared_ptr<Demuxed> demuxed;
  std::size_t track = 0;
  std::size_t next = 0;
  std::unique_ptr<Decoder> decoder;
  StreamAdapter adapter;
  bool configured = false;
  bool flushed = false;
  bool finished = false;
  Progress progress;
  Warnings warnings;
  std::uint64_t max_pixels = 0;
  bool no_pts_codec = false;  // MPEG-1/2/4 Part 2: may reorder, AVI has no PTS
};

template <class Decoder>
Result<void> configure(DecodeState<Decoder>& s) {
  const TrackInfo& t = s.demuxed->tracks[s.track];
  s.adapter = StreamAdapter(t.codec, t.extradata, decoder_packaging);
  s.no_pts_codec = t.codec == "mpeg4" || t.codec == "mpeg1video" || t.codec == "mpeg2video" || t.codec == "msmpeg4v3";
  // Learn in-band configuration from the first packets (Annex-B parameter sets, ADTS headers).
  for (std::size_t i = 0; i < t.packets.size() && i < 16 && !s.adapter.configured(); ++i) {
    const ByteView view = s.demuxed->packet_view(t.packets[i]);
    if (!view.empty()) {
      s.adapter.learn(view);
    } else {
      const Bytes copy = s.demuxed->packet_bytes(t.packets[i]);
      s.adapter.learn(copy);
    }
  }
  codec::CodecParams params = params_for(t, s.adapter);
  params.max_pixels = s.max_pixels;
  CVT_TRY(s.decoder->configure(params));
  s.configured = true;
  return {};
}

template <class Decoder>
Result<bool> feed(DecodeState<Decoder>& s) {
  const TrackInfo& t = s.demuxed->tracks[s.track];
  // Decoding progress (by packets) doubles as the cancellation point of every reader.
  const double done = t.packets.empty() ? 1.0 : static_cast<double>(s.next) / static_cast<double>(t.packets.size());
  if (!s.progress.report(done)) return fail(Status::Cancelled, "the conversion was cancelled");
  if (s.next < t.packets.size()) {
    const PacketRef& ref = t.packets[s.next++];
    const Bytes raw = s.demuxed->packet_bytes(ref);
    CVT_TRY_ASSIGN(Bytes payload, s.adapter.convert(raw, ref.keyframe));
    ir::Packet packet;
    packet.bytes = std::move(payload);
    packet.pts = ref.pts;
    packet.dts = ref.dts;
    packet.duration = ref.duration;
    packet.keyframe = ref.keyframe;
    // AVI stores no presentation times. For codecs that reorder pictures the
    // convention of codec_legacy/codec_notes.hpp applies: pts = INT64_MIN (none)
    // and dts = the chunk's place in the stream; the decoder derives the times.
    if (s.demuxed->format_id == "avi" && s.no_pts_codec) packet.pts = std::numeric_limits<std::int64_t>::min();
    CVT_TRY(s.decoder->send_packet(packet));
    return true;
  }
  if (!s.flushed) {
    s.flushed = true;
    CVT_TRY(s.decoder->flush());
    return true;
  }
  return false;
}

}  // namespace

Result<ir::Video> make_video(std::shared_ptr<Demuxed> demuxed, const Context& ctx) {
  ir::Video video;
  const std::int64_t audio_choice = ctx.options.get_int("audio_track", 0, 0, 1000);
  std::optional<std::size_t> video_track;
  std::optional<std::size_t> audio_track;
  std::size_t audio_seen = 0;
  for (std::size_t i = 0; i < demuxed->tracks.size(); ++i) {
    const TrackInfo& t = demuxed->tracks[i];
    if (t.type == ir::TrackType::Video && !video_track) video_track = i;
    if (t.type == ir::TrackType::Audio) {
      if (static_cast<std::int64_t>(audio_seen) == audio_choice) audio_track = i;
      ++audio_seen;
    }
  }
  if (!audio_track && audio_seen > 0) {
    for (std::size_t i = 0; i < demuxed->tracks.size(); ++i) {
      if (demuxed->tracks[i].type == ir::TrackType::Audio) {
        audio_track = i;
        break;
      }
    }
    if (audio_choice > 0) ctx.warn(std::format("the file has {} audio track(s); the first one was used", audio_seen));
  }

  ir::VideoInfo& info = video.info;
  info.metadata = demuxed->metadata;
  info.duration_ms = demuxed->duration_ms;
  if (video_track) {
    TrackInfo& t = demuxed->tracks[*video_track];
    if ((t.width == 0 || t.height == 0) && (t.codec == "h264" || t.codec == "hevc")) {
      const bool hevc = t.codec == "hevc";
      StreamAdapter probe(t.codec, t.extradata, decoder_packaging);
      for (std::size_t i = 0; i < t.packets.size() && i < 8 && !probe.configured(); ++i) {
        const Bytes data = demuxed->packet_bytes(t.packets[i]);
        probe.learn(data);
      }
      const Bytes record = probe.extradata();
      const auto sets = hevc ? bits::parse_hvcc(record) : bits::parse_avcc(record);
      if (sets && !sets->sps.empty()) {
        const auto sps = hevc ? bits::parse_hevc_sps(sets->sps.front()) : bits::parse_h264_sps(sets->sps.front());
        if (sps) {
          t.width = sps->width;
          t.height = sps->height;
        }
      }
    }
    info.width = t.width;
    info.height = t.height;
    info.fps = t.fps;
    info.rotation = t.rotation;
    info.frame_count = t.packets.size();
  }
  if (audio_track) {
    const TrackInfo& t = demuxed->tracks[*audio_track];
    info.has_audio = true;
    info.audio_sample_rate = t.sample_rate;
    info.audio_channels = t.channels;
    if (t.codec == "aac") {
      StreamAdapter probe(t.codec, t.extradata, decoder_packaging);
      if (!t.packets.empty()) {
        const Bytes data = demuxed->packet_bytes(t.packets.front());
        probe.learn(data);
      }
      if (const bits::AacConfig* aac = probe.aac()) {
        if (info.audio_sample_rate == 0) info.audio_sample_rate = aac->sample_rate;
        if (info.audio_channels == 0) info.audio_channels = static_cast<std::uint16_t>(aac->channel_config);
      }
    }
  }

  if (info.width != 0 && info.height != 0) CVT_TRY(ctx.limits.check_pixels(info.width, info.height));

  // Compressed tracks for pass-through, in the canonical packaging of
  // codec/codec.hpp (length-prefixed NAL units + avcC/hvcC, raw AAC + ASC,
  // AV1 OBUs without temporal delimiters + av1C), whatever the container used.
  for (std::size_t i = 0; i < demuxed->tracks.size(); ++i) {
    const TrackInfo& t = demuxed->tracks[i];
    ir::Track track;
    track.type = t.type;
    track.codec = t.codec.empty() ? std::string("unknown:") + t.tag : t.codec;
    track.extradata = t.extradata;
    track.timescale = t.timescale;
    auto cursor = std::make_shared<TrackCursor>(TrackCursor{demuxed, i, 0, std::nullopt});
    if (t.codec == "h264" || t.codec == "hevc" || t.codec == "aac" || t.codec == "av1") {
      StreamAdapter adapter(t.codec, t.extradata, decoder_packaging);
      for (std::size_t k = 0; k < t.packets.size() && k < 16 && !adapter.configured(); ++k) {
        adapter.learn(demuxed->packet_bytes(t.packets[k]));
      }
      if (t.codec != "av1" || !adapter.extradata().empty()) {
        Bytes canonical = adapter.extradata();
        if (!canonical.empty()) track.extradata = std::move(canonical);
      }
      cursor->adapter = std::move(adapter);
    }
    track.next_packet = [cursor]() -> Result<std::optional<ir::Packet>> {
      const TrackInfo& info_ = cursor->demuxed->tracks[cursor->track];
      if (cursor->next >= info_.packets.size()) return std::optional<ir::Packet>{};
      const PacketRef& ref = info_.packets[cursor->next++];
      ir::Packet packet;
      packet.bytes = cursor->demuxed->packet_bytes(ref);
      if (cursor->adapter) {
        CVT_TRY_ASSIGN(packet.bytes, cursor->adapter->convert(packet.bytes, ref.keyframe));
      }
      packet.pts = ref.pts;
      packet.dts = ref.dts;
      packet.duration = ref.duration;
      packet.keyframe = ref.keyframe;
      return std::optional<ir::Packet>(std::move(packet));
    };
    video.tracks.push_back(std::move(track));
  }

  const codec::Registry* media = ctx.registry != nullptr ? &ctx.registry->media() : nullptr;
  const std::string format_id = demuxed->format_id;
  if (video_track) {
    const std::size_t index = *video_track;
    const TrackInfo& t = demuxed->tracks[index];
    std::unique_ptr<codec::VideoDecoder> decoder =
        (media != nullptr && !t.codec.empty()) ? media->make_video_decoder(t.codec) : nullptr;
    if (!decoder) {
      const std::string message = missing_decoder(t, index, format_id, true);
      video.next_frame = [message]() -> Result<std::optional<ir::VideoFrame>> { return fail(Status::Decode, message); };
    } else {
      auto state = std::make_shared<DecodeState<codec::VideoDecoder>>();
      state->demuxed = demuxed;
      state->track = index;
      state->decoder = std::move(decoder);
      state->progress = ctx.progress;
      state->warnings = ctx.warnings;
      state->max_pixels = ctx.limits.max_pixels;
      video.next_frame = [state]() -> Result<std::optional<ir::VideoFrame>> {
        if (state->finished) return std::optional<ir::VideoFrame>{};
        if (!state->configured) CVT_TRY(configure(*state));
        while (true) {
          auto received = state->decoder->receive_frame();
          for (std::string& note : state->decoder->take_warnings()) state->warnings.add(std::move(note));
          CVT_TRY_ASSIGN(auto frame, std::move(received));
          if (frame) return frame;
          CVT_TRY_ASSIGN(const bool fed, feed(*state));
          if (!fed) {
            state->finished = true;
            return std::optional<ir::VideoFrame>{};
          }
        }
      };
    }
  }
  if (audio_track) {
    const std::size_t index = *audio_track;
    const TrackInfo& t = demuxed->tracks[index];
    std::unique_ptr<codec::AudioDecoder> decoder =
        (media != nullptr && !t.codec.empty()) ? media->make_audio_decoder(t.codec) : nullptr;
    if (!decoder) {
      const std::string message = missing_decoder(t, index, format_id, false);
      video.next_audio = [message]() -> Result<std::optional<ir::AudioChunk>> { return fail(Status::Decode, message); };
    } else {
      auto state = std::make_shared<DecodeState<codec::AudioDecoder>>();
      state->demuxed = demuxed;
      state->track = index;
      state->decoder = std::move(decoder);
      state->progress = ctx.progress;
      video.next_audio = [state]() -> Result<std::optional<ir::AudioChunk>> {
        if (state->finished) return std::optional<ir::AudioChunk>{};
        if (!state->configured) CVT_TRY(configure(*state));
        while (true) {
          CVT_TRY_ASSIGN(auto chunk, state->decoder->receive_chunk());
          if (chunk) return chunk;
          CVT_TRY_ASSIGN(const bool fed, feed(*state));
          if (!fed) {
            state->finished = true;
            return std::optional<ir::AudioChunk>{};
          }
        }
      };
    }
  }
  return video;
}

}  // namespace cvt::video
