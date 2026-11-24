// Convertible core - science family: macromolecular structures (PDB and
// PDBx/mmCIF) on the atoms/bonds model. Atom extras: record, serial, name,
// alt_loc, residue, chain, residue_number, insertion_code, occupancy,
// b_factor. Each MODEL becomes one molecule.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <format>
#include <map>

#include "families/science/chem.hpp"
#include "families/science/science.hpp"
#include "util/text.hpp"

namespace cvt::science::chem {

namespace {

std::string_view field(std::string_view line, std::size_t from, std::size_t to) {  // 1-based inclusive columns
  if (from - 1 >= line.size()) return {};
  return util::trim(line.substr(from - 1, std::min(to, line.size()) - from + 1));
}

std::string guess_element(std::string_view atom_name, std::string_view residue) {
  // Columns 13-14 hold the element right-justified for standard atoms.
  std::string letters;
  for (const char c : atom_name) {
    if (util::is_ascii_alpha(c)) letters.push_back(c);
  }
  if (letters.empty()) return "";
  const bool hetero = residue != "" && residue.size() <= 3;
  if (letters.size() >= 2 && !hetero) return normalize_element(letters.substr(0, 1));
  if (letters.size() >= 2) {
    const std::string two = normalize_element(letters.substr(0, 2));
    if (!two.empty() && (atom_name.size() < 4 || atom_name[0] != ' ')) return two;
  }
  return normalize_element(letters.substr(0, 1));
}

Result<std::vector<Molecule>> read_pdb(std::string_view text, ReadContext& ctx) {
  const auto lines = lines_of(text);
  std::vector<Molecule> molecules(1);
  Json properties = Json::object();
  std::map<std::int64_t, std::size_t> serial_index;
  std::map<std::pair<std::size_t, std::size_t>, int> conect;
  bool in_model = false;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if ((i & 4095) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(lines.size())));
    const std::string_view line = lines[i];
    const std::string_view record = util::trim(line.substr(0, std::min<std::size_t>(6, line.size())));
    if (record == "MODEL") {
      if (in_model || !molecules.back().atoms.empty()) molecules.emplace_back();
      molecules.back().name = "model " + std::string(field(line, 11, 14));
      in_model = true;
      serial_index.clear();
    } else if (record == "ENDMDL") {
      in_model = false;
    } else if (record == "ATOM" || record == "HETATM") {
      Molecule& m = molecules.back();
      Atom a;
      const auto x = to_double(field(line, 31, 38));
      const auto y = to_double(field(line, 39, 46));
      const auto z = to_double(field(line, 47, 54));
      if (!x || !y || !z) return fail(Status::Decode, "bad coordinates on PDB line {}", i + 1);
      a.x = *x;
      a.y = *y;
      a.z = *z;
      const std::string_view name_raw = line.size() >= 16 ? line.substr(12, 4) : std::string_view{};
      const std::string_view residue = field(line, 18, 20);
      a.element = normalize_element(field(line, 77, 78));
      if (a.element.empty()) a.element = guess_element(name_raw, residue);
      const std::string_view charge = field(line, 79, 80);
      if (charge.size() == 2 && util::is_ascii_digit(charge[0])) {
        a.charge = (charge[0] - '0') * (charge[1] == '-' ? -1 : 1);
      }
      const auto serial = to_int(field(line, 7, 11));
      a.extra["record"] = std::string(record);
      a.extra["serial"] = serial.value_or(0);
      a.extra["name"] = std::string(field(line, 13, 16));
      a.extra["alt_loc"] = std::string(field(line, 17, 17));
      a.extra["residue"] = std::string(residue);
      a.extra["chain"] = std::string(field(line, 22, 22));
      a.extra["residue_number"] = to_int(field(line, 23, 26)).value_or(0);
      a.extra["insertion_code"] = std::string(field(line, 27, 27));
      a.extra["occupancy"] = json_number(to_double(field(line, 55, 60)).value_or(1.0));
      a.extra["b_factor"] = json_number(to_double(field(line, 61, 66)).value_or(0.0));
      if (serial) serial_index[*serial] = m.atoms.size() + 1;
      m.atoms.push_back(std::move(a));
      CVT_TRY(check_cells(ctx, m.atoms.size() * 16));
    } else if (record == "CONECT") {
      const auto from = to_int(field(line, 7, 11));
      if (!from) continue;
      for (std::size_t col = 12; col + 4 <= 31 && col <= line.size(); col += 5) {
        const auto to = to_int(field(line, col, col + 4));
        if (!to) continue;
        const auto a = serial_index.find(*from);
        const auto b = serial_index.find(*to);
        if (a == serial_index.end() || b == serial_index.end()) continue;
        ++conect[{a->second, b->second}];  // per direction
      }
    } else if (record == "HEADER") {
      properties["header"] = std::string(field(line, 11, 50));
      if (const auto id = field(line, 63, 66); !id.empty()) properties["id_code"] = std::string(id);
    } else if (record == "TITLE" || record == "COMPND" || record == "KEYWDS" || record == "EXPDTA" ||
               record == "AUTHOR" || record == "REMARK" || record == "SOURCE") {
      if (record == "REMARK" || record == "AUTHOR") continue;  // free text, often personal
      const std::string key = util::to_lower(record);
      std::string value = properties[key].as_string();
      if (!value.empty()) value += ' ';
      value += field(line, 11, 80);
      properties[key] = value;
    } else if (record == "CRYST1") {
      properties["cryst1"] = std::string(util::trim(line.substr(std::min<std::size_t>(6, line.size()))));
    }
  }
  if (molecules.size() == 1 && molecules[0].atoms.empty()) return fail(Status::Decode, "no ATOM/HETATM records found");
  // CONECT may list a bond from one or both ends; a partner repeated within
  // one atom's records means a higher bond order.
  for (const auto& [pair, count] : conect) {
    if (pair.first > pair.second && conect.contains({pair.second, pair.first})) continue;
    int order = count;
    if (const auto back = conect.find({pair.second, pair.first}); back != conect.end()) order = std::max(order, back->second);
    molecules.front().bonds.push_back(Bond{std::min(pair.first, pair.second), std::max(pair.first, pair.second),
                                           std::clamp(order, 1, 3)});
  }
  for (Molecule& m : molecules) {
    m.properties = properties;
    if (m.name.empty()) m.name = properties["id_code"].as_string();
  }
  return molecules;
}

