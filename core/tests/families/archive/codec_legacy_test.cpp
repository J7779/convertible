// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// Legacy decode-only codecs against data written by independent tools:
// Deflate64 (7-Zip), MSZIP (gcab), LZX (chmcmd, wimlib), XPRESS Huffman
// (wimlib), LHA (jLHA, checked with lhasa), ARJ (ARJ32 3.10) and LZO1X (lzop).
#include "families/archive/codec_legacy_util.hpp"

using namespace cvt;
namespace cz = cvt::compression;
namespace tc = cvt::test::codec;
namespace tl = cvt::test::legacy;

namespace {

Bytes decode_ok(cz::Method m, ByteView data, const cz::DecodeOptions& options) {
  auto out = cz::decompress(m, data, options);
  REQUIRE_OK(out);
  return std::move(*out);
}

cz::DecodeOptions sized(std::uint64_t size, int window_bits = 0, std::uint32_t flags = 0) {
  cz::DecodeOptions d;
  d.output_size = size;
  d.window_bits = window_bits;
  d.flags = flags;
  return d;
}

/// Decode a WIM chunk list (LE32 uncompressed size, LE32 compressed size, data).
Bytes decode_chunks(cz::Method m, ByteView list) {
  Bytes out;
  std::size_t at = 0;
  while (at < list.size()) {
    const std::uint32_t usize = tl::le32(list, at);
    const std::uint32_t csize = tl::le32(list, at + 4);
    const ByteView chunk = list.subspan(at + 8, csize);
    at += 8 + csize;
    if (csize == usize) {
      append(out, chunk);  // stored chunk
      continue;
    }
    append(out, decode_ok(m, chunk, sized(usize)));
  }
  return out;
}

struct ZipEntry {
  std::string name;
  int method = 0;
  ByteView data;
  std::uint32_t size = 0;
};

std::vector<ZipEntry> zip_entries(ByteView zip) {
  std::vector<ZipEntry> out;
  std::size_t at = 0;
  while (at + 30 <= zip.size() && tl::le32(zip, at) == 0x04034B50u) {
    ZipEntry e;
    e.method = static_cast<int>(tl::le16(zip, at + 8));
    const std::uint32_t csize = tl::le32(zip, at + 18);
    e.size = tl::le32(zip, at + 22);
    const std::uint32_t name_len = tl::le16(zip, at + 26);
    const std::uint32_t extra_len = tl::le16(zip, at + 28);
    e.name = std::string(as_chars(zip.subspan(at + 30, name_len)));
    e.data = zip.subspan(at + 30 + name_len + extra_len, csize);
    out.push_back(e);
    at += 30 + name_len + extra_len + csize;
  }
  return out;
}

Bytes tree_file(std::string_view name) {
  for (const test::tree::File& f : test::tree::files()) {
    if (f.path == name) return f.bytes;
  }
  return {};
}

}  // namespace

TEST_CASE("archive/deflate64: entries written by 7-Zip decode exactly") {
  const Bytes zip = test::fixture("archive/zip/deflate64.zip");
  int checked = 0;
  for (const ZipEntry& e : zip_entries(zip)) {
    if (e.method != 9) continue;
    INFO(e.name);
    const Bytes plain = decode_ok(cz::Method::Deflate64, e.data, sized(e.size));
    CHECK(plain == tree_file(e.name));
    // Without a declared size the stream ends at its final block.
    CHECK(decode_ok(cz::Method::Deflate64, e.data, {}) == plain);
    cz::DecodeOptions prefix;
    prefix.stop_after = 100;
    CHECK(decode_ok(cz::Method::Deflate64, e.data, prefix) == Bytes(plain.begin(), plain.begin() + 100));
    cz::DecodeOptions small;
    small.max_output = 1000;
    CHECK_STATUS(cz::decompress(cz::Method::Deflate64, e.data, small), Status::Limit);
    tc::hostile(cz::Method::Deflate64, e.data);
    ++checked;
  }
  CHECK_GE(checked, 1);
}

