// Convertible core - vector family: scene readers/writers as engine codecs.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/vector/vector_codecs.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <memory>
#include <utility>

#include "engine/standard_options.hpp"
#include "families/image/image_ops.hpp"
#include "families/vector/render.hpp"
#include "families/vector/scene_json.hpp"
#include "families/vector/trace.hpp"
#include "ir/document.hpp"
#include "util/text.hpp"

namespace cvt::vector {

namespace {

const Registry& registry_of(const Context& ctx) { return ctx.registry != nullptr ? *ctx.registry : Registry::instance(); }

Result<std::optional<Color>> parse_background(const std::string& text) {
  const std::string t = util::to_lower(util::trim(text));
  if (t.empty() || t == "transparent" || t == "none") return std::optional<Color>{};
  auto rgb = image::parse_color(t);
  if (!rgb) return fail(Status::InvalidArgument, "option 'background': {}", rgb.error().message);
  return std::optional<Color>{Color::rgb8((*rgb)[0], (*rgb)[1], (*rgb)[2])};
}

Result<RenderOptions> render_options_from(const Options& options) {
  RenderOptions r;
  r.width = static_cast<std::uint32_t>(options.get_int("width", 0, 0, 65535));
  r.height = static_cast<std::uint32_t>(options.get_int("height", 0, 0, 65535));
  r.dpi = options.get_float("dpi", 96.0, 1.0, 9600.0);
  CVT_TRY_ASSIGN(r.background, parse_background(options.get_string("background", "transparent")));
  r.page = static_cast<std::size_t>(options.get_int("page", 1, 1, 100000) - 1);
  return r;
}

void collect_text(const Group& group, std::vector<std::string>& out) {
  if (!group.visible) return;
  for (const Node& node : group.children) {
    if (const auto* t = std::get_if<Text>(&node.value)) {
      const std::string_view trimmed = util::trim(t->content);
      if (!trimmed.empty()) out.emplace_back(trimmed);
    } else if (const auto* g = std::get_if<Group>(&node.value)) {
      collect_text(*g, out);
    }
  }
}

TraceOptions trace_options_from(const Options& options, bool can_embed) {
  TraceOptions t;
  const std::string mode = options.get_string("trace", can_embed ? "off" : "color");
  t.mode = mode == "bw" ? TraceOptions::Mode::BlackWhite
           : mode == "color" ? TraceOptions::Mode::Color
                             : TraceOptions::Mode::Embed;
  if (!can_embed && t.mode == TraceOptions::Mode::Embed) t.mode = TraceOptions::Mode::Color;
  t.colors = static_cast<int>(options.get_int("trace_colors", 8, 2, 64));
  t.threshold = static_cast<int>(options.get_int("trace_threshold", 128, 0, 255));
  t.tolerance = options.get_float("trace_tolerance", 1.0, 0.05, 20.0);
  t.speckle = static_cast<int>(options.get_int("trace_speckle", 4, 0, 100000));
  t.corner_angle = options.get_float("trace_corner_angle", 60.0, 0.0, 180.0);
  return t;
}

}  // namespace

std::vector<OptionSpec> render_read_options() {
  return {
      OptionSpec::integer("width", "Width", 0, 0, 65535,
                          "Width of the rendered picture in pixels; 0 follows the drawing size and resolution."),
      OptionSpec::integer("height", "Height", 0, 0, 65535,
                          "Height of the rendered picture in pixels; 0 follows the drawing size and resolution."),
      OptionSpec::number("dpi", "Resolution (dpi)", 96.0, 1.0, 9600.0,
                         "Pixels per inch when rendering; 96 draws one pixel per CSS pixel."),
      OptionSpec::text("background", "Background colour", "transparent",
                       "Colour behind the drawing: transparent, or six hexadecimal digits (RRGGBB)."),
      OptionSpec::integer("page", "Page", 1, 1, 100000, "Page or artboard to render (1 = first).").advanced(),
  };
}

std::vector<OptionSpec> image_write_options(bool can_embed) {
  std::vector<OptionSpec> specs;
  if (can_embed) {
    specs.push_back(OptionSpec::choice("trace", "Tracing", "off", {"off", "bw", "color"},
                                       "off embeds the picture unchanged; bw and color trace it into vector shapes."));
  } else {
    specs.push_back(OptionSpec::choice("trace", "Tracing", "color", {"bw", "color"},
                                       "Trace the picture into black-and-white or coloured vector shapes."));
  }
  specs.push_back(OptionSpec::integer("trace_colors", "Trace colours", 8, 2, 64,
                                      "Number of colour layers when tracing in colour."));
  specs.push_back(OptionSpec::integer("trace_threshold", "Trace threshold", 128, 0, 255,
                                      "Black-and-white tracing: pixels darker than this become black.")
                      .advanced());
  specs.push_back(OptionSpec::number("trace_tolerance", "Trace tolerance", 1.0, 0.05, 20.0,
                                     "Curve fitting tolerance in pixels; larger gives fewer, smoother curves.")
                      .advanced());
  specs.push_back(OptionSpec::integer("trace_speckle", "Ignore specks", 4, 0, 100000,
                                      "Regions smaller than this many pixels are dropped when tracing.")
                      .advanced());
  specs.push_back(OptionSpec::number("trace_corner_angle", "Corner angle", 60.0, 0.0, 180.0,
                                     "Turns of at least this many degrees stay sharp corners when tracing.")
                      .advanced());
  return specs;
}

void strip_scene_metadata(Scene& scene) { scene.metadata.clear(); }

void register_vector_format(Registry& registry, VectorFormat format) {
  const std::string id = format.info.id;
  registry.add_format(format.info);

  constexpr int max_depth = 256;

  // ---- Data: the lossless scene envelope
  {
    Codec codec;
    codec.format_id = id;
    codec.kind = Kind::Data;
    codec.ir_tag = "vector-scene";  // only vector writers understand the envelope (see Codec::ir_tag)
    codec.read_cost = format.read_cost;
    codec.write_cost = format.write_cost;
    codec.lossy = format.lossy_read || format.lossy_write;
    if (format.read) {
      SceneReader read = format.read;
      codec.read = [read](ReadContext& ctx) -> Result<AnyIR> {
        CVT_TRY_ASSIGN(Scene scene, read(ctx));
        ir::Data data;
        data.value = scene_to_json(scene);
        return AnyIR(std::move(data));
      };
      codec.read_options = format.read_options;
    }
    if (format.write) {
      SceneWriter write = format.write;
      codec.write = [write](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
        const ir::Data& data = value.get<ir::Data>();
        CVT_TRY_ASSIGN(Scene scene, scene_from_json(data.value, ctx.limits.max_pixels, max_depth));
        if (options::resolved_strip_metadata(ctx.options)) strip_scene_metadata(scene);
        return write(scene, ctx);
      };
      codec.write_options = format.write_options;
      codec.write_options.push_back(options::strip_metadata());
    }
    if (codec.read || codec.write) {
      // A lossy reader and a faithful writer are registered separately so
      // that the planner sees each direction's real cost.
      if (format.lossy_read != format.lossy_write && codec.read && codec.write) {
        Codec writer = codec;
        writer.read = nullptr;
        writer.read_options.clear();
        writer.lossy = format.lossy_write;
        codec.write = nullptr;
        codec.write_options.clear();
        codec.lossy = format.lossy_read;
        registry.add_codec(std::move(codec));
        registry.add_codec(std::move(writer));
      } else {
        registry.add_codec(std::move(codec));
      }
    }
  }

  // ---- Image reader: rasterise
  if (format.read && format.image_reader) {
    Codec codec;
    codec.format_id = id;
    codec.kind = Kind::Image;
    codec.lossy = true;
    codec.read_cost = format.read_cost + 20;
    codec.read_options = format.read_options;
    for (OptionSpec& spec : render_read_options()) codec.read_options.push_back(std::move(spec));
    SceneReader read = format.read;
    codec.read = [read](ReadContext& ctx) -> Result<AnyIR> {
      CVT_TRY_ASSIGN(const RenderOptions options, render_options_from(ctx.options));
      CVT_TRY_ASSIGN(Scene scene, read(ctx));
      CVT_TRY_ASSIGN(ir::Frame frame, render_page(scene, options, ctx));
      ir::Image image;
      image.frames.push_back(std::move(frame));
      image.dpi_x = image.dpi_y = options.width == 0 && options.height == 0 ? options.dpi : 0.0;
      if (scene.metadata.contains("title")) image.metadata["Title"] = scene.metadata.at("title");
      return AnyIR(std::move(image));
    };
    registry.add_codec(std::move(codec));
  }

  // ---- Image writer: embed or trace
  if (format.write && format.image_writer) {
    Codec codec;
    codec.format_id = id;
    codec.kind = Kind::Image;
    codec.lossy = !format.can_embed_images || format.lossy_write;
    codec.write_cost = format.write_cost + 30;
    codec.write_options = format.write_options;
    for (OptionSpec& spec : image_write_options(format.can_embed_images)) codec.write_options.push_back(std::move(spec));
    codec.write_options.push_back(options::strip_metadata());
    SceneWriter write = format.write;
    const bool can_embed = format.can_embed_images;
    codec.write = [write, can_embed](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
      const ir::Image& image = value.get<ir::Image>();
      if (!image.valid()) return fail(Status::Encode, "the image has no pixels");
      if (image.animated()) ctx.warn("Only the first frame of the animation was converted.");
      CVT_TRY_ASSIGN(Scene scene, image_to_scene(image, trace_options_from(ctx.options, can_embed), ctx));
      if (options::resolved_strip_metadata(ctx.options)) strip_scene_metadata(scene);
      return write(scene, ctx);
    };
    registry.add_codec(std::move(codec));
  }

  // ---- Document reader: a rendering plus the text runs
  if (format.read && format.document_reader) {
    Codec codec;
    codec.format_id = id;
    codec.kind = Kind::Document;
    codec.lossy = true;
    codec.read_cost = format.read_cost + 30;
    codec.read_options = format.read_options;
    SceneReader read = format.read;
    codec.read = [read](ReadContext& ctx) -> Result<AnyIR> {
      CVT_TRY_ASSIGN(Scene scene, read(ctx));
      ir::Document doc;
      if (scene.metadata.contains("title")) doc.metadata.title = scene.metadata.at("title");
      for (std::size_t p = 0; p < scene.pages.size(); ++p) {
        RenderOptions options;
        options.page = p;
        options.background = Color{1, 1, 1, 1};
        auto frame = render_page(scene, options, ctx);
        if (frame) {
          ir::Image image;
          image.frames.push_back(std::move(*frame));
          image.dpi_x = image.dpi_y = 96.0;
          doc.blocks.push_back(ir::build::image_block(std::move(image), scene.pages[p].name));
        } else if (frame.error().code == Status::Cancelled) {
          return fail(frame.error());
        } else {
          ctx.warn(std::format("A page could not be rendered: {}", frame.error().message));
        }
        std::vector<std::string> texts;
        collect_text(scene.pages[p].root, texts);
        for (std::string& t : texts) doc.blocks.push_back(ir::build::paragraph(std::move(t)));
      }
      return AnyIR(std::move(doc));
    };
    registry.add_codec(std::move(codec));
  }
}

Result<Scene> read_scene(std::string_view format_id, ByteView bytes, const Context& ctx, const Options& options) {
  const Registry& registry = registry_of(ctx);
  for (const Codec* codec : registry.codecs_for(normalize_format_id(format_id))) {
    if (codec->kind != Kind::Data || !codec->read) continue;
    ReadContext read;
    CVT_TRY_ASSIGN(read.options, options.normalized(codec->read_options));
    read.progress = ctx.progress;
    read.limits = ctx.limits;
    read.warnings = ctx.warnings;
    read.registry = &registry;
    read.input = bytes;
    read.format_id = codec->format_id;
    CVT_TRY_ASSIGN(AnyIR value, codec->read(read));
    const ir::Data* data = value.get_if<ir::Data>();
    if (data == nullptr) return fail(Status::Internal, "the '{}' reader returned another kind", format_id);
    return scene_from_json(data->value, ctx.limits.max_pixels);
  }
  return fail(Status::UnsupportedConversion, "no vector reader is available for '{}'", format_id);
}

Result<Bytes> write_scene(std::string_view format_id, const Scene& scene, const Context& ctx, const Options& options) {
  const Registry& registry = registry_of(ctx);
  for (const Codec* codec : registry.codecs_for(normalize_format_id(format_id))) {
    if (codec->kind != Kind::Data || !codec->write) continue;
    WriteContext write;
    CVT_TRY_ASSIGN(write.options, options.normalized(codec->write_options));
    write.progress = ctx.progress;
    write.limits = ctx.limits;
    write.warnings = ctx.warnings;
    write.registry = &registry;
    write.format_id = codec->format_id;
    ir::Data data;
    data.value = scene_to_json(scene);
    const AnyIR value(std::move(data));
    return codec->write(value, write);
  }
  return fail(Status::UnsupportedConversion, "no vector writer is available for '{}'", format_id);
}

}  // namespace cvt::vector
