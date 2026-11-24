// Convertible core - image_modern family: HEIF-based images (AVIF, HEIC, HEIF).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

namespace cvt {
class Registry;
}

namespace cvt::image_modern {

/// Registers the `avif`, `heic` and `heif` formats with their readers and writers.
void register_heif(Registry& registry);

}  // namespace cvt::image_modern
