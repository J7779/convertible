// Convertible core - code family: diffs, ignore files and simple config files.
//
//   diff / patch       unified (and context) diffs -> Document: a heading and
//                      summary per file, then either an inline highlighted
//                      block or a side-by-side table; also -> plain text
//   gitignore,         ignore files <-> Data {"patterns": [...]} with each
//   dockerignore       pattern's negation/directory/anchoring decoded
//   editorconfig       .editorconfig <-> Data {"root": true, "sections": {...}}
//   npmrc              .npmrc-style key=value files <-> Data object
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "engine/registry.hpp"
#include "families/code/code.hpp"
#include "families/code/devtools.hpp"
#include "util/json.hpp"
#include "util/text.hpp"

namespace cvt::code {

namespace {

std::string_view basename(std::string_view filename) {
  const std::size_t slash = filename.find_last_of("/\\");
  return slash == std::string_view::npos ? filename : filename.substr(slash + 1);
}

Result<std::string> input_text(const ReadContext& ctx) {
  CVT_TRY(ctx.limits.check_decompressed(static_cast<std::uint64_t>(ctx.input.size()) * 3));
  const util::EncodingGuess guess = util::guess_encoding(ctx.input);
  return util::convert_line_endings(util::decode_text(ctx.input, guess.encoding).text, util::LineEnding::Lf);
}

// ---------------------------------------------------------------------------- diff parsing

std::string strip_prefix(std::string_view path) {
  path = util::trim(path);
  if (const std::size_t tab = path.find('\t'); tab != std::string_view::npos) path = path.substr(0, tab);  // timestamps
  if (path.starts_with("a/") || path.starts_with("b/")) path.remove_prefix(2);
  return std::string(path);
}

}  // namespace

Result<std::vector<DiffFile>> parse_diff(std::string_view text) {
  std::vector<DiffFile> files;
  const std::vector<std::string_view> lines = util::split_lines(text);
  DiffFile* file = nullptr;
  DiffHunk* hunk = nullptr;
  std::int64_t old_line = 0, new_line = 0;
  const auto new_file = [&]() -> DiffFile& {
    files.emplace_back();
    hunk = nullptr;
    return files.back();
  };
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const std::string_view line = lines[i];
    if (line.starts_with("diff --git ") || line.starts_with("Index: ") || line.starts_with("diff -")) {
      file = &new_file();
      file->header = std::string(line);
      if (line.starts_with("diff --git ")) {
        const std::string_view rest = line.substr(11);
        const std::size_t space = rest.find(" b/");
        if (space != std::string_view::npos) {
          file->old_path = strip_prefix(rest.substr(0, space));
          file->new_path = strip_prefix(rest.substr(space + 1));
        }
      }
      continue;
    }
    if (line.starts_with("--- ") && i + 1 < lines.size() && lines[i + 1].starts_with("+++ ")) {
      if (file == nullptr || !file->hunks.empty()) file = &new_file();
      file->old_path = strip_prefix(line.substr(4));
      file->new_path = strip_prefix(lines[i + 1].substr(4));
      ++i;
      hunk = nullptr;
      continue;
    }
    if (line.starts_with("@@")) {
      if (file == nullptr) file = &new_file();
      // @@ -l,s +l,s @@ section
      const auto numbers = [](std::string_view spec, std::int64_t& start) {
        const std::size_t comma = spec.find(',');
        const auto value = util::parse_int(spec.substr(0, comma));
        if (!value) return false;
        start = *value;
        return true;
      };
      const std::size_t minus = line.find('-');
      const std::size_t plus = line.find('+', minus == std::string_view::npos ? 0 : minus);
      if (minus == std::string_view::npos || plus == std::string_view::npos) {
        return fail(Status::Decode, "malformed hunk header on line {}", i + 1);
      }
      const std::string_view old_spec = line.substr(minus + 1, line.find(' ', minus) - minus - 1);
      const std::string_view new_spec = line.substr(plus + 1, line.find(' ', plus) - plus - 1);
      if (!numbers(old_spec, old_line) || !numbers(new_spec, new_line)) {
        return fail(Status::Decode, "malformed hunk header on line {}", i + 1);
      }
      file->hunks.emplace_back();
      hunk = &file->hunks.back();
      hunk->header = std::string(line);
      continue;
    }
    if (hunk == nullptr) {
      if (file != nullptr && !line.empty()) file->extra.emplace_back(line);  // "new file mode", "index ..."
      continue;
    }
    if (line.starts_with('\\')) continue;  // "\ No newline at end of file"
    DiffLine entry;
    if (line.starts_with('+')) {
      entry = {DiffLine::Kind::Added, -1, new_line++, std::string(line.substr(1))};
      ++file->added;
    } else if (line.starts_with('-')) {
      entry = {DiffLine::Kind::Removed, old_line++, -1, std::string(line.substr(1))};
      ++file->removed;
    } else if (line.starts_with(' ') || line.empty()) {
      entry = {DiffLine::Kind::Context, old_line++, new_line++, std::string(line.empty() ? line : line.substr(1))};
    } else {
      hunk = nullptr;  // trailing garbage or the next file's preamble
      if (file != nullptr) file->extra.emplace_back(line);
      continue;
    }
    hunk->lines.push_back(std::move(entry));
  }
  if (files.empty()) return fail(Status::Decode, "no diff hunks found");
  return files;
}

