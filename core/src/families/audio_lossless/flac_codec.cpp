// Convertible core - audio_lossless family: FLAC frame decoder and encoder.
//
// Decoder: every subframe type (constant, verbatim, fixed 0-4, LPC 1-32),
// wasted bits, all channel decorrelations, Rice and Rice2 residuals with
// escapes, 4-32 bit samples (33-bit side channels), fixed and variable
// block sizes, header CRC-8 and frame CRC-16 verification.
//
// Encoder: per block, every channel (and the mid/side combinations) is
// coded as constant, verbatim, fixed or LPC subframe, whichever is
// smallest. LPC uses windowed autocorrelation (Tukey and partial Tukey
// windows), Levinson-Durbin, the reference encoder's order estimate and
// coefficient quantisation, and an exact Rice partition search (exact bit
// counts for every partition order and parameter).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/audio_lossless/flac_codec.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <utility>

#include "families/audio_lossless/bits.hpp"

namespace cvt::audio::flac {

namespace {

constexpr std::array<std::uint8_t, 256> make_crc8() {
  std::array<std::uint8_t, 256> t{};
  for (unsigned i = 0; i < 256; ++i) {
    unsigned c = i;
    for (int k = 0; k < 8; ++k) c = (c & 0x80) ? ((c << 1) ^ 0x07) : (c << 1);
    t[i] = static_cast<std::uint8_t>(c);
  }
  return t;
}
constexpr std::array<std::uint16_t, 256> make_crc16() {
  std::array<std::uint16_t, 256> t{};
  for (unsigned i = 0; i < 256; ++i) {
    unsigned c = i << 8;
    for (int k = 0; k < 8; ++k) c = (c & 0x8000) ? ((c << 1) ^ 0x8005) : (c << 1);
    t[i] = static_cast<std::uint16_t>(c);
  }
  return t;
}
constexpr std::array<std::uint8_t, 256> crc8_table = make_crc8();
constexpr std::array<std::uint16_t, 256> crc16_table = make_crc16();

std::uint8_t crc8(std::span<const std::uint8_t> data) {
  std::uint8_t c = 0;
  for (const std::uint8_t b : data) c = crc8_table[c ^ b];
  return c;
}
std::uint16_t crc16(std::span<const std::uint8_t> data) {
  std::uint16_t c = 0;
  for (const std::uint8_t b : data) c = static_cast<std::uint16_t>((c << 8) ^ crc16_table[(c >> 8) ^ b]);
  return c;
}

constexpr std::array<std::uint32_t, 16> rate_table = {0,     88200, 176400, 192000, 8000, 16000, 22050, 24000,
                                                      32000, 44100, 48000,  96000,  0,    0,     0,     0};
constexpr std::array<std::uint32_t, 8> size_table = {0, 8, 12, 0, 16, 20, 24, 32};

struct Header {
  std::uint32_t block_size = 0;
  std::uint32_t sample_rate = 0;
  std::uint32_t assignment = 0;
  std::uint32_t channels = 0;
  std::uint32_t bits = 0;
  std::uint64_t number = 0;
  bool variable = false;
  std::size_t length = 0;
};

Result<Header> parse_header(ByteView data, const StreamInfo* info) {
  const std::span<const std::uint8_t> b = as_u8(data);
  if (b.size() < 6) return fail(Status::Decode, "truncated FLAC frame header");
  if (b[0] != 0xFF || (b[1] & 0xFE) != 0xF8) return fail(Status::Decode, "lost FLAC frame sync");
  Header h;
  h.variable = (b[1] & 1) != 0;
  const unsigned bs_code = b[2] >> 4;
  const unsigned sr_code = b[2] & 0x0F;
  h.assignment = b[3] >> 4;
  const unsigned ss_code = (b[3] >> 1) & 7;
  if ((b[3] & 1) != 0) return fail(Status::Decode, "invalid FLAC frame header");
  std::size_t pos = 4;
  // UTF-8-like coded frame/sample number.
  const std::uint8_t first = b[pos++];
  int extra = 0;
  std::uint64_t number = 0;
  if ((first & 0x80) == 0) {
    number = first;
  } else if ((first & 0xE0) == 0xC0) {
    number = first & 0x1F;
    extra = 1;
  } else if ((first & 0xF0) == 0xE0) {
    number = first & 0x0F;
    extra = 2;
  } else if ((first & 0xF8) == 0xF0) {
    number = first & 0x07;
    extra = 3;
  } else if ((first & 0xFC) == 0xF8) {
    number = first & 0x03;
    extra = 4;
  } else if ((first & 0xFE) == 0xFC) {
    number = first & 0x01;
    extra = 5;
  } else if (first == 0xFE) {
    extra = 6;
  } else {
    return fail(Status::Decode, "invalid FLAC frame number");
  }
  for (int i = 0; i < extra; ++i) {
    if (pos >= b.size()) return fail(Status::Decode, "truncated FLAC frame header");
    const std::uint8_t c = b[pos++];
    if ((c & 0xC0) != 0x80) return fail(Status::Decode, "invalid FLAC frame number");
    number = (number << 6) | (c & 0x3F);
  }
  h.number = number;
  const auto take = [&](int count) -> std::uint32_t {
    std::uint32_t v = 0;
    for (int i = 0; i < count; ++i) v = (v << 8) | (pos < b.size() ? b[pos++] : (pos++, 0u));
    return v;
  };
  if (bs_code == 0) return fail(Status::Decode, "reserved FLAC block size");
  if (bs_code == 1) h.block_size = 192;
  else if (bs_code <= 5) h.block_size = 576u << (bs_code - 2);
  else if (bs_code == 6) h.block_size = take(1) + 1;
  else if (bs_code == 7) h.block_size = take(2) + 1;
  else h.block_size = 256u << (bs_code - 8);
  if (sr_code == 0) h.sample_rate = info != nullptr ? info->sample_rate : 0;
  else if (sr_code <= 11) h.sample_rate = rate_table[sr_code];
  else if (sr_code == 12) h.sample_rate = take(1) * 1000;
  else if (sr_code == 13) h.sample_rate = take(2);
  else if (sr_code == 14) h.sample_rate = take(2) * 10;
  else return fail(Status::Decode, "invalid FLAC sample rate code");
  if (ss_code == 3) return fail(Status::Decode, "reserved FLAC sample size");
  h.bits = ss_code == 0 ? (info != nullptr ? info->bits : 0) : size_table[ss_code];
  if (h.bits == 0) return fail(Status::Decode, "FLAC frame without a sample size");
  if (h.assignment <= 7) h.channels = h.assignment + 1;
  else if (h.assignment <= 10) h.channels = 2;
  else return fail(Status::Decode, "reserved FLAC channel assignment");
  if (pos >= b.size()) return fail(Status::Decode, "truncated FLAC frame header");
  if (crc8(b.first(pos)) != b[pos]) return fail(Status::Decode, "FLAC frame header checksum mismatch");
  h.length = pos + 1;
  return h;
}

// ---- decoding

Result<void> decode_residual(BitReader& br, std::uint32_t block_size, std::uint32_t order, std::int64_t* residual) {
  const std::uint32_t method = br.read(2);
  if (method > 1) return fail(Status::Decode, "reserved FLAC residual coding");
  const unsigned param_bits = method == 0 ? 4 : 5;
  const std::uint32_t escape = method == 0 ? 15 : 31;
  const std::uint32_t partition_order = br.read(4);
  const std::uint32_t partitions = 1u << partition_order;
  if (block_size % partitions != 0) return fail(Status::Decode, "FLAC partition order does not divide the block");
  const std::uint32_t per_partition = block_size >> partition_order;
  if (per_partition < order) return fail(Status::Decode, "FLAC partition shorter than the predictor order");
  std::uint32_t at = 0;
  for (std::uint32_t p = 0; p < partitions; ++p) {
    const std::uint32_t count = p == 0 ? per_partition - order : per_partition;
    const std::uint32_t k = br.read(param_bits);
    if (k == escape) {
      const std::uint32_t raw = br.read(5);
      for (std::uint32_t i = 0; i < count; ++i) residual[at++] = raw == 0 ? 0 : br.read_signed(raw);
    } else {
      for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint64_t q = br.unary_zeros(0xFFFFFFFFu);
        if (br.overrun()) return fail(Status::Decode, "truncated FLAC residual");
        const std::uint64_t u = (q << k) | br.read(k);
        residual[at++] = static_cast<std::int64_t>(u >> 1) ^ -static_cast<std::int64_t>(u & 1);
      }
    }
    if (br.overrun()) return fail(Status::Decode, "truncated FLAC residual");
  }
  return {};
}

std::int64_t read_sample(BitReader& br, std::uint32_t bits) {
  if (bits <= 32) return br.read_signed(bits);
  const std::uint64_t v = br.read64(bits);
  const std::uint64_t sign = std::uint64_t{1} << (bits - 1);
  return static_cast<std::int64_t>(v ^ sign) - static_cast<std::int64_t>(sign);
}

Result<void> decode_subframe(BitReader& br, std::uint32_t block_size, std::uint32_t bits, std::int64_t* out) {
  if (br.read(1) != 0) return fail(Status::Decode, "invalid FLAC subframe padding");
  const std::uint32_t type = br.read(6);
  std::uint32_t wasted = 0;
  if (br.bit()) wasted = br.unary_zeros(bits) + 1;
  if (wasted >= bits) return fail(Status::Decode, "invalid FLAC wasted bits");
  bits -= wasted;
  if (type == 0) {
    const std::int64_t v = read_sample(br, bits);
    for (std::uint32_t i = 0; i < block_size; ++i) out[i] = v;
  } else if (type == 1) {
    for (std::uint32_t i = 0; i < block_size; ++i) out[i] = read_sample(br, bits);
  } else if (type >= 8 && type <= 12) {
    const std::uint32_t order = type - 8;
    if (order > block_size) return fail(Status::Decode, "FLAC predictor order exceeds the block");
    for (std::uint32_t i = 0; i < order; ++i) out[i] = read_sample(br, bits);
    CVT_TRY(decode_residual(br, block_size, order, out + order));
    for (std::uint32_t i = order; i < block_size; ++i) {
      // Wrapping arithmetic (hostile streams must not cause undefined behaviour).
      const auto u = [&](std::uint32_t back) { return static_cast<std::uint64_t>(out[i - back]); };
      std::uint64_t p = 0;
      switch (order) {
        case 0: p = 0; break;
        case 1: p = u(1); break;
        case 2: p = 2 * u(1) - u(2); break;
        case 3: p = 3 * u(1) - 3 * u(2) + u(3); break;
        default: p = 4 * u(1) - 6 * u(2) + 4 * u(3) - u(4); break;
      }
      out[i] = static_cast<std::int64_t>(static_cast<std::uint64_t>(out[i]) + p);
    }
  } else if (type >= 32) {
    const std::uint32_t order = type - 31;
    if (order > block_size) return fail(Status::Decode, "FLAC predictor order exceeds the block");
    for (std::uint32_t i = 0; i < order; ++i) out[i] = read_sample(br, bits);
    const std::uint32_t precision = br.read(4) + 1;
    if (precision == 16) return fail(Status::Decode, "invalid FLAC coefficient precision");
    const std::int32_t shift = br.read_signed(5);
    if (shift < 0) return fail(Status::Decode, "negative FLAC LPC shift");
    std::array<std::int64_t, 32> coefs{};
    for (std::uint32_t i = 0; i < order; ++i) coefs[i] = br.read_signed(precision);
    CVT_TRY(decode_residual(br, block_size, order, out + order));
    for (std::uint32_t i = order; i < block_size; ++i) {
      // Wrapping arithmetic: valid streams never come close to overflow, and
      // hostile ones must not cause undefined behaviour (the result is garbage).
      std::uint64_t sum = 0;
      for (std::uint32_t j = 0; j < order; ++j) {
        sum += static_cast<std::uint64_t>(coefs[j]) * static_cast<std::uint64_t>(out[i - 1 - j]);
      }
      out[i] = static_cast<std::int64_t>(static_cast<std::uint64_t>(out[i]) + static_cast<std::uint64_t>(static_cast<std::int64_t>(sum) >> shift));
    }
  } else {
    return fail(Status::Decode, "reserved FLAC subframe type {}", type);
  }
  if (wasted != 0) {
    for (std::uint32_t i = 0; i < block_size; ++i) out[i] = static_cast<std::int64_t>(static_cast<std::uint64_t>(out[i]) << wasted);
  }
  return {};
}

}  // namespace

