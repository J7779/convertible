// Convertible core - science family: multiple sequence alignments (PHYLIP,
// Clustal, Stockholm, NEXUS) on the sequence record model, and phylogenetic
// trees (Newick, NEXUS TREES blocks) as Data.
//
// Tree model: {"trees":[{"name":"tree1","rooted":true,"root":{"name":"A",
// "length":0.1,"children":[...]}}]}. NEXUS Data carries both "records"
// (the character matrix) and "trees".
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <format>
#include <map>

#include "families/science/bio.hpp"
#include "families/science/science.hpp"
#include "util/text.hpp"

namespace cvt::science::bio {

namespace {

/// Nodes deeper than this are refused (a caterpillar tree of this many taxa).
constexpr std::size_t max_tree_depth = 100000;

// --------------------------------------------------------------------------- Newick

struct TreeNode {
  std::string name;
  std::optional<double> length;
  std::string comment;
  std::vector<std::size_t> children;
};

class NewickParser {
 public:
  explicit NewickParser(std::string_view text) : text_(text) {}

  /// Parse one tree ending in ';' (or at the end of the text). Returns the
  /// node list with the root at index 0.
  Result<std::vector<TreeNode>> parse() {
    std::vector<TreeNode> nodes(1);
    std::vector<std::size_t> stack = {0};
    skip_space();
    bool expect_label = true;
    while (pos_ < text_.size()) {
      skip_space();
      if (pos_ >= text_.size()) break;
      const char c = text_[pos_];
      if (c == '(') {
        if (!expect_label) return fail(Status::Decode, "unexpected '(' in Newick tree at offset {}", pos_);
        if (stack.size() > max_tree_depth) return fail(Status::Limit, "Newick tree is nested too deeply");
        nodes.emplace_back();
        nodes[stack.back()].children.push_back(nodes.size() - 1);
        stack.push_back(nodes.size() - 1);
        ++pos_;
        continue;
      }
      if (c == ',') {
        if (stack.size() < 2) return fail(Status::Decode, "',' outside parentheses in Newick tree");
        stack.pop_back();
        nodes.emplace_back();
        nodes[stack.back()].children.push_back(nodes.size() - 1);
        stack.push_back(nodes.size() - 1);
        ++pos_;
        expect_label = true;
        continue;
      }
      if (c == ')') {
        if (stack.size() < 2) return fail(Status::Decode, "unbalanced ')' in Newick tree");
        stack.pop_back();
        ++pos_;
        expect_label = true;
        continue;
      }
      if (c == ';') {
        ++pos_;
        break;
      }
      // Label and/or branch length of the current node.
      TreeNode& node = nodes[stack.back()];
      if (c == ':') {
        ++pos_;
        skip_space();
        const std::size_t start = pos_;
        while (pos_ < text_.size() && std::string_view("(),:;[").find(text_[pos_]) == std::string_view::npos &&
               !util::is_ascii_space(text_[pos_])) {
          ++pos_;
        }
        const auto value = to_double(text_.substr(start, pos_ - start));
        if (!value) return fail(Status::Decode, "bad Newick branch length '{}'", text_.substr(start, pos_ - start));
        node.length = *value;
        expect_label = false;
        continue;
      }
      if (!expect_label) return fail(Status::Decode, "unexpected '{}' in Newick tree at offset {}", c, pos_);
      CVT_TRY_ASSIGN(node.name, label());
      expect_label = false;
      if (node.name.empty()) return fail(Status::Decode, "unexpected '{}' in Newick tree at offset {}", c, pos_);
    }
    if (stack.size() != 1) return fail(Status::Decode, "Newick tree has unbalanced parentheses");
    return nodes;
  }

  [[nodiscard]] bool at_end() {
    skip_space();
    return pos_ >= text_.size();
  }

 private:
  void skip_space() {
    while (pos_ < text_.size()) {
      if (util::is_ascii_space(text_[pos_])) {
        ++pos_;
      } else if (text_[pos_] == '[') {
        const std::size_t close = text_.find(']', pos_);
        pos_ = close == std::string_view::npos ? text_.size() : close + 1;
      } else {
        break;
      }
    }
  }

