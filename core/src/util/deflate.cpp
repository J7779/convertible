// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
//
// DEFLATE decoder and encoder, with the zlib and gzip wrappers.
//
// Decoder. A state machine that can stop at any input byte. Codes are decoded
// through two-level lookup tables (a root table indexed by the next 8-10
// stream bits, plus subtables for longer codes). Two decoding paths share
// the tables:
//   * a fast loop, used while at least 16 input bytes and 320 bytes of
//     output room remain, which refills a 64-bit bit buffer eight bytes at a
//     time and needs no bounds checks per symbol, and
//   * a careful path, used near the end of the input or of the permitted
//     output, which loads one byte only when a code cannot be decoded with
//     the bits at hand and checks every write.
// The careful path never holds more than seven bits beyond what it decoded,
// and the fast loop hands unused whole bytes back when it leaves, so the
// number of bytes consumed is exact at the end of the stream.
//
// Encoder. Input is collected in a window buffer and compressed in fixed
// steps (so the result does not depend on how the caller chunked it):
// LZ77 parsing with hash chains of 4-byte prefixes over a 32 KiB window plus
// a table of the latest position of every 3-byte prefix (short matches are
// only worth coding when close, so walking chains for them is wasted), greedy at levels 1-3
// and lazy at levels 4-9; the token stream is cut into blocks where the
// symbol statistics change; each block is written as the cheapest of stored,
// fixed-Huffman and dynamic-Huffman, with optimal length-limited codes from
// the package-merge algorithm.
#include "util/bytes.hpp"
#include "util/deflate.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

#include "util/crc.hpp"

