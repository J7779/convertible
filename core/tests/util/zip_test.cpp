// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <optional>
#include <string>
#include <string_view>

#include "ir/archive.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"
#include "util/hex.hpp"
#include "util/zip.hpp"

using namespace cvt;
using namespace cvt::util;
using test::str;

namespace {

// Written by an independent ZIP implementation: "dir/hello.txt" (deflated,
// 2020-01-02 03:04:06, mode 0644) and "stored.bin" (stored, 1999-12-31 23:59:58).
constexpr std::string_view reference_zip =
    "504b0304140000000800831822500088590b0b000000180000000d0000006469722f68656c6c6f2e747874cb48cdc9c957c84027b900"
    "504b03041400000000007dbf9f279f68aa8808000000080000000a00000073746f7265642e62696e0001020304050607"
    "504b01021403140000000800831822500088590b0b000000180000000d0000000000000000000000a401000000006469722f68656c6c"
    "6f2e747874"
    "504b010214031400000000007dbf9f279f68aa8808000000080000000a000000000000000000000080013600000073746f7265642e62"
    "696e"
    "504b0506000000000200020073000000660000000000";

// The content of the fixtures in core/tests/data/util (see SOURCES.md there).
constexpr std::string_view lorem =
    "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et "
    "dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea "
    "commodo consequat. Duis aute irure dolor in reprehenderit in voluptate velit esse cillum dolore eu fugiat "
    "nulla pariatur. Excepteur sint occaecat cupidatat non proident, sunt in culpa qui officia deserunt mollit "
    "anim id est laborum.\n";

std::string lorem_times(std::size_t count) {
  std::string out;
  for (std::size_t i = 0; i < count; ++i) out += lorem;
  return out;
}

Bytes pattern(std::size_t size) {
  Bytes out(size);
  for (std::size_t i = 0; i < size; ++i) out[i] = static_cast<std::byte>((i % 251) ^ ((i / 251) & 15));
  return out;
}

std::uint32_t u16_at(const Bytes& data, std::size_t offset) {
  return std::to_integer<std::uint32_t>(data[offset]) | (std::to_integer<std::uint32_t>(data[offset + 1]) << 8);
}
std::uint32_t u32_at(const Bytes& data, std::size_t offset) { return u16_at(data, offset) | (u16_at(data, offset + 2) << 16); }

/// Replace every occurrence of `from` by `to` (same length), in the local
/// headers and the central directory alike.
void replace_all(Bytes& archive, std::string_view from, std::string_view to) {
  REQUIRE(from.size() == to.size());
  std::size_t replaced = 0;
  for (std::size_t at = 0; at + from.size() <= archive.size(); ++at) {
    if (as_chars(ByteView(archive).subspan(at, from.size())) == from) {
      std::copy(to.begin(), to.end(), reinterpret_cast<char*>(archive.data() + at));
      ++replaced;
    }
  }
  REQUIRE(replaced >= 2);
}

constexpr std::int64_t fixture_time = 981173106;  // 2001-02-03T04:05:06Z

Bytes simple_archive() {
  ZipWriter writer;
  REQUIRE(writer.add("mimetype", "application/test", {.level = 0}));
  REQUIRE(writer.add("docs/readme.txt", std::string(2000, 'a'), {.level = 9, .mtime = 1577934246, .unix_mode = 0600}));
  REQUIRE(writer.add_directory("empty"));
  REQUIRE(writer.add("na\xC3\xAFve/\xE2\x82\xAC.bin", ByteView(test::random_bytes(300, 2))));
  return *writer.finish();
}

}  // namespace

