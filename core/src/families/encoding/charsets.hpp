// Convertible core - encoding family: legacy character sets.
//
// The tables, decoders, encoders and the detector live in util/charsets.hpp
// (shared with the subtitle, document and office families); this family
// offers them through the `txt` reader and writer (source_encoding /
// encoding options) and the `charset-detect` report (charsets_register.cpp).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include "util/charsets.hpp"

namespace cvt::encoding {

using util::CharsetBytes;
using util::CharsetGuess;
using util::CharsetInfo;
using util::CharsetText;
using util::charset_decode;
using util::charset_encode;
using util::detect_charsets;
using util::find_charset;
using util::legacy_charsets;

}  // namespace cvt::encoding
