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
}

}  // namespace cvt
