// Convertible core - entry points of the format families.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

namespace cvt {

class Registry;

/// Each family implements exactly one of these in
/// `src/families/<family>/<family>_family.cpp` and registers its formats,
/// format codecs, bridges and (through `registry.media()`) audio/video
/// decoders and encoders on the given registry.
/// Registration must not perform any I/O and must not touch global state.
void register_image_family(Registry& registry);
void register_image_modern_family(Registry& registry);
void register_vector_family(Registry& registry);
void register_audio_lossless_family(Registry& registry);
void register_audio_lossy_family(Registry& registry);
void register_video_family(Registry& registry);
void register_codec_h264_family(Registry& registry);
void register_codec_hevc_family(Registry& registry);
void register_codec_vpx_family(Registry& registry);
void register_codec_av1_family(Registry& registry);
void register_codec_legacy_family(Registry& registry);
void register_document_family(Registry& registry);
void register_office_family(Registry& registry);
void register_pdf_family(Registry& registry);
void register_data_family(Registry& registry);
void register_table_family(Registry& registry);
void register_archive_family(Registry& registry);
void register_font_family(Registry& registry);
void register_subtitle_family(Registry& registry);
void register_pim_family(Registry& registry);
void register_mesh_family(Registry& registry);
void register_geo_family(Registry& registry);
void register_science_family(Registry& registry);
void register_code_family(Registry& registry);
void register_encoding_family(Registry& registry);

/// The detection-only database of well-known formats
/// (`src/engine/known_formats.cpp`). Fills gaps; never overrides a family.
void register_known_formats(Registry& registry);

/// Populate a registry with everything above, in a fixed order.
void register_all(Registry& registry);

}  // namespace cvt
