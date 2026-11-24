// Convertible core - audio_lossy family: AAC streams in ADTS and MPEG-4
// files: demultiplexing (sample tables, edit lists, iTunSMPB gapless data,
// iTunes metadata), decoding with trimming, encoding, and a minimal .m4a
// multiplexer (moov first, edit list and iTunSMPB for gapless playback).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <string_view>

#include "util/bytes.hpp"
#include "families/audio_lossless/bits.hpp"
#include "families/audio_lossless/common.hpp"
#include "families/audio_lossless/mp4_boxes.hpp"
#include "families/audio_lossless/tags.hpp"
#include "families/audio_lossy/aac/aac.hpp"
#include "util/text.hpp"

namespace cvt::lossy::aac {

using audio::Box;
using audio::box;
using audio::find_box;
using audio::find_path;
using audio::full_box;
using audio::parse_boxes;

// =========================================================================== decoding

Result<ir::Audio> decode_track(const Track& track, const Context& ctx) {
  Decoder decoder;
  CVT_TRY(decoder.configure(track.config));
  ir::Audio audio;
  std::size_t errors = 0;
  for (std::size_t i = 0; i < track.units.size(); ++i) {
    if ((i & 63) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(track.units.size()), "Decoding"));
    if (!decoder.decode(track.units[i], audio.samples)) ++errors;
    CVT_TRY(audio::check_sample_budget(ctx, audio.samples.size()));
  }
  if (errors * 2 > track.units.size() && errors > 0) return fail(Status::Decode, "the AAC stream is too damaged to decode");
  if (errors > 0) ctx.warn("Damaged AAC frames were replaced by silence.");
  audio.sample_rate = decoder.sample_rate();
  audio.channels = static_cast<std::uint16_t>(decoder.channels());
  CVT_TRY(audio::check_stream_params(audio.sample_rate, audio.channels, "the AAC stream"));
  const std::size_t frames = audio.frame_count();
  const auto skip = static_cast<std::size_t>(std::min<std::uint64_t>(track.priming + decoder.extra_delay(), frames));
  audio.samples.erase(audio.samples.begin(), audio.samples.begin() + static_cast<std::ptrdiff_t>(skip * audio.channels));
  if (track.valid != 0) audio::set_frame_count(audio, std::min<std::uint64_t>(track.valid, audio.frame_count()));
  audio.tags = track.tags;
  return audio;
}

// =========================================================================== ADTS

Result<Track> read_adts(ByteView data, const Context& ctx) {
  const auto d = as_u8(data);
  Track track;
  std::size_t pos = 0;
  // Skip a leading ID3v2 tag.
  if (d.size() >= 10 && ::cvt::util::compare_bytes(d.data(), "ID3", 3) == 0) {
    pos = 10 + ((std::size_t{d[6] & 0x7Fu} << 21) | (std::size_t{d[7] & 0x7Fu} << 14) | (std::size_t{d[8] & 0x7Fu} << 7) |
                (d[9] & 0x7Fu));
    audio::tags::parse_id3v2(data.first(std::min(pos, d.size())), track.tags);
  }
  bool have_config = false;
  std::uint32_t key = 0;
  std::size_t frames = 0;
  while (pos + 7 <= d.size()) {
    if (d[pos] != 0xFF || (d[pos + 1] & 0xF6) != 0xF0) {
      ++pos;
      continue;
    }
    const bool crc = (d[pos + 1] & 1) == 0;
    const int profile = d[pos + 2] >> 6;
    const int sfi = (d[pos + 2] >> 2) & 15;
    const int channels = ((d[pos + 2] & 1) << 2) | (d[pos + 3] >> 6);
    const std::size_t length = (std::size_t{d[pos + 3] & 3u} << 11) | (std::size_t{d[pos + 4]} << 3) | (d[pos + 5] >> 5);
    const int blocks = (d[pos + 6] & 3) + 1;
    const std::size_t header = crc ? 9 : 7;
    const std::uint32_t this_key = static_cast<std::uint32_t>((profile << 8) | (sfi << 4) | channels);
    if (sfi > 12 || length < header || pos + length > d.size() || (have_config && this_key != key)) {
      ++pos;
      continue;
    }
    // Confirm the first frame with the next one.
    if (!have_config && pos + length + 2 <= d.size() &&
        (d[pos + length] != 0xFF || (d[pos + length + 1] & 0xF6) != 0xF0)) {
      ++pos;
      continue;
    }
    if (!have_config) {
      have_config = true;
      key = this_key;
      audio::BitWriter w;
      w.write(static_cast<std::uint32_t>(profile + 1), 5);
      w.write(static_cast<std::uint32_t>(sfi), 4);
      w.write(static_cast<std::uint32_t>(channels), 4);
      w.write(0, 3);
      const auto bytes = w.take();
      track.config = to_bytes(std::span<const std::uint8_t>(bytes));
    }
    if (blocks == 1) {
      track.units.push_back(data.subspan(pos + header, length - header));
    } else {
      // Several raw data blocks in one frame: hand them over together; the
      // decoder reads one block per call, so split evenly is not possible.
      // Such streams are rare; decode the first block only.
      track.units.push_back(data.subspan(pos + header, length - header));
    }
    pos += length;
    if ((++frames & 1023) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(pos) / static_cast<double>(d.size()), "Reading"));
  }
  if (!have_config || track.units.empty()) return fail(Status::Decode, "no ADTS frames were found");
  return track;
}

