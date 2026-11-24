// Tests of the syntax highlighter (highlight.hpp), the source-code readers
// and the highlighted writers (html-highlight, rtf-highlight, ansi).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "engine/detect.hpp"
#include "engine/registry.hpp"
#include "families/code/highlight.hpp"
#include "support/test_support.hpp"
#include "util/text.hpp"

using namespace cvt;
using namespace std::string_view_literals;
using code::TokenClass;
using code::tokenize;

namespace {

/// The class of the token that covers the first occurrence of `needle`;
/// fails the test when the needle is split across tokens.
TokenClass class_of(std::string_view source, std::string_view language, std::string_view needle,
                    std::size_t occurrence = 0) {
  std::size_t at = source.find(needle);
  for (std::size_t n = 0; n < occurrence && at != std::string_view::npos; ++n) at = source.find(needle, at + 1);
  REQUIRE_MESSAGE(at != std::string_view::npos, needle);
  for (const code::Token& token : tokenize(source, language)) {
    if (token.offset <= at && at < token.offset + token.length) {
      CHECK_MESSAGE(at + needle.size() <= token.offset + token.length, "token boundary inside '", needle, "'");
      return token.cls;
    }
  }
  FAIL("no token covers the needle");
  return TokenClass::Plain;
}

bool covers_exactly(std::string_view source, const std::vector<code::Token>& tokens) {
  std::size_t at = 0;
  for (const code::Token& token : tokens) {
    if (token.offset != at || token.length == 0) return false;
    at += token.length;
  }
  return at == source.size();
}

}  // namespace

TEST_CASE("code/highlight: C and C++ token classes") {
  constexpr std::string_view c = "#include <stdio.h>\n/* block */ int main(void) {\n  // line\n  char c = 'x';\n"
                                 "  printf(\"%d\\n\", 0x1Fu + 1.5e-3);\n  return MAX_SIZE;\n}\n";
  CHECK(class_of(c, "c", "#include <stdio.h>") == TokenClass::Preprocessor);
  CHECK(class_of(c, "c", "/* block */") == TokenClass::Comment);
  CHECK(class_of(c, "c", "int") == TokenClass::Type);
  CHECK(class_of(c, "c", "main") == TokenClass::Function);
  CHECK(class_of(c, "c", "// line") == TokenClass::Comment);
  CHECK(class_of(c, "c", "'x'") == TokenClass::String);
  CHECK(class_of(c, "c", "\"%d\\n\"") == TokenClass::String);
  CHECK(class_of(c, "c", "0x1Fu") == TokenClass::Number);
  CHECK(class_of(c, "c", "1.5e-3") == TokenClass::Number);
  CHECK(class_of(c, "c", "return") == TokenClass::Keyword);
  CHECK(class_of(c, "c", "MAX_SIZE") == TokenClass::Constant);
  constexpr std::string_view cpp = "auto s = R\"x(raw \"quoted\" )x\";\nclass Widget final {};\nstd::size_t n = nullptr;\n";
  CHECK(class_of(cpp, "cpp", "R\"x(raw \"quoted\" )x\"") == TokenClass::String);
  CHECK(class_of(cpp, "cpp", "Widget") == TokenClass::Type);
  CHECK(class_of(cpp, "cpp", "nullptr") == TokenClass::Constant);
  CHECK(class_of(cpp, "cpp", "final") == TokenClass::Keyword);
}

