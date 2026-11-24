// Convertible core - audio_lossless family: Apple Lossless (ALAC) codec.
//
// Bitstream: per packet a sequence of elements (SCE mono, CPE stereo pair,
// LFE, plus skippable DSE/FIL), each either "escaped" (verbatim samples) or
// compressed: optional raw low bytes ("shift"), stereo mid/side-like
// mixing with a mix weight, a sign-LMS adaptive FIR predictor whose
// starting coefficients are transmitted, and an adaptive Golomb-Rice coder
// with run-length coding of zeros. Decoder and encoder below implement the
// published format; the decoder mirrors the reference arithmetic exactly
// (32-bit wrap-around included) so that every conforming stream decodes
// bit-exactly.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/audio_lossless/alac.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>
#include <limits>

#include "families/audio_lossless/bits.hpp"

namespace cvt::audio::alac {

namespace {

constexpr std::uint32_t id_sce = 0, id_cpe = 1, id_cce = 2, id_lfe = 3, id_dse = 4, id_pce = 5, id_fil = 6, id_end = 7;

constexpr std::uint32_t qbshift = 9;
constexpr std::uint32_t qb = 1u << qbshift;
constexpr std::uint32_t mmulshift = 2;
constexpr std::uint32_t mdenshift = qbshift - mmulshift - 1;
constexpr std::uint32_t moff = 1u << (mdenshift - 2);
constexpr std::uint32_t bitoff = 24;
constexpr std::uint32_t max_prefix_16 = 9;
constexpr std::uint32_t max_prefix_32 = 9;
constexpr std::uint32_t max_datatype_bits_16 = 16;
constexpr std::uint32_t n_max_mean_clamp = 0xFFFF;
constexpr std::uint32_t n_mean_clamp_val = 0xFFFF;
constexpr std::uint32_t denshift_default = 9;

std::uint32_t lead(std::uint32_t x) noexcept { return static_cast<std::uint32_t>(std::countl_zero(x)); }
std::uint32_t lg3a(std::uint32_t x) noexcept { return 31 - lead(x + 3); }

std::int32_t sign_of(std::int32_t i) noexcept { return (i > 0) - (i < 0); }

// Sign-extend the low `bits` bits (two's complement wrap like the reference).
std::int32_t extend(std::int32_t value, std::uint32_t bits) noexcept {
  const std::uint32_t shift = 32 - bits;
  return static_cast<std::int32_t>(static_cast<std::uint32_t>(value) << shift) >> shift;
}
std::int32_t wrap_add(std::int32_t a, std::int32_t b) noexcept {
  return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}
std::int32_t wrap_sub(std::int32_t a, std::int32_t b) noexcept {
  return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b));
}
std::int32_t wrap_mul(std::int32_t a, std::int32_t b) noexcept {
  return static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::int64_t>(a) * b));
}

struct AgParams {
  std::uint32_t mb0, pb, kb, wb;
};

AgParams ag_params(std::uint32_t mb, std::uint32_t pb, std::uint32_t kb) {
  return {mb, pb, kb, (1u << kb) - 1u};
}

// ---- adaptive Golomb decoding

std::uint32_t dyn_get(BitReader& br, std::uint32_t m, std::uint32_t k) {
  const std::uint32_t stream = br.peek(32);
  const std::uint32_t pre = lead(~stream);
  if (pre >= max_prefix_16) {
    br.skip(max_prefix_16);
    return br.read(max_datatype_bits_16);
  }
  br.skip(pre + 1);
  const std::uint32_t v = br.read(k);
  std::uint32_t result = pre * m + v - 1;
  if (v < 2) {
    result -= (v - 1);
    br.set_position(br.position() - 1);
  }
  return result;
}

std::uint32_t dyn_get_32(BitReader& br, std::uint32_t m, std::uint32_t k, std::uint32_t max_bits) {
  const std::uint32_t stream = br.peek(32);
  std::uint32_t result = lead(~stream);
  if (result >= max_prefix_32) {
    br.skip(max_prefix_32);
    return br.read(max_bits);
  }
  if (k != 1) {
    br.skip(result + 1);
    const std::uint32_t v = br.read(k);
    result = result * m;
    if (v >= 2) {
      result += v - 1;
    } else {
      br.set_position(br.position() - 1);
    }
  } else {
    br.skip(result + 1);
  }
  return result;
}