Bytes adts_header(const AudioConfig& config, std::size_t payload) {
  const std::size_t length = payload + 7;
  const int sfi = rate_index(config.sample_rate);
  std::array<std::uint8_t, 7> h{};
  h[0] = 0xFF;
  h[1] = 0xF1;  // MPEG-4, no CRC
  h[2] = static_cast<std::uint8_t>(((config.object_type - 1) << 6) | (sfi << 2) | ((config.channel_config >> 2) & 1));
  h[3] = static_cast<std::uint8_t>(((config.channel_config & 3) << 6) | ((length >> 11) & 3));
  h[4] = static_cast<std::uint8_t>((length >> 3) & 0xFF);
  h[5] = static_cast<std::uint8_t>(((length & 7) << 5) | 0x1F);
  h[6] = 0xFC;
  return to_bytes(std::span<const std::uint8_t>(h));
}

// =========================================================================== encoding

Result<Encoded> encode_audio(const ir::Audio& audio, std::uint32_t bitrate, const Context& ctx) {
  Encoder encoder;
  CVT_TRY(encoder.configure(audio.sample_rate, audio.channels, bitrate));
  Encoded out;
  out.config = encoder.config();
  out.priming = Encoder::delay();
  out.frames = audio.frame_count();
  out.sample_rate = audio.sample_rate;
  out.channels = audio.channels;
  // The encoder outputs the first frame after one frame of look-ahead:
  // feed priming + input, padded, plus one frame to flush.
  const std::uint64_t total = out.frames + out.priming;
  const std::uint64_t units = (total + 1023) / 1024 + 1;
  const std::size_t ch = audio.channels;
  std::vector<float> frame(1024 * ch);
  std::uint64_t bytes = 0;
  for (std::uint64_t u = 0; u < units; ++u) {
    if ((u & 63) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(u) / static_cast<double>(units), "Encoding"));
    std::fill(frame.begin(), frame.end(), 0.0f);
    const std::uint64_t first = u * 1024;
    if (first < out.frames) {
      const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(1024, out.frames - first));
      std::copy_n(audio.samples.begin() + static_cast<std::ptrdiff_t>(first * ch), n * ch, frame.begin());
    }
    CVT_TRY_ASSIGN(Bytes unit, encoder.encode(frame.data()));
    bytes += unit.size();
    CVT_TRY(ctx.limits.check_output(bytes));
    out.units.push_back(std::move(unit));
  }
  return out;
}

// =========================================================================== MPEG-4 reading

