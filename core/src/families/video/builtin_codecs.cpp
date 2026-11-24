// Convertible core - video family: built-in trivial codecs.
//
// rawvideo  uncompressed pictures. The track's extradata is a 4-character
//           layout tag: "I420", "YV12", "Y42B" (I422), "444P" (I444), "NV12",
//           "I0AL" (I420P10), "RGBA", "BGRA", "RGB3" (packed RGB, top-down)
//           or "BGR3" (Windows DIB: BGR, bottom-up, rows padded to 4 bytes).
//           Without a tag the layout is inferred from the packet size.
//           The encoder writes I420/I422/I444/I420P10/RGBA as configured.
// pcm_*     uncompressed audio (s16le, s16be, s24le, f32le, u8, A-law, mu-law
//           decoders; s16le, s16be, s24le, f32le and u8 encoders).
//
// They are registered only when no other family registered the same id
// first, so a faster or more complete implementation always wins.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>

#include "util/bytes.hpp"
#include "codec/ids.hpp"
#include "codec/registry.hpp"
#include "families/video/builtin_codecs.hpp"
#include "families/video/common.hpp"

namespace cvt::video {

namespace {

std::int64_t to_ms(std::int64_t ticks, std::uint32_t timescale) { return rescale(ticks, timescale == 0 ? 1000 : timescale, 1000); }

struct Layout {
  ir::PixelFormat format = ir::PixelFormat::I420;
  bool swap_chroma = false;  // YV12
  int packed = 0;            // 0 = planar per ir layout, 3 = RGB3, -3 = BGR3 (bottom-up), 4 = BGRA
};

std::optional<Layout> layout_for(std::string_view tag) {
  if (tag == "I420" || tag == "IYUV") return Layout{ir::PixelFormat::I420, false, 0};
  if (tag == "YV12") return Layout{ir::PixelFormat::I420, true, 0};
  if (tag == "Y42B" || tag == "I422") return Layout{ir::PixelFormat::I422, false, 0};
  if (tag == "444P" || tag == "I444") return Layout{ir::PixelFormat::I444, false, 0};
  if (tag == "NV12") return Layout{ir::PixelFormat::NV12, false, 0};
  if (tag == "I0AL") return Layout{ir::PixelFormat::I420P10, false, 0};
  if (tag == "RGBA") return Layout{ir::PixelFormat::RGBA8, false, 0};
  if (tag == "BGRA") return Layout{ir::PixelFormat::RGBA8, false, 4};
  if (tag == "RGB3") return Layout{ir::PixelFormat::RGBA8, false, 3};
  if (tag == "BGR3") return Layout{ir::PixelFormat::RGBA8, false, -3};
  return std::nullopt;
}

std::uint64_t planar_size(ir::PixelFormat f, std::uint32_t w, std::uint32_t h) {
  std::uint64_t total = 0;
  for (std::size_t p = 0; p < ir::plane_count(f); ++p) total += std::uint64_t{ir::plane_row_bytes(f, p, w)} * ir::plane_height(f, p, h);
  return total;
}

class RawVideoDecoder final : public codec::VideoDecoder {
 public:
  Result<void> configure(const codec::CodecParams& params) override {
    params_ = params;
    if (params.width == 0 || params.height == 0) return fail(Status::Decode, "raw video needs the picture size from the container");
    if (params.width > 32768 || params.height > 32768) return fail(Status::Limit, "raw video picture is too large");
    if (params.extradata.size() == 4) {
      const auto l = layout_for(as_chars(params.extradata));
      if (!l) return fail(Status::Decode, "unsupported raw video layout '{}'", as_chars(params.extradata));
      layout_ = *l;
      known_ = true;
    }
    return {};
  }

