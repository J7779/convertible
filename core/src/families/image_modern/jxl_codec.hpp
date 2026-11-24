// Convertible core - image_modern family: JPEG XL reader/writer internals.
// Private to the jxl*.cpp files; nothing here exposes libjxl types.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "ir/image.hpp"
#include "util/bytes.hpp"

namespace cvt::image_modern::jxl {

/// Option keys specific to the JPEG XL writer.
inline constexpr std::string_view key_effort = "effort";
/// Default encoder effort (libjxl's own default, "squirrel").
inline constexpr int default_effort = 7;

/// Caps every allocation libjxl makes through its memory manager, so a
/// hostile file cannot make the codec allocate more than `limit` bytes.
class MemoryBudget {
 public:
  explicit MemoryBudget(std::uint64_t limit) noexcept : limit_(limit) {}
  MemoryBudget(const MemoryBudget&) = delete;
  MemoryBudget& operator=(const MemoryBudget&) = delete;

  void set_limit(std::uint64_t limit) noexcept { limit_ = limit; }
  [[nodiscard]] std::uint64_t limit() const noexcept { return limit_; }
  [[nodiscard]] std::uint64_t used() const noexcept { return used_; }
  [[nodiscard]] std::uint64_t peak() const noexcept { return peak_; }
  /// True once an allocation was refused.
  [[nodiscard]] bool exceeded() const noexcept { return exceeded_; }

  [[nodiscard]] void* allocate(std::size_t size) noexcept;
  void release(void* address) noexcept;

 private:
  std::uint64_t limit_ = 0;
  std::uint64_t used_ = 0;
  std::uint64_t peak_ = 0;
  bool exceeded_ = false;
};

/// Memory budget for decoding a picture of `pixels` pixels per frame.
[[nodiscard]] std::uint64_t decode_budget(const Limits& limits, std::uint64_t pixels) noexcept;
/// Memory budget for encoding a picture of `pixels` pixels per frame.
[[nodiscard]] std::uint64_t encode_budget(const Limits& limits, std::uint64_t pixels) noexcept;

[[nodiscard]] Result<ir::Image> read(ReadContext& ctx);
[[nodiscard]] Result<Bytes> write(const ir::Image& image, WriteContext& ctx);

}  // namespace cvt::image_modern::jxl
