// Convertible core - science family: molecule model, element table and the
// CIF/STAR syntax shared by small-molecule CIF and mmCIF.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>

#include "families/science/chem.hpp"
#include "util/text.hpp"

namespace cvt::science::chem {

namespace {

constexpr std::array<std::string_view, 119> elements = {
    "",   "H",  "He", "Li", "Be", "B",  "C",  "N",  "O",  "F",  "Ne", "Na", "Mg", "Al", "Si", "P",  "S",
    "Cl", "Ar", "K",  "Ca", "Sc", "Ti", "V",  "Cr", "Mn", "Fe", "Co", "Ni", "Cu", "Zn", "Ga", "Ge", "As",
    "Se", "Br", "Kr", "Rb", "Sr", "Y",  "Zr", "Nb", "Mo", "Tc", "Ru", "Rh", "Pd", "Ag", "Cd", "In", "Sn",
    "Sb", "Te", "I",  "Xe", "Cs", "Ba", "La", "Ce", "Pr", "Nd", "Pm", "Sm", "Eu", "Gd", "Tb", "Dy", "Ho",
    "Er", "Tm", "Yb", "Lu", "Hf", "Ta", "W",  "Re", "Os", "Ir", "Pt", "Au", "Hg", "Tl", "Pb", "Bi", "Po",
    "At", "Rn", "Fr", "Ra", "Ac", "Th", "Pa", "U",  "Np", "Pu", "Am", "Cm", "Bk", "Cf", "Es", "Fm", "Md",
    "No", "Lr", "Rf", "Db", "Sg", "Bh", "Hs", "Mt", "Ds", "Rg", "Cn", "Nh", "Fl", "Mc", "Lv", "Ts", "Og"};

const std::vector<std::string> base_columns = {"molecule", "index", "element", "x", "y", "z", "charge"};

}  // namespace

int atomic_number(std::string_view symbol) {
  for (std::size_t i = 1; i < elements.size(); ++i) {
    if (util::iequals(elements[i], symbol)) return static_cast<int>(i);
  }
  return 0;
}

std::string_view element_name(int z) {
  return z > 0 && z < static_cast<int>(elements.size()) ? elements[static_cast<std::size_t>(z)] : std::string_view{};
}

std::string normalize_element(std::string_view symbol) {
  symbol = util::trim(symbol);
  if (symbol == "D" || symbol == "T") return std::string(symbol);  // deuterium / tritium labels
  const int z = atomic_number(symbol);
  return z == 0 ? std::string() : std::string(elements[static_cast<std::size_t>(z)]);
}

Json molecules_to_json(const std::vector<Molecule>& molecules) {
  Json list = Json::array();
  for (const Molecule& m : molecules) {
    Json mol = Json::object();
    mol["name"] = m.name;
    Json atoms = Json::array();
    for (const Atom& a : m.atoms) {
      Json atom = Json::object();
      atom["element"] = a.element;
      atom["x"] = json_number(a.x);
      atom["y"] = json_number(a.y);
      atom["z"] = json_number(a.z);
      atom["charge"] = a.charge;
      for (const auto& [key, value] : a.extra.items()) atom[key] = value;
      atoms.push_back(std::move(atom));
    }
    mol["atoms"] = std::move(atoms);
    Json bonds = Json::array();
    for (const Bond& b : m.bonds) {
      bonds.push_back(Json::object({{"from", static_cast<std::uint64_t>(b.from)},
                                    {"to", static_cast<std::uint64_t>(b.to)},
                                    {"order", b.order}}));
    }
    mol["bonds"] = std::move(bonds);
    if (!m.properties.empty()) mol["properties"] = m.properties;
    list.push_back(std::move(mol));
  }
  return Json::object({{"molecules", std::move(list)}});
}

Result<std::vector<Molecule>> json_to_molecules(const Json& value) {
  const Json* list = &value;
  if (value.is_object()) {
    list = value.find("molecules");
    if (list == nullptr && value.contains("atoms")) {
      list = &value;
    } else if (list == nullptr) {
      return fail(Status::Encode, "the data has no \"molecules\" list");
    }
  }
  std::vector<const Json*> items;
  if (list->is_array()) {
    for (const Json& m : list->elements()) items.push_back(&m);
  } else {
    items.push_back(list);
  }
  std::vector<Molecule> molecules;
  for (const Json* item : items) {
    if (!item->is_object()) continue;
    Molecule m;
    m.name = (*item)["name"].as_string();
    for (const Json& a : (*item)["atoms"].elements()) {
      Atom atom;
      atom.element = a["element"].as_string();
      atom.x = a["x"].as_double();
      atom.y = a["y"].as_double();
      atom.z = a["z"].as_double();
      atom.charge = static_cast<int>(a["charge"].as_int64());
      for (const auto& [key, v] : a.items()) {
        if (key != "element" && key != "x" && key != "y" && key != "z" && key != "charge") atom.extra[key] = v;
      }
      m.atoms.push_back(std::move(atom));
    }
    for (const Json& b : (*item)["bonds"].elements()) {
      Bond bond;
      bond.from = static_cast<std::size_t>(b["from"].as_uint64());
      bond.to = static_cast<std::size_t>(b["to"].as_uint64());
      bond.order = static_cast<int>(b["order"].as_int64(1));
      if (bond.from == 0 || bond.to == 0 || bond.from > m.atoms.size() || bond.to > m.atoms.size()) {
        return fail(Status::Encode, "bond {}-{} refers to a missing atom", bond.from, bond.to);
      }
      m.bonds.push_back(bond);
    }
    if (const Json* p = item->find("properties"); p != nullptr && p->is_object()) m.properties = *p;
    molecules.push_back(std::move(m));
  }
  if (molecules.empty()) return fail(Status::Encode, "the data holds no molecules");
  return molecules;
}

ir::Table molecules_to_table(const std::vector<Molecule>& molecules) {
  std::vector<std::string> columns = base_columns;
  for (const Molecule& m : molecules) {
    for (const Atom& a : m.atoms) {
      for (const auto& member : a.extra.items()) {
        if (std::find(columns.begin(), columns.end(), member.key()) == columns.end()) columns.push_back(member.key());
      }
    }
  }
  ir::Sheet atoms = header_sheet("atoms", columns);
  ir::Sheet bonds = header_sheet("bonds", {"molecule", "from", "to", "order"});
  ir::Sheet properties = header_sheet("properties", {"molecule", "key", "value"});
  for (std::size_t mi = 0; mi < molecules.size(); ++mi) {
    const Molecule& m = molecules[mi];
    const ir::Cell name = m.name.empty() ? int_cell(static_cast<std::int64_t>(mi + 1)) : ir::Cell::string(m.name);
    for (std::size_t i = 0; i < m.atoms.size(); ++i) {
      const Atom& a = m.atoms[i];
      std::vector<ir::Cell> row = {name,           int_cell(static_cast<std::int64_t>(i + 1)), text_cell(a.element),
                                   number_cell(a.x), number_cell(a.y), number_cell(a.z), int_cell(a.charge)};
      for (std::size_t c = base_columns.size(); c < columns.size(); ++c) {
        const Json* v = a.extra.find(columns[c]);
        if (v == nullptr || v->is_null()) {
          row.emplace_back();
        } else if (v->is_number()) {
          row.push_back(number_cell(v->as_double()));
        } else {
          row.push_back(text_cell(v->is_string() ? v->as_string() : util::dump_json(*v)));
        }
      }
      atoms.rows.push_back(std::move(row));
    }
    for (const Bond& b : m.bonds) {
      bonds.rows.push_back({name, int_cell(static_cast<std::int64_t>(b.from)), int_cell(static_cast<std::int64_t>(b.to)),
                            int_cell(b.order)});
    }
    for (const auto& [key, value] : m.properties.items()) {
      properties.rows.push_back(
          {name, ir::Cell::string(key), text_cell(value.is_string() ? value.as_string() : util::dump_json(value))});
    }
  }
  ir::Table table;
  table.sheets.push_back(std::move(atoms));
  if (bonds.rows.size() > 1) table.sheets.push_back(std::move(bonds));
  if (properties.rows.size() > 1) table.sheets.push_back(std::move(properties));
  return table;
}

Result<std::vector<Molecule>> table_to_molecules(const ir::Table& table) {
  if (table.sheets.empty()) return fail(Status::Encode, "the table has no sheets");
  const ir::Sheet* atoms = find_sheet(table, "atoms");
  if (atoms == nullptr) atoms = &table.sheets.front();
  const int c_mol = column_of(*atoms, "molecule");
  const int c_el = std::max(column_of(*atoms, "element"), column_of(*atoms, "symbol"));
  const int c_x = column_of(*atoms, "x");
  const int c_y = column_of(*atoms, "y");
  const int c_z = column_of(*atoms, "z");
  const int c_charge = column_of(*atoms, "charge");
  if (c_el < 0 || c_x < 0 || c_y < 0) return fail(Status::Encode, "the atoms table needs element, x and y columns");
  std::vector<Molecule> molecules;
  std::vector<std::string> keys;
  const auto molecule_for = [&](std::string_view key) -> Molecule& {
    const auto it = std::find(keys.begin(), keys.end(), key);
    if (it != keys.end()) return molecules[static_cast<std::size_t>(it - keys.begin())];
    keys.emplace_back(key);
    molecules.emplace_back();
    molecules.back().name = to_int(key) ? std::string() : std::string(key);
    return molecules.back();
  };
  const auto& header = atoms->rows.front();
  for (std::size_t r = 1; r < atoms->rows.size(); ++r) {
    Molecule& m = molecule_for(cell_text(*atoms, r, c_mol));
    Atom a;
    a.element = std::string(cell_text(*atoms, r, c_el));
    a.x = to_double(cell_text(*atoms, r, c_x)).value_or(0);
    a.y = to_double(cell_text(*atoms, r, c_y)).value_or(0);
    a.z = to_double(cell_text(*atoms, r, c_z)).value_or(0);
    a.charge = static_cast<int>(to_int(cell_text(*atoms, r, c_charge)).value_or(0));
    for (std::size_t c = 0; c < header.size(); ++c) {
      const std::string& title = header[c].text;
      if (std::find(base_columns.begin(), base_columns.end(), title) != base_columns.end() || title == "symbol") continue;
      const ir::Cell* cell = c < atoms->rows[r].size() ? &atoms->rows[r][c] : nullptr;
      if (cell == nullptr || cell->empty()) continue;
      a.extra[title] = cell->type == ir::CellType::Number ? json_number(cell->number) : Json(cell->text);
    }
    m.atoms.push_back(std::move(a));
  }
  if (const ir::Sheet* bonds = find_sheet(table, "bonds")) {
    const int b_mol = column_of(*bonds, "molecule");
    const int b_from = column_of(*bonds, "from");
    const int b_to = column_of(*bonds, "to");
    const int b_order = column_of(*bonds, "order");
    for (std::size_t r = 1; r < bonds->rows.size(); ++r) {
      Molecule& m = molecule_for(cell_text(*bonds, r, b_mol));
      Bond b;
      b.from = static_cast<std::size_t>(std::max<std::int64_t>(0, to_int(cell_text(*bonds, r, b_from)).value_or(0)));
      b.to = static_cast<std::size_t>(std::max<std::int64_t>(0, to_int(cell_text(*bonds, r, b_to)).value_or(0)));
      b.order = static_cast<int>(to_int(cell_text(*bonds, r, b_order)).value_or(1));
      if (b.from == 0 || b.to == 0 || b.from > m.atoms.size() || b.to > m.atoms.size()) {
        return fail(Status::Encode, "bond row {} refers to a missing atom", r);
      }
      m.bonds.push_back(b);
    }
  }
  if (const ir::Sheet* props = find_sheet(table, "properties")) {
    for (std::size_t r = 1; r < props->rows.size(); ++r) {
      Molecule& m = molecule_for(cell_text(*props, r, 0));
      m.properties[cell_text(*props, r, 1)] = std::string(cell_text(*props, r, 2));
    }
  }
  if (molecules.empty()) return fail(Status::Encode, "the table holds no atoms");
  return molecules;
}

void add_molecule_codecs(Registry& registry, const std::string& id, MoleculeReader read, MoleculeWriter write,
                         std::vector<OptionSpec> write_options) {
  ReadFn read_data;
  ReadFn read_table;
  WriteFn write_data;
  WriteFn write_table;
  if (read) {
    read_data = [read](ReadContext& ctx) -> Result<AnyIR> {
      CVT_TRY_ASSIGN(auto molecules, read(input_text(ctx.input), ctx));
      return ir::Data{molecules_to_json(molecules)};
    };
    read_table = [read](ReadContext& ctx) -> Result<AnyIR> {
      CVT_TRY_ASSIGN(auto molecules, read(input_text(ctx.input), ctx));
      return molecules_to_table(molecules);
    };
  }
  if (write) {
    write_data = [write](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
      CVT_TRY_ASSIGN(auto molecules, json_to_molecules(value.get<ir::Data>().value));
      CVT_TRY_ASSIGN(std::string text, write(molecules, ctx));
      return to_bytes(text);
    };
    write_table = [write](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
      CVT_TRY_ASSIGN(auto molecules, table_to_molecules(value.get<ir::Table>()));
      CVT_TRY_ASSIGN(std::string text, write(molecules, ctx));
      return to_bytes(text);
    };
  }
  add_codec(registry, id, Kind::Data, read_data, write_data, {}, write_options, 10, 10);
  add_codec(registry, id, Kind::Table, read_table, write_table, {}, write_options, 12, 12);
}

// --------------------------------------------------------------------------- CIF

const std::string* CifBlock::item(std::string_view tag) const {
  for (const auto& [key, value] : items) {
    if (util::iequals(key, tag)) return &value;
  }
  return nullptr;
}

const CifBlock::Loop* CifBlock::loop_with(std::string_view tag) const {
  for (const auto& loop : loops) {
    for (const auto& t : loop.tags) {
      if (util::iequals(t, tag)) return &loop;
    }
  }
  return nullptr;
}

namespace {

class CifLexer {
 public:
  explicit CifLexer(std::string_view text) : text_(text) {}
  struct Token {
    std::string text;
    bool quoted = false;
    bool end = false;
  };
  Result<Token> next() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == '#') {
        while (pos_ < text_.size() && text_[pos_] != '\n' && text_[pos_] != '\r') ++pos_;
      } else if (util::is_ascii_space(c)) {
        ++pos_;
      } else {
        break;
      }
    }
    if (pos_ >= text_.size()) return Token{{}, false, true};
    const bool line_start = pos_ == 0 || text_[pos_ - 1] == '\n' || text_[pos_ - 1] == '\r';
    const char c = text_[pos_];
    if (c == ';' && line_start) {
      // Multi-line text field up to a line starting with ';'.
      std::size_t search = pos_ + 1;
      while (true) {
        const std::size_t nl = text_.find('\n', search);
        if (nl == std::string_view::npos) return fail(Status::Decode, "unterminated CIF text field");
        if (nl + 1 < text_.size() && text_[nl + 1] == ';') {
          std::string value(text_.substr(pos_ + 1, nl - pos_ - 1));
          if (!value.empty() && value.back() == '\r') value.pop_back();
          if (value.starts_with('\n')) value.erase(0, 1);
          pos_ = nl + 2;
          return Token{std::move(value), true, false};
        }
        search = nl + 1;
      }
    }
    if (c == '\'' || c == '"') {
      // Closing quote must be followed by whitespace or the end.
      std::size_t at = pos_ + 1;
      while (at < text_.size()) {
        if (text_[at] == c && (at + 1 >= text_.size() || util::is_ascii_space(text_[at + 1]))) break;
        if (text_[at] == '\n') return fail(Status::Decode, "unterminated CIF quoted string");
        ++at;
      }
      if (at >= text_.size()) return fail(Status::Decode, "unterminated CIF quoted string");
      std::string value(text_.substr(pos_ + 1, at - pos_ - 1));
      pos_ = at + 1;
      return Token{std::move(value), true, false};
    }
    const std::size_t start = pos_;
    while (pos_ < text_.size() && !util::is_ascii_space(text_[pos_])) ++pos_;
    return Token{std::string(text_.substr(start, pos_ - start)), false, false};
  }

 private:
  std::string_view text_;
  std::size_t pos_ = 0;
};

}  // namespace

