// Convertible core - video family: the transcoding pipeline.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/video/pipeline.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <format>
#include <limits>
#include <optional>

#include "codec/colorspace.hpp"
#include "codec/ids.hpp"
#include "engine/standard_options.hpp"
#include "families/video/adapt.hpp"
#include "families/video/bitstream.hpp"
#include "families/video/common.hpp"
#include "families/video/containers.hpp"
#include "families/video/mux.hpp"
#include "families/video/policy.hpp"

namespace cvt::video {

namespace {

constexpr std::uint32_t video_timescale = 90000;

// --------------------------------------------------------------------------- options

bool any_set(const Options& o, std::initializer_list<std::string_view> keys) {
  return std::ranges::any_of(keys, [&](std::string_view k) { return o.was_set(k); });
}

codec::EncodePreset encoder_preset(const Options& o) {
  const std::string speed = o.get_enum("encoder_speed", "auto", {"auto", "fast", "balanced", "slow"});
  if (speed == "fast") return codec::EncodePreset::Fast;
  if (speed == "balanced") return codec::EncodePreset::Balanced;
  if (speed == "slow") return codec::EncodePreset::Small;
  return options::resolved_preset(o) == options::Preset::Small ? codec::EncodePreset::Small : codec::EncodePreset::Balanced;
}

ir::Rational rational_fps(double fps) {
  if (fps <= 0) return {};
  for (const std::int64_t den : {1, 1001}) {
    const double num = fps * static_cast<double>(den) / (den == 1001 ? 1000.0 : 1.0);
    if (den == 1001 && std::abs(num - std::round(num)) < 0.01 && std::fmod(std::round(num), 1000.0) == 0.0) {
      return {static_cast<std::int64_t>(std::round(num)), 1001};
    }
    if (den == 1 && std::abs(fps - std::round(fps)) < 1e-6) return {static_cast<std::int64_t>(std::round(fps)), 1};
  }
  return {static_cast<std::int64_t>(std::llround(fps * 1000.0)), 1000};
}

// --------------------------------------------------------------------------- audio helpers

/// Sample rate and channel count of an audio stream, from its configuration
/// or first packet, for containers that need them in their headers.
void probe_audio(std::string_view codec, ByteView extradata, ByteView first, std::uint32_t& rate, std::uint16_t& channels) {
  if (codec == codec::id::aac) {
    if (auto asc = bits::parse_asc(extradata)) {
      rate = asc->sample_rate;
      channels = static_cast<std::uint16_t>(asc->channel_config);
    } else if (auto adts = bits::parse_adts(first)) {
      rate = adts->config.sample_rate;
      channels = static_cast<std::uint16_t>(adts->config.channel_config);
    }
  } else if (codec == codec::id::mp3 || codec == codec::id::mp2) {
    if (auto h = bits::parse_mpeg_audio(first)) {
      rate = h->sample_rate;
      channels = static_cast<std::uint16_t>(h->channels);
    }
  } else if (codec == codec::id::ac3) {
    if (auto h = parse_ac3(first)) {
      rate = h->sample_rate;
      channels = static_cast<std::uint16_t>(h->channels);
    }
  } else if (codec == codec::id::opus) {
    rate = 48000;
    if (extradata.size() >= 10) channels = std::to_integer<std::uint16_t>(extradata[9]);
  } else if (codec == codec::id::flac) {
    if (auto info = bits::parse_flac_streaminfo(extradata.size() == 34 ? ByteView{} : extradata)) {
      rate = info->sample_rate;
      channels = static_cast<std::uint16_t>(info->channels);
    } else if (extradata.size() == 34) {
      Bytes block = {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{34}};
      block.insert(block.end(), extradata.begin(), extradata.end());
      if (auto i = bits::parse_flac_streaminfo(block)) {
        rate = i->sample_rate;
        channels = static_cast<std::uint16_t>(i->channels);
      }
    }
  } else if (codec == codec::id::vorbis) {
    if (auto headers = bits::split_xiph_lacing(extradata); headers && !headers->empty()) {
      if (auto id = bits::parse_vorbis_id(headers->front())) {
        rate = id->sample_rate;
        channels = static_cast<std::uint16_t>(id->channels);
      }
    }
  }
}

std::uint16_t pcm_bits(std::string_view codec) {
  if (codec == codec::id::pcm_s24le) return 24;
  if (codec == codec::id::pcm_f32le) return 32;
  if (codec == codec::id::pcm_u8 || codec == codec::id::pcm_alaw || codec == codec::id::pcm_mulaw) return 8;
  return 16;
}

/// A sample rate the codec accepts, as close to `wanted` as possible.
std::uint32_t supported_rate(std::string_view codec, std::uint32_t wanted) {
  std::vector<std::uint32_t> rates;
  if (codec == codec::id::opus) rates = {48000, 24000, 16000, 12000, 8000};
  if (codec == codec::id::aac) rates = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000};
  if (codec == codec::id::mp3 || codec == codec::id::mp2) rates = {48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000};
  if (codec == codec::id::ac3) rates = {48000, 44100, 32000};
  if (codec == codec::id::amr_nb) rates = {8000};
  if (codec == codec::id::amr_wb) rates = {16000};
  if (rates.empty() || std::ranges::find(rates, wanted) != rates.end()) return wanted;
  if (codec == codec::id::opus) return 48000;
  // The smallest supported rate at or above the wanted one, else the largest.
  std::uint32_t best = 0;
  for (const std::uint32_t r : rates) {
    if (r >= wanted && (best == 0 || r < best)) best = r;
  }
  return best != 0 ? best : *std::ranges::max_element(rates);
}

std::uint16_t supported_channels(std::string_view codec, std::uint16_t wanted) {
  if (codec == codec::id::amr_nb || codec == codec::id::amr_wb) return 1;
  if (codec == codec::id::mp3 || codec == codec::id::mp2) return std::min<std::uint16_t>(wanted, 2);
  if (codec == codec::id::ac3 || codec == codec::id::opus) return std::min<std::uint16_t>(wanted, 6);
  return wanted;
}

// --------------------------------------------------------------------------- streams

struct Window {
  std::int64_t origin_us = 0;  // source time that becomes output time 0
  std::int64_t end_us = std::numeric_limits<std::int64_t>::max();
};

class OutStream {
 public:
  virtual ~OutStream() = default;
  virtual Result<std::optional<ir::Packet>> next() = 0;
  /// Configuration known only after encoding started; empty if unchanged.
  virtual Bytes late_extradata() { return {}; }
  MuxTrack desc;
};

// Pass-through of a compressed track.
class CopyStream final : public OutStream {
 public:
  CopyStream(const ir::Track& track, std::optional<ir::Packet> first) : track_(track), head_(std::move(first)) {}
  void set_window(const Window& w, bool trim) {
    offset_ = rescale(w.origin_us, 1'000'000, track_.timescale);
    end_ = w.end_us == std::numeric_limits<std::int64_t>::max() ? std::numeric_limits<std::int64_t>::max()
                                                                : rescale(w.end_us, 1'000'000, track_.timescale);
    trim_ = trim;
  }
  Result<std::optional<ir::Packet>> next() override {
    while (true) {
      std::optional<ir::Packet> p;
      if (head_) {
        p = std::move(head_);
        head_.reset();
      } else {
        if (done_) return std::optional<ir::Packet>{};
        CVT_TRY_ASSIGN(p, track_.next_packet());
        if (!p) {
          done_ = true;
          return p;
        }
      }
      if (trim_) {
        if (p->pts >= end_) {
          done_ = true;
          return std::optional<ir::Packet>{};
        }
        if (!started_) {
          const bool audio = track_.type == ir::TrackType::Audio;
          if (p->pts + p->duration <= offset_ || (!audio && (!p->keyframe || p->pts < offset_))) continue;
          started_ = true;
        }
      }
      p->pts -= offset_;
      p->dts -= offset_;
      return p;
    }
  }

