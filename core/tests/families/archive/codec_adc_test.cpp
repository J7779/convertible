// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// Apple Data Compression (Method::Adc), from hand-assembled streams and from
// a small reference encoder written here (the DMG tests decode real images).
#include <array>
#include <string>

#include "families/archive/codec_test_util.hpp"

using namespace cvt;
namespace cz = cvt::compression;
namespace tc = cvt::test::codec;

namespace {

Bytes bytes(std::initializer_list<int> list) {
  Bytes b;
  for (const int v : list) b.push_back(static_cast<std::byte>(v));
  return b;
}

/// Greedy ADC encoder: literal runs of up to 128 bytes, 2-byte copies for
/// distances up to 1024 and lengths 3..18, 3-byte copies up to 65536 / 4..67.
Bytes adc_encode(ByteView in) {
  Bytes out;
  std::vector<std::size_t> head(1u << 16, SIZE_MAX);
  std::size_t lit_start = 0;
  const auto flush = [&](std::size_t end) {
    while (lit_start < end) {
      const std::size_t n = std::min<std::size_t>(128, end - lit_start);
      out.push_back(static_cast<std::byte>(0x80u | (n - 1)));
      out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(lit_start),
                 in.begin() + static_cast<std::ptrdiff_t>(lit_start + n));
      lit_start += n;
    }
  };
  const auto hash = [&](std::size_t i) {
    return ((std::to_integer<unsigned>(in[i]) << 8) ^ (std::to_integer<unsigned>(in[i + 1]) << 4) ^
            std::to_integer<unsigned>(in[i + 2])) & 0xFFFFu;
  };
  std::size_t i = 0;
  while (i + 3 <= in.size()) {
    const unsigned h = hash(i);
    const std::size_t cand = head[h];
    head[h] = i;
    std::size_t len = 0;
    if (cand != SIZE_MAX && i - cand <= 65536) {
      while (i + len < in.size() && len < 67 && in[cand + len] == in[i + len]) ++len;
    }
    const std::size_t dist = i - cand;
    if (len >= 4 || (len == 3 && dist <= 1024)) {
      flush(i);
      if (dist <= 1024 && len <= 18) {
        const std::size_t off = dist - 1;
        out.push_back(static_cast<std::byte>(((len - 3) << 2) | (off >> 8)));
        out.push_back(static_cast<std::byte>(off & 0xFFu));
      } else {
        const std::size_t off = dist - 1;
        out.push_back(static_cast<std::byte>(0x40u | (len - 4)));
        out.push_back(static_cast<std::byte>(off >> 8));
        out.push_back(static_cast<std::byte>(off & 0xFFu));
      }
      i += len;
      lit_start = i;
    } else {
      ++i;
    }
  }
  flush(in.size());
  return out;
}

Result<Bytes> adc(ByteView data, std::optional<std::uint64_t> size = std::nullopt) {
  cz::DecodeOptions d;
  d.output_size = size;
  return cz::decompress(cz::Method::Adc, data, d);
}

}  // namespace

TEST_CASE("archive/adc: hand-assembled streams") {
  // Literal run of 5, a 2-byte copy (3 bytes from 4 back), a 3-byte copy (4 bytes from 1 back).
  const Bytes s = bytes({0x84, 'h', 'e', 'l', 'l', 'o', 0x00, 0x03, 0x40, 0x00, 0x00});
  CHECK(test::str(*adc(s)) == "helloellllll");
  CHECK(test::str(*adc(s, 12)) == "helloellllll");
  // Longest forms: 2-byte copy of 18 bytes from 1024 back, 3-byte copy of 67 from 65536 back.
  Bytes big = bytes({});
  const Bytes noise = test::tree::noise(70000);
  for (std::size_t at = 0; at < noise.size(); at += 128) {
    const std::size_t n = std::min<std::size_t>(128, noise.size() - at);
    big.push_back(static_cast<std::byte>(0x80u | (n - 1)));
    big.insert(big.end(), noise.begin() + static_cast<std::ptrdiff_t>(at),
               noise.begin() + static_cast<std::ptrdiff_t>(at + n));
  }
  append(big, bytes({0x3F, 0xFF, 0x7F, 0xFF, 0xFF}));
  const auto plain = adc(big);
  REQUIRE_OK(plain);
  REQUIRE_EQ(plain->size(), 70000u + 18u + 67u);
  CHECK(Bytes(plain->begin(), plain->begin() + 70000) == noise);
  for (std::size_t k = 0; k < 18; ++k) CHECK((*plain)[70000 + k] == noise[70000 - 1024 + k]);
  for (std::size_t k = 0; k < 67; ++k) CHECK((*plain)[70018 + k] == (*plain)[70018 - 65536 + k]);
  CHECK(adc({})->empty());
}

TEST_CASE("archive/adc: round trips through a reference encoder") {
  for (const Bytes& input : {tc::text_corpus(300000), tc::mixed(), test::random_bytes(5000, 3), Bytes(100000, std::byte{0}),
                             tc::lorem()}) {
    const Bytes packed = adc_encode(input);
    const auto plain = adc(packed, input.size());
    REQUIRE_OK(plain);
    CHECK(*plain == input);
  }
  CHECK_LT(adc_encode(tc::text_corpus(300000)).size(), 300000u / 2);
}

TEST_CASE("archive/adc: options and damaged input") {
  const Bytes s = bytes({0x84, 'h', 'e', 'l', 'l', 'o', 0x00, 0x03, 0x40, 0x00, 0x00});
  CHECK_STATUS(adc(bytes({0x00, 0x00})), Status::Decode);            // copy before the start
  CHECK_STATUS(adc(bytes({0x80, 'a', 0x40, 0x00, 0x01})), Status::Decode);  // distance 2 > 1 byte
  CHECK_STATUS(adc(bytes({0x84, 'a'})), Status::Decode);                // truncated literal
  CHECK_STATUS(adc(bytes({0x80, 'a', 0x40, 0x00})), Status::Decode);  // truncated copy
  CHECK_STATUS(adc(bytes({0x80, 'a', 0x00})), Status::Decode);
  CHECK_STATUS(adc(s, 13), Status::Decode);  // shorter than declared
  CHECK(test::str(*adc(s, 8)) == "helloell");   // a chunk ends when its declared size is reached
  CHECK_STATUS(adc(s, 7), Status::Decode);   // ... but not inside an element
  cz::DecodeOptions d;
  d.max_output = 10;
  CHECK_STATUS(cz::decompress(cz::Method::Adc, s, d), Status::Limit);
  d.output_size = 12;
  CHECK_STATUS(cz::decompress(cz::Method::Adc, s, d), Status::Limit);
  cz::DecodeOptions prefix;
  prefix.stop_after = 6;
  CHECK(test::str(*cz::decompress(cz::Method::Adc, s, prefix)) == "helloe");
  // A tiny stream that expands a lot is bounded by max_output.
  Bytes bomb = bytes({0x80, 'x'});
  for (int i = 0; i < 20000; ++i) append(bomb, bytes({0x7F, 0x00, 0x00}));
  cz::DecodeOptions limited;
  limited.max_output = 100000;
  CHECK_STATUS(cz::decompress(cz::Method::Adc, bomb, limited), Status::Limit);
  tc::hostile(cz::Method::Adc, adc_encode(tc::mixed()));
  cz::DecodeOptions sized;
  sized.output_size = tc::mixed().size();
  tc::hostile(cz::Method::Adc, adc_encode(tc::mixed()), sized);
  CHECK(cz::can_decode(cz::Method::Adc));
  CHECK_FALSE(cz::can_encode(cz::Method::Adc));
}
