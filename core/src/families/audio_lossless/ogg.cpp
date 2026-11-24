// Convertible core - audio_lossless family: minimal Ogg page demultiplexer.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/audio_lossless/ogg.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace cvt::audio {

namespace {

constexpr std::array<std::uint32_t, 256> make_table() {
  std::array<std::uint32_t, 256> t{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i << 24;
    for (int k = 0; k < 8; ++k) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : (c << 1);
    t[i] = c;
  }
  return t;
}
constexpr std::array<std::uint32_t, 256> crc_table = make_table();

}  // namespace

std::uint32_t ogg_crc(ByteView page) noexcept {
  std::uint32_t c = 0;
  for (std::size_t i = 0; i < page.size(); ++i) {
    const std::uint32_t b = (i >= 22 && i < 26) ? 0u : std::to_integer<std::uint32_t>(page[i]);
    c = (c << 8) ^ crc_table[((c >> 24) ^ b) & 0xFFu];
  }
  return c;
}

Result<OggStream> read_ogg_stream(ByteView input, std::string_view magic, const Context& ctx) {
  OggStream out;
  std::optional<std::uint32_t> serial;
  std::optional<std::uint32_t> expected_sequence;
  Bytes partial;
  bool partial_valid = false;
  std::uint64_t total = 0;
  std::size_t pos = 0;
  while (pos + 27 <= input.size()) {
    util::ByteReader r(input.subspan(pos));
    if (!r.consume("OggS")) {
      out.damaged = true;
      ++pos;
      while (pos + 4 <= input.size() && as_chars(input.subspan(pos, 4)) != "OggS") ++pos;
      continue;
    }
    const std::uint8_t version = r.u8();
    const std::uint8_t type = r.u8();
    r.skip(8);  // granule position
    const std::uint32_t page_serial = r.u32le();
    const std::uint32_t sequence = r.u32le();
    const std::uint32_t crc = r.u32le();
    const std::uint8_t segments = r.u8();
    const ByteView lacing = r.bytes(segments);
    if (!r.ok() || version != 0) {
      out.damaged = true;
      ++pos;
      continue;
    }
    std::size_t body = 0;
    for (const std::byte b : lacing) body += std::to_integer<std::uint8_t>(b);
    const std::size_t header = 27 + std::size_t{segments};
    if (pos + header + body > input.size()) {
      out.damaged = true;
      break;
    }
    const ByteView page = input.subspan(pos, header + body);
    pos += header + body;
    if (ogg_crc(page) != crc) {
      out.damaged = true;
      partial_valid = false;
      continue;
    }
    const ByteView data = page.subspan(header);
    if (!serial) {
      if ((type & 0x02) == 0 || data.size() < magic.size() || as_chars(data.first(magic.size())) != magic) continue;
      serial = page_serial;
    }
    if (page_serial != *serial) continue;
    if (expected_sequence && sequence != *expected_sequence) {
      out.damaged = true;
      partial_valid = false;
    }
    expected_sequence = sequence + 1;
    if ((type & 0x01) == 0) {
      partial.clear();
      partial_valid = true;
    }
    std::size_t at = 0;
    for (std::size_t i = 0; i < segments; ++i) {
      const std::size_t length = std::to_integer<std::uint8_t>(lacing[i]);
      partial.insert(partial.end(), data.begin() + static_cast<std::ptrdiff_t>(at),
                     data.begin() + static_cast<std::ptrdiff_t>(at + length));
      at += length;
      total += length;
      if (length < 255) {
        if (partial_valid) out.packets.push_back(std::move(partial));
        partial = Bytes{};
        partial_valid = true;
      }
    }
    CVT_TRY(ctx.limits.check_decompressed(total));
    if ((type & 0x04) != 0) break;  // end of stream
  }
  return out;
}

}  // namespace cvt::audio
