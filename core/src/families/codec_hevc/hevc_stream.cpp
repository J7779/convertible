// Convertible core - codec_hevc family: raw H.265/HEVC elementary streams
// (Annex-B byte streams, .h265 / .hevc / .265) read as and written from
// ir::Video.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "codec/codec.hpp"
#include "codec/ids.hpp"
#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/codec_hevc/hevc_api.hpp"
#include "families/codec_hevc/hevc_internal.hpp"

namespace cvt::hevc {

namespace {

constexpr std::uint32_t track_timescale = 90000;
constexpr std::string_view key_keyframe_interval = "keyframe_interval";
constexpr std::string_view key_encoder_speed = "encoder_speed";

/// One picture of the stream, in decode order.
struct AccessUnit {
  std::vector<ByteView> nals;
  std::int64_t pts = 0;  ///< in track_timescale ticks
  std::int64_t dts = 0;
};

struct ParsedStream {
  std::vector<AccessUnit> units;
  std::vector<ByteView> parameter_sets;  ///< first copy of each VPS/SPS/PPS id
  SequenceInfo sps;                      ///< of the first SPS
  ir::Rational fps{25, 1};
  std::int64_t frame_ticks = 3600;
};

/// Frame rate from the VUI timing information, if it is plausible.
[[nodiscard]] std::optional<ir::Rational> vui_fps(const SequenceInfo& sps) {
  if (sps.num_units_in_tick == 0 || sps.time_scale == 0) return std::nullopt;
  const std::int64_t num = sps.time_scale;
  const std::int64_t den = sps.num_units_in_tick;
  const double value = static_cast<double>(num) / static_cast<double>(den);
  if (!(value >= 1.0 && value <= 1000.0)) return std::nullopt;
  const std::int64_t g = std::gcd(num, den);
  return ir::Rational{num / g, den / g};
}

[[nodiscard]] Result<ParsedStream> parse_stream(ByteView input) {
  ParsedStream stream;
  const std::vector<ByteView> nals = split_annexb(input);
  AccessUnitSplitter splitter;
  std::vector<std::vector<ByteView>> units;
  for (const ByteView nal : nals) {
    if (auto unit = splitter.push(nal)) units.push_back(std::move(*unit));
  }
  if (auto unit = splitter.finish()) units.push_back(std::move(*unit));

  std::map<std::uint32_t, bool> seen;  // (type << 8 | id) of the parameter sets kept
  bool have_sps = false;
  PocTracker poc;
  // Presentation order: within each coded video sequence, by POC.
  struct Picture {
    std::size_t unit = 0;
    std::int64_t poc = 0;
  };
  std::vector<Picture> sequence;
  std::int64_t presented = 0;
  std::vector<std::int64_t> display_index;
  std::vector<std::vector<ByteView>> kept;
  const auto close_sequence = [&] {
    std::stable_sort(sequence.begin(), sequence.end(), [](const Picture& a, const Picture& b) { return a.poc < b.poc; });
    for (const Picture& picture : sequence) display_index[picture.unit] = presented++;
    sequence.clear();
  };
  for (auto& unit : units) {
    bool has_vcl = false;
    for (const ByteView nal : unit) {
      const std::uint8_t type = nal_type(nal);
      if (is_parameter_set(type) || type == static_cast<std::uint8_t>(NalType::Eos)) {
        poc.parameter_set(nal);
        if (is_parameter_set(type) && nal.size() >= 3) {
          std::uint32_t id = 0;
          if (type == 32) id = (std::to_integer<std::uint32_t>(nal[2]) >> 4) & 0xF;
          if (type == 33) {
            if (auto sps = parse_sps(nal)) {
              id = sps->sps_id;
              if (!have_sps) {
                stream.sps = *sps;
                have_sps = true;
              }
            }
          }
          if (type == 34) {
            if (auto pps = parse_pps_lite(nal)) id = pps->pps_id;
          }
          if (!seen.contains((std::uint32_t{type} << 8) | id)) {
            seen[(std::uint32_t{type} << 8) | id] = true;
            stream.parameter_sets.push_back(nal);
          }
        }
      }
      if (is_vcl(type)) has_vcl = true;
    }
    if (!has_vcl) continue;  // trailing parameter sets or SEI without a picture
    const ByteView first_vcl = *std::find_if(unit.begin(), unit.end(), [](ByteView n) { return is_vcl(nal_type(n)); });
    bool new_sequence = false;
    const std::optional<std::int64_t> order = poc.picture(first_vcl, new_sequence);
    const std::size_t index = kept.size();
    kept.push_back(std::move(unit));
    display_index.push_back(0);
    if (new_sequence) close_sequence();
    sequence.push_back({index, order.value_or(sequence.empty() ? 0 : sequence.back().poc + 1)});
  }
  close_sequence();
  if (!have_sps) return fail(Status::Decode, "not an HEVC stream: no sequence parameter set found");
  if (kept.empty()) return fail(Status::Decode, "the HEVC stream holds no pictures");

  if (const auto fps = vui_fps(stream.sps)) stream.fps = *fps;
  stream.frame_ticks = std::max<std::int64_t>(
      1, std::llround(static_cast<double>(track_timescale) * static_cast<double>(stream.fps.den) /
                      static_cast<double>(stream.fps.num)));
  // DTS: decode index shifted by the reorder depth, so that DTS <= PTS.
  const std::int64_t delay = static_cast<std::int64_t>(stream.sps.max_num_reorder_pics) * stream.frame_ticks;
  stream.units.reserve(kept.size());
  for (std::size_t i = 0; i < kept.size(); ++i) {
    AccessUnit unit;
    unit.nals = std::move(kept[i]);
    unit.pts = display_index[i] * stream.frame_ticks;
    unit.dts = static_cast<std::int64_t>(i) * stream.frame_ticks - delay;
    stream.units.push_back(std::move(unit));
  }
  return stream;
}

/// Shared state of the functions of one ir::Video.
struct StreamState {
  ParsedStream stream;
  std::unique_ptr<codec::VideoDecoder> decoder;
  std::size_t next_unit = 0;
  std::size_t next_packet = 0;
  bool flushed = false;
  Progress progress;
};

Result<AnyIR> read_stream(ReadContext& ctx) {
  CVT_TRY_ASSIGN(ParsedStream parsed, parse_stream(ctx.input));
  CVT_TRY(ctx.limits.check_pixels(parsed.sps.width, parsed.sps.height));
  const std::vector<ByteView> parameter_sets = parsed.parameter_sets;
  CVT_TRY_ASSIGN(Bytes hvcc, build_hvcc(parameter_sets, 4));

  auto state = std::make_shared<StreamState>();
  state->stream = std::move(parsed);
  state->progress = ctx.progress;
  state->decoder = make_decoder();
  codec::CodecParams params;
  params.codec = std::string(codec::id::hevc);
  params.timescale = track_timescale;
  CVT_TRY(state->decoder->configure(params));

  ir::Video video;
  const ParsedStream& stream = state->stream;
  video.info.width = stream.sps.width;
  video.info.height = stream.sps.height;
  video.info.fps = stream.fps;
  video.info.frame_count = stream.units.size();
  video.info.duration_ms = static_cast<std::int64_t>(std::llround(static_cast<double>(stream.units.size()) *
                                                                  static_cast<double>(stream.frame_ticks) * 1000.0 /
                                                                  track_timescale));
  video.next_frame = [state]() -> Result<std::optional<ir::VideoFrame>> {
    while (true) {
      CVT_TRY_ASSIGN(std::optional<ir::VideoFrame> frame, state->decoder->receive_frame());
      if (frame) return frame;
      if (state->flushed) return std::optional<ir::VideoFrame>{};
      if (state->next_unit < state->stream.units.size()) {
        const AccessUnit& unit = state->stream.units[state->next_unit++];
        if (!state->progress.report(static_cast<double>(state->next_unit) /
                                        static_cast<double>(state->stream.units.size()),
                                    "Decoding HEVC")) {
          return fail(Status::Cancelled, "the conversion was cancelled");
        }
        codec::Packet packet;
        packet.bytes = join_annexb(unit.nals);
        packet.pts = unit.pts;
        packet.dts = unit.dts;
        packet.duration = state->stream.frame_ticks;
        CVT_TRY(state->decoder->send_packet(packet));
      } else {
        state->flushed = true;
        CVT_TRY(state->decoder->flush());
      }
    }
  };

  // The same pictures, compressed, for containers that can take them as is.
  ir::Track track;
  track.type = ir::TrackType::Video;
  track.codec = std::string(codec::id::hevc);
  track.extradata = std::move(hvcc);
  track.timescale = track_timescale;
  track.next_packet = [state]() -> Result<std::optional<ir::Packet>> {
    if (state->next_packet >= state->stream.units.size()) return std::optional<ir::Packet>{};
    const AccessUnit& unit = state->stream.units[state->next_packet++];
    std::vector<ByteView> nals;
    for (const ByteView nal : unit.nals) {
      const std::uint8_t type = nal_type(nal);
      // Parameter sets identical to those of the configuration record are dropped.
      const bool in_record = is_parameter_set(type) &&
                             std::any_of(state->stream.parameter_sets.begin(), state->stream.parameter_sets.end(),
                                         [nal](ByteView p) { return std::ranges::equal(p, nal); });
      if (!in_record && type != static_cast<std::uint8_t>(NalType::Aud)) nals.push_back(nal);
    }
    ir::Packet packet;
    CVT_TRY_ASSIGN(packet.bytes, join_length_prefixed(nals, 4));
    packet.pts = unit.pts;
    packet.dts = unit.dts;
    packet.duration = state->stream.frame_ticks;
    packet.keyframe = std::any_of(unit.nals.begin(), unit.nals.end(), [](ByteView n) { return is_irap(nal_type(n)); });
    return std::optional<ir::Packet>{std::move(packet)};
  };
  video.tracks.push_back(std::move(track));
  return AnyIR(std::move(video));
}

[[nodiscard]] codec::EncodePreset encoder_speed(std::string_view value) noexcept {
  if (value == "fast") return codec::EncodePreset::Fast;
  if (value == "slow") return codec::EncodePreset::Small;
  return codec::EncodePreset::Balanced;
}

Result<Bytes> write_stream(const AnyIR& value, WriteContext& ctx) {
  const ir::Video& video = value.get<ir::Video>();
  if (!video.next_frame) return fail(Status::Encode, "the video has no pictures that can be encoded");
  if (video.next_audio || video.info.has_audio) {
    ctx.warn("A raw HEVC stream holds only video; the audio was left out.");
  }
  CVT_TRY_ASSIGN(std::optional<ir::VideoFrame> first, video.next_frame());
  if (!first) return fail(Status::Encode, "the video has no pictures");
  CVT_TRY(ctx.limits.check_pixels(first->width, first->height));

  codec::VideoEncodeParams params;
  params.width = first->width;
  params.height = first->height;
  params.fps = video.info.fps.num > 0 && video.info.fps.den > 0 ? video.info.fps : ir::Rational{25, 1};
  params.quality = options::resolved_quality(ctx.options);
  params.bitrate_kbps = options::resolved_video_bitrate_kbps(ctx.options);
  params.keyframe_interval = static_cast<std::uint32_t>(ctx.options.get_int(key_keyframe_interval, 0, 0, 100000));
  params.preset = encoder_speed(ctx.options.get_string(key_encoder_speed, "balanced"));
  params.pixel_format = ir::PixelFormat::I420;
  params.color = ir::is_yuv(first->format) ? first->color : ir::ColorInfo{};
  params.timescale = track_timescale;
  const std::unique_ptr<codec::VideoEncoder> encoder = make_encoder();
  CVT_TRY(encoder->configure(params));

  // Parameter sets before every keyframe, so that decoding can start there.
  CVT_TRY_ASSIGN(const DecoderConfig config, parse_hvcc(encoder->extradata()));
  std::vector<ByteView> parameter_sets;
  for (const Bytes& nal : config.nals) parameter_sets.emplace_back(nal);
  const Bytes headers = join_annexb(parameter_sets);

  util::ByteWriter out;
  const auto drain = [&]() -> Result<void> {
    while (true) {
      CVT_TRY_ASSIGN(std::optional<codec::Packet> packet, encoder->receive_packet());
      if (!packet) return {};
      if (packet->keyframe) out.bytes(headers);
      CVT_TRY_ASSIGN(const Bytes annexb, length_prefixed_to_annexb(packet->bytes, 4));
      out.bytes(annexb);
      CVT_TRY(ctx.limits.check_output(out.size()));
    }
  };

  const double total = video.info.frame_count ? static_cast<double>(*video.info.frame_count) : 0.0;
  std::uint64_t count = 0;
  std::optional<ir::VideoFrame> frame = std::move(first);
  while (frame) {
    if (frame->width != params.width || frame->height != params.height) {
      return fail(Status::Encode, "the picture size changes within the video ({}x{} to {}x{})", params.width,
                  params.height, frame->width, frame->height);
    }
    CVT_TRY(encoder->send_frame(*frame));
    CVT_TRY(drain());
    ++count;
    CVT_TRY(ctx.checkpoint(total > 0 ? static_cast<double>(count) / total : 0.0, "Encoding HEVC"));
    CVT_TRY_ASSIGN(frame, video.next_frame());
  }
  CVT_TRY(encoder->flush());
  CVT_TRY(drain());
  return out.take();
}

/// Content test: plausible NAL headers and a parseable SPS near the start.
int sniff_stream(std::span<const std::byte> data, std::string_view /*filename*/) {
  const ByteView head = data.first(std::min<std::size_t>(data.size(), 64 * 1024));
  if (!starts_with_start_code(head)) return 0;
  const std::vector<ByteView> nals = split_annexb(head);
  bool vps = false;
  bool sps = false;
  bool pps = false;
  std::size_t checked = 0;
  for (const ByteView nal : nals) {
    if (++checked > 16) break;
    if (nal.size() < 2) return 0;
    const auto b0 = std::to_integer<unsigned>(nal[0]);
    const auto b1 = std::to_integer<unsigned>(nal[1]);
    // forbidden_zero_bit, nuh_layer_id 0 and a temporal id are required.
    if ((b0 & 0x81u) != 0 || (b1 & 0xF8u) != 0 || (b1 & 7u) == 0) return 0;
    const std::uint8_t type = nal_type(nal);
    if (type == 32) vps = true;
    if (type == 33) {
      if (!parse_sps(nal)) return 0;
      sps = true;
    }
    if (type == 34) pps = true;
    if (is_vcl(type) && !(vps && sps && pps)) return 0;
  }
  if (vps && sps && pps) return 92;
  return sps ? 60 : 0;
}

}  // namespace

void register_stream_format(Registry& registry) {
  FormatInfo info;
  info.id = std::string(stream_format_id);
  info.name = "H.265/HEVC video stream";
  info.description = "Raw High Efficiency Video Coding bitstream (Annex B), as written by encoders and cameras.";
  info.category = Category::Video;
  info.extensions = {"hevc", "h265", "265"};
  info.mime = "video/h265";
  info.popularity = 30;
  info.signatures = {Signature::at(0, "00 00 00 01 40 01"), Signature::at(0, "00 00 01 40 01")};
  info.sniff = sniff_stream;
  registry.add_format(std::move(info));

  Codec reader;
  reader.format_id = std::string(stream_format_id);
  reader.kind = Kind::Video;
  reader.read = read_stream;
  reader.read_cost = 20;
  registry.add_codec(std::move(reader));

  Codec writer;
  writer.format_id = std::string(stream_format_id);
  writer.kind = Kind::Video;
  writer.write = write_stream;
  writer.write_cost = 40;
  writer.lossy = true;
  writer.write_options = {
      options::quality(),
      options::preset(),
      options::video_bitrate_kbps(),
      OptionSpec::integer(std::string(key_keyframe_interval), "Keyframe interval", 0, 0, 100000,
                          "Most frames between two keyframes; 0 picks about five seconds. Shorter intervals seek "
                          "faster and cost size.")
          .advanced(),
      OptionSpec::choice(std::string(key_encoder_speed), "Encoder speed", "balanced", {"fast", "balanced", "slow"},
                         "Slower encoding finds a smaller file at the same quality.")
          .advanced(),
  };
  registry.add_codec(std::move(writer));
}

}  // namespace cvt::hevc
