// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/science/science_test.hpp"

using namespace cvt;
using test::science::cell;
using test::science::detected;

namespace {

std::string base() {
  std::string s;
  for (int i = 0; i < 7; ++i) s += "ACGTTGCAAC";
  return s;
}

std::vector<std::string> expected_sequences() {
  std::string a = base();
  std::string b = a;
  b[5] = '-';
  b[40] = 'G';
  std::string c = a;
  c[69] = 'A';
  return {a, b, c};
}

void check_alignment(const ir::Table& table, const std::vector<std::string>& names) {
  const auto seqs = expected_sequences();
  REQUIRE(table.sheets.size() == 1);
  REQUIRE(table.sheets[0].rows.size() == 4);
  for (std::size_t i = 0; i < 3; ++i) {
    CHECK(cell(table, i + 1, 0) == names[i]);
    CHECK(cell(table, i + 1, 2) == seqs[i]);
  }
}

}  // namespace

TEST_CASE("science/alignments: decodes Biopython PHYLIP, Clustal, Stockholm and NEXUS files") {
  const std::vector<std::string> names = {"taxonA", "taxonB", "taxonC"};
  for (const auto& [format, file] : std::vector<std::pair<std::string, std::string>>{
           {"phylip", "msa.phy"},
           {"phylip", "msa_sequential.phy"},
           {"clustal", "msa.aln"},
           {"sto", "msa.sto"},
           {"nex", "msa.nex"}}) {
    INFO(file);
    const auto table = test::read_as<ir::Table>(format, test::fixture("science/bio/" + file));
    REQUIRE_OK(table);
    check_alignment(*table, names);
    CHECK(detected(test::fixture("science/bio/" + file), file) == format);
  }
  const auto relaxed = test::read_as<ir::Table>("phylip", test::fixture("science/bio/msa_relaxed.phy"));
  REQUIRE_OK(relaxed);
  check_alignment(*relaxed, {"a_rather_long_name_1", "a_rather_long_name_2", "short"});
}

TEST_CASE("science/alignments: every writer round trips through every reader") {
  const auto source = test::read_as<ir::Table>("clustal", test::fixture("science/bio/msa.aln"));
  REQUIRE_OK(source);
  for (const char* format : {"phylip", "clustal", "sto", "nex", "fasta"}) {
    INFO(format);
    const auto written = test::write_from(format, *source);
    REQUIRE_OK(written);
    const auto back = test::read_as<ir::Table>(format, *written);
    REQUIRE_OK(back);
    check_alignment(*back, {"taxonA", "taxonB", "taxonC"});
  }
  // Our sequential PHYLIP matches Biopython's sequential writer byte for byte.
  const auto phylip = test::write_from("phylip", *source);
  REQUIRE_OK(phylip);
  CHECK(test::str(*phylip) == test::fixture_text("science/bio/msa_sequential.phy"));
  // Long names: auto switches to relaxed, strict truncates with a warning.
  const auto relaxed = test::read_as<ir::Table>("phylip", test::fixture("science/bio/msa_relaxed.phy"));
  REQUIRE_OK(relaxed);
  const auto auto_out = test::write_from("phylip", *relaxed);
  REQUIRE_OK(auto_out);
  CHECK(test::str(*auto_out).find("a_rather_long_name_1 ") != std::string::npos);
  std::vector<std::string> warnings;
  const auto strict = test::write_from("phylip", *relaxed, R"({"phylip_names_out":"strict"})", &warnings);
  REQUIRE_OK(strict);
  CHECK(test::str(*strict).find("a_rather_l") != std::string::npos);
  CHECK(test::str(*strict).find("a_rather_long") == std::string::npos);
  CHECK(warnings.size() == 1);
  // Unequal sequences are padded with gaps.
  ir::Table ragged = *source;
  ragged.sheets[0].rows[3][2].text = "ACG";
  const auto padded = test::write_from("clustal", ragged, "", &warnings);
  REQUIRE_OK(padded);
  CHECK(warnings.size() == 1);
}

