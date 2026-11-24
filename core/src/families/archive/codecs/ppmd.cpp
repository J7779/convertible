// Convertible core - archive family: PPMd codec table (Method::Ppmd7, Method::Ppmd8).
// The models live in ppmd_h.cpp (var.H) and ppmd_i.cpp (var.I).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <array>

#include "families/archive/codecs/ppmd.hpp"

namespace cvt::compression::detail {

namespace {

const std::array<CodecImpl, 2> table = {{
    {Method::Ppmd7, ppmd7_decode, ppmd7_encode, nullptr, nullptr, ppmd7_properties},
    {Method::Ppmd8, ppmd8_decode, nullptr, nullptr, nullptr, nullptr},
}};

}  // namespace

std::span<const CodecImpl> ppmd_codecs() { return table; }

}  // namespace cvt::compression::detail
