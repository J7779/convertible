// Convertible core - image_modern family: camera RAW formats (registration).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

namespace cvt {
class Registry;
}

namespace cvt::image_modern {

/// Adobe DNG and the maker RAW formats (cr2, cr3, nef, nrw, arw, sr2, srf,
/// raf, orf, rw2, rwl, pef, srw, ...): readers into ir::Image.
void register_raw(Registry& registry);

}  // namespace cvt::image_modern