Result<void> dyn_decomp(const AgParams& params, BitReader& br, std::int32_t* out, std::uint32_t samples,
                        std::uint32_t max_size) {
  std::uint32_t mb = params.mb0;
  const std::uint32_t pb = params.pb;
  const std::uint32_t kb = params.kb;
  const std::uint32_t wb = params.wb;
  std::uint32_t zmode = 0;
  std::uint32_t c = 0;
  while (c < samples) {
    std::uint32_t m = mb >> qbshift;
    std::uint32_t k = std::min(lg3a(m), kb);
    m = (1u << k) - 1u;
    const std::uint32_t n = dyn_get_32(br, m, k, max_size);
    const std::uint32_t ndecode = n + zmode;
    const std::int32_t multiplier = -static_cast<std::int32_t>(ndecode & 1u) | 1;
    out[c++] = static_cast<std::int32_t>((ndecode + 1u) >> 1) * multiplier;
    mb = pb * (n + zmode) + mb - ((pb * mb) >> qbshift);
    if (n > n_max_mean_clamp) mb = n_mean_clamp_val;
    zmode = 0;
    if (((mb << mmulshift) < qb) && (c < samples)) {
      zmode = 1;
      k = lead(mb) - bitoff + ((mb + moff) >> mdenshift);
      if (k == 0 || k > 31) return fail(Status::Decode, "ALAC run parameter out of range");
      const std::uint32_t mz = ((1u << k) - 1u) & wb;
      if (mz == 0) return fail(Status::Decode, "ALAC run parameter out of range");
      const std::uint32_t run = dyn_get(br, mz, k);
      if (run > samples - c) return fail(Status::Decode, "ALAC run length overflows the frame");
      for (std::uint32_t j = 0; j < run; ++j) out[c++] = 0;
      if (run >= 65535) zmode = 0;
      mb = 0;
    }
    if (br.overrun()) return fail(Status::Decode, "ALAC packet is truncated");
  }
  return {};
}

// ---- adaptive Golomb encoding (Sink: BitWriter or a bit counter)

struct CountSink {
  std::uint64_t bits = 0;
  void write(std::uint32_t, unsigned count) { bits += count; }
};

template <class Sink>
void dyn_code_16(Sink& sink, std::uint32_t m, std::uint32_t k, std::uint32_t n) {
  const std::uint32_t division = n / m;
  if (division < max_prefix_16) {
    const std::uint32_t modulo = n - m * division;
    const std::uint32_t de = modulo == 0 ? 1 : 0;
    const std::uint32_t num_bits = division + k + 1 - de;
    if (num_bits <= 25) {
      const std::uint32_t value = (((1u << division) - 1u) << (num_bits - division)) + modulo + 1 - de;
      sink.write(value, num_bits);
      return;
    }
  }
  sink.write(((1u << max_prefix_16) - 1u), max_prefix_16);
  sink.write(n, max_datatype_bits_16);
}

template <class Sink>
void dyn_code_32(Sink& sink, std::uint32_t m, std::uint32_t k, std::uint32_t n, std::uint32_t max_bits) {
  const std::uint32_t division = n / m;
  if (division < max_prefix_32) {
    const std::uint32_t modulo = n - m * division;
    const std::uint32_t de = modulo == 0 ? 1 : 0;
    const std::uint32_t num_bits = division + k + 1 - de;
    if (num_bits <= 25) {
      const std::uint32_t value = (((1u << division) - 1u) << (num_bits - division)) + modulo + 1 - de;
      sink.write(value, num_bits);
      return;
    }
  }
  sink.write((1u << max_prefix_32) - 1u, max_prefix_32);
  sink.write(n, max_bits);
}

template <class Sink>
void dyn_comp(const AgParams& params, const std::int32_t* in, std::uint32_t samples, std::uint32_t bit_size,
              Sink& sink) {
  std::uint32_t mb = params.mb0;
  const std::uint32_t pb = params.pb;
  const std::uint32_t kb = params.kb;
  const std::uint32_t wb = params.wb;
  std::uint32_t zmode = 0;
  std::uint32_t c = 0;
  while (c < samples) {
    std::uint32_t m = mb >> qbshift;
    std::uint32_t k = std::min(lg3a(m), kb);
    m = (1u << k) - 1u;
    const std::int32_t del = in[c++];
    const std::uint32_t magnitude = del < 0 ? 0u - static_cast<std::uint32_t>(del) : static_cast<std::uint32_t>(del);
    const std::uint32_t n = (magnitude << 1) - (del < 0 ? 1u : 0u) - zmode;
    dyn_code_32(sink, m, k, n, bit_size);
    mb = pb * (n + zmode) + mb - ((pb * mb) >> qbshift);
    if (n > n_max_mean_clamp) mb = n_mean_clamp_val;
    zmode = 0;
    if (((mb << mmulshift) < qb) && (c < samples)) {
      zmode = 1;
      std::uint32_t run = 0;
      while (c < samples && in[c] == 0) {
        ++c;
        ++run;
        if (run >= 65535) {
          zmode = 0;
          break;
        }
      }
      k = lead(mb) - bitoff + ((mb + moff) >> mdenshift);
      const std::uint32_t mz = ((1u << k) - 1u) & wb;
      dyn_code_16(sink, mz, k, run);
      mb = 0;
    }
  }
}

