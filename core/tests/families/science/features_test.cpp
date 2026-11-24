// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/science/science_test.hpp"

using namespace cvt;
using test::science::cell;
using test::science::detected;

TEST_CASE("science/gff3: decodes a gffread file with directives") {
  const auto table = test::read_as<ir::Table>("gff3", test::fixture("science/bio/annotation.gff3"));
  REQUIRE_OK(table);
  REQUIRE(table->sheets.size() == 2);
  const auto& rows = table->sheets[0].rows;
  REQUIRE(rows.size() == 7);
  CHECK(cell(*table, 0, 0) == "seqid");
  CHECK(cell(*table, 1, 2) == "gene");
  CHECK(rows[1][3].type == ir::CellType::Number);
  CHECK(rows[1][3].number == 100);
  CHECK(cell(*table, 1, 8) == "ID=gene1;Name=tstA");
  CHECK(cell(*table, 6, 7) == "2");
  CHECK(table->sheets[1].name == "header");
  CHECK(cell(*table, 0, 0, 1) == "##gff-version 3");
  // Same-format round trip is byte exact.
  const auto written = test::write_from("gff3", *table);
  REQUIRE_OK(written);
  CHECK(test::str(*written) == test::fixture_text("science/bio/annotation.gff3"));
  // Expanded attributes add columns but write back the same nine.
  const auto expanded =
      test::read_as<ir::Table>("gff3", test::fixture("science/bio/annotation.gff3"), R"({"expand_attributes":true})");
  REQUIRE_OK(expanded);
  CHECK(cell(*expanded, 0, 9) == "attr:ID");
  CHECK(cell(*expanded, 0, 11) == "attr:Parent");
  CHECK(cell(*expanded, 2, 11) == "gene1");
  const auto again = test::write_from("gff3", *expanded);
  REQUIRE_OK(again);
  CHECK(test::str(*again) == test::fixture_text("science/bio/annotation.gff3"));
}

TEST_CASE("science/gtf: decodes and expands GTF attributes") {
  const auto table = test::read_as<ir::Table>("gtf", test::fixture("science/bio/annotation.gtf"),
                                              R"({"expand_attributes":true})");
  REQUIRE_OK(table);
  CHECK(cell(*table, 1, 2) == "transcript");
  CHECK(cell(*table, 0, 9) == "attr:transcript_id");
  CHECK(cell(*table, 3, 10) == "gene1");
  CHECK(detected(test::fixture("science/bio/annotation.gtf"), "a.gtf") == "gtf");
  const auto plain = test::read_as<ir::Table>("gtf", test::fixture("science/bio/annotation.gtf"));
  REQUIRE_OK(plain);
  const auto written = test::write_from("gtf", *plain);
  REQUIRE_OK(written);
  CHECK(test::str(*written) == test::fixture_text("science/bio/annotation.gtf"));
}

TEST_CASE("science/bed: decodes a bedtools-sorted file") {
  const auto table = test::read_as<ir::Table>("bed", test::fixture("science/bio/intervals.bed"));
  REQUIRE_OK(table);
  CHECK(cell(*table, 0, 1) == "chromStart");
  CHECK(cell(*table, 1, 3) == "peakA");
  CHECK(table->sheets[0].rows[1][1].number == 10);
  CHECK(cell(*table, 3, 5) == ".");
  const auto written = test::write_from("bed", *table);
  REQUIRE_OK(written);
  CHECK(test::str(*written) == test::fixture_text("science/bio/intervals.bed"));
  CHECK(detected(test::fixture("science/bio/intervals.bed"), "x.bed") == "bed");
}

TEST_CASE("science/vcf: decodes a pysam file with samples") {
  const Bytes vcf = test::fixture("science/bio/variants.vcf");
  const auto table = test::read_as<ir::Table>("vcf-variant", vcf);
  REQUIRE_OK(table);
  CHECK(cell(*table, 0, 0) == "CHROM");
  CHECK(cell(*table, 0, 9) == "sampleA");
  CHECK(cell(*table, 1, 2) == "var1");
  CHECK(table->sheets[0].rows[2][5].number == 12.5);
  CHECK(cell(*table, 2, 10) == "./.");
  CHECK(cell(*table, 0, 0, 1) == "##fileformat=VCFv4.2");
  const auto written = test::write_from("vcf-variant", *table);
  REQUIRE_OK(written);
  CHECK(test::str(*written) == test::str(vcf));
  // vCard files share the extension; the content decides.
  CHECK(detected(vcf, "calls.vcf") == "vcf-variant");
}

TEST_CASE("science/sam: decodes a pysam file and BAM gives the same records") {
  const auto sam = test::read_as<ir::Table>("sam", test::fixture("science/bio/aligned.sam"));
  REQUIRE_OK(sam);
  CHECK(cell(*sam, 1, 0) == "r001");
  CHECK(cell(*sam, 1, 5) == "8M2I");
  CHECK(cell(*sam, 1, 11) == "NM:i:1");
  CHECK(cell(*sam, 1, 14) == "XB:B:C,1,2,3");
  CHECK(cell(*sam, 0, 11) == "TAG1");
  const auto written = test::write_from("sam", *sam);
  REQUIRE_OK(written);
  CHECK(test::str(*written) == test::fixture_text("science/bio/aligned.sam"));

  const auto bam = test::read_as<ir::Table>("bam", test::fixture("science/bio/aligned.bam"));
  REQUIRE_OK(bam);
  CHECK_MESSAGE(test::tables_equal(*bam, *sam), test::table_diff(*bam, *sam));
  CHECK(detected(test::fixture("science/bio/aligned.bam")) == "bam");
  CHECK(detected(test::fixture("science/bio/aligned.sam")) == "sam");
  // BAM -> SAM through the engine.
  const auto converted = test::convert(test::fixture("science/bio/aligned.bam"), "bam", "sam");
  REQUIRE(converted.ok());
  CHECK(converted.text() == test::fixture_text("science/bio/aligned.sam"));
}

TEST_CASE("science/feature tables: damaged input never crashes") {
  CHECK_STATUS(test::read_as<ir::Table>("bed", as_bytes("one two\n")), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Table>("bam", as_bytes("BAM\x01")), Status::Decode);
  for (const auto& [format, name] : std::vector<std::pair<std::string, std::string>>{
           {"gff3", "annotation.gff3"}, {"gtf", "annotation.gtf"}, {"bed", "intervals.bed"},
           {"vcf-variant", "variants.vcf"}, {"sam", "aligned.sam"}, {"bam", "aligned.bam"}}) {
    CHECK(test::fuzz_reader<ir::Table>(format, test::fixture("science/bio/" + name)) > 0);
  }
  // A BAM whose record claims more bytes than exist.
  const Bytes bam = test::fixture("science/bio/aligned.bam");
  CHECK(test::fuzz_reader<ir::Table>("bam", ByteView(bam).first(bam.size() - 28)) > 0);
}