 private:
  const ir::Track& track_;
  std::optional<ir::Packet> head_;
  std::int64_t offset_ = 0;
  std::int64_t end_ = std::numeric_limits<std::int64_t>::max();
  bool trim_ = false;
  bool started_ = false;
  bool done_ = false;
};

struct VideoSettings {
  std::string codec;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  double fps = 0;  // 0 = keep the source timing
  int rotate = 0;
  codec::VideoEncodeParams params;
};

class VideoEncodeStream final : public OutStream {
 public:
  VideoEncodeStream(const ir::Video& video, std::optional<ir::VideoFrame> first, std::unique_ptr<codec::VideoEncoder> encoder,
                    VideoSettings settings, Window window, Limits limits, Progress progress)
      : video_(video),
        head_(std::move(first)),
        encoder_(std::move(encoder)),
        s_(std::move(settings)),
        window_(window),
        limits_(limits),
        progress_(std::move(progress)) {}

  Result<void> start() {
    CVT_TRY(limits_.check_pixels(s_.width, s_.height));
    CVT_TRY(encoder_->configure(s_.params));
    desc.extradata = encoder_->extradata();
    return {};
  }

  Result<std::optional<ir::Packet>> next() override {
    while (true) {
      if (progress_.cancelled()) return fail(Status::Cancelled, "the conversion was cancelled");
      CVT_TRY_ASSIGN(auto packet, encoder_->receive_packet());
      if (packet) return packet;
      if (flushed_) return std::optional<ir::Packet>{};
      CVT_TRY_ASSIGN(auto frame, next_output_frame());
      if (!frame) {
        CVT_TRY(encoder_->flush());
        flushed_ = true;
        continue;
      }
      CVT_TRY(encoder_->send_frame(*frame));
    }
  }

  Bytes late_extradata() override { return encoder_->extradata(); }

