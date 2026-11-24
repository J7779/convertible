// Convertible core - office family: OfficeArt (Escher) picture records,
// shared by the Word and PowerPoint binary readers.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <optional>

#include "ir/document.hpp"
#include "util/bytes.hpp"

namespace cvt::office {

/// The first picture (BLIP record) in the OfficeArt records of
/// data[at, end): containers are descended, FBSE wrappers skipped. PNG and
/// JPEG are returned as they are, DIBs as BMP files, EMF/WMF inflated and
/// kept as bytes.
[[nodiscard]] std::optional<ir::EncodedImage> find_blip(ByteView data, std::size_t at, std::size_t end);

}  // namespace cvt::office