namespace cvt::util {

namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;

// --------------------------------------------------------------------------- shared tables (RFC 1951, 3.2.5 - 3.2.7)

constexpr std::array<u16, 29> length_base = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                             31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<u8, 29> length_extra = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                             2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<u16, 30> dist_base = {1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                           33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                           1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<u8, 30> dist_extra = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                           6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
// Order in which the code lengths of the code-length alphabet are stored.
constexpr std::array<u8, 19> precode_order = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

constexpr unsigned max_code_bits = 15;
constexpr unsigned max_precode_bits = 7;
constexpr unsigned num_lit_symbols = 286;   // literal/length symbols that may appear in a block
constexpr unsigned num_dist_symbols = 30;   // distance symbols that may appear in a block
constexpr unsigned num_precode_symbols = 19;
constexpr unsigned end_of_block = 256;
constexpr unsigned min_match = 3;
constexpr unsigned max_match = 258;
constexpr std::size_t window_size = 32768;
constexpr std::size_t max_stored_block = 65535;

/// Code lengths of the fixed Huffman codes (RFC 1951, 3.2.6).
constexpr std::array<u8, 288> make_fixed_lit_lengths() noexcept {
  std::array<u8, 288> lens{};
  for (unsigned i = 0; i < 288; ++i) lens[i] = static_cast<u8>(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
  return lens;
}
constexpr std::array<u8, 32> make_fixed_dist_lengths() noexcept {
  std::array<u8, 32> lens{};
  for (u8& len : lens) len = 5;
  return lens;
}
constexpr std::array<u8, 288> fixed_lit_lengths = make_fixed_lit_lengths();
constexpr std::array<u8, 32> fixed_dist_lengths = make_fixed_dist_lengths();

/// The low `len` bits of `value`, mirrored. Huffman codes are defined most
/// significant bit first but DEFLATE packs them starting at the least
/// significant bit of each byte.
[[nodiscard]] constexpr u32 reverse_bits(u32 value, unsigned len) noexcept {
  u32 out = 0;
  for (unsigned i = 0; i < len; ++i) {
    out = (out << 1) | (value & 1u);
    value >>= 1;
  }
  return out;
}

[[nodiscard]] inline u64 load64le(const u8* p) noexcept {
  if constexpr (std::endian::native == std::endian::little) {
    u64 value;
    ::cvt::util::copy_bytes(&value, p, sizeof value);
    return value;
  } else {
    u64 value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= static_cast<u64>(p[i]) << (8 * i);
    return value;
  }
}

inline void store64le(u8* p, u64 value) noexcept {
  if constexpr (std::endian::native == std::endian::little) {
    ::cvt::util::copy_bytes(p, &value, sizeof value);
  } else {
    for (unsigned i = 0; i < 8; ++i) p[i] = static_cast<u8>(value >> (8 * i));
  }
}

[[nodiscard]] inline ByteView view_of(const u8* data, std::size_t size) noexcept {
  return {reinterpret_cast<const std::byte*>(data), size};
}

// --------------------------------------------------------------------------- decoding tables

// One table entry, 32 bits:
//   bits  0-7   number of stream bits this entry consumes (code bits held by
//               this table level, plus the extra bits of a length/distance)
//   bits  8-11  code bits held by this table level; for a subtable link, the
//               number of index bits of the subtable
//   bits 12-15  kind flags (none set: a match length or a distance)
//   bits 16-31  literal byte, base length, base distance, code-length symbol,
//               or the index of the first subtable entry
constexpr u32 entry_literal = 0x8000;
constexpr u32 entry_subtable = 0x4000;
constexpr u32 entry_end = 0x2000;
constexpr u32 entry_invalid = 0x1000;
/// A slot no code maps to (only in incomplete codes): a one-bit invalid code.
constexpr u32 entry_unused = entry_invalid | (1u << 8) | 1u;

constexpr unsigned lit_root_bits = 10;
constexpr unsigned dist_root_bits = 8;
constexpr unsigned pre_root_bits = 7;

// Upper bounds for the subtables. Subtables exist only for complete codes,
// are completely filled, and a subtable indexed by k bits holds at least
// k + 1 codes; with at most 286 (30) codes and k <= 5 (7) the totals below
// cannot be exceeded.
constexpr std::size_t lit_table_size = (std::size_t{1} << lit_root_bits) + 1512;
constexpr std::size_t dist_table_size = (std::size_t{1} << dist_root_bits) + 416;
constexpr std::size_t pre_table_size = std::size_t{1} << pre_root_bits;

/// Per-symbol entry templates: everything but the code length.
constexpr std::array<u32, 288> make_lit_symbol_entries() noexcept {
  std::array<u32, 288> out{};
  for (u32 s = 0; s < 256; ++s) out[s] = (s << 16) | entry_literal;
  out[end_of_block] = entry_end;
  for (u32 i = 0; i < 29; ++i) out[257 + i] = (static_cast<u32>(length_base[i]) << 16) | length_extra[i];
  out[286] = entry_invalid;  // these two have codes in the fixed tree but must not occur
  out[287] = entry_invalid;
  return out;
}
constexpr std::array<u32, 32> make_dist_symbol_entries() noexcept {
  std::array<u32, 32> out{};
  for (u32 i = 0; i < 30; ++i) out[i] = (static_cast<u32>(dist_base[i]) << 16) | dist_extra[i];
  out[30] = entry_invalid;
  out[31] = entry_invalid;
  return out;
}
constexpr std::array<u32, 19> make_precode_symbol_entries() noexcept {
  std::array<u32, 19> out{};
  for (u32 s = 0; s < 19; ++s) out[s] = s << 16;
  return out;
}
constexpr std::array<u32, 288> lit_symbol_entries = make_lit_symbol_entries();
constexpr std::array<u32, 32> dist_symbol_entries = make_dist_symbol_entries();
constexpr std::array<u32, 19> precode_symbol_entries = make_precode_symbol_entries();

enum class CodeKind { Precode, LitLen, Dist };

/// Build the decoding table of the canonical Huffman code with the given
/// code lengths (0 = symbol unused). Returns false when the lengths do not
/// describe an acceptable code. Like zlib, the code must be complete, with
/// two exceptions: a literal/length or distance code consisting of a single
/// one-bit code, and a distance code with no codes at all (a block of
/// literals only).
[[nodiscard]] bool build_decode_table(const u8* lens, unsigned count, const u32* symbol_entries, CodeKind kind,
                                      unsigned root_bits, u32* table, std::size_t capacity) noexcept {
  std::array<u16, max_code_bits + 1> length_count{};
  for (unsigned i = 0; i < count; ++i) ++length_count[lens[i]];
  unsigned max_len = max_code_bits;
  while (max_len > 0 && length_count[max_len] == 0) --max_len;

  const std::size_t root_size = std::size_t{1} << root_bits;
  if (max_len == 0) {
    if (kind != CodeKind::Dist) return false;
    std::fill_n(table, root_size, entry_unused);
    return true;
  }

  // Kraft inequality: `left` is the number of unassigned codes of the current length.
  i32 left = 1;
  for (unsigned len = 1; len <= max_code_bits; ++len) {
    left = left * 2 - static_cast<i32>(length_count[len]);
    if (left < 0) return false;  // over-subscribed
  }
  if (left > 0) {
    if (kind == CodeKind::Precode || max_len != 1) return false;  // incomplete
    std::fill_n(table, root_size, entry_unused);
  }

  // Symbols ordered by code length, then by value: the canonical code order.
  std::array<u16, max_code_bits + 2> offset{};
  for (unsigned len = 1; len <= max_code_bits; ++len) {
    offset[len + 1] = static_cast<u16>(offset[len] + length_count[len]);
  }
  std::array<u16, 288> sorted{};
  for (unsigned i = 0; i < count; ++i) {
    if (lens[i] != 0) sorted[offset[lens[i]]++] = static_cast<u16>(i);
  }
  const unsigned used = count - length_count[0];

  std::size_t next_free = root_size;
  std::size_t sub_start = 0;
  unsigned sub_bits = 0;
  u32 sub_prefix = ~u32{0};
  u32 code = 0;
  unsigned index = 0;
  for (unsigned len = 1; len <= max_len; ++len, code <<= 1) {
    for (unsigned k = 0; k < length_count[len]; ++k, ++index, ++code) {
      const u32 symbol = sorted[index];
      const u32 reversed = reverse_bits(code, len);
      if (len <= root_bits) {
        const u32 entry = symbol_entries[symbol] + (len << 8) + len;
        for (std::size_t i = reversed; i < root_size; i += std::size_t{1} << len) table[i] = entry;
        continue;
      }
      const u32 prefix = reversed & static_cast<u32>(root_size - 1);
      if (prefix != sub_prefix) {
        // A new subtable. Its codes are the following ones in canonical
        // order, up to the point where they fill the space below this prefix;
        // the longest of them decides the number of index bits.
        unsigned longest = len;
        u32 space = u32{1} << (len - root_bits);
        for (unsigned j = index; j < used; ++j) {
          const unsigned other = lens[sorted[j]];
          space <<= other - longest;
          longest = other;
          if (--space == 0) break;
        }
        sub_bits = longest - root_bits;
        const std::size_t sub_size = std::size_t{1} << sub_bits;
        if (sub_size > capacity - next_free) return false;  // cannot happen, see the size bounds above
        sub_start = next_free;
        next_free += sub_size;
        sub_prefix = prefix;
        std::fill_n(table + sub_start, sub_size, entry_unused);
        table[prefix] = (static_cast<u32>(sub_start) << 16) | entry_subtable | (sub_bits << 8) | root_bits;
      }
      const unsigned low_len = len - root_bits;
      const u32 entry = symbol_entries[symbol] + (low_len << 8) + low_len;
      const std::size_t sub_size = std::size_t{1} << sub_bits;
      for (std::size_t i = reversed >> root_bits; i < sub_size; i += std::size_t{1} << low_len) {
        table[sub_start + i] = entry;
      }
    }
  }
  return true;
}

/// Find the entry for the code at the low end of `bits`, of which only the
/// low `available` bits are real (the rest are zero). Returns false when
/// more bits are needed to identify the code and its extra bits; otherwise
/// `entry` is the final entry and `skip` the number of bits to drop before
/// the bits that entry describes.
[[nodiscard]] inline bool peek_code(const u32* table, unsigned root_bits, u64 bits, unsigned available, u32& entry,
                                    unsigned& skip) noexcept {
  u32 e = table[static_cast<std::size_t>(bits) & ((std::size_t{1} << root_bits) - 1)];
  unsigned dropped = 0;
  if (e & entry_subtable) {
    if (available <= root_bits) return false;
    dropped = root_bits;
    e = table[(e >> 16) + (static_cast<u32>(bits >> root_bits) & ((u32{1} << ((e >> 8) & 15u)) - 1))];
  }
  if (dropped + (e & 0xFFu) > available) return false;
  entry = e;
  skip = dropped;
  return true;
}

// The fast loop runs only while this much input and output room remain.
constexpr std::size_t fast_input_margin = 16;   // two 8-byte refills per iteration
constexpr std::size_t fast_output_margin = 320; // two literals plus a 258-byte match copied in 8-byte steps

/// Per-call view of the input and of the caller's output buffer.
struct InflateCursor {
  const u8* in = nullptr;
  const u8* in_end = nullptr;
  Bytes* out = nullptr;
  std::size_t base = 0;       ///< output size when the call started
  std::size_t pos = 0;        ///< write position; [base, pos) was produced by this call
  std::size_t cap = 0;        ///< current size of the output buffer
  std::size_t hard_cap = 0;   ///< the buffer may never grow beyond this
  std::size_t min_room = 64;  ///< smallest growth step
  std::size_t checked = 0;    ///< [base, checked) is already covered by the checksum
  std::uint64_t limit = 0;    ///< the caller's limit, for messages
  bool address_bound = false; ///< hard_cap comes from the address space, not from the limit
  bool out_of_memory = false;

  [[nodiscard]] u8* data() const noexcept { return reinterpret_cast<u8*>(out->data()); }

  /// Make room for `want` more bytes at `pos`. Growth is geometric in the
  /// amount produced and never passes `hard_cap`. Returns false when the room
  /// cannot be had. Invalidates pointers into the buffer.
  [[nodiscard]] bool reserve(std::size_t want) {
    if (cap - pos >= want) return true;
    if (cap >= hard_cap) return false;
    const std::size_t room = std::max(want, std::max(pos - base, min_room));
    const std::size_t target = room > hard_cap - pos ? hard_cap : pos + room;
    try {
      if (target > out->capacity()) {
        const u64 doubled = static_cast<u64>(out->capacity()) * 2;
        out->reserve(static_cast<std::size_t>(std::min<u64>(hard_cap, std::max<u64>(target, doubled))));
      }
      out->resize(target);
    } catch (const std::bad_alloc&) {
      out_of_memory = true;
      return false;
    }
    cap = target;
    return cap - pos >= want;
  }
};

}  // namespace

// --------------------------------------------------------------------------- Inflater

struct Inflater::Impl {
  enum class State : u8 {
    ZlibHeader,
    GzipHeader,
    GzipExtraLength,
    GzipExtra,
    GzipName,
    GzipComment,
    GzipHeaderCrc,
    BlockHeader,
    StoredLength,
    Stored,
    DynamicHeader,
    DynamicPrecode,
    DynamicLengths,
    Tokens,
    Trailer,
    GzipLength,
    Done,
    Failed,
  };
  enum class Outcome : u8 { NeedInput, Finished, Failed, BlockEnd, Continue };

  explicit Impl(Format stream_format) : format(stream_format) { reset(); }

  void reset() {
    state = format == Format::Zlib ? State::ZlibHeader : format == Format::Gzip ? State::GzipHeader : State::BlockHeader;
    bitbuf = 0;
    bitcnt = 0;
    last_block = false;
    fixed_tables = false;
    stored_left = 0;
    num_lit = num_dist = num_pre = length_index = 0;
    gzip_flags = 0;
    gzip_extra_left = 0;
    header_crc = 0;
    scratch_len = 0;
    checksum = format == Format::Zlib ? 1u : 0u;
    total_in = total_out = 0;
    window_pos = window_fill = 0;
    gzip = {};
    error = {};
  }

  // ---- small helpers

  [[nodiscard]] Outcome fail_with(Status code, std::string message) {
    error = Error{code, std::move(message)};
    state = State::Failed;
    return Outcome::Failed;
  }
  [[nodiscard]] Outcome corrupt(std::string_view what) { return fail_with(Status::Decode, std::string(what)); }
  [[nodiscard]] Outcome over_limit(const InflateCursor& c) {
    if (c.out_of_memory) return fail_with(Status::Limit, "not enough memory for the decompressed data");
    if (c.address_bound) return fail_with(Status::Limit, "decompressed data is too large to hold in memory");
    return fail_with(Status::Limit, std::format("decompressed data exceeds the limit of {} bytes", c.limit));
  }

  /// Load one input byte into the bit buffer.
  [[nodiscard]] bool load_byte(InflateCursor& c) noexcept {
    if (c.in == c.in_end) return false;
    bitbuf |= static_cast<u64>(*c.in++) << bitcnt;
    bitcnt += 8;
    return true;
  }
  /// Load input until `count` bits are available.
  [[nodiscard]] bool need_bits(InflateCursor& c, unsigned count) noexcept {
    while (bitcnt < count) {
      if (!load_byte(c)) return false;
    }
    return true;
  }
  void drop_bits(unsigned count) noexcept {
    bitbuf >>= count;
    bitcnt -= count;
  }
  /// Skip to the next byte boundary; whole bytes still in the bit buffer
  /// move to the scratch area so byte-wise reading can continue there.
  void align_to_byte() noexcept {
    drop_bits(bitcnt & 7u);
    while (bitcnt >= 8 && scratch_len < scratch.size()) {
      scratch[scratch_len++] = static_cast<u8>(bitbuf & 0xFFu);
      drop_bits(8);
    }
  }
  /// Collect `count` bytes in the scratch area (for byte-aligned fields).
  [[nodiscard]] bool fill_scratch(InflateCursor& c, unsigned count) noexcept {
    while (scratch_len < count) {
      if (c.in == c.in_end) return false;
      scratch[scratch_len++] = *c.in++;
    }
    return true;
  }
  [[nodiscard]] u32 scratch_u16(unsigned at) const noexcept {
    return static_cast<u32>(scratch[at]) | (static_cast<u32>(scratch[at + 1]) << 8);
  }
  [[nodiscard]] u32 scratch_u32(unsigned at) const noexcept { return scratch_u16(at) | (scratch_u16(at + 2) << 16); }

  /// Bring the running checksum up to date with everything written so far.
  void update_checksum(InflateCursor& c) noexcept {
    if (c.pos > c.checked) {
      const ByteView fresh = view_of(c.data() + c.checked, c.pos - c.checked);
      if (format == Format::Zlib) checksum = adler32(fresh, checksum);
      if (format == Format::Gzip) checksum = crc32(fresh, checksum);
    }
    c.checked = c.pos;
  }

  /// Remember the output of this call so later calls can copy from it.
  void save_window(const InflateCursor& c) {
    const std::size_t produced = c.pos - c.base;
    if (produced == 0) return;
    if (window.empty()) window.resize(window_size);
    const u8* const src = c.data() + c.base;
    if (produced >= window_size) {
      ::cvt::util::copy_bytes(window.data(), src + (produced - window_size), window_size);
      window_pos = 0;
      window_fill = window_size;
      return;
    }
    const std::size_t first = std::min(produced, window_size - window_pos);
    ::cvt::util::copy_bytes(window.data() + window_pos, src, first);
    if (produced > first) ::cvt::util::copy_bytes(window.data(), src + first, produced - first);
    window_pos = (window_pos + produced) & (window_size - 1);
    window_fill = std::min(window_size, window_fill + produced);
  }

  /// Copy a match that starts in the saved window (`distance` reaches back
  /// past the `history` bytes written by the current call). Returns the new
  /// write pointer.
  [[nodiscard]] u8* copy_from_window(u8* out, std::size_t history, std::size_t distance, unsigned length) const noexcept {
    std::size_t before = distance - history;
    std::size_t index = (window_pos + window_size - before) & (window_size - 1);
    for (; length > 0 && before > 0; --length, --before) {
      *out++ = window[index];
      index = (index + 1) & (window_size - 1);
    }
    for (; length > 0; --length, ++out) *out = *(out - distance);
    return out;
  }

  [[nodiscard]] bool use_fixed_tables() noexcept {
    if (fixed_tables) return true;
    if (!build_decode_table(fixed_lit_lengths.data(), 288, lit_symbol_entries.data(), CodeKind::LitLen, lit_root_bits,
                            lit_table.data(), lit_table.size()) ||
        !build_decode_table(fixed_dist_lengths.data(), 32, dist_symbol_entries.data(), CodeKind::Dist, dist_root_bits,
                            dist_table.data(), dist_table.size())) {
      return false;
    }
    fixed_tables = true;
    return true;
  }

  [[nodiscard]] Outcome run(InflateCursor& c);
  [[nodiscard]] Outcome decode_tokens(InflateCursor& c);
  [[nodiscard]] Outcome decode_fast(InflateCursor& c);

  Format format;
  State state = State::BlockHeader;
  bool last_block = false;
  bool fixed_tables = false;   ///< lit_table / dist_table currently hold the fixed codes
  u8 gzip_flags = 0;
  unsigned bitcnt = 0;         ///< valid bits in `bitbuf`; the bits above them are zero
  u64 bitbuf = 0;
  u32 stored_left = 0;
  unsigned num_lit = 0;
  unsigned num_dist = 0;
  unsigned num_pre = 0;
  unsigned length_index = 0;
  u32 gzip_extra_left = 0;
  u32 header_crc = 0;
  unsigned scratch_len = 0;
  u32 checksum = 0;
  u64 total_in = 0;
  u64 total_out = 0;
  std::size_t window_pos = 0;   ///< next write index in the circular window
  std::size_t window_fill = 0;  ///< valid bytes in the window
  std::vector<u8> window;       ///< last 32 KiB of output from earlier calls (allocated on demand)
  GzipHeader gzip;
  Error error;
  std::array<u8, 10> scratch{};
  std::array<u8, num_precode_symbols> pre_lens{};
  std::array<u8, 288 + 32> lens{};
  std::array<u32, pre_table_size> pre_table{};
  std::array<u32, lit_table_size> lit_table{};
  std::array<u32, dist_table_size> dist_table{};
};

Inflater::Impl::Outcome Inflater::Impl::run(InflateCursor& c) {
  for (;;) {
    switch (state) {
      case State::ZlibHeader: {
        if (!fill_scratch(c, 2)) return Outcome::NeedInput;
        const u32 cmf = scratch[0];
        const u32 flg = scratch[1];
        scratch_len = 0;
        if (((cmf << 8) | flg) % 31 != 0) return corrupt("corrupt zlib header");
        if ((cmf & 0x0Fu) != 8) return corrupt("zlib stream uses an unsupported compression method");
        if ((cmf >> 4) > 7) return corrupt("zlib stream declares an invalid window size");
        if (flg & 0x20u) return corrupt("zlib stream requires a preset dictionary");
        state = State::BlockHeader;
        break;
      }
      case State::GzipHeader: {
        // Ten fixed bytes; each is checked as soon as it arrives.
        const bool complete = fill_scratch(c, 10);
        if ((scratch_len >= 1 && scratch[0] != 0x1F) || (scratch_len >= 2 && scratch[1] != 0x8B)) {
          return corrupt("not a gzip stream");
        }
        if (scratch_len >= 3 && scratch[2] != 8) {
          return fail_with(Status::Decode, std::format("unsupported gzip compression method {}", scratch[2]));
        }
        if (scratch_len >= 4 && (scratch[3] & 0xE0u) != 0) return corrupt("gzip header uses reserved flags");
        if (!complete) return Outcome::NeedInput;
        gzip_flags = scratch[3];
        gzip.mtime = scratch_u32(4);
        header_crc = crc32(view_of(scratch.data(), 10));
        scratch_len = 0;
        state = State::GzipExtraLength;
        break;
      }
      case State::GzipExtraLength: {
        if (gzip_flags & 0x04u) {
          if (!fill_scratch(c, 2)) return Outcome::NeedInput;
          gzip_extra_left = scratch_u16(0);
          header_crc = crc32(view_of(scratch.data(), 2), header_crc);
          scratch_len = 0;
        }
        state = State::GzipExtra;
        break;
      }
      case State::GzipExtra: {
        if (gzip_extra_left > 0) {
          const std::size_t take = std::min<std::size_t>(gzip_extra_left, static_cast<std::size_t>(c.in_end - c.in));
          if (take > 0) header_crc = crc32(view_of(c.in, take), header_crc);
          c.in += take;
          gzip_extra_left -= static_cast<u32>(take);
          if (gzip_extra_left > 0) return Outcome::NeedInput;
        }
        state = State::GzipName;
        break;
      }
      case State::GzipName:
      case State::GzipComment: {
        const bool is_name = state == State::GzipName;
        if (gzip_flags & (is_name ? 0x08u : 0x10u)) {
          // A NUL-terminated string.
          const std::size_t available = static_cast<std::size_t>(c.in_end - c.in);
          if (available == 0) return Outcome::NeedInput;
          const void* const terminator = std::memchr(c.in, 0, available);
          const std::size_t text = terminator != nullptr
                                       ? static_cast<std::size_t>(static_cast<const u8*>(terminator) - c.in)
                                       : available;
          const std::size_t take = terminator != nullptr ? text + 1 : text;
          if (is_name) gzip.filename.append(reinterpret_cast<const char*>(c.in), text);
          header_crc = crc32(view_of(c.in, take), header_crc);
          c.in += take;
          if (terminator == nullptr) return Outcome::NeedInput;
        }
        state = is_name ? State::GzipComment : State::GzipHeaderCrc;
        break;
      }
      case State::GzipHeaderCrc: {
        if (gzip_flags & 0x02u) {
          if (!fill_scratch(c, 2)) return Outcome::NeedInput;
          const u32 stored = scratch_u16(0);
          scratch_len = 0;
          if (stored != (header_crc & 0xFFFFu)) return corrupt("gzip header checksum mismatch");
        }
        state = State::BlockHeader;
        break;
      }
      case State::BlockHeader: {
        if (!need_bits(c, 3)) return Outcome::NeedInput;
        last_block = (bitbuf & 1u) != 0;
        const unsigned type = static_cast<unsigned>(bitbuf >> 1) & 3u;
        drop_bits(3);
        if (type == 0) {
          align_to_byte();
          state = State::StoredLength;
        } else if (type == 1) {
          if (!use_fixed_tables()) return fail_with(Status::Internal, "could not build the fixed DEFLATE tables");
          state = State::Tokens;
        } else if (type == 2) {
          state = State::DynamicHeader;
        } else {
          return corrupt("invalid DEFLATE block type");
        }
        break;
      }
      case State::StoredLength: {
        if (!fill_scratch(c, 4)) return Outcome::NeedInput;
        const u32 length = scratch_u16(0);
        const u32 check = scratch_u16(2);
        scratch_len = 0;
        if ((length ^ 0xFFFFu) != check) return corrupt("corrupt stored block length");
        stored_left = length;
        state = State::Stored;
        break;
      }
      case State::Stored: {
        if (stored_left > 0) {
          const std::size_t take = std::min<std::size_t>(stored_left, static_cast<std::size_t>(c.in_end - c.in));
          if (take == 0) return Outcome::NeedInput;
          if (!c.reserve(take)) return over_limit(c);
          ::cvt::util::copy_bytes(c.data() + c.pos, c.in, take);
          c.pos += take;
          c.in += take;
          stored_left -= static_cast<u32>(take);
          if (stored_left > 0) return Outcome::NeedInput;
        }
        state = last_block ? State::Trailer : State::BlockHeader;
        break;
      }
      case State::DynamicHeader: {
        if (!need_bits(c, 14)) return Outcome::NeedInput;
        num_lit = (static_cast<unsigned>(bitbuf) & 31u) + 257;
        num_dist = (static_cast<unsigned>(bitbuf >> 5) & 31u) + 1;
        num_pre = (static_cast<unsigned>(bitbuf >> 10) & 15u) + 4;
        drop_bits(14);
        if (num_lit > num_lit_symbols || num_dist > num_dist_symbols) {
          return corrupt("too many length or distance codes in a DEFLATE block");
        }
        pre_lens.fill(0);
        length_index = 0;
        state = State::DynamicPrecode;
        break;
      }
      case State::DynamicPrecode: {
        while (length_index < num_pre) {
          if (!need_bits(c, 3)) return Outcome::NeedInput;
          pre_lens[precode_order[length_index++]] = static_cast<u8>(bitbuf & 7u);
          drop_bits(3);
        }
        if (!build_decode_table(pre_lens.data(), num_precode_symbols, precode_symbol_entries.data(), CodeKind::Precode,
                                pre_root_bits, pre_table.data(), pre_table.size())) {
          return corrupt("invalid code-length code in a DEFLATE block");
        }
        length_index = 0;
        state = State::DynamicLengths;
        break;
      }
      case State::DynamicLengths: {
        const unsigned total = num_lit + num_dist;
        while (length_index < total) {
          const u32 entry = pre_table[static_cast<std::size_t>(bitbuf) & (pre_table_size - 1)];
          const unsigned code_bits = entry & 0xFFu;
          const unsigned symbol = entry >> 16;
          const unsigned extra = symbol == 16 ? 2u : symbol == 17 ? 3u : symbol == 18 ? 7u : 0u;
          if (code_bits + extra > bitcnt) {
            if (!load_byte(c)) return Outcome::NeedInput;
            continue;
          }
          const unsigned extra_value = static_cast<unsigned>(bitbuf >> code_bits) & ((1u << extra) - 1);
          drop_bits(code_bits + extra);
          if (symbol < 16) {
            lens[length_index++] = static_cast<u8>(symbol);
            continue;
          }
          u8 value = 0;
          unsigned repeat = 0;
          if (symbol == 16) {
            if (length_index == 0) return corrupt("code-length repeat with nothing to repeat");
            value = lens[length_index - 1];
            repeat = 3 + extra_value;
          } else {
            repeat = (symbol == 17 ? 3u : 11u) + extra_value;
          }
          if (repeat > total - length_index) return corrupt("code-length repeat runs past the end");
          std::fill_n(lens.data() + length_index, repeat, value);
          length_index += repeat;
        }
        if (lens[end_of_block] == 0) return corrupt("DEFLATE block has no end-of-block code");
        if (!build_decode_table(lens.data(), num_lit, lit_symbol_entries.data(), CodeKind::LitLen, lit_root_bits,
                                lit_table.data(), lit_table.size())) {
          return corrupt("invalid literal/length code lengths");
        }
        if (!build_decode_table(lens.data() + num_lit, num_dist, dist_symbol_entries.data(), CodeKind::Dist,
                                dist_root_bits, dist_table.data(), dist_table.size())) {
          return corrupt("invalid distance code lengths");
        }
        fixed_tables = false;
        state = State::Tokens;
        break;
      }
      case State::Tokens: {
        const Outcome outcome = decode_tokens(c);
        if (outcome != Outcome::BlockEnd) return outcome;
        state = last_block ? State::Trailer : State::BlockHeader;
        break;
      }
      case State::Trailer: {
        if (format == Format::Raw) {
          state = State::Done;
          break;
        }
        // The checksum of the decompressed data: Adler-32, most significant
        // byte first (zlib), or CRC-32, least significant byte first (gzip).
        align_to_byte();
        if (!fill_scratch(c, 4)) return Outcome::NeedInput;
        update_checksum(c);
        scratch_len = 0;
        if (format == Format::Zlib) {
          const u32 stored = (static_cast<u32>(scratch[0]) << 24) | (static_cast<u32>(scratch[1]) << 16) |
                             (static_cast<u32>(scratch[2]) << 8) | static_cast<u32>(scratch[3]);
          if (stored != checksum) return corrupt("zlib checksum mismatch");
          state = State::Done;
        } else {
          if (scratch_u32(0) != checksum) return corrupt("gzip checksum mismatch");
          state = State::GzipLength;
        }
        break;
      }
      case State::GzipLength: {
        if (!fill_scratch(c, 4)) return Outcome::NeedInput;
        scratch_len = 0;
        const u64 size = total_out + (c.pos - c.base);
        if (scratch_u32(0) != static_cast<u32>(size & 0xFFFFFFFFu)) return corrupt("gzip length mismatch");
        state = State::Done;
        break;
      }
      case State::Done:
        return Outcome::Finished;
      case State::Failed:
        return Outcome::Failed;
    }
  }
}

Inflater::Impl::Outcome Inflater::Impl::decode_tokens(InflateCursor& c) {
  for (;;) {
    if (static_cast<std::size_t>(c.in_end - c.in) >= fast_input_margin) {
      if (c.cap - c.pos < fast_output_margin) (void)c.reserve(fast_output_margin);
      if (c.cap - c.pos >= fast_output_margin) {
        const Outcome outcome = decode_fast(c);
        if (outcome != Outcome::Continue) return outcome;
      }
    }

    // One literal or match, decoded without consuming anything until all of
    // its bits are present; input is loaded a byte at a time, only on demand.
    u32 entry = 0;
    unsigned skip = 0;
    if (!peek_code(lit_table.data(), lit_root_bits, bitbuf, bitcnt, entry, skip)) {
      if (!load_byte(c)) return Outcome::NeedInput;
      continue;
    }
    if (entry & entry_literal) {
      if (c.pos == c.cap && !c.reserve(1)) return over_limit(c);
      c.data()[c.pos++] = static_cast<u8>(entry >> 16);
      drop_bits(skip + (entry & 0xFFu));
      continue;
    }
    if (entry & entry_end) {
      drop_bits(skip + (entry & 0xFFu));
      return Outcome::BlockEnd;
    }
    if (entry & entry_invalid) return corrupt("invalid literal/length code");

    unsigned code_bits = (entry >> 8) & 15u;
    unsigned total_bits = entry & 0xFFu;
    const unsigned length =
        (entry >> 16) + (static_cast<unsigned>(bitbuf >> (skip + code_bits)) & ((1u << (total_bits - code_bits)) - 1));
    unsigned used = skip + total_bits;

    if (!peek_code(dist_table.data(), dist_root_bits, bitbuf >> used, bitcnt - used, entry, skip)) {
      if (!load_byte(c)) return Outcome::NeedInput;
      continue;
    }
    if (entry & entry_invalid) return corrupt("invalid distance code");
    code_bits = (entry >> 8) & 15u;
    total_bits = entry & 0xFFu;
    const std::size_t distance = (entry >> 16) + (static_cast<unsigned>(bitbuf >> (used + skip + code_bits)) &
                                                  ((1u << (total_bits - code_bits)) - 1));
    used += skip + total_bits;

    const std::size_t history = c.pos - c.base;
    if (distance > history && distance - history > window_fill) {
      return corrupt("invalid distance (before the start of the data)");
    }
    if (c.cap - c.pos < length && !c.reserve(length)) return over_limit(c);
    u8* const out = c.data() + c.pos;
    if (distance > history) {
      (void)copy_from_window(out, history, distance, length);
    } else {
      for (unsigned i = 0; i < length; ++i) out[i] = *(out + i - distance);
    }
    c.pos += length;
    drop_bits(used);
  }
}

Inflater::Impl::Outcome Inflater::Impl::decode_fast(InflateCursor& c) {
  u8* const out_data = c.data();
  u8* const out_begin = out_data + c.base;
  u8* const out_limit = out_data + (c.cap - fast_output_margin);
  u8* out = out_data + c.pos;
  const u8* const in_start = c.in;
  const u8* const in_limit = c.in_end - fast_input_margin;
  const u8* in = c.in;
  u64 bb = bitbuf;
  unsigned cnt = bitcnt;
  const u32* const lit = lit_table.data();
  const u32* const dist = dist_table.data();
  constexpr std::size_t lit_mask = (std::size_t{1} << lit_root_bits) - 1;
  constexpr std::size_t dist_mask = (std::size_t{1} << dist_root_bits) - 1;
  Outcome outcome = Outcome::Continue;
  const char* problem = nullptr;

  // Top the bit buffer up to at least 56 bits. Bits above `cnt` then hold a
  // preview of the following input; refilling ORs the same values in again.
  const auto refill = [&]() noexcept {
    bb |= load64le(in) << cnt;
    in += (63 - cnt) >> 3;
    cnt |= 56;
  };
  const auto lookup_literal_or_length = [&]() noexcept {
    u32 e = lit[static_cast<std::size_t>(bb) & lit_mask];
    if (e & entry_subtable) {
      bb >>= lit_root_bits;
      cnt -= lit_root_bits;
      e = lit[(e >> 16) + (static_cast<u32>(bb) & ((u32{1} << ((e >> 8) & 15u)) - 1))];
    }
    return e;
  };

  while (in <= in_limit && out <= out_limit) {
    refill();
    u32 entry = lookup_literal_or_length();
    if (entry & entry_literal) {
      // Up to three literals fit in one refill.
      bb >>= entry & 0xFFu;
      cnt -= entry & 0xFFu;
      *out++ = static_cast<u8>(entry >> 16);
      entry = lookup_literal_or_length();
      if (entry & entry_literal) {
        bb >>= entry & 0xFFu;
        cnt -= entry & 0xFFu;
        *out++ = static_cast<u8>(entry >> 16);
        entry = lookup_literal_or_length();
        if (entry & entry_literal) {
          bb >>= entry & 0xFFu;
          cnt -= entry & 0xFFu;
          *out++ = static_cast<u8>(entry >> 16);
          continue;
        }
      }
    }
    if (entry & (entry_end | entry_invalid)) {
      if (entry & entry_end) {
        bb >>= entry & 0xFFu;
        cnt -= entry & 0xFFu;
        outcome = Outcome::BlockEnd;
      } else {
        problem = "invalid literal/length code";
      }
      break;
    }
    unsigned total_bits = entry & 0xFFu;
    unsigned code_bits = (entry >> 8) & 15u;
    const unsigned length = (entry >> 16) + (static_cast<u32>(bb >> code_bits) & ((u32{1} << (total_bits - code_bits)) - 1));
    bb >>= total_bits;
    cnt -= total_bits;

    refill();
    entry = dist[static_cast<std::size_t>(bb) & dist_mask];
    if (entry & entry_subtable) {
      bb >>= dist_root_bits;
      cnt -= dist_root_bits;
      entry = dist[(entry >> 16) + (static_cast<u32>(bb) & ((u32{1} << ((entry >> 8) & 15u)) - 1))];
    }
    if (entry & entry_invalid) {
      problem = "invalid distance code";
      break;
    }
    total_bits = entry & 0xFFu;
    code_bits = (entry >> 8) & 15u;
    const std::size_t distance =
        (entry >> 16) + (static_cast<u32>(bb >> code_bits) & ((u32{1} << (total_bits - code_bits)) - 1));
    bb >>= total_bits;
    cnt -= total_bits;

    const std::size_t history = static_cast<std::size_t>(out - out_begin);
    if (distance > history) {
      if (distance - history > window_fill) {
        problem = "invalid distance (before the start of the data)";
        break;
      }
      out = copy_from_window(out, history, distance, length);
      continue;
    }
    const u8* src = out - distance;
    u8* dst = out;
    out += length;
    if (distance >= 8) {
      // Non-overlapping 8-byte steps; may write up to 7 bytes past the match,
      // inside the output margin, which later output overwrites.
      do {
        ::cvt::util::copy_bytes(dst, src, 8);
        dst += 8;
        src += 8;
      } while (dst < out);
    } else if (distance == 1) {
      ::cvt::util::fill_bytes(dst, *src, length);
    } else {
      // A short period: copy bytes until a multiple of the period that is at
      // least 8 has been written, then continue in 8-byte steps from there.
      const std::size_t period = distance * ((7 + distance) / distance);
      const std::size_t head = std::min<std::size_t>(length, period);
      for (std::size_t i = 0; i < head; ++i) dst[i] = src[i];
      dst += head;
      src = dst - period;
      while (dst < out) {
        ::cvt::util::copy_bytes(dst, src, 8);
        dst += 8;
        src += 8;
      }
    }
  }

  // Hand back the whole bytes that were loaded but not used.
  const std::size_t unread = std::min<std::size_t>(cnt >> 3, static_cast<std::size_t>(in - in_start));
  in -= unread;
  cnt -= static_cast<unsigned>(unread) * 8;
  bb &= (u64{1} << cnt) - 1;
  bitbuf = bb;
  bitcnt = cnt;
  c.in = in;
  c.pos = static_cast<std::size_t>(out - out_data);
  if (problem != nullptr) return corrupt(problem);
  return outcome;
}

Inflater::Inflater(Format format) : impl_(std::make_unique<Impl>(format)) {}
Inflater::~Inflater() = default;
Inflater::Inflater(Inflater&&) noexcept = default;
Inflater& Inflater::operator=(Inflater&&) noexcept = default;

bool Inflater::finished() const noexcept { return impl_->state == Impl::State::Done; }
std::uint64_t Inflater::total_in() const noexcept { return impl_->total_in; }
std::uint64_t Inflater::total_out() const noexcept { return impl_->total_out; }
const GzipHeader& Inflater::gzip_header() const noexcept { return impl_->gzip; }
void Inflater::reset() { impl_->reset(); }

Result<Inflater::Progress> Inflater::feed(ByteView input, Bytes& output, std::uint64_t max_total_output) {
  Impl& s = *impl_;
  if (s.state == Impl::State::Failed) return fail(s.error);
  if (s.state == Impl::State::Done) return Progress{0, 0, true};

  static constexpr u8 no_input = 0;
  InflateCursor c;
  // An empty span may carry a null pointer; give the cursor a real address.
  c.in = input.empty() ? &no_input : reinterpret_cast<const u8*>(input.data());
  c.in_end = c.in + input.size();
  const u8* const in_begin = c.in;
  c.out = &output;
  c.base = c.pos = c.cap = c.checked = output.size();
  const u64 allowed = max_total_output > s.total_out ? max_total_output - s.total_out : 0;
  const u64 addressable = static_cast<u64>(output.max_size() - output.size());
  c.address_bound = addressable < allowed;
  c.hard_cap = c.base + static_cast<std::size_t>(std::min(allowed, addressable));
  c.limit = max_total_output;
  // Small inputs get small buffers so that feeding byte by byte stays cheap.
  c.min_room = input.size() < fast_input_margin
                   ? 64
                   : std::clamp<std::size_t>(std::min<std::size_t>(input.size(), std::size_t{1} << 18) * 4, 512,
                                             std::size_t{1} << 20);

  const Impl::Outcome outcome = s.run(c);
  if (outcome != Impl::Outcome::Failed) s.update_checksum(c);
  if (outcome == Impl::Outcome::NeedInput) s.save_window(c);
  output.resize(c.pos);

  Progress progress;
  progress.consumed = static_cast<std::size_t>(c.in - in_begin);
  progress.produced = c.pos - c.base;
  progress.finished = outcome == Impl::Outcome::Finished;
  s.total_in += progress.consumed;
  s.total_out += progress.produced;
  if (outcome == Impl::Outcome::Failed) return fail(s.error);
  return progress;
}

// --------------------------------------------------------------------------- compressor: tables

namespace {

struct LevelConfig {
  u16 good_length;  ///< lazy parsing: with a match at least this long, search the next position less hard
  u16 max_lazy;     ///< lazy: do not look for a better match once one is this long; greedy: longest match whose
                    ///< inner positions are all entered in the hash table
  u16 nice_length;  ///< stop searching when a match of this length is found
  u16 max_chain;    ///< hash chain entries examined per search
  bool lazy;
};
// The effort ladder. zlib's table is the reference point; level 4 searches a little harder so that every
// step up is also a step down in size now that the greedy levels keep more hash entries. With chains of
// 4-byte prefixes a walk wastes no steps on 3-byte look-alikes, so level 6 affords a longer chain.
constexpr std::array<LevelConfig, 10> level_configs = {{
    {0, 0, 0, 0, false},
    {4, 4, 8, 4, false},
    {4, 5, 16, 8, false},
    {4, 6, 32, 32, false},
    {4, 6, 32, 24, true},
    {8, 16, 32, 32, true},
    {8, 16, 128, 160, true},
    {8, 32, 128, 256, true},
    {32, 128, 258, 1024, true},
    {32, 258, 258, 4096, true},
}};

/// A match of the minimum length is only worth coding when it is close.
constexpr unsigned short_match_max_distance = 4096;

constexpr std::size_t deflate_chunk = 8 * window_size;                 // new data compressed per step
constexpr std::size_t deflate_buffer_limit = window_size + deflate_chunk;
constexpr unsigned max_hash_bits = 15;
constexpr i32 no_position = -(i32{1} << 30);
constexpr std::size_t split_unit_tokens = 1024;  // granularity of block boundaries
// Greedy levels do not enter the inside of a long match in the hash table,
// except for its last few positions: they cost little and are where the next
// match is most likely to be found (runs and short periods in particular).
constexpr std::size_t skipped_match_tail = 16;

struct SymbolLookup {
  std::array<u8, max_match + 1> length{};  ///< match length -> length symbol - 257
  std::array<u8, 256> dist_low{};          ///< distance - 1 (< 256) -> distance symbol
  std::array<u8, 256> dist_high{};         ///< (distance - 1) >> 7 -> distance symbol
};
constexpr SymbolLookup make_symbol_lookup() noexcept {
  SymbolLookup t{};
  for (unsigned i = 0; i < 29; ++i) {
    const unsigned last = std::min<unsigned>(max_match, length_base[i] + (1u << length_extra[i]) - 1);
    for (unsigned len = length_base[i]; len <= last; ++len) t.length[len] = static_cast<u8>(i);
  }
  for (unsigned i = 0; i < 30; ++i) {
    const unsigned first = dist_base[i] - 1u;
    const unsigned last = first + (1u << dist_extra[i]);
    for (unsigned d = first; d < last; d += d < 256 ? 1u : 128u) {
      if (d < 256) {
        t.dist_low[d] = static_cast<u8>(i);
      } else {
        t.dist_high[d >> 7] = static_cast<u8>(i);
      }
    }
  }
  return t;
}
constexpr SymbolLookup symbol_lookup = make_symbol_lookup();

[[nodiscard]] inline unsigned distance_symbol(unsigned distance) noexcept {
  const unsigned d = distance - 1;
  return d < 256 ? symbol_lookup.dist_low[d] : symbol_lookup.dist_high[d >> 7];
}

/// log2(1 + i/256) in 16.16 fixed point, computed with integers only so the
/// block-splitting decisions are identical on every platform.
constexpr std::array<u32, 257> make_log2_table() noexcept {
  std::array<u32, 257> table{};
  for (u32 i = 0; i < 256; ++i) {
    // Binary logarithm of x in [1, 2) by repeated squaring, x in 2.30 fixed point.
    u64 x = (u64{256} + i) << 22;
    u32 result = 0;
    for (unsigned bit = 0; bit < 16; ++bit) {
      x = (x * x) >> 30;
      result <<= 1;
      if (x >= (u64{2} << 30)) {
        result |= 1u;
        x >>= 1;
      }
    }
    table[i] = result;
  }
  table[256] = u32{1} << 16;
  return table;
}
constexpr std::array<u32, 257> log2_table = make_log2_table();

/// log2(x) in 16.16 fixed point, x >= 1.
[[nodiscard]] inline u32 log2_fixed(u32 x) noexcept {
  const unsigned exponent = static_cast<unsigned>(std::bit_width(x)) - 1;
  // The 24 bits below the leading one: 8 select a table entry, 16 interpolate.
  const u32 mantissa = exponent >= 24 ? (x >> (exponent - 24)) & 0xFFFFFFu : (x << (24 - exponent)) & 0xFFFFFFu;
  const u32 index = mantissa >> 16;
  const u32 fraction = mantissa & 0xFFFFu;
  const u32 low = log2_table[index];
  const u32 high = log2_table[index + 1];
  return (static_cast<u32>(exponent) << 16) + low + static_cast<u32>((static_cast<u64>(high - low) * fraction) >> 16);
}

/// Shannon cost, in 1/65536 bit, of coding the symbols counted in `freq`.
[[nodiscard]] u64 entropy_fixed(const u32* freq, unsigned count, unsigned& distinct) noexcept {
  u64 total = 0;
  u64 weighted = 0;
  for (unsigned i = 0; i < count; ++i) {
    if (freq[i] == 0) continue;
    ++distinct;
    total += freq[i];
    weighted += static_cast<u64>(freq[i]) * log2_fixed(freq[i]);
  }
  if (total == 0) return 0;
  const u64 all = total * log2_fixed(static_cast<u32>(std::min<u64>(total, 0xFFFFFFFFu)));
  return all > weighted ? all - weighted : 0;
}

/// Symbol statistics of a run of tokens.
struct Histogram {
  std::array<u32, num_lit_symbols> lit{};
  std::array<u32, num_dist_symbols> dist{};
  u64 extra_bits = 0;     ///< extra bits of all lengths and distances
  std::size_t bytes = 0;  ///< input bytes the tokens stand for

  void add(const Histogram& other) noexcept {
    for (unsigned i = 0; i < num_lit_symbols; ++i) lit[i] += other.lit[i];
    for (unsigned i = 0; i < num_dist_symbols; ++i) dist[i] += other.dist[i];
    extra_bits += other.extra_bits;
    bytes += other.bytes;
  }
};

// A token is a literal (the byte value, distance field zero) or a match
// (length in the low 16 bits, distance in the high 16).
[[nodiscard]] Histogram count_tokens(const u32* tokens, std::size_t count) noexcept {
  Histogram h;
  for (std::size_t i = 0; i < count; ++i) {
    const u32 token = tokens[i];
    const u32 distance = token >> 16;
    if (distance == 0) {
      ++h.lit[token];
      ++h.bytes;
      continue;
    }
    const u32 length = token & 0xFFFFu;
    const unsigned ls = symbol_lookup.length[length];
    const unsigned ds = distance_symbol(distance);
    ++h.lit[257 + ls];
    ++h.dist[ds];
    h.extra_bits += static_cast<u64>(length_extra[ls]) + dist_extra[ds];
    h.bytes += length;
  }
  return h;
}

/// Estimated size in bits of a dynamic block for these statistics (without
/// the extra bits, which do not depend on where blocks are cut).
[[nodiscard]] u64 estimate_block_bits(const Histogram& h) noexcept {
  unsigned distinct = 0;
  const u64 payload = entropy_fixed(h.lit.data(), num_lit_symbols, distinct) +
                      entropy_fixed(h.dist.data(), num_dist_symbols, distinct);
  // Header: block type, counts, the code-length code, and roughly four bits
  // for every code length that has to be described.
  return (payload >> 16) + 3 + 14 + 3 * num_precode_symbols + 4 * static_cast<u64>(distinct) + 24;
}

// --------------------------------------------------------------------------- compressor: Huffman codes

/// Scratch memory for `build_code_lengths` (about 29 KiB): kept in the
/// compressor object rather than on the stack, which is small on some targets.
struct CodeLengthWorkspace {
  static constexpr unsigned max_leaves = 288;
  struct Leaf {
    u32 weight;
    u16 symbol;
  };
  std::array<Leaf, max_leaves> leaves{};
  std::array<std::array<u16, 2 * max_leaves>, max_code_bits> leaves_in_prefix{};
  std::array<u64, 2 * max_leaves> previous{};
  std::array<u64, 2 * max_leaves> current{};
};

/// Optimal code lengths for the given symbol frequencies, none longer than
/// `limit` bits (package-merge). Symbols with frequency 0 get length 0,
/// except that at least two symbols always receive a code, so the result is
/// a complete prefix code that every decoder accepts.
void build_code_lengths(const u32* freq, unsigned count, unsigned limit, u8* lens,
                        CodeLengthWorkspace& work) noexcept {
  using Leaf = CodeLengthWorkspace::Leaf;
  auto& leaves = work.leaves;
  auto& leaves_in_prefix = work.leaves_in_prefix;
  auto& previous = work.previous;
  auto& current = work.current;
  unsigned m = 0;
  for (unsigned i = 0; i < count; ++i) {
    lens[i] = 0;
    if (freq[i] != 0) leaves[m++] = Leaf{freq[i], static_cast<u16>(i)};
  }
  if (m == 0) {
    leaves[0] = Leaf{1, 0};
    leaves[1] = Leaf{1, 1};
    m = 2;
  } else if (m == 1) {
    leaves[1] = Leaf{1, static_cast<u16>(leaves[0].symbol == 0 ? 1 : 0)};
    m = 2;
  }
  std::sort(leaves.begin(), leaves.begin() + m, [](const Leaf& a, const Leaf& b) {
    return a.weight != b.weight ? a.weight < b.weight : a.symbol < b.symbol;
  });

  // Level k holds the cheapest items for codes of at most k + 1 bits: the
  // leaves merged with pairs ("packages") of the items of level k - 1. Only
  // the first 2m - 2 items of a level can ever be selected. Because leaves
  // and packages each appear in increasing order, a prefix of a level is
  // described by how many leaves it contains.
  const unsigned levels = std::min(limit, m - 1);
  const unsigned keep = 2 * m - 2;
  std::array<u16, max_code_bits> level_size{};

  for (unsigned i = 0; i < m; ++i) {
    previous[i] = leaves[i].weight;
    leaves_in_prefix[0][i] = static_cast<u16>(i);
  }
  leaves_in_prefix[0][m] = static_cast<u16>(m);
  level_size[0] = static_cast<u16>(m);
  for (unsigned level = 1; level < levels; ++level) {
    const unsigned packages = level_size[level - 1] / 2u;
    unsigned leaf = 0;
    unsigned package = 0;
    unsigned size = 0;
    leaves_in_prefix[level][0] = 0;
    while (size < keep && (leaf < m || package < packages)) {
      const u64 package_weight = package < packages ? previous[2 * package] + previous[2 * package + 1] : 0;
      if (leaf < m && (package >= packages || leaves[leaf].weight <= package_weight)) {
        current[size] = leaves[leaf].weight;
        ++leaf;
      } else {
        current[size] = package_weight;
        ++package;
      }
      ++size;
      leaves_in_prefix[level][size] = static_cast<u16>(leaf);
    }
    level_size[level] = static_cast<u16>(size);
    std::copy_n(current.begin(), size, previous.begin());
  }

  // Select the first 2m - 2 items of the last level; every package selected
  // there selects two items one level down. A leaf's code length is the
  // number of levels in which it is selected.
  unsigned selected = keep;
  for (unsigned level = levels; level-- > 0;) {
    selected = std::min<unsigned>(selected, level_size[level]);
    const unsigned leaf_count = leaves_in_prefix[level][selected];
    for (unsigned i = 0; i < leaf_count; ++i) ++lens[leaves[i].symbol];
    selected = 2 * (selected - leaf_count);
  }
}

/// Canonical codes for the given lengths, already bit-reversed for output.
constexpr void assign_codes(const u8* lens, unsigned count, u16* codes) noexcept {
  std::array<u16, max_code_bits + 1> length_count{};
  for (unsigned i = 0; i < count; ++i) ++length_count[lens[i]];
  length_count[0] = 0;
  std::array<u16, max_code_bits + 1> next{};
  u32 code = 0;
  for (unsigned len = 1; len <= max_code_bits; ++len) {
    code = (code + length_count[len - 1]) << 1;
    next[len] = static_cast<u16>(code);
  }
  for (unsigned i = 0; i < count; ++i) {
    codes[i] = lens[i] != 0 ? static_cast<u16>(reverse_bits(next[lens[i]]++, lens[i])) : u16{0};
  }
}

struct FixedCodes {
  std::array<u16, 288> lit{};
  std::array<u16, 32> dist{};
};
constexpr FixedCodes make_fixed_codes() noexcept {
  FixedCodes codes{};
  assign_codes(fixed_lit_lengths.data(), 288, codes.lit.data());
  assign_codes(fixed_dist_lengths.data(), 32, codes.dist.data());
  return codes;
}
constexpr FixedCodes fixed_codes = make_fixed_codes();

/// One symbol of the run-length coded code lengths (RFC 1951, 3.2.7).
struct LengthCode {
  u8 symbol;  ///< 0-15: a length; 16: repeat previous; 17, 18: runs of zeros
  u8 extra;   ///< value of the extra bits
};

/// Run-length code `count` code lengths. Returns the number of symbols.
unsigned run_length_code(const u8* lens, unsigned count, LengthCode* out) noexcept {
  unsigned n = 0;
  unsigned i = 0;
  while (i < count) {
    const u8 value = lens[i];
    unsigned run = 1;
    while (i + run < count && lens[i + run] == value) ++run;
    i += run;
    if (value == 0) {
      while (run >= 11) {
        const unsigned take = std::min(run, 138u);
        out[n++] = LengthCode{18, static_cast<u8>(take - 11)};
        run -= take;
      }
      if (run >= 3) {
        out[n++] = LengthCode{17, static_cast<u8>(run - 3)};
        run = 0;
      }
    } else {
      out[n++] = LengthCode{value, 0};
      --run;
      while (run >= 3) {
        const unsigned take = std::min(run, 6u);
        out[n++] = LengthCode{16, static_cast<u8>(take - 3)};
        run -= take;
      }
    }
    for (; run > 0; --run) out[n++] = LengthCode{value, 0};
  }
  return n;
}

constexpr unsigned precode_extra_bits(unsigned symbol) noexcept {
  return symbol == 16 ? 2u : symbol == 17 ? 3u : symbol == 18 ? 7u : 0u;
}

/// Grow `out` to `size` bytes with geometric capacity growth.
void grow_to(Bytes& out, std::size_t size) {
  if (size > out.capacity()) out.reserve(std::max(size, out.capacity() + out.capacity() / 2));
  out.resize(size);
}

/// Appends bits to a byte vector, least significant bit first.
struct BitWriter {
  Bytes& out;
  std::size_t pos;  ///< bytes of `out` that are final; the vector may be larger
  u64 acc;          ///< pending bits
  unsigned bits;    ///< number of pending bits

  /// Make sure `bytes` more bytes (plus slack for `flush`) can be written.
  void reserve(std::size_t bytes) {
    if (out.size() < pos + bytes + 8) grow_to(out, pos + bytes + 8);
  }
  [[nodiscard]] u8* cursor() noexcept { return reinterpret_cast<u8*>(out.data()) + pos; }
  /// Queue `count` bits; the caller keeps the total at 63 or fewer.
  void put(u32 value, unsigned count) noexcept {
    acc |= static_cast<u64>(value) << bits;
    bits += count;
  }
  /// Write out all whole pending bytes. Needs 8 writable bytes at `pos`.
  void flush() noexcept {
    store64le(cursor(), acc);
    pos += bits >> 3;
    acc >>= bits & ~7u;
    bits &= 7u;
  }
  void put_flush(u32 value, unsigned count) noexcept {
    put(value, count);
    flush();
  }
  /// Pad with zero bits to the next byte boundary.
  void align() noexcept {
    flush();
    if (bits > 0) {
      bits = 8;
      flush();
    }
  }
};

}  // namespace

// --------------------------------------------------------------------------- Deflater

struct Deflater::Impl {
  Impl(Format stream_format, int compression_level, const GzipHeader& header)
      : format(stream_format), level(std::clamp(compression_level, 0, 9)), gzip(header) {
    checksum = format == Format::Zlib ? 1u : 0u;
  }

  struct Match {
    unsigned length;
    unsigned distance;
  };

  void write_header(Bytes& out);
  void write_trailer(Bytes& out);
  void feed_stored(ByteView input, Bytes& out);
  void feed_compressed(ByteView input, Bytes& out);
  void compress_pending(Bytes& out, bool final);
  void prepare_matcher(bool final);
  void slide();
  [[nodiscard]] std::size_t parse(std::size_t start, std::size_t end);
  [[nodiscard]] Match find_match(const u8* data, std::size_t pos, std::size_t end, unsigned floor_length,
                                 unsigned max_chain, unsigned nice_length) noexcept;
  void insert_until(const u8* data, std::size_t pos) noexcept;
  void emit_blocks(Bytes& out, std::size_t token_count, const u8* data, bool final);
  void emit_block(Bytes& out, const u32* block_tokens, std::size_t token_count, Histogram& histogram, const u8* data,
                  bool final);
  void emit_stored(BitWriter& writer, const u8* data, std::size_t size, bool final);

  // The hash chains link positions with equal 4-byte prefixes, so a chain
  // walk only meets candidates that can give a match of 4 or more bytes; the
  // most recent position of each 3-byte prefix is kept separately for the
  // short matches (worth coding only when close). Positions whose fourth
  // byte lies beyond the data hash a zero in its place.
  [[nodiscard]] u32 hash_at(const u8* p) const noexcept {
    const u32 fourth = p + 3 < data_end ? p[3] : 0u;
    const u32 value = static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) | (fourth << 24);
    return (value * 0x9E3779B1u) >> hash_shift;
  }
  [[nodiscard]] u32 hash3_at(const u8* p) const noexcept {
    const u32 value = static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16);
    return (value * 0x9E3779B1u) >> hash_shift;
  }