TEST_CASE("zip: DOS time conversion is pure UTC arithmetic") {
  const DosDateTime epoch = unix_to_dos_time(315532800);  // 1980-01-01T00:00:00Z
  CHECK(epoch.date == 0x0021);
  CHECK(epoch.time == 0);
  CHECK(dos_to_unix_time(0x0021, 0) == 315532800);
  const DosDateTime stamp = unix_to_dos_time(1577934246);  // 2020-01-02T03:04:06Z
  CHECK(stamp.date == ((40u << 9) | (1u << 5) | 2u));
  CHECK(stamp.time == ((3u << 11) | (4u << 5) | 3u));
  CHECK(dos_to_unix_time(stamp.date, stamp.time) == 1577934246);
  CHECK(dos_to_unix_time(unix_to_dos_time(1709210096).date, unix_to_dos_time(1709210096).time) == 1709210096);  // leap day
  // Out-of-range times clamp to what the format can hold.
  CHECK(dos_to_unix_time(unix_to_dos_time(0).date, unix_to_dos_time(0).time) == 315532800);
  CHECK(dos_to_unix_time(unix_to_dos_time(-5).date, unix_to_dos_time(-5).time) == 315532800);
  CHECK(dos_to_unix_time(unix_to_dos_time(9999999999).date, unix_to_dos_time(9999999999).time) == 4354819198);
  // Nonsense fields are clamped, not trusted.
  CHECK(dos_to_unix_time(0, 0xFFFF) > 0);
}

TEST_CASE("zip: reads an archive written by another implementation") {
  const Bytes archive = *hex_decode(reference_zip);
  auto zip = ZipReader::open(archive);
  REQUIRE(zip);
  REQUIRE(zip->size() == 2);
  const ZipEntry& first = zip->entries()[0];
  CHECK(first.name == "dir/hello.txt");
  CHECK(first.method == 8);
  CHECK(first.uncompressed_size == 24);
  CHECK(first.mtime == 1577934246);
  CHECK((first.unix_mode & 0777u) == 0644u);
  CHECK_FALSE(first.is_directory);
  CHECK(str(*zip->read(std::size_t{0})) == "hello hello hello hello\n");
  const ZipEntry& second = zip->entries()[1];
  CHECK(second.name == "stored.bin");
  CHECK(second.method == 0);
  CHECK(second.mtime == 946684798);
  CHECK(zip->read("stored.bin")->size() == 8);
  CHECK(zip->decompressed_total() == 32);
  CHECK(zip->find("stored.bin") == std::size_t{1});
  CHECK_FALSE(zip->contains("missing"));
  CHECK(zip->read("missing").error().code == Status::Decode);
  CHECK(zip->read(std::size_t{7}).error().code == Status::InvalidArgument);
}

TEST_CASE("zip: writer output round-trips through the reader") {
  const Bytes archive = simple_archive();
  CHECK(archive == simple_archive());  // deterministic
  auto zip = ZipReader::open(archive);
  REQUIRE(zip);
  REQUIRE(zip->size() == 4);

  const auto& entries = zip->entries();
  CHECK(entries[0].name == "mimetype");
  CHECK(entries[0].method == 0);
  CHECK(entries[0].local_header_offset == 0);
  // EPUB/ODF readers expect the stored mimetype payload at a fixed offset.
  CHECK(as_chars(ByteView(archive).subspan(30, 8)) == "mimetype");
  CHECK(as_chars(ByteView(archive).subspan(38, 16)) == "application/test");
  CHECK(entries[0].mtime == 315532800);

  CHECK(entries[1].name == "docs/readme.txt");
  CHECK(entries[1].method == 8);
  CHECK(entries[1].compressed_size < 100);
  CHECK(entries[1].mtime == 1577934246);
  CHECK((entries[1].unix_mode & 0777u) == 0600u);
  CHECK(str(*zip->read("docs/readme.txt")) == std::string(2000, 'a'));

  CHECK(entries[2].name == "empty/");
  CHECK(entries[2].is_directory);
  CHECK(zip->read("empty/")->empty());

  CHECK(entries[3].name == "na\xC3\xAFve/\xE2\x82\xAC.bin");
  CHECK(entries[3].method == 0);  // random data does not shrink, so it is stored
  CHECK(*zip->read(std::size_t{3}) == test::random_bytes(300, 2));
}