namespace {

std::uint32_t be32(std::span<const std::uint8_t> d, std::size_t at) {
  return (std::uint32_t{d[at]} << 24) | (std::uint32_t{d[at + 1]} << 16) | (std::uint32_t{d[at + 2]} << 8) | d[at + 3];
}

/// The DecoderSpecificInfo inside an 'esds' box body.
std::optional<std::pair<int, Bytes>> parse_esds(ByteView body) {
  util::ByteReader r(body);
  r.skip(4);  // version/flags
  const auto length = [&r]() {
    std::uint32_t n = 0;
    for (int i = 0; i < 4; ++i) {
      const std::uint8_t b = r.u8();
      n = (n << 7) | (b & 0x7Fu);
      if ((b & 0x80) == 0) break;
    }
    return n;
  };
  if (r.u8() != 0x03) return std::nullopt;
  length();
  r.skip(2);
  const std::uint8_t flags = r.u8();
  if ((flags & 0x80) != 0) r.skip(2);
  if ((flags & 0x40) != 0) r.skip(r.u8());
  if ((flags & 0x20) != 0) r.skip(2);
  if (r.u8() != 0x04) return std::nullopt;
  length();
  const int object_type = r.u8();
  r.skip(12);
  if (!r.ok()) return std::nullopt;
  Bytes specific;
  if (r.u8() == 0x05) {
    const std::uint32_t n = length();
    if (n <= r.remaining()) {
      const ByteView v = r.bytes(n);
      specific.assign(v.begin(), v.end());
    }
  }
  return std::make_pair(object_type, std::move(specific));
}

void put_tag(std::map<std::string, std::string>& tags, std::string_view key, ByteView data_body) {
  // 'data' box: type (4: version + flags), locale (4), payload.
  if (data_body.size() < 8) return;
  const auto d = as_u8(data_body);
  const std::uint32_t type = be32(d, 0) & 0xFFFFFF;
  const ByteView payload = data_body.subspan(8);
  if (key == "track" || key == "disc") {
    if (payload.size() >= 6) {
      const auto p = as_u8(payload);
      const int n = (p[2] << 8) | p[3];
      const int total = (p[4] << 8) | p[5];
      if (n > 0) audio::tags::put(tags, key, total > 0 ? std::format("{}/{}", n, total) : std::to_string(n));
    }
    return;
  }
  if (type == 1) audio::tags::put(tags, key, as_chars(payload));
}

void parse_ilst(ByteView ilst, std::map<std::string, std::string>& tags, std::optional<std::string>& smpb) {
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 13> names = {{
      {"\xA9nam", "title"},
      {"\xA9" "ART", "artist"},
      {"\xA9" "alb", "album"},
      {"aART", "albumartist"},
      {"\xA9" "day", "date"},
      {"\xA9" "gen", "genre"},
      {"\xA9" "cmt", "comment"},
      {"\xA9wrt", "composer"},
      {"cprt", "copyright"},
      {"trkn", "track"},
      {"disk", "disc"},
      {"\xA9lyr", "lyrics"},
      {"\xA9grp", "grouping"},
  }};
  for (const Box& item : parse_boxes(ilst, 0)) {
    const auto children = parse_boxes(item.body, 0);
    if (item.type == "----") {
      const Box* name = find_box(children, "name");
      const Box* data = find_box(children, "data");
      if (name != nullptr && data != nullptr && name->body.size() > 4 && data->body.size() > 8) {
        const std::string_view n = as_chars(name->body.subspan(4));
        if (n == "iTunSMPB") smpb = std::string(as_chars(data->body.subspan(8)));
      }
      continue;
    }
    for (const auto& [atom, key] : names) {
      if (item.type == atom) {
        if (const Box* data = find_box(children, "data")) put_tag(tags, key, data->body);
      }
    }
  }
}

/// Sample offsets and sizes from stsz/stz2, stsc and stco/co64.
Result<std::vector<ByteView>> sample_table(ByteView file, const std::vector<Box>& stbl, const Context& ctx) {
  const Box* stsz = find_box(stbl, "stsz");
  const Box* stsc = find_box(stbl, "stsc");
  const Box* stco = find_box(stbl, "stco");
  const Box* co64 = find_box(stbl, "co64");
  if (stsz == nullptr || stsc == nullptr || (stco == nullptr && co64 == nullptr)) {
    return fail(Status::Decode, "the MPEG-4 sample tables are incomplete");
  }
  util::ByteReader sz(stsz->body);
  sz.skip(4);
  const std::uint32_t fixed = sz.u32be();
  const std::uint32_t count = sz.u32be();
  if (!sz.ok() || count > 50'000'000 || (fixed == 0 && sz.remaining() / 4 < count)) {
    return fail(Status::Decode, "invalid MPEG-4 sample size table");
  }
  std::vector<std::uint64_t> offsets;
  util::ByteReader co(stco != nullptr ? stco->body : co64->body);
  co.skip(4);
  const std::uint32_t chunks = co.u32be();
  if (!co.ok() || co.remaining() / (stco != nullptr ? 4 : 8) < chunks) return fail(Status::Decode, "invalid MPEG-4 chunk table");
  offsets.reserve(chunks);
  for (std::uint32_t i = 0; i < chunks; ++i) offsets.push_back(stco != nullptr ? co.u32be() : co.u64be());
  util::ByteReader sc(stsc->body);
  sc.skip(4);
  const std::uint32_t entries = sc.u32be();
  if (!sc.ok() || sc.remaining() / 12 < entries || entries == 0) return fail(Status::Decode, "invalid MPEG-4 sample-to-chunk table");
  struct Entry {
    std::uint32_t first, per_chunk;
  };
  std::vector<Entry> map;
  for (std::uint32_t i = 0; i < entries; ++i) {
    const std::uint32_t first = sc.u32be();
    const std::uint32_t per = sc.u32be();
    sc.skip(4);
    map.push_back({first, per});
  }
  std::vector<ByteView> samples;
  samples.reserve(count);
  std::uint32_t sample = 0;
  for (std::uint32_t c = 0; c < chunks && sample < count; ++c) {
    std::uint32_t per = 0;
    for (const Entry& e : map) {
      if (e.first <= c + 1) per = e.per_chunk;
    }
    std::uint64_t at = offsets[c];
    for (std::uint32_t k = 0; k < per && sample < count; ++k, ++sample) {
      const std::uint32_t size = fixed != 0 ? fixed : sz.u32be();
      if (at > file.size() || size > file.size() - at) return fail(Status::Decode, "MPEG-4 sample {} lies outside the file", sample);
      samples.push_back(file.subspan(static_cast<std::size_t>(at), size));
      at += size;
    }
    if ((c & 4095) == 0) CVT_TRY(ctx.checkpoint(0.0, "Reading"));
  }
  return samples;
}

}  // namespace

