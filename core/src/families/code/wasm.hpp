// Convertible core - code family: WebAssembly disassembler (wasm.cpp).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <string>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "util/bytes.hpp"

namespace cvt::code {

/// The WebAssembly text format of a binary module, laid out like wabt's
/// wasm2wat output (indices as comments, flat instructions, block labels).
[[nodiscard]] Result<std::string> disassemble_wasm(ByteView module, const Context& ctx);

}  // namespace cvt::code
