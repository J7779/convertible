// Convertible core - image_modern family: JPEG 2000 registration.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

namespace cvt {
class Registry;
}

namespace cvt::image_modern {

/// Registers `jp2` (JP2 file), `j2k` (raw codestream: j2k, j2c, jpc), `jpx`
/// and `jpf` (JPX files read through the same box parser, written as
/// JP2-compatible JPX). See jp2.cpp for the mapping of components, precisions
/// and colour spaces onto `ir::Image`.
void register_jp2(Registry& registry);

}  // namespace cvt::image_modern