  Format format;
  int level;
  GzipHeader gzip;
  bool header_written = false;
  bool done = false;
  bool matcher_ready = false;
  u32 checksum = 0;
  u64 total_in = 0;
  u64 total_out = 0;
  u64 bit_acc = 0;        ///< bits not yet written (fewer than 8 between calls)
  unsigned bit_count = 0;
  std::vector<u8> buf;    ///< [0, history): already compressed, kept for matches; then the pending input
  std::size_t history = 0;
  std::size_t next_insert = 0;  ///< first position not yet entered in the hash table
  unsigned hash_shift = 0;
  u32 chain_mask = 0;
  std::vector<i32> head;  ///< 4-byte hash -> most recent position
  std::vector<i32> head3; ///< 3-byte hash -> most recent position
  const u8* data_end = nullptr;  ///< end of the buffered data (for hashing near the end)
  std::vector<u16> prev;  ///< position & chain_mask -> distance to the previous position with the same hash, 0 = none
  std::vector<u32> tokens;
  std::unique_ptr<CodeLengthWorkspace> code_workspace;  ///< allocated with the matcher tables
};

void Deflater::Impl::write_header(Bytes& out) {
  header_written = true;
  if (format == Format::Zlib) {
    const unsigned cmf = 0x78;  // deflate, 32 KiB window
    const unsigned strength = level < 2 ? 0u : level < 6 ? 1u : level == 6 ? 2u : 3u;
    unsigned flg = strength << 6;
    flg += 31 - ((cmf << 8) + flg) % 31;
    out.push_back(static_cast<std::byte>(cmf));
    out.push_back(static_cast<std::byte>(flg));
  } else if (format == Format::Gzip) {
    const bool has_name = !gzip.filename.empty() && gzip.filename.find('\0') == std::string::npos;
    ByteWriter header(16 + gzip.filename.size());
    header.u8(0x1F).u8(0x8B).u8(8);  // magic, CM = deflate
    header.u8(static_cast<std::uint8_t>(has_name ? 0x08 : 0x00));  // FLG: FNAME
    header.u32le(gzip.mtime);
    header.u8(static_cast<std::uint8_t>(level >= 9 ? 2 : level <= 1 ? 4 : 0));  // XFL
    header.u8(255);  // OS: unknown (keeps output platform independent)
    if (has_name) header.text(gzip.filename).u8(0);
    append(out, header.view());
  }
}

