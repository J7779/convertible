// Convertible core - audio_lossless family: audio decoders and encoders for
// the media codec registry (used by the video containers): PCM flavours,
// G.711, IMA and Microsoft ADPCM, FLAC, ALAC and WavPack.
//
// Conventions for `extradata` (CodecParams / AudioEncoder::extradata):
//   flac        the 34-byte STREAMINFO body (also accepted: "fLaC" + metadata
//               blocks, or a STREAMINFO block with its 4-byte header)
//   alac        the 24-byte ALACSpecificConfig (also accepted wrapped in
//               'frma'/'alac' atoms)
//   adpcm_ms    the WAVE format extension (samples per block, coefficient
//               count, coefficient pairs); empty = standard coefficients
//   adpcm_ima   the WAVE format extension (samples per block); optional
//   wavpack     unused (every block carries its own header)
//   pcm_*       unused
// Packets: PCM any whole number of sample frames; ADPCM one or more blocks
// of `block_align` bytes; FLAC one or more complete frames; ALAC one
// packet; WavPack one or more complete blocks ("wvpk" headers included).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "codec/codec.hpp"
#include "codec/ids.hpp"
#include "codec/registry.hpp"
#include "engine/registry.hpp"
#include "families/audio_lossless/alac.hpp"
#include "families/audio_lossless/audio_api.hpp"
#include "families/audio_lossless/common.hpp"
#include "families/audio_lossless/flac_codec.hpp"
#include "families/audio_lossless/pcm.hpp"
#include "families/audio_lossless/wavpack.hpp"

namespace cvt::audio {

namespace {

using codec::AudioChunk;
using codec::Packet;

std::optional<PcmFormat> pcm_for(std::string_view id) {
  if (id == codec::id::pcm_s16le) return pcm_signed(16, std::endian::little);
  if (id == codec::id::pcm_s16be) return pcm_signed(16, std::endian::big);
  if (id == codec::id::pcm_s24le) return pcm_signed(24, std::endian::little);
  if (id == codec::id::pcm_f32le) return pcm_float(32, std::endian::little);
  if (id == codec::id::pcm_u8) return pcm_unsigned8();
  if (id == codec::id::pcm_alaw) return pcm_alaw();
  if (id == codec::id::pcm_mulaw) return pcm_mulaw();
  if (id == codec::id::pcm_s24be) return pcm_signed(24, std::endian::big);
  if (id == codec::id::pcm_s32le) return pcm_signed(32, std::endian::little);
  if (id == codec::id::pcm_f64le) return pcm_float(64, std::endian::little);
  if (id == codec::id::pcm_s8) return pcm_signed(8, std::endian::little);
  return std::nullopt;
}

std::int64_t to_ms(std::int64_t ticks, std::uint32_t timescale) {
  return timescale == 0 ? 0 : ticks * 1000 / static_cast<std::int64_t>(timescale);
}

// Shared output queue.
class ChunkQueue {
 public:
  void push(std::vector<float> samples, std::int64_t pts_ms) {
    if (samples.empty()) return;
    AudioChunk chunk;
    chunk.samples = std::move(samples);
    chunk.pts_ms = pts_ms;
    queue_.push_back(std::move(chunk));
  }
  std::optional<AudioChunk> pop() {
    if (queue_.empty()) return std::nullopt;
    AudioChunk chunk = std::move(queue_.front());
    queue_.pop_front();
    return chunk;
  }

 private:
  std::deque<AudioChunk> queue_;
};

// =========================================================================== decoders

class PcmDecoder final : public codec::AudioDecoder {
 public:
  Result<void> configure(const codec::CodecParams& params) override {
    const auto layout = pcm_for(params.codec);
    if (!layout) return fail(Status::Decode, "unsupported PCM codec '{}'", params.codec);
    CVT_TRY(check_stream_params(params.sample_rate, params.channels, "the PCM stream"));
    layout_ = *layout;
    params_ = params;
    return {};
  }
  Result<void> send_packet(const Packet& packet) override {
    std::vector<float> out;
    const std::size_t frame = std::size_t{layout_.container_bytes} * params_.channels;
    decode_pcm(ByteView(packet.bytes).first(packet.bytes.size() / frame * frame), layout_, out);
    queue_.push(std::move(out), to_ms(packet.pts, params_.timescale));
    return {};
  }
  Result<std::optional<AudioChunk>> receive_chunk() override { return queue_.pop(); }
  Result<void> flush() override { return {}; }
  std::uint32_t sample_rate() const override { return params_.sample_rate; }
  std::uint16_t channels() const override { return params_.channels; }

