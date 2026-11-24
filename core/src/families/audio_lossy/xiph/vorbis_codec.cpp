// Convertible core - audio_lossy family: Vorbis through the imported
// libvorbis, in Ogg (granule positions, gapless start/end trimming, Vorbis
// comments) and as a codec for other containers (Xiph-laced headers).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "families/audio_lossless/common.hpp"
#include "families/audio_lossless/tags.hpp"
#include "families/audio_lossy/xiph/ogg_container.hpp"
#include "families/audio_lossy/xiph/xiph.hpp"
#include "util/bytes.hpp"

// The library's names carry a private prefix (see vorbis_rename.h).
#include "families/audio_lossy/xiph/vorbis_rename.h"
#include <vorbis/codec.h>
#include <vorbis/vorbisenc.h>

namespace cvt::lossy::xiph {

namespace {

ogg_packet make_packet(ByteView data, bool bos, std::int64_t number) {
  ogg_packet op{};
  op.packet = const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(data.data()));
  op.bytes = static_cast<long>(data.size());
  op.b_o_s = bos ? 1 : 0;
  op.e_o_s = 0;
  op.granulepos = -1;
  op.packetno = number;
  return op;
}

/// libvorbis synthesis state with RAII cleanup.
class Synthesis {
 public:
  Synthesis() {
    vorbis_info_init(&info_);
    vorbis_comment_init(&comment_);
  }
  ~Synthesis() {
    if (block_ready_) vorbis_block_clear(&block_);
    if (dsp_ready_) vorbis_dsp_clear(&dsp_);
    vorbis_comment_clear(&comment_);
    vorbis_info_clear(&info_);
  }
  Synthesis(const Synthesis&) = delete;
  Synthesis& operator=(const Synthesis&) = delete;

  Result<void> headers(ByteView id, ByteView comment, ByteView setup) {
    const ByteView packets[3] = {id, comment, setup};
    for (int i = 0; i < 3; ++i) {
      ogg_packet op = make_packet(packets[i], i == 0, i);
      if (vorbis_synthesis_headerin(&info_, &comment_, &op) != 0) {
        return fail(Status::Decode, "invalid Vorbis header packet {}", i + 1);
      }
    }
    if (info_.channels < 1 || info_.channels > 255 || info_.rate < 1 || info_.rate > 768000) {
      return fail(Status::Decode, "unsupported Vorbis stream parameters");
    }
    if (vorbis_synthesis_init(&dsp_, &info_) != 0) return fail(Status::Decode, "the Vorbis stream cannot be decoded");
    dsp_ready_ = true;
    if (vorbis_block_init(&dsp_, &block_) != 0) return fail(Status::Decode, "the Vorbis stream cannot be decoded");
    block_ready_ = true;
    return {};
  }
  /// Decode one audio packet; appends WAVE-ordered samples; returns frames.
  std::uint64_t decode(ByteView packet, std::int64_t number, std::vector<float>& out) {
    ogg_packet op = make_packet(packet, false, number);
    if (vorbis_synthesis(&block_, &op) == 0) {
      vorbis_synthesis_blockin(&dsp_, &block_);
    } else {
      damaged_ = !packet.empty();
    }
    std::uint64_t total = 0;
    float** pcm = nullptr;
    int n = 0;
    const auto ch = static_cast<std::size_t>(info_.channels);
    while ((n = vorbis_synthesis_pcmout(&dsp_, &pcm)) > 0) {
      const std::size_t at = out.size();
      out.resize(at + static_cast<std::size_t>(n) * ch);
      for (int i = 0; i < n; ++i) {
        for (std::size_t c = 0; c < ch; ++c) out[at + static_cast<std::size_t>(i) * ch + c] = pcm[c][i];
      }
      vorbis_to_wave(std::span<float>(out).subspan(at), info_.channels);
      vorbis_synthesis_read(&dsp_, n);
      total += static_cast<std::uint64_t>(n);
    }
    return total;
  }
  [[nodiscard]] int channels() const noexcept { return info_.channels; }
  [[nodiscard]] std::uint32_t rate() const noexcept { return static_cast<std::uint32_t>(info_.rate); }
  [[nodiscard]] bool damaged() const noexcept { return damaged_; }

