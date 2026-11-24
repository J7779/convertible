// Convertible core - image_modern family.
// Modern and professional raster images (webp, avif, heic, jxl, jp2, exr, camera raw, ...).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/registry.hpp"
#include "families/families.hpp"
#include "families/image_modern/exr.hpp"
#include "families/image_modern/heif.hpp"
#include "families/image_modern/jp2.hpp"
#include "families/image_modern/jxl.hpp"
#include "families/image_modern/misc.hpp"
#include "families/image_modern/raw.hpp"
#include "families/image_modern/webp.hpp"

namespace cvt {

void register_image_modern_family(Registry& registry) {
  image_modern::register_webp(registry);
  image_modern::register_heif(registry);
  image_modern::register_jxl(registry);
  image_modern::register_jp2(registry);
  image_modern::register_exr(registry);
  image_modern::register_raw(registry);
  image_modern::register_misc(registry);
}

}  // namespace cvt