TEST_CASE("code/highlight: Python, Ruby and shell") {
  constexpr std::string_view py = "@decorator\ndef area(r: float) -> float:\n    \"\"\"Doc\n    string.\"\"\"\n"
                                  "    return 3.14 * r ** 2  # pi r squared\nprint(f'{x}', rb\"\\x00\", None)\n";
  CHECK(class_of(py, "py", "@decorator") == TokenClass::Preprocessor);
  CHECK(class_of(py, "py", "def") == TokenClass::Keyword);
  CHECK(class_of(py, "py", "area") == TokenClass::Function);
  CHECK(class_of(py, "py", "float") == TokenClass::Type);
  CHECK(class_of(py, "py", "\"\"\"Doc\n    string.\"\"\"") == TokenClass::String);
  CHECK(class_of(py, "py", "# pi r squared") == TokenClass::Comment);
  CHECK(class_of(py, "py", "f'{x}'") == TokenClass::String);
  CHECK(class_of(py, "py", "rb\"\\x00\"") == TokenClass::String);
  CHECK(class_of(py, "py", "None") == TokenClass::Constant);
  CHECK(class_of(py, "py", "print") == TokenClass::Function);
  constexpr std::string_view rb = "def empty?(list) = list.size.zero?\n=begin\ndocs\n=end\nputs :sym, nil\n";
  CHECK(class_of(rb, "rb", "empty?") == TokenClass::Function);
  CHECK(class_of(rb, "rb", "=begin\ndocs\n=end") == TokenClass::Comment);
  CHECK(class_of(rb, "rb", "nil") == TokenClass::Constant);
  constexpr std::string_view sh = "#!/bin/sh\nfor f in \"$@\"; do echo \"${f%.txt}\" $HOME a#b; done # end\n";
  CHECK(class_of(sh, "sh", "#!/bin/sh") == TokenClass::Comment);
  CHECK(class_of(sh, "sh", "for") == TokenClass::Keyword);
  CHECK(class_of(sh, "sh", "$HOME") == TokenClass::Variable);
  CHECK(class_of(sh, "sh", "# end") == TokenClass::Comment);
  CHECK(class_of(sh, "sh", "done") == TokenClass::Keyword);  // "a#b" did not start a comment
}

TEST_CASE("code/highlight: JavaScript, Rust, Go, SQL, Lua, Lisp, CSS") {
  constexpr std::string_view js = "const t = `a ${b + `c`} d`; // tail\nlet $el = document.querySelector('#x');\n";
  CHECK(class_of(js, "js", "`a ${b + `c`} d`") == TokenClass::String);
  CHECK(class_of(js, "js", "// tail") == TokenClass::Comment);
  CHECK(class_of(js, "js", "$el") == TokenClass::Plain);
  CHECK(class_of(js, "js", "querySelector") == TokenClass::Function);
  constexpr std::string_view rs = "fn longest<'a>(x: &'a str) -> char { let c = '\\n'; r#\"raw \"#; /* /* nested */ */ }\n";
  CHECK(class_of(rs, "rs", "longest") == TokenClass::Function);
  CHECK(class_of(rs, "rs", "str") == TokenClass::Type);
  CHECK(class_of(rs, "rs", "'\\n'") == TokenClass::String);
  CHECK(class_of(rs, "rs", "r#\"raw \"#") == TokenClass::String);
  CHECK(class_of(rs, "rs", "/* /* nested */ */") == TokenClass::Comment);
  CHECK(class_of(rs, "rs", "'a") != TokenClass::String);  // a lifetime is not a character literal
  constexpr std::string_view go = "package main\nfunc main() { x := `raw\nstring`; fmt.Println(len(x), nil) }\n";
  CHECK(class_of(go, "go", "`raw\nstring`") == TokenClass::String);
  CHECK(class_of(go, "go", "len") == TokenClass::Function);
  CHECK(class_of(go, "go", "nil") == TokenClass::Constant);
  constexpr std::string_view sql = "SeLeCt name, COUNT(*) FROM users -- note\nWHERE nick = 'O''Brien';\n";
  CHECK(class_of(sql, "sql", "SeLeCt") == TokenClass::Keyword);
  CHECK(class_of(sql, "sql", "COUNT") == TokenClass::Function);
  CHECK(class_of(sql, "sql", "-- note") == TokenClass::Comment);
  CHECK(class_of(sql, "sql", "'O''Brien'") == TokenClass::String);
  constexpr std::string_view lua = "local s = [==[long ]] string]==] --[[ block\ncomment ]] print(s)\n";
  CHECK(class_of(lua, "lua", "[==[long ]] string]==]") == TokenClass::String);
  CHECK(class_of(lua, "lua", "--[[ block\ncomment ]]") == TokenClass::Comment);
  constexpr std::string_view clj = "(defn square [x] (* x x)) ; comment\n(my-fn? 1)\n";
  CHECK(class_of(clj, "clj", "defn") == TokenClass::Keyword);
  CHECK(class_of(clj, "clj", "my-fn?") == TokenClass::Function);
  CHECK(class_of(clj, "clj", "; comment") == TokenClass::Comment);
  constexpr std::string_view css = "@media screen { a.link:hover { color: #ff0000; margin: 1.5em; } }\n";
  CHECK(class_of(css, "css", "@media") == TokenClass::Preprocessor);
  CHECK(class_of(css, "css", "color") == TokenClass::Variable);
  CHECK(class_of(css, "css", "#ff0000") == TokenClass::Number);
  CHECK(class_of(css, "css", "1.5em") == TokenClass::Number);
}