// ---- adaptive predictor

void unpc_block(const std::int32_t* pc, std::int32_t* out, std::uint32_t num, std::int16_t* coefs,
                std::uint32_t active, std::uint32_t chanbits, std::uint32_t denshift) {
  if (num == 0) return;
  out[0] = pc[0];
  if (active == 0) {
    for (std::uint32_t j = 1; j < num; ++j) out[j] = pc[j];
    return;
  }
  if (active == 31) {
    std::int32_t prev = out[0];
    for (std::uint32_t j = 1; j < num; ++j) {
      prev = extend(wrap_add(pc[j], prev), chanbits);
      out[j] = prev;
    }
    return;
  }
  const std::uint32_t first = std::min(active, num - 1);
  for (std::uint32_t j = 1; j <= first; ++j) out[j] = extend(wrap_add(pc[j], out[j - 1]), chanbits);
  const std::uint32_t lim = active + 1;
  const std::int32_t denhalf = denshift == 0 ? 0 : 1 << (denshift - 1);
  for (std::uint32_t j = lim; j < num; ++j) {
    const std::int32_t* pout = out + j - 1;
    const std::int32_t top = out[j - lim];
    std::int32_t sum = 0;
    for (std::uint32_t k = 0; k < active; ++k) sum = wrap_add(sum, wrap_mul(coefs[k], wrap_sub(pout[-static_cast<std::ptrdiff_t>(k)], top)));
    std::int32_t del = pc[j];
    std::int32_t del0 = del;
    const std::int32_t sg = sign_of(del);
    del = wrap_add(del, wrap_add(top, wrap_add(sum, denhalf) >> denshift));
    out[j] = extend(del, chanbits);
    if (sg > 0) {
      for (std::int32_t k = static_cast<std::int32_t>(active) - 1; k >= 0; --k) {
        const std::int32_t dd = wrap_sub(top, pout[-k]);
        const std::int32_t sgn = sign_of(dd);
        coefs[k] = static_cast<std::int16_t>(coefs[k] - sgn);
        del0 = wrap_sub(del0, wrap_mul(static_cast<std::int32_t>(active) - k, wrap_mul(sgn, dd) >> denshift));
        if (del0 <= 0) break;
      }
    } else if (sg < 0) {
      for (std::int32_t k = static_cast<std::int32_t>(active) - 1; k >= 0; --k) {
        const std::int32_t dd = wrap_sub(top, pout[-k]);
        const std::int32_t sgn = sign_of(dd);
        coefs[k] = static_cast<std::int16_t>(coefs[k] + sgn);
        del0 = wrap_sub(del0, wrap_mul(static_cast<std::int32_t>(active) - k, wrap_mul(-sgn, dd) >> denshift));
        if (del0 >= 0) break;
      }
    }
  }
}

void pc_block(const std::int32_t* in, std::int32_t* pc, std::uint32_t num, std::int16_t* coefs, std::uint32_t active,
              std::uint32_t chanbits, std::uint32_t denshift) {
  if (num == 0) return;
  pc[0] = in[0];
  if (active == 0) {
    for (std::uint32_t j = 1; j < num; ++j) pc[j] = in[j];
    return;
  }
  const std::uint32_t first = std::min(active, num - 1);
  for (std::uint32_t j = 1; j <= first; ++j) pc[j] = extend(wrap_sub(in[j], in[j - 1]), chanbits);
  const std::uint32_t lim = active + 1;
  const std::int32_t denhalf = 1 << (denshift - 1);
  for (std::uint32_t j = lim; j < num; ++j) {
    const std::int32_t* pout = in + j - 1;
    const std::int32_t top = in[j - lim];
    std::int32_t sum = 0;
    for (std::uint32_t k = 0; k < active; ++k) sum = wrap_add(sum, wrap_mul(coefs[k], wrap_sub(pout[-static_cast<std::ptrdiff_t>(k)], top)));
    std::int32_t del = wrap_sub(wrap_sub(in[j], top), wrap_add(sum, denhalf) >> denshift);
    del = extend(del, chanbits);
    pc[j] = del;
    std::int32_t del0 = del;
    const std::int32_t sg = sign_of(del);
    if (sg > 0) {
      for (std::int32_t k = static_cast<std::int32_t>(active) - 1; k >= 0; --k) {
        const std::int32_t dd = wrap_sub(top, pout[-k]);
        const std::int32_t sgn = sign_of(dd);
        coefs[k] = static_cast<std::int16_t>(coefs[k] - sgn);
        del0 = wrap_sub(del0, wrap_mul(static_cast<std::int32_t>(active) - k, wrap_mul(sgn, dd) >> denshift));
        if (del0 <= 0) break;
      }
    } else if (sg < 0) {
      for (std::int32_t k = static_cast<std::int32_t>(active) - 1; k >= 0; --k) {
        const std::int32_t dd = wrap_sub(top, pout[-k]);
        const std::int32_t sgn = sign_of(dd);
        coefs[k] = static_cast<std::int16_t>(coefs[k] + sgn);
        del0 = wrap_sub(del0, wrap_mul(static_cast<std::int32_t>(active) - k, wrap_mul(-sgn, dd) >> denshift));
        if (del0 >= 0) break;
      }
    }
  }
}

