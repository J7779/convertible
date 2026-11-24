// Convertible core - image_modern family: OpenRaster (.ora) and Krita (.kra).
//
// Both are ZIP packages whose first entry is an uncompressed `mimetype`.
// Reading prefers the flattened `mergedimage.png`; an OpenRaster file
// without one is rebuilt from `stack.xml` (nested stacks, layer offsets,
// opacity, visibility; every layer blended with the normal operator).
// Krita's own tile format is not decoded: the merged image (or, failing
// that, the smaller `preview.png`) is used.
// Writing produces a single-layer OpenRaster 0.0.5 file.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/image/common.hpp"
#include "families/image/image_api.hpp"
#include "families/image/image_ops.hpp"
#include "families/image_modern/misc.hpp"
#include "families/image_modern/misc_composite.hpp"
#include "util/bytes.hpp"
#include "util/xml.hpp"
#include "util/zip.hpp"

namespace cvt::image_modern {

namespace {

using image::ImageCodecSpec;

constexpr std::string_view ora_mime = "image/openraster";
constexpr std::string_view kra_mime = "application/x-krita";
constexpr int max_stack_depth = 64;

/// Decode a picture stored inside the package (PNG normally; anything the
/// registry can sniff is accepted).
Result<ir::Frame> decode_entry(util::ZipReader& zip, std::string_view name, const ReadContext& ctx) {
  CVT_TRY_ASSIGN(const Bytes data, zip.read(name));
  const std::string format = image::sniff_format(data).value_or("png");
  CVT_TRY_ASSIGN(ir::Image picture, image::decode(format, data, ctx));
  if (!picture.valid()) return fail(Status::Decode, "the layer '{}' holds no picture", name);
  return std::move(picture.frames.front());
}

struct StackState {
  util::ZipReader& zip;
  const ReadContext& ctx;
  std::uint64_t layers_done = 0;
  bool warned_blend = false;
};

/// Render one <stack> (children listed top-most first) onto `canvas`.
Result<void> render_stack(const util::XmlNode& stack, ir::Frame& canvas, std::int64_t origin_x, std::int64_t origin_y,
                          StackState& state, int depth) {
  if (depth > max_stack_depth) return fail(Status::Limit, "OpenRaster stacks are nested too deeply");
  std::vector<util::XmlNode> children;
  for (const util::XmlNode child : stack.elements()) children.push_back(child);
  CVT_TRY(state.ctx.limits.check_archive_entries(children.size()));
  // Bottom-most first.
  for (auto it = children.rbegin(); it != children.rend(); ++it) {
    const util::XmlNode& node = *it;
    const std::string_view name = node.name();
    if (name != "layer" && name != "stack") continue;
    if (node.attribute("visibility").value() == "hidden") continue;
    const double opacity = node.attribute("opacity") ? node.attribute("opacity").as_double(1.0) : 1.0;
    const std::int64_t x = origin_x + node.attribute("x").as_int(0);
    const std::int64_t y = origin_y + node.attribute("y").as_int(0);
    const std::string_view op = node.attribute("composite-op").value();
    if (!op.empty() && op != "svg:src-over" && !state.warned_blend) {
      state.ctx.warn("OpenRaster blend modes other than normal are composited as normal.");
      state.warned_blend = true;
    }
    if (name == "stack") {
      ir::Frame group = ir::Frame::filled(canvas.width, canvas.height);
      CVT_TRY(render_stack(node, group, x, y, state, depth + 1));
      composite_normal(canvas, group, 0, 0, opacity);
      continue;
    }
    const std::string_view src = node.attribute("src").value();
    if (src.empty() || !state.zip.contains(src)) {
      state.ctx.warn("An OpenRaster layer refers to a missing file; it was skipped.");
      continue;
    }
    CVT_TRY_ASSIGN(const ir::Frame layer, decode_entry(state.zip, src, state.ctx));
    composite_normal(canvas, layer, x, y, opacity);
    ++state.layers_done;
    CVT_TRY(state.ctx.checkpoint(std::min(0.99, static_cast<double>(state.layers_done) / (state.layers_done + 4.0))));
  }
  return {};
}

Result<ir::Image> read_package(ReadContext& ctx, bool krita) {
  CVT_TRY_ASSIGN(util::ZipReader zip, util::ZipReader::open(ctx.input, ctx.limits));
  if (zip.contains("mimetype")) {
    CVT_TRY_ASSIGN(const Bytes mime, zip.read("mimetype"));
    const std::string_view text = as_chars(mime);
    const std::string_view expected = krita ? kra_mime : ora_mime;
    if (text.substr(0, expected.size()) != expected) {
      return fail(Status::Decode, "the package is not {} (mimetype '{}')", krita ? "a Krita document" : "OpenRaster",
                  std::string(text.substr(0, 64)));
    }
  } else if (!zip.contains(krita ? "maindoc.xml" : "stack.xml")) {
    return fail(Status::Decode, "the ZIP package is not {}", krita ? "a Krita document" : "an OpenRaster image");
  }

  ir::Image image;
  if (zip.contains("mergedimage.png")) {
    CVT_TRY_ASSIGN(ir::Frame merged, decode_entry(zip, "mergedimage.png", ctx));
    image.frames.push_back(std::move(merged));
    return image;
  }
  if (krita) {
    if (!zip.contains("preview.png")) {
      return fail(Status::UnsupportedConversion,
                  "this Krita document has no merged image (Krita layer tiles cannot be decoded)");
    }
    ctx.warn("The Krita document has no merged image; its small preview was used instead.");
    CVT_TRY_ASSIGN(ir::Frame preview, decode_entry(zip, "preview.png", ctx));
    image.frames.push_back(std::move(preview));
    return image;
  }

  CVT_TRY_ASSIGN(const Bytes stack_xml, zip.read("stack.xml"));
  CVT_TRY_ASSIGN(const util::XmlDocument doc, util::parse_xml(stack_xml));
  const util::XmlNode root = doc.root();
  if (!root || root.name() != "image") return fail(Status::Decode, "OpenRaster stack.xml has no <image> element");
  const std::int64_t width = root.attribute("w").as_int64(0);
  const std::int64_t height = root.attribute("h").as_int64(0);
  if (width <= 0 || height <= 0 || width > 0xFFFFFFFF || height > 0xFFFFFFFF) {
    return fail(Status::Decode, "OpenRaster image size is invalid");
  }
  CVT_TRY(image::check_size(ctx, static_cast<std::uint64_t>(width), static_cast<std::uint64_t>(height)));
  // A sparse canvas is legitimate, but not one of gigapixels from a few bytes.
  CVT_TRY(image::check_plausible(static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height),
                                 ctx.input.size(), 65536));
  ir::Frame canvas = ir::Frame::filled(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
  const util::XmlNode stack = root.child("stack");
  if (!stack) return fail(Status::Decode, "OpenRaster stack.xml has no root <stack>");
  StackState state{zip, ctx, 0, false};
  CVT_TRY(render_stack(stack, canvas, 0, 0, state, 0));
  image.frames.push_back(std::move(canvas));
  const double dpi_x = root.attribute("xres").as_double(0.0);
  const double dpi_y = root.attribute("yres").as_double(0.0);
  if (dpi_x > 0 && dpi_x < 1e6) image.dpi_x = dpi_x;
  if (dpi_y > 0 && dpi_y < 1e6) image.dpi_y = dpi_y;
  return image;
}

Result<ir::Image> read_ora(ReadContext& ctx) { return read_package(ctx, false); }
Result<ir::Image> read_kra(ReadContext& ctx) { return read_package(ctx, true); }

Result<Bytes> write_ora(const ir::Image& source, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const image::WriteImage prepared, image::prepare_write(source, ctx));
  image::warn_if_animated(*prepared, ctx, "OpenRaster");
  const ir::Frame& frame = prepared->frames.front();
  const int level = options::resolved_compression_level(ctx.options);

