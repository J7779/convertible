// Convertible core - video family: bridges to and from other kinds, and the
// audio-file targets of videos.
//
//   Video -> Audio   decode the audio track into PCM (any audio writer).
//   Video -> Image   an animation (for GIF/APNG/WebP writers), a single poster
//                    frame or a contact sheet of evenly spaced thumbnails.
//   Image -> Video   an animation, or a still shown for a while, as frames.
//   mp3/aac/flac/wav/ogg/opus written from a Video: the audio track is copied
//                    unchanged when it already has the right codec, encoded
//                    through codec::Registry otherwise, and as a last resort
//                    decoded and handed to the audio family's own writer.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <memory>

#include "codec/colorspace.hpp"
#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/video/mux.hpp"
#include "families/video/pipeline.hpp"
#include "families/video/policy.hpp"
#include "families/video/video.hpp"

namespace cvt::video {

namespace {

// Size of extracted pictures: explicit width/height (one alone keeps the aspect ratio), else the image rules.
options::Size frame_size(const Options& o, options::Size source) {
  const auto w = static_cast<std::uint32_t>(o.get_int("width", 0, 0, 65536));
  const auto h = static_cast<std::uint32_t>(o.get_int("height", 0, 0, 65536));
  if (w == 0 && h == 0) return options::resolved_image_size(o, source);
  if (w != 0 && h != 0) return {w, h};
  if (w != 0) {
    return {w, std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::llround(static_cast<double>(w) * source.height / source.width)))};
  }
  return {std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::llround(static_cast<double>(h) * source.width / source.height))), h};
}

// --------------------------------------------------------------------------- Video -> Audio

Result<ir::Audio> decode_audio(const ir::Video& video, const Context& ctx) {
  if (!video.next_audio) return fail(Status::Decode, "the video has no audio track");
  const std::uint32_t rate = video.info.audio_sample_rate;
  const std::uint16_t channels = video.info.audio_channels;
  if (rate == 0 || channels == 0) return fail(Status::Decode, "the audio track does not state its format");
  ir::Audio audio;
  audio.sample_rate = rate;
  audio.channels = channels;
  if (!options::resolved_strip_metadata(ctx.options)) audio.tags = video.info.metadata;
  std::int64_t origin = -1;
  const double total = video.info.duration_ms > 0 ? static_cast<double>(video.info.duration_ms) : 0.0;
  while (true) {
    CVT_TRY_ASSIGN(auto chunk, video.next_audio());
    if (!chunk) break;
    if (chunk->samples.size() % channels != 0) return fail(Status::Decode, "the audio decoder returned a partial sample frame");
    std::span<const float> samples = chunk->samples;
    // Drop decoder priming that sits before time zero, keep gaps silent.
    std::int64_t start = chunk->pts_ms * static_cast<std::int64_t>(rate) / 1000;
    if (origin < 0) origin = std::max<std::int64_t>(0, start);
    start -= origin;
    const auto have = static_cast<std::int64_t>(audio.frame_count());
    if (start < have - static_cast<std::int64_t>(rate / 50)) {
      const std::int64_t drop = std::min<std::int64_t>(have - start, static_cast<std::int64_t>(samples.size() / channels));
      samples = samples.subspan(static_cast<std::size_t>(drop) * channels);
    } else if (start > have + static_cast<std::int64_t>(rate / 50)) {
      audio.samples.resize(audio.samples.size() + static_cast<std::size_t>(start - have) * channels, 0.0f);
    }
    const std::uint64_t bytes = (audio.samples.size() + samples.size()) * sizeof(float);
    CVT_TRY(ctx.limits.check_decompressed(bytes));
    audio.samples.insert(audio.samples.end(), samples.begin(), samples.end());
    if (total > 0) CVT_TRY(ctx.checkpoint(static_cast<double>(chunk->pts_ms) / total, "Decoding audio"));
  }
  if (audio.samples.empty()) return fail(Status::Decode, "the audio track holds no samples");
  return audio;
}

// --------------------------------------------------------------------------- Video -> Image