 private:
  vorbis_info info_{};
  vorbis_comment comment_{};
  vorbis_dsp_state dsp_{};
  vorbis_block block_{};
  bool dsp_ready_ = false;
  bool block_ready_ = false;
  bool damaged_ = false;
};

/// libvorbis analysis state.
class Analysis {
 public:
  Analysis() { vorbis_info_init(&info_); }
  ~Analysis() {
    if (block_ready_) vorbis_block_clear(&block_);
    if (dsp_ready_) vorbis_dsp_clear(&dsp_);
    vorbis_info_clear(&info_);
  }
  Analysis(const Analysis&) = delete;
  Analysis& operator=(const Analysis&) = delete;

  /// Returns the three header packets (the comment packet is built by the
  /// caller: it carries no encoder identification).
  Result<void> open(int channels, std::uint32_t rate, const VorbisSettings& s, Bytes& id, Bytes& setup) {
    int r = 0;
    if (s.bitrate) {
      r = vorbis_encode_init(&info_, channels, static_cast<long>(rate), -1, static_cast<long>(*s.bitrate), -1);
    } else {
      r = vorbis_encode_init_vbr(&info_, channels, static_cast<long>(rate), std::clamp(s.quality, -0.1f, 1.0f));
    }
    if (r != 0) return fail(Status::Encode, "Vorbis cannot encode {} channels at {} Hz with these settings", channels, rate);
    if (vorbis_analysis_init(&dsp_, &info_) != 0) return fail(Status::Encode, "the Vorbis encoder failed to start");
    dsp_ready_ = true;
    vorbis_block_init(&dsp_, &block_);
    block_ready_ = true;
    vorbis_comment comment;
    vorbis_comment_init(&comment);
    ogg_packet h{}, hc{}, hs{};
    vorbis_analysis_headerout(&dsp_, &comment, &h, &hc, &hs);
    id = to_bytes(std::span<const std::uint8_t>(h.packet, static_cast<std::size_t>(h.bytes)));
    setup = to_bytes(std::span<const std::uint8_t>(hs.packet, static_cast<std::size_t>(hs.bytes)));
    vorbis_comment_clear(&comment);
    channels_ = channels;
    return {};
  }
  /// Feed WAVE-ordered samples (count = frames); `frames == 0` ends the stream.
  void write(const float* samples, std::size_t frames) {
    if (frames == 0) {
      vorbis_analysis_wrote(&dsp_, 0);
      return;
    }
    float** buffer = vorbis_analysis_buffer(&dsp_, static_cast<int>(frames));
    const auto map = vorbis_to_wave_map(channels_);
    const auto ch = static_cast<std::size_t>(channels_);
    for (std::size_t c = 0; c < ch; ++c) {
      const std::size_t src = static_cast<std::size_t>(std::find(map.begin(), map.begin() + static_cast<std::ptrdiff_t>(ch), static_cast<int>(c)) - map.begin());
      for (std::size_t i = 0; i < frames; ++i) buffer[c][i] = samples[i * ch + src];
    }
    vorbis_analysis_wrote(&dsp_, static_cast<int>(frames));
  }
  /// Packets that are ready: (bytes, granule).
  void collect(std::vector<std::pair<Bytes, std::int64_t>>& out) {
    while (vorbis_analysis_blockout(&dsp_, &block_) == 1) {
      vorbis_analysis(&block_, nullptr);
      vorbis_bitrate_addblock(&block_);
      ogg_packet op{};
      while (vorbis_bitrate_flushpacket(&dsp_, &op) != 0) {
        out.emplace_back(to_bytes(std::span<const std::uint8_t>(op.packet, static_cast<std::size_t>(op.bytes))),
                         static_cast<std::int64_t>(op.granulepos));
      }
    }
  }

 private:
  vorbis_info info_{};
  vorbis_dsp_state dsp_{};
  vorbis_block block_{};
  bool dsp_ready_ = false;
  bool block_ready_ = false;
  int channels_ = 0;
};

Bytes comment_packet(const ir::Audio& audio) {
  util::ByteWriter w;
  w.u8(3);
  w.text("vorbis");
  w.bytes(audio::tags::vorbis_comment(audio.tags));
  w.u8(1);  // framing bit
  return w.take();
}

/// The three header packets from Xiph lacing (Matroska CodecPrivate) or
/// from 16-bit length prefixes.
bool split_headers(ByteView extradata, ByteView& a, ByteView& b, ByteView& c) {
  const auto d = as_u8(extradata);
  if (d.size() >= 3 && d[0] == 2) {
    std::size_t pos = 1;
    std::size_t sizes[2] = {0, 0};
    for (std::size_t& size : sizes) {
      while (pos < d.size() && d[pos] == 255) {
        size += 255;
        ++pos;
      }
      if (pos >= d.size()) return false;
      size += d[pos++];
    }
    if (pos + sizes[0] + sizes[1] > d.size()) return false;
    a = extradata.subspan(pos, sizes[0]);
    b = extradata.subspan(pos + sizes[0], sizes[1]);
    c = extradata.subspan(pos + sizes[0] + sizes[1]);
    return true;
  }
  std::size_t pos = 0;
  ByteView* parts[3] = {&a, &b, &c};
  for (ByteView* part : parts) {
    if (d.size() - pos < 2) return false;
    const std::size_t size = (std::size_t{d[pos]} << 8) | d[pos + 1];
    pos += 2;
    if (d.size() - pos < size) return false;
    *part = extradata.subspan(pos, size);
    pos += size;
  }
  return true;
}

}  // namespace