void init_coefs(std::int16_t* coefs, std::uint32_t order) {
  const std::int32_t den = 1 << denshift_default;
  for (std::uint32_t k = 0; k < 32; ++k) coefs[k] = 0;
  if (order >= 1) coefs[0] = static_cast<std::int16_t>((38 * den) >> 4);
  if (order >= 2) coefs[1] = static_cast<std::int16_t>((-29 * den) >> 4);
  if (order >= 3) coefs[2] = static_cast<std::int16_t>((-2 * den) >> 4);
}

// Element layout per channel count: true = stereo pair.
std::vector<bool> elements_for(std::uint8_t channels) {
  switch (channels) {
    case 1: return {false};
    case 2: return {true};
    case 3: return {false, true};
    case 4: return {false, true, false};
    case 5: return {false, true, true};
    case 6: return {false, true, true, false};
    case 7: return {false, true, true, false, false};
    case 8: return {false, true, true, true, false};
    default: return {};
  }
}

}  // namespace

// =========================================================================== config

Result<Config> parse_config(ByteView cookie) {
  // Unwrap 'frma' and 'alac' atoms.
  for (int pass = 0; pass < 2 && cookie.size() >= 12; ++pass) {
    const std::string_view type = as_chars(cookie.subspan(4, 4));
    if (type == "frma") {
      cookie = cookie.subspan(12);
    } else if (type == "alac") {
      cookie = cookie.subspan(12);
    }
  }
  util::ByteReader r(cookie);
  Config c;
  c.frame_length = r.u32be();
  c.compatible_version = r.u8();
  c.bit_depth = r.u8();
  c.pb = r.u8();
  c.mb = r.u8();
  c.kb = r.u8();
  c.channels = r.u8();
  c.max_run = r.u16be();
  c.max_frame_bytes = r.u32be();
  c.avg_bit_rate = r.u32be();
  c.sample_rate = r.u32be();
  if (!r.ok()) return fail(Status::Decode, "the ALAC configuration is truncated");
  if (c.compatible_version > 0) return fail(Status::Decode, "unsupported ALAC version {}", c.compatible_version);
  if (c.frame_length == 0 || c.frame_length > 1u << 20) return fail(Status::Decode, "invalid ALAC frame length {}", c.frame_length);
  if (c.bit_depth != 16 && c.bit_depth != 20 && c.bit_depth != 24 && c.bit_depth != 32) {
    return fail(Status::Decode, "unsupported ALAC bit depth {}", c.bit_depth);
  }
  if (c.channels == 0 || c.channels > 8) return fail(Status::Decode, "unsupported ALAC channel count {}", c.channels);
  if (c.kb == 0 || c.kb > 31) return fail(Status::Decode, "invalid ALAC Rice limit");
  return c;
}

Bytes serialize_config(const Config& c) {
  util::ByteWriter w;
  w.u32be(c.frame_length);
  w.u8(c.compatible_version);
  w.u8(c.bit_depth);
  w.u8(c.pb);
  w.u8(c.mb);
  w.u8(c.kb);
  w.u8(c.channels);
  w.u16be(c.max_run);
  w.u32be(c.max_frame_bytes);
  w.u32be(c.avg_bit_rate);
  w.u32be(c.sample_rate);
  return w.take();
}

