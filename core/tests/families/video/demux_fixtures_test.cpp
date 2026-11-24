// Container readers against real files: every packet's size, payload CRC,
// key flag and timestamps, and every track's codec and configuration, are
// compared with what ffprobe reported for the same file (probe/*.csv; see
// core/tests/data/video/SOURCES.md). Payloads are compared as the demuxer
// found them in the file; the ir::Track the reader returns re-packages
// H.264/HEVC/AAC/AV1 into the canonical form of codec/codec.hpp, which is
// checked separately.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/video/bitstream.hpp"
#include "families/video/common.hpp"
#include "families/video/containers.hpp"
#include "families/video/video_test_util.hpp"

using namespace cvt;
using namespace cvt::test::video;

namespace {

struct Case {
  const char* file;
  const char* format;
  bool pts = true;        // compare presentation timestamps
  bool dts = true;        // compare decode timestamps
  bool key = true;        // compare key-frame flags
  bool extradata = true;  // compare configuration records
  bool payload = true;    // compare packet sizes and CRCs
  bool audio_pts = true;  // compare audio timestamps (off where ffprobe interpolates them its own way)
};

std::string canonical_probe_codec(const std::string& codec) {
  if (codec == "mpeg1video" || codec == "mpeg2video") return codec;
  return codec;
}

// The demuxer's packets exactly as stored in the file.
Result<std::shared_ptr<video::Demuxed>> demux_native(std::string_view format, ByteView data) {
  const video::ReadSettings settings{Limits{}, Progress{}, Warnings{}};
  if (format == "mp4" || format == "mov" || format == "m4v" || format == "3gp" || format == "m4a") return video::demux_mp4(data, settings);
  if (format == "mkv" || format == "webm" || format == "mka") return video::demux_mkv(data, settings);
  if (format == "avi") return video::demux_avi(data, settings);
  if (format == "flv") return video::demux_flv(data, settings);
  if (format == "mpegts") return video::demux_ts(data, settings);
  if (format == "vob" || format == "mpg") return video::demux_ps(data, settings);
  if (format == "ogg" || format == "ogv") return video::demux_ogg(data, settings);
  if (format == "wmv" || format == "wma") return video::demux_asf(data, settings);
  if (format == "ivf") return video::demux_ivf(data, settings);
  if (format == "y4m") return video::demux_y4m(data, settings);
  if (format == "rm") return video::demux_rm(data, settings);
  if (format == "h264" || format == "hevc") return video::demux_raw(data, settings, format, 0.0);
  if (format == "obu") return video::demux_raw(data, settings, "av1", 0.0);
  if (format == "m2v") return video::demux_raw(data, settings, "mpeg2video", 0.0);
  return fail(Status::Internal, "no demuxer for {}", format);
}

// The canonical form: length-prefixed NAL units with a parseable avcC/hvcC,
// AAC without ADTS headers, AV1 without temporal delimiters.
void check_canonical(const ir::Track& track, const std::vector<ir::Packet>& packets) {
  if (track.codec == "h264" || track.codec == "hevc") {
    const bool hevc = track.codec == "hevc";
    const auto record = hevc ? video::bits::parse_hvcc(track.extradata) : video::bits::parse_avcc(track.extradata);
    REQUIRE(record.has_value());
    for (const ir::Packet& p : packets) {
      CHECK(!video::bits::starts_with_start_code(p.bytes));
      CHECK(video::bits::split_length_prefixed(p.bytes, record->length_size).has_value());
    }
  } else if (track.codec == "aac") {
    CHECK(video::bits::parse_asc(track.extradata).has_value());
    for (const ir::Packet& p : packets) CHECK(!video::bits::parse_adts(p.bytes).has_value());
  } else if (track.codec == "av1") {
    for (const ir::Packet& p : packets) {
      REQUIRE(!p.bytes.empty());
      CHECK(((std::to_integer<int>(p.bytes[0]) >> 3) & 0xF) != 2);  // no temporal delimiter
    }
  }
}

void check_case(const Case& c) {
  INFO(c.file);
  const Bytes data = test::fixture(std::string("video/") + c.file);
  REQUIRE(!data.empty());
  auto video = test::read_as<ir::Video>(c.format, data);
  REQUIRE_OK(video);
  const auto streams = probe_streams(c.file);
  const auto packets = probe_packets(c.file);
  auto native = demux_native(c.format, data);
  REQUIRE_OK(native);
  REQUIRE((*native)->tracks.size() == streams.size());
  REQUIRE(video->tracks.size() == streams.size());
  for (std::size_t i = 0; i < streams.size(); ++i) {
    const ProbeStream& s = streams[i];
    const ir::Track& track = video->tracks[i];
    INFO("stream ", i, " ", s.codec);
    CHECK(track.codec == canonical_probe_codec(s.codec));
    CHECK((track.type == ir::TrackType::Video) == (s.type == "video"));
    if (c.extradata && s.extradata_size != 0) {
      CHECK(track.extradata.size() == s.extradata_size);
      CHECK(util::crc32(track.extradata) == s.extradata_crc);
    }
    auto ours = packets_of(track);
    REQUIRE_OK(ours);
    const auto theirs = probe_packets_of(packets, s.index);
    REQUIRE(ours->size() == theirs.size());
    check_canonical(track, *ours);
    const video::TrackInfo& stored = (*native)->tracks[i];
    REQUIRE(stored.packets.size() == theirs.size());
    // ffprobe states timestamps in the stream time base; ours are in the track timescale.
    const auto slash = s.time_base.find('/');
    const std::int64_t tb_num = util::parse_int(s.time_base.substr(0, slash)).value_or(1);
    const std::int64_t tb_den = util::parse_int(s.time_base.substr(slash + 1)).value_or(1);
    const auto to_tb = [&](std::int64_t ticks) {
      const std::int64_t den = static_cast<std::int64_t>(track.timescale) * tb_num;
      const std::int64_t num = ticks * tb_den;
      return (num >= 0 ? num + den / 2 : num - den / 2) / den;
    };
    for (std::size_t k = 0; k < theirs.size(); ++k) {
      INFO("packet ", k);
      const ir::Packet& p = (*ours)[k];
      if (c.payload) {
        const Bytes in_file = (*native)->packet_bytes(stored.packets[k]);
        CHECK(in_file.size() == theirs[k].size);
        CHECK(util::crc32(in_file) == theirs[k].crc);
      }
      if (c.key) {
        CHECK(p.keyframe == theirs[k].key);
      }
      const bool audio = track.type == ir::TrackType::Audio;
      if (c.pts && theirs[k].pts && (c.audio_pts || !audio)) {
        CHECK(to_tb(p.pts) == *theirs[k].pts);
      }
      if (c.dts && theirs[k].dts && (c.audio_pts || !audio)) {
        CHECK(to_tb(p.dts) == *theirs[k].dts);
      }
    }
  }
}

}  // namespace

