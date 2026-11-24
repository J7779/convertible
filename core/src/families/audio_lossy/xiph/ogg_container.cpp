// Convertible core - audio_lossy family: in-house Ogg page demultiplexer and
// multiplexer.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "util/bytes.hpp"
#include "families/audio_lossy/xiph/ogg_container.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <map>
#include <optional>

#include "families/audio_lossless/ogg.hpp"

namespace cvt::lossy::xiph {

namespace {

struct Assembly {
  OggLogicalStream stream;
  Bytes partial;
  bool have_partial = false;
  std::optional<std::uint32_t> next_sequence;
  bool skip_continuation = false;
};

}  // namespace

Result<std::vector<OggLogicalStream>> ogg_demux(ByteView input, const Context& ctx) {
  const auto d = as_u8(input);
  std::map<std::uint32_t, Assembly> streams;
  std::vector<std::uint32_t> order;
  bool data_seen = false;
  std::uint64_t total = 0;
  std::size_t pos = 0;
  std::size_t pages = 0;
  while (pos + 27 <= d.size()) {
    if (::cvt::util::compare_bytes(d.data() + pos, "OggS", 4) != 0 || d[pos + 4] != 0) {
      // Resynchronise on the next capture pattern.
      const std::size_t found = as_chars(input).find("OggS", pos + 1);
      if (found == std::string_view::npos) break;
      pos = found;
      continue;
    }
    if ((++pages & 255) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(pos) / static_cast<double>(d.size()), "Reading"));
    const std::uint8_t type = d[pos + 5];
    std::int64_t granule = 0;
    for (int i = 7; i >= 0; --i) granule = static_cast<std::int64_t>((static_cast<std::uint64_t>(granule) << 8) | d[pos + 6 + static_cast<std::size_t>(i)]);
    const std::uint32_t serial = d[pos + 14] | (std::uint32_t{d[pos + 15]} << 8) | (std::uint32_t{d[pos + 16]} << 16) |
                                 (std::uint32_t{d[pos + 17]} << 24);
    const std::uint32_t sequence = d[pos + 18] | (std::uint32_t{d[pos + 19]} << 8) | (std::uint32_t{d[pos + 20]} << 16) |
                                   (std::uint32_t{d[pos + 21]} << 24);
    const std::uint32_t crc = d[pos + 22] | (std::uint32_t{d[pos + 23]} << 8) | (std::uint32_t{d[pos + 24]} << 16) |
                              (std::uint32_t{d[pos + 25]} << 24);
    const std::size_t segments = d[pos + 26];
    if (pos + 27 + segments > d.size()) break;
    std::size_t body = 0;
    for (std::size_t i = 0; i < segments; ++i) body += d[pos + 27 + i];
    const std::size_t header = 27 + segments;
    if (pos + header + body > d.size()) break;  // truncated page
    const ByteView page = input.subspan(pos, header + body);
    const std::size_t page_start = pos;
    pos += header + body;
    if (audio::ogg_crc(page) != crc) {
      if (auto it = streams.find(serial); it != streams.end()) {
        it->second.stream.damaged = true;
        it->second.have_partial = false;
        it->second.partial.clear();
        it->second.skip_continuation = true;
      }
      continue;
    }
    const bool bos = (type & 2) != 0;
    auto it = streams.find(serial);
    if (it == streams.end()) {
      // A stream that starts after data pages belongs to a later chain.
      if (!bos || data_seen) continue;
      it = streams.emplace(serial, Assembly{}).first;
      it->second.stream.serial = serial;
      order.push_back(serial);
    } else if (bos) {
      continue;
    }
    if (!bos) data_seen = true;
    Assembly& a = it->second;
    if (a.next_sequence && sequence != *a.next_sequence) {
      a.stream.damaged = true;
      a.have_partial = false;
      a.partial.clear();
      a.skip_continuation = true;
    }
    a.next_sequence = sequence + 1;
    const bool continued = (type & 1) != 0;
    if (!continued) {
      if (a.have_partial) a.stream.damaged = true;
      a.have_partial = false;
      a.partial.clear();
      a.skip_continuation = false;
    }
    std::size_t offset = page_start + header;
    std::ptrdiff_t last_complete = -1;
    for (std::size_t i = 0; i < segments; ++i) {
      const std::size_t len = d[page_start + 27 + i];
      if (!a.skip_continuation) {
        total += len;
        if (total > ctx.limits.max_decompressed_bytes) return fail(Status::Limit, "the Ogg stream is larger than allowed");
        const auto* p = reinterpret_cast<const std::byte*>(d.data() + offset);
        a.partial.insert(a.partial.end(), p, p + len);
        a.have_partial = true;
      }
      offset += len;
      if (len < 255) {
        if (!a.skip_continuation) {
          a.stream.packets.push_back(std::move(a.partial));
          a.stream.granules.push_back(-1);
          last_complete = static_cast<std::ptrdiff_t>(a.stream.packets.size()) - 1;
        }
        a.partial.clear();
        a.have_partial = false;
        a.skip_continuation = false;
      }
    }
    if (last_complete >= 0) a.stream.granules[static_cast<std::size_t>(last_complete)] = granule;
    if ((type & 4) != 0) a.stream.complete = true;
  }
  std::vector<OggLogicalStream> result;
  for (const std::uint32_t s : order) result.push_back(std::move(streams[s].stream));
  if (result.empty()) return fail(Status::Decode, "no Ogg pages were found");
  return result;
}

