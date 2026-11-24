// Convertible core - image_modern family: non-TIFF maker RAW containers.
//
// Fujifilm RAF (fixed big-endian header with the JPEG preview), Canon CR3
// (ISO base media file: full-size JPEG track, PRVW and THMB previews,
// orientation from the CMT1 TIFF structure) and Canon CRW (CIFF heaps, the
// JpgFromRaw record). These read the embedded previews; their sensor data
// (Fujifilm compressed/X-Trans layouts, Canon CRX, CRW Huffman) is not
// decoded.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#include "families/image_modern/raw_readers.hpp"

namespace cvt::image_modern::raw::detail {

namespace {

std::uint32_t be32(ByteView d, std::size_t at) {
  if (at > d.size() || d.size() - at < 4) return 0;
  const auto* p = reinterpret_cast<const std::uint8_t*>(d.data()) + at;
  return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}
std::uint64_t be64(ByteView d, std::size_t at) {
  return (std::uint64_t{be32(d, at)} << 32) | be32(d, at + 4);
}
std::uint32_t le32(ByteView d, std::size_t at) {
  if (at > d.size() || d.size() - at < 4) return 0;
  const auto* p = reinterpret_cast<const std::uint8_t*>(d.data()) + at;
  return (std::uint32_t{p[3]} << 24) | (std::uint32_t{p[2]} << 16) | (std::uint32_t{p[1]} << 8) | p[0];
}
std::uint16_t le16(ByteView d, std::size_t at) {
  if (at > d.size() || d.size() - at < 2) return 0;
  const auto* p = reinterpret_cast<const std::uint8_t*>(d.data()) + at;
  return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

ByteView sub(ByteView d, std::uint64_t at, std::uint64_t n) {
  if (at > d.size() || n > d.size() - at) return {};
  return d.subspan(static_cast<std::size_t>(at), static_cast<std::size_t>(n));
}

// The first JPEG (FF D8 FF) inside a range, to the end of the range.
ByteView find_jpeg(ByteView d) {
  const auto* p = reinterpret_cast<const std::uint8_t*>(d.data());
  for (std::size_t i = 0; i + 3 <= d.size() && i < 256; ++i) {
    if (p[i] == 0xFF && p[i + 1] == 0xD8 && p[i + 2] == 0xFF) return d.subspan(i);
  }
  return {};
}

// ---------------------------------------------------------------------------- ISO BMFF

struct Box {
  std::string_view type;
  ByteView payload;  // after the header (and the uuid for 'uuid' boxes)
  ByteView uuid;
};

std::vector<Box> boxes(ByteView d) {
  std::vector<Box> out;
  std::size_t at = 0;
  while (at + 8 <= d.size() && out.size() < 4096) {
    std::uint64_t size = be32(d, at);
    std::size_t header = 8;
    if (size == 1) {
      if (at + 16 > d.size()) break;
      size = be64(d, at + 8);
      header = 16;
    } else if (size == 0) {
      size = d.size() - at;
    }
    if (size < header || size > d.size() - at) break;
    Box box;
    box.type = as_chars(d.subspan(at + 4, 4));
    box.payload = d.subspan(at + header, static_cast<std::size_t>(size) - header);
    if (box.type == "uuid" && box.payload.size() >= 16) {
      box.uuid = box.payload.first(16);
      box.payload = box.payload.subspan(16);
    }
    out.push_back(box);
    at += static_cast<std::size_t>(size);
  }
  return out;
}

const Box* child(const std::vector<Box>& list, std::string_view type) {
  for (const Box& b : list) {
    if (b.type == type) return &b;
  }
  return nullptr;
}

// Offset and size of the first sample of a 'trak'.
std::optional<std::pair<std::uint64_t, std::uint64_t>> first_sample(const Box& trak) {
  const auto t = boxes(trak.payload);
  const Box* mdia = child(t, "mdia");
  if (mdia == nullptr) return std::nullopt;
  const auto m = boxes(mdia->payload);
  const Box* minf = child(m, "minf");
  if (minf == nullptr) return std::nullopt;
  const auto mi = boxes(minf->payload);
  const Box* stbl = child(mi, "stbl");
  if (stbl == nullptr) return std::nullopt;
  const auto st = boxes(stbl->payload);
  const Box* stsz = child(st, "stsz");
  if (stsz == nullptr || stsz->payload.size() < 12) return std::nullopt;
  std::uint64_t size = be32(stsz->payload, 4);
  if (size == 0) {
    if (be32(stsz->payload, 8) == 0 || stsz->payload.size() < 16) return std::nullopt;
    size = be32(stsz->payload, 12);
  }
  std::uint64_t offset = 0;
  if (const Box* stco = child(st, "stco"); stco != nullptr && stco->payload.size() >= 12 && be32(stco->payload, 4) > 0) {
    offset = be32(stco->payload, 8);
  } else if (const Box* co64 = child(st, "co64"); co64 != nullptr && co64->payload.size() >= 16 && be32(co64->payload, 4) > 0) {
    offset = be64(co64->payload, 8);
  } else {
    return std::nullopt;
  }
  return std::make_pair(offset, size);
}

// Canon's metadata uuid (85c0b687-820f-11e0-8111-f4ce462b6a48) and preview
// uuid (eaf42b5e-1c98-4b88-b9fb-b7dc406e4d16).
constexpr std::uint8_t canon_uuid[16] = {0x85, 0xc0, 0xb6, 0x87, 0x82, 0x0f, 0x11, 0xe0,
                                         0x81, 0x11, 0xf4, 0xce, 0x46, 0x2b, 0x6a, 0x48};
constexpr std::uint8_t preview_uuid[16] = {0xea, 0xf4, 0x2b, 0x5e, 0x1c, 0x98, 0x4b, 0x88,
                                           0xb9, 0xfb, 0xb7, 0xdc, 0x40, 0x6e, 0x4d, 0x16};

bool is_uuid(const Box& b, const std::uint8_t (&id)[16]) {
  return b.uuid.size() == 16 && std::memcmp(b.uuid.data(), id, 16) == 0;
}

// ---------------------------------------------------------------------------- CIFF

void ciff_heap(ByteView data, std::uint64_t start, std::uint64_t length, int depth, std::vector<Preview>& out) {
  if (depth > 4 || length < 6) return;
  const ByteView heap = sub(data, start, length);
  if (heap.empty()) return;
  const std::uint32_t table = le32(heap, heap.size() - 4);
  if (table >= heap.size() - 4) return;
  const std::uint16_t count = le16(heap, table);
  for (std::uint32_t i = 0; i < count && i < 512; ++i) {
    const std::size_t rec = table + 2 + std::size_t{i} * 10;
    if (rec + 10 > heap.size() - 4) break;
    const std::uint16_t tag = le16(heap, rec);
    const std::uint32_t size = le32(heap, rec + 2);
    const std::uint32_t offset = le32(heap, rec + 6);
    if ((tag >> 14) != 0) continue;  // value stored in the record itself
    const std::uint16_t data_type = tag & 0x3800;
    if (data_type == 0x2800 || data_type == 0x3000) {
      ciff_heap(data, start + offset, size, depth + 1, out);
    } else if ((tag & 0x3FFF) == 0x2007) {
      add_jpeg_preview(sub(heap, offset, size), 1, out);
    }
  }
}

}  // namespace

Result<ir::Image> read_raf(ByteView data, const Request& request, const Context& ctx) {
  if (data.size() < 108 || as_chars(data.first(16)) != "FUJIFILMCCD-RAW ") return fail(Status::Decode, "not a Fujifilm RAF file");
  std::vector<Preview> previews;
  add_jpeg_preview(sub(data, be32(data, 84), be32(data, 88)), 1, previews);
  if (request.preview) return decode_largest_preview(previews, ctx);
  return preview_fallback(previews, "Fujifilm RAF sensor data is not supported", ctx);
}

Result<ir::Image> read_cr3(ByteView data, const Request& request, const Context& ctx) {
  const auto top = boxes(data);
  const Box* ftyp = child(top, "ftyp");
  if (ftyp == nullptr || ftyp->payload.size() < 4 || as_chars(ftyp->payload.first(4)) != "crx ") {
    return fail(Status::Decode, "not a Canon CR3 file");
  }
  std::vector<Preview> previews;
  int orientation = 1;
  for (const Box& b : top) {
    if (b.type == "moov") {
      for (const Box& m : boxes(b.payload)) {
        if (m.type == "trak") {
          if (const auto sample = first_sample(m)) add_jpeg_preview(sub(data, sample->first, sample->second), 1, previews);
        } else if (is_uuid(m, canon_uuid)) {
          for (const Box& c : boxes(m.payload)) {
            if (c.type == "CMT1") {
              if (const auto tree = parse_tree(c.payload, 0, 4)) {
                orientation = static_cast<int>(tree->tiff.integer(tree->ifds.front(), 274, 1));
              }
            } else if (c.type == "THMB") {
              add_jpeg_preview(find_jpeg(c.payload), 1, previews);
            }
          }
        }
      }
    } else if (is_uuid(b, preview_uuid)) {
      for (const Box& c : boxes(b.payload.size() > 8 ? b.payload.subspan(8) : ByteView{})) {
        if (c.type == "PRVW") add_jpeg_preview(find_jpeg(c.payload), 1, previews);
      }
    }
  }
  for (Preview& p : previews) p.orientation = orientation;
  if (request.preview) return decode_largest_preview(previews, ctx);
  return preview_fallback(previews, "Canon CR3 (CRX) sensor data is not supported", ctx);
}

Result<ir::Image> read_crw(ByteView data, const Request& request, const Context& ctx) {
  if (data.size() < 26 || as_chars(data.subspan(6, 8)) != "HEAPCCDR") return fail(Status::Decode, "not a Canon CRW file");
  const std::uint32_t header = le32(data, 2);
  if (header < 14 || header >= data.size()) return fail(Status::Decode, "invalid CRW header");
  std::vector<Preview> previews;
  ciff_heap(data, header, data.size() - header, 0, previews);
  if (request.preview) return decode_largest_preview(previews, ctx);
  return preview_fallback(previews, "Canon CRW sensor data is not supported", ctx);
}

}  // namespace cvt::image_modern::raw::detail
