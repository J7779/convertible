// Convertible core - science family: small-molecule formats on the shared
// atoms/bonds model: XYZ, MDL Molfile/SDF (V2000 read+write, V3000 read),
// Tripos MOL2, Chemical Markup Language, crystallographic CIF.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

#include "families/science/chem.hpp"
#include "families/science/science.hpp"
#include "util/text.hpp"
#include "util/xml.hpp"

namespace cvt::science::chem {

namespace {

Result<double> number_field(std::string_view text, std::string_view what) {
  const auto value = to_double(text);
  if (!value || !std::isfinite(*value)) return fail(Status::Decode, "bad {} '{}'", what, util::trim(text));
  return *value;
}

std::string_view column(std::string_view line, std::size_t start, std::size_t width) {
  if (start >= line.size()) return {};
  return line.substr(start, std::min(width, line.size() - start));
}

std::string element_or_label(std::string_view text) {
  std::string e = normalize_element(text);
  return e.empty() ? std::string(util::trim(text)) : e;
}

// --------------------------------------------------------------------------- XYZ

Result<std::vector<Molecule>> read_xyz(std::string_view text, ReadContext& ctx) {
  const auto lines = lines_of(text);
  std::vector<Molecule> molecules;
  std::size_t i = 0;
  while (i < lines.size()) {
    if (util::trim(lines[i]).empty()) {
      ++i;
      continue;
    }
    const auto count = to_int(lines[i]);
    if (!count || *count < 0) return fail(Status::Decode, "XYZ frame must start with an atom count (line {})", i + 1);
    if (static_cast<std::uint64_t>(*count) + 2 > lines.size() - i) return fail(Status::Decode, "XYZ file is truncated");
    Molecule m;
    m.name = std::string(util::trim(lines[i + 1]));
    for (std::int64_t a = 0; a < *count; ++a) {
      const auto parts = words(lines[i + 2 + static_cast<std::size_t>(a)]);
      if (parts.size() < 4) return fail(Status::Decode, "XYZ atom line {} needs an element and three coordinates", i + 3 + a);
      Atom atom;
      if (const auto z = to_int(parts[0]); z && *z > 0 && *z <= 118) {
        atom.element = std::string(element_name(static_cast<int>(*z)));
      } else {
        atom.element = element_or_label(parts[0]);
      }
      CVT_TRY_ASSIGN(atom.x, number_field(parts[1], "x coordinate"));
      CVT_TRY_ASSIGN(atom.y, number_field(parts[2], "y coordinate"));
      CVT_TRY_ASSIGN(atom.z, number_field(parts[3], "z coordinate"));
      m.atoms.push_back(std::move(atom));
    }
    i += 2 + static_cast<std::size_t>(*count);
    molecules.push_back(std::move(m));
    CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(lines.size())));
  }
  if (molecules.empty()) return fail(Status::Decode, "no XYZ frames found");
  return molecules;
}

Result<std::string> write_xyz(const std::vector<Molecule>& molecules, WriteContext& ctx) {
  const int decimals = static_cast<int>(ctx.options.get_int("decimals", 5));
  std::string out;
  for (const Molecule& m : molecules) {
    out += std::to_string(m.atoms.size()) + '\n';
    std::string comment = m.name;
    std::replace(comment.begin(), comment.end(), '\n', ' ');
    out += comment + '\n';
    for (const Atom& a : m.atoms) {
      out += std::format("{:<2} {:>14} {:>14} {:>14}\n", a.element.empty() ? "X" : a.element, fmt_fixed(a.x, decimals),
                         fmt_fixed(a.y, decimals), fmt_fixed(a.z, decimals));
    }
  }
  return out;
}

int sniff_xyz(ByteView data, std::string_view name) {
  const auto lines = lines_of(head_text(data, 2048));
  if (lines.size() < 3) return 0;
  const auto count = to_int(lines[0]);
  if (!count || *count <= 0) return 0;
  const auto parts = words(lines[2]);
  if (parts.size() < 4 || (normalize_element(parts[0]).empty() && !to_int(parts[0]))) return 0;
  for (std::size_t k = 1; k < 4; ++k) {
    if (!to_double(parts[k])) return 0;
  }
  return has_extension(name, "xyz") ? 92 : 70;
}

// --------------------------------------------------------------------------- MDL Molfile / SDF

int charge_from_code(int code) {
  switch (code) {
    case 1: return 3;
    case 2: return 2;
    case 3: return 1;
    case 5: return -1;
    case 6: return -2;
    case 7: return -3;
    default: return 0;
  }
}

/// V3000: "M  V30 " lines with '-' continuations joined.
std::vector<std::string> v30_lines(const std::vector<std::string_view>& lines, std::size_t& i) {
  std::vector<std::string> out;
  std::string pending;
  for (; i < lines.size(); ++i) {
    const std::string_view line = lines[i];
    if (line.starts_with("M  END")) break;
    if (!line.starts_with("M  V30 ")) continue;
    std::string body(util::trim_right(line.substr(7)));
    if (body.ends_with('-')) {
      body.pop_back();
      pending += body;
      continue;
    }
    out.push_back(pending + body);
    pending.clear();
  }
  return out;
}

