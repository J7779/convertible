// Convertible core - image_modern family: ISO base media file parsing for
// HEIF images (ISO/IEC 14496-12 boxes, ISO/IEC 23008-12 items) and a small
// box writer.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "families/image_modern/heif_internal.hpp"
#include "util/bytes.hpp"

namespace cvt::image_modern::heif {

std::string fourcc_text(std::uint32_t code) {
  std::string text(4, '?');
  for (std::size_t i = 0; i < 4; ++i) {
    const auto c = static_cast<unsigned char>((code >> (24 - 8 * i)) & 0xFFu);
    if (c >= 0x20 && c < 0x7F) text[i] = static_cast<char>(c);
  }
  return text;
}

namespace {

constexpr std::size_t max_boxes_per_level = 1u << 16;

struct Box {
  std::uint32_t type = 0;
  ByteView body;
};

/// The boxes directly inside `data`.
Result<std::vector<Box>> list_boxes(ByteView data, std::string_view where) {
  std::vector<Box> out;
  std::size_t at = 0;
  while (at < data.size()) {
    if (data.size() - at < 8) return fail(Status::Decode, "truncated box header in {}", where);
    util::ByteReader r(data.subspan(at));
    std::uint64_t size = r.u32be();
    const std::uint32_t type = r.u32be();
    std::uint64_t header = 8;
    if (size == 1) {
      if (r.remaining() < 8) return fail(Status::Decode, "truncated box header in {}", where);
      size = r.u64be();
      header = 16;
    } else if (size == 0) {
      size = data.size() - at;
    }
    if (type == fourcc("uuid")) header += 16;
    if (size < header || size > data.size() - at) {
      return fail(Status::Decode, "box '{}' in {} has an invalid size", fourcc_text(type), where);
    }
    out.push_back({type, data.subspan(at + static_cast<std::size_t>(header), static_cast<std::size_t>(size - header))});
    if (out.size() > max_boxes_per_level) return fail(Status::Limit, "too many boxes in {}", where);
    at += static_cast<std::size_t>(size);
  }
  return out;
}

/// A full box: version, flags and a reader positioned after them.
struct FullBox {
  std::uint8_t version = 0;
  std::uint32_t flags = 0;
  util::ByteReader r;
};
Result<FullBox> full_box(ByteView body, std::string_view name) {
  if (body.size() < 4) return fail(Status::Decode, "'{}' box is truncated", name);
  util::ByteReader r(body);
  FullBox box;
  box.version = r.u8();
  box.flags = r.u24be();
  box.r = util::ByteReader(r.rest());
  return box;
}

/// An unsigned field of 0, 4 or 8 bytes (iloc).
bool read_sized(util::ByteReader& r, unsigned bytes, std::uint64_t& value) {
  switch (bytes) {
    case 0: value = 0; return true;
    case 4: value = r.u32be(); return r.ok();
    case 8: value = r.u64be(); return r.ok();
    default: return false;
  }
}

/// A NUL-terminated string, or the rest of the data when the NUL is missing.
std::string read_string(util::ByteReader& r) {
  const ByteView rest = r.peek_rest();
  const std::string_view text = as_chars(rest);
  const std::size_t end = text.find('\0');
  if (end == std::string_view::npos) {
    (void)r.rest();
    return std::string(text);
  }
  (void)r.bytes(end + 1);
  return std::string(text.substr(0, end));
}

Result<void> parse_iinf(ByteView body, BmffFile& file, const Limits& limits) {
  CVT_TRY_ASSIGN(FullBox iinf, full_box(body, "iinf"));
  const std::uint32_t count = iinf.version == 0 ? iinf.r.u16be() : iinf.r.u32be();
  if (!iinf.r.ok()) return fail(Status::Decode, "'iinf' box is truncated");
  CVT_TRY(limits.check_archive_entries(count));
  CVT_TRY_ASSIGN(const std::vector<Box> entries, list_boxes(iinf.r.rest(), "iinf"));
  for (const Box& entry : entries) {
    if (entry.type != fourcc("infe")) continue;
    CVT_TRY_ASSIGN(FullBox infe, full_box(entry.body, "infe"));
    Item item;
    if (infe.version >= 2) {
      item.id = infe.version == 2 ? infe.r.u16be() : infe.r.u32be();
      (void)infe.r.u16be();  // item_protection_index
      item.type = infe.r.u32be();
      if (!infe.r.ok()) return fail(Status::Decode, "'infe' box is truncated");
      item.name = read_string(infe.r);
      if (item.type == fourcc("mime")) {
        item.content_type = read_string(infe.r);
        if (!infe.r.at_end()) item.content_encoding = read_string(infe.r);
      }
    } else {
      item.id = infe.r.u16be();
      (void)infe.r.u16be();
      if (!infe.r.ok()) return fail(Status::Decode, "'infe' box is truncated");
      item.name = read_string(infe.r);
      item.content_type = read_string(infe.r);
    }
    item.hidden = (infe.flags & 1u) != 0;
    if (file.item(item.id) != nullptr) continue;  // first definition wins
    file.items.push_back(std::move(item));
  }
  CVT_TRY(limits.check_archive_entries(file.items.size()));
  return {};
}

Result<void> parse_iloc(ByteView body, BmffFile& file) {
  CVT_TRY_ASSIGN(FullBox iloc, full_box(body, "iloc"));
  if (iloc.version > 2) return fail(Status::Decode, "unsupported 'iloc' version {}", iloc.version);
  util::ByteReader& r = iloc.r;
  const std::uint8_t sizes = r.u8();
  const std::uint8_t sizes2 = r.u8();
  const unsigned offset_size = sizes >> 4;
  const unsigned length_size = sizes & 15u;
  const unsigned base_offset_size = sizes2 >> 4;
  const unsigned index_size = iloc.version >= 1 ? (sizes2 & 15u) : 0u;
  const std::uint32_t count = iloc.version < 2 ? r.u16be() : r.u32be();
  if (!r.ok()) return fail(Status::Decode, "'iloc' box is truncated");
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint32_t id = iloc.version < 2 ? r.u16be() : r.u32be();
    std::uint8_t method = 0;
    if (iloc.version >= 1) method = static_cast<std::uint8_t>(r.u16be() & 15u);
    const std::uint16_t data_ref = r.u16be();
    std::uint64_t base = 0;
    if (!read_sized(r, base_offset_size, base)) return fail(Status::Decode, "malformed 'iloc' box");
    const std::uint16_t extents = r.u16be();
    if (!r.ok()) return fail(Status::Decode, "'iloc' box is truncated");
    // Every extent needs at least offset_size + length_size bytes: bound the
    // allocation by what the box can hold.
    const std::size_t per_extent = std::max<std::size_t>(1, offset_size + length_size + index_size);
    if (std::size_t{extents} > r.remaining() / per_extent + 1) return fail(Status::Decode, "'iloc' box is truncated");
    Item* item = nullptr;
    for (Item& candidate : file.items) {
      if (candidate.id == id) item = &candidate;
    }
    std::vector<Extent> list;
    list.reserve(extents);
    for (std::uint16_t e = 0; e < extents; ++e) {
      std::uint64_t index = 0;
      std::uint64_t offset = 0;
      std::uint64_t length = 0;
      if (!read_sized(r, index_size, index) || !read_sized(r, offset_size, offset) ||
          !read_sized(r, length_size, length)) {
        return fail(Status::Decode, "malformed 'iloc' box");
      }
      if (offset > std::numeric_limits<std::uint64_t>::max() - base) return fail(Status::Decode, "bad item offset");
      list.push_back({base + offset, length});
    }
    if (item == nullptr || item->has_location) continue;
    item->has_location = true;
    item->construction_method = method;
    item->data_reference_index = data_ref;
    item->extents = std::move(list);
  }
  return {};
}

Result<void> parse_iref(ByteView body, BmffFile& file, const Limits& limits) {
  CVT_TRY_ASSIGN(FullBox iref, full_box(body, "iref"));
  CVT_TRY_ASSIGN(const std::vector<Box> refs, list_boxes(iref.r.rest(), "iref"));
  std::uint64_t total = 0;
  for (const Box& box : refs) {
    util::ByteReader r(box.body);
    Reference ref;
    ref.type = box.type;
    ref.from = iref.version == 0 ? r.u16be() : r.u32be();
    const std::uint16_t count = r.u16be();
    if (!r.ok()) return fail(Status::Decode, "'iref' entry is truncated");
    if (std::size_t{count} * (iref.version == 0 ? 2u : 4u) > r.remaining()) {
      return fail(Status::Decode, "'iref' entry is truncated");
    }
    total += count;
    CVT_TRY(limits.check_archive_entries(total));
    for (std::uint16_t i = 0; i < count; ++i) ref.to.push_back(iref.version == 0 ? r.u16be() : r.u32be());
    file.references.push_back(std::move(ref));
  }
  return {};
}

Result<void> parse_iprp(ByteView body, BmffFile& file, const Limits& limits) {
  CVT_TRY_ASSIGN(const std::vector<Box> children, list_boxes(body, "iprp"));
  for (const Box& child : children) {
    if (child.type == fourcc("ipco")) {
      if (!file.properties.empty()) continue;  // only one container is allowed
      CVT_TRY_ASSIGN(const std::vector<Box> props, list_boxes(child.body, "ipco"));
      for (const Box& p : props) file.properties.push_back({p.type, p.body});
    }
  }
  for (const Box& child : children) {
    if (child.type != fourcc("ipma")) continue;
    CVT_TRY_ASSIGN(FullBox ipma, full_box(child.body, "ipma"));
    util::ByteReader& r = ipma.r;
    const std::uint32_t count = r.u32be();
    if (!r.ok()) return fail(Status::Decode, "'ipma' box is truncated");
    CVT_TRY(limits.check_archive_entries(count));
    for (std::uint32_t i = 0; i < count; ++i) {
      const std::uint32_t id = ipma.version < 1 ? r.u16be() : r.u32be();
      const std::uint8_t links = r.u8();
      if (!r.ok()) return fail(Status::Decode, "'ipma' box is truncated");
      Item* item = nullptr;
      for (Item& candidate : file.items) {
        if (candidate.id == id) item = &candidate;
      }
      std::vector<PropertyLink> list;
      for (std::uint8_t k = 0; k < links; ++k) {
        PropertyLink link;
        if ((ipma.flags & 1u) != 0) {
          const std::uint16_t v = r.u16be();
          link.essential = (v & 0x8000u) != 0;
          link.index = static_cast<std::uint16_t>(v & 0x7FFFu);
        } else {
          const std::uint8_t v = r.u8();
          link.essential = (v & 0x80u) != 0;
          link.index = static_cast<std::uint16_t>(v & 0x7Fu);
        }
        if (!r.ok()) return fail(Status::Decode, "'ipma' box is truncated");
        if (link.index > file.properties.size()) {
          return fail(Status::Decode, "item {} refers to missing property {}", id, link.index);
        }
        list.push_back(link);
      }
      if (item != nullptr && item->properties.empty()) item->properties = std::move(list);
    }
  }
  return {};
}

Result<void> parse_meta(ByteView body, BmffFile& file, const Limits& limits) {
  CVT_TRY_ASSIGN(FullBox meta, full_box(body, "meta"));
  CVT_TRY_ASSIGN(const std::vector<Box> children, list_boxes(meta.r.rest(), "meta"));
  bool picture_handler = false;
  for (const Box& child : children) {
    if (child.type == fourcc("hdlr")) {
      util::ByteReader r(child.body);
      (void)r.u32be();  // version + flags
      (void)r.u32be();  // pre_defined
      picture_handler = r.ok() && r.u32be() == fourcc("pict") && r.ok();
    }
  }
  if (!picture_handler) return {};  // a 'meta' box that does not describe images
  file.has_meta = true;
  // iinf first: iloc and ipma refer to the items it declares.
  for (const Box& child : children) {
    if (child.type == fourcc("iinf")) {
      CVT_TRY(parse_iinf(child.body, file, limits));
      break;
    }
  }
  for (const Box& child : children) {
    if (child.type == fourcc("pitm")) {
      CVT_TRY_ASSIGN(FullBox pitm, full_box(child.body, "pitm"));
      file.primary = pitm.version == 0 ? pitm.r.u16be() : pitm.r.u32be();
      if (!pitm.r.ok()) return fail(Status::Decode, "'pitm' box is truncated");
    } else if (child.type == fourcc("iloc")) {
      CVT_TRY(parse_iloc(child.body, file));
    } else if (child.type == fourcc("iref")) {
      CVT_TRY(parse_iref(child.body, file, limits));
    } else if (child.type == fourcc("iprp")) {
      CVT_TRY(parse_iprp(child.body, file, limits));
    } else if (child.type == fourcc("idat")) {
      file.idat = child.body;
    }
  }
  return {};
}

// ---------------------------------------------------------------------------------------- tracks

const Box* find_box(const std::vector<Box>& boxes, std::uint32_t type) {
  for (const Box& box : boxes) {
    if (box.type == type) return &box;
  }
  return nullptr;
}

Result<std::vector<Box>> children_of(const std::vector<Box>& boxes, std::uint32_t type) {
  const Box* box = find_box(boxes, type);
  if (box == nullptr) return std::vector<Box>{};
  return list_boxes(box->body, fourcc_text(type));
}

struct ChunkRun {
  std::uint32_t first_chunk = 0;
  std::uint32_t samples_per_chunk = 0;
};

Result<void> parse_sample_table(const std::vector<Box>& stbl, Track& track, ByteView data, const Limits& limits) {
  // Sample description: the first entry's coding type and configuration.
  if (const Box* stsd = find_box(stbl, fourcc("stsd")); stsd != nullptr) {
    CVT_TRY_ASSIGN(FullBox box, full_box(stsd->body, "stsd"));
    (void)box.r.u32be();
    CVT_TRY_ASSIGN(const std::vector<Box> entries, list_boxes(box.r.rest(), "stsd"));
    if (!entries.empty()) {
      const Box& entry = entries.front();
      track.codec = entry.type;
      constexpr std::size_t visual_entry_size = 78;
      if (entry.body.size() >= visual_entry_size) {
        util::ByteReader r(entry.body.subspan(24));
        track.width = r.u16be();
        track.height = r.u16be();
        CVT_TRY_ASSIGN(const std::vector<Box> inner, list_boxes(entry.body.subspan(visual_entry_size), "sample entry"));
        for (const Box& b : inner) {
          if (b.type == fourcc("av1C") || b.type == fourcc("hvcC")) {
            track.config = b.body;
          } else if (b.type == fourcc("colr")) {
            if (const auto colr = parse_colr(b.body)) {
              if (colr->nclx && !track.nclx) track.nclx = colr->nclx;
              if (!colr->icc.empty() && track.icc.empty()) track.icc = colr->icc;
            }
          } else if (b.type == fourcc("auxi")) {
            track.aux_type = parse_aux_type(b.body);
          }
        }
      }
    }
  }

  const Box* stsz = find_box(stbl, fourcc("stsz"));
  const Box* stz2 = find_box(stbl, fourcc("stz2"));
  std::vector<std::uint32_t> sizes;
  if (stsz != nullptr) {
    CVT_TRY_ASSIGN(FullBox box, full_box(stsz->body, "stsz"));
    const std::uint32_t fixed = box.r.u32be();
    const std::uint32_t count = box.r.u32be();
    if (!box.r.ok()) return fail(Status::Decode, "'stsz' box is truncated");
    CVT_TRY(limits.check_archive_entries(count));
    if (fixed == 0 && std::uint64_t{count} * 4 > box.r.remaining()) return fail(Status::Decode, "'stsz' is truncated");
    sizes.resize(count, fixed);
    if (fixed == 0) {
      for (std::uint32_t& s : sizes) s = box.r.u32be();
    }
  } else if (stz2 != nullptr) {
    CVT_TRY_ASSIGN(FullBox box, full_box(stz2->body, "stz2"));
    (void)box.r.u24be();
    const std::uint8_t field = box.r.u8();
    const std::uint32_t count = box.r.u32be();
    if (!box.r.ok() || (field != 4 && field != 8 && field != 16)) return fail(Status::Decode, "malformed 'stz2' box");
    CVT_TRY(limits.check_archive_entries(count));
    if ((std::uint64_t{count} * field + 7) / 8 > box.r.remaining()) return fail(Status::Decode, "'stz2' is truncated");
    sizes.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      if (field == 16) {
        sizes[i] = box.r.u16be();
      } else if (field == 8) {
        sizes[i] = box.r.u8();
      } else {
        const std::uint8_t pair = (i % 2 == 0) ? box.r.peek_u8() : box.r.u8();
        sizes[i] = (i % 2 == 0) ? (pair >> 4) : (pair & 15u);
      }
    }
  } else {
    return {};  // no samples
  }

