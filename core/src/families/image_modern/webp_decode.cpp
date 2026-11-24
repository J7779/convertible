// Convertible core - image_modern family: WebP reader (libwebp).
// Lossy (VP8), lossless (VP8L), alpha (ALPH), extended (VP8X) and animated
// (ANIM/ANMF) files, plus ICC, EXIF and XMP chunks.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "families/image/common.hpp"
#include "families/image_modern/webp_codec.hpp"
#include "src/webp/decode.h"
#include "src/webp/demux.h"
#include "util/bytes.hpp"

namespace cvt::image_modern::webp {

namespace {

const std::uint8_t* data_of(ByteView bytes) { return reinterpret_cast<const std::uint8_t*>(bytes.data()); }

const char* status_text(VP8StatusCode status) {
  switch (status) {
    case VP8_STATUS_OK: return "ok";
    case VP8_STATUS_OUT_OF_MEMORY: return "out of memory";
    case VP8_STATUS_INVALID_PARAM: return "invalid parameter";
    case VP8_STATUS_BITSTREAM_ERROR: return "the bitstream is corrupt";
    case VP8_STATUS_UNSUPPORTED_FEATURE: return "unsupported feature";
    case VP8_STATUS_SUSPENDED: return "the file is truncated";
    case VP8_STATUS_USER_ABORT: return "cancelled";
    case VP8_STATUS_NOT_ENOUGH_DATA: return "the file is truncated";
  }
  return "unknown error";
}

struct DemuxDeleter {
  void operator()(WebPDemuxer* demux) const noexcept { WebPDemuxDelete(demux); }
};
using DemuxPtr = std::unique_ptr<WebPDemuxer, DemuxDeleter>;

struct AnimDecoderDeleter {
  void operator()(WebPAnimDecoder* decoder) const noexcept { WebPAnimDecoderDelete(decoder); }
};
using AnimDecoderPtr = std::unique_ptr<WebPAnimDecoder, AnimDecoderDeleter>;

struct IDecoderDeleter {
  void operator()(WebPIDecoder* decoder) const noexcept { WebPIDelete(decoder); }
};
using IDecoderPtr = std::unique_ptr<WebPIDecoder, IDecoderDeleter>;

// The RIFF size field may be smaller than the file (trailing bytes are
// allowed and ignored); never larger than what is there.
ByteView riff_payload(ByteView input) {
  if (input.size() < 12) return input;
  util::ByteReader reader(input);
  reader.skip(4);
  const std::uint64_t total = std::uint64_t{reader.u32le()} + 8;
  if (total >= 12 && total < input.size()) return input.first(static_cast<std::size_t>(total));
  return input;
}

// ICC, EXIF and XMP chunks of an extended file.
void read_metadata(const WebPDemuxer* demux, ir::Image& image) {
  const auto chunk = [&](const char* fourcc) -> ByteView {
    WebPChunkIterator iter;
    ::cvt::util::fill_bytes(&iter, 0, sizeof iter);
    if (WebPDemuxGetChunk(demux, fourcc, 1, &iter) == 0) return {};
    const ByteView bytes(reinterpret_cast<const std::byte*>(iter.chunk.bytes), iter.chunk.size);
    WebPDemuxReleaseChunkIterator(&iter);
    return bytes;  // points into the input, which outlives the image build
  };
  const std::uint32_t flags = WebPDemuxGetI(demux, WEBP_FF_FORMAT_FLAGS);
  if ((flags & ICCP_FLAG) != 0) {
    const ByteView icc = chunk("ICCP");
    image.icc_profile.assign(icc.begin(), icc.end());
  }
  if ((flags & XMP_FLAG) != 0) {
    const ByteView xmp = chunk("XMP ");
    image.xmp.assign(reinterpret_cast<const char*>(xmp.data()), xmp.size());
  }
  if ((flags & EXIF_FLAG) != 0) {
    const ByteView exif = chunk("EXIF");
    if (!exif.empty()) (void)image::attach_exif(image, exif);
  }
}

Result<ir::Frame> decode_still(ReadContext& ctx, ByteView data, const WebPBitstreamFeatures& features) {
  const auto width = static_cast<std::uint32_t>(features.width);
  const auto height = static_cast<std::uint32_t>(features.height);
  CVT_TRY_ASSIGN(ir::Frame frame, image::new_frame(ctx, width, height, {0, 0, 0, 255}));

  WebPDecoderConfig config;
  if (WebPInitDecoderConfig(&config) == 0) return fail(Status::Internal, "libwebp version mismatch");
  config.options.use_threads = 0;
  config.output.colorspace = MODE_RGBA;  // straight (not premultiplied) alpha
  config.output.is_external_memory = 1;
  config.output.u.RGBA.rgba = frame.rgba.data();
  config.output.u.RGBA.stride = static_cast<int>(width * 4);
  config.output.u.RGBA.size = frame.rgba.size();

  IDecoderPtr decoder(WebPIDecode(nullptr, 0, &config));
  if (!decoder) return fail(Status::Decode, "cannot start the WebP decoder");
  // Reveal the data in growing slices so that progress is reported and
  // cancellation is honoured while large pictures decode. WebPIUpdate reads
  // the caller's buffer in place (no copy).
  constexpr std::size_t slice = 256 * 1024;
  VP8StatusCode status = VP8_STATUS_SUSPENDED;
  for (std::size_t offset = 0; offset < data.size() && status == VP8_STATUS_SUSPENDED; offset += slice) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(offset) / static_cast<double>(data.size()), "Decoding WebP"));
    const std::size_t visible = offset + std::min(slice, data.size() - offset);
    status = WebPIUpdate(decoder.get(), data_of(data), visible);
  }
  if (status == VP8_STATUS_SUSPENDED || status == VP8_STATUS_NOT_ENOUGH_DATA) {
    return fail(Status::Decode, "the WebP file is truncated");
  }
  if (status != VP8_STATUS_OK) return fail(Status::Decode, "WebP decoding failed: {}", status_text(status));
  return frame;
}

