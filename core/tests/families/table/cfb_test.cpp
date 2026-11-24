// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cstring>
#include <string>

#include "families/table/cfb.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"

using namespace cvt;
using table::CfbReader;
using table::CfbWriter;

TEST_CASE("table/cfb: reads the directory of an Excel file written by LibreOffice") {
  const Bytes bytes = test::fixture("table/lo_sample.xls");
  const auto cfb = CfbReader::open(bytes);
  REQUIRE_OK(cfb);
  CHECK(cfb->sector_size() == 512);
  REQUIRE(cfb->contains("Workbook"));
  CHECK(cfb->contains("workbook"));  // names compare case-insensitively
  CHECK(cfb->entries().front().type == table::CfbEntryType::Root);
  const auto workbook = cfb->read("Workbook");
  REQUIRE_OK(workbook);
  CHECK(workbook->size() == cfb->entries()[*cfb->find("Workbook")].size);
  CHECK(std::to_integer<int>((*workbook)[0]) == 0x09);  // BOF record
}

TEST_CASE("table/cfb: write and read back streams, storages, mini and regular sectors") {
  CfbWriter writer;
  const Bytes small = test::random_bytes(100, 1);
  const Bytes exact = test::random_bytes(4096, 2);  // the first size kept outside the mini stream
  const Bytes large = test::random_bytes(70000, 3);
  REQUIRE_OK(writer.add_stream("Small", small));
  REQUIRE_OK(writer.add_stream("Exact", exact));
  REQUIRE_OK(writer.add_stream("Folder/Large", large));
  REQUIRE_OK(writer.add_stream("Folder/Deeper/Tiny", test::bytes("x")));
  REQUIRE_OK(writer.add_stream("\x05SummaryInformation", test::bytes("props")));
  REQUIRE_OK(writer.add_stream("Empty", Bytes{}));
  REQUIRE_OK(writer.add_storage("Unused storage"));
  for (int i = 0; i < 40; ++i) REQUIRE_OK(writer.add_stream("Folder/item" + std::to_string(i), test::random_bytes(static_cast<std::size_t>(i * 37), static_cast<std::uint32_t>(i))));
  const auto file = writer.finish();
  REQUIRE_OK(file);
  const auto again = writer.finish();
  REQUIRE_OK(again);
  CHECK(*file == *again);  // deterministic
  const auto cfb = CfbReader::open(*file);
  REQUIRE_OK(cfb);
  CHECK(cfb->read("Small").value() == small);
  CHECK(cfb->read("Exact").value() == exact);
  CHECK(cfb->read("Folder/Large").value() == large);
  CHECK(cfb->read("folder/deeper/TINY").value() == test::bytes("x"));
  CHECK(cfb->read("\x05SummaryInformation").value() == test::bytes("props"));
  CHECK(cfb->read("Empty").value().empty());
  CHECK(cfb->read("Folder/item39").value() == test::random_bytes(39 * 37, 39));
  CHECK_STATUS(cfb->read("Folder"), Status::Decode);  // a storage
  CHECK_STATUS(cfb->read("Missing"), Status::Decode);
  const auto folder = cfb->find("Folder");
  REQUIRE(folder.has_value());
  CHECK(cfb->children(*folder).size() == 42);
}

TEST_CASE("table/cfb: invalid names are rejected") {
  CfbWriter writer;
  CHECK_STATUS(writer.add_stream("", test::bytes("x")), Status::InvalidArgument);
  CHECK_STATUS(writer.add_stream(std::string(32, 'n'), test::bytes("x")), Status::InvalidArgument);
  CHECK_STATUS(writer.add_stream("a:b", test::bytes("x")), Status::InvalidArgument);
  REQUIRE_OK(writer.add_stream("Data", test::bytes("x")));
  CHECK_STATUS(writer.add_stream("Data/inside", test::bytes("x")), Status::InvalidArgument);
}

TEST_CASE("table/cfb: hostile files fail cleanly") {
  CfbWriter writer;
  REQUIRE_OK(writer.add_stream("Big", test::random_bytes(20000, 7)));
  REQUIRE_OK(writer.add_stream("Small", test::random_bytes(300, 8)));
  const auto file = writer.finish();
  REQUIRE_OK(file);
  CHECK_STATUS(CfbReader::open(ByteView(*file).first(200)), Status::Decode);
  // Every truncation and some corruption.
  for (std::size_t length = 0; length < file->size(); length += 97) {
    if (auto cfb = CfbReader::open(ByteView(*file).first(length))) {
      for (std::size_t i = 0; i < cfb->entries().size(); ++i) (void)cfb->read(i);
    }
  }
  for (std::uint32_t seed = 1; seed <= 64; ++seed) {
    Bytes damaged = *file;
    const Bytes noise = test::random_bytes(8, seed);
    for (int k = 0; k < 4; ++k) {
      const std::size_t at = (std::to_integer<std::size_t>(noise[static_cast<std::size_t>(k) * 2]) * 131u + seed * 7919u) % damaged.size();
      damaged[at] = noise[static_cast<std::size_t>(k) * 2 + 1];
    }
    if (auto cfb = CfbReader::open(damaged)) {
      for (std::size_t i = 0; i < cfb->entries().size(); ++i) (void)cfb->read(i);
    }
  }
  // A FAT chain that loops: make the first sector of "Big" point to itself.
  {
    Bytes looped = *file;
    const auto cfb = CfbReader::open(looped);
    REQUIRE_OK(cfb);
    const std::size_t big = *cfb->find("Big");
    const std::uint32_t start = cfb->entries()[big].start_sector;
    // The FAT is the first FAT sector listed in the header's DIFAT array.
    const std::uint32_t fat_sector = std::to_integer<std::uint32_t>(looped[76]) | (std::to_integer<std::uint32_t>(looped[77]) << 8);
    const std::size_t entry = (std::size_t{fat_sector} + 1) * 512 + std::size_t{start} * 4;
    for (int i = 0; i < 4; ++i) looped[entry + static_cast<std::size_t>(i)] = static_cast<std::byte>(start >> (8 * i));
    const auto damaged = CfbReader::open(looped);
    REQUIRE_OK(damaged);
    const auto read = damaged->read("Big");
    CHECK_STATUS(read, Status::Decode);
  }
  // A directory whose sibling links form a cycle.
  {
    Bytes cyclic = *file;
    const std::uint32_t directory = std::to_integer<std::uint32_t>(cyclic[48]) | (std::to_integer<std::uint32_t>(cyclic[49]) << 8);
    const std::size_t base = (std::size_t{directory} + 1) * 512;
    // Entry 1's left sibling := entry 1.
    cyclic[base + 128 + 68] = std::byte{1};
    cyclic[base + 128 + 69] = std::byte{0};
    cyclic[base + 128 + 70] = std::byte{0};
    cyclic[base + 128 + 71] = std::byte{0};
    CHECK_STATUS(CfbReader::open(cyclic), Status::Decode);
  }
  // Reading more than the limit allows.
  Limits limits;
  limits.max_decompressed_bytes = 10000;
  const auto limited = CfbReader::open(*file, limits);
  REQUIRE_OK(limited);
  CHECK_STATUS(limited->read("Big"), Status::Limit);
}
