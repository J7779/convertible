// Convertible core - code family: table-driven syntax highlighting.
//
// `tokenize(code, language)` splits source text into classified tokens with
// one linear pass of a small lexer that is configured per language by a
// `Lexicon` (comment markers, string forms, keyword lists, ...). The same
// token stream drives the highlighted writers of this family (HTML, RTF,
// ANSI) and is meant to be adopted by other writers (PDF, HTML documents)
// for code blocks: include this header and call `tokenize`.
//
// Guarantees: every byte of the input belongs to exactly one token, tokens
// are in order and never empty, the run time is linear in the input size,
// and any byte sequence (invalid UTF-8, NULs, binary) is accepted.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cvt::code {

enum class TokenClass : std::uint8_t {
  Plain,         ///< identifiers, whitespace, anything unclassified
  Keyword,       ///< reserved words and control flow
  Type,          ///< built-in and conventional type names
  String,        ///< string and character literals (including delimiters)
  Number,        ///< numeric literals
  Comment,       ///< comments and documentation comments
  Operator,      ///< operators and punctuation
  Preprocessor,  ///< directives, annotations, decorators, attributes
  Function,      ///< an identifier that is called or defined as a function
  Constant,      ///< true/false/null, ALL_CAPS names, built-in constants
  Variable,      ///< sigil variables ($x in shells, PHP, Perl), CSS properties
  Tag,           ///< markup tag names
  Attribute,     ///< markup attribute names
  Inserted,      ///< diff: added lines
  Deleted,       ///< diff: removed lines
  Heading,       ///< diff hunk headers, section titles
};
inline constexpr std::size_t token_class_count = 16;

/// Short stable name ("keyword", "string", ...) used for CSS classes and tests.
[[nodiscard]] std::string_view token_class_name(TokenClass cls) noexcept;

struct Token {
  std::size_t offset = 0;
  std::size_t length = 0;
  TokenClass cls = TokenClass::Plain;
};

/// How a language is lexed. Every field is optional; the defaults describe a
/// language without comments, strings or keywords.
struct Lexicon {
  std::string_view line_comments{};   ///< space-separated markers: "//", "#", "--", ";", "%", "'" ...
  std::string_view block_comments{};  ///< space-separated open/close pairs: "/* */ <!-- -->"
  bool nested_comments = false;     ///< block comments nest (Rust, Swift, Haskell, Scala, D, Nim ...)
  std::string_view quotes = "\"";   ///< characters that open a string ending at the same character
  char escape = '\\';               ///< escape character inside strings; 0 = none (doubled quotes)
  bool multiline_strings = false;   ///< strings may span lines (shell, SQL, Lua, PHP ...)
  bool triple_quotes = false;       ///< """...""" and '''...''' (Python, Kotlin, Swift, Julia, Scala ...)
  bool template_literals = false;   ///< `...${expr}...` (JavaScript, TypeScript)
  bool rust_raw_strings = false;    ///< r"...", r#"..."#, b"..."
  bool cpp_raw_strings = false;     ///< R"delim(...)delim"
  bool char_literals = false;       ///< 'x' is a character literal even when ' is not in `quotes` (Rust lifetimes are not)
  bool lua_long_brackets = false;   ///< [[...]], [==[...]==] strings and --[[ ]] comments
  bool hash_preprocessor = false;   ///< a '#' at the start of a line begins a directive (C, C#, ...)
  bool at_annotations = false;      ///< @name is an annotation/decorator
  bool dollar_variables = false;    ///< $name and ${...} are variables
  bool case_insensitive = false;    ///< keywords match in any case (SQL, Pascal, Fortran, VHDL, ...)
  bool capitalized_types = false;   ///< identifiers starting with an upper-case letter are types
  bool upper_constants = false;     ///< ALL_CAPS identifiers are constants
  bool lisp = false;                ///< identifiers may contain - ? ! * + < > = / and ' quotes a datum
  bool markup = false;              ///< <tag attr="..."> structure (HTML-like templates)
  bool diff = false;                ///< line-oriented unified/context diff
  bool css = false;                 ///< property names before ':' are variables, #abc and units are numbers
  std::string_view identifier_extra{};  ///< further identifier characters ("$" for JS, "?!" for Ruby)
  std::string_view keywords{};        ///< space-separated
  std::string_view types{};
  std::string_view constants{};
  std::string_view builtins{};        ///< highlighted as functions
};

/// A language the highlighter and the source-code readers know.
struct Language {
  std::string_view id{};            ///< format id, normally the canonical extension ("py")
  std::string_view name{};          ///< "Python"
  std::string_view extensions{};    ///< space-separated, first = default ("py pyw pyi")
  std::string_view mime{};
  int popularity;                 ///< 0..100
  std::string_view interpreters{};  ///< space-separated shebang interpreter names ("python python3")
  std::string_view filenames{};     ///< space-separated exact file names ("Dockerfile Containerfile")
  std::string_view aliases{};       ///< space-separated other names ("python3 py3"), used by find_language
  const Lexicon* lexicon;
};

/// Every language, in registration order.
[[nodiscard]] std::span<const Language> languages() noexcept;
/// By id, alias, name or extension (ASCII case-insensitive); nullptr when unknown.
[[nodiscard]] const Language* find_language(std::string_view name) noexcept;

/// The token stream of `code` lexed as `lexicon`.
[[nodiscard]] std::vector<Token> tokenize(std::string_view code, const Lexicon& lexicon);
/// Convenience: lexed as the language named `language`; one Plain token when unknown.
[[nodiscard]] std::vector<Token> tokenize(std::string_view code, std::string_view language);

}  // namespace cvt::code