Result<std::optional<Track>> read_mp4_aac(ByteView data, const Context& ctx) {
  const auto top = parse_boxes(data, 0);
  const Box* moov = find_box(top, "moov");
  if (moov == nullptr) return fail(Status::Decode, "the MPEG-4 file has no movie header");
  Track track;
  std::optional<std::string> smpb;
  if (const auto ilst = find_path(moov->body, {"udta", "meta", "ilst"})) parse_ilst(ilst->body, track.tags, smpb);
  std::uint32_t movie_timescale = 1000;
  const std::vector<Box> moov_boxes = parse_boxes(moov->body, moov->offset);
  if (const Box* mvhd = find_box(moov_boxes, "mvhd")) {
    util::ByteReader r(mvhd->body);
    const std::uint8_t version = r.u8();
    r.skip(3 + (version == 1 ? 16 : 8));
    movie_timescale = r.u32be();
    if (!r.ok() || movie_timescale == 0) movie_timescale = 1000;
  }
  for (const Box& trak : parse_boxes(moov->body, moov->offset)) {
    if (trak.type != "trak") continue;
    const auto hdlr = find_path(trak.body, {"mdia", "hdlr"});
    if (!hdlr || hdlr->body.size() < 12 || as_chars(hdlr->body.subspan(8, 4)) != "soun") continue;
    const auto stbl = find_path(trak.body, {"mdia", "minf", "stbl"});
    if (!stbl) continue;
    const auto stbl_boxes = parse_boxes(stbl->body, stbl->offset);
    const Box* stsd = find_box(stbl_boxes, "stsd");
    if (stsd == nullptr || stsd->body.size() < 16) continue;
    const ByteView entry_all = stsd->body.subspan(8);
    const auto entries = parse_boxes(entry_all, 0);
    if (entries.empty()) continue;
    const Box& entry = entries.front();
    if (entry.type != "mp4a") return std::optional<Track>{};
    // Sound sample entry: 28 bytes, plus 16 (version 1) or 36 (version 2).
    if (entry.body.size() < 28) return fail(Status::Decode, "invalid MPEG-4 audio sample entry");
    const auto e = as_u8(entry.body);
    const int version = (e[8] << 8) | e[9];
    const std::size_t skip = 28 + (version == 1 ? 16 : version == 2 ? 36 : 0);
    if (entry.body.size() < skip) return fail(Status::Decode, "invalid MPEG-4 audio sample entry");
    const auto children = parse_boxes(entry.body.subspan(skip), 0);
    std::vector<Box> wave_children;
    const Box* esds = find_box(children, "esds");
    if (esds == nullptr) {
      // QuickTime: the esds sits inside a 'wave' box.
      if (const Box* wave = find_box(children, "wave")) {
        wave_children = parse_boxes(wave->body, 0);
        esds = find_box(wave_children, "esds");
      }
    }
    if (esds == nullptr) return fail(Status::Decode, "the MPEG-4 audio track has no decoder configuration");
    const auto config = parse_esds(esds->body);
    if (!config || (config->first != 0x40 && config->first != 0x66 && config->first != 0x67 && config->first != 0x68)) {
      return std::optional<Track>{};
    }
    track.config = config->second;
    if (track.config.empty()) return fail(Status::Decode, "the AAC track has no AudioSpecificConfig");
    CVT_TRY_ASSIGN(track.units, sample_table(data, stbl_boxes, ctx));
    std::uint32_t media_timescale = 0;
    if (const auto mdhd = find_path(trak.body, {"mdia", "mdhd"})) {
      util::ByteReader r(mdhd->body);
      const std::uint8_t v = r.u8();
      r.skip(3 + (v == 1 ? 16 : 8));
      media_timescale = r.u32be();
    }
    // Gapless: the edit list first, else iTunSMPB.
    bool have_edit = false;
    if (const auto elst = find_path(trak.body, {"edts", "elst"})) {
      util::ByteReader r(elst->body);
      const std::uint8_t v = r.u8();
      r.skip(3);
      const std::uint32_t n = r.u32be();
      for (std::uint32_t i = 0; i < n && i < 8 && r.ok(); ++i) {
        const std::uint64_t duration = v == 1 ? r.u64be() : r.u32be();
        const std::int64_t media_time = v == 1 ? static_cast<std::int64_t>(r.u64be()) : static_cast<std::int32_t>(r.u32be());
        r.skip(4);
        if (media_time < 0) continue;  // empty edit
        if (!r.ok()) break;
        track.priming = static_cast<std::uint64_t>(media_time);
        if (duration != 0 && media_timescale != 0) track.valid = duration * media_timescale / movie_timescale;
        have_edit = true;
        break;
      }
    }
    if (!have_edit && smpb) {
      const auto fields = util::split(util::trim(*smpb), ' ');
      if (fields.size() >= 4) {
        const auto priming = util::parse_int(fields[1], 16);
        const auto length = util::parse_int(fields[3], 16);
        if (priming && length && *priming >= 0 && *length > 0) {
          track.priming = static_cast<std::uint64_t>(*priming);
          track.valid = static_cast<std::uint64_t>(*length);
        }
      }
    }
    // With SBR the media time base may be the core rate: scale to output.
    if (const auto asc = parse_audio_config(track.config); asc && asc->sbr && media_timescale != 0 &&
                                                          asc->extension_rate == 2 * media_timescale) {
      track.priming *= 2;
      track.valid *= 2;
    }
    return std::optional<Track>(std::move(track));
  }
  return fail(Status::Decode, "the MPEG-4 file has no audio track");
}