  Result<void> send_packet(const ir::Packet& packet) override {
    const std::uint32_t w = params_.width;
    const std::uint32_t h = params_.height;
    if (!known_) {
      // Infer from the size: I420 or RGBA.
      if (packet.bytes.size() == planar_size(ir::PixelFormat::I420, w, h)) {
        layout_ = Layout{};
      } else if (packet.bytes.size() == std::uint64_t{w} * h * 4) {
        layout_ = Layout{ir::PixelFormat::RGBA8, false, 0};
      } else {
        return fail(Status::Decode, "raw video frame of {} bytes does not match {}x{}", packet.bytes.size(), w, h);
      }
      known_ = true;
    }
    ir::VideoFrame frame = ir::VideoFrame::allocate(layout_.format, w, h);
    frame.color = ir::ColorInfo{ir::ColorMatrix::BT601, ir::ColorRange::Limited, ir::ChromaSiting::Center};
    const auto* in = reinterpret_cast<const std::uint8_t*>(packet.bytes.data());
    const std::size_t size = packet.bytes.size();
    if (layout_.packed == 0) {
      if (size < planar_size(layout_.format, w, h)) return fail(Status::Decode, "raw video frame is truncated");
      std::size_t at = 0;
      for (std::size_t p = 0; p < frame.planes.size(); ++p) {
        const std::size_t target = (layout_.swap_chroma && p > 0) ? 3 - p : p;
        const std::size_t bytes = frame.planes[target].data.size();
        ::cvt::util::copy_bytes(frame.planes[target].data.data(), in + at, bytes);
        at += bytes;
      }
    } else {
      const std::size_t bpp = layout_.packed == 4 ? 4 : 3;
      const std::size_t row = layout_.packed == -3 ? ((std::size_t{w} * 3 + 3) & ~std::size_t{3}) : std::size_t{w} * bpp;
      if (size < row * h) return fail(Status::Decode, "raw video frame is truncated");
      for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint8_t* src = in + (layout_.packed == -3 ? (h - 1 - y) : y) * row;
        std::uint8_t* dst = frame.planes[0].data.data() + std::size_t{y} * frame.planes[0].stride;
        for (std::uint32_t x = 0; x < w; ++x) {
          const std::uint8_t* s = src + x * bpp;
          const bool bgr = layout_.packed != 3;
          dst[x * 4] = bgr ? s[2] : s[0];
          dst[x * 4 + 1] = s[1];
          dst[x * 4 + 2] = bgr ? s[0] : s[2];
          dst[x * 4 + 3] = bpp == 4 ? s[3] : 255;
        }
      }
    }
    frame.pts_ms = to_ms(packet.pts, params_.timescale);
    frame.duration_ms = to_ms(packet.duration, params_.timescale);
    // Reorder by presentation time (raw streams have no reordering, but be safe).
    auto pos = std::ranges::find_if(queue_, [&](const ir::VideoFrame& f) { return f.pts_ms > frame.pts_ms; });
    queue_.insert(pos, std::move(frame));
    return {};
  }

  Result<std::optional<ir::VideoFrame>> receive_frame() override {
    if (queue_.empty()) return std::optional<ir::VideoFrame>{};
    ir::VideoFrame f = std::move(queue_.front());
    queue_.pop_front();
    return std::optional<ir::VideoFrame>(std::move(f));
  }
  Result<void> flush() override { return {}; }

 private:
  codec::CodecParams params_;
  Layout layout_;
  bool known_ = false;
  std::deque<ir::VideoFrame> queue_;
};

class RawVideoEncoder final : public codec::VideoEncoder {
 public:
  Result<void> configure(const codec::VideoEncodeParams& params) override {
    params_ = params;
    switch (params.pixel_format) {
      case ir::PixelFormat::I420: tag_ = "I420"; break;
      case ir::PixelFormat::I422: tag_ = "Y42B"; break;
      case ir::PixelFormat::I444: tag_ = "444P"; break;
      case ir::PixelFormat::I420P10: tag_ = "I0AL"; break;
      case ir::PixelFormat::RGBA8: tag_ = "RGBA"; break;
      default: return fail(Status::InvalidArgument, "raw video cannot store this pixel format");
    }
    return {};
  }
  Result<void> send_frame(const ir::VideoFrame& frame) override {
    if (!frame.valid() || frame.format != params_.pixel_format) {
      return fail(Status::InvalidArgument, "raw video encoder received a frame in the wrong format");
    }
    ir::Packet packet;
    for (std::size_t p = 0; p < frame.planes.size(); ++p) {
      const std::size_t row = ir::plane_row_bytes(frame.format, p, frame.width);
      for (std::uint32_t y = 0; y < ir::plane_height(frame.format, p, frame.height); ++y) {
        const auto* src = reinterpret_cast<const std::byte*>(frame.planes[p].data.data() + std::size_t{y} * frame.planes[p].stride);
        packet.bytes.insert(packet.bytes.end(), src, src + row);
      }
    }
    packet.pts = rescale(frame.pts_ms, 1000, params_.timescale);
    packet.dts = packet.pts;
    packet.duration = rescale(frame.duration_ms, 1000, params_.timescale);
    packet.keyframe = true;
    queue_.push_back(std::move(packet));
    return {};
  }
  Result<std::optional<ir::Packet>> receive_packet() override {
    if (queue_.empty()) return std::optional<ir::Packet>{};
    ir::Packet p = std::move(queue_.front());
    queue_.pop_front();
    return std::optional<ir::Packet>(std::move(p));
  }
  Bytes extradata() const override { return to_bytes(tag_); }
  Result<void> flush() override { return {}; }

