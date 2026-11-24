// Convertible core - image_modern family: WebP writer (libwebp).
// Lossy (VP8 + ALPH) and lossless (VP8L) stills, near-lossless, animations
// through libwebp's animation encoder, and ICC/EXIF/XMP chunks on request.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "engine/standard_options.hpp"
#include "families/image/common.hpp"
#include "families/image_modern/webp_codec.hpp"
#include "src/webp/encode.h"
#include "src/webp/mux.h"
#include "util/bytes.hpp"

namespace cvt::image_modern::webp {

namespace {

constexpr std::string_view key_method = "method";
constexpr std::string_view key_near_lossless = "near_lossless";
constexpr std::string_view key_exact = "exact";
constexpr std::string_view key_sharp_yuv = "sharp_yuv";
constexpr std::string_view key_alpha_quality = "alpha_quality";

// Longest frame duration an ANMF chunk can store (24 bits), in ms.
constexpr std::uint32_t max_frame_duration = (1u << 24) - 1;

const char* encoding_error_text(WebPEncodingError error) {
  switch (error) {
    case VP8_ENC_OK: return "ok";
    case VP8_ENC_ERROR_OUT_OF_MEMORY:
    case VP8_ENC_ERROR_BITSTREAM_OUT_OF_MEMORY: return "out of memory";
    case VP8_ENC_ERROR_NULL_PARAMETER:
    case VP8_ENC_ERROR_INVALID_CONFIGURATION: return "invalid encoder configuration";
    case VP8_ENC_ERROR_BAD_DIMENSION: return "the picture size is not supported";
    case VP8_ENC_ERROR_PARTITION0_OVERFLOW: return "the first partition is too large (try a lower quality)";
    case VP8_ENC_ERROR_PARTITION_OVERFLOW: return "a partition is too large (try a lower quality)";
    case VP8_ENC_ERROR_BAD_WRITE: return "write error";
    case VP8_ENC_ERROR_FILE_TOO_BIG: return "the file would exceed 4 GiB";
    case VP8_ENC_ERROR_USER_ABORT: return "cancelled";
    case VP8_ENC_ERROR_LAST: break;
  }
  return "unknown error";
}

// Progress reporting from inside WebPEncode.
struct Progress {
  const WriteContext* ctx = nullptr;
  double base = 0.0;
  double span = 1.0;
  std::optional<Error> error;
};

int progress_hook(int percent, const WebPPicture* picture) {
  auto* progress = static_cast<Progress*>(picture->user_data);
  if (progress == nullptr || progress->ctx == nullptr) return 1;
  auto step = progress->ctx->checkpoint(progress->base + progress->span * percent / 100.0, "Encoding WebP");
  if (!step) {
    progress->error = std::move(step).error();
    return 0;
  }
  return 1;
}

// RAII owners of libwebp objects.
struct Picture {
  WebPPicture picture{};
  Picture() { WebPPictureInit(&picture); }
  ~Picture() { WebPPictureFree(&picture); }
  Picture(const Picture&) = delete;
  Picture& operator=(const Picture&) = delete;
};

struct MemoryWriter {
  WebPMemoryWriter writer{};
  MemoryWriter() { WebPMemoryWriterInit(&writer); }
  ~MemoryWriter() { WebPMemoryWriterClear(&writer); }
  MemoryWriter(const MemoryWriter&) = delete;
  MemoryWriter& operator=(const MemoryWriter&) = delete;
};

struct AnimEncoderDeleter {
  void operator()(WebPAnimEncoder* encoder) const noexcept { WebPAnimEncoderDelete(encoder); }
};
struct MuxDeleter {
  void operator()(WebPMux* mux) const noexcept { WebPMuxDelete(mux); }
};

struct OwnedData {
  WebPData data{};
  OwnedData() { WebPDataInit(&data); }
  ~OwnedData() { WebPDataClear(&data); }
  OwnedData(const OwnedData&) = delete;
  OwnedData& operator=(const OwnedData&) = delete;
};

Bytes to_bytes_copy(const std::uint8_t* data, std::size_t size) {
  Bytes out(size);
  if (size > 0) std::memcpy(out.data(), data, size);
  return out;
}

Result<WebPConfig> make_config(const WriteContext& ctx) {
  WebPConfig config;
  if (WebPConfigInit(&config) == 0) return fail(Status::Internal, "libwebp version mismatch");
  const options::Preset preset = options::resolved_preset(ctx.options);
  const int near_lossless = static_cast<int>(ctx.options.get_int(key_near_lossless, 100, 0, 100));
  // Near-lossless is a preprocessing step of the lossless encoder (as in
  // cwebp), so asking for it selects lossless mode.
  const bool lossless = options::resolved_lossless(ctx.options) || near_lossless < 100;
  if (lossless) {
    // Effort 0..9 exactly as `cwebp -lossless -z <level>`.
    const int level = options::resolved_compression_level(ctx.options, 9);
    if (WebPConfigLosslessPreset(&config, level) == 0) return fail(Status::Internal, "invalid lossless preset");
    config.near_lossless = near_lossless;
  } else {
    const int quality = options::resolved_quality(ctx.options);
    if (WebPConfigPreset(&config, WEBP_PRESET_DEFAULT, static_cast<float>(quality)) == 0) {
      return fail(Status::Internal, "invalid lossy preset");
    }
    config.method = preset == options::Preset::Small ? 6 : 4;
    config.use_sharp_yuv = ctx.options.get_bool(key_sharp_yuv, false) ? 1 : 0;
    config.alpha_quality = static_cast<int>(ctx.options.get_int(key_alpha_quality, 100, 0, 100));
  }
  if (ctx.options.was_set(key_method)) config.method = static_cast<int>(ctx.options.get_int(key_method, 4, 0, 6));
  config.exact = ctx.options.get_bool(key_exact, false) ? 1 : 0;
  config.thread_level = 0;
  if (WebPValidateConfig(&config) == 0) return fail(Status::InvalidArgument, "invalid WebP encoder settings");
  return config;
}

Result<void> import_frame(const ir::Frame& frame, WebPPicture& picture) {
  picture.use_argb = 1;  // WebPEncode converts to YUV itself (with sharp YUV when asked)
  picture.width = static_cast<int>(frame.width);
  picture.height = static_cast<int>(frame.height);
  if (WebPPictureImportRGBA(&picture, frame.rgba.data(), static_cast<int>(frame.width * 4)) == 0) {
    return fail(Status::Encode, "WebP encoding failed: {}", encoding_error_text(picture.error_code));
  }
  return {};
}

Result<Bytes> encode_still(const ir::Frame& frame, const WebPConfig& config, const WriteContext& ctx) {
  Picture holder;
  CVT_TRY(import_frame(frame, holder.picture));
  MemoryWriter writer;
  Progress progress;
  progress.ctx = &ctx;
  holder.picture.writer = WebPMemoryWrite;
  holder.picture.custom_ptr = &writer.writer;
  holder.picture.progress_hook = progress_hook;
  holder.picture.user_data = &progress;
  if (WebPEncode(&config, &holder.picture) == 0) {
    if (progress.error) return fail(std::move(*progress.error));
    return fail(Status::Encode, "WebP encoding failed: {}", encoding_error_text(holder.picture.error_code));
  }
  return to_bytes_copy(writer.writer.mem, writer.writer.size);
}

Result<Bytes> encode_animation(const ir::Image& image, const WebPConfig& config, const WriteContext& ctx) {
  WebPAnimEncoderOptions options;
  if (WebPAnimEncoderOptionsInit(&options) == 0) return fail(Status::Internal, "libwebp version mismatch");
  options.anim_params.loop_count = static_cast<int>(std::min<std::uint32_t>(image.loop_count, 65535));
  options.minimize_size = options::resolved_preset(ctx.options) == options::Preset::Small ? 1 : 0;
  options.allow_mixed = 0;
  options.verbose = 0;
  const std::unique_ptr<WebPAnimEncoder, AnimEncoderDeleter> encoder(
      WebPAnimEncoderNew(static_cast<int>(image.width()), static_cast<int>(image.height()), &options));
  if (!encoder) return fail(Status::Encode, "cannot start the WebP animation encoder");

  const auto encoder_error = [&]() {
    const char* text = WebPAnimEncoderGetError(encoder.get());
    return fail(Status::Encode, "WebP animation encoding failed: {}",
                text != nullptr && *text != '\0' ? text : "unknown error");
  };
  // Timestamps are start times in ms; the arithmetic wraps like libwebp's.
  std::uint32_t timestamp = 0;
  const std::size_t count = image.frames.size();
  for (std::size_t index = 0; index < count; ++index) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(index) / static_cast<double>(count), "Encoding WebP animation"));
    const ir::Frame& frame = image.frames[index];
    Picture holder;
    CVT_TRY(import_frame(frame, holder.picture));
    if (WebPAnimEncoderAdd(encoder.get(), &holder.picture, static_cast<int>(timestamp), &config) == 0) {
      return encoder_error();
    }
    timestamp += std::min(frame.delay_ms, max_frame_duration);
  }
  if (WebPAnimEncoderAdd(encoder.get(), nullptr, static_cast<int>(timestamp), nullptr) == 0) return encoder_error();
  OwnedData assembled;
  if (WebPAnimEncoderAssemble(encoder.get(), &assembled.data) == 0) return encoder_error();
  return to_bytes_copy(assembled.data.bytes, assembled.data.size);
}

