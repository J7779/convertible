// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "support/fixtures.hpp"
#include "support/test_support.hpp"
#include "util/crc.hpp"
#include "util/deflate.hpp"
#include "util/hex.hpp"

using namespace cvt;
using namespace cvt::util;
using test::str;

namespace {

constexpr std::string_view sample = "hello hello hello hello hello\n";
Bytes from_hex(std::string_view hex) { return *hex_decode(hex); }

template <class T>
Status status_of(const Result<T>& result) {
  return result ? Status::Ok : result.error().code;
}

// --------------------------------------------------------------------------- known content

// The content of the fixtures in core/tests/data/util (see SOURCES.md there).
constexpr std::string_view lorem =
    "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et "
    "dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea "
    "commodo consequat. Duis aute irure dolor in reprehenderit in voluptate velit esse cillum dolore eu fugiat "
    "nulla pariatur. Excepteur sint occaecat cupidatat non proident, sunt in culpa qui officia deserunt mollit "
    "anim id est laborum.\n";

Bytes lorem_times(std::size_t count) {
  Bytes out;
  for (std::size_t i = 0; i < count; ++i) append(out, lorem);
  return out;
}

Bytes pattern(std::size_t size) {
  Bytes out(size);
  for (std::size_t i = 0; i < size; ++i) out[i] = static_cast<std::byte>((i % 251) ^ ((i / 251) & 15));
  return out;
}

Bytes concat(std::initializer_list<ByteView> parts) {
  Bytes out;
  for (const ByteView part : parts) append(out, part);
  return out;
}

// --------------------------------------------------------------------------- generated data

Bytes text_like(std::size_t size, std::uint32_t seed) {
  static constexpr std::array<std::string_view, 16> words = {
      "the", "quick", "brown", "fox", "jumps", "over", "lazy", "dog", "compression", "window", "literal",
      "distance", "length", "block", "stream", "table"};
  std::string out;
  out.reserve(size + 16);
  std::uint32_t state = seed * 2654435761u + 1u;
  while (out.size() < size) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    out += words[(state >> 8) % words.size()];
    out += (state >> 28) == 0 ? ".\n" : " ";
  }
  out.resize(size);
  return to_bytes(std::string_view(out));
}

// Random bytes in which every stretch repeats what stood a fixed, long
// distance earlier (up to the 32768-byte limit of the format).
Bytes long_distance_repeats(std::size_t size, std::uint32_t seed) {
  Bytes out = test::random_bytes(size, seed);
  const std::size_t distance = size > 40000 ? 32768 : size / 2;
  if (distance == 0) return out;
  for (std::size_t at = distance; at + 300 <= size; at += 700) {
    std::copy_n(out.begin() + static_cast<std::ptrdiff_t>(at - distance), 300, out.begin() + static_cast<std::ptrdiff_t>(at));
  }
  return out;
}

Bytes mixed_data(std::size_t size, std::uint32_t seed) {
  Bytes out = text_like(size / 3, seed);
  append(out, ByteView(test::random_bytes(size / 6, seed + 1)));
  out.resize(out.size() + size / 6, std::byte{0});
  append(out, ByteView(pattern(size / 6)));
  append(out, ByteView(text_like(size - out.size(), seed + 2)));
  return out;
}

// --------------------------------------------------------------------------- hand-built streams

/// Assembles a DEFLATE bit stream: plain fields least significant bit first,
/// Huffman codes most significant bit first (RFC 1951, 3.1.1).
class Bits {
 public:
  Bits& bits(std::uint32_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) push(i < 32 ? (value >> i) & 1u : 0u);  // counts above 32 pad with zeros
    return *this;
  }
  Bits& code(std::uint32_t value, unsigned length) {
    for (unsigned i = length; i-- > 0;) push((value >> i) & 1u);
    return *this;
  }
  Bits& align() {
    while (count_ % 8 != 0) push(0);
    return *this;
  }
  Bits& raw(ByteView data) {
    align();
    for (const std::byte b : data) bits(std::to_integer<std::uint32_t>(b), 8);
    return *this;
  }
  Bits& block(bool final, unsigned type) { return bits(final ? 1 : 0, 1).bits(type, 2); }
  /// A complete stored block.
  Bits& stored(bool final, ByteView data) {
    block(final, 0).align();
    bits(static_cast<std::uint32_t>(data.size()), 16).bits(static_cast<std::uint32_t>(data.size()) ^ 0xFFFFu, 16);
    return raw(data);
  }
  /// The stream so far, zero-padded to a whole byte.
  [[nodiscard]] const Bytes& bytes() const { return bytes_; }

 private:
  void push(std::uint32_t bit) {
    if (count_ % 8 == 0) bytes_.push_back(std::byte{0});
    if (bit != 0) bytes_.back() |= static_cast<std::byte>(1u << (count_ % 8));
    ++count_;
  }
  Bytes bytes_;
  std::size_t count_ = 0;
};

constexpr std::array<unsigned, 29> length_base = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                  31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<unsigned, 29> length_extra = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                   2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<unsigned, 30> distance_base = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                                    33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                                    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<unsigned, 30> distance_extra = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                     6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

unsigned length_index(unsigned length) {
  unsigned index = 28;
  while (length_base[index] > length) --index;
  return index;
}
unsigned distance_index(unsigned distance) {
  unsigned index = 29;
  while (distance_base[index] > distance) --index;
  return index;
}

/// A literal/length symbol in the fixed code (RFC 1951, 3.2.6).
Bits& fixed_symbol(Bits& out, unsigned symbol) {
  if (symbol < 144) return out.code(0x30 + symbol, 8);
  if (symbol < 256) return out.code(0x190 + (symbol - 144), 9);
  if (symbol < 280) return out.code(symbol - 256, 7);
  return out.code(0xC0 + (symbol - 280), 8);
}
Bits& fixed_text(Bits& out, std::string_view text) {
  for (const char c : text) fixed_symbol(out, static_cast<unsigned char>(c));
  return out;
}
Bits& fixed_match(Bits& out, unsigned length, unsigned distance) {
  const unsigned li = length_index(length);
  fixed_symbol(out, 257 + li).bits(length - length_base[li], length_extra[li]);
  const unsigned di = distance_index(distance);
  return out.code(di, 5).bits(distance - distance_base[di], distance_extra[di]);
}
Bits& fixed_end(Bits& out) { return fixed_symbol(out, 256); }

/// Canonical Huffman codes for a set of code lengths (RFC 1951, 3.2.2).
std::vector<std::uint32_t> canonical_codes(const std::vector<unsigned>& lengths) {
  std::array<std::uint32_t, 17> count{};
  for (const unsigned length : lengths) ++count[length];
  count[0] = 0;
  std::array<std::uint32_t, 17> next{};
  std::uint32_t code = 0;
  for (unsigned bits = 1; bits <= 15; ++bits) {
    code = (code + count[bits - 1]) << 1;
    next[bits] = code;
  }
  std::vector<std::uint32_t> codes(lengths.size(), 0);
  for (std::size_t i = 0; i < lengths.size(); ++i) {
    if (lengths[i] != 0) codes[i] = next[lengths[i]]++;
  }
  return codes;
}

constexpr std::array<unsigned, 19> code_length_order = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

/// A dynamic-Huffman block under construction. The header sends every code
/// length verbatim: the code-length alphabet gives each of the symbols 0..15
/// a four-bit code (which is then simply the length itself).
struct DynamicBlock {
  std::vector<unsigned> lit;   ///< literal/length code lengths (257..288 entries)
  std::vector<unsigned> dist;  ///< distance code lengths (1..32 entries)

  DynamicBlock() : lit(286, 0), dist(30, 0) {}

  Bits& header(Bits& out, bool final) const {
    out.block(final, 2);
    out.bits(static_cast<std::uint32_t>(lit.size() - 257), 5).bits(static_cast<std::uint32_t>(dist.size() - 1), 5);
    out.bits(19 - 4, 4);
    for (const unsigned symbol : code_length_order) out.bits(symbol < 16 ? 4 : 0, 3);
    for (const unsigned length : lit) out.code(length, 4);
    for (const unsigned length : dist) out.code(length, 4);
    return out;
  }
  Bits& symbol(Bits& out, unsigned value) const { return out.code(canonical_codes(lit)[value], lit[value]); }
  Bits& match(Bits& out, unsigned length, unsigned distance) const {
    const unsigned li = length_index(length);
    symbol(out, 257 + li).bits(length - length_base[li], length_extra[li]);
    const unsigned di = distance_index(distance);
    return out.code(canonical_codes(dist)[di], dist[di]).bits(distance - distance_base[di], distance_extra[di]);
  }
};

// --------------------------------------------------------------------------- incremental helpers

struct Fed {
  Status status = Status::Ok;
  bool finished = false;
  std::size_t consumed = 0;
  Bytes data;
};

/// Run `stream` through an Inflater in pieces of `chunk` bytes. With
/// `keep_output` false the output buffer is emptied after every call, as a
/// caller writing to a file would do.
Fed feed_in_chunks(Inflater::Format format, ByteView stream, std::size_t chunk, bool keep_output = true,
                   std::uint64_t limit = no_output_limit) {
  Fed result;
  Inflater inflater(format);
  Bytes buffer;
  std::size_t offset = 0;
  while (true) {
    const std::size_t take = std::min(chunk, stream.size() - offset);
    const std::size_t before = buffer.size();
    const auto progress = inflater.feed(stream.subspan(offset, take), buffer, limit);
    if (!progress) {
      result.status = progress.error().code;
      break;
    }
    CHECK(progress->produced == buffer.size() - before);
    result.consumed += progress->consumed;
    offset += take;
    if (!keep_output) {
      append(result.data, ByteView(buffer));
      buffer.clear();
    }
    if (progress->finished) {
      result.finished = true;
      break;
    }
    CHECK(progress->consumed == take);  // an unfinished stream always takes everything
    if (offset == stream.size()) break;
  }
  if (keep_output) result.data = std::move(buffer);
  CHECK(inflater.finished() == result.finished);
  if (result.status == Status::Ok) {
    CHECK(inflater.total_in() == result.consumed);
    CHECK(inflater.total_out() == result.data.size());
  }
  return result;
}

