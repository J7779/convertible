// Convertible core - image_modern family: OpenEXR registration.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include "engine/registry.hpp"

namespace cvt::image_modern {

/// Register the OpenEXR format (`exr`) and its Image codec.
void register_exr(Registry& registry);

}  // namespace cvt::image_modern
