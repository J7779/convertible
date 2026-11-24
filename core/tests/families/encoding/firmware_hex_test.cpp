// Tests of the firmware image formats (firmware_hex.cpp) against files written
// by SRecord's srec_cat and GNU objcopy (tests/data/encoding/firmware/SOURCES.md).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>
#include <string_view>

#include "engine/detect.hpp"
#include "engine/registry.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"
#include "util/text.hpp"

using namespace cvt;
using test::convert;

namespace {

Bytes sample() { return test::fixture("encoding/bases/sample.bin"); }
std::string fixture(std::string_view name) { return test::fixture_text("encoding/firmware/" + std::string(name)); }

std::string detected(std::string_view data, std::string_view filename) {
  const auto detection = detect_format(Registry::instance(), as_bytes(data), filename);
  return detection ? std::string(detection->format->id) : std::string("none");
}

}  // namespace

TEST_CASE("encoding/ihex: decodes srec_cat and objcopy output") {
  for (const std::string_view name : {"sample_1000.ihex", "sample_ffc0.ihex", "sample_objcopy.ihex"}) {
    INFO(name);
    std::vector<std::string> warnings;
    const auto blob = test::read_as<ir::Blob>("ihex", as_bytes(fixture(name)), {}, &warnings);
    REQUIRE_OK(blob);
    CHECK(blob->bytes == sample());
    CHECK(detected(fixture(name), "firmware.hex") == "ihex");
  }
  // Writing at 0xFFC0 crosses 64 KiB: two extended linear address records, no record crosses the boundary.
  const std::string ours = convert(sample(), "bin", "ihex", R"({"address":65472,"record_size":32})").text();
  CHECK(ours.find(":020000040001F9") != std::string::npos);
  CHECK(convert(ours, "ihex", "bin").data == sample());
  // srec_cat wrote 32-byte records at 0x1000: our records are identical (srec_cat also states the
  // implicit upper address 0 in a first extended address record, which objcopy and we leave out).
  const std::string at1000 = convert(sample(), "bin", "ihex", R"({"address":4096,"record_size":32})").text();
  const std::string theirs = fixture("sample_1000.ihex");
  CHECK(at1000 == theirs.substr(theirs.find('\n') + 1));
  // objcopy uses 16-byte records from address 0, which is our default.
  CHECK(convert(sample(), "bin", "ihex", R"({"line_ending":"crlf"})").text() == fixture("sample_objcopy.ihex"));
}

TEST_CASE("encoding/srec: decodes S19/S28 files and writes compatible records") {
  for (const std::string_view name : {"sample_1000.srec", "sample_123400.s28", "sample_objcopy.srec"}) {
    INFO(name);
    const auto blob = test::read_as<ir::Blob>("srec", as_bytes(fixture(name)));
    REQUIRE_OK(blob);
    CHECK(blob->bytes == sample());
    CHECK(detected(fixture(name), "image.s19") == "srec");
  }
  const std::string ours = convert(sample(), "bin", "srec", R"({"address":4096})").text();
  // The data records match srec_cat's (it writes its own S0 header text).
  const std::string theirs = fixture("sample_1000.srec");
  const auto theirs_lines = util::split_lines(theirs);
  const auto our_lines = util::split_lines(ours);
  REQUIRE(our_lines.size() >= theirs_lines.size());
  for (std::size_t i = 1; i + 1 < theirs_lines.size(); ++i) CHECK(util::trim(our_lines[i]) == util::trim(theirs_lines[i]));
  CHECK(our_lines.back() == "S9031000EC");
  CHECK(convert(ours, "srec", "bin").data == sample());
  const std::string wide = convert(sample(), "bin", "srec", R"({"address":1193984})").text();
  CHECK(wide.find("\nS2") != std::string::npos);
  CHECK(wide.find("\nS8") != std::string::npos);
}

TEST_CASE("encoding/ti-txt and encoding/readmemh: srec_cat files") {
  const auto ti = test::read_as<ir::Blob>("ti-txt", as_bytes(fixture("sample_1000.ti-txt")));
  REQUIRE_OK(ti);
  CHECK(ti->bytes == sample());
  CHECK(convert(sample(), "bin", "ti-txt", R"({"address":4096})").text() == fixture("sample_1000.ti-txt"));
  CHECK(detected(fixture("sample_1000.ti-txt"), "fw.txt") == "ti-txt");
  const auto vmem = test::read_as<ir::Blob>("readmemh", as_bytes(fixture("sample.vmem")));
  REQUIRE_OK(vmem);
  CHECK(vmem->bytes == sample());
  // 16-bit words, little-endian, with an address jump.
  const std::string words = convert(Bytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}}, "bin", "readmemh",
                                    R"({"word_bytes":2,"big_endian":false,"address":16})")
                                .text();
  CHECK(words == "@10\n0201\n0403\n");
  const auto back = test::read_as<ir::Blob>("readmemh", as_bytes(std::string("// c\n0201 /* x */ 0403\n")),
                                            R"({"big_endian":false})");
  REQUIRE_OK(back);
  CHECK(back->bytes == (Bytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}}));
}

TEST_CASE("encoding/ihex: gaps, checksums, limits and damaged files") {
  // Two records with a gap: filled with the fill byte.
  const std::string gapped = ":0100000011EE\n:0100040022D9\n:00000001FF\n";
  std::vector<std::string> warnings;
  const auto filled = test::read_as<ir::Blob>("ihex", as_bytes(gapped), R"({"fill":0})", &warnings);
  REQUIRE_OK(filled);
  CHECK(filled->bytes == (Bytes{std::byte{0x11}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0x22}}));
  CHECK(!warnings.empty());
  CHECK_STATUS(test::read_as<ir::Blob>("ihex", as_bytes(std::string(":0100000011EF\n"))), Status::Decode);  // checksum
  CHECK_STATUS(test::read_as<ir::Blob>("srec", as_bytes(std::string("S1040000110A\n"))), Status::Decode);
  // A record far away would make a 4 GiB image: refused by the limits, not allocated.
  const std::string far = ":0100000011EE\n:02000004FFFFFC\n:01FFFF0022DF\n:00000001FF\n";
  CHECK_STATUS(test::read_as<ir::Blob>("ihex", as_bytes(far)), Status::Limit);
  for (const std::string_view format : {"ihex", "srec", "ti-txt", "readmemh"}) {
    INFO(format);
    const test::Converted encoded = convert(test::random_bytes(700, 5), "bin", format);
    REQUIRE(encoded.ok());
    CHECK(convert(encoded.data, format, "bin").data == test::random_bytes(700, 5));
    CHECK(test::fuzz_reader<ir::Blob>(format, encoded.data, 64) > 0);
  }
}