Bytes deflate_in_chunks(Deflater::Format format, int level, ByteView data, std::size_t chunk) {
  Deflater deflater(format, level);
  Bytes out;
  for (std::size_t offset = 0; offset < data.size(); offset += chunk) {
    REQUIRE(deflater.feed(data.subspan(offset, std::min(chunk, data.size() - offset)), out));
  }
  REQUIRE(deflater.finish(out));
  CHECK(deflater.finished());
  CHECK(deflater.total_in() == data.size());
  CHECK(deflater.total_out() == out.size());
  return out;
}

}  // namespace

// =========================================================================== streams from other implementations

TEST_CASE("deflate: streams produced by an independent implementation decode correctly") {
  // Produced with zlib level 9: zlib, raw deflate and gzip wrappings of `sample`.
  CHECK(str(*zlib_decompress(from_hex("78dacb48cdc9c957c8c04e720100adff0aef"))) == sample);
  std::size_t consumed = 0;
  CHECK(str(*inflate_raw(from_hex("cb48cdc9c957c8c04e720100"), no_output_limit, &consumed)) == sample);
  CHECK(consumed == 12);
  CHECK(str(*gzip_decompress(from_hex("1f8b08000000000002ffcb48cdc9c957c8c04e720100680231661e000000"))) == sample);
  // A hand-assembled gzip member holding one stored block.
  CHECK(str(*gzip_decompress(from_hex("1f8b0800000000000003" "010500faff" "68656c6c6f" "86a61036" "05000000"))) == "hello");
  // gzip of nothing.
  CHECK(gzip_decompress(from_hex("1f8b08000000000000030300" "00000000" "00000000"))->empty());
}

TEST_CASE("deflate: fixtures written by zlib decode to their known content") {
  struct Case {
    std::string_view name;
    Bytes expected;
    unsigned first_block;  // BFINAL | BTYPE << 1 of the first block
  };
  const std::vector<Case> cases = {
      {"util/pattern_4000_fixed.deflate", pattern(4000), 1u | (1u << 1)},
      {"util/lorem_x8_dynamic.deflate", lorem_times(8), 1u | (2u << 1)},
      {"util/lorem_pattern_lorem_flushed.deflate", concat({as_bytes(lorem), pattern(1000), as_bytes(lorem)}), 2u << 1},
      {"util/lorem_pattern_stored.deflate", concat({as_bytes(lorem), pattern(300)}), 0u},
  };
  for (const Case& c : cases) {
    const Bytes stream = test::fixture(c.name);
    REQUIRE_MESSAGE(!stream.empty(), c.name);
    CHECK_MESSAGE((std::to_integer<unsigned>(stream[0]) & 7u) == c.first_block, c.name);
    std::size_t consumed = 0;
    const auto data = inflate_raw(stream, no_output_limit, &consumed);
    REQUIRE_MESSAGE(data.has_value(), c.name);
    CHECK_MESSAGE(*data == c.expected, c.name);
    CHECK_MESSAGE(consumed == stream.size(), c.name);
    // The same, one byte at a time, and with an exact output limit.
    const Fed fed = feed_in_chunks(Inflater::Format::Raw, stream, 1);
    CHECK_MESSAGE(fed.finished, c.name);
    CHECK_MESSAGE(fed.data == c.expected, c.name);
    CHECK_MESSAGE(inflate_raw(stream, c.expected.size()).has_value(), c.name);
    CHECK_MESSAGE(status_of(inflate_raw(stream, c.expected.size() - 1)) == Status::Limit, c.name);
  }

  const Bytes zlib_stream = test::fixture("util/lorem_x4_level9.zlib");
  REQUIRE(zlib_stream.size() > 2);
  CHECK(zlib_stream[0] == std::byte{0x78});
  CHECK(zlib_stream[1] == std::byte{0xDA});
  const auto text = zlib_decompress(zlib_stream);
  REQUIRE(text);
  CHECK(*text == lorem_times(4));
  const Fed fed = feed_in_chunks(Inflater::Format::Zlib, zlib_stream, 1);
  CHECK(fed.finished);
  CHECK(fed.consumed == zlib_stream.size());
  CHECK(fed.data == lorem_times(4));
}

TEST_CASE("deflate: fixtures written by gzip decode to their known content") {
  const Bytes named = test::fixture("util/lorem_x2_named.gz");
  REQUIRE(!named.empty());
  GzipHeader header;
  const auto text = gzip_decompress(named, no_output_limit, &header);
  REQUIRE(text);
  CHECK(*text == lorem_times(2));
  CHECK(header.filename == "lorem.txt");
  CHECK(header.mtime == 981173106u);  // 2001-02-03T04:05:06Z

  Inflater inflater(Inflater::Format::Gzip);
  Bytes out;
  for (const std::byte b : named) REQUIRE(inflater.feed(ByteView(&b, 1), out));
  CHECK(inflater.finished());
  CHECK(out == lorem_times(2));
  CHECK(inflater.gzip_header().filename == "lorem.txt");
  CHECK(inflater.gzip_header().mtime == 981173106u);

  const Bytes two = test::fixture("util/two_members.gz");
  REQUIRE(!two.empty());
  GzipHeader unnamed{.filename = "overwritten", .mtime = 7};
  CHECK(str(*gzip_decompress(two, no_output_limit, &unnamed)) == "first member, second member\n");
  CHECK(unnamed.filename.empty());
  CHECK(unnamed.mtime == 0u);
  // The Inflater stops after one member and reports what is left.
  Inflater first(Inflater::Format::Gzip);
  Bytes member;
  const auto progress = first.feed(two, member);
  REQUIRE(progress);
  CHECK(progress->finished);
  CHECK(str(member) == "first member, ");
  CHECK(progress->consumed == 34);
  CHECK(std::to_integer<int>(two[progress->consumed]) == 0x1F);
  // reset() makes it ready for the next one.
  first.reset();
  member.clear();
  const auto second = first.feed(ByteView(two).subspan(progress->consumed), member);
  REQUIRE(second);
  CHECK(second->finished);
  CHECK(str(member) == "second member\n");
  CHECK(progress->consumed + second->consumed == two.size());
}

// =========================================================================== block types, built by hand

TEST_CASE("deflate: hand-built stored blocks") {
  // The shortest valid stream: one empty stored block.
  CHECK(inflate_raw(from_hex("010000ffff"))->empty());

  Bits stream;
  stream.stored(false, {}).stored(false, as_bytes("abc")).stored(false, {}).stored(true, as_bytes("defgh"));
  std::size_t consumed = 0;
  CHECK(str(*inflate_raw(stream.bytes(), no_output_limit, &consumed)) == "abcdefgh");
  CHECK(consumed == stream.bytes().size());

  // A stored block after a block that ends inside a byte: the bits up to the
  // next byte boundary are skipped.
  Bits mixed;
  fixed_end(fixed_text(mixed.block(false, 1), "xy"));
  mixed.stored(true, as_bytes("z"));
  CHECK(str(*inflate_raw(mixed.bytes())) == "xyz");

  // The largest stored block.
  const Bytes big = test::random_bytes(65535, 5);
  Bits full;
  full.stored(true, big);
  CHECK(*inflate_raw(full.bytes()) == big);
  const Fed fed = feed_in_chunks(Inflater::Format::Raw, full.bytes(), 1);
  CHECK(fed.finished);
  CHECK(fed.data == big);
}

