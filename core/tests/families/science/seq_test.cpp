// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/science/science_test.hpp"

using namespace cvt;
using test::science::cell;
using test::science::detected;

namespace {

std::vector<int> phred(const std::string& quality) {
  std::vector<int> out;
  for (const char c : quality) out.push_back(c - 33);
  return out;
}

}  // namespace

TEST_CASE("science/fasta: decodes a Biopython file") {
  const auto table = test::read_as<ir::Table>("fasta", test::fixture("science/bio/three.fasta"));
  REQUIRE_OK(table);
  REQUIRE(table->sheets.size() == 1);
  REQUIRE(table->sheets[0].rows.size() == 4);
  CHECK(cell(*table, 0, 0) == "id");
  CHECK(cell(*table, 1, 0) == "seq1");
  CHECK(cell(*table, 1, 1) == "synthetic test sequence one");
  std::string expected;
  for (int i = 0; i < 7; ++i) expected += "ACGTACGTAC";
  CHECK(cell(*table, 1, 2) == expected + "GGCC");
  CHECK(cell(*table, 2, 2) == "TTTTGGGGCCCCAAAA");
  CHECK(cell(*table, 3, 0) == "prot1");
  CHECK(cell(*table, 3, 2) == "MKTAYIAKQRQISFVKSHFSRQ");
}

TEST_CASE("science/fasta: round trip, line width and detection") {
  const Bytes original = test::fixture("science/bio/three.fasta");
  const auto table = test::read_as<ir::Table>("fasta", original);
  REQUIRE_OK(table);
  const auto written = test::write_from("fasta", *table);
  REQUIRE_OK(written);
  CHECK(test::str(*written) == test::str(original));  // Biopython also wraps at 60
  const auto one_line = test::write_from("fasta", *table, R"({"line_width":0})");
  REQUIRE_OK(one_line);
  CHECK(test::str(*one_line).find("ACGTACGTACACGT") != std::string::npos);
  CHECK(test::str(*one_line).size() < written->size());
  CHECK(detected(original, "x.fasta") == "fasta");
  CHECK(detected(original) == "fasta");
  CHECK(test::convert(test::str(original), "", "fasta", "", "three.fa").ok());
}

