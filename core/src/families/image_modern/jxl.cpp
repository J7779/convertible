// Convertible core - image_modern family: JPEG XL (ISO/IEC 18181) registration
// and the shared memory budget. Decoding and encoding are done by the
// imported libjxl (see jxl_decode.cpp / jxl_encode.cpp).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "util/bytes.hpp"
#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/image/common.hpp"
#include "families/image_modern/jxl.hpp"
#include "families/image_modern/jxl_codec.hpp"

namespace cvt::image_modern {

namespace jxl {

namespace {

// Each block starts with a header that records its size; 16 bytes keep the
// alignment malloc guarantees for the payload on every target that matters
// (libjxl aligns its own buffers on top of that).
constexpr std::size_t header_size = 16;

}  // namespace

void* MemoryBudget::allocate(std::size_t size) noexcept {
  const std::uint64_t total = std::uint64_t{size} + header_size;
  if (size > SIZE_MAX - header_size || total > limit_ || used_ > limit_ - total) {
    exceeded_ = true;
    return nullptr;
  }
  auto* block = static_cast<unsigned char*>(std::malloc(static_cast<std::size_t>(total)));
  if (block == nullptr) {
    exceeded_ = true;
    return nullptr;
  }
  const std::uint64_t recorded = total;
  ::cvt::util::copy_bytes(block, &recorded, sizeof recorded);
  used_ += total;
  peak_ = std::max(peak_, used_);
  return block + header_size;
}

void MemoryBudget::release(void* address) noexcept {
  if (address == nullptr) return;
  unsigned char* block = static_cast<unsigned char*>(address) - header_size;
  std::uint64_t recorded = 0;
  ::cvt::util::copy_bytes(&recorded, block, sizeof recorded);
  used_ -= std::min(used_, recorded);
  std::free(block);
}

namespace {

// Working memory beyond the pixels themselves: entropy-coder tables, group
// caches, ICC handling. Measured peaks stay far below this for small images.
constexpr std::uint64_t base_budget = 96ull * 1024 * 1024;
constexpr std::uint64_t floor_budget = 256ull * 1024 * 1024;

std::uint64_t budget(const Limits& limits, std::uint64_t pixels, std::uint64_t bytes_per_pixel) noexcept {
  const std::uint64_t pixel_cap = std::min<std::uint64_t>(pixels, limits.max_pixels);
  // Never below the floor (which tiny images need for fixed-size tables) and
  // never above what a 64-bit size can express.
  const std::uint64_t wanted = base_budget + pixel_cap * bytes_per_pixel;
  return std::max(floor_budget, wanted);
}

}  // namespace

// libjxl keeps the decoded frame in float planes plus the output buffer it
// writes into (which we own): measured peaks are about 30 bytes per pixel.
std::uint64_t decode_budget(const Limits& limits, std::uint64_t pixels) noexcept {
  return budget(limits, pixels, 64);
}

// The encoder holds several float copies of the picture (XYB, opsin, error
// maps for the butteraugli-guided search at higher efforts).
std::uint64_t encode_budget(const Limits& limits, std::uint64_t pixels) noexcept {
  return budget(limits, pixels, 192);
}

}  // namespace jxl

namespace {

Result<ir::Image> read_jxl(ReadContext& ctx) { return jxl::read(ctx); }
Result<Bytes> write_jxl(const ir::Image& image, WriteContext& ctx) { return jxl::write(image, ctx); }

std::vector<OptionSpec> jxl_write_options() {
  std::vector<OptionSpec> specs = {
      options::quality(),
      options::preset(),
      options::lossless(false),
      OptionSpec::integer(std::string(jxl::key_effort), "Effort", jxl::default_effort, 1, 9,
                          "Encoder effort: higher values search harder for a smaller file at the same quality "
                          "and are slower (1 fastest, 9 slowest).")
          .advanced(),
  };
  for (OptionSpec& spec : image::size_options()) specs.push_back(std::move(spec));
  specs.push_back(options::strip_metadata());
  return specs;
}

}  // namespace

void register_jxl(Registry& registry) {
  FormatInfo info;
  info.id = "jxl";
  info.name = "JPEG XL image";
  info.description = "Modern lossy and lossless image format (ISO/IEC 18181) with alpha, animation and HDR.";
  info.category = Category::Image;
  info.extensions = {"jxl"};
  info.mime = "image/jxl";
  info.popularity = 35;
  // A bare codestream starts with FF 0A; the ISO BMFF container with the
  // 12-byte "JXL " signature box.
  info.signatures = {Signature::at(0, "FF 0A"), Signature::at(0, "00 00 00 0C 'JXL ' 0D 0A 87 0A")};
  registry.add_format(std::move(info));

  image::ImageCodecSpec spec;
  spec.format_id = "jxl";
  spec.read = image::reader(read_jxl);
  spec.write = image::writer(write_jxl);
  spec.write_options = jxl_write_options();
  spec.lossy_write = true;
  spec.read_cost = 15;
  spec.write_cost = 25;
  add_image_codec(registry, std::move(spec));
}

}  // namespace cvt::image_modern