TEST_CASE("archive/mszip: a CAB folder written by gcab decodes exactly") {
  const Bytes cab = test::fixture("archive/cab/mszip.cab");
  REQUIRE_GE(cab.size(), std::size_t{60});
  REQUIRE(as_chars(ByteView(cab).first(4)) == "MSCF");
  const std::uint32_t files_at = tl::le32(cab, 16);
  const std::uint32_t folders = tl::le16(cab, 26);
  const std::uint32_t files = tl::le16(cab, 28);
  const std::uint32_t flags = tl::le16(cab, 30);
  REQUIRE_EQ(folders, 1u);
  REQUIRE_EQ(flags & 4u, 0u);
  const std::uint32_t data_at = tl::le32(cab, 36);
  const std::uint32_t blocks = tl::le16(cab, 40);
  REQUIRE_EQ(tl::le16(cab, 42) & 0x0Fu, 1u);  // MSZIP
  Bytes payload;
  std::vector<std::uint32_t> sizes;
  std::size_t at = data_at;
  for (std::uint32_t b = 0; b < blocks; ++b) {
    const std::uint32_t csize = tl::le16(cab, at + 4);
    sizes.push_back(tl::le16(cab, at + 6));
    append(payload, ByteView(cab).subspan(at + 8, csize));
    at += 8 + csize;
  }
  std::uint64_t total = 0;
  for (const std::uint32_t s : sizes) total += s;
  cz::DecodeOptions d = sized(total);
  d.block_sizes = sizes;
  const Bytes folder = decode_ok(cz::Method::Mszip, payload, d);
  REQUIRE_EQ(folder.size(), total);
  // Compare every file of the folder with the tree.
  at = files_at;
  for (std::uint32_t f = 0; f < files; ++f) {
    const std::uint32_t size = tl::le32(cab, at);
    const std::uint32_t offset = tl::le32(cab, at + 4);
    std::size_t end = at + 16;
    while (cab[end] != std::byte{0}) ++end;
    const std::string name(as_chars(ByteView(cab).subspan(at + 16, end - at - 16)));
    INFO(name);
    CHECK(Bytes(folder.begin() + offset, folder.begin() + offset + size) == tree_file(name));
    at = end + 1;
  }
  CHECK(decode_ok(cz::Method::Mszip, payload, {}) == folder);  // sizes are optional
  cz::DecodeOptions wrong = d;
  std::vector<std::uint32_t> bad = sizes;
  bad[0] += 1;
  wrong.block_sizes = bad;
  wrong.output_size = total + 1;
  CHECK_STATUS(cz::decompress(cz::Method::Mszip, payload, wrong), Status::Decode);
  tc::hostile(cz::Method::Mszip, payload, d);
}

TEST_CASE("archive/lzx: WIM chunks written by wimlib (LZX maximum and XPRESS fast)") {
  CHECK(decode_chunks(cz::Method::LzxWim, tl::fixture("text40k_maximum.chunks")) == tl::text40k());
  CHECK(decode_chunks(cz::Method::LzxWim, tl::fixture("x86_maximum.chunks")) == tl::x86_like(45000));
  CHECK(decode_chunks(cz::Method::XpressHuffman, tl::fixture("text40k_fast.chunks")) == tl::text40k());
  CHECK(decode_chunks(cz::Method::XpressHuffman, tl::fixture("x86_fast.chunks")) == tl::x86_like(45000));
  const Bytes list = tl::fixture("x86_maximum.chunks");
  const ByteView first = ByteView(list).subspan(8, tl::le32(list, 4));
  tc::hostile(cz::Method::LzxWim, first, sized(32768));
  const Bytes xlist = tl::fixture("text40k_fast.chunks");
  tc::hostile(cz::Method::XpressHuffman, ByteView(xlist).subspan(8, tl::le32(xlist, 4)), sized(32768));
}

TEST_CASE("archive/lzx: CHM content written by chmcmd, with reset intervals") {
  const Bytes content = tl::fixture("chm_content.lzx");
  constexpr std::uint64_t total = 106888;
  const std::vector<std::uint32_t> intervals = {65536, 41352};
  cz::DecodeOptions d = sized(total, 16);
  d.block_sizes = intervals;
  const Bytes all = decode_ok(cz::Method::Lzx, content, d);
  REQUIRE_EQ(all.size(), total);
  Bytes html = to_bytes("<html><body><pre>\n");
  append(html, tc::text_corpus(100000));
  append(html, "\n</pre></body></html>\n");
  CHECK(Bytes(all.begin() + 2751, all.begin() + 2751 + 100040) == html);
  // The second reset interval decodes on its own from its reset-table offset.
  const Bytes second = decode_ok(cz::Method::Lzx, ByteView(content).subspan(12774), sized(41352, 16));
  CHECK(second == Bytes(all.begin() + 65536, all.end()));
  // The reset flag with interval-sized blocks of 32 KiB granularity.
  cz::DecodeOptions flagged = d;
  flagged.flags = 1;
  CHECK(decode_ok(cz::Method::Lzx, content, flagged) == all);
  cz::DecodeOptions prefix = d;
  prefix.stop_after = 40000;
  CHECK(decode_ok(cz::Method::Lzx, content, prefix) == Bytes(all.begin(), all.begin() + 40000));
  CHECK_STATUS(cz::decompress(cz::Method::Lzx, content, sized(total, 14)), Status::InvalidArgument);
  cz::DecodeOptions limited = d;
  limited.max_output = 1000;
  CHECK_STATUS(cz::decompress(cz::Method::Lzx, content, limited), Status::Limit);
  tc::hostile(cz::Method::Lzx, ByteView(content).first(12774), sized(65536, 16));
}

