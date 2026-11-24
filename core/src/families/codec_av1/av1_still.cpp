// Convertible core - codec_av1 family: single-picture decode and encode
// (av1_api.hpp).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cstdint>
#include <optional>
#include <vector>

#include "codec/colorspace.hpp"
#include "families/codec_av1/av1_api.hpp"
#include "families/codec_av1/av1_codec.hpp"
#include "families/codec_av1/libaom_glue.hpp"
#include "families/codec_av1/obu.hpp"

namespace cvt::av1 {

namespace {

ir::ColorInfo conversion_color(const PictureColor& color) {
  ir::ColorInfo info;
  switch (color.matrix_coefficients) {
    case 1: info.matrix = ir::ColorMatrix::BT709; break;
    case 9:
    case 10: info.matrix = ir::ColorMatrix::BT2020; break;
    default: info.matrix = ir::ColorMatrix::BT601; break;
  }
  info.range = color.full_range ? ir::ColorRange::Full : ir::ColorRange::Limited;
  info.siting = color.chroma_sample_position == 2 ? ir::ChromaSiting::TopLeft : ir::ChromaSiting::Left;
  return info;
}

/// RGBA8 -> I444 with planes G, B, R (identity matrix, full range).
ir::VideoFrame pack_gbr(const ir::VideoFrame& rgba) {
  ir::VideoFrame out = ir::VideoFrame::allocate(ir::PixelFormat::I444, rgba.width, rgba.height);
  for (std::uint32_t y = 0; y < rgba.height; ++y) {
    const std::uint8_t* src = rgba.planes[0].data.data() + rgba.planes[0].stride * y;
    std::uint8_t* g = out.planes[0].data.data() + out.planes[0].stride * y;
    std::uint8_t* b = out.planes[1].data.data() + out.planes[1].stride * y;
    std::uint8_t* r = out.planes[2].data.data() + out.planes[2].stride * y;
    for (std::uint32_t x = 0; x < rgba.width; ++x) {
      r[x] = src[4 * x];
      g[x] = src[4 * x + 1];
      b[x] = src[4 * x + 2];
    }
  }
  return out;
}

}  // namespace

Bytes with_temporal_delimiter(ByteView temporal_unit) {
  Bytes out;
  out.reserve(temporal_unit.size() + 2);
  out.push_back(std::byte{0x12});  // OBU_TEMPORAL_DELIMITER with obu_has_size_field
  out.push_back(std::byte{0x00});
  out.insert(out.end(), temporal_unit.begin(), temporal_unit.end());
  return out;
}

Result<Picture> decode_still(ByteView data, ByteView av1c, const StillDecodeOptions& options) {
  if (data.empty()) return fail(Status::Decode, "no AV1 data");
  Bytes converted;
  if (!is_section5(data)) {
    if (is_annexb(data)) {
      CVT_TRY_ASSIGN(converted, annexb_to_section5(data));
      data = converted;
    } else if (!is_section5(data, true)) {
      return fail(Status::Decode, "the data is not an AV1 bitstream");
    }
  }
  Bytes config;
  if (!av1c.empty()) {
    CVT_TRY_ASSIGN(const Av1Config parsed, parse_av1c(av1c));
    config = parsed.config_obus;
  }
  const auto check = [&](ByteView payload) -> Result<void> {
    CVT_TRY_ASSIGN(const SequenceHeader header, parse_sequence_header(payload));
    if (header.max_frame_width > 65536 || header.max_frame_height > 65536 ||
        std::uint64_t{header.max_frame_width} * header.max_frame_height > options.max_pixels) {
      return fail(Status::Limit, "the AV1 picture ({}x{}) exceeds the size limit", header.max_frame_width,
                  header.max_frame_height);
    }
    return {};
  };

  CVT_TRY_ASSIGN(const std::vector<Obu> obus, split_section5(data));
  bool has_sequence_header = false;
  for (const Obu& obu : obus) {
    if (obu.type != ObuType::SequenceHeader) continue;
    CVT_TRY(check(obu.payload));
    has_sequence_header = true;
  }
  Bytes stream;
  if (!has_sequence_header) {
    if (config.empty()) return fail(Status::Decode, "the AV1 data has no sequence header");
    CVT_TRY_ASSIGN(const std::vector<Obu> config_obus, split_section5(config));
    bool found = false;
    for (const Obu& obu : config_obus) {
      if (obu.type != ObuType::SequenceHeader) continue;
      CVT_TRY(check(obu.payload));
      append_obu(stream, obu);
      found = true;
    }
    if (!found) return fail(Status::Decode, "the AV1 data has no sequence header");
    for (const Obu& obu : obus) {
      if (obu.type != ObuType::TemporalDelimiter) append_obu(stream, obu);
    }
    data = stream;
  }

  detail::DecoderCore core;
  CVT_TRY(core.open(options.apply_film_grain));
  CVT_TRY_ASSIGN(const std::vector<ByteView> units, split_temporal_units(data));
  for (const ByteView unit : units) {
    CVT_TRY(core.decode(unit));
    if (std::optional<Picture> picture = core.next_picture()) return std::move(*picture);
  }
  CVT_TRY(core.flush());
  if (std::optional<Picture> picture = core.next_picture()) return std::move(*picture);
  return fail(Status::Decode, "the AV1 data contains no shown picture");
}

Result<EncodedStill> encode_still(const ir::VideoFrame& input, const StillEncodeOptions& options) {
  if (!input.valid()) return fail(Status::InvalidArgument, "invalid picture for AV1 encoding");
  if (input.width > 65536 || input.height > 65536) {
    return fail(Status::InvalidArgument, "AV1 cannot encode a {}x{} picture", input.width, input.height);
  }
  const ir::ColorInfo color = conversion_color(options.color);
  ir::VideoFrame converted;
  const ir::VideoFrame* frame = &input;
  switch (input.format) {
    case ir::PixelFormat::RGBA8: {
      if (options.color.matrix_coefficients == 0 && !options.monochrome) {
        converted = pack_gbr(input);
      } else {
        const ir::PixelFormat target = options.lossless ? ir::PixelFormat::I444 : ir::PixelFormat::I420;
        CVT_TRY_ASSIGN(converted, codec::from_rgba(input, target, color));
      }
      frame = &converted;
      break;
    }
    case ir::PixelFormat::NV12: {
      CVT_TRY_ASSIGN(converted, codec::convert_frame(input, ir::PixelFormat::I420, input.color));
      frame = &converted;
      break;
    }
    case ir::PixelFormat::P010: {
      CVT_TRY_ASSIGN(converted, codec::convert_frame(input, ir::PixelFormat::I420P10, input.color));
      frame = &converted;
      break;
    }
    default: break;
  }

  const SpeedChoice speed = choose_still_speed(options.preset, std::uint64_t{frame->width} * frame->height);
  detail::EncoderSettings s;
  s.width = frame->width;
  s.height = frame->height;
  s.format = frame->format;
  s.monochrome = options.monochrome;
  s.usage = speed.usage;
  s.cpu_used = speed.cpu_used;
  s.lag_in_frames = 0;
  s.cq_level = quality_to_cq_level(options.quality);
  s.lossless = options.lossless;
  s.still_picture = true;
  s.color = options.color;
  if (options.monochrome) s.color.chroma_sample_position = 0;

  detail::EncoderCore core;
  CVT_TRY(core.open(s));
  CVT_TRY(core.encode(*frame, 0, 1));
  for (int guard = 0; guard < 1000; ++guard) {
    CVT_TRY_ASSIGN(const bool more, core.flush_step());
    if (!more) break;
  }
  Bytes all;
  while (std::optional<detail::EncodedUnit> unit = core.next_unit()) {
    all.insert(all.end(), unit->data.begin(), unit->data.end());
  }
  if (all.empty()) return fail(Status::Encode, "the AV1 encoder produced no data");
  EncodedStill still;
  CVT_TRY_ASSIGN(still.obus, strip_temporal_delimiters(all));
  CVT_TRY_ASSIGN(const std::optional<Bytes> header, find_sequence_header_obu(still.obus));
  if (!header) return fail(Status::Encode, "the AV1 encoder produced no sequence header");
  CVT_TRY_ASSIGN(still.av1c, make_av1c(*header));
  CVT_TRY_ASSIGN(const std::vector<Obu> obus, split_section5(*header));
  CVT_TRY_ASSIGN(still.sequence, parse_sequence_header(obus.at(0).payload));
  return still;
}

}  // namespace cvt::av1