 private:
  codec::VideoEncodeParams params_;
  std::string_view tag_ = "I420";
  std::deque<ir::Packet> queue_;
};

// --------------------------------------------------------------------------- PCM

enum class Pcm { S16LE, S16BE, S24LE, F32LE, U8, ALaw, MuLaw };

std::uint32_t pcm_bytes(Pcm f) {
  switch (f) {
    case Pcm::S16LE:
    case Pcm::S16BE: return 2;
    case Pcm::S24LE: return 3;
    case Pcm::F32LE: return 4;
    default: return 1;
  }
}

float alaw_to_float(std::uint8_t a) {
  a ^= 0x55;
  int t = (a & 0x0F) << 4;
  const int seg = (a & 0x70) >> 4;
  switch (seg) {
    case 0: t += 8; break;
    case 1: t += 0x108; break;
    default: t += 0x108; t <<= seg - 1;
  }
  return static_cast<float>((a & 0x80) ? t : -t) / 32768.0f;
}

float mulaw_to_float(std::uint8_t u) {
  u = static_cast<std::uint8_t>(~u);
  int t = ((u & 0x0F) << 3) + 0x84;
  t <<= (u & 0x70) >> 4;
  return static_cast<float>((u & 0x80) ? (0x84 - t) : (t - 0x84)) / 32768.0f;
}

class PcmDecoder final : public codec::AudioDecoder {
 public:
  explicit PcmDecoder(Pcm format) : format_(format) {}
  Result<void> configure(const codec::CodecParams& params) override {
    if (params.sample_rate == 0 || params.channels == 0) return fail(Status::Decode, "PCM audio needs a sample rate and channel count");
    rate_ = params.sample_rate;
    channels_ = params.channels;
    timescale_ = params.timescale == 0 ? rate_ : params.timescale;
    return {};
  }
  Result<void> send_packet(const ir::Packet& packet) override {
    const std::uint32_t width = pcm_bytes(format_);
    const std::size_t frame_bytes = std::size_t{width} * channels_;
    const std::size_t frames = packet.bytes.size() / frame_bytes;
    ir::AudioChunk chunk;
    chunk.pts_ms = rescale(packet.pts, timescale_, 1000);
    chunk.samples.resize(frames * channels_);
    const auto* p = reinterpret_cast<const std::uint8_t*>(packet.bytes.data());
    for (std::size_t i = 0; i < chunk.samples.size(); ++i) {
      const std::uint8_t* s = p + i * width;
      float v = 0.0f;
      switch (format_) {
        case Pcm::S16LE: v = static_cast<float>(static_cast<std::int16_t>(s[0] | (s[1] << 8))) / 32768.0f; break;
        case Pcm::S16BE: v = static_cast<float>(static_cast<std::int16_t>((s[0] << 8) | s[1])) / 32768.0f; break;
        case Pcm::S24LE: {
          std::int32_t x = s[0] | (s[1] << 8) | (s[2] << 16);
          if (x & 0x800000) x -= 0x1000000;
          v = static_cast<float>(x) / 8388608.0f;
          break;
        }
        case Pcm::F32LE: {
          const std::uint32_t bits = s[0] | (s[1] << 8) | (s[2] << 16) | (static_cast<std::uint32_t>(s[3]) << 24);
          v = std::bit_cast<float>(bits);
          if (!std::isfinite(v)) v = 0.0f;
          break;
        }
        case Pcm::U8: v = (static_cast<float>(s[0]) - 128.0f) / 128.0f; break;
        case Pcm::ALaw: v = alaw_to_float(s[0]); break;
        case Pcm::MuLaw: v = mulaw_to_float(s[0]); break;
      }
      chunk.samples[i] = v;
    }
    if (!chunk.samples.empty()) queue_.push_back(std::move(chunk));
    return {};
  }
  Result<std::optional<ir::AudioChunk>> receive_chunk() override {
    if (queue_.empty()) return std::optional<ir::AudioChunk>{};
    ir::AudioChunk c = std::move(queue_.front());
    queue_.pop_front();
    return std::optional<ir::AudioChunk>(std::move(c));
  }
  Result<void> flush() override { return {}; }
  std::uint32_t sample_rate() const override { return rate_; }
  std::uint16_t channels() const override { return channels_; }

