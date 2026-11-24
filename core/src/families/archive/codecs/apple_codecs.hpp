// Convertible core - archive family: Apple codecs (private interface).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include "families/archive/codec_impl.hpp"

namespace cvt::compression::detail {

/// LZFSE stream: a sequence of "bvx-" (stored), "bvxn" (LZVN), "bvx1" / "bvx2"
/// (LZ77 + finite state entropy) blocks ending with "bvx$" (apple_lzfse.cpp).
Result<Bytes> lzfse_decode(ByteView data, const DecodeOptions& options);

/// Apple Data Compression as found in DMG "ADC" chunks (apple.cpp).
Result<Bytes> adc_decode(ByteView data, const DecodeOptions& options);

}  // namespace cvt::compression::detail