Result<void> read_v3000(const std::vector<std::string_view>& lines, std::size_t& i, Molecule& m) {
  const auto body = v30_lines(lines, i);
  std::string section;
  for (const std::string& line : body) {
    const auto parts = words(line);
    if (parts.empty()) continue;
    if (parts[0] == "BEGIN" && parts.size() > 1) {
      section = std::string(parts[1]);
      continue;
    }
    if (parts[0] == "END") {
      section.clear();
      continue;
    }
    if (section == "ATOM") {
      if (parts.size() < 6) return fail(Status::Decode, "malformed V3000 atom line");
      Atom atom;
      atom.element = element_or_label(parts[1]);
      CVT_TRY_ASSIGN(atom.x, number_field(parts[2], "x coordinate"));
      CVT_TRY_ASSIGN(atom.y, number_field(parts[3], "y coordinate"));
      CVT_TRY_ASSIGN(atom.z, number_field(parts[4], "z coordinate"));
      for (std::size_t k = 6; k < parts.size(); ++k) {
        if (parts[k].starts_with("CHG=")) atom.charge = static_cast<int>(to_int(parts[k].substr(4)).value_or(0));
        if (parts[k].starts_with("MASS=")) atom.extra["mass"] = json_number(to_double(parts[k].substr(5)).value_or(0));
      }
      m.atoms.push_back(std::move(atom));
    } else if (section == "BOND") {
      if (parts.size() < 4) return fail(Status::Decode, "malformed V3000 bond line");
      Bond bond;
      bond.order = static_cast<int>(to_int(parts[1]).value_or(1));
      bond.from = static_cast<std::size_t>(std::max<std::int64_t>(0, to_int(parts[2]).value_or(0)));
      bond.to = static_cast<std::size_t>(std::max<std::int64_t>(0, to_int(parts[3]).value_or(0)));
      m.bonds.push_back(bond);
    }
  }
  return {};
}

Result<std::vector<Molecule>> read_sdf(std::string_view text, ReadContext& ctx) {
  const auto lines = lines_of(text);
  std::vector<Molecule> molecules;
  std::size_t i = 0;
  while (i < lines.size()) {
    // Skip blank lines between records only when nothing else follows.
    std::size_t probe = i;
    while (probe < lines.size() && util::trim(lines[probe]).empty()) ++probe;
    if (probe >= lines.size()) break;
    if (i + 3 >= lines.size()) return fail(Status::Decode, "Molfile header is truncated");
    Molecule m;
    m.name = std::string(util::trim(lines[i]));
    const std::string_view counts = lines[i + 3];
    i += 4;
    if (counts.find("V3000") != std::string_view::npos) {
      CVT_TRY(read_v3000(lines, i, m));
    } else {
      const auto n_atoms = to_int(column(counts, 0, 3));
      const auto n_bonds = to_int(column(counts, 3, 3));
      if (!n_atoms || !n_bonds || *n_atoms < 0 || *n_bonds < 0) return fail(Status::Decode, "bad Molfile counts line");
      if (static_cast<std::uint64_t>(*n_atoms + *n_bonds) > lines.size() - i) {
        return fail(Status::Decode, "Molfile atom/bond block is truncated");
      }
      for (std::int64_t a = 0; a < *n_atoms; ++a) {
        const std::string_view line = lines[i++];
        Atom atom;
        CVT_TRY_ASSIGN(atom.x, number_field(column(line, 0, 10), "x coordinate"));
        CVT_TRY_ASSIGN(atom.y, number_field(column(line, 10, 10), "y coordinate"));
        CVT_TRY_ASSIGN(atom.z, number_field(column(line, 20, 10), "z coordinate"));
        atom.element = element_or_label(column(line, 31, 3));
        atom.charge = charge_from_code(static_cast<int>(to_int(column(line, 36, 3)).value_or(0)));
        m.atoms.push_back(std::move(atom));
      }
      for (std::int64_t b = 0; b < *n_bonds; ++b) {
        const std::string_view line = lines[i++];
        Bond bond;
        bond.from = static_cast<std::size_t>(std::max<std::int64_t>(0, to_int(column(line, 0, 3)).value_or(0)));
        bond.to = static_cast<std::size_t>(std::max<std::int64_t>(0, to_int(column(line, 3, 3)).value_or(0)));
        bond.order = static_cast<int>(to_int(column(line, 6, 3)).value_or(1));
        m.bonds.push_back(bond);
      }
      // Properties block: M  CHG overrides the atom-line charges.
      bool charges_reset = false;
      for (; i < lines.size(); ++i) {
        const std::string_view line = lines[i];
        if (line.starts_with("M  END")) break;
        if (line.starts_with("M  CHG")) {
          if (!charges_reset) {
            for (Atom& a : m.atoms) a.charge = 0;
            charges_reset = true;
          }
          const auto parts = words(line.substr(6));
          for (std::size_t k = 1; k + 1 < parts.size(); k += 2) {
            const auto index = to_int(parts[k]);
            if (index && *index >= 1 && static_cast<std::size_t>(*index) <= m.atoms.size()) {
              m.atoms[static_cast<std::size_t>(*index - 1)].charge = static_cast<int>(to_int(parts[k + 1]).value_or(0));
            }
          }
        }
      }
    }
    for (const Bond& b : m.bonds) {
      if (b.from == 0 || b.to == 0 || b.from > m.atoms.size() || b.to > m.atoms.size()) {
        return fail(Status::Decode, "Molfile bond {}-{} refers to a missing atom", b.from, b.to);
      }
    }
    if (i < lines.size()) ++i;  // M  END
    // SD data items up to $$$$.
    std::string key;
    std::string value;
    const auto flush = [&]() {
      if (!key.empty()) m.properties[key] = value;
      key.clear();
      value.clear();
    };
    for (; i < lines.size(); ++i) {
      const std::string_view line = lines[i];
      if (line.starts_with("$$$$")) {
        ++i;
        break;
      }
      if (line.starts_with('>')) {
        flush();
        const std::size_t open = line.find('<');
        const std::size_t close = line.find('>', open == std::string_view::npos ? 1 : open);
        if (open != std::string_view::npos && close != std::string_view::npos) key = std::string(line.substr(open + 1, close - open - 1));
        continue;
      }
      if (line.empty()) {
        flush();
        continue;
      }
      if (!key.empty()) {
        if (!value.empty()) value += '\n';
        value += line;
      }
    }
    flush();
    molecules.push_back(std::move(m));
    CVT_TRY(check_cells(ctx, molecules.size() * 4));
    CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(lines.size())));
  }
  if (molecules.empty()) return fail(Status::Decode, "no molecules in the Molfile/SDF data");
  return molecules;
}

