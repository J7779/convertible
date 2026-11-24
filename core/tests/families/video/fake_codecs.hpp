// Tiny fake codecs for the video pipeline tests, registered under real codec
// ids in a test-only registry so that the MP4/Matroska/... paths run end to end.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <algorithm>
#include <cmath>
#include <deque>
#include <memory>
#include <vector>

#include "codec/codec.hpp"
#include "codec/registry.hpp"
#include "util/bytes.hpp"

namespace cvt::test::video {

/// "Compresses" I420 frames by dividing every sample by a step chosen from the
/// quality (q >= 90: step 1 = lossless; q >= 50: 4; else 16). Packets come out
/// in the order of a codec with B-frames (I, then P B pairs), so containers
/// and the pipeline must carry composition offsets. Extradata ("FAKE" + width
/// + height) is only known after the first packet, like some real encoders.
class FakeVideoEncoder final : public codec::VideoEncoder {
 public:
  Result<void> configure(const codec::VideoEncodeParams& p) override {
    params_ = p;
    step_ = p.quality >= 90 ? 1 : p.quality >= 50 ? 4 : 16;
    if (p.bitrate_kbps && *p.bitrate_kbps < 50) step_ = 32;
    return {};
  }
  Result<void> send_frame(const ir::VideoFrame& f) override {
    if (f.format != ir::PixelFormat::I420 || f.width != params_.width || f.height != params_.height || !f.valid()) {
      return fail(Status::InvalidArgument, "fake encoder: unexpected frame {}x{}", f.width, f.height);
    }
    ir::Packet packet;
    packet.bytes.push_back(static_cast<std::byte>(step_));
    for (std::size_t p = 0; p < f.planes.size(); ++p) {
      const std::size_t row = ir::plane_row_bytes(f.format, p, f.width);
      for (std::uint32_t y = 0; y < ir::plane_height(f.format, p, f.height); ++y) {
        for (std::size_t x = 0; x < row; ++x) {
          packet.bytes.push_back(static_cast<std::byte>(f.planes[p].data[y * f.planes[p].stride + x] / step_));
        }
      }
    }
    packet.pts = f.pts_ms * params_.timescale / 1000;
    packet.duration = std::max<std::int64_t>(1, f.duration_ms * params_.timescale / 1000);
    packet.keyframe = index_ % 10 == 0;
    ++index_;
    if (packet.keyframe) {
      release_all();
      emit(std::move(packet));
    } else if (held_) {
      emit(std::move(packet));  // the later frame first ("P") ...
      emit(std::move(*held_));  // ... then the held earlier one ("B")
      held_.reset();
    } else {
      held_ = std::move(packet);
    }
    return {};
  }
  Result<std::optional<ir::Packet>> receive_packet() override {
    if (out_.empty()) return std::optional<ir::Packet>{};
    ir::Packet p = std::move(out_.front());
    out_.pop_front();
    return std::optional<ir::Packet>(std::move(p));
  }
  Bytes extradata() const override {
    if (emitted_ == 0) return {};
    util::ByteWriter w;
    w.text("FAKE").u16be(static_cast<std::uint16_t>(params_.width)).u16be(static_cast<std::uint16_t>(params_.height));
    return w.take();
  }
  Result<void> flush() override {
    release_all();
    return {};
  }