TEST_CASE("video/mp4: packets, timestamps and configuration match ffprobe") {
  const Case cases[] = {
      {"h264_aac.mp4", "mp4"},
      {"h264_aac_frag.mp4", "mp4"},
      {"h264_aac_large_mdat.mp4", "mp4"},
      {"h264_aac_cmov.mov", "mov"},
      {"hevc.mp4", "mp4"},
      {"av1.mp4", "mp4"},
      {"h264_rot90.mov", "mov"},
      {"mjpeg_pcm.mov", "mov"},
      {"mpeg4_aac.m4v", "m4v"},
      {"h263_aac.3gp", "3gp"},
      {"two_audio.mp4", "mp4"},
      {"aac.m4a", "m4a"},
      {"vfr.mp4", "mp4"},
  };
  for (const Case& c : cases) check_case(c);
}

TEST_CASE("video/mkv: packets, timestamps and configuration match ffprobe") {
  // Matroska stores presentation times only; ffprobe leaves decode times unset.
  const Case cases[] = {
      {"h264_vorbis.mkv", "mkv", true, false},
      {"vp9_opus.webm", "webm", true, false},
      {"vp8_vorbis.webm", "webm", true, false},
      {"av1.webm", "webm", true, false},
      {"mpeg4_flac_pcm.mkv", "mkv", true, false},
      {"mkvmerge_h264.mkv", "mkv", true, false},
      {"mkvmerge_lacing.mka", "mka", true, false},
  };
  for (const Case& c : cases) check_case(c);
}