TEST_CASE("code/highlight: markup components embed script and style") {
  constexpr std::string_view vue = "<template>\n  <a href=\"/x\" :class=\"c\">Hi</a>\n</template>\n<!-- note -->\n"
                                   "<script>\nexport default { data() { return 1 } }\n</script>\n"
                                   "<style scoped>\n.a { color: red }\n</style>\n";
  CHECK(class_of(vue, "vue", "template") == TokenClass::Tag);
  CHECK(class_of(vue, "vue", "href") == TokenClass::Attribute);
  CHECK(class_of(vue, "vue", "\"/x\"") == TokenClass::String);
  CHECK(class_of(vue, "vue", "<!-- note -->") == TokenClass::Comment);
  CHECK(class_of(vue, "vue", "export") == TokenClass::Keyword);
  CHECK(class_of(vue, "vue", "color") == TokenClass::Variable);
  CHECK(covers_exactly(vue, tokenize(vue, "vue")));
}

TEST_CASE("code/highlight: unified diffs") {
  constexpr std::string_view diff = "--- a/x\n+++ b/x\n@@ -1,2 +1,2 @@\n context\n-old\n+new\n";
  CHECK(class_of(diff, "diff", "--- a/x") == TokenClass::Heading);
  CHECK(class_of(diff, "diff", "@@ -1,2 +1,2 @@") == TokenClass::Preprocessor);
  CHECK(class_of(diff, "diff", "-old") == TokenClass::Deleted);
  CHECK(class_of(diff, "diff", "+new") == TokenClass::Inserted);
}

TEST_CASE("code/highlight: every language covers any input exactly, in linear time") {
  std::vector<std::string> inputs = {
      "", "\"", "'", "/*", "<!--", "R\"(", "r#\"", "`${", "[[", "--[==[", "#", "@", "$", "${", "<a b='", "\\",
      std::string(test::str(test::random_bytes(4000, 7))), std::string(test::str(test::all_byte_values())),
  };
  // Adversarial repetitions that would be quadratic for a backtracking lexer.
  for (const std::string_view unit : {"/*", "\"", "${", "<a ", "[[", "R\"x(", "'", "#[", "<script>", "--[[", "@\""}) {
    std::string big;
    while (big.size() < 16000) big += unit;
    inputs.push_back(std::move(big));
  }
  std::vector<const code::Lexicon*> seen;
  for (const code::Language& language : code::languages()) {
    if (std::ranges::find(seen, language.lexicon) != seen.end()) continue;  // shared lexicons once
    seen.push_back(language.lexicon);
    CAPTURE(language.id);
    for (const std::string& input : inputs) {
      const std::vector<code::Token> tokens = tokenize(input, *language.lexicon);
      CHECK(covers_exactly(input, tokens));
    }
  }
  CHECK(tokenize("plain", "no-such-language").size() == 1);
  CHECK(code::find_language("Python") == code::find_language("py"));
  CHECK(code::find_language("python3") == code::find_language("py"));
  CHECK(code::find_language("tsx")->id == "tsx");
}