TEST_CASE("zip: an empty archive is valid") {
  ZipWriter writer;
  const Bytes archive = *writer.finish();
  CHECK(archive.size() == 22);
  auto zip = ZipReader::open(archive);
  REQUIRE(zip);
  CHECK(zip->size() == 0);
}

TEST_CASE("zip: unsafe or malformed entry names are refused by the writer") {
  ZipWriter writer;
  for (const std::string_view bad : {"", "/absolute", "../escape", "a/../b", "back\\slash", "C:drive", "dir/", "bad\xFFutf8"}) {
    CHECK_MESSAGE(writer.add(bad, "x").error().code == Status::InvalidArgument, "name: ", bad);
  }
  CHECK(writer.add(std::string(70000, 'n'), "x").error().code == Status::InvalidArgument);
  CHECK(writer.add_directory("../up").error().code == Status::InvalidArgument);
  CHECK(writer.entry_count() == 0);
  CHECK(writer.add("ok/..hidden", "x"));  // dots inside a name are fine
}

TEST_CASE("zip: more than 65535 entries switches to ZIP64 and still round-trips") {
  ZipWriter writer;
  constexpr std::size_t count = 66000;
  for (std::size_t i = 0; i < count; ++i) REQUIRE(writer.add("f" + std::to_string(i), std::to_string(i), {.level = 0}));
  const Bytes archive = *writer.finish();
  auto zip = ZipReader::open(archive);
  REQUIRE(zip);
  REQUIRE(zip->size() == count);
  CHECK(zip->entries()[65999].name == "f65999");
  CHECK(str(*zip->read(std::size_t{65999})) == "65999");
  CHECK(str(*zip->read("f12345")) == "12345");

  Limits tight;
  tight.max_archive_entries = 65999;
  CHECK(ZipReader::open(archive, tight).error().code == Status::Limit);
}

TEST_CASE("zip: decompression is bounded by the limits") {
  ZipWriter writer;
  REQUIRE(writer.add("a", std::string(60000, 'a')));
  REQUIRE(writer.add("b", std::string(60000, 'b')));
  const Bytes archive = *writer.finish();
  CHECK(archive.size() < 2000);  // a (very mild) bomb

  Limits limits;
  limits.max_decompressed_bytes = 100000;
  auto zip = ZipReader::open(archive, limits);
  REQUIRE(zip);
  CHECK(zip->read("a").has_value());
  CHECK(zip->read("b").error().code == Status::Limit);  // the running total would exceed the cap
  CHECK(zip->decompressed_total() == 60000);

  // An entry that claims to be small but inflates to more is corrupt.
  Bytes lying = archive;
  auto reader = ZipReader::open(archive);
  REQUIRE(reader);
  // Patch the central directory's uncompressed size of "a" (first central record).
  const std::string_view text = as_chars(ByteView(lying));
  const std::size_t central = text.find("PK\x01\x02");
  REQUIRE(central != std::string_view::npos);
  lying[central + 24] = std::byte{10};
  lying[central + 25] = std::byte{0};
  lying[central + 26] = std::byte{0};
  lying[central + 27] = std::byte{0};
  auto liar = ZipReader::open(lying);
  REQUIRE(liar);
  CHECK(liar->read("a").error().code == Status::Decode);
}

TEST_CASE("zip: archives with prepended data and comments are found") {
  const Bytes archive = simple_archive();
  Bytes prefixed = to_bytes("#!/bin/sh\nexit 0\n");
  append(prefixed, ByteView(archive));
  auto zip = ZipReader::open(prefixed);
  REQUIRE(zip);
  CHECK(str(*zip->read("mimetype")) == "application/test");

  Bytes commented = archive;
  commented[commented.size() - 2] = std::byte{5};  // comment length
  append(commented, "hello");
  auto with_comment = ZipReader::open(commented);
  REQUIRE(with_comment);
  CHECK(with_comment->size() == 4);
}

