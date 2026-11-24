// Convertible core - audio_lossy family: Speex through the imported libspeex,
// in Ogg (Speex header, comments, granule positions, in-band stereo) and as
// a decoder for other containers.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "families/audio_lossless/common.hpp"
#include "families/audio_lossless/tags.hpp"
#include "families/audio_lossy/xiph/ogg_container.hpp"
#include "families/audio_lossy/xiph/xiph.hpp"
#include "util/bytes.hpp"

#include "families/audio_lossy/xiph/speex_rename.h"
#include <speex/speex.h>
#include <speex/speex_callbacks.h>
#include <speex/speex_stereo.h>

namespace cvt::lossy::xiph {

namespace {

const SpeexMode* mode_for(int id) {
  switch (id) {
    case 0: return &speex_nb_mode;
    case 1: return &speex_wb_mode;
    case 2: return &speex_uwb_mode;
    default: return nullptr;
  }
}

std::int32_t le32s(const std::uint8_t* p) {
  return static_cast<std::int32_t>(p[0] | (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16) | (std::uint32_t{p[3]} << 24));
}

struct SpeexHeader {
  int rate = 8000;
  int mode = 0;
  int channels = 1;
  int frames_per_packet = 1;
};

std::optional<SpeexHeader> parse_header(ByteView data) {
  const auto d = as_u8(data);
  if (d.size() < 80 || ::cvt::util::compare_bytes(d.data(), "Speex   ", 8) != 0) return std::nullopt;
  SpeexHeader h;
  h.rate = le32s(d.data() + 36);
  h.mode = le32s(d.data() + 40);
  h.channels = le32s(d.data() + 48);
  h.frames_per_packet = le32s(d.data() + 64);
  if (h.mode < 0 || h.mode > 2 || h.channels < 1 || h.channels > 2 || h.rate < 1000 || h.rate > 96000) return std::nullopt;
  if (h.frames_per_packet < 1 || h.frames_per_packet > 10) h.frames_per_packet = 1;
  return h;
}

class Decoder {
 public:
  ~Decoder() {
    if (state_ != nullptr) speex_decoder_destroy(state_);
    if (bits_ready_) speex_bits_destroy(&bits_);
  }
  Result<void> open(const SpeexHeader& h) {
    const SpeexMode* mode = mode_for(h.mode);
    if (mode == nullptr) return fail(Status::Decode, "unsupported Speex mode");
    state_ = speex_decoder_init(mode);
    if (state_ == nullptr) return fail(Status::Decode, "the Speex decoder failed to start");
    int enhance = 1;
    speex_decoder_ctl(state_, SPEEX_SET_ENH, &enhance);
    speex_decoder_ctl(state_, SPEEX_GET_FRAME_SIZE, &frame_size_);
    if (frame_size_ <= 0 || frame_size_ > 2048) return fail(Status::Decode, "invalid Speex frame size");
    std::int32_t rate = h.rate;
    speex_decoder_ctl(state_, SPEEX_SET_SAMPLING_RATE, &rate);
    speex_bits_init(&bits_);
    bits_ready_ = true;
    channels_ = h.channels;
    frames_per_packet_ = h.frames_per_packet;
    if (channels_ == 2) {
      callback_.callback_id = SPEEX_INBAND_STEREO;
      callback_.func = speex_std_stereo_request_handler;
      callback_.data = &stereo_;
      speex_decoder_ctl(state_, SPEEX_SET_HANDLER, &callback_);
    }
    frame_.resize(static_cast<std::size_t>(frame_size_) * 2);
    return {};
  }
  void decode(ByteView packet, std::vector<float>& out) {
    speex_bits_read_from(&bits_, const_cast<char*>(reinterpret_cast<const char*>(packet.data())), static_cast<int>(packet.size()));
    for (int f = 0; f < frames_per_packet_; ++f) {
      const int r = speex_decode(state_, &bits_, frame_.data());
      if (r == -1) break;  // end of stream marker
      if (r == -2 || speex_bits_remaining(&bits_) < 0) {
        damaged_ = true;
        break;
      }
      if (channels_ == 2) speex_decode_stereo(frame_.data(), frame_size_, &stereo_);
      const std::size_t n = static_cast<std::size_t>(frame_size_) * static_cast<std::size_t>(channels_);
      for (std::size_t i = 0; i < n; ++i) out.push_back(std::clamp(frame_[i] / 32768.0f, -1.0f, 1.0f));
    }
  }
  [[nodiscard]] bool damaged() const noexcept { return damaged_; }