 private:
  // Next decoded frame inside the window, filtered; times relative to the window origin.
  Result<std::optional<ir::VideoFrame>> next_filtered() {
    while (true) {
      std::optional<ir::VideoFrame> frame;
      if (head_) {
        frame = std::move(head_);
        head_.reset();
      } else {
        if (source_done_) return std::optional<ir::VideoFrame>{};
        CVT_TRY_ASSIGN(frame, video_.next_frame());
        if (!frame) {
          source_done_ = true;
          return frame;
        }
      }
      if (!frame->valid()) return fail(Status::Decode, "the video decoder produced an invalid frame");
      const std::int64_t t_us = frame->pts_ms * 1000;
      if (t_us >= window_.end_us) {
        source_done_ = true;
        return std::optional<ir::VideoFrame>{};
      }
      const std::int64_t end_us = t_us + std::max<std::int64_t>(frame->duration_ms, 1) * 1000;
      if (end_us <= window_.origin_us) continue;
      ir::VideoFrame f = std::move(*frame);
      CVT_TRY(limits_.check_pixels(f.width, f.height));
      if (f.format != s_.params.pixel_format) {
        CVT_TRY_ASSIGN(f, codec::convert_frame(f, s_.params.pixel_format, s_.params.color));
      }
      if (s_.rotate != 0) {
        CVT_TRY_ASSIGN(f, rotate_frame(f, s_.rotate));
      }
      if (f.width != s_.width || f.height != s_.height) {
        CVT_TRY_ASSIGN(f, scale_frame(f, s_.width, s_.height));
      }
      if (f.format != s_.params.pixel_format) {
        CVT_TRY_ASSIGN(f, codec::convert_frame(f, s_.params.pixel_format, s_.params.color));
      }
      f.pts_ms = (t_us - window_.origin_us) / 1000;
      if (f.pts_ms < 0) f.pts_ms = 0;
      return std::optional<ir::VideoFrame>(std::move(f));
    }
  }

  Result<std::optional<ir::VideoFrame>> next_output_frame() {
    if (s_.fps <= 0) {
      // Variable timing: frames keep their own timestamps.
      CVT_TRY_ASSIGN(auto f, next_filtered());
      if (f && f->pts_ms <= last_pts_ && emitted_ > 0) f->pts_ms = last_pts_ + 1;
      if (f) {
        last_pts_ = f->pts_ms;
        ++emitted_;
      }
      return f;
    }
    // Constant frame rate: output frame k shows the newest input frame at k / fps.
    const double tick_ms = 1000.0 / s_.fps;
    const double end_ms =
        window_.end_us == std::numeric_limits<std::int64_t>::max() ? 1e300 : static_cast<double>(window_.end_us - window_.origin_us) / 1000.0;
    while (true) {
      const double t = static_cast<double>(emitted_) * tick_ms;
      if (t >= end_ms) return std::optional<ir::VideoFrame>{};
      if (!current_) {
        CVT_TRY_ASSIGN(current_, next_filtered());
        if (!current_) return std::optional<ir::VideoFrame>{};
        CVT_TRY_ASSIGN(upcoming_, next_filtered());
      }
      // Advance while the next frame is due at or before the middle of this tick.
      while (upcoming_ && static_cast<double>(upcoming_->pts_ms) <= t + tick_ms / 2) {
        current_ = std::move(upcoming_);
        CVT_TRY_ASSIGN(upcoming_, next_filtered());
      }
      if (!upcoming_) {
        const double last_end = static_cast<double>(current_->pts_ms) +
                                (current_->duration_ms > 0 ? static_cast<double>(current_->duration_ms) : tick_ms);
        if (t >= last_end - tick_ms / 2) return std::optional<ir::VideoFrame>{};
      }
      ir::VideoFrame out = *current_;
      out.pts_ms = static_cast<std::int64_t>(std::llround(t));
      out.duration_ms = static_cast<std::int64_t>(std::llround(tick_ms));
      ++emitted_;
      return std::optional<ir::VideoFrame>(std::move(out));
    }
  }

  const ir::Video& video_;
  std::optional<ir::VideoFrame> head_;
  std::unique_ptr<codec::VideoEncoder> encoder_;
  VideoSettings s_;
  Window window_;
  Limits limits_;
  Progress progress_;
  bool source_done_ = false;
  bool flushed_ = false;
  std::optional<ir::VideoFrame> current_;
  std::optional<ir::VideoFrame> upcoming_;
  std::int64_t emitted_ = 0;
  std::int64_t last_pts_ = -1;
};

struct AudioSettings {
  std::string codec;
  std::uint32_t in_rate = 0;
  std::uint16_t in_channels = 0;
  codec::AudioEncodeParams params;
};

class AudioEncodeStream final : public OutStream {
 public:
  AudioEncodeStream(const ir::Video& video, std::optional<ir::AudioChunk> first, std::unique_ptr<codec::AudioEncoder> encoder,
                    AudioSettings settings, Window window, Progress progress)
      : video_(video),
        head_(std::move(first)),
        encoder_(std::move(encoder)),
        s_(std::move(settings)),
        window_(window),
        progress_(std::move(progress)) {
    resampler_ = make_resampler(s_.in_rate, s_.params.sample_rate, s_.in_channels);
  }

  Result<void> start() {
    CVT_TRY(encoder_->configure(s_.params));
    desc.extradata = encoder_->extradata();
    return {};
  }