  std::vector<std::uint64_t> chunks;
  if (const Box* stco = find_box(stbl, fourcc("stco")); stco != nullptr) {
    CVT_TRY_ASSIGN(FullBox box, full_box(stco->body, "stco"));
    const std::uint32_t count = box.r.u32be();
    if (!box.r.ok() || std::uint64_t{count} * 4 > box.r.remaining()) return fail(Status::Decode, "'stco' is truncated");
    CVT_TRY(limits.check_archive_entries(count));
    chunks.resize(count);
    for (std::uint64_t& c : chunks) c = box.r.u32be();
  } else if (const Box* co64 = find_box(stbl, fourcc("co64")); co64 != nullptr) {
    CVT_TRY_ASSIGN(FullBox box, full_box(co64->body, "co64"));
    const std::uint32_t count = box.r.u32be();
    if (!box.r.ok() || std::uint64_t{count} * 8 > box.r.remaining()) return fail(Status::Decode, "'co64' is truncated");
    CVT_TRY(limits.check_archive_entries(count));
    chunks.resize(count);
    for (std::uint64_t& c : chunks) c = box.r.u64be();
  } else {
    return fail(Status::Decode, "the image sequence has no chunk offsets");
  }

  std::vector<ChunkRun> runs;
  if (const Box* stsc = find_box(stbl, fourcc("stsc")); stsc != nullptr) {
    CVT_TRY_ASSIGN(FullBox box, full_box(stsc->body, "stsc"));
    const std::uint32_t count = box.r.u32be();
    if (!box.r.ok() || std::uint64_t{count} * 12 > box.r.remaining()) return fail(Status::Decode, "'stsc' is truncated");
    CVT_TRY(limits.check_archive_entries(count));
    for (std::uint32_t i = 0; i < count; ++i) {
      ChunkRun run;
      run.first_chunk = box.r.u32be();
      run.samples_per_chunk = box.r.u32be();
      (void)box.r.u32be();
      if (run.first_chunk == 0 || (!runs.empty() && run.first_chunk <= runs.back().first_chunk)) {
        return fail(Status::Decode, "malformed 'stsc' box");
      }
      runs.push_back(run);
    }
  }
  if (runs.empty()) return fail(Status::Decode, "the image sequence has no sample-to-chunk table");