// =========================================================================== MPEG-4 writing

namespace {

Bytes u32(std::uint32_t v) {
  util::ByteWriter w;
  w.u32be(v);
  return w.take();
}

void append(Bytes& out, const Bytes& more) { out.insert(out.end(), more.begin(), more.end()); }

Bytes text_item(std::string_view atom, std::string_view value) {
  util::ByteWriter data;
  data.u32be(1);  // UTF-8
  data.u32be(0);
  data.text(value);
  return box(atom, box("data", data.view()));
}

Bytes number_pair_item(std::string_view atom, std::string_view value) {
  const auto parts = util::split(value, '/');
  const auto n = parts.empty() ? std::nullopt : util::parse_int(util::trim(parts[0]));
  const auto total = parts.size() > 1 ? util::parse_int(util::trim(parts[1])) : std::nullopt;
  if (!n || *n <= 0 || *n > 65535) return {};
  util::ByteWriter data;
  data.u32be(0);
  data.u32be(0);
  data.u16be(0);
  data.u16be(static_cast<std::uint16_t>(*n));
  data.u16be(static_cast<std::uint16_t>(total && *total > 0 && *total < 65536 ? *total : 0));
  if (atom == "trkn") data.u16be(0);
  return box(atom, box("data", data.view()));
}

Bytes esds_box(const Bytes& asc, std::uint32_t bitrate) {
  const auto descriptor = [](std::uint8_t tag, const Bytes& body) {
    util::ByteWriter w;
    w.u8(tag);
    // Four-byte length form, as most muxers write it.
    const auto n = static_cast<std::uint32_t>(body.size());
    w.u8(static_cast<std::uint8_t>(0x80 | ((n >> 21) & 0x7F)));
    w.u8(static_cast<std::uint8_t>(0x80 | ((n >> 14) & 0x7F)));
    w.u8(static_cast<std::uint8_t>(0x80 | ((n >> 7) & 0x7F)));
    w.u8(static_cast<std::uint8_t>(n & 0x7F));
    w.bytes(body);
    return w.take();
  };
  util::ByteWriter dcd;
  dcd.u8(0x40);  // MPEG-4 audio
  dcd.u8(0x15);  // audio stream
  dcd.u8(0);
  dcd.u16be(0);  // buffer size
  dcd.u32be(bitrate);
  dcd.u32be(bitrate);
  dcd.bytes(descriptor(0x05, asc));
  util::ByteWriter es;
  es.u16be(1);  // ES_ID
  es.u8(0);
  es.bytes(descriptor(0x04, dcd.take()));
  es.bytes(descriptor(0x06, Bytes{std::byte{2}}));  // SLConfig: predefined MP4
  return full_box("esds", 0, 0, descriptor(0x03, es.take()));
}

}  // namespace

