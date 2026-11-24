// Convertible core - video family: ISO base media file reader (MP4, MOV, M4V,
// M4A, 3GP): sample tables, fragments, edit lists, composition offsets,
// rotation matrices, 64-bit boxes and compressed QuickTime headers.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <format>
#include <map>
#include <numbers>
#include <optional>
#include <vector>

#include "codec/ids.hpp"
#include "families/video/bitstream.hpp"
#include "families/video/containers.hpp"
#include "util/deflate.hpp"

namespace cvt::video {

namespace {

using bits::fourcc;

struct Box {
  std::uint32_t type = 0;
  std::uint64_t offset = 0;  // absolute position of the box header in the file (or buffer)
  ByteView body;
  std::uint64_t body_offset = 0;
};

// Splits `data` (located at absolute `base`) into boxes. Truncated last boxes
// are clipped to the available data; malformed sizes end the list.
std::vector<Box> boxes_of(ByteView data, std::uint64_t base) {
  std::vector<Box> out;
  util::ByteReader r(data);
  while (r.remaining() >= 8) {
    const std::size_t start = r.position();
    std::uint64_t size = r.u32be();
    const std::uint32_t type = r.u32be();
    std::size_t header = 8;
    if (size == 1) {
      size = r.u64be();
      header = 16;
      if (!r.ok()) break;
    } else if (size == 0) {
      size = data.size() - start;
    }
    if (size < header) break;
    const std::uint64_t available = data.size() - start;
    const std::uint64_t clipped = std::min(size, available);
    Box box;
    box.type = type;
    box.offset = base + start;
    box.body = data.subspan(start + header, static_cast<std::size_t>(clipped - header));
    box.body_offset = base + start + header;
    out.push_back(box);
    if (size > available) break;
    r.seek(start + static_cast<std::size_t>(size));
  }
  return out;
}

const Box* find_box(const std::vector<Box>& list, std::uint32_t type) {
  for (const Box& b : list) {
    if (b.type == type) return &b;
  }
  return nullptr;
}

struct Edit {
  std::int64_t segment_duration = 0;  // movie timescale
  std::int64_t media_time = 0;        // track timescale, -1 = empty
};

struct SampleTables {
  std::vector<std::pair<std::uint32_t, std::uint32_t>> stts;  // count, delta
  std::vector<std::pair<std::uint32_t, std::int32_t>> ctts;   // count, offset
  std::vector<std::uint32_t> stss;
  bool has_stss = false;
  std::uint32_t constant_size = 0;
  std::vector<std::uint32_t> sizes;
  std::uint64_t sample_count = 0;
  struct Stsc {
    std::uint32_t first_chunk;
    std::uint32_t samples;
    std::uint32_t description;
  };
  std::vector<Stsc> stsc;
  std::vector<std::uint64_t> chunk_offsets;
};

struct TrackState {
  TrackInfo info;
  std::uint32_t track_id = 0;
  SampleTables tables;
  std::vector<Edit> edits;
  // fragment defaults (trex)
  std::uint32_t trex_duration = 0;
  std::uint32_t trex_size = 0;
  std::uint32_t trex_flags = 0;
  std::int64_t next_fragment_dts = 0;
  bool grouped_pcm = false;
  std::uint32_t bytes_per_frame = 0;
};

struct Mp4Reader {
  ByteView input;
  const ReadSettings& settings;
  std::shared_ptr<Demuxed> out;
  std::vector<TrackState> tracks;
  std::uint32_t movie_timescale = 1000;
  std::int64_t movie_duration = 0;
  Bytes decompressed_moov;  // keeps a decompressed 'cmov' alive while parsing
  std::uint64_t entry_budget = 0;

  Result<void> check_entries(std::uint64_t count) {
    if (count > entry_budget) return fail(Status::Decode, "MP4 sample table declares more entries than the file can hold");
    entry_budget -= count;
    return {};
  }

  // ------------------------------------------------------------------ sample descriptions

  void parse_esds(ByteView body, TrackInfo& t) {
    util::ByteReader r(body);
    r.skip(4);
    auto read_descriptor = [&](util::ByteReader& rr, std::uint8_t& tag) -> std::optional<util::ByteReader> {
      tag = rr.u8();
      std::uint32_t length = 0;
      for (int i = 0; i < 4; ++i) {
        const std::uint8_t b = rr.u8();
        length = (length << 7) | (b & 0x7F);
        if ((b & 0x80) == 0) break;
      }
      if (!rr.ok() || length > rr.remaining()) return std::nullopt;
      return rr.sub(length);
    };
    std::uint8_t tag = 0;
    auto es = read_descriptor(r, tag);
    if (!es || tag != 3) return;
    es->skip(2);
    const std::uint8_t flags = es->u8();
    if (flags & 0x80) es->skip(2);
    if (flags & 0x40) es->skip(es->u8());
    if (flags & 0x20) es->skip(2);
    auto dc = read_descriptor(*es, tag);
    if (!dc || tag != 4) return;
    const std::uint8_t oti = dc->u8();
    dc->skip(12);
    switch (oti) {
      case 0x40:
      case 0x66:
      case 0x67:
      case 0x68: t.codec = std::string(codec::id::aac); break;
      case 0x69:
      case 0x6B: t.codec = std::string(codec::id::mp3); break;  // refined by the frame header later
      case 0x20: t.codec = std::string(codec::id::mpeg4); break;
      case 0x60:
      case 0x61:
      case 0x62:
      case 0x63:
      case 0x64:
      case 0x65: t.codec = std::string(codec::id::mpeg2video); break;
      case 0x6A: t.codec = std::string(codec::id::mpeg1video); break;
      case 0x6C: t.codec = std::string(codec::id::mjpeg); break;
      case 0xA5: t.codec = std::string(codec::id::ac3); break;
      case 0xDD: t.codec = std::string(codec::id::vorbis); break;
      default: break;
    }
    auto dsi = read_descriptor(*dc, tag);
    if (dsi && tag == 5) {
      const ByteView data = dsi->rest();
      t.extradata.assign(data.begin(), data.end());
    }
  }