void Deflater::Impl::write_trailer(Bytes& out) {
  if (format == Format::Zlib) {
    ByteWriter trailer(4);
    trailer.u32be(checksum);
    append(out, trailer.view());
  } else if (format == Format::Gzip) {
    ByteWriter trailer(8);
    trailer.u32le(checksum);
    trailer.u32le(static_cast<u32>(total_in & 0xFFFFFFFFu));
    append(out, trailer.view());
  }
}

/// Write `size` bytes as stored blocks (several when they exceed 65535
/// bytes). `final` marks the last of them as the last block of the stream.
void Deflater::Impl::emit_stored(BitWriter& writer, const u8* data, std::size_t size, bool final) {
  do {
    const std::size_t take = std::min(size, max_stored_block);
    const bool last = take == size;
    writer.reserve(take + 16);
    writer.put_flush(final && last ? 1u : 0u, 3);
    writer.align();
    writer.put_flush(static_cast<u32>(take) | ((static_cast<u32>(take) ^ 0xFFFFu) << 16), 32);
    if (take > 0) ::cvt::util::copy_bytes(writer.cursor(), data, take);
    writer.pos += take;
    data += take;
    size -= take;
  } while (size > 0);
}

/// Level 0: blocks of exactly 65535 bytes, the remainder at `finish()`.
void Deflater::Impl::feed_stored(ByteView input, Bytes& out) {
  BitWriter writer{out, out.size(), bit_acc, bit_count};
  while (!input.empty()) {
    if (buf.size() == max_stored_block) {
      emit_stored(writer, buf.data(), buf.size(), false);
      buf.clear();
    }
    if (buf.empty() && input.size() > max_stored_block) {
      emit_stored(writer, reinterpret_cast<const u8*>(input.data()), max_stored_block, false);
      input = input.subspan(max_stored_block);
      continue;
    }
    const std::size_t take = std::min(input.size(), max_stored_block - buf.size());
    const u8* const bytes = reinterpret_cast<const u8*>(input.data());
    buf.insert(buf.end(), bytes, bytes + take);
    input = input.subspan(take);
  }
  out.resize(writer.pos);
  bit_acc = writer.acc;
  bit_count = writer.bits;
}

