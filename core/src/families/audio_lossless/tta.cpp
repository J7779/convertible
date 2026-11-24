// Convertible core - audio_lossless family: True Audio (TTA1) decoder and
// encoder, and the .tta file format.
//
// A TTA1 file: 22-byte header (CRC-32), a seek table of frame sizes (CRC-32),
// then frames of about 1.045 s. Each sample of each channel goes through
// inter-channel decorrelation, a first-order fixed predictor, an 8-tap
// sign-LMS "hybrid" filter and an adaptive two-level Rice coder (LSB-first);
// every frame ends with a CRC-32. Integer PCM of 8, 16 and 24 bits.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/audio_lossless/audio_api.hpp"
#include "families/audio_lossless/common.hpp"
#include "families/audio_lossless/tags.hpp"
#include "util/crc.hpp"

namespace cvt::audio {

namespace {

constexpr std::uint32_t frame_length_for(std::uint32_t rate) { return static_cast<std::uint32_t>(std::uint64_t{rate} * 256 / 245); }

std::uint32_t shift_1(std::uint32_t k) { return k >= 32 ? 0 : 1u << k; }
std::uint32_t shift_16(std::uint32_t k) { return k + 4 >= 32 ? 0x80000000u : 1u << (k + 4); }

struct Filter {
  std::int32_t shift = 0;
  std::int32_t round = 0;
  std::int32_t error = 0;
  std::array<std::int32_t, 8> qm{}, dx{}, dl{};
};

struct Rice {
  std::uint32_t k0 = 10, k1 = 10, sum0 = 0, sum1 = 0;
};

struct Channel {
  Filter filter;
  Rice rice;
  std::int32_t predictor = 0;
};

std::int32_t wrap(std::int64_t v) { return static_cast<std::int32_t>(static_cast<std::uint32_t>(v)); }

void init_channel(Channel& c, std::uint32_t bytes) {
  static constexpr std::array<std::int32_t, 4> configs = {10, 9, 10, 12};
  c = Channel{};
  c.filter.shift = configs[std::min<std::uint32_t>(bytes, 4) - 1];
  c.filter.round = 1 << (c.filter.shift - 1);
  c.rice.sum0 = shift_16(10);
  c.rice.sum1 = shift_16(10);
}

// Shared part of the filter; `decode` selects the direction.
void filter_process(Filter& f, std::int32_t& value, bool decode) {
  if (f.error < 0) {
    for (std::size_t i = 0; i < 8; ++i) f.qm[i] = wrap(static_cast<std::int64_t>(f.qm[i]) - f.dx[i]);
  } else if (f.error > 0) {
    for (std::size_t i = 0; i < 8; ++i) f.qm[i] = wrap(static_cast<std::int64_t>(f.qm[i]) + f.dx[i]);
  }
  std::int32_t sum = f.round;
  for (std::size_t i = 0; i < 8; ++i) sum = wrap(static_cast<std::int64_t>(sum) + wrap(static_cast<std::int64_t>(f.dl[i]) * f.qm[i]));
  for (std::size_t i = 0; i < 4; ++i) {
    f.dx[i] = f.dx[i + 1];
    f.dl[i] = f.dl[i + 1];
  }
  f.dx[4] = (f.dl[4] >> 30) | 1;
  f.dx[5] = ((f.dl[5] >> 30) | 2) & ~1;
  f.dx[6] = ((f.dl[6] >> 30) | 2) & ~1;
  f.dx[7] = ((f.dl[7] >> 30) | 4) & ~3;
  if (decode) {
    f.error = value;
    value = wrap(static_cast<std::int64_t>(value) + (sum >> f.shift));
  }
  f.dl[4] = -f.dl[5];
  f.dl[5] = -f.dl[6];
  f.dl[6] = wrap(static_cast<std::int64_t>(value) - f.dl[7]);
  f.dl[7] = value;
  f.dl[5] = wrap(static_cast<std::int64_t>(f.dl[5]) + f.dl[6]);
  f.dl[4] = wrap(static_cast<std::int64_t>(f.dl[4]) + f.dl[5]);
  if (!decode) {
    value = wrap(static_cast<std::int64_t>(value) - (sum >> f.shift));
    f.error = value;
  }
}

std::int32_t predict(std::int32_t previous, std::uint32_t bytes) {
  const int k = bytes == 1 ? 4 : 5;
  return static_cast<std::int32_t>((static_cast<std::int64_t>(previous) * ((1 << k) - 1)) >> k);
}

void adapt(std::uint32_t& sum, std::uint32_t& k, std::uint32_t value) {
  sum += value - (sum >> 4);
  if (k > 0 && sum < shift_16(k)) {
    --k;
  } else if (sum > shift_16(k + 1)) {
    ++k;
  }
}

class LsbBits {
 public:
  explicit LsbBits(std::span<const std::uint8_t> data) : data_(data) {}
  std::uint32_t bit() {
    if (pos_ >= data_.size() * 8) {
      overrun_ = true;
      return 0;
    }
    const std::uint32_t b = (data_[static_cast<std::size_t>(pos_ >> 3)] >> (pos_ & 7)) & 1u;
    ++pos_;
    return b;
  }
  std::uint32_t bits(std::uint32_t n) {
    std::uint32_t v = 0;
    for (std::uint32_t i = 0; i < n; ++i) v |= bit() << i;
    return v;
  }
  [[nodiscard]] bool overrun() const { return overrun_; }
  [[nodiscard]] std::size_t byte_position() const { return static_cast<std::size_t>((pos_ + 7) / 8); }