std::string pdb_atom_name(std::string name, const std::string& element) {
  if (name.size() >= 4) return name.substr(0, 4);
  // Single-letter elements start in column 14.
  if (element.size() <= 1 && name.size() < 4) name = " " + name;
  name.resize(4, ' ');
  return name;
}

Result<std::string> write_pdb(const std::vector<Molecule>& molecules, WriteContext& /*ctx*/) {
  std::string out;
  const Json& properties = molecules.front().properties;
  if (const Json* title = properties.find("title")) {
    const std::string text = title->as_string();
    for (std::size_t at = 0, n = 1; at < text.size(); at += 70, ++n) {
      out += n == 1 ? std::format("TITLE     {}\n", text.substr(at, 70)) : std::format("TITLE  {:>3}{}\n", n, text.substr(at, 70));
    }
  }
  if (const Json* cryst = properties.find("cryst1")) out += "CRYST1" + cryst->as_string().substr(0, 60) + "\n";
  const bool models = molecules.size() > 1;
  for (std::size_t mi = 0; mi < molecules.size(); ++mi) {
    const Molecule& m = molecules[mi];
    if (m.atoms.size() > 99999) return fail(Status::Encode, "PDB files hold at most 99999 atoms per model");
    if (models) out += std::format("MODEL     {:>4}\n", mi + 1);
    for (std::size_t i = 0; i < m.atoms.size(); ++i) {
      const Atom& a = m.atoms[i];
      if (std::abs(a.x) >= 1e4 || std::abs(a.y) >= 1e4 || std::abs(a.z) >= 1e4) {
        return fail(Status::Encode, "coordinate out of the PDB range (-9999.999..9999.999)");
      }
      std::string record = a.extra["record"].as_string();
      if (record != "ATOM" && record != "HETATM") record = "HETATM";
      std::string name = a.extra["name"].as_string();
      if (name.empty()) name = a.element.empty() ? "X" : a.element;
      std::string residue = a.extra["residue"].as_string();
      if (residue.empty()) residue = "UNL";
      std::string chain = a.extra["chain"].as_string();
      if (chain.empty()) chain = "A";
      std::string charge;
      if (a.charge != 0) charge = std::to_string(std::abs(a.charge)) + (a.charge < 0 ? "-" : "+");
      out += std::format("{:<6}{:>5} {}{:1}{:>3} {:1}{:>4}{:1}   {:>8}{:>8}{:>8}{:>6}{:>6}          {:>2}{:<2}\n",
                         record, i + 1, pdb_atom_name(name, a.element), a.extra["alt_loc"].as_string().substr(0, 1),
                         residue.substr(0, 3), chain.substr(0, 1), a.extra["residue_number"].as_int64(1) % 10000,
                         a.extra["insertion_code"].as_string().substr(0, 1), fmt_fixed(a.x, 3), fmt_fixed(a.y, 3),
                         fmt_fixed(a.z, 3), fmt_fixed(a.extra["occupancy"].as_double(1.0), 2),
                         fmt_fixed(a.extra["b_factor"].as_double(0.0), 2), util::to_upper(a.element), charge);
    }
    if (models) out += "ENDMDL\n";
  }
  // CONECT records (each bond listed from both ends, repeated for its order).
  const Molecule& first = molecules.front();
  std::map<std::size_t, std::vector<std::size_t>> partners;
  for (const Bond& b : first.bonds) {
    const int repeats = b.order >= 1 && b.order <= 3 ? b.order : 1;
    for (int r = 0; r < repeats; ++r) {
      partners[b.from].push_back(b.to);
      partners[b.to].push_back(b.from);
    }
  }
  for (const auto& [atom, list] : partners) {
    for (std::size_t at = 0; at < list.size(); at += 4) {
      std::string line = std::format("CONECT{:>5}", atom);
      for (std::size_t k = at; k < std::min(at + 4, list.size()); ++k) line += std::format("{:>5}", list[k]);
      out += line + "\n";
    }
  }
  out += "END\n";
  return out;
}

