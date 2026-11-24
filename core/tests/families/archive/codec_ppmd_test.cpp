// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// PPMd var.H (7z, Method::Ppmd7), var.I rev.1 (ZIP method 98, Method::Ppmd8)
// and the RAR flavour of var.H.
#include <chrono>
#include <string>

#include "families/archive/codec_test_util.hpp"
#include "families/archive/codecs/ppmd.hpp"
#include "util/crc.hpp"

using namespace cvt;
namespace cz = cvt::compression;
namespace tc = cvt::test::codec;

namespace {

/// Hands the RAR test decoder its bytes.
class ViewSource final : public cz::detail::PpmdByteSource {
 public:
  explicit ViewSource(ByteView data) : data_(data) {}
  std::uint8_t next_byte() override {
    if (pos_ < data_.size()) return std::to_integer<std::uint8_t>(data_[pos_++]);
    ++overrun_;
    return 0;
  }
  std::size_t overrun() const { return overrun_; }

 private:
  ByteView data_;
  std::size_t pos_ = 0;
  std::size_t overrun_ = 0;
};

std::uint64_t le(ByteView b, std::size_t at, int bytes) {
  std::uint64_t v = 0;
  for (int i = bytes - 1; i >= 0; --i) v = (v << 8) | std::to_integer<std::uint64_t>(b[at + static_cast<std::size_t>(i)]);
  return v;
}

/// The packed stream of a single-file 7z archive written with -mhc=off: it
/// starts right after the 32-byte signature header and runs up to the header.
Bytes seven_zip_stream(std::string_view name) {
  const Bytes file = test::fixture(std::string("archive/ppmd/") + std::string(name));
  REQUIRE(file.size() > 32);
  const std::uint64_t length = le(file, 12, 8);
  REQUIRE(length <= file.size() - 32);
  return Bytes(file.begin() + 32, file.begin() + 32 + static_cast<std::ptrdiff_t>(length));
}

struct ZipEntry {
  Bytes data;  // starts with the PPMd parameter word
  std::uint64_t size = 0;
};

/// The data of the first entry of a ZIP file, from its local header.
ZipEntry zip_stream(std::string_view dir, std::string_view name) {
  const Bytes file = test::fixture(std::string("archive/") + std::string(dir) + "/" + std::string(name));
  std::size_t at = 0;
  while (at + 30 <= file.size() && le(file, at, 4) == 0x04034B50u) {
    const std::size_t packed = static_cast<std::size_t>(le(file, at + 18, 4));
    const std::size_t data = at + 30 + static_cast<std::size_t>(le(file, at + 26, 2) + le(file, at + 28, 2));
    REQUIRE(data + packed <= file.size());
    if (le(file, at + 8, 2) == 98u) {
      return {Bytes(file.begin() + static_cast<std::ptrdiff_t>(data), file.begin() + static_cast<std::ptrdiff_t>(data + packed)),
              le(file, at + 22, 4)};
    }
    at = data + packed;
  }
  FAIL("no PPMd entry in ", name);
  return {};
}

/// Sentences from a small grammar: structure at the word and phrase level,
/// where longer contexts pay off (unlike text_corpus, whose words are independent).
Bytes prose(std::size_t size) {
  static constexpr std::string_view subjects[] = {"The archive", "A stream", "Every block", "The decoder", "This model"};
  static constexpr std::string_view verbs[] = {"reads", "writes", "predicts", "keeps", "copies", "restores"};
  static constexpr std::string_view objects[] = {"the next symbol", "a long match", "its own context", "the window",
                                                 "every byte of the header", "a table of frequencies"};
  static constexpr std::string_view tails[] = {" before the end.", ", and then it stops.", " again.",
                                               " in the same order as before.", "."};
  std::string text;
  std::uint32_t state = 7;
  const auto pick = [&](std::size_t n) {
    state = state * 1664525u + 1013904223u;
    return (state >> 16) % n;
  };
  while (text.size() < size) {
    text += subjects[pick(std::size(subjects))];
    text += ' ';
    text += verbs[pick(std::size(verbs))];
    text += ' ';
    text += objects[pick(std::size(objects))];
    text += tails[pick(std::size(tails))];
    text += pick(7) == 0 ? "\n" : " ";
  }
  text.resize(size);
  return to_bytes(text);
}

Bytes props7(unsigned order, std::uint32_t memory) {
  Bytes p(5);
  p[0] = static_cast<std::byte>(order);
  for (std::size_t i = 0; i < 4; ++i) p[1 + i] = static_cast<std::byte>((memory >> (8 * i)) & 0xFFu);
  return p;
}

Bytes both() {
  Bytes b = tc::text_corpus(40000);
  append(b, test::tree::noise(10000));
  return b;
}

}  // namespace