// =========================================================================== STREAMINFO

Result<StreamInfo> parse_streaminfo(ByteView body) {
  if (body.size() < 34) return fail(Status::Decode, "FLAC STREAMINFO is truncated");
  BitReader br(body);
  StreamInfo s;
  s.min_block = br.read(16);
  s.max_block = br.read(16);
  s.min_frame = br.read(24);
  s.max_frame = br.read(24);
  s.sample_rate = br.read(20);
  s.channels = br.read(3) + 1;
  s.bits = br.read(5) + 1;
  s.total_samples = br.read64(36);
  for (std::uint8_t& b : s.md5) b = static_cast<std::uint8_t>(br.read(8));
  if (s.sample_rate == 0) return fail(Status::Decode, "FLAC stream with a sample rate of 0");
  if (s.bits < 4) return fail(Status::Decode, "unsupported FLAC sample size {}", s.bits);
  if (s.max_block != 0 && s.max_block < 16 && s.max_block != s.min_block) {
    return fail(Status::Decode, "invalid FLAC block sizes");
  }
  return s;
}

Bytes serialize_streaminfo(const StreamInfo& s) {
  BitWriter w;
  w.write(s.min_block, 16);
  w.write(s.max_block, 16);
  w.write(s.min_frame, 24);
  w.write(s.max_frame, 24);
  w.write(s.sample_rate, 20);
  w.write(s.channels - 1, 3);
  w.write(s.bits - 1, 5);
  w.write64(s.total_samples, 36);
  for (const std::uint8_t b : s.md5) w.write(b, 8);
  std::vector<std::uint8_t> bytes = w.take();
  return to_bytes(std::span<const std::uint8_t>(bytes));
}

