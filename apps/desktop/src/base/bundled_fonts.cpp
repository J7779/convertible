// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "base/resources.hpp"
#include "text/text.hpp"

namespace cvd {

bool install_bundled_fonts() {
  static const bool ok = [] {
    return text::FontSet::instance().install({
        resource("fonts/Inter-Regular.ttf"),
        resource("fonts/Inter-Medium.ttf"),
        resource("fonts/Inter-SemiBold.ttf"),
        resource("fonts/JetBrainsMonoNL-Regular.ttf"),
        resource("fonts/JetBrainsMonoNL-Medium.ttf"),
    });
  }();
  return ok;
}

}  // namespace cvd
