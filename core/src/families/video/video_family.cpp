// Convertible core - video family.
// Video containers (mp4, mov, m4v, 3gp, m4a, mkv, webm, avi, flv, ts, mpg/vob,
// ogv, wmv/wma, ivf, rm, raw elementary streams, y4m), the transcoding
// pipeline, the Video <-> Image/Audio bridges and the built-in rawvideo/PCM codecs.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/registry.hpp"
#include "families/families.hpp"
#include "families/video/builtin_codecs.hpp"
#include "families/video/video.hpp"

namespace cvt {

void register_video_family(Registry& registry) {
  video::register_formats(registry);
  video::register_bridges(registry);
  video::register_builtin_codecs(registry.media());
}

}  // namespace cvt