  track.samples.resize(sizes.size());
  std::size_t sample = 0;
  for (std::size_t chunk = 0; chunk < chunks.size() && sample < sizes.size(); ++chunk) {
    std::uint32_t per_chunk = 0;
    for (const ChunkRun& run : runs) {
      if (run.first_chunk <= chunk + 1) per_chunk = run.samples_per_chunk;
    }
    std::uint64_t offset = chunks[chunk];
    for (std::uint32_t k = 0; k < per_chunk && sample < sizes.size(); ++k, ++sample) {
      const std::uint32_t size = sizes[sample];
      if (offset > data.size() || size > data.size() - offset) {
        return fail(Status::Decode, "sample {} lies outside the file", sample + 1);
      }
      track.samples[sample].offset = offset;
      track.samples[sample].size = size;
      offset += size;
    }
  }
  if (sample < sizes.size()) return fail(Status::Decode, "the chunk table does not cover all samples");

  if (const Box* stts = find_box(stbl, fourcc("stts")); stts != nullptr) {
    CVT_TRY_ASSIGN(FullBox box, full_box(stts->body, "stts"));
    const std::uint32_t count = box.r.u32be();
    if (!box.r.ok() || std::uint64_t{count} * 8 > box.r.remaining()) return fail(Status::Decode, "'stts' is truncated");
    std::size_t at = 0;
    std::uint32_t last = 0;
    for (std::uint32_t i = 0; i < count && at < track.samples.size(); ++i) {
      const std::uint32_t n = box.r.u32be();
      last = box.r.u32be();
      for (std::uint32_t k = 0; k < n && at < track.samples.size(); ++k) track.samples[at++].duration = last;
    }
    for (; at < track.samples.size(); ++at) track.samples[at].duration = last;
  }
  if (const Box* stss = find_box(stbl, fourcc("stss")); stss != nullptr) {
    CVT_TRY_ASSIGN(FullBox box, full_box(stss->body, "stss"));
    const std::uint32_t count = box.r.u32be();
    if (!box.r.ok() || std::uint64_t{count} * 4 > box.r.remaining()) return fail(Status::Decode, "'stss' is truncated");
    for (Sample& s : track.samples) s.sync = false;
    for (std::uint32_t i = 0; i < count; ++i) {
      const std::uint32_t number = box.r.u32be();
      if (number >= 1 && number <= track.samples.size()) track.samples[number - 1].sync = true;
    }
  }
  return {};
}