 private:
  void release_all() {
    if (held_) emit(std::move(*held_));
    held_.reset();
  }
  void emit(ir::Packet packet) {
    // Decode times trail presentation by one frame.
    if (emitted_ == 0) next_dts_ = packet.pts - packet.duration;
    packet.dts = next_dts_;
    next_dts_ += packet.duration;
    ++emitted_;
    out_.push_back(std::move(packet));
  }
  codec::VideoEncodeParams params_;
  int step_ = 1;
  std::uint64_t index_ = 0;
  std::optional<ir::Packet> held_;
  std::deque<ir::Packet> out_;
  std::int64_t next_dts_ = 0;
  std::size_t emitted_ = 0;
};

class FakeVideoDecoder final : public codec::VideoDecoder {
 public:
  Result<void> configure(const codec::CodecParams& p) override {
    params_ = p;
    if (p.extradata.size() == 8) {
      util::ByteReader r(p.extradata);
      r.skip(4);
      params_.width = r.u16be();
      params_.height = r.u16be();
    }
    if (params_.width == 0 || params_.height == 0) return fail(Status::Decode, "fake decoder: unknown size");
    return {};
  }
  Result<void> send_packet(const ir::Packet& packet) override {
    if (packet.bytes.empty()) return fail(Status::Decode, "fake decoder: empty packet");
    const int step = std::to_integer<int>(packet.bytes[0]);
    ir::VideoFrame f = ir::VideoFrame::allocate(ir::PixelFormat::I420, params_.width, params_.height);
    std::size_t at = 1;
    for (ir::Plane& plane : f.planes) {
      for (std::uint8_t& v : plane.data) {
        if (at >= packet.bytes.size()) return fail(Status::Decode, "fake decoder: short packet");
        const int q = std::to_integer<int>(packet.bytes[at++]);
        v = static_cast<std::uint8_t>(std::min(255, q * step + (step > 1 ? step / 2 : 0)));
      }
    }
    f.pts_ms = packet.pts * 1000 / static_cast<std::int64_t>(params_.timescale);
    f.duration_ms = packet.duration * 1000 / static_cast<std::int64_t>(params_.timescale);
    buffer_.push_back(std::move(f));
    std::ranges::sort(buffer_, [](const ir::VideoFrame& a, const ir::VideoFrame& b) { return a.pts_ms < b.pts_ms; });
    if (buffer_.size() > 2) {
      ready_.push_back(std::move(buffer_.front()));
      buffer_.erase(buffer_.begin());
    }
    return {};
  }
  Result<std::optional<ir::VideoFrame>> receive_frame() override {
    if (ready_.empty()) return std::optional<ir::VideoFrame>{};
    ir::VideoFrame f = std::move(ready_.front());
    ready_.pop_front();
    return std::optional<ir::VideoFrame>(std::move(f));
  }
  Result<void> flush() override {
    for (ir::VideoFrame& f : buffer_) ready_.push_back(std::move(f));
    buffer_.clear();
    return {};
  }

 private:
  codec::CodecParams params_;
  std::vector<ir::VideoFrame> buffer_;
  std::deque<ir::VideoFrame> ready_;
};

/// An "AAC" stand-in: 1024-sample frames of 16-bit PCM with an encoder delay of
/// 1024 samples (one silent priming frame), a real AudioSpecificConfig as
/// extradata and packet timestamps starting at 0 including the priming, as many
/// encoders report them.
class FakeAacEncoder final : public codec::AudioEncoder {
 public:
  Result<void> configure(const codec::AudioEncodeParams& p) override {
    params_ = p;
    if (params_.timescale == 0) params_.timescale = p.sample_rate;
    pending_.assign(std::size_t{1024} * p.channels, 0.0f);  // priming
    return {};
  }
  Result<void> send_chunk(const ir::AudioChunk& chunk) override {
    pending_.insert(pending_.end(), chunk.samples.begin(), chunk.samples.end());
    drain(false);
    return {};
  }
  Result<std::optional<ir::Packet>> receive_packet() override {
    if (out_.empty()) return std::optional<ir::Packet>{};
    ir::Packet p = std::move(out_.front());
    out_.pop_front();
    return std::optional<ir::Packet>(std::move(p));
  }
  Bytes extradata() const override {
    // AAC LC, sampling index for the rate, channel configuration.
    static constexpr std::uint32_t rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000};
    std::uint32_t index = 4;
    for (std::uint32_t i = 0; i < 12; ++i) {
      if (rates[i] == params_.sample_rate) index = i;
    }
    const std::uint32_t asc = (2u << 11) | (index << 7) | (static_cast<std::uint32_t>(params_.channels) << 3);
    return {static_cast<std::byte>(asc >> 8), static_cast<std::byte>(asc & 0xFF)};
  }
  Result<void> flush() override {
    drain(true);
    return {};
  }
  std::uint32_t frame_size() const override { return 1024; }
  std::uint32_t delay() const override { return 1024; }