Result<ir::Audio> read_ogg_vorbis(ByteView input, const Context& ctx) {
  CVT_TRY_ASSIGN(OggLogicalStream stream, ogg_find_stream(input, "\x01vorbis", ctx));
  if (stream.packets.size() < 3) return fail(Status::Decode, "the Vorbis stream has no complete headers");
  Synthesis synthesis;
  CVT_TRY(synthesis.headers(stream.packets[0], stream.packets[1], stream.packets[2]));
  ir::Audio audio;
  audio.sample_rate = synthesis.rate();
  audio.channels = static_cast<std::uint16_t>(synthesis.channels());
  if (stream.packets[1].size() > 7) audio::tags::parse_vorbis_comment(ByteView(stream.packets[1]).subspan(7), audio.tags);
  std::uint64_t decoded = 0;
  std::int64_t last_granule = -1;
  std::int64_t start_skip = 0;
  bool first_page_done = false;
  std::size_t last_page_packet = 0;
  for (std::size_t i = 0; i < stream.granules.size(); ++i) {
    if (stream.granules[i] >= 0) last_page_packet = i;
  }
  for (std::size_t i = 3; i < stream.packets.size(); ++i) {
    if ((i & 63) == 0) {
      CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(stream.packets.size()), "Decoding"));
    }
    decoded += synthesis.decode(stream.packets[i], static_cast<std::int64_t>(i), audio.samples);
    CVT_TRY(audio::check_sample_budget(ctx, audio.samples.size()));
    if (stream.granules[i] >= 0) {
      if (!first_page_done) {
        // A stream may start at a position after zero: the first page's
        // granule tells how many leading samples to drop.
        first_page_done = true;
        if (i != last_page_packet && stream.granules[i] < static_cast<std::int64_t>(decoded)) {
          start_skip = static_cast<std::int64_t>(decoded) - stream.granules[i];
        }
      }
      last_granule = stream.granules[i];
    }
  }
  if (synthesis.damaged() || stream.damaged) ctx.warn("Damaged Vorbis packets were skipped.");
  const std::size_t ch = audio.channels;
  std::uint64_t keep = decoded;
  if (last_granule >= 0 && stream.complete) {
    keep = std::min<std::uint64_t>(decoded, static_cast<std::uint64_t>(std::max<std::int64_t>(0, last_granule)));
  }
  const auto skip = static_cast<std::uint64_t>(std::min<std::int64_t>(start_skip, static_cast<std::int64_t>(keep)));
  audio.samples.resize(static_cast<std::size_t>(keep) * ch);
  audio.samples.erase(audio.samples.begin(), audio.samples.begin() + static_cast<std::ptrdiff_t>(skip * ch));
  return audio;
}

Result<Bytes> write_ogg_vorbis(const ir::Audio& audio, const VorbisSettings& settings, const Context& ctx) {
  Analysis analysis;
  Bytes id, setup;
  CVT_TRY(analysis.open(audio.channels, audio.sample_rate, settings, id, setup));
  OggWriter ogg(0x564F5242u);
  ogg.add(id, 0, true);
  ogg.add(comment_packet(audio), 0, false);
  ogg.add(setup, 0, true);
  const std::size_t frames = audio.frame_count();
  const std::size_t chunk = 4096;
  std::vector<std::pair<Bytes, std::int64_t>> packets;
  const auto flush_packets = [&]() -> Result<void> {
    for (auto& [bytes, granule] : packets) ogg.add(bytes, granule, false);
    packets.clear();
    return ctx.limits.check_output(ogg.size());
  };
  for (std::size_t first = 0; first < frames; first += chunk) {
    if ((first / chunk & 15) == 0) {
      CVT_TRY(ctx.checkpoint(static_cast<double>(first) / static_cast<double>(frames), "Encoding"));
    }
    const std::size_t n = std::min(chunk, frames - first);
    analysis.write(audio.samples.data() + first * audio.channels, n);
    analysis.collect(packets);
    CVT_TRY(flush_packets());
  }
  analysis.write(nullptr, 0);
  analysis.collect(packets);
  CVT_TRY(flush_packets());
  return ogg.finish();
}