namespace {

ir::TableCell cell(std::string text) { return ir::TableCell{{ir::Text{std::move(text)}}, 1, 1}; }
ir::TableCell code_cell(std::string text) { return ir::TableCell{{ir::Code{std::move(text)}}, 1, 1}; }

ir::Document diff_document(const std::vector<DiffFile>& files, bool side_by_side, std::string_view title) {
  ir::Document document;
  document.metadata.title = std::string(title);
  std::int64_t added = 0, removed = 0;
  for (const DiffFile& f : files) {
    added += f.added;
    removed += f.removed;
  }
  document.blocks.push_back(ir::Paragraph{{ir::Text{std::format("{} file{} changed, {} insertion{}(+), {} deletion{}(-)",
                                                                files.size(), files.size() == 1 ? "" : "s", added,
                                                                added == 1 ? "" : "s", removed, removed == 1 ? "" : "s")}}});
  for (const DiffFile& f : files) {
    std::string name = f.new_path == "/dev/null" ? f.old_path : f.new_path;
    if (f.old_path != f.new_path && f.old_path != "/dev/null" && f.new_path != "/dev/null") name = f.old_path + " → " + f.new_path;
    if (f.old_path == "/dev/null") name += " (new file)";
    if (f.new_path == "/dev/null") name += " (deleted)";
    if (name.empty()) name = "(unnamed file)";
    document.blocks.push_back(ir::Heading{2, {ir::Text{name}}});
    document.blocks.push_back(ir::Paragraph{{ir::Text{std::format("+{} −{}", f.added, f.removed)}}});
    if (!side_by_side) {
      std::string code;
      for (const DiffHunk& h : f.hunks) {
        code += h.header + "\n";
        for (const DiffLine& l : h.lines) {
          code.push_back(l.kind == DiffLine::Kind::Added ? '+' : l.kind == DiffLine::Kind::Removed ? '-' : ' ');
          code += l.text + "\n";
        }
      }
      if (!code.empty()) code.pop_back();
      document.blocks.push_back(ir::CodeBlock{"diff", std::move(code)});
      continue;
    }
    ir::TableBlock table;
    table.rows.push_back(ir::build::row({"", "Before", "", "After"}, true));
    for (const DiffHunk& h : f.hunks) {
      table.rows.push_back(ir::TableRow{{ir::TableCell{{ir::Text{h.header}}, 4, 1}}, false});
      // Pair runs of removals with the following additions, as side-by-side viewers do.
      std::size_t i = 0;
      while (i < h.lines.size()) {
        if (h.lines[i].kind == DiffLine::Kind::Context) {
          const DiffLine& l = h.lines[i++];
          table.rows.push_back(ir::TableRow{{cell(std::to_string(l.old_number)), code_cell(l.text),
                                             cell(std::to_string(l.new_number)), code_cell(l.text)},
                                            false});
          continue;
        }
        std::vector<const DiffLine*> left, right;
        while (i < h.lines.size() && h.lines[i].kind == DiffLine::Kind::Removed) left.push_back(&h.lines[i++]);
        while (i < h.lines.size() && h.lines[i].kind == DiffLine::Kind::Added) right.push_back(&h.lines[i++]);
        for (std::size_t k = 0; k < std::max(left.size(), right.size()); ++k) {
          ir::TableRow row;
          if (k < left.size()) {
            row.cells.push_back(cell(std::format("−{}", left[k]->old_number)));
            row.cells.push_back(code_cell(left[k]->text));
          } else {
            row.cells.push_back(cell(""));
            row.cells.push_back(cell(""));
          }
          if (k < right.size()) {
            row.cells.push_back(cell(std::format("+{}", right[k]->new_number)));
            row.cells.push_back(code_cell(right[k]->text));
          } else {
            row.cells.push_back(cell(""));
            row.cells.push_back(cell(""));
          }
          table.rows.push_back(std::move(row));
        }
      }
    }
    document.blocks.push_back(std::move(table));
  }
  return document;
}

int sniff_diff(ByteView data, std::string_view filename) {
  const std::string_view text = as_chars(data.first(std::min<std::size_t>(data.size(), 4096)));
  const bool extension = util::iends_with(filename, ".diff") || util::iends_with(filename, ".patch");
  const bool git = text.starts_with("diff --git ") || text.find("\ndiff --git ") != std::string_view::npos;
  const std::size_t minus = text.find("--- ");
  const bool unified = minus != std::string_view::npos && text.find("\n+++ ", minus) != std::string_view::npos &&
                       text.find("\n@@ ", minus) != std::string_view::npos;
  if (git || unified) return extension ? 95 : 88;
  return extension && util::looks_like_text(data.first(std::min<std::size_t>(data.size(), 4096))) ? 70 : 0;
}

// ---------------------------------------------------------------------------- ignore files

Json ignore_to_data(std::string_view text) {
  Json patterns = Json::array();
  for (std::string_view line : util::split_lines(text)) {
    line = util::trim_right(line);
    if (line.empty() || line.starts_with('#')) continue;
    std::string pattern(line);
    Json entry = Json::object();
    entry["pattern"] = pattern;
    std::string_view body = line;
    const bool negated = body.starts_with('!');
    if (negated) body.remove_prefix(1);
    entry["negated"] = negated;
    entry["directory_only"] = body.ends_with('/');
    entry["anchored"] = body.starts_with('/') || body.substr(0, body.size() - (body.ends_with('/') ? 1 : 0)).find('/') != std::string_view::npos;
    patterns.push_back(std::move(entry));
  }
  Json root = Json::object();
  root["patterns"] = std::move(patterns);
  return root;
}

Result<std::string> data_to_ignore(const Json& value) {
  const Json* list = value.is_array() ? &value : value.find("patterns");
  if (list == nullptr || !list->is_array()) {
    return fail(Status::Encode, "an ignore file needs a list of patterns (an array, or an object with \"patterns\")");
  }
  std::string out;
  for (const Json& item : list->elements()) {
    std::string pattern;
    if (item.is_string()) {
      pattern = item.as_string();
    } else if (const Json* p = item.find("pattern"); item.is_object() && p != nullptr && p->is_string()) {
      pattern = p->as_string();
    } else {
      return fail(Status::Encode, "every pattern must be a string or an object with a \"pattern\" string");
    }
    if (pattern.find('\n') != std::string::npos) return fail(Status::Encode, "patterns cannot contain line breaks");
    out += pattern + "\n";
  }
  return out;
}

// ---------------------------------------------------------------------------- editorconfig and npmrc

Json typed_value(std::string_view value) {
  const std::string lower = util::to_lower(value);
  if (lower == "true") return true;
  if (lower == "false") return false;
  if (const auto number = util::parse_int(value); number && !value.starts_with('+') && !(value.size() > 1 && value[0] == '0')) {
    return *number;
  }
  return std::string(value);
}

Result<Json> editorconfig_to_data(std::string_view text) {
  Json root = Json::object();
  Json sections = Json::object();
  Json* current = nullptr;
  std::size_t number = 0;
  for (std::string_view line : util::split_lines(text)) {
    ++number;
    line = util::trim(line);
    if (line.empty() || line.starts_with('#') || line.starts_with(';')) continue;
    if (line.starts_with('[')) {
      if (!line.ends_with(']')) return fail(Status::Decode, "unterminated section header on line {}", number);
      const std::string name(line.substr(1, line.size() - 2));
      if (sections.find(name) == nullptr) sections[name] = Json::object();
      current = sections.find(name);
      continue;
    }
    const std::size_t equals = line.find('=');
    if (equals == std::string_view::npos) return fail(Status::Decode, "expected 'key = value' on line {}", number);
    const std::string key = util::to_lower(util::trim(line.substr(0, equals)));
    const std::string_view value = util::trim(line.substr(equals + 1));
    if (current == nullptr) {
      root[key] = typed_value(value);  // preamble, normally "root = true"
    } else {
      (*current)[key] = typed_value(value);
    }
  }
  root["sections"] = std::move(sections);
  return root;
}

std::string scalar_text(const Json& value) {
  if (value.is_string()) return value.as_string();
  if (value.is_bool()) return value.as_bool() ? "true" : "false";
  return util::dump_json(value);
}

Result<std::string> data_to_editorconfig(const Json& value) {
  if (!value.is_object()) return fail(Status::Encode, "an .editorconfig is written from an object");
  std::string out;
  for (const auto& [key, item] : value.items()) {
    if (key == "sections") continue;
    if (item.is_structured()) return fail(Status::Encode, "top-level value '{}' must be a scalar", key);
    out += key + " = " + scalar_text(item) + "\n";
  }
  const Json* sections = value.find("sections");
  if (sections != nullptr) {
    if (!sections->is_object()) return fail(Status::Encode, "\"sections\" must be an object of objects");
    for (const auto& [glob, props] : sections->items()) {
      if (!props.is_object()) return fail(Status::Encode, "section '{}' must be an object", glob);
      out += "\n[" + glob + "]\n";
      for (const auto& [key, item] : props.items()) {
        if (item.is_structured()) return fail(Status::Encode, "value of '{}' in [{}] must be a scalar", key, glob);
        out += key + " = " + scalar_text(item) + "\n";
      }
    }
  }
  return out;
}

Result<Json> npmrc_to_data(std::string_view text) {
  Json root = Json::object();
  std::size_t number = 0;
  for (std::string_view line : util::split_lines(text)) {
    ++number;
    line = util::trim(line);
    if (line.empty() || line.starts_with('#') || line.starts_with(';')) continue;
    const std::size_t equals = line.find('=');
    if (equals == std::string_view::npos) return fail(Status::Decode, "expected 'key=value' on line {}", number);
    const std::string key(util::trim(line.substr(0, equals)));
    std::string_view value = util::trim(line.substr(equals + 1));
    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) {
      value = value.substr(1, value.size() - 2);
    }
    if (key.ends_with("[]")) {  // array syntax: key[]=value
      const std::string name = key.substr(0, key.size() - 2);
      if (root.find(name) == nullptr || !root[name].is_array()) root[name] = Json::array();
      root[name].push_back(std::string(value));
    } else {
      root[key] = typed_value(value);
    }
  }
  return root;
}