void Deflater::Impl::feed_compressed(ByteView input, Bytes& out) {
  while (!input.empty()) {
    if (buf.size() == deflate_buffer_limit) {
      // The buffer is full and more data follows: compress it and keep the
      // last 32 KiB as history.
      compress_pending(out, false);
      slide();
    }
    const std::size_t take = std::min(input.size(), deflate_buffer_limit - buf.size());
    const u8* const bytes = reinterpret_cast<const u8*>(input.data());
    buf.insert(buf.end(), bytes, bytes + take);
    input = input.subspan(take);
  }
}

void Deflater::Impl::prepare_matcher(bool final) {
  if (matcher_ready) return;
  matcher_ready = true;
  code_workspace = std::make_unique<CodeLengthWorkspace>();
  // A short stream that is complete gets tables sized to match, which keeps
  // compressing many small buffers cheap. The choice depends only on the
  // length of the stream, never on how it was fed.
  const bool small = final && buf.size() < window_size;
  const std::size_t span = small ? std::bit_ceil(std::max<std::size_t>(buf.size(), 256)) : window_size;
  const unsigned hash_bits =
      small ? std::clamp(static_cast<unsigned>(std::bit_width(buf.size())), 8u, max_hash_bits) : max_hash_bits;
  hash_shift = 32 - hash_bits;
  chain_mask = static_cast<u32>(span - 1);
  head.assign(std::size_t{1} << hash_bits, no_position);
  head3.assign(std::size_t{1} << hash_bits, no_position);
  prev.assign(span, 0);
}

