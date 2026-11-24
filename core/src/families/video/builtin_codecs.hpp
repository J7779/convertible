// Convertible core - video family: built-in trivial codecs (rawvideo, PCM).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

namespace cvt::codec {
class Registry;
}

namespace cvt::video {

/// Register rawvideo and PCM decoders/encoders for every id that has no
/// implementation yet (a family registered earlier always wins).
void register_builtin_codecs(codec::Registry& media);

}  // namespace cvt::video
