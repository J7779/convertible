// Convertible core - video family: built-in trivial codecs (rawvideo, PCM).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

namespace cvt::codec {
class Registry;
}

namespace cvt::video {

/// Register the rawvideo codecs (owned by this family) and PCM decoders and
/// encoders as fallbacks (owned by the lossless audio family, which replaces
/// them whatever the registration order).
void register_builtin_codecs(codec::Registry& media);

}  // namespace cvt::video