int sniff_pdb(ByteView data, std::string_view name) {
  const auto lines = lines_of(head_text(data, 8192));
  int records = 0;
  for (const auto line : lines) {
    for (const std::string_view r : {"HEADER", "ATOM  ", "HETATM", "CRYST1", "COMPND", "REMARK", "MODEL ", "TITLE "}) {
      if (line.starts_with(r)) {
        ++records;
        break;
      }
    }
  }
  if (records == 0) return 0;
  const bool atoms = head_text(data, 65536).find("\nATOM  ") != std::string_view::npos ||
                     head_text(data, 65536).find("\nHETATM") != std::string_view::npos ||
                     head_text(data, 16).starts_with("ATOM  ") || head_text(data, 16).starts_with("HETATM");
  if (!atoms) return 0;
  return has_extension(name, "pdb") || has_extension(name, "ent") ? 93 : 80;
}

// --------------------------------------------------------------------------- mmCIF

Result<std::vector<Molecule>> read_mmcif(std::string_view text, ReadContext& ctx) {
  CVT_TRY_ASSIGN(const auto blocks, parse_cif(text, ctx));
  std::vector<Molecule> molecules;
  for (const CifBlock& block : blocks) {
    const CifBlock::Loop* sites = block.loop_with("_atom_site.Cartn_x");
    if (sites == nullptr) continue;
    const auto col = [&](std::string_view tag) -> int {
      for (std::size_t i = 0; i < sites->tags.size(); ++i) {
        if (util::iequals(sites->tags[i], tag)) return static_cast<int>(i);
      }
      return -1;
    };
    const auto first_col = [&](std::initializer_list<std::string_view> tags) {
      for (const auto t : tags) {
        if (const int c = col(t); c >= 0) return c;
      }
      return -1;
    };
    const int c_group = col("_atom_site.group_PDB");
    const int c_id = col("_atom_site.id");
    const int c_type = col("_atom_site.type_symbol");
    const int c_name = first_col({"_atom_site.auth_atom_id", "_atom_site.label_atom_id"});
    const int c_alt = col("_atom_site.label_alt_id");
    const int c_res = first_col({"_atom_site.auth_comp_id", "_atom_site.label_comp_id"});
    const int c_chain = first_col({"_atom_site.auth_asym_id", "_atom_site.label_asym_id"});
    const int c_seq = first_col({"_atom_site.auth_seq_id", "_atom_site.label_seq_id"});
    const int c_ins = col("_atom_site.pdbx_PDB_ins_code");
    const int c_x = col("_atom_site.Cartn_x"), c_y = col("_atom_site.Cartn_y"), c_z = col("_atom_site.Cartn_z");
    const int c_occ = col("_atom_site.occupancy");
    const int c_b = col("_atom_site.B_iso_or_equiv");
    const int c_charge = col("_atom_site.pdbx_formal_charge");
    const int c_model = col("_atom_site.pdbx_PDB_model_num");
    std::map<std::string, std::size_t> model_index;
    const std::string* title = block.item("_struct.title");
    for (const auto& row : sites->rows) {
      const auto get = [&row](int c) -> std::string {
        if (c < 0) return {};
        const std::string& v = row[static_cast<std::size_t>(c)];
        return v == "?" || v == "." ? std::string() : v;
      };
      const std::string model = get(c_model);
      auto it = model_index.find(model);
      if (it == model_index.end()) {
        it = model_index.emplace(model, molecules.size()).first;
        molecules.emplace_back();
        molecules.back().name = model.empty() ? block.name : block.name + " model " + model;
        if (title != nullptr) molecules.back().properties["title"] = *title;
        molecules.back().properties["id_code"] = block.name;
      }
      Molecule& m = molecules[it->second];
      Atom a;
      const auto x = to_double(get(c_x)), y = to_double(get(c_y)), z = to_double(get(c_z));
      if (!x || !y || !z) return fail(Status::Decode, "bad mmCIF coordinates");
      a.x = *x;
      a.y = *y;
      a.z = *z;
      a.element = normalize_element(get(c_type));
      if (a.element.empty()) a.element = get(c_type);
      a.charge = static_cast<int>(to_int(get(c_charge)).value_or(0));
      a.extra["record"] = get(c_group).empty() ? std::string("ATOM") : get(c_group);
      a.extra["serial"] = to_int(get(c_id)).value_or(static_cast<std::int64_t>(m.atoms.size() + 1));
      a.extra["name"] = get(c_name);
      a.extra["alt_loc"] = get(c_alt);
      a.extra["residue"] = get(c_res);
      a.extra["chain"] = get(c_chain);
      a.extra["residue_number"] = to_int(get(c_seq)).value_or(0);
      a.extra["insertion_code"] = get(c_ins);
      a.extra["occupancy"] = json_number(to_double(get(c_occ)).value_or(1.0));
      a.extra["b_factor"] = json_number(to_double(get(c_b)).value_or(0.0));
      m.atoms.push_back(std::move(a));
    }
  }
  if (molecules.empty()) return fail(Status::Decode, "no _atom_site loop in the mmCIF file");
  return molecules;
}