Result<Bytes> write_m4a(const Encoded& stream, const std::map<std::string, std::string>& tags, std::string_view brand,
                        const Context& ctx) {
  const std::uint32_t rate = stream.sample_rate;
  const auto count = static_cast<std::uint32_t>(stream.units.size());
  const std::uint64_t media_duration = std::uint64_t{count} * 1024;
  std::uint64_t data_size = 0;
  std::uint32_t max_unit = 0;
  for (const Bytes& u : stream.units) {
    data_size += u.size();
    max_unit = std::max<std::uint32_t>(max_unit, static_cast<std::uint32_t>(u.size()));
  }
  const auto bitrate = static_cast<std::uint32_t>(media_duration == 0 ? 0 : data_size * 8 * rate / media_duration);
  const auto build_moov = [&](std::uint64_t data_offset) {
    // mvhd
    util::ByteWriter mvhd;
    mvhd.u32be(0);  // creation time: none (no current date in output)
    mvhd.u32be(0);
    mvhd.u32be(rate);
    mvhd.u32be(static_cast<std::uint32_t>(stream.frames));
    mvhd.u32be(0x00010000);
    mvhd.u16be(0x0100);
    for (int i = 0; i < 10; ++i) mvhd.u8(0);
    for (const std::uint32_t v : {0x00010000u, 0u, 0u, 0u, 0x00010000u, 0u, 0u, 0u, 0x40000000u}) mvhd.u32be(v);
    for (int i = 0; i < 6; ++i) mvhd.u32be(0);
    mvhd.u32be(2);
    // tkhd
    util::ByteWriter tkhd;
    tkhd.u32be(0);
    tkhd.u32be(0);
    tkhd.u32be(1);  // track id
    tkhd.u32be(0);
    tkhd.u32be(static_cast<std::uint32_t>(stream.frames));
    tkhd.u32be(0);
    tkhd.u32be(0);
    tkhd.u16be(0);
    tkhd.u16be(0);
    tkhd.u16be(0x0100);
    tkhd.u16be(0);
    for (const std::uint32_t v : {0x00010000u, 0u, 0u, 0u, 0x00010000u, 0u, 0u, 0u, 0x40000000u}) tkhd.u32be(v);
    tkhd.u32be(0);
    tkhd.u32be(0);
    // elst: skip the encoder priming, play exactly the input length.
    util::ByteWriter elst;
    elst.u32be(1);
    elst.u32be(static_cast<std::uint32_t>(stream.frames));
    elst.u32be(static_cast<std::uint32_t>(stream.priming));
    elst.u32be(0x00010000);
    // mdhd
    util::ByteWriter mdhd;
    mdhd.u32be(0);
    mdhd.u32be(0);
    mdhd.u32be(rate);
    mdhd.u32be(static_cast<std::uint32_t>(media_duration));
    mdhd.u16be(0x55C4);  // "und"
    mdhd.u16be(0);
    util::ByteWriter hdlr;
    hdlr.u32be(0);
    hdlr.text("soun");
    for (int i = 0; i < 12; ++i) hdlr.u8(0);
    hdlr.text("SoundHandler");
    hdlr.u8(0);
    util::ByteWriter smhd;
    smhd.u32be(0);
    util::ByteWriter dref;
    dref.u32be(1);
    dref.bytes(full_box("url ", 0, 1, {}));
    // stsd / mp4a
    util::ByteWriter mp4a;
    for (int i = 0; i < 6; ++i) mp4a.u8(0);
    mp4a.u16be(1);
    mp4a.u32be(0);
    mp4a.u32be(0);
    mp4a.u16be(static_cast<std::uint16_t>(stream.channels));
    mp4a.u16be(16);
    mp4a.u16be(0);
    mp4a.u16be(0);
    mp4a.u32be(rate <= 65535 ? rate << 16 : 0);
    mp4a.bytes(esds_box(stream.config, bitrate));
    util::ByteWriter stsd;
    stsd.u32be(1);
    stsd.bytes(box("mp4a", mp4a.view()));
    util::ByteWriter stts;
    stts.u32be(1);
    stts.u32be(count);
    stts.u32be(1024);
    util::ByteWriter stsc;
    stsc.u32be(1);
    stsc.u32be(1);
    stsc.u32be(count);
    stsc.u32be(1);
    util::ByteWriter stsz;
    stsz.u32be(0);
    stsz.u32be(count);
    for (const Bytes& u : stream.units) stsz.u32be(static_cast<std::uint32_t>(u.size()));
    util::ByteWriter stco;
    stco.u32be(1);
    stco.u32be(static_cast<std::uint32_t>(data_offset));
    Bytes stbl;
    append(stbl, full_box("stsd", 0, 0, stsd.view()));
    append(stbl, full_box("stts", 0, 0, stts.view()));
    append(stbl, full_box("stsc", 0, 0, stsc.view()));
    append(stbl, full_box("stsz", 0, 0, stsz.view()));
    append(stbl, full_box("stco", 0, 0, stco.view()));
    Bytes minf;
    append(minf, full_box("smhd", 0, 0, smhd.view()));
    append(minf, box("dinf", full_box("dref", 0, 0, dref.view())));
    append(minf, box("stbl", stbl));
    Bytes mdia;
    append(mdia, full_box("mdhd", 0, 0, mdhd.view()));
    append(mdia, full_box("hdlr", 0, 0, hdlr.view()));
    append(mdia, box("minf", minf));
    Bytes trak;
    append(trak, full_box("tkhd", 0, 3, tkhd.view()));
    append(trak, box("edts", full_box("elst", 0, 0, elst.view())));
    append(trak, box("mdia", mdia));
    // iTunes items: gapless data always (Apple players read it), tags only when kept.
    Bytes ilst;
    const std::uint64_t padding = media_duration - stream.priming - stream.frames;
    {
      util::ByteWriter mean, name, data;
      mean.u32be(0);
      mean.text("com.apple.iTunes");
      name.u32be(0);
      name.text("iTunSMPB");
      data.u32be(1);
      data.u32be(0);
      data.text(std::format(" 00000000 {:08X} {:08X} {:016X} 00000000 00000000 00000000 00000000 00000000 00000000 "
                            "00000000 00000000",
                            static_cast<std::uint32_t>(stream.priming), static_cast<std::uint32_t>(padding), stream.frames));
      Bytes item;
      append(item, box("mean", mean.view()));
      append(item, box("name", name.view()));
      append(item, box("data", data.view()));
      append(ilst, box("----", item));
    }
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 11> atoms = {{
        {"title", "\xA9nam"},
        {"artist", "\xA9" "ART"},
        {"album", "\xA9" "alb"},
        {"albumartist", "aART"},
        {"date", "\xA9" "day"},
        {"genre", "\xA9" "gen"},
        {"comment", "\xA9" "cmt"},
        {"composer", "\xA9wrt"},
        {"copyright", "cprt"},
        {"track", "trkn"},
        {"disc", "disk"},
    }};
    for (const auto& [key, atom] : atoms) {
      const auto it = tags.find(std::string(key));
      if (it == tags.end()) continue;
      append(ilst, (key == "track" || key == "disc") ? number_pair_item(atom, it->second) : text_item(atom, it->second));
    }
    util::ByteWriter meta_hdlr;
    meta_hdlr.u32be(0);
    meta_hdlr.text("mdir");
    meta_hdlr.text("appl");
    for (int i = 0; i < 9; ++i) meta_hdlr.u8(0);  // reserved, empty name
    Bytes meta;
    append(meta, full_box("hdlr", 0, 0, meta_hdlr.view()));
    append(meta, box("ilst", ilst));
    Bytes moov;
    append(moov, full_box("mvhd", 0, 0, mvhd.view()));
    append(moov, box("trak", trak));
    append(moov, box("udta", full_box("meta", 0, 0, meta)));
    return box("moov", moov);
  };
  util::ByteWriter ftyp;
  ftyp.text(brand);
  ftyp.u32be(0);
  ftyp.text(brand);
  ftyp.text("mp42");
  ftyp.text("isom");
  const Bytes ftyp_box = box("ftyp", ftyp.view());
  const std::size_t moov_size = build_moov(0).size();
  const std::uint64_t data_offset = ftyp_box.size() + moov_size + 8;
  if (data_offset + data_size > 0xFFFFFFFFull) return fail(Status::Encode, "the AAC stream is too large for a 32-bit MPEG-4 file");
  CVT_TRY(ctx.limits.check_output(data_offset + data_size));
  Bytes out = ftyp_box;
  append(out, build_moov(data_offset));
  append(out, u32(static_cast<std::uint32_t>(data_size + 8)));
  const std::string_view mdat = "mdat";
  out.insert(out.end(), reinterpret_cast<const std::byte*>(mdat.data()), reinterpret_cast<const std::byte*>(mdat.data()) + 4);
  for (const Bytes& u : stream.units) append(out, u);
  (void)max_unit;
  return out;
}