  ir::Image layer;
  layer.frames.push_back(frame);
  layer.dpi_x = prepared->dpi_x;
  layer.dpi_y = prepared->dpi_y;
  Options png_options;
  png_options.set(std::string(options::key_compression_level), static_cast<std::int64_t>(level));
  CVT_TRY_ASSIGN(const Bytes layer_png, image::encode("png", layer, png_options, ctx));

  // Thumbnail: at most 256 pixels on the longer side (spec requirement).
  ir::Image thumb;
  const std::uint32_t longest = std::max(frame.width, frame.height);
  if (longest > 256) {
    const auto scaled = [&](std::uint32_t v) {
      return std::max<std::uint32_t>(1, static_cast<std::uint32_t>((std::uint64_t{v} * 256 + longest / 2) / longest));
    };
    CVT_TRY_ASSIGN(ir::Frame small, image::resize(frame, scaled(frame.width), scaled(frame.height)));
    thumb.frames.push_back(std::move(small));
  } else {
    thumb.frames.push_back(frame);
  }
  CVT_TRY_ASSIGN(const Bytes thumb_png, image::encode("png", thumb, png_options, ctx));

  std::string stack = "<?xml version='1.0' encoding='UTF-8'?>\n<image version=\"0.0.5\" w=\"" +
                      std::to_string(frame.width) + "\" h=\"" + std::to_string(frame.height) + "\"";
  if (prepared->dpi_x > 0 && prepared->dpi_y > 0) {
    stack += " xres=\"" + std::to_string(static_cast<int>(prepared->dpi_x + 0.5)) + "\" yres=\"" +
             std::to_string(static_cast<int>(prepared->dpi_y + 0.5)) + "\"";
  }
  stack +=
      ">\n <stack>\n  <layer name=\"Layer\" src=\"data/layer0.png\" x=\"0\" y=\"0\" opacity=\"1.0\" "
      "visibility=\"visible\" composite-op=\"svg:src-over\"/>\n </stack>\n</image>\n";

