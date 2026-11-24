// Convertible core - office family: OfficeArt (Escher) picture records.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/office/escher.hpp"

#include <algorithm>

#include "util/deflate.hpp"

namespace cvt::office {

namespace {

std::uint32_t le32(ByteView d, std::size_t at) {
  util::ByteReader r(d);
  if (!r.seek(at)) return 0;
  return r.u32le();
}
std::uint16_t le16(ByteView d, std::size_t at) {
  util::ByteReader r(d);
  if (!r.seek(at)) return 0;
  return r.u16le();
}

}  // namespace

std::optional<ir::EncodedImage> find_blip(ByteView data, std::size_t at, std::size_t end) {
  end = std::min(end, data.size());
  // Walk the OfficeArt records for the first blip.
  int guard = 0;
  while (at + 8 <= end && ++guard < 256) {
    const std::uint16_t ver_inst = le16(data, at);
    const std::uint16_t type = le16(data, at + 2);
    const std::uint32_t len = le32(data, at + 4);
    const std::uint16_t ver = ver_inst & 0x0F;
    const std::uint16_t inst = ver_inst >> 4;
    if (std::uint64_t{at} + 8 + len > end) break;
    if (ver == 0x0F) {
      at += 8;  // container: descend
      continue;
    }
    if (type == 0xF007) {  // OfficeArtFBSE: 36 bytes header then optional name, then the embedded blip
      const std::uint8_t name_len = std::to_integer<std::uint8_t>(data[at + 8 + 33]);
      at += 8 + 36 + name_len;
      continue;
    }
    if (type >= 0xF01A && type <= 0xF029) {
      const ByteView body = data.subspan(at + 8, len);
      ir::EncodedImage image;
      std::size_t header = 0;
      if (type == 0xF01E) {  // PNG
        header = (inst == 0x6E1 ? 32 : 16) + 1;
        image.mime = "image/png";
      } else if (type == 0xF01D || type == 0xF02A) {  // JPEG
        header = (inst == 0x46B || inst == 0x6E3 ? 32 : 16) + 1;
        image.mime = "image/jpeg";
      } else if (type == 0xF01F) {  // DIB -> BMP
        header = (inst == 0x7A9 ? 32 : 16) + 1;
        if (header >= body.size()) return std::nullopt;
        const ByteView dib = body.subspan(header);
        util::ByteWriter bmp;
        const std::uint32_t info = le32(dib, 0);
        const std::uint16_t bits = le16(dib, 14);
        const std::uint32_t colors = le32(dib, 32);
        const std::uint32_t palette = bits <= 8 ? 4 * (colors ? colors : (1u << bits)) : 0;
        bmp.text("BM").u32le(static_cast<std::uint32_t>(14 + dib.size())).u32le(0).u32le(14 + info + palette);
        bmp.bytes(dib);
        image.bytes = bmp.take();
        image.mime = "image/bmp";
        return image;
      } else if (type == 0xF01A || type == 0xF01B) {  // EMF / WMF: kept as bytes
        header = (inst == 0x3D5 || inst == 0x217 ? 32 : 16) + 34;
        image.mime = type == 0xF01A ? "image/x-emf" : "image/x-wmf";
        if (header >= body.size()) return std::nullopt;
        const std::uint8_t compression = std::to_integer<std::uint8_t>(body[header - 2]);
        const ByteView payload = body.subspan(header);
        if (compression == 0) {
          auto inflated = util::zlib_decompress(payload, 64 * 1024 * 1024);
          if (!inflated) return std::nullopt;
          image.bytes = std::move(*inflated);
        } else {
          image.bytes = to_bytes(payload);
        }
        return image;
      } else {
        return std::nullopt;
      }
      if (header >= body.size()) return std::nullopt;
      image.bytes = to_bytes(body.subspan(header));
      return image;
    }
    at += 8 + len;
  }
  return std::nullopt;
}

}  // namespace cvt::office