Result<void> parse_moov(ByteView body, BmffFile& file, const Limits& limits) {
  CVT_TRY_ASSIGN(const std::vector<Box> moov, list_boxes(body, "moov"));
  for (const Box& trak_box : moov) {
    if (trak_box.type != fourcc("trak")) continue;
    if (file.tracks.size() >= 64) return fail(Status::Limit, "too many tracks");
    CVT_TRY_ASSIGN(const std::vector<Box> trak, list_boxes(trak_box.body, "trak"));
    Track track;
    std::uint64_t track_duration = 0;
    bool indefinite = false;
    if (const Box* tkhd = find_box(trak, fourcc("tkhd")); tkhd != nullptr) {
      CVT_TRY_ASSIGN(FullBox box, full_box(tkhd->body, "tkhd"));
      if (box.version == 1) {
        (void)box.r.u64be();
        (void)box.r.u64be();
        track.id = box.r.u32be();
        (void)box.r.u32be();
        track_duration = box.r.u64be();
        indefinite = track_duration == std::numeric_limits<std::uint64_t>::max();
      } else {
        (void)box.r.u32be();
        (void)box.r.u32be();
        track.id = box.r.u32be();
        (void)box.r.u32be();
        track_duration = box.r.u32be();
        indefinite = track_duration == 0xFFFFFFFFu;
      }
    }
    CVT_TRY_ASSIGN(const std::vector<Box> tref, children_of(trak, fourcc("tref")));
    for (const Box& ref : tref) {
      if (ref.type != fourcc("auxl")) continue;
      util::ByteReader r(ref.body);
      while (r.remaining() >= 4 && track.auxl.size() < 64) track.auxl.push_back(r.u32be());
    }
    CVT_TRY_ASSIGN(const std::vector<Box> edts, children_of(trak, fourcc("edts")));
    if (const Box* elst = find_box(edts, fourcc("elst")); elst != nullptr) {
      CVT_TRY_ASSIGN(FullBox box, full_box(elst->body, "elst"));
      const std::uint32_t count = box.r.u32be();
      const std::uint64_t segment = count == 0 ? 0 : (box.version == 1 ? box.r.u64be() : box.r.u32be());
      if ((box.flags & 1u) == 0) {
        track.plays = 1;
      } else if (indefinite || segment == 0) {
        track.plays = 0;
      } else {
        const std::uint64_t plays = (track_duration + segment - 1) / segment;
        track.plays = static_cast<std::uint32_t>(std::min<std::uint64_t>(plays, 0xFFFFu));
      }
    }
    CVT_TRY_ASSIGN(const std::vector<Box> mdia, children_of(trak, fourcc("mdia")));
    if (const Box* mdhd = find_box(mdia, fourcc("mdhd")); mdhd != nullptr) {
      CVT_TRY_ASSIGN(FullBox box, full_box(mdhd->body, "mdhd"));
      if (box.version == 1) {
        (void)box.r.u64be();
        (void)box.r.u64be();
      } else {
        (void)box.r.u32be();
        (void)box.r.u32be();
      }
      track.timescale = box.r.u32be();
    }
    if (const Box* hdlr = find_box(mdia, fourcc("hdlr")); hdlr != nullptr) {
      util::ByteReader r(hdlr->body);
      (void)r.u32be();
      (void)r.u32be();
      track.handler = r.u32be();
    }
    CVT_TRY_ASSIGN(const std::vector<Box> minf, children_of(mdia, fourcc("minf")));
    CVT_TRY_ASSIGN(const std::vector<Box> stbl, children_of(minf, fourcc("stbl")));
    CVT_TRY(parse_sample_table(stbl, track, file.data, limits));
    file.tracks.push_back(std::move(track));
  }
  return {};
}

}  // namespace

