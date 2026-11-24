// Convertible core - archive family: internals shared by the Cabinet (cab.cpp) and Compiled HTML Help
// (chm_read.cpp) readers.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "ir/archive.hpp"

namespace cvt::archive::cab {

/// Read a Compiled HTML Help file (ITSF) as an archive of its pages.
[[nodiscard]] Result<ir::Archive> read_chm(ReadContext& ctx);

}  // namespace cvt::archive::cab