Result<std::string> molblock(const Molecule& m) {
  if (m.atoms.size() > 999 || m.bonds.size() > 999) {
    return fail(Status::Encode, "V2000 Molfiles hold at most 999 atoms and 999 bonds");
  }
  std::string name = m.name;
  std::replace(name.begin(), name.end(), '\n', ' ');
  // Line 2 carries only the dimension code (columns 21-22); no program name or date.
  bool flat = true;
  for (const Atom& a : m.atoms) flat = flat && a.z == 0.0;
  std::string out = name.substr(0, 80) + "\n" + std::string(20, ' ') + (flat ? "2D" : "3D") + "\n\n";
  out += std::format("{:>3}{:>3}  0  0  0  0  0  0  0  0999 V2000\n", m.atoms.size(), m.bonds.size());
  for (const Atom& a : m.atoms) {
    std::string symbol = a.element.empty() ? "*" : a.element.substr(0, 3);
    out += std::format("{:>10}{:>10}{:>10} {:<3} 0  0  0  0  0  0  0  0  0  0  0  0\n", fmt_fixed(a.x, 4),
                       fmt_fixed(a.y, 4), fmt_fixed(a.z, 4), symbol);
  }
  for (const Bond& b : m.bonds) out += std::format("{:>3}{:>3}{:>3}  0\n", b.from, b.to, b.order);
  std::vector<std::pair<std::size_t, int>> charges;
  for (std::size_t i = 0; i < m.atoms.size(); ++i) {
    if (m.atoms[i].charge != 0) charges.emplace_back(i + 1, m.atoms[i].charge);
  }
  for (std::size_t at = 0; at < charges.size(); at += 8) {
    const std::size_t n = std::min<std::size_t>(8, charges.size() - at);
    out += std::format("M  CHG{:>3}", n);
    for (std::size_t k = 0; k < n; ++k) out += std::format(" {:>3} {:>3}", charges[at + k].first, charges[at + k].second);
    out += '\n';
  }
  out += "M  END\n";
  return out;
}

Result<std::string> write_mol(const std::vector<Molecule>& molecules, WriteContext& ctx) {
  if (molecules.size() > 1) ctx.warn("A Molfile holds one molecule; only the first was written (use SDF for several).");
  return molblock(molecules.front());
}

Result<std::string> write_sdf(const std::vector<Molecule>& molecules, WriteContext& /*ctx*/) {
  std::string out;
  for (const Molecule& m : molecules) {
    CVT_TRY_ASSIGN(const std::string block, molblock(m));
    out += block;
    for (const auto& [key, value] : m.properties.items()) {
      out += "> <" + key + ">\n" + (value.is_string() ? value.as_string() : util::dump_json(value)) + "\n\n";
    }
    out += "$$$$\n";
  }
  return out;
}