// ------------------------------------------------------------------------------------------- file

const Item* BmffFile::item(std::uint32_t id) const noexcept {
  for (const Item& candidate : items) {
    if (candidate.id == id) return &candidate;
  }
  return nullptr;
}

bool BmffFile::has_brand(std::uint32_t brand) const noexcept {
  return std::find(brands.begin(), brands.end(), brand) != brands.end();
}

std::vector<std::pair<const Property*, bool>> BmffFile::properties_of(const Item& of) const {
  std::vector<std::pair<const Property*, bool>> out;
  for (const PropertyLink& link : of.properties) {
    if (link.index == 0 || link.index > properties.size()) continue;
    out.emplace_back(&properties[link.index - 1u], link.essential);
  }
  return out;
}

const Property* BmffFile::property(const Item& of, std::uint32_t type) const {
  for (const auto& [prop, essential] : properties_of(of)) {
    if (prop->type == type) return prop;
  }
  return nullptr;
}

std::vector<std::uint32_t> BmffFile::refs_from(std::uint32_t from, std::uint32_t type) const {
  for (const Reference& ref : references) {
    if (ref.from == from && ref.type == type) return ref.to;
  }
  return {};
}

std::vector<std::uint32_t> BmffFile::refs_to(std::uint32_t to, std::uint32_t type) const {
  std::vector<std::uint32_t> out;
  for (const Reference& ref : references) {
    if (ref.type == type && std::find(ref.to.begin(), ref.to.end(), to) != ref.to.end()) out.push_back(ref.from);
  }
  return out;
}