// =========================================================================== registry

namespace {

class VorbisPacketDecoder final : public codec::AudioDecoder {
 public:
  Result<void> configure(const codec::CodecParams& params) override {
    ByteView a, b, c;
    if (!split_headers(params.extradata, a, b, c)) return fail(Status::Decode, "missing Vorbis headers");
    CVT_TRY(synthesis_.headers(a, b, c));
    return {};
  }
  Result<void> send_packet(const codec::Packet& packet) override {
    synthesis_.decode(packet.bytes, number_++, pending_);
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
  [[nodiscard]] std::uint32_t sample_rate() const override { return synthesis_.rate(); }
  [[nodiscard]] std::uint16_t channels() const override { return static_cast<std::uint16_t>(synthesis_.channels()); }

 private:
  Synthesis synthesis_;
  std::vector<float> pending_;
  std::int64_t number_ = 3;
};

class VorbisPacketEncoder final : public codec::AudioEncoder {
 public:
  Result<void> configure(const codec::AudioEncodeParams& params) override {
    VorbisSettings s;
    s.quality = static_cast<float>(std::clamp((params.quality - 20) / 100.0 + 0.0, -0.1, 1.0));
    if (params.bitrate_kbps) s.bitrate = *params.bitrate_kbps * 1000;
    Bytes id, setup;
    CVT_TRY(analysis_.open(params.channels, params.sample_rate, s, id, setup));
    ir::Audio none;
    const Bytes comment = comment_packet(none);
    // Xiph lacing of the three headers.
    util::ByteWriter w;
    w.u8(2);
    for (const std::size_t size : {id.size(), comment.size()}) {
      std::size_t left = size;
      while (left >= 255) {
        w.u8(255);
        left -= 255;
      }
      w.u8(static_cast<std::uint8_t>(left));
    }
    w.bytes(id);
    w.bytes(comment);
    w.bytes(setup);
    extradata_ = w.take();
    channels_ = params.channels;
    rate_ = params.sample_rate;
    timescale_ = params.timescale != 0 ? params.timescale : params.sample_rate;
    return {};
  }
  Result<void> send_chunk(const codec::AudioChunk& chunk) override {
    if (!chunk.samples.empty()) analysis_.write(chunk.samples.data(), chunk.samples.size() / channels_);
    take();
    return {};
  }
  Result<std::optional<codec::Packet>> receive_packet() override {
    if (next_ >= ready_.size()) {
      ready_.clear();
      next_ = 0;
      return std::optional<codec::Packet>{};
    }
    return std::optional<codec::Packet>(std::move(ready_[next_++]));
  }
  [[nodiscard]] Bytes extradata() const override { return extradata_; }
  Result<void> flush() override {
    analysis_.write(nullptr, 0);
    take();
    return {};
  }
  [[nodiscard]] std::uint32_t frame_size() const override { return 0; }
  [[nodiscard]] std::uint32_t delay() const override { return 0; }

 private:
  void take() {
    std::vector<std::pair<Bytes, std::int64_t>> packets;
    analysis_.collect(packets);
    for (auto& [bytes, granule] : packets) {
      codec::Packet p;
      p.bytes = std::move(bytes);
      const std::int64_t end = std::max<std::int64_t>(granule, last_);
      p.pts = p.dts = static_cast<std::int64_t>(static_cast<std::uint64_t>(last_) * timescale_ / rate_);
      p.duration = static_cast<std::int64_t>(static_cast<std::uint64_t>(end - last_) * timescale_ / rate_);
      p.keyframe = true;
      last_ = end;
      ready_.push_back(std::move(p));
    }
  }
  Analysis analysis_;
  Bytes extradata_;
  std::vector<codec::Packet> ready_;
  std::size_t next_ = 0;
  std::size_t channels_ = 2;
  std::uint32_t rate_ = 44100;
  std::uint32_t timescale_ = 44100;
  std::int64_t last_ = 0;
};

}  // namespace

std::unique_ptr<codec::AudioDecoder> make_vorbis_decoder() { return std::make_unique<VorbisPacketDecoder>(); }
std::unique_ptr<codec::AudioEncoder> make_vorbis_encoder() { return std::make_unique<VorbisPacketEncoder>(); }

}  // namespace cvt::lossy::xiph
