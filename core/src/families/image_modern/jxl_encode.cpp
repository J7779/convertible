// Convertible core - image_modern family: JPEG XL writer (imported libjxl).
//
// Lossy (VarDCT in XYB, `quality` mapped onto libjxl's butteraugli distance)
// and lossless (modular) encoding, alpha, animation, greyscale, 16-bit and
// float sources (from `hdr`), effort, and the EXIF/XMP/ICC metadata when
// `strip_metadata` is off.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <jxl/codestream_header.h>
#include <jxl/color_encoding.h>
#include <jxl/encode.h>
#include <jxl/memory_manager.h>
#include <jxl/types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "util/bytes.hpp"
#include "engine/standard_options.hpp"
#include "families/image/common.hpp"
#include "families/image/image_ops.hpp"
#include "families/image_modern/jxl_codec.hpp"

namespace cvt::image_modern::jxl {

namespace {

void* budget_alloc(void* opaque, std::size_t size) { return static_cast<MemoryBudget*>(opaque)->allocate(size); }
void budget_free(void* opaque, void* address) { static_cast<MemoryBudget*>(opaque)->release(address); }

struct EncoderDeleter {
  void operator()(JxlEncoder* encoder) const noexcept { JxlEncoderDestroy(encoder); }
};
using EncoderPtr = std::unique_ptr<JxlEncoder, EncoderDeleter>;

/// Samples of one frame in the layout handed to libjxl.
struct Samples {
  std::vector<std::uint8_t> u8;
  std::vector<std::uint16_t> u16;
  std::vector<float> f32;
  JxlPixelFormat format{};

  [[nodiscard]] const void* data() const noexcept {
    if (!u16.empty()) return u16.data();
    if (!f32.empty()) return f32.data();
    return u8.data();
  }
  [[nodiscard]] std::size_t bytes() const noexcept {
    return u8.size() + u16.size() * sizeof(std::uint16_t) + f32.size() * sizeof(float);
  }
};

/// Keep `channels` (1 = grey, 2 = grey + alpha, 3 = RGB, 4 = RGBA) of RGBA data.
template <class T>
std::vector<T> pack(std::span<const T> rgba, std::uint32_t channels) {
  if (channels == 4) return {rgba.begin(), rgba.end()};
  const std::size_t pixels = rgba.size() / 4;
  std::vector<T> out(pixels * channels);
  for (std::size_t i = 0; i < pixels; ++i) {
    const T* px = rgba.data() + i * 4;
    T* dst = out.data() + i * channels;
    switch (channels) {
      case 1: dst[0] = px[0]; break;
      case 2: dst[0] = px[0]; dst[1] = px[3]; break;
      default: dst[0] = px[0]; dst[1] = px[1]; dst[2] = px[2]; break;
    }
  }
  return out;
}

bool hdr_is_gray(const ir::HdrPixels& hdr) {
  for (std::size_t i = 0; i < hdr.rgba.size(); i += 4) {
    if (hdr.rgba[i] != hdr.rgba[i + 1] || hdr.rgba[i] != hdr.rgba[i + 2]) return false;
  }
  return true;
}

bool hdr_has_alpha(const ir::HdrPixels& hdr) {
  for (std::size_t i = 3; i < hdr.rgba.size(); i += 4) {
    if (hdr.rgba[i] < 1.0f) return true;
  }
  return false;
}

Result<void> check(JxlEncoderStatus status, const char* what) {
  if (status != JXL_ENC_SUCCESS) return fail(Status::Encode, "JPEG XL encoder: {}", what);
  return {};
}

void add_be32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value >> 24));
  out.push_back(static_cast<std::uint8_t>(value >> 16));
  out.push_back(static_cast<std::uint8_t>(value >> 8));
  out.push_back(static_cast<std::uint8_t>(value));
}

}  // namespace

