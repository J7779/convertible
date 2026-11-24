// Convertible core - image_modern family: JPEG XL registration entry point.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

namespace cvt {
class Registry;
}

namespace cvt::image_modern {

/// Registers the `jxl` format (reader and writer, imported libjxl).
void register_jxl(Registry& registry);

}  // namespace cvt::image_modern
