// Convertible core - science family: the atoms/bonds record model shared by
// the chemistry and structural-biology formats.
//
// Data: {"molecules":[{"name":"...","atoms":[{"element":"C","x":0,"y":0,"z":0,
// "charge":0, ...format-specific fields...}],"bonds":[{"from":1,"to":2,
// "order":1}],"properties":{...}}]}. Bond ends are 1-based atom indices;
// order 1/2/3, 4 = aromatic.
// Table: sheet "atoms" (molecule, index, element, x, y, z, charge, extras...),
// sheet "bonds" (molecule, from, to, order), sheet "properties" (molecule,
// key, value).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <string>
#include <vector>

#include "families/science/common.hpp"

namespace cvt::science::chem {

struct Atom {
  std::string element;
  double x = 0;
  double y = 0;
  double z = 0;
  int charge = 0;
  Json extra = Json::object();  ///< ordered format-specific fields (name, residue, chain, ...)
};

struct Bond {
  std::size_t from = 0;  ///< 1-based
  std::size_t to = 0;
  int order = 1;
};

struct Molecule {
  std::string name;
  std::vector<Atom> atoms;
  std::vector<Bond> bonds;
  Json properties = Json::object();
};

[[nodiscard]] Json molecules_to_json(const std::vector<Molecule>& molecules);
[[nodiscard]] Result<std::vector<Molecule>> json_to_molecules(const Json& value);
[[nodiscard]] ir::Table molecules_to_table(const std::vector<Molecule>& molecules);
[[nodiscard]] Result<std::vector<Molecule>> table_to_molecules(const ir::Table& table);

/// Element symbol with canonical case ("CL" -> "Cl"); empty when unknown.
[[nodiscard]] std::string normalize_element(std::string_view symbol);
/// Atomic number of an element symbol, 0 when unknown.
[[nodiscard]] int atomic_number(std::string_view symbol);
/// Symbol of an atomic number, empty when out of range.
[[nodiscard]] std::string_view element_name(int z);

using MoleculeReader = std::function<Result<std::vector<Molecule>>(std::string_view text, ReadContext& ctx)>;
using MoleculeWriter = std::function<Result<std::string>(const std::vector<Molecule>& molecules, WriteContext& ctx)>;
/// Register Data and Table codecs for a molecule format.
void add_molecule_codecs(Registry& registry, const std::string& id, MoleculeReader read, MoleculeWriter write,
                         std::vector<OptionSpec> write_options = {});

// --------------------------------------------------------------------------- CIF syntax

/// One data block of a CIF/mmCIF/STAR file.
struct CifBlock {
  std::string name;
  std::vector<std::pair<std::string, std::string>> items;  ///< tag (with leading '_') -> value
  struct Loop {
    std::vector<std::string> tags;
    std::vector<std::vector<std::string>> rows;
  };
  std::vector<Loop> loops;
  [[nodiscard]] const std::string* item(std::string_view tag) const;
  [[nodiscard]] const Loop* loop_with(std::string_view tag) const;
};
[[nodiscard]] Result<std::vector<CifBlock>> parse_cif(std::string_view text, const Context& ctx);
/// Quote a value for CIF output when needed.
[[nodiscard]] std::string cif_value(std::string_view value);

}  // namespace cvt::science::chem