 private:
  std::span<const std::uint8_t> data_;
  std::uint64_t pos_ = 0;
  bool overrun_ = false;
};

// =========================================================================== decoding

struct TtaHeader {
  std::uint16_t format = 0;
  std::uint16_t channels = 0;
  std::uint16_t bits = 0;
  std::uint32_t rate = 0;
  std::uint32_t length = 0;
};

Result<std::size_t> decode_tta_frame(std::span<const std::uint8_t> frame, const TtaHeader& h, std::uint32_t samples,
                                     std::vector<std::int32_t>& out) {
  const std::uint32_t bytes = (h.bits + 7u) / 8u;
  std::vector<Channel> channels(h.channels);
  for (Channel& c : channels) init_channel(c, bytes);
  LsbBits bits(frame);
  const std::size_t start = out.size();
  out.resize(start + std::size_t{samples} * h.channels);
  std::int32_t* p = out.data() + start;
  for (std::uint32_t i = 0; i < samples; ++i) {
    for (std::uint32_t ch = 0; ch < h.channels; ++ch, ++p) {
      Channel& c = channels[ch];
      std::uint32_t unary = 0;
      while (bits.bit() != 0) {
        if (++unary > (1u << 24) || bits.overrun()) return fail(Status::Decode, "damaged TTA frame");
      }
      std::uint32_t k;
      std::uint32_t depth;
      if (unary == 0) {
        depth = 0;
        k = c.rice.k0;
      } else {
        depth = 1;
        k = c.rice.k1;
        --unary;
      }
      if (k > 31) return fail(Status::Decode, "damaged TTA frame");
      std::uint32_t value = k != 0 ? (unary << k) + bits.bits(k) : unary;
      if (depth == 1) {
        adapt(c.rice.sum1, c.rice.k1, value);
        value += shift_1(c.rice.k0);
      }
      adapt(c.rice.sum0, c.rice.k0, value);
      std::int32_t v = static_cast<std::int32_t>(1 + ((value >> 1) ^ ((value & 1u) - 1u)));
      filter_process(c.filter, v, true);
      if (bytes <= 3) v = wrap(static_cast<std::int64_t>(v) + predict(c.predictor, bytes));
      else v = wrap(static_cast<std::int64_t>(v) + c.predictor);
      c.predictor = v;
      *p = v;
    }
    if (h.channels > 1) {
      std::int32_t* last = p - 1;
      std::int32_t* r = last - 1;
      *last = wrap(static_cast<std::int64_t>(*last) + *r / 2);
      for (; r >= p - h.channels; --r) *r = wrap(static_cast<std::int64_t>(*(r + 1)) - *r);
    }
    if (bits.overrun()) return fail(Status::Decode, "truncated TTA frame");
  }
  const std::size_t used = bits.byte_position();
  if (used + 4 > frame.size()) return fail(Status::Decode, "truncated TTA frame");
  util::ByteReader crc_reader(as_bytes(frame.subspan(used, 4)));
  if (util::crc32(as_bytes(frame.first(used))) != crc_reader.u32le()) return fail(Status::Decode, "TTA frame checksum mismatch");
  return used + 4;
}

std::size_t skip_id3(ByteView data) {
  util::ByteReader r(data);
  if (!r.consume("ID3") || r.remaining() < 7) return 0;
  r.skip(2);
  const std::uint8_t flags = r.u8();
  const std::uint32_t raw = r.u32be();
  const std::uint32_t size = ((raw >> 24) & 0x7F) << 21 | ((raw >> 16) & 0x7F) << 14 | ((raw >> 8) & 0x7F) << 7 | (raw & 0x7F);
  const std::size_t total = 10 + std::size_t{size} + ((flags & 0x10) != 0 ? 10 : 0);
  return total <= data.size() ? total : 0;
}

Result<AnyIR> read_tta(ReadContext& ctx) {
  const std::size_t start = skip_id3(ctx.input);
  tags::TagMap tag_map;
  if (start > 0) tags::parse_id3v2(ctx.input.first(start), tag_map);
  const std::size_t tail = tags::parse_apev2(ctx.input, tag_map);
  if (ctx.input.size() < start + 22 + tail) return fail(Status::Decode, "not a TTA file");
  const ByteView input = ctx.input.subspan(start, ctx.input.size() - start - tail);
  util::ByteReader r(input);
  if (!r.consume("TTA1")) return fail(Status::Decode, "not a TTA file");
  TtaHeader h;
  h.format = r.u16le();
  h.channels = r.u16le();
  h.bits = r.u16le();
  h.rate = r.u32le();
  h.length = r.u32le();
  const std::uint32_t crc = r.u32le();
  if (!r.ok()) return fail(Status::Decode, "the TTA header is truncated");
  if (util::crc32(input.first(18)) != crc) return fail(Status::Decode, "TTA header checksum mismatch");
  if (h.format == 2) return fail(Status::Decode, "password-protected TTA files are not supported");
  if (h.format != 1) return fail(Status::Decode, "unsupported TTA format {}", h.format);
  if (h.bits < 8 || h.bits > 24) return fail(Status::Decode, "unsupported TTA sample size {}", h.bits);
  CVT_TRY(check_stream_params(h.rate, h.channels, "the TTA file"));
  const std::uint32_t frame_length = frame_length_for(h.rate);
  if (frame_length == 0) return fail(Status::Decode, "invalid TTA sample rate");
  const std::uint64_t frames = (std::uint64_t{h.length} + frame_length - 1) / frame_length;
  CVT_TRY(check_sample_budget(ctx, std::uint64_t{h.length} * h.channels));
  if (frames * 4 + 4 > r.remaining()) return fail(Status::Decode, "the TTA seek table is truncated");
  std::vector<std::uint32_t> sizes(static_cast<std::size_t>(frames));
  const ByteView table = input.subspan(22, static_cast<std::size_t>(frames * 4));
  util::ByteReader t(table);
  for (std::uint32_t& s : sizes) s = t.u32le();
  r.skip(static_cast<std::size_t>(frames * 4));
  const std::uint32_t table_crc = r.u32le();
  const bool table_ok = util::crc32(table) == table_crc;
  if (!table_ok) ctx.warn("The TTA seek table is damaged; frames were located by decoding.");
  std::vector<std::int32_t> ints;
  ints.reserve(static_cast<std::size_t>(std::uint64_t{h.length} * h.channels));
  std::size_t pos = r.position();
  const std::span<const std::uint8_t> bytes = as_u8(input);
  bool damaged = false;
  for (std::uint64_t f = 0; f < frames; ++f) {
    if ((f & 7) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(f) / static_cast<double>(frames), "Decoding"));
    const auto samples = static_cast<std::uint32_t>(std::min<std::uint64_t>(frame_length, h.length - f * frame_length));
    std::size_t size = bytes.size() - pos;
    if (table_ok) size = std::min<std::size_t>(size, sizes[static_cast<std::size_t>(f)]);
    const std::size_t before = ints.size();
    auto used = decode_tta_frame(bytes.subspan(pos, size), h, samples, ints);
    if (!used) {
      ints.resize(before);
      if (!table_ok) return std::unexpected(used.error());
      ints.resize(before + std::size_t{samples} * h.channels, 0);  // keep timing: silence for a damaged frame
      damaged = true;
      pos += size;
      continue;
    }
    pos += table_ok ? size : *used;
    if (pos > bytes.size()) break;
  }
  if (damaged) ctx.warn("Damaged TTA frames were replaced by silence.");
  ir::Audio audio = make_audio(h.rate, h.channels, h.bits);
  audio.tags = std::move(tag_map);
  audio.samples.resize(ints.size());
  const double scale = 1.0 / static_cast<double>(std::uint64_t{1} << (h.bits - 1));
  for (std::size_t i = 0; i < ints.size(); ++i) audio.samples[i] = static_cast<float>(ints[i] * scale);
  return audio;
}

// =========================================================================== encoding

class LsbOut {
 public:
  void bits(std::uint32_t value, std::uint32_t count) {
    for (std::uint32_t i = 0; i < count; ++i) {
      if ((value >> i) & 1u) current_ |= static_cast<std::uint8_t>(1u << filled_);
      if (++filled_ == 8) {
        bytes_.push_back(current_);
        current_ = 0;
        filled_ = 0;
      }
    }
  }
  void ones(std::uint32_t count) {
    while (count >= 32) {
      bits(0xFFFFFFFFu, 32);
      count -= 32;
    }
    bits((count == 0 ? 0u : (0xFFFFFFFFu >> (32 - count))), count);
  }
  std::vector<std::uint8_t> finish() {
    if (filled_ > 0) bytes_.push_back(current_);
    current_ = 0;
    filled_ = 0;
    return std::move(bytes_);
  }

