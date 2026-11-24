// Convertible core - image_modern family: TIFF-based maker RAW formats.
//
// Canon CR2 (lossless JPEG slices; sensor borders and as-shot white balance
// from the maker note), Nikon NEF/NRW (uncompressed; compressed files fall
// back to the preview), Sony ARW (ARW2 "cRAW" and uncompressed; older
// variants fall back to the preview), Pentax PEF, Samsung SRW, Olympus ORF,
// Panasonic RW2/RWL and the other TIFF-EP based formats (uncompressed or
// lossless JPEG CFA data where present, otherwise the largest preview).
// Written from public descriptions of the containers (TIFF-EP, the EXIF
// maker-note layouts documented by the ExifTool tag tables, Laurent Clevy's
// CR2 description) and of the ARW2 block format.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#include "util/bytes.hpp"
#include "families/image_modern/raw_readers.hpp"
#include "util/text.hpp"

namespace cvt::image_modern::raw {

namespace {

enum : std::uint16_t {
  kNewSubFileType = 254, kImageWidth = 256, kImageLength = 257, kBitsPerSample = 258, kCompression = 259,
  kPhotometric = 262, kMake = 271, kModel = 272, kStripOffsets = 273, kOrientation = 274,
  kSamplesPerPixel = 277, kStripByteCounts = 279, kMakerNote = 0x927C, kDngVersion = 0xC612,
  kBlackLevel = 50714, kWhiteLevel = 50717, kCr2Slice = 0xC640,
};

bool has_extension(std::string_view filename, std::string_view ext) {
  if (filename.size() <= ext.size() + 1) return false;
  const std::string_view tail = filename.substr(filename.size() - ext.size());
  return filename[filename.size() - ext.size() - 1] == '.' && util::to_lower(tail) == ext;
}

bool starts_with_ci(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && util::to_lower(text.substr(0, prefix.size())) == util::to_lower(prefix);
}

bool maker_compression(std::uint64_t c) {
  return c == 34713 || c == 32767 || c == 32769 || c == 32770 || c == 32772 || c == 65535 || c == 34316 ||
         c == 65000;
}

// The RAW data directory of a TIFF-EP style file: CFA / LinearRaw
// photometry or a maker compression; the largest wins.
const Ifd* find_raw_ifd(const TiffTree& tree) {
  const TiffData& t = tree.tiff;
  const Ifd* best = nullptr;
  std::uint64_t best_pixels = 0;
  for (const Ifd& ifd : tree.ifds) {
    const std::uint64_t photometric = t.integer(ifd, kPhotometric);
    const std::uint64_t compression = t.integer(ifd, kCompression, 1);
    if (photometric != 32803 && photometric != 34892 && !maker_compression(compression)) continue;
    if ((t.integer(ifd, kNewSubFileType, 0) & 1) != 0) continue;
    const std::uint64_t pixels = t.integer(ifd, kImageWidth) * t.integer(ifd, kImageLength);
    if (best == nullptr || pixels > best_pixels) {
      best = &ifd;
      best_pixels = pixels;
    }
  }
  return best;
}

// The maker-note bytes (absolute position in the file and length).
struct MakerNote {
  std::uint64_t at = 0;
  std::uint64_t size = 0;
};
std::optional<MakerNote> maker_note(const TiffTree& tree) {
  if (!tree.exif) return std::nullopt;
  const Field* f = tree.exif->find(kMakerNote);
  if (f == nullptr || f->count < 8 || !tree.tiff.has(f->value_at, f->count)) return std::nullopt;
  return MakerNote{f->value_at, f->count};
}

std::string_view chars_at(const TiffData& t, std::uint64_t at, std::size_t n) {
  if (!t.has(at, n)) return {};
  return as_chars(t.data().subspan(static_cast<std::size_t>(at), n));
}

// ----------------------------------------------------------------------------- shared info

RawInfo base_info(const TiffTree& tree, const Ifd& raw) {
  const TiffData& t = tree.tiff;
  const Ifd& ifd0 = tree.ifds.front();
  RawInfo info;
  info.orientation = static_cast<int>(t.integer(ifd0, kOrientation, 1));
  info.make = t.text(ifd0, kMake);
  info.model = t.text(ifd0, kModel);
  info.cfa = tiff_cfa(t, raw);
  if (!info.cfa && tree.exif) info.cfa = tiff_cfa(t, *tree.exif);
  if (info.cfa) {
    for (std::uint32_t i = 0; i < info.cfa->width * info.cfa->height; ++i) {
      if (info.cfa->colors[i] > 2) info.cfa.reset();
      if (!info.cfa) break;
    }
  }
  const auto black = t.numbers(raw, kBlackLevel, 4);
  if (!black.empty()) {
    double sum = 0;
    for (const double b : black) sum += b;
    info.levels.black = {sum / static_cast<double>(black.size())};
  }
  info.levels.white = t.numbers(raw, kWhiteLevel, 1);
  if (info.levels.white.empty()) info.levels.estimate_white = true;
  return info;
}

Result<ir::Image> develop_or_preview(const TiffTree& tree, const Ifd& raw, RawInfo info, const Request& request,
                                     const std::vector<Preview>& previews, const Context& ctx, Packing packing,
                                     std::string_view format_name) {
  if (!info.cfa) {
    ctx.warn(std::format("The {} file does not describe its colour filter pattern; RGGB was assumed.", format_name));
    info.cfa = Cfa::bayer_from("RGGB");
  }
  auto sensor = read_samples(tree.tiff, raw, ctx, packing);
  if (!sensor) {
    if (sensor.error().code == Status::Cancelled || sensor.error().code == Status::Limit) return std::unexpected(sensor.error());
    return preview_fallback(previews, std::format("The {} RAW data could not be decoded ({})", format_name, sensor.error().message), ctx);
  }
  if (sensor->spp != 1) return preview_fallback(previews, std::format("The {} RAW layout is not supported", format_name), ctx);
  return develop(std::move(*sensor), info, request.develop, ctx);
}

// ----------------------------------------------------------------------------- Canon CR2

// WB_RGGBLevelsAsShot position (in shorts) inside ColorData, by ColorData size.
int canon_wb_index(std::uint32_t count) {
  switch (count) {
    case 582: return 25;
    case 653: return 24;
    case 796: case 674: case 692: case 702: case 1227: case 1250: case 1251: case 1337: case 1338: case 1346:
    case 1273: case 1275: case 1312: case 1313: case 1316: case 1506: case 1560: case 1592: case 1353: case 1602:
      return 63;
    case 5120: case 1816: case 1820: case 1824: return 71;
    case 2024: case 3656: return 85;
    case 3973: case 3778: return 105;
    default: return -1;
  }
}

Result<ir::Image> read_cr2(const TiffTree& tree, const Request& request, const std::vector<Preview>& previews,
                           const Context& ctx) {
  const TiffData& t = tree.tiff;
  // The RAW directory holds a lossless JPEG strip (old-style JPEG compression 6).
  const Ifd* raw = nullptr;
  ByteView stream;
  for (const Ifd& ifd : tree.ifds) {
    if (t.integer(ifd, kCompression, 0) != 6) continue;
    const auto offsets = t.integers(ifd, kStripOffsets, 1);
    const auto counts = t.integers(ifd, kStripByteCounts, 1);
    if (offsets.empty() || counts.empty()) continue;
    const ByteView data = t.range(offsets[0], counts[0]);
    const auto info = ljpeg_info(data);
    if (!info) continue;
    raw = &ifd;
    stream = data;
  }
  if (raw == nullptr) return preview_fallback(previews, "The CR2 file has no lossless RAW data", ctx);
  const auto slices = t.integers(*raw, kCr2Slice, 3);
  auto frame = decode_ljpeg(stream, ctx.limits.max_pixels * 2);
  if (!frame) {
    if (frame.error().code == Status::Limit) return std::unexpected(frame.error());
    return preview_fallback(previews, std::format("The CR2 RAW data could not be decoded ({})", frame.error().message), ctx);
  }
  auto sensor = unslice_cr2(*frame, slices);
  if (!sensor) return preview_fallback(previews, std::format("The CR2 RAW layout is not supported ({})", sensor.error().message), ctx);

  RawInfo info;
  const Ifd& ifd0 = tree.ifds.front();
  info.orientation = static_cast<int>(t.integer(ifd0, kOrientation, 1));
  info.make = t.text(ifd0, kMake);
  info.model = t.text(ifd0, kModel);
  info.cfa = Cfa::bayer_from("RGGB");
  info.levels.estimate_white = true;

  // Canon maker note: a plain IFD with file-relative offsets.
  std::uint32_t mask_left = 0, mask_right = 0;
  if (const auto note = maker_note(tree)) {
    if (const auto mn = t.parse_ifd(note->at, 2)) {
      const auto sensor_info = t.integers(*mn, 0x00E0, 17);
      if (sensor_info.size() >= 9) {
        const std::uint64_t left = sensor_info[5], top = sensor_info[6], right = sensor_info[7], bottom = sensor_info[8];
        if (left < right && top < bottom && right < sensor->width && bottom < sensor->height) {
          info.active = {static_cast<std::uint32_t>(top), static_cast<std::uint32_t>(left),
                         static_cast<std::uint32_t>(bottom + 1), static_cast<std::uint32_t>(right + 1)};
          if (sensor_info.size() >= 13 && sensor_info[11] > sensor_info[9] && sensor_info[11] < left) {
            mask_left = static_cast<std::uint32_t>(sensor_info[9]);
            mask_right = static_cast<std::uint32_t>(sensor_info[11]) + 1;
          } else if (left >= 8) {
            mask_left = 2;
            mask_right = static_cast<std::uint32_t>(left) - 2;
          }
        }
      }
      if (const Field* color = mn->find(0x4001)) {
        const int index = canon_wb_index(color->count);
        const auto levels = t.integers(*color, 200);
        if (index >= 0 && levels.size() >= static_cast<std::size_t>(index) + 4) {
          const auto r = static_cast<double>(levels[static_cast<std::size_t>(index)]);
          const auto g1 = static_cast<double>(levels[static_cast<std::size_t>(index) + 1]);
          const auto g2 = static_cast<double>(levels[static_cast<std::size_t>(index) + 2]);
          const auto b = static_cast<double>(levels[static_cast<std::size_t>(index) + 3]);
          const double g = 0.5 * (g1 + g2);
          if (r > 0 && b > 0 && g > 0 && std::fabs(g1 - g2) < 0.1 * g && r / g < 16 && b / g < 16 && g / r < 16 && g / b < 16) {
            info.color.wb_gains = {r / g, 1.0, b / g};
          }
        }
      }
    }
  }
  // Black level from the optical-black columns left of the active area.
  if (mask_right > mask_left && info.active[2] > info.active[0]) {
    double sum = 0;
    std::uint64_t n = 0;
    for (std::uint32_t y = info.active[0]; y < info.active[2]; ++y) {
      for (std::uint32_t x = mask_left; x < mask_right && x < sensor->width; ++x) {
        sum += sensor->u16[std::size_t{y} * sensor->width + x];
        ++n;
      }
    }
    if (n > 0) info.levels.black = {sum / static_cast<double>(n)};
  } else {
    ctx.warn("The CR2 file records no optical-black area; a black level of zero was assumed.");
  }
  return develop(std::move(*sensor), info, request.develop, ctx);
}

// ----------------------------------------------------------------------------- Nikon NEF / NRW

struct NikonNote {
  std::optional<TiffData> tiff;
  std::optional<Ifd> ifd;
};

NikonNote nikon_note(const TiffTree& tree) {
  NikonNote out;
  const auto note = maker_note(tree);
  if (!note) return out;
  const TiffData& t = tree.tiff;
  // "Nikon\0" + version (4 bytes) + embedded TIFF header.
  if (chars_at(t, note->at, 6) != std::string_view("Nikon\0", 6)) return out;
  const auto header = parse_header(t.data(), note->at + 10);
  if (!header) return out;
  out.tiff = TiffData(t.data(), note->at + 10, header->little);
  out.ifd = out.tiff->parse_ifd(header->first_ifd, 2);
  return out;
}

Result<ir::Image> read_nef(const TiffTree& tree, const Request& request, std::vector<Preview>& previews,
                           const Context& ctx) {
  const TiffData& t = tree.tiff;
  const NikonNote note = nikon_note(tree);
  if (note.ifd) {
    // PreviewIFD (0x0011): a JPEG with offsets relative to the maker note's TIFF header.
    if (const auto rel = note.tiff->integers(*note.ifd, 0x0011, 1); !rel.empty()) {
      if (const auto pv = note.tiff->parse_ifd(note.tiff->base() + rel[0], 3)) {
        const std::uint64_t at = note.tiff->integer(*pv, 0x0201);
        const std::uint64_t len = note.tiff->integer(*pv, 0x0202);
        if (at != 0 && len != 0) add_jpeg_preview(note.tiff->range(at, len), static_cast<int>(t.integer(tree.ifds.front(), kOrientation, 1)), previews);
      }
    }
  }
  if (request.preview) return decode_largest_preview(previews, ctx);
  const Ifd* raw = find_raw_ifd(tree);
  if (raw == nullptr) return preview_fallback(previews, "The NEF file has no RAW data directory", ctx);
  RawInfo info = base_info(tree, *raw);
  if (note.ifd) {
    const auto wb = note.tiff->numbers(*note.ifd, 0x000C, 4);
    if (wb.size() >= 2 && wb[0] > 0.05 && wb[1] > 0.05 && wb[0] < 20 && wb[1] < 20) info.color.wb_gains = {wb[0], 1.0, wb[1]};
    const auto black = note.tiff->numbers(*note.ifd, 0x003D, 4);
    if (black.size() == 4) info.levels.black = {(black[0] + black[1] + black[2] + black[3]) / 4.0};
  }
  const std::uint64_t compression = t.integer(*raw, kCompression, 1);
  if (compression == 34713) {
    // Uncompressed data is sometimes labelled 34713; real Nikon Huffman data is smaller.
    const std::uint64_t w = t.integer(*raw, kImageWidth), h = t.integer(*raw, kImageLength);
    const std::uint64_t bits = t.integer(*raw, kBitsPerSample, 12);
    std::uint64_t bytes = 0;
    for (const std::uint64_t c : t.integers(*raw, kStripByteCounts, 1u << 16)) bytes += c;
    if (bytes != w * h * 2 && bytes != (w * bits + 7) / 8 * h) {
      return preview_fallback(previews, "Nikon compressed NEF data is not supported", ctx);
    }
  }
  return develop_or_preview(tree, *raw, std::move(info), request, previews, ctx, Packing::Auto, "NEF");
}

// ----------------------------------------------------------------------------- Sony ARW

Result<ir::Image> read_arw(const TiffTree& tree, const Request& request, const std::vector<Preview>& previews,
                           const Context& ctx) {
  const TiffData& t = tree.tiff;
  const Ifd* raw = find_raw_ifd(tree);
  if (raw == nullptr) return preview_fallback(previews, "The ARW file has no RAW data directory", ctx);
  RawInfo info = base_info(tree, *raw);
  const std::uint64_t compression = t.integer(*raw, kCompression, 1);
  const std::uint64_t w = t.integer(*raw, kImageWidth), h = t.integer(*raw, kImageLength);
  if (compression == 32767) {
    const auto offsets = t.integers(*raw, kStripOffsets, 1);
    const auto counts = t.integers(*raw, kStripByteCounts, 1);
    if (offsets.empty() || counts.empty() || w == 0 || h == 0 || counts[0] != w * h || w % 32 != 0) {
      return preview_fallback(previews, "This Sony ARW variant is not supported", ctx);
    }
    const ByteView data = t.range(offsets[0], counts[0]);
    if (data.size() != counts[0]) return fail(Status::Decode, "the ARW RAW data is truncated");
    CVT_TRY(ctx.limits.check_pixels(w, h));
    const auto curve = t.integers(*raw, 0x7010, 4);
    CVT_TRY_ASSIGN(Sensor sensor, decode_arw2(data, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), curve, ctx));
    if (!info.cfa) info.cfa = Cfa::bayer_from("RGGB");
    if (info.levels.black.empty()) info.levels.black = {512.0};
    return develop(std::move(sensor), info, request.develop, ctx);
  }
  if (compression == 1 && info.levels.black.empty() && t.integer(*raw, kBitsPerSample, 0) == 14) info.levels.black = {512.0};
  return develop_or_preview(tree, *raw, std::move(info), request, previews, ctx, Packing::Auto, "ARW");
}

// ----------------------------------------------------------------------------- Olympus ORF

struct OlympusNote {
  std::optional<TiffData> tiff;  // offsets relative to the maker note (new) or the file (old)
  std::optional<Ifd> ifd;
};

OlympusNote olympus_note(const TiffTree& tree) {
  OlympusNote out;
  const auto note = maker_note(tree);
  if (!note) return out;
  const TiffData& t = tree.tiff;
  if (chars_at(t, note->at, 8) == std::string_view("OLYMPUS\0", 8) || chars_at(t, note->at, 8) == std::string_view("OM SYSTE", 8)) {
    const std::uint64_t header = chars_at(t, note->at, 8) == std::string_view("OM SYSTE", 8) ? note->at + 12 : note->at + 8;
    const std::string_view order = chars_at(t, header, 2);
    if (order != "II" && order != "MM") return out;
    out.tiff = TiffData(t.data(), note->at, order == "II");
    out.ifd = out.tiff->parse_ifd(header + 4, 2);
  } else if (chars_at(t, note->at, 6) == std::string_view("OLYMP\0", 6)) {
    out.tiff = t;
    out.ifd = t.parse_ifd(note->at + 8, 2);
  }
  return out;
}

std::optional<Ifd> olympus_sub(const OlympusNote& note, std::uint16_t tag) {
  if (!note.ifd) return std::nullopt;
  const auto rel = note.tiff->integers(*note.ifd, tag, 1);
  if (rel.empty()) return std::nullopt;
  return note.tiff->parse_ifd(note.tiff->base() + rel[0], 3);
}

Result<ir::Image> read_orf(const TiffTree& tree, const Request& request, std::vector<Preview>& previews,
                           const Context& ctx) {
  const TiffData& t = tree.tiff;
  const OlympusNote note = olympus_note(tree);
  const int orientation = static_cast<int>(t.integer(tree.ifds.front(), kOrientation, 1));
  if (const auto settings = olympus_sub(note, 0x2020)) {
    const std::uint64_t at = note.tiff->integer(*settings, 0x0101);
    const std::uint64_t len = note.tiff->integer(*settings, 0x0102);
    if (at != 0 && len != 0) add_jpeg_preview(note.tiff->range(at, len), orientation, previews);
  }
  if (request.preview) return decode_largest_preview(previews, ctx);
  const Ifd& raw = tree.ifds.front();
  const std::uint64_t w = t.integer(raw, kImageWidth), h = t.integer(raw, kImageLength);
  std::uint64_t bytes = 0;
  for (const std::uint64_t c : t.integers(raw, kStripByteCounts, 1u << 16)) bytes += c;
  if (w == 0 || h == 0 || bytes < w * h * 2) {
    return preview_fallback(previews, "Olympus compressed and packed ORF data is not supported", ctx);
  }
  RawInfo info = base_info(tree, raw);
  if (const auto processing = olympus_sub(note, 0x2040)) {
    const auto wb = note.tiff->numbers(*processing, 0x0100, 2);
    if (wb.size() == 2 && wb[0] > 0 && wb[1] > 0) info.color.wb_gains = {wb[0] / 256.0, 1.0, wb[1] / 256.0};
    const auto black = note.tiff->numbers(*processing, 0x0600, 4);
    if (black.size() == 4) info.levels.black = {(black[0] + black[1] + black[2] + black[3]) / 4.0};
  }
  return develop_or_preview(tree, raw, std::move(info), request, previews, ctx, Packing::Container16, "ORF");
}

// ----------------------------------------------------------------------------- generic

Result<ir::Image> read_generic(std::string_view name, const TiffTree& tree, const Request& request,
                               const std::vector<Preview>& previews, const Context& ctx) {
  if (request.preview) return decode_largest_preview(previews, ctx);
  const TiffData& t = tree.tiff;
  const Ifd* raw = find_raw_ifd(tree);
  if (raw == nullptr) return preview_fallback(previews, std::format("The {} file has no supported RAW data", name), ctx);
  const std::uint64_t compression = t.integer(*raw, kCompression, 1);
  if (compression != 1 && compression != 7) {
    return preview_fallback(previews, std::format("{} RAW compression {} is not supported", name, compression), ctx);
  }
  RawInfo info = base_info(tree, *raw);
  return develop_or_preview(tree, *raw, std::move(info), request, previews, ctx, Packing::Auto, name);
}

std::string upper(std::string_view id) {
  std::string out(id);
  for (char& c : out) c = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
  return out;
}

}  // namespace