ir::Frame to_image_frame(const ir::VideoFrame& rgba) {
  ir::Frame frame;
  frame.width = rgba.width;
  frame.height = rgba.height;
  frame.rgba.resize(static_cast<std::size_t>(rgba.width) * rgba.height * 4);
  for (std::uint32_t y = 0; y < rgba.height; ++y) {
    std::copy_n(rgba.planes[0].data.data() + std::size_t{y} * rgba.planes[0].stride, std::size_t{rgba.width} * 4,
                frame.rgba.data() + std::size_t{y} * rgba.width * 4);
  }
  return frame;
}

Result<ir::VideoFrame> prepare(const ir::VideoFrame& in, int rotation, std::uint32_t width, std::uint32_t height) {
  ir::VideoFrame f = in;
  if (rotation != 0) {
    CVT_TRY_ASSIGN(f, rotate_frame(f, rotation));
  }
  if (f.width != width || f.height != height) {
    CVT_TRY_ASSIGN(f, scale_frame(f, width, height));
  }
  if (f.format != ir::PixelFormat::RGBA8) {
    CVT_TRY_ASSIGN(f, codec::to_rgba(f));
  }
  return f;
}

Result<AnyIR> video_to_image(AnyIR value, Context& ctx) {
  const ir::Video video = std::move(value).get<ir::Video>();
  if (!video.next_frame) return fail(Status::Decode, "the file has no video frames");
  const Options& o = ctx.options;
  const std::string mode = o.get_enum("image_mode", "animation", {"animation", "poster", "contact_sheet"});
  const int rotation = video.info.rotation;
  const double total_ms = static_cast<double>(video.info.duration_ms);
  ir::Image image;
  CVT_TRY_ASSIGN(auto first, video.next_frame());
  if (!first) return fail(Status::Decode, "the video has no frames");
  std::uint32_t sw = first->width;
  std::uint32_t sh = first->height;
  if (rotation == 90 || rotation == 270) std::swap(sw, sh);
  const options::Size size = frame_size(o, {sw, sh});
  if (mode == "poster") {
    const double at_ms = o.get_float("poster_time", 0.0, 0.0, 1e7) * 1000.0;
    std::optional<ir::VideoFrame> chosen = std::move(first);
    const std::int64_t origin = chosen->pts_ms;
    while (static_cast<double>(chosen->pts_ms - origin) + static_cast<double>(chosen->duration_ms) <= at_ms) {
      CVT_TRY_ASSIGN(auto next, video.next_frame());
      if (!next) break;
      chosen = std::move(next);
      CVT_TRY(ctx.checkpoint(total_ms > 0 ? static_cast<double>(chosen->pts_ms) / std::max(1.0, at_ms) : 0.0, "Seeking"));
    }
    CVT_TRY(ctx.limits.check_pixels(size.width, size.height));
    CVT_TRY_ASSIGN(const ir::VideoFrame rgba, prepare(*chosen, rotation, size.width, size.height));
    image.frames.push_back(to_image_frame(rgba));
    return image;
  }
  if (mode == "contact_sheet") {
    const auto columns = static_cast<std::uint32_t>(o.get_int("sheet_columns", 4, 1, 16));
    const auto rows = static_cast<std::uint32_t>(o.get_int("sheet_rows", 4, 1, 16));
    const std::uint32_t cells = columns * rows;
    // Thumbnails at most 320 wide unless the size options ask otherwise.
    options::Size cell = size;
    if (!o.was_set("width") && !o.was_set("max_width") && !o.was_set("scale") && cell.width > 320) {
      cell.height = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::llround(320.0 * cell.height / cell.width)));
      cell.width = 320;
    }
    const std::uint32_t gap = 4;
    const std::uint64_t sheet_w = std::uint64_t{columns} * cell.width + (columns + 1) * gap;
    const std::uint64_t sheet_h = std::uint64_t{rows} * cell.height + (rows + 1) * gap;
    CVT_TRY(ctx.limits.check_pixels(sheet_w, sheet_h));
    ir::Frame sheet = ir::Frame::filled(static_cast<std::uint32_t>(sheet_w), static_cast<std::uint32_t>(sheet_h), {0, 0, 0, 255});
    const std::int64_t origin = first->pts_ms;
    const double span = total_ms > 0 ? total_ms : static_cast<double>(video.info.frame_count.value_or(cells)) * 40.0;
    std::optional<ir::VideoFrame> current = std::move(first);
    std::uint32_t placed = 0;
    while (current && placed < cells) {
      const double target = span * (placed + 0.5) / cells;
      CVT_TRY_ASSIGN(auto next, video.next_frame());
      const bool take = !next || static_cast<double>(next->pts_ms - origin) > target;
      if (take) {
        CVT_TRY_ASSIGN(const ir::VideoFrame rgba, prepare(*current, rotation, cell.width, cell.height));
        const std::uint32_t cx = gap + (placed % columns) * (cell.width + gap);
        const std::uint32_t cy = gap + (placed / columns) * (cell.height + gap);
        for (std::uint32_t y = 0; y < cell.height; ++y) {
          std::copy_n(rgba.planes[0].data.data() + std::size_t{y} * rgba.planes[0].stride, std::size_t{cell.width} * 4,
                      sheet.rgba.data() + sheet.offset(cx, cy + y));
        }
        ++placed;
        CVT_TRY(ctx.checkpoint(static_cast<double>(placed) / cells, "Thumbnails"));
        if (!next) break;
      }
      current = std::move(next);
    }
    image.frames.push_back(std::move(sheet));
    return image;
  }
  // Animation: frames resampled to `fps` (default 10, 0 = source timing), at most `max_frames`.
  const double fps = o.get_float("fps", 10.0, 0.0, 120.0);
  const auto max_frames = static_cast<std::size_t>(o.get_int("max_frames", 300, 1, 10000));
  CVT_TRY(ctx.limits.check_pixels(size.width, size.height, max_frames));
  const std::int64_t origin = first->pts_ms;
  std::optional<ir::VideoFrame> current = std::move(first);
  double next_due = 0.0;
  std::uint64_t pixels = 0;
  while (current && image.frames.size() < max_frames) {
    CVT_TRY_ASSIGN(auto next, video.next_frame());
    const double t = static_cast<double>(current->pts_ms - origin);
    const double end = next ? static_cast<double>(next->pts_ms - origin) : t + static_cast<double>(std::max<std::int64_t>(current->duration_ms, 40));
    if (fps <= 0) {
      CVT_TRY_ASSIGN(const ir::VideoFrame rgba, prepare(*current, rotation, size.width, size.height));
      ir::Frame frame = to_image_frame(rgba);
      frame.delay_ms = static_cast<std::uint32_t>(std::max(10.0, end - t));
      image.frames.push_back(std::move(frame));
    } else {
      const double tick = 1000.0 / fps;
      while (next_due < end && image.frames.size() < max_frames) {
        if (next_due >= t - tick / 2) {
          CVT_TRY_ASSIGN(const ir::VideoFrame rgba, prepare(*current, rotation, size.width, size.height));
          ir::Frame frame = to_image_frame(rgba);
          frame.delay_ms = static_cast<std::uint32_t>(std::llround(tick));
          image.frames.push_back(std::move(frame));
        }
        next_due += tick;
      }
    }
    pixels += std::uint64_t{size.width} * size.height;
    CVT_TRY(ctx.limits.check_pixels(pixels, 1));
    if (total_ms > 0) CVT_TRY(ctx.checkpoint(t / total_ms, "Extracting frames"));
    current = std::move(next);
  }
  if (image.frames.empty()) return fail(Status::Decode, "the video has no frames");
  image.loop_count = 0;
  return image;
}