int sniff_molfile(ByteView data, std::string_view name, bool sdf) {
  const auto lines = lines_of(head_text(data, 4096));
  if (lines.size() < 4) return 0;
  if (lines[3].find("V2000") == std::string_view::npos && lines[3].find("V3000") == std::string_view::npos) return 0;
  const bool many = head_text(data, data.size()).find("$$$$") != std::string_view::npos;
  if (sdf) return many || has_extension(name, "sdf") || has_extension(name, "sd") ? 92 : 70;
  return many ? 60 : (has_extension(name, "mol") ? 92 : 80);
}

// --------------------------------------------------------------------------- MOL2

int mol2_order(std::string_view type) {
  if (type == "ar") return 4;
  if (type == "am") return 1;
  return static_cast<int>(to_int(type).value_or(1));
}

Result<std::vector<Molecule>> read_mol2(std::string_view text, ReadContext& ctx) {
  const auto lines = lines_of(text);
  std::vector<Molecule> molecules;
  std::string section;
  std::size_t section_line = 0;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const std::string_view line = util::trim(lines[i]);
    if (line.empty() || line.starts_with('#')) continue;
    if (line.starts_with("@<TRIPOS>")) {
      section = std::string(line.substr(9));
      section_line = 0;
      if (section == "MOLECULE") molecules.emplace_back();
      continue;
    }
    if (molecules.empty()) continue;
    Molecule& m = molecules.back();
    ++section_line;
    if (section == "MOLECULE") {
      if (section_line == 1) m.name = std::string(line);
      if (section_line == 3) m.properties["molecule_type"] = std::string(line);
      if (section_line == 4) m.properties["charge_type"] = std::string(line);
    } else if (section == "ATOM") {
      const auto parts = words(line);
      if (parts.size() < 6) return fail(Status::Decode, "malformed MOL2 atom line {}", i + 1);
      Atom atom;
      atom.extra["name"] = std::string(parts[1]);
      CVT_TRY_ASSIGN(atom.x, number_field(parts[2], "x coordinate"));
      CVT_TRY_ASSIGN(atom.y, number_field(parts[3], "y coordinate"));
      CVT_TRY_ASSIGN(atom.z, number_field(parts[4], "z coordinate"));
      const std::string_view type = parts[5];
      atom.extra["atom_type"] = std::string(type);
      atom.element = element_or_label(type.substr(0, type.find('.')));
      if (parts.size() > 7) atom.extra["substructure"] = std::string(parts[7]);
      if (parts.size() > 8) atom.extra["partial_charge"] = json_number(to_double(parts[8]).value_or(0));
      m.atoms.push_back(std::move(atom));
    } else if (section == "BOND") {
      const auto parts = words(line);
      if (parts.size() < 4) return fail(Status::Decode, "malformed MOL2 bond line {}", i + 1);
      Bond bond;
      bond.from = static_cast<std::size_t>(std::max<std::int64_t>(0, to_int(parts[1]).value_or(0)));
      bond.to = static_cast<std::size_t>(std::max<std::int64_t>(0, to_int(parts[2]).value_or(0)));
      bond.order = mol2_order(parts[3]);
      if (bond.from == 0 || bond.to == 0 || bond.from > m.atoms.size() || bond.to > m.atoms.size()) {
        return fail(Status::Decode, "MOL2 bond {}-{} refers to a missing atom", bond.from, bond.to);
      }
      m.bonds.push_back(bond);
    }
    if ((i & 4095) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(lines.size())));
  }
  if (molecules.empty()) return fail(Status::Decode, "no @<TRIPOS>MOLECULE record");
  CVT_TRY(check_cells(ctx, molecules.size()));
  return molecules;
}

Result<std::string> write_mol2(const std::vector<Molecule>& molecules, WriteContext& /*ctx*/) {
  std::string out;
  for (const Molecule& m : molecules) {
    out += "@<TRIPOS>MOLECULE\n" + (m.name.empty() ? std::string("*****") : m.name) + '\n';
    out += std::format(" {} {} 0 0 0\n", m.atoms.size(), m.bonds.size());
    const std::string type = m.properties["molecule_type"].as_string();
    const std::string charge = m.properties["charge_type"].as_string();
    out += (type.empty() ? "SMALL" : type) + '\n' + (charge.empty() ? "NO_CHARGES" : charge) + "\n\n";
    out += "@<TRIPOS>ATOM\n";
    std::vector<int> counters(119, 0);
    for (std::size_t i = 0; i < m.atoms.size(); ++i) {
      const Atom& a = m.atoms[i];
      std::string name = a.extra["name"].as_string();
      if (name.empty()) {
        const int z = atomic_number(a.element);
        name = (a.element.empty() ? "X" : a.element) + std::to_string(++counters[static_cast<std::size_t>(z)]);
      }
      std::string atom_type = a.extra["atom_type"].as_string();
      if (atom_type.empty()) atom_type = a.element.empty() ? "Du" : a.element;
      std::string substructure = a.extra["substructure"].as_string();
      if (substructure.empty()) substructure = "UNL1";
      const double partial = a.extra["partial_charge"].as_double(static_cast<double>(a.charge));
      out += std::format("{:>7} {:<8}{:>10}{:>10}{:>10} {:<6}{:>5}  {:<8}{:>10}\n", i + 1, name, fmt_fixed(a.x, 4),
                         fmt_fixed(a.y, 4), fmt_fixed(a.z, 4), atom_type, 1, substructure, fmt_fixed(partial, 4));
    }
    out += "@<TRIPOS>BOND\n";
    for (std::size_t i = 0; i < m.bonds.size(); ++i) {
      const Bond& b = m.bonds[i];
      out += std::format("{:>6}{:>6}{:>6} {}\n", i + 1, b.from, b.to, b.order == 4 ? std::string("ar") : std::to_string(b.order));
    }
  }
  return out;
}