  void parse_child_config(const std::vector<Box>& children, TrackInfo& t) {
    for (const Box& c : children) {
      switch (c.type) {
        case fourcc("avcC"):
        case fourcc("hvcC"):
        case fourcc("av1C"):
        case fourcc("glbl"): t.extradata.assign(c.body.begin(), c.body.end()); break;
        case fourcc("esds"): parse_esds(c.body, t); break;
        case fourcc("dOps"):
          if (auto head = bits::dops_to_opus_head(c.body)) {
            t.extradata = std::move(*head);
            t.priming = bits::opus_pre_skip(t.extradata);
          }
          break;
        case fourcc("dfLa"):
          if (c.body.size() >= 4 + 4 + 34) {
            t.extradata.assign(c.body.begin() + 8, c.body.begin() + 8 + 34);
          }
          break;
        case fourcc("alac"):
          if (c.body.size() >= 4) t.extradata.assign(c.body.begin() + 4, c.body.end());
          break;
        case fourcc("wave"): parse_child_config(boxes_of(c.body, c.body_offset), t); break;
        default: break;
      }
    }
  }

  void parse_stsd(ByteView body, std::uint64_t body_offset, std::uint32_t handler, TrackState& ts) {
    TrackInfo& t = ts.info;
    util::ByteReader r(body);
    r.skip(4);
    const std::uint32_t count = r.u32be();
    if (!r.ok() || count == 0) return;
    const auto entries = boxes_of(r.rest(), body_offset + 8);
    if (entries.empty()) return;
    const Box& e = entries.front();
    const std::uint32_t format = e.type;
    t.tag = bits::fourcc_text(format);
    util::ByteReader er(e.body);
    er.skip(8);  // reserved + data reference index
    if (handler == fourcc("vide")) {
      t.type = ir::TrackType::Video;
      er.skip(16);
      t.width = er.u16be();
      t.height = er.u16be();
      er.skip(14);
      er.skip(32);
      const std::uint16_t depth = er.u16be();
      er.skip(2);
      if (!er.ok()) return;
      switch (format) {
        case fourcc("avc1"):
        case fourcc("avc3"): t.codec = std::string(codec::id::h264); break;
        case fourcc("hvc1"):
        case fourcc("hev1"): t.codec = std::string(codec::id::hevc); break;
        case fourcc("av01"): t.codec = std::string(codec::id::av1); break;
        case fourcc("vp09"): t.codec = std::string(codec::id::vp9); break;
        case fourcc("vp08"): t.codec = std::string(codec::id::vp8); break;
        case fourcc("mp4v"): t.codec = std::string(codec::id::mpeg4); break;
        case fourcc("jpeg"):
        case fourcc("mjpa"):
        case fourcc("AVDJ"): t.codec = std::string(codec::id::mjpeg); break;
        case fourcc("s263"):
        case fourcc("h263"):
        case fourcc("H263"): t.codec = std::string(codec::id::h263); break;
        case fourcc("png "): t.codec = std::string(codec::id::png); break;
        case fourcc("apch"):
        case fourcc("apcn"):
        case fourcc("apcs"):
        case fourcc("apco"):
        case fourcc("ap4h"):
        case fourcc("ap4x"): t.codec = std::string(codec::id::prores); break;
        case fourcc("rle "): t.codec = std::string(codec::id::qtrle); break;
        case fourcc("rpza"): t.codec = std::string(codec::id::rpza); break;
        case fourcc("smc "): t.codec = std::string(codec::id::smc); break;
        case fourcc("cvid"): t.codec = std::string(codec::id::cinepak); break;
        case fourcc("2vuy"):
        case fourcc("UYVY"): t.codec = std::string(codec::id::uyvy); break;
        case fourcc("yuvs"):
        case fourcc("YUY2"): t.codec = std::string(codec::id::yuy2); break;
        case fourcc("v210"): t.codec = std::string(codec::id::v210); break;
        case fourcc("r210"): t.codec = std::string(codec::id::r210); break;
        case fourcc("raw "):
          if (depth == 24) {
            t.codec = std::string(codec::id::rawvideo);
            t.extradata = to_bytes(std::string_view("RGB3"));
          }
          break;
        default: break;
      }
      parse_child_config(boxes_of(er.rest(), e.body_offset + er.position()), t);
      if (format == fourcc("raw ") && depth == 24) t.extradata = to_bytes(std::string_view("RGB3"));
      // The sample description depth is the coded depth of the legacy codecs
      // (qtrle 1..32, 33/34/36/40 = grey); see codec_legacy/codec_notes.hpp.
      if (t.codec != codec::id::rawvideo) t.bits_per_sample = depth;
    } else if (handler == fourcc("soun")) {
      t.type = ir::TrackType::Audio;
      const std::uint16_t version = er.u16be();
      er.skip(6);
      t.channels = er.u16be();
      t.bits_per_sample = er.u16be();
      er.skip(4);
      t.sample_rate = er.u32be() >> 16;
      if (version == 1) {
        er.skip(8);
        ts.bytes_per_frame = er.u32be();
        er.skip(4);
      } else if (version == 2) {
        er.skip(4);
        t.sample_rate = static_cast<std::uint32_t>(std::lround(std::clamp(er.f64be(), 0.0, 1e7)));
        t.channels = static_cast<std::uint16_t>(std::min<std::uint32_t>(er.u32be(), 64));
        er.skip(4);
        t.bits_per_sample = static_cast<std::uint16_t>(std::min<std::uint32_t>(er.u32be(), 64));
        const std::uint32_t flags = er.u32be();
        ts.bytes_per_frame = er.u32be();
        er.skip(4);
        if (format == fourcc("lpcm")) {
          const bool is_float = (flags & 1) != 0;
          const bool big = (flags & 2) != 0;
          if (is_float && t.bits_per_sample == 32 && !big) t.codec = std::string(codec::id::pcm_f32le);
          if (!is_float && t.bits_per_sample == 16) t.codec = std::string(big ? codec::id::pcm_s16be : codec::id::pcm_s16le);
          if (!is_float && t.bits_per_sample == 24 && !big) t.codec = std::string(codec::id::pcm_s24le);
          if (!is_float && t.bits_per_sample == 8) t.codec = std::string(codec::id::pcm_u8);
        }
      }
      if (!er.ok()) return;
      switch (format) {
        case fourcc("mp4a"): t.codec = std::string(codec::id::aac); break;
        case fourcc(".mp3"):
        case 0x6D730055u: t.codec = std::string(codec::id::mp3); break;
        case fourcc("Opus"): t.codec = std::string(codec::id::opus); break;
        case fourcc("fLaC"): t.codec = std::string(codec::id::flac); break;
        case fourcc("alac"): t.codec = std::string(codec::id::alac); break;
        case fourcc("ac-3"): t.codec = std::string(codec::id::ac3); break;
        case fourcc("ima4"): t.codec = std::string(codec::id::adpcm_ima_qt); break;
        case fourcc("in24"): t.codec = std::string(codec::id::pcm_s24be); break;
        case fourcc("ec-3"): t.codec = std::string(cid::eac3); break;
        case fourcc("sowt"): t.codec = std::string(codec::id::pcm_s16le); break;
        case fourcc("twos"):
          if (t.bits_per_sample != 8) t.codec = std::string(codec::id::pcm_s16be);
          break;
        case fourcc("raw "): t.codec = std::string(codec::id::pcm_u8); break;
        case fourcc("ulaw"): t.codec = std::string(codec::id::pcm_mulaw); break;
        case fourcc("alaw"): t.codec = std::string(codec::id::pcm_alaw); break;
        case fourcc("samr"): t.codec = std::string(codec::id::amr_nb); break;
        case fourcc("sawb"): t.codec = std::string(codec::id::amr_wb); break;
        default: break;
      }
      const auto children = boxes_of(er.rest(), e.body_offset + er.position());
      parse_child_config(children, t);
      // 'in24'/'fl32' are big-endian unless an 'enda' atom says otherwise.
      if (format == fourcc("in24") || format == fourcc("fl32")) {
        bool little = false;
        for (const Box& c : children) {
          if (c.type == fourcc("wave")) {
            for (const Box& w : boxes_of(c.body, c.body_offset)) {
              if (w.type == fourcc("enda") && w.body.size() >= 2 && std::to_integer<int>(w.body[1]) != 0) little = true;
            }
          }
        }
        if (little) t.codec = std::string(format == fourcc("in24") ? codec::id::pcm_s24le : codec::id::pcm_f32le);
      }
      const bool pcm = t.codec.starts_with("pcm_");
      if (pcm) {
        if (t.codec == codec::id::pcm_u8 || t.codec == codec::id::pcm_alaw || t.codec == codec::id::pcm_mulaw) {
          t.bits_per_sample = 8;
        } else if (t.codec == codec::id::pcm_s24le || t.codec == codec::id::pcm_s24be) {
          t.bits_per_sample = 24;
        } else if (t.codec == codec::id::pcm_f32le) {
          t.bits_per_sample = 32;
        } else {
          t.bits_per_sample = 16;
        }
        const std::uint32_t natural = std::uint32_t{t.channels} * (t.bits_per_sample / 8u);
        if (ts.bytes_per_frame == 0) ts.bytes_per_frame = natural;
        t.block_align = ts.bytes_per_frame;
        ts.grouped_pcm = ts.bytes_per_frame > 0;
      }
      if (t.codec == codec::id::aac && t.extradata.size() >= 2) {
        if (const auto asc = bits::parse_asc(t.extradata)) {
          if (asc->sample_rate != 0) t.sample_rate = asc->sample_rate;
          if (asc->channel_config != 0) t.channels = static_cast<std::uint16_t>(asc->channel_config);
        }
      }
    } else {
      t.type = ir::TrackType::Subtitle;
      if (format == fourcc("tx3g")) t.codec = std::string(cid::mov_text);
      if (format == fourcc("wvtt")) t.codec = std::string(cid::webvtt);
    }
  }