TEST_CASE("zip: legacy names, encryption and unknown methods") {
  Bytes archive = *hex_decode(reference_zip);
  const std::string_view text = as_chars(ByteView(archive));
  const std::size_t central = text.find("PK\x01\x02");
  REQUIRE(central != std::string_view::npos);

  SUBCASE("CP437 names are converted to UTF-8") {
    // Rename "dir/hello.txt" to "dir/h\x82llo.txt" (0x82 is e-acute in CP437) in the central record.
    archive[central + 46 + 5] = std::byte{0x82};
    auto zip = ZipReader::open(archive);
    REQUIRE(zip);
    CHECK(zip->entries()[0].name == "dir/h\xC3\xA9llo.txt");
  }
  SUBCASE("backslashes become forward slashes") {
    archive[central + 46 + 3] = std::byte{'\\'};
    auto zip = ZipReader::open(archive);
    REQUIRE(zip);
    CHECK(zip->entries()[0].name == "dir/hello.txt");
  }
  SUBCASE("encrypted entries are reported, not decoded") {
    archive[central + 8] |= std::byte{0x01};
    auto zip = ZipReader::open(archive);
    REQUIRE(zip);
    CHECK(zip->entries()[0].encrypted);
    CHECK(zip->read(std::size_t{0}).error().code == Status::Decode);
    CHECK(zip->read(std::size_t{1}).has_value());
  }
  SUBCASE("unsupported compression methods are reported") {
    archive[central + 10] = std::byte{14};  // LZMA
    auto zip = ZipReader::open(archive);
    REQUIRE(zip);
    CHECK(zip->read(std::size_t{0}).error().code == Status::Decode);
  }
  SUBCASE("a wrong checksum is detected") {
    archive[central + 16] ^= std::byte{0xFF};
    auto zip = ZipReader::open(archive);
    REQUIRE(zip);
    CHECK(zip->read(std::size_t{0}).error().code == Status::Decode);
  }
}

TEST_CASE("zip: garbage and truncated archives never crash") {
  CHECK(ZipReader::open({}).error().code == Status::Decode);
  CHECK(ZipReader::open(as_bytes("definitely not a zip file, just some text")).error().code == Status::Decode);
  const Bytes archive = simple_archive();
  for (std::size_t length = 0; length < archive.size(); length += 13) {
    auto zip = ZipReader::open(ByteView(archive).first(length));
    if (zip) {
      for (std::size_t i = 0; i < zip->size(); ++i) (void)zip->read(i);
    }
  }
  for (std::uint32_t seed = 1; seed <= 300; ++seed) {
    Bytes mutated = archive;
    const Bytes noise = test::random_bytes(6, seed);
    for (std::size_t k = 0; k < 3; ++k) {
      // Concentrate damage on the directory structures at the end.
      const std::size_t span = std::min<std::size_t>(mutated.size(), 400);
      const std::size_t position = mutated.size() - 1 - (std::to_integer<std::size_t>(noise[k * 2]) * 7 + seed * 31) % span;
      mutated[position] = noise[k * 2 + 1];
    }
    auto zip = ZipReader::open(mutated);
    if (zip) {
      for (std::size_t i = 0; i < zip->size(); ++i) (void)zip->read(i);
    }
  }
  // A directory claiming 4 billion entries must not allocate for them.
  Bytes huge = *hex_decode("504b0506" "00000000" "ffffffff" "ffffffff" "00000000" "0000");
  CHECK_FALSE(ZipReader::open(huge));
}

// --------------------------------------------------------------------------- archives written by other tools