Result<std::string> data_to_npmrc(const Json& value) {
  if (!value.is_object()) return fail(Status::Encode, "an .npmrc is written from an object");
  std::string out;
  for (const auto& [key, item] : value.items()) {
    if (item.is_array()) {
      for (const Json& element : item.elements()) out += key + "[]=" + scalar_text(element) + "\n";
    } else if (item.is_object()) {
      return fail(Status::Encode, "nested objects cannot be written to an .npmrc ('{}')", key);
    } else {
      out += key + "=" + scalar_text(item) + "\n";
    }
  }
  return out;
}

void add_data_format(Registry& registry, std::string id, std::string name, std::string description,
                     std::vector<std::string> extensions, int popularity, std::function<int(ByteView, std::string_view)> sniff,
                     std::function<Result<Json>(std::string_view)> parse, std::function<Result<std::string>(const Json&)> write) {
  FormatInfo info;
  info.id = id;
  info.name = std::move(name);
  info.description = std::move(description);
  info.category = Category::Code;
  info.extensions = std::move(extensions);
  info.mime = "text/plain";
  info.popularity = popularity;
  info.sniff = std::move(sniff);
  registry.add_format(std::move(info));
  Codec codec;
  codec.format_id = id;
  codec.kind = Kind::Data;
  codec.read = [parse = std::move(parse)](ReadContext& ctx) -> Result<AnyIR> {
    CVT_TRY_ASSIGN(const std::string text, input_text(ctx));
    ir::Data data;
    CVT_TRY_ASSIGN(data.value, parse(text));
    return data;
  };
  codec.write = [write = std::move(write)](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
    CVT_TRY_ASSIGN(const std::string text, write(value.get<ir::Data>().value));
    CVT_TRY(ctx.limits.check_output(text.size()));
    return to_bytes(text);
  };
  registry.add_codec(std::move(codec));
}