// ----------------------------------------------------------------------------- shared decoders

Result<Sensor> unslice_cr2(const LjpegFrame& frame, std::span<const std::uint64_t> slices) {
  const std::uint64_t total = frame.samples.size();
  std::uint64_t width = std::uint64_t{frame.width} * static_cast<std::uint64_t>(frame.components);
  std::uint64_t n = 0, slice_w = 0, last_w = 0;
  if (slices.size() == 3 && slices[1] > 0 && slices[2] > 0 && slices[0] < 1024) {
    n = slices[0];
    slice_w = slices[1];
    last_w = slices[2];
    width = n * slice_w + last_w;
  }
  if (width == 0 || total % width != 0) return fail(Status::Decode, "CR2 slice layout does not match the image");
  const std::uint64_t height = total / width;
  if (width > 0xFFFFFFFFu || height > 0xFFFFFFFFu) return fail(Status::Decode, "CR2 image is too large");
  Sensor s;
  s.width = static_cast<std::uint32_t>(width);
  s.height = static_cast<std::uint32_t>(height);
  s.bits = frame.precision;
  if (n == 0) {
    s.u16 = frame.samples;
    return s;
  }
  s.u16.resize(static_cast<std::size_t>(total));
  const std::uint16_t* in = frame.samples.data();
  std::uint64_t k = 0;
  for (std::uint64_t slice = 0; slice <= n; ++slice) {
    const std::uint64_t sw = slice < n ? slice_w : last_w;
    const std::uint64_t x0 = slice * slice_w;
    for (std::uint64_t y = 0; y < height; ++y) {
      ::cvt::util::copy_bytes(&s.u16[static_cast<std::size_t>(y * width + x0)], in + k, static_cast<std::size_t>(sw) * sizeof(std::uint16_t));
      k += sw;
    }
  }
  return s;
}

