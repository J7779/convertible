// Convertible core - vector family: lossless JSON form of a scene.
//
// The envelope is an object whose first member is "vector_scene": 1. It is
// the Data representation every vector codec reads and writes, and doubles
// as a readable structure dump (svg -> json). Paths are SVG path-data
// strings with absolute M/L/Q/C/Z commands; shared paint servers, clips,
// masks, filters and patterns live in "defs" and are referenced by key.
// Pictures are stored as their original encoded bytes (Base64) or, when
// there are none, as zlib-compressed RGBA.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <string>
#include <string_view>

#include "engine/errors.hpp"
#include "families/vector/scene.hpp"
#include "util/json.hpp"

namespace cvt::vector {

inline constexpr std::string_view scene_envelope_key = "vector_scene";
inline constexpr int scene_envelope_version = 1;

[[nodiscard]] Json scene_to_json(const Scene& scene);
/// Fails with Status::Encode when `value` is not a scene envelope, and with
/// Status::Decode when it is one but malformed. `max_depth` bounds group nesting;
/// `max_pixels` bounds decoded pictures.
[[nodiscard]] Result<Scene> scene_from_json(const Json& value, std::uint64_t max_pixels = 268'435'456ull,
                                            int max_depth = 256);
[[nodiscard]] bool is_scene_json(const Json& value);

/// Compact absolute path data ("M0 0L10 0Q..Z"), numbers in shortest round-trip form.
[[nodiscard]] std::string path_to_string(const Path& path, int precision = -1);
/// Parse what `path_to_string` produces (absolute M L Q C Z only).
[[nodiscard]] Result<Path> path_from_string(std::string_view text);

}  // namespace cvt::vector