TEST_CASE("archive/ppmd: 7z var.H streams from 7-Zip, with restarts at 64 KiB") {
  struct Case {
    const char* name;
    unsigned order;
    std::uint32_t memory;
    Bytes expected;
  };
  const Case cases[] = {
      {"corpus_o6_64k.7z", 6, 1u << 16, tc::text_corpus(60000)},
      {"corpus_o32_64k.7z", 32, 1u << 16, tc::text_corpus(60000)},
      {"corpus_o2_1m.7z", 2, 1u << 20, tc::text_corpus(60000)},
      {"both_o16_64k.7z", 16, 1u << 16, both()},
  };
  for (const Case& c : cases) {
    INFO("fixture ", c.name);
    const Bytes packed = seven_zip_stream(c.name);
    const Bytes props = props7(c.order, c.memory);
    cz::DecodeOptions d;
    d.properties = props;
    d.output_size = c.expected.size();
    const auto plain = cz::decompress(cz::Method::Ppmd7, packed, d);
    REQUIRE_OK(plain);
    CHECK(*plain == c.expected);
  }
}

TEST_CASE("archive/ppmd: ZIP var.I streams from 7-Zip (restart and cut-off)") {
  struct Case {
    const char* name;
    Bytes expected;
  };
  const Case cases[] = {
      {"corpus_o16_restart.zip", tc::text_corpus(100000)},
      {"corpus_o16_cutoff.zip", tc::text_corpus(100000)},
      {"both_o16_cutoff.zip", both()},
      {"both_o3.zip", both()},
  };
  for (const Case& c : cases) {
    INFO("fixture ", c.name);
    const ZipEntry e = zip_stream("ppmd", c.name);
    REQUIRE_EQ(e.size, c.expected.size());
    cz::DecodeOptions d;
    d.output_size = e.size;
    const auto plain = cz::decompress(cz::Method::Ppmd8, e.data, d);
    REQUIRE_OK(plain);
    CHECK(*plain == c.expected);
  }
}

TEST_CASE("archive/ppmd: our var.H encoder writes exactly what 7-Zip writes") {
  struct Case {
    const char* name;
    int level;
    Bytes input;
  };
  const Case cases[] = {
      {"corpus_o6_64k.7z", 5, tc::text_corpus(60000)},
      {"corpus_o32_64k.7z", 9, tc::text_corpus(60000)},
      {"both_o16_64k.7z", 7, both()},
  };
  for (const Case& c : cases) {
    INFO("fixture ", c.name);
    cz::EncodeOptions e;
    e.level = c.level;
    e.dictionary_size = 1u << 16;
    const auto packed = cz::compress(cz::Method::Ppmd7, c.input, e);
    REQUIRE_OK(packed);
    CHECK(*packed == seven_zip_stream(c.name));
  }
}

TEST_CASE("archive/ppmd: a 64 MiB model runs in a small heap, and falls back when that is not enough") {
  const Bytes packed = seven_zip_stream("corpus_o6_64m.7z");
  const Bytes props = props7(6, 64u << 20);
  const Bytes expected = tc::text_corpus(60000);
  cz::DecodeOptions d;
  d.properties = props;
  d.output_size = expected.size();
  const auto automatic = cz::decompress(cz::Method::Ppmd7, packed, d);
  REQUIRE_OK(automatic);
  CHECK(*automatic == expected);
  // A first run in 64 KiB must notice that it ran out and start over in the full 64 MiB.
  for (const std::uint32_t probe : {1u << 11, 1u << 16, 1u << 18}) {
    INFO("probe ", probe);
    const auto plain = cz::detail::ppmd7_decode_probed(packed, d, probe);
    REQUIRE_OK(plain);
    CHECK(*plain == expected);
  }
  // Same for var.I: a 1 MiB stream that never ran out, first decoded in 64 KiB.
  const ZipEntry e = zip_stream("ppmd", "both_o3.zip");
  cz::DecodeOptions d8;
  d8.output_size = e.size;
  for (const std::uint32_t probe : {1u << 11, 1u << 16}) {
    INFO("probe ", probe);
    const auto plain = cz::detail::ppmd8_decode_probed(e.data, d8, probe);
    REQUIRE_OK(plain);
    CHECK(*plain == both());
  }
}

