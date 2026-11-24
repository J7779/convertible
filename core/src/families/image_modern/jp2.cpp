// Convertible core - image_modern family: JPEG 2000 (ISO/IEC 15444-1 JP2 files
// and raw codestreams, 15444-2 JPX files read through the same box parser).
//
// The container (boxes, palette, channel definitions, colour specification,
// resolution, EXIF/XMP) is handled in-house; the wavelet codestream itself
// goes through the imported OpenJPEG library (jp2_opj.cpp).
//
// Reading, from decoded components to `ir::Image`:
// - pclr/cmap palettes are applied per pixel; cdef assigns colour and opacity
//   (premultiplied opacity is un-premultiplied); without cdef the channels
//   after the colour channels are taken as alpha (the first one only).
// - Colour: enumerated sRGB, greyscale, sYCC, e-sYCC and CMYK are converted
//   to sRGB; an ICC profile (restricted or any) is kept verbatim in
//   `icc_profile` with the samples unchanged. Raw codestreams have no colour
//   box: one or two components are grey (+ alpha), three or more RGB(A),
//   except that three components whose 2nd and 3rd are sub-sampled are taken
//   as sYCC (the usual 4:2:x layout). Sub-sampled components are expanded by
//   sample replication.
// - Precision: every sample is mapped linearly onto the full 8-bit range
//   (`v * 255 / (2^p - 1)`). Signed components are offset by 2^(p-1) first,
//   so the most negative value maps to 0 and zero to mid-grey. For p > 8 the
//   image also carries `hdr`, built exactly like a 16-bit PNG: each sample is
//   scaled to 16 bits (`round(u * 65535 / (2^p - 1))`, u = offset sample) and
//   interpreted as sRGB-encoded, so `linear_to_srgb16(hdr)` gives the 16-bit
//   values back exactly, and from them `round(v16 * (2^p - 1) / 65535)` (minus
//   2^(p-1) for signed data) recovers the original samples. `source_bit_depth`
//   is the largest channel precision (at most 16); `source_grayscale` is set
//   for grey sources. Callers that need raw medical or scientific values
//   (DICOM, PDF) can therefore get them back without precision loss.
// - Resolution boxes (capture preferred over display) set dpi.
//
// Writing: 1 (grey), 2 (grey + alpha), 3 (RGB, with the RCT/ICT colour
// transform) or 4 (RGBA) components at 8 bits, or at the source precision
// (9..16 bits) from `hdr` when the source had more than 8 bits. Lossless uses
// the reversible 5/3 wavelet; lossy uses the 9/7 wavelet with PSNR-targeted
// rate allocation: quality 90 aims at 42 dB, each step of 5 quality points is
// one dB. Options add quality layers, tiles and the progression order.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/image/common.hpp"
#include "families/image/image_ops.hpp"
#include "families/image_modern/jp2.hpp"
#include "families/image_modern/jp2_codec.hpp"
#include "util/bytes.hpp"

namespace cvt::image_modern {

namespace {

using image::check_size;
using namespace std::string_view_literals;

enum class Space { Unknown, Grey, Rgb, Ycc, Eycc, Cmyk };

/// One channel of the output: a decoded component, possibly mapped through
/// a palette column.
struct Channel {
  const jp2::Plane* plane = nullptr;
  const jp2::Palette* palette = nullptr;
  std::uint32_t column = 0;
  std::uint32_t precision = 8;
  bool is_signed = false;
  std::vector<std::uint32_t> column_of_x;  ///< plane sample column for each output x

