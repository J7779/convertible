// Convertible core - audio_lossy family: in-house Ogg (RFC 3533) page
// demultiplexer and multiplexer with granule positions, for Ogg Opus,
// Ogg Vorbis and Ogg Speex.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "util/bytes.hpp"

namespace cvt::lossy::xiph {

/// One logical stream: its packets and, for every packet that ends a page,
/// the page's granule position (-1 for packets that do not end a page).
struct OggLogicalStream {
  std::uint32_t serial = 0;
  std::vector<Bytes> packets;
  std::vector<std::int64_t> granules;
  bool damaged = false;      ///< pages were dropped (bad CRC, sequence gap)
  bool complete = false;     ///< an end-of-stream page was seen
};

/// All logical streams of the first chain (streams that begin before any
/// data page). Packet bytes count against the decompression limit.
[[nodiscard]] Result<std::vector<OggLogicalStream>> ogg_demux(ByteView input, const Context& ctx);

/// The first logical stream whose first packet starts with `magic`.
[[nodiscard]] Result<OggLogicalStream> ogg_find_stream(ByteView input, std::string_view magic, const Context& ctx);

/// Writes one logical stream.
class OggWriter {
 public:
  explicit OggWriter(std::uint32_t serial) : serial_(serial) {}
  /// Append a packet; `granule` is the stream position after it.
  /// `flush` ends the page after this packet (used for header packets).
  void add(ByteView packet, std::int64_t granule, bool flush = false);
  /// Close the stream (the last page gets the end-of-stream flag).
  [[nodiscard]] Bytes finish();
  [[nodiscard]] std::size_t size() const noexcept { return out_.size(); }

 private:
  void emit(bool last);
  std::uint32_t serial_;
  std::uint32_t sequence_ = 0;
  bool started_ = false;
  std::vector<std::uint8_t> lacing_;
  Bytes body_;
  std::int64_t granule_ = -1;
  bool pending_packet_end_ = false;
  bool continued_ = false;
  std::size_t last_page_ = 0;
  Bytes out_;
};

}  // namespace cvt::lossy::xiph