Result<BmffFile> parse_file(ByteView data, const Limits& limits) {
  BmffFile file;
  file.data = data;
  CVT_TRY_ASSIGN(const std::vector<Box> top, list_boxes(data, "the file"));
  if (top.empty() || top.front().type != fourcc("ftyp")) return fail(Status::Decode, "not an ISO-BMFF file (no 'ftyp')");
  {
    util::ByteReader r(top.front().body);
    file.major_brand = r.u32be();
    (void)r.u32be();
    if (!r.ok()) return fail(Status::Decode, "'ftyp' box is truncated");
    file.brands.push_back(file.major_brand);
    while (r.remaining() >= 4 && file.brands.size() < 256) file.brands.push_back(r.u32be());
  }
  for (const Box& box : top) {
    if (box.type == fourcc("meta") && !file.has_meta) {
      CVT_TRY(parse_meta(box.body, file, limits));
    } else if (box.type == fourcc("moov") && file.tracks.empty()) {
      CVT_TRY(parse_moov(box.body, file, limits));
    }
  }
  if (!file.has_meta && file.tracks.empty()) return fail(Status::Decode, "the file holds no HEIF images");
  return file;
}

Result<ItemData> item_data(const BmffFile& file, const Item& item) {
  if (!item.has_location) return fail(Status::Decode, "item {} has no location", item.id);
  if (item.data_reference_index != 0) return fail(Status::Decode, "item {} is stored in another file", item.id);
  ByteView source;
  if (item.construction_method == 0) {
    source = file.data;
  } else if (item.construction_method == 1) {
    source = file.idat;
  } else {
    return fail(Status::Decode, "item {} uses an unsupported construction method {}", item.id,
                item.construction_method);
  }
  ItemData out;
  std::uint64_t total = 0;
  std::vector<ByteView> parts;
  for (const Extent& extent : item.extents) {
    if (extent.offset > source.size()) return fail(Status::Decode, "item {} lies outside the file", item.id);
    const std::uint64_t available = source.size() - extent.offset;
    const std::uint64_t length = extent.length == 0 ? available : extent.length;
    if (length > available) return fail(Status::Decode, "item {} lies outside the file", item.id);
    total += length;
    if (total > file.data.size()) return fail(Status::Decode, "item {} is larger than the file", item.id);
    parts.push_back(source.subspan(static_cast<std::size_t>(extent.offset), static_cast<std::size_t>(length)));
  }
  if (parts.size() == 1) {
    out.view = parts.front();
    return out;
  }
  out.owned.reserve(static_cast<std::size_t>(total));
  for (const ByteView part : parts) out.owned.insert(out.owned.end(), part.begin(), part.end());
  out.view = out.owned;
  return out;
}