bool frame_header_at(ByteView data, const StreamInfo* info) {
  if (data.size() < 2) return false;
  const auto b0 = std::to_integer<std::uint8_t>(data[0]);
  const auto b1 = std::to_integer<std::uint8_t>(data[1]);
  if (b0 != 0xFF || (b1 & 0xFE) != 0xF8) return false;
  const auto h = parse_header(data.first(std::min<std::size_t>(data.size(), 16)), info);
  if (!h) return false;
  if (info != nullptr && (h->channels != info->channels || h->bits != info->bits)) return false;
  return true;
}

Result<std::size_t> decode_frame(ByteView data, const StreamInfo* info, std::vector<std::int32_t>& out, FrameInfo& frame) {
  CVT_TRY_ASSIGN(const Header h, parse_header(data, info));
  if (h.bits > 32) return fail(Status::Decode, "unsupported FLAC sample size {}", h.bits);
  if (info != nullptr && (h.channels != info->channels)) return fail(Status::Decode, "FLAC frame channel count changes");
  BitReader br(data);
  br.skip(std::uint64_t{h.length} * 8);
  std::vector<std::int64_t> planes(std::size_t{h.block_size} * h.channels);
  for (std::uint32_t c = 0; c < h.channels; ++c) {
    std::uint32_t bits = h.bits;
    if ((h.assignment == 8 && c == 1) || (h.assignment == 9 && c == 0) || (h.assignment == 10 && c == 1)) ++bits;
    CVT_TRY(decode_subframe(br, h.block_size, bits, planes.data() + std::size_t{c} * h.block_size));
    if (br.overrun()) return fail(Status::Decode, "truncated FLAC frame");
  }
  br.align();
  const std::size_t end = br.byte_position();
  if (end + 2 > data.size()) return fail(Status::Decode, "truncated FLAC frame");
  const std::span<const std::uint8_t> bytes = as_u8(data);
  const std::uint16_t stored = static_cast<std::uint16_t>((bytes[end] << 8) | bytes[end + 1]);
  if (crc16(bytes.first(end)) != stored) return fail(Status::Decode, "FLAC frame checksum mismatch");
  std::int64_t* a = planes.data();
  std::int64_t* b = planes.data() + h.block_size;
  const std::uint32_t n = h.block_size;
  switch (h.assignment) {
    case 8:
      for (std::uint32_t i = 0; i < n; ++i) b[i] = a[i] - b[i];
      break;
    case 9:
      for (std::uint32_t i = 0; i < n; ++i) a[i] += b[i];
      break;
    case 10:
      for (std::uint32_t i = 0; i < n; ++i) {
        std::int64_t mid = a[i];
        const std::int64_t side = b[i];
        mid = static_cast<std::int64_t>(static_cast<std::uint64_t>(mid) << 1) | (side & 1);
        a[i] = (mid + side) >> 1;
        b[i] = (mid - side) >> 1;
      }
      break;
    default: break;
  }
  const std::size_t start = out.size();
  out.resize(start + std::size_t{n} * h.channels);
  const std::int64_t low = -(std::int64_t{1} << (h.bits - 1));
  const std::int64_t high = (std::int64_t{1} << (h.bits - 1)) - 1;
  for (std::uint32_t i = 0; i < n; ++i) {
    for (std::uint32_t c = 0; c < h.channels; ++c) {
      const std::int64_t v = planes[std::size_t{c} * n + i];
      if (v < low || v > high) return fail(Status::Decode, "FLAC sample out of range");
      out[start + std::size_t{i} * h.channels + c] = static_cast<std::int32_t>(v);
    }
  }
  frame.block_size = n;
  frame.sample_rate = h.sample_rate;
  frame.channels = h.channels;
  frame.bits = h.bits;
  frame.first_sample = h.variable ? h.number : h.number * (info != nullptr && info->min_block == info->max_block && info->max_block != 0 ? info->max_block : n);
  return end + 2;
}