/// Drop everything but the last 32 KiB of the buffer.
void Deflater::Impl::slide() {
  if (buf.size() <= window_size) {
    history = buf.size();
    return;
  }
  // The shift is a multiple of the window size, so `position & chain_mask`
  // is unchanged and only the absolute positions in `head` need adjusting.
  const std::size_t shift = buf.size() - window_size;
  ::cvt::util::move_bytes(buf.data(), buf.data() + shift, window_size);
  buf.resize(window_size);
  history = window_size;
  next_insert -= shift;
  const i32 delta = static_cast<i32>(shift);
  for (i32& position : head) position = position >= delta ? position - delta : no_position;
  for (i32& position : head3) position = position >= delta ? position - delta : no_position;
}

/// Enter the positions [next_insert, pos) in the hash table. Each needs
/// three readable bytes, which the callers guarantee.
void Deflater::Impl::insert_until(const u8* data, std::size_t pos) noexcept {
  for (; next_insert < pos; ++next_insert) {
    const u32 h = hash_at(data + next_insert);
    const i32 here = static_cast<i32>(next_insert);
    const i32 gap = here - head[h];
    prev[next_insert & chain_mask] = static_cast<u16>(gap <= static_cast<i32>(window_size) ? gap : 0);
    head[h] = here;
    head3[hash3_at(data + next_insert)] = here;
  }
}