 private:
  void* state_ = nullptr;
  SpeexBits bits_{};
  bool bits_ready_ = false;
  SpeexStereoState stereo_ = SPEEX_STEREO_STATE_INIT;
  SpeexCallback callback_{};
  std::vector<float> frame_;
  int frame_size_ = 160;
  int channels_ = 1;
  int frames_per_packet_ = 1;
  bool damaged_ = false;
};

}  // namespace

Result<ir::Audio> read_ogg_speex(ByteView input, const Context& ctx) {
  CVT_TRY_ASSIGN(OggLogicalStream stream, ogg_find_stream(input, "Speex   ", ctx));
  const auto header = parse_header(stream.packets.front());
  if (!header) return fail(Status::Decode, "invalid Speex header");
  Decoder decoder;
  CVT_TRY(decoder.open(*header));
  ir::Audio audio;
  audio.sample_rate = static_cast<std::uint32_t>(header->rate);
  audio.channels = static_cast<std::uint16_t>(header->channels);
  if (stream.packets.size() > 1) audio::tags::parse_vorbis_comment(stream.packets[1], audio.tags);
  std::int64_t last_granule = -1;
  std::int64_t start_skip = 0;
  bool first_page = false;
  std::size_t last_page_packet = 0;
  for (std::size_t i = 0; i < stream.granules.size(); ++i) {
    if (stream.granules[i] >= 0) last_page_packet = i;
  }
  for (std::size_t i = 2; i < stream.packets.size(); ++i) {
    if ((i & 63) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(stream.packets.size()), "Decoding"));
    decoder.decode(stream.packets[i], audio.samples);
    CVT_TRY(audio::check_sample_budget(ctx, audio.samples.size()));
    if (stream.granules[i] >= 0) {
      const auto decoded = static_cast<std::int64_t>(audio.samples.size() / audio.channels);
      if (!first_page) {
        first_page = true;
        if (i != last_page_packet && stream.granules[i] < decoded) start_skip = decoded - stream.granules[i];
      }
      last_granule = stream.granules[i];
    }
  }
  if (decoder.damaged() || stream.damaged) ctx.warn("Damaged Speex packets were skipped.");
  const std::size_t ch = audio.channels;
  std::uint64_t decoded = audio.samples.size() / ch;
  std::uint64_t keep = decoded;
  if (last_granule >= 0 && stream.complete) {
    keep = std::min<std::uint64_t>(decoded, static_cast<std::uint64_t>(std::max<std::int64_t>(0, last_granule + start_skip)));
  }
  audio.samples.resize(static_cast<std::size_t>(keep) * ch);
  const auto skip = static_cast<std::size_t>(std::min<std::int64_t>(start_skip, static_cast<std::int64_t>(keep)));
  audio.samples.erase(audio.samples.begin(), audio.samples.begin() + static_cast<std::ptrdiff_t>(skip * ch));
  return audio;
}

