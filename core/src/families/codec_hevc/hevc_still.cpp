// Convertible core - codec_hevc family: single coded pictures (HEIF/HEIC items).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "codec/codec.hpp"
#include "families/codec_hevc/hevc_api.hpp"
#include "families/codec_hevc/hevc_internal.hpp"

namespace cvt::hevc {

namespace {

[[nodiscard]] Result<void> check_size(const SequenceInfo& sps, std::uint64_t max_pixels) {
  if (std::uint64_t{sps.width} * sps.height > max_pixels) {
    return fail(Status::Limit, "HEVC picture of {}x{} pixels exceeds the limit of {} pixels", sps.width, sps.height,
                max_pixels);
  }
  return {};
}

}  // namespace

Result<ir::VideoFrame> decode_picture(ByteView hvcc, ByteView sample, std::uint64_t max_pixels) {
  // Find the sequence size before anything is allocated for it.
  std::vector<ByteView> nals;
  std::optional<DecoderConfig> config;
  if (!hvcc.empty()) {
    CVT_TRY_ASSIGN(config, parse_hvcc(hvcc));
    for (const Bytes& nal : config->nals) nals.emplace_back(nal);
    CVT_TRY_ASSIGN(const std::vector<ByteView> units, split_length_prefixed(sample, config->length_size));
    nals.insert(nals.end(), units.begin(), units.end());
  } else {
    nals = split_annexb(sample);
  }
  bool have_sps = false;
  for (const ByteView nal : nals) {
    if (nal_type(nal) != static_cast<std::uint8_t>(NalType::Sps)) continue;
    CVT_TRY_ASSIGN(const SequenceInfo sps, parse_sps(nal));
    CVT_TRY(check_size(sps, max_pixels));
    have_sps = true;
  }
  if (!have_sps) return fail(Status::Decode, "the HEVC picture has no sequence parameter set");

  const std::unique_ptr<codec::VideoDecoder> decoder = make_decoder();
  codec::CodecParams params;
  params.codec = "hevc";
  params.extradata = Bytes(hvcc.begin(), hvcc.end());
  CVT_TRY(decoder->configure(params));
  codec::Packet packet;
  packet.bytes = Bytes(sample.begin(), sample.end());
  packet.keyframe = true;
  CVT_TRY(decoder->send_packet(packet));
  CVT_TRY(decoder->flush());
  CVT_TRY_ASSIGN(std::optional<codec::VideoFrame> frame, decoder->receive_frame());
  if (!frame) return fail(Status::Decode, "the HEVC data holds no decodable picture");
  return std::move(*frame);
}

Result<EncodedPicture> encode_picture(const ir::VideoFrame& frame, const PictureEncodeOptions& options) {
  if (!frame.valid()) return fail(Status::InvalidArgument, "invalid picture");
  const std::unique_ptr<codec::VideoEncoder> encoder = make_encoder();
  codec::VideoEncodeParams params;
  params.width = frame.width;
  params.height = frame.height;
  params.fps = {1, 1};
  params.quality = options.quality;
  params.keyframe_interval = 1;
  params.preset = options.preset;
  params.color = options.color;
  params.timescale = 1000;
  CVT_TRY(encoder->configure(params));
  ir::VideoFrame source = frame;
  source.pts_ms = 0;
  source.duration_ms = 1000;
  CVT_TRY(encoder->send_frame(source));
  CVT_TRY(encoder->flush());
  CVT_TRY_ASSIGN(std::optional<codec::Packet> packet, encoder->receive_packet());
  if (!packet) return fail(Status::Encode, "the HEVC encoder produced no picture");
  EncodedPicture out;
  out.hvcc = encoder->extradata();
  out.sample = std::move(packet->bytes);
  out.width = frame.width + (frame.width & 1u);
  out.height = frame.height + (frame.height & 1u);
  return out;
}

}  // namespace cvt::hevc