 private:
  PcmFormat layout_;
  codec::CodecParams params_;
  ChunkQueue queue_;
};

/// Apple IMA4 (34 bytes per channel and 64 frames per block) and GSM 06.10
/// in the Microsoft packing (65 bytes per 320 frames): whole blocks per packet.
class BlockDecoder final : public codec::AudioDecoder {
 public:
  enum class Kind { Ima4, Gsm };
  explicit BlockDecoder(Kind kind) : kind_(kind) {}
  Result<void> configure(const codec::CodecParams& params) override {
    CVT_TRY(check_stream_params(params.sample_rate, params.channels, kind_ == Kind::Gsm ? "the GSM stream" : "the IMA4 stream"));
    if (kind_ == Kind::Gsm && params.channels != 1) return fail(Status::Decode, "GSM 06.10 audio is mono");
    params_ = params;
    return {};
  }
  Result<void> send_packet(const Packet& packet) override {
    std::vector<float> out;
    if (kind_ == Kind::Ima4) {
      const std::size_t block = std::size_t{34} * params_.channels;
      CVT_TRY(decode_ima4(ByteView(packet.bytes).first(packet.bytes.size() / block * block), params_.channels, out));
    } else {
      CVT_TRY(decode_gsm610_wav(ByteView(packet.bytes).first(packet.bytes.size() / 65 * 65), out));
    }
    queue_.push(std::move(out), to_ms(packet.pts, params_.timescale));
    return {};
  }
  Result<std::optional<AudioChunk>> receive_chunk() override { return queue_.pop(); }
  Result<void> flush() override { return {}; }
  std::uint32_t sample_rate() const override { return params_.sample_rate; }
  std::uint16_t channels() const override { return params_.channels; }

 private:
  Kind kind_;
  codec::CodecParams params_;
  ChunkQueue queue_;
};

class AdpcmDecoder final : public codec::AudioDecoder {
 public:
  explicit AdpcmDecoder(bool ima) : ima_(ima) {}
  Result<void> configure(const codec::CodecParams& params) override {
    CVT_TRY(check_stream_params(params.sample_rate, params.channels, "the ADPCM stream"));
    if (params.block_align == 0) return fail(Status::Decode, "ADPCM stream without a block size");
    params_ = params;
    if (!ima_) {
      coefficients_ = ms_adpcm_standard_coefficients();
      util::ByteReader r(params.extradata);
      if (r.remaining() >= 4) {
        r.skip(2);
        const std::uint16_t count = r.u16le();
        MsAdpcmCoefficients custom;
        for (std::uint16_t i = 0; i < count && r.remaining() >= 4; ++i) {
          custom.c1.push_back(r.i16le());
          custom.c2.push_back(r.i16le());
        }
        if (!custom.c1.empty()) coefficients_ = custom;
      }
    }
    return {};
  }
  Result<void> send_packet(const Packet& packet) override {
    std::vector<float> out;
    if (ima_) {
      CVT_TRY(decode_ima_wav(packet.bytes, params_.channels, params_.block_align, out));
    } else {
      CVT_TRY(decode_ms_adpcm(packet.bytes, params_.channels, params_.block_align, coefficients_, out));
    }
    queue_.push(std::move(out), to_ms(packet.pts, params_.timescale));
    return {};
  }
  Result<std::optional<AudioChunk>> receive_chunk() override { return queue_.pop(); }
  Result<void> flush() override { return {}; }
  std::uint32_t sample_rate() const override { return params_.sample_rate; }
  std::uint16_t channels() const override { return params_.channels; }