// =========================================================================== MD5

SampleMd5::SampleMd5(std::uint32_t bits) : bytes_((bits + 7) / 8) {}

void SampleMd5::update(std::span<const std::int32_t> samples) {
  buffer_.resize(samples.size() * bytes_);
  std::size_t at = 0;
  for (const std::int32_t s : samples) {
    const auto u = static_cast<std::uint32_t>(s);
    for (std::uint32_t k = 0; k < bytes_; ++k) buffer_[at++] = static_cast<std::uint8_t>(u >> (8 * k));
  }
  md5_.update(as_bytes(std::span<const std::uint8_t>(buffer_)));
}

std::array<std::uint8_t, 16> SampleMd5::finish() { return md5_.finish(); }

// =========================================================================== encoder

Settings settings_for_level(int level) {
  Settings s;
  level = std::clamp(level, 0, 8);
  s.block_size = level <= 2 ? 1152 : 4096;
  s.mid_side = !(level == 0 || level == 3);
  s.max_lpc_order = level <= 2 ? 0 : level == 3 ? 6 : level <= 6 ? 8 : 12;
  s.max_partition_order = level <= 2 ? 3 : level == 3 ? 4 : level == 4 ? 4 : level == 5 ? 5 : 6;
  s.windows = level <= 5 ? 1 : level <= 7 ? 2 : 3;
  s.exhaustive = false;
  return s;
}

Result<void> FrameEncoder::configure(std::uint32_t sample_rate, std::uint32_t channels, std::uint32_t bits,
                                     const Settings& settings) {
  if (channels == 0 || channels > 8) return fail(Status::InvalidArgument, "FLAC holds 1 to 8 channels");
  if (bits < 4 || bits > 32) return fail(Status::InvalidArgument, "FLAC sample size must be 4 to 32 bits");
  if (sample_rate == 0 || sample_rate > 1048575) return fail(Status::InvalidArgument, "FLAC sample rate out of range");
  if (settings.block_size < 16 || settings.block_size > 65535) return fail(Status::InvalidArgument, "invalid FLAC block size");
  sample_rate_ = sample_rate;
  channels_ = channels;
  bits_ = bits;
  settings_ = settings;
  settings_.max_lpc_order = std::min<std::uint32_t>(settings_.max_lpc_order, 32);
  settings_.max_partition_order = std::min<std::uint32_t>(settings_.max_partition_order, 15);
  return {};
}

