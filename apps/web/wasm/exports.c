/*
 * Convertible web app - WebAssembly entry translation unit.
 *
 * The module exports the public C ABI of the core (convertible.h) plus
 * malloc/free and nothing else; the export list lives in
 * apps/web/CMakeLists.txt (-sEXPORTED_FUNCTIONS). The module has no main
 * (linked with --no-entry). This file only exists because an executable
 * target needs at least one source.
 *
 * Copyright (c) Convertible contributors. MIT licence; see LICENSE.
 */
#include "convertible/convertible.h"
