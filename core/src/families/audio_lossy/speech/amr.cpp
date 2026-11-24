// Convertible core - audio_lossy family: AMR-NB and AMR-WB in the RFC 4867
// storage format (.amr, .awb) through the imported opencore-amr decoders and
// encoders (AMR-WB encoding by vo-amrwbenc), and as registry codecs.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "util/bytes.hpp"
#include "families/audio_lossy/speech/speech.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "families/audio_lossless/common.hpp"

// The libraries are built with every global name prefixed (see the
// *_rename.h headers); only the entry points used here are mapped, so that
// the libraries' generic internal names cannot leak into this file.
#define Decoder_Interface_init cvt_oc_Decoder_Interface_init
#define Decoder_Interface_exit cvt_oc_Decoder_Interface_exit
#define Decoder_Interface_Decode cvt_oc_Decoder_Interface_Decode
#define Encoder_Interface_init cvt_oc_Encoder_Interface_init
#define Encoder_Interface_exit cvt_oc_Encoder_Interface_exit
#define Encoder_Interface_Encode cvt_oc_Encoder_Interface_Encode
#define D_IF_init cvt_oc_D_IF_init
#define D_IF_exit cvt_oc_D_IF_exit
#define D_IF_decode cvt_oc_D_IF_decode
#define E_IF_init cvt_voamr_E_IF_init
#define E_IF_exit cvt_voamr_E_IF_exit
#define E_IF_encode cvt_voamr_E_IF_encode
#include <dec_if.h>
#include <enc_if.h>
#include <interf_dec.h>
#include <interf_enc.h>

namespace cvt::lossy::speech {

namespace {

constexpr std::string_view nb_magic = "#!AMR\n";
constexpr std::string_view wb_magic = "#!AMR-WB\n";
// Frame sizes including the table-of-contents byte, by frame type.
constexpr std::array<std::uint8_t, 16> nb_sizes = {13, 14, 16, 18, 20, 21, 27, 32, 6, 1, 1, 1, 1, 1, 1, 1};
constexpr std::array<std::uint8_t, 16> wb_sizes = {18, 24, 33, 37, 41, 47, 51, 59, 61, 6, 1, 1, 1, 1, 1, 1};
constexpr std::array<std::uint32_t, 8> nb_rates = {4750, 5150, 5900, 6700, 7400, 7950, 10200, 12200};
constexpr std::array<std::uint32_t, 9> wb_rates = {6600, 8850, 12650, 14250, 15850, 18250, 19850, 23050, 23850};

class AmrDecoder {
 public:
  explicit AmrDecoder(bool wide) : wide_(wide) { state_ = wide ? D_IF_init() : Decoder_Interface_init(); }
  ~AmrDecoder() {
    if (state_ == nullptr) return;
    if (wide_) D_IF_exit(state_);
    else Decoder_Interface_exit(state_);
  }
  AmrDecoder(const AmrDecoder&) = delete;
  AmrDecoder& operator=(const AmrDecoder&) = delete;
  /// One storage-format frame (TOC byte first, at least its full size).
  void decode(const std::uint8_t* frame, std::vector<float>& out) {
    std::array<std::uint8_t, 64> buffer{};
    const std::size_t size = (wide_ ? wb_sizes : nb_sizes)[(frame[0] >> 3) & 15];
    ::cvt::util::copy_bytes(buffer.data(), frame, size);
    std::array<short, 320> pcm{};
    if (wide_) D_IF_decode(state_, buffer.data(), pcm.data(), 0);
    else Decoder_Interface_Decode(state_, buffer.data(), pcm.data(), 0);
    const std::size_t n = wide_ ? 320 : 160;
    for (std::size_t i = 0; i < n; ++i) out.push_back(pcm[i] / 32768.0f);
  }

 private:
  bool wide_;
  void* state_ = nullptr;
};

}  // namespace

bool is_amr(ByteView data, bool& wide) {
  const std::string_view s = as_chars(data);
  if (s.starts_with(wb_magic)) {
    wide = true;
    return true;
  }
  if (s.starts_with(nb_magic)) {
    wide = false;
    return true;
  }
  return false;
}

Result<ir::Audio> read_amr(ByteView data, const Context& ctx) {
  bool wide = false;
  if (!is_amr(data, wide)) return fail(Status::Decode, "not an AMR file (missing #!AMR header)");
  const auto d = as_u8(data);
  std::size_t pos = wide ? wb_magic.size() : nb_magic.size();
  const auto& sizes = wide ? wb_sizes : nb_sizes;
  ir::Audio audio;
  audio.sample_rate = wide ? 16000 : 8000;
  audio.channels = 1;
  AmrDecoder decoder(wide);
  std::size_t frames = 0;
  bool damaged = false;
  while (pos < d.size()) {
    const std::size_t size = sizes[(d[pos] >> 3) & 15];
    if ((d[pos] & 0x83) != 0 || pos + size > d.size()) {
      damaged = true;
      if (pos + size > d.size()) break;
    }
    decoder.decode(d.data() + pos, audio.samples);
    pos += size;
    if ((++frames & 255) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(pos) / static_cast<double>(d.size()), "Decoding"));
    CVT_TRY(audio::check_sample_budget(ctx, audio.samples.size()));
  }
  if (frames == 0) return fail(Status::Decode, "the AMR file holds no frames");
  if (damaged) ctx.warn("The AMR file is damaged; some frames may be wrong.");
  return audio;
}