Result<std::string> write_mmcif(const std::vector<Molecule>& molecules, WriteContext& /*ctx*/) {
  std::string id = molecules.front().properties["id_code"].as_string();
  if (id.empty()) id = molecules.front().name.empty() ? "structure" : molecules.front().name;
  std::replace_if(id.begin(), id.end(), [](char c) { return util::is_ascii_space(c); }, '_');
  std::string out = "data_" + id + "\n#\n_entry.id " + cif_value(id) + "\n#\n";
  if (const Json* title = molecules.front().properties.find("title")) {
    out += "_struct.title " + cif_value(title->as_string()) + "\n#\n";
  }
  out +=
      "loop_\n_atom_site.group_PDB\n_atom_site.id\n_atom_site.type_symbol\n_atom_site.label_atom_id\n"
      "_atom_site.label_alt_id\n_atom_site.label_comp_id\n_atom_site.label_asym_id\n_atom_site.label_seq_id\n"
      "_atom_site.pdbx_PDB_ins_code\n_atom_site.Cartn_x\n_atom_site.Cartn_y\n_atom_site.Cartn_z\n"
      "_atom_site.occupancy\n_atom_site.B_iso_or_equiv\n_atom_site.pdbx_formal_charge\n"
      "_atom_site.auth_seq_id\n_atom_site.auth_asym_id\n_atom_site.pdbx_PDB_model_num\n";
  std::size_t serial = 0;
  for (std::size_t mi = 0; mi < molecules.size(); ++mi) {
    for (const Atom& a : molecules[mi].atoms) {
      const auto value = [](const Json& v) { return cif_value(v.is_string() ? v.as_string() : util::dump_json(v)); };
      const auto or_dot = [](const std::string& s) { return s.empty() ? std::string(".") : cif_value(s); };
      std::string record = a.extra["record"].as_string();
      if (record != "ATOM" && record != "HETATM") record = "HETATM";
      std::string name = a.extra["name"].as_string();
      if (name.empty()) name = a.element.empty() ? "X" : a.element;
      std::string residue = a.extra["residue"].as_string();
      if (residue.empty()) residue = "UNL";
      std::string chain = a.extra["chain"].as_string();
      if (chain.empty()) chain = "A";
      const std::string seq = std::to_string(a.extra["residue_number"].as_int64(1));
      out += std::format("{} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {}\n", record, ++serial,
                         a.element.empty() ? "X" : a.element, cif_value(name), or_dot(a.extra["alt_loc"].as_string()),
                         cif_value(residue), cif_value(chain), seq, or_dot(a.extra["insertion_code"].as_string()),
                         fmt_fixed(a.x, 3), fmt_fixed(a.y, 3), fmt_fixed(a.z, 3),
                         fmt_fixed(a.extra["occupancy"].as_double(1.0), 2), fmt_fixed(a.extra["b_factor"].as_double(0), 2),
                         a.charge == 0 ? std::string("?") : std::to_string(a.charge), seq, cif_value(chain), mi + 1);
      (void)value;
    }
  }
  out += "#\n";
  return out;
}