// Add ICCP/EXIF/XMP chunks (the mux turns the file into the extended format).
Result<Bytes> add_metadata(Bytes encoded, const ir::Image& image) {
  const bool has_icc = !image.icc_profile.empty();
  const bool has_exif = !image.exif.empty();
  const bool has_xmp = !image.xmp.empty();
  if (!has_icc && !has_exif && !has_xmp) return encoded;
  const WebPData bitstream{reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size()};
  const std::unique_ptr<WebPMux, MuxDeleter> mux(WebPMuxCreate(&bitstream, 1));
  if (!mux) return fail(Status::Internal, "cannot re-read the encoded WebP file");
  const auto set = [&](const char* fourcc, const void* data, std::size_t size) -> Result<void> {
    const WebPData chunk{static_cast<const std::uint8_t*>(data), size};
    if (WebPMuxSetChunk(mux.get(), fourcc, &chunk, 1) != WEBP_MUX_OK) {
      return fail(Status::Encode, "cannot store the {} chunk in the WebP file", fourcc);
    }
    return {};
  };
  if (has_icc) CVT_TRY(set("ICCP", image.icc_profile.data(), image.icc_profile.size()));
  if (has_exif) CVT_TRY(set("EXIF", image.exif.data(), image.exif.size()));
  if (has_xmp) CVT_TRY(set("XMP ", image.xmp.data(), image.xmp.size()));
  OwnedData assembled;
  if (WebPMuxAssemble(mux.get(), &assembled.data) != WEBP_MUX_OK) {
    return fail(Status::Encode, "cannot assemble the WebP file with its metadata");
  }
  return to_bytes_copy(assembled.data.bytes, assembled.data.size);
}

}  // namespace