std::array<std::uint8_t, 8> channel_order(std::uint8_t channels) noexcept {
  switch (channels) {
    case 3: return {2, 0, 1, 3, 4, 5, 6, 7};  // C L R -> slots of L R C
    case 4: return {2, 0, 1, 3, 4, 5, 6, 7};
    case 5: return {2, 0, 1, 3, 4, 5, 6, 7};
    case 6: return {2, 0, 1, 4, 5, 3, 6, 7};  // C L R Ls Rs LFE -> L R C LFE Ls Rs
    case 7: return {2, 0, 1, 5, 6, 4, 3, 7};  // C L R Ls Rs Cs LFE -> L R C LFE Cs Ls Rs
    case 8: return {2, 6, 7, 0, 1, 4, 5, 3};  // C Lc Rc L R Ls Rs LFE -> L R C LFE Ls Rs Lc Rc
    default: return {0, 1, 2, 3, 4, 5, 6, 7};
  }
}

std::uint32_t channel_mask(std::uint8_t channels) noexcept {
  switch (channels) {
    case 1: return 0x4;
    case 2: return 0x3;
    case 3: return 0x7;
    case 4: return 0x107;  // L R C Cs
    case 5: return 0x37;
    case 6: return 0x3F;
    case 7: return 0x70F;
    case 8: return 0xFF;
    default: return 0;
  }
}

// =========================================================================== decoder

Result<void> Decoder::configure(const Config& config) {
  config_ = config;
  if (elements_for(config.channels).empty()) return fail(Status::Decode, "unsupported ALAC channel count");
  predictor_.assign(config.frame_length, 0);
  mix_u_.assign(config.frame_length, 0);
  mix_v_.assign(config.frame_length, 0);
  shift_.assign(std::size_t{config.frame_length} * 2, 0);
  return {};
}

