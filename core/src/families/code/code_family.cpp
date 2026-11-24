// Convertible core - code family.
// Source code and developer formats: every programming and configuration
// language as a readable document with syntax highlighting, token-level
// minifiers and pretty-printers, developer interchange formats (diffs, ignore
// files, cURL commands, cron expressions, ...) and WebAssembly.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/registry.hpp"
#include "families/code/code.hpp"
#include "families/families.hpp"

namespace cvt {

void register_code_family(Registry& registry) {
  code::register_sources(registry);
  code::register_highlight(registry);
  code::register_minify(registry);
  code::register_devtools(registry);
  code::register_wasm(registry);
  // Schema-bound Data codecs (see Codec::ir_tag): ignore-pattern lists and HTTP requests.
  registry.set_ir_tag({"gitignore", "dockerignore"}, Kind::Data, "ignore-patterns");
  registry.set_ir_tag({"curl", "http", "fetch-js", "python-requests"}, Kind::Data, "http-request");
  // Text tools (a date, a UUID, a colour explained) and configuration
  // writers take content read as such, never one made by a bridge.
  registry.set_only_from({"color", "datetime", "uuid-info"}, Kind::PlainText, {"txt"});
  registry.set_direct_only({"editorconfig", "npmrc", "http-headers"}, Kind::Data);
}

}  // namespace cvt