Result<Bytes> write_amr(const ir::Audio& audio, bool wide, std::uint32_t bitrate, bool dtx, const Context& ctx) {
  if (audio.channels != 1 || audio.sample_rate != (wide ? 16000u : 8000u)) {
    return fail(Status::Encode, "AMR{} needs mono audio at {} Hz", wide ? "-WB" : "", wide ? 16000 : 8000);
  }
  // The highest mode whose rate does not exceed the request.
  int mode = 0;
  if (wide) {
    for (std::size_t i = 0; i < wb_rates.size(); ++i) {
      if (wb_rates[i] <= bitrate) mode = static_cast<int>(i);
    }
  } else {
    for (std::size_t i = 0; i < nb_rates.size(); ++i) {
      if (nb_rates[i] <= bitrate) mode = static_cast<int>(i);
    }
  }
  void* state = wide ? E_IF_init() : Encoder_Interface_init(dtx ? 1 : 0);
  if (state == nullptr) return fail(Status::Encode, "the AMR encoder failed to start");
  struct Cleanup {
    void* state;
    bool wide;
    ~Cleanup() {
      if (wide) E_IF_exit(state);
      else Encoder_Interface_exit(state);
    }
  } cleanup{state, wide};
  Bytes out(as_bytes(wide ? wb_magic : nb_magic).begin(), as_bytes(wide ? wb_magic : nb_magic).end());
  const std::size_t frame = wide ? 320 : 160;
  const std::size_t frames = (audio.frame_count() + frame - 1) / frame;
  std::vector<short> pcm(frame);
  std::array<unsigned char, 64> buffer{};
  for (std::size_t f = 0; f < frames; ++f) {
    if ((f & 255) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(f) / static_cast<double>(frames), "Encoding"));
    for (std::size_t i = 0; i < frame; ++i) {
      const std::size_t n = f * frame + i;
      const float v = n < audio.samples.size() ? std::clamp(audio.samples[n], -1.0f, 1.0f) * 32767.0f : 0.0f;
      pcm[i] = static_cast<short>(v < 0 ? v - 0.5f : v + 0.5f);
    }
    // The encoders expect 14/16-bit speech with the low bits clear.
    if (!wide) {
      for (short& s : pcm) s = static_cast<short>(s & ~7);
    }
    const int n = wide ? E_IF_encode(state, mode, pcm.data(), buffer.data(), dtx ? 1 : 0)
                       : Encoder_Interface_Encode(state, static_cast<Mode>(mode), pcm.data(), buffer.data(), 0);
    if (n <= 0 || n > static_cast<int>(buffer.size())) return fail(Status::Encode, "the AMR encoder failed");
    const auto* p = reinterpret_cast<const std::byte*>(buffer.data());
    out.insert(out.end(), p, p + n);
  }
  CVT_TRY(ctx.limits.check_output(out.size()));
  return out;
}

namespace {

class AmrPacketDecoder final : public codec::AudioDecoder {
 public:
  explicit AmrPacketDecoder(bool wide) : wide_(wide), decoder_(wide) {}
  Result<void> configure(const codec::CodecParams&) override { return {}; }
  Result<void> send_packet(const codec::Packet& packet) override {
    // A packet holds one or more storage-format frames.
    const auto d = as_u8(packet.bytes);
    const auto& sizes = wide_ ? wb_sizes : nb_sizes;
    std::size_t pos = 0;
    while (pos < d.size()) {
      const std::size_t size = sizes[(d[pos] >> 3) & 15];
      if (pos + size > d.size()) break;
      decoder_.decode(d.data() + pos, pending_);
      pos += size;
    }
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
  [[nodiscard]] std::uint32_t sample_rate() const override { return wide_ ? 16000 : 8000; }
  [[nodiscard]] std::uint16_t channels() const override { return 1; }

 private:
  bool wide_;
  AmrDecoder decoder_;
  std::vector<float> pending_;
};

}  // namespace

std::unique_ptr<codec::AudioDecoder> make_amr_decoder(bool wide) { return std::make_unique<AmrPacketDecoder>(wide); }

}  // namespace cvt::lossy::speech