TEST_CASE("archive/ppmd: the ZIP fixture of the zip tests, and var.I end markers") {
  const ZipEntry e = zip_stream("zip", "ppmd.zip");
  cz::DecodeOptions d;
  d.output_size = e.size;
  const auto plain = cz::decompress(cz::Method::Ppmd8, e.data, d);
  REQUIRE_OK(plain);
  CHECK(*plain == tc::lorem());
  // 7-Zip ends var.I data with an end marker: without a size the decoder stops there.
  const auto unsized = cz::decompress(cz::Method::Ppmd8, e.data);
  REQUIRE_OK(unsized);
  CHECK(*unsized == tc::lorem());
  const ZipEntry big = zip_stream("ppmd", "corpus_o16_cutoff.zip");
  const auto unsized_big = cz::decompress(cz::Method::Ppmd8, big.data);
  REQUIRE_OK(unsized_big);
  CHECK(*unsized_big == tc::text_corpus(100000));
}

namespace {

struct RarFile {
  Bytes packed;
  std::uint64_t size = 0;
  std::uint32_t crc = 0;
};

/// The data of the first file of a RAR 1.5-4.x archive.
RarFile rar_file(std::string_view name) {
  const Bytes file = test::fixture(std::string("archive/ppmd/") + std::string(name));
  REQUIRE(file.size() > 7);
  std::size_t at = 7;  // marker block
  while (at + 7 <= file.size()) {
    const auto type = std::to_integer<unsigned>(file[at + 2]);
    const auto flags = static_cast<unsigned>(le(file, at + 3, 2));
    const auto head = static_cast<std::size_t>(le(file, at + 5, 2));
    REQUIRE(head >= 7);
    if (type == 0x74) {
      const auto packed = static_cast<std::size_t>(le(file, at + 7, 4));
      REQUIRE(at + head + packed <= file.size());
      return {Bytes(file.begin() + static_cast<std::ptrdiff_t>(at + head),
                    file.begin() + static_cast<std::ptrdiff_t>(at + head + packed)),
              le(file, at + 11, 4), static_cast<std::uint32_t>(le(file, at + 16, 4))};
    }
    at += head + ((flags & 0x8000u) != 0 ? static_cast<std::size_t>(le(file, at + 7, 4)) : 0);
  }
  FAIL("no file header in ", name);
  return {};
}

/// A RAR 2.9 stream made of one PPM block, decoded with RAR's escape protocol
/// on top of RarPpmdDecoder (escape, then 2 = end, 4 = match, 5 = run, other = the escape byte itself).
Bytes rar_ppm_decode(ByteView data, std::uint64_t size) {
  ViewSource src(data);
  const unsigned flags = src.next_byte();
  REQUIRE((flags & 0x80u) != 0);  // PPM block
  REQUIRE((flags & 0x20u) != 0);  // new model
  const unsigned mb = src.next_byte();
  unsigned esc = 2;
  if ((flags & 0x40u) != 0) esc = src.next_byte();
  int order = static_cast<int>(flags & 0x1Fu) + 1;
  if (order > 16) order = 16 + (order - 16) * 3;
  cz::detail::RarPpmdDecoder ppm;
  CHECK_FALSE(ppm.has_model());
  REQUIRE_OK(ppm.restart(order, (mb + 1) << 20));
  CHECK(ppm.has_model());
  REQUIRE_OK(ppm.init_range_coder(src));
  Bytes out;
  const auto next = [&]() -> unsigned {
    const auto c = ppm.decode_symbol();
    REQUIRE_OK(c);
    return *c;
  };
  const auto copy = [&](std::size_t length, std::size_t distance) {
    REQUIRE(distance <= out.size());
    for (std::size_t i = 0; i < length; ++i) out.push_back(out[out.size() - distance]);
  };
  while (out.size() < size) {
    const unsigned c = next();
    if (c != esc) {
      out.push_back(static_cast<std::byte>(c));
      continue;
    }
    const unsigned code = next();
    if (code == 2) break;
    REQUIRE(code != 0);  // switch to LZ: not in these fixtures
    REQUIRE(code != 3);  // filter code: not in these fixtures
    if (code == 4) {
      std::size_t distance = 0;
      for (int i = 0; i < 3; ++i) distance = (distance << 8) | next();
      copy(next() + 32u, distance + 2);
    } else if (code == 5) {
      copy(next() + 4u, 1);
    } else {
      out.push_back(static_cast<std::byte>(c));
    }
  }
  CHECK_EQ(src.overrun(), 0u);
  return out;
}

}  // namespace