TEST_CASE("science/fastq: quality encodings are detected and normalised") {
  const std::vector<int> q1 = {40, 39, 30, 20, 2, 10, 35, 38, 41, 0};
  const std::vector<int> q2 = {5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
  for (const char* name : {"reads_sanger.fastq", "reads_illumina13.fastq", "reads_solexa.fastq"}) {
    INFO(name);
    std::vector<std::string> warnings;
    const auto table =
        test::read_as<ir::Table>("fastq", test::fixture(std::string("science/bio/") + name), "", &warnings);
    REQUIRE_OK(table);
    REQUIRE(table->sheets[0].rows.size() == 3);
    CHECK(cell(*table, 1, 0) == "read1");
    CHECK(cell(*table, 1, 1) == "lane=1");
    CHECK(cell(*table, 1, 2) == "ACGTNACGTA");
    const auto got1 = phred(cell(*table, 1, 3));
    const auto got2 = phred(cell(*table, 2, 3));
    REQUIRE(got1.size() == q1.size());
    const bool solexa = std::string_view(name).find("solexa") != std::string_view::npos;
    for (std::size_t i = 0; i < q1.size(); ++i) {
      // Solexa scores cannot express Phred < 1 exactly; allow one step there.
      CHECK(std::abs(got1[i] - q1[i]) <= (solexa && q1[i] < 10 ? 1 : 0));
      CHECK(std::abs(got2[i] - q2[i]) <= (solexa && q2[i] < 10 ? 1 : 0));
    }
    const bool converted = solexa || std::string_view(name).find("illumina") != std::string_view::npos;
    CHECK(warnings.empty() == !converted);
  }
}

TEST_CASE("science/fastq: writes every encoding and matches Biopython byte for byte") {
  const auto table = test::read_as<ir::Table>("fastq", test::fixture("science/bio/reads_sanger.fastq"));
  REQUIRE_OK(table);
  const auto sanger = test::write_from("fastq", *table);
  REQUIRE_OK(sanger);
  CHECK(test::str(*sanger) == test::fixture_text("science/bio/reads_sanger.fastq"));
  const auto illumina = test::write_from("fastq", *table, R"({"quality_encoding":"phred64"})");
  REQUIRE_OK(illumina);
  CHECK(test::str(*illumina) == test::fixture_text("science/bio/reads_illumina13.fastq"));
  const auto solexa = test::write_from("fastq", *table, R"({"quality_encoding":"solexa"})");
  REQUIRE_OK(solexa);
  CHECK(test::str(*solexa) == test::fixture_text("science/bio/reads_solexa.fastq"));
  // Forced input encoding.
  const auto forced =
      test::read_as<ir::Table>("fastq", test::fixture("science/bio/reads_illumina13.fastq"),
                               R"({"quality_encoding_in":"phred64"})");
  REQUIRE_OK(forced);
  CHECK(test::tables_equal(*forced, *table));
  // FASTA -> FASTQ fills qualities with the default.
  const auto fasta = test::read_as<ir::Table>("fasta", test::fixture("science/bio/three.fasta"));
  REQUIRE_OK(fasta);
  std::vector<std::string> warnings;
  const auto filled = test::write_from("fastq", *fasta, R"({"default_quality":30})", &warnings);
  REQUIRE_OK(filled);
  CHECK(test::str(*filled).find("\n+\n???") != std::string::npos);
  CHECK(warnings.size() == 1);
}

TEST_CASE("science/genbank: decodes a Biopython record with features") {
  const auto data = test::read_as<ir::Data>("gbk", test::fixture("science/bio/construct.gb"));
  REQUIRE_OK(data);
  const Json& r = data->value["records"][0];
  CHECK(r["id"].as_string() == "TEST0001.1");
  CHECK(r["name"].as_string() == "TEST0001");
  CHECK(r["description"].as_string() == "Synthetic test construct for parser checks");
  CHECK(r["topology"].as_string() == "circular");
  CHECK(r["molecule_type"].as_string() == "DNA");
  CHECK(r["length"].as_int64() == 95);
  std::string expected = "ATGGCTAAACGTTGA";
  for (int i = 0; i < 20; ++i) expected += "ACGT";
  CHECK(r["sequence"].as_string() == expected);
  CHECK(r["annotations"]["organism"].as_string() == "synthetic construct");
  CHECK(r["annotations"]["taxonomy"] == Json::array({"other sequences", "artificial sequences"}));
  CHECK(r["annotations"]["keywords"] == Json::array({"test", "parser"}));
  const Json& features = r["features"];
  REQUIRE(features.size() == 3);
  CHECK(features[1]["type"].as_string() == "CDS");
  CHECK(features[1]["location"].as_string() == "1..15");
  CHECK(features[1]["qualifiers"]["translation"].as_string() == "MAKR");
  CHECK(features[1]["qualifiers"]["codon_start"].as_string() == "1");
  CHECK(features[1]["qualifiers"]["note"].as_string() ==
        "a long note that will need to wrap across more than one line in the flat file output format");
  CHECK(features[2]["location"].as_string() == "complement(21..40)");
}

TEST_CASE("science/embl: matches the GenBank record Biopython wrote from the same data") {
  const auto gb = test::read_as<ir::Data>("gbk", test::fixture("science/bio/construct.gb"));
  const auto embl = test::read_as<ir::Data>("embl", test::fixture("science/bio/construct.embl"));
  REQUIRE_OK(gb);
  REQUIRE_OK(embl);
  const Json& a = gb->value["records"][0];
  const Json& b = embl->value["records"][0];
  CHECK(b["id"].as_string() == "TEST0001.1");
  CHECK(b["sequence"] == a["sequence"]);
  CHECK(b["features"] == a["features"]);
  CHECK(b["annotations"]["taxonomy"] == a["annotations"]["taxonomy"]);
  CHECK(b["annotations"]["keywords"] == a["annotations"]["keywords"]);
  CHECK(b["description"] == a["description"]);
}

TEST_CASE("science/genbank: GenBank and EMBL writers round trip") {
  const auto gb = test::read_as<ir::Data>("gbk", test::fixture("science/bio/construct.gb"));
  REQUIRE_OK(gb);
  for (const char* format : {"gbk", "embl"}) {
    INFO(format);
    const auto written = test::write_from(format, *gb);
    REQUIRE_OK(written);
    const auto back = test::read_as<ir::Data>(format, *written);
    REQUIRE_OK(back);
    const Json& a = gb->value["records"][0];
    const Json& b = back->value["records"][0];
    CHECK(b["sequence"] == a["sequence"]);
    CHECK(b["id"] == a["id"]);
    CHECK_MESSAGE(test::data_equal(b["features"], a["features"]), test::data_diff(b["features"], a["features"]));
    CHECK(b["annotations"]["organism"] == a["annotations"]["organism"]);
    CHECK(b["annotations"]["taxonomy"] == a["annotations"]["taxonomy"]);
  }
  // The GenBank writer reproduces the Biopython file exactly for its sequence block.
  const auto written = test::write_from("gbk", *gb);
  REQUIRE_OK(written);
  const std::string text = test::str(*written);
  CHECK(text.find("        1 atggctaaac gttgaacgta cgtacgtacg tacgtacgta cgtacgtacg tacgtacgta\n") != std::string::npos);
  CHECK(text.find("     misc_feature    complement(21..40)\n") != std::string::npos);
  // GenBank -> FASTA through the shared Table model.
  const auto table = test::read_as<ir::Table>("gbk", test::fixture("science/bio/construct.gb"));
  REQUIRE_OK(table);
  const auto fasta = test::write_from("fasta", *table);
  REQUIRE_OK(fasta);
  CHECK(test::str(*fasta).starts_with(">TEST0001.1 Synthetic test construct for parser checks\nATGGCTAAACG"));
}

TEST_CASE("science/uniprot: decodes a Swiss-Prot entry") {
  const auto data = test::read_as<ir::Data>("uniprot", test::fixture("science/bio/test_protein.swiss"));
  REQUIRE_OK(data);
  const Json& r = data->value["records"][0];
  CHECK(r["id"].as_string() == "Q00000");
  CHECK(r["name"].as_string() == "TEST1_SYNTH");
  CHECK(r["description"].as_string() == "Synthetic test protein 1");
  CHECK(r["sequence"].as_string() == "MKTAYIAKQRQISFVKSHFSRQLEERLGLI");
  CHECK(r["annotations"]["accessions"] == Json::array({"Q00000", "Q00001"}));
  CHECK(r["annotations"]["keywords"] == Json::array({"Reference proteome", "Test entry"}));
  CHECK(r["annotations"]["molecular_weight"].as_int64() == 3337);
  REQUIRE(r["features"].size() == 2);
  CHECK(r["features"][1]["location"].as_string() == "5..20");
  CHECK(detected(test::fixture("science/bio/test_protein.swiss")) == "uniprot");
}

TEST_CASE("science/sequences: detection tells the flat files apart") {
  CHECK(detected(test::fixture("science/bio/construct.gb")) == "gbk");
  CHECK(detected(test::fixture("science/bio/construct.embl")) == "embl");
  CHECK(detected(test::fixture("science/bio/reads_sanger.fastq")) == "fastq");
}

TEST_CASE("science/sequences: damaged input never crashes") {
  CHECK_STATUS(test::read_as<ir::Table>("fasta", as_bytes("not fasta")), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Table>("fastq", as_bytes("@r\nACGT\n+\n!!")), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Table>("gbk", as_bytes("hello")), Status::Decode);
  for (const char* name : {"three.fasta", "reads_sanger.fastq", "construct.gb", "construct.embl",
                           "test_protein.swiss"}) {
    const std::string path = std::string("science/bio/") + name;
    const std::string format = std::string(name).ends_with(".fasta")   ? "fasta"
                               : std::string(name).ends_with(".fastq") ? "fastq"
                               : std::string(name).ends_with(".gb")    ? "gbk"
                               : std::string(name).ends_with(".embl")  ? "embl"
                                                                       : "uniprot";
    CHECK(test::fuzz_reader<ir::Data>(format, test::fixture(path), 128) > 0);
  }
}
