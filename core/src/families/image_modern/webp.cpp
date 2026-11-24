// Convertible core - image_modern family: WebP format registration and
// libwebp initialisation.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/image_modern/webp.hpp"

#include <cstdint>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/image/common.hpp"
#include "families/image_modern/webp_codec.hpp"
#include "src/webp/decode.h"
#include "src/webp/encode.h"

// libwebp's lazily run DSP initialisers (src/dsp/*.c). All have the exact
// signature void(void); they are not part of the public headers.
extern "C" {
void VP8DspInit(void);
void VP8LDspInit(void);
void VP8EncDspInit(void);
void VP8EncDspCostInit(void);
void VP8LEncDspInit(void);
void VP8SSIMDspInit(void);
void VP8FiltersInit(void);
void WebPInitAlphaProcessing(void);
void WebPInitUpsamplers(void);
void WebPInitYUV444Converters(void);
void WebPInitSamplers(void);
void WebPInitConvertARGBToYUV(void);
void WebPRescalerDspInit(void);
}

namespace cvt::image_modern {

namespace webp {

namespace {

// Encode a small picture through the paths whose initialisers are private to
// libwebp (the gamma tables of the RGB->YUV conversion, libsharpyuv's tables)
// and decode the result (the VP8 coefficient reader).
void warm_up(bool sharp_yuv) {
  constexpr int size = 16;
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(size) * size * 4);
  for (std::size_t i = 0; i < rgba.size(); ++i) rgba[i] = static_cast<std::uint8_t>(i * 37u);
  WebPConfig config;
  WebPPicture picture;
  if (WebPConfigInit(&config) == 0 || WebPPictureInit(&picture) == 0) return;
  config.use_sharp_yuv = sharp_yuv ? 1 : 0;
  config.method = 0;
  picture.width = size;
  picture.height = size;
  picture.use_argb = 1;
  WebPMemoryWriter writer;
  WebPMemoryWriterInit(&writer);
  picture.writer = WebPMemoryWrite;
  picture.custom_ptr = &writer;
  if (WebPPictureImportRGBA(&picture, rgba.data(), size * 4) != 0 && WebPEncode(&config, &picture) != 0) {
    int width = 0;
    int height = 0;
    std::uint8_t* decoded = WebPDecodeRGBA(writer.mem, writer.size, &width, &height);
    WebPFree(decoded);
  }
  WebPPictureFree(&picture);
  WebPMemoryWriterClear(&writer);
}

}  // namespace

void ensure_initialised() {
  static const bool done = [] {
    VP8DspInit();
    VP8LDspInit();
    VP8EncDspInit();
    VP8EncDspCostInit();
    VP8LEncDspInit();
    VP8SSIMDspInit();
    VP8FiltersInit();
    WebPInitAlphaProcessing();
    WebPInitUpsamplers();
    WebPInitYUV444Converters();
    WebPInitSamplers();
    WebPInitConvertARGBToYUV();
    WebPRescalerDspInit();
    warm_up(false);
    warm_up(true);
    return true;
  }();
  (void)done;
}

}  // namespace webp

void register_webp(Registry& registry) {
  FormatInfo info;
  info.id = "webp";
  info.name = "WebP image";
  info.description = "Google's lossy and lossless web image format with transparency and animation.";
  info.category = Category::Image;
  info.extensions = {"webp"};
  info.mime = "image/webp";
  info.popularity = 85;
  info.signatures = {Signature::at(0, "'RIFF' ?? ?? ?? ?? 'WEBP'")};
  registry.add_format(std::move(info));

  image::ImageCodecSpec spec;
  spec.format_id = "webp";
  spec.read = image::reader(webp::read_webp);
  spec.write = image::writer(webp::write_webp);
  spec.write_options = webp::write_options();
  spec.lossy_write = true;
  spec.read_cost = 10;
  spec.write_cost = 15;
  add_image_codec(registry, std::move(spec));
}

}  // namespace cvt::image_modern
