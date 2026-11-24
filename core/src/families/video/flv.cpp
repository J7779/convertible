// Convertible core - video family: Flash Video (FLV) reader and writer,
// including the enhanced-RTMP FourCC video tags (HEVC, AV1, VP9).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <format>

#include "codec/ids.hpp"
#include "families/video/adapt.hpp"
#include "families/video/bitstream.hpp"
#include "families/video/containers.hpp"

namespace cvt::video {

namespace {
namespace id = codec::id;
using bits::fourcc;
}  // namespace

Result<std::shared_ptr<Demuxed>> demux_flv(ByteView input, const ReadSettings& settings) {
  util::ByteReader r(input);
  if (!r.consume("FLV")) return fail(Status::Decode, "not an FLV file");
  r.skip(2);
  const std::uint32_t header = r.u32be();
  if (!r.ok() || header < 9 || header > input.size()) return fail(Status::Decode, "FLV header is truncated");
  auto out = std::make_shared<Demuxed>();
  out->input = input;
  out->format_id = "flv";
  std::optional<std::size_t> audio;
  std::optional<std::size_t> video;
  std::vector<TrackInfo>& tracks = out->tracks;
  r.seek(header);
  r.skip(4);  // PreviousTagSize0
  std::uint64_t tags = 0;
  while (r.remaining() >= 11) {
    const std::size_t start = r.position();
    const std::uint8_t type = r.u8() & 0x1F;
    const std::uint32_t size = r.u24be();
    const std::uint32_t ts_low = r.u24be();
    const std::uint32_t ts_high = r.u8();
    r.skip(3);
    const std::int64_t ts = static_cast<std::int64_t>((ts_high << 24) | ts_low);
    if (size > r.remaining()) {
      settings.warnings.add("the FLV file is truncated; the last tag was dropped");
      break;
    }
    const std::size_t body = r.position();
    util::ByteReader t(input.subspan(body, size));
    if (type == 8 && size >= 1) {
      const std::uint8_t flags = t.u8();
      const int format = flags >> 4;
      if (!audio) {
        TrackInfo info;
        info.type = ir::TrackType::Audio;
        info.timescale = 1000;
        static constexpr std::uint32_t rates[] = {5512, 11025, 22050, 44100};
        info.sample_rate = rates[(flags >> 2) & 3];
        info.channels = (flags & 1) ? 2 : 1;
        info.bits_per_sample = (flags & 2) ? 16 : 8;
        info.tag = std::format("flv audio {}", format);
        switch (format) {
          case 0:
          case 3: info.codec = std::string(info.bits_per_sample == 16 ? id::pcm_s16le : id::pcm_u8); break;
          case 2:
          case 14: info.codec = std::string(id::mp3); break;
          case 7: info.codec = std::string(id::pcm_alaw); break;
          case 8: info.codec = std::string(id::pcm_mulaw); break;
          case 10: info.codec = std::string(id::aac); break;
          case 11: info.codec = std::string(id::speex); info.sample_rate = 16000; break;
          default: break;
        }
        if (format == 14) info.sample_rate = 8000;
        audio = tracks.size();
        tracks.push_back(std::move(info));
      }
      TrackInfo& a = tracks[*audio];
      if (format == 10) {
        const std::uint8_t packet_type = t.u8();
        if (!t.ok()) break;
        if (packet_type == 0) {
          const ByteView asc = t.rest();
          a.extradata.assign(asc.begin(), asc.end());
          if (const auto c = bits::parse_asc(asc)) {
            a.sample_rate = c->sample_rate;
            a.channels = static_cast<std::uint16_t>(c->channel_config);
          }
        } else {
          out->add_input_packet(a, body + 2, size - 2, ts, ts, 0, true);
        }
      } else if (size > 1) {
        if (format == 2 && a.packets.empty()) {
          if (const auto h = bits::parse_mpeg_audio(input.subspan(body + 1, size - 1))) {
            a.sample_rate = h->sample_rate;
            a.channels = static_cast<std::uint16_t>(h->channels);
          }
        }
        out->add_input_packet(a, body + 1, size - 1, ts, ts, 0, true);
      }
    } else if (type == 9 && size >= 1) {
      const std::uint8_t flags = t.u8();
      const bool enhanced = (flags & 0x80) != 0;
      const int frame_type = enhanced ? (flags >> 4) & 7 : flags >> 4;
      std::string codec;
      std::string tag;
      int packet_type = 1;
      std::int64_t cts = 0;
      if (enhanced) {
        packet_type = flags & 0xF;
        const std::uint32_t f = t.u32be();
        tag = bits::fourcc_text(f);
        if (f == fourcc("hvc1")) codec = std::string(id::hevc);
        if (f == fourcc("av01")) codec = std::string(id::av1);
        if (f == fourcc("vp09")) codec = std::string(id::vp9);
        if (f == fourcc("avc1")) codec = std::string(id::h264);
        if (packet_type == 1 && (codec == id::hevc || codec == id::h264)) {
          const std::uint32_t raw = t.u24be();
          cts = (raw & 0x800000) ? static_cast<std::int64_t>(raw) - 0x1000000 : raw;
        }
        if (packet_type == 3) packet_type = 1;
      } else {
        const int codec_id = flags & 0xF;
        tag = std::format("flv video {}", codec_id);
        if (codec_id == 2) codec = std::string(cid::flv1);
        if (codec_id == 4) codec = std::string(cid::vp6f);
        if (codec_id == 7) codec = std::string(id::h264);
        if (codec_id == 12) codec = std::string(id::hevc);
        if (codec_id == 7 || codec_id == 12) {
          packet_type = t.u8();
          const std::uint32_t raw = t.u24be();
          cts = (raw & 0x800000) ? static_cast<std::int64_t>(raw) - 0x1000000 : raw;
        }
        if (codec_id == 4) t.skip(1);  // VP6 adjustment byte
      }
      if (!t.ok()) break;
      if (!video) {
        TrackInfo info;
        info.type = ir::TrackType::Video;
        info.timescale = 1000;
        info.codec = codec;
        info.tag = tag;
        video = tracks.size();
        tracks.push_back(std::move(info));
      }
      TrackInfo& v = tracks[*video];
      const std::size_t at = body + t.position();
      const std::size_t length = size - t.position();
      if (packet_type == 0) {
        v.extradata.assign(input.begin() + static_cast<std::ptrdiff_t>(at), input.begin() + static_cast<std::ptrdiff_t>(at + length));
      } else if (packet_type == 1 && frame_type != 5 && length > 0) {
        out->add_input_packet(v, at, length, ts + cts, ts, 0, frame_type == 1);
      }
    } else if (type == 18) {
      // onMetaData: nothing that the streams do not tell us better.
    }
    r.seek(body + size);
    r.skip(4);
    if ((++tags & 1023) == 0 && !settings.progress.report(static_cast<double>(start) / static_cast<double>(input.size()))) {
      return fail(Status::Cancelled, "the conversion was cancelled");
    }
  }
  for (TrackInfo& t : tracks) {
    if (t.type == ir::TrackType::Video && t.codec == cid::flv1 && !t.packets.empty()) {
      // Sorenson H.263 picture header: picture size.
      const Bytes first = out->packet_bytes(t.packets.front());
      bits::BitReader b(first);
      b.bits(17);
      b.bits(5);
      b.bits(8);
      const std::uint32_t format = b.bits(3);
      static constexpr std::uint32_t sizes[][2] = {{0, 0}, {0, 0}, {352, 288}, {176, 144}, {128, 96}, {320, 240}, {160, 120}, {0, 0}};
      if (format == 0) {
        t.width = b.bits(8);
        t.height = b.bits(8);
      } else if (format == 1) {
        t.width = b.bits(16);
        t.height = b.bits(16);
      } else {
        t.width = sizes[format][0];
        t.height = sizes[format][1];
      }
    }
  }
  CVT_TRY(finish_demux(*out, "FLV"));
  return out;
}

// =========================================================================== writer

namespace {

void amf_string(util::ByteWriter& w, std::string_view s) {
  w.u16be(static_cast<std::uint16_t>(s.size())).text(s);
}
void amf_number(util::ByteWriter& w, std::string_view key, double value) {
  amf_string(w, key);
  w.u8(0).f64be(value);
}

class FlvMuxer final : public Muxer {
 public:
  Result<void> begin(std::vector<MuxTrack> tracks, const MuxSettings& settings) override {
    settings_ = settings;
    for (MuxTrack& t : tracks) {
      if (!container_accepts("flv", t.type, t.codec)) return fail(Status::Encode, "FLV cannot store {}", t.codec);
      Out o;
      o.adapter = StreamAdapter(t.codec, t.extradata, Packaging{});
      o.desc = std::move(t);
      if (o.desc.type == ir::TrackType::Video) {
        if (video_) return fail(Status::Encode, "FLV holds one video stream");
        video_ = tracks_.size();
      } else {
        if (audio_) return fail(Status::Encode, "FLV holds one audio stream");
        audio_ = tracks_.size();
      }
      tracks_.push_back(std::move(o));
    }
    return {};
  }