TEST_CASE("deflate: hand-built fixed-Huffman blocks") {
  SUBCASE("literals from all four code groups") {
    Bits stream;
    stream.block(true, 1);
    for (const unsigned literal : {0u, 65u, 143u, 144u, 200u, 255u}) fixed_symbol(stream, literal);
    fixed_end(stream);
    const auto data = inflate_raw(stream.bytes());
    REQUIRE(data);
    CHECK(*data == from_hex("00418f90c8ff"));
  }
  SUBCASE("an empty block") {
    Bits stream;
    fixed_end(stream.block(true, 1));
    CHECK(stream.bytes() == from_hex("0300"));
    CHECK(inflate_raw(stream.bytes())->empty());
  }
  SUBCASE("overlapping copies") {
    // Distance 1 repeats one byte; length 258 is the longest match.
    Bits run;
    fixed_end(fixed_match(fixed_text(run.block(true, 1), "a"), 258, 1));
    CHECK(str(*inflate_raw(run.bytes())) == std::string(259, 'a'));

    // Short periods of every size, with lengths that are not multiples of them.
    for (unsigned period = 1; period <= 9; ++period) {
      for (const unsigned length : {3u, 4u, 7u, 8u, 9u, 15u, 16u, 17u, 100u, 257u, 258u}) {
        const std::string seed = std::string("abcdefghi").substr(0, period);
        Bits stream;
        fixed_end(fixed_text(fixed_match(fixed_text(stream.block(true, 1), seed), length, period), "!"));
        std::string expected = seed;
        for (unsigned i = 0; i < length; ++i) expected.push_back(expected[expected.size() - period]);
        expected.push_back('!');
        // Padding makes the fast decoding path handle the match too.
        Bytes padded = stream.bytes();
        padded.resize(padded.size() + 32, std::byte{0});
        CHECK_MESSAGE(str(*inflate_raw(stream.bytes())) == expected, "period ", period, " length ", length);
        CHECK_MESSAGE(str(*inflate_raw(padded)) == expected, "period ", period, " length ", length);
      }
    }
  }
  SUBCASE("every match length and the distance code boundaries") {
    const Bytes history = pattern(32768);
    Bits prefix;
    prefix.stored(false, history);
    for (unsigned length = 3; length <= 258; ++length) {
      const unsigned distance = distance_base[(length * 7) % 30] + (length % 2);
      Bits stream = prefix;
      fixed_end(fixed_match(stream.block(true, 1), length, distance));
      Bytes expected = history;
      for (unsigned i = 0; i < length; ++i) expected.push_back(expected[expected.size() - distance]);
      const auto data = inflate_raw(stream.bytes());
      REQUIRE_MESSAGE(data.has_value(), "length ", length, " distance ", distance);
      CHECK_MESSAGE(*data == expected, "length ", length, " distance ", distance);
    }
  }
  SUBCASE("length 258 at distance 32768, the far end of the window") {
    const Bytes history = pattern(32768);
    Bits stream;
    stream.stored(false, history);
    fixed_end(fixed_match(stream.block(true, 1), 258, 32768));
    Bytes expected = history;
    expected.insert(expected.end(), history.begin(), history.begin() + 258);
    CHECK(*inflate_raw(stream.bytes()) == expected);
    for (const std::size_t chunk : {std::size_t{1}, std::size_t{100}, std::size_t{32768}}) {
      for (const bool keep : {true, false}) {
        const Fed fed = feed_in_chunks(Inflater::Format::Raw, stream.bytes(), chunk, keep);
        CHECK(fed.finished);
        CHECK(fed.data == expected);
      }
    }
    // One byte less history and the same match points before the data.
    Bits early;
    early.stored(false, ByteView(history).first(32767));
    fixed_end(fixed_match(early.block(true, 1), 258, 32768));
    CHECK(status_of(inflate_raw(early.bytes())) == Status::Decode);
    CHECK(feed_in_chunks(Inflater::Format::Raw, early.bytes(), 1, false).status == Status::Decode);
  }
}

TEST_CASE("deflate: hand-built dynamic-Huffman blocks") {
  SUBCASE("literals, matches and long codes") {
    // A complete code with lengths 1, 2, 3, ..., 14, 15, 15 (longer than the
    // root lookup table, so subtables are exercised).
    DynamicBlock block;
    const std::string alphabet = "abcdefghijklmn";
    for (std::size_t i = 0; i < alphabet.size(); ++i) {
      block.lit[static_cast<unsigned char>(alphabet[i])] = static_cast<unsigned>(i + 1);
    }
    block.lit[256] = 15;
    block.lit[257 + length_index(10)] = 15;
    // Distance codes of 1..14 bits for the symbols 0..13 and two 15-bit codes.
    for (unsigned i = 0; i < 14; ++i) block.dist[i] = i + 1;
    block.dist[14] = 15;
    block.dist[16] = 15;
    Bits stream;
    block.header(stream, true);
    std::string expected;
    for (int round = 0; round < 40; ++round) {
      for (const char c : alphabet) {
        block.symbol(stream, static_cast<unsigned char>(c));
        expected.push_back(c);
      }
      const unsigned distance = distance_base[static_cast<std::size_t>(round) % 5];
      block.match(stream, 10, distance);
      for (unsigned i = 0; i < 10; ++i) expected.push_back(expected[expected.size() - distance]);
    }
    block.match(stream, 10, distance_base[14] + 3);   // a 15-bit distance code
    for (unsigned i = 0; i < 10; ++i) expected.push_back(expected[expected.size() - (distance_base[14] + 3)]);
    block.match(stream, 10, distance_base[16]);       // the other one
    for (unsigned i = 0; i < 10; ++i) expected.push_back(expected[expected.size() - distance_base[16]]);
    block.symbol(stream, 256);
    const auto data = inflate_raw(stream.bytes());
    REQUIRE(data);
    CHECK(str(*data) == expected);
    for (const std::size_t chunk : {std::size_t{1}, std::size_t{3}, std::size_t{17}}) {
      const Fed fed = feed_in_chunks(Inflater::Format::Raw, stream.bytes(), chunk, chunk != 3);
      CHECK(fed.finished);
      CHECK(str(fed.data) == expected);
    }
  }
  SUBCASE("a block with literals only needs no distance code") {
    DynamicBlock block;
    block.lit['x'] = 1;
    block.lit[256] = 1;
    block.dist.assign(1, 0);
    Bits stream;
    block.header(stream, true);
    for (int i = 0; i < 5; ++i) block.symbol(stream, 'x');
    block.symbol(stream, 256);
    CHECK(str(*inflate_raw(stream.bytes())) == "xxxxx");
  }
  SUBCASE("a single distance code of one bit is accepted, its unused twin is not") {
    DynamicBlock block;
    block.lit['x'] = 2;
    block.lit[256] = 2;
    block.lit[257] = 1;  // length 3
    block.dist.assign(4, 0);
    block.dist[2] = 1;   // distance 3, code "0"
    Bits stream;
    block.header(stream, true);
    for (int i = 0; i < 3; ++i) block.symbol(stream, 'x');
    block.match(stream, 3, 3);
    block.symbol(stream, 256);
    CHECK(str(*inflate_raw(stream.bytes())) == "xxxxxx");

    Bits bad;
    block.header(bad, true);
    for (int i = 0; i < 3; ++i) block.symbol(bad, 'x');
    block.symbol(bad, 257).code(1, 1);  // the distance code "1" does not exist
    block.symbol(bad, 256);
    CHECK(status_of(inflate_raw(bad.bytes())) == Status::Decode);
  }
  SUBCASE("a single literal/length code (end of block only) is accepted") {
    DynamicBlock block;
    block.lit[256] = 1;
    block.dist.assign(1, 0);
    Bits stream;
    block.header(stream, true);
    block.symbol(stream, 256);
    CHECK(inflate_raw(stream.bytes())->empty());
  }
  SUBCASE("code lengths sent with the repeat codes 16, 17 and 18") {
    // Literal/length code: 'a'..'g' and end-of-block, three bits each.
    // Code-length alphabet: 3, 16 and 18 get two bits, 0 and 17 three.
    const std::vector<unsigned> pre = [] {
      std::vector<unsigned> lengths(19, 0);
      lengths[18] = 2;
      lengths[3] = 2;
      lengths[16] = 2;
      lengths[17] = 3;
      lengths[0] = 3;
      return lengths;
    }();
    const std::vector<std::uint32_t> pre_code = canonical_codes(pre);
    Bits stream;
    stream.block(true, 2).bits(257 - 257, 5).bits(1 - 1, 5);
    // Lengths are stored in the fixed permutation; the last non-zero one here is symbol 3 (position 13).
    stream.bits(14 - 4, 4);
    for (unsigned i = 0; i < 14; ++i) stream.bits(pre[code_length_order[i]], 3);
    const auto send = [&](unsigned symbol) -> Bits& { return stream.code(pre_code[symbol], pre[symbol]); };
    send(18).bits(97 - 11, 7);        // symbols 0..96: zero
    send(3);                          // 'a'
    send(16).bits(3, 2);              // 'b'..'g': repeat the 3 six times
    send(18).bits(138 - 11, 7);       // 104..241: zero
    send(18).bits(11 - 11, 7);        // 242..252: zero
    send(17).bits(3 - 3, 3);          // 253..255: zero
    send(3);                          // 256
    send(0);                          // the one (unused) distance code
    DynamicBlock block;
    block.lit.assign(257, 0);
    for (unsigned c = 'a'; c <= 'g'; ++c) block.lit[c] = 3;
    block.lit[256] = 3;
    for (const char c : std::string_view("gfedcbaabcdefg")) block.symbol(stream, static_cast<unsigned char>(c));
    block.symbol(stream, 256);
    CHECK(str(*inflate_raw(stream.bytes())) == "gfedcbaabcdefg");
    const Fed fed = feed_in_chunks(Inflater::Format::Raw, stream.bytes(), 1);
    CHECK(fed.finished);
    CHECK(str(fed.data) == "gfedcbaabcdefg");
  }
}

// =========================================================================== malformed streams

TEST_CASE("deflate: malformed block headers are rejected") {
  // Block type 3 does not exist.
  CHECK(status_of(inflate_raw(from_hex("07"))) == Status::Decode);
  CHECK(status_of(inflate_raw(from_hex("0600000000"))) == Status::Decode);
  // Stored block whose length and its complement disagree.
  CHECK(status_of(inflate_raw(from_hex("010500faff68656c6c6f"))) == Status::Ok);
  CHECK(status_of(inflate_raw(from_hex("010500fafe68656c6c6f"))) == Status::Decode);
  CHECK(status_of(inflate_raw(from_hex("010500ffff68656c6c6f"))) == Status::Decode);
  CHECK(status_of(inflate_raw(from_hex("0100000000"))) == Status::Decode);
  // Stored block shorter than it claims.
  CHECK(status_of(inflate_raw(from_hex("010500faff68656c6c"))) == Status::Decode);
  // Nothing at all, and a stream that stops before its last block.
  CHECK(status_of(inflate_raw({})) == Status::Decode);
  CHECK(status_of(inflate_raw(from_hex("000000ffff"))) == Status::Decode);
}