 private:
  bool ima_;
  codec::CodecParams params_;
  MsAdpcmCoefficients coefficients_;
  ChunkQueue queue_;
};

class FlacDecoder final : public codec::AudioDecoder {
 public:
  Result<void> configure(const codec::CodecParams& params) override {
    params_ = params;
    ByteView extra = params.extradata;
    if (extra.size() >= 4 && as_chars(extra.first(4)) == "fLaC") extra = extra.subspan(4);
    if (extra.size() >= 38 && std::to_integer<std::uint8_t>(extra[0] & std::byte{0x7F}) == 0 &&
        util::ByteReader(extra.subspan(1, 3)).u24be() == 34) {
      extra = extra.subspan(4);
    }
    if (extra.size() >= 34) {
      CVT_TRY_ASSIGN(info_, flac::parse_streaminfo(extra.first(34)));
      have_info_ = true;
    }
    return {};
  }
  Result<void> send_packet(const Packet& packet) override {
    ByteView data = packet.bytes;
    std::vector<std::int32_t> ints;
    while (data.size() >= 2) {
      flac::FrameInfo frame;
      CVT_TRY_ASSIGN(const std::size_t used, flac::decode_frame(data, have_info_ ? &info_ : nullptr, ints, frame));
      if (!have_info_) {
        info_.sample_rate = frame.sample_rate;
        info_.channels = frame.channels;
        info_.bits = frame.bits;
        have_info_ = true;
      }
      bits_ = frame.bits;
      data = data.subspan(used);
    }
    std::vector<float> out(ints.size());
    const double scale = 1.0 / static_cast<double>(std::uint64_t{1} << (bits_ - 1));
    for (std::size_t i = 0; i < ints.size(); ++i) out[i] = static_cast<float>(ints[i] * scale);
    queue_.push(std::move(out), to_ms(packet.pts, params_.timescale));
    return {};
  }
  Result<std::optional<AudioChunk>> receive_chunk() override { return queue_.pop(); }
  Result<void> flush() override { return {}; }
  std::uint32_t sample_rate() const override { return have_info_ ? info_.sample_rate : params_.sample_rate; }
  std::uint16_t channels() const override {
    return static_cast<std::uint16_t>(have_info_ ? info_.channels : params_.channels);
  }

 private:
  codec::CodecParams params_;
  flac::StreamInfo info_;
  bool have_info_ = false;
  std::uint32_t bits_ = 16;
  ChunkQueue queue_;
};

class AlacDecoder final : public codec::AudioDecoder {
 public:
  Result<void> configure(const codec::CodecParams& params) override {
    params_ = params;
    CVT_TRY_ASSIGN(const alac::Config config, alac::parse_config(params.extradata));
    return decoder_.configure(config);
  }
  Result<void> send_packet(const Packet& packet) override {
    std::vector<std::int32_t> ints;
    CVT_TRY(decoder_.decode(packet.bytes, ints));
    std::vector<float> out(ints.size());
    const double scale = 1.0 / static_cast<double>(std::uint64_t{1} << (decoder_.config().bit_depth - 1));
    for (std::size_t i = 0; i < ints.size(); ++i) out[i] = static_cast<float>(ints[i] * scale);
    queue_.push(std::move(out), to_ms(packet.pts, params_.timescale));
    return {};
  }
  Result<std::optional<AudioChunk>> receive_chunk() override { return queue_.pop(); }
  Result<void> flush() override { return {}; }
  std::uint32_t sample_rate() const override {
    return decoder_.config().sample_rate != 0 ? decoder_.config().sample_rate : params_.sample_rate;
  }
  std::uint16_t channels() const override { return decoder_.config().channels; }

 private:
  codec::CodecParams params_;
  alac::Decoder decoder_;
  ChunkQueue queue_;
};

class WavPackDecoder final : public codec::AudioDecoder {
 public:
  Result<void> configure(const codec::CodecParams& params) override {
    params_ = params;
    return {};
  }
  Result<void> send_packet(const Packet& packet) override {
    std::vector<float> out;
    CVT_TRY(decoder_.decode_blocks(packet.bytes, out));
    queue_.push(std::move(out), to_ms(packet.pts, params_.timescale));
    return {};
  }
  Result<std::optional<AudioChunk>> receive_chunk() override { return queue_.pop(); }
  Result<void> flush() override { return {}; }
  std::uint32_t sample_rate() const override {
    return decoder_.sample_rate() != 0 ? decoder_.sample_rate() : params_.sample_rate;
  }
  std::uint16_t channels() const override {
    return decoder_.channels() != 0 ? decoder_.channels() : params_.channels;
  }