// --------------------------------------------------------------------------- Image -> Video

Result<AnyIR> image_to_video(AnyIR value, Context& ctx) {
  auto image = std::make_shared<ir::Image>(std::move(value).get<ir::Image>());
  if (!image->valid()) return fail(Status::Decode, "the image has no pixels");
  const Options& o = ctx.options;
  const double still_seconds = o.get_float("still_duration", 3.0, 0.1, 3600.0);
  const double fps_opt = o.get_float("fps", 0.0, 0.0, 120.0);
  ir::Video video;
  video.info.width = image->width();
  video.info.height = image->height();
  const bool still = !image->animated();
  const double fps = fps_opt > 0 ? fps_opt : (still ? 25.0 : 0.0);
  std::int64_t total_ms = 0;
  if (still) {
    total_ms = static_cast<std::int64_t>(std::llround(still_seconds * 1000.0));
  } else {
    for (const ir::Frame& f : image->frames) total_ms += f.delay_ms == 0 ? 100 : f.delay_ms;
  }
  video.info.duration_ms = total_ms;
  if (fps > 0) {
    video.info.fps = ir::Rational{static_cast<std::int64_t>(std::llround(fps * 1000)), 1000};
    video.info.frame_count = static_cast<std::uint64_t>(std::llround(static_cast<double>(total_ms) * fps / 1000.0));
  } else {
    video.info.frame_count = image->frames.size();
  }
  struct State {
    std::size_t index = 0;
    std::int64_t frame_start = 0;
    std::int64_t emitted = 0;
  };
  auto state = std::make_shared<State>();
  video.next_frame = [image, state, fps, total_ms, still]() -> Result<std::optional<ir::VideoFrame>> {
    // Which source frame is visible at time t?
    auto visible = [&](std::int64_t t) -> std::size_t {
      if (still) return 0;
      std::int64_t acc = 0;
      for (std::size_t i = 0; i < image->frames.size(); ++i) {
        acc += image->frames[i].delay_ms == 0 ? 100 : image->frames[i].delay_ms;
        if (t < acc) return i;
      }
      return image->frames.size() - 1;
    };
    std::int64_t t = 0;
    std::int64_t duration = 0;
    std::size_t index = 0;
    if (fps > 0) {
      t = static_cast<std::int64_t>(std::llround(static_cast<double>(state->emitted) * 1000.0 / fps));
      if (t >= total_ms) return std::optional<ir::VideoFrame>{};
      duration = static_cast<std::int64_t>(std::llround(static_cast<double>(state->emitted + 1) * 1000.0 / fps)) - t;
      index = visible(t);
    } else {
      if (state->index >= image->frames.size()) return std::optional<ir::VideoFrame>{};
      index = state->index++;
      t = state->frame_start;
      duration = image->frames[index].delay_ms == 0 ? 100 : image->frames[index].delay_ms;
      state->frame_start += duration;
    }
    ++state->emitted;
    const ir::Frame& src = image->frames[index];
    ir::VideoFrame frame = ir::VideoFrame::allocate(ir::PixelFormat::RGBA8, src.width, src.height);
    // Video has no transparency: blend onto black.
    for (std::size_t i = 0; i < src.rgba.size(); i += 4) {
      const std::uint32_t a = src.rgba[i + 3];
      for (std::size_t c = 0; c < 3; ++c) frame.planes[0].data[i + c] = static_cast<std::uint8_t>((src.rgba[i + c] * a + 127) / 255);
      frame.planes[0].data[i + 3] = 255;
    }
    frame.pts_ms = t;
    frame.duration_ms = duration;
    return std::optional<ir::VideoFrame>(std::move(frame));
  };
  (void)ctx;
  return AnyIR(std::move(video));
}

