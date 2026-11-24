// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/science/science_test.hpp"

using namespace cvt;
using test::science::detected;

namespace {

// Ethanol as written by RDKit (EmbedMolecule seed 7), the reference for every format.
struct RefAtom {
  const char* element;
  double x, y, z;
};
constexpr RefAtom ethanol[] = {
    {"C", -0.914639, 0.284325, 0.060859}, {"C", 0.422115, -0.366012, -0.214139}, {"O", 1.449086, 0.546228, 0.058640},
    {"H", -1.695343, -0.082510, -0.650364}, {"H", -0.882089, 1.394320, -0.076831}, {"H", -1.225412, 0.057798, 1.096797},
    {"H", 0.484622, -1.229291, 0.496425}, {"H", 0.477108, -0.785552, -1.226213}, {"H", 1.884551, 0.180694, 0.882066}};

void check_ethanol(const Json& molecule, double tolerance, bool bonds = true) {
  const Json& atoms = molecule["atoms"];
  REQUIRE(atoms.size() == 9);
  for (std::size_t i = 0; i < 9; ++i) {
    INFO(i);
    CHECK(atoms[i]["element"].as_string() == ethanol[i].element);
    CHECK_APPROX(atoms[i]["x"].as_double(), ethanol[i].x, tolerance);
    CHECK_APPROX(atoms[i]["y"].as_double(), ethanol[i].y, tolerance);
    CHECK_APPROX(atoms[i]["z"].as_double(), ethanol[i].z, tolerance);
  }
  if (!bonds) return;
  REQUIRE(molecule["bonds"].size() == 8);
  CHECK(molecule["bonds"][1] == Json::object({{"from", 2}, {"to", 3}, {"order", 1}}));
}

Json read_data(const std::string& format, const std::string& file) {
  const auto data = test::read_as<ir::Data>(format, test::fixture("science/chem/" + file));
  if (!data) {
    FAIL_CHECK(file, ": ", data.error().message);
    return Json();
  }
  return data->value;
}

}  // namespace

TEST_CASE("science/chem: ethanol from RDKit and Open Babel in every format") {
  check_ethanol(read_data("mol", "ethanol.mol")["molecules"][0], 1e-4);
  check_ethanol(read_data("sdf", "three.sdf")["molecules"][0], 1e-4);
  check_ethanol(read_data("xyz", "ethanol.xyz")["molecules"][0], 1e-6, false);
  check_ethanol(read_data("mol2", "ethanol.mol2")["molecules"][0], 1e-4);
  check_ethanol(read_data("cml", "ethanol.cml")["molecules"][0], 1e-4);
  const Json mol2 = read_data("mol2", "ethanol.mol2");
  CHECK(mol2["molecules"][0]["atoms"][0]["atom_type"].as_string() == "C.3");
  CHECK_APPROX(mol2["molecules"][0]["atoms"][2]["partial_charge"].as_double(), -0.3953, 1e-9);
  CHECK(read_data("cml", "ethanol.cml")["molecules"][0]["atoms"][0]["hydrogen_count"].as_int64() == 3);
  CHECK(read_data("xyz", "ethanol.xyz")["molecules"][0]["name"].as_string() == "ethanol");
}

TEST_CASE("science/sdf: three records, charges, aromatic bonds and data items") {
  const Json data = read_data("sdf", "three.sdf");
  const Json& molecules = data["molecules"];
  REQUIRE(molecules.size() == 3);
  CHECK(molecules[1]["name"].as_string() == "acetate");
  CHECK(molecules[1]["atoms"][3]["charge"].as_int64() == -1);
  CHECK(molecules[1]["atoms"][2]["charge"].as_int64() == 0);
  CHECK(molecules[1]["bonds"][1]["order"].as_int64() == 2);
  CHECK(molecules[1]["properties"]["test_value"].as_string() == "42");
  CHECK(molecules[1]["properties"]["note"].as_string() == "made-up property");
  CHECK(molecules[2]["atoms"].size() == 12);
  // V3000 gives the same acetate.
  const Json v3 = read_data("mol", "acetate_v3000.mol");
  const Json& a = v3["molecules"][0];
  REQUIRE(a["atoms"].size() == 7);
  CHECK(a["atoms"][3]["charge"].as_int64() == -1);
  for (std::size_t i = 0; i < 7; ++i) {
    CHECK_APPROX(a["atoms"][i]["x"].as_double(), molecules[1]["atoms"][i]["x"].as_double(), 1e-4);
  }
  CHECK(a["bonds"] == molecules[1]["bonds"]);
  CHECK(detected(test::fixture("science/chem/three.sdf")) == "sdf");
  CHECK(detected(test::fixture("science/chem/ethanol.mol"), "e.mol") == "mol");
  CHECK(detected(test::fixture("science/chem/ethanol.mol2")) == "mol2");
  CHECK(detected(test::fixture("science/chem/ethanol.cml")) == "cml");
  CHECK(detected(test::fixture("science/chem/ethanol.xyz")) == "xyz");
}