  Result<void> write(std::size_t index, const ir::Packet& packet) override {
    Out& t = tracks_[index];
    CVT_TRY_ASSIGN(Bytes data, t.adapter.convert(packet.bytes, packet.keyframe));
    if (!started_) {
      // One shift for all streams keeps them in sync while making times non-negative.
      started_ = true;
      shift_ = std::max<std::int64_t>(0, -rescale(packet.dts, t.desc.timescale, 1000));
    }
    const std::int64_t dts = rescale(packet.dts, t.desc.timescale, 1000) + shift_;
    const std::int64_t pts = rescale(packet.pts, t.desc.timescale, 1000) + shift_;
    if (dts < 0) return {};
    duration_ = std::max(duration_, pts + rescale(packet.duration, t.desc.timescale, 1000));
    util::ByteWriter body;
    if (t.desc.type == ir::TrackType::Video) {
      if (t.desc.codec == id::h264) {
        if (!t.config_written) {
          const Bytes config = t.adapter.extradata();
          if (config.empty()) return fail(Status::Encode, "the H.264 stream carries no parameter sets");
          util::ByteWriter seq;
          seq.u8(0x17).u8(0).u24be(0).bytes(config);
          tag(9, 0, seq.view());
          t.config_written = true;
        }
        body.u8(packet.keyframe ? 0x17 : 0x27).u8(1).u24be(static_cast<std::uint32_t>(pts - dts) & 0xFFFFFF);
      } else {
        body.u8(packet.keyframe ? 0x12 : 0x22);
      }
    } else {
      const std::uint32_t rate = t.desc.sample_rate;
      const std::uint8_t rate_bits = rate >= 44100 ? 3 : rate >= 22050 ? 2 : rate >= 11025 ? 1 : 0;
      const std::uint8_t stereo = t.desc.channels >= 2 ? 1 : 0;
      std::uint8_t format = 2;
      if (t.desc.codec == id::aac) format = 10;
      if (t.desc.codec == id::pcm_s16le) format = 3;
      if (t.desc.codec == id::pcm_alaw) format = 7;
      if (t.desc.codec == id::pcm_mulaw) format = 8;
      if (t.desc.codec == id::speex) format = 11;
      const std::uint8_t size_bit = (t.desc.codec == id::pcm_alaw || t.desc.codec == id::pcm_mulaw) ? 0 : 1;
      const std::uint8_t flags = static_cast<std::uint8_t>((format << 4) | ((format == 10 ? 3 : rate_bits) << 2) |
                                                           (size_bit << 1) | (format == 10 ? 1 : stereo));
      if (format == 10) {
        if (!t.config_written) {
          const Bytes config = t.adapter.extradata();
          if (config.empty()) return fail(Status::Encode, "the AAC stream has no configuration");
          util::ByteWriter seq;
          seq.u8(flags).u8(0).bytes(config);
          tag(8, 0, seq.view());
          t.config_written = true;
        }
        body.u8(flags).u8(1);
      } else {
        body.u8(flags);
      }
    }
    body.bytes(data);
    tag(t.desc.type == ir::TrackType::Video ? 9 : 8, dts, body.view());
    if (tags_.size() > settings_.limits.max_output_bytes) return fail(Status::Limit, "the FLV output exceeds the output size limit");
    return {};
  }