/// Number of equal bytes at `a` and `b`, at most `limit` (both readable for
/// `limit` bytes).
[[nodiscard]] static inline unsigned match_length(const u8* a, const u8* b, unsigned limit) noexcept {
  unsigned n = 0;
  while (n + 8 <= limit) {
    const u64 diff = load64le(a + n) ^ load64le(b + n);
    if (diff != 0) return n + (static_cast<unsigned>(std::countr_zero(diff)) >> 3);
    n += 8;
  }
  while (n < limit && a[n] == b[n]) ++n;
  return n;
}

/// Enter `pos` in the hash table and look along its chain for the longest
/// earlier match that is longer than `floor_length`. Requires
/// `next_insert == pos` and at least three bytes at `pos`. Returns
/// `floor_length` and distance 0 when there is none.
Deflater::Impl::Match Deflater::Impl::find_match(const u8* data, std::size_t pos, std::size_t end, unsigned floor_length,
                                                 unsigned max_chain, unsigned nice_length) noexcept {
  const u8* const here = data + pos;
  const u32 h = hash_at(here);
  const i32 position = static_cast<i32>(pos);
  i32 candidate = head[h];
  const i32 gap = position - candidate;
  prev[pos & chain_mask] = static_cast<u16>(gap <= static_cast<i32>(window_size) ? gap : 0);
  head[h] = position;
  const u32 h3 = hash3_at(here);
  const i32 short_candidate = head3[h3];
  head3[h3] = position;
  next_insert = pos + 1;

  Match best{floor_length, 0};
  const unsigned limit = static_cast<unsigned>(std::min<std::size_t>(max_match, end - pos));
  if (floor_length >= limit) return best;
  // A 3-byte match, considered only when nothing longer is wanted yet: the
  // nearest one is the best (the parser codes it only when close).
  if (floor_length < min_match && short_candidate != no_position && position - short_candidate <= static_cast<i32>(short_match_max_distance)) {
    const u8* const there = data + short_candidate;
    if (there[0] == here[0] && there[1] == here[1] && there[2] == here[2]) {
      best.length = min_match;
      best.distance = static_cast<unsigned>(position - short_candidate);
      // The chain walk below reads here[best.length]: nothing longer fits.
      if (best.length >= limit) return best;
    }
  }
  nice_length = std::min(nice_length, limit);
  // Candidates strictly decrease along a chain, so the walk always ends. The
  // chain link of a candidate within the window has not been overwritten by
  // a newer position (the slot written for `pos` above belongs to a
  // candidate exactly one window back, whose link then leads out of range).
  const i32 oldest = position - static_cast<i32>(window_size);
  while (candidate >= oldest) {
    const u8* const there = data + candidate;
    if (there[best.length] == here[best.length] && there[0] == here[0] && there[1] == here[1]) {
      const unsigned length = match_length(there, here, limit);
      if (length > best.length) {
        best.length = length;
        best.distance = static_cast<unsigned>(position - candidate);
        if (length >= nice_length) break;
      }
    }
    if (--max_chain == 0) break;
    const unsigned back = prev[static_cast<std::size_t>(candidate) & chain_mask];
    if (back == 0) break;
    candidate -= static_cast<i32>(back);
  }
  return best;
}

/// LZ77-parse buf[start, end) into `tokens`. Returns the number of tokens.
std::size_t Deflater::Impl::parse(std::size_t start, std::size_t end) {
  const LevelConfig config = level_configs[static_cast<std::size_t>(level)];
  const u8* const data = buf.data();
  u32* const out = tokens.data();
  std::size_t count = 0;
  std::size_t pos = start;
  while (pos < end) {
    if (end - pos < min_match) {
      out[count++] = data[pos++];
      continue;
    }
    insert_until(data, pos);
    Match match = find_match(data, pos, end, min_match - 1, config.max_chain, config.nice_length);
    if (match.length < min_match || (match.length == min_match && match.distance > short_match_max_distance)) {
      out[count++] = data[pos++];
      continue;
    }
    if (config.lazy) {
      // If the next position starts a longer match, code this byte as a
      // literal and take that match instead; repeat from there.
      while (match.length < config.max_lazy && end - pos > min_match) {
        const unsigned chain =
            match.length >= config.good_length ? std::max<unsigned>(config.max_chain >> 2, 1) : config.max_chain;
        const Match next = find_match(data, pos + 1, end, match.length, chain, config.nice_length);
        if (next.length <= match.length) break;
        out[count++] = data[pos++];
        match = next;
      }
    }
    out[count++] = (static_cast<u32>(match.distance) << 16) | match.length;
    pos += match.length;
    // Greedy levels skip the hash entries inside long matches for speed.
    if (!config.lazy && match.length > config.max_lazy) {
      next_insert = std::max(next_insert, pos - std::min(skipped_match_tail, pos));
    }
  }
  return count;
}

void Deflater::Impl::compress_pending(Bytes& out, bool final) {
  const std::size_t start = history;
  const std::size_t end = buf.size();
  if (start == end) {
    if (final) {
      // Nothing left but the stream still needs a last block: an empty fixed block.
      BitWriter writer{out, out.size(), bit_acc, bit_count};
      writer.reserve(8);
      writer.put_flush(1u | (1u << 1), 3);
      writer.put_flush(fixed_codes.lit[end_of_block], fixed_lit_lengths[end_of_block]);
      out.resize(writer.pos);
      bit_acc = writer.acc;
      bit_count = writer.bits;
    }
    return;
  }
  prepare_matcher(final);
  data_end = buf.data() + end;
  if (tokens.size() < end - start) tokens.resize(end - start);
  const std::size_t token_count = parse(start, end);
  emit_blocks(out, token_count, buf.data() + start, final);
}

/// Cut the tokens into blocks where the symbol statistics change and write
/// each block. `data` points at the input bytes the tokens stand for.
void Deflater::Impl::emit_blocks(Bytes& out, std::size_t token_count, const u8* data, bool final) {
  const u32* const all = tokens.data();
  std::size_t block_start = 0;
  std::size_t unit_start = 0;
  std::size_t unit_end = std::min(token_count, split_unit_tokens);
  Histogram block = count_tokens(all, unit_end);
  u64 block_estimate = estimate_block_bits(block);
  while (unit_end < token_count) {
    unit_start = unit_end;
    unit_end = std::min(token_count, unit_start + split_unit_tokens);
    const Histogram unit = count_tokens(all + unit_start, unit_end - unit_start);
    Histogram merged = block;
    merged.add(unit);
    const u64 merged_estimate = estimate_block_bits(merged);
    const u64 unit_estimate = estimate_block_bits(unit);
    if (merged_estimate <= block_estimate + unit_estimate) {
      block = merged;
      block_estimate = merged_estimate;
      continue;
    }
    const std::size_t block_bytes = block.bytes;
    emit_block(out, all + block_start, unit_start - block_start, block, data, false);
    data += block_bytes;
    block_start = unit_start;
    block = unit;
    block_estimate = unit_estimate;
  }
  emit_block(out, all + block_start, token_count - block_start, block, data, final);
}