TEST_CASE("science/chem: writers round trip through their own readers and across formats") {
  const auto source = test::read_as<ir::Data>("sdf", test::fixture("science/chem/three.sdf"));
  REQUIRE_OK(source);
  for (const char* format : {"sdf", "mol2", "cml", "xyz", "pdb", "mmcif", "cif"}) {
    INFO(format);
    const auto written = test::write_from(format, *source);
    REQUIRE_OK(written);
    const auto back = test::read_as<ir::Data>(format, *written);
    REQUIRE_OK(back);
    const Json& a = source->value["molecules"][1];
    const Json& b = back->value["molecules"][std::string_view(format) == "pdb" ? 1 : 1];
    REQUIRE(b["atoms"].size() == a["atoms"].size());
    for (std::size_t i = 0; i < a["atoms"].size(); ++i) {
      CHECK(b["atoms"][i]["element"] == a["atoms"][i]["element"]);
      CHECK_APPROX(b["atoms"][i]["x"].as_double(), a["atoms"][i]["x"].as_double(), 1e-3);
      CHECK_APPROX(b["atoms"][i]["z"].as_double(), a["atoms"][i]["z"].as_double(), 1e-3);
    }
    const std::string_view f(format);
    if (f == "sdf" || f == "mol2" || f == "cml") CHECK(b["bonds"] == a["bonds"]);
    if (f == "sdf" || f == "cml" || f == "pdb") CHECK(b["atoms"][3]["charge"].as_int64() == -1);
  }
  // Molfile writer: exact text of the V2000 atom and bond blocks.
  const auto mol = test::write_from("mol", *source);
  REQUIRE_OK(mol);
  const std::string text = test::str(*mol);
  CHECK(text.find("  9  8  0  0  0  0  0  0  0  0999 V2000\n") != std::string::npos);
  CHECK(text.find("   -0.9146    0.2843    0.0609 C   0  0") != std::string::npos);
  // Table route: atoms + bonds sheets back to SDF.
  const auto table = test::read_as<ir::Table>("sdf", test::fixture("science/chem/three.sdf"));
  REQUIRE_OK(table);
  CHECK(table->sheets[0].name == "atoms");
  CHECK(table->sheets[1].name == "bonds");
  const auto from_table = test::write_from("sdf", *table);
  REQUIRE_OK(from_table);
  const auto again = test::read_as<ir::Data>("sdf", *from_table);
  REQUIRE_OK(again);
  CHECK(again->value["molecules"][1]["bonds"] == source->value["molecules"][1]["bonds"]);
  CHECK(again->value["molecules"][1]["properties"]["test_value"].as_string() == "42");
}