  Result<std::string> label() {
    std::string out;
    if (text_[pos_] == '\'' || text_[pos_] == '"') {
      const char quote = text_[pos_++];
      while (true) {
        if (pos_ >= text_.size()) return fail(Status::Decode, "unterminated quoted Newick label");
        if (text_[pos_] == quote) {
          if (pos_ + 1 < text_.size() && text_[pos_ + 1] == quote) {
            out.push_back(quote);
            pos_ += 2;
            continue;
          }
          ++pos_;
          break;
        }
        out.push_back(text_[pos_++]);
      }
      return out;
    }
    while (pos_ < text_.size() && std::string_view("(),:;[").find(text_[pos_]) == std::string_view::npos &&
           !util::is_ascii_space(text_[pos_])) {
      out.push_back(text_[pos_++]);
    }
    return out;
  }

  std::string_view text_;
  std::size_t pos_ = 0;
};

/// Node list -> nested JSON, iteratively (post-order).
Json nodes_to_json(const std::vector<TreeNode>& nodes, const std::map<std::string, std::string>* translate) {
  std::vector<Json> built(nodes.size());
  std::vector<std::pair<std::size_t, bool>> stack = {{0, false}};
  while (!stack.empty()) {
    auto [index, expanded] = stack.back();
    stack.pop_back();
    const TreeNode& node = nodes[index];
    if (!expanded) {
      stack.emplace_back(index, true);
      for (const std::size_t child : node.children) stack.emplace_back(child, false);
      continue;
    }
    Json json = Json::object();
    if (!node.name.empty()) {
      std::string name = node.name;
      if (translate != nullptr) {
        if (const auto it = translate->find(name); it != translate->end()) name = it->second;
      }
      json["name"] = name;
    }
    if (node.length) json["length"] = json_number(*node.length);
    if (!node.children.empty()) {
      Json children = Json::array();
      for (const std::size_t child : node.children) children.push_back(std::move(built[child]));
      json["children"] = std::move(children);
    }
    built[index] = std::move(json);
  }
  return std::move(built[0]);
}

std::string quote_label(std::string_view name) {
  if (name.find_first_of(" ()[]':;,\t") == std::string_view::npos) return std::string(name);
  return "'" + util::replace_all(name, "'", "''") + "'";
}

/// Nested JSON -> Newick text, iteratively.
Result<std::string> json_to_newick(const Json& root) {
  std::string out;
  struct Frame {
    const Json* node;
    std::size_t next_child;
  };
  std::vector<Frame> stack = {{&root, 0}};
  while (!stack.empty()) {
    Frame& frame = stack.back();
    const Json& children = (*frame.node)["children"];
    const std::size_t count = children.is_array() ? children.size() : 0;
    if (count > 0 && frame.next_child < count) {
      out += frame.next_child == 0 ? "(" : ",";
      if (stack.size() > max_tree_depth) return fail(Status::Limit, "tree is nested too deeply");
      const Json* child = &children[frame.next_child++];
      stack.push_back({child, 0});
      continue;
    }
    if (count > 0) out += ")";
    out += quote_label((*frame.node)["name"].as_string());
    if (const Json* length = frame.node->find("length"); length != nullptr && length->is_number()) {
      out += ":" + fmt_num(length->as_double());
    }
    stack.pop_back();
  }
  return out + ";";
}

Result<Json> parse_newick_text(std::string_view text, ReadContext& ctx) {
  Json trees = Json::array();
  NewickParser parser(text);
  while (!parser.at_end()) {
    CVT_TRY_ASSIGN(const auto nodes, parser.parse());
    CVT_TRY(check_cells(ctx, nodes.size() * 2));
    Json tree = Json::object();
    tree["name"] = "tree" + std::to_string(trees.size() + 1);
    tree["root"] = nodes_to_json(nodes, nullptr);
    trees.push_back(std::move(tree));
  }
  if (trees.empty()) return fail(Status::Decode, "no Newick tree found");
  return Json::object({{"trees", std::move(trees)}});
}

Result<AnyIR> read_newick(ReadContext& ctx) {
  const std::string text = input_text(ctx.input);
  CVT_TRY_ASSIGN(Json data, parse_newick_text(text, ctx));
  return ir::Data{std::move(data)};
}

const Json* trees_of(const Json& value) {
  if (const Json* trees = value.find("trees"); trees != nullptr && trees->is_array()) return trees;
  if (value.is_array()) return &value;
  return nullptr;
}

Result<Bytes> write_newick(const AnyIR& value, WriteContext& /*ctx*/) {
  const Json& data = value.get<ir::Data>().value;
  std::string out;
  if (const Json* trees = trees_of(data)) {
    for (const Json& tree : trees->elements()) {
      CVT_TRY_ASSIGN(const std::string text, json_to_newick(tree.contains("root") ? tree["root"] : tree));
      out += text + '\n';
    }
  } else if (data.contains("children") || data.contains("name")) {
    CVT_TRY_ASSIGN(const std::string text, json_to_newick(data));
    out += text + '\n';
  }
  if (out.empty()) return fail(Status::Encode, "the data holds no trees (expected {\"trees\":[{\"root\":...}]})");
  return to_bytes(out);
}

int sniff_newick(ByteView data, std::string_view name) {
  const std::string_view head = util::trim(head_text(data, 1024));
  if (!head.starts_with('(')) return 0;
  const std::string_view all = util::trim(head_text(data, data.size()));
  if (data.size() <= 1024 && !all.ends_with(';')) return 0;
  return has_extension(name, "nwk") || has_extension(name, "newick") || has_extension(name, "tree") ||
                 has_extension(name, "tre")
             ? 92
             : 60;
}

// --------------------------------------------------------------------------- PHYLIP

Result<std::vector<SeqRecord>> read_phylip(std::string_view text, ReadContext& ctx) {
  auto lines = lines_of(text);
  std::size_t i = 0;
  while (i < lines.size() && util::trim(lines[i]).empty()) ++i;
  if (i >= lines.size()) return fail(Status::Decode, "empty PHYLIP file");
  const auto dims = words(lines[i]);
  if (dims.size() < 2) return fail(Status::Decode, "PHYLIP must start with the taxon and character counts");
  const auto ntax = to_int(dims[0]);
  const auto nchar = to_int(dims[1]);
  if (!ntax || !nchar || *ntax <= 0 || *nchar < 0) return fail(Status::Decode, "bad PHYLIP dimensions");
  if (static_cast<std::uint64_t>(*ntax) > lines.size()) return fail(Status::Decode, "PHYLIP file is truncated");
  CVT_TRY(check_cells(ctx, static_cast<std::uint64_t>(*ntax) * 3));
  const std::string mode = ctx.options.get_string("phylip_names", "auto");
  std::vector<SeqRecord> records(static_cast<std::size_t>(*ntax));
  ++i;
  std::size_t filled = 0;
  std::size_t cycle = 0;
  for (; i < lines.size(); ++i) {
    const std::string_view line = lines[i];
    if (util::trim(line).empty()) continue;
    std::string_view residues = line;
    if (filled < records.size()) {
      SeqRecord& r = records[filled];
      const std::string_view trimmed = util::trim_left(line);
      const std::size_t token_end = trimmed.find_first_of(" \t");
      const std::string_view token = trimmed.substr(0, token_end);
      const bool strict = mode == "strict" || (mode == "auto" && token.size() > 10 && token_end == std::string_view::npos);
      if (strict) {
        r.id = std::string(util::trim(line.substr(0, std::min<std::size_t>(10, line.size()))));
        residues = line.size() > 10 ? line.substr(10) : std::string_view{};
      } else {
        r.id = std::string(token);
        residues = token_end == std::string_view::npos ? std::string_view{} : trimmed.substr(token_end);
      }
      ++filled;
      for (const char c : residues) {
        if (!util::is_ascii_space(c)) r.sequence.push_back(c);
      }
      continue;
    }
    // Interleaved continuation blocks: cycle through the records in order.
    SeqRecord& r = records[cycle % records.size()];
    ++cycle;
    for (const char c : residues) {
      if (!util::is_ascii_space(c)) r.sequence.push_back(c);
    }
  }
  if (filled < records.size()) return fail(Status::Decode, "PHYLIP file lists {} of {} taxa", filled, records.size());
  for (const auto& r : records) {
    if (r.sequence.size() != static_cast<std::size_t>(*nchar)) {
      ctx.warn(std::format("PHYLIP taxon '{}' has {} characters instead of {}.", r.id, r.sequence.size(), *nchar));
    }
  }
  return records;
}

std::size_t alignment_length(const std::vector<SeqRecord>& records) {
  std::size_t length = 0;
  for (const auto& r : records) length = std::max(length, r.sequence.size());
  return length;
}

std::vector<SeqRecord> padded(std::vector<SeqRecord> records, WriteContext& ctx) {
  const std::size_t length = alignment_length(records);
  bool changed = false;
  for (auto& r : records) {
    if (r.sequence.size() < length) {
      r.sequence.resize(length, '-');
      changed = true;
    }
  }
  if (changed) ctx.warn("Sequences of unequal length were padded with gaps to form an alignment.");
  return records;
}

Result<std::string> write_phylip(const std::vector<SeqRecord>& input, WriteContext& ctx) {
  if (input.empty()) return fail(Status::Encode, "an alignment needs at least one sequence");
  const auto records = padded(input, ctx);
  std::string mode = ctx.options.get_string("phylip_names_out", "auto");
  bool long_names = false;
  for (const auto& r : records) long_names = long_names || r.id.size() > 10;
  if (mode == "auto") mode = long_names ? "relaxed" : "strict";
  if (mode == "strict" && long_names) ctx.warn("Names longer than 10 characters were truncated (strict PHYLIP).");
  std::string out = std::format(" {} {}\n", records.size(), alignment_length(records));
  std::size_t name_width = 10;
  if (mode == "relaxed") {
    for (const auto& r : records) name_width = std::max(name_width, r.id.size() + 1);
  }
  for (const auto& r : records) {
    std::string name = r.id.substr(0, mode == "strict" ? 10 : r.id.size());
    std::replace(name.begin(), name.end(), ' ', '_');
    name.resize(name_width, ' ');
    out += name + r.sequence + '\n';
  }
  return out;
}

int sniff_phylip(ByteView data, std::string_view name) {
  const auto lines = lines_of(head_text(data, 1024));
  if (lines.size() < 2) return 0;
  const auto dims = words(lines[0]);
  if (dims.size() != 2 || !to_int(dims[0]) || !to_int(dims[1])) return 0;
  return has_extension(name, "phy") || has_extension(name, "phylip") ? 90 : 55;
}

// --------------------------------------------------------------------------- Clustal

Result<std::vector<SeqRecord>> read_clustal(std::string_view text, ReadContext& ctx) {
  const auto lines = lines_of(text);
  if (lines.empty() || !(lines[0].starts_with("CLUSTAL") || lines[0].starts_with("MUSCLE") ||
                         lines[0].starts_with("PROBCONS") || lines[0].starts_with("Kalign"))) {
    return fail(Status::Decode, "Clustal files start with a CLUSTAL header line");
  }
  std::vector<SeqRecord> records;
  std::map<std::string, std::size_t, std::less<>> index;
  for (std::size_t i = 1; i < lines.size(); ++i) {
    const std::string_view line = lines[i];
    if (line.empty() || util::is_ascii_space(line[0])) continue;  // blank or conservation line
    const auto parts = words(line);
    if (parts.size() < 2) continue;
    auto it = index.find(parts[0]);
    if (it == index.end()) {
      it = index.emplace(std::string(parts[0]), records.size()).first;
      records.push_back(SeqRecord{});
      records.back().id = std::string(parts[0]);
      CVT_TRY(check_cells(ctx, records.size() * 3));
    }
    records[it->second].sequence.append(parts[1]);
  }
  if (records.empty()) return fail(Status::Decode, "no sequences in the Clustal file");
  return records;
}

Result<std::string> write_clustal(const std::vector<SeqRecord>& input, WriteContext& ctx) {
  if (input.empty()) return fail(Status::Encode, "an alignment needs at least one sequence");
  const auto records = padded(input, ctx);
  std::size_t width = 10;
  for (const auto& r : records) width = std::max(width, r.id.size() + 6);
  const std::size_t length = alignment_length(records);
  std::string out = "CLUSTAL multiple sequence alignment\n\n\n";
  for (std::size_t at = 0; at < length; at += 60) {
    for (const auto& r : records) {
      std::string name = r.id;
      std::replace(name.begin(), name.end(), ' ', '_');
      name.resize(width, ' ');
      out += name + r.sequence.substr(at, 60) + '\n';
    }
    std::string conservation(width, ' ');
    for (std::size_t c = at; c < std::min(at + 60, length); ++c) {
      const char first = util::ascii_upper(records.front().sequence[c]);
      bool same = first != '-';
      for (const auto& r : records) same = same && util::ascii_upper(r.sequence[c]) == first;
      conservation.push_back(same ? '*' : ' ');
    }
    out += conservation + "\n\n";
  }
  return out;
}

int sniff_clustal(ByteView data, std::string_view /*name*/) {
  const std::string_view head = head_text(data, 64);
  return head.starts_with("CLUSTAL") ? 95 : 0;
}

// --------------------------------------------------------------------------- Stockholm

Result<std::vector<SeqRecord>> read_stockholm(std::string_view text, ReadContext& ctx) {
  const auto lines = lines_of(text);
  if (lines.empty() || !lines[0].starts_with("# STOCKHOLM")) return fail(Status::Decode, "missing '# STOCKHOLM' header");
  std::vector<SeqRecord> records;
  std::map<std::string, std::size_t, std::less<>> index;
  Json file_annotations = Json::object();
  Json column_annotations = Json::object();
  const auto record_for = [&](std::string_view name) -> SeqRecord& {
    auto it = index.find(name);
    if (it == index.end()) {
      it = index.emplace(std::string(name), records.size()).first;
      records.push_back(SeqRecord{});
      records.back().id = std::string(name);
    }
    return records[it->second];
  };
  for (std::size_t i = 1; i < lines.size(); ++i) {
    const std::string_view line = util::trim_right(lines[i]);
    if (line.empty()) continue;
    if (line.starts_with("//")) break;
    if (line.starts_with("#=GF ")) {
      const auto rest = util::trim(line.substr(5));
      const std::size_t space = rest.find_first_of(" \t");
      const std::string key(rest.substr(0, space));
      const std::string value(space == std::string_view::npos ? "" : util::trim(rest.substr(space)));
      std::string joined = file_annotations[key].as_string();
      if (!joined.empty()) joined += ' ';
      file_annotations[key] = joined + value;
    } else if (line.starts_with("#=GC ")) {
      const auto parts = words(line.substr(5));
      if (parts.size() >= 2) {
        column_annotations[std::string(parts[0])] = column_annotations[std::string(parts[0])].as_string() + std::string(parts[1]);
      }
    } else if (line.starts_with("#=GS ") || line.starts_with("#=GR ")) {
      const auto rest = util::trim(line.substr(5));
      const auto parts = words(rest);
      if (parts.size() < 3) continue;
      SeqRecord& r = record_for(parts[0]);
      const std::string_view tail = rest.substr(static_cast<std::size_t>(parts[1].data() - rest.data()) + parts[1].size());
      if (line[3] == 'S') {
        if (parts[1] == "DE") {
          r.description = std::string(util::trim(tail));
        } else {
          r.annotations[std::string(parts[1])] = std::string(util::trim(tail));
        }
      } else {
        Json& per_residue = r.annotations["per_residue"];
        if (!per_residue.is_object()) per_residue = Json::object();
        per_residue[std::string(parts[1])] = per_residue[std::string(parts[1])].as_string() + std::string(parts[2]);
      }
    } else if (line.starts_with('#')) {
      continue;
    } else {
      const auto parts = words(line);
      if (parts.size() < 2) return fail(Status::Decode, "malformed Stockholm sequence line {}", i + 1);
      record_for(parts[0]).sequence.append(parts[1]);
      CVT_TRY(check_cells(ctx, records.size() * 3));
    }
  }
  if (records.empty()) return fail(Status::Decode, "no sequences in the Stockholm file");
  Json alignment = Json::object();
  if (!file_annotations.empty()) alignment["file"] = std::move(file_annotations);
  if (!column_annotations.empty()) alignment["columns"] = std::move(column_annotations);
  if (!alignment.empty()) records.front().annotations["alignment"] = std::move(alignment);
  return records;
}

Result<std::string> write_stockholm(const std::vector<SeqRecord>& input, WriteContext& ctx) {
  if (input.empty()) return fail(Status::Encode, "an alignment needs at least one sequence");
  const auto records = padded(input, ctx);
  std::string out = "# STOCKHOLM 1.0\n";
  const Json& alignment = records.front().annotations["alignment"];
  for (const auto& [key, value] : alignment["file"].items()) out += "#=GF " + key + " " + value.as_string() + '\n';
  std::size_t width = 0;
  for (const auto& r : records) width = std::max(width, r.id.size() + 1);
  for (const auto& r : records) {
    if (!r.description.empty()) out += "#=GS " + r.id + " DE " + r.description + '\n';
    for (const auto& [key, value] : r.annotations.items()) {
      if (key == "alignment" || key == "per_residue" || !value.is_string()) continue;
      out += "#=GS " + r.id + " " + key + " " + value.as_string() + '\n';
    }
  }
  for (const auto& r : records) {
    std::string name = r.id;
    std::replace(name.begin(), name.end(), ' ', '_');
    name.resize(std::max(width, std::size_t{10}), ' ');
    out += name + r.sequence + '\n';
    for (const auto& [key, value] : r.annotations["per_residue"].items()) {
      out += "#=GR " + r.id + " " + key + " " + value.as_string() + '\n';
    }
  }
  for (const auto& [key, value] : alignment["columns"].items()) out += "#=GC " + key + " " + value.as_string() + '\n';
  out += "//\n";
  return out;
}

int sniff_stockholm(ByteView data, std::string_view /*name*/) {
  return head_text(data, 64).starts_with("# STOCKHOLM") ? 97 : 0;
}

// --------------------------------------------------------------------------- NEXUS

class NexusTokens {
 public:
  explicit NexusTokens(std::string_view text) : text_(text) {}
  /// Next token: a word, a quoted string, or one punctuation character.
  /// Comments [...] are skipped.
  std::string next() {
    skip();
    if (pos_ >= text_.size()) return {};
    const char c = text_[pos_];
    if (c == ';' || c == '=' || c == ',') {
      ++pos_;
      return std::string(1, c);
    }
    if (c == '\'' || c == '"') {
      std::string out;
      ++pos_;
      while (pos_ < text_.size()) {
        if (text_[pos_] == c) {
          if (pos_ + 1 < text_.size() && text_[pos_ + 1] == c) {
            out.push_back(c);
            pos_ += 2;
            continue;
          }
          ++pos_;
          break;
        }
        out.push_back(text_[pos_++]);
      }
      quoted_ = true;
      return out;
    }
    quoted_ = false;
    const std::size_t start = pos_;
    while (pos_ < text_.size() && !util::is_ascii_space(text_[pos_]) &&
           std::string_view(";=,[").find(text_[pos_]) == std::string_view::npos) {
      ++pos_;
    }
    return std::string(text_.substr(start, pos_ - start));
  }
  /// Raw text up to the next ';' (exclusive), consuming the ';'.
  std::string_view until_semicolon() {
    const std::size_t start = pos_;
    while (pos_ < text_.size() && text_[pos_] != ';') {
      if (text_[pos_] == '[') {
        const std::size_t close = text_.find(']', pos_);
        pos_ = close == std::string_view::npos ? text_.size() : close + 1;
        continue;
      }
      if (text_[pos_] == '\'') {
        const std::size_t close = text_.find('\'', pos_ + 1);
        pos_ = close == std::string_view::npos ? text_.size() : close + 1;
        continue;
      }
      ++pos_;
    }
    const std::string_view out = text_.substr(start, pos_ - start);
    if (pos_ < text_.size()) ++pos_;
    return out;
  }
  [[nodiscard]] bool done() {
    skip();
    return pos_ >= text_.size();
  }
  [[nodiscard]] bool was_quoted() const { return quoted_; }

