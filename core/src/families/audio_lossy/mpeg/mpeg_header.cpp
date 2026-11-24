// Convertible core - audio_lossy family: MPEG audio frame headers, stream
// scanning (frame sync with confirmation, free format, resynchronisation)
// and Xing/Info/LAME/VBRI information frames.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "families/audio_lossy/mpeg/mpeg_audio.hpp"
#include "families/audio_lossy/mpeg/mpeg_tables.hpp"

namespace cvt::lossy::mpeg {

std::size_t FrameHeader::frame_bytes(std::uint32_t kbps) const noexcept {
  if (sample_rate == 0 || kbps == 0) return 0;
  const std::uint64_t pad = padding ? 1 : 0;
  std::uint64_t bytes = 0;
  if (layer == 1) {
    bytes = (std::uint64_t{12000} * kbps / sample_rate + pad) * 4;
  } else if (layer == 3 && lsf()) {
    bytes = std::uint64_t{72000} * kbps / sample_rate + pad;
  } else {
    bytes = std::uint64_t{144000} * kbps / sample_rate + pad;
  }
  return static_cast<std::size_t>(bytes);
}

std::uint32_t FrameHeader::stream_key() const noexcept {
  return static_cast<std::uint32_t>((version << 8) | (layer << 4) | (rate_index << 2) | (mode == 3 ? 1 : 0)) |
         (bitrate_index == 0 ? 0x10000u : 0u);
}

std::optional<FrameHeader> parse_header(std::uint32_t word) noexcept {
  if ((word & 0xFFE00000u) != 0xFFE00000u) return std::nullopt;
  const unsigned version_bits = (word >> 19) & 3;
  const unsigned layer_bits = (word >> 17) & 3;
  const unsigned bitrate_bits = (word >> 12) & 15;
  const unsigned rate_bits = (word >> 10) & 3;
  if (version_bits == 1 || layer_bits == 0 || bitrate_bits == 15 || rate_bits == 3) return std::nullopt;
  FrameHeader h;
  h.version = version_bits == 3 ? 0 : version_bits == 2 ? 1 : 2;
  h.layer = 4 - static_cast<int>(layer_bits);
  h.crc = ((word >> 16) & 1) == 0;
  h.bitrate_index = static_cast<int>(bitrate_bits);
  h.rate_index = static_cast<int>(rate_bits);
  h.padding = ((word >> 9) & 1) != 0;
  h.mode = static_cast<int>((word >> 6) & 3);
  h.mode_ext = static_cast<int>((word >> 4) & 3);
  h.copyright = ((word >> 3) & 1) != 0;
  h.original = ((word >> 2) & 1) != 0;
  h.emphasis = static_cast<int>(word & 3);
  if (h.emphasis == 2) return std::nullopt;  // reserved
  h.sample_rate = sample_rates[h.version][h.rate_index];
  h.bitrate_kbps = bitrates[h.lsf() ? 1 : 0][h.layer - 1][h.bitrate_index];
  // Layer II does not allow every bit rate with every mode (MPEG-1 only).
  if (h.layer == 2 && !h.lsf() && h.bitrate_index != 0) {
    const std::uint32_t kbps = h.bitrate_kbps;
    if (h.mode == 3 ? kbps > 192 : (kbps == 32 || kbps == 48 || kbps == 56 || kbps == 80)) return std::nullopt;
  }
  return h;
}

std::uint32_t header_word(ByteView data, std::size_t at) noexcept {
  if (at > data.size() || data.size() - at < 4) return 0;
  const auto b = as_u8(data.subspan(at, 4));
  return (std::uint32_t{b[0]} << 24) | (std::uint32_t{b[1]} << 16) | (std::uint32_t{b[2]} << 8) | b[3];
}

namespace {

std::uint32_t be32(std::span<const std::uint8_t> d, std::size_t at) {
  return (std::uint32_t{d[at]} << 24) | (std::uint32_t{d[at + 1]} << 16) | (std::uint32_t{d[at + 2]} << 8) | d[at + 3];
}

bool same_stream(const FrameHeader& a, const FrameHeader& b) noexcept { return a.stream_key() == b.stream_key(); }

/// Size of the frame at `at` (free format uses `free_size`, the size without padding).
std::size_t size_at(const FrameHeader& h, std::size_t free_size) noexcept {
  if (h.bitrate_index != 0) return h.frame_bytes(h.bitrate_kbps);
  if (free_size == 0) return 0;
  const std::size_t unit = h.layer == 1 ? 4 : 1;
  return free_size + (h.padding ? unit : 0);
}

/// Free format: the distance to the next header of the same stream.
std::size_t measure_free_format(ByteView data, std::size_t at, std::size_t end, const FrameHeader& h) {
  const std::size_t unit = h.layer == 1 ? 4 : 1;
  const std::size_t pad = h.padding ? unit : 0;
  const std::size_t limit = std::min(end, at + 8000);
  for (std::size_t q = at + 24; q + 4 <= limit; ++q) {
    const auto next = parse_header(header_word(data, q));
    if (!next || !same_stream(*next, h)) continue;
    const std::size_t base = q - at - pad;
    // Confirm with a third frame where there is one.
    const std::size_t third = q + base + (next->padding ? unit : 0);
    if (third + 4 <= end) {
      const auto after = parse_header(header_word(data, third));
      if (!after || !same_stream(*after, h)) continue;
    }
    return base;
  }
  return 0;
}

/// A header at `at` that is followed by another one of the same stream
/// (or by the end of the data).
bool confirmed(ByteView data, std::size_t at, std::size_t end, const FrameHeader& h, std::size_t free_size) {
  const std::size_t size = size_at(h, free_size);
  if (size < 4) return false;
  const std::size_t next = at + size;
  if (next == end || (next < end && end - next < 4)) return true;
  if (next > end) return false;
  const auto n = parse_header(header_word(data, next));
  return n && same_stream(*n, h);
}

}  // namespace

std::optional<StreamInfo> parse_info_frame(ByteView frame, const FrameHeader& header) {
  const auto d = as_u8(frame);
  StreamInfo info;
  if (header.layer == 3) {
    const std::size_t at = 4 + (header.crc ? 2 : 0) + header.side_info_bytes();
    if (d.size() >= at + 8 && (std::memcmp(d.data() + at, "Xing", 4) == 0 || std::memcmp(d.data() + at, "Info", 4) == 0)) {
      info.has_tag = true;
      const std::uint32_t flags = be32(d, at + 4);
      std::size_t p = at + 8;
      if ((flags & 1) != 0) {
        if (d.size() < p + 4) return info;
        info.frames = be32(d, p);
        p += 4;
      }
      if ((flags & 2) != 0) p += 4;
      if ((flags & 4) != 0) p += 100;
      if ((flags & 8) != 0) p += 4;
      // LAME extension: 9-byte version string, then at +21 three bytes
      // holding the 12-bit encoder delay and padding.
      if (d.size() >= p + 24) {
        bool printable = true;
        for (std::size_t i = 0; i < 4; ++i) {
          const std::uint8_t c = d[p + i];
          if (c < 0x20 || c > 0x7E) printable = false;
        }
        if (printable) {
          info.has_lame = true;
          const std::uint32_t v = (std::uint32_t{d[p + 21]} << 16) | (std::uint32_t{d[p + 22]} << 8) | d[p + 23];
          info.encoder_delay = v >> 12;
          info.encoder_padding = v & 0xFFF;
        }
      }
      return info;
    }
    if (d.size() >= 36 + 18 && std::memcmp(d.data() + 36, "VBRI", 4) == 0) {
      info.has_tag = true;
      info.frames = be32(d, 36 + 14);
      return info;
    }
  }
  return std::nullopt;
}

std::optional<FrameHeader> probe_stream(ByteView data, std::size_t start, std::size_t window) {
  const std::size_t end = data.size();
  const std::size_t stop = std::min(end, start + window);
  for (std::size_t pos = start; pos + 4 <= stop; ++pos) {
    if (as_u8(data)[pos] != 0xFF) continue;
    const auto h = parse_header(header_word(data, pos));
    if (!h) continue;
    std::size_t fs = 0;
    if (h->bitrate_index == 0) {
      fs = measure_free_format(data, pos, end, *h);
      if (fs == 0) continue;
    }
    if (!confirmed(data, pos, end, *h, fs)) continue;
    const std::size_t next = pos + size_at(*h, fs);
    if (next + 4 <= end) {
      const auto n = parse_header(header_word(data, next));
      if (!n || !confirmed(data, next, end, *n, fs)) continue;
    }
    return h;
  }
  return std::nullopt;
}

Result<ScanResult> scan_stream(ByteView data, std::size_t start, std::size_t end, const Context& ctx, int wanted_layer) {
  ScanResult result;
  end = std::min(end, data.size());
  std::size_t pos = start;
  std::size_t free_size = 0;
  bool found = false;
  // First frame: a valid header confirmed by the next one.
  for (; pos + 4 <= end; ++pos) {
    if (as_u8(data)[pos] != 0xFF) continue;
    const auto h = parse_header(header_word(data, pos));
    if (!h) continue;
    if (wanted_layer != 0 && h->layer != wanted_layer) continue;
    std::size_t fs = 0;
    if (h->bitrate_index == 0) {
      fs = measure_free_format(data, pos, end, *h);
      if (fs == 0) continue;
    }
    if (!confirmed(data, pos, end, *h, fs)) continue;
    result.first = *h;
    free_size = fs;
    found = true;
    break;
  }
  if (!found) return fail(Status::Decode, "no MPEG audio frames were found");
  result.audio_start = pos;
  const FrameHeader& first = result.first;
  std::size_t index = 0;
  while (pos + 4 <= end) {
    if ((++index & 4095) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(pos) / static_cast<double>(end), "Scanning"));
    const auto h = parse_header(header_word(data, pos));
    std::size_t size = h && same_stream(*h, first) ? size_at(*h, free_size) : 0;
    if (size >= 4 && pos + size <= end) {
      result.frames.push_back({pos, size});
      pos += size;
      continue;
    }
    if (size >= 4 && pos + size > end) break;  // truncated last frame
    // Damage: look for the next confirmed header of the same stream.
    bool resynced = false;
    for (std::size_t q = pos + 1; q + 4 <= end; ++q) {
      if (as_u8(data)[q] != 0xFF) continue;
      const auto n = parse_header(header_word(data, q));
      if (!n || !same_stream(*n, first)) continue;
      if (!confirmed(data, q, end, *n, free_size)) continue;
      pos = q;
      resynced = true;
      break;
    }
    if (!resynced) break;
    result.resynced = true;
  }
  result.audio_end = pos;
  if (result.frames.empty()) return fail(Status::Decode, "no complete MPEG audio frame was found");
  return result;
}

}  // namespace cvt::lossy::mpeg