 private:
  codec::CodecParams params_;
  wavpack::StreamDecoder decoder_;
  ChunkQueue queue_;
};

// =========================================================================== encoders

// Buffers input to whole packets of `frame_size` frames.
class BufferedEncoder : public codec::AudioEncoder {
 public:
  Result<void> send_chunk(const AudioChunk& chunk) override {
    pending_.insert(pending_.end(), chunk.samples.begin(), chunk.samples.end());
    const std::size_t packet = std::size_t{frame_size_} * channels_;
    std::size_t at = 0;
    while (pending_.size() - at >= packet) {
      CVT_TRY(emit(std::span<const float>(pending_).subspan(at, packet), frame_size_));
      at += packet;
    }
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(at));
    return {};
  }
  Result<std::optional<Packet>> receive_packet() override {
    if (packets_.empty()) return std::optional<Packet>();
    Packet p = std::move(packets_.front());
    packets_.pop_front();
    return std::optional<Packet>(std::move(p));
  }
  Result<void> flush() override {
    if (!pending_.empty()) {
      const auto frames = static_cast<std::uint32_t>(pending_.size() / channels_);
      CVT_TRY(emit(pending_, frames));
      pending_.clear();
    }
    return finish();
  }
  std::uint32_t frame_size() const override { return frame_size_; }
  std::uint32_t delay() const override { return 0; }

 protected:
  virtual Result<void> emit(std::span<const float> samples, std::uint32_t frames) = 0;
  virtual Result<void> finish() { return {}; }
  void push(Bytes bytes, std::uint32_t frames) {
    Packet p;
    p.bytes = std::move(bytes);
    p.pts = static_cast<std::int64_t>(position_) * timescale_ / rate_;
    p.dts = p.pts;
    p.duration = static_cast<std::int64_t>(frames) * timescale_ / rate_;
    p.keyframe = true;
    position_ += frames;
    packets_.push_back(std::move(p));
  }
  Result<void> base_configure(const codec::AudioEncodeParams& params, std::uint32_t frame_size) {
    if (params.sample_rate == 0 || params.channels == 0) {
      return fail(Status::InvalidArgument, "the encoder needs a sample rate and a channel count");
    }
    rate_ = params.sample_rate;
    channels_ = params.channels;
    timescale_ = params.timescale != 0 ? params.timescale : params.sample_rate;
    frame_size_ = frame_size;
    return {};
  }
  std::uint32_t rate_ = 0;
  std::uint16_t channels_ = 0;
  std::uint32_t timescale_ = 0;
  std::uint32_t frame_size_ = 1024;

 private:
  std::vector<float> pending_;
  std::deque<Packet> packets_;
  std::uint64_t position_ = 0;
};

class PcmEncoder final : public BufferedEncoder {
 public:
  explicit PcmEncoder(PcmFormat layout) : layout_(layout) {}
  Result<void> configure(const codec::AudioEncodeParams& params) override { return base_configure(params, 1024); }
  Bytes extradata() const override { return {}; }

 private:
  Result<void> emit(std::span<const float> samples, std::uint32_t frames) override {
    push(encode_pcm(samples, channels_, layout_, dsp::Dither::Triangular, rate_), frames);
    return {};
  }
  PcmFormat layout_;
};

class AdpcmEncoder final : public BufferedEncoder {
 public:
  explicit AdpcmEncoder(bool ima) : ima_(ima) {}
  Result<void> configure(const codec::AudioEncodeParams& params) override {
    if (!ima_ && params.channels > 2) return fail(Status::InvalidArgument, "Microsoft ADPCM holds at most two channels");
    const std::size_t scale = params.sample_rate <= 11025 ? 1 : params.sample_rate <= 22050 ? 2 : 4;
    block_align_ = 256u * params.channels * scale;
    if (block_align_ > 65535) return fail(Status::InvalidArgument, "too many channels for ADPCM");
    const std::size_t per = ima_ ? ima_wav_frames_per_block(block_align_, params.channels)
                                 : ms_adpcm_frames_per_block(block_align_, params.channels);
    return base_configure(params, static_cast<std::uint32_t>(per));
  }
  Bytes extradata() const override {
    util::ByteWriter w;
    w.u16le(static_cast<std::uint16_t>(frame_size_));
    if (!ima_) {
      const MsAdpcmCoefficients c = ms_adpcm_standard_coefficients();
      w.u16le(static_cast<std::uint16_t>(c.c1.size()));
      for (std::size_t i = 0; i < c.c1.size(); ++i) {
        w.i16le(c.c1[i]);
        w.i16le(c.c2[i]);
      }
    }
    return w.take();
  }
  /// Bytes per block, for the muxer's block_align field.
  [[nodiscard]] std::size_t block_align() const noexcept { return block_align_; }