TEST_CASE("archive/ppmd: RAR 4 text compression from rar 6.23 through RarPpmdDecoder") {
  struct Case {
    const char* name;
    Bytes expected;
  };
  const Case cases[] = {
      {"lorem_ppm.rar", tc::lorem()},
      {"corpus_ppm.rar", tc::text_corpus(60000)},
      {"both_ppm.rar", both()},
  };
  for (const Case& c : cases) {
    INFO("fixture ", c.name);
    const RarFile f = rar_file(c.name);
    REQUIRE_EQ(f.size, c.expected.size());
    const Bytes plain = rar_ppm_decode(f.packed, f.size);
    CHECK_EQ(util::crc32(plain), f.crc);
    CHECK(plain == c.expected);
  }
  cz::detail::RarPpmdDecoder ppm;
  CHECK_STATUS(ppm.decode_symbol(), Status::Decode);
  CHECK_STATUS(ppm.restart(1, 1u << 20), Status::InvalidArgument);
  CHECK_STATUS(ppm.restart(65, 1u << 20), Status::InvalidArgument);
  CHECK_STATUS(ppm.restart(6, 1000), Status::InvalidArgument);
  CHECK_STATUS(ppm.restart(6, 257u << 20), Status::InvalidArgument);
  // Garbage never crashes the model.
  REQUIRE_OK(ppm.restart(8, 1u << 20));
  const Bytes noise = test::random_bytes(3000, 5);
  ViewSource src(noise);
  REQUIRE_OK(ppm.init_range_coder(src));
  for (int i = 0; i < 20000; ++i) {
    if (!ppm.decode_symbol()) break;
  }
}

TEST_CASE("archive/ppmd: var.H round trips at every level") {
  // Prose, then the same prose with small edits: long repeats reward the
  // higher orders of the higher levels.
  Bytes text = prose(100000);
  Bytes edited = text;
  for (std::size_t i = 500; i < edited.size(); i += 997) edited[i] = std::byte{'#'};
  append(text, edited);
  std::size_t previous = text.size();
  for (const int level : {1, 3, 5, 7, 9}) {
    INFO("level ", level);
    cz::EncodeOptions e;
    e.level = level;
    const auto packed = cz::compress(cz::Method::Ppmd7, text, e);
    REQUIRE_OK(packed);
    const auto props = cz::encoder_properties(cz::Method::Ppmd7, e, text.size());
    REQUIRE_OK(props);
    REQUIRE_EQ(props->size(), 5u);
    cz::DecodeOptions d;
    d.properties = *props;
    d.output_size = text.size();
    const auto plain = cz::decompress(cz::Method::Ppmd7, *packed, d);
    REQUIRE_OK(plain);
    CHECK(*plain == text);
    CHECK_LE(packed->size(), previous + previous / 50);
    previous = packed->size();
  }
  CHECK_LT(previous, text.size() / 8);
  const auto round = [](ByteView input, int level) {
    cz::EncodeOptions e;
    e.level = level;
    const auto packed = cz::compress(cz::Method::Ppmd7, input, e);
    REQUIRE_OK(packed);
    cz::DecodeOptions d;
    const Bytes props = *cz::encoder_properties(cz::Method::Ppmd7, e, input.size());
    d.properties = props;
    d.output_size = input.size();
    const auto plain = cz::decompress(cz::Method::Ppmd7, *packed, d);
    REQUIRE_OK(plain);
    CHECK(*plain == Bytes(input.begin(), input.end()));
    return packed->size();
  };
  CHECK_LE(round(test::random_bytes(100000, 7), 6), 100000u + 100000u / 20);
  round({}, 6);
  round(as_bytes("x"), 0);
  round(Bytes(300000, std::byte{'a'}), 9);
  Bytes big = tc::text_corpus(600000);
  append(big, test::random_bytes(300000, 8));
  append(big, tc::text_corpus(200000));
  round(big, 4);  // ~1 MB, 2 MiB model: restarts along the way
}