Result<void> decode_animation(ReadContext& ctx, ByteView data, ir::Image& image) {
  const WebPData webp_data{data_of(data), data.size()};
  WebPAnimDecoderOptions options;
  if (WebPAnimDecoderOptionsInit(&options) == 0) return fail(Status::Internal, "libwebp version mismatch");
  options.color_mode = MODE_RGBA;
  options.use_threads = 0;
  AnimDecoderPtr decoder(WebPAnimDecoderNew(&webp_data, &options));
  if (!decoder) return fail(Status::Decode, "the WebP animation is malformed");
  WebPAnimInfo info;
  if (WebPAnimDecoderGetInfo(decoder.get(), &info) == 0) return fail(Status::Decode, "the WebP animation is malformed");
  const std::uint32_t width = info.canvas_width;
  const std::uint32_t height = info.canvas_height;
  const std::uint32_t count = info.frame_count;
  image.loop_count = info.loop_count;
  image.frames.reserve(count);
  int previous_timestamp = 0;
  for (std::uint32_t index = 0; index < count; ++index) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(index) / count, "Decoding WebP animation"));
    if (WebPAnimDecoderHasMoreFrames(decoder.get()) == 0) break;
    std::uint8_t* pixels = nullptr;
    int timestamp = 0;
    if (WebPAnimDecoderGetNext(decoder.get(), &pixels, &timestamp) == 0 || pixels == nullptr) {
      return fail(Status::Decode, "frame {} of the WebP animation is corrupt or truncated", index + 1);
    }
    CVT_TRY_ASSIGN(ir::Frame frame, image::new_frame(ctx, width, height, {0, 0, 0, 0}));
    ::cvt::util::copy_bytes(frame.rgba.data(), pixels, frame.rgba.size());
    // Timestamps are the end times of the frames.
    frame.delay_ms = timestamp > previous_timestamp ? static_cast<std::uint32_t>(timestamp - previous_timestamp) : 0;
    previous_timestamp = timestamp;
    image.frames.push_back(std::move(frame));
  }
  if (image.frames.empty()) return fail(Status::Decode, "the WebP animation has no frames");
  return {};
}

}  // namespace

Result<ir::Image> read_webp(ReadContext& ctx) {
  ensure_initialised();
  const ByteView input = riff_payload(ctx.input);
  if (input.size() < 12 || as_chars(input.first(4)) != "RIFF" || as_chars(input.subspan(8, 4)) != "WEBP") {
    return fail(Status::Decode, "not a WebP file");
  }
  WebPBitstreamFeatures features;
  const VP8StatusCode probe = WebPGetFeatures(data_of(input), input.size(), &features);
  if (probe == VP8_STATUS_NOT_ENOUGH_DATA) return fail(Status::Decode, "the WebP file is truncated");
  if (probe != VP8_STATUS_OK) return fail(Status::Decode, "malformed WebP header: {}", status_text(probe));

  ir::Image image;
  const WebPData webp_data{data_of(input), input.size()};
  // The demuxer only parses the chunk structure (no pixel allocation); it
  // gives the canvas size and frame count to check against the limits
  // before anything sized by the file is allocated.
  WebPDemuxState state = WEBP_DEMUX_PARSING_HEADER;
  DemuxPtr demux(WebPDemuxInternal(&webp_data, 1, &state, WEBP_DEMUX_ABI_VERSION));
  if (!demux || state == WEBP_DEMUX_PARSE_ERROR) {
    if (features.has_animation != 0) return fail(Status::Decode, "the WebP animation is malformed");
    demux.reset();
  }
  if (features.has_animation != 0) {
    if (state != WEBP_DEMUX_DONE) return fail(Status::Decode, "the WebP animation is truncated");
    const std::uint32_t canvas_width = WebPDemuxGetI(demux.get(), WEBP_FF_CANVAS_WIDTH);
    const std::uint32_t canvas_height = WebPDemuxGetI(demux.get(), WEBP_FF_CANVAS_HEIGHT);
    const std::uint32_t frames = WebPDemuxGetI(demux.get(), WEBP_FF_FRAME_COUNT);
    if (frames == 0) return fail(Status::Decode, "the WebP animation has no frames");
    // Every frame becomes a full canvas.
    CVT_TRY(image::check_size(ctx, canvas_width, canvas_height, frames));
    CVT_TRY(decode_animation(ctx, input, image));
  } else {
    CVT_TRY(image::check_size(ctx, static_cast<std::uint64_t>(features.width),
                              static_cast<std::uint64_t>(features.height)));
    CVT_TRY_ASSIGN(ir::Frame frame, decode_still(ctx, input, features));
    image.frames.push_back(std::move(frame));
  }
  if (demux) read_metadata(demux.get(), image);  // the complete chunks, even of a cut file
  const bool extended = input.size() >= 16 && as_chars(input.subspan(12, 4)) == "VP8X";
  if (extended && state != WEBP_DEMUX_DONE) {
    ctx.warn("The WebP file is damaged or cut short after the picture; incomplete metadata was ignored.");
  }
  return image;
}

}  // namespace cvt::image_modern::webp
