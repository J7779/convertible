// Convertible core - science family: colour-space conversions used by the
// palette formats. sRGB (IEC 61966-2-1), CIE XYZ, L*a*b* and LCh(ab) with a
// D50 or D65 white (Bradford adaptation from the D65 sRGB primaries), HSL,
// HSV/HSB and naive CMYK.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>

namespace cvt::science::colour {

using Vec3 = std::array<double, 3>;

/// sRGB components in 0..1 <-> linear light.
[[nodiscard]] double srgb_to_linear(double v) noexcept;
[[nodiscard]] double linear_to_srgb(double v) noexcept;

/// sRGB (0..1) <-> CIE XYZ (Y of white = 1) relative to D65.
[[nodiscard]] Vec3 srgb_to_xyz_d65(const Vec3& rgb) noexcept;
[[nodiscard]] Vec3 xyz_d65_to_srgb(const Vec3& xyz) noexcept;
/// Bradford chromatic adaptation between D65 and D50.
[[nodiscard]] Vec3 d65_to_d50(const Vec3& xyz) noexcept;
[[nodiscard]] Vec3 d50_to_d65(const Vec3& xyz) noexcept;

enum class Illuminant { D50, D65 };
[[nodiscard]] Vec3 white(Illuminant illuminant) noexcept;
[[nodiscard]] Vec3 xyz_to_lab(const Vec3& xyz, Illuminant illuminant) noexcept;
[[nodiscard]] Vec3 lab_to_xyz(const Vec3& lab, Illuminant illuminant) noexcept;

/// sRGB (0..1) <-> L*a*b* under the given white (sRGB is adapted from D65).
[[nodiscard]] Vec3 srgb_to_lab(const Vec3& rgb, Illuminant illuminant) noexcept;
[[nodiscard]] Vec3 lab_to_srgb(const Vec3& lab, Illuminant illuminant) noexcept;
[[nodiscard]] Vec3 lab_to_lch(const Vec3& lab) noexcept;
[[nodiscard]] Vec3 lch_to_lab(const Vec3& lch) noexcept;

/// HSL / HSV with hue in degrees, saturation/lightness/value in 0..1.
[[nodiscard]] Vec3 rgb_to_hsl(const Vec3& rgb) noexcept;
[[nodiscard]] Vec3 hsl_to_rgb(const Vec3& hsl) noexcept;
[[nodiscard]] Vec3 rgb_to_hsv(const Vec3& rgb) noexcept;
[[nodiscard]] Vec3 hsv_to_rgb(const Vec3& hsv) noexcept;
/// Naive (device) CMYK in 0..1.
[[nodiscard]] std::array<double, 4> rgb_to_cmyk(const Vec3& rgb) noexcept;
[[nodiscard]] Vec3 cmyk_to_rgb(const std::array<double, 4>& cmyk) noexcept;

}  // namespace cvt::science::colour