TEST_CASE("science/newick: decodes Biopython trees, NEXUS trees agree") {
  const auto data = test::read_as<ir::Data>("newick", test::fixture("science/bio/tree.nwk"));
  REQUIRE_OK(data);
  const Json& root = data->value["trees"][0]["root"];
  REQUIRE(root["children"].size() == 3);
  CHECK(root["children"][0]["name"].as_string() == "0.95");
  CHECK(root["children"][0]["length"].as_double() == 0.05);
  CHECK(root["children"][0]["children"][1]["name"].as_string() == "taxonB");
  CHECK(root["children"][0]["children"][1]["length"].as_double() == 0.25);
  CHECK(root["children"][1]["name"].as_string() == "taxon C");
  CHECK(root["children"][2]["name"].as_string() == "inner");
  CHECK(root["children"][2]["children"][0]["length"].as_double() == 0.001);
  CHECK(root["children"][2]["children"][1]["length"].as_double() == 2.0);
  const auto nexus = test::read_as<ir::Data>("nex", test::fixture("science/bio/tree.nex"));
  REQUIRE_OK(nexus);
  CHECK(nexus->value["trees"][0]["name"].as_string() == "tree1");
  CHECK(nexus->value["trees"][0]["root"] == root);
  // Newick -> Newick and Newick -> NEXUS -> Newick.
  const auto newick = test::write_from("newick", *data);
  REQUIRE_OK(newick);
  CHECK(test::str(*newick) == test::fixture_text("science/bio/tree.nwk"));
  const auto nex = test::write_from("nex", *data);
  REQUIRE_OK(nex);
  const auto back = test::read_as<ir::Data>("nex", *nex);
  REQUIRE_OK(back);
  CHECK(back->value["trees"][0]["root"] == root);
  CHECK(detected(test::fixture("science/bio/tree.nwk"), "t.nwk") == "newick");
}

TEST_CASE("science/nexus: TRANSLATE tables, quoted names and comments") {
  const std::string text =
      "#NEXUS\n[a comment]\nbegin trees;\n translate 1 'first one', 2 second;\n tree t = [&U] (1:1,(2:2,3:3)x);\nend;\n";
  const auto data = test::read_as<ir::Data>("nex", as_bytes(text));
  REQUIRE_OK(data);
  const Json& tree = data->value["trees"][0];
  CHECK(tree["rooted"].as_bool(true) == false);
  CHECK(tree["root"]["children"][0]["name"].as_string() == "first one");
  CHECK(tree["root"]["children"][1]["children"][0]["name"].as_string() == "second");
  CHECK(tree["root"]["children"][1]["children"][1]["name"].as_string() == "3");
}

TEST_CASE("science/alignments: hostile input") {
  CHECK_STATUS(test::read_as<ir::Data>("newick", as_bytes("((a,b);")), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Data>("newick", as_bytes("(a,b));")), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Table>("phylip", as_bytes(" 99999999 5\nx ACGTA\n")), Status::Decode);
  // A deep caterpillar tree is fine (no recursion); absurd depth is refused.
  std::string deep(5000, '(');
  deep += "a";
  for (int i = 0; i < 5000; ++i) deep += ",b)";
  deep += ";";
  CHECK_OK(test::read_as<ir::Data>("newick", as_bytes(deep)));
  const auto deep_data = test::read_as<ir::Data>("newick", as_bytes(deep));
  REQUIRE_OK(deep_data);
  CHECK_OK(test::write_from("newick", *deep_data));
  for (const auto& [format, name] : std::vector<std::pair<std::string, std::string>>{
           {"phylip", "msa.phy"}, {"clustal", "msa.aln"}, {"sto", "msa.sto"}, {"nex", "msa.nex"},
           {"nex", "tree.nex"}, {"newick", "tree.nwk"}}) {
    CHECK(test::fuzz_reader<ir::Data>(format, test::fixture("science/bio/" + name)) > 0);
  }
}
