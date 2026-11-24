// UTF-8 <-> UTF-16 conversion for the JNI bridge.
//
// JNI's *StringUTF* functions speak "modified UTF-8": U+0000 is encoded as
// C0 80 and supplementary characters as two 3-byte surrogate encodings
// (CESU-8). The core speaks standard UTF-8. Passing core strings straight to
// NewStringUTF would corrupt emoji and other non-BMP characters (and aborts
// on invalid input under CheckJNI), so the bridge converts through UTF-16 with
// these functions and only ever uses NewString / GetStringRegion.
//
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace cvt_jni {

// Standard UTF-8 to UTF-16. Malformed sequences (overlong forms, encoded
// surrogates, values above U+10FFFF, truncated sequences) each become U+FFFD.
std::u16string utf8_to_utf16(std::string_view in);

// UTF-16 to standard UTF-8. Unpaired surrogates become U+FFFD. U+0000 is
// encoded as a single 0x00 byte (callers decide whether that is acceptable).
std::string utf16_to_utf8(std::u16string_view in);

}  // namespace cvt_jni