  // ------------------------------------------------------------------ sample tables

  Result<void> parse_stbl(const std::vector<Box>& stbl, std::uint32_t handler, TrackState& ts) {
    SampleTables& st = ts.tables;
    for (const Box& b : stbl) {
      util::ByteReader r(b.body);
      switch (b.type) {
        case fourcc("stsd"): parse_stsd(b.body, b.body_offset, handler, ts); break;
        case fourcc("stts"): {
          r.skip(4);
          const std::uint32_t n = r.u32be();
          if (std::uint64_t{n} * 8 > r.remaining()) return fail(Status::Decode, "MP4 'stts' table is truncated");
          CVT_TRY(check_entries(n));
          st.stts.reserve(n);
          for (std::uint32_t i = 0; i < n; ++i) {
            const std::uint32_t count = r.u32be();
            const std::uint32_t delta = r.u32be();
            st.stts.emplace_back(count, delta);
          }
          break;
        }
        case fourcc("ctts"): {
          r.skip(4);
          const std::uint32_t n = r.u32be();
          if (std::uint64_t{n} * 8 > r.remaining()) return fail(Status::Decode, "MP4 'ctts' table is truncated");
          CVT_TRY(check_entries(n));
          for (std::uint32_t i = 0; i < n; ++i) {
            const std::uint32_t count = r.u32be();
            const auto offset = static_cast<std::int32_t>(r.u32be());
            st.ctts.emplace_back(count, offset);
          }
          break;
        }
        case fourcc("stss"): {
          r.skip(4);
          const std::uint32_t n = r.u32be();
          if (std::uint64_t{n} * 4 > r.remaining()) return fail(Status::Decode, "MP4 'stss' table is truncated");
          CVT_TRY(check_entries(n));
          st.has_stss = true;
          for (std::uint32_t i = 0; i < n; ++i) st.stss.push_back(r.u32be());
          break;
        }
        case fourcc("stsz"): {
          r.skip(4);
          st.constant_size = r.u32be();
          const std::uint32_t n = r.u32be();
          if (!r.ok()) return fail(Status::Decode, "MP4 'stsz' box is truncated");
          st.sample_count = n;
          if (st.constant_size == 0) {
            if (std::uint64_t{n} * 4 > r.remaining()) return fail(Status::Decode, "MP4 'stsz' table is truncated");
            CVT_TRY(check_entries(n));
            st.sizes.reserve(n);
            for (std::uint32_t i = 0; i < n; ++i) st.sizes.push_back(r.u32be());
          }
          break;
        }
        case fourcc("stz2"): {
          r.skip(7);
          const std::uint8_t field = r.u8();
          const std::uint32_t n = r.u32be();
          if (field != 4 && field != 8 && field != 16) return fail(Status::Decode, "MP4 'stz2' field size is invalid");
          if (std::uint64_t{n} * field / 8 > r.remaining()) return fail(Status::Decode, "MP4 'stz2' table is truncated");
          CVT_TRY(check_entries(n));
          st.sample_count = n;
          for (std::uint32_t i = 0; i < n; ++i) {
            if (field == 16) {
              st.sizes.push_back(r.u16be());
            } else if (field == 8) {
              st.sizes.push_back(r.u8());
            } else {
              const std::uint8_t pair = r.u8();
              st.sizes.push_back(pair >> 4);
              if (++i < n) st.sizes.push_back(pair & 0xF);
            }
          }
          break;
        }
        case fourcc("stsc"): {
          r.skip(4);
          const std::uint32_t n = r.u32be();
          if (std::uint64_t{n} * 12 > r.remaining()) return fail(Status::Decode, "MP4 'stsc' table is truncated");
          CVT_TRY(check_entries(n));
          for (std::uint32_t i = 0; i < n; ++i) {
            SampleTables::Stsc e{};
            e.first_chunk = r.u32be();
            e.samples = r.u32be();
            e.description = r.u32be();
            st.stsc.push_back(e);
          }
          break;
        }
        case fourcc("stco"):
        case fourcc("co64"): {
          r.skip(4);
          const std::uint32_t n = r.u32be();
          const std::uint64_t width = b.type == fourcc("co64") ? 8 : 4;
          if (std::uint64_t{n} * width > r.remaining()) return fail(Status::Decode, "MP4 chunk offset table is truncated");
          CVT_TRY(check_entries(n));
          for (std::uint32_t i = 0; i < n; ++i) st.chunk_offsets.push_back(width == 8 ? r.u64be() : r.u32be());
          break;
        }
        default: break;
      }
    }
    return {};
  }