 private:
  void skip() {
    while (pos_ < text_.size()) {
      if (util::is_ascii_space(text_[pos_])) {
        ++pos_;
      } else if (text_[pos_] == '[') {
        const std::size_t close = text_.find(']', pos_);
        pos_ = close == std::string_view::npos ? text_.size() : close + 1;
      } else {
        break;
      }
    }
  }
  std::string_view text_;
  std::size_t pos_ = 0;
  bool quoted_ = false;
};

struct NexusContent {
  std::vector<SeqRecord> records;
  Json trees = Json::array();
};

Result<NexusContent> parse_nexus(std::string_view text, ReadContext& ctx) {
  if (!util::istarts_with(util::trim_left(text), "#NEXUS")) return fail(Status::Decode, "missing #NEXUS header");
  NexusTokens tokens(util::trim_left(text).substr(6));
  NexusContent content;
  std::string block;
  bool interleave = false;
  std::map<std::string, std::string> translate_;  // TRANSLATE table of the current TREES block
  while (!tokens.done()) {
    std::string word = util::to_lower(tokens.next());
    if (word == "begin") {
      block = util::to_lower(tokens.next());
      (void)tokens.until_semicolon();
      continue;
    }
    if (word == "end" || word == "endblock") {
      (void)tokens.until_semicolon();
      block.clear();
      continue;
    }
    if (word == ";" || word.empty()) continue;
    if ((block == "data" || block == "characters") && word == "format") {
      const std::string format = util::to_lower(tokens.until_semicolon());
      interleave = format.find("interleave") != std::string::npos &&
                   format.find("interleave=no") == std::string::npos;
      continue;
    }
    if ((block == "data" || block == "characters" || block == "unaligned") && word == "matrix") {
      const std::string_view matrix = tokens.until_semicolon();
      std::map<std::string, std::size_t, std::less<>> index;
      for (const std::string_view line : lines_of(matrix)) {
        NexusTokens row(line);
        std::string name = row.next();
        if (name.empty()) continue;
        auto it = index.find(name);
        if (it == index.end()) {
          it = index.emplace(name, content.records.size()).first;
          content.records.push_back(SeqRecord{});
          content.records.back().id = name;
          CVT_TRY(check_cells(ctx, content.records.size() * 3));
        }
        while (!row.done()) {
          const std::string piece = row.next();
          if (piece != ",") content.records[it->second].sequence += piece;
        }
      }
      (void)interleave;
      continue;
    }
    if (block == "trees" && word == "translate") {
      const std::string_view list = tokens.until_semicolon();
      NexusTokens items(list);
      std::map<std::string, std::string> translate;
      while (!items.done()) {
        const std::string key = items.next();
        const std::string value = items.next();
        if (!key.empty() && !value.empty()) translate[key] = value;
        if (!items.done() && items.next() != ",") break;
      }
      // Apply to the trees that follow: remembered in a side table.
      for (const auto& [k, v] : translate) translate_[k] = v;
      continue;
    }
    if (block == "trees" && (word == "tree" || word == "utree")) {
      const std::string_view definition = tokens.until_semicolon();
      const std::size_t eq = definition.find('=');
      if (eq == std::string_view::npos) return fail(Status::Decode, "malformed NEXUS tree statement");
      std::string name(util::trim(definition.substr(0, eq)));
      if (name.starts_with('*')) name = std::string(util::trim(name.substr(1)));
      if (name.size() >= 2 && name.front() == '\'' && name.back() == '\'') name = name.substr(1, name.size() - 2);
      std::string_view body = util::trim(definition.substr(eq + 1));
      bool rooted = word == "tree";
      if (util::istarts_with(body, "[&U]")) rooted = false;
      if (util::istarts_with(body, "[&R]")) rooted = true;
      NewickParser parser(body);
      CVT_TRY_ASSIGN(const auto nodes, parser.parse());
      CVT_TRY(check_cells(ctx, nodes.size() * 2));
      Json tree = Json::object();
      tree["name"] = name;
      tree["rooted"] = rooted;
      tree["root"] = nodes_to_json(nodes, translate_.empty() ? nullptr : &translate_);
      content.trees.push_back(std::move(tree));
      continue;
    }
    (void)tokens.until_semicolon();  // any other command
  }
  if (content.records.empty() && content.trees.empty()) return fail(Status::Decode, "the NEXUS file has no matrix or trees");
  return content;
}

Result<std::string> nexus_text(const std::vector<SeqRecord>& input, const Json& trees, WriteContext& ctx) {
  std::string out = "#NEXUS\n";
  if (!input.empty()) {
    const auto records = padded(input, ctx);
    std::string datatype = "protein";
    std::string all;
    for (const auto& r : records) all += r.sequence;
    std::size_t nucleotides = 0;
    for (const char c : all) nucleotides += std::string_view("ACGTUNacgtun-?").find(c) != std::string_view::npos ? 1 : 0;
    if (nucleotides == all.size()) datatype = all.find_first_of("Uu") != std::string::npos ? "rna" : "dna";
    out += std::format("begin data;\n  dimensions ntax={} nchar={};\n  format datatype={} missing=? gap=-;\n  matrix\n",
                       records.size(), alignment_length(records), datatype);
    std::size_t width = 0;
    for (const auto& r : records) width = std::max(width, quote_label(r.id).size() + 1);
    for (const auto& r : records) {
      std::string name = quote_label(r.id);
      name.resize(width, ' ');
      out += "  " + name + r.sequence + '\n';
    }
    out += "  ;\nend;\n";
  }
  if (trees.is_array() && !trees.empty()) {
    out += "begin trees;\n";
    std::size_t n = 0;
    for (const Json& tree : trees.elements()) {
      ++n;
      std::string name = tree["name"].as_string();
      if (name.empty()) name = "tree" + std::to_string(n);
      CVT_TRY_ASSIGN(const std::string newick, json_to_newick(tree.contains("root") ? tree["root"] : tree));
      out += "  tree " + quote_label(name) + " = " + (tree["rooted"].as_bool(true) ? "[&R] " : "[&U] ") + newick + '\n';
    }
    out += "end;\n";
  }
  return out;
}

int sniff_nexus(ByteView data, std::string_view /*name*/) {
  return util::istarts_with(util::trim_left(head_text(data, 64)), "#NEXUS") ? 97 : 0;
}

}  // namespace
}  // namespace cvt::science::bio