 private:
  Pcm format_;
  std::uint32_t rate_ = 0;
  std::uint16_t channels_ = 0;
  std::uint32_t timescale_ = 1000;
  std::deque<ir::AudioChunk> queue_;
};

class PcmEncoder final : public codec::AudioEncoder {
 public:
  explicit PcmEncoder(Pcm format) : format_(format) {}
  Result<void> configure(const codec::AudioEncodeParams& params) override {
    if (params.sample_rate == 0 || params.channels == 0) return fail(Status::InvalidArgument, "PCM needs a sample rate and channels");
    params_ = params;
    if (params_.timescale == 0) params_.timescale = params_.sample_rate;
    return {};
  }
  Result<void> send_chunk(const ir::AudioChunk& chunk) override {
    const std::size_t frames = chunk.samples.size() / params_.channels;
    if (frames == 0) return {};
    // Packets of at most 4096 sample frames.
    for (std::size_t start = 0; start < frames; start += 4096) {
      const std::size_t n = std::min<std::size_t>(4096, frames - start);
      ir::Packet packet;
      packet.bytes.reserve(n * params_.channels * pcm_bytes(format_));
      for (std::size_t i = start * params_.channels; i < (start + n) * params_.channels; ++i) {
        const float v = std::clamp(std::isfinite(chunk.samples[i]) ? chunk.samples[i] : 0.0f, -1.0f, 1.0f);
        switch (format_) {
          case Pcm::S16LE:
          case Pcm::S16BE: {
            const auto x = static_cast<std::int16_t>(std::lround(std::clamp(v * 32768.0f, -32768.0f, 32767.0f)));
            const auto u = static_cast<std::uint16_t>(x);
            if (format_ == Pcm::S16LE) {
              packet.bytes.push_back(static_cast<std::byte>(u & 0xFF));
              packet.bytes.push_back(static_cast<std::byte>(u >> 8));
            } else {
              packet.bytes.push_back(static_cast<std::byte>(u >> 8));
              packet.bytes.push_back(static_cast<std::byte>(u & 0xFF));
            }
            break;
          }
          case Pcm::S24LE: {
            const auto x = static_cast<std::int32_t>(std::lround(std::clamp(v * 8388608.0f, -8388608.0f, 8388607.0f)));
            const auto u = static_cast<std::uint32_t>(x);
            for (int b = 0; b < 3; ++b) packet.bytes.push_back(static_cast<std::byte>((u >> (8 * b)) & 0xFF));
            break;
          }
          case Pcm::F32LE: {
            const auto u = std::bit_cast<std::uint32_t>(v);
            for (int b = 0; b < 4; ++b) packet.bytes.push_back(static_cast<std::byte>((u >> (8 * b)) & 0xFF));
            break;
          }
          default: {
            const long x = std::lround(std::clamp(v * 128.0f + 128.0f, 0.0f, 255.0f));
            packet.bytes.push_back(static_cast<std::byte>(x));
            break;
          }
        }
      }
      packet.pts = rescale(position_, params_.sample_rate, params_.timescale);
      packet.dts = packet.pts;
      packet.duration = rescale(static_cast<std::int64_t>(n), params_.sample_rate, params_.timescale);
      packet.keyframe = true;
      position_ += static_cast<std::int64_t>(n);
      queue_.push_back(std::move(packet));
    }
    return {};
  }
  Result<std::optional<ir::Packet>> receive_packet() override {
    if (queue_.empty()) return std::optional<ir::Packet>{};
    ir::Packet p = std::move(queue_.front());
    queue_.pop_front();
    return std::optional<ir::Packet>(std::move(p));
  }
  Bytes extradata() const override { return {}; }
  Result<void> flush() override { return {}; }
  std::uint32_t frame_size() const override { return 0; }
  std::uint32_t delay() const override { return 0; }

 private:
  Pcm format_;
  codec::AudioEncodeParams params_;
  std::int64_t position_ = 0;
  std::deque<ir::Packet> queue_;
};

}  // namespace

void register_builtin_codecs(codec::Registry& media) {
  // Raw video belongs to this family. The PCM codecs belong to the lossless
  // audio family; these simple ones are fallbacks for builds without it.
  media.register_video_decoder(codec::id::rawvideo, [] { return std::make_unique<RawVideoDecoder>(); });
  media.register_video_encoder(codec::id::rawvideo, [] { return std::make_unique<RawVideoEncoder>(); });
  const std::pair<std::string_view, Pcm> decoders[] = {
      {codec::id::pcm_s16le, Pcm::S16LE}, {codec::id::pcm_s16be, Pcm::S16BE}, {codec::id::pcm_s24le, Pcm::S24LE},
      {codec::id::pcm_f32le, Pcm::F32LE}, {codec::id::pcm_u8, Pcm::U8},       {codec::id::pcm_alaw, Pcm::ALaw},
      {codec::id::pcm_mulaw, Pcm::MuLaw}};
  for (const auto& [name, format] : decoders) {
    const Pcm f = format;
    media.register_audio_decoder_fallback(name, [f] { return std::make_unique<PcmDecoder>(f); });
    if (f != Pcm::ALaw && f != Pcm::MuLaw) {
      media.register_audio_encoder_fallback(name, [f] { return std::make_unique<PcmEncoder>(f); });
    }
  }
}

}  // namespace cvt::video