TEST_CASE("archive/ppmd: properties, limits, options and damaged input") {
  const Bytes packed = seven_zip_stream("corpus_o6_64k.7z");
  cz::DecodeOptions d;
  d.output_size = 60000;
  for (const Bytes& bad : {Bytes{}, Bytes(4), props7(1, 1u << 16), props7(65, 1u << 16), props7(6, 1000),
                           props7(6, 0xFFFFFFFFu)}) {
    d.properties = bad;
    CHECK_STATUS(cz::decompress(cz::Method::Ppmd7, packed, d), Status::Decode);
  }
  // An absurd model: fine for a short output (small first run), refused for a long one.
  const Bytes absurd = props7(6, 0xFFFFFF00u);
  d.properties = absurd;
  d.output_size = 100;
  const auto prefix = cz::decompress(cz::Method::Ppmd7, seven_zip_stream("corpus_o6_64m.7z"), d);
  REQUIRE_OK(prefix);  // the 64 MiB stream decodes identically in any heap for 100 symbols
  CHECK(*prefix == tc::text_corpus(100));
  d.output_size = std::uint64_t{1} << 30;
  CHECK_STATUS(cz::decompress(cz::Method::Ppmd7, packed, d), Status::Limit);

  const Bytes props = props7(6, 1u << 16);
  d.properties = props;
  d.output_size = 60000;
  d.max_output = 59999;
  CHECK_STATUS(cz::decompress(cz::Method::Ppmd7, packed, d), Status::Limit);
  d.max_output = cz::unlimited;
  d.stop_after = 1234;
  const auto head = cz::decompress(cz::Method::Ppmd7, packed, d);
  REQUIRE_OK(head);
  CHECK(*head == tc::text_corpus(1234));
  d.stop_after = 0;
  // Truncated data and a size beyond the stream are errors.
  CHECK_STATUS(cz::decompress(cz::Method::Ppmd7, ByteView(packed).first(packed.size() - 10), d), Status::Decode);
  d.output_size = 60100;
  CHECK_STATUS(cz::decompress(cz::Method::Ppmd7, packed, d), Status::Decode);
  d.output_size = 60000;
  // Cancellation.
  d.progress = Progress([](double, std::string_view) { return true; });
  CHECK_STATUS(cz::decompress(cz::Method::Ppmd7, packed, d), Status::Cancelled);
  d.progress = {};
  // Hostile input never crashes.
  cz::DecodeOptions small;
  small.properties = props;
  small.output_size = 20000;
  tc::hostile(cz::Method::Ppmd7, ByteView(packed).first(3000), small);
  const ZipEntry e = zip_stream("ppmd", "corpus_o16_cutoff.zip");
  cz::DecodeOptions d8;
  d8.output_size = 20000;
  tc::hostile(cz::Method::Ppmd8, ByteView(e.data).first(3000), d8);
  // var.I: parameter word checks, limits, prefixes.
  CHECK_STATUS(cz::decompress(cz::Method::Ppmd8, as_bytes("\x01")), Status::Decode);
  Bytes order1 = e.data;
  order1[0] = std::byte{0xF0};  // order 1
  CHECK_STATUS(cz::decompress(cz::Method::Ppmd8, order1, d8), Status::Decode);
  Bytes restore2 = e.data;
  restore2[1] = static_cast<std::byte>((std::to_integer<unsigned>(restore2[1]) & 0x0Fu) | 0x20u);
  CHECK_STATUS(cz::decompress(cz::Method::Ppmd8, restore2, d8), Status::Decode);
  d8.max_output = 100;
  d8.output_size = 101;
  CHECK_STATUS(cz::decompress(cz::Method::Ppmd8, e.data, d8), Status::Limit);
  d8.max_output = cz::unlimited;
  d8.output_size = 100000;
  d8.stop_after = 777;
  const auto head8 = cz::decompress(cz::Method::Ppmd8, e.data, d8);
  REQUIRE_OK(head8);
  CHECK(*head8 == tc::text_corpus(777));
  const ZipEntry small_zip = zip_stream("zip", "ppmd.zip");
  for (std::size_t n = 0; n < 200; ++n) {
    cz::DecodeOptions sized;
    sized.output_size = small_zip.size;
    CHECK_FALSE(cz::decompress(cz::Method::Ppmd8, ByteView(small_zip.data).first(n), sized).has_value());
  }
}

TEST_CASE("archive/ppmd: encoder determinism and streaming adapter") {
  const Bytes text = tc::text_corpus(150000);
  cz::EncodeOptions e;
  e.level = 6;
  const auto one = cz::compress(cz::Method::Ppmd7, text, e);
  REQUIRE_OK(one);
  auto encoder = cz::make_encoder(cz::Method::Ppmd7, e);
  REQUIRE_OK(encoder);
  Bytes streamed;
  for (std::size_t at = 0; at < text.size(); at += 4097) {
    REQUIRE_OK((*encoder)->encode(ByteView(text).subspan(at, std::min<std::size_t>(4097, text.size() - at)), streamed));
  }
  REQUIRE_OK((*encoder)->finish(streamed));
  CHECK(streamed == *one);
  e.progress = Progress([](double, std::string_view) { return true; });
  CHECK_STATUS(cz::compress(cz::Method::Ppmd7, text, e), Status::Cancelled);
  CHECK(cz::can_encode(cz::Method::Ppmd7));
  CHECK_FALSE(cz::can_encode(cz::Method::Ppmd8));
}