  Result<std::optional<ir::Packet>> next() override {
    while (true) {
      if (progress_.cancelled()) return fail(Status::Cancelled, "the conversion was cancelled");
      CVT_TRY_ASSIGN(auto packet, encoder_->receive_packet());
      if (packet) {
        if (!shift_known_) {
          // Encoders either start their packets at -delay already or at 0;
          // normalise to "first real sample at 0" so muxers can signal priming.
          const std::int64_t delay = encoder_->delay();
          shift_ = (delay > 0 && packet->pts >= 0) ? -rescale(delay, s_.params.sample_rate, s_.params.timescale) : 0;
          shift_known_ = true;
        }
        packet->pts += shift_;
        packet->dts += shift_;
        return packet;
      }
      if (flushed_) return std::optional<ir::Packet>{};
      CVT_TRY_ASSIGN(const bool more, feed());
      if (!more) {
        CVT_TRY(encoder_->flush());
        flushed_ = true;
      }
    }
  }

  Bytes late_extradata() override { return encoder_->extradata(); }

 private:
  Result<bool> send(std::vector<float> samples) {
    if (samples.empty()) return true;
    ir::AudioChunk chunk;
    chunk.pts_ms = sent_frames_ * 1000 / static_cast<std::int64_t>(s_.params.sample_rate);
    sent_frames_ += static_cast<std::int64_t>(samples.size() / s_.params.channels);
    chunk.samples = std::move(samples);
    CVT_TRY(encoder_->send_chunk(chunk));
    return true;
  }

  Result<bool> feed() {
    if (source_done_) return false;
    std::optional<ir::AudioChunk> chunk;
    if (head_) {
      chunk = std::move(head_);
      head_.reset();
    } else {
      CVT_TRY_ASSIGN(chunk, video_.next_audio());
    }
    const std::uint16_t in_ch = s_.in_channels;
    if (!chunk) {
      source_done_ = true;
      std::vector<float> tail = resampler_->process({}, true);
      CVT_TRY(send(remix_channels(tail, in_ch, s_.params.channels)));
      return true;
    }
    if (in_ch == 0 || chunk->samples.size() % in_ch != 0) return fail(Status::Decode, "the audio decoder returned a partial sample frame");
    // Position in input sample frames relative to the window origin.
    const std::int64_t rate = s_.in_rate;
    std::int64_t start = (chunk->pts_ms * 1000 - window_.origin_us) * rate / 1'000'000;
    std::span<const float> samples = chunk->samples;
    std::int64_t frames = static_cast<std::int64_t>(samples.size() / in_ch);
    if (!started_) {
      expected_ = std::max<std::int64_t>(start, 0);
      started_ = true;
      if (start > 0) expected_ = 0;  // audio starts late: pad with silence below
    }
    // Keep A/V sync: fill gaps with silence, drop overlaps (beyond 20 ms of jitter).
    const std::int64_t tolerance = rate / 50;
    if (start > expected_ + tolerance) {
      std::vector<float> silence(static_cast<std::size_t>(start - expected_) * in_ch, 0.0f);
      CVT_TRY(process(silence));
      expected_ = start;
    } else if (start < expected_ - tolerance) {
      const std::int64_t drop = std::min(frames, expected_ - start);
      samples = samples.subspan(static_cast<std::size_t>(drop) * in_ch);
      frames -= drop;
      start += drop;
    }
    // Trim to the window.
    if (start < 0) {
      const std::int64_t drop = std::min(frames, -start);
      samples = samples.subspan(static_cast<std::size_t>(drop) * in_ch);
      frames -= drop;
      start += drop;
    }
    if (window_.end_us != std::numeric_limits<std::int64_t>::max()) {
      const std::int64_t end = (window_.end_us - window_.origin_us) * rate / 1'000'000;
      if (start + frames > end) {
        frames = std::max<std::int64_t>(0, end - start);
        samples = samples.first(static_cast<std::size_t>(frames) * in_ch);
        source_done_ = frames == 0 || true;
        CVT_TRY(process(samples));
        std::vector<float> tail = resampler_->process({}, true);
        CVT_TRY(send(remix_channels(tail, in_ch, s_.params.channels)));
        return true;
      }
    }
    expected_ = start + frames;
    CVT_TRY(process(samples));
    return true;
  }

  Result<void> process(std::span<const float> samples) {
    std::vector<float> resampled = resampler_->process(samples, false);
    CVT_TRY(send(remix_channels(resampled, s_.in_channels, s_.params.channels)));
    return {};
  }

  const ir::Video& video_;
  std::optional<ir::AudioChunk> head_;
  std::unique_ptr<codec::AudioEncoder> encoder_;
  AudioSettings s_;
  Window window_;
  Progress progress_;
  std::unique_ptr<AudioResampler> resampler_;
  bool source_done_ = false;
  bool flushed_ = false;
  bool started_ = false;
  bool shift_known_ = false;
  std::int64_t shift_ = 0;
  std::int64_t expected_ = 0;
  std::int64_t sent_frames_ = 0;
};

// --------------------------------------------------------------------------- planning

struct SourceTracks {
  std::optional<std::size_t> video;
  std::vector<std::size_t> audio;  // in file order
};

SourceTracks find_tracks(const ir::Video& video) {
  SourceTracks s;
  for (std::size_t i = 0; i < video.tracks.size(); ++i) {
    const ir::Track& t = video.tracks[i];
    if (!t.next_packet) continue;
    if (t.type == ir::TrackType::Video && !s.video) s.video = i;
    if (t.type == ir::TrackType::Audio) s.audio.push_back(i);
  }
  return s;
}

}  // namespace

