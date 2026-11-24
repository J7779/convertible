// Convertible core - subtitle family.
// Subtitles, captions and lyrics: SubRip, WebVTT, ASS/SSA, MicroDVD, SubViewer,
// YouTube SBV/json3/srv3, SAMI, TTML/DFXP/IMSC/EBU-TT/SMPTE-TT, Scenarist SCC,
// EBU STL, Spruce STL, QuickTime Text, MPL2, TMPlayer and LRC; plus bridges
// to tables, data and transcripts. Bitmap subtitles (VobSub, PGS, DVB) are
// not handled here.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/registry.hpp"
#include "families/families.hpp"
#include "families/subtitle/subtitle.hpp"

namespace cvt {

void register_subtitle_family(Registry& registry) {
  subtitle::register_text_formats(registry);
  subtitle::register_ass(registry);
  subtitle::register_xml_formats(registry);
  subtitle::register_scc(registry);
  subtitle::register_ebu_stl(registry);
  subtitle::register_youtube(registry);
  subtitle::register_bridges(registry);
}

}  // namespace cvt