  // Turn the sample tables of a track into packets.
  Result<void> build_samples(TrackState& ts) {
    SampleTables& st = ts.tables;
    TrackInfo& t = ts.info;
    const std::uint64_t count = st.sample_count;
    if (count == 0 || st.chunk_offsets.empty() || st.stsc.empty()) return {};
    if (st.constant_size == 0 && st.sizes.size() < count) return fail(Status::Decode, "MP4 sample size table is short");
    if (st.constant_size != 0) CVT_TRY(check_entries(ts.grouped_pcm ? st.chunk_offsets.size() : count));
    std::size_t stts_i = 0;
    std::uint32_t stts_left = st.stts.empty() ? 0 : st.stts[0].first;
    std::size_t ctts_i = 0;
    std::uint32_t ctts_left = st.ctts.empty() ? 0 : st.ctts[0].first;
    std::size_t stss_i = 0;
    std::int64_t dts = 0;
    std::uint64_t sample = 0;
    bool truncated = false;
    const auto next_delta = [&]() -> std::uint32_t {
      while (stts_i < st.stts.size() && stts_left == 0) {
        if (++stts_i < st.stts.size()) stts_left = st.stts[stts_i].first;
      }
      if (stts_i >= st.stts.size()) return st.stts.empty() ? 0 : st.stts.back().second;
      --stts_left;
      return st.stts[stts_i].second;
    };
    const auto next_offset = [&]() -> std::int32_t {
      if (st.ctts.empty()) return 0;
      while (ctts_i < st.ctts.size() && ctts_left == 0) {
        if (++ctts_i < st.ctts.size()) ctts_left = st.ctts[ctts_i].first;
      }
      if (ctts_i >= st.ctts.size()) return 0;
      --ctts_left;
      return st.ctts[ctts_i].second;
    };
    const std::size_t chunks = st.chunk_offsets.size();
    std::size_t stsc_i = 0;
    for (std::size_t c = 0; c < chunks && sample < count && !truncated; ++c) {
      // samples in this chunk from stsc
      while (stsc_i + 1 < st.stsc.size() && st.stsc[stsc_i + 1].first_chunk <= c + 1) ++stsc_i;
      const std::uint32_t per_chunk = st.stsc[stsc_i].first_chunk <= c + 1 ? st.stsc[stsc_i].samples : 0;
      std::uint64_t offset = st.chunk_offsets[c];
      if ((c & 1023) == 0 && !settings.progress.report(static_cast<double>(sample) / static_cast<double>(count))) {
        return fail(Status::Cancelled, "the conversion was cancelled");
      }
      if (ts.grouped_pcm && st.constant_size != 0) {
        const std::uint64_t n = std::min<std::uint64_t>(per_chunk, count - sample);
        const std::uint64_t size = n * ts.bytes_per_frame;
        std::int64_t duration = 0;
        for (std::uint64_t k = 0; k < n; ++k) duration += next_delta();
        if (!out->add_input_packet(t, offset, size, dts, dts, duration, true)) {
          truncated = true;
          break;
        }
        dts = sat_add(dts, duration);
        sample += n;
        continue;
      }
      for (std::uint32_t k = 0; k < per_chunk && sample < count; ++k) {
        const std::uint32_t size = st.constant_size != 0 ? st.constant_size : st.sizes[static_cast<std::size_t>(sample)];
        const std::uint32_t delta = next_delta();
        const std::int32_t cto = next_offset();
        bool key = true;
        if (st.has_stss) {
          while (stss_i < st.stss.size() && st.stss[stss_i] < sample + 1) ++stss_i;
          key = stss_i < st.stss.size() && st.stss[stss_i] == sample + 1;
        }
        if (!out->add_input_packet(t, offset, size, sat_add(dts, cto), dts, delta, key)) {
          truncated = true;
          break;
        }
        offset += size;
        dts += delta;
        ++sample;
      }
    }
    if (truncated) {
      settings.warnings.add(std::format("the file is truncated: {} of {} samples of {} could be read", sample, count,
                                        track_label(t, 0)));
    }
    ts.next_fragment_dts = dts;
    return {};
  }