Result<Sensor> decode_arw2(ByteView data, std::uint32_t width, std::uint32_t height,
                           std::span<const std::uint64_t> curve_points, const Context& ctx) {
  if (width % 32 != 0 || data.size() < std::uint64_t{width} * height) return fail(Status::Decode, "invalid ARW2 data size");
  // Tone curve: a piecewise linear expansion whose slope doubles at each of
  // the four knots (given in units of 4, 12-bit domain).
  std::vector<std::uint16_t> curve(4096);
  std::array<std::uint32_t, 6> knots = {0, 0, 0, 0, 0, 4095};
  if (curve_points.size() == 4) {
    for (std::size_t i = 0; i < 4; ++i) knots[i + 1] = static_cast<std::uint32_t>((curve_points[i] >> 2) & 0xFFF);
  } else {
    knots = {0, 4095, 4095, 4095, 4095, 4095};
  }
  for (std::size_t i = 1; i < knots.size(); ++i) knots[i] = std::max(knots[i], knots[i - 1]);
  std::uint32_t value = 0;
  curve[0] = 0;
  for (std::uint32_t j = 1; j < 4096; ++j) {
    std::uint32_t segment = 0;
    while (segment < 4 && j > knots[segment + 1]) ++segment;
    value += 1u << segment;
    curve[j] = static_cast<std::uint16_t>(std::min<std::uint32_t>(value, 0xFFFF));
  }

  Sensor s;
  s.width = width;
  s.height = height;
  s.bits = 14;
  s.u16.resize(std::size_t{width} * height);
  const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
  std::array<std::uint32_t, 16> pix{};
  for (std::uint32_t y = 0; y < height; ++y) {
    if ((y & 127) == 0) CVT_TRY(ctx.checkpoint(0.5 * y / height, "Decoding ARW2"));
    const std::uint8_t* row = p + std::size_t{y} * width;
    std::uint16_t* out = s.u16.data() + std::size_t{y} * width;
    for (std::uint32_t block = 0; block < width / 16; ++block) {
      const std::uint8_t* b = row + std::size_t{block} * 16;
      std::uint64_t lo = 0, hi = 0;
      for (int i = 7; i >= 0; --i) {
        lo = (lo << 8) | b[i];
        hi = (hi << 8) | b[8 + i];
      }
      const auto field = [&](int at, int bits) -> std::uint32_t {
        std::uint64_t v = 0;
        if (at >= 128) return 0;  // corrupt block (min and max at the same index)
        if (at >= 64) v = hi >> (at - 64);
        else if (at + bits <= 64) v = lo >> at;
        else v = (lo >> at) | (hi << (64 - at));
        return static_cast<std::uint32_t>(v & ((1u << bits) - 1u));
      };
      const std::uint32_t max = field(0, 11);
      const std::uint32_t min = field(11, 11);
      const std::uint32_t imax = field(22, 4);
      const std::uint32_t imin = field(26, 4);
      int shift = 0;
      while (shift < 4 && (0x80u << shift) <= (max > min ? max - min : 0)) ++shift;
      int at = 30;
      for (std::uint32_t i = 0; i < 16; ++i) {
        if (i == imax) {
          pix[i] = max;
        } else if (i == imin) {
          pix[i] = min;
        } else {
          pix[i] = std::min<std::uint32_t>((field(at, 7) << shift) + min, 0x7FF);
          at += 7;
        }
      }
      // Block 2k covers the even pixels of a 32-pixel span, block 2k+1 the odd ones.
      const std::uint32_t span = (block / 2) * 32 + (block % 2);
      for (std::uint32_t i = 0; i < 16; ++i) out[span + i * 2] = curve[pix[i] << 1];
    }
  }
  return s;
}