std::vector<OptionSpec> write_options() {
  std::vector<OptionSpec> specs = {
      options::quality(),
      options::preset(),
      options::lossless(),
      OptionSpec::integer(std::string(key_method), "Effort", 4, 0, 6,
                          "Compression effort, 0 (fastest) to 6 (smallest files). Default: 4, or 6 with the "
                          "'small' preset; in lossless mode it follows the compression level.")
          .advanced(),
      options::compression_level(9),
      OptionSpec::integer(std::string(key_near_lossless), "Near-lossless", 100, 0, 100,
                          "Below 100, lossless output is preprocessed to compress better with barely visible "
                          "changes (lower = smaller). Implies lossless mode.")
          .advanced(),
      OptionSpec::integer(std::string(key_alpha_quality), "Transparency quality", 100, 0, 100,
                          "Quality of the transparency channel in lossy mode (100 keeps it lossless).")
          .advanced(),
      OptionSpec::boolean(std::string(key_exact), "Keep hidden colours", false,
                          "Preserve the colour of fully transparent pixels instead of simplifying it.")
          .advanced(),
      OptionSpec::boolean(std::string(key_sharp_yuv), "Sharp colour conversion", false,
                          "Slower, more accurate RGB to YUV conversion for lossy output (crisper colour edges).")
          .advanced(),
  };
  for (OptionSpec& spec : image::size_options()) specs.push_back(std::move(spec));
  specs.push_back(options::strip_metadata());
  return specs;
}

Result<Bytes> write_webp(const ir::Image& source, WriteContext& ctx) {
  ensure_initialised();
  CVT_TRY_ASSIGN(const image::WriteImage prepared, image::prepare_write(source, ctx));
  const ir::Image& image = *prepared;
  if (image.width() > max_dimension || image.height() > max_dimension) {
    return fail(Status::Encode,
                "WebP pictures are at most {} x {} pixels (this one is {} x {}); use max_width/max_height to shrink it",
                max_dimension, max_dimension, image.width(), image.height());
  }
  CVT_TRY_ASSIGN(const WebPConfig config, make_config(ctx));
  Bytes encoded;
  if (image.animated()) {
    CVT_TRY_ASSIGN(encoded, encode_animation(image, config, ctx));
  } else {
    CVT_TRY_ASSIGN(encoded, encode_still(image.frames.front(), config, ctx));
  }
  if (prepared.keep_metadata()) return add_metadata(std::move(encoded), image);
  return encoded;
}

}  // namespace cvt::image_modern::webp