TEST_CASE("code/sources: languages are detected by name, shebang, extension and content") {
  const auto detected = [](std::string_view data, std::string_view filename) -> std::string {
    const auto detection = detect_format(Registry::instance(), as_bytes(data), filename);
    return detection ? std::string(detection->format->id) : std::string("none");
  };
  CHECK(detected("#!/usr/bin/env python3\nprint(1)\n", "tool") == "py");
  CHECK(detected("#!/bin/bash\necho hi\n", "run") == "bash");
  CHECK(detected("#!/usr/bin/env -S node --harmony\nconsole.log(1)\n", "cli") == "js");
  CHECK(detected("FROM alpine:3\nRUN apk add curl\n", "Dockerfile") == "dockerfile");
  CHECK(detected("all:\n\techo hi\n", "Makefile") == "makefile");
  CHECK(detected("cmake_minimum_required(VERSION 3.22)\n", "CMakeLists.txt") == "cmake");
  CHECK(detected("<?php echo 1;\n", "") == "php");
  CHECK(detected("#include <stdio.h>\nint main(void) { return 0; }\n", "") == "c");
  CHECK(detected("#include <vector>\nstd::vector<int> v;\n", "") == "cpp");
  CHECK(detected("package main\n\nfunc main() {}\n", "") == "go");
  CHECK(detected("SELECT 1;\n", "q.sql") == "sql");
  CHECK(detected("query { me { id } }\n", "q.graphql") == "graphql");
  CHECK(detected("resource \"x\" \"y\" {}\n", "main.tf") == "tf");
  CHECK(detected("interface A {}\n", "a.ts") == "ts");
  CHECK(detected("Dear diary,\nnothing to see.\n", "") == "txt");
  // Binary data with a code extension (an MPEG transport stream called .ts) is not claimed.
  Bytes ts(188 * 4);
  for (std::size_t i = 0; i < ts.size(); i += 188) ts[i] = std::byte{0x47};
  const auto binary = detect_format(Registry::instance(), ts, "video.ts");
  CHECK((!binary.has_value() || binary->format->id != "ts" || binary->confidence <= 25));
}

TEST_CASE("code/sources: readers produce a code block and plain text") {
  constexpr std::string_view source = "fn main() {\r\n    println!(\"hi\");\r\n}\r\n";
  const auto document = test::read_as<ir::Document>("rs", as_bytes(source), {}, nullptr);
  REQUIRE_OK(document);
  REQUIRE(document->blocks.size() == 1);
  const auto* block = document->blocks[0].get_if<ir::CodeBlock>();
  REQUIRE(block != nullptr);
  CHECK(block->language == "rs");
  CHECK(block->code == "fn main() {\n    println!(\"hi\");\n}");
  const auto headed = test::read_as<ir::Document>("rs", as_bytes(source), R"({"heading":true})");
  REQUIRE_OK(headed);
  const auto text = test::read_as<ir::PlainText>("rs", as_bytes(source));
  REQUIRE_OK(text);
  CHECK(text->text == source);
  // UTF-16 sources are decoded.
  const Bytes utf16 = util::encode_text("x = 'é'\n", util::TextEncoding::Utf16LE, true).bytes;
  const auto decoded = test::read_as<ir::PlainText>("py", utf16);
  REQUIRE_OK(decoded);
  CHECK(decoded->text == "x = 'é'\n");
  CHECK(test::convert(source, "rs", "txt").text() == source);
}