  // ------------------------------------------------------------------ boxes

  Result<void> parse_trak(const Box& trak) {
    TrackState ts;
    const auto children = boxes_of(trak.body, trak.body_offset);
    if (const Box* tkhd = find_box(children, fourcc("tkhd"))) {
      util::ByteReader r(tkhd->body);
      const std::uint8_t version = r.u8();
      r.skip(3);
      r.skip(version == 1 ? 16 : 8);
      ts.track_id = r.u32be();
      r.skip(4);
      r.skip(version == 1 ? 8 : 4);
      r.skip(8 + 8);
      const auto a = static_cast<std::int32_t>(r.u32be());
      const auto b = static_cast<std::int32_t>(r.u32be());
      if (r.ok() && (a != 0 || b != 0)) {
        const double angle = std::atan2(static_cast<double>(b), static_cast<double>(a)) * 180.0 / std::numbers::pi;
        int degrees = static_cast<int>(std::lround(angle / 90.0)) * 90;
        degrees = ((degrees % 360) + 360) % 360;
        ts.info.rotation = degrees;
      }
    }
    if (const Box* edts = find_box(children, fourcc("edts"))) {
      for (const Box& e : boxes_of(edts->body, edts->body_offset)) {
        if (e.type != fourcc("elst")) continue;
        util::ByteReader r(e.body);
        const std::uint8_t version = r.u8();
        r.skip(3);
        const std::uint32_t n = r.u32be();
        for (std::uint32_t i = 0; i < n && r.ok() && i < 1024; ++i) {
          Edit edit;
          if (version == 1) {
            edit.segment_duration = static_cast<std::int64_t>(r.u64be());
            edit.media_time = r.i64be();
          } else {
            edit.segment_duration = r.u32be();
            edit.media_time = r.i32be();
          }
          r.skip(4);
          if (r.ok()) ts.edits.push_back(edit);
        }
      }
    }
    const Box* mdia = find_box(children, fourcc("mdia"));
    if (mdia == nullptr) return {};
    const auto mdia_children = boxes_of(mdia->body, mdia->body_offset);
    std::uint32_t handler = 0;
    if (const Box* hdlr = find_box(mdia_children, fourcc("hdlr"))) {
      util::ByteReader r(hdlr->body);
      r.skip(8);
      handler = r.u32be();
    }
    if (const Box* mdhd = find_box(mdia_children, fourcc("mdhd"))) {
      util::ByteReader r(mdhd->body);
      const std::uint8_t version = r.u8();
      r.skip(3);
      r.skip(version == 1 ? 16 : 8);
      ts.info.timescale = r.u32be();
      ts.info.duration = version == 1 ? static_cast<std::int64_t>(r.u64be()) : r.u32be();
      const std::uint16_t lang = r.u16be();
      if (lang != 0 && lang != 0x7FFF && lang >= 0x400) {
        std::string code;
        code.push_back(static_cast<char>(((lang >> 10) & 0x1F) + 0x60));
        code.push_back(static_cast<char>(((lang >> 5) & 0x1F) + 0x60));
        code.push_back(static_cast<char>((lang & 0x1F) + 0x60));
        if (code != "und") ts.info.language = code;
      }
      if (ts.info.duration < 0) ts.info.duration = 0;
    }
    if (ts.info.timescale == 0) return fail(Status::Decode, "MP4 track has a zero timescale");
    if (handler != fourcc("vide") && handler != fourcc("soun") && handler != fourcc("sbtl") &&
        handler != fourcc("text") && handler != fourcc("subt")) {
      return {};  // hint, timecode, metadata tracks
    }
    const Box* minf = find_box(mdia_children, fourcc("minf"));
    if (minf == nullptr) return {};
    const auto minf_children = boxes_of(minf->body, minf->body_offset);
    const Box* stbl = find_box(minf_children, fourcc("stbl"));
    if (stbl == nullptr) return {};
    CVT_TRY(parse_stbl(boxes_of(stbl->body, stbl->body_offset), handler, ts));
    ts.info.id = ts.track_id;
    tracks.push_back(std::move(ts));
    return {};
  }

