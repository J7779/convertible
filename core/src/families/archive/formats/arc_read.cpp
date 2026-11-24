// Convertible core - archive family: ARC archives (SEA ARC 5/6, PKPAK; .arc, .ark), read only.
//
// An ARC file is a sequence of entries, each `1A method name[13] packed:u32
// date:u16 time:u16 crc16:u16 [size:u32]` (method 1 has no size field)
// followed by the packed data, and ends with `1A 00`. Methods:
//   1, 2  stored
//   3     "packed": run-length encoding with 0x90 as the repeat marker
//   4     "squeezed": static Huffman tree in the data, then run-length decoding
//   5..7  "crunched" (early): 12-bit LZW with a hashed string table; 6 and 7
//         add run-length decoding, 7 uses a faster hash
//   8     "crunched": LZW with 9..12-bit codes and table reset (the Unix
//         `compress` algorithm), then run-length decoding
//   9     "squashed": the same LZW with 9..13-bit codes, no run-length stage
//   20..29 informational records (skipped), 30 a sub-directory (ARC 6),
//   10, 11 PAK "crushed"/"distilled" (not supported)
// Every entry's CRC-16 is verified. Names are DOS 8.3 names (code page 437).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "families/archive/archive_common.hpp"
#include "families/archive/checksums.hpp"
#include "families/archive/compression.hpp"
#include "families/archive/formats/legacy_util.hpp"
#include "util/text.hpp"
#include "util/zip.hpp"

namespace cvt::archive::legacy {

std::int64_t dos_time(std::uint16_t date, std::uint16_t time) noexcept {
  if (date == 0 && time == 0) return 0;
  return util::dos_to_unix_time(date, time);
}

namespace {

constexpr std::byte marker{0x1A};
constexpr std::uint8_t rle_marker = 0x90;

Result<void> put(Bytes& out, std::byte b, std::uint64_t limit) {
  if (out.size() >= limit) return fail(Status::Decode, "ARC entry is longer than its declared size");
  out.push_back(b);
  return {};
}

// Run-length decoder state machine, fed byte by byte.
class Unrle {
 public:
  Unrle(Bytes& out, std::uint64_t limit) : out_(&out), limit_(limit) {}
  Result<void> feed(std::uint8_t c) {
    if (repeat_) {
      repeat_ = false;
      if (c == 0) return put(*out_, std::byte{rle_marker}, limit_);
      if (!have_last_) return fail(Status::Decode, "ARC run-length data starts with a repeat");
      const std::uint64_t n = static_cast<std::uint64_t>(c) - 1;
      if (n > limit_ - out_->size()) return fail(Status::Decode, "ARC entry is longer than its declared size");
      out_->insert(out_->end(), static_cast<std::size_t>(n), static_cast<std::byte>(last_));
      return {};
    }
    if (c == rle_marker) {
      repeat_ = true;
      return {};
    }
    last_ = c;
    have_last_ = true;
    return put(*out_, static_cast<std::byte>(c), limit_);
  }