// =========================================================================== registry

namespace {

class AacPacketDecoder final : public codec::AudioDecoder {
 public:
  Result<void> configure(const codec::CodecParams& params) override {
    Bytes config = params.extradata;
    if (config.empty()) {
      if (params.sample_rate == 0 || params.channels == 0) return fail(Status::Decode, "AAC without configuration");
      config = make_lc_config(params.sample_rate, params.channels);
    }
    CVT_TRY(decoder_.configure(config));
    return {};
  }
  Result<void> send_packet(const codec::Packet& packet) override {
    ByteView data(packet.bytes);
    // Tolerate ADTS-framed packets.
    const auto d = as_u8(data);
    if (d.size() >= 7 && d[0] == 0xFF && (d[1] & 0xF6) == 0xF0) data = data.subspan((d[1] & 1) == 0 ? 9 : 7);
    (void)decoder_.decode(data, pending_);
    return {};
  }
  Result<std::optional<codec::AudioChunk>> receive_chunk() override {
    if (pending_.empty()) return std::optional<codec::AudioChunk>{};
    codec::AudioChunk chunk;
    chunk.samples = std::move(pending_);
    pending_.clear();
    return std::optional<codec::AudioChunk>(std::move(chunk));
  }
  Result<void> flush() override { return {}; }
  [[nodiscard]] std::uint32_t sample_rate() const override { return decoder_.sample_rate(); }
  [[nodiscard]] std::uint16_t channels() const override { return static_cast<std::uint16_t>(decoder_.channels()); }