namespace {

// Exact Rice coding of a residual: best partition order and parameters.
struct RicePlan {
  std::uint64_t bits = std::numeric_limits<std::uint64_t>::max();
  std::uint32_t partition_order = 0;
  bool rice2 = false;
  std::vector<std::uint32_t> params;
};

constexpr std::uint32_t max_rice_k = 30;

RicePlan plan_rice(const std::int64_t* residual, std::uint32_t block_size, std::uint32_t order,
                   std::uint32_t min_porder, std::uint32_t max_porder) {
  // Largest usable order: divides the block and leaves the first partition > order.
  std::uint32_t top = 0;
  while (top < max_porder && (block_size % (2u << top)) == 0 && (block_size >> (top + 1)) > order) ++top;
  min_porder = std::min(min_porder, top);
  const std::uint32_t partitions = 1u << top;
  const std::uint32_t per = block_size >> top;
  // sums[p][k] = sum over the partition of (u >> k); count[p].
  std::vector<std::array<std::uint64_t, max_rice_k + 1>> sums(partitions);
  std::vector<std::uint32_t> counts(partitions);
  std::size_t at = 0;
  for (std::uint32_t p = 0; p < partitions; ++p) {
    const std::uint32_t count = p == 0 ? per - order : per;
    counts[p] = count;
    std::array<std::uint64_t, max_rice_k + 1>& s = sums[p];
    s.fill(0);
    for (std::uint32_t i = 0; i < count; ++i) {
      const std::int64_t r = residual[at++];
      const std::uint64_t u = r >= 0 ? static_cast<std::uint64_t>(r) << 1 : (static_cast<std::uint64_t>(-(r + 1)) << 1) | 1u;
      std::uint64_t v = u;
      for (std::uint32_t k = 0; k <= max_rice_k && v != 0; ++k) {
        s[k] += v;
        v >>= 1;
      }
    }
  }
  RicePlan best;
  for (std::uint32_t porder = top + 1; porder-- > min_porder;) {
    if (porder < top) {
      // Merge neighbouring partitions.
      const std::uint32_t n = 1u << porder;
      for (std::uint32_t p = 0; p < n; ++p) {
        for (std::uint32_t k = 0; k <= max_rice_k; ++k) sums[p][k] = sums[2 * p][k] + sums[2 * p + 1][k];
        counts[p] = counts[2 * p] + counts[2 * p + 1];
      }
    }
    const std::uint32_t n = 1u << porder;
    RicePlan plan;
    plan.partition_order = porder;
    plan.params.resize(n);
    std::uint64_t total = 0;
    bool needs_rice2 = false;
    for (std::uint32_t p = 0; p < n; ++p) {
      std::uint64_t best_bits = std::numeric_limits<std::uint64_t>::max();
      std::uint32_t best_k = 0;
      for (std::uint32_t k = 0; k <= max_rice_k; ++k) {
        const std::uint64_t bits = std::uint64_t{counts[p]} * (k + 1) + sums[p][k];
        if (bits < best_bits) {
          best_bits = bits;
          best_k = k;
        }
        if (sums[p][k] == 0) break;
      }
      plan.params[p] = best_k;
      if (best_k >= 15) needs_rice2 = true;
      total += best_bits;
    }
    plan.rice2 = needs_rice2;
    total += std::uint64_t{n} * (needs_rice2 ? 5 : 4) + 2 + 4;
    plan.bits = total;
    if (total < best.bits) best = std::move(plan);
  }
  return best;
}

void write_residual(BitWriter& w, const std::int64_t* residual, std::uint32_t block_size, std::uint32_t order,
                    const RicePlan& plan) {
  w.write(plan.rice2 ? 1 : 0, 2);
  w.write(plan.partition_order, 4);
  const std::uint32_t n = 1u << plan.partition_order;
  const std::uint32_t per = block_size >> plan.partition_order;
  std::size_t at = 0;
  for (std::uint32_t p = 0; p < n; ++p) {
    const std::uint32_t k = plan.params[p];
    w.write(k, plan.rice2 ? 5 : 4);
    const std::uint32_t count = p == 0 ? per - order : per;
    for (std::uint32_t i = 0; i < count; ++i) {
      const std::int64_t r = residual[at++];
      const std::uint64_t u = r >= 0 ? static_cast<std::uint64_t>(r) << 1 : (static_cast<std::uint64_t>(-(r + 1)) << 1) | 1u;
      const std::uint64_t q = u >> k;
      w.unary(static_cast<std::uint32_t>(q));
      if (k > 0) w.write(static_cast<std::uint32_t>(u & ((std::uint64_t{1} << k) - 1u)), k);
    }
  }
}

struct Subframe {
  enum class Type { Constant, Verbatim, Fixed, Lpc } type = Type::Verbatim;
  std::uint64_t bits = std::numeric_limits<std::uint64_t>::max();
  std::uint32_t order = 0;
  std::uint32_t precision = 0;
  std::int32_t shift = 0;
  std::array<std::int32_t, 32> coefs{};
  std::uint32_t wasted = 0;
  RicePlan rice;
  std::vector<std::int64_t> residual;
};

std::uint32_t auto_precision(std::uint32_t bits, std::uint32_t block_size) {
  if (bits < 16) return std::max<std::uint32_t>(5, 2 + bits / 2);
  if (bits == 16) {
    if (block_size <= 192) return 7;
    if (block_size <= 384) return 8;
    if (block_size <= 576) return 9;
    if (block_size <= 1152) return 10;
    if (block_size <= 2304) return 11;
    if (block_size <= 4608) return 12;
    return 13;
  }
  return 15;
}

// Windows: 0 = tukey(0.5); 1, 2 = partial tukey halves; 3 = punch-out middle; 4..6 = thirds.
void make_window(int kind, std::uint32_t n, std::vector<double>& w) {
  w.assign(n, 0.0);
  const auto tukey = [&](std::uint32_t start, std::uint32_t end, double p) {
    const std::uint32_t len = end - start;
    if (len == 0) return;
    const auto taper = static_cast<std::uint32_t>(p / 2.0 * len);
    for (std::uint32_t i = 0; i < len; ++i) {
      double v = 1.0;
      if (taper > 0 && i < taper) v = 0.5 - 0.5 * std::cos(std::numbers::pi * i / taper);
      else if (taper > 0 && i >= len - taper) v = 0.5 - 0.5 * std::cos(std::numbers::pi * (len - 1 - i) / taper);
      w[start + i] = v;
    }
  };
  switch (kind) {
    case 0: tukey(0, n, 0.5); break;
    case 1: tukey(0, n / 2, 0.5); break;
    case 2: tukey(n / 2, n, 0.5); break;
    case 3:
      tukey(0, n, 0.5);
      for (std::uint32_t i = n / 4; i < 3 * n / 4; ++i) w[i] = 0.0;
      break;
    case 4: tukey(0, n / 3, 0.5); break;
    case 5: tukey(n / 3, 2 * n / 3, 0.5); break;
    default: tukey(2 * n / 3, n, 0.5); break;
  }
}

void fixed_residual(const std::int64_t* s, std::uint32_t n, std::uint32_t order, std::int64_t* r) {
  for (std::uint32_t i = order; i < n; ++i) {
    std::int64_t p = 0;
    switch (order) {
      case 0: p = 0; break;
      case 1: p = s[i - 1]; break;
      case 2: p = 2 * s[i - 1] - s[i - 2]; break;
      case 3: p = 3 * s[i - 1] - 3 * s[i - 2] + s[i - 3]; break;
      default: p = 4 * s[i - 1] - 6 * s[i - 2] + 4 * s[i - 3] - s[i - 4]; break;
    }
    r[i - order] = s[i] - p;
  }
}

bool quantize_lpc(const double* lp, std::uint32_t order, std::uint32_t precision, std::array<std::int32_t, 32>& q,
                  std::int32_t& shift) {
  double cmax = 0.0;
  for (std::uint32_t i = 0; i < order; ++i) cmax = std::max(cmax, std::abs(lp[i]));
  if (!(cmax > 0.0) || !std::isfinite(cmax)) return false;
  const std::int32_t qmax = (1 << (precision - 1)) - 1;
  const std::int32_t qmin = -(1 << (precision - 1));
  int log2cmax = 0;
  (void)std::frexp(cmax, &log2cmax);
  --log2cmax;
  shift = static_cast<std::int32_t>(precision) - 1 - log2cmax - 1;
  if (shift > 15) shift = 15;
  if (shift < 0) return false;
  double error = 0.0;
  for (std::uint32_t i = 0; i < order; ++i) {
    error += lp[i] * static_cast<double>(1 << shift);
    std::int64_t v = std::llround(error);
    v = std::clamp<std::int64_t>(v, qmin, qmax);
    q[i] = static_cast<std::int32_t>(v);
    error -= static_cast<double>(v);
  }
  return true;
}

bool lpc_residual(const std::int64_t* s, std::uint32_t n, std::uint32_t order, const std::array<std::int32_t, 32>& c,
                  std::int32_t shift, std::uint32_t bits, std::int64_t* r) {
  // Residuals must fit 32 bits for decoders.
  const std::int64_t limit = std::numeric_limits<std::int32_t>::max();
  for (std::uint32_t i = order; i < n; ++i) {
    std::int64_t sum = 0;
    for (std::uint32_t j = 0; j < order; ++j) sum += static_cast<std::int64_t>(c[j]) * s[i - 1 - j];
    const std::int64_t v = s[i] - (sum >> shift);
    if (v > limit || v < -limit) return false;
    r[i - order] = v;
  }
  (void)bits;
  return true;
}

Subframe encode_subframe_plan(const std::int64_t* input, std::uint32_t n, std::uint32_t bits, const Settings& settings,
                              std::uint32_t sample_rate) {
  (void)sample_rate;
  Subframe best;
  // Constant?
  bool constant = true;
  for (std::uint32_t i = 1; i < n; ++i) {
    if (input[i] != input[0]) {
      constant = false;
      break;
    }
  }
  if (constant) {
    best.type = Subframe::Type::Constant;
    best.bits = 8 + bits;
    return best;
  }
  // Wasted bits.
  std::uint64_t all = 0;
  for (std::uint32_t i = 0; i < n; ++i) all |= static_cast<std::uint64_t>(input[i]);
  std::uint32_t wasted = 0;
  while (wasted + 1 < bits && ((all >> wasted) & 1u) == 0) ++wasted;
  std::vector<std::int64_t> shifted;
  const std::int64_t* s = input;
  if (wasted != 0) {
    shifted.resize(n);
    for (std::uint32_t i = 0; i < n; ++i) shifted[i] = input[i] >> wasted;
    s = shifted.data();
  }
  const std::uint32_t b = bits - wasted;
  const std::uint64_t header = 8 + (wasted != 0 ? wasted + 1 : 0);
  // Verbatim.
  best.type = Subframe::Type::Verbatim;
  best.bits = header + std::uint64_t{b} * n;
  best.wasted = wasted;

  std::vector<std::int64_t> residual(n);
  // Fixed predictors: pick by sum of |residual|, then code exactly.
  {
    std::uint32_t best_order = 0;
    std::uint64_t best_sum = std::numeric_limits<std::uint64_t>::max();
    for (std::uint32_t order = 0; order <= std::min<std::uint32_t>(4, n - 1); ++order) {
      fixed_residual(s, n, order, residual.data());
      std::uint64_t sum = 0;
      for (std::uint32_t i = 0; i < n - order; ++i) sum += static_cast<std::uint64_t>(residual[i] < 0 ? -residual[i] : residual[i]);
      if (sum < best_sum) {
        best_sum = sum;
        best_order = order;
      }
    }
    fixed_residual(s, n, best_order, residual.data());
    RicePlan rice = plan_rice(residual.data(), n, best_order, settings.min_partition_order, settings.max_partition_order);
    const std::uint64_t total = header + std::uint64_t{b} * best_order + rice.bits;
    if (total < best.bits) {
      best.type = Subframe::Type::Fixed;
      best.bits = total;
      best.order = best_order;
      best.rice = std::move(rice);
      best.residual.assign(residual.begin(), residual.begin() + (n - best_order));
    }
  }
  // LPC.
  const std::uint32_t max_order = std::min<std::uint32_t>(settings.max_lpc_order, n > 1 ? n - 1 : 0);
  if (max_order > 0 && n > max_order * 2) {
    const std::uint32_t precision =
        settings.qlp_precision != 0 ? settings.qlp_precision : std::min<std::uint32_t>(auto_precision(b, n), 15);
    std::vector<int> kinds = {0};
    if (settings.windows >= 2) kinds = {0, 1, 2, 3};
    if (settings.windows >= 3) kinds = {0, 1, 2, 3, 4, 5, 6};
    std::vector<double> window, x(n);
    for (const int kind : kinds) {
      make_window(kind, n, window);
      for (std::uint32_t i = 0; i < n; ++i) x[i] = static_cast<double>(s[i]) * window[i];
      std::array<double, 33> autoc{};
      for (std::uint32_t lag = 0; lag <= max_order; ++lag) {
        double sum = 0.0;
        for (std::uint32_t i = lag; i < n; ++i) sum += x[i] * x[i - lag];
        autoc[lag] = sum;
      }
      if (!(autoc[0] > 0.0)) continue;
      autoc[0] *= 1.0 + 1e-10;  // tiny regularisation for numerically flat signals
      // Levinson-Durbin: coefficients and error for every order.
      std::array<std::array<double, 32>, 32> lp{};
      std::array<double, 33> err{};
      std::array<double, 32> a{};
      double e = autoc[0];
      std::uint32_t reached = 0;
      for (std::uint32_t i = 0; i < max_order; ++i) {
        double r = -autoc[i + 1];
        for (std::uint32_t j = 0; j < i; ++j) r -= a[j] * autoc[i - j];
        if (!(e > 0.0)) break;
        r /= e;
        a[i] = r;
        for (std::uint32_t j = 0; j < i / 2; ++j) {
          const double tmp = a[j];
          a[j] += r * a[i - 1 - j];
          a[i - 1 - j] += r * tmp;
        }
        if ((i & 1) != 0) a[i / 2] += a[i / 2] * r;
        e *= 1.0 - r * r;
        for (std::uint32_t j = 0; j <= i; ++j) lp[i][j] = -a[j];
        err[i] = e;
        reached = i + 1;
      }
      if (reached == 0) continue;
      // Order estimate (as the reference encoder): expected bits per sample.
      std::vector<std::uint32_t> orders;
      if (settings.exhaustive) {
        for (std::uint32_t o = 1; o <= reached; ++o) orders.push_back(o);
      } else {
        const double error_scale = 0.5 / static_cast<double>(n);
        double best_est = std::numeric_limits<double>::infinity();
        std::uint32_t best_o = 1;
        for (std::uint32_t o = 1; o <= reached; ++o) {
          const double ev = err[o - 1];
          double bps = ev > 0.0 ? 0.5 * std::log2(error_scale * ev) : 0.0;
          if (bps < 0.0) bps = 0.0;
          const double est = bps * static_cast<double>(n - o) + static_cast<double>(o) * (precision + b);
          if (est < best_est) {
            best_est = est;
            best_o = o;
          }
        }
        orders.push_back(best_o);
      }
      for (const std::uint32_t order : orders) {
        std::array<std::int32_t, 32> q{};
        std::int32_t shift = 0;
        if (!quantize_lpc(lp[order - 1].data(), order, precision, q, shift)) continue;
        if (!lpc_residual(s, n, order, q, shift, b, residual.data())) continue;
        RicePlan rice = plan_rice(residual.data(), n, order, settings.min_partition_order, settings.max_partition_order);
        const std::uint64_t total = header + std::uint64_t{b} * order + 4 + 5 + std::uint64_t{precision} * order + rice.bits;
        if (total < best.bits) {
          best.type = Subframe::Type::Lpc;
          best.bits = total;
          best.order = order;
          best.precision = precision;
          best.shift = shift;
          best.coefs = q;
          best.rice = std::move(rice);
          best.residual.assign(residual.begin(), residual.begin() + (n - order));
        }
      }
    }
  }
  best.wasted = wasted;
  return best;
}

void write_subframe(BitWriter& w, const Subframe& sf, const std::int64_t* input, std::uint32_t n, std::uint32_t bits) {
  w.write(0, 1);
  const auto put_sample = [&](std::int64_t v, std::uint32_t width) { w.write64(static_cast<std::uint64_t>(v), width); };
  if (sf.type == Subframe::Type::Constant) {
    w.write(0, 6);
    w.write(0, 1);
    put_sample(input[0], bits);
    return;
  }
  const std::uint32_t b = bits - sf.wasted;
  const auto sample = [&](std::uint32_t i) { return input[i] >> sf.wasted; };
  switch (sf.type) {
    case Subframe::Type::Verbatim: w.write(1, 6); break;
    case Subframe::Type::Fixed: w.write(8 + sf.order, 6); break;
    case Subframe::Type::Lpc: w.write(31 + sf.order, 6); break;
    case Subframe::Type::Constant: break;
  }
  if (sf.wasted != 0) {
    w.write(1, 1);
    w.unary(sf.wasted - 1);
  } else {
    w.write(0, 1);
  }
  if (sf.type == Subframe::Type::Verbatim) {
    for (std::uint32_t i = 0; i < n; ++i) put_sample(sample(i), b);
    return;
  }
  for (std::uint32_t i = 0; i < sf.order; ++i) put_sample(sample(i), b);
  if (sf.type == Subframe::Type::Lpc) {
    w.write(sf.precision - 1, 4);
    w.write(static_cast<std::uint32_t>(sf.shift), 5);
    for (std::uint32_t i = 0; i < sf.order; ++i) w.write(static_cast<std::uint32_t>(sf.coefs[i]), sf.precision);
  }
  write_residual(w, sf.residual.data(), n, sf.order, sf.rice);
}

void write_utf8(BitWriter& w, std::uint64_t v) {
  if (v < 0x80) {
    w.write(static_cast<std::uint32_t>(v), 8);
    return;
  }
  int bytes = 2;
  while (bytes < 7 && v >= (std::uint64_t{1} << (5 * bytes + 1))) ++bytes;
  const std::uint32_t lead_bits = bytes == 7 ? 0 : 7 - bytes;
  const auto lead_mask = static_cast<std::uint32_t>((0xFF00u >> bytes) & 0xFFu);
  w.write(lead_mask | (static_cast<std::uint32_t>(v >> (6 * (bytes - 1))) & ((1u << lead_bits) - 1u)), 8);
  for (int i = bytes - 2; i >= 0; --i) w.write(0x80u | static_cast<std::uint32_t>((v >> (6 * i)) & 0x3F), 8);
}

}  // namespace

