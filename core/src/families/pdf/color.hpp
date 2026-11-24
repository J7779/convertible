// Convertible core - pdf family: functions and colour spaces.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "families/pdf/document.hpp"
#include "families/pdf/object.hpp"

namespace cvt::pdf {

/// A PDF function (types 0, 2, 3, 4), or an array of 1-output functions.
class Function {
 public:
  virtual ~Function() = default;
  /// Evaluate; `in` has `inputs()` values, `out` receives `outputs()` values.
  virtual void eval(const double* in, double* out) const = 0;
  [[nodiscard]] int inputs() const noexcept { return inputs_; }
  [[nodiscard]] int outputs() const noexcept { return outputs_; }

 protected:
  int inputs_ = 1;
  int outputs_ = 1;
};

/// Load a function object (dict, stream or array of functions). Null when
/// the object is not a valid function.
[[nodiscard]] std::shared_ptr<const Function> load_function(PdfFile& file, const Object& object, int depth = 0);

struct Rgb {
  double r = 0, g = 0, b = 0;
};

class ColorSpace {
 public:
  enum class Kind : std::uint8_t { Gray, Rgb, Cmyk, Lab, Indexed, Separation, DeviceN, Pattern, Unknown };
  virtual ~ColorSpace() = default;
  [[nodiscard]] Kind kind() const noexcept { return kind_; }
  /// Number of colour components of one colour value.
  [[nodiscard]] int components() const noexcept { return n_; }
  /// Convert one colour (components in the space's natural ranges) to sRGB 0..1.
  [[nodiscard]] virtual Rgb to_rgb(const double* c) const = 0;
  /// The initial colour when the space is selected with cs/CS.
  [[nodiscard]] virtual std::vector<double> initial_color() const;
  /// Default /Decode range of component `i` for images with `bpc` bits.
  [[nodiscard]] virtual std::array<double, 2> decode_range(int i, int bpc) const;
  /// For Pattern spaces: the underlying space of uncoloured patterns (may be null).
  [[nodiscard]] virtual std::shared_ptr<const ColorSpace> base() const { return nullptr; }

 protected:
  Kind kind_ = Kind::Unknown;
  int n_ = 1;
};

[[nodiscard]] std::shared_ptr<const ColorSpace> device_gray();
[[nodiscard]] std::shared_ptr<const ColorSpace> device_rgb();
[[nodiscard]] std::shared_ptr<const ColorSpace> device_cmyk();

/// Resolve a colour space object: a name (DeviceGray, ..., or a key in the
/// resources' /ColorSpace dict), or an array. `resources` may be null.
/// Returns null when unusable (callers fall back to DeviceGray).
[[nodiscard]] std::shared_ptr<const ColorSpace> load_color_space(PdfFile& file, const Object& object,
                                                                 const Dict* resources, int depth = 0);

}  // namespace cvt::pdf