int sniff_mol2(ByteView data, std::string_view /*name*/) {
  return head_text(data, 4096).find("@<TRIPOS>MOLECULE") != std::string_view::npos ? 95 : 0;
}

// --------------------------------------------------------------------------- CML

int cml_order(std::string_view order) {
  if (order == "S" || order == "1") return 1;
  if (order == "D" || order == "2") return 2;
  if (order == "T" || order == "3") return 3;
  if (order == "A" || order == "4") return 4;
  return 1;
}

Result<std::vector<Molecule>> read_cml(std::string_view text, ReadContext& ctx) {
  CVT_TRY_ASSIGN(const util::XmlDocument doc, util::parse_xml(text));
  std::vector<Molecule> molecules;
  std::vector<util::XmlNode> found;
  for (const util::XmlNode node : doc.root().descendants()) {
    if (node.is_element() && node.local_name() == "molecule") found.push_back(node);
  }
  if (doc.root() && doc.root().local_name() == "molecule") found.insert(found.begin(), doc.root());
  for (const util::XmlNode node : found) {
    Molecule m;
    m.name = std::string(node.attribute("title") ? node.attribute("title").value() : node.attribute("id").value());
    if (const util::XmlNode name = node.child_local("name")) m.name = name.text();
    std::vector<std::string> ids;
    if (const util::XmlNode atoms = node.child_local("atomArray")) {
      for (const util::XmlNode atom_node : atoms.children_local("atom")) {
        Atom a;
        a.element = element_or_label(atom_node.attribute("elementType").value());
        if (atom_node.attribute("x3")) {
          a.x = atom_node.attribute("x3").as_double();
          a.y = atom_node.attribute("y3").as_double();
          a.z = atom_node.attribute("z3").as_double();
        } else {
          a.x = atom_node.attribute("x2").as_double();
          a.y = atom_node.attribute("y2").as_double();
        }
        a.charge = atom_node.attribute("formalCharge").as_int();
        if (const auto h = atom_node.attribute("hydrogenCount")) a.extra["hydrogen_count"] = h.as_int();
        ids.emplace_back(atom_node.attribute("id").value());
        m.atoms.push_back(std::move(a));
      }
    }
    if (const util::XmlNode bonds = node.child_local("bondArray")) {
      for (const util::XmlNode bond_node : bonds.children_local("bond")) {
        const auto refs = words(bond_node.attribute("atomRefs2").value());
        if (refs.size() != 2) return fail(Status::Decode, "CML bond without two atom references");
        Bond b;
        const auto find = [&ids](std::string_view id) -> std::size_t {
          const auto it = std::find(ids.begin(), ids.end(), id);
          return it == ids.end() ? 0 : static_cast<std::size_t>(it - ids.begin()) + 1;
        };
        b.from = find(refs[0]);
        b.to = find(refs[1]);
        if (b.from == 0 || b.to == 0) return fail(Status::Decode, "CML bond refers to an unknown atom");
        b.order = cml_order(bond_node.attribute("order").value());
        m.bonds.push_back(b);
      }
    }
    for (const util::XmlNode p : node.descendants()) {
      if (!p.is_element() || p.local_name() != "property") continue;
      const std::string title(p.attribute("title") ? p.attribute("title").value() : p.attribute("dictRef").value());
      if (!title.empty()) m.properties[title] = p.text();
    }
    molecules.push_back(std::move(m));
  }
  if (molecules.empty()) return fail(Status::Decode, "no <molecule> element in the CML document");
  CVT_TRY(check_cells(ctx, molecules.size()));
  return molecules;
}