Result<std::uint32_t> Decoder::decode(ByteView packet, std::vector<std::int32_t>& out) {
  BitReader br(packet);
  const std::uint32_t channels = config_.channels;
  const std::array<std::uint8_t, 8> order = channel_order(config_.channels);
  std::vector<std::int32_t> frame;  // interleaved, WAVE order
  std::uint32_t channel = 0;
  std::uint32_t frames = 0;
  bool first_element = true;
  for (int guard = 0; guard < 64; ++guard) {
    const std::uint32_t tag = br.read(3);
    if (br.overrun()) return fail(Status::Decode, "ALAC packet is truncated");
    if (tag == id_end) break;
    if (tag == id_dse) {
      br.read(4);
      const bool align = br.bit();
      std::uint32_t count = br.read(8);
      if (count == 255) count += br.read(8);
      if (align) br.align();
      br.skip(std::uint64_t{count} * 8);
      continue;
    }
    if (tag == id_fil) {
      std::uint32_t count = br.read(4);
      if (count == 15) count += br.read(8) - 1;
      br.skip(std::uint64_t{count} * 8);
      continue;
    }
    if (tag == id_cce || tag == id_pce || (tag != id_sce && tag != id_lfe && tag != id_cpe)) {
      return fail(Status::Decode, "unsupported ALAC element {}", tag);
    }
    const bool stereo = tag == id_cpe;
    br.read(4);  // element instance
    if (channel + (stereo ? 2u : 1u) > channels) return fail(Status::Decode, "ALAC packet has too many channels");
    const std::uint32_t unused = br.read(12);
    if (unused != 0) return fail(Status::Decode, "invalid ALAC element header");
    const std::uint32_t header = br.read(4);
    const bool partial = (header >> 3) != 0;
    const std::uint32_t bytes_shifted = (header >> 1) & 3u;
    const bool escape = (header & 1u) != 0;
    if (bytes_shifted == 3) return fail(Status::Decode, "invalid ALAC shift");
    std::uint32_t samples = config_.frame_length;
    if (partial) samples = br.read(32);
    if (samples == 0 || samples > config_.frame_length) return fail(Status::Decode, "invalid ALAC frame size {}", samples);
    if (!first_element && samples != frames) return fail(Status::Decode, "ALAC elements disagree on the frame size");
    first_element = false;
    frames = samples;
    if (frame.empty()) frame.assign(std::size_t{samples} * channels, 0);
    const std::uint32_t shift = bytes_shifted * 8;
    const std::uint32_t chan_bits = config_.bit_depth - shift + (stereo ? 1u : 0u);
    if (!escape && (chan_bits > 32 || chan_bits < 1)) return fail(Status::Decode, "invalid ALAC sample size");
    std::uint32_t mix_bits = 0;
    std::int32_t mix_res = 0;
    if (!escape) {
      mix_bits = br.read(8);
      mix_res = static_cast<std::int8_t>(br.read(8));
      struct Params {
        std::uint32_t mode, den_shift, pb_factor, num;
        std::array<std::int16_t, 32> coefs;
      };
      std::array<Params, 2> params{};
      for (std::uint32_t ch = 0; ch < (stereo ? 2u : 1u); ++ch) {
        Params& p = params[ch];
        std::uint32_t h = br.read(8);
        p.mode = h >> 4;
        p.den_shift = h & 0x0F;
        h = br.read(8);
        p.pb_factor = h >> 5;
        p.num = h & 0x1F;
        for (std::uint32_t i = 0; i < p.num; ++i) p.coefs[i] = static_cast<std::int16_t>(br.read(16));
      }
      BitReader shift_reader;
      if (bytes_shifted != 0) {
        shift_reader = br;
        br.skip(std::uint64_t{shift} * samples * (stereo ? 2u : 1u));
      }
      for (std::uint32_t ch = 0; ch < (stereo ? 2u : 1u); ++ch) {
        Params& p = params[ch];
        std::int32_t* target = ch == 0 ? mix_u_.data() : mix_v_.data();
        const AgParams ag = ag_params(config_.mb, (config_.pb * p.pb_factor) / 4, config_.kb);
        CVT_TRY(dyn_decomp(ag, br, predictor_.data(), samples, chan_bits));
        if (p.mode == 0) {
          unpc_block(predictor_.data(), target, samples, p.coefs.data(), p.num, chan_bits, p.den_shift);
        } else {
          // Mode 15: an extra first-order stage before the adaptive filter.
          unpc_block(predictor_.data(), predictor_.data(), samples, nullptr, 31, chan_bits, 0);
          unpc_block(predictor_.data(), target, samples, p.coefs.data(), p.num, chan_bits, p.den_shift);
        }
      }
      if (bytes_shifted != 0) {
        for (std::uint32_t i = 0; i < samples * (stereo ? 2u : 1u); ++i) {
          shift_[i] = static_cast<std::int32_t>(shift_reader.read(shift));
        }
      }
    } else {
      for (std::uint32_t i = 0; i < samples; ++i) {
        mix_u_[i] = br.read_signed(config_.bit_depth);
        if (stereo) mix_v_[i] = br.read_signed(config_.bit_depth);
      }
    }
    if (br.overrun()) return fail(Status::Decode, "ALAC packet is truncated");
    const std::uint32_t slot0 = order[channel];
    const std::uint32_t slot1 = stereo ? order[channel + 1] : 0;
    for (std::uint32_t i = 0; i < samples; ++i) {
      if (stereo) {
        std::int32_t l, r;
        if (mix_res != 0) {
          l = wrap_sub(wrap_add(mix_u_[i], mix_v_[i]), wrap_mul(mix_res, mix_v_[i]) >> mix_bits);
          r = wrap_sub(l, mix_v_[i]);
        } else {
          l = mix_u_[i];
          r = mix_v_[i];
        }
        if (bytes_shifted != 0) {
          l = static_cast<std::int32_t>((static_cast<std::uint32_t>(l) << shift) | static_cast<std::uint32_t>(shift_[i * 2]));
          r = static_cast<std::int32_t>((static_cast<std::uint32_t>(r) << shift) |
                                        static_cast<std::uint32_t>(shift_[i * 2 + 1]));
        }
        frame[std::size_t{i} * channels + slot0] = l;
        frame[std::size_t{i} * channels + slot1] = r;
      } else {
        std::int32_t v = mix_u_[i];
        if (bytes_shifted != 0) {
          v = static_cast<std::int32_t>((static_cast<std::uint32_t>(v) << shift) | static_cast<std::uint32_t>(shift_[i]));
        }
        frame[std::size_t{i} * channels + slot0] = v;
      }
    }
    channel += stereo ? 2 : 1;
  }
  if (channel != channels) return fail(Status::Decode, "ALAC packet has {} of {} channels", channel, channels);
  // Values must lie in the declared bit depth.
  const std::int32_t depth = config_.bit_depth;
  if (depth < 32) {
    for (std::int32_t& v : frame) v = extend(v, static_cast<std::uint32_t>(depth));
  }
  out.insert(out.end(), frame.begin(), frame.end());
  return frames;
}

// =========================================================================== encoder

