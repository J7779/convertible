// Convertible core - vector family: how scene readers and writers become codecs.
//
// A vector format implements two functions - bytes -> Scene and Scene ->
// bytes - and calls `register_vector_format`. That registers, for the
// format id:
//
//   Data reader/writer   the scene as a lossless tagged JSON envelope
//                        (scene_json.hpp). Every vector reader and writer
//                        meets here, so svg -> eps -> dxf ... never go
//                        through pixels. Cost read_cost / write_cost.
//   Image reader         the scene rasterised (render.hpp); options width,
//                        height, dpi, background, page. Lossy.
//   Image writer         a raster picture embedded as an image, or traced
//                        into paths (trace.hpp, option `trace`). Formats that
//                        cannot hold pictures always trace.
//   Document reader      a rendering of the drawing plus its text runs.
//
// The Data writer refuses JSON that is not a scene envelope with
// Status::Encode, so data files offered "as svg" fail with a clear message.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "engine/options.hpp"
#include "engine/registry.hpp"
#include "families/vector/scene.hpp"
#include "util/bytes.hpp"

namespace cvt::vector {

/// Bytes (ctx.input) -> scene. Use ctx.options for reader options, ctx.limits,
/// ctx.warn, ctx.checkpoint, ctx.registry (embedded pictures via image_api).
using SceneReader = std::function<Result<Scene>(ReadContext& ctx)>;
/// Scene -> bytes. Honour `strip_metadata` (options::resolved_strip_metadata).
using SceneWriter = std::function<Result<Bytes>(const Scene& scene, WriteContext& ctx)>;

struct VectorFormat {
  FormatInfo info;
  SceneReader read;   ///< may be empty (write-only format)
  SceneWriter write;  ///< may be empty (read-only format)
  std::vector<OptionSpec> read_options;
  std::vector<OptionSpec> write_options;
  /// The writer drops information by design (fills, gradients, text ...).
  bool lossy_write = false;
  /// The reader approximates (e.g. raster operations, unsupported records).
  bool lossy_read = false;
  /// Register the rasterising Image reader (off when the format registers its own).
  bool image_reader = true;
  /// Register an Image writer (embed or trace raster pictures).
  bool image_writer = true;
  /// The format can store raster pictures; when false the Image writer always traces.
  bool can_embed_images = true;
  /// Register the Document reader.
  bool document_reader = true;
  int read_cost = 10;
  int write_cost = 10;
};

void register_vector_format(Registry& registry, VectorFormat format);

/// Reader option keys shared by every vector format's Image reader.
[[nodiscard]] std::vector<OptionSpec> render_read_options();
/// Writer option keys of every vector format's Image writer.
[[nodiscard]] std::vector<OptionSpec> image_write_options(bool can_embed);

/// Remove metadata, editor data and ids that could identify a person or a
/// machine (keeps only what renders). Called by writers when
/// `strip_metadata` is on.
void strip_scene_metadata(Scene& scene);

/// Run a vector format's registered Data reader/writer through the
/// registry (for tests and for formats that embed others).
[[nodiscard]] Result<Scene> read_scene(std::string_view format_id, ByteView bytes, const Context& ctx,
                                       const Options& options = Options{});
[[nodiscard]] Result<Bytes> write_scene(std::string_view format_id, const Scene& scene, const Context& ctx,
                                        const Options& options = Options{});

}  // namespace cvt::vector
