// Convertible core - vector family: smaller vector formats (VectorDrawable, TikZ, Lottie, PICT).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/registry.hpp"
#include "families/vector/misc_common.hpp"
#include "families/vector/vector.hpp"

namespace cvt::vector {

void register_misc_vector(Registry& registry) {
  misc::register_vectordrawable(registry);
  misc::register_tikz(registry);
  misc::register_lottie(registry);
  misc::register_pict(registry);
}

}  // namespace cvt::vector