Result<std::string> write_cml(const std::vector<Molecule>& molecules, WriteContext& /*ctx*/) {
  util::XmlDocument doc;
  doc.set_declaration();
  util::XmlNode parent = doc.append_child(molecules.size() == 1 ? "molecule" : "cml");
  parent.set_attribute("xmlns", "http://www.xml-cml.org/schema");
  for (std::size_t mi = 0; mi < molecules.size(); ++mi) {
    const Molecule& m = molecules[mi];
    util::XmlNode mol = molecules.size() == 1 ? parent : parent.append_child("molecule");
    mol.set_attribute("id", "m" + std::to_string(mi + 1));
    if (!m.name.empty()) mol.set_attribute("title", m.name);
    util::XmlNode atoms = mol.append_child("atomArray");
    for (std::size_t i = 0; i < m.atoms.size(); ++i) {
      const Atom& a = m.atoms[i];
      util::XmlNode atom = atoms.append_child("atom");
      atom.set_attribute("id", "a" + std::to_string(i + 1));
      atom.set_attribute("elementType", a.element.empty() ? "Du" : a.element);
      atom.set_attribute("x3", fmt_num(a.x));
      atom.set_attribute("y3", fmt_num(a.y));
      atom.set_attribute("z3", fmt_num(a.z));
      if (a.charge != 0) atom.set_attribute("formalCharge", std::to_string(a.charge));
    }
    if (!m.bonds.empty()) {
      util::XmlNode bonds = mol.append_child("bondArray");
      for (const Bond& b : m.bonds) {
        util::XmlNode bond = bonds.append_child("bond");
        bond.set_attribute("atomRefs2", std::format("a{} a{}", b.from, b.to));
        bond.set_attribute("order", b.order == 4 ? std::string("A") : std::to_string(b.order));
      }
    }
    if (!m.properties.empty()) {
      util::XmlNode list = mol.append_child("propertyList");
      for (const auto& [key, value] : m.properties.items()) {
        util::XmlNode property = list.append_child("property");
        property.set_attribute("title", key);
        property.append_child("scalar").append_text(value.is_string() ? value.as_string() : util::dump_json(value));
      }
    }
  }
  return util::xml_to_string(doc) + "\n";
}

int sniff_cml(ByteView data, std::string_view name) {
  const std::string_view head = head_text(data, 2048);
  if (head.find("<molecule") == std::string_view::npos && head.find("<cml") == std::string_view::npos) return 0;
  if (head.find("xml-cml.org") != std::string_view::npos || head.find("<atomArray") != std::string_view::npos) return 92;
  return has_extension(name, "cml") ? 85 : 0;
}

// --------------------------------------------------------------------------- crystallographic CIF

double cif_number(const std::string* text) {
  if (text == nullptr) return 0;
  std::string_view value = *text;
  if (const std::size_t paren = value.find('('); paren != std::string_view::npos) value = value.substr(0, paren);
  return to_double(value).value_or(0);
}

struct Cell {
  double a = 1, b = 1, c = 1, alpha = 90, beta = 90, gamma = 90;
  /// Fractional -> Cartesian (a along x, b in the xy plane).
  [[nodiscard]] std::array<double, 3> to_cartesian(double u, double v, double w) const {
    const double d = std::numbers::pi / 180.0;
    const double ca = std::cos(alpha * d), cb = std::cos(beta * d), cg = std::cos(gamma * d), sg = std::sin(gamma * d);
    const double volume_term = std::sqrt(std::max(0.0, 1 - ca * ca - cb * cb - cg * cg + 2 * ca * cb * cg));
    const double x = a * u + b * cg * v + c * cb * w;
    const double y = b * sg * v + c * (ca - cb * cg) / sg * w;
    const double z = c * volume_term / sg * w;
    return {x, y, z};
  }
  [[nodiscard]] std::array<double, 3> to_fractional(double x, double y, double z) const {
    const double d = std::numbers::pi / 180.0;
    const double ca = std::cos(alpha * d), cb = std::cos(beta * d), cg = std::cos(gamma * d), sg = std::sin(gamma * d);
    const double volume_term = std::sqrt(std::max(0.0, 1 - ca * ca - cb * cb - cg * cg + 2 * ca * cb * cg));
    const double w = z * sg / (c * volume_term);
    const double v = (y - c * (ca - cb * cg) / sg * w) / (b * sg);
    const double u = (x - b * cg * v - c * cb * w) / a;
    return {u, v, w};
  }
};

std::string strip_su(std::string_view value) {
  if (const std::size_t paren = value.find('('); paren != std::string_view::npos) value = value.substr(0, paren);
  return std::string(value);
}

