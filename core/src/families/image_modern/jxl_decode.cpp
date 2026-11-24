// Convertible core - image_modern family: JPEG XL reader (imported libjxl).
//
// Still images (VarDCT and modular), alpha, animation (coalesced frames with
// their delays), colour handling (enumerated colour encodings are converted
// to sRGB by libjxl's CMS; images described by an ICC profile keep their
// pixels and the profile), more than 8 bits or float samples -> `hdr` (linear
// sRGB) plus a tone-mapped 8-bit frame, the codestream orientation applied to
// the pixels, and the container's `Exif` / `xml ` boxes (also when they are
// Brotli-compressed in `brob` boxes).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <jxl/cms.h>
#include <jxl/codestream_header.h>
#include <jxl/color_encoding.h>
#include <jxl/decode.h>
#include <jxl/encode.h>
#include <jxl/memory_manager.h>
#include <jxl/types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "families/image/common.hpp"
#include "families/image/image_ops.hpp"
#include "families/image_modern/jxl_codec.hpp"

namespace cvt::image_modern::jxl {

namespace {

/// Largest metadata box we keep (EXIF / XMP); larger ones are skipped.
constexpr std::size_t max_metadata_box = 16u * 1024 * 1024;
/// Reference white in nits for PQ/HLG content (ITU-R BT.2408): linear 1.0 in
/// `ir::HdrPixels` means SDR white.
constexpr float reference_white_nits = 203.0f;

void* budget_alloc(void* opaque, std::size_t size) { return static_cast<MemoryBudget*>(opaque)->allocate(size); }
void budget_free(void* opaque, void* address) { static_cast<MemoryBudget*>(opaque)->release(address); }

struct DecoderDeleter {
  void operator()(JxlDecoder* decoder) const noexcept { JxlDecoderDestroy(decoder); }
};
using DecoderPtr = std::unique_ptr<JxlDecoder, DecoderDeleter>;

enum class BoxKind { None, Exif, Xmp };

/// Collects one metadata box through JXL_DEC_BOX / JXL_DEC_BOX_NEED_MORE_OUTPUT.
struct BoxCollector {
  BoxKind kind = BoxKind::None;
  std::vector<std::uint8_t> data;
  std::size_t filled = 0;
  bool too_large = false;