// ----------------------------------------------------------------------------------- properties

std::optional<Ispe> parse_ispe(ByteView body) {
  util::ByteReader r(body);
  (void)r.u32be();
  Ispe ispe;
  ispe.width = r.u32be();
  ispe.height = r.u32be();
  if (!r.ok()) return std::nullopt;
  return ispe;
}

std::optional<Clap> parse_clap(ByteView body) {
  util::ByteReader r(body);
  Clap c;
  c.width_n = r.u32be();
  c.width_d = r.u32be();
  c.height_n = r.u32be();
  c.height_d = r.u32be();
  c.horiz_n = r.i32be();
  c.horiz_d = r.u32be();
  c.vert_n = r.i32be();
  c.vert_d = r.u32be();
  if (!r.ok()) return std::nullopt;
  return c;
}

std::optional<CropRect> clap_rect(const Clap& c, std::uint32_t width, std::uint32_t height) {
  if (c.width_d <= 0 || c.height_d <= 0 || c.horiz_d <= 0 || c.vert_d <= 0 || c.width_n <= 0 || c.height_n <= 0) {
    return std::nullopt;
  }
  const double clean_w = static_cast<double>(c.width_n) / static_cast<double>(c.width_d);
  const double clean_h = static_cast<double>(c.height_n) / static_cast<double>(c.height_d);
  const double off_x = static_cast<double>(c.horiz_n) / static_cast<double>(c.horiz_d);
  const double off_y = static_cast<double>(c.vert_n) / static_cast<double>(c.vert_d);
  // The aperture is centred on (width - 1) / 2 + off_x; its left edge is that
  // centre minus (clean_w - 1) / 2.
  const double left = std::floor(off_x + (static_cast<double>(width) - clean_w) / 2.0 + 0.5);
  const double top = std::floor(off_y + (static_cast<double>(height) - clean_h) / 2.0 + 0.5);
  const double w = std::floor(clean_w + 0.5);
  const double h = std::floor(clean_h + 0.5);
  if (!(left >= 0) || !(top >= 0) || !(w >= 1) || !(h >= 1) || left + w > width || top + h > height) {
    return std::nullopt;
  }
  return CropRect{static_cast<std::uint32_t>(left), static_cast<std::uint32_t>(top), static_cast<std::uint32_t>(w),
                  static_cast<std::uint32_t>(h)};
}

