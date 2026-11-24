// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/families.hpp"

namespace cvt {

void register_all(Registry& registry) {
  // Families first, in a fixed order so that registration-order tie-breaks in
  // the planner are deterministic. The known-formats database goes last: it
  // only fills in what no family described.
  register_image_family(registry);
  register_image_modern_family(registry);
  register_vector_family(registry);
  register_audio_lossless_family(registry);
  register_audio_lossy_family(registry);
  register_video_family(registry);
  register_codec_h264_family(registry);
  register_codec_hevc_family(registry);
  register_codec_vpx_family(registry);
  register_codec_av1_family(registry);
  register_codec_legacy_family(registry);
  register_document_family(registry);
  register_office_family(registry);
  register_pdf_family(registry);
  register_data_family(registry);
  register_table_family(registry);
  register_archive_family(registry);
  register_font_family(registry);
  register_subtitle_family(registry);
  register_pim_family(registry);
  register_mesh_family(registry);
  register_geo_family(registry);
  register_science_family(registry);
  register_code_family(registry);
  register_encoding_family(registry);
  register_known_formats(registry);
}

}  // namespace cvt