Result<std::vector<CifBlock>> parse_cif(std::string_view text, const Context& ctx) {
  std::vector<CifBlock> blocks;
  CifLexer lexer(text);
  std::optional<CifLexer::Token> pending;
  const auto take = [&]() -> Result<CifLexer::Token> {
    if (pending) {
      auto t = std::move(*pending);
      pending.reset();
      return t;
    }
    return lexer.next();
  };
  std::uint64_t values = 0;
  while (true) {
    CVT_TRY_ASSIGN(auto token, take());
    if (token.end) break;
    if (!token.quoted && util::istarts_with(token.text, "data_")) {
      blocks.push_back(CifBlock{token.text.substr(5), {}, {}});
      continue;
    }
    if (!token.quoted && (util::istarts_with(token.text, "save_") || util::iequals(token.text, "global_"))) continue;
    if (blocks.empty()) blocks.push_back(CifBlock{"", {}, {}});
    CifBlock& block = blocks.back();
    if (!token.quoted && util::iequals(token.text, "loop_")) {
      CifBlock::Loop loop;
      while (true) {
        CVT_TRY_ASSIGN(auto tag, take());
        if (!tag.end && !tag.quoted && tag.text.starts_with('_')) {
          loop.tags.push_back(tag.text);
          continue;
        }
        pending = std::move(tag);
        break;
      }
      if (loop.tags.empty()) return fail(Status::Decode, "CIF loop_ without tags");
      std::vector<std::string> row;
      while (true) {
        CVT_TRY_ASSIGN(auto value, take());
        if (value.end || (!value.quoted && (value.text.starts_with('_') || util::iequals(value.text, "loop_") ||
                                            util::istarts_with(value.text, "data_")))) {
          pending = std::move(value);
          break;
        }
        row.push_back(std::move(value.text));
        if (++values > ctx.limits.max_pixels / 4) return fail(Status::Limit, "CIF file has too many values");
        if (row.size() == loop.tags.size()) {
          loop.rows.push_back(std::move(row));
          row.clear();
        }
      }
      if (!row.empty()) ctx.warn("A CIF loop ended with an incomplete row; it was dropped.");
      block.loops.push_back(std::move(loop));
      continue;
    }
    if (!token.quoted && token.text.starts_with('_')) {
      CVT_TRY_ASSIGN(auto value, take());
      if (value.end) return fail(Status::Decode, "CIF tag {} has no value", token.text);
      block.items.emplace_back(token.text, value.text);
      ++values;
      continue;
    }
    return fail(Status::Decode, "unexpected CIF value '{}'", token.text.substr(0, 40));
  }
  if (blocks.empty()) return fail(Status::Decode, "no CIF data blocks");
  return blocks;
}

std::string cif_value(std::string_view value) {
  if (value.empty()) return "?";
  if (value.find('\n') != std::string_view::npos) return ";" + std::string(value) + "\n;";
  const bool plain = value.find_first_of(" \t'\"") == std::string_view::npos && value.front() != '_' &&
                     value.front() != '#' && value.front() != '$' && value.front() != ';' &&
                     !util::istarts_with(value, "data_") && !util::istarts_with(value, "loop_");
  if (plain) return std::string(value);
  if (value.find("' ") == std::string_view::npos && !value.ends_with('\'')) return "'" + std::string(value) + "'";
  return "\"" + std::string(value) + "\"";
}

}  // namespace cvt::science::chem
