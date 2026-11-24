// Convertible core - image_modern family: registration of the assorted formats.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/registry.hpp"
#include "families/image_modern/misc.hpp"

namespace cvt::image_modern {

void register_misc(Registry& registry) {
  register_misc_ora(registry);
  register_misc_ani(registry);
  register_misc_xcf(registry);
  register_misc_jls(registry);
  register_misc_texture(registry);
}

}  // namespace cvt::image_modern