TEST_CASE("deflate: invalid Huffman code descriptions are rejected") {
  const auto decode = [](const DynamicBlock& block, bool with_end = true) {
    Bits stream;
    block.header(stream, true);
    if (with_end && block.lit.size() > 256 && block.lit[256] != 0) block.symbol(stream, 256);
    stream.bits(0, 32);
    return status_of(inflate_raw(stream.bytes()));
  };
  DynamicBlock good;
  good.lit['a'] = 1;
  good.lit[256] = 1;
  good.dist.assign(1, 0);
  REQUIRE(decode(good) == Status::Ok);

  SUBCASE("over-subscribed literal/length code") {
    DynamicBlock block = good;
    block.lit['b'] = 1;  // three one-bit codes
    CHECK(decode(block) == Status::Decode);
    block = good;
    block.lit['b'] = 2;
    block.lit['c'] = 2;  // 1/2 + 1/2 + 1/4 + 1/4
    CHECK(decode(block) == Status::Decode);
  }
  SUBCASE("incomplete literal/length code") {
    DynamicBlock block = good;
    block.lit['a'] = 2;  // 1/4 + 1/2
    CHECK(decode(block) == Status::Decode);
    block.lit['a'] = 2;
    block.lit[256] = 2;  // two two-bit codes
    CHECK(decode(block) == Status::Decode);
    block.lit['a'] = 0;
    block.lit[256] = 2;  // a single code, but not of one bit
    CHECK(decode(block) == Status::Decode);
    block.lit['a'] = 15;
    block.lit[256] = 15;
    CHECK(decode(block) == Status::Decode);
  }
  SUBCASE("no end-of-block code") {
    DynamicBlock block = good;
    block.lit[256] = 0;
    block.lit['b'] = 1;
    CHECK(decode(block, false) == Status::Decode);
    block.lit.assign(257, 0);  // no codes at all
    CHECK(decode(block, false) == Status::Decode);
  }
  SUBCASE("over-subscribed distance code") {
    DynamicBlock block = good;
    block.dist.assign(3, 1);
    CHECK(decode(block) == Status::Decode);
  }
  SUBCASE("incomplete distance code") {
    DynamicBlock block = good;
    block.dist.assign(4, 0);
    block.dist[0] = 1;
    CHECK(decode(block) == Status::Ok);  // the one exception: a single one-bit code
    block.dist[0] = 2;                   // a single code of two bits
    CHECK(decode(block) == Status::Decode);
    block.dist[0] = 1;
    block.dist[1] = 2;                   // 1/2 + 1/4
    CHECK(decode(block) == Status::Decode);
    block.dist[2] = 2;                   // complete again
    CHECK(decode(block) == Status::Ok);
  }
  SUBCASE("more length or distance codes than the format has") {
    DynamicBlock block = good;
    block.lit.resize(286, 0);
    block.dist.assign(30, 0);
    CHECK(decode(block) == Status::Ok);
    block.lit.resize(287, 0);
    CHECK(decode(block) == Status::Decode);
    block.lit.resize(288, 0);
    CHECK(decode(block) == Status::Decode);
    block.lit.resize(286, 0);
    block.dist.assign(31, 0);
    CHECK(decode(block) == Status::Decode);
    block.dist.assign(32, 0);
    CHECK(decode(block) == Status::Decode);
  }
  SUBCASE("invalid code-length code") {
    // HLIT = 257, HDIST = 1, four code-length code lengths (for 16, 17, 18, 0).
    const auto header = [](std::array<unsigned, 4> lengths) {
      Bits stream;
      stream.block(true, 2).bits(0, 5).bits(0, 5).bits(0, 4);
      for (const unsigned length : lengths) stream.bits(length, 3);
      stream.bits(0, 400);
      return status_of(inflate_raw(stream.bytes()));
    };
    CHECK(header({1, 1, 1, 0}) == Status::Decode);  // over-subscribed
    CHECK(header({0, 0, 0, 1}) == Status::Decode);  // incomplete: a single code
    CHECK(header({0, 0, 2, 2}) == Status::Decode);  // incomplete
    CHECK(header({0, 0, 0, 0}) == Status::Decode);  // empty
  }
  SUBCASE("repeat codes without a previous length or past the end") {
    // Code-length alphabet: 16 -> one bit ("0"), 1 -> two bits ("10"), 18 -> two bits ("11").
    const auto header = [](unsigned hlit) -> Bits {
      Bits stream;
      stream.block(true, 2).bits(hlit - 257, 5).bits(0, 5).bits(18 - 4, 4);
      for (std::size_t i = 0; i < 18; ++i) {
        const unsigned symbol = code_length_order[i];
        stream.bits(symbol == 16 ? 1 : symbol == 18 || symbol == 1 ? 2 : 0, 3);
      }
      return stream;
    };
    Bits first = header(257);
    first.code(0, 1).bits(0, 2).bits(0, 64);  // "repeat the previous length" as the very first symbol
    CHECK(status_of(inflate_raw(first.bytes())) == Status::Decode);

    Bits past = header(257);
    past.code(3, 2).bits(138 - 11, 7);  // 138 zeros
    past.code(3, 2).bits(138 - 11, 7);  // 138 more: 276 > 257 + 1
    past.bits(0, 64);
    CHECK(status_of(inflate_raw(past.bytes())) == Status::Decode);

    Bits repeat_past = header(257);
    repeat_past.code(3, 2).bits(138 - 11, 7);  // 138 zeros
    repeat_past.code(3, 2).bits(115 - 11, 7);  // 253 zeros
    repeat_past.code(2, 2);                    // symbol 253: length 1
    repeat_past.code(0, 1).bits(3, 2);         // repeat it six times: 260 > 258
    repeat_past.bits(0, 64);
    CHECK(status_of(inflate_raw(repeat_past.bytes())) == Status::Decode);
  }
}

TEST_CASE("deflate: invalid symbols and distances are rejected") {
  const auto status = [](const Bits& stream) {
    // Once as is (careful decoding path) and once padded (fast path).
    Bytes padded = stream.bytes();
    padded.resize(padded.size() + 64, std::byte{0});
    const Status plain = status_of(inflate_raw(stream.bytes()));
    CHECK(status_of(inflate_raw(padded)) == plain);
    CHECK(feed_in_chunks(Inflater::Format::Raw, stream.bytes(), 1).status == plain);
    return plain;
  };
  SUBCASE("length symbols 286 and 287 of the fixed code") {
    for (const unsigned symbol : {286u, 287u}) {
      Bits stream;
      fixed_end(fixed_symbol(fixed_text(stream.block(true, 1), "abc"), symbol).code(0, 5));
      CHECK(status(stream) == Status::Decode);
    }
    Bits fine;
    fixed_end(fixed_symbol(fixed_text(fine.block(true, 1), "abc"), 285).code(0, 5));
    CHECK(status(fine) == Status::Ok);
  }
  SUBCASE("distance symbols 30 and 31 of the fixed code") {
    for (const unsigned symbol : {30u, 31u}) {
      Bits stream;
      fixed_end(fixed_symbol(fixed_text(stream.block(true, 1), "abc"), 257).code(symbol, 5));
      CHECK(status(stream) == Status::Decode);
    }
  }
  SUBCASE("a distance that reaches before the start of the data") {
    Bits first;
    fixed_end(fixed_match(first.block(true, 1), 3, 1));  // nothing to copy from at all
    CHECK(status(first) == Status::Decode);
    for (const unsigned distance : {4u, 5u, 100u, 32768u}) {
      Bits stream;
      fixed_end(fixed_match(fixed_text(stream.block(true, 1), "abc"), 3, distance));
      CHECK(status(stream) == Status::Decode);
    }
    Bits fine;
    fixed_end(fixed_match(fixed_text(fine.block(true, 1), "abc"), 3, 3));
    CHECK(status(fine) == Status::Ok);
    // Data before the start of the stream is not history, even when the
    // output buffer was not empty.
    Bits stream;
    fixed_end(fixed_match(fixed_text(stream.block(true, 1), "abc"), 3, 4));
    Bytes out = to_bytes("previous content of the buffer");
    Inflater inflater;
    CHECK(status_of(inflater.feed(stream.bytes(), out)) == Status::Decode);
  }
  SUBCASE("a match in a block without distance codes") {
    DynamicBlock block;
    block.lit['x'] = 2;
    block.lit[256] = 2;
    block.lit[257] = 1;
    block.dist.assign(1, 0);
    Bits stream;
    block.header(stream, true);
    block.symbol(stream, 'x');
    block.symbol(stream, 257).bits(0, 1);
    block.symbol(stream, 256);
    CHECK(status(stream) == Status::Decode);
  }
}

