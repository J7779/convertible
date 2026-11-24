// Convertible core - audio_lossless family: MPEG-4 audio files (.m4a) with
// Apple Lossless, via a minimal ISO base media file format reader/writer.
//
// Reader: the first sound track's sample table (stsd/stts/stsc/stsz/stco/
// co64) gives the packets; ALAC is decoded in-house, any other codec
// (AAC, FLAC, Opus, ...) through the media codec registry when a decoder is
// registered. iTunes-style metadata (moov/udta/meta/ilst) becomes tags.
// Writer: ALAC in an 'M4A ' file (ftyp, moov with one track, mdat), iTunes
// tags only when metadata is kept; no dates or software names are written.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "codec/ids.hpp"
#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/audio_lossless/alac.hpp"
#include "families/audio_lossless/audio_api.hpp"
#include "families/audio_lossless/common.hpp"
#include "families/audio_lossless/mp4_boxes.hpp"
#include "families/audio_lossless/packet_decode.hpp"
#include "families/audio_lossless/tags.hpp"
#include "util/text.hpp"

namespace cvt::audio {

// =========================================================================== box helpers

std::vector<Box> parse_boxes(ByteView data, std::size_t header_offset) {
  std::vector<Box> boxes;
  std::size_t pos = 0;
  std::size_t guard = 0;
  while (pos + 8 <= data.size() && ++guard < 100000) {
    util::ByteReader r(data.subspan(pos));
    std::uint64_t size = r.u32be();
    const std::string_view type = r.chars(4);
    std::size_t header = 8;
    if (size == 1) {
      size = r.u64be();
      header = 16;
      if (!r.ok()) break;
    } else if (size == 0) {
      size = data.size() - pos;
    }
    if (size < header || size > data.size() - pos) {
      // Truncated last box: keep what is there (mdat of a cut file).
      if (size >= header && pos + header <= data.size()) {
        boxes.push_back({std::string(type), data.subspan(pos + header), header_offset + pos + header, true});
      }
      break;
    }
    boxes.push_back({std::string(type), data.subspan(pos + header, static_cast<std::size_t>(size) - header),
                     header_offset + pos + header, false});
    pos += static_cast<std::size_t>(size);
  }
  return boxes;
}

const Box* find_box(const std::vector<Box>& boxes, std::string_view type) {
  for (const Box& box : boxes) {
    if (box.type == type) return &box;
  }
  return nullptr;
}

std::optional<Box> find_path(ByteView data, std::initializer_list<std::string_view> path) {
  std::optional<Box> current;
  ByteView scope = data;
  std::size_t offset = 0;
  for (const std::string_view type : path) {
    const std::vector<Box> boxes = parse_boxes(scope, offset);
    const Box* box = find_box(boxes, type);
    if (box == nullptr) return std::nullopt;
    current = *box;
    scope = box->body;
    offset = box->offset;
    if (type == "meta" && scope.size() >= 4) {
      // 'meta' is a full box in MP4 (version/flags), but not in old QuickTime files.
      if (scope.size() >= 8 && as_chars(scope.subspan(4, 4)) == "hdlr") {
        // QuickTime style: no version field.
      } else {
        scope = scope.subspan(4);
        offset += 4;
      }
      current->body = scope;
      current->offset = offset;
    }
  }
  return current;
}

void write_box(util::ByteWriter& out, std::string_view type, ByteView body) {
  out.u32be(static_cast<std::uint32_t>(body.size() + 8));
  out.text(type);
  out.bytes(body);
}

Bytes box(std::string_view type, ByteView body) {
  util::ByteWriter w;
  write_box(w, type, body);
  return w.take();
}

Bytes full_box(std::string_view type, std::uint8_t version, std::uint32_t flags, ByteView body) {
  util::ByteWriter w;
  w.u32be(static_cast<std::uint32_t>(body.size() + 12));
  w.text(type);
  w.u32be((std::uint32_t{version} << 24) | (flags & 0xFFFFFF));
  w.bytes(body);
  return w.take();
}

// =========================================================================== iTunes tags

namespace {

struct ItunesKey {
  std::string_view atom;
  std::string_view key;
};
constexpr std::array<ItunesKey, 11> itunes_keys = {{{"\xA9nam", "title"},
                                                    {"\xA9" "ART", "artist"},
                                                    {"\xA9" "alb", "album"},
                                                    {"aART", "albumartist"},
                                                    {"\xA9" "day", "date"},
                                                    {"\xA9gen", "genre"},
                                                    {"\xA9" "cmt", "comment"},
                                                    {"\xA9wrt", "composer"},
                                                    {"cprt", "copyright"},
                                                    {"\xA9too", "encoder"},
                                                    {"\xA9lyr", "lyrics"}}};

void parse_ilst(ByteView ilst, tags::TagMap& tag_map) {
  for (const Box& item : parse_boxes(ilst, 0)) {
    const std::vector<Box> children = parse_boxes(item.body, 0);
    const Box* data = find_box(children, "data");
    if (data == nullptr || data->body.size() < 8) continue;
    util::ByteReader r(data->body);
    const std::uint32_t type = r.u32be() & 0xFFFFFF;
    r.skip(4);
    const ByteView value = r.rest();
    if (item.type == "trkn" || item.type == "disk") {
      if (value.size() >= 6) {
        util::ByteReader v(value);
        v.skip(2);
        const std::uint16_t number = v.u16be();
        const std::uint16_t total = v.u16be();
        std::string text = std::to_string(number);
        if (total != 0) text += "/" + std::to_string(total);
        if (number != 0) tags::put(tag_map, item.type == "trkn" ? "track" : "disc", text);
      }
      continue;
    }
    if (type != 1) continue;  // only UTF-8 text
    if (item.type == "----") {
      const Box* name = find_box(children, "name");
      if (name != nullptr && name->body.size() > 4) tags::put(tag_map, util::to_lower(as_chars(name->body.subspan(4))), as_chars(value));
      continue;
    }
    for (const ItunesKey& k : itunes_keys) {
      if (k.atom == item.type) tags::put(tag_map, k.key, as_chars(value));
    }
  }
}

Bytes ilst_box(const tags::TagMap& tag_map) {
  util::ByteWriter items;
  for (const auto& [key, value] : tag_map) {
    if (key == "track" || key == "disc") {
      const std::vector<std::string_view> parts = util::split(value, '/');
      const auto number = util::parse_int(util::trim(parts[0]));
      const auto total = parts.size() > 1 ? util::parse_int(util::trim(parts[1])) : std::optional<std::int64_t>(0);
      if (!number || *number < 0 || *number > 65535) continue;
      util::ByteWriter data;
      data.u32be(0);
      data.u32be(0);
      data.u16be(0);
      data.u16be(static_cast<std::uint16_t>(*number));
      data.u16be(static_cast<std::uint16_t>(total && *total >= 0 && *total <= 65535 ? *total : 0));
      if (key == "track") data.u16be(0);
      write_box(items, key == "track" ? "trkn" : "disk", box("data", data.view()));
      continue;
    }
    std::string_view atom;
    for (const ItunesKey& k : itunes_keys) {
      if (k.key == key) atom = k.atom;
    }
    util::ByteWriter data;
    data.u32be(1);
    data.u32be(0);
    data.text(value);
    if (atom.empty()) {
      // Free-form: ----/mean/name/data.
      util::ByteWriter free;
      free.bytes(full_box("mean", 0, 0, as_bytes("com.apple.iTunes")));
      free.bytes(full_box("name", 0, 0, as_bytes(util::to_upper(key))));
      write_box(free, "data", data.view());
      write_box(items, "----", free.view());
    } else {
      write_box(items, atom, box("data", data.view()));
    }
  }
  if (items.size() == 0) return {};
  return box("ilst", items.view());
}

// =========================================================================== reading

struct SampleEntry {
  std::string type;
  std::uint16_t channels = 0;
  std::uint16_t sample_size = 0;
  std::uint32_t sample_rate = 0;
  std::vector<Box> children;
};

// AudioSpecificConfig and object type from an 'esds' body.
struct Esds {
  std::uint8_t object_type = 0;
  Bytes specific;
};

std::optional<Esds> parse_esds(ByteView body) {
  util::ByteReader r(body);
  r.skip(4);  // version/flags
  const auto descriptor = [&](std::uint8_t& tag) -> std::uint32_t {
    tag = r.u8();
    std::uint32_t length = 0;
    for (int i = 0; i < 4; ++i) {
      const std::uint8_t b = r.u8();
      length = (length << 7) | (b & 0x7F);
      if ((b & 0x80) == 0) break;
    }
    return length;
  };
  std::uint8_t tag = 0;
  descriptor(tag);
  if (tag != 3) return std::nullopt;
  r.skip(2);
  const std::uint8_t flags = r.u8();
  if (flags & 0x80) r.skip(2);
  if (flags & 0x40) r.skip(r.u8());
  if (flags & 0x20) r.skip(2);
  descriptor(tag);
  if (tag != 4) return std::nullopt;
  Esds out;
  out.object_type = r.u8();
  r.skip(12);
  const std::uint32_t length = descriptor(tag);
  if (tag == 5 && r.ok()) {
    const ByteView specific = r.bytes(std::min<std::size_t>(length, r.remaining()));
    out.specific = to_bytes(specific);
  }
  if (!r.ok()) return std::nullopt;
  return out;
}

Result<SampleEntry> parse_sample_entry(ByteView stsd) {
  util::ByteReader r(stsd);
  r.skip(4);
  const std::uint32_t count = r.u32be();
  if (!r.ok() || count == 0) return fail(Status::Decode, "the track has no sample description");
  const std::vector<Box> entries = parse_boxes(r.rest(), 0);
  if (entries.empty()) return fail(Status::Decode, "the track has no sample description");
  const Box& entry = entries.front();
  util::ByteReader e(entry.body);
  e.skip(8);  // reserved + data reference index
  const std::uint16_t version = e.u16be();
  e.skip(6);
  SampleEntry out;
  out.type = entry.type;
  out.channels = e.u16be();
  out.sample_size = e.u16be();
  e.skip(4);
  out.sample_rate = e.u32be() >> 16;
  if (version == 1) e.skip(16);
  if (version == 2) {
    e.skip(4);
    const double rate = std::bit_cast<double>(e.u64be());
    const std::uint32_t channels = e.u32be();
    e.skip(20);
    if (rate > 0.0 && rate < 10'000'000.0) out.sample_rate = static_cast<std::uint32_t>(rate);
    out.channels = static_cast<std::uint16_t>(std::min<std::uint32_t>(channels, 255));
  }
  if (!e.ok()) return fail(Status::Decode, "the audio sample description is truncated");
  out.children = parse_boxes(e.rest(), 0);
  return out;
}

Result<std::vector<ByteView>> sample_table(ByteView input, ByteView stbl, const Context& ctx) {
  const std::vector<Box> boxes = parse_boxes(stbl, 0);
  const Box* stsz = find_box(boxes, "stsz");
  const Box* stsc = find_box(boxes, "stsc");
  const Box* stco = find_box(boxes, "stco");
  const Box* co64 = find_box(boxes, "co64");
  if (stsz == nullptr || stsc == nullptr || (stco == nullptr && co64 == nullptr)) {
    return fail(Status::Decode, "the track's sample table is incomplete");
  }
  util::ByteReader z(stsz->body);
  z.skip(4);
  const std::uint32_t uniform = z.u32be();
  const std::uint32_t count = z.u32be();
  if (!z.ok()) return fail(Status::Decode, "damaged sample size table");
  if (count > input.size()) return fail(Status::Decode, "the sample table lists more samples than the file holds");
  CVT_TRY(ctx.limits.check_archive_entries(count / 64));
  if (uniform == 0 && z.remaining() < std::uint64_t{count} * 4) return fail(Status::Decode, "damaged sample size table");
  std::vector<std::uint32_t> sizes(count);
  for (std::uint32_t i = 0; i < count; ++i) sizes[i] = uniform != 0 ? uniform : z.u32be();
  std::vector<std::uint64_t> chunks;
  util::ByteReader o((stco != nullptr ? stco : co64)->body);
  o.skip(4);
  const std::uint32_t chunk_count = o.u32be();
  if (chunk_count > input.size()) return fail(Status::Decode, "damaged chunk offset table");
  for (std::uint32_t i = 0; i < chunk_count && o.ok(); ++i) chunks.push_back(stco != nullptr ? o.u32be() : o.u64be());
  if (!o.ok()) return fail(Status::Decode, "damaged chunk offset table");
  util::ByteReader c(stsc->body);
  c.skip(4);
  const std::uint32_t entries = c.u32be();
  struct Run {
    std::uint32_t first, per;
  };
  std::vector<Run> runs;
  for (std::uint32_t i = 0; i < entries && c.ok() && i < 1'000'000; ++i) {
    const std::uint32_t first = c.u32be();
    const std::uint32_t per = c.u32be();
    c.skip(4);
    if (c.ok()) runs.push_back({first, per});
  }
  if (runs.empty()) return fail(Status::Decode, "damaged sample-to-chunk table");
  std::vector<ByteView> packets;
  packets.reserve(count);
  std::uint32_t sample = 0;
  for (std::size_t chunk = 0; chunk < chunks.size() && sample < count; ++chunk) {
    std::uint32_t per = 0;
    for (const Run& run : runs) {
      if (run.first <= chunk + 1) per = run.per;
    }
    std::uint64_t offset = chunks[chunk];
    for (std::uint32_t k = 0; k < per && sample < count; ++k, ++sample) {
      const std::uint32_t size = sizes[sample];
      if (offset > input.size() || size > input.size() - offset) {
        ctx.warn("The file is truncated; the audio ends early.");
        return packets;
      }
      packets.push_back(input.subspan(static_cast<std::size_t>(offset), size));
      offset += size;
    }
  }
  return packets;
}

}  // namespace

Result<ir::Audio> read_mp4_audio(ByteView input, const Context& ctx) {
  const std::vector<Box> top = parse_boxes(input, 0);
  const Box* moov = find_box(top, "moov");
  if (moov == nullptr) return fail(Status::Decode, "the MPEG-4 file has no movie header (moov)");
  tags::TagMap tag_map;
  if (const auto ilst = find_path(moov->body, {"udta", "meta", "ilst"})) parse_ilst(ilst->body, tag_map);
  for (const Box& trak : parse_boxes(moov->body, moov->offset)) {
    if (trak.type != "trak") continue;
    const auto hdlr = find_path(trak.body, {"mdia", "hdlr"});
    if (!hdlr || hdlr->body.size() < 12 || as_chars(hdlr->body.subspan(8, 4)) != "soun") continue;
    const auto stbl = find_path(trak.body, {"mdia", "minf", "stbl"});
    if (!stbl) return fail(Status::Decode, "the sound track has no sample table");
    const auto stsd = find_path(stbl->body, {"stsd"});
    if (!stsd) return fail(Status::Decode, "the sound track has no sample description");
    CVT_TRY_ASSIGN(const SampleEntry entry, parse_sample_entry(stsd->body));
    PacketTrack track;
    track.sample_rate = entry.sample_rate;
    track.channels = entry.channels;
    track.bits = entry.sample_size;
    if (entry.type == "alac") {
      const Box* cookie = find_box(entry.children, "alac");
      if (cookie == nullptr || cookie->body.size() < 28) return fail(Status::Decode, "the ALAC track has no configuration");
      track.codec = std::string(codec::id::alac);
      track.extradata = to_bytes(cookie->body.subspan(4));
    } else if (entry.type == "mp4a") {
      const Box* esds_box = find_box(entry.children, "esds");
      const std::optional<Esds> esds = esds_box != nullptr ? parse_esds(esds_box->body) : std::nullopt;
      if (!esds) return fail(Status::Decode, "the MPEG-4 audio track has no decoder configuration");
      if (esds->object_type == 0x40 || esds->object_type == 0x66 || esds->object_type == 0x67 || esds->object_type == 0x68) {
        track.codec = std::string(codec::id::aac);
      } else if (esds->object_type == 0x69 || esds->object_type == 0x6B) {
        track.codec = std::string(codec::id::mp3);
      } else {
        return fail(Status::Decode, "unsupported MPEG-4 audio object type 0x{:02X}", esds->object_type);
      }
      track.extradata = esds->specific;
    } else if (entry.type == "fLaC") {
      const Box* dfla = find_box(entry.children, "dfLa");
      track.codec = std::string(codec::id::flac);
      if (dfla != nullptr && dfla->body.size() > 8) track.extradata = to_bytes(dfla->body.subspan(8));  // after version and block header
    } else if (entry.type == "Opus") {
      const Box* dops = find_box(entry.children, "dOps");
      track.codec = std::string(codec::id::opus);
      if (dops != nullptr) track.extradata = to_bytes(dops->body);
    } else if (entry.type == "ac-3") {
      track.codec = std::string(codec::id::ac3);
    } else {
      return fail(Status::Decode, "unsupported audio codec '{}' in the MPEG-4 file", entry.type);
    }
    CVT_TRY_ASSIGN(track.packets, sample_table(input, stbl->body, ctx));
    // Length from the media header when it is shorter (trailing padding of the last packet).
    if (const auto mdhd = find_path(trak.body, {"mdia", "mdhd"})) {
      util::ByteReader m(mdhd->body);
      const std::uint8_t version = m.u8();
      m.skip(3);
      std::uint32_t timescale = 0;
      std::uint64_t duration = 0;
      if (version == 1) {
        m.skip(16);
        timescale = m.u32be();
        duration = m.u64be();
      } else {
        m.skip(8);
        timescale = m.u32be();
        duration = m.u32be();
      }
      if (m.ok() && track.codec == codec::id::alac && timescale != 0 && timescale == entry.sample_rate) {
        track.valid_frames = duration;
      }
    }
    CVT_TRY_ASSIGN(ir::Audio audio, decode_track(track, ctx));
    audio.tags = std::move(tag_map);
    return audio;
  }
  return fail(Status::Decode, "the MPEG-4 file has no audio track");
}

namespace {

Result<AnyIR> read_m4a(ReadContext& ctx) {
  CVT_TRY_ASSIGN(ir::Audio audio, read_mp4_audio(ctx.input, ctx));
  return audio;
}

// =========================================================================== writing

}  // namespace

Result<AlacStream> encode_alac_stream(const ir::Audio& audio, const Context& ctx) {
  if (audio.channels > 8) return fail(Status::Encode, "ALAC holds at most 8 channels");
  static constexpr std::array<int, 4> allowed = {16, 20, 24, 32};
  std::string choice = ctx.options.get_enum(key_sample_format, "auto", {"auto", "s16", "s20", "s24", "s32"});
  int bits = choice == "auto" ? default_bit_depth(audio, allowed, audio.float_samples ? 24 : 16)
                              : static_cast<int>(util::parse_int(std::string_view(choice).substr(1)).value_or(16));
  AlacStream out;
  out.config.bit_depth = static_cast<std::uint8_t>(bits);
  out.config.channels = static_cast<std::uint8_t>(audio.channels);
  out.config.sample_rate = audio.sample_rate;
  out.config.frame_length = 4096;
  alac::Encoder encoder;
  CVT_TRY(encoder.configure(out.config, options::resolved_compression_level(ctx.options, 9)));
  const std::vector<std::int32_t> ints = quantize_for(audio, bits, ctx.options);
  out.frames = audio.frame_count();
  std::uint64_t total_bytes = 0;
  std::uint32_t max_packet = 0;
  std::size_t index = 0;
  for (std::uint64_t first = 0; first < out.frames; first += out.config.frame_length, ++index) {
    if ((index & 15) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(first) / static_cast<double>(out.frames), "Encoding"));
    const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(out.config.frame_length, out.frames - first));
    Bytes packet = encoder.encode(std::span<const std::int32_t>(ints).subspan(
                                      static_cast<std::size_t>(first * audio.channels), std::size_t{count} * audio.channels),
                                  count);
    total_bytes += packet.size();
    max_packet = std::max<std::uint32_t>(max_packet, static_cast<std::uint32_t>(packet.size()));
    CVT_TRY(ctx.limits.check_output(total_bytes));
    out.packets.push_back(std::move(packet));
  }
  out.config.max_frame_bytes = max_packet;
  const double seconds = static_cast<double>(out.frames) / audio.sample_rate;
  out.config.avg_bit_rate = seconds > 0.0 ? static_cast<std::uint32_t>(std::min(4.0e9, static_cast<double>(total_bytes) * 8.0 / seconds)) : 0;
  return out;
}

