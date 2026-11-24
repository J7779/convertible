// Convertible core - encoding family: the bytes of a file as a source-code array.
//
// Adding another language: add a value to `ArrayLanguage` (encoding.hpp), one
// row to `languages` below, and one `case` to `open_declaration`. Everything
// else (options, identifier clean-up, detection, registration) is shared.
// The row's two `shape` strings must both appear in the text written and
// tell it apart from the other languages; they drive content detection, which
// is what allows the format to share an extension with real source code.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "engine/registry.hpp"
#include "families/encoding/detail.hpp"
#include "families/encoding/encoding.hpp"
#include "families/encoding/simple.hpp"
#include "util/text.hpp"

namespace cvt::encoding {

namespace {

struct Language {
  ArrayLanguage language;
  std::string_view id;
  std::string_view name;
  std::string_view description;
  std::string_view extension;
  std::string_view mime;
  int popularity;
  std::string_view comment_prefix;
  std::string_view indent;
  std::string_view closing;                  // what follows the last element
  std::array<std::string_view, 2> shape;     // both occur in every file written
  std::string_view not_shape;                // must not occur (empty = no such rule)
  bool upper_case_name;
  bool signed_bytes;                         // values above 127 need a cast
  unsigned expansion;                        // output characters per byte with one byte per line
};

constexpr std::array<Language, 8> languages = {{
    {ArrayLanguage::Cpp, "cpp-array", "C++ array header", "The file as a constexpr std::array of bytes for C++.", "hpp",
     "text/x-c++", 25, "// ", "    ", "};", {"constexpr std::array<std::uint8_t, ", "> "}, "", false, false, 10},
    {ArrayLanguage::Python, "python-bytes", "Python bytes literal", "The file as a bytes object for Python.", "py",
     "text/x-python", 25, "# ", "    ", "])", {" = bytes([", "])"}, "", false, false, 10},
    {ArrayLanguage::JavaScript, "js-array", "JavaScript Uint8Array", "The file as a Uint8Array for JavaScript.", "js",
     "text/javascript", 25, "// ", "  ", "]);", {"const ", " = new Uint8Array(["}, "", false, false, 8},
    {ArrayLanguage::Java, "java-array", "Java byte array", "The file as a byte[] initialiser for Java.", "java",
     "text/x-java", 15, "// ", "    ", "};", {"byte[] ", " = {"}, "new byte[]", false, true, 17},
    {ArrayLanguage::Go, "go-array", "Go byte slice", "The file as a []byte literal for Go.", "go", "text/x-go", 15, "// ",
     "\t", "}", {"var ", " = []byte{"}, "", false, false, 7},
    {ArrayLanguage::Rust, "rust-array", "Rust byte array", "The file as a static [u8; N] array for Rust.", "rs",
     "text/x-rust", 15, "// ", "    ", "];", {"pub static ", ": [u8; "}, "", true, false, 10},
    {ArrayLanguage::Swift, "swift-array", "Swift byte array", "The file as a [UInt8] array for Swift.", "swift",
     "text/x-swift", 12, "// ", "    ", "]", {"let ", ": [UInt8] = ["}, "", false, false, 10},
    {ArrayLanguage::CSharp, "csharp-array", "C# byte array", "The file as a byte[] initialiser for C#.", "cs",
     "text/x-csharp", 15, "// ", "    ", "};", {"byte[] ", " = new byte[] {"}, "", false, false, 10},
}};

constexpr const Language& language_info(ArrayLanguage language) noexcept {
  for (const Language& entry : languages) {
    if (entry.language == language) return entry;
  }
  return languages[0];
}

// Words that cannot be used as a variable name in at least one of the
// languages above. An identifier that collides gets a trailing underscore.
constexpr auto reserved_words = std::to_array<std::string_view>({
    "False", "None", "Self", "True", "abstract", "and", "as", "assert", "async", "auto", "await", "base", "bool",
    "boolean", "break", "byte", "case", "catch", "chan", "char", "class", "const", "continue", "crate", "decimal",
    "def", "default", "defer", "del", "delete", "do", "double", "elif", "else", "enum", "except", "export",
    "extends", "extern", "false", "final", "finally", "fixed", "float", "fn", "for", "friend", "from", "func",
    "function", "global", "go", "goto", "guard", "if", "impl", "import", "in", "init", "inline", "int", "interface",
    "internal", "is", "lambda", "let", "lock", "long", "map", "match", "mod", "mut", "namespace", "native", "new",
    "nil", "not", "null", "object", "operator", "or", "out", "override", "package", "pass", "private", "protected",
    "pub", "public", "raise", "range", "ref", "register", "return", "self", "short", "signed", "sizeof", "static",
    "string", "struct", "super", "switch", "template", "this", "throw", "trait", "true", "try", "type", "typedef",
    "typeof", "union", "unsigned", "use", "using", "var", "virtual", "void", "volatile", "where", "while", "with",
    "yield",
});

bool is_reserved_word(std::string_view word) {
  return std::ranges::find(reserved_words, word) != reserved_words.end();
}

// A comment must stay on its line whatever the file was called: control
// characters and the Unicode line terminators become spaces.
std::string single_line(std::string_view text) {
  const std::string valid = util::sanitize_utf8(text);
  std::string out;
  out.reserve(valid.size());
  for (std::size_t i = 0; i < valid.size();) {
    const util::Utf8Decoded decoded = util::decode_utf8(valid, i);
    const char32_t cp = decoded.code_point;
    const bool breaks_line = cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp == 0x2028 || cp == 0x2029;
    if (breaks_line) {
      out.push_back(' ');
    } else {
      out.append(valid, i, decoded.length);
    }
    i += decoded.length;
  }
  return out;
}

// The text up to and including the opening bracket of the element list.
std::string open_declaration(ArrayLanguage language, std::string_view name, std::size_t size) {
  switch (language) {
    case ArrayLanguage::Cpp:
      return std::format("#pragma once\n\n#include <array>\n#include <cstdint>\n\n"
                         "inline constexpr std::array<std::uint8_t, {}> {} = {{",
                         size, name);
    case ArrayLanguage::Python: return std::format("{} = bytes([", name);
    case ArrayLanguage::JavaScript: return std::format("const {} = new Uint8Array([", name);
    case ArrayLanguage::Java: return std::format("byte[] {} = {{", name);
    case ArrayLanguage::Go: return std::format("var {} = []byte{{", name);
    case ArrayLanguage::Rust: return std::format("pub static {}: [u8; {}] = [", name, size);
    case ArrayLanguage::Swift: return std::format("let {}: [UInt8] = [", name);
    case ArrayLanguage::CSharp: return std::format("byte[] {} = new byte[] {{", name);
  }
  return {};
}

int sniff_array(const Language& spec, ByteView data) {
  const std::string_view text = as_chars(data).substr(0, 1024);
  for (const std::string_view needle : spec.shape) {
    if (text.find(needle) == std::string_view::npos) return 0;
  }
  if (!spec.not_shape.empty() && text.find(spec.not_shape) != std::string_view::npos) return 0;
  return 82;
}

}  // namespace

std::string array_identifier(std::string_view text, ArrayLanguage language) {
  std::string name = c_identifier(text);
  if (language_info(language).upper_case_name) name = util::to_upper(name);
  if (is_reserved_word(name)) name.push_back('_');
  return name;
}

std::string array_literal(ByteView data, ArrayLanguage language, std::string_view variable_name,
                          std::size_t bytes_per_line, std::string_view comment) {
  const Language& spec = language_info(language);
  bytes_per_line = std::clamp<std::size_t>(bytes_per_line, 1, 256);
  std::string out;
  out.reserve(data.size() * 6 + data.size() / bytes_per_line * 6 + comment.size() + 160);
  if (!comment.empty()) {
    out.append(spec.comment_prefix);
    out.append(single_line(comment));
    out.push_back('\n');
  }
  out.append(open_declaration(language, array_identifier(variable_name, language), data.size()));
  for (std::size_t i = 0; i < data.size(); ++i) {
    if (i % bytes_per_line == 0) {
      out.push_back('\n');
      out.append(spec.indent);
    } else {
      out.push_back(' ');
    }
    const std::uint8_t value = detail::byte_at(data, i);
    if (spec.signed_bytes && value > 127) out.append("(byte) ");
    out.append("0x");
    detail::append_hex_byte(out, value, false);
    out.push_back(',');
  }
  out.push_back('\n');
  out.append(spec.closing);
  out.push_back('\n');
  return out;
}

void register_array_literals(Registry& registry) {
  for (const Language& spec : languages) {
    const ArrayLanguage language = spec.language;
    add_binary_encoding(registry, {
        .id = std::string(spec.id), .name = std::string(spec.name), .description = std::string(spec.description),
        .extensions = {std::string(spec.extension)}, .mime = std::string(spec.mime), .popularity = spec.popularity,
        .encode = [language](const ir::Blob& blob, const WriteContext& ctx) -> Result<std::string> {
          std::string name = ctx.options.get_string("variable_name");
          if (name.empty()) name = blob.filename.empty() ? ctx.source_filename : blob.filename;
          std::string comment;
          if (!blob.filename.empty()) comment = std::format("{}, {} bytes", blob.filename, blob.bytes.size());
          return array_literal(blob.bytes, language, name,
                               static_cast<std::size_t>(ctx.options.get_int("bytes_per_line", 12, 1, 64)), comment);
        },
        .encode_options = {OptionSpec::text("variable_name", "Variable name", "",
                                            "Name of the array; empty derives it from the file name."),
                           OptionSpec::integer("bytes_per_line", "Bytes per line", 12, 1, 64,
                                               "Array elements on each line.")},
        .expansion = spec.expansion,
        .sniff = [&spec](ByteView data, std::string_view /*filename*/) { return sniff_array(spec, data); },
    });
  }
}

}  // namespace cvt::encoding
