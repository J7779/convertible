// Convertible core - archive family: Blob <-> Archive bridges (see bridges.cpp).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <string_view>

#include "engine/errors.hpp"
#include "ir/archive.hpp"
#include "ir/blob.hpp"

namespace cvt {
class Registry;
}

namespace cvt::archive {

/// One-entry archive holding the blob, named after its file name (or
/// "data.<extension of its format>" when it has none).
[[nodiscard]] Result<ir::Archive> wrap_blob(ir::Blob blob, const Registry* registry);
/// The file named `wanted`, or the only file when `wanted` is empty.
[[nodiscard]] Result<ir::Blob> extract_entry(ir::Archive archive, std::string_view wanted);

void register_bridges(Registry& registry);

}  // namespace cvt::archive