 private:
  std::vector<std::uint8_t> bytes_;
  std::uint8_t current_ = 0;
  std::uint32_t filled_ = 0;
};

Bytes encode_tta_frame(std::span<const std::int32_t> samples, std::uint32_t channels, std::uint32_t bits) {
  const std::uint32_t bytes = (bits + 7) / 8;
  std::vector<Channel> state(channels);
  for (Channel& c : state) init_channel(c, bytes);
  LsbOut out;
  const std::size_t frames = samples.size() / channels;
  for (std::size_t i = 0; i < frames; ++i) {
    const std::int32_t* s = samples.data() + i * channels;
    std::int32_t res = 0;
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
      Channel& c = state[ch];
      std::int32_t value = s[ch];
      if (channels > 1) {
        if (ch < channels - 1) {
          value = res = wrap(static_cast<std::int64_t>(s[ch + 1]) - s[ch]);
        } else {
          value = wrap(static_cast<std::int64_t>(value) - res / 2);
        }
      }
      const std::int32_t temp = value;
      value = wrap(static_cast<std::int64_t>(value) - (bytes <= 3 ? predict(c.predictor, bytes) : c.predictor));
      c.predictor = temp;
      filter_process(c.filter, value, false);
      std::uint32_t outval = value > 0 ? (static_cast<std::uint32_t>(value) << 1) - 1u : static_cast<std::uint32_t>(-static_cast<std::int64_t>(value)) << 1;
      std::uint32_t k = c.rice.k0;
      adapt(c.rice.sum0, c.rice.k0, outval);
      if (outval >= shift_1(k)) {
        outval -= shift_1(k);
        k = c.rice.k1;
        adapt(c.rice.sum1, c.rice.k1, outval);
        const std::uint32_t unary = 1 + (outval >> k);
        out.ones(unary);
        out.bits(0, 1);
        if (k != 0) out.bits(outval & (shift_1(k) - 1u), k);
      } else {
        out.bits(0, 1);
        if (k != 0) out.bits(outval, k);
      }
    }
  }
  std::vector<std::uint8_t> data = out.finish();
  const std::uint32_t crc = util::crc32(as_bytes(std::span<const std::uint8_t>(data)));
  Bytes frame = to_bytes(std::span<const std::uint8_t>(data));
  util::ByteWriter tail;
  tail.u32le(crc);
  append(frame, tail.view());
  return frame;
}