  void parse_metadata(const Box& udta) {
    static const std::map<std::uint32_t, std::string_view> keys = {
        {0xA96E616Du, "title"},   {0xA9415254u, "artist"},  {0xA9616C62u, "album"}, {0xA9646179u, "date"},
        {0xA9636D74u, "comment"}, {0xA9746F6Fu, "encoder"}, {0xA967656Eu, "genre"}, {fourcc("desc"), "description"}};
    for (const Box& b : boxes_of(udta.body, udta.body_offset)) {
      if (b.type == fourcc("meta")) {
        ByteView body = b.body;
        if (body.size() >= 4 && std::to_integer<int>(body[0]) == 0 && std::to_integer<int>(body[1]) == 0) {
          body = body.subspan(4);
        }
        for (const Box& m : boxes_of(body, 0)) {
          if (m.type != fourcc("ilst")) continue;
          for (const Box& item : boxes_of(m.body, 0)) {
            const auto key = keys.find(item.type);
            if (key == keys.end()) continue;
            for (const Box& data : boxes_of(item.body, 0)) {
              if (data.type != fourcc("data") || data.body.size() < 8) continue;
              out->metadata[std::string(key->second)] = std::string(as_chars(data.body.subspan(8)));
            }
          }
        }
      } else if (const auto key = keys.find(b.type); key != keys.end() && b.body.size() >= 4) {
        util::ByteReader r(b.body);
        const std::uint16_t length = r.u16be();
        r.skip(2);
        const std::string_view text = r.chars(std::min<std::size_t>(length, r.remaining()));
        out->metadata[std::string(key->second)] = std::string(text);
      }
    }
  }