int sniff_mmcif(ByteView data, std::string_view /*name*/) {
  const std::string_view head = head_text(data, 65536);
  if (!head.starts_with("data_") && head.find("\ndata_") == std::string_view::npos) return 0;
  return head.find("_atom_site.") != std::string_view::npos || head.find("_entry.id") != std::string_view::npos ? 93 : 0;
}

}  // namespace
}  // namespace cvt::science::chem

namespace cvt::science {

void register_structures(Registry& registry) {
  using namespace chem;
  add_format(registry, {.id = "pdb",
                        .name = "Protein Data Bank file",
                        .description = "Macromolecular structure in the legacy PDB format.",
                        .extensions = {"pdb", "ent"},
                        .mime = "chemical/x-pdb",
                        .popularity = 35,
                        .sniff = sniff_pdb});
  add_molecule_codecs(registry, "pdb", read_pdb, write_pdb);
  add_format(registry, {.id = "mmcif",
                        .name = "PDBx/mmCIF structure",
                        .description = "Macromolecular structure in the PDBx/mmCIF format.",
                        .extensions = {"cif", "mmcif"},
                        .mime = "chemical/x-mmcif",
                        .popularity = 25,
                        .sniff = sniff_mmcif});
  add_molecule_codecs(registry, "mmcif", read_mmcif, write_mmcif);
}

}  // namespace cvt::science