Result<Bytes> write_tta(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Audio audio, prepare_for_writing(value.get<ir::Audio>(), ctx, 1, 0, 0));
  const std::string choice = ctx.options.get_enum(key_sample_format, "auto", {"auto", "s8", "s16", "s24"});
  int bits;
  if (choice == "auto") {
    static constexpr std::array<int, 3> allowed = {8, 16, 24};
    bits = default_bit_depth(audio, allowed, audio.float_samples ? 24 : 16);
    if (audio.float_samples || audio.bits_per_sample > 24) ctx.warn("TTA stores up to 24-bit integers; the audio was stored as 24-bit.");
  } else {
    bits = choice == "s8" ? 8 : choice == "s16" ? 16 : 24;
  }
  const std::vector<std::int32_t> ints = quantize_for(audio, bits, ctx.options);
  const std::uint64_t total = audio.frame_count();
  if (total > 0xFFFFFFFFull) return fail(Status::Encode, "the audio is too long for TTA");
  const std::uint32_t frame_length = frame_length_for(audio.sample_rate);
  if (frame_length == 0) return fail(Status::Encode, "the sample rate is too low for TTA");
  std::vector<Bytes> frames;
  for (std::uint64_t first = 0; first < total; first += frame_length) {
    if ((frames.size() & 7) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(first) / static_cast<double>(total), "Encoding"));
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(frame_length, total - first));
    frames.push_back(encode_tta_frame(std::span<const std::int32_t>(ints).subspan(
                                          static_cast<std::size_t>(first * audio.channels), count * audio.channels),
                                      audio.channels, static_cast<std::uint32_t>(bits)));
  }
  util::ByteWriter header;
  header.text("TTA1");
  header.u16le(1);
  header.u16le(audio.channels);
  header.u16le(static_cast<std::uint16_t>(bits));
  header.u32le(audio.sample_rate);
  header.u32le(static_cast<std::uint32_t>(total));
  const std::uint32_t header_crc = util::crc32(header.view());
  header.u32le(header_crc);
  util::ByteWriter table;
  for (const Bytes& f : frames) table.u32le(static_cast<std::uint32_t>(f.size()));
  const std::uint32_t table_crc = util::crc32(table.view());
  util::ByteWriter out;
  out.bytes(header.view());
  out.bytes(table.view());
  out.u32le(table_crc);
  for (const Bytes& f : frames) {
    out.bytes(f);
    CVT_TRY(ctx.limits.check_output(out.size()));
  }
  if (!options::resolved_strip_metadata(ctx.options)) out.bytes(tags::apev2_tag(audio.tags));
  return out.take();
}

}  // namespace

void register_tta(Registry& registry) {
  FormatInfo info;
  info.id = "tta";
  info.name = "True Audio";
  info.description = "TTA lossless audio.";
  info.category = Category::Audio;
  info.extensions = {"tta"};
  info.mime = "audio/x-tta";
  info.popularity = 10;
  info.signatures = {Signature::at(0, "'TTA1'")};
  registry.add_format(std::move(info));

  Codec codec;
  codec.format_id = "tta";
  codec.kind = Kind::Audio;
  codec.read = read_tta;
  codec.write = write_tta;
  codec.write_options = standard_write_options();
  codec.write_options.insert(codec.write_options.begin(), sample_format_option({"auto", "s8", "s16", "s24"}));
  codec.write_options.push_back(dither_option());
  registry.add_codec(std::move(codec));
}

}  // namespace cvt::audio