TEST_CASE("archive/lh: -lh1- and -lh4- .. -lh7- streams written by jLHA") {
  const Bytes mixed = tc::mixed();
  CHECK(decode_ok(cz::Method::Lh, tl::fixture("mixed.lh1"), sized(mixed.size(), 12, 1)) == mixed);
  CHECK(decode_ok(cz::Method::Lh, tl::fixture("mixed.lh4"), sized(mixed.size(), 12)) == mixed);
  CHECK(decode_ok(cz::Method::Lh, tl::fixture("mixed.lh5"), sized(mixed.size(), 13)) == mixed);
  CHECK(decode_ok(cz::Method::Lh, tl::fixture("mixed.lh6"), sized(mixed.size(), 15)) == mixed);
  CHECK(decode_ok(cz::Method::Lh, tl::fixture("mixed.lh7"), sized(mixed.size(), 16)) == mixed);
  const Bytes text = tl::text40k();
  CHECK(decode_ok(cz::Method::Lh, tl::fixture("text40k.lh1"), sized(text.size(), 12, 1)) == text);
  CHECK(decode_ok(cz::Method::Lh, tl::fixture("text40k.lh5"), sized(text.size(), 13)) == text);
  CHECK(decode_ok(cz::Method::Lh, tl::fixture("text40k.lh7"), sized(text.size(), 16)) == text);
  cz::DecodeOptions prefix = sized(text.size(), 13);
  prefix.stop_after = 5000;
  CHECK(decode_ok(cz::Method::Lh, tl::fixture("text40k.lh5"), prefix) == Bytes(text.begin(), text.begin() + 5000));
  CHECK_STATUS(cz::decompress(cz::Method::Lh, tl::fixture("text40k.lh5"), sized(text.size() + 10, 13)),
               Status::Decode);
  CHECK_STATUS(cz::decompress(cz::Method::Lh, tl::fixture("text40k.lh5"), sized(text.size(), 14)),
               Status::InvalidArgument);
  tc::hostile(cz::Method::Lh, tl::fixture("mixed.lh5"), sized(mixed.size(), 13));
  tc::hostile(cz::Method::Lh, tl::fixture("mixed.lh1"), sized(mixed.size(), 12, 1));
}

TEST_CASE("archive/lzs: -lzs- and -lz5- streams written by jLHA") {
  const Bytes mixed = tc::mixed();
  CHECK(decode_ok(cz::Method::Lzs, tl::fixture("mixed.lzs"), sized(mixed.size(), 11)) == mixed);
  CHECK(decode_ok(cz::Method::Lzs, tl::fixture("mixed.lz5"), sized(mixed.size(), 12)) == mixed);
  tc::hostile(cz::Method::Lzs, tl::fixture("mixed.lzs"), sized(mixed.size(), 11));
  tc::hostile(cz::Method::Lzs, tl::fixture("mixed.lz5"), sized(mixed.size(), 12));
}

TEST_CASE("archive/arj: methods 1..4 written by ARJ32 3.10") {
  const Bytes mixed = tc::mixed();
  for (std::uint32_t m = 1; m <= 4; ++m) {
    INFO("method ", m);
    CHECK(decode_ok(cz::Method::Arj, tl::fixture("mixed.arj" + std::to_string(m)), sized(mixed.size(), 0, m)) ==
          mixed);
  }
  const Bytes text = tl::text40k();
  CHECK(decode_ok(cz::Method::Arj, tl::fixture("text40k.arj1"), sized(text.size(), 0, 1)) == text);
  CHECK(decode_ok(cz::Method::Arj, tl::fixture("text40k.arj4"), sized(text.size(), 0, 4)) == text);
  CHECK_STATUS(cz::decompress(cz::Method::Arj, tl::fixture("mixed.arj1"), sized(mixed.size(), 0, 7)),
               Status::InvalidArgument);
  tc::hostile(cz::Method::Arj, tl::fixture("mixed.arj1"), sized(mixed.size(), 0, 1));
  tc::hostile(cz::Method::Arj, tl::fixture("mixed.arj4"), sized(mixed.size(), 0, 4));
}

TEST_CASE("archive/lzo1x: blocks written by lzop -1 (LZO1X-1) and -9 (LZO1X-999)") {
  const Bytes mixed = tc::mixed();
  CHECK(decode_ok(cz::Method::Lzo1x, tl::fixture("mixed_1.lzo1x"), sized(mixed.size())) == mixed);
  CHECK(decode_ok(cz::Method::Lzo1x, tl::fixture("mixed_9.lzo1x"), sized(mixed.size())) == mixed);
  const Bytes text = tl::text40k();
  CHECK(decode_ok(cz::Method::Lzo1x, tl::fixture("text40k_9.lzo1x"), sized(text.size())) == text);
  // The end marker makes the size optional; max_output bounds it.
  CHECK(decode_ok(cz::Method::Lzo1x, tl::fixture("text40k_9.lzo1x"), {}) == text);
  cz::DecodeOptions limited;
  limited.max_output = 1000;
  CHECK_STATUS(cz::decompress(cz::Method::Lzo1x, tl::fixture("text40k_9.lzo1x"), limited), Status::Limit);
  CHECK_STATUS(cz::decompress(cz::Method::Lzo1x, tl::fixture("text40k_9.lzo1x"), sized(text.size() + 1)),
               Status::Decode);
  tc::hostile(cz::Method::Lzo1x, tl::fixture("mixed_1.lzo1x"));
  tc::hostile(cz::Method::Lzo1x, tl::fixture("text40k_9.lzo1x"), sized(text.size()));
}