Result<Bytes> write(const ir::Image& source, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const image::WriteImage prepared, image::prepare_write(source, ctx));
  const ir::Image& img = *prepared;
  const ir::Frame& first = img.frames.front();
  const bool animated = img.frames.size() > 1;
  const bool lossless = options::resolved_lossless(ctx.options);
  const int quality = options::resolved_quality(ctx.options);
  const int effort = static_cast<int>(ctx.options.get_int(key_effort, default_effort, 1, 9));
  const bool keep = prepared.keep_metadata();

  // High-precision path: a still picture whose source had more than 8 bits.
  const ir::HdrPixels* hdr = nullptr;
  if (!animated && img.hdr && img.source_bit_depth > 8 && img.hdr->width == first.width &&
      img.hdr->height == first.height && img.hdr->rgba.size() == std::size_t{first.width} * first.height * 4) {
    hdr = &*img.hdr;
  }
  const bool float_samples = hdr != nullptr && img.source_bit_depth == 32;
  const bool use_icc = keep && !img.icc_profile.empty() && !float_samples;

  bool alpha = false;
  bool gray = !use_icc;
  if (hdr != nullptr) {
    alpha = hdr_has_alpha(*hdr);
    gray = gray && hdr_is_gray(*hdr);
  } else {
    for (const ir::Frame& frame : img.frames) {
      alpha = alpha || frame.has_transparency();
      gray = gray && image::is_grayscale(frame);
    }
  }
  const std::uint32_t channels = (gray ? 1u : 3u) + (alpha ? 1u : 0u);

  MemoryBudget memory(encode_budget(ctx.limits, std::uint64_t{first.width} * first.height));
  const JxlMemoryManager manager{&memory, budget_alloc, budget_free};
  EncoderPtr encoder(JxlEncoderCreate(&manager));
  if (!encoder) return fail(Status::Internal, "cannot create the JPEG XL encoder");
  JxlEncoder* enc = encoder.get();

  const bool boxes = keep && (!img.exif.empty() || !img.xmp.empty());
  if (boxes) CVT_TRY(check(JxlEncoderUseBoxes(enc), "cannot enable metadata boxes"));

  JxlBasicInfo info;
  JxlEncoderInitBasicInfo(&info);
  info.xsize = first.width;
  info.ysize = first.height;
  if (float_samples) {
    info.bits_per_sample = 32;
    info.exponent_bits_per_sample = 8;
  } else {
    info.bits_per_sample = hdr != nullptr ? 16 : 8;
    info.exponent_bits_per_sample = 0;
  }
  info.num_color_channels = gray ? 1 : 3;
  info.num_extra_channels = alpha ? 1 : 0;
  info.alpha_bits = alpha ? info.bits_per_sample : 0;
  info.alpha_exponent_bits = alpha ? info.exponent_bits_per_sample : 0;
  info.uses_original_profile = lossless ? JXL_TRUE : JXL_FALSE;
  if (animated) {
    info.have_animation = JXL_TRUE;
    info.animation.tps_numerator = 1000;  // ticks are milliseconds
    info.animation.tps_denominator = 1;
    info.animation.num_loops = img.loop_count;
  }
  CVT_TRY(check(JxlEncoderSetBasicInfo(enc, &info), "unsupported picture parameters"));

  bool icc_set = false;
  if (use_icc) {
    const auto* icc = reinterpret_cast<const std::uint8_t*>(img.icc_profile.data());
    icc_set = JxlEncoderSetICCProfile(enc, icc, img.icc_profile.size()) == JXL_ENC_SUCCESS;
    if (!icc_set) ctx.warn("The ICC colour profile is not usable in JPEG XL and was replaced by sRGB.");
  }
  if (!icc_set) {
    JxlColorEncoding colour{};
    if (float_samples) {
      JxlColorEncodingSetToLinearSRGB(&colour, gray ? JXL_TRUE : JXL_FALSE);
    } else {
      JxlColorEncodingSetToSRGB(&colour, gray ? JXL_TRUE : JXL_FALSE);
    }
    CVT_TRY(check(JxlEncoderSetColorEncoding(enc, &colour), "cannot set the colour encoding"));
  }

  JxlEncoderFrameSettings* settings = JxlEncoderFrameSettingsCreate(enc, nullptr);
  if (settings == nullptr) return fail(Status::Internal, "cannot create JPEG XL frame settings");
  CVT_TRY(check(JxlEncoderFrameSettingsSetOption(settings, JXL_ENC_FRAME_SETTING_EFFORT, effort), "bad effort"));
  if (lossless) {
    CVT_TRY(check(JxlEncoderSetFrameLossless(settings, JXL_TRUE), "cannot select lossless mode"));
  } else {
    CVT_TRY(check(JxlEncoderSetFrameDistance(settings, JxlEncoderDistanceFromQuality(static_cast<float>(quality))),
                  "bad distance"));
  }

  if (boxes) {
    if (!img.exif.empty()) {
      // `Exif` box: a 4-byte offset to the TIFF header, then the TIFF data.
      std::vector<std::uint8_t> exif;
      exif.reserve(img.exif.size() + 4);
      add_be32(exif, 0);
      const auto* bytes = reinterpret_cast<const std::uint8_t*>(img.exif.data());
      exif.insert(exif.end(), bytes, bytes + img.exif.size());
      CVT_TRY(check(JxlEncoderAddBox(enc, "Exif", exif.data(), exif.size(), JXL_FALSE), "cannot add the EXIF box"));
    }
    if (!img.xmp.empty()) {
      const auto* bytes = reinterpret_cast<const std::uint8_t*>(img.xmp.data());
      CVT_TRY(check(JxlEncoderAddBox(enc, "xml ", bytes, img.xmp.size(), JXL_FALSE), "cannot add the XMP box"));
    }
    JxlEncoderCloseBoxes(enc);
  }

  for (std::size_t index = 0; index < img.frames.size(); ++index) {
    CVT_TRY(ctx.checkpoint(0.5 * static_cast<double>(index) / static_cast<double>(img.frames.size()),
                           "Encoding JPEG XL"));
    const ir::Frame& frame = img.frames[index];
    if (animated) {
      JxlFrameHeader header;
      JxlEncoderInitFrameHeader(&header);
      header.duration = frame.delay_ms;
      CVT_TRY(check(JxlEncoderSetFrameHeader(settings, &header), "bad frame header"));
    }
    Samples samples;
    samples.format = JxlPixelFormat{channels, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    if (float_samples) {
      samples.format.data_type = JXL_TYPE_FLOAT;
      samples.f32 = pack<float>(hdr->rgba, channels);
    } else if (hdr != nullptr) {
      samples.format.data_type = JXL_TYPE_UINT16;
      const std::vector<std::uint16_t> srgb16 = image::srgb16_from_linear(*hdr);
      samples.u16 = pack<std::uint16_t>(srgb16, channels);
    } else {
      samples.u8 = pack<std::uint8_t>(frame.rgba, channels);
    }
    CVT_TRY(check(JxlEncoderAddImageFrame(settings, &samples.format, samples.data(), samples.bytes()),
                  "cannot add the frame"));
  }
  JxlEncoderCloseInput(enc);

  std::vector<std::uint8_t> out(std::max<std::size_t>(65536, std::size_t{first.width} * first.height / 4));
  std::size_t used = 0;
  for (;;) {
    std::uint8_t* next = out.data() + used;
    std::size_t avail = out.size() - used;
    const JxlEncoderStatus status = JxlEncoderProcessOutput(enc, &next, &avail);
    used = static_cast<std::size_t>(next - out.data());
    if (status == JXL_ENC_SUCCESS) break;
    if (status != JXL_ENC_NEED_MORE_OUTPUT) {
      if (memory.exceeded()) {
        return fail(Status::Limit, "encoding this JPEG XL image needs more than {} MiB of memory",
                    memory.limit() / (1024 * 1024));
      }
      return fail(Status::Encode, "JPEG XL encoding failed (error {})", static_cast<int>(JxlEncoderGetError(enc)));
    }
    CVT_TRY(ctx.limits.check_output(std::uint64_t{out.size()} * 2));
    out.resize(out.size() * 2);
  }
  CVT_TRY(ctx.checkpoint(1.0, "Encoding JPEG XL"));
  Bytes result(used);
  ::cvt::util::copy_bytes(result.data(), out.data(), used);
  return result;
}

}  // namespace cvt::image_modern::jxl