Result<std::vector<Molecule>> read_cif(std::string_view text, ReadContext& ctx) {
  CVT_TRY_ASSIGN(const auto blocks, parse_cif(text, ctx));
  std::vector<Molecule> molecules;
  for (const CifBlock& block : blocks) {
    const CifBlock::Loop* sites = block.loop_with("_atom_site_label");
    if (sites == nullptr) sites = block.loop_with("_atom_site_fract_x");
    if (sites == nullptr) continue;
    Molecule m;
    m.name = block.name;
    Cell cell;
    cell.a = cif_number(block.item("_cell_length_a"));
    cell.b = cif_number(block.item("_cell_length_b"));
    cell.c = cif_number(block.item("_cell_length_c"));
    cell.alpha = cif_number(block.item("_cell_angle_alpha"));
    cell.beta = cif_number(block.item("_cell_angle_beta"));
    cell.gamma = cif_number(block.item("_cell_angle_gamma"));
    const bool has_cell = cell.a > 0 && cell.b > 0 && cell.c > 0 && cell.alpha > 0 && cell.beta > 0 && cell.gamma > 0;
    for (const auto& [tag, value] : block.items) {
      if (tag.starts_with("_cell_") || tag.starts_with("_symmetry_") || tag.starts_with("_space_group_") ||
          tag.starts_with("_chemical_")) {
        m.properties[tag.substr(1)] = value;
      }
    }
    const auto col = [&](std::string_view tag) -> int {
      for (std::size_t i = 0; i < sites->tags.size(); ++i) {
        if (util::iequals(sites->tags[i], tag)) return static_cast<int>(i);
      }
      return -1;
    };
    const int c_label = col("_atom_site_label");
    const int c_type = col("_atom_site_type_symbol");
    const int fx = col("_atom_site_fract_x"), fy = col("_atom_site_fract_y"), fz = col("_atom_site_fract_z");
    const int cx = col("_atom_site_Cartn_x"), cy = col("_atom_site_Cartn_y"), cz = col("_atom_site_Cartn_z");
    const int c_occ = col("_atom_site_occupancy");
    for (const auto& row : sites->rows) {
      const auto get = [&row](int c) -> std::string { return c < 0 ? std::string() : strip_su(row[static_cast<std::size_t>(c)]); };
      Atom a;
      const std::string label = get(c_label);
      std::string type = get(c_type);
      if (type.empty()) {
        std::size_t n = 0;
        while (n < label.size() && util::is_ascii_alpha(label[n])) ++n;
        type = label.substr(0, std::min<std::size_t>(n, 2));
        if (normalize_element(type).empty()) type = type.substr(0, 1);
      }
      std::string symbol;
      for (const char ch : type) {
        if (!util::is_ascii_alpha(ch)) break;
        symbol.push_back(ch);
      }
      a.element = element_or_label(symbol);
      if (int charge_sign = 0; type.size() > symbol.size()) {
        const std::string_view tail = std::string_view(type).substr(symbol.size());
        charge_sign = tail.ends_with('-') ? -1 : tail.ends_with('+') ? 1 : 0;
        if (charge_sign != 0) a.charge = charge_sign * static_cast<int>(to_int(tail.substr(0, tail.size() - 1)).value_or(1));
      }
      if (!label.empty()) a.extra["label"] = label;
      if (fx >= 0 && fy >= 0 && fz >= 0) {
        const double u = to_double(get(fx)).value_or(0), v = to_double(get(fy)).value_or(0), w = to_double(get(fz)).value_or(0);
        a.extra["fract_x"] = json_number(u);
        a.extra["fract_y"] = json_number(v);
        a.extra["fract_z"] = json_number(w);
        if (!has_cell) return fail(Status::Decode, "fractional coordinates without a complete unit cell");
        const auto xyz = cell.to_cartesian(u, v, w);
        a.x = xyz[0];
        a.y = xyz[1];
        a.z = xyz[2];
      } else if (cx >= 0 && cy >= 0 && cz >= 0) {
        a.x = to_double(get(cx)).value_or(0);
        a.y = to_double(get(cy)).value_or(0);
        a.z = to_double(get(cz)).value_or(0);
      }
      if (c_occ >= 0) a.extra["occupancy"] = json_number(to_double(get(c_occ)).value_or(1));
      m.atoms.push_back(std::move(a));
    }
    if (const CifBlock::Loop* bonds = block.loop_with("_geom_bond_atom_site_label_1")) {
      const auto index_of = [&m](std::string_view label) -> std::size_t {
        for (std::size_t i = 0; i < m.atoms.size(); ++i) {
          if (m.atoms[i].extra["label"].as_string() == label) return i + 1;
        }
        return 0;
      };
      for (const auto& row : bonds->rows) {
        if (row.size() < 2) continue;
        Bond b;
        b.from = index_of(row[0]);
        b.to = index_of(row[1]);
        if (b.from != 0 && b.to != 0) m.bonds.push_back(b);
      }
    }
    molecules.push_back(std::move(m));
  }
  if (molecules.empty()) return fail(Status::Decode, "no _atom_site loop in the CIF file");
  CVT_TRY(check_cells(ctx, molecules.size()));
  return molecules;
}