Result<void> Encoder::configure(const Config& config, int effort) {
  if (elements_for(config.channels).empty()) return fail(Status::InvalidArgument, "ALAC holds 1 to 8 channels");
  if (config.bit_depth != 16 && config.bit_depth != 20 && config.bit_depth != 24 && config.bit_depth != 32) {
    return fail(Status::InvalidArgument, "ALAC bit depth must be 16, 20, 24 or 32");
  }
  config_ = config;
  effort_ = std::clamp(effort, 0, 9);
  for (auto& c : coefs_) init_coefs(c.data(), 8);
  return {};
}

namespace {

struct ChannelChoice {
  std::uint32_t order = 8;
  std::array<std::int16_t, 32> coefs{};  // starting coefficients to transmit
  std::uint64_t bits = std::numeric_limits<std::uint64_t>::max();
};

ChannelChoice choose_predictor(const std::int32_t* data, std::uint32_t samples, std::uint32_t chan_bits,
                               const Config& config, const std::array<std::int16_t, 32>& carried,
                               std::span<const std::uint32_t> orders, std::vector<std::int32_t>& scratch) {
  ChannelChoice best;
  const AgParams ag = ag_params(config.mb, config.pb, config.kb);
  for (const std::uint32_t order : orders) {
    for (int start = 0; start < 2; ++start) {
      std::array<std::int16_t, 32> coefs{};
      if (start == 0) {
        init_coefs(coefs.data(), order);
      } else {
        coefs = carried;
        for (std::uint32_t k = order; k < 32; ++k) coefs[k] = 0;
      }
      std::array<std::int16_t, 32> working = coefs;
      pc_block(data, scratch.data(), samples, working.data(), order, chan_bits, denshift_default);
      CountSink counter;
      dyn_comp(ag, scratch.data(), samples, chan_bits, counter);
      const std::uint64_t bits = counter.bits + 16 + 16ull * order;
      if (bits < best.bits) {
        best.bits = bits;
        best.order = order;
        best.coefs = coefs;
      }
    }
  }
  return best;
}

}  // namespace

