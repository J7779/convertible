// Convertible core - image family: helpers shared by the format codecs.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/image/common.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>

#include "engine/standard_options.hpp"
#include "util/bytes.hpp"

namespace cvt::image {

// --------------------------------------------------------------------------- option sets

std::vector<OptionSpec> size_options() { return {options::max_width(), options::max_height(), options::scale()}; }

OptionSpec background_option() {
  return OptionSpec::text("background", "Background colour", "ffffff",
                          "Colour placed behind transparent areas when the format has no transparency "
                          "(RRGGBB hex or a name such as white or black).")
      .advanced();
}

Result<Rgb> resolved_background(const Options& options) {
  const std::string text = options.get_string("background", "ffffff");
  auto color = parse_color(text);
  if (!color) return fail(Status::InvalidArgument, "option 'background': {}", color.error().message);
  return *color;
}

OptionSpec dither_option() {
  return OptionSpec::choice("dither", "Dithering", "floyd-steinberg", {"floyd-steinberg", "ordered", "none"},
                            "How colours missing from the palette are simulated: error diffusion (best for "
                            "photos), an ordered pattern (stable in animations) or none (flat areas).")
      .advanced();
}

Dither resolved_dither(const Options& options) {
  const std::string value = options.get_enum("dither", "floyd-steinberg", {"floyd-steinberg", "ordered", "none"});
  if (value == "ordered") return Dither::Ordered;
  if (value == "none") return Dither::None;
  return Dither::FloydSteinberg;
}

std::vector<OptionSpec> with(std::vector<OptionSpec> specs, std::initializer_list<OptionSpec> more) {
  specs.insert(specs.end(), more.begin(), more.end());
  return specs;
}

std::vector<OptionSpec> lossless_writer_options(int max_level) {
  std::vector<OptionSpec> specs = {options::compression_level(max_level), options::preset()};
  for (OptionSpec& spec : size_options()) specs.push_back(std::move(spec));
  specs.push_back(options::strip_metadata());
  return specs;
}

std::vector<OptionSpec> lossy_writer_options() {
  std::vector<OptionSpec> specs = {options::quality(), options::preset()};
  for (OptionSpec& spec : size_options()) specs.push_back(std::move(spec));
  specs.push_back(options::strip_metadata());
  return specs;
}

std::vector<OptionSpec> plain_writer_options() {
  std::vector<OptionSpec> specs = {options::preset()};
  for (OptionSpec& spec : size_options()) specs.push_back(std::move(spec));
  specs.push_back(options::strip_metadata());
  return specs;
}

// --------------------------------------------------------------------------- registration

void add_image_codec(Registry& registry, ImageCodecSpec spec) {
  if (spec.lossy_write && spec.read && spec.write) {
    Codec read_codec;
    read_codec.format_id = spec.format_id;
    read_codec.kind = Kind::Image;
    read_codec.read = std::move(spec.read);
    read_codec.read_cost = spec.read_cost;
    read_codec.read_options = std::move(spec.read_options);
    registry.add_codec(std::move(read_codec));
    spec.read = nullptr;
    spec.read_options.clear();
  }
  Codec codec;
  codec.format_id = std::move(spec.format_id);
  codec.kind = Kind::Image;
  codec.read = std::move(spec.read);
  codec.write = std::move(spec.write);
  codec.read_cost = spec.read_cost;
  codec.write_cost = spec.write_cost;
  codec.lossy = spec.lossy_write;
  codec.write_options = std::move(spec.write_options);
  codec.read_options = std::move(spec.read_options);
  registry.add_codec(std::move(codec));
}

std::function<Result<AnyIR>(ReadContext&)> reader(ImageReader fn) {
  return [fn](ReadContext& ctx) -> Result<AnyIR> {
    CVT_TRY_ASSIGN(ir::Image image, fn(ctx));
    if (!image.valid()) return fail(Status::Decode, "the file contains no picture");
    return AnyIR(std::move(image));
  };
}

std::function<Result<Bytes>(const AnyIR&, WriteContext&)> writer(ImageWriter fn) {
  return [fn](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
    const ir::Image* image = value.get_if<ir::Image>();
    if (image == nullptr) return fail(Status::Internal, "image writer called with another kind");
    return fn(*image, ctx);
  };
}

// --------------------------------------------------------------------------- reading

Result<void> check_size(const Context& ctx, std::uint64_t width, std::uint64_t height, std::uint64_t frames) {
  if (width == 0 || height == 0) return fail(Status::Decode, "the image has no pixels ({}x{})", width, height);
  if (width > 0x7FFFFFFFu || height > 0x7FFFFFFFu) {
    return fail(Status::Limit, "image dimensions {}x{} are too large", width, height);
  }
  CVT_TRY(ctx.limits.check_pixels(width, height, frames == 0 ? 1 : frames));
  const std::uint64_t bytes = width * height * 4;
  if (bytes / 4 / width != height || bytes > static_cast<std::uint64_t>(SIZE_MAX / 2)) {
    return fail(Status::Limit, "image of {}x{} pixels does not fit in memory", width, height);
  }
  return {};
}

Result<void> check_plausible(std::uint64_t pixels, std::uint64_t data_bytes, std::uint64_t max_pixels_per_byte) {
  // Small pictures are always fine; the check is about absurd allocations.
  if (pixels <= (1u << 16)) return {};
  const std::uint64_t budget = (data_bytes + 64) * max_pixels_per_byte;
  if (max_pixels_per_byte != 0 && budget / max_pixels_per_byte != data_bytes + 64) return {};  // overflow: plenty
  if (pixels > budget) {
    return fail(Status::Decode, "the declared picture size ({} pixels) does not match the {} bytes of image data",
                pixels, data_bytes);
  }
  return {};
}

Result<ir::Frame> new_frame(const Context& ctx, std::uint32_t width, std::uint32_t height, Rgba fill) {
  CVT_TRY(check_size(ctx, width, height));
  return ir::Frame::filled(width, height, fill);
}

// --------------------------------------------------------------------------- EXIF

namespace {

struct TiffView {
  ByteView data;
  bool little = true;
  [[nodiscard]] bool has(std::uint64_t offset, std::uint64_t count) const noexcept {
    return offset <= data.size() && count <= data.size() - offset;
  }
  [[nodiscard]] std::uint16_t u16(std::uint64_t offset) const noexcept {
    if (!has(offset, 2)) return 0;
    const auto a = std::to_integer<std::uint16_t>(data[offset]);
    const auto b = std::to_integer<std::uint16_t>(data[offset + 1]);
    return little ? static_cast<std::uint16_t>(a | (b << 8)) : static_cast<std::uint16_t>((a << 8) | b);
  }
  [[nodiscard]] std::uint32_t u32(std::uint64_t offset) const noexcept {
    if (!has(offset, 4)) return 0;
    const std::uint32_t a = u16(offset);
    const std::uint32_t b = u16(offset + 2);
    return little ? (a | (b << 16)) : ((a << 16) | b);
  }
};

std::optional<TiffView> tiff_view(ByteView exif) {
  if (exif.size() >= 6 && as_chars(exif.first(6)) == exif_app1_prefix) exif = exif.subspan(6);
  if (exif.size() < 8) return std::nullopt;
  TiffView view{exif, true};
  const std::string_view magic = as_chars(exif.first(4));
  if (magic == std::string_view("II*\0", 4)) {
    view.little = true;
  } else if (magic == std::string_view("MM\0*", 4)) {
    view.little = false;
  } else {
    return std::nullopt;
  }
  return view;
}

double rational_at(const TiffView& view, std::uint32_t offset) {
  const std::uint32_t num = view.u32(offset);
  const std::uint32_t den = view.u32(offset + 4);
  if (!view.has(offset, 8) || den == 0) return 0.0;
  return static_cast<double>(num) / den;
}

}  // namespace

std::optional<ExifInfo> parse_exif(ByteView exif) {
  const auto view = tiff_view(exif);
  if (!view) return std::nullopt;
  const std::uint32_t ifd = view->u32(4);
  if (!view->has(ifd, 2)) return std::nullopt;
  const std::uint16_t count = view->u16(ifd);
  if (!view->has(ifd + 2, std::uint64_t{count} * 12)) return std::nullopt;
  ExifInfo info;
  double xres = 0.0;
  double yres = 0.0;
  int unit = 2;
  for (std::uint16_t i = 0; i < count; ++i) {
    const std::uint32_t entry = ifd + 2 + i * 12u;
    const std::uint16_t tag = view->u16(entry);
    const std::uint16_t type = view->u16(entry + 2);
    const std::uint32_t n = view->u32(entry + 4);
    const std::uint32_t value_offset = entry + 8;
    switch (tag) {
      case 0x0112: {  // Orientation, SHORT
        const int value = type == 3 ? view->u16(value_offset) : static_cast<int>(view->u32(value_offset));
        if (value >= 1 && value <= 8) info.orientation = value;
        break;
      }
      case 0x011A:  // XResolution, RATIONAL
        if (type == 5) xres = rational_at(*view, view->u32(value_offset));
        break;
      case 0x011B:
        if (type == 5) yres = rational_at(*view, view->u32(value_offset));
        break;
      case 0x0128:
        unit = type == 3 ? view->u16(value_offset) : 2;
        break;
      case 0x010E:
      case 0x010F:
      case 0x0110:
      case 0x0131:
      case 0x0132:
      case 0x013B:
      case 0x8298: {
        if (type != 2 || n == 0 || n > 4096) break;
        const std::uint32_t at = n <= 4 ? value_offset : view->u32(value_offset);
        if (!view->has(at, n)) break;
        std::string text(as_chars(view->data.subspan(at, n)));
        while (!text.empty() && (text.back() == '\0' || text.back() == ' ')) text.pop_back();
        if (text.empty()) break;
        const char* name = tag == 0x010E   ? "ImageDescription"
                           : tag == 0x010F ? "Make"
                           : tag == 0x0110 ? "Model"
                           : tag == 0x0131 ? "Software"
                           : tag == 0x0132 ? "DateTime"
                           : tag == 0x013B ? "Artist"
                                           : "Copyright";
        info.text[name] = std::move(text);
        break;
      }
      default: break;
    }
  }
  if (xres > 0 && yres > 0 && std::isfinite(xres) && std::isfinite(yres)) {
    const double factor = unit == 3 ? 2.54 : 1.0;  // 3 = centimetres
    if (unit == 2 || unit == 3) {
      info.dpi_x = xres * factor;
      info.dpi_y = yres * factor;
    }
  }
  return info;
}

void reset_exif_orientation(Bytes& exif) noexcept {
  std::size_t base = 0;
  if (exif.size() >= 6 && as_chars(ByteView(exif).first(6)) == exif_app1_prefix) base = 6;
  const auto view = tiff_view(exif);
  if (!view) return;
  const std::uint32_t ifd = view->u32(4);
  if (!view->has(ifd, 2)) return;
  const std::uint16_t count = view->u16(ifd);
  if (!view->has(ifd + 2, std::uint64_t{count} * 12)) return;
  for (std::uint16_t i = 0; i < count; ++i) {
    const std::uint32_t entry = ifd + 2 + i * 12u;
    if (view->u16(entry) != 0x0112) continue;
    const std::size_t at = base + entry + 8;
    const std::uint16_t type = view->u16(entry + 2);
    if (type == 3) {
      exif[at] = std::byte{static_cast<unsigned char>(view->little ? 1 : 0)};
      exif[at + 1] = std::byte{static_cast<unsigned char>(view->little ? 0 : 1)};
    } else if (type == 4) {
      for (std::size_t k = 0; k < 4; ++k) exif[at + k] = std::byte{0};
      exif[at + (view->little ? 0 : 3)] = std::byte{1};
    }
    return;
  }
}

int attach_exif(ir::Image& image, ByteView exif) {
  if (exif.size() >= 6 && as_chars(exif.first(6)) == exif_app1_prefix) exif = exif.subspan(6);
  const auto info = parse_exif(exif);
  if (!info) return 1;
  image.exif = to_bytes(exif);
  reset_exif_orientation(image.exif);
  for (const auto& [key, value] : info->text) image.metadata.emplace(key, value);
  if (image.dpi_x <= 0 && info->dpi_x > 0) {
    image.dpi_x = info->dpi_x;
    image.dpi_y = info->dpi_y;
  }
  if (info->orientation != 1) apply_orientation(image, info->orientation);
  return info->orientation;
}

// --------------------------------------------------------------------------- writing

Result<WriteImage> prepare_write(const ir::Image& source, const WriteContext& ctx) {
  if (!source.valid()) return fail(Status::Encode, "the image has no pixels");
  WriteImage result;
  result.source_ = &source;
  result.keep_metadata_ = !options::resolved_strip_metadata(ctx.options);
  const options::Size target =
      options::resolved_image_size(ctx.options, {source.width(), source.height()});
  if (target.width != source.width() || target.height != source.height()) {
    CVT_TRY(ctx.limits.check_pixels(target.width, target.height, source.frames.size()));
    ir::Image& image = result.mutate();
    CVT_TRY(resize(image, target.width, target.height));
  }
  return result;
}

void warn_if_animated(const ir::Image& image, const WriteContext& ctx, std::string_view format_name) {
  if (image.animated()) {
    ctx.warn(std::format("{} holds a single picture; only the first of {} frames was written.", format_name,
                         image.frames.size()));
  }
}

Result<ir::Frame> opaque_frame(const ir::Frame& frame, const WriteContext& ctx, std::string_view format_name) {
  ir::Frame copy = frame;
  if (!is_opaque(copy)) {
    CVT_TRY_ASSIGN(const Rgb background, resolved_background(ctx.options));
    flatten_alpha(copy, background);
    ctx.warn(std::format("{} has no transparency; transparent areas were blended onto the background colour.",
                         format_name));
  }
  return copy;
}

}  // namespace cvt::image