  Result<void> parse_moov(const Box& moov, int depth) {
    if (depth > 2) return fail(Status::Decode, "MP4 movie header is nested too deeply");
    const auto children = boxes_of(moov.body, moov.body_offset);
    if (const Box* cmov = find_box(children, fourcc("cmov"))) {
      const auto parts = boxes_of(cmov->body, 0);
      const Box* dcom = find_box(parts, fourcc("dcom"));
      const Box* cmvd = find_box(parts, fourcc("cmvd"));
      if (dcom == nullptr || cmvd == nullptr || dcom->body.size() < 4 || as_chars(dcom->body.first(4)) != "zlib") {
        return fail(Status::Decode, "QuickTime compressed movie header uses an unsupported compression");
      }
      util::ByteReader r(cmvd->body);
      const std::uint32_t size = r.u32be();
      if (!r.ok()) return fail(Status::Decode, "QuickTime compressed movie header is truncated");
      CVT_TRY(settings.limits.check_decompressed(size));
      CVT_TRY_ASSIGN(decompressed_moov, util::zlib_decompress(r.rest(), std::uint64_t{size} + 16));
      const auto inner = boxes_of(decompressed_moov, 0);
      const Box* real = find_box(inner, fourcc("moov"));
      if (real == nullptr) return fail(Status::Decode, "QuickTime compressed movie header holds no movie");
      return parse_moov(*real, depth + 1);
    }
    for (const Box& b : children) {
      switch (b.type) {
        case fourcc("mvhd"): {
          util::ByteReader r(b.body);
          const std::uint8_t version = r.u8();
          r.skip(3);
          r.skip(version == 1 ? 16 : 8);
          movie_timescale = r.u32be();
          movie_duration = version == 1 ? static_cast<std::int64_t>(r.u64be()) : r.u32be();
          if (movie_timescale == 0) movie_timescale = 1000;
          break;
        }
        case fourcc("trak"): CVT_TRY(parse_trak(b)); break;
        case fourcc("mvex"):
          for (const Box& e : boxes_of(b.body, b.body_offset)) {
            if (e.type != fourcc("trex")) continue;
            util::ByteReader r(e.body);
            r.skip(4);
            const std::uint32_t id = r.u32be();
            r.skip(4);
            const std::uint32_t duration = r.u32be();
            const std::uint32_t size = r.u32be();
            const std::uint32_t flags = r.u32be();
            for (TrackState& ts : tracks) {
              if (ts.track_id == id) {
                ts.trex_duration = duration;
                ts.trex_size = size;
                ts.trex_flags = flags;
              }
            }
          }
          break;
        case fourcc("udta"): parse_metadata(b); break;
        default: break;
      }
    }
    return {};
  }

  Result<void> parse_moof(const Box& moof) {
    const auto children = boxes_of(moof.body, moof.body_offset);
    for (const Box& traf : children) {
      if (traf.type != fourcc("traf")) continue;
      const auto parts = boxes_of(traf.body, traf.body_offset);
      const Box* tfhd = find_box(parts, fourcc("tfhd"));
      if (tfhd == nullptr) continue;
      util::ByteReader hr(tfhd->body);
      hr.skip(1);
      const std::uint32_t flags = hr.u24be();
      const std::uint32_t id = hr.u32be();
      TrackState* ts = nullptr;
      for (TrackState& candidate : tracks) {
        if (candidate.track_id == id) ts = &candidate;
      }
      if (ts == nullptr || !hr.ok()) continue;
      std::uint64_t base = moof.offset;
      if (flags & 0x1) base = hr.u64be();
      if (flags & 0x2) hr.skip(4);
      std::uint32_t default_duration = ts->trex_duration;
      std::uint32_t default_size = ts->trex_size;
      std::uint32_t default_flags = ts->trex_flags;
      if (flags & 0x8) default_duration = hr.u32be();
      if (flags & 0x10) default_size = hr.u32be();
      if (flags & 0x20) default_flags = hr.u32be();
      std::int64_t dts = ts->next_fragment_dts;
      if (const Box* tfdt = find_box(parts, fourcc("tfdt"))) {
        util::ByteReader r(tfdt->body);
        const std::uint8_t version = r.u8();
        r.skip(3);
        const std::uint64_t t = version == 1 ? r.u64be() : r.u32be();
        if (r.ok()) dts = clamp_timestamp(static_cast<std::int64_t>(t & 0x7FFFFFFFFFFFFFFFull));
      }
      std::uint64_t data_offset = base;
      for (const Box& trun : parts) {
        if (trun.type != fourcc("trun")) continue;
        util::ByteReader r(trun.body);
        const std::uint8_t version = r.u8();
        const std::uint32_t tflags = r.u24be();
        const std::uint32_t n = r.u32be();
        if (!r.ok()) return fail(Status::Decode, "MP4 'trun' box is truncated");
        CVT_TRY(check_entries(n));
        if (tflags & 0x1) data_offset = base + static_cast<std::uint64_t>(static_cast<std::int64_t>(r.i32be()));
        std::uint32_t first_flags = default_flags;
        const bool has_first = (tflags & 0x4) != 0;
        if (has_first) first_flags = r.u32be();
        for (std::uint32_t i = 0; i < n; ++i) {
          const std::uint32_t duration = (tflags & 0x100) ? r.u32be() : default_duration;
          const std::uint32_t size = (tflags & 0x200) ? r.u32be() : default_size;
          std::uint32_t sflags = (tflags & 0x400) ? r.u32be() : (i == 0 && has_first ? first_flags : default_flags);
          if (i == 0 && has_first && !(tflags & 0x400)) sflags = first_flags;
          std::int64_t cto = 0;
          if (tflags & 0x800) cto = version == 0 ? static_cast<std::int64_t>(r.u32be()) : r.i32be();
          if (!r.ok()) return fail(Status::Decode, "MP4 'trun' box is truncated");
          const bool key = ts->info.type != ir::TrackType::Video || (sflags & 0x10000) == 0;
          if (!out->add_input_packet(ts->info, data_offset, size, sat_add(dts, cto), dts, duration, key)) {
            settings.warnings.add("the file is truncated: the last fragment could not be read completely");
            return {};
          }
          data_offset += size;
          dts = sat_add(dts, duration);
        }
      }
      ts->next_fragment_dts = dts;
    }
    return {};
  }