std::vector<OptionSpec> container_read_options(bool raw_stream) {
  std::vector<OptionSpec> specs = {
      OptionSpec::integer("audio_track", "Audio track", 0, 0, 1000,
                          "Which audio track to convert when the file has several (0 = the first).")
          .advanced()};
  if (raw_stream) {
    specs.push_back(OptionSpec::number("raw_fps", "Frame rate of a raw stream", 25, 1, 1000,
                                       "Raw video streams carry no timestamps; frames are timed at this rate.")
                        .advanced());
  }
  return specs;
}

std::vector<OptionSpec> container_write_options(std::string_view format_id) {
  const ContainerPolicy* policy = policy_for(format_id);
  const bool has_video = policy != nullptr && !policy->video_storable.empty();
  const bool has_audio = policy != nullptr && !policy->audio_storable.empty();
  std::vector<OptionSpec> specs = {options::quality(), options::preset()};
  if (has_video) {
    std::vector<std::string> choices = {"auto", "copy"};
    for (const std::string_view c : policy->video_storable) choices.emplace_back(c);
    specs.push_back(OptionSpec::choice("video_codec", "Video codec", "auto", choices,
                                       "Codec of the video stream: auto picks the container's default (or keeps the "
                                       "source stream when it fits), copy never re-encodes."));
    specs.push_back(options::width());
    specs.push_back(options::height());
    specs.push_back(options::scale());
    specs.push_back(options::max_width());
    specs.push_back(options::max_height());
    specs.push_back(options::fps());
    specs.push_back(options::video_bitrate_kbps());
    specs.push_back(OptionSpec::number("keyframe_interval", "Keyframe interval", 0, 0, 60,
                                       "Seconds between keyframes; 0 lets the encoder decide.")
                        .advanced());
    specs.push_back(OptionSpec::choice("encoder_speed", "Encoder speed", "auto", {"auto", "fast", "balanced", "slow"},
                                       "Trade encoding time for file size; auto follows the preset.")
                        .advanced());
    specs.push_back(OptionSpec::boolean("auto_rotate", "Apply rotation", true,
                                        "When re-encoding, turn the pictures upright instead of keeping a rotation flag.")
                        .advanced());
  }
  if (has_audio) {
    std::vector<std::string> choices = {"auto", "copy"};
    for (const std::string_view c : policy->audio_storable) choices.emplace_back(c);
    specs.push_back(OptionSpec::choice("audio_codec", "Audio codec", "auto", choices,
                                       "Codec of the audio stream: auto picks the container's default (or keeps the "
                                       "source stream when it fits), copy never re-encodes."));
    specs.push_back(options::audio_bitrate_kbps());
    specs.push_back(options::sample_rate());
    specs.push_back(options::channels());
  }
  if (has_video && has_audio) {
    specs.push_back(OptionSpec::choice("streams", "Streams", "all", {"all", "video_only", "audio_only"},
                                       "Keep both picture and sound, or drop one of them."));
  }
  specs.push_back(OptionSpec::number("start", "Start time", 0, 0, 1e7, "Skip this many seconds from the beginning."));
  specs.push_back(OptionSpec::number("duration", "Duration", 0, 0, 1e7, "Keep at most this many seconds (0 = to the end)."));
  specs.push_back(options::strip_metadata());
  return specs;
}