TEST_CASE("science/pdb: decodes an RDKit peptide with residues and CONECT bond orders") {
  const Json data = read_data("pdb", "peptide.pdb");
  const Json& m = data["molecules"][0];
  REQUIRE(m["atoms"].size() == 14);
  const Json& a = m["atoms"][1];
  CHECK(a["element"].as_string() == "C");
  CHECK(a["name"].as_string() == "CA");
  CHECK(a["residue"].as_string() == "GLY");
  CHECK(a["chain"].as_string() == "A");
  CHECK(a["residue_number"].as_int64() == 1);
  CHECK_APPROX(a["x"].as_double(), -3.967, 1e-9);
  CHECK(m["atoms"][13]["name"].as_string() == "OXT");
  // RDKit lists C=O twice: "CONECT    3    4    4    5".
  bool found_double = false;
  for (const Json& b : m["bonds"].elements()) {
    if (b["from"].as_int64() == 3 && b["to"].as_int64() == 4) found_double = b["order"].as_int64() == 2;
  }
  CHECK(found_double);
  CHECK(m["bonds"].size() == 13);
  // mmCIF written by Open Babel from the same PDB.
  const Json cif = read_data("mmcif", "peptide.cif");
  const Json& c = cif["molecules"][0];
  REQUIRE(c["atoms"].size() == 14);
  for (std::size_t i = 0; i < 14; ++i) {
    CHECK(c["atoms"][i]["element"] == m["atoms"][i]["element"]);
    CHECK(c["atoms"][i]["name"] == m["atoms"][i]["name"]);
    CHECK(c["atoms"][i]["residue"] == m["atoms"][i]["residue"]);
    CHECK_APPROX(c["atoms"][i]["y"].as_double(), m["atoms"][i]["y"].as_double(), 1e-9);
  }
  // PDB writer reproduces RDKit's ATOM lines.
  const auto pdb = test::write_from("pdb", ir::Data{data});
  REQUIRE_OK(pdb);
  CHECK(test::str(*pdb).find("ATOM      2  CA  GLY A   1      -3.967   0.223   1.136  1.00  0.00           C  \n") !=
        std::string::npos);
  CHECK(detected(test::fixture("science/chem/peptide.pdb"), "p.pdb") == "pdb");
  CHECK(detected(test::fixture("science/chem/peptide.cif"), "p.cif") == "mmcif");
}

TEST_CASE("science/cif: fractional coordinates become Cartesian (ASE files)") {
  const Json toy = read_data("cif", "monoclinic.cif");
  const Json& atoms = toy["molecules"][0]["atoms"];
  REQUIRE(atoms.size() == 2);
  // Positions reported by ASE for the same cell.
  CHECK_APPROX(atoms[0]["x"].as_double(), 0.087433, 1e-5);
  CHECK_APPROX(atoms[0]["y"].as_double(), 1.0, 1e-9);
  CHECK_APPROX(atoms[0]["z"].as_double(), 1.772654, 1e-5);
  CHECK_APPROX(atoms[1]["x"].as_double(), 0.974867, 1e-5);
  CHECK_APPROX(atoms[1]["z"].as_double(), 3.545308, 1e-5);
  CHECK(atoms[0]["label"].as_string() == "C1");
  const Json nacl = read_data("cif", "nacl.cif");
  CHECK(nacl["molecules"][0]["atoms"].size() == 2);
  CHECK(nacl["molecules"][0]["atoms"][1]["element"].as_string() == "Cl");
  // Writing keeps the cell and returns the same fractional coordinates.
  const auto written = test::write_from("cif", ir::Data{toy});
  REQUIRE_OK(written);
  CHECK(test::str(*written).find("C1 C 0.10000 0.20000 0.30000") != std::string::npos);
  CHECK(detected(test::fixture("science/chem/nacl.cif"), "x.cif") == "cif");
}

TEST_CASE("science/chem: hostile input") {
  CHECK_STATUS(test::read_as<ir::Data>("xyz", as_bytes("5\nfive atoms\nC 0 0 0\n")), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Data>("mol", as_bytes("x\n\n\n  1  1  0  0  0  0  0  0  0  0999 V2000\n"
                                                         "    0.0000    0.0000    0.0000 C   0  0\n  1  5  1  0\nM  END\n")),
               Status::Decode);
  CHECK_STATUS(test::read_as<ir::Data>("cif", as_bytes("data_x\n_cell_length_a 'unterminated\n")), Status::Decode);
  for (const auto& [format, name] : std::vector<std::pair<std::string, std::string>>{
           {"sdf", "three.sdf"}, {"mol", "acetate_v3000.mol"}, {"xyz", "ethanol.xyz"}, {"mol2", "ethanol.mol2"},
           {"cml", "ethanol.cml"}, {"pdb", "peptide.pdb"}, {"mmcif", "peptide.cif"}, {"cif", "monoclinic.cif"}}) {
    CHECK(test::fuzz_reader<ir::Data>(format, test::fixture("science/chem/" + name), 256) > 0);
  }
}