TEST_CASE("zip: reads an archive written by Info-ZIP with stored, deflated and directory entries") {
  const Bytes archive = test::fixture("util/zip_stored_deflated_dir.zip");
  auto zip = ZipReader::open(archive);
  REQUIRE(zip);
  REQUIRE(zip->size() == 3);

  const ZipEntry& stored = zip->entries()[0];
  CHECK(stored.name == "pattern.bin");
  CHECK(stored.method == 0);
  CHECK(stored.uncompressed_size == 600);
  CHECK(stored.compressed_size == 600);
  CHECK_FALSE(stored.is_directory);
  CHECK(stored.mtime == fixture_time);
  CHECK(stored.unix_mode == 0100644u);
  CHECK(stored.safe_path() == "pattern.bin");
  CHECK(*zip->read("pattern.bin") == pattern(600));

  const ZipEntry& directory = zip->entries()[1];
  CHECK(directory.name == "dir/");
  CHECK(directory.is_directory);
  CHECK(directory.unix_mode == 0040755u);
  CHECK(directory.mtime == fixture_time);
  CHECK(directory.safe_path() == "dir");
  CHECK(zip->read("dir/")->empty());

  const ZipEntry& deflated = zip->entries()[2];
  CHECK(deflated.name == "dir/lorem.txt");
  CHECK(deflated.method == 8);
  CHECK(deflated.uncompressed_size == 2 * lorem.size());
  CHECK(deflated.compressed_size < deflated.uncompressed_size);
  CHECK(deflated.mtime == fixture_time);
  CHECK((deflated.unix_mode & 0777u) == 0644u);
  CHECK(deflated.safe_path() == "dir/lorem.txt");
  CHECK(str(*zip->read(std::size_t{2})) == lorem_times(2));
  CHECK(zip->decompressed_total() == 600 + 2 * lorem.size());
}

TEST_CASE("zip: entries written with data descriptors read through the central directory") {
  const Bytes archive = test::fixture("util/data_descriptors.zip");
  REQUIRE(archive.size() > 30);
  auto zip = ZipReader::open(archive);
  REQUIRE(zip);
  REQUIRE(zip->size() == 2);
  // The fixture really is a streamed archive: bit 3 set, sizes and CRC
  // absent from the local header, and a signed descriptor after the data.
  for (const ZipEntry& entry : zip->entries()) {
    const auto local = static_cast<std::size_t>(entry.local_header_offset);
    CHECK(u32_at(archive, local) == 0x04034b50u);
    CHECK((u16_at(archive, local + 6) & 0x0008u) != 0);
    CHECK(u32_at(archive, local + 14) == 0u);  // CRC-32
    CHECK(u32_at(archive, local + 18) == 0u);  // compressed size
    CHECK(u32_at(archive, local + 22) == 0u);  // uncompressed size
    const std::size_t data = local + 30 + u16_at(archive, local + 26) + u16_at(archive, local + 28);
    const auto descriptor = data + static_cast<std::size_t>(entry.compressed_size);
    CHECK(u32_at(archive, descriptor) == 0x08074b50u);
    CHECK(u32_at(archive, descriptor + 4) == entry.crc32);
  }
  const ZipEntry& deflated = zip->entries()[0];
  CHECK(deflated.name == "lorem.txt");
  CHECK(deflated.method == 8);
  CHECK(deflated.uncompressed_size == 3 * lorem.size());
  CHECK(deflated.mtime == fixture_time);
  CHECK(str(*zip->read("lorem.txt")) == lorem_times(3));
  const ZipEntry& stored = zip->entries()[1];
  CHECK(stored.name == "sub/pattern.bin");
  CHECK(stored.method == 0);
  CHECK(stored.mtime == fixture_time + 2);
  CHECK(*zip->read("sub/pattern.bin") == pattern(500));

  // A descriptor without the optional signature: drop the four bytes after
  // the first entry's data and shift everything behind it.
  const auto first_local = static_cast<std::size_t>(deflated.local_header_offset);
  const std::size_t first_data = first_local + 30 + u16_at(archive, first_local + 26) + u16_at(archive, first_local + 28);
  const auto signature = first_data + static_cast<std::size_t>(deflated.compressed_size);
  Bytes unsigned_descriptor = archive;
  unsigned_descriptor.erase(unsigned_descriptor.begin() + static_cast<std::ptrdiff_t>(signature),
                            unsigned_descriptor.begin() + static_cast<std::ptrdiff_t>(signature + 4));
  const std::string_view text = as_chars(ByteView(unsigned_descriptor));
  const std::size_t central = text.find("PK\x01\x02");
  const std::size_t second_central = text.find("PK\x01\x02", central + 4);
  const std::size_t end = text.rfind("PK\x05\x06");
  REQUIRE(second_central != std::string_view::npos);
  REQUIRE(end != std::string_view::npos);
  const auto patch = [&](std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
      unsigned_descriptor[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 0xFFu);
    }
  };
  patch(second_central + 42, u32_at(unsigned_descriptor, second_central + 42) - 4);  // local header offset of entry 2
  patch(end + 16, u32_at(unsigned_descriptor, end + 16) - 4);                        // central directory offset
  auto shifted = ZipReader::open(unsigned_descriptor);
  REQUIRE(shifted);
  CHECK(str(*shifted->read("lorem.txt")) == lorem_times(3));
  CHECK(*shifted->read("sub/pattern.bin") == pattern(500));
}