Result<std::string> write_cif(const std::vector<Molecule>& molecules, WriteContext& /*ctx*/) {
  std::string out;
  for (std::size_t mi = 0; mi < molecules.size(); ++mi) {
    const Molecule& m = molecules[mi];
    std::string name = m.name.empty() ? "molecule" + std::to_string(mi + 1) : m.name;
    std::replace_if(name.begin(), name.end(), [](char c) { return util::is_ascii_space(c); }, '_');
    out += "data_" + name + "\n";
    Cell cell;
    const auto prop = [&m](std::string_view key) { return to_double(strip_su(m.properties[key].as_string())); };
    const auto a = prop("cell_length_a"), b = prop("cell_length_b"), c = prop("cell_length_c");
    const auto al = prop("cell_angle_alpha"), be = prop("cell_angle_beta"), ga = prop("cell_angle_gamma");
    const bool has_cell = a && b && c && al && be && ga && *a > 0 && *b > 0 && *c > 0;
    for (const auto& [key, value] : m.properties.items()) {
      out += "_" + key + " " + cif_value(value.is_string() ? value.as_string() : util::dump_json(value)) + "\n";
    }
    out += "loop_\n_atom_site_label\n_atom_site_type_symbol\n";
    if (has_cell) {
      cell = Cell{*a, *b, *c, *al, *be, *ga};
      out += "_atom_site_fract_x\n_atom_site_fract_y\n_atom_site_fract_z\n";
    } else {
      out += "_atom_site_Cartn_x\n_atom_site_Cartn_y\n_atom_site_Cartn_z\n";
    }
    std::vector<int> counters(119, 0);
    std::vector<std::string> labels;
    for (const Atom& atom : m.atoms) {
      std::string label = atom.extra["label"].as_string();
      if (label.empty()) {
        label = (atom.element.empty() ? "X" : atom.element) +
                std::to_string(++counters[static_cast<std::size_t>(atomic_number(atom.element))]);
      }
      labels.push_back(label);
      std::array<double, 3> p = {atom.x, atom.y, atom.z};
      if (has_cell) p = cell.to_fractional(atom.x, atom.y, atom.z);
      out += std::format("{} {} {} {} {}\n", cif_value(label), atom.element.empty() ? "X" : atom.element,
                         fmt_fixed(p[0], 5), fmt_fixed(p[1], 5), fmt_fixed(p[2], 5));
    }
    if (!m.bonds.empty()) {
      out += "loop_\n_geom_bond_atom_site_label_1\n_geom_bond_atom_site_label_2\n";
      for (const Bond& bond : m.bonds) out += labels[bond.from - 1] + " " + labels[bond.to - 1] + "\n";
    }
  }
  return out;
}

int sniff_cif(ByteView data, std::string_view /*name*/) {
  const std::string_view head = head_text(data, 16384);
  if (head.find("data_") == std::string_view::npos) return 0;
  if (head.find("_atom_site.") != std::string_view::npos) return 0;  // mmCIF
  if (head.find("_cell_length_a") != std::string_view::npos || head.find("_atom_site_fract_x") != std::string_view::npos ||
      head.find("_atom_site_label") != std::string_view::npos) {
    return 92;
  }
  return 0;
}

}  // namespace

}  // namespace cvt::science::chem

namespace cvt::science {

void register_molecules(Registry& registry) {
  using namespace chem;
  add_format(registry, {.id = "xyz-molecule",
                        .name = "XYZ molecule",
                        .description = "Element symbols and Cartesian coordinates, one or more frames.",
                        .extensions = {"xyz"},
                        .mime = "chemical/x-xyz",
                        .popularity = 25,
                        .sniff = sniff_xyz});
  add_molecule_codecs(registry, "xyz-molecule", read_xyz, write_xyz,
                      {OptionSpec::integer("decimals", "Decimals", 5, 1, 12, "Digits after the decimal point.")});

  add_format(registry, {.id = "mol",
                        .name = "MDL Molfile",
                        .description = "MDL/Accelrys connection table (V2000; V3000 read).",
                        .extensions = {"mol"},
                        .mime = "chemical/x-mdl-molfile",
                        .popularity = 30,
                        .sniff = [](ByteView d, std::string_view n) { return sniff_molfile(d, n, false); }});
  add_molecule_codecs(registry, "mol", read_sdf, write_mol);

  add_format(registry, {.id = "sdf",
                        .name = "Structure-data file",
                        .description = "MDL SD file: several Molfiles with data items.",
                        .extensions = {"sdf", "sd"},
                        .mime = "chemical/x-mdl-sdfile",
                        .popularity = 30,
                        .sniff = [](ByteView d, std::string_view n) { return sniff_molfile(d, n, true); }});
  add_molecule_codecs(registry, "sdf", read_sdf, write_sdf);

  add_format(registry, {.id = "mol2",
                        .name = "Tripos MOL2",
                        .description = "Tripos/SYBYL molecule with atom types and partial charges.",
                        .extensions = {"mol2", "ml2"},
                        .mime = "chemical/x-mol2",
                        .popularity = 20,
                        .sniff = sniff_mol2});
  add_molecule_codecs(registry, "mol2", read_mol2, write_mol2);

  add_format(registry, {.id = "cml",
                        .name = "Chemical Markup Language",
                        .description = "XML description of molecules (CML).",
                        .extensions = {"cml"},
                        .mime = "chemical/x-cml",
                        .popularity = 12,
                        .sniff = sniff_cml});
  add_molecule_codecs(registry, "cml", read_cml, write_cml);

  add_format(registry, {.id = "cif",
                        .name = "Crystallographic Information File",
                        .description = "Small-molecule crystal structure (CIF 1.1).",
                        .extensions = {"cif"},
                        .mime = "chemical/x-cif",
                        .popularity = 22,
                        .sniff = sniff_cif});
  add_molecule_codecs(registry, "cif", read_cif, write_cif);
}

}  // namespace cvt::science
