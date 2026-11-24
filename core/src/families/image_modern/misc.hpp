// Convertible core - image_modern family: assorted in-house formats
// (OpenRaster, Krita, animated cursors, DCX, GIMP XCF, JPEG-LS, GPU texture
// containers, Valve VTF).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

namespace cvt {
class Registry;
}

namespace cvt::image_modern {

void register_misc_ora(Registry& registry);      // ora (read + write), kra (read)
void register_misc_ani(Registry& registry);      // ani (read), dcx (read + write)
void register_misc_xcf(Registry& registry);      // xcf (read)
void register_misc_jls(Registry& registry);      // jls (read + write)
void register_misc_texture(Registry& registry);  // ktx, ktx2 (read + write), pvr, astc, vtf (read)

/// Everything above.
void register_misc(Registry& registry);

}  // namespace cvt::image_modern
