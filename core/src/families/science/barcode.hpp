// Convertible core - science family: barcode symbol generation (no decoding).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "engine/errors.hpp"

namespace cvt::science::barcode {

/// A matrix of modules, row-major, true = dark.
struct Matrix {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> modules;
  [[nodiscard]] bool get(int x, int y) const { return modules[static_cast<std::size_t>(y) * width + x] != 0; }
  void set(int x, int y, bool dark) { modules[static_cast<std::size_t>(y) * width + x] = dark ? 1 : 0; }
};

enum class QrEcc { L, M, Q, H };

struct QrOptions {
  QrEcc ecc = QrEcc::M;
  int min_version = 1;
  int max_version = 40;
  int mask = -1;  ///< -1 = best by the penalty rules
  bool boost_ecc = true;  ///< raise the level while the version stays the same
};

/// QR Code model 2 (ISO/IEC 18004): numeric, alphanumeric and byte (UTF-8) segments.
[[nodiscard]] Result<Matrix> qr_encode(std::string_view text, const QrOptions& options, int* version_out = nullptr,
                                       int* mask_out = nullptr);
/// Data Matrix ECC 200 (ISO/IEC 16022), ASCII encodation with digit pairs and upper shift.
[[nodiscard]] Result<Matrix> datamatrix_encode(std::string_view text, bool rectangular);
/// Linear symbols as a 1-row module pattern (bars = true).
[[nodiscard]] Result<std::vector<bool>> code128_encode(std::string_view text);
[[nodiscard]] Result<std::vector<bool>> ean13_encode(std::string_view digits, std::string* full_out = nullptr);
[[nodiscard]] Result<std::vector<bool>> upca_encode(std::string_view digits, std::string* full_out = nullptr);

}  // namespace cvt::science::barcode