// ----------------------------------------------------------------------------- identification

Identified identify(ByteView data, std::string_view filename) {
  Identified out;
  if (data.size() < 16) return out;
  const std::string_view head = as_chars(data.first(16));
  if (head == "FUJIFILMCCD-RAW ") return {"raf", true};
  if (data.size() >= 14 && head.substr(0, 2) == "II" && as_chars(data.subspan(6, 8)) == "HEAPCCDR") return {"crw", true};
  if (head.substr(4, 8) == "ftypcrx ") return {"cr3", true};
  const auto tree = parse_tree(data, 0, 16);
  if (!tree) return out;
  const TiffData& t = tree->tiff;
  if (tree->magic == 0x55) return {has_extension(filename, "rwl") ? "rwl" : "rw2", true};
  if (tree->magic == 0x4F52 || tree->magic == 0x5352) return {"orf", true};
  if (tree->magic != 42) return out;
  if (head.substr(8, 3) == std::string_view("CR\x02", 3)) return {"cr2", true};
  const Ifd& ifd0 = tree->ifds.front();
  if (ifd0.has(kDngVersion)) return {has_extension(filename, "gpr") ? "gpr" : "dng", true};
  const std::string make = t.text(ifd0, kMake);
  if (make.empty()) return out;
  struct Mapping {
    std::string_view make, id, alt_id;
  };
  static constexpr Mapping makes[] = {
      {"NIKON", "nef", "nrw"},         {"SONY", "arw", "sr2"},       {"PENTAX", "pef", ""},
      {"RICOH IMAGING", "pef", ""},    {"SAMSUNG", "srw", ""},       {"Hasselblad", "3fr", "fff"},
      {"EPSON", "erf", ""},            {"Kodak", "dcr", "kdc"},      {"EASTMAN KODAK", "dcr", "kdc"},
      {"Mamiya", "mef", ""},           {"Leaf", "mos", ""},          {"Phase One", "iiq", ""},
  };
  for (const Mapping& m : makes) {
    if (!starts_with_ci(make, m.make)) continue;
    out.id = std::string(m.id);
    if (!m.alt_id.empty() && has_extension(filename, m.alt_id)) out.id = std::string(m.alt_id);
    if (m.id == "arw" && has_extension(filename, "srf")) out.id = "srf";
    out.certain = find_raw_ifd(*tree) != nullptr;
    return out;
  }
  return out;
}

