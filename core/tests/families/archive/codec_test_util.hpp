// Helpers shared by the compression codec tests of the archive family.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <string>

#include "families/archive/tree_fixture.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"

#include "families/archive/compression.hpp"

namespace cvt::test::codec {

namespace cz = cvt::compression;

/// lorem.txt + noise.bin of the fixture tree (the "mixed.bin" the fixtures were made from).
inline Bytes mixed() {
  Bytes out;
  for (const tree::File& f : tree::files()) {
    if (f.path == "lorem.txt") append(out, f.bytes);
  }
  append(out, tree::noise(4096));
  return out;
}

inline Bytes lorem() {
  for (const tree::File& f : tree::files()) {
    if (f.path == "lorem.txt") return f.bytes;
  }
  return {};
}

/// Text-like data with long-range repetition and variety (~`size` bytes).
inline Bytes text_corpus(std::size_t size) {
  static constexpr std::string_view words[] = {
      "archive", "stream", "the",  "of",    "compression", "block",  "window", "and",  "match", "literal",
      "offset",  "length", "data", "table", "Huffman",     "symbol", "a",      "file", "entry", "header"};
  std::string text;
  std::uint32_t state = 12345;
  while (text.size() < size) {
    state = state * 1103515245u + 12345u;
    text += words[(state >> 16) % std::size(words)];
    text += (state >> 8) % 11 == 0 ? ".\n" : " ";
  }
  text.resize(size);
  return to_bytes(text);
}

/// Encode and decode with the given level; checks exactness and returns the encoded size.
inline std::size_t round_trip(cz::Method method, ByteView input, int level) {
  cz::EncodeOptions e;
  e.level = level;
  const auto packed = cz::compress(method, input, e);
  REQUIRE_OK(packed);
  cz::DecodeOptions d;
  d.output_size = input.size();
  const auto plain = cz::decompress(method, *packed, d);
  REQUIRE_OK(plain);
  CHECK(plain->size() == input.size());
  CHECK(*plain == Bytes(input.begin(), input.end()));
  return packed->size();
}

/// Truncations, bit flips and garbage must fail cleanly (or decode to
/// something) without crashing; run under ASan.
inline void hostile(cz::Method method, ByteView valid, cz::DecodeOptions options = {}) {
  if (options.max_output == cz::unlimited) options.max_output = 1u << 24;
  const std::size_t step = valid.size() / 300 + 1;
  for (std::size_t n = 0; n < valid.size(); n += step) {
    (void)cz::decompress(method, valid.first(n), options);
  }
  for (std::uint32_t seed = 1; seed <= 48 && !valid.empty(); ++seed) {
    Bytes mutated(valid.begin(), valid.end());
    const Bytes noise = test::random_bytes(6, seed);
    for (std::size_t k = 0; k < 3; ++k) {
      const std::size_t at = (std::to_integer<std::size_t>(noise[k * 2]) * 977u + seed * 7919u) % mutated.size();
      mutated[at] ^= noise[k * 2 + 1] | std::byte{1};
    }
    (void)cz::decompress(method, mutated, options);
  }
  (void)cz::decompress(method, test::random_bytes(2000, 77), options);
}

}  // namespace cvt::test::codec