Bytes Encoder::encode(std::span<const std::int32_t> interleaved, std::uint32_t frames) {
  const std::uint32_t channels = config_.channels;
  frames = std::min<std::uint32_t>(frames, config_.frame_length);
  frames = std::min<std::uint32_t>(frames, static_cast<std::uint32_t>(interleaved.size() / channels));
  const bool partial = frames < config_.frame_length;
  const std::array<std::uint8_t, 8> order = channel_order(config_.channels);
  const std::uint32_t depth = config_.bit_depth;
  const std::uint32_t bytes_shifted = depth == 32 ? 2 : depth == 24 ? 1 : 0;
  const std::uint32_t shift = bytes_shifted * 8;
  const std::int32_t shift_mask = static_cast<std::int32_t>((1u << shift) - 1u);
  const AgParams ag = ag_params(config_.mb, config_.pb, config_.kb);

  std::vector<std::uint32_t> orders = {8};
  if (effort_ >= 3) orders = {4, 8};
  if (effort_ >= 7) orders = {4, 8, 12, 16};
  std::vector<std::int32_t> mix_candidates = {2};
  if (effort_ >= 2) mix_candidates = {0, 2, 4};
  if (effort_ >= 5) mix_candidates = {0, 1, 2, 3, 4};
  constexpr std::uint32_t mix_bits = 2;

  BitWriter out;
  std::vector<std::int32_t> u(frames), v(frames), scratch(frames), shifts(std::size_t{frames} * 2);
  std::uint32_t channel = 0;
  std::uint32_t sce_tag = 0, cpe_tag = 0;
  for (const bool stereo : elements_for(config_.channels)) {
    const std::uint32_t c0 = channel;
    const std::uint32_t chan_bits = depth - shift + (stereo ? 1u : 0u);
    const auto sample = [&](std::uint32_t i, std::uint32_t c) {
      return interleaved[std::size_t{i} * channels + order[c]];
    };
    // Compressed candidate.
    BitWriter element;
    element.write(stereo ? id_cpe : id_sce, 3);
    element.write(stereo ? cpe_tag++ : sce_tag++, 4);
    element.write(0, 12);
    element.write((partial ? 8u : 0u) | (bytes_shifted << 1), 4);
    if (partial) element.write(frames, 32);
    std::int32_t best_mix = 0;
    if (stereo) {
      std::uint64_t best_bits = std::numeric_limits<std::uint64_t>::max();
      std::vector<std::int32_t> tu(frames), tv(frames);
      for (const std::int32_t mix_res : mix_candidates) {
        for (std::uint32_t i = 0; i < frames; ++i) {
          const std::int32_t l = sample(i, c0) >> shift;
          const std::int32_t r = sample(i, c0 + 1) >> shift;
          if (mix_res != 0) {
            const std::int32_t m2 = (1 << mix_bits) - mix_res;
            tu[i] = static_cast<std::int32_t>((static_cast<std::int64_t>(mix_res) * l + static_cast<std::int64_t>(m2) * r) >> mix_bits);
            tv[i] = l - r;
          } else {
            tu[i] = l;
            tv[i] = r;
          }
        }
        std::uint64_t total = 0;
        for (const auto* data : {&tu, &tv}) {
          std::array<std::int16_t, 32> coefs{};
          init_coefs(coefs.data(), 8);
          pc_block(data->data(), scratch.data(), frames, coefs.data(), 8, chan_bits, denshift_default);
          CountSink counter;
          dyn_comp(ag, scratch.data(), frames, chan_bits, counter);
          total += counter.bits;
        }
        if (total < best_bits) {
          best_bits = total;
          best_mix = mix_res;
        }
      }
    }
    for (std::uint32_t i = 0; i < frames; ++i) {
      const std::int32_t l = sample(i, c0);
      if (stereo) {
        const std::int32_t r = sample(i, c0 + 1);
        shifts[std::size_t{i} * 2] = l & shift_mask;
        shifts[std::size_t{i} * 2 + 1] = r & shift_mask;
        const std::int32_t ls = l >> shift, rs = r >> shift;
        if (best_mix != 0) {
          const std::int32_t m2 = (1 << mix_bits) - best_mix;
          u[i] = static_cast<std::int32_t>((static_cast<std::int64_t>(best_mix) * ls + static_cast<std::int64_t>(m2) * rs) >> mix_bits);
          v[i] = ls - rs;
        } else {
          u[i] = ls;
          v[i] = rs;
        }
      } else {
        shifts[i] = l & shift_mask;
        u[i] = l >> shift;
      }
    }
    element.write(stereo ? mix_bits : 0, 8);
    element.write(static_cast<std::uint32_t>(static_cast<std::uint8_t>(stereo ? best_mix : 0)), 8);
    std::array<ChannelChoice, 2> choices{};
    for (std::uint32_t ch = 0; ch < (stereo ? 2u : 1u); ++ch) {
      std::array<std::int16_t, 32>& carried = coefs_[std::min<std::size_t>(c0 + ch, coefs_.size() - 1)];
      choices[ch] = choose_predictor(ch == 0 ? u.data() : v.data(), frames, chan_bits, config_, carried, orders, scratch);
      element.write((0u << 4) | denshift_default, 8);
      element.write((4u << 5) | choices[ch].order, 8);
      for (std::uint32_t k = 0; k < choices[ch].order; ++k) {
        element.write(static_cast<std::uint32_t>(static_cast<std::uint16_t>(choices[ch].coefs[k])), 16);
      }
    }
    if (bytes_shifted != 0) {
      for (std::uint32_t i = 0; i < frames * (stereo ? 2u : 1u); ++i) {
        element.write(static_cast<std::uint32_t>(shifts[i]), shift);
      }
    }
    for (std::uint32_t ch = 0; ch < (stereo ? 2u : 1u); ++ch) {
      std::array<std::int16_t, 32> working = choices[ch].coefs;
      pc_block(ch == 0 ? u.data() : v.data(), scratch.data(), frames, working.data(), choices[ch].order, chan_bits,
               denshift_default);
      dyn_comp(ag, scratch.data(), frames, chan_bits, element);
      coefs_[std::min<std::size_t>(c0 + ch, coefs_.size() - 1)] = working;  // adapted state seeds the next frame
    }
    // Escape (verbatim) candidate size.
    const std::uint64_t escape_bits =
        3 + 4 + 12 + 4 + (partial ? 32 : 0) + std::uint64_t{frames} * (stereo ? 2u : 1u) * depth;
    if (element.bit_count() < escape_bits) {
      out.append(element);
    } else {
      out.write(stereo ? id_cpe : id_sce, 3);
      out.write(0, 4);
      out.write(0, 12);
      out.write((partial ? 8u : 0u) | 1u, 4);
      if (partial) out.write(frames, 32);
      for (std::uint32_t i = 0; i < frames; ++i) {
        out.write(static_cast<std::uint32_t>(sample(i, c0)), depth);
        if (stereo) out.write(static_cast<std::uint32_t>(sample(i, c0 + 1)), depth);
      }
    }
    channel += stereo ? 2 : 1;
  }
  out.write(id_end, 3);
  std::vector<std::uint8_t> bytes = out.take();
  return to_bytes(std::span<const std::uint8_t>(bytes));
}

}  // namespace cvt::audio::alac