  util::ZipWriter zip;
  util::ZipEntryOptions stored;
  stored.level = 0;
  util::ZipEntryOptions packed;
  packed.level = level;
  // The PNGs are already compressed: store them unless deflate still helps.
  CVT_TRY(zip.add("mimetype", ora_mime, stored));
  CVT_TRY(zip.add("stack.xml", stack, packed));
  CVT_TRY(zip.add("data/layer0.png", layer_png, packed));
  CVT_TRY(zip.add("Thumbnails/thumbnail.png", thumb_png, packed));
  CVT_TRY(zip.add("mergedimage.png", layer_png, packed));
  return zip.finish();
}

}  // namespace

void register_misc_ora(Registry& registry) {
  FormatInfo ora;
  ora.id = "ora";
  ora.name = "OpenRaster image";
  ora.description = "Layered raster image exchanged by MyPaint, Krita, GIMP and Pinta.";
  ora.category = Category::Image;
  ora.extensions = {"ora"};
  ora.mime = "image/openraster";
  ora.popularity = 12;
  ora.signatures = {Signature::at(30, "'mimetypeimage/openraster'")};
  registry.add_format(std::move(ora));

  FormatInfo kra;
  kra.id = "kra";
  kra.name = "Krita document";
  kra.description = "Krita painting (the flattened image is read).";
  kra.category = Category::Image;
  kra.extensions = {"kra"};
  kra.mime = "application/x-krita";
  kra.popularity = 15;
  kra.signatures = {Signature::at(30, "'mimetypeapplication/x-krita'")};
  registry.add_format(std::move(kra));

  ImageCodecSpec ora_codec;
  ora_codec.format_id = "ora";
  ora_codec.read = image::reader(read_ora);
  ora_codec.write = image::writer(write_ora);
  ora_codec.write_options = image::lossless_writer_options();
  image::add_image_codec(registry, std::move(ora_codec));

  ImageCodecSpec kra_codec;
  kra_codec.format_id = "kra";
  kra_codec.read = image::reader(read_kra);
  image::add_image_codec(registry, std::move(kra_codec));
}

}  // namespace cvt::image_modern
