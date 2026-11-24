// Convertible core - pdf family: image decoding.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/pdf/pdf_image.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "engine/registry.hpp"

namespace cvt::pdf {

namespace {

const Codec* find_codec(const Registry& registry, std::string_view format_id, bool reader) {
  for (const Codec* codec : registry.codecs_for(format_id)) {
    if (codec->kind == Kind::Image && (reader ? static_cast<bool>(codec->read) : static_cast<bool>(codec->write))) {
      return codec;
    }
  }
  return nullptr;
}

struct Sampler {
  const std::uint8_t* data;
  std::size_t size;
  int bpc;
  [[nodiscard]] std::uint32_t at(std::uint64_t bit_offset) const {
    if (bpc == 8) {
      const std::size_t i = static_cast<std::size_t>(bit_offset / 8);
      return i < size ? data[i] : 0;
    }
    if (bpc == 16) {
      const std::size_t i = static_cast<std::size_t>(bit_offset / 8);
      return i + 1 < size ? (static_cast<std::uint32_t>(data[i]) << 8) | data[i + 1] : 0;
    }
    const std::size_t i = static_cast<std::size_t>(bit_offset / 8);
    if (i >= size) return 0;
    const int shift = 8 - bpc - static_cast<int>(bit_offset % 8);
    return (data[i] >> shift) & ((1u << bpc) - 1);
  }
};

}  // namespace

Result<ir::Image> decode_with_registry(const Context* ctx, std::string_view format_id, ByteView data) {
  if (ctx == nullptr || ctx->registry == nullptr) {
    return fail(Status::UnsupportedConversion, "no {} decoder is available", format_id);
  }
  const Codec* codec = find_codec(*ctx->registry, format_id, true);
  if (codec == nullptr) return fail(Status::UnsupportedConversion, "no {} decoder is available", format_id);
  ReadContext rc;
  CVT_TRY_ASSIGN(rc.options, Options().normalized(codec->read_options));
  rc.limits = ctx->limits;
  rc.registry = ctx->registry;
  rc.progress = Progress();
  rc.input = data;
  rc.format_id = std::string(format_id);
  CVT_TRY_ASSIGN(AnyIR value, codec->read(rc));
  if (!value.is<ir::Image>()) return fail(Status::Decode, "the {} decoder returned no image", format_id);
  ir::Image image = std::move(value).get<ir::Image>();
  if (!image.valid()) return fail(Status::Decode, "the embedded {} image is empty", format_id);
  return image;
}

Result<Bytes> encode_with_registry(const Context* ctx, std::string_view format_id, const ir::Image& image,
                                   std::string_view options_json) {
  if (ctx == nullptr || ctx->registry == nullptr) {
    return fail(Status::UnsupportedConversion, "no {} encoder is available", format_id);
  }
  const Codec* codec = find_codec(*ctx->registry, format_id, false);
  if (codec == nullptr) return fail(Status::UnsupportedConversion, "no {} encoder is available", format_id);
  CVT_TRY_ASSIGN(const Options options, Options::parse(options_json));
  WriteContext wc;
  CVT_TRY_ASSIGN(wc.options, options.normalized(codec->write_options));
  wc.limits = ctx->limits;
  wc.registry = ctx->registry;
  wc.format_id = std::string(format_id);
  return codec->write(AnyIR(image), wc);
}

std::optional<Bytes> passthrough_jpeg(PdfFile& file, const Object& image_ref) {
  const Object image = file.resolve(image_ref);
  const Stream* stream = image.as_stream();
  if (stream == nullptr) return std::nullopt;
  const Dict& d = stream->dict;
  const Object filter = file.get(d, "Filter");
  const bool dct = filter.is_name("DCTDecode") ||
                   (filter.is_array() && filter.as_array().size() == 1 && file.resolve(filter.as_array()[0]).is_name("DCTDecode"));
  if (!dct || d.contains("SMask") || d.contains("Mask") || d.contains("Decode")) return std::nullopt;
  const Object cs = file.get(d, "ColorSpace");
  const std::string name = cs.is_name() ? cs.as_name() : std::string();
  if (name != "DeviceRGB" && name != "DeviceGray") {
    if (!(cs.is_array() && !cs.as_array().empty() && file.resolve(cs.as_array()[0]).is_name("ICCBased"))) return std::nullopt;
    const std::int64_t n = file.get(file.resolve(cs.as_array().size() > 1 ? cs.as_array()[1] : Object{}).as_dict(), "N").as_int(0);
    if (n != 1 && n != 3) return std::nullopt;
  }
  if (stream->data.size() < 4 || std::to_integer<int>(stream->data[0]) != 0xFF || std::to_integer<int>(stream->data[1]) != 0xD8) {
    return std::nullopt;
  }
  return stream->data;
}

Result<DecodedImage> decode_image(PdfFile& file, const ImageEvent& event, const Context* ctx, int depth) {
  if (depth > 2 || event.dict == nullptr) return fail(Status::Decode, "bad image");
  const Dict& d = *event.dict;
  const auto get = [&](std::string_view key) { return file.get(d, key); };
  const std::int64_t w = get("Width").as_int(0), h = get("Height").as_int(0);
  if (w <= 0 || h <= 0 || w > 1'000'000 || h > 1'000'000) return fail(Status::Decode, "image has an invalid size");
  const Limits limits = ctx != nullptr ? ctx->limits : file.limits();
  CVT_TRY(limits.check_pixels(static_cast<std::uint64_t>(w), static_cast<std::uint64_t>(h)));
  DecodedImage out;
  out.width = static_cast<std::uint32_t>(w);
  out.height = static_cast<std::uint32_t>(h);
  out.interpolate = get("Interpolate").as_bool(false);
  const std::size_t pixels = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
  const bool stencil = get("ImageMask").as_bool(false);
  out.stencil = stencil;

  // Raw data through the filters.
  DecodedData decoded;
  if (event.is_inline) {
    FilterEnv env;
    env.max_output = limits.max_decompressed_bytes;
    env.max_pixels = limits.max_pixels;
    env.resolve = [&file](const Object& o) { return file.resolve(o); };
    CVT_TRY_ASSIGN(decoded, apply_filters(event.inline_data, get("Filter"), get("DecodeParms"), env));
  } else {
    CVT_TRY_ASSIGN(decoded, file.decode(event.stream, true));
  }

  out.rgba.assign(pixels * 4, 255);
  const Object mask = get("Mask");
  // Encoded image codecs via the registry.
  if (!decoded.pending_filter.empty()) {
    const std::string format = decoded.pending_filter == "DCTDecode" ? "jpg" : "jp2";
    CVT_TRY_ASSIGN(ir::Image picture, decode_with_registry(ctx, format, decoded.data));
    const ir::Frame& frame = picture.frames.front();
    // Resample to the declared size if the codec disagrees (rare).
    for (std::uint32_t y = 0; y < out.height; ++y) {
      const std::uint32_t sy = static_cast<std::uint32_t>(static_cast<std::uint64_t>(y) * frame.height / out.height);
      for (std::uint32_t x = 0; x < out.width; ++x) {
        const std::uint32_t sx = static_cast<std::uint32_t>(static_cast<std::uint64_t>(x) * frame.width / out.width);
        const std::size_t s = frame.offset(sx, sy), t = (static_cast<std::size_t>(y) * out.width + x) * 4;
        for (int c = 0; c < 4; ++c) out.rgba[t + static_cast<std::size_t>(c)] = frame.rgba[s + static_cast<std::size_t>(c)];
      }
    }
    // A Decode array [1 0 ...] on a JPEG inverts it (Adobe CMYK); stencil JPEGs are rare.
    const Array& dec = get("Decode").as_array();
    if (dec.size() >= 2 && file.resolve(dec[0]).as_number(0) > file.resolve(dec[1]).as_number(1)) {
      for (std::size_t i = 0; i < out.rgba.size(); i += 4) {
        for (int c = 0; c < 3; ++c) out.rgba[i + static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(255 - out.rgba[i + static_cast<std::size_t>(c)]);
      }
    }
    if (stencil) {
      for (std::size_t i = 0; i < out.rgba.size(); i += 4) out.rgba[i + 3] = static_cast<std::uint8_t>(255 - out.rgba[i]);
      out.has_alpha = true;
      return out;
    }
  } else {
    const auto* data = reinterpret_cast<const std::uint8_t*>(decoded.data.data());
    const std::size_t size = decoded.data.size();
    int bpc = static_cast<int>(get("BitsPerComponent").as_int(stencil ? 1 : 8));
    if (stencil) bpc = 1;
    if (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8 && bpc != 16) bpc = 8;
    std::shared_ptr<const ColorSpace> space;
    if (!stencil) {
      space = load_color_space(file, get("ColorSpace"), event.resources);
      if (!space) {
        // JBIG2 and CCITT images may omit the colour space: gray.
        space = device_gray();
      }
      if (space->kind() == ColorSpace::Kind::Pattern) space = device_gray();
    }
    const int n = stencil ? 1 : std::max(1, space->components());
    // Decode ranges.
    std::vector<double> decode(static_cast<std::size_t>(2 * n));
    const Array& dec = get("Decode").as_array();
    for (int c = 0; c < n; ++c) {
      std::array<double, 2> range{0, 1};
      if (!stencil) range = space->decode_range(c, bpc);
      if (dec.size() >= static_cast<std::size_t>(2 * n)) {
        range = {file.resolve(dec[static_cast<std::size_t>(2 * c)]).as_number(range[0]),
                 file.resolve(dec[static_cast<std::size_t>(2 * c + 1)]).as_number(range[1])};
      }
      decode[static_cast<std::size_t>(2 * c)] = range[0];
      decode[static_cast<std::size_t>(2 * c + 1)] = range[1];
    }
    const double max_value = std::pow(2.0, bpc) - 1;
    const std::uint64_t row_bits = (static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(n) * static_cast<std::uint64_t>(bpc) + 7) / 8 * 8;
    const Sampler sampler{data, size, bpc};
    // Colour key masking on raw values.
    std::vector<std::int64_t> key;
    if (mask.is_array()) {
      for (const Object& v : mask.as_array()) key.push_back(file.resolve(v).as_int());
      if (key.size() < static_cast<std::size_t>(2 * n)) key.clear();
    }
    if (stencil) {
      const bool inverted = decode[0] > decode[1];  // [1 0]: sample 1 paints
      for (std::size_t y = 0; y < out.height; ++y) {
        for (std::size_t x = 0; x < out.width; ++x) {
          const std::uint32_t v = sampler.at(y * row_bits + x);
          const bool paint = inverted ? v == 1 : v == 0;
          out.rgba[(y * out.width + x) * 4 + 3] = paint ? 255 : 0;
        }
      }
      out.has_alpha = true;
      return out;
    }
    // Lookup tables for single-component 8-bit-or-less images.
    const bool fast_device = bpc == 8 && dec.empty() &&
                             (space->kind() == ColorSpace::Kind::Gray || space->kind() == ColorSpace::Kind::Rgb);
    std::vector<std::array<std::uint8_t, 3>> table;
    if (n == 1 && bpc <= 8) {
      table.resize(static_cast<std::size_t>(max_value) + 1);
      for (std::size_t v = 0; v < table.size(); ++v) {
        const double c = decode[0] + static_cast<double>(v) * (decode[1] - decode[0]) / max_value;
        const Rgb rgb = space->to_rgb(&c);
        table[v] = {static_cast<std::uint8_t>(std::lround(rgb.r * 255)), static_cast<std::uint8_t>(std::lround(rgb.g * 255)),
                    static_cast<std::uint8_t>(std::lround(rgb.b * 255))};
      }
    }
    std::unordered_map<std::uint64_t, std::array<std::uint8_t, 3>> cache;
    std::vector<double> comps(static_cast<std::size_t>(n));
    std::vector<std::uint32_t> raw(static_cast<std::size_t>(n));
    for (std::size_t y = 0; y < out.height; ++y) {
      if ((y & 255) == 0 && ctx != nullptr && ctx->progress.cancelled()) return fail(Status::Cancelled, "cancelled");
      const std::uint64_t row = y * row_bits;
      std::uint8_t* dst = out.rgba.data() + y * out.width * 4;
      if (fast_device) {
        const std::size_t base = static_cast<std::size_t>(row / 8);
        for (std::size_t x = 0; x < out.width; ++x, dst += 4) {
          if (n == 1) {
            const std::uint8_t v = base + x < size ? data[base + x] : 0;
            dst[0] = dst[1] = dst[2] = v;
          } else {
            const std::size_t i = base + 3 * x;
            dst[0] = i < size ? data[i] : 0;
            dst[1] = i + 1 < size ? data[i + 1] : 0;
            dst[2] = i + 2 < size ? data[i + 2] : 0;
          }
        }
      } else {
        for (std::size_t x = 0; x < out.width; ++x, dst += 4) {
          const std::uint64_t bit = row + x * static_cast<std::uint64_t>(n) * static_cast<std::uint64_t>(bpc);
          std::uint64_t hash = 0;
          for (int c = 0; c < n; ++c) {
            raw[static_cast<std::size_t>(c)] = sampler.at(bit + static_cast<std::uint64_t>(c * bpc));
            hash = hash * 65599 + raw[static_cast<std::size_t>(c)];
          }
          std::array<std::uint8_t, 3> rgb{};
          if (!table.empty()) {
            rgb = table[std::min<std::size_t>(raw[0], table.size() - 1)];
          } else if (const auto it = cache.find(hash); it != cache.end() && n <= 4 && bpc <= 8) {
            rgb = it->second;
          } else {
            for (int c = 0; c < n; ++c) {
              const auto k = static_cast<std::size_t>(c);
              comps[k] = decode[2 * k] + raw[k] * (decode[2 * k + 1] - decode[2 * k]) / max_value;
            }
            const Rgb v = space->to_rgb(comps.data());
            rgb = {static_cast<std::uint8_t>(std::lround(std::clamp(v.r, 0.0, 1.0) * 255)),
                   static_cast<std::uint8_t>(std::lround(std::clamp(v.g, 0.0, 1.0) * 255)),
                   static_cast<std::uint8_t>(std::lround(std::clamp(v.b, 0.0, 1.0) * 255))};
            if (n <= 4 && bpc <= 8 && cache.size() < 65536) cache.emplace(hash, rgb);
          }
          dst[0] = rgb[0];
          dst[1] = rgb[1];
          dst[2] = rgb[2];
          if (!key.empty()) {
            bool masked = true;
            for (int c = 0; c < n && masked; ++c) {
              const auto k = static_cast<std::size_t>(c);
              masked = raw[k] >= key[2 * k] && raw[k] <= key[2 * k + 1];
            }
            if (masked) {
              dst[3] = 0;
              out.has_alpha = true;
            }
          }
        }
      }
    }
  }
  // Soft mask / stencil mask from another image.
  const auto apply_alpha = [&](const DecodedImage& m, bool use_luminance, bool invert) {
    for (std::uint32_t y = 0; y < out.height; ++y) {
      const std::uint32_t my = static_cast<std::uint32_t>(static_cast<std::uint64_t>(y) * m.height / out.height);
      for (std::uint32_t x = 0; x < out.width; ++x) {
        const std::uint32_t mx = static_cast<std::uint32_t>(static_cast<std::uint64_t>(x) * m.width / out.width);
        const std::size_t s = (static_cast<std::size_t>(my) * m.width + mx) * 4;
        std::uint8_t a = use_luminance ? m.rgba[s] : m.rgba[s + 3];
        if (invert) a = static_cast<std::uint8_t>(255 - a);
        std::uint8_t& dst = out.rgba[(static_cast<std::size_t>(y) * out.width + x) * 4 + 3];
        dst = static_cast<std::uint8_t>(dst * a / 255);
      }
    }
    out.has_alpha = true;
  };
  const Object smask = get("SMask");
  if (smask.is_stream()) {
    ImageEvent mev;
    mev.dict = &smask.as_dict();
    mev.stream = smask;
    mev.resources = event.resources;
    // The soft mask is a gray image; its colour space is implied.
    auto m = decode_image(file, mev, ctx, depth + 1);
    if (m) {
      apply_alpha(*m, true, false);
      const Array& matte = file.get(smask.as_dict(), "Matte").as_array();
      if (matte.size() >= 1) {
        // Un-premultiply against the matte colour (approximated in RGB).
        const std::uint8_t mr = static_cast<std::uint8_t>(std::clamp(file.resolve(matte[0]).as_number() * 255, 0.0, 255.0));
        for (std::size_t i = 0; i < out.rgba.size(); i += 4) {
          const int a = out.rgba[i + 3];
          if (a == 0) continue;
          for (int c = 0; c < 3; ++c) {
            const int v = mr + (out.rgba[i + static_cast<std::size_t>(c)] - mr) * 255 / a;
            out.rgba[i + static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(std::clamp(v, 0, 255));
          }
        }
      }
    }
  } else if (mask.is_stream()) {
    ImageEvent mev;
    mev.dict = &mask.as_dict();
    mev.stream = mask;
    mev.resources = event.resources;
    auto m = decode_image(file, mev, ctx, depth + 1);
    if (m && m->stencil) apply_alpha(*m, false, false);
  }
  return out;
}

}  // namespace cvt::pdf