std::vector<std::uint8_t> parse_pixi(ByteView body) {
  util::ByteReader r(body);
  (void)r.u32be();
  const std::uint8_t channels = r.u8();
  std::vector<std::uint8_t> bits;
  for (std::uint8_t i = 0; i < channels && r.ok(); ++i) bits.push_back(r.u8());
  if (!r.ok()) return {};
  return bits;
}

std::optional<Colr> parse_colr(ByteView body) {
  util::ByteReader r(body);
  const std::uint32_t type = r.u32be();
  if (!r.ok()) return std::nullopt;
  Colr colr;
  if (type == fourcc("nclx") || type == fourcc("nclc")) {
    Cicp cicp;
    cicp.primaries = r.u16be();
    cicp.transfer = r.u16be();
    cicp.matrix = r.u16be();
    if (type == fourcc("nclx")) cicp.full_range = (r.u8() & 0x80u) != 0;
    if (!r.ok()) return std::nullopt;
    colr.nclx = cicp;
  } else if (type == fourcc("prof") || type == fourcc("rICC")) {
    colr.icc = r.rest();
  } else {
    return std::nullopt;
  }
  return colr;
}

std::string parse_aux_type(ByteView body) {
  util::ByteReader r(body);
  (void)r.u32be();  // version + flags
  if (!r.ok()) return {};
  return read_string(r);
}

int sniff_generic_heif(ByteView data) {
  util::ByteReader r(data);
  const std::uint32_t size = r.u32be();
  if (r.u32be() != fourcc("ftyp") || size < 16 || !r.ok()) return 0;
  const std::uint32_t major = r.u32be();
  (void)r.u32be();
  if (major != fourcc("mif1") && major != fourcc("msf1")) return 0;
  const std::size_t end = std::min<std::size_t>(size, data.size());
  for (std::size_t at = 16; at + 4 <= end; at += 4) {
    util::ByteReader b(data.subspan(at, 4));
    const std::uint32_t brand = b.u32be();
    for (const char* specific : {"heic", "heix", "heim", "heis", "hevc", "hevx", "hevm", "hevs", "avif", "avis"}) {
      if (brand == fourcc(specific)) return 0;
    }
  }
  return 90;
}

// ----------------------------------------------------------------------------------------- writer

std::size_t BoxWriter::begin(std::string_view type) {
  const std::size_t mark = out_.size();
  out_.u32be(0).text(type);
  return mark;
}

std::size_t BoxWriter::begin_full(std::string_view type, std::uint8_t version, std::uint32_t flags) {
  const std::size_t mark = begin(type);
  out_.u8(version).u24be(flags);
  return mark;
}

Result<void> BoxWriter::end(std::size_t mark) {
  const std::size_t size = out_.size() - mark;
  if (size > 0xFFFFFFFFu) return fail(Status::Limit, "a HEIF box would exceed 4 GiB");
  out_.patch_u32be(mark, static_cast<std::uint32_t>(size));
  return {};
}

}  // namespace cvt::image_modern::heif
