// Convertible core - code family: internal registration entry points.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

namespace cvt {
class Registry;
}  // namespace cvt

namespace cvt::code {

void register_sources(Registry& registry);    // sources.cpp: programming/config languages as Document readers
void register_highlight(Registry& registry);  // highlight_writers.cpp: html-highlight, rtf-highlight, ansi
void register_minify(Registry& registry);     // minify.cpp: css, js, html, sql, graphql minify/prettify
void register_devtools(Registry& registry);   // devtools.cpp: diff, ignore files, curl/http, cron, dates, ids, colours
void register_wasm(Registry& registry);       // wasm.cpp: WebAssembly binary <-> text, bytecode dumps

}  // namespace cvt::code
