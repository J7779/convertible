// Convertible core - code family: source-code documents.
//
// Every language of the highlighter's table (languages.cpp) becomes a
// readable format: a `Document` with one code block tagged with the
// language, so source files convert to HTML, PDF, DOCX, RTF, images ... with
// the document writers of other families, and to highlighted HTML/RTF/ANSI
// with this family's writers; plus a `PlainText` reader for text targets.
//
// Detection: well-known file names (Dockerfile, Makefile, CMakeLists.txt),
// shebang lines, the file extension, and a few unmistakable content markers
// (`<?php`, `#include`, a Go package clause). Sniffs never claim binary data
// or JSON/XML/HTML-looking text without a matching extension.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "engine/sniffers.hpp"
#include "families/code/code.hpp"
#include "families/code/highlight.hpp"
#include "util/text.hpp"

namespace cvt::code {

namespace {

std::vector<std::string> words(std::string_view list) {
  std::vector<std::string> out;
  for (const std::string_view part : util::split(list, ' ', false)) out.emplace_back(part);
  return out;
}

bool in_list(std::string_view list, std::string_view word) {
  for (const std::string_view part : util::split(list, ' ', false)) {
    if (util::iequals(part, word)) return true;  // file systems differ in case (CMakeLists.TXT)
  }
  return false;
}

std::string_view basename(std::string_view filename) {
  const std::size_t slash = filename.find_last_of("/\\");
  return slash == std::string_view::npos ? filename : filename.substr(slash + 1);
}

/// The interpreter named by a "#!" line: "/usr/bin/env -S python3 -u" -> "python3".
std::string_view shebang_interpreter(std::string_view text) {
  if (!text.starts_with("#!")) return {};
  std::string_view line = text.substr(2, text.find('\n') == std::string_view::npos ? text.size() : text.find('\n') - 2);
  line = util::trim(line);
  auto next_word = [&line]() {
    line = util::trim_left(line);
    const std::size_t end = line.find_first_of(" \t\r");
    const std::string_view word = line.substr(0, end);
    line = end == std::string_view::npos ? std::string_view{} : line.substr(end);
    return word;
  };
  std::string_view program = basename(next_word());
  if (program == "env") {
    do {
      program = next_word();
    } while (program.starts_with("-") || program.find('=') != std::string_view::npos);
  }
  return basename(program);
}

/// "python3.12" matches "python3" and "python"; "bash" matches "bash".
bool interpreter_matches(std::string_view interpreters, std::string_view program) {
  if (program.empty()) return false;
  if (in_list(interpreters, program)) return true;
  while (!program.empty() && (util::is_ascii_digit(program.back()) || program.back() == '.')) {
    program.remove_suffix(1);
    if (in_list(interpreters, program)) return true;
  }
  return false;
}

bool has_extension(std::string_view filename, std::string_view extensions) {
  const std::string_view name = basename(filename);
  const std::size_t dot = name.rfind('.');
  if (dot == std::string_view::npos || dot == 0) return false;
  return in_list(extensions, util::to_lower(name.substr(dot + 1)));
}

bool line_starts_with(std::string_view text, std::string_view prefix) {
  std::size_t at = 0;
  while (at < text.size()) {
    std::string_view line = text.substr(at, text.find('\n', at) - at);
    if (util::trim_left(line).starts_with(prefix)) return true;
    const std::size_t nl = text.find('\n', at);
    if (nl == std::string_view::npos) break;
    at = nl + 1;
  }
  return false;
}

// Content markers for files without a helpful name. Scores stay modest
// (just above plain text's 60) and only fire on unmistakable patterns.
int content_hint(const Language& language, std::string_view text) {
  const std::string_view id = language.id;
  if (id == "php" && util::trim_left(text).starts_with("<?php")) return 90;
  if ((id == "c" || id == "cpp") && line_starts_with(text, "#include <") && text.find(';') != std::string_view::npos) {
    const bool cpp_like = text.find("std::") != std::string_view::npos ||
                          text.find("namespace ") != std::string_view::npos ||
                          text.find("template <") != std::string_view::npos ||
                          text.find("template<") != std::string_view::npos || text.find("class ") != std::string_view::npos;
    return (id == "cpp") == cpp_like ? 64 : 0;
  }
  if (id == "go" && line_starts_with(text, "package ") && text.find("\nfunc ") != std::string_view::npos) return 66;
  if (id == "dockerfile" && util::istarts_with(util::trim_left(text), "FROM ") && line_starts_with(text, "RUN ")) {
    return 70;
  }
  return 0;
}

int sniff_source(const Language& language, ByteView data, std::string_view filename) {
  const ByteView head = data.first(std::min<std::size_t>(data.size(), 4096));
  if (!util::looks_like_text(head)) return 0;
  const std::string_view text = as_chars(head);
  const std::string_view name = basename(filename);
  if (!name.empty() && in_list(language.filenames, name)) return 95;
  if (interpreter_matches(language.interpreters, shebang_interpreter(text))) return 92;
  int hint = content_hint(language, text);
  if (hint >= 90) return hint;
  // The shared text classifier (the detection database's rules for source
  // code: keywords, idioms, shebangs) recognises unnamed files too.
  const sniff::Verdict verdict = sniff::classify(sniff::Family::Text, data, filename, language.id);
  // Capped below the certain answers above (a shebang naming bash beats the
  // classifier's generic "sh"), and well below a name that says another
  // format (a .edn file full of parentheses is not Clojure source).
  const bool ours = has_extension(filename, language.extensions);
  const bool other_name = !ours && basename(filename).find('.') != std::string_view::npos;
  if (verdict.id == language.id) hint = std::max(hint, std::min(verdict.confidence, other_name ? 62 : 88));
  if (ours) {
    // Text with the right extension. Data formats saved under a code
    // extension (a JSON file called x.js) still read fine as code.
    return std::max(hint, 80);
  }
  return hint;
}

Result<std::string> decode_source(ReadContext& ctx) {
  CVT_TRY(ctx.limits.check_decompressed(static_cast<std::uint64_t>(ctx.input.size()) * 3));
  const util::EncodingGuess guess = util::guess_encoding(ctx.input);
  if (!guess.confident) ctx.warn("The source is not valid Unicode; it was read as Windows-1252.");
  util::DecodedText decoded = util::decode_text(ctx.input, guess.encoding);
  if (decoded.replaced > 0) ctx.warn(std::to_string(decoded.replaced) + " malformed byte sequence(s) were replaced.");
  return std::move(decoded.text);
}

Result<AnyIR> read_source_document(ReadContext& ctx, const Language& language) {
  CVT_TRY_ASSIGN(std::string text, decode_source(ctx));
  text = util::convert_line_endings(text, util::LineEnding::Lf);
  if (text.ends_with('\n')) text.pop_back();
  ir::Document document;
  document.metadata.title = ctx.filename;
  if (ctx.options.get_bool("heading", false) && !ctx.filename.empty()) {
    document.blocks.push_back(ir::Heading{1, {ir::Text{ctx.filename}}});
  }
  document.blocks.push_back(ir::CodeBlock{std::string(language.id), std::move(text)});
  return document;
}

Result<AnyIR> read_source_text(ReadContext& ctx) {
  ir::PlainText out;
  CVT_TRY_ASSIGN(out.text, decode_source(ctx));
  out.source_encoding = "utf-8";
  return out;
}

}  // namespace

void register_sources(Registry& registry) {
  for (const Language& language : languages()) {
    if (language.id == "diff") continue;  // registered with the developer formats (devtools.cpp)
    FormatInfo info;
    info.id = std::string(language.id);
    info.name = std::string(language.name) + (language.id == "dockerfile" || language.id == "makefile" ? "" : " source");
    info.description = "Source code; converts to highlighted HTML, RTF or terminal text and to any document format.";
    info.category = Category::Code;
    info.extensions = words(language.extensions);
    info.mime = std::string(language.mime);
    info.popularity = language.popularity;
    info.sniff = [&language](ByteView data, std::string_view filename) { return sniff_source(language, data, filename); };
    registry.add_format(std::move(info));

    Codec document;
    document.format_id = std::string(language.id);
    document.kind = Kind::Document;
    document.read = [&language](ReadContext& ctx) { return read_source_document(ctx, language); };
    document.read_options = {OptionSpec::boolean("heading", "File name heading", false,
                                                 "Put the file name above the code as a heading.")};
    registry.add_codec(std::move(document));

    Codec text;
    text.format_id = std::string(language.id);
    text.kind = Kind::PlainText;
    text.read = read_source_text;
    registry.add_codec(std::move(text));
  }
}

}  // namespace cvt::code
