// Shared helpers of the cross-family integration tests: conversions through
// the public C ABI and content checks of what comes out.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "convertible/convertible.h"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"
#include "support/video_support.hpp"

#include "codec/colorspace.hpp"
#include "codec/registry.hpp"
#include "engine/registry.hpp"
#include "ir/video.hpp"
#include "util/deflate.hpp"

namespace cvt::test::integration {

/// Outcome of one cvt_convert call, copied out of the C result.
struct CapiResult {
  int status = -1;
  std::string message;
  Bytes data;
  std::string format;
  std::string warnings_json;
  [[nodiscard]] bool ok() const noexcept { return status == CVT_OK; }
};

/// Convert through the C ABI, exactly as an app does.
inline CapiResult capi_convert(ByteView input, std::string_view filename, std::string_view target,
                               std::string_view options = {}, std::string_view source = {}) {
  const std::string name(filename);
  const std::string to(target);
  const std::string from(source);
  const std::string opts(options);
  cvt_result* result = cvt_convert(reinterpret_cast<const std::uint8_t*>(input.data()), input.size(),
                                   name.empty() ? nullptr : name.c_str(), from.empty() ? nullptr : from.c_str(),
                                   to.c_str(), opts.empty() ? nullptr : opts.c_str(), nullptr, nullptr);
  CapiResult out;
  out.status = cvt_result_status(result);
  if (const char* m = cvt_result_message(result)) out.message = m;
  const std::uint8_t* data = cvt_result_data(result);
  out.data.resize(cvt_result_size(result));
  if (!out.data.empty() && data != nullptr) std::memcpy(out.data.data(), data, out.data.size());
  if (const char* f = cvt_result_format(result)) out.format = f;
  if (const char* w = cvt_result_warnings_json(result)) out.warnings_json = w;
  cvt_result_free(result);
  return out;
}

/// Raw I420 reference frames stored gzip-compressed.
inline std::vector<ir::VideoFrame> reference_i420(std::string_view name, std::uint32_t width, std::uint32_t height) {
  std::vector<ir::VideoFrame> frames;
  auto raw = util::gzip_decompress(fixture(name), 64u << 20);
  if (!raw) return frames;
  const std::size_t size = std::size_t{width} * height + 2 * (std::size_t{(width + 1) / 2} * ((height + 1) / 2));
  for (std::size_t at = 0; at + size <= raw->size(); at += size) {
    ir::VideoFrame f = ir::VideoFrame::allocate(ir::PixelFormat::I420, width, height);
    std::size_t offset = at;
    for (ir::Plane& p : f.planes) {
      std::memcpy(p.data.data(), raw->data() + offset, p.data.size());
      offset += p.data.size();
    }
    frames.push_back(std::move(f));
  }
  return frames;
}

/// The testsrc clips: 64x64 at 10 fps, 10 frames (video/), 64x48 at 10 fps, 5 frames (integration/).
inline const std::vector<ir::VideoFrame>& testsrc_64x64() {
  static const std::vector<ir::VideoFrame> frames = reference_i420("video/ref_testsrc_64x64_i420.yuv.gz", 64, 64);
  return frames;
}
inline const std::vector<ir::VideoFrame>& testsrc_64x48() {
  static const std::vector<ir::VideoFrame> frames = reference_i420("integration/ref_testsrc_64x48_i420.yuv.gz", 64, 48);
  return frames;
}

/// Frames as limited-range I420 with the decoder's matrix (no sample change
/// for YCbCr input), RGB as FFmpeg's default BT.601, for comparison with the
/// encoders' input samples.
inline std::vector<ir::VideoFrame> as_i420(const std::vector<ir::VideoFrame>& frames) {
  std::vector<ir::VideoFrame> out;
  for (const ir::VideoFrame& f : frames) {
    ir::ColorInfo target = f.color;
    target.range = ir::ColorRange::Limited;
    if (f.format == ir::PixelFormat::RGBA8) target = ir::ColorInfo{ir::ColorMatrix::BT601, ir::ColorRange::Limited, ir::ChromaSiting::Center};
    auto converted = codec::convert_frame(f, ir::PixelFormat::I420, target);
    if (!converted) return {};
    out.push_back(std::move(*converted));
  }
  return out;
}

/// Everything a file of `format` decodes to (the bytes stay alive inside).
inline Result<Drained> decode_all(std::string_view format, const Bytes& data, std::string_view options = {}) {
  CVT_TRY_ASSIGN(auto video, read_owned<ir::Video>(format, data, options));
  return drain(*video);
}

/// Minimum per-frame PSNR against the first frames of `reference`; NaN when
/// the frame count differs from `expected_frames`.
inline double clip_psnr(const Drained& drained, const std::vector<ir::VideoFrame>& reference, std::size_t expected_frames) {
  if (drained.frames.size() != expected_frames || reference.size() < expected_frames) return detail::not_a_number;
  const std::vector<ir::VideoFrame> wanted(reference.begin(), reference.begin() + static_cast<std::ptrdiff_t>(expected_frames));
  return min_psnr(as_i420(drained.frames), wanted);
}

[[nodiscard]] inline bool has_video_encoder(std::string_view id) { return Registry::instance().media().has_video_encoder(id); }
[[nodiscard]] inline bool has_video_decoder(std::string_view id) { return Registry::instance().media().has_video_decoder(id); }
[[nodiscard]] inline bool has_audio_encoder(std::string_view id) { return Registry::instance().media().has_audio_encoder(id); }
[[nodiscard]] inline bool has_audio_decoder(std::string_view id) { return Registry::instance().media().has_audio_decoder(id); }

}  // namespace cvt::test::integration