  void set_extradata(std::size_t track, Bytes extradata) override {
    tracks_[track].adapter = StreamAdapter(tracks_[track].desc.codec, std::move(extradata), Packaging{});
  }

  Result<Bytes> finish() override {
    util::ByteWriter out;
    out.text("FLV").u8(1).u8(static_cast<std::uint8_t>((audio_ ? 4 : 0) | (video_ ? 1 : 0))).u32be(9).u32be(0);
    util::ByteWriter meta;
    meta.u8(2);
    amf_string(meta, "onMetaData");
    meta.u8(8).u32be(0);
    amf_number(meta, "duration", static_cast<double>(duration_) / 1000.0);
    if (video_) {
      const MuxTrack& v = tracks_[*video_].desc;
      amf_number(meta, "width", v.width);
      amf_number(meta, "height", v.height);
      if (v.fps.num > 0) amf_number(meta, "framerate", v.fps.value());
      amf_number(meta, "videocodecid", v.codec == id::h264 ? 7 : 2);
    }
    if (audio_) {
      const MuxTrack& a = tracks_[*audio_].desc;
      amf_number(meta, "audiosamplerate", a.sample_rate);
      amf_number(meta, "audiocodecid", a.codec == id::aac ? 10 : a.codec == id::mp3 ? 2 : 3);
    }
    amf_string(meta, "");
    meta.u8(9);
    util::ByteWriter head;
    head.u8(18).u24be(static_cast<std::uint32_t>(meta.size())).u24be(0).u8(0).u24be(0).bytes(meta.view());
    head.u32be(static_cast<std::uint32_t>(meta.size() + 11));
    out.bytes(head.view());
    out.bytes(tags_.view());
    CVT_TRY(settings_.limits.check_output(out.size()));
    return out.take();
  }

 private:
  struct Out {
    MuxTrack desc;
    StreamAdapter adapter;
    bool config_written = false;
  };

  void tag(std::uint8_t type, std::int64_t ts, ByteView body) {
    const auto t = static_cast<std::uint32_t>(std::max<std::int64_t>(ts, 0));
    tags_.u8(type).u24be(static_cast<std::uint32_t>(body.size())).u24be(t & 0xFFFFFF).u8(static_cast<std::uint8_t>(t >> 24)).u24be(0);
    tags_.bytes(body);
    tags_.u32be(static_cast<std::uint32_t>(body.size() + 11));
  }

  MuxSettings settings_;
  std::vector<Out> tracks_;
  std::optional<std::size_t> video_;
  std::optional<std::size_t> audio_;
  util::ByteWriter tags_;
  std::int64_t duration_ = 0;
  bool started_ = false;
  std::int64_t shift_ = 0;
};

}  // namespace

std::unique_ptr<Muxer> make_flv_muxer() { return std::make_unique<FlvMuxer>(); }

}  // namespace cvt::video
