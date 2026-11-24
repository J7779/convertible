// Convertible core - image family: decode/encode any registered image format.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/image/image_api.hpp"

#include <cstring>
#include <utility>

#include "engine/registry.hpp"

namespace cvt::image {

namespace {

const Registry& registry_of(const Context& ctx) { return ctx.registry != nullptr ? *ctx.registry : Registry::instance(); }

const Codec* image_codec(const Registry& registry, std::string_view format_id, bool want_reader) {
  std::string id = normalize_format_id(format_id);
  if (registry.codecs_for(id).empty()) {
    // Accept extensions and aliases ("jpeg", "tif", ".PNG").
    if (const FormatInfo* info = registry.resolve(format_id)) id = info->id;
  }
  for (const Codec* codec : registry.codecs_for(id)) {
    if (codec->kind != Kind::Image) continue;
    if (want_reader ? static_cast<bool>(codec->read) : static_cast<bool>(codec->write)) return codec;
  }
  return nullptr;
}

}  // namespace

Result<ir::Image> decode(std::string_view format_id, ByteView bytes, const Context& ctx, const Options& options) {
  const Registry& registry = registry_of(ctx);
  const Codec* codec = image_codec(registry, format_id, true);
  if (codec == nullptr) return fail(Status::UnsupportedConversion, "no image decoder is available for '{}'", format_id);
  ReadContext read;
  CVT_TRY_ASSIGN(read.options, options.normalized(codec->read_options));
  read.progress = ctx.progress;
  read.limits = ctx.limits;
  read.warnings = ctx.warnings;
  read.registry = &registry;
  read.input = bytes;
  read.format_id = codec->format_id;
  CVT_TRY_ASSIGN(AnyIR value, codec->read(read));
  ir::Image* image = value.get_if<ir::Image>();
  if (image == nullptr) return fail(Status::Internal, "the '{}' image decoder returned another kind", format_id);
  return std::move(*image);
}

Result<Bytes> encode(std::string_view format_id, const ir::Image& image, const Options& options, const Context& ctx) {
  const Registry& registry = registry_of(ctx);
  const Codec* codec = image_codec(registry, format_id, false);
  if (codec == nullptr) return fail(Status::UnsupportedConversion, "no image encoder is available for '{}'", format_id);
  WriteContext write;
  CVT_TRY_ASSIGN(write.options, options.normalized(codec->write_options));
  write.progress = ctx.progress;
  write.limits = ctx.limits;
  write.warnings = ctx.warnings;
  write.registry = &registry;
  write.format_id = codec->format_id;
  const AnyIR value(image);
  return codec->write(value, write);
}

bool can_decode(std::string_view format_id, const Context& ctx) {
  return image_codec(registry_of(ctx), format_id, true) != nullptr;
}

bool can_encode(std::string_view format_id, const Context& ctx) {
  return image_codec(registry_of(ctx), format_id, false) != nullptr;
}

std::optional<std::string> sniff_format(ByteView bytes) {
  const std::string_view s = as_chars(bytes);
  const auto starts = [&](std::string_view magic, std::size_t at = 0) {
    return s.size() >= at + magic.size() && s.substr(at, magic.size()) == magic;
  };
  using namespace std::string_view_literals;
  if (starts("\x89PNG\r\n\x1A\n"sv)) return "png";
  if (starts("\xFF\xD8\xFF"sv)) return "jpg";
  if (starts("GIF87a") || starts("GIF89a")) return "gif";
  if (starts("RIFF") && starts("WEBP", 8)) return "webp";
  if (starts("II*\0"sv) || starts("MM\0*"sv) || starts("II+\0"sv) || starts("MM\0+"sv)) return "tiff";
  if (starts("BM") && s.size() >= 18) return "bmp";
  if (starts("\0\0\1\0"sv) && s.size() >= 6) return "ico";
  if (starts("\0\0\2\0"sv) && s.size() >= 6) return "cur";
  if (starts("icns")) return "icns";
  if (starts("qoif")) return "qoi";
  if (starts("8BPS")) return "psd";
  if (starts("DDS ")) return "dds";
  if (starts("#?RADIANCE") || starts("#?RGBE")) return "hdr";
  if (starts("farbfeld")) return "ff";
  if (starts("\x01\xDA"sv)) return "sgi";
  if (starts("\x59\xA6\x6A\x95"sv)) return "ras";
  if (starts("\0\0\0\x0CjP  \r\n\x87\n"sv) || starts("\xFF\x4F\xFF\x51"sv)) return "jp2";
  if (starts("\0\0\0\x0CJXL \r\n\x87\n"sv) || starts("\xFF\x0A"sv)) return "jxl";
  if (s.size() >= 12 && starts("ftyp", 4)) {
    const std::string_view brand = s.substr(8, 4);
    if (brand == "avif" || brand == "avis") return "avif";
    if (brand == "heic" || brand == "heix" || brand == "mif1" || brand == "msf1") return "heic";
  }
  if (s.size() >= 3 && s[0] == 'P' && s[1] >= '1' && s[1] <= '7') {
    switch (s[1]) {
      case '1':
      case '4': return "pbm";
      case '2':
      case '5': return "pgm";
      case '3':
      case '6': return "ppm";
      default: return "pam";
    }
  }
  if (starts("PF\n") || starts("Pf\n") || starts("PF\r") || starts("Pf\r")) return "pfm";
  if (s.size() >= 4 && static_cast<unsigned char>(s[0]) == 0x0A && static_cast<unsigned char>(s[2]) == 1 &&
      static_cast<unsigned char>(s[1]) <= 5) {
    return "pcx";
  }
  return std::nullopt;
}

}  // namespace cvt::image