 private:
  Decoder decoder_;
  std::vector<float> pending_;
};

class AacPacketEncoder final : public codec::AudioEncoder {
 public:
  Result<void> configure(const codec::AudioEncodeParams& params) override {
    const std::uint32_t per_channel = params.quality >= 90 ? 128000 : params.quality >= 75 ? 96000 : params.quality >= 50 ? 64000 : 48000;
    const std::uint32_t bitrate = params.bitrate_kbps ? *params.bitrate_kbps * 1000 : per_channel * params.channels;
    CVT_TRY(encoder_.configure(params.sample_rate, params.channels, bitrate));
    channels_ = params.channels;
    rate_ = params.sample_rate;
    timescale_ = params.timescale != 0 ? params.timescale : params.sample_rate;
    return {};
  }
  Result<void> send_chunk(const codec::AudioChunk& chunk) override {
    buffer_.insert(buffer_.end(), chunk.samples.begin(), chunk.samples.end());
    return drain(false);
  }
  Result<std::optional<codec::Packet>> receive_packet() override {
    if (next_ >= ready_.size()) {
      ready_.clear();
      next_ = 0;
      return std::optional<codec::Packet>{};
    }
    return std::optional<codec::Packet>(std::move(ready_[next_++]));
  }
  [[nodiscard]] Bytes extradata() const override { return encoder_.config(); }
  Result<void> flush() override { return drain(true); }
  [[nodiscard]] std::uint32_t frame_size() const override { return 1024; }
  [[nodiscard]] std::uint32_t delay() const override { return Encoder::delay(); }

 private:
  Result<void> drain(bool final) {
    const std::size_t per = 1024 * channels_;
    if (final) {
      buffer_.resize(((buffer_.size() + per - 1) / per) * per, 0.0f);
      buffer_.resize(buffer_.size() + per, 0.0f);
    }
    std::size_t pos = 0;
    while (buffer_.size() - pos >= per) {
      CVT_TRY_ASSIGN(Bytes unit, encoder_.encode(buffer_.data() + pos));
      codec::Packet p;
      p.bytes = std::move(unit);
      p.pts = p.dts = static_cast<std::int64_t>(index_ * 1024 * timescale_ / rate_);
      p.duration = static_cast<std::int64_t>(1024ull * timescale_ / rate_);
      p.keyframe = true;
      ready_.push_back(std::move(p));
      ++index_;
      pos += per;
    }
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(pos));
    return {};
  }
  Encoder encoder_;
  std::vector<float> buffer_;
  std::vector<codec::Packet> ready_;
  std::size_t next_ = 0;
  std::size_t channels_ = 2;
  std::uint32_t rate_ = 44100;
  std::uint32_t timescale_ = 44100;
  std::uint64_t index_ = 0;
};

}  // namespace

std::unique_ptr<codec::AudioDecoder> make_aac_decoder() { return std::make_unique<AacPacketDecoder>(); }
std::unique_ptr<codec::AudioEncoder> make_aac_encoder() { return std::make_unique<AacPacketEncoder>(); }

}  // namespace cvt::lossy::aac