Result<OggLogicalStream> ogg_find_stream(ByteView input, std::string_view magic, const Context& ctx) {
  CVT_TRY_ASSIGN(auto streams, ogg_demux(input, ctx));
  for (auto& s : streams) {
    if (s.packets.empty()) continue;
    const Bytes& first = s.packets.front();
    if (first.size() >= magic.size() && ::cvt::util::compare_bytes(first.data(), magic.data(), magic.size()) == 0) return std::move(s);
  }
  return fail(Status::Decode, "the Ogg file holds no stream of the expected kind");
}

// =========================================================================== writer

void OggWriter::add(ByteView packet, std::int64_t granule, bool flush) {
  std::size_t pos = 0;
  std::size_t remaining = packet.size();
  while (true) {
    if (lacing_.size() == 255) emit(false);
    const std::size_t chunk = std::min<std::size_t>(remaining, 255);
    lacing_.push_back(static_cast<std::uint8_t>(chunk));
    body_.insert(body_.end(), packet.begin() + static_cast<std::ptrdiff_t>(pos),
                 packet.begin() + static_cast<std::ptrdiff_t>(pos + chunk));
    pos += chunk;
    remaining -= chunk;
    if (chunk < 255) break;
    if (remaining == 0) {
      if (lacing_.size() == 255) emit(false);
      lacing_.push_back(0);
      break;
    }
  }
  granule_ = granule;
  pending_packet_end_ = true;
  if (flush || body_.size() >= 4000) emit(false);
}

void OggWriter::emit(bool last) {
  if (lacing_.empty() && !last) return;
  // A page that ends inside a packet carries granule -1; the next page
  // then continues that packet.
  const bool ends_packet = !lacing_.empty() && lacing_.back() < 255;
  std::vector<std::uint8_t> page(27 + lacing_.size());
  ::cvt::util::copy_bytes(page.data(), "OggS", 4);
  page[4] = 0;
  std::uint8_t type = 0;
  if (continued_) type |= 1;
  if (!started_) type |= 2;
  if (last) type |= 4;
  page[5] = type;
  const std::int64_t g = ends_packet ? granule_ : -1;
  for (int i = 0; i < 8; ++i) page[6 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(static_cast<std::uint64_t>(g) >> (8 * i));
  for (int i = 0; i < 4; ++i) {
    page[14 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(serial_ >> (8 * i));
    page[18 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(sequence_ >> (8 * i));
  }
  page[26] = static_cast<std::uint8_t>(lacing_.size());
  std::copy(lacing_.begin(), lacing_.end(), page.begin() + 27);
  Bytes full = to_bytes(std::span<const std::uint8_t>(page));
  full.insert(full.end(), body_.begin(), body_.end());
  const std::uint32_t crc = audio::ogg_crc(full);
  for (int i = 0; i < 4; ++i) full[22 + static_cast<std::size_t>(i)] = static_cast<std::byte>(crc >> (8 * i));
  last_page_ = out_.size();
  out_.insert(out_.end(), full.begin(), full.end());
  continued_ = !ends_packet && !lacing_.empty();
  started_ = true;
  ++sequence_;
  lacing_.clear();
  body_.clear();
  pending_packet_end_ = false;
}

Bytes OggWriter::finish() {
  if (!lacing_.empty() || out_.empty()) {
    emit(true);
  } else {
    // Mark the page already written as the last one.
    out_[last_page_ + 5] |= std::byte{4};
    for (std::size_t i = 22; i < 26; ++i) out_[last_page_ + i] = std::byte{0};
    const std::uint32_t crc = audio::ogg_crc(ByteView(out_).subspan(last_page_));
    for (int i = 0; i < 4; ++i) out_[last_page_ + 22 + static_cast<std::size_t>(i)] = static_cast<std::byte>(crc >> (8 * i));
  }
  return std::move(out_);
}

}  // namespace cvt::lossy::xiph
