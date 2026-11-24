// Convertible core - image_modern family: WebP registration.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

namespace cvt {
class Registry;
}

namespace cvt::image_modern {

/// Registers the `webp` format and its Image reader and writer (libwebp).
void register_webp(Registry& registry);

}  // namespace cvt::image_modern