  [[nodiscard]] std::uint64_t max_value() const noexcept { return (std::uint64_t{1} << precision) - 1; }
};

/// Sample of `channel` at output column x of plane row `row`, offset to
/// unsigned (0 .. 2^p - 1).
inline std::uint64_t unsigned_sample(const Channel& c, const std::int32_t* row, std::uint32_t x) noexcept {
  std::int64_t v = row[c.column_of_x[x]];
  if (c.palette != nullptr) {
    const std::int64_t index = std::clamp<std::int64_t>(v, 0, std::int64_t{c.palette->entries} - 1);
    v = c.palette->values[static_cast<std::size_t>(index) * c.palette->columns.size() + c.column];
  }
  if (c.is_signed) v += std::int64_t{1} << (c.precision - 1);
  const auto max = static_cast<std::int64_t>(c.max_value());
  return static_cast<std::uint64_t>(std::clamp<std::int64_t>(v, 0, max));
}

inline std::uint16_t to16(std::uint64_t u, std::uint64_t max) noexcept {
  return static_cast<std::uint16_t>((u * 65535u + max / 2u) / max);
}

inline std::uint16_t unit_to16(double v) noexcept {
  return static_cast<std::uint16_t>(std::lround(std::clamp(v, 0.0, 1.0) * 65535.0));
}

inline std::uint8_t to8(std::uint16_t v) noexcept { return static_cast<std::uint8_t>((v * 255u + 32767u) / 65535u); }

Space space_from_enum(std::uint32_t cs, const Context& ctx) {
  switch (cs) {
    case 16: return Space::Rgb;
    case 17: return Space::Grey;
    case 18: return Space::Ycc;
    case 24: return Space::Eycc;
    case 12: return Space::Cmyk;
    case 20:
    case 21:
      ctx.warn("The JPEG 2000 colour space (e-sRGB / ROMM-RGB) was treated as sRGB.");
      return Space::Rgb;
    case 14: ctx.warn("CIELab JPEG 2000 colour is not converted; the samples are shown as they are."); break;
    default: ctx.warn(std::format("JPEG 2000 colour space {} is not supported; the samples are shown as they are.", cs));
  }
  return Space::Unknown;
}

Space space_from_icc(ByteView icc) {
  if (icc.size() < 20) return Space::Unknown;
  const std::string_view kind = as_chars(icc.subspan(16, 4));
  if (kind == "GRAY") return Space::Grey;
  if (kind == "RGB ") return Space::Rgb;
  if (kind == "CMYK") return Space::Cmyk;
  return Space::Unknown;
}

std::size_t colour_count(Space space, std::size_t channels) {
  switch (space) {
    case Space::Grey: return 1;
    case Space::Rgb:
    case Space::Ycc:
    case Space::Eycc: return 3;
    case Space::Cmyk: return 4;
    case Space::Unknown: break;
  }
  return channels >= 3 ? 3 : 1;
}

/// The planes and file boxes turned into an RGBA picture.
Result<ir::Image> assemble(const jp2::DecodedPlanes& decoded, const jp2::FileInfo* file, const Context& ctx) {
  const std::uint32_t width = decoded.x1 - decoded.x0;
  const std::uint32_t height = decoded.y1 - decoded.y0;

  // --- channels (palette mapping)
  std::vector<Channel> channels;
  const bool use_palette = file != nullptr && file->palette && !file->mapping.empty();
  if (file != nullptr && file->palette && file->mapping.empty()) {
    ctx.warn("The JPEG 2000 palette has no component mapping and was ignored.");
  }
  if (file != nullptr && !file->mapping.empty()) {
    for (const jp2::ComponentMapping& m : file->mapping) {
      if (m.component >= decoded.planes.size()) {
        return fail(Status::Decode, "the JPEG 2000 component mapping refers to a missing component");
      }
      Channel c;
      c.plane = &decoded.planes[m.component];
      if (m.type == 1) {
        if (!use_palette || m.column >= file->palette->columns.size()) {
          return fail(Status::Decode, "the JPEG 2000 component mapping refers to a missing palette column");
        }
        c.palette = &*file->palette;
        c.column = m.column;
        c.precision = file->palette->columns[m.column].precision;
        c.is_signed = file->palette->columns[m.column].is_signed;
      } else {
        c.precision = c.plane->precision;
        c.is_signed = c.plane->is_signed;
      }
      channels.push_back(std::move(c));
    }
  } else {
    for (const jp2::Plane& plane : decoded.planes) {
      Channel c;
      c.plane = &plane;
      c.precision = plane.precision;
      c.is_signed = plane.is_signed;
      channels.push_back(std::move(c));
    }
  }
  if (channels.empty()) return fail(Status::Decode, "the JPEG 2000 image has no channels");
  for (Channel& c : channels) c.precision = std::clamp<std::uint32_t>(c.precision, 1, 38);

  // --- colour space
  Space space = Space::Unknown;
  ir::Image image;
  if (file != nullptr) {
    for (const jp2::ColourSpec& colour : file->colours) {
      if (colour.method == 1) {
        space = space_from_enum(colour.enumerated, ctx);
        break;
      }
      if ((colour.method == 2 || colour.method == 3) && !colour.icc.empty()) {
        image.icc_profile = colour.icc;
        space = space_from_icc(colour.icc);
        break;
      }
    }
  }
  if (space == Space::Unknown && channels.size() >= 3 && channels[0].palette == nullptr) {
    const jp2::Plane& p0 = *channels[0].plane;
    const jp2::Plane& p1 = *channels[1].plane;
    const jp2::Plane& p2 = *channels[2].plane;
    if (p0.dx == 1 && p0.dy == 1 && (p1.dx > 1 || p1.dy > 1) && (p2.dx > 1 || p2.dy > 1)) space = Space::Ycc;
  }
  std::size_t colours = colour_count(space, channels.size());
  if (colours > channels.size()) {
    if (space != Space::Unknown) ctx.warn("The JPEG 2000 file has fewer components than its colour space needs.");
    space = Space::Unknown;
    colours = colour_count(space, channels.size());
    if (colours > channels.size()) colours = 1;
  }
  if (space == Space::Unknown) space = colours == 1 ? Space::Grey : Space::Rgb;

  // --- channel roles
  std::array<int, 4> colour_index = {-1, -1, -1, -1};
  int alpha_index = -1;
  bool premultiplied = false;
  if (file != nullptr && !file->channels.empty()) {
    for (const jp2::ChannelDefinition& d : file->channels) {
      if (d.channel >= channels.size()) continue;
      if (d.type == 0 && d.association >= 1 && d.association <= colours) {
        colour_index[d.association - 1u] = d.channel;
      } else if ((d.type == 1 || d.type == 2) && alpha_index < 0) {
        alpha_index = d.channel;
        premultiplied = d.type == 2;
      }
    }
    for (std::size_t i = 0; i < colours; ++i) {
      if (colour_index[i] < 0) {
        ctx.warn("The JPEG 2000 channel definitions are incomplete; the component order was used instead.");
        colour_index = {0, 1, 2, 3};
        break;
      }
    }
  } else {
    for (std::size_t i = 0; i < colours; ++i) colour_index[i] = static_cast<int>(i);
    if (channels.size() > colours) alpha_index = static_cast<int>(colours);
  }
  std::size_t used = colours + (alpha_index >= 0 ? 1u : 0u);
  if (channels.size() > used) {
    ctx.warn(std::format("The JPEG 2000 image has {} components; {} extra ones were ignored.", channels.size(),
                         channels.size() - used));
  }

  // --- precomputed sample columns
  std::uint32_t precision = 1;
  const auto prepare = [&](int index) {
    if (index < 0) return;
    Channel& c = channels[static_cast<std::size_t>(index)];
    if (!c.column_of_x.empty()) return;
    c.column_of_x.resize(width);
    for (std::uint32_t x = 0; x < width; ++x) {
      const std::uint64_t gx = std::uint64_t{decoded.x0} + x;
      const std::int64_t s = static_cast<std::int64_t>(gx / c.plane->dx) - c.plane->x0;
      c.column_of_x[x] = static_cast<std::uint32_t>(std::clamp<std::int64_t>(s, 0, c.plane->width - 1));
    }
    precision = std::max(precision, c.precision);
  };
  for (std::size_t i = 0; i < colours; ++i) prepare(colour_index[i]);
  prepare(alpha_index);
  const bool wide = precision > 8;

  CVT_TRY_ASSIGN(ir::Frame frame, image::new_frame(ctx, width, height, {0, 0, 0, 255}));
  std::vector<std::uint16_t> wide_rgba;
  if (wide) {
    CVT_TRY(ctx.limits.check_decompressed(std::uint64_t{width} * height * 24));
    wide_rgba.resize(std::size_t{width} * height * 4);
  }

  const auto row_of = [&](int index, std::uint32_t y) -> const std::int32_t* {
    const Channel& c = channels[static_cast<std::size_t>(index)];
    const std::uint64_t gy = std::uint64_t{decoded.y0} + y;
    const std::int64_t s = static_cast<std::int64_t>(gy / c.plane->dy) - c.plane->y0;
    const auto row = static_cast<std::size_t>(std::clamp<std::int64_t>(s, 0, c.plane->height - 1));
    return c.plane->data + row * c.plane->width;
  };
  const auto unit = [&](int index, const std::int32_t* row, std::uint32_t x) {
    const Channel& c = channels[static_cast<std::size_t>(index)];
    return static_cast<double>(unsigned_sample(c, row, x)) / static_cast<double>(c.max_value());
  };
  const auto chroma = [&](int index, const std::int32_t* row, std::uint32_t x) {
    const Channel& c = channels[static_cast<std::size_t>(index)];
    const double half = static_cast<double>(std::uint64_t{1} << (c.precision - 1));
    return (static_cast<double>(unsigned_sample(c, row, x)) - half) / static_cast<double>(c.max_value());
  };

  for (std::uint32_t y = 0; y < height; ++y) {
    if ((y & 31) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(y) / height));
    std::array<const std::int32_t*, 4> rows{};
    for (std::size_t i = 0; i < colours; ++i) rows[i] = row_of(colour_index[i], y);
    const std::int32_t* alpha_row = alpha_index >= 0 ? row_of(alpha_index, y) : nullptr;
    for (std::uint32_t x = 0; x < width; ++x) {
      std::array<std::uint16_t, 4> px = {0, 0, 0, 65535};
      switch (space) {
        case Space::Grey: {
          const Channel& c = channels[static_cast<std::size_t>(colour_index[0])];
          px[0] = px[1] = px[2] = to16(unsigned_sample(c, rows[0], x), c.max_value());
          break;
        }
        case Space::Rgb:
        case Space::Unknown:
          for (std::size_t i = 0; i < 3; ++i) {
            const Channel& c = channels[static_cast<std::size_t>(colour_index[i])];
            px[i] = to16(unsigned_sample(c, rows[i], x), c.max_value());
          }
          break;
        case Space::Ycc: {
          const double yy = unit(colour_index[0], rows[0], x);
          const double cb = chroma(colour_index[1], rows[1], x);
          const double cr = chroma(colour_index[2], rows[2], x);
          px[0] = unit_to16(yy + 1.402 * cr);
          px[1] = unit_to16(yy - 0.344136 * cb - 0.714136 * cr);
          px[2] = unit_to16(yy + 1.772 * cb);
          break;
        }
        case Space::Eycc: {
          const double yy = unit(colour_index[0], rows[0], x);
          const double cb = chroma(colour_index[1], rows[1], x);
          const double cr = chroma(colour_index[2], rows[2], x);
          px[0] = unit_to16(yy - 0.0000368 * cb + 1.40199 * cr);
          px[1] = unit_to16(1.0003 * yy - 0.344125 * cb - 0.7141128 * cr);
          px[2] = unit_to16(0.999823 * yy + 1.77204 * cb - 0.000008 * cr);
          break;
        }
        case Space::Cmyk: {
          const double k = 1.0 - unit(colour_index[3], rows[3], x);
          for (std::size_t i = 0; i < 3; ++i) px[i] = unit_to16((1.0 - unit(colour_index[i], rows[i], x)) * k);
          break;
        }
      }
      if (alpha_row != nullptr) {
        const Channel& c = channels[static_cast<std::size_t>(alpha_index)];
        px[3] = to16(unsigned_sample(c, alpha_row, x), c.max_value());
        if (premultiplied) {
          for (std::size_t i = 0; i < 3; ++i) {
            px[i] = px[3] == 0 ? std::uint16_t{0}
                               : static_cast<std::uint16_t>(std::min<std::uint32_t>(
                                     65535u, (std::uint32_t{px[i]} * 65535u + px[3] / 2u) / px[3]));
          }
        }
      }
      const std::size_t at = frame.offset(x, y);
      for (std::size_t i = 0; i < 4; ++i) frame.rgba[at + i] = to8(px[i]);
      if (wide) std::copy(px.begin(), px.end(), wide_rgba.begin() + static_cast<std::ptrdiff_t>(at));
    }
  }