 private:
  Bytes* out_;
  std::uint64_t limit_;
  std::uint8_t last_ = 0;
  bool have_last_ = false;
  bool repeat_ = false;
};

}  // namespace

Result<Bytes> arc_unrle(ByteView data, std::uint64_t limit) {
  Bytes out;
  Unrle rle(out, limit);
  for (const std::byte b : data) CVT_TRY(rle.feed(std::to_integer<std::uint8_t>(b)));
  return out;
}

Result<Bytes> arc_unsqueeze(ByteView data, std::uint64_t limit) {
  if (data.size() < 2) return fail(Status::Decode, "ARC squeezed data is truncated");
  const std::size_t nodes = le16(data, 0);
  if (nodes > 256) return fail(Status::Decode, "ARC squeezed data has an invalid tree");
  if (data.size() < 2 + 4 * nodes) return fail(Status::Decode, "ARC squeezed data is truncated");
  std::vector<std::array<int, 2>> tree(nodes);
  for (std::size_t i = 0; i < nodes; ++i) {
    for (std::size_t k = 0; k < 2; ++k) {
      const int v = static_cast<std::int16_t>(le16(data, 2 + 4 * i + 2 * k));
      if (v >= static_cast<int>(nodes) || v < -257) return fail(Status::Decode, "ARC squeezed data has an invalid tree");
      tree[i][k] = v;
    }
  }
  Bytes out;
  Unrle rle(out, limit);
  if (nodes == 0) return out;
  std::size_t pos = 2 + 4 * nodes;
  int bit = 8;
  std::uint8_t cur = 0;
  int node = 0;
  std::size_t steps = 0;
  for (;;) {
    if (bit == 8) {
      // Some writers (arc 5.21 on 64-bit systems) omit the end code: the
      // end of the data ends the entry, and the size and CRC checks decide.
      if (pos >= data.size()) break;
      cur = u8(data, pos++);
      bit = 0;
    }
    node = tree[static_cast<std::size_t>(node)][(cur >> bit) & 1u];
    ++bit;
    if (node < 0) {
      const int symbol = -(node + 1);
      if (symbol == 256) break;  // end of data
      CVT_TRY(rle.feed(static_cast<std::uint8_t>(symbol)));
      node = 0;
      steps = 0;
    } else if (++steps > 512) {
      return fail(Status::Decode, "ARC squeezed data has a looping tree");
    }
  }
  return out;
}

namespace {

// The string table of early crunching: 4096 entries placed by hashing.
class OldLzw {
 public:
  explicit OldLzw(bool fast_hash) : fast_(fast_hash) {
    for (int c = 0; c < 256; ++c) add(no_pred, static_cast<std::uint8_t>(c));
  }

  static constexpr std::uint32_t no_pred = 0xFFFF;
  static constexpr std::uint32_t size = 4096;

  struct Slot {
    bool used = false;
    std::uint16_t next = 0;
    std::uint32_t pred = no_pred;
    std::uint8_t follower = 0;
  };

  [[nodiscard]] bool full() const noexcept { return count_ >= size; }
  [[nodiscard]] const Slot& at(std::uint32_t code) const noexcept { return table_[code]; }

  void add(std::uint32_t pred, std::uint8_t follower) {
    if (full()) return;
    std::uint32_t h = hash(pred, follower);
    if (table_[h].used) {
      // Walk to the end of the collision chain, then probe for a free slot.
      std::size_t guard = 0;
      while (table_[h].next != 0 && guard++ < size) h = table_[h].next;
      std::uint32_t free = (h + 101) & (size - 1);
      while (table_[free].used) free = (free + 1) & (size - 1);
      table_[h].next = static_cast<std::uint16_t>(free);
      h = free;
    }
    table_[h] = Slot{true, 0, pred, follower};
    ++count_;
  }

 private:
  [[nodiscard]] std::uint32_t hash(std::uint32_t pred, std::uint8_t follower) const noexcept {
    if (fast_) return ((pred + follower) * 15073u) & (size - 1);
    std::uint32_t local = ((pred + follower) | 0x0800u) & 0xFFFFu;
    local *= local;
    return (local >> 6) & (size - 1);
  }