 private:
  Result<void> emit(std::span<const float> samples, std::uint32_t frames) override {
    const std::vector<std::int32_t> ints = dsp::to_integers(samples, channels_, 16, dsp::Dither::Triangular, rate_);
    const std::vector<std::int16_t> pcm(ints.begin(), ints.end());
    push(ima_ ? encode_ima_wav(pcm, channels_, block_align_, &ima_steps_) : encode_ms_adpcm(pcm, channels_, block_align_), frames);
    return {};
  }
  bool ima_;
  std::size_t block_align_ = 0;
  std::vector<int> ima_steps_;
};

class FlacEncoder final : public BufferedEncoder {
 public:
  explicit FlacEncoder(int bits) : bits_(bits) {}
  Result<void> configure(const codec::AudioEncodeParams& params) override {
    if (params.bits_per_sample != 0) bits_ = std::clamp(static_cast<int>(params.bits_per_sample), 4, 32);
    const flac::Settings settings = flac::settings_for_level(std::clamp(params.compression_level, 0, 8));
    CVT_TRY(encoder_.configure(params.sample_rate, params.channels, static_cast<std::uint32_t>(bits_), settings));
    info_.sample_rate = params.sample_rate;
    info_.channels = params.channels;
    info_.bits = static_cast<std::uint32_t>(bits_);
    info_.min_block = info_.max_block = settings.block_size;
    info_.min_frame = 0xFFFFFF;
    md5_.emplace(static_cast<std::uint32_t>(bits_));
    return base_configure(params, settings.block_size);
  }
  Bytes extradata() const override {
    flac::StreamInfo info = info_;
    if (info.min_frame == 0xFFFFFF) info.min_frame = 0;
    return flac::serialize_streaminfo(info);
  }

 private:
  Result<void> emit(std::span<const float> samples, std::uint32_t frames) override {
    const std::vector<std::int32_t> ints = dsp::to_integers(samples, channels_, bits_, dsp::Dither::Triangular, rate_);
    md5_->update(ints);
    Bytes frame = encoder_.encode(ints, frames, index_++);
    info_.min_frame = std::min<std::uint32_t>(info_.min_frame, static_cast<std::uint32_t>(frame.size()));
    info_.max_frame = std::max<std::uint32_t>(info_.max_frame, static_cast<std::uint32_t>(frame.size()));
    info_.total_samples += frames;
    push(std::move(frame), frames);
    return {};
  }
  Result<void> finish() override {
    if (md5_) info_.md5 = md5_->finish();
    md5_.reset();
    return {};
  }
  int bits_;
  flac::FrameEncoder encoder_;
  flac::StreamInfo info_;
  std::optional<flac::SampleMd5> md5_;
  std::uint64_t index_ = 0;
};

class AlacEncoder final : public BufferedEncoder {
 public:
  explicit AlacEncoder(int bits) : bits_(bits) {}
  Result<void> configure(const codec::AudioEncodeParams& params) override {
    if (params.bits_per_sample != 0) {
      const int b = params.bits_per_sample;
      bits_ = b <= 16 ? 16 : b <= 20 ? 20 : b <= 24 ? 24 : 32;
    }
    if (params.channels > 8) return fail(Status::InvalidArgument, "ALAC holds at most 8 channels");
    config_.bit_depth = static_cast<std::uint8_t>(bits_);
    config_.channels = static_cast<std::uint8_t>(params.channels);
    config_.sample_rate = params.sample_rate;
    CVT_TRY(encoder_.configure(config_, std::clamp(params.compression_level, 0, 9)));
    return base_configure(params, config_.frame_length);
  }
  Bytes extradata() const override { return alac::serialize_config(config_); }