namespace cvt::science {

void register_alignments(Registry& registry) {
  using namespace bio;
  add_format(registry, {.id = "phylip",
                        .name = "PHYLIP alignment",
                        .description = "PHYLIP sequential or interleaved multiple sequence alignment.",
                        .extensions = {"phy", "phylip"},
                        .mime = "text/x-phylip",
                        .popularity = 15,
                        .sniff = sniff_phylip});
  add_record_codecs(
      registry,
      {.id = "phylip",
       .read = read_phylip,
       .write = write_phylip,
       .read_options = {OptionSpec::choice("phylip_names", "Input name style", "auto", {"auto", "strict", "relaxed"},
                                           "Strict PHYLIP uses exactly 10 characters for names; relaxed ends a name at whitespace.")},
       .write_options = {OptionSpec::choice("phylip_names_out", "Name style", "auto", {"auto", "strict", "relaxed"},
                                            "Strict truncates names to 10 characters; auto uses strict when all names fit.")},
       .lossy_table = false});

  add_format(registry, {.id = "clustal",
                        .name = "Clustal alignment",
                        .description = "Clustal W/X/Omega multiple sequence alignment.",
                        .extensions = {"aln", "clustal", "clw"},
                        .mime = "text/x-clustalw",
                        .popularity = 18,
                        .sniff = sniff_clustal});
  add_record_codecs(registry, {.id = "clustal", .read = read_clustal, .write = write_clustal});

  add_format(registry, {.id = "sto",
                        .name = "Stockholm alignment",
                        .description = "Stockholm multiple sequence alignment with markup (Pfam, Rfam).",
                        .extensions = {"sto", "stk", "stockholm"},
                        .mime = "text/x-stockholm",
                        .popularity = 12,
                        .sniff = sniff_stockholm});
  add_record_codecs(registry, {.id = "sto", .read = read_stockholm, .write = write_stockholm, .lossy_table = true});

  add_format(registry, {.id = "newick",
                        .name = "Newick tree",
                        .description = "Phylogenetic tree(s) in Newick/New Hampshire notation.",
                        .extensions = {"nwk", "newick", "tree", "tre"},
                        .mime = "text/x-newick",
                        .popularity = 20,
                        .sniff = sniff_newick});
  add_codec(registry, "newick", Kind::Data, read_newick, write_newick);

  add_format(registry, {.id = "nex",
                        .name = "NEXUS file",
                        .description = "NEXUS character matrix and/or phylogenetic trees.",
                        .extensions = {"nex", "nexus", "nxs"},
                        .mime = "text/x-nexus",
                        .popularity = 15,
                        .sniff = sniff_nexus});
  add_codec(
      registry, "nex", Kind::Table,
      [](ReadContext& ctx) -> Result<AnyIR> {
        CVT_TRY_ASSIGN(auto content, parse_nexus(input_text(ctx.input), ctx));
        if (content.records.empty()) return fail(Status::Decode, "the NEXUS file has no character matrix");
        return records_to_table(content.records);
      },
      [](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
        CVT_TRY_ASSIGN(auto records, table_to_records(value.get<ir::Table>()));
        CVT_TRY_ASSIGN(std::string text, nexus_text(records, Json::array(), ctx));
        return to_bytes(text);
      },
      {}, {}, 10, 10, true);
  add_codec(
      registry, "nex", Kind::Data,
      [](ReadContext& ctx) -> Result<AnyIR> {
        CVT_TRY_ASSIGN(auto content, parse_nexus(input_text(ctx.input), ctx));
        Json data = records_to_json(content.records);
        if (content.records.empty()) data = Json::object();
        data["trees"] = std::move(content.trees);
        return ir::Data{std::move(data)};
      },
      [](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
        const Json& data = value.get<ir::Data>().value;
        std::vector<SeqRecord> records;
        if (data.contains("records")) {
          CVT_TRY_ASSIGN(records, json_to_records(data));
        }
        const Json* trees = trees_of(data);
        if (records.empty() && trees == nullptr) return fail(Status::Encode, "the data has neither records nor trees");
        CVT_TRY_ASSIGN(std::string text, nexus_text(records, trees != nullptr ? *trees : Json::array(), ctx));
        return to_bytes(text);
      },
      {}, {}, 12, 12);
}

}  // namespace cvt::science