  bool fast_;
  std::array<Slot, size> table_{};
  std::uint32_t count_ = 0;
};

}  // namespace

Result<Bytes> arc_uncrunch_old(ByteView data, int method, std::uint64_t limit) {
  auto lzw = std::make_unique<OldLzw>(method == 7);
  Bytes plain;
  const bool rle_stage = method != 5;
  Bytes out;
  Unrle rle(out, limit);
  const auto emit = [&](std::uint8_t c) -> Result<void> {
    if (rle_stage) return rle.feed(c);
    return put(out, static_cast<std::byte>(c), limit);
  };
  // 12-bit codes, most significant bits first.
  std::size_t bitpos = 0;
  const std::size_t total_bits = data.size() * 8;
  const auto next_code = [&]() -> std::optional<std::uint32_t> {
    if (bitpos + 12 > total_bits) return std::nullopt;
    std::uint32_t v = 0;
    for (int k = 0; k < 12; ++k, ++bitpos) {
      v = (v << 1) | ((u8(data, bitpos >> 3) >> (7 - (bitpos & 7))) & 1u);
    }
    return v;
  };
  auto first = next_code();
  if (!first) return out;
  if (!lzw->at(*first).used || lzw->at(*first).pred != OldLzw::no_pred) return fail(Status::Decode, "ARC crunched data is damaged");
  std::uint32_t old = *first;
  std::uint8_t fin = lzw->at(old).follower;
  CVT_TRY(emit(fin));
  std::vector<std::uint8_t> stack;
  while (auto code = next_code()) {
    const std::uint32_t in = *code;
    std::uint32_t c = in;
    stack.clear();
    if (!lzw->at(c).used) {  // the KwKwK case: the string being defined
      stack.push_back(fin);
      c = old;
    }
    while (lzw->at(c).pred != OldLzw::no_pred) {
      if (stack.size() > OldLzw::size) return fail(Status::Decode, "ARC crunched data is damaged");
      stack.push_back(lzw->at(c).follower);
      c = lzw->at(c).pred;
      if (c >= OldLzw::size || !lzw->at(c).used) return fail(Status::Decode, "ARC crunched data is damaged");
    }
    fin = lzw->at(c).follower;
    stack.push_back(fin);
    for (auto it = stack.rbegin(); it != stack.rend(); ++it) CVT_TRY(emit(*it));
    lzw->add(old, fin);
    old = in;
  }
  return out;
}

namespace {

Result<Bytes> unpack(int method, ByteView packed, std::uint64_t size, const Context& ctx) {
  namespace cz = compression;
  switch (method) {
    case 1:
    case 2:
      if (packed.size() != size) return fail(Status::Decode, "ARC stored entry has the wrong size");
      return to_bytes(packed);
    case 3:
      return arc_unrle(packed, size);
    case 4:
      return arc_unsqueeze(packed, size);
    case 5:
    case 6:
    case 7:
      return arc_uncrunch_old(packed, method, size);
    case 8:
    case 9: {
      // Unix `compress` data without its 3-byte header: supply one.
      ByteView body = packed;
      int bits = 13;
      if (method == 8) {
        if (body.empty()) return fail(Status::Decode, "ARC crunched data is truncated");
        bits = u8(body, 0);
        body = body.subspan(1);
        if (bits < 9 || bits > 16) return fail(Status::Decode, "ARC crunched data uses {} bits", bits);
      }
      Bytes z = {std::byte{0x1F}, std::byte{0x9D}, static_cast<std::byte>(0x80 | bits)};
      append(z, body);
      cz::DecodeOptions d;
      d.max_output = method == 9 ? size : (size + 1) * 2 + 1024;  // run-length stage still to come
      if (method == 8) d.max_output = std::min<std::uint64_t>(d.max_output, ctx.limits.max_decompressed_bytes);
      d.progress = ctx.progress;
      auto lzw = cz::decompress(cz::Method::Lzw, z, d);
      if (!lzw) {
        if (lzw.error().code == Status::Limit) return fail(Status::Decode, "ARC entry is longer than its declared size");
        return std::unexpected(std::move(lzw).error());
      }
      if (method == 9) return lzw;
      return arc_unrle(*lzw, size);
    }
    case 10:
    case 11:
      return fail(Status::Decode, "PAK {} entries are not supported", method == 10 ? "crushed" : "distilled");
    default:
      return fail(Status::Decode, "ARC compression method {} is not supported", method);
  }
}

struct Reader {
  ByteView data;
  EntrySink& sink;
  const ReadContext& ctx;
  std::size_t count = 0;