 private:
  void drain(bool all) {
    const std::size_t frame = std::size_t{1024} * params_.channels;
    while (pending_.size() >= frame || (all && !pending_.empty())) {
      if (pending_.size() < frame) pending_.resize(frame, 0.0f);
      ir::Packet p;
      for (std::size_t i = 0; i < frame; ++i) {
        const auto v = static_cast<std::int16_t>(std::lround(std::clamp(pending_[i], -1.0f, 1.0f) * 32767.0f));
        p.bytes.push_back(static_cast<std::byte>(static_cast<std::uint16_t>(v) & 0xFF));
        p.bytes.push_back(static_cast<std::byte>(static_cast<std::uint16_t>(v) >> 8));
      }
      pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(frame));
      p.pts = position_ * params_.timescale / params_.sample_rate;
      p.dts = p.pts;
      p.duration = 1024LL * params_.timescale / params_.sample_rate;
      p.keyframe = true;
      position_ += 1024;
      out_.push_back(std::move(p));
    }
  }
  codec::AudioEncodeParams params_;
  std::vector<float> pending_;
  std::deque<ir::Packet> out_;
  std::int64_t position_ = 0;
};

class FakeAacDecoder final : public codec::AudioDecoder {
 public:
  Result<void> configure(const codec::CodecParams& p) override {
    params_ = p;
    if (p.extradata.size() >= 2) {
      const std::uint32_t asc = (std::to_integer<std::uint32_t>(p.extradata[0]) << 8) | std::to_integer<std::uint32_t>(p.extradata[1]);
      static constexpr std::uint32_t rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000};
      const std::uint32_t index = (asc >> 7) & 0xF;
      if (index < 12) rate_ = rates[index];
      channels_ = static_cast<std::uint16_t>((asc >> 3) & 0xF);
    }
    if (rate_ == 0) rate_ = p.sample_rate;
    if (channels_ == 0) channels_ = p.channels;
    if (rate_ == 0 || channels_ == 0) return fail(Status::Decode, "fake aac: unknown format");
    return {};
  }
  Result<void> send_packet(const ir::Packet& packet) override {
    ir::AudioChunk chunk;
    chunk.pts_ms = packet.pts * 1000 / static_cast<std::int64_t>(params_.timescale);
    for (std::size_t i = 0; i + 1 < packet.bytes.size(); i += 2) {
      const auto v = static_cast<std::int16_t>(std::to_integer<std::uint16_t>(packet.bytes[i]) |
                                               (std::to_integer<std::uint16_t>(packet.bytes[i + 1]) << 8));
      chunk.samples.push_back(static_cast<float>(v) / 32767.0f);
    }
    out_.push_back(std::move(chunk));
    return {};
  }
  Result<std::optional<ir::AudioChunk>> receive_chunk() override {
    if (out_.empty()) return std::optional<ir::AudioChunk>{};
    ir::AudioChunk c = std::move(out_.front());
    out_.pop_front();
    return std::optional<ir::AudioChunk>(std::move(c));
  }
  Result<void> flush() override { return {}; }
  std::uint32_t sample_rate() const override { return rate_; }
  std::uint16_t channels() const override { return channels_; }

 private:
  codec::CodecParams params_;
  std::uint32_t rate_ = 0;
  std::uint16_t channels_ = 0;
  std::deque<ir::AudioChunk> out_;
};

/// Register the fakes as mpeg4 (opaque packets in every container) and aac (and every video id in `extra_video`).
inline void register_fakes(codec::Registry& media, std::initializer_list<std::string_view> extra_video = {}) {
  media.register_video_encoder("mpeg4", [] { return std::make_unique<FakeVideoEncoder>(); });
  media.register_video_decoder("mpeg4", [] { return std::make_unique<FakeVideoDecoder>(); });
  for (const std::string_view id : extra_video) {
    media.register_video_encoder(id, [] { return std::make_unique<FakeVideoEncoder>(); });
    media.register_video_decoder(id, [] { return std::make_unique<FakeVideoDecoder>(); });
  }
  media.register_audio_encoder("aac", [] { return std::make_unique<FakeAacEncoder>(); });
  media.register_audio_decoder("aac", [] { return std::make_unique<FakeAacDecoder>(); });
}

}  // namespace cvt::test::video