TEST_CASE("zip: entries carrying ZIP64 extra fields") {
  const Bytes archive = test::fixture("util/forced_zip64.zip");
  REQUIRE(archive.size() > 30);
  auto zip = ZipReader::open(archive);
  REQUIRE(zip);
  REQUIRE(zip->size() == 2);
  for (const ZipEntry& entry : zip->entries()) {
    const auto local = static_cast<std::size_t>(entry.local_header_offset);
    CHECK(u16_at(archive, local + 4) == 45u);           // version needed: 4.5
    CHECK(u32_at(archive, local + 18) == 0xFFFFFFFFu);  // sizes live in the ZIP64 extra field
    CHECK(u32_at(archive, local + 22) == 0xFFFFFFFFu);
    CHECK(u16_at(archive, local + 28) == 20u);
  }
  CHECK(zip->entries()[0].name == "lorem.txt");
  CHECK(zip->entries()[0].uncompressed_size == 3 * lorem.size());
  CHECK(zip->entries()[0].mtime == fixture_time);
  CHECK(str(*zip->read("lorem.txt")) == lorem_times(3));
  CHECK(zip->entries()[1].name == "pattern.bin");
  CHECK(zip->entries()[1].uncompressed_size == 500);
  CHECK(*zip->read("pattern.bin") == pattern(500));

  // Our own writer switches the end records to ZIP64 only when needed, and
  // the reader takes the counts from them (see the 66000-entry test above).
  CHECK(as_chars(ByteView(archive)).find("PK\x06\x06") == std::string_view::npos);
}

TEST_CASE("zip: fixture archives survive truncation and damage without crashing") {
  for (const std::string_view name :
       {"util/zip_stored_deflated_dir.zip", "util/data_descriptors.zip", "util/forced_zip64.zip"}) {
    const Bytes archive = test::fixture(name);
    REQUIRE(!archive.empty());
    for (std::size_t length = 0; length < archive.size(); length += 3) {
      auto zip = ZipReader::open(ByteView(archive).first(length));
      if (zip) {
        for (std::size_t i = 0; i < zip->size(); ++i) (void)zip->read(i);
      }
    }
    for (std::size_t position = 0; position < archive.size(); ++position) {
      Bytes damaged = archive;
      damaged[position] ^= std::byte{0x41};
      auto zip = ZipReader::open(damaged);
      if (!zip) continue;
      for (std::size_t i = 0; i < zip->size(); ++i) {
        (void)zip->entries()[i].safe_path();
        (void)zip->read(i);
      }
    }
  }
}

// --------------------------------------------------------------------------- entry names