namespace {

Result<Bytes> write_m4a(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Audio audio, prepare_for_writing(value.get<ir::Audio>(), ctx, 1, 0, 8));
  CVT_TRY_ASSIGN(const AlacStream stream, encode_alac_stream(audio, ctx));
  const std::uint32_t rate = audio.sample_rate;
  const std::uint64_t frames = stream.frames;
  // stts: all packets of frame_length, the last possibly shorter.
  util::ByteWriter stts;
  {
    const std::size_t n = stream.packets.size();
    const std::uint64_t last = n == 0 ? 0 : frames - (n - 1) * std::uint64_t{stream.config.frame_length};
    std::vector<std::pair<std::uint32_t, std::uint32_t>> runs;
    if (n > 1) runs.push_back({static_cast<std::uint32_t>(n - 1), stream.config.frame_length});
    if (n > 0) runs.push_back({1, static_cast<std::uint32_t>(last)});
    stts.u32be(static_cast<std::uint32_t>(runs.size()));
    for (const auto& [count, delta] : runs) {
      stts.u32be(count);
      stts.u32be(delta);
    }
  }
  util::ByteWriter stsz;
  stsz.u32be(0);
  stsz.u32be(static_cast<std::uint32_t>(stream.packets.size()));
  std::uint64_t data_size = 0;
  for (const Bytes& p : stream.packets) {
    stsz.u32be(static_cast<std::uint32_t>(p.size()));
    data_size += p.size();
  }
  util::ByteWriter stsc;
  stsc.u32be(1);
  stsc.u32be(1);
  stsc.u32be(static_cast<std::uint32_t>(std::max<std::size_t>(stream.packets.size(), 1)));
  stsc.u32be(1);
  // Sample entry.
  util::ByteWriter entry;
  entry.zeros(6);
  entry.u16be(1);
  entry.zeros(8);
  entry.u16be(audio.channels);
  entry.u16be(static_cast<std::uint16_t>(stream.config.bit_depth));
  entry.u16be(0);
  entry.u16be(0);
  entry.u32be(rate <= 65535 ? rate << 16 : 0);
  entry.bytes(full_box("alac", 0, 0, alac::serialize_config(stream.config)));
  if (audio.channels >= 3) {
    // Core Audio layout tags of the ALAC element orders (C L R ..., LFE last).
    static constexpr std::array<std::uint32_t, 9> layout_tags = {
        0, 0, 0, (113u << 16) | 3, (116u << 16) | 4, (120u << 16) | 5, (124u << 16) | 6, (142u << 16) | 7, (127u << 16) | 8};
    util::ByteWriter chan;
    chan.u32be(layout_tags[audio.channels]);
    chan.u32be(0);
    chan.u32be(0);
    entry.bytes(full_box("chan", 0, 0, chan.view()));
  }
  util::ByteWriter stsd;
  stsd.u32be(1);
  write_box(stsd, "alac", entry.view());
  const bool large = data_size + 4096 > 0xFFFFFFF0ull;
  const auto build_moov = [&](std::uint64_t mdat_data_offset) {
    util::ByteWriter stbl;
    stbl.bytes(full_box("stsd", 0, 0, stsd.view()));
    stbl.bytes(full_box("stts", 0, 0, stts.view()));
    stbl.bytes(full_box("stsc", 0, 0, stsc.view()));
    stbl.bytes(full_box("stsz", 0, 0, stsz.view()));
    util::ByteWriter co;
    co.u32be(1);
    if (large) {
      co.u64be(mdat_data_offset);
      stbl.bytes(full_box("co64", 0, 0, co.view()));
    } else {
      co.u32be(static_cast<std::uint32_t>(mdat_data_offset));
      stbl.bytes(full_box("stco", 0, 0, co.view()));
    }
    util::ByteWriter dref;
    dref.u32be(1);
    dref.bytes(full_box("url ", 0, 1, {}));
    util::ByteWriter minf;
    util::ByteWriter smhd;
    smhd.u32be(0);
    minf.bytes(full_box("smhd", 0, 0, smhd.view()));
    minf.bytes(box("dinf", full_box("dref", 0, 0, dref.view())));
    write_box(minf, "stbl", stbl.view());
    const bool long_duration = frames > 0xFFFFFFFFull;
    util::ByteWriter mdhd;
    if (long_duration) {
      mdhd.zeros(16);
      mdhd.u32be(rate);
      mdhd.u64be(frames);
    } else {
      mdhd.zeros(8);
      mdhd.u32be(rate);
      mdhd.u32be(static_cast<std::uint32_t>(frames));
    }
    mdhd.u16be(0x55C4);  // "und"
    mdhd.u16be(0);
    util::ByteWriter hdlr;
    hdlr.u32be(0);
    hdlr.text("soun");
    hdlr.zeros(12);
    hdlr.u8(0);
    util::ByteWriter mdia;
    mdia.bytes(full_box("mdhd", long_duration ? 1 : 0, 0, mdhd.view()));
    mdia.bytes(full_box("hdlr", 0, 0, hdlr.view()));
    write_box(mdia, "minf", minf.view());
    const std::uint64_t movie_duration = rate == 0 ? 0 : frames * 1000 / rate;
    const auto matrix = [](util::ByteWriter& w) {
      w.u32be(0x00010000);
      w.zeros(12);
      w.u32be(0x00010000);
      w.zeros(12);
      w.u32be(0x40000000);
    };
    util::ByteWriter tkhd;
    tkhd.zeros(8);
    tkhd.u32be(1);
    tkhd.u32be(0);
    tkhd.u32be(static_cast<std::uint32_t>(std::min<std::uint64_t>(movie_duration, 0xFFFFFFFFu)));
    tkhd.zeros(8);
    tkhd.u16be(0);
    tkhd.u16be(0);
    tkhd.u16be(0x0100);
    tkhd.u16be(0);
    matrix(tkhd);
    tkhd.u32be(0);
    tkhd.u32be(0);
    util::ByteWriter trak;
    trak.bytes(full_box("tkhd", 0, 7, tkhd.view()));
    write_box(trak, "mdia", mdia.view());
    util::ByteWriter mvhd;
    mvhd.zeros(8);
    mvhd.u32be(1000);
    mvhd.u32be(static_cast<std::uint32_t>(std::min<std::uint64_t>(movie_duration, 0xFFFFFFFFu)));
    mvhd.u32be(0x00010000);
    mvhd.u16be(0x0100);
    mvhd.zeros(10);
    matrix(mvhd);
    mvhd.zeros(24);
    mvhd.u32be(2);
    util::ByteWriter moov;
    moov.bytes(full_box("mvhd", 0, 0, mvhd.view()));
    write_box(moov, "trak", trak.view());
    if (!options::resolved_strip_metadata(ctx.options)) {
      const Bytes ilst = ilst_box(audio.tags);
      if (!ilst.empty()) {
        util::ByteWriter meta_hdlr;
        meta_hdlr.u32be(0);
        meta_hdlr.text("mdir");
        meta_hdlr.text("appl");
        meta_hdlr.zeros(8);
        meta_hdlr.u8(0);
        util::ByteWriter meta;
        meta.bytes(full_box("hdlr", 0, 0, meta_hdlr.view()));
        meta.bytes(ilst);
        moov.bytes(box("udta", full_box("meta", 0, 0, meta.view())));
      }
    }
    return box("moov", moov.view());
  };
  util::ByteWriter ftyp;
  ftyp.text("M4A ");
  ftyp.u32be(0);
  ftyp.text("M4A ");
  ftyp.text("mp42");
  ftyp.text("isom");
  const Bytes ftyp_box = box("ftyp", ftyp.view());
  const std::size_t moov_size = build_moov(0).size();
  const std::uint64_t mdat_header = large ? 16 : 8;
  const std::uint64_t data_offset = ftyp_box.size() + moov_size + mdat_header;
  const Bytes moov = build_moov(data_offset);
  CVT_TRY(ctx.limits.check_output(data_offset + data_size));
  util::ByteWriter out(static_cast<std::size_t>(data_offset + data_size));
  out.bytes(ftyp_box);
  out.bytes(moov);
  if (large) {
    out.u32be(1);
    out.text("mdat");
    out.u64be(data_size + 16);
  } else {
    out.u32be(static_cast<std::uint32_t>(data_size + 8));
    out.text("mdat");
  }
  for (const Bytes& p : stream.packets) out.bytes(p);
  return out.take();
}