TEST_CASE("deflate: truncated streams are rejected at every length") {
  const Bytes data = mixed_data(2000, 3);
  for (const int level : {0, 1, 6}) {
    const Bytes raw = *deflate_raw(data, level);
    const Bytes zlib_stream = *zlib_compress(data, level);
    const Bytes gzip_stream = *gzip_compress(data, level, {.filename = "name.bin", .mtime = 5});
    const std::size_t step = level == 0 ? 7 : level == 1 ? 1 : 3;
    for (std::size_t length = 0; length < raw.size(); length += step) {
      CHECK(status_of(inflate_raw(ByteView(raw).first(length))) == Status::Decode);
    }
    for (std::size_t length = 0; length < zlib_stream.size(); length += step) {
      CHECK(status_of(zlib_decompress(ByteView(zlib_stream).first(length))) == Status::Decode);
    }
    for (std::size_t length = 0; length < gzip_stream.size(); length += step) {
      CHECK(status_of(gzip_decompress(ByteView(gzip_stream).first(length))) == Status::Decode);
    }
    // An unfinished Inflater is not an error by itself: more input may follow.
    Inflater inflater(Inflater::Format::Zlib);
    Bytes out;
    const auto progress = inflater.feed(ByteView(zlib_stream).first(zlib_stream.size() - 1), out);
    REQUIRE(progress);
    CHECK_FALSE(progress->finished);
    CHECK(progress->consumed == zlib_stream.size() - 1);
    const auto rest = inflater.feed(ByteView(zlib_stream).last(1), out);
    REQUIRE(rest);
    CHECK(rest->finished);
    CHECK(out == data);
  }
}

// =========================================================================== wrappers

TEST_CASE("deflate: zlib header and trailer checks") {
  const Bytes stream = *zlib_compress(as_bytes(sample));
  CHECK(str(*zlib_decompress(stream)) == sample);
  // The header announces the level class: 78 01 / 78 5E / 78 9C / 78 DA.
  CHECK(hex_encode(ByteView(*zlib_compress(as_bytes(sample), 0)).first(2)) == "7801");
  CHECK(hex_encode(ByteView(*zlib_compress(as_bytes(sample), 1)).first(2)) == "7801");
  CHECK(hex_encode(ByteView(*zlib_compress(as_bytes(sample), 5)).first(2)) == "785e");
  CHECK(hex_encode(ByteView(*zlib_compress(as_bytes(sample), 6)).first(2)) == "789c");
  CHECK(hex_encode(ByteView(*zlib_compress(as_bytes(sample), 9)).first(2)) == "78da");

  Bytes dictionary = stream;
  dictionary[1] = std::byte{0x20};  // 78 20: valid check bits, FDICT set
  CHECK(status_of(zlib_decompress(dictionary)) == Status::Decode);
  Bytes bad_check = stream;
  bad_check[1] ^= std::byte{0x01};
  CHECK(status_of(zlib_decompress(bad_check)) == Status::Decode);
  Bytes bad_method = stream;
  bad_method[0] = std::byte{0x79};  // CM = 9
  bad_method[1] = std::byte{0x18};  // 0x7918 is a multiple of 31
  CHECK(status_of(zlib_decompress(bad_method)) == Status::Decode);
  Bytes bad_window = stream;
  bad_window[0] = std::byte{0x88};  // 64 KiB window
  bad_window[1] = std::byte{0x1C};  // 0x881C is a multiple of 31
  CHECK(status_of(zlib_decompress(bad_window)) == Status::Decode);
  // Smaller windows are fine: the data cannot reach further back than declared.
  Bytes small_window = stream;
  small_window[0] = std::byte{0x08};
  small_window[1] = std::byte{0x1D};  // 0x081D is a multiple of 31
  CHECK(str(*zlib_decompress(small_window)) == sample);

  for (std::size_t i = stream.size() - 4; i < stream.size(); ++i) {
    Bytes bad_sum = stream;
    bad_sum[i] ^= std::byte{0x80};
    CHECK(status_of(zlib_decompress(bad_sum)) == Status::Decode);
  }
  // Bytes after the stream are not part of it.
  Bytes trailing = stream;
  append(trailing, "and then some");
  CHECK(str(*zlib_decompress(trailing)) == sample);
  const Fed fed = feed_in_chunks(Inflater::Format::Zlib, trailing, 5);
  CHECK(fed.finished);
  CHECK(fed.consumed == stream.size());
}

TEST_CASE("deflate: gzip header fields and concatenated members") {
  const auto gz = gzip_compress(as_bytes("payload"), 6, {.filename = "notes.txt", .mtime = 1577934246});
  REQUIRE(gz);
  GzipHeader header;
  CHECK(str(*gzip_decompress(*gz, no_output_limit, &header)) == "payload");
  CHECK(header.filename == "notes.txt");
  CHECK(header.mtime == 1577934246u);

  Bytes two = *gzip_compress(as_bytes("first,"));
  append(two, ByteView(*gzip_compress(as_bytes("second"))));
  CHECK(str(*gzip_decompress(two)) == "first,second");
  // Trailing garbage after a complete member is ignored.
  Bytes padded = *gzip_compress(as_bytes("data"));
  append(padded, "\0\0\0\0garbage");
  CHECK(str(*gzip_decompress(padded)) == "data");
  Bytes half_magic = *gzip_compress(as_bytes("data"));
  half_magic.push_back(std::byte{0x1F});
  CHECK(str(*gzip_decompress(half_magic)) == "data");
  // Something that starts like another member must be one.
  Bytes broken_second = *gzip_compress(as_bytes("data"));
  append(broken_second, ByteView(from_hex("1f8b0800")));
  CHECK(status_of(gzip_decompress(broken_second)) == Status::Decode);
  // A name with an embedded NUL cannot be stored and is left out.
  const Bytes unnamed = *gzip_compress(as_bytes("x"), 6, {.filename = std::string("a\0b", 3), .mtime = 0});
  CHECK(std::to_integer<int>(unnamed[3]) == 0);
  // The header is otherwise fixed: no time, "unknown" operating system.
  CHECK(hex_encode(ByteView(*gzip_compress(as_bytes("x"), 6)).first(10)) == "1f8b08000000000000ff");
  CHECK(hex_encode(ByteView(*gzip_compress(as_bytes("x"), 9)).first(10)) == "1f8b08000000000002ff");
  CHECK(hex_encode(ByteView(*gzip_compress(as_bytes("x"), 1)).first(10)) == "1f8b08000000000004ff");
}

TEST_CASE("deflate: gzip optional header fields are parsed and checked") {
  const Bytes body = *deflate_raw(as_bytes(sample));
  const auto member = [&](unsigned flags, bool good_header_crc = true) {
    util::ByteWriter out;
    out.u8(0x1F).u8(0x8B).u8(8).u8(static_cast<std::uint8_t>(flags)).u32le(123456789).u8(0).u8(3);
    if (flags & 0x04) out.u16le(6).text("ab").u16le(2).text("xy");  // FEXTRA: one subfield
    if (flags & 0x08) out.text("file.name").u8(0);                  // FNAME
    if (flags & 0x10) out.text("a comment").u8(0);                  // FCOMMENT
    if (flags & 0x02) {                                             // FHCRC: low 16 bits of the header CRC-32
      const std::uint32_t crc = crc32(out.view());
      out.u16le(static_cast<std::uint16_t>((crc & 0xFFFFu) ^ (good_header_crc ? 0u : 1u)));
    }
    out.bytes(ByteView(body));
    out.u32le(crc32(as_bytes(sample))).u32le(static_cast<std::uint32_t>(sample.size()));
    return out.take();
  };
  for (unsigned flags = 0; flags < 32; ++flags) {
    if (flags & 0x01) continue;  // FTEXT is only a hint; covered below
    const Bytes stream = member(flags);
    GzipHeader header;
    const auto data = gzip_decompress(stream, no_output_limit, &header);
    REQUIRE_MESSAGE(data.has_value(), "flags ", flags);
    CHECK(str(*data) == sample);
    CHECK(header.filename == ((flags & 0x08) ? "file.name" : ""));
    CHECK(header.mtime == 123456789u);
    const Fed fed = feed_in_chunks(Inflater::Format::Gzip, stream, 1);
    CHECK(fed.finished);
    CHECK(fed.consumed == stream.size());
    CHECK(str(fed.data) == sample);
    for (std::size_t length = 0; length < stream.size(); ++length) {
      CHECK(status_of(gzip_decompress(ByteView(stream).first(length))) == Status::Decode);
    }
  }
  CHECK(str(*gzip_decompress(member(0x01 | 0x08))) == sample);
  CHECK(status_of(gzip_decompress(member(0x02, false))) == Status::Decode);
  CHECK(status_of(gzip_decompress(member(0x1E, false))) == Status::Decode);
  for (const unsigned reserved : {0x20u, 0x40u, 0x80u}) {
    CHECK(status_of(gzip_decompress(member(reserved))) == Status::Decode);
  }
  // Wrong magic, unknown method.
  CHECK(status_of(gzip_decompress(as_bytes("not gzip at all"))) == Status::Decode);
  CHECK(status_of(gzip_decompress(from_hex("1f8b0900000000000003"))) == Status::Decode);
  CHECK(status_of(gzip_decompress({})) == Status::Decode);
}

// =========================================================================== limits

TEST_CASE("deflate: decompression bombs hit the output limit") {
  const Bytes zeros(1000000, std::byte{0});
  const Bytes raw = *deflate_raw(zeros, 9);
  CHECK(inflate_raw(raw, 999999).error().code == Status::Limit);
  CHECK(inflate_raw(raw, 1000000).has_value());
  CHECK(inflate_raw(raw, 0).error().code == Status::Limit);
  CHECK(zlib_decompress(*zlib_compress(zeros), 4096).error().code == Status::Limit);
  CHECK(gzip_decompress(*gzip_compress(zeros), 4096).error().code == Status::Limit);
  // The limit applies to the total across gzip members.
  Bytes two = *gzip_compress(as_bytes("12345"));
  append(two, ByteView(*gzip_compress(as_bytes("67890"))));
  CHECK(gzip_decompress(two, 9).error().code == Status::Limit);
  CHECK(gzip_decompress(two, 10).has_value());
  CHECK(inflate_raw(*deflate_raw({}), 0)->empty());
}

