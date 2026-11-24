// Convertible core - code family: token-level minifiers and pretty-printers
// (minify.cpp). They only remove or re-flow whitespace and comments; no
// identifier is renamed and no expression is rewritten.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <string>
#include <string_view>

#include "engine/errors.hpp"

namespace cvt::code {

struct MinifyOptions {
  bool keep_license = true;  ///< keep /*! ... */ comments (licences, banners)
};

/// CSS: comments, redundant whitespace and the last ';' of each block go;
/// strings, url(...) and calc() spacing are untouched.
[[nodiscard]] Result<std::string> minify_css(std::string_view css, const MinifyOptions& options = {});
[[nodiscard]] Result<std::string> pretty_css(std::string_view css, int indent = 2);

/// JavaScript: comments and whitespace go; line breaks stay wherever
/// automatic semicolon insertion could depend on them. Strings, template
/// literals and regular expression literals are copied verbatim.
[[nodiscard]] Result<std::string> minify_js(std::string_view js, const MinifyOptions& options = {});
[[nodiscard]] Result<std::string> pretty_js(std::string_view js, int indent = 2);

/// HTML: comments go (except conditional comments), whitespace runs become
/// one space; <pre>, <textarea>, <script> and <style> content is kept as is.
[[nodiscard]] Result<std::string> minify_html(std::string_view html, const MinifyOptions& options = {});

/// SQL: comments go (optimizer hints /*+ */ and /*! */ stay), whitespace collapses.
[[nodiscard]] Result<std::string> minify_sql(std::string_view sql);
/// One clause per line, upper-case keywords (`keyword_case`: upper, lower, keep).
[[nodiscard]] Result<std::string> pretty_sql(std::string_view sql, std::string_view keyword_case = "upper", int indent = 2);

[[nodiscard]] Result<std::string> minify_graphql(std::string_view graphql);
[[nodiscard]] Result<std::string> pretty_graphql(std::string_view graphql, int indent = 2);

}  // namespace cvt::code
