// Convertible core - font family: internal declarations shared by the codecs.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <string_view>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "engine/options.hpp"
#include "ir/font.hpp"

namespace cvt {
class Registry;
}

namespace cvt::font {

/// Format ids this family adds to the shared vocabulary (the detection
/// database has no rows for them).
inline constexpr std::string_view id_cff = "cff";
inline constexpr std::string_view id_otb = "otb";
inline constexpr std::string_view id_svg_font = "svg-font";

/// The outline flavour a writer needs.
enum class OutlineTarget { Keep, TrueType, Cff };

/// Write-side options shared by every outline font writer.
[[nodiscard]] std::vector<OptionSpec> subset_option_specs();

/// Turn the IR handed to a writer into what the target needs: bitmap-only
/// fonts get outlines, editor tables are stripped (strip_metadata),
/// subsetting/optimisation options are applied and outlines are converted
/// to the target flavour. `all_faces` keeps the other faces of a collection.
[[nodiscard]] Result<ir::Font> prepare_font(const ir::Font& font, OutlineTarget target, bool all_faces,
                                            const WriteContext& ctx);

void register_sfnt_formats(Registry& registry);
void register_type1_formats(Registry& registry);
void register_bitmap_formats(Registry& registry);
void register_svg_font(Registry& registry);
void register_font_bridges(Registry& registry);

}  // namespace cvt::font