  Result<void> run(std::size_t pos, std::size_t end, const std::string& prefix, int depth) {
    if (depth > 16) return fail(Status::Decode, "ARC sub-directories are nested too deeply");
    for (;;) {
      if (pos >= end) return fail(Status::Decode, "ARC archive is truncated (no end marker)");
      if (data[pos] != marker) return fail(Status::Decode, "ARC entry header is damaged");
      if (pos + 2 > end) return fail(Status::Decode, "ARC archive is truncated");
      const int method = u8(data, pos + 1);
      if (method == 0) return {};
      const std::size_t header = method == 1 ? 25 : 29;
      if (end - pos < header) return fail(Status::Decode, "ARC entry header is truncated");
      const ByteView h = data.subspan(pos, header);
      std::string_view raw_name = as_chars(h.subspan(2, 13));
      raw_name = raw_name.substr(0, raw_name.find('\0'));
      const std::uint32_t packed_size = le32(h, 15);
      const std::uint16_t date = le16(h, 19);
      const std::uint16_t time = le16(h, 21);
      const std::uint16_t crc = le16(h, 23);
      const std::uint32_t size = method == 1 ? packed_size : le32(h, 25);
      pos += header;
      if (packed_size > end - pos) return fail(Status::Decode, "ARC entry data is truncated");
      const ByteView packed = data.subspan(pos, packed_size);
      pos += packed_size;
      if ((++count & 63u) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(pos) / static_cast<double>(data.size() + 1)));
      const std::string name = prefix + decode_name(raw_name, false, true);
      if (method >= 20 && method < 30) continue;  // comments and other information records
      if (method == 30) {
        ir::ArchiveEntry dir;
        dir.path = name;
        dir.is_directory = true;
        dir.mtime = dos_time(date, time);
        CVT_TRY(sink.add(std::move(dir)));
        const std::size_t sub = static_cast<std::size_t>(packed.data() - data.data());
        CVT_TRY(run(sub, sub + packed.size(), name + "/", depth + 1));
        continue;
      }
      if (method == 31) return {};  // end of a sub-directory
      CVT_TRY(sink.charge(size));
      CVT_TRY_ASSIGN(Bytes content, unpack(method, packed, size, ctx));
      if (content.size() != size) return fail(Status::Decode, "ARC entry '{}' has the wrong size", name);
      if (crc16_arc(content) != crc) return fail(Status::Decode, "ARC entry '{}' fails its CRC check", name);
      ir::ArchiveEntry e;
      e.path = name;
      e.bytes = std::move(content);
      e.mtime = dos_time(date, time);
      CVT_TRY(sink.add(std::move(e)));
    }
  }
};

}  // namespace

Result<ir::Archive> read_arc(ReadContext& ctx) {
  if (ctx.input.size() < 2 || ctx.input[0] != marker) return fail(Status::Decode, "not an ARC archive");
  EntrySink sink(ctx, ctx.input.size());
  Reader reader{ctx.input, sink, ctx, 0};
  CVT_TRY(reader.run(0, ctx.input.size(), "", 0));
  return sink.finish();
}

int sniff_arc(std::span<const std::byte> data, std::string_view filename) {
  if (data.size() < 2 || data[0] != marker) return 0;
  const int method = u8(data, 1);
  const bool named = util::iends_with(filename, ".arc") || util::iends_with(filename, ".ark") ||
                     util::iends_with(filename, ".pak");
  if (method == 0) return data.size() == 2 && named ? 70 : 0;
  if (!((method >= 1 && method <= 11) || (method >= 20 && method <= 30))) return 0;
  const std::size_t header = method == 1 ? 25 : 29;
  if (data.size() < header) return named ? 60 : 0;
  // The name: printable characters, NUL-terminated within 13 bytes.
  std::size_t len = 0;
  while (len < 13 && data[2 + len] != std::byte{0}) {
    const auto c = u8(data, 2 + len);
    if (c < 0x20 || c == 0x7F) return 0;
    ++len;
  }
  if (len == 0 || len == 13) return 0;
  const std::uint64_t packed = le32(data, 15);
  if (packed > data.size()) return 0;
  // The next header (or the end marker) follows the data.
  const std::uint64_t next = header + packed;
  if (next < data.size() && data[static_cast<std::size_t>(next)] != marker) return 0;
  return named ? 88 : 62;
}

}  // namespace cvt::archive::legacy