// --------------------------------------------------------------------------- audio-file targets

Result<Bytes> write_audio_target(const AnyIR& value, WriteContext& ctx) {
  const ir::Video& video = value.get<ir::Video>();
  const ContainerPolicy* policy = policy_for(ctx.format_id);
  if (policy == nullptr) return fail(Status::Internal, "no policy for '{}'", ctx.format_id);
  // Direct copy or encode through codec::Registry when possible.
  bool copyable = false;
  for (const ir::Track& t : video.tracks) {
    if (t.type == ir::TrackType::Audio && container_accepts(ctx.format_id, ir::TrackType::Audio, t.codec)) copyable = true;
  }
  bool encodable = false;
  for (const std::string_view c : policy->audio_preferred) {
    if (ctx.registry->media().has_audio_encoder(c)) encodable = true;
  }
  const bool forced = ctx.options.was_set("sample_rate") || ctx.options.was_set("channels") ||
                      ctx.options.was_set("audio_bitrate_kbps") || ctx.options.was_set("quality") ||
                      ctx.options.was_set("preset") || ctx.options.was_set("start") || ctx.options.was_set("duration");
  if ((copyable && !forced) || encodable) return write_container(video, ctx, ctx.format_id);
  // Fall back to the audio family's writer for this format.
  const Codec* writer = nullptr;
  for (const Codec* c : ctx.registry->codecs_for(ctx.format_id)) {
    if (c->kind == Kind::Audio && c->write) writer = c;
  }
  if (writer == nullptr) return write_container(video, ctx, ctx.format_id);  // reports the precise error
  CVT_TRY_ASSIGN(ir::Audio audio, decode_audio(video, ctx));
  WriteContext inner;
  inner.progress = ctx.progress;
  inner.limits = ctx.limits;
  inner.warnings = ctx.warnings;
  inner.registry = ctx.registry;
  inner.format_id = ctx.format_id;
  inner.source_filename = ctx.source_filename;
  inner.source_format_id = ctx.source_format_id;
  CVT_TRY_ASSIGN(inner.options, ctx.options.normalized(writer->write_options));
  return writer->write(AnyIR(std::move(audio)), inner);
}

}  // namespace

