// Convertible core - resource limits that defeat hostile or absurd inputs.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>

#include "engine/errors.hpp"

namespace cvt {

/// Hard ceilings applied to every conversion. Codecs receive a copy in their
/// context and MUST consult it before allocating anything whose size comes
/// from the input (pixel buffers, decompressed streams, entry tables, ...).
///
/// The engine itself enforces `max_input_bytes` and `max_output_bytes`.
/// Apps can lower or raise individual values per conversion through the
/// reserved `"limits"` object of the options JSON, for example
/// `{"limits":{"max_pixels":1000000}}`.
struct Limits {
  /// Largest input accepted, in bytes.
  std::uint64_t max_input_bytes = 2ull * 1024 * 1024 * 1024;
  /// Largest encoded output a conversion may return, in bytes.
  std::uint64_t max_output_bytes = 2ull * 1024 * 1024 * 1024;
  /// Largest decoded raster, in pixels, summed over all frames (2^28).
  std::uint64_t max_pixels = 268'435'456ull;
  /// Largest number of entries read from (or written to) one container.
  std::uint64_t max_archive_entries = 100'000ull;
  /// Largest total size of data inflated from compressed input, in bytes.
  /// This is what stops decompression bombs.
  std::uint64_t max_decompressed_bytes = 2ull * 1024 * 1024 * 1024;
  /// Deepest nesting a recursive parser may follow (JSON/XML/YAML trees,
  /// nested archives, PDF object graphs, box/atom trees ...).
  std::uint32_t max_depth = 256;

  [[nodiscard]] Result<void> check_input(std::uint64_t bytes) const {
    if (bytes > max_input_bytes) {
      return fail(Status::Limit, "input of {} bytes exceeds the limit of {} bytes", bytes, max_input_bytes);
    }
    return {};
  }
  [[nodiscard]] Result<void> check_output(std::uint64_t bytes) const {
    if (bytes > max_output_bytes) {
      return fail(Status::Limit, "output of {} bytes exceeds the limit of {} bytes", bytes, max_output_bytes);
    }
    return {};
  }
  /// `frames` images of `width` x `height` pixels. Overflow safe.
  [[nodiscard]] Result<void> check_pixels(std::uint64_t width, std::uint64_t height, std::uint64_t frames = 1) const {
    const auto over = [this](std::uint64_t a, std::uint64_t b) { return b != 0 && a > max_pixels / b; };
    if (over(width, height) || over(width * height, frames)) {
      return fail(Status::Limit, "image of {}x{} pixels ({} frame(s)) exceeds the limit of {} pixels", width, height,
                  frames, max_pixels);
    }
    return {};
  }
  [[nodiscard]] Result<void> check_archive_entries(std::uint64_t count) const {
    if (count > max_archive_entries) {
      return fail(Status::Limit, "{} entries exceed the limit of {} entries", count, max_archive_entries);
    }
    return {};
  }
  /// `depth` = the nesting level about to be entered (the outermost is 1).
  [[nodiscard]] Result<void> check_depth(std::uint64_t depth) const {
    if (depth > max_depth) return fail(Status::Limit, "nesting deeper than {} levels", max_depth);
    return {};
  }
  /// `total` = bytes decompressed so far plus the bytes about to be produced.
  [[nodiscard]] Result<void> check_decompressed(std::uint64_t total) const {
    if (total > max_decompressed_bytes) {
      return fail(Status::Limit, "decompressed size of {} bytes exceeds the limit of {} bytes", total,
                  max_decompressed_bytes);
    }
    return {};
  }
};

}  // namespace cvt
