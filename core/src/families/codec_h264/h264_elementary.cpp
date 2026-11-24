// Convertible core - codec_h264 family: raw H.264 elementary streams
// (Annex B byte streams, ".h264" / ".264" files).
//
// Reading decodes the stream into an ir::Video (frames only: a raw stream
// has no timestamps, so frames are timed from the VUI frame rate, or 25 fps).
// Writing encodes an ir::Video with the H.264 encoder and stores the packets
// as an Annex B stream with the parameter sets repeated before every IDR
// picture, which is what players and muxers expect of such files.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstring>
#include <memory>

#include "codec/colorspace.hpp"
#include "codec/ids.hpp"
#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/codec_h264/h264_bitstream.hpp"
#include "families/codec_h264/h264_codec.hpp"

namespace cvt::h264 {

namespace {

constexpr std::string_view format_id = "h264";

/// Confidence that `data` is an Annex B H.264 stream: its first NAL units
/// must be well-formed H.264 units and an SPS that parses must turn up.
int sniff_h264(std::span<const std::byte> data, std::string_view /*filename*/) {
  if (!starts_with_start_code(data)) return 0;
  const ByteView head = data.first(std::min<std::size_t>(data.size(), 65536));
  const std::vector<ByteView> nals = split_annexb(head);
  if (nals.empty()) return 0;
  bool sps_ok = false;
  bool slice = false;
  std::size_t checked = 0;
  for (const ByteView nal : nals) {
    if (++checked > 64) break;
    const auto header = std::to_integer<unsigned>(nal[0]);
    if ((header & 0x80u) != 0) return 0;  // forbidden_zero_bit (MPEG-1/2 start codes have it set)
    const unsigned type = header & 0x1Fu;
    if (type == 0 || (type >= 21 && type <= 23) || type >= 24) return 0;  // unspecified / reserved
    if (type == nal_sps) {
      if (!parse_sps(nal)) return 0;
      sps_ok = true;
    }
    if (is_vcl(static_cast<std::uint8_t>(type))) {
      if (!sps_ok) return 0;  // a stream starts with its parameter sets
      slice = true;
    }
  }
  if (!sps_ok) return 0;
  return slice ? 85 : 60;
}

/// Shared decoding state behind ir::Video::next_frame.
struct ReadState {
  H264Decoder decoder;
  std::vector<ByteView> units;
  std::size_t next = 0;
  bool flushed = false;
  bool done = false;
  std::uint64_t frames = 0;
  Progress progress;
  Warnings warnings;
  Limits limits;
};

Result<std::optional<codec::VideoFrame>> next_frame(ReadState& state) {
  if (state.done) return std::optional<codec::VideoFrame>{};
  for (;;) {
    CVT_TRY_ASSIGN(std::optional<codec::VideoFrame> frame, state.decoder.receive_frame());
    for (std::string& warning : state.decoder.take_warnings()) state.warnings.add(std::move(warning));
    if (frame) {
      ++state.frames;
      return frame;
    }
    if (state.next < state.units.size()) {
      if (!state.progress.report(static_cast<double>(state.next) / static_cast<double>(state.units.size()))) {
        return fail(Status::Cancelled, "the conversion was cancelled");
      }
      codec::Packet packet;
      const ByteView unit = state.units[state.next++];
      packet.bytes.assign(unit.begin(), unit.end());
      CVT_TRY(state.decoder.send_packet(packet));
      continue;
    }
    if (!state.flushed) {
      state.flushed = true;
      CVT_TRY(state.decoder.flush());
      continue;
    }
    state.done = true;
    if (state.frames == 0) return fail(Status::Decode, "the H.264 stream contains no decodable pictures");
    return std::optional<codec::VideoFrame>{};
  }
}

Result<AnyIR> read_h264(ReadContext& ctx) {
  const std::vector<ByteView> nals = split_annexb(ctx.input);
  std::optional<Sps> sps;
  for (const ByteView nal : nals) {
    if (nal_type(nal) != nal_sps) continue;
    Result<Sps> parsed = parse_sps(nal);
    if (!parsed) continue;
    if (const auto reason = unsupported_reason(*parsed)) return fail(Status::Decode, "{}", *reason);
    sps = *parsed;
    break;
  }
  if (!sps) return fail(Status::Decode, "no H.264 sequence parameter set found");
  CVT_TRY(ctx.limits.check_pixels(sps->width, sps->height));

  auto state = std::make_shared<ReadState>();
  state->units = split_access_units(ctx.input);
  state->progress = ctx.progress;
  state->warnings = ctx.warnings;
  state->limits = ctx.limits;
  codec::CodecParams params;
  params.codec = std::string(codec::id::h264);
  params.timescale = 1000;
  params.width = sps->width;
  params.height = sps->height;
  CVT_TRY(state->decoder.configure(params));

  ir::Video video;
  video.info.width = sps->width;
  video.info.height = sps->height;
  video.info.fps = sps->frame_rate();
  if (video.info.fps.num <= 0) video.info.fps = {25, 1};
  const std::uint64_t pictures = state->units.size();
  if (sps->frame_mbs_only) {  // with field coding, access units are not frames
    video.info.frame_count = pictures;
    video.info.duration_ms = static_cast<std::int64_t>(pictures * 1000 * static_cast<std::uint64_t>(video.info.fps.den) /
                                                       static_cast<std::uint64_t>(video.info.fps.num));
  }
  video.next_frame = [state]() { return next_frame(*state); };
  return AnyIR(std::move(video));
}

/// Copy `frame` (I420) into a frame of even size, dropping a last odd column / row.
codec::VideoFrame crop_to_even(const codec::VideoFrame& frame) {
  const std::uint32_t width = frame.width & ~1u;
  const std::uint32_t height = frame.height & ~1u;
  codec::VideoFrame out = codec::VideoFrame::allocate(ir::PixelFormat::I420, width, height);
  out.color = frame.color;
  out.pts_ms = frame.pts_ms;
  out.duration_ms = frame.duration_ms;
  for (std::size_t p = 0; p < 3; ++p) {
    const std::size_t rows = ir::plane_height(out.format, p, height);
    const std::size_t bytes = ir::plane_row_bytes(out.format, p, width);
    for (std::size_t r = 0; r < rows; ++r) {
      std::memcpy(out.planes[p].data.data() + r * out.planes[p].stride,
                  frame.planes[p].data.data() + r * frame.planes[p].stride, bytes);
    }
  }
  return out;
}

Result<Bytes> write_h264(const AnyIR& value, WriteContext& ctx) {
  const ir::Video& video = value.get<ir::Video>();
  if (!video.next_frame) return fail(Status::Encode, "the video has no picture track to encode");

  H264Encoder encoder;
  bool configured = false;
  bool cropped = false;
  ir::ColorInfo color;
  Bytes out;
  std::vector<ByteView> parameter_sets;
  Bytes avcc;
  std::uint64_t frames = 0;
  const std::uint64_t expected = video.info.frame_count.value_or(0);

  const auto drain = [&]() -> Result<void> {
    for (;;) {
      CVT_TRY_ASSIGN(std::optional<codec::Packet> packet, encoder.receive_packet());
      if (!packet) return {};
      CVT_TRY_ASSIGN(const std::vector<ByteView> nals, split_length_prefixed(packet->bytes, 4));
      if (packet->keyframe) {
        const Bytes sets = to_annexb(parameter_sets);
        out.insert(out.end(), sets.begin(), sets.end());
      }
      const Bytes data = to_annexb(nals);
      out.insert(out.end(), data.begin(), data.end());
      CVT_TRY(ctx.limits.check_output(out.size()));
    }
  };

  for (;;) {
    CVT_TRY_ASSIGN(std::optional<codec::VideoFrame> next, video.next_frame());
    if (!next) break;
    codec::VideoFrame frame = std::move(*next);
    if (!configured) {
      if (frame.width < 2 || frame.height < 2) return fail(Status::Encode, "the video frames are too small for H.264");
      CVT_TRY(ctx.limits.check_pixels(frame.width, frame.height));
      codec::VideoEncodeParams params;
      params.width = frame.width & ~1u;
      params.height = frame.height & ~1u;
      if (params.width < 16 || params.height < 16) {
        return fail(Status::Encode, "H.264 needs pictures of at least 16x16 pixels (got {}x{})", frame.width,
                    frame.height);
      }
      params.fps = video.info.fps;
      params.quality = options::resolved_quality(ctx.options);
      params.bitrate_kbps = options::resolved_video_bitrate_kbps(ctx.options);
      const std::int64_t keyint = ctx.options.get_int("keyframe_interval", 0);
      params.keyframe_interval = static_cast<std::uint32_t>(std::clamp<std::int64_t>(keyint, 0, 100000));
      switch (options::resolved_preset(ctx.options)) {
        case options::Preset::High: params.preset = codec::EncodePreset::Balanced; break;
        case options::Preset::Balanced: params.preset = codec::EncodePreset::Balanced; break;
        case options::Preset::Small: params.preset = codec::EncodePreset::Small; break;
      }
      const std::string speed = ctx.options.get_string("speed", "balanced");
      if (speed == "fast") params.preset = codec::EncodePreset::Fast;
      if (speed == "slow") params.preset = codec::EncodePreset::Small;
      color = frame.format == ir::PixelFormat::RGBA8 ? ir::ColorInfo{} : frame.color;
      if (frame.format == ir::PixelFormat::RGBA8) {
        color.matrix = params.height >= 720 ? ir::ColorMatrix::BT709 : ir::ColorMatrix::BT601;
      }
      params.color = color;
      params.timescale = 90000;
      CVT_TRY(encoder.configure(params));
      avcc = encoder.extradata();
      configured = true;
    }
    if (frame.format != ir::PixelFormat::I420) {
      CVT_TRY_ASSIGN(frame, codec::convert_frame(frame, ir::PixelFormat::I420, color));
    }
    if ((frame.width & 1) != 0 || (frame.height & 1) != 0) {
      if (!cropped) ctx.warn("H.264 needs an even picture size; the last odd row or column was dropped.");
      cropped = true;
      frame = crop_to_even(frame);
    }
    CVT_TRY(encoder.send_frame(frame));
    ++frames;
    if (parameter_sets.empty()) {
      // The avcC record holds the parameter sets; keep views for the keyframes.
      util::ByteReader reader(avcc);
      reader.skip(5);
      const std::size_t sps_count = reader.u8() & 0x1Fu;
      for (std::size_t i = 0; i < sps_count; ++i) parameter_sets.push_back(reader.bytes(reader.u16be()));
      const std::size_t pps_count = reader.u8();
      for (std::size_t i = 0; i < pps_count; ++i) parameter_sets.push_back(reader.bytes(reader.u16be()));
    }
    CVT_TRY(drain());
    if (expected > 0) {
      CVT_TRY(ctx.checkpoint(std::min(1.0, static_cast<double>(frames) / static_cast<double>(expected)), "Encoding"));
    } else if ((frames & 15) == 0) {
      CVT_TRY(ctx.checkpoint(0.5, "Encoding"));
    }
  }
  if (!configured) return fail(Status::Encode, "the video has no frames");
  CVT_TRY(encoder.flush());
  CVT_TRY(drain());
  return out;
}

}  // namespace

void register_elementary_stream(Registry& registry) {
  FormatInfo info;
  info.id = std::string(format_id);
  info.name = "H.264 video stream";
  info.description = "Raw H.264/AVC elementary stream (Annex B), as written by encoders and camera tools.";
  info.category = Category::Video;
  info.extensions = {"h264", "264", "avc", "jsv"};
  info.mime = "video/h264";
  info.popularity = 15;
  info.sniff = sniff_h264;
  registry.add_format(std::move(info));

  Codec reader;
  reader.format_id = std::string(format_id);
  reader.kind = Kind::Video;
  reader.read = read_h264;
  reader.read_cost = 20;
  registry.add_codec(std::move(reader));

  Codec writer;
  writer.format_id = std::string(format_id);
  writer.kind = Kind::Video;
  writer.write = write_h264;
  writer.write_cost = 30;
  writer.lossy = true;
  writer.write_options = {
      options::quality(),
      options::preset(),
      options::video_bitrate_kbps(),
      OptionSpec::integer("keyframe_interval", "Keyframe interval", 0, 0, 100000,
                          "Maximum number of frames between keyframes; 0 chooses about ten seconds.")
          .advanced(),
      OptionSpec::choice("speed", "Encoding speed", "balanced", {"fast", "balanced", "slow"},
                         "Fast skips B-frames and searches less; slow searches more for smaller files.")
          .advanced(),
      options::strip_metadata(),
  };
  registry.add_codec(std::move(writer));
}

}  // namespace cvt::h264