TEST_CASE("deflate: a stream expanding to a gibibyte fails at the limit without allocating for it") {
  // One-bit codes for "length 258" and "distance 1": every two bits of
  // input become 258 bytes of output.
  DynamicBlock block;
  block.lit[0] = 2;
  block.lit[256] = 2;
  block.lit[285] = 1;
  block.dist.assign(1, 1);

  // First a small, complete instance to prove the construction is valid.
  Bits small;
  block.header(small, true);
  block.symbol(small, 0);
  for (int i = 0; i < 1000; ++i) block.match(small, 258, 1);
  block.symbol(small, 256);
  const auto expanded = inflate_raw(small.bytes());
  REQUIRE(expanded);
  CHECK(expanded->size() == 258001);
  CHECK(std::ranges::all_of(*expanded, [](std::byte b) { return b == std::byte{0}; }));

  // The matches are all-zero bits, so the bomb is the header followed by zeros.
  Bits head;
  block.header(head, true);
  block.symbol(head, 0);
  Bytes bomb = head.bytes();
  const std::size_t header_size = bomb.size();
  bomb.resize(header_size + (std::size_t{1} << 30) / 1032 + 1, std::byte{0});
  CHECK(bomb.size() < 1100000);  // about 1 MiB in, more than 1 GiB out

  for (const std::uint64_t limit : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{4096}, std::uint64_t{1} << 20,
                                    std::uint64_t{1} << 24}) {
    CHECK(status_of(inflate_raw(bomb, limit)) == Status::Limit);
    Inflater inflater;
    Bytes out;
    CHECK(status_of(inflater.feed(bomb, out, limit)) == Status::Limit);
    CHECK(out.size() <= limit);
    CHECK(out.capacity() <= limit + 64);  // never more memory than the limit permits
    CHECK(inflater.total_out() == out.size());
  }
  // The limit counts across calls, and across output buffers.
  Inflater inflater;
  Bytes out;
  std::size_t offset = 0;
  Status status = Status::Ok;
  std::uint64_t produced = 0;
  while (status == Status::Ok && offset < bomb.size()) {
    const std::size_t take = std::min<std::size_t>(100, bomb.size() - offset);
    const auto progress = inflater.feed(ByteView(bomb).subspan(offset, take), out, 500000);
    status = status_of(progress);
    produced += out.size();
    CHECK(out.capacity() <= 500000 + 64);
    out.clear();
    offset += take;
  }
  CHECK(status == Status::Limit);
  CHECK(produced <= 500000);
  CHECK(produced > 500000 - 258);
  CHECK(offset < header_size + 1000);
  // Wrapped in zlib and gzip framing.
  Bytes zlib_bomb = from_hex("789c");
  append(zlib_bomb, ByteView(bomb));
  CHECK(status_of(zlib_decompress(zlib_bomb, 1 << 20)) == Status::Limit);
  Bytes gzip_bomb = from_hex("1f8b08000000000000ff");
  append(gzip_bomb, ByteView(bomb));
  CHECK(status_of(gzip_decompress(gzip_bomb, 1 << 20)) == Status::Limit);
}

TEST_CASE("deflate: the output limit is exact") {
  const Bytes data = mixed_data(100000, 9);
  for (const int level : {0, 1, 6, 9}) {
    const Bytes raw = *deflate_raw(data, level);
    CHECK(status_of(inflate_raw(raw, data.size())) == Status::Ok);
    CHECK(status_of(inflate_raw(raw, data.size() - 1)) == Status::Limit);
    CHECK(status_of(inflate_raw(raw, data.size() / 2)) == Status::Limit);
    CHECK(status_of(inflate_raw(raw, 1)) == Status::Limit);
    // The same with input arriving in pieces.
    CHECK(feed_in_chunks(Inflater::Format::Raw, raw, 1000, true, data.size()).finished);
    CHECK(feed_in_chunks(Inflater::Format::Raw, raw, 1000, false, data.size()).finished);
    CHECK(feed_in_chunks(Inflater::Format::Raw, raw, 1000, true, data.size() - 1).status == Status::Limit);
    CHECK(feed_in_chunks(Inflater::Format::Raw, raw, 1000, false, data.size() - 1).status == Status::Limit);
  }
  // Output is appended: what the buffer held before does not count.
  Bytes out = to_bytes("prefix:");
  Inflater inflater;
  const Bytes raw = *deflate_raw(as_bytes(sample));
  const auto progress = inflater.feed(raw, out, sample.size());
  REQUIRE(progress);
  CHECK(progress->finished);
  CHECK(progress->produced == sample.size());
  CHECK(str(out) == "prefix:" + std::string(sample));
}

// =========================================================================== incremental interfaces

TEST_CASE("deflate: Inflater fed in pieces equals one-shot decoding") {
  const Bytes data = mixed_data(70000, 21);
  struct Wrapped {
    Inflater::Format format;
    Bytes stream;
  };
  const std::vector<Wrapped> streams = {
      {Inflater::Format::Raw, *deflate_raw(data, 6)},
      {Inflater::Format::Zlib, *zlib_compress(data, 6)},
      {Inflater::Format::Gzip, *gzip_compress(data, 6, {.filename = "data.bin", .mtime = 99})},
      {Inflater::Format::Raw, *deflate_raw(data, 0)},
      {Inflater::Format::Zlib, *zlib_compress(data, 1)},
      {Inflater::Format::Gzip, *gzip_compress(data, 9)},
  };
  for (const Wrapped& wrapped : streams) {
    for (const std::size_t chunk : {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{7}, std::size_t{15},
                                    std::size_t{16}, std::size_t{17}, std::size_t{4096}, std::size_t{100000}}) {
      for (const bool keep : {true, false}) {
        const Fed fed = feed_in_chunks(wrapped.format, wrapped.stream, chunk, keep);
        CHECK(fed.status == Status::Ok);
        CHECK(fed.finished);
        CHECK(fed.consumed == wrapped.stream.size());
        CHECK_MESSAGE(fed.data == data, "chunk ", chunk, " keep ", keep);
      }
    }
  }
}

TEST_CASE("deflate: Inflater reports where the stream ends") {
  const Bytes data = text_like(5000, 4);
  for (const Inflater::Format format : {Inflater::Format::Raw, Inflater::Format::Zlib, Inflater::Format::Gzip}) {
    for (const int level : {0, 6}) {
      Deflater deflater(format, level);
      Bytes stream;
      REQUIRE(deflater.feed(data, stream));
      REQUIRE(deflater.finish(stream));
      const std::size_t stream_size = stream.size();
      Bytes followed = stream;
      append(followed, ByteView(test::random_bytes(40, 8)));
      for (const std::size_t chunk : {std::size_t{1}, std::size_t{5}, std::size_t{64}, followed.size()}) {
        const Fed fed = feed_in_chunks(format, followed, chunk);
        CHECK(fed.finished);
        CHECK(fed.consumed == stream_size);
        CHECK(fed.data == data);
      }
      // After the end nothing more is consumed or produced.
      Inflater inflater(format);
      Bytes out;
      const auto progress = inflater.feed(followed, out);
      REQUIRE(progress);
      CHECK(progress->finished);
      CHECK(progress->consumed == stream_size);
      const auto again = inflater.feed(followed, out);
      REQUIRE(again);
      CHECK(again->finished);
      CHECK(again->consumed == 0);
      CHECK(again->produced == 0);
      CHECK(out == data);
      CHECK(inflater.total_in() == stream_size);
      CHECK(inflater.total_out() == data.size());
    }
  }
}

TEST_CASE("deflate: Inflater errors are sticky and reset() starts over") {
  Inflater inflater(Inflater::Format::Zlib);
  Bytes out;
  const auto bad = inflater.feed(as_bytes("garbage garbage garbage"), out);
  REQUIRE_FALSE(bad);
  CHECK(bad.error().code == Status::Decode);
  const Bytes good = *zlib_compress(as_bytes(sample));
  const auto still_bad = inflater.feed(good, out);
  REQUIRE_FALSE(still_bad);
  CHECK(still_bad.error().code == Status::Decode);
  CHECK(still_bad.error().message == bad.error().message);
  CHECK_FALSE(inflater.finished());

  inflater.reset();
  out.clear();
  const auto progress = inflater.feed(good, out);
  REQUIRE(progress);
  CHECK(progress->finished);
  CHECK(str(out) == sample);
  CHECK(inflater.total_out() == sample.size());

  // Moving keeps the state.
  Inflater first(Inflater::Format::Zlib);
  Bytes moved_out;
  REQUIRE(first.feed(ByteView(good).first(5), moved_out));
  Inflater second = std::move(first);
  const auto rest = second.feed(ByteView(good).subspan(5), moved_out);
  REQUIRE(rest);
  CHECK(rest->finished);
  CHECK(str(moved_out) == sample);
}