  void apply_edits(TrackState& ts) {
    if (ts.edits.empty()) return;
    std::int64_t empty = 0;
    std::size_t i = 0;
    while (i < ts.edits.size() && ts.edits[i].media_time == -1) {
      empty = clamp_timestamp(sat_add(empty, ts.edits[i].segment_duration));
      ++i;
    }
    if (i >= ts.edits.size()) return;
    std::size_t non_empty = 0;
    for (std::size_t k = i; k < ts.edits.size(); ++k) {
      if (ts.edits[k].media_time != -1) ++non_empty;
    }
    if (non_empty > 1) settings.warnings.add("the file has a multi-part edit list; only its first part is honoured");
    const std::int64_t shift = sat_add(rescale(clamp_timestamp(empty), movie_timescale, ts.info.timescale), -clamp_timestamp(ts.edits[i].media_time));
    if (shift == 0) return;
    for (PacketRef& p : ts.info.packets) {
      p.pts = clamp_timestamp(sat_add(p.pts, shift));
      p.dts = clamp_timestamp(sat_add(p.dts, shift));
    }
    if (ts.info.type == ir::TrackType::Audio && ts.edits[i].media_time > 0 && empty == 0) {
      ts.info.priming = ts.info.sample_rate != 0
                            ? rescale(ts.edits[i].media_time, ts.info.timescale, ts.info.sample_rate)
                            : ts.edits[i].media_time;
    }
  }
};

}  // namespace

Result<std::shared_ptr<Demuxed>> demux_mp4(ByteView input, const ReadSettings& settings) {
  Mp4Reader reader{input, settings, std::make_shared<Demuxed>(), {}, 1000, 0, {}, 0};
  reader.out->input = input;
  reader.out->format_id = "mp4";
  reader.entry_budget = input.size() + 4096;
  const auto top = boxes_of(input, 0);
  if (top.empty()) return fail(Status::Decode, "not an MP4/QuickTime file (no boxes)");
  bool known = false;
  for (const Box& b : top) {
    if (b.type == fourcc("ftyp") || b.type == fourcc("moov") || b.type == fourcc("mdat") || b.type == fourcc("free") ||
        b.type == fourcc("wide") || b.type == fourcc("skip") || b.type == fourcc("moof") || b.type == fourcc("styp")) {
      known = true;
    }
  }
  if (!known) return fail(Status::Decode, "not an MP4/QuickTime file");
  const Box* moov = find_box(top, fourcc("moov"));
  if (moov == nullptr) return fail(Status::Decode, "the MP4 file has no movie header ('moov'); it is truncated or incomplete");
  CVT_TRY(reader.parse_moov(*moov, 0));
  for (TrackState& ts : reader.tracks) CVT_TRY(reader.build_samples(ts));
  for (const Box& b : top) {
    if (b.type == fourcc("moof")) CVT_TRY(reader.parse_moof(b));
  }
  Demuxed& out = *reader.out;
  for (TrackState& ts : reader.tracks) {
    reader.apply_edits(ts);
    TrackInfo& t = ts.info;
    // MPEG audio in 'mp4a': tell layer II from layer III by the first frame.
    if (t.codec == codec::id::mp3 && !t.packets.empty()) {
      const Bytes first = out.packet_bytes(t.packets.front());
      if (const auto h = bits::parse_mpeg_audio(first); h && h->layer == 2) t.codec = std::string(codec::id::mp2);
    }
    if (t.codec == codec::id::opus && t.sample_rate == 0) t.sample_rate = 48000;
    if (t.type == ir::TrackType::Video && t.packets.size() > 1) {
      std::int64_t total = 0;
      for (const PacketRef& p : t.packets) total += p.duration;
      const std::int64_t d = t.packets.front().duration;
      if (d > 0 && std::ranges::all_of(t.packets, [d](const PacketRef& p) { return p.duration == d; })) {
        t.fps = ir::Rational{t.timescale, d};
      } else if (total > 0) {
        t.fps = {};  // variable frame rate
      }
    }
    out.tracks.push_back(std::move(t));
  }
  if (reader.movie_duration > 0) out.duration_ms = rescale(reader.movie_duration, reader.movie_timescale, 1000);
  CVT_TRY(finish_demux(out, "MP4"));
  return reader.out;
}

}  // namespace cvt::video