TEST_CASE("code/html-highlight: standalone page, themes, line numbers") {
  constexpr std::string_view source = "def f():\n    return 1  # one\n";
  const test::Converted page = test::convert(source, "py", "html-highlight", {}, "f.py");
  REQUIRE(page.ok());
  const std::string html = page.text();
  CHECK(html.starts_with("<!DOCTYPE html>"));
  CHECK(html.find("<title>f.py</title>") != std::string::npos);
  CHECK(html.find("<span class=\"hl-keyword\">def</span>") != std::string::npos);
  CHECK(html.find("<span class=\"hl-comment\"># one</span>") != std::string::npos);
  CHECK(html.find("prefers-color-scheme: dark") != std::string::npos);
  CHECK(test::convert(source, "py", "html-highlight", R"({"theme":"light"})").text().find("prefers-color-scheme") ==
        std::string::npos);
  const std::string numbered = test::convert(source, "py", "html-highlight", R"({"line_numbers":true})").text();
  CHECK(numbered.find("<span class=\"ln\">") != std::string::npos);
  const std::string fragment = test::convert(source, "py", "html-highlight", R"({"standalone":false})").text();
  CHECK(fragment.starts_with("<pre class=\"hl\">"));
  // HTML special characters are escaped and multi-line tokens keep numbered lines well-formed.
  const std::string tricky =
      test::convert("/* a\n<b> */ x = \"&\";\n", "c", "html-highlight", R"({"line_numbers":true,"standalone":false})")
          .text();
  CHECK(tricky.find("&lt;b&gt;") != std::string::npos);
  CHECK(tricky.find("&quot;&amp;&quot;") != std::string::npos);
  std::size_t opened = 0, closed = 0;
  for (std::size_t at = tricky.find("<span"); at != std::string::npos; at = tricky.find("<span", at + 1)) ++opened;
  for (std::size_t at = tricky.find("</span>"); at != std::string::npos; at = tricky.find("</span>", at + 1)) ++closed;
  CHECK(opened == closed);
  // The language option overrides the source format.
  CHECK(test::convert("SELECT 1", "txt", "html-highlight", R"({"language":"sql"})").text().find("hl-keyword") !=
        std::string::npos);
}

TEST_CASE("code/rtf-highlight and code/ansi: coloured output and ANSI input") {
  const std::string rtf = test::convert("int x = 1; // c\n", "c", "rtf-highlight", R"({"font_size":12})").text();
  CHECK(rtf.starts_with("{\\rtf1"));
  CHECK(rtf.find("\\colortbl") != std::string::npos);
  CHECK(rtf.find("\\fs24") != std::string::npos);
  CHECK(rtf.find("// c") != std::string::npos);
  CHECK(test::convert("s = \"\xC3\xA9{}\"\n", "py", "rtf-highlight").text().find("\\u233?\\{\\}") != std::string::npos);

  const std::string ansi = test::convert("if x:\n    pass\n", "py", "ansi").text();
  CHECK(ansi.find("\x1b[1;31mif\x1b[0m") != std::string::npos);
  CHECK(test::convert("if x: pass\n", "py", "ansi", R"({"colors":"256"})").text().find("\x1b[1;38;5;203m") !=
        std::string::npos);
  CHECK(test::convert("if x: pass\n", "py", "ansi", R"({"colors":"truecolor"})").text().find("\x1b[1;38;2;") !=
        std::string::npos);
  // Control characters in the source cannot reach the terminal.
  CHECK(test::convert("x = 1 \x1b[2J\n", "py", "ansi").text().find("\x1b[2J") == std::string::npos);

  // Coloured logs: escapes are removed when reading.
  constexpr std::string_view log = "\x1b[32mINFO\x1b[0m started\n\x1b[1;31mERROR\x1b[0m failed \x1b]0;title\x07\n";
  CHECK(test::convert(log, "ansi", "txt").text() == "INFO started\nERROR failed \n");
  const auto detection = detect_format(Registry::instance(), as_bytes(log), "build.log");
  REQUIRE(detection.has_value());
  CHECK(detection->format->id == "ansi");
  const test::Converted html = test::convert(log, "ansi", "html-highlight");
  CHECK(html.ok());
  CHECK(html.text().find("ERROR failed") != std::string::npos);
  CHECK(test::fuzz_reader<ir::Document>("ansi", as_bytes(log)) > 0);
}