TEST_CASE("deflate: Deflater fed in pieces produces exactly the one-shot stream") {
  // Long enough to make the compressor slide its window twice.
  const Bytes data = mixed_data(700000, 31);
  for (const int level : {0, 1, 6, 9}) {
    const Bytes raw = *deflate_raw(data, level);
    for (const std::size_t chunk : {std::size_t{1000}, std::size_t{65535}, std::size_t{65536}, std::size_t{300000}}) {
      if (level == 9 && chunk != 65535) continue;  // the slowest level: one variant is enough
      CHECK_MESSAGE(deflate_in_chunks(Deflater::Format::Raw, level, data, chunk) == raw, "level ", level, " chunk ", chunk);
    }
    if (level <= 1) CHECK(deflate_in_chunks(Deflater::Format::Zlib, level, data, 77777) == *zlib_compress(data, level));
    if (level == 6) CHECK(deflate_in_chunks(Deflater::Format::Gzip, level, data, 77777) == *gzip_compress(data, level));
    CHECK(*inflate_raw(raw) == data);
  }
  const Bytes small = text_like(3000, 6);
  for (const int level : {0, 1, 4, 6, 9}) {
    CHECK(deflate_in_chunks(Deflater::Format::Raw, level, small, 1) == *deflate_raw(small, level));
    CHECK(deflate_in_chunks(Deflater::Format::Zlib, level, small, 1) == *zlib_compress(small, level));
    CHECK(deflate_in_chunks(Deflater::Format::Gzip, level, small, 7) == *gzip_compress(small, level));
  }
}

TEST_CASE("deflate: Deflater life cycle") {
  Deflater deflater(Deflater::Format::Gzip, 6, {.filename = "f.txt", .mtime = 42});
  Bytes out;
  CHECK_FALSE(deflater.finished());
  REQUIRE(deflater.feed(as_bytes("abc"), out));
  REQUIRE(deflater.feed({}, out));
  REQUIRE(deflater.feed(as_bytes("def"), out));
  REQUIRE(deflater.finish(out));
  CHECK(deflater.finished());
  CHECK(deflater.total_in() == 6);
  CHECK(deflater.total_out() == out.size());
  GzipHeader header;
  CHECK(str(*gzip_decompress(out, no_output_limit, &header)) == "abcdef");
  CHECK(header.filename == "f.txt");
  CHECK(header.mtime == 42u);
  CHECK(out == *gzip_compress(as_bytes("abcdef"), 6, {.filename = "f.txt", .mtime = 42}));
  const std::size_t size = out.size();
  CHECK(status_of(deflater.feed(as_bytes("more"), out)) == Status::InvalidArgument);
  CHECK(status_of(deflater.finish(out)) == Status::InvalidArgument);
  CHECK(out.size() == size);

  // Output is appended to whatever the buffer holds.
  Bytes prefixed = to_bytes("PREFIX");
  Deflater raw(Deflater::Format::Raw, 9);
  REQUIRE(raw.feed(as_bytes(sample), prefixed));
  REQUIRE(raw.finish(prefixed));
  CHECK(str(ByteView(prefixed).first(6)) == "PREFIX");
  CHECK(str(*inflate_raw(ByteView(prefixed).subspan(6))) == sample);

  // Nothing fed at all.
  for (const int level : {0, 6}) {
    Deflater empty(Deflater::Format::Zlib, level);
    Bytes stream;
    REQUIRE(empty.finish(stream));
    CHECK(zlib_decompress(stream)->empty());
  }
}

// =========================================================================== compressor

TEST_CASE("deflate: round trips at every level and size") {
  for (const std::size_t size : {0u, 1u, 2u, 100u, 65535u, 65536u, 65537u, 300000u}) {
    // Half compressible, half random.
    Bytes data = test::random_bytes(size / 2, static_cast<std::uint32_t>(size) + 3);
    data.resize(size, std::byte{'x'});
    for (const int level : {0, 1, 6, 9}) {
      const auto raw = deflate_raw(data, level);
      REQUIRE(raw);
      std::size_t consumed = 0;
      const auto raw_back = inflate_raw(*raw, no_output_limit, &consumed);
      REQUIRE(raw_back);
      CHECK(*raw_back == data);
      CHECK(consumed == raw->size());

      const auto z = zlib_compress(data, level);
      REQUIRE(z);
      CHECK(*zlib_decompress(*z) == data);

      const auto gz = gzip_compress(data, level);
      REQUIRE(gz);
      CHECK(*gzip_decompress(*gz) == data);
    }
  }
  // Out-of-range levels are clamped rather than rejected.
  CHECK(*inflate_raw(*deflate_raw(as_bytes(sample), 42)) == to_bytes(sample));
  CHECK(*inflate_raw(*deflate_raw(as_bytes(sample), -3)) == to_bytes(sample));
  CHECK(*deflate_raw(as_bytes(sample), 42) == *deflate_raw(as_bytes(sample), 9));
  CHECK(*deflate_raw(as_bytes(sample), -3) == *deflate_raw(as_bytes(sample), 0));
}

TEST_CASE("deflate: round trips across sizes, kinds of data and levels") {
  const std::array<std::size_t, 14> sizes = {0,     1,     2,     3,     257,   258,   259,
                                             32767, 32768, 32769, 65535, 65536, 65537, std::size_t{1} << 20};
  std::uint32_t seed = 100;
  for (const std::size_t size : sizes) {
    const std::array<Bytes, 4> kinds = {Bytes(size, std::byte{0}), test::random_bytes(size, ++seed),
                                        text_like(size, ++seed), long_distance_repeats(size, ++seed)};
    for (std::size_t kind = 0; kind < kinds.size(); ++kind) {
      const Bytes& data = kinds[kind];
      for (const int level : {0, 1, 4, 6, 9}) {
        const auto packed = deflate_raw(data, level);
        REQUIRE(packed);
        std::size_t consumed = 0;
        const auto back = inflate_raw(*packed, data.size(), &consumed);
        REQUIRE_MESSAGE(back.has_value(), "size ", size, " kind ", kind, " level ", level);
        CHECK_MESSAGE(*back == data, "size ", size, " kind ", kind, " level ", level);
        CHECK(consumed == packed->size());
        // Incompressible data must not grow by more than the stored-block
        // overhead: five bytes per 65535, plus one block per compression step.
        CHECK(packed->size() <= size + 5 * (size / 65535 + 1) + (level > 0 ? 5 * (size / 262144 + 1) : 0));
        if (level > 0 && size >= 257 && kind == 0) CHECK(packed->size() < 32 + size / 500);
        if (level > 0 && size >= 257 && kind == 2) CHECK(packed->size() < size - size / 3);
        if (level >= 4 && kind == 3 && size >= 32767) {
          // Three of every seven bytes after the first `distance` repeat what
          // stood `distance` earlier (32768 for the larger sizes: the very
          // edge of the window); at least a third of that region must be saved.
          // (The greedy levels skip hash entries inside matches and may miss
          // repeats of repeats, so this is asked of the lazy levels only.)
          const std::size_t distance = size > 40000 ? 32768 : size / 2;
          CHECK_MESSAGE(packed->size() < size - (size - distance) / 3, "size ", size, " level ", level);
        }
        if (level == 1 && kind == 3 && size >= 32767) CHECK(packed->size() < size - size / 20);
      }
    }
  }
}

TEST_CASE("deflate: level 0 writes stored blocks split at 65535 bytes") {
  CHECK(hex_encode(*deflate_raw({}, 0)) == "010000ffff");
  CHECK(hex_encode(*deflate_raw(as_bytes("hello"), 0)) == "010500faff68656c6c6f");
  CHECK(hex_encode(*deflate_raw({}, 6)) == "0300");  // an empty fixed block

  const Bytes data = test::random_bytes(65535 * 2 + 10, 77);
  const Bytes stored = *deflate_raw(data, 0);
  REQUIRE(stored.size() == data.size() + 3 * 5);
  CHECK(hex_encode(ByteView(stored).first(5)) == "00ffff0000");
  CHECK(hex_encode(ByteView(stored).subspan(5 + 65535, 5)) == "00ffff0000");
  CHECK(hex_encode(ByteView(stored).subspan(2 * (5 + 65535), 5)) == "010a00f5ff");
  CHECK(std::equal(data.begin(), data.begin() + 65535, stored.begin() + 5));
  CHECK(*inflate_raw(stored) == data);

  // An exact multiple ends with a full block, not an empty one.
  const Bytes exact = test::random_bytes(65535 * 2, 78);
  const Bytes exact_stored = *deflate_raw(exact, 0);
  REQUIRE(exact_stored.size() == exact.size() + 2 * 5);
  CHECK(hex_encode(ByteView(exact_stored).subspan(5 + 65535, 5)) == "01ffff0000");
  CHECK(*inflate_raw(exact_stored) == exact);
}

TEST_CASE("deflate: compression actually compresses and is deterministic") {
  const Bytes zeros(100000, std::byte{0});
  const auto packed = deflate_raw(zeros, 9);
  REQUIRE(packed);
  CHECK(packed->size() < 1000);
  CHECK(*deflate_raw(zeros, 9) == *packed);
  CHECK(*gzip_compress(zeros) == *gzip_compress(zeros));

  const Bytes megabyte(std::size_t{1} << 20, std::byte{0});
  for (const int level : {1, 4, 6, 9}) CHECK_MESSAGE(deflate_raw(megabyte, level)->size() < 2048, "level ", level);

  const Bytes text = text_like(400000, 12);
  std::size_t previous = text.size();
  for (const int level : {1, 4, 6, 9}) {
    // More effort may lose a few bytes to a different choice of blocks, never much.
    const std::size_t size = deflate_raw(text, level)->size();
    CHECK_MESSAGE(size <= previous + previous / 200, "level ", level);
    CHECK(size < text.size() / 2);
    previous = size;
  }
  CHECK(deflate_raw(text, 9)->size() < deflate_raw(text, 1)->size());

  // Short inputs prefer the fixed code or a stored block over a dynamic header.
  CHECK(deflate_raw(as_bytes("a"), 9)->size() <= 3);
  CHECK(deflate_raw(as_bytes(sample), 9)->size() <= 12);
  const Bytes noise = test::random_bytes(1000, 4);
  CHECK(deflate_raw(noise, 9)->size() == noise.size() + 5);

  // Blocks follow the data: three megabyte-scale sections of different
  // character must each compress about as well as on their own.
  const Bytes a = text_like(200000, 1);
  const Bytes b = test::random_bytes(200000, 2);
  const Bytes c = pattern(200000);
  const std::size_t separate = deflate_raw(a, 6)->size() + deflate_raw(b, 6)->size() + deflate_raw(c, 6)->size();
  CHECK(deflate_raw(concat({a, b, c}), 6)->size() < separate + separate / 100);
}