Result<Bytes> write_container(const ir::Video& video, WriteContext& ctx, std::string_view format_id) {
  const ContainerPolicy* policy = policy_for(format_id);
  if (policy == nullptr) return fail(Status::Internal, "no container policy for '{}'", format_id);
  if (ctx.registry == nullptr) return fail(Status::Internal, "the video pipeline needs a registry");
  const codec::Registry& media = ctx.registry->media();
  const Options& o = ctx.options;

  const SourceTracks src = find_tracks(video);
  const std::string streams = o.get_enum("streams", "all", {"all", "video_only", "audio_only"});
  const bool source_has_video = src.video.has_value() || static_cast<bool>(video.next_frame);
  const bool source_has_audio = !src.audio.empty() || static_cast<bool>(video.next_audio);
  const bool want_video = !policy->video_storable.empty() && streams != "audio_only" && source_has_video;
  const bool want_audio = !policy->audio_storable.empty() && streams != "video_only" && source_has_audio;
  if (!want_video && !want_audio) {
    if (policy->video_storable.empty() && !source_has_audio) {
      return fail(Status::Encode, "the source has no audio track to write as {}", format_id);
    }
    return fail(Status::Encode, "nothing to write: the source has no stream that {} can hold", format_id);
  }
  if (!policy->video_storable.empty() && source_has_video && !want_video && streams != "audio_only") {
    ctx.warn(std::format("{} holds only audio; the picture was dropped", format_id));
  }
  if (!policy->audio_storable.empty() && source_has_audio && !want_audio && streams != "video_only" &&
      policy->audio_storable.empty()) {
    ctx.warn(std::format("{} holds no audio; the sound was dropped", format_id));
  }
  if (policy->audio_storable.empty() && source_has_audio && streams != "video_only") {
    ctx.warn(std::format("{} cannot hold audio; the sound was dropped", format_id));
  }

  const double start_s = o.get_float("start", 0.0, 0.0, 1e7);
  const double duration_s = o.get_float("duration", 0.0, 0.0, 1e7);
  const bool trim = start_s > 0 || duration_s > 0;
  const bool same_format = !ctx.source_format_id.empty() && ctx.source_format_id == format_id;
  const std::string video_req = o.get_string("video_codec", "auto");
  const std::string audio_req = o.get_string("audio_codec", "auto");

  // ---------------------------------------------------------------- decide per stream
  const ir::Track* vtrack = src.video ? &video.tracks[*src.video] : nullptr;
  std::vector<const ir::Track*> atracks;
  const std::int64_t audio_choice = o.get_int("audio_track", 0, 0, 1000);
  for (const std::size_t i : src.audio) atracks.push_back(&video.tracks[i]);
  const ir::Track* atrack = atracks.empty() ? nullptr
                                            : atracks[static_cast<std::size_t>(std::min<std::int64_t>(
                                                  audio_choice, static_cast<std::int64_t>(atracks.size()) - 1))];

  const bool video_forced = any_set(o, {"width", "height", "scale", "max_width", "max_height", "fps", "video_bitrate_kbps",
                                        "quality", "preset", "keyframe_interval", "encoder_speed"}) ||
                            trim || same_format || (video_req != "auto" && video_req != "copy" && vtrack && vtrack->codec != video_req);
  const bool audio_forced = any_set(o, {"sample_rate", "channels", "audio_bitrate_kbps", "quality", "preset"}) || trim ||
                            same_format || (audio_req != "auto" && audio_req != "copy" && atrack && atrack->codec != audio_req);
  const bool video_copy_ok = vtrack != nullptr && container_accepts(format_id, ir::TrackType::Video, vtrack->codec);
  const bool audio_copy_ok = atrack != nullptr && container_accepts(format_id, ir::TrackType::Audio, atrack->codec);
  bool copy_video = false;
  bool copy_audio = false;
  if (want_video) {
    if (video_req == "copy") {
      if (!video_copy_ok) {
        return fail(Status::Encode, "the {} video cannot be copied into {} unchanged", vtrack ? vtrack->codec : "decoded", format_id);
      }
      copy_video = true;
    } else {
      copy_video = video_copy_ok && !video_forced;
    }
  }
  if (want_audio) {
    if (audio_req == "copy") {
      if (!audio_copy_ok) {
        return fail(Status::Encode, "the {} audio cannot be copied into {} unchanged", atrack ? atrack->codec : "decoded", format_id);
      }
      copy_audio = true;
    } else {
      copy_audio = audio_copy_ok && !audio_forced;
    }
  }
  const bool trim_copy = trim && (copy_video || copy_audio);

  // ---------------------------------------------------------------- peek sources for timing
  std::optional<ir::VideoFrame> first_frame;
  std::optional<ir::AudioChunk> first_chunk;
  if (want_video && !copy_video) {
    if (!video.next_frame) return fail(Status::Decode, "the video stream of this file cannot be decoded");
    CVT_TRY_ASSIGN(first_frame, video.next_frame());
    if (!first_frame) {
      if (!want_audio) return fail(Status::Decode, "the video has no frames");
    }
  }
  if (want_audio && !copy_audio) {
    if (!video.next_audio) return fail(Status::Decode, "the audio stream of this file cannot be decoded");
    CVT_TRY_ASSIGN(first_chunk, video.next_audio());
  }
  std::vector<std::unique_ptr<OutStream>> streams_out;
  std::vector<std::pair<const ir::Track*, std::optional<ir::Packet>>> copies;
  if (copy_video) {
    CVT_TRY_ASSIGN(auto p, vtrack->next_packet());
    copies.emplace_back(vtrack, std::move(p));
  }
  if (copy_audio) {
    const bool all = policy->multiple_audio && !o.was_set("audio_track");
    for (const ir::Track* t : atracks) {
      if (!all && t != atrack) continue;
      if (!container_accepts(format_id, ir::TrackType::Audio, t->codec)) {
        ctx.warn(std::format("an audio track in {} was dropped because {} cannot hold it", t->codec, format_id));
        continue;
      }
      CVT_TRY_ASSIGN(auto p, t->next_packet());
      copies.emplace_back(t, std::move(p));
    }
  }
  // Output time 0 = the earliest start of any kept stream (priming before 0 ignored), plus `start`.
  std::int64_t origin_us = std::numeric_limits<std::int64_t>::max();
  if (first_frame) origin_us = std::min(origin_us, first_frame->pts_ms * 1000);
  if (first_chunk) origin_us = std::min(origin_us, std::max<std::int64_t>(0, first_chunk->pts_ms * 1000));
  for (const auto& [t, p] : copies) {
    if (p) origin_us = std::min(origin_us, std::max<std::int64_t>(0, rescale(p->pts, t->timescale, 1'000'000)));
  }
  if (origin_us == std::numeric_limits<std::int64_t>::max()) origin_us = 0;
  if (origin_us < 0) origin_us = 0;
  Window window;
  window.origin_us = origin_us + static_cast<std::int64_t>(std::llround(start_s * 1e6));
  if (duration_s > 0) window.end_us = window.origin_us + static_cast<std::int64_t>(std::llround(duration_s * 1e6));

  // ---------------------------------------------------------------- build output streams
  const ir::VideoInfo& info = video.info;
  for (auto& [t, p] : copies) {
    auto stream = std::make_unique<CopyStream>(*t, p);
    stream->set_window(window, trim_copy);
    MuxTrack& d = stream->desc;
    d.type = t->type;
    d.codec = t->codec;
    d.extradata = t->extradata;
    d.timescale = t->timescale;
    if (t->type == ir::TrackType::Video) {
      d.width = info.width;
      d.height = info.height;
      d.fps = info.fps;
      d.rotation = info.rotation;
      if (info.rotation != 0 && !policy->stores_rotation) {
        ctx.warn(std::format("{} cannot record the {} degree display rotation of the copied video", format_id, info.rotation));
      }
    } else {
      d.sample_rate = t == atrack ? info.audio_sample_rate : 0;
      d.channels = t == atrack ? info.audio_channels : 0;
      std::uint32_t rate = 0;
      std::uint16_t channels = 0;
      probe_audio(t->codec, t->extradata, p ? ByteView(p->bytes) : ByteView{}, rate, channels);
      if (rate != 0) d.sample_rate = rate;
      if (channels != 0) d.channels = channels;
      if (d.codec.starts_with("pcm_")) {
        d.bits_per_sample = pcm_bits(d.codec);
        d.block_align = static_cast<std::uint32_t>(d.channels) * (d.bits_per_sample / 8u);
      }
    }
    streams_out.push_back(std::move(stream));
  }
  if (want_video && !copy_video && first_frame) {
    CVT_TRY_ASSIGN(const CodecChoice choice, choose_encoder(*policy, ir::TrackType::Video, video_req, media));
    auto encoder = media.make_video_encoder(choice.codec);
    if (!encoder) return fail(Status::Internal, "the {} encoder factory returned nothing", choice.codec);
    VideoSettings vs;
    vs.codec = choice.codec;
    const bool auto_rotate = o.get_bool("auto_rotate", true);
    vs.rotate = auto_rotate ? info.rotation : 0;
    std::uint32_t sw = first_frame->width;
    std::uint32_t sh = first_frame->height;
    if (vs.rotate == 90 || vs.rotate == 270) std::swap(sw, sh);
    const options::Size size = options::resolved_video_size(o, {sw, sh});
    vs.width = size.width;
    vs.height = size.height;
    if (choice.codec == codec::id::rawvideo) {
      // Raw video can keep the exact source size.
      if (!any_set(o, {"width", "height", "scale", "max_width", "max_height"})) {
        vs.width = sw;
        vs.height = sh;
      }
    }
    const double source_fps = info.fps.value();
    double fps = options::resolved_fps(o, 0.0);
    if (fps <= 0 && policy->constant_frame_rate) fps = source_fps > 0 ? source_fps : 25.0;
    vs.fps = fps;
    codec::VideoEncodeParams& vp = vs.params;
    vp.width = vs.width;
    vp.height = vs.height;
    vp.fps = fps > 0 ? (o.was_set("fps") ? rational_fps(fps) : (info.fps.num > 0 ? info.fps : rational_fps(fps))) : info.fps;
    vp.quality = options::resolved_quality(o);
    vp.bitrate_kbps = options::resolved_video_bitrate_kbps(o);
    const double kf = o.get_float("keyframe_interval", 0.0, 0.0, 60.0);
    const double rate_for_gop = fps > 0 ? fps : (source_fps > 0 ? source_fps : 25.0);
    vp.keyframe_interval = kf > 0 ? static_cast<std::uint32_t>(std::max(1.0, std::round(kf * rate_for_gop))) : 0;
    vp.preset = encoder_preset(o);
    vp.pixel_format = ir::PixelFormat::I420;
    if (choice.codec == codec::id::rawvideo && format_id == "y4m" && ir::is_yuv(first_frame->format) &&
        first_frame->format != ir::PixelFormat::NV12 && first_frame->format != ir::PixelFormat::P010) {
      vp.pixel_format = first_frame->format;
    }
    vp.color = ir::is_yuv(first_frame->format) ? first_frame->color : ir::ColorInfo{};
    vp.timescale = video_timescale;
    auto stream = std::make_unique<VideoEncodeStream>(video, std::move(first_frame), std::move(encoder), vs, window,
                                                      ctx.limits, ctx.progress);
    CVT_TRY(stream->start());
    MuxTrack& d = stream->desc;
    d.type = ir::TrackType::Video;
    d.codec = choice.codec;
    d.timescale = video_timescale;
    d.width = vs.width;
    d.height = vs.height;
    d.fps = vp.fps;
    d.rotation = auto_rotate ? 0 : info.rotation;
    if (!auto_rotate && info.rotation != 0 && !policy->stores_rotation) {
      ctx.warn(std::format("{} cannot record the {} degree display rotation", format_id, info.rotation));
    }
    streams_out.push_back(std::move(stream));
  }
  if (want_audio && !copy_audio && first_chunk) {
    CVT_TRY_ASSIGN(const CodecChoice choice, choose_encoder(*policy, ir::TrackType::Audio, audio_req, media));
    auto encoder = media.make_audio_encoder(choice.codec);
    if (!encoder) return fail(Status::Internal, "the {} encoder factory returned nothing", choice.codec);
    if (info.audio_sample_rate == 0 || info.audio_channels == 0) {
      return fail(Status::Decode, "the audio stream does not state its sample rate and channel count");
    }
    AudioSettings as;
    as.codec = choice.codec;
    as.in_rate = info.audio_sample_rate;
    as.in_channels = info.audio_channels;
    codec::AudioEncodeParams& ap = as.params;
    ap.sample_rate = supported_rate(choice.codec, options::resolved_sample_rate(o, info.audio_sample_rate));
    ap.channels = supported_channels(choice.codec, options::resolved_channels(o, info.audio_channels));
    if (ap.sample_rate != options::resolved_sample_rate(o, info.audio_sample_rate)) {
      ctx.warn(std::format("{} does not support {} Hz; the audio was resampled to {} Hz", choice.codec,
                           options::resolved_sample_rate(o, info.audio_sample_rate), ap.sample_rate));
    }
    ap.quality = options::resolved_quality(o);
    ap.bitrate_kbps = options::resolved_audio_bitrate_kbps(o);
    ap.compression_level = options::resolved_compression_level(o);
    ap.timescale = ap.sample_rate;
    auto stream = std::make_unique<AudioEncodeStream>(video, std::move(first_chunk), std::move(encoder), as, window, ctx.progress);
    CVT_TRY(stream->start());
    MuxTrack& d = stream->desc;
    d.type = ir::TrackType::Audio;
    d.codec = choice.codec;
    d.timescale = ap.sample_rate;
    d.sample_rate = ap.sample_rate;
    d.channels = ap.channels;
    if (d.codec.starts_with("pcm_")) {
      d.bits_per_sample = pcm_bits(d.codec);
      d.block_align = static_cast<std::uint32_t>(d.channels) * (d.bits_per_sample / 8u);
    }
    streams_out.push_back(std::move(stream));
  }
  if (streams_out.empty()) return fail(Status::Decode, "the source contains no frames or samples to write");

  // ---------------------------------------------------------------- mux
  std::unique_ptr<Muxer> muxer = make_muxer(format_id);
  if (!muxer) return fail(Status::Internal, "no muxer for '{}'", format_id);
  MuxSettings settings;
  settings.format_id = std::string(format_id);
  if (!options::resolved_strip_metadata(o)) settings.metadata = info.metadata;
  settings.limits = ctx.limits;
  settings.warnings = ctx.warnings;
  std::vector<MuxTrack> descs;
  for (const auto& s : streams_out) descs.push_back(s->desc);
  CVT_TRY(muxer->begin(std::move(descs), settings));

  std::vector<std::optional<ir::Packet>> heads(streams_out.size());
  for (std::size_t i = 0; i < streams_out.size(); ++i) {
    CVT_TRY_ASSIGN(heads[i], streams_out[i]->next());
  }
  const double total_s = info.duration_ms > 0 ? static_cast<double>(info.duration_ms) / 1000.0 : 0.0;
  std::uint64_t written = 0;
  while (true) {
    std::size_t best = streams_out.size();
    long double best_time = 0;
    for (std::size_t i = 0; i < heads.size(); ++i) {
      if (!heads[i]) continue;
      const long double t = static_cast<long double>(heads[i]->dts) / streams_out[i]->desc.timescale;
      if (best == streams_out.size() || t < best_time) {
        best = i;
        best_time = t;
      }
    }
    if (best == streams_out.size()) break;
    CVT_TRY(muxer->write(best, *heads[best]));
    if ((++written & 31) == 0) {
      const double fraction = total_s > 0 ? static_cast<double>(best_time) / total_s : 0.0;
      CVT_TRY(ctx.checkpoint(std::clamp(fraction, 0.0, 0.99), "Encoding"));
    }
    CVT_TRY_ASSIGN(heads[best], streams_out[best]->next());
  }
  for (std::size_t i = 0; i < streams_out.size(); ++i) {
    Bytes late = streams_out[i]->late_extradata();
    if (!late.empty() && late != streams_out[i]->desc.extradata) muxer->set_extradata(i, std::move(late));
  }
  CVT_TRY_ASSIGN(Bytes out, muxer->finish());
  CVT_TRY(ctx.limits.check_output(out.size()));
  return out;
}

}  // namespace cvt::video