// The M4A brand alone does not say which codec is inside: look for 'alac'
// in the sample description.
int sniff_alac(ByteView data, std::string_view) {
  if (data.size() < 12 || as_chars(data.subspan(4, 4)) != "ftyp") return 0;
  const std::string_view text = as_chars(data.first(std::min<std::size_t>(data.size(), 1 << 20)));
  const std::size_t stsd = text.find("stsd");
  if (stsd == std::string_view::npos) {
    // moov may be at the end of the file.
    if (data.size() > (1 << 20)) {
      const std::string_view tail = as_chars(data.last(1 << 20));
      const std::size_t at = tail.find("stsd");
      return at != std::string_view::npos && tail.substr(at, 64).find("alac") != std::string_view::npos ? 97 : 0;
    }
    return 0;
  }
  return text.substr(stsd, 64).find("alac") != std::string_view::npos ? 97 : 0;
}

}  // namespace

void register_m4a(Registry& registry) {
  FormatInfo info;
  info.id = "alac";
  info.name = "Apple Lossless audio";
  info.description = "ALAC in an MPEG-4 (.m4a) file: lossless audio for Apple devices.";
  info.category = Category::Audio;
  info.extensions = {"m4a"};
  info.mime = "audio/mp4";
  info.popularity = 50;
  info.sniff = sniff_alac;
  registry.add_format(std::move(info));

  std::vector<OptionSpec> options = standard_write_options();
  options.insert(options.begin(), options::compression_level(9));
  options.insert(options.begin(), options::preset());
  options.insert(options.begin(), sample_format_option({"auto", "s16", "s20", "s24", "s32"}));
  options.push_back(dither_option());

  Codec alac_codec;
  alac_codec.format_id = "alac";
  alac_codec.kind = Kind::Audio;
  alac_codec.read = read_m4a;
  alac_codec.write = write_m4a;
  alac_codec.write_options = options;
  registry.add_codec(std::move(alac_codec));

  // Any audio-only MPEG-4 file: ALAC in-house, other codecs through the
  // media registry. Slightly more expensive than a dedicated reader so that
  // one registered by another family for the same format wins.
  Codec m4a_reader;
  m4a_reader.format_id = "m4a";
  m4a_reader.kind = Kind::Audio;
  m4a_reader.read = read_m4a;
  m4a_reader.read_cost = 12;
  registry.add_codec(std::move(m4a_reader));
}

}  // namespace cvt::audio