 private:
  Result<void> emit(std::span<const float> samples, std::uint32_t frames) override {
    const std::vector<std::int32_t> ints = dsp::to_integers(samples, channels_, bits_, dsp::Dither::Triangular, rate_);
    Bytes packet = encoder_.encode(ints, frames);
    config_.max_frame_bytes = std::max<std::uint32_t>(config_.max_frame_bytes, static_cast<std::uint32_t>(packet.size()));
    push(std::move(packet), frames);
    return {};
  }
  int bits_;
  alac::Config config_;
  alac::Encoder encoder_;
};

class WavPackEncoder final : public BufferedEncoder {
 public:
  explicit WavPackEncoder(int bits) : bits_(bits) {}
  Result<void> configure(const codec::AudioEncodeParams& params) override {
    if (params.bits_per_sample != 0) bits_ = std::clamp(static_cast<int>(params.bits_per_sample), 8, 32);
    CVT_TRY(encoder_.configure(params.sample_rate, params.channels, bits_, std::clamp(params.compression_level, 0, 9)));
    return base_configure(params, encoder_.block_frames());
  }
  Bytes extradata() const override { return {}; }

 private:
  Result<void> emit(std::span<const float> samples, std::uint32_t frames) override {
    const std::vector<std::int32_t> ints = dsp::to_integers(samples, channels_, bits_, dsp::Dither::Triangular, rate_);
    CVT_TRY_ASSIGN(Bytes block, encoder_.encode_block(ints, frames));
    push(std::move(block), frames);
    return {};
  }
  int bits_;
  wavpack::BlockEncoder encoder_;
};

}  // namespace

std::unique_ptr<codec::AudioEncoder> make_lossless_encoder(std::string_view codec_id, int bits) {
  if (codec_id == codec::id::flac) return std::make_unique<FlacEncoder>(std::clamp(bits, 4, 32));
  if (codec_id == codec::id::alac) {
    const int b = bits <= 16 ? 16 : bits <= 20 ? 20 : bits <= 24 ? 24 : 32;
    return std::make_unique<AlacEncoder>(b);
  }
  if (codec_id == codec::id::wavpack) return std::make_unique<WavPackEncoder>(std::clamp(bits, 8, 32));
  return nullptr;
}

void register_media_codecs(Registry& registry) {
  codec::Registry& media = registry.media();
  for (const std::string_view id : {codec::id::pcm_s16le, codec::id::pcm_s16be, codec::id::pcm_s24le, codec::id::pcm_f32le,
                                    codec::id::pcm_u8, codec::id::pcm_alaw, codec::id::pcm_mulaw, codec::id::pcm_s24be,
                                    codec::id::pcm_s32le, codec::id::pcm_f64le, codec::id::pcm_s8}) {
    const PcmFormat layout = *pcm_for(id);
    media.register_audio_decoder(id, [] { return std::make_unique<PcmDecoder>(); });
    media.register_audio_encoder(id, [layout] { return std::make_unique<PcmEncoder>(layout); });
  }
  media.register_audio_decoder(codec::id::adpcm_ima, [] { return std::make_unique<AdpcmDecoder>(true); });
  media.register_audio_encoder(codec::id::adpcm_ima, [] { return std::make_unique<AdpcmEncoder>(true); });
  media.register_audio_decoder(codec::id::adpcm_ms, [] { return std::make_unique<AdpcmDecoder>(false); });
  media.register_audio_decoder(codec::id::adpcm_ima_qt, [] { return std::make_unique<BlockDecoder>(BlockDecoder::Kind::Ima4); });
  media.register_audio_decoder(codec::id::gsm_ms, [] { return std::make_unique<BlockDecoder>(BlockDecoder::Kind::Gsm); });
  media.register_audio_encoder(codec::id::adpcm_ms, [] { return std::make_unique<AdpcmEncoder>(false); });
  media.register_audio_decoder(codec::id::flac, [] { return std::make_unique<FlacDecoder>(); });
  media.register_audio_encoder(codec::id::flac, [] { return std::make_unique<FlacEncoder>(16); });
  media.register_audio_decoder(codec::id::alac, [] { return std::make_unique<AlacDecoder>(); });
  media.register_audio_encoder(codec::id::alac, [] { return std::make_unique<AlacEncoder>(16); });
  media.register_audio_decoder(codec::id::wavpack, [] { return std::make_unique<WavPackDecoder>(); });
  media.register_audio_encoder(codec::id::wavpack, [] { return std::make_unique<WavPackEncoder>(16); });
}

}  // namespace cvt::audio