TEST_CASE("deflate: output is identical on every platform") {
  // Golden values: they change only when the compressor itself is changed
  // on purpose, never between compilers, CPUs or word sizes.
  const Bytes data = concat({lorem_times(40), pattern(30000), text_like(50000, 2), test::random_bytes(3000, 3),
                             Bytes(10000, std::byte{7}), lorem_times(20)});
  struct Golden {
    int level;
    std::size_t size;
    std::uint32_t crc;
  };
  const std::array<Golden, 4> golden = {
      {{1, 17255, 1552293043u}, {4, 15188, 2844861365u}, {6, 14448, 1470399089u}, {9, 14222, 1009594540u}}};
  for (const Golden& g : golden) {
    const Bytes packed = *deflate_raw(data, g.level);
    CHECK_MESSAGE(packed.size() == g.size, "level ", g.level, " size ", packed.size());
    CHECK_MESSAGE(crc32(packed) == g.crc, "level ", g.level, " crc ", crc32(packed));
    CHECK(*inflate_raw(packed) == data);
  }
}

TEST_CASE("deflate: compression ratio does not regress (benchmark guard)") {
  // Sizes recorded with the 4-byte hash chains + nearest 3-byte match
  // matcher, with 1 % slack: a faster matcher must not pay in size. (Time is
  // not checked; the speed-up is measured outside the suite.)
  Bytes photo;  // PNG-style rows: filter byte "Sub" then RGB differences of a smooth noisy picture
  {
    constexpr std::uint32_t w = 400;
    constexpr std::uint32_t h = 300;
    std::uint32_t state = 12345;
    std::vector<std::uint8_t> row(w * 3);
    for (std::uint32_t y = 0; y < h; ++y) {
      for (std::uint32_t x = 0; x < w * 3; ++x) {
        state = state * 1664525u + 1013904223u;
        const int smooth = 128 + static_cast<int>((x * 7 + y * 5) % 97) - 48;
        row[x] = static_cast<std::uint8_t>(smooth + static_cast<int>((state >> 24) & 7));
      }
      photo.push_back(std::byte{1});
      for (std::uint32_t i = 0; i < w * 3; ++i) photo.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(row[i] - (i >= 3 ? row[i - 3] : 0))));
    }
  }
  const Bytes text = text_like(300000, 21);
  struct Bound {
    const Bytes* data;
    int level;
    std::size_t recorded;
  };
  const Bound bounds[] = {{&photo, 1, 221002}, {&photo, 6, 217643}, {&photo, 9, 217643},
                          {&text, 1, 61775},   {&text, 6, 46119},   {&text, 9, 44025}};
  for (const Bound& b : bounds) {
    const Bytes packed = *deflate_raw(*b.data, b.level);
    MESSAGE("level ", b.level, ": ", b.data->size(), " -> ", packed.size());
    CHECK(packed.size() <= b.recorded + b.recorded / 100);
    CHECK(*inflate_raw(packed) == *b.data);
  }
}

// =========================================================================== robustness

TEST_CASE("deflate: corrupt and truncated input is rejected, never crashes") {
  const Bytes data = test::random_bytes(5000, 11);
  const Bytes z = *zlib_compress(data);
  const Bytes gz = *gzip_compress(data);
  const Bytes raw = *deflate_raw(data);

  for (std::size_t length = 0; length < z.size(); length += 97) {
    CHECK_FALSE(zlib_decompress(ByteView(z).first(length)));
  }
  for (std::size_t length = 0; length < gz.size(); length += 97) {
    CHECK_FALSE(gzip_decompress(ByteView(gz).first(length)));
  }
  CHECK_FALSE(inflate_raw(ByteView(raw).first(raw.size() / 2)));
  CHECK(inflate_raw({}).error().code == Status::Decode);

  Bytes bad_checksum = z;
  bad_checksum.back() ^= std::byte{0x01};
  CHECK(zlib_decompress(bad_checksum).error().code == Status::Decode);
  Bytes bad_crc = gz;
  bad_crc[bad_crc.size() - 5] ^= std::byte{0x01};
  CHECK(gzip_decompress(bad_crc).error().code == Status::Decode);
  Bytes bad_length = gz;
  bad_length.back() ^= std::byte{0x01};
  CHECK(gzip_decompress(bad_length).error().code == Status::Decode);

  CHECK(gzip_decompress(as_bytes("not gzip at all")).error().code == Status::Decode);
  CHECK(gzip_decompress(*hex_decode("1f8b0900000000000003")).error().code == Status::Decode);  // unknown method
  CHECK(zlib_decompress(as_bytes("garbage garbage garbage")).error().code == Status::Decode);
  for (std::uint32_t seed = 1; seed <= 200; ++seed) {
    const Bytes noise = test::random_bytes(64 + seed, seed);
    (void)inflate_raw(noise, 1 << 20);
    (void)zlib_decompress(noise, 1 << 20);
    Bytes fake_gzip = *hex_decode("1f8b0800000000000003");
    append(fake_gzip, ByteView(noise));
    (void)gzip_decompress(fake_gzip, 1 << 20);
  }
}

TEST_CASE("deflate: randomly damaged streams never crash and decode the same in pieces") {
  // Valid streams of several shapes, then thousands of mutations of them.
  // Whatever the verdict, one-shot and byte-wise decoding must agree on it
  // and on the output; the sanitizer builds turn any stray access into a failure.
  std::vector<Bytes> seeds;
  const Bytes text = text_like(4000, 41);
  const Bytes mixed = mixed_data(5000, 42);
  seeds.push_back(*deflate_raw(ByteView(mixed).first(1200), 0));
  for (const int level : {1, 6, 9}) {
    seeds.push_back(*deflate_raw(text, level));
    seeds.push_back(*deflate_raw(mixed, level));
  }
  seeds.push_back(*deflate_raw(as_bytes(sample), 6));
  seeds.push_back(test::fixture("util/pattern_4000_fixed.deflate"));
  seeds.push_back(test::fixture("util/lorem_x8_dynamic.deflate"));
  seeds.push_back(test::fixture("util/lorem_pattern_lorem_flushed.deflate"));

  std::uint32_t state = 0x9E3779B9u;
  const auto next = [&state]() {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  };
  constexpr std::uint64_t limit = 1 << 22;
  std::size_t accepted = 0;
  std::size_t rejected = 0;
  for (int round = 0; round < 3000; ++round) {
    Bytes stream = seeds[next() % seeds.size()];
    REQUIRE(!stream.empty());
    const unsigned kind = next() % 6;
    if (kind == 0) {
      stream.resize(next() % stream.size());
    } else if (kind == 1) {
      // Damage in the first bytes: block headers and code descriptions.
      const std::size_t span = std::min<std::size_t>(stream.size(), 60);
      for (unsigned i = 0; i <= next() % 3; ++i) stream[next() % span] ^= static_cast<std::byte>(1u << (next() % 8));
    } else if (kind == 2) {
      for (unsigned i = 0; i <= next() % 3; ++i) stream[next() % stream.size()] ^= static_cast<std::byte>(1u << (next() % 8));
    } else if (kind == 3) {
      stream[next() % stream.size()] = static_cast<std::byte>(next() & 0xFFu);
    } else if (kind == 4) {
      stream.erase(stream.begin() + static_cast<std::ptrdiff_t>(next() % stream.size()));
    } else {
      const Bytes& other = seeds[next() % seeds.size()];
      stream.resize(next() % stream.size());
      append(stream, ByteView(other).subspan(next() % other.size()));
    }
    std::size_t consumed = 0;
    const auto whole = inflate_raw(stream, limit, &consumed);
    const Fed pieces = feed_in_chunks(Inflater::Format::Raw, stream, 1 + next() % 9, (round & 1) != 0, limit);
    if (whole) {
      ++accepted;
      CHECK(pieces.finished);
      CHECK(pieces.consumed == consumed);
      CHECK(pieces.data == *whole);
    } else {
      ++rejected;
      CHECK_FALSE(pieces.finished);
      CHECK((pieces.status == Status::Ok || pieces.status == whole.error().code));
    }
  }
  CHECK(accepted > 100);
  CHECK(rejected > 1000);

  // The same for the framed formats, where checksums catch what the bit stream does not.
  const Bytes z = *zlib_compress(mixed, 6);
  const Bytes gz = *gzip_compress(mixed, 6, {.filename = "x", .mtime = 1});
  for (int round = 0; round < 1500; ++round) {
    Bytes damaged = (round & 1) != 0 ? z : gz;
    damaged[next() % damaged.size()] ^= static_cast<std::byte>(1u << (next() % 8));
    const auto result = (round & 1) != 0 ? zlib_decompress(damaged, limit) : gzip_decompress(damaged, limit);
    // A flipped bit may land in a field that is not checked (gzip mtime, XFL, OS, or the name).
    if (result) CHECK(*result == mixed);
  }
}