TEST_CASE("video/raw: IVF, YUV4MPEG2 and elementary streams match ffprobe") {
  const Case cases[] = {
      {"vp8.ivf", "ivf"},
      {"vp9.ivf", "ivf"},
      {"av1.ivf", "ivf", true, true, true, false},  // ffprobe takes the configuration from a parser
      {"i420.y4m", "y4m"},
      {"i444.y4m", "y4m"},
      // Raw streams carry no timestamps and ffprobe extracts the configuration with a parser.
      {"h264.h264", "h264", false, false, true, false},
      {"hevc.h265", "hevc", false, false, true, false},
      {"av1.obu", "obu", false, false, true, false},
      {"mpeg2.m2v", "m2v", false, false, true, false},
  };
  for (const Case& c : cases) check_case(c);
}

TEST_CASE("video/avi: packets, timestamps and configuration match ffprobe") {
  const Case cases[] = {
      {"mpeg4_mp3.avi", "avi", true, true, true, false},  // ffprobe extracts the configuration with a parser
      {"mjpeg_pcm.avi", "avi"},
      {"rawvideo_i420.avi", "avi"},
      {"h264_pcmu8.avi", "avi", true, true, true, false},
  };
  for (const Case& c : cases) check_case(c);
}

TEST_CASE("video/flv: packets, timestamps and configuration match ffprobe") {
  const Case cases[] = {
      {"h264_aac.flv", "flv"},
      {"flv1_mp3.flv", "flv"},
  };
  for (const Case& c : cases) check_case(c);
}

TEST_CASE("video/ts: packets, timestamps and configuration match ffprobe") {
  // ffprobe derives TS configuration records with parsers (TS has none); MPEG audio
  // frames after the first of each PES get timestamps interpolated by ffprobe.
  const Case cases[] = {
      {"h264_aac.ts", "mpegts", true, true, true, false},
      {"mpeg2_mp2.ts", "mpegts", true, true, true, false, true, false},
      {"hevc_ac3.ts", "mpegts", true, true, true, false, true, false},
  };
  for (const Case& c : cases) check_case(c);
}

TEST_CASE("video/mpg: program streams match ffprobe") {
  const Case cases[] = {
      {"mpeg2_mp2.vob", "vob", true, false, true, false, true, false},
      {"mpeg1_mp2.mpg", "mpg", true, false, true, false, true, false},
  };
  for (const Case& c : cases) check_case(c);
}

TEST_CASE("video/ogg: packets, timestamps and configuration match ffprobe") {
  const Case cases[] = {
      {"theora_vorbis.ogv", "ogv", true, true, true, false},  // ffprobe keeps Ogg headers in its own layout
      {"opus.ogg", "ogg"},
      {"flac.oga", "ogg"},
  };
  for (const Case& c : cases) check_case(c);
}

TEST_CASE("video/asf: Windows Media files match ffprobe") {
  const Case cases[] = {
      {"wmv2_wmav2.wmv", "wmv"},
      {"wmav2.wma", "wma"},
  };
  for (const Case& c : cases) check_case(c);
}

TEST_CASE("video/rm: RealMedia files match ffprobe") {
  const Case cases[] = {
      {"rv10_ac3.rm", "rm"},
  };
  for (const Case& c : cases) check_case(c);
}