Result<Bytes> write_ogg_speex(const ir::Audio& audio, const SpeexSettings& settings, const Context& ctx) {
  if (audio.channels != 1) return fail(Status::Encode, "Speex is written as mono");
  const int mode_id = audio.sample_rate <= 8000 ? 0 : audio.sample_rate <= 16000 ? 1 : 2;
  const SpeexMode* mode = mode_for(mode_id);
  void* state = speex_encoder_init(mode);
  if (state == nullptr) return fail(Status::Encode, "the Speex encoder failed to start");
  struct Cleanup {
    void* state;
    SpeexBits* bits;
    ~Cleanup() {
      speex_bits_destroy(bits);
      speex_encoder_destroy(state);
    }
  };
  SpeexBits bits;
  speex_bits_init(&bits);
  Cleanup cleanup{state, &bits};
  int quality = std::clamp(settings.quality, 0, 10);
  speex_encoder_ctl(state, SPEEX_SET_QUALITY, &quality);
  int complexity = 4;
  speex_encoder_ctl(state, SPEEX_SET_COMPLEXITY, &complexity);
  std::int32_t rate = static_cast<std::int32_t>(audio.sample_rate);
  speex_encoder_ctl(state, SPEEX_SET_SAMPLING_RATE, &rate);
  int frame_size = 160, lookahead = 0;
  speex_encoder_ctl(state, SPEEX_GET_FRAME_SIZE, &frame_size);
  speex_encoder_ctl(state, SPEEX_GET_LOOKAHEAD, &lookahead);
  // Header packet (80 bytes, little-endian fields).
  util::ByteWriter head;
  head.text("Speex   ");
  std::array<char, 20> version{};
  ::cvt::util::copy_bytes(version.data(), "1.2.1", 5);
  head.bytes(as_bytes(std::string_view(version.data(), version.size())));
  head.u32le(1);   // version id
  head.u32le(80);  // header size
  head.u32le(audio.sample_rate);
  head.u32le(static_cast<std::uint32_t>(mode_id));
  head.u32le(4);   // mode bit-stream version
  head.u32le(1);   // channels
  head.u32le(static_cast<std::uint32_t>(-1));  // bitrate unknown
  head.u32le(static_cast<std::uint32_t>(frame_size));
  head.u32le(0);   // vbr
  head.u32le(1);   // frames per packet
  head.u32le(0);   // extra headers
  head.u32le(0);
  head.u32le(0);
  OggWriter ogg(0x53504558u);
  ogg.add(head.view(), 0, true);
  ogg.add(audio::tags::vorbis_comment(audio.tags), 0, true);
  const std::size_t frames = audio.frame_count();
  const std::size_t total = frames + static_cast<std::size_t>(lookahead);
  const std::size_t packets = (total + static_cast<std::size_t>(frame_size) - 1) / static_cast<std::size_t>(frame_size);
  std::vector<float> buffer(static_cast<std::size_t>(frame_size));
  std::vector<char> out(4096);
  for (std::size_t p = 0; p < packets; ++p) {
    if ((p & 127) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(p) / static_cast<double>(packets), "Encoding"));
    for (std::size_t i = 0; i < buffer.size(); ++i) {
      const std::size_t n = p * buffer.size() + i;
      buffer[i] = n < frames ? std::clamp(audio.samples[n], -1.0f, 1.0f) * 32767.0f : 0.0f;
    }
    speex_bits_reset(&bits);
    speex_encode(state, buffer.data(), &bits);
    const int bytes = speex_bits_write(&bits, out.data(), static_cast<int>(out.size()));
    const auto granule = static_cast<std::int64_t>(std::min((p + 1) * buffer.size(), total)) - lookahead;
    ogg.add(as_bytes(std::string_view(out.data(), static_cast<std::size_t>(bytes))), granule, false);
  }
  return ogg.finish();
}

namespace {

class SpeexPacketDecoder final : public codec::AudioDecoder {
 public:
  Result<void> configure(const codec::CodecParams& params) override {
    std::optional<SpeexHeader> h = parse_header(params.extradata);
    if (!h) {
      SpeexHeader g;
      g.rate = static_cast<int>(params.sample_rate != 0 ? params.sample_rate : 8000);
      g.mode = g.rate <= 8000 ? 0 : g.rate <= 16000 ? 1 : 2;
      g.channels = params.channels == 2 ? 2 : 1;
      h = g;
    }
    header_ = *h;
    return decoder_.open(header_);
  }
  Result<void> send_packet(const codec::Packet& packet) override {
    decoder_.decode(packet.bytes, pending_);
    return {};
  }
  Result<std::optional<codec::AudioChunk>> receive_chunk() override {
    if (pending_.empty()) return std::optional<codec::AudioChunk>{};
    codec::AudioChunk chunk;
    chunk.samples = std::move(pending_);
    pending_.clear();
    return std::optional<codec::AudioChunk>(std::move(chunk));
  }
  Result<void> flush() override { return {}; }
  [[nodiscard]] std::uint32_t sample_rate() const override { return static_cast<std::uint32_t>(header_.rate); }
  [[nodiscard]] std::uint16_t channels() const override { return static_cast<std::uint16_t>(header_.channels); }

 private:
  Decoder decoder_;
  SpeexHeader header_;
  std::vector<float> pending_;
};

}  // namespace

std::unique_ptr<codec::AudioDecoder> make_speex_decoder() { return std::make_unique<SpeexPacketDecoder>(); }

}  // namespace cvt::lossy::xiph