/// Write one block in the cheapest of the three encodings.
void Deflater::Impl::emit_block(Bytes& out, const u32* block_tokens, std::size_t token_count, Histogram& histogram,
                                const u8* data, bool final) {
  histogram.lit[end_of_block] = 1;

  // Dynamic codes and the description of their lengths.
  std::array<u8, num_lit_symbols + num_dist_symbols> lens{};
  build_code_lengths(histogram.lit.data(), num_lit_symbols, max_code_bits, lens.data(), *code_workspace);
  unsigned lit_count = num_lit_symbols;
  while (lit_count > 257 && lens[lit_count - 1] == 0) --lit_count;
  std::array<u8, num_dist_symbols> dist_lens{};
  build_code_lengths(histogram.dist.data(), num_dist_symbols, max_code_bits, dist_lens.data(), *code_workspace);
  unsigned dist_count = num_dist_symbols;
  while (dist_count > 1 && dist_lens[dist_count - 1] == 0) --dist_count;
  std::array<u8, num_lit_symbols> lit_lens{};
  std::copy_n(lens.begin(), num_lit_symbols, lit_lens.begin());
  std::copy_n(dist_lens.begin(), dist_count, lens.begin() + lit_count);

  std::array<LengthCode, num_lit_symbols + num_dist_symbols> length_codes{};
  const unsigned length_code_count = run_length_code(lens.data(), lit_count + dist_count, length_codes.data());
  std::array<u32, num_precode_symbols> pre_freq{};
  for (unsigned i = 0; i < length_code_count; ++i) ++pre_freq[length_codes[i].symbol];
  std::array<u8, num_precode_symbols> pre_lens{};
  build_code_lengths(pre_freq.data(), num_precode_symbols, max_precode_bits, pre_lens.data(), *code_workspace);
  unsigned pre_count = num_precode_symbols;
  while (pre_count > 4 && pre_lens[precode_order[pre_count - 1]] == 0) --pre_count;

  u64 dynamic_bits = 3 + 14 + 3 * static_cast<u64>(pre_count) + histogram.extra_bits;
  for (unsigned i = 0; i < num_precode_symbols; ++i) {
    dynamic_bits += static_cast<u64>(pre_freq[i]) * (pre_lens[i] + precode_extra_bits(i));
  }
  u64 fixed_bits = 3 + histogram.extra_bits;
  for (unsigned i = 0; i < num_lit_symbols; ++i) {
    dynamic_bits += static_cast<u64>(histogram.lit[i]) * lit_lens[i];
    fixed_bits += static_cast<u64>(histogram.lit[i]) * fixed_lit_lengths[i];
  }
  for (unsigned i = 0; i < num_dist_symbols; ++i) {
    dynamic_bits += static_cast<u64>(histogram.dist[i]) * dist_lens[i];
    fixed_bits += static_cast<u64>(histogram.dist[i]) * 5u;
  }
  // Stored: header bits, padding to a byte boundary, LEN and NLEN, then the
  // bytes; repeated for every 65535 bytes.
  const std::size_t size = histogram.bytes;
  const u64 stored_blocks = (size + max_stored_block - 1) / max_stored_block;
  const u64 first_padding = (8 - ((bit_count + 3) & 7u)) & 7u;
  const u64 stored_bits = 3 + first_padding + 32 + (stored_blocks - 1) * (8 + 32) + 8 * static_cast<u64>(size);

  BitWriter writer{out, out.size(), bit_acc, bit_count};
  if (stored_bits <= dynamic_bits && stored_bits <= fixed_bits) {
    emit_stored(writer, data, size, final);
  } else {
    const bool use_fixed = fixed_bits <= dynamic_bits;
    writer.reserve(static_cast<std::size_t>((std::min(fixed_bits, dynamic_bits) + 7) / 8) + 16);
    std::array<u16, num_lit_symbols> lit_codes{};
    std::array<u16, num_dist_symbols> dist_codes{};
    const u8* lit_bits = lit_lens.data();
    const u8* dist_bits = dist_lens.data();
    if (use_fixed) {
      writer.put_flush((final ? 1u : 0u) | (1u << 1), 3);
      std::copy_n(fixed_codes.lit.begin(), num_lit_symbols, lit_codes.begin());
      std::copy_n(fixed_codes.dist.begin(), num_dist_symbols, dist_codes.begin());
      lit_bits = fixed_lit_lengths.data();
      dist_bits = fixed_dist_lengths.data();
    } else {
      writer.put_flush((final ? 1u : 0u) | (2u << 1), 3);
      writer.put_flush((lit_count - 257) | ((dist_count - 1) << 5) | ((pre_count - 4) << 10), 14);
      for (unsigned i = 0; i < pre_count; ++i) writer.put_flush(pre_lens[precode_order[i]], 3);
      std::array<u16, num_precode_symbols> pre_codes{};
      assign_codes(pre_lens.data(), num_precode_symbols, pre_codes.data());
      for (unsigned i = 0; i < length_code_count; ++i) {
        const LengthCode code = length_codes[i];
        writer.put(pre_codes[code.symbol], pre_lens[code.symbol]);
        writer.put(code.extra, precode_extra_bits(code.symbol));
        writer.flush();
      }
      assign_codes(lit_lens.data(), num_lit_symbols, lit_codes.data());
      assign_codes(dist_lens.data(), num_dist_symbols, dist_codes.data());
    }

    // The tokens. A match takes at most 48 bits, so flushing whenever more
    // than 15 bits are pending keeps the accumulator within 64 bits.
    u8* p = writer.cursor();
    u64 acc = writer.acc;
    unsigned bits = writer.bits;
    for (std::size_t i = 0; i < token_count; ++i) {
      if (bits > 15) {
        store64le(p, acc);
        p += bits >> 3;
        acc >>= bits & ~7u;
        bits &= 7u;
      }
      const u32 token = block_tokens[i];
      const u32 distance = token >> 16;
      if (distance == 0) {
        acc |= static_cast<u64>(lit_codes[token]) << bits;
        bits += lit_bits[token];
        continue;
      }
      const u32 length = token & 0xFFFFu;
      const unsigned ls = symbol_lookup.length[length];
      acc |= static_cast<u64>(lit_codes[257 + ls]) << bits;
      bits += lit_bits[257 + ls];
      acc |= static_cast<u64>(length - length_base[ls]) << bits;
      bits += length_extra[ls];
      const unsigned ds = distance_symbol(distance);
      acc |= static_cast<u64>(dist_codes[ds]) << bits;
      bits += dist_bits[ds];
      acc |= static_cast<u64>(distance - dist_base[ds]) << bits;
      bits += dist_extra[ds];
    }
    writer.pos += static_cast<std::size_t>(p - writer.cursor());
    writer.acc = acc;
    writer.bits = bits;
    writer.flush();
    writer.put_flush(lit_codes[end_of_block], lit_bits[end_of_block]);
  }
  out.resize(writer.pos);
  bit_acc = writer.acc;
  bit_count = writer.bits;
}

Deflater::Deflater(Format format, int level, const GzipHeader& header)
    : impl_(std::make_unique<Impl>(format, level, header)) {}
Deflater::~Deflater() = default;
Deflater::Deflater(Deflater&&) noexcept = default;
Deflater& Deflater::operator=(Deflater&&) noexcept = default;

bool Deflater::finished() const noexcept { return impl_->done; }
std::uint64_t Deflater::total_in() const noexcept { return impl_->total_in; }
std::uint64_t Deflater::total_out() const noexcept { return impl_->total_out; }

Result<void> Deflater::feed(ByteView input, Bytes& output) {
  Impl& s = *impl_;
  if (s.done) return fail(Status::InvalidArgument, "the compressed stream is already finished");
  const std::size_t before = output.size();
  if (!s.header_written) s.write_header(output);
  if (s.format == Format::Zlib) s.checksum = adler32(input, s.checksum);
  if (s.format == Format::Gzip) s.checksum = crc32(input, s.checksum);
  s.total_in += input.size();
  if (s.level == 0) {
    s.feed_stored(input, output);
  } else {
    s.feed_compressed(input, output);
  }
  s.total_out += output.size() - before;
  return {};
}

Result<void> Deflater::finish(Bytes& output) {
  Impl& s = *impl_;
  if (s.done) return fail(Status::InvalidArgument, "the compressed stream is already finished");
  const std::size_t before = output.size();
  if (!s.header_written) s.write_header(output);
  if (s.level == 0) {
    BitWriter writer{output, output.size(), s.bit_acc, s.bit_count};
    s.emit_stored(writer, s.buf.data(), s.buf.size(), true);
    output.resize(writer.pos);
    s.bit_acc = writer.acc;
    s.bit_count = writer.bits;
  } else {
    s.compress_pending(output, true);
  }
  if (s.bit_count > 0) {
    output.push_back(static_cast<std::byte>(s.bit_acc & 0xFFu));
    s.bit_acc = 0;
    s.bit_count = 0;
  }
  s.write_trailer(output);
  s.done = true;
  s.buf = {};
  s.head = {};
  s.prev = {};
  s.tokens = {};
  s.code_workspace.reset();
  s.total_out += output.size() - before;
  return {};
}

// --------------------------------------------------------------------------- one-shot functions

namespace {

Result<Bytes> compress_all(ByteView data, int level, DeflateFormat format, const GzipHeader& header = {}) {
  Deflater deflater(format, level, header);
  Bytes out;
  CVT_TRY(deflater.feed(data, out));
  CVT_TRY(deflater.finish(out));
  return out;
}

}  // namespace

Result<Bytes> deflate_raw(ByteView data, int level) { return compress_all(data, level, DeflateFormat::Raw); }

Result<Bytes> inflate_raw(ByteView data, std::uint64_t max_output, std::size_t* consumed) {
  if (consumed != nullptr) *consumed = 0;
  Inflater inflater(DeflateFormat::Raw);
  Bytes out;
  CVT_TRY_ASSIGN(const Inflater::Progress progress, inflater.feed(data, out, max_output));
  if (!progress.finished) return fail(Status::Decode, "compressed data is truncated");
  if (consumed != nullptr) *consumed = progress.consumed;
  return out;
}

Result<Bytes> zlib_compress(ByteView data, int level) { return compress_all(data, level, DeflateFormat::Zlib); }

Result<Bytes> zlib_decompress(ByteView data, std::uint64_t max_output) {
  Inflater inflater(DeflateFormat::Zlib);
  Bytes out;
  CVT_TRY_ASSIGN(const Inflater::Progress progress, inflater.feed(data, out, max_output));
  if (!progress.finished) return fail(Status::Decode, "zlib stream is truncated");
  return out;
}

Result<Bytes> gzip_compress(ByteView data, int level, const GzipHeader& header) {
  return compress_all(data, level, DeflateFormat::Gzip, header);
}

Result<Bytes> gzip_decompress(ByteView data, std::uint64_t max_output, GzipHeader* header) {
  Inflater inflater(DeflateFormat::Gzip);
  Bytes out;
  bool first = true;
  while (true) {
    if (!first) {
      // Whatever follows a complete member and is not another member is
      // ignored, as gzip does.
      if (data.size() < 2 || data[0] != std::byte{0x1F} || data[1] != std::byte{0x8B}) break;
      inflater.reset();
    }
    // The limit applies to the total across members.
    const std::uint64_t budget = max_output - std::min<std::uint64_t>(max_output, out.size());
    CVT_TRY_ASSIGN(const Inflater::Progress progress, inflater.feed(data, out, budget));
    if (!progress.finished) return fail(Status::Decode, first ? "truncated gzip stream" : "truncated gzip member");
    if (first && header != nullptr) *header = inflater.gzip_header();
    first = false;
    data = data.subspan(progress.consumed);
    if (data.empty()) break;
  }
  return out;
}

}  // namespace cvt::util
