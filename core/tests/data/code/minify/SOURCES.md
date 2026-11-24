# Sources: code/minify

Hand-written edge-case inputs for `core/tests/families/code/minify_test.cpp` (made up for these tests,
no third-party content), plus one golden output.

| File | Contents |
| --- | --- |
| `tricky.js` | regex vs division (also across a line break), regexes with `/` in classes and escapes, nested template literals with `}` and `{` inside, strings holding `//` and `/*`, ASI cases (`return` + newline, `++` on a new line, `let` without semicolons), unary/binary `+ +` and `- --`, `1 .toString()`, labels, tagged templates, private fields, computed class members, async/await, a hashbang and a `/*!` banner |
| `tricky.expected.min.js` | the output of our `js-min` writer for `tricky.js` (golden file) |
| `tricky.css` | `@charset`, `@import url(...)` with a media query, custom properties (one holding a `{}` block), combinators, `calc()` with nested `var()`, unquoted `url()` with a space, strings with `;` and `}`, `!important`, `a :first-child` vs `a:hover`, `@media`, attribute selectors, CSS nesting with `&` |
| `report.sql` | a SELECT with a join, a string containing `''` and `--`, line and block comments, a sub-query, GROUP BY/HAVING/ORDER BY/LIMIT |
| `hero.graphql` | a query with variables and defaults, a directive, a fragment spread and a fragment definition, a comment and an insignificant comma |
| `page.html` | comments, a conditional comment, `<pre>`, `<style>`, `<script>`, a multi-space attribute value, text spread over lines |

Behaviour preservation was checked while developing (not by the test suite):

- JavaScript: `node --check` accepts our `js-min` and `js-pretty` output of `tricky.js`, and running the
  original, the minified and the pretty-printed file with Node 26 prints identical output.
- CSS: `tinycss2` 1.5.1 parses `tricky.css`, our minified and our pretty-printed versions to the same
  component trees (ignoring whitespace, comments and the block-final `;` the minifier drops).
- SQL: `sqlglot` 30.34 parses `report.sql`, our minified and pretty versions to the same ASTs (comments excluded).
- GraphQL: `graphql-core` 3.3.2 `print_ast(parse(...))` is identical for the original, minified and pretty versions.
- HTML: Python's `html.parser` yields the same tags, attributes and (whitespace-collapsed) text for
  `page.html` and our minified output; `pre`/`script`/`style` content is byte-identical.
