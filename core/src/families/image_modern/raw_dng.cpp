// Convertible core - image_modern family: Adobe DNG reader.
// Written from the Digital Negative specification 1.4-1.7 (Adobe) and
// TIFF/EP (ISO 12234-2).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <vector>

#include "families/image_modern/raw_readers.hpp"

namespace cvt::image_modern::raw {

namespace {

enum : std::uint16_t {
  kNewSubFileType = 254, kImageWidth = 256, kImageLength = 257, kBitsPerSample = 258, kCompression = 259,
  kPhotometric = 262, kMake = 271, kModel = 272, kOrientation = 274, kSamplesPerPixel = 277,
  kCfaRepeatPatternDim = 33421, kCfaPattern = 33422, kCfaPatternExif = 41730, kXmp = 700,
  kUniqueCameraModel = 50708, kCfaPlaneColor = 50710, kCfaLayout = 50711, kLinearizationTable = 50712,
  kBlackLevelRepeatDim = 50713, kBlackLevel = 50714, kBlackLevelDeltaH = 50715, kBlackLevelDeltaV = 50716,
  kWhiteLevel = 50717, kDefaultCropOrigin = 50719, kDefaultCropSize = 50720, kColorMatrix1 = 50721,
  kColorMatrix2 = 50722, kCameraCalibration1 = 50723, kCameraCalibration2 = 50724, kAnalogBalance = 50727,
  kAsShotNeutral = 50728, kAsShotWhiteXY = 50729, kBaselineExposure = 50730, kCalibrationIlluminant1 = 50778,
  kCalibrationIlluminant2 = 50779, kActiveArea = 50829, kForwardMatrix1 = 50964, kForwardMatrix2 = 50965,
  kOpcodeList1 = 51008, kOpcodeList2 = 51009, kOpcodeList3 = 51022, kBaselineExposureOffset = 51109,
};

constexpr std::uint16_t kPhotometricCfa = 32803;
constexpr std::uint16_t kPhotometricLinear = 34892;

std::optional<Matrix3> matrix(const TiffData& t, const Ifd& a, const Ifd& b, std::uint16_t tag) {
  auto v = t.numbers(a, tag, 9);
  if (v.size() < 9) v = t.numbers(b, tag, 9);
  if (v.size() < 9) return std::nullopt;
  Matrix3 m{};
  for (std::size_t i = 0; i < 9; ++i) m[i] = v[i];
  bool zero = true;
  for (const double x : m) zero = zero && x == 0.0;
  if (zero) return std::nullopt;
  return m;
}

std::vector<double> numbers2(const TiffData& t, const Ifd& a, const Ifd& b, std::uint16_t tag, std::size_t limit) {
  auto v = t.numbers(a, tag, limit);
  return v.empty() ? t.numbers(b, tag, limit) : v;
}

// OpcodeList2 (always big-endian): GainMap opcodes (id 9) are kept; returns
// false when a mandatory opcode of another kind is present.
bool parse_gain_maps(ByteView list, std::vector<RawInfo::GainMap>& out) {
  const TiffData d(list, 0, false);
  if (list.size() < 4) return true;
  const std::uint32_t count = d.u32(0);
  std::uint64_t at = 4;
  bool all_applied = true;
  for (std::uint32_t i = 0; i < count && i < 4096; ++i) {
    if (!d.has(at, 16)) return false;
    const std::uint32_t id = d.u32(at);
    const std::uint32_t flags = d.u32(at + 8);
    const std::uint32_t size = d.u32(at + 12);
    const std::uint64_t p = at + 16;
    if (!d.has(p, size)) return false;
    if (id == 9 && size >= 76) {
      RawInfo::GainMap gm;
      gm.top = d.u32(p);
      gm.left = d.u32(p + 4);
      gm.bottom = d.u32(p + 8);
      gm.right = d.u32(p + 12);
      gm.plane = d.u32(p + 16);
      gm.planes = d.u32(p + 20);
      gm.row_pitch = d.u32(p + 24);
      gm.col_pitch = d.u32(p + 28);
      gm.points_v = d.u32(p + 32);
      gm.points_h = d.u32(p + 36);
      const auto f64 = [&](std::uint64_t q) {
        const double v = std::bit_cast<double>(d.uint(q, 8));
        return std::isfinite(v) ? v : 0.0;
      };
      gm.spacing_v = f64(p + 40);
      gm.spacing_h = f64(p + 48);
      gm.origin_v = f64(p + 56);
      gm.origin_h = f64(p + 64);
      gm.map_planes = d.u32(p + 72);
      const std::uint64_t n = std::uint64_t{gm.points_v} * gm.points_h * gm.map_planes;
      if (gm.points_v == 0 || gm.points_h == 0 || gm.map_planes == 0 || gm.planes == 0 || gm.planes > 4 ||
          n > (1u << 20) || 76 + n * 4 > size) {
        all_applied = false;
      } else {
        gm.gains.resize(static_cast<std::size_t>(n));
        for (std::uint64_t k = 0; k < n; ++k) {
          const float g = std::bit_cast<float>(d.u32(p + 76 + k * 4));
          gm.gains[static_cast<std::size_t>(k)] = std::isfinite(g) ? std::clamp(g, 0.0f, 64.0f) : 1.0f;
        }
        out.push_back(std::move(gm));
      }
    } else if ((flags & 1u) == 0) {
      all_applied = false;  // a mandatory opcode we do not implement
    }
    at = p + size;
  }
  return all_applied;
}

}  // namespace

std::optional<Cfa> tiff_cfa(const TiffData& t, const Ifd& ifd) {
  Cfa cfa;
  std::vector<std::uint64_t> pattern;
  const auto dims = t.integers(ifd, kCfaRepeatPatternDim, 2);
  if (dims.size() == 2) {
    cfa.height = static_cast<std::uint32_t>(dims[0]);
    cfa.width = static_cast<std::uint32_t>(dims[1]);
    pattern = t.integers(ifd, kCfaPattern, 64);
  } else if (const ByteView exif = t.bytes(ifd, kCfaPatternExif); exif.size() >= 4) {
    // EXIF form: horizontal repeat, vertical repeat (shorts), then the colours.
    const TiffData sub(exif, 0, t.little());
    cfa.width = sub.u16(0);
    cfa.height = sub.u16(2);
    for (std::size_t i = 4; i < exif.size(); ++i) pattern.push_back(std::to_integer<std::uint64_t>(exif[i]));
  } else {
    return std::nullopt;
  }
  if (cfa.width == 0 || cfa.height == 0 || cfa.width > 8 || cfa.height > 8 || pattern.size() < std::size_t{cfa.width} * cfa.height) {
    return std::nullopt;
  }
  std::vector<std::uint64_t> planes = t.integers(ifd, kCfaPlaneColor, 8);
  if (planes.empty()) planes = {0, 1, 2};
  for (std::uint32_t i = 0; i < cfa.width * cfa.height; ++i) {
    const std::uint64_t v = pattern[i];
    // Pattern values are indices into CFAPlaneColor (TIFF-EP colour codes:
    // 0 red, 1 green, 2 blue, 3 cyan, 4 magenta, 5 yellow, 6 white).
    const std::uint64_t color = v < planes.size() ? planes[static_cast<std::size_t>(v)] : 99;
    cfa.colors[i] = static_cast<std::uint8_t>(std::min<std::uint64_t>(color, 99));
  }
  return cfa;
}

Result<ir::Image> preview_fallback(const std::vector<Preview>& previews, std::string_view reason, const Context& ctx) {
  auto image = decode_largest_preview(previews, ctx);
  if (!image) {
    if (image.error().code == Status::Cancelled || image.error().code == Status::Limit) return image;
    return fail(Status::Decode, "{} and the file has no usable embedded preview", reason);
  }
  ctx.warn(std::format("{}; the embedded preview ({}x{}) was used instead of the RAW data.", reason, image->width(),
                       image->height()));
  return image;
}

Result<ir::Image> develop_tiff_raw(const TiffTree& tree, const Ifd& raw_ifd, RawInfo info, const Request& request,
                                   const Context& ctx) {
  CVT_TRY_ASSIGN(Sensor sensor, read_samples(tree.tiff, raw_ifd, ctx));
  return develop(std::move(sensor), info, request.develop, ctx);
}

Result<ir::Image> read_dng(const TiffTree& tree, const Request& request, const Context& ctx) {
  const TiffData& t = tree.tiff;
  const Ifd& ifd0 = tree.ifds.front();
  const int orientation = static_cast<int>(t.integer(ifd0, kOrientation, 1));
  std::vector<Preview> previews;
  collect_tiff_previews(tree, orientation, previews);
  if (request.preview) {
    auto image = decode_largest_preview(previews, ctx);
    if (image) return image;
    ctx.warn("The DNG file has no embedded preview; the RAW data was developed instead.");
  }

  // The main RAW image: NewSubFileType 0 with CFA or LinearRaw photometry;
  // an "enhanced" image (16) when nothing else exists.
  const Ifd* raw = nullptr;
  std::uint64_t best = 0;
  for (const std::uint64_t wanted : {std::uint64_t{0}, std::uint64_t{16}}) {
    for (const Ifd& ifd : tree.ifds) {
      const std::uint64_t photometric = t.integer(ifd, kPhotometric);
      if (photometric != kPhotometricCfa && photometric != kPhotometricLinear) continue;
      if (t.integer(ifd, kNewSubFileType, 0) != wanted) continue;
      const std::uint64_t pixels = t.integer(ifd, kImageWidth) * t.integer(ifd, kImageLength);
      if (raw == nullptr || pixels > best) {
        raw = &ifd;
        best = pixels;
      }
    }
    if (raw != nullptr) break;
  }
  if (raw == nullptr) return preview_fallback(previews, "The DNG file contains no RAW image", ctx);

  const std::uint64_t compression = t.integer(*raw, kCompression, 1);
  if (compression != 1 && compression != 7 && compression != 8 && compression != 32946 && compression != 34892) {
    return preview_fallback(previews, std::format("DNG compression {} is not supported", compression), ctx);
  }

  RawInfo info;
  const std::uint64_t photometric = t.integer(*raw, kPhotometric);
  const int spp = static_cast<int>(t.integer(*raw, kSamplesPerPixel, 1));
  if (photometric == kPhotometricCfa) {
    if (t.integer(*raw, kCfaLayout, 1) != 1) {
      return preview_fallback(previews, "Non-rectangular colour filter layouts are not supported", ctx);
    }
    auto cfa = tiff_cfa(t, *raw);
    if (!cfa) return fail(Status::Decode, "the DNG colour filter pattern is missing or invalid");
    for (std::uint32_t i = 0; i < cfa->width * cfa->height; ++i) {
      if (cfa->colors[i] > 2) {
        return preview_fallback(previews, "Colour filter arrays other than red/green/blue are not supported", ctx);
      }
    }
    if (spp != 1) return fail(Status::Decode, "DNG CFA data must have one sample per pixel");
    info.cfa = *cfa;
  } else if (spp != 1 && spp != 3) {
    return preview_fallback(previews, std::format("Linear DNG data with {} samples is not supported", spp), ctx);
  }

  // Levels.
  for (const std::uint64_t v : t.integers(*raw, kLinearizationTable, 65536)) {
    info.levels.linearization.push_back(static_cast<std::uint16_t>(std::min<std::uint64_t>(v, 65535)));
  }
  const auto repeat = t.integers(*raw, kBlackLevelRepeatDim, 2);
  if (repeat.size() == 2 && repeat[0] >= 1 && repeat[1] >= 1 && repeat[0] <= 64 && repeat[1] <= 64) {
    info.levels.black_rows = static_cast<std::uint32_t>(repeat[0]);
    info.levels.black_cols = static_cast<std::uint32_t>(repeat[1]);
  }
  info.levels.black = t.numbers(*raw, kBlackLevel, 64 * 64 * 4);
  const std::size_t needed = std::size_t{info.levels.black_rows} * info.levels.black_cols * static_cast<std::size_t>(spp);
  if (info.levels.black.size() == 1 && needed > 1) info.levels.black.assign(needed, info.levels.black.front());
  if (info.levels.black.size() < needed) {
    info.levels.black.clear();
    info.levels.black_rows = info.levels.black_cols = 1;
  }
  info.levels.black_delta_h = t.numbers(*raw, kBlackLevelDeltaH, 1u << 20);
  info.levels.black_delta_v = t.numbers(*raw, kBlackLevelDeltaV, 1u << 20);
  info.levels.white = t.numbers(*raw, kWhiteLevel, 4);

  // Geometry.
  const auto active = t.integers(*raw, kActiveArea, 4);
  if (active.size() == 4) {
    for (std::size_t i = 0; i < 4; ++i) info.active[i] = static_cast<std::uint32_t>(std::min<std::uint64_t>(active[i], 0xFFFFFFFFu));
  }
  const auto origin = t.numbers(*raw, kDefaultCropOrigin, 2);
  const auto size = t.numbers(*raw, kDefaultCropSize, 2);
  if (origin.size() == 2 && size.size() == 2 && size[0] >= 1 && size[1] >= 1) {
    info.crop_x = origin[0];
    info.crop_y = origin[1];
    info.crop_w = size[0];
    info.crop_h = size[1];
  }
  info.orientation = orientation;

  // Colour.
  ColorInfo& c = info.color;
  c.color_matrix1 = matrix(t, ifd0, *raw, kColorMatrix1);
  c.color_matrix2 = matrix(t, ifd0, *raw, kColorMatrix2);
  c.forward_matrix1 = matrix(t, ifd0, *raw, kForwardMatrix1);
  c.forward_matrix2 = matrix(t, ifd0, *raw, kForwardMatrix2);
  c.calibration1 = matrix(t, ifd0, *raw, kCameraCalibration1);
  c.calibration2 = matrix(t, ifd0, *raw, kCameraCalibration2);
  if (const auto ab = numbers2(t, ifd0, *raw, kAnalogBalance, 3); ab.size() == 3 && ab[0] > 0 && ab[1] > 0 && ab[2] > 0) {
    c.analog_balance = std::array<double, 3>{ab[0], ab[1], ab[2]};
  }
  c.neutral = numbers2(t, ifd0, *raw, kAsShotNeutral, 4);
  if (c.neutral.size() < 3 || c.neutral[0] <= 0 || c.neutral[1] <= 0 || c.neutral[2] <= 0) c.neutral.clear();
  if (const auto xy = numbers2(t, ifd0, *raw, kAsShotWhiteXY, 2); xy.size() == 2 && xy[0] > 0 && xy[1] > 0) {
    c.white_xy = std::array<double, 2>{xy[0], xy[1]};
  }
  c.illuminant1 = static_cast<int>(t.integer(ifd0, kCalibrationIlluminant1, c.color_matrix1 ? 21 : 0));
  c.illuminant2 = static_cast<int>(t.integer(ifd0, kCalibrationIlluminant2, 0));
  c.baseline_exposure = std::clamp(t.number(ifd0, kBaselineExposure, 0.0) + t.number(ifd0, kBaselineExposureOffset, 0.0), -10.0, 10.0);
  if (spp == 1 && !info.cfa) {
    // Monochrome: no colour processing.
    c = ColorInfo{};
    c.baseline_exposure = std::clamp(t.number(ifd0, kBaselineExposure, 0.0), -10.0, 10.0);
  }

  info.make = t.text(ifd0, kMake);
  info.model = t.text(ifd0, kModel);
  if (info.model.empty()) info.model = t.text(ifd0, kUniqueCameraModel);
  bool skipped_opcodes = raw->has(kOpcodeList1) || raw->has(kOpcodeList3);
  if (raw->has(kOpcodeList2)) skipped_opcodes |= !parse_gain_maps(t.bytes(*raw, kOpcodeList2), info.gain_maps);
  if (skipped_opcodes) ctx.warn("Some DNG opcodes (lens distortion or vignetting corrections) were not applied.");

  auto image = develop_tiff_raw(tree, *raw, std::move(info), request, ctx);
  if (!image) {
    if (image.error().code == Status::Cancelled || image.error().code == Status::Limit) return image;
    if (!previews.empty()) {
      return preview_fallback(previews, std::format("The DNG RAW data could not be decoded ({})", image.error().message), ctx);
    }
    return image;
  }
  const std::string xmp(as_chars(t.bytes(ifd0, kXmp)));
  if (!xmp.empty() && xmp.size() < (1u << 20)) image->xmp = xmp;
  return image;
}

}  // namespace cvt::image_modern::raw
