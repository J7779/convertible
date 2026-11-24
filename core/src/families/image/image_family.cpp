// Convertible core - image family.
// Common raster images (png, jpg, gif, bmp, tiff, ico, tga, pnm, ...), the
// shared image toolbox (image_ops.hpp) and the decode/encode API for other
// families (image_api.hpp).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/registry.hpp"
#include "families/families.hpp"
#include "families/image/image.hpp"

namespace cvt {

void register_image_family(Registry& registry) {
  image::register_png(registry);
  image::register_jpeg(registry);
  image::register_gif(registry);
  image::register_bmp(registry);
  image::register_ico(registry);
  image::register_icns(registry);
  image::register_tga(registry);
  image::register_pcx(registry);
  image::register_tiff(registry);
  image::register_psd(registry);
  image::register_dds(registry);
  image::register_pnm(registry);
  image::register_qoi(registry);
  image::register_hdr(registry);
  image::register_wbmp(registry);
  image::register_xbm(registry);
  image::register_xpm(registry);
  image::register_farbfeld(registry);
  image::register_sgi(registry);
  image::register_sun(registry);
}

}  // namespace cvt