TEST_CASE("zip: entry paths are sanitised for extraction") {
  using Path = std::optional<std::string>;
  const auto none = Path(std::nullopt);
  struct Case {
    std::string_view raw;
    Path expected;
  };
  using namespace std::string_view_literals;
  const std::vector<Case> cases = {
      // Ordinary names pass through; directories lose the trailing slash.
      {"file.txt", "file.txt"},
      {"dir/sub/file.txt", "dir/sub/file.txt"},
      {"dir/", "dir"},
      {"na\xC3\xAFve/\xE2\x82\xAC.bin", "na\xC3\xAFve/\xE2\x82\xAC.bin"},
      {"..hidden", "..hidden"},
      {"a..b/c...", "a..b/c..."},
      {"...", "..."},
      // Attempts to leave the extraction root.
      {"../../etc/passwd", none},
      {"..", none},
      {"../", none},
      {"a/../../b", none},
      {"a/../b", none},
      {"a/b/..", none},
      {"..\\..\\windows\\system32", none},
      {"a\\..\\b", none},
      {"/../x", none},
      {"C:..\\x", none},
      // Absolute paths and drive letters become relative.
      {"/abs", "abs"},
      {"///abs//path/", "abs/path"},
      {"C:\\x\\y", "x/y"},
      {"c:/x", "x"},
      {"C:x", "x"},
      {"\\\\server\\share\\f", "server/share/f"},
      {"C:", none},
      {"C:\\", none},
      // Drive-like prefixes are removed from every segment, however nested.
      {"C:D:x", "x"},
      {"/C:/x", "x"},
      {"dir/a:b", "dir/b"},
      {"a/C:/../b", none},
      // A colon that cannot be a drive prefix is an ordinary character.
      {"1:/x", "1:/x"},
      {"dir/ab:c", "dir/ab:c"},
      // "." and empty segments vanish.
      {"a/./b//c", "a/b/c"},
      {"./a", "a"},
      {"a/.", "a"},
      {".", none},
      {"./", none},
      {"/", none},
      {"//", none},
      {"", none},
      // Control characters and NULs are stripped before anything else.
      {"a\0b"sv, "ab"},
      {"a\x01\x1F\x7F" "b/c\n.txt", "ab/c.txt"},
      {"\0"sv, none},
      {"\x01\x02", none},
      {".\0."sv, none},
      {"a/.\x07./b", none},
      {"\t/\r/x", "x"},
      {"safe\0/../../evil"sv, none},
  };
  for (const Case& c : cases) {
    const Path got = sanitize_entry_path(c.raw);
    CHECK_MESSAGE(got == c.expected, "raw: ", hex_encode(as_bytes(c.raw)), " got: ", got.value_or("<none>"));
    // The archive IR forwards to the same implementation.
    CHECK(ir::sanitize_archive_path(c.raw) == got);
    if (got) {
      // A sanitised path has no hostile parts left, and is stable.
      if (got->find(':') == std::string::npos) CHECK(sanitize_entry_path(*got) == got);
      CHECK(got->front() != '/');
      CHECK(got->back() != '/');
      CHECK(got->find('\\') == std::string::npos);
      CHECK(("/" + *got + "/").find("/../") == std::string::npos);
      CHECK(("/" + *got + "/").find("/./") == std::string::npos);
      CHECK(("/" + *got + "/").find("//") == std::string::npos);
    }
  }
}