namespace detail {
Result<ir::Image> read_raf(ByteView data, const Request& request, const Context& ctx);
Result<ir::Image> read_cr3(ByteView data, const Request& request, const Context& ctx);
Result<ir::Image> read_crw(ByteView data, const Request& request, const Context& ctx);
}  // namespace detail

Result<ir::Image> read_maker(std::string_view id, ByteView data, const Request& request, const Context& ctx) {
  if (id == "raf") return detail::read_raf(data, request, ctx);
  if (id == "cr3") return detail::read_cr3(data, request, ctx);
  if (id == "crw") return detail::read_crw(data, request, ctx);
  const auto tree = parse_tree(data);
  if (!tree) return fail(Status::Decode, "not a {} file (no TIFF structure)", upper(id));
  const int orientation = static_cast<int>(tree->tiff.integer(tree->ifds.front(), kOrientation, 1));
  std::vector<Preview> previews;
  collect_tiff_previews(*tree, orientation, previews);
  if (tree->magic == 0x55) {
    // Panasonic: the full-size JPEG is stored in tag 0x2E of IFD0.
    add_jpeg_preview(tree->tiff.bytes(tree->ifds.front(), 0x002E), orientation, previews);
  }
  if (tree->ifds.front().has(kDngVersion)) return read_dng(*tree, request, ctx);
  if (id == "cr2") {
    if (request.preview) return decode_largest_preview(previews, ctx);
    return read_cr2(*tree, request, previews, ctx);
  }
  if (id == "nef" || id == "nrw") return read_nef(*tree, request, previews, ctx);
  if (id == "arw" || id == "sr2" || id == "srf") {
    if (request.preview) return decode_largest_preview(previews, ctx);
    return read_arw(*tree, request, previews, ctx);
  }
  if (id == "orf") return read_orf(*tree, request, previews, ctx);
  if (id == "rw2" || id == "rwl") {
    if (request.preview) return decode_largest_preview(previews, ctx);
    return preview_fallback(previews, "Panasonic RAW data is not supported", ctx);
  }
  return read_generic(upper(id), *tree, request, previews, ctx);
}

}  // namespace cvt::image_modern::raw