  image.frames.push_back(std::move(frame));
  if (wide) image.hdr = image::linear_from_srgb16(wide_rgba, width, height);
  image.source_bit_depth = static_cast<std::uint8_t>(std::min<std::uint32_t>(precision, 16));
  image.source_grayscale = space == Space::Grey;
  return image;
}

Result<ir::Image> decode_jpeg2000(ByteView data, const Context& ctx) {
  std::optional<jp2::FileInfo> file;
  ByteView codestream;
  if (jp2::has_codestream_signature(data)) {
    codestream = data;
  } else if (jp2::has_jp2_signature(data)) {
    CVT_TRY_ASSIGN(file, jp2::parse_file(data, ctx));
    codestream = file->codestream;
  } else {
    return fail(Status::Decode, "not a JPEG 2000 file or codestream");
  }
  CVT_TRY_ASSIGN(const jp2::SizInfo siz, jp2::parse_siz(codestream));
  CVT_TRY(check_size(ctx, siz.width(), siz.height()));
  if (file && file->has_ihdr && (file->ihdr_width != siz.width() || file->ihdr_height != siz.height())) {
    ctx.warn("The JP2 image header disagrees with the codestream; the codestream size was used.");
  }
  // OpenJPEG holds every component as 32-bit samples for the whole image and
  // again in tile buffers while decoding: check before it allocates.
  CVT_TRY(ctx.limits.check_decompressed(siz.samples() * 8));
  for (const jp2::SizComponent& c : siz.components) {
    if (c.precision > 31) return fail(Status::Decode, "{}-bit JPEG 2000 components are not supported", c.precision);
  }

  Progress whole = ctx.progress;
  Context sub = ctx;
  sub.progress = whole.sub(0.0, 0.8);
  CVT_TRY_ASSIGN(const jp2::DecodedPlanes planes, jp2::decode_codestream(codestream, siz, sub));
  sub.progress = whole.sub(0.8, 1.0);
  CVT_TRY_ASSIGN(ir::Image image, assemble(planes, file ? &*file : nullptr, sub));

  if (file) {
    image.dpi_x = file->dpi_x;
    image.dpi_y = file->dpi_y;
    if (!file->xmp.empty()) image.xmp = file->xmp;
    if (!file->exif.empty()) image::attach_exif(image, file->exif);
  }
  return image;
}

Result<ir::Image> read_jpeg2000(ReadContext& ctx) { return decode_jpeg2000(ctx.input, ctx); }

// --------------------------------------------------------------------------- writing

enum class Container { Codestream, Jp2, Jpx };

/// Final-layer PSNR for a quality value: 90 -> 42 dB, one dB per 5 points.
double psnr_for_quality(int quality) { return 24.0 + 0.2 * std::clamp(quality, 1, 100); }

jp2::Progression progression_from(std::string_view name) {
  if (name == "RLCP") return jp2::Progression::RLCP;
  if (name == "RPCL") return jp2::Progression::RPCL;
  if (name == "PCRL") return jp2::Progression::PCRL;
  if (name == "CPRL") return jp2::Progression::CPRL;
  return jp2::Progression::LRCP;
}

Result<Bytes> write_jpeg2000(const ir::Image& source, WriteContext& ctx, Container container) {
  CVT_TRY_ASSIGN(const image::WriteImage prepared, image::prepare_write(source, ctx));
  const ir::Image& picture = *prepared;
  image::warn_if_animated(picture, ctx, "JPEG 2000");
  const ir::Frame& frame = picture.frames.front();

  const std::string depth = ctx.options.get_enum("bit_depth", "auto", {"auto", "8", "16"});
  const bool hdr_usable = picture.hdr && picture.hdr->width == frame.width && picture.hdr->height == frame.height;
  const bool wide = hdr_usable && (depth == "16" || (depth == "auto" && picture.source_bit_depth > 8 &&
                                                       picture.source_bit_depth <= 16));
  const std::uint32_t precision = !wide ? 8u : depth == "16" ? 16u : picture.source_bit_depth;
  std::vector<std::uint16_t> samples16;
  if (wide) samples16 = image::srgb16_from_linear(*picture.hdr);

  const bool keep = prepared.keep_metadata();
  const Bytes icc = keep ? picture.icc_profile : Bytes{};
  const Space icc_space = space_from_icc(icc);
  bool grey = icc_space == Space::Grey || (icc_space != Space::Rgb && image::is_grayscale(frame));
  if (grey && wide) {
    for (std::size_t i = 0; i + 3 < samples16.size() && grey; i += 4) {
      grey = samples16[i] == samples16[i + 1] && samples16[i] == samples16[i + 2];
    }
  }
  if (icc_space == Space::Cmyk || (icc_space == Space::Unknown && !icc.empty())) {
    // A profile for another colour model would misdescribe RGB samples.
    ctx.warn("The ICC profile does not describe RGB or grey data and was not written.");
  }
  const bool write_icc = !icc.empty() && (icc_space == Space::Rgb || icc_space == Space::Grey);
  const bool alpha = !image::is_opaque(frame);
  const std::uint32_t colours = grey ? 1u : 3u;
  const std::uint32_t components = colours + (alpha ? 1u : 0u);

  jp2::EncodeParams params;
  params.width = frame.width;
  params.height = frame.height;
  params.components = components;
  params.precision = precision;
  params.lossless = options::resolved_lossless(ctx.options);
  params.psnr = psnr_for_quality(options::resolved_quality(ctx.options));
  params.layers = static_cast<int>(ctx.options.get_int("layers", 1, 1, 20));
  params.tile_size = static_cast<std::uint32_t>(ctx.options.get_int("tile_size", 0, 0, 65535));
  if (params.tile_size > 0 && params.tile_size < 16) params.tile_size = 16;
  params.progression = progression_from(ctx.options.get_enum("progression", "LRCP",
                                                             {"LRCP", "RLCP", "RPCL", "PCRL", "CPRL"}));
  params.colour_transform = !grey;
  const std::uint64_t max = (std::uint64_t{1} << precision) - 1;
  params.fill = [&](std::uint32_t component, std::int32_t* out) {
    const std::size_t channel = (alpha && component == components - 1) ? 3u : (grey ? 0u : component);
    const std::size_t count = std::size_t{frame.width} * frame.height;
    if (wide) {
      for (std::size_t i = 0; i < count; ++i) {
        out[i] = static_cast<std::int32_t>((std::uint64_t{samples16[i * 4 + channel]} * max + 32767u) / 65535u);
      }
    } else {
      for (std::size_t i = 0; i < count; ++i) out[i] = frame.rgba[i * 4 + channel];
    }
  };
  CVT_TRY(ctx.limits.check_decompressed(std::uint64_t{frame.width} * frame.height * components * 12));
  CVT_TRY_ASSIGN(Bytes codestream, jp2::encode_codestream(params, ctx));
  if (container == Container::Codestream) {
    if (alpha) ctx.warn("A raw JPEG 2000 codestream cannot label its alpha channel; readers usually assume it.");
    return codestream;
  }

  jp2::FileSpec spec;
  spec.jpx = container == Container::Jpx;
  spec.width = frame.width;
  spec.height = frame.height;
  spec.components.assign(components, jp2::SizComponent{precision, false, 1, 1});
  spec.enumerated = grey ? 17u : 16u;
  if (write_icc) spec.icc = icc;
  spec.alpha_last = alpha;
  spec.dpi_x = picture.dpi_x;
  spec.dpi_y = picture.dpi_y;
  if (keep) {
    spec.exif = picture.exif;
    spec.xmp = picture.xmp;
  }
  Bytes file = jp2::build_file(spec, codestream);
  CVT_TRY(ctx.limits.check_output(file.size()));
  return file;
}

Result<Bytes> write_codestream(const ir::Image& image, WriteContext& ctx) {
  return write_jpeg2000(image, ctx, Container::Codestream);
}
Result<Bytes> write_jp2(const ir::Image& image, WriteContext& ctx) { return write_jpeg2000(image, ctx, Container::Jp2); }
Result<Bytes> write_jpx(const ir::Image& image, WriteContext& ctx) { return write_jpeg2000(image, ctx, Container::Jpx); }

std::vector<OptionSpec> writer_options() {
  std::vector<OptionSpec> specs = image::lossy_writer_options();
  specs.insert(specs.begin() + 2, options::lossless());
  specs.push_back(OptionSpec::integer("layers", "Quality layers", 1, 1, 20,
                                      "Number of quality layers: viewers can show a coarse picture from the first "
                                      "layers while the rest loads. The last layer meets the chosen quality.")
                      .advanced());
  specs.push_back(OptionSpec::integer("tile_size", "Tile size", 0, 0, 65535,
                                      "Edge of square tiles in pixels (0 = one tile; 16 at least). Tiles lower "
                                      "memory use for very large pictures at a small cost in quality.")
                      .advanced());
  specs.push_back(OptionSpec::choice("progression", "Progression order", "LRCP",
                                     {"LRCP", "RLCP", "RPCL", "PCRL", "CPRL"},
                                     "Order of the data in the file: by layer (LRCP), resolution (RLCP, RPCL), "
                                     "position (PCRL) or component (CPRL).")
                      .advanced());
  specs.push_back(OptionSpec::choice("bit_depth", "Bit depth", "auto", {"auto", "8", "16"},
                                     "Precision per sample. Auto keeps 9 to 16 bits when the source has them.")
                      .advanced());
  return specs;
}

// --------------------------------------------------------------------------- detection

bool ends_with_extension(std::string_view filename, std::string_view extension) {
  if (filename.size() <= extension.size() || filename[filename.size() - extension.size() - 1] != '.') return false;
  const std::string_view tail = filename.substr(filename.size() - extension.size());
  for (std::size_t i = 0; i < tail.size(); ++i) {
    const char c = tail[i] >= 'A' && tail[i] <= 'Z' ? static_cast<char>(tail[i] - 'A' + 'a') : tail[i];
    if (c != extension[i]) return false;
  }
  return true;
}

int sniff_jp2(std::span<const std::byte> data, std::string_view /*filename*/) {
  return jp2::file_brand(data) == "jp2 " ? 98 : 0;
}

int sniff_jpx(std::span<const std::byte> data, std::string_view filename) {
  if (jp2::file_brand(data) != "jpx ") return 0;
  return ends_with_extension(filename, "jpf") ? 90 : 98;
}

int sniff_jpf(std::span<const std::byte> data, std::string_view filename) {
  return jp2::file_brand(data) == "jpx " && ends_with_extension(filename, "jpf") ? 98 : 0;
}

void add(Registry& registry, std::string id, std::string name, std::string description,
         std::vector<std::string> extensions, std::string mime, int popularity,
         std::function<int(std::span<const std::byte>, std::string_view)> sniff, image::ImageWriter write) {
  FormatInfo info;
  info.id = id;
  info.name = std::move(name);
  info.description = std::move(description);
  info.category = Category::Image;
  info.extensions = std::move(extensions);
  info.mime = std::move(mime);
  info.popularity = popularity;
  if (sniff) {
    info.sniff = std::move(sniff);
  } else {
    info.signatures = {Signature::at(0, "FF 4F FF 51")};
  }
  registry.add_format(std::move(info));

  image::ImageCodecSpec spec;
  spec.format_id = std::move(id);
  spec.read = image::reader(read_jpeg2000);
  spec.write = image::writer(write);
  spec.write_options = writer_options();
  spec.lossy_write = true;
  spec.read_cost = 15;
  spec.write_cost = 20;
  image::add_image_codec(registry, std::move(spec));
}

}  // namespace

void register_jp2(Registry& registry) {
  add(registry, "jp2", "JPEG 2000 image", "Wavelet-based JPEG 2000 image in the JP2 container, lossy or lossless.",
      {"jp2"}, "image/jp2", 25, sniff_jp2, write_jp2);
  add(registry, "j2k", "JPEG 2000 codestream", "Raw JPEG 2000 codestream without a file container.",
      {"j2k", "j2c", "jpc"}, "image/j2c", 12, nullptr, write_codestream);
  add(registry, "jpx", "JPEG 2000 extended image", "JPEG 2000 Part 2 (JPX) file; written JP2-compatible.",
      {"jpx"}, "image/jpx", 8, sniff_jpx, write_jpx);
  add(registry, "jpf", "JPEG 2000 extended image (.jpf)", "JPX file with the .jpf extension; written JP2-compatible.",
      {"jpf"}, "image/jpx", 6, sniff_jpf, write_jpx);
}

}  // namespace cvt::image_modern