Bytes FrameEncoder::encode(std::span<const std::int32_t> interleaved, std::uint32_t frames, std::uint64_t frame_number) {
  const std::uint32_t n = frames;
  const std::uint32_t ch = channels_;
  std::vector<std::vector<std::int64_t>> planes(ch, std::vector<std::int64_t>(n));
  for (std::uint32_t i = 0; i < n; ++i) {
    for (std::uint32_t c = 0; c < ch; ++c) planes[c][i] = interleaved[std::size_t{i} * ch + c];
  }
  std::vector<Subframe> subframes(ch);
  for (std::uint32_t c = 0; c < ch; ++c) subframes[c] = encode_subframe_plan(planes[c].data(), n, bits_, settings_, sample_rate_);
  std::uint32_t assignment = ch - 1;
  std::vector<std::int64_t> mid, side;
  Subframe mid_sf, side_sf;
  if (ch == 2 && settings_.mid_side && bits_ < 32 && n > 0) {
    mid.resize(n);
    side.resize(n);
    for (std::uint32_t i = 0; i < n; ++i) {
      mid[i] = (planes[0][i] + planes[1][i]) >> 1;
      side[i] = planes[0][i] - planes[1][i];
    }
    mid_sf = encode_subframe_plan(mid.data(), n, bits_, settings_, sample_rate_);
    side_sf = encode_subframe_plan(side.data(), n, bits_ + 1, settings_, sample_rate_);
    const std::uint64_t lr = subframes[0].bits + subframes[1].bits;
    const std::uint64_t ls = subframes[0].bits + side_sf.bits;
    const std::uint64_t sr = side_sf.bits + subframes[1].bits;
    const std::uint64_t ms = mid_sf.bits + side_sf.bits;
    const std::uint64_t best = std::min({lr, ls, sr, ms});
    if (best == ms) assignment = 10;
    else if (best == ls) assignment = 8;
    else if (best == sr) assignment = 9;
  }
  BitWriter w;
  w.write(0xFFF8, 16);
  std::uint32_t bs_code;
  std::uint32_t bs_extra_bits = 0;
  switch (n) {
    case 192: bs_code = 1; break;
    case 576: bs_code = 2; break;
    case 1152: bs_code = 3; break;
    case 2304: bs_code = 4; break;
    case 4608: bs_code = 5; break;
    case 256: bs_code = 8; break;
    case 512: bs_code = 9; break;
    case 1024: bs_code = 10; break;
    case 2048: bs_code = 11; break;
    case 4096: bs_code = 12; break;
    case 8192: bs_code = 13; break;
    case 16384: bs_code = 14; break;
    case 32768: bs_code = 15; break;
    default:
      bs_code = n <= 256 ? 6 : 7;
      bs_extra_bits = n <= 256 ? 8 : 16;
      break;
  }
  std::uint32_t sr_code = 0;
  std::uint32_t sr_extra = 0, sr_extra_bits = 0;
  for (std::uint32_t i = 1; i <= 11; ++i) {
    if (rate_table[i] == sample_rate_) sr_code = i;
  }
  if (sr_code == 0) {
    if (sample_rate_ % 1000 == 0 && sample_rate_ / 1000 <= 255) {
      sr_code = 12;
      sr_extra = sample_rate_ / 1000;
      sr_extra_bits = 8;
    } else if (sample_rate_ <= 65535) {
      sr_code = 13;
      sr_extra = sample_rate_;
      sr_extra_bits = 16;
    } else if (sample_rate_ % 10 == 0 && sample_rate_ / 10 <= 65535) {
      sr_code = 14;
      sr_extra = sample_rate_ / 10;
      sr_extra_bits = 16;
    }
  }
  std::uint32_t ss_code = 0;
  for (std::uint32_t i = 1; i < 8; ++i) {
    if (size_table[i] == bits_) ss_code = i;
  }
  w.write(bs_code, 4);
  w.write(sr_code, 4);
  w.write(assignment, 4);
  w.write(ss_code, 3);
  w.write(0, 1);
  write_utf8(w, frame_number);
  if (bs_extra_bits != 0) w.write(n - 1, bs_extra_bits);
  if (sr_extra_bits != 0) w.write(sr_extra, sr_extra_bits);
  {
    const std::vector<std::uint8_t>& header = w.bytes();
    w.write(crc8(header), 8);
  }
  switch (assignment) {
    case 8:
      write_subframe(w, subframes[0], planes[0].data(), n, bits_);
      write_subframe(w, side_sf, side.data(), n, bits_ + 1);
      break;
    case 9:
      write_subframe(w, side_sf, side.data(), n, bits_ + 1);
      write_subframe(w, subframes[1], planes[1].data(), n, bits_);
      break;
    case 10:
      write_subframe(w, mid_sf, mid.data(), n, bits_);
      write_subframe(w, side_sf, side.data(), n, bits_ + 1);
      break;
    default:
      for (std::uint32_t c = 0; c < ch; ++c) write_subframe(w, subframes[c], planes[c].data(), n, bits_);
      break;
  }
  w.align();
  const std::uint16_t crc = crc16(w.bytes());
  w.write(crc, 16);
  std::vector<std::uint8_t> bytes = w.take();
  return to_bytes(std::span<const std::uint8_t>(bytes));
}

}  // namespace cvt::audio::flac