  // Hands the unused tail of `data` to the decoder.
  [[nodiscard]] bool attach(JxlDecoder* decoder) {
    return JxlDecoderSetBoxBuffer(decoder, data.data() + filled, data.size() - filled) == JXL_DEC_SUCCESS;
  }
  // Takes the buffer back; records how much was written.
  void detach(JxlDecoder* decoder) {
    const std::size_t remaining = JxlDecoderReleaseBoxBuffer(decoder);
    filled = data.size() - std::min(remaining, data.size() - filled);
  }
};

bool is_type(const JxlBoxType type, const char (&name)[5]) { return std::memcmp(type, name, 4) == 0; }

/// The scale that makes linear 1.0 SDR white for PQ/HLG content.
float hdr_scale(const JxlColorEncoding& original, const JxlBasicInfo& info) {
  if (original.transfer_function == JXL_TRANSFER_FUNCTION_PQ ||
      original.transfer_function == JXL_TRANSFER_FUNCTION_HLG) {
    if (info.intensity_target > reference_white_nits) return info.intensity_target / reference_white_nits;
  }
  return 1.0f;
}

void store_exif(ir::Image& image, const std::vector<std::uint8_t>& box, const ReadContext& ctx) {
  // A JPEG XL `Exif` box starts with a big-endian offset to the TIFF header.
  if (box.size() < 4) return;
  const std::uint32_t offset = (std::uint32_t{box[0]} << 24) | (std::uint32_t{box[1]} << 16) |
                               (std::uint32_t{box[2]} << 8) | std::uint32_t{box[3]};
  if (offset > box.size() - 4) {
    ctx.warn("The JPEG XL file's EXIF box is malformed and was ignored.");
    return;
  }
  Bytes exif(box.size() - 4 - offset);
  std::memcpy(exif.data(), box.data() + 4 + offset, exif.size());
  const auto parsed = image::parse_exif(exif);
  if (!parsed) {
    ctx.warn("The JPEG XL file's EXIF data is malformed and was ignored.");
    return;
  }
  // The codestream orientation is authoritative and has already been applied
  // by the decoder, so the EXIF orientation is only reset, never applied.
  image::reset_exif_orientation(exif);
  for (const auto& [key, value] : parsed->text) image.metadata.emplace(key, value);
  if (parsed->dpi_x > 0 && parsed->dpi_y > 0) {
    image.dpi_x = parsed->dpi_x;
    image.dpi_y = parsed->dpi_y;
  }
  image.exif = std::move(exif);
}

}  // namespace

Result<ir::Image> read(ReadContext& ctx) {
  const auto* data = reinterpret_cast<const std::uint8_t*>(ctx.input.data());
  const std::size_t size = ctx.input.size();
  const JxlSignature signature = JxlSignatureCheck(data, size);
  if (signature != JXL_SIG_CODESTREAM && signature != JXL_SIG_CONTAINER) {
    return fail(Status::Decode, "not a JPEG XL file");
  }

  MemoryBudget memory(decode_budget(ctx.limits, 0));
  const JxlMemoryManager manager{&memory, budget_alloc, budget_free};
  DecoderPtr decoder(JxlDecoderCreate(&manager));
  if (!decoder) return fail(Status::Internal, "cannot create the JPEG XL decoder");
  JxlDecoder* dec = decoder.get();

  const int events = JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE | JXL_DEC_BOX;
  if (JxlDecoderSubscribeEvents(dec, events) != JXL_DEC_SUCCESS ||
      JxlDecoderSetDecompressBoxes(dec, JXL_TRUE) != JXL_DEC_SUCCESS ||
      JxlDecoderSetUnpremultiplyAlpha(dec, JXL_TRUE) != JXL_DEC_SUCCESS) {
    return fail(Status::Internal, "cannot configure the JPEG XL decoder");
  }
  JxlDecoderSetCms(dec, *JxlGetDefaultCms());
  if (JxlDecoderSetInput(dec, data, size) != JXL_DEC_SUCCESS) {
    return fail(Status::Internal, "cannot pass input to the JPEG XL decoder");
  }
  JxlDecoderCloseInput(dec);

  ir::Image image;
  JxlBasicInfo info{};
  bool have_info = false;
  bool high_precision = false;
  float scale = 1.0f;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t delay_ms = 0;
  std::vector<float> hdr_pixels;
  ir::Frame frame;
  BoxCollector box;
  bool in_frame = false;

  const auto finish_box = [&] {
    if (box.kind == BoxKind::None) return;
    box.detach(dec);
    box.data.resize(box.filled);
    if (box.too_large) {
      ctx.warn("A JPEG XL metadata box is too large and was ignored.");
    } else if (box.kind == BoxKind::Exif && image.exif.empty()) {
      store_exif(image, box.data, ctx);
    } else if (box.kind == BoxKind::Xmp && image.xmp.empty()) {
      image.xmp.assign(box.data.begin(), box.data.end());
    }
    box = BoxCollector{};
  };
  const auto decode_error = [&]() -> Result<ir::Image> {
    if (memory.exceeded()) {
      return fail(Status::Limit, "decoding this JPEG XL image needs more than {} MiB of memory",
                  memory.limit() / (1024 * 1024));
    }
    return fail(Status::Decode, "the JPEG XL data is corrupt or truncated");
  };

  for (;;) {
    const JxlDecoderStatus status = JxlDecoderProcessInput(dec);
    switch (status) {
      case JXL_DEC_ERROR:
        // A cut-off animation still yields the frames decoded so far.
        if (!memory.exceeded() && !in_frame && !image.frames.empty()) {
          ctx.warn("The JPEG XL file is truncated or damaged; only the frames before the damage were decoded.");
          finish_box();
          return image;
        }
        return decode_error();
      case JXL_DEC_NEED_MORE_INPUT:
        // The input is complete (closed), so this means truncation.
        if (!in_frame && !image.frames.empty()) {
          ctx.warn("The JPEG XL file is truncated; only the frames before the cut were decoded.");
          finish_box();
          return image;
        }
        return fail(Status::Decode, "the JPEG XL file is truncated");
      case JXL_DEC_BASIC_INFO: {
        if (JxlDecoderGetBasicInfo(dec, &info) != JXL_DEC_SUCCESS) return decode_error();
        have_info = true;
        width = info.xsize;
        height = info.ysize;
        if (info.orientation > JXL_ORIENT_ROTATE_180) std::swap(width, height);  // transposing orientations
        CVT_TRY(image::check_size(ctx, width, height));
        memory.set_limit(decode_budget(ctx.limits, std::uint64_t{width} * height));
        high_precision = info.bits_per_sample > 8 || info.exponent_bits_per_sample > 0;
        image.source_bit_depth =
            info.exponent_bits_per_sample > 0 ? 32 : static_cast<std::uint8_t>(std::clamp<std::uint32_t>(info.bits_per_sample, 1, 16));
        image.source_grayscale = info.num_color_channels == 1;
        if (info.have_animation) image.loop_count = info.animation.num_loops;
        for (std::uint32_t i = 0; i < info.num_extra_channels; ++i) {
          JxlExtraChannelInfo extra{};
          if (JxlDecoderGetExtraChannelInfo(dec, i, &extra) == JXL_DEC_SUCCESS && extra.type != JXL_CHANNEL_ALPHA &&
              extra.type != JXL_CHANNEL_SPOT_COLOR) {
            ctx.warn("Extra JPEG XL channels other than alpha (depth, thermal, ...) were ignored.");
            break;
          }
        }
        break;
      }
      case JXL_DEC_COLOR_ENCODING: {
        JxlColorEncoding original{};
        const bool enumerated =
            JxlDecoderGetColorAsEncodedProfile(dec, JXL_COLOR_PROFILE_TARGET_ORIGINAL, &original) == JXL_DEC_SUCCESS;
        const bool gray = info.num_color_channels == 1;
        if (high_precision || enumerated || gray) {
          // Convert to (linear) sRGB with libjxl's CMS.
          JxlColorEncoding target{};
          if (high_precision) {
            JxlColorEncodingSetToLinearSRGB(&target, gray ? JXL_TRUE : JXL_FALSE);
            if (enumerated) scale = hdr_scale(original, info);
          } else {
            JxlColorEncodingSetToSRGB(&target, gray ? JXL_TRUE : JXL_FALSE);
          }
          if (JxlDecoderSetOutputColorProfile(dec, &target, nullptr, 0) != JXL_DEC_SUCCESS) {
            return fail(Status::Decode, "cannot convert the JPEG XL colour space to sRGB");
          }
        } else {
          // Described by an ICC profile: keep the pixels in that space and
          // hand the profile on.
          std::size_t icc_size = 0;
          if (JxlDecoderGetICCProfileSize(dec, JXL_COLOR_PROFILE_TARGET_DATA, &icc_size) == JXL_DEC_SUCCESS &&
              icc_size > 0 && icc_size <= max_metadata_box) {
            std::vector<std::uint8_t> icc(icc_size);
            if (JxlDecoderGetColorAsICCProfile(dec, JXL_COLOR_PROFILE_TARGET_DATA, icc.data(), icc.size()) ==
                JXL_DEC_SUCCESS) {
              image.icc_profile.resize(icc.size());
              std::memcpy(image.icc_profile.data(), icc.data(), icc.size());
            }
          }
        }
        break;
      }
      case JXL_DEC_FRAME: {
        if (!have_info) return decode_error();
        JxlFrameHeader header{};
        if (JxlDecoderGetFrameHeader(dec, &header) != JXL_DEC_SUCCESS) return decode_error();
        delay_ms = 0;
        if (info.have_animation && info.animation.tps_numerator > 0) {
          const std::uint64_t ms = std::uint64_t{header.duration} * 1000u * info.animation.tps_denominator /
                                   info.animation.tps_numerator;
          delay_ms = static_cast<std::uint32_t>(std::min<std::uint64_t>(ms, UINT32_MAX));
        }
        CVT_TRY(image::check_size(ctx, width, height, image.frames.size() + 1));
        CVT_TRY(ctx.checkpoint(0.0, "Decoding JPEG XL"));
        in_frame = true;
        break;
      }
      case JXL_DEC_NEED_IMAGE_OUT_BUFFER: {
        const JxlPixelFormat format{4, high_precision ? JXL_TYPE_FLOAT : JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
        std::size_t needed = 0;
        if (JxlDecoderImageOutBufferSize(dec, &format, &needed) != JXL_DEC_SUCCESS) return decode_error();
        const std::uint64_t sample_bytes = high_precision ? sizeof(float) : 1;
        const std::uint64_t expected = std::uint64_t{width} * height * 4 * sample_bytes;
        // 32-bit targets (wasm32, armv7) cannot address a float frame this big.
        if (expected > SIZE_MAX / 2) return fail(Status::Limit, "the JPEG XL frame is too large for this device");
        if (needed != expected) {
          return fail(Status::Decode, "unexpected JPEG XL output size");
        }
        if (high_precision) {
          hdr_pixels.assign(std::size_t{width} * height * 4, 0.0f);
          if (JxlDecoderSetImageOutBuffer(dec, &format, hdr_pixels.data(), needed) != JXL_DEC_SUCCESS) {
            return decode_error();
          }
        } else {
          frame = ir::Frame::filled(width, height);
          if (JxlDecoderSetImageOutBuffer(dec, &format, frame.rgba.data(), needed) != JXL_DEC_SUCCESS) {
            return decode_error();
          }
        }
        break;
      }
      case JXL_DEC_FULL_IMAGE: {
        if (high_precision) {
          ir::HdrPixels pixels;
          pixels.width = width;
          pixels.height = height;
          pixels.rgba = std::move(hdr_pixels);
          hdr_pixels.clear();
          for (std::size_t i = 0; i < pixels.rgba.size(); ++i) {
            if (i % 4 != 3) pixels.rgba[i] *= scale;
          }
          frame = image::tone_map(pixels);
          if (image.frames.empty()) image.hdr = std::move(pixels);
        }
        frame.delay_ms = info.have_animation ? delay_ms : 0;
        image.frames.push_back(std::move(frame));
        frame = ir::Frame{};
        in_frame = false;
        break;
      }
      case JXL_DEC_BOX: {
        finish_box();
        JxlBoxType type{};
        if (JxlDecoderGetBoxType(dec, type, JXL_TRUE) != JXL_DEC_SUCCESS) break;
        const BoxKind kind = is_type(type, "Exif") ? BoxKind::Exif : is_type(type, "xml ") ? BoxKind::Xmp : BoxKind::None;
        if (kind == BoxKind::None) break;
        box.kind = kind;
        box.data.resize(4096);
        if (!box.attach(dec)) box = BoxCollector{};
        break;
      }
      case JXL_DEC_BOX_NEED_MORE_OUTPUT: {
        box.detach(dec);
        if (box.data.size() >= max_metadata_box) {
          // Keep consuming into a scratch window so decoding can go on.
          box.too_large = true;
          box.filled = 0;
        } else {
          box.data.resize(std::min(box.data.size() * 2, max_metadata_box));
        }
        if (!box.attach(dec)) return decode_error();
        break;
      }
      case JXL_DEC_SUCCESS: {
        finish_box();
        if (image.frames.empty()) return fail(Status::Decode, "the JPEG XL file contains no image");
        return image;
      }
      default:
        // Events we did not subscribe to (preview, JPEG reconstruction ...).
        break;
    }
  }
}

}  // namespace cvt::image_modern::jxl