void register_bridges(Registry& registry) {
  Bridge to_audio;
  to_audio.from = Kind::Video;
  to_audio.to = Kind::Audio;
  to_audio.cost = 15;
  to_audio.lossy = false;
  to_audio.label = "decode the audio track of a video";
  to_audio.options = {options::strip_metadata()};
  to_audio.fn = [](AnyIR value, Context& ctx) -> Result<AnyIR> {
    const ir::Video video = std::move(value).get<ir::Video>();
    CVT_TRY_ASSIGN(ir::Audio audio, decode_audio(video, ctx));
    return AnyIR(std::move(audio));
  };
  registry.add_bridge(std::move(to_audio));

  Bridge to_image;
  to_image.from = Kind::Video;
  to_image.to = Kind::Image;
  to_image.cost = 20;
  to_image.lossy = true;
  to_image.label = "extract frames of a video";
  to_image.options = {
      OptionSpec::choice("image_mode", "Frames", "animation", {"animation", "poster", "contact_sheet"},
                         "An animation of the video, one poster frame, or a contact sheet of thumbnails."),
      options::fps(),
      OptionSpec::integer("max_frames", "Maximum frames", 300, 1, 10000, "Animation length limit in frames.").advanced(),
      OptionSpec::number("poster_time", "Poster time", 0, 0, 1e7, "Time of the poster frame in seconds."),
      OptionSpec::integer("sheet_columns", "Sheet columns", 4, 1, 16, "Thumbnails per row of a contact sheet.").advanced(),
      OptionSpec::integer("sheet_rows", "Sheet rows", 4, 1, 16, "Rows of a contact sheet.").advanced(),
      options::width(),
      options::height(),
      options::scale(),
      options::max_width(),
      options::max_height(),
  };
  to_image.fn = video_to_image;
  registry.add_bridge(std::move(to_image));

  Bridge from_image;
  from_image.from = Kind::Image;
  from_image.to = Kind::Video;
  from_image.cost = 30;
  from_image.lossy = false;
  from_image.label = "turn an image or animation into video frames";
  from_image.options = {
      options::fps(),
      OptionSpec::number("still_duration", "Still duration", 3, 0.1, 3600, "Seconds a still picture is shown."),
  };
  from_image.fn = image_to_video;
  registry.add_bridge(std::move(from_image));

  // Audio files written from a video: copy, encode, or decode + audio writer.
  for (const char* id : {"mp3", "aac", "flac", "wav", "ogg", "opus"}) {
    Codec writer;
    writer.format_id = id;
    writer.kind = Kind::Video;
    writer.lossy = true;
    writer.write_cost = 10;
    writer.write_options = container_write_options(id);
    writer.write = write_audio_target;
    registry.add_codec(std::move(writer));
  }
}

}  // namespace cvt::video
