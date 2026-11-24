// Convertible core - video family: internal declarations.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <span>
#include <string_view>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "ir/any.hpp"
#include "util/bytes.hpp"

namespace cvt {
class Registry;
}

namespace cvt::video {

/// Container formats, their readers and writers (formats.cpp).
void register_formats(Registry& registry);
/// Video -> Audio, Video -> Image and Image -> Video bridges, and the
/// audio-file targets of videos (bridges.cpp).
void register_bridges(Registry& registry);

/// Detection helpers, exposed for tests.
[[nodiscard]] int sniff_iso(std::span<const std::byte> data, std::string_view filename, std::string_view format_id);
[[nodiscard]] int sniff_ebml(std::span<const std::byte> data, std::string_view filename, std::string_view format_id);

}  // namespace cvt::video
