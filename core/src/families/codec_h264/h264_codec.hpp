// Convertible core - codec_h264 family: H.264/AVC decoder and encoder.
//
// Both wrap the imported libavc codec (Apache-2.0, core/src/imported/libavc)
// behind the codec/codec.hpp interfaces; no libavc type appears here.
//
// Decoder
//   Input packets may be Annex B (start codes) or length-prefixed; when the
//   extradata is an avcC record its NAL length size is used, otherwise each
//   packet is inspected. Parameter sets from the extradata are fed before the
//   first packet. Frames come out in presentation order as I420 with the
//   stream's cropping applied, colour information from the SPS VUI, and
//   `pts_ms` / `duration_ms` taken from the packets (or synthesised from the
//   VUI frame rate when the packets carry none). Damaged slices are concealed
//   and reported through `take_warnings()`, never by crashing.
//
// Encoder
//   Main profile (CABAC; B-frames for the Balanced and Small presets), the
//   best profile libavc's encoder offers. Packets are length-prefixed with
//   4-byte lengths and `extradata()` is the avcC record, as MP4/MOV/MKV
//   expect; IDR pictures are flagged as keyframes. Timestamps: PTS follows the
//   input frames, DTS increases monotonically and never exceeds PTS (with
//   B-frames the first DTS is one frame before the first PTS).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "codec/codec.hpp"

namespace cvt {
class Registry;
}

namespace cvt::h264 {

/// libavc limits: the decoder accepts up to 4096 x 4096 luma samples, the
/// encoder up to this size per dimension.
inline constexpr std::uint32_t max_decode_dimension = 4096;
inline constexpr std::uint32_t max_encode_dimension = 4096;

class H264Decoder final : public codec::VideoDecoder {
 public:
  H264Decoder();
  ~H264Decoder() override;
  H264Decoder(const H264Decoder&) = delete;
  H264Decoder& operator=(const H264Decoder&) = delete;

  [[nodiscard]] Result<void> configure(const codec::CodecParams& params) override;
  [[nodiscard]] Result<void> send_packet(const codec::Packet& packet) override;
  [[nodiscard]] Result<std::optional<codec::VideoFrame>> receive_frame() override;
  [[nodiscard]] Result<void> flush() override;

  /// Non-fatal problems met so far (concealed corruption, skipped data),
  /// each reported once; the list is cleared by the call.
  [[nodiscard]] std::vector<std::string> take_warnings() override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class H264Encoder final : public codec::VideoEncoder {
 public:
  H264Encoder();
  ~H264Encoder() override;
  H264Encoder(const H264Encoder&) = delete;
  H264Encoder& operator=(const H264Encoder&) = delete;

  [[nodiscard]] Result<void> configure(const codec::VideoEncodeParams& params) override;
  [[nodiscard]] Result<void> send_frame(const codec::VideoFrame& frame) override;
  [[nodiscard]] Result<std::optional<codec::Packet>> receive_packet() override;
  [[nodiscard]] Bytes extradata() const override;
  [[nodiscard]] Result<void> flush() override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Quantiser for P pictures used for a `quality` (1..100) in constant
/// quality mode: 100 -> 10, 90 -> 21, 75 -> 27, 50 -> 33, 1 -> 46.
[[nodiscard]] int quality_to_qp(int quality) noexcept;

/// Registers the decoder and encoder under codec id "h264". The raw H.264
/// elementary stream format (.h264) belongs to the video family.
void register_codecs(Registry& registry);

}  // namespace cvt::h264