TEST_CASE("zip: hostile entry names in an archive are reported as stored and sanitised on request") {
  // The writer refuses such names, so plant them in a finished archive.
  ZipWriter writer;
  REQUIRE(writer.add("xx/xx/etc/passwd", "root"));
  REQUIRE(writer.add("Xabs.txt", "absolute"));
  REQUIRE(writer.add("Q_/dos/path.txt", "drive"));
  REQUIRE(writer.add("nul_ctl.txt", "control"));
  REQUIRE(writer.add("up/xx/x/down", "middle"));
  REQUIRE(writer.add_directory("folder"));
  Bytes archive = *writer.finish();
  replace_all(archive, "xx/xx/etc/passwd", "../../etc/passwd");
  replace_all(archive, "Xabs.txt", "/abs.txt");
  replace_all(archive, "Q_/dos/path.txt", "C:\\dos\\path.txt");
  replace_all(archive, "nul_ctl.txt", std::string_view("nul\0ctl\x01txt", 11));
  replace_all(archive, "up/xx/x/down", "up/../x/down");

  auto zip = ZipReader::open(archive);
  REQUIRE(zip);
  REQUIRE(zip->size() == 6);
  const auto& entries = zip->entries();
  CHECK(entries[0].name == "../../etc/passwd");
  CHECK(entries[0].safe_path() == std::nullopt);
  CHECK(entries[1].name == "/abs.txt");
  CHECK(entries[1].safe_path() == "abs.txt");
  CHECK(entries[2].name == "C:/dos/path.txt");  // backslashes are normalised when the directory is read
  CHECK(entries[2].safe_path() == "dos/path.txt");
  CHECK(entries[3].safe_path() == "nulctltxt");
  CHECK(entries[4].name == "up/../x/down");
  CHECK(entries[4].safe_path() == std::nullopt);
  CHECK(entries[5].name == "folder/");
  CHECK(entries[5].safe_path() == "folder");
  // The data is still readable by index or stored name; where it goes is the caller's decision.
  CHECK(str(*zip->read(std::size_t{0})) == "root");
  CHECK(str(*zip->read("/abs.txt")) == "absolute");
}

TEST_CASE("zip: writer records modes, times and methods the way other tools expect") {
  // Field-by-field check of the central directory, which is what unzip and
  // Python's zipfile read back as date_time and external_attr.
  ZipWriter writer;
  REQUIRE(writer.add("script.sh", "#!/bin/sh\n", {.level = 0, .mtime = 1709210096, .unix_mode = 0755}));
  REQUIRE(writer.add("text.txt", std::string(1000, 't'), {.level = 6, .mtime = 1577934246}));
  REQUIRE(writer.add_directory("d", {.mtime = 1577934246, .unix_mode = 0700}));
  const Bytes archive = *writer.finish();
  const std::string_view text = as_chars(ByteView(archive));
  std::size_t central = text.find("PK\x01\x02");
  REQUIRE(central != std::string_view::npos);

  CHECK((u16_at(archive, central + 4) >> 8) == 3u);              // made on Unix
  CHECK(u16_at(archive, central + 10) == 0u);                    // stored
  CHECK(u16_at(archive, central + 12) == ((12u << 11) | (34u << 5) | 28u));          // 12:34:56
  CHECK(u16_at(archive, central + 14) == ((44u << 9) | (2u << 5) | 29u));            // 2024-02-29
  CHECK(u32_at(archive, central + 38) == (0100755u << 16));
  central = text.find("PK\x01\x02", central + 4);
  REQUIRE(central != std::string_view::npos);
  CHECK(u16_at(archive, central + 10) == 8u);                    // deflated
  CHECK(u32_at(archive, central + 24) == 1000u);
  CHECK(u32_at(archive, central + 38) == (0100644u << 16));
  central = text.find("PK\x01\x02", central + 4);
  REQUIRE(central != std::string_view::npos);
  CHECK(u32_at(archive, central + 38) == ((0040700u << 16) | 0x10u));
  CHECK(text.substr(central + 46, 2) == "d/");

  auto zip = ZipReader::open(archive);
  REQUIRE(zip);
  CHECK(zip->entries()[0].mtime == 1709210096);
  CHECK(zip->entries()[0].unix_mode == 0100755u);
  CHECK(zip->entries()[2].is_directory);
  CHECK(zip->entries()[2].unix_mode == 0040700u);
  CHECK(str(*zip->read("text.txt")) == std::string(1000, 't'));
}