int sniff_by_name(ByteView data, std::string_view filename, std::initializer_list<std::string_view> names) {
  const std::string_view name = basename(filename);
  for (const std::string_view n : names) {
    if (name == n) return util::looks_like_text(data.first(std::min<std::size_t>(data.size(), 4096))) || data.empty() ? 95 : 0;
  }
  return 0;
}

}  // namespace

void register_diff(Registry& registry) {
  for (const std::string_view id : {"diff", "patch"}) {
    FormatInfo info;
    info.id = std::string(id);
    info.name = id == "diff" ? "Diff" : "Patch";
    info.description = "Unified or context diff; renders as an inline or side-by-side document.";
    info.category = Category::Code;
    info.extensions = {std::string(id)};
    info.mime = id == "diff" ? "text/x-diff" : "text/x-patch";
    info.popularity = id == "diff" ? 30 : 30;
    info.sniff = sniff_diff;
    registry.add_format(std::move(info));

    Codec document;
    document.format_id = std::string(id);
    document.kind = Kind::Document;
    document.read = [](ReadContext& ctx) -> Result<AnyIR> {
      CVT_TRY_ASSIGN(const std::string text, input_text(ctx));
      CVT_TRY_ASSIGN(const std::vector<DiffFile> files, parse_diff(text));
      const bool side = ctx.options.get_enum("layout", "inline", {"inline", "side-by-side"}) == "side-by-side";
      return diff_document(files, side, ctx.filename);
    };
    document.read_options = {OptionSpec::choice("layout", "Layout", "inline", {"inline", "side-by-side"},
                                                "inline: one highlighted listing per file; side-by-side: a table with "
                                                "the old and new lines next to each other.")};
    registry.add_codec(std::move(document));
    Codec text;
    text.format_id = std::string(id);
    text.kind = Kind::PlainText;
    text.read = [](ReadContext& ctx) -> Result<AnyIR> {
      ir::PlainText out;
      CVT_TRY_ASSIGN(out.text, input_text(ctx));
      return out;
    };
    registry.add_codec(std::move(text));
  }

  add_data_format(
      registry, "gitignore", "Git ignore file", "A .gitignore pattern list; reads into a list of decoded patterns.",
      {"gitignore"}, 30,
      [](ByteView data, std::string_view filename) {
        return sniff_by_name(data, filename, {".gitignore", ".hgignore", ".npmignore", ".eslintignore", ".prettierignore",
                                              ".stylelintignore", ".gcloudignore", ".vercelignore"});
      },
      [](std::string_view text) -> Result<Json> { return ignore_to_data(text); }, data_to_ignore);
  add_data_format(
      registry, "dockerignore", "Docker ignore file", "A .dockerignore pattern list; reads into a list of decoded patterns.",
      {"dockerignore"}, 20,
      [](ByteView data, std::string_view filename) { return sniff_by_name(data, filename, {".dockerignore", ".containerignore"}); },
      [](std::string_view text) -> Result<Json> { return ignore_to_data(text); }, data_to_ignore);
  add_data_format(
      registry, "editorconfig", "EditorConfig file", "An .editorconfig: a root flag and properties per file glob.",
      {"editorconfig"}, 20,
      [](ByteView data, std::string_view filename) { return sniff_by_name(data, filename, {".editorconfig"}); },
      editorconfig_to_data, data_to_editorconfig);
  add_data_format(
      registry, "npmrc", "npm configuration file", "An .npmrc (also .yarnrc and similar key=value files).", {"npmrc"}, 15,
      [](ByteView data, std::string_view filename) { return sniff_by_name(data, filename, {".npmrc", ".yarnrc", ".pnpmrc"}); },
      npmrc_to_data, data_to_npmrc);
}

}  // namespace cvt::code
