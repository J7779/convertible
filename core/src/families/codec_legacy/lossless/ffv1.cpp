// Convertible core - codec_legacy family: FFV1 (RFC 9043).
//
// Decoder: versions 0, 1 and 3; Golomb-Rice and range coding with the default
// or a custom state transition table; slices; CRC-protected configuration
// record (extradata) and slices; YCbCr 4:2:0/4:2:2/4:4:4 (+ alpha) and RGB
// (JPEG 2000 RCT); 8 and 10 bits per sample (other depths are scaled to the
// nearest IR format). Output: I420/I422/I444, the P10 variants, or RGBA8.
// Encoder: version 3, range coder, one slice, CRC, lossless for I420, I422,
// I444, their 10-bit variants and RGBA8 (RCT, alpha when present).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "families/codec_legacy/common/bitstream.hpp"
#include "families/codec_legacy/common/codec_base.hpp"
#include "families/codec_legacy/lossless/lossless.hpp"

namespace cvt::legacy::lossless {

namespace {

constexpr int kContextSize = 32;

// RFC 9043 default state transition table.
constexpr std::uint8_t kDefaultTransition[256] = {
    0,   0,   0,   0,   0,   0,   0,   0,   20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,
    34,  35,  36,  37,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,
    55,  56,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,
    75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  94,  95,
    96,  97,  98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 114, 115, 116,
    117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 133, 134, 135, 136, 137,
    138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 152, 153, 154, 155, 156, 157, 158,
    159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 171, 172, 173, 174, 175, 176, 177, 178, 179,
    180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 190, 191, 192, 194, 194, 195, 196, 197, 198, 199, 200,
    201, 202, 202, 204, 205, 206, 207, 208, 209, 209, 210, 211, 212, 213, 215, 215, 216, 217, 218, 219, 220, 220,
    222, 223, 224, 225, 226, 227, 227, 229, 229, 230, 231, 232, 234, 234, 235, 236, 237, 238, 239, 240, 241, 242,
    243, 244, 245, 246, 247, 248, 248, 0,   0,   0,   0,   0,   0,   0};

// JPEG-LS run length exponents (Golomb-Rice run mode).
constexpr std::uint8_t kLog2Run[41] = {0, 0, 0, 0, 1, 1, 1, 1, 2, 2,  2,  2,  3,  3,  3,  3,  4,  4,  5,  5, 6,
                                       6, 7, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24};

/// CRC-32 with polynomial 0x04C11DB7, MSB first, initial value 0, no final
/// xor: the check value of FFV1 configuration records and slices.
std::uint32_t crc32_msb(std::span<const std::uint8_t> data, std::uint32_t crc = 0) {
  for (const std::uint8_t b : data) {
    crc ^= static_cast<std::uint32_t>(b) << 24;
    for (int k = 0; k < 8; ++k) crc = (crc & 0x80000000u) != 0 ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
  }
  return crc;
}

struct States {
  std::array<std::uint8_t, 256> one{};
  std::array<std::uint8_t, 256> zero{};
  void set(const std::uint8_t* table) {
    for (int i = 0; i < 256; ++i) one[static_cast<std::size_t>(i)] = table[i];
    zero[0] = 0;
    for (int i = 1; i < 256; ++i) zero[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(256 - one[static_cast<std::size_t>(256 - i)]);
  }
};

class RangeDecoder {
 public:
  RangeDecoder(std::span<const std::uint8_t> data, const States* states) : data_(data), states_(states) {
    pos_ = std::min<std::size_t>(2, data_.size());
    low_ = data_.size() >= 2 ? (data_[0] << 8 | data_[1]) : 0;
    if (low_ >= 0xFF00) {
      low_ = 0xFF00;
      end_ = pos_;
    } else {
      end_ = data_.size();
    }
  }
  bool bit(std::uint8_t& state) {
    const std::uint32_t range1 = (range_ * state) >> 8;
    range_ -= range1;
    if (low_ < range_) {
      state = states_->zero[state];
      refill();
      return false;
    }
    low_ -= range_;
    state = states_->one[state];
    range_ = range1;
    refill();
    return true;
  }
  int symbol(std::uint8_t* state, bool is_signed) {
    if (bit(state[0])) return 0;
    int e = 0;
    while (bit(state[1 + std::min(e, 9)])) {
      if (++e > 31) {
        overread_ = 1 << 20;
        return 0;
      }
    }
    std::uint32_t a = 1;
    for (int i = e - 1; i >= 0; --i) a = 2 * a + (bit(state[22 + std::min(i, 9)]) ? 1u : 0u);
    const bool negative = is_signed && bit(state[11 + std::min(e, 10)]);
    const int v = static_cast<int>(a & 0x7FFFFFFF);
    return negative ? -v : v;
  }
  void set_states(const States* states) { states_ = states; }
  /// Bytes consumed so far (the next unread byte).
  [[nodiscard]] std::size_t position() const noexcept { return pos_; }
  [[nodiscard]] bool overread() const noexcept { return overread_ > 8; }

 private:
  void refill() {
    if (range_ < 0x100) {
      range_ <<= 8;
      low_ <<= 8;
      if (pos_ < end_) {
        low_ += data_[pos_++];
      } else {
        ++overread_;
      }
    }
  }
  std::span<const std::uint8_t> data_;
  const States* states_;
  std::size_t pos_ = 0;
  std::size_t end_ = 0;
  std::uint32_t low_ = 0;
  std::uint32_t range_ = 0xFF00;
  int overread_ = 0;
};

class RangeEncoder {
 public:
  explicit RangeEncoder(const States* states) : states_(states) {}
  void bit(std::uint8_t& state, bool v) {
    const std::uint32_t range1 = (range_ * state) >> 8;
    if (!v) {
      range_ -= range1;
      state = states_->zero[state];
    } else {
      low_ += range_ - range1;
      range_ = range1;
      state = states_->one[state];
    }
    renorm();
  }
  void symbol(std::uint8_t* state, int v, bool is_signed) {
    if (v == 0) {
      bit(state[0], true);
      return;
    }
    const std::uint32_t a = static_cast<std::uint32_t>(v < 0 ? -v : v);
    int e = 0;
    while ((a >> (e + 1)) != 0) ++e;
    bit(state[0], false);
    for (int i = 0; i < e; ++i) bit(state[1 + std::min(i, 9)], true);
    bit(state[1 + std::min(e, 9)], false);
    for (int i = e - 1; i >= 0; --i) bit(state[22 + std::min(i, 9)], ((a >> i) & 1) != 0);
    if (is_signed) bit(state[11 + std::min(e, 10)], v < 0);
  }
  /// Terminates the coder the way RFC 9043 version 3 slices expect.
  std::vector<std::uint8_t> finish() {
    std::uint8_t s = 129;
    bit(s, false);
    range_ = 0xFF;
    low_ += 0xFF;
    renorm();
    range_ = 0xFF;
    renorm();
    return std::move(out_);
  }

 private:
  void renorm() {
    while (range_ < 0x100) {
      if (outstanding_byte_ < 0) {
        outstanding_byte_ = static_cast<int>(low_ >> 8);
      } else if (low_ <= 0xFF00) {
        out_.push_back(static_cast<std::uint8_t>(outstanding_byte_));
        for (; outstanding_count_ > 0; --outstanding_count_) out_.push_back(0xFF);
        outstanding_byte_ = static_cast<int>(low_ >> 8);
      } else if (low_ >= 0x10000) {
        out_.push_back(static_cast<std::uint8_t>(outstanding_byte_ + 1));
        for (; outstanding_count_ > 0; --outstanding_count_) out_.push_back(0x00);
        outstanding_byte_ = static_cast<int>(low_ >> 8) - 0x100;
      } else {
        ++outstanding_count_;
      }
      low_ = (low_ & 0xFF) << 8;
      range_ <<= 8;
    }
  }
  const States* states_;
  std::vector<std::uint8_t> out_;
  std::uint32_t low_ = 0;
  std::uint32_t range_ = 0xFF00;
  int outstanding_byte_ = -1;
  int outstanding_count_ = 0;
};

/// Five quantisation tables of 256 entries for one context set.
struct QuantTables {
  std::array<std::array<std::int16_t, 256>, 5> q{};
  int context_count = 1;
};

bool read_quant_tables(RangeDecoder& c, QuantTables& t) {
  int product = 1;
  for (auto& table : t.q) {
    std::array<std::uint8_t, kContextSize> state{};
    state.fill(128);
    int v = 0;
    for (int i = 0; i < 128; ++v) {
      const int len = c.symbol(state.data(), false) + 1;
      if (len <= 0 || len > 128 - i || c.overread()) return false;
      for (int k = 0; k < len; ++k) table[static_cast<std::size_t>(i++)] = static_cast<std::int16_t>(product * v);
    }
    for (int i = 1; i < 128; ++i) table[static_cast<std::size_t>(256 - i)] = static_cast<std::int16_t>(-table[static_cast<std::size_t>(i)]);
    table[128] = static_cast<std::int16_t>(-table[127]);
    product *= 2 * v - 1;
    if (product > 32768) return false;
  }
  t.context_count = (product + 1) / 2;
  return true;
}

/// Adaptive Golomb-Rice state of one context.
struct GolombState {
  int drift = 0;
  int error_sum = 4;
  int bias = 0;
  int count = 1;
  void update(int v) {
    error_sum += v < 0 ? -v : v;
    drift += v;
    if (count == 128) {
      count >>= 1;
      drift >>= 1;
      error_sum >>= 1;
    }
    ++count;
    if (drift <= -count) {
      bias = std::max(bias - 1, -128);
      drift = std::max(drift + count, -count + 1);
    } else if (drift > 0) {
      bias = std::min(bias + 1, 127);
      drift = std::min(drift - count, 0);
    }
  }
};

/// Per-plane contexts of one slice.
struct PlaneContext {
  int table = 0;
  std::vector<std::array<std::uint8_t, kContextSize>> states;
  std::vector<GolombState> golomb;
};

struct Config {
  int version = 0;
  int micro_version = 0;
  int coder = 0;  // 0 Golomb-Rice, 1 range default, 2 range custom
  States states;  // transition tables for slice data
  int colorspace = 0;
  int bits = 8;
  bool chroma_planes = true;
  int h_shift = 1;
  int v_shift = 1;
  bool alpha = false;
  int h_slices = 1;
  int v_slices = 1;
  bool ec = false;
  std::vector<QuantTables> tables;
  std::vector<std::vector<std::array<std::uint8_t, kContextSize>>> initial;  // per table
};

/// One slice: position, contexts (persist across frames for non-key frames).
struct Slice {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  std::vector<PlaneContext> planes;
  int run_index = 0;
};

[[nodiscard]] int mid_pred(int a, int b, int c) { return std::max(std::min(a, b), std::min(std::max(a, b), c)); }

/// Context of the sample at x of `cur` (`top` is the line above, `cur`
/// still holds the line two above at x and beyond).
[[nodiscard]] int context_of(const QuantTables& t, const int* cur, const int* top, int x) {
  const int lt = top[x - 1];
  const int tt = top[x];
  const int rt = top[x + 1];
  const int l = cur[x - 1];
  const auto& q = t.q;
  int ctx = q[0][static_cast<std::size_t>((l - lt) & 0xFF)] + q[1][static_cast<std::size_t>((lt - tt) & 0xFF)] +
            q[2][static_cast<std::size_t>((tt - rt) & 0xFF)];
  if (q[3][127] != 0 || q[4][127] != 0) {  // the two-sample-distance tables are in use
    ctx += q[3][static_cast<std::size_t>((cur[x - 2] - l) & 0xFF)] + q[4][static_cast<std::size_t>((cur[x] - tt) & 0xFF)];
  }
  return ctx;
}

/// Two line buffers with 3 samples of margin on each side, as RFC 9043 uses them.
struct Lines {
  std::vector<int> a;
  std::vector<int> b;
  int* cur = nullptr;
  int* top = nullptr;
  int width = 0;
  void reset(int w) {
    width = w;
    a.assign(static_cast<std::size_t>(w) + 6, 0);
    b.assign(static_cast<std::size_t>(w) + 6, 0);
    top = a.data() + 3;
    cur = b.data() + 3;
  }
  void next() {
    std::swap(cur, top);
    cur[-1] = top[0];
    top[width] = top[width - 1];
  }
};

class Ffv1Decoder final : public codec::VideoDecoder {
 public:
  Result<void> configure(const CodecParams& params) override {
    params_ = params;
    if (params_.timescale == 0) params_.timescale = 1000;
    default_states_.set(kDefaultTransition);
    if (!params.extradata.empty()) CVT_TRY(read_config(as_u8(ByteView(params.extradata))));
    return {};
  }

  Result<void> send_packet(const Packet& packet) override {
    if (packet.bytes.empty()) {
      if (!last_) return {};
      VideoFrame f = *last_;
      f.pts_ms = ticks_to_ms(packet.pts, params_.timescale);
      output_.push_back(std::move(f));
      return {};
    }
    CVT_TRY_ASSIGN(VideoFrame frame, decode(as_u8(ByteView(packet.bytes))));
    frame.pts_ms = ticks_to_ms(packet.pts, params_.timescale);
    frame.duration_ms = packet.duration > 0 ? ticks_to_ms(packet.duration, params_.timescale) : 0;
    last_ = frame;
    output_.push_back(std::move(frame));
    return {};
  }

  Result<std::optional<VideoFrame>> receive_frame() override {
    if (output_.empty()) return std::optional<VideoFrame>{};
    VideoFrame f = std::move(output_.front());
    output_.erase(output_.begin());
    return std::optional<VideoFrame>(std::move(f));
  }
  Result<void> flush() override { return {}; }

 private:
  Result<void> read_config(std::span<const std::uint8_t> data) {
    if (data.size() < 4) return fail(Status::Decode, "FFV1 configuration record is too short");
    RangeDecoder c(data, &default_states_);
    std::array<std::uint8_t, kContextSize> state{};
    state.fill(128);
    cfg_.version = c.symbol(state.data(), false);
    if (cfg_.version < 2) return fail(Status::Decode, "FFV1 version {} does not use a configuration record", cfg_.version);
    if (cfg_.version > 3) return fail(Status::Decode, "FFV1 version {} is not supported", cfg_.version);
    if (crc32_msb(data) != 0) return fail(Status::Decode, "FFV1 configuration record CRC mismatch");
    cfg_.micro_version = c.symbol(state.data(), false);
    CVT_TRY(read_coder(c, state.data()));
    cfg_.colorspace = c.symbol(state.data(), false);
    cfg_.bits = c.symbol(state.data(), false);
    if (cfg_.bits == 0) cfg_.bits = 8;
    cfg_.chroma_planes = c.bit(state[0]);
    cfg_.h_shift = c.symbol(state.data(), false);
    cfg_.v_shift = c.symbol(state.data(), false);
    cfg_.alpha = c.bit(state[0]);
    cfg_.h_slices = c.symbol(state.data(), false) + 1;
    cfg_.v_slices = c.symbol(state.data(), false) + 1;
    if (cfg_.h_slices > 64 || cfg_.v_slices > 64) return fail(Status::Decode, "too many FFV1 slices");
    const int table_count = c.symbol(state.data(), false);
    if (table_count <= 0 || table_count > 8) return fail(Status::Decode, "invalid FFV1 quantisation table count");
    cfg_.tables.assign(static_cast<std::size_t>(table_count), {});
    for (auto& t : cfg_.tables) {
      if (!read_quant_tables(c, t)) return fail(Status::Decode, "invalid FFV1 quantisation tables");
    }
    cfg_.initial.assign(cfg_.tables.size(), {});
    for (std::size_t i = 0; i < cfg_.tables.size(); ++i) {
      auto& init = cfg_.initial[i];
      init.assign(static_cast<std::size_t>(cfg_.tables[i].context_count), {});
      for (auto& s : init) s.fill(128);
      if (c.bit(state[0])) {
        std::array<std::array<std::uint8_t, kContextSize>, kContextSize> state2{};
        for (auto& s : state2) s.fill(128);
        for (std::size_t j = 0; j < init.size(); ++j) {
          for (std::size_t k = 0; k < kContextSize; ++k) {
            const int pred = j > 0 ? init[j - 1][k] : 128;
            init[j][k] = static_cast<std::uint8_t>((pred + c.symbol(state2[k].data(), true)) & 0xFF);
          }
        }
      }
    }
    if (cfg_.version > 2) {
      cfg_.ec = c.symbol(state.data(), false) != 0;
      if (cfg_.micro_version > 2) (void)c.symbol(state.data(), false);  // intra
    }
    if (c.overread()) return fail(Status::Decode, "truncated FFV1 configuration record");
    CVT_TRY(check_format());
    have_config_ = true;
    return {};
  }

  Result<void> read_coder(RangeDecoder& c, std::uint8_t* state) {
    cfg_.coder = c.symbol(state, false);
    if (cfg_.coder > 2) return fail(Status::Decode, "unknown FFV1 coder type {}", cfg_.coder);
    std::uint8_t table[256];
    std::memcpy(table, kDefaultTransition, 256);
    if (cfg_.coder == 2) {
      for (int i = 1; i < 256; ++i) {
        const int st = c.symbol(state, true) + default_states_.one[static_cast<std::size_t>(i)];
        if (st < 1 || st > 255) return fail(Status::Decode, "invalid FFV1 state transition table");
        table[i] = static_cast<std::uint8_t>(st);
      }
    }
    cfg_.states.set(table);
    return {};
  }

  Result<void> check_format() {
    if (cfg_.colorspace > 1) return fail(Status::Decode, "unknown FFV1 colour space {}", cfg_.colorspace);
    if (cfg_.bits < 8 || cfg_.bits > 16) return fail(Status::Decode, "unsupported FFV1 bit depth {}", cfg_.bits);
    if (cfg_.colorspace == 0 && (cfg_.h_shift > 1 || cfg_.v_shift > 1 || cfg_.v_shift > cfg_.h_shift)) {
      return fail(Status::Decode, "unsupported FFV1 chroma subsampling");
    }
    if (cfg_.colorspace == 1 && cfg_.bits > 8) return fail(Status::Decode, "FFV1 RGB above 8 bits is not supported");
    return {};
  }

  /// Version 0/1 header at the start of a key frame.
  Result<void> read_v1_header(RangeDecoder& c) {
    std::array<std::uint8_t, kContextSize> state{};
    state.fill(128);
    cfg_.version = c.symbol(state.data(), false);
    if (cfg_.version > 1) return fail(Status::Decode, "invalid FFV1 frame header version {}", cfg_.version);
    CVT_TRY(read_coder(c, state.data()));
    cfg_.colorspace = c.symbol(state.data(), false);
    cfg_.bits = cfg_.version > 0 ? c.symbol(state.data(), false) : 8;
    if (cfg_.bits == 0) cfg_.bits = 8;
    cfg_.chroma_planes = c.bit(state[0]);
    cfg_.h_shift = c.symbol(state.data(), false);
    cfg_.v_shift = c.symbol(state.data(), false);
    cfg_.alpha = c.bit(state[0]);
    cfg_.tables.assign(1, {});
    if (!read_quant_tables(c, cfg_.tables[0])) return fail(Status::Decode, "invalid FFV1 quantisation tables");
    cfg_.initial.assign(1, {});
    cfg_.initial[0].assign(static_cast<std::size_t>(cfg_.tables[0].context_count), {});
    for (auto& s : cfg_.initial[0]) s.fill(128);
    cfg_.h_slices = cfg_.v_slices = 1;
    cfg_.ec = false;
    if (c.overread()) return fail(Status::Decode, "truncated FFV1 frame header");
    CVT_TRY(check_format());
    have_config_ = true;
    return {};
  }

  [[nodiscard]] int plane_count() const { return 1 + 1 + (cfg_.alpha ? 1 : 0); }

  void reset_contexts(Slice& s) {
    if (s.planes.size() != static_cast<std::size_t>(plane_count())) s.planes.assign(static_cast<std::size_t>(plane_count()), {});
    for (PlaneContext& p : s.planes) {
      const auto t = static_cast<std::size_t>(std::min<int>(p.table, static_cast<int>(cfg_.tables.size()) - 1));
      p.states = cfg_.initial[t];
      p.golomb.assign(p.states.size(), GolombState{});
    }
  }

  Result<VideoFrame> decode(std::span<const std::uint8_t> data) {
    if (data.size() < 2) return fail(Status::Decode, "FFV1 frame is too short");
    CVT_TRY(check_dimensions(params_.width, params_.height));
    if (params_.width == 0) return fail(Status::Decode, "FFV1 needs the picture size from the container");
    const int width = static_cast<int>(params_.width);
    const int height = static_cast<int>(params_.height);
    // Locate the slices (version 3: every slice ends with a size trailer).
    RangeDecoder first(data, &default_states_);
    std::uint8_t key_state = 128;
    const bool keyframe = first.bit(key_state);
    if (cfg_.version < 2 || !have_config_) {
      if (keyframe) {
        CVT_TRY(read_v1_header(first));
      } else if (!have_config_) {
        return fail(Status::Decode, "FFV1 stream does not start with a key frame");
      }
    }
    if (!have_config_) return fail(Status::Decode, "FFV1 configuration missing");
    first.set_states(&cfg_.states);  // slice data (and v3 slice headers) use the stream's table
    if (!keyframe && !had_keyframe_) return fail(Status::Decode, "FFV1 stream does not start with a key frame");
    if (keyframe) had_keyframe_ = true;

    std::vector<std::pair<std::size_t, std::size_t>> ranges;  // [begin, end) of each slice
    const std::size_t trailer = cfg_.version > 2 ? 3 + (cfg_.ec ? 5 : 0) : 0;
    if (cfg_.version > 2) {
      std::size_t end = data.size();
      while (end > 0) {
        if (end < trailer) return fail(Status::Decode, "damaged FFV1 slice trailer");
        const std::size_t size = static_cast<std::size_t>(data[end - trailer]) << 16 | static_cast<std::size_t>(data[end - trailer + 1]) << 8 |
                                 data[end - trailer + 2];
        if (size + trailer > end) return fail(Status::Decode, "damaged FFV1 slice size");
        if (cfg_.ec && crc32_msb(data.subspan(end - trailer - size, size + trailer)) != 0) {
          return fail(Status::Decode, "FFV1 slice CRC mismatch");
        }
        ranges.insert(ranges.begin(), {end - trailer - size, end - trailer});
        end -= size + trailer;
        if (ranges.size() > 4096) return fail(Status::Decode, "too many FFV1 slices");
      }
    } else {
      ranges.push_back({0, data.size()});
    }
    if (slices_.size() != ranges.size()) slices_.assign(ranges.size(), {});

    // Output frame layout.
    PixelFormat format;
    const bool wide = cfg_.bits > 8;
    if (cfg_.colorspace == 1) format = PixelFormat::RGBA8;
    else if (cfg_.h_shift == 0) format = wide ? PixelFormat::I444P10 : PixelFormat::I444;
    else if (cfg_.v_shift == 0) format = wide ? PixelFormat::I422P10 : PixelFormat::I422;
    else format = wide ? PixelFormat::I420P10 : PixelFormat::I420;
    VideoFrame frame = VideoFrame::allocate(format, params_.width, params_.height);
    frame.color = {params_.height >= 720 ? ir::ColorMatrix::BT709 : ir::ColorMatrix::BT601, ir::ColorRange::Limited, ir::ChromaSiting::Left};
    if (!cfg_.chroma_planes && format != PixelFormat::RGBA8) {
      for (std::size_t p = 1; p < 3; ++p) {
        auto& d = frame.planes[p].data;
        if (wide) {
          for (std::size_t i = 0; i + 1 < d.size(); i += 2) d[i] = 0, d[i + 1] = 2;  // 512
        } else {
          std::fill(d.begin(), d.end(), 128);
        }
      }
    }

    for (std::size_t i = 0; i < ranges.size(); ++i) {
      // The range decoder may read into the slice trailer, as the reference decoder does.
      const auto chunk = data.subspan(ranges[i].first, ranges[i].second + trailer - ranges[i].first);
      Slice& s = slices_[i];
      CVT_TRY(decode_slice(i == 0 ? first : RangeDecoder(chunk, &cfg_.states), chunk, i == 0, s, keyframe, frame, width, height, i, ranges.size()));
    }
    return frame;
  }

  Result<void> decode_slice(RangeDecoder c, std::span<const std::uint8_t> chunk, bool first_slice, Slice& s, bool keyframe,
                            VideoFrame& frame, int width, int height, std::size_t index, std::size_t count) {
    (void)first_slice;
    if (cfg_.version > 2) {
      std::array<std::uint8_t, kContextSize> state{};
      state.fill(128);
      const int sx = c.symbol(state.data(), false);
      const int sy = c.symbol(state.data(), false);
      const int sw = c.symbol(state.data(), false) + 1;
      const int sh = c.symbol(state.data(), false) + 1;
      if (sx < 0 || sy < 0 || sw <= 0 || sh <= 0 || sx > cfg_.h_slices - sw || sy > cfg_.v_slices - sh) {
        return fail(Status::Decode, "invalid FFV1 slice position");
      }
      s.x = width * sx / cfg_.h_slices;
      s.y = height * sy / cfg_.v_slices;
      s.w = width * (sx + sw) / cfg_.h_slices - s.x;
      s.h = height * (sy + sh) / cfg_.v_slices - s.y;
      std::vector<int> tables(static_cast<std::size_t>(plane_count()));
      for (int& t : tables) {
        t = c.symbol(state.data(), false);
        if (t < 0 || t >= static_cast<int>(cfg_.tables.size())) return fail(Status::Decode, "invalid FFV1 quantisation table index");
      }
      (void)c.symbol(state.data(), false);  // picture structure
      (void)c.symbol(state.data(), false);  // sample aspect ratio
      (void)c.symbol(state.data(), false);
      if (keyframe || s.planes.size() != tables.size()) {
        s.planes.assign(tables.size(), {});
        for (std::size_t p = 0; p < tables.size(); ++p) s.planes[p].table = tables[p];
        reset_contexts(s);
      }
    } else {
      // Versions 0/1: one slice covering the picture.
      s.x = 0;
      s.y = 0;
      s.w = width;
      s.h = height;
      if (keyframe || s.planes.empty()) {
        s.planes.assign(static_cast<std::size_t>(plane_count()), {});
        reset_contexts(s);
      }
      (void)index;
      (void)count;
    }
    if (s.w <= 0 || s.h <= 0) return fail(Status::Decode, "empty FFV1 slice");
    // Golomb-Rice data follows the range coded header.
    BitReader golomb;
    std::vector<std::uint8_t> none;
    if (cfg_.coder == 0) {
      if (cfg_.version == 3 && cfg_.micro_version > 1) {
        std::uint8_t st = 129;
        (void)c.bit(st);
      }
      const std::size_t start = c.position() > 0 ? c.position() - 1 : 0;
      golomb = BitReader(chunk.subspan(std::min(start, chunk.size())));
    }
    const bool rgb = cfg_.colorspace == 1;
    if (rgb) return decode_rgb(c, golomb, s, frame);
    // YCbCr planes.
    const int cw = (s.w + (1 << cfg_.h_shift) - 1) >> cfg_.h_shift;
    const int ch = (s.h + (1 << cfg_.v_shift) - 1) >> cfg_.v_shift;
    const int cx = s.x >> cfg_.h_shift;
    const int cy = s.y >> cfg_.v_shift;
    CVT_TRY(decode_plane(c, golomb, s, s.planes[0], frame, 0, s.x, s.y, s.w, s.h));
    if (cfg_.chroma_planes) {
      CVT_TRY(decode_plane(c, golomb, s, s.planes[1], frame, 1, cx, cy, cw, ch));
      CVT_TRY(decode_plane(c, golomb, s, s.planes[1], frame, 2, cx, cy, cw, ch));
    }
    if (cfg_.alpha) {
      // Alpha has no IR plane in YCbCr output: decoded and discarded.
      Lines lines;
      lines.reset(s.w);
      s.run_index = 0;
      for (int y = 0; y < s.h; ++y) {
        lines.next();
        CVT_TRY(decode_line(c, golomb, s, s.planes[2], lines, cfg_.bits));
      }
    }
    return {};
  }

  Result<void> decode_plane(RangeDecoder& c, BitReader& golomb, Slice& s, PlaneContext& p, VideoFrame& frame, int plane,
                            int x0, int y0, int w, int h) {
    Lines lines;
    lines.reset(w);
    s.run_index = 0;
    const bool wide = cfg_.bits > 8;
    auto& out = frame.planes[static_cast<std::size_t>(plane)];
    const int shift_out = wide ? std::max(0, cfg_.bits - 10) : 0;
    const int shift_up = wide ? std::max(0, 10 - cfg_.bits) : 0;
    for (int y = 0; y < h; ++y) {
      lines.next();
      CVT_TRY(decode_line(c, golomb, s, p, lines, cfg_.bits));
      const std::size_t row = static_cast<std::size_t>(y0 + y);
      for (int x = 0; x < w; ++x) {
        const int v = lines.cur[x];
        const std::size_t col = static_cast<std::size_t>(x0 + x);
        if (wide) {
          const int v10 = (v >> shift_out) << shift_up;
          out.data[row * out.stride + col * 2] = static_cast<std::uint8_t>(v10 & 0xFF);
          out.data[row * out.stride + col * 2 + 1] = static_cast<std::uint8_t>(v10 >> 8);
        } else {
          out.data[row * out.stride + col] = static_cast<std::uint8_t>(v);
        }
      }
    }
    return {};
  }

  Result<void> decode_rgb(RangeDecoder& c, BitReader& golomb, Slice& s, VideoFrame& frame) {
    const int planes = 3 + (cfg_.alpha ? 1 : 0);
    std::array<Lines, 4> lines;
    for (int p = 0; p < planes; ++p) lines[static_cast<std::size_t>(p)].reset(s.w);
    s.run_index = 0;
    const int offset = 1 << cfg_.bits;
    for (int y = 0; y < s.h; ++y) {
      for (int p = 0; p < planes; ++p) {
        lines[static_cast<std::size_t>(p)].next();
        PlaneContext& ctx = s.planes[static_cast<std::size_t>((p + 1) / 2)];
        CVT_TRY(decode_line(c, golomb, s, ctx, lines[static_cast<std::size_t>(p)], cfg_.bits + 1));
      }
      std::uint8_t* row = frame.planes[0].data.data() + static_cast<std::size_t>(s.y + y) * frame.planes[0].stride;
      for (int x = 0; x < s.w; ++x) {
        int g = lines[0].cur[x];
        int b = lines[1].cur[x] - offset;
        int r = lines[2].cur[x] - offset;
        const int a = cfg_.alpha ? lines[3].cur[x] : 255;
        g -= (b + r) >> 2;
        b += g;
        r += g;
        std::uint8_t* px = row + static_cast<std::size_t>(s.x + x) * 4;
        px[0] = static_cast<std::uint8_t>(r);
        px[1] = static_cast<std::uint8_t>(g);
        px[2] = static_cast<std::uint8_t>(b);
        px[3] = static_cast<std::uint8_t>(a);
      }
    }
    return {};
  }

  /// Decodes one line into lines.cur (lines.top is the line above).
  Result<void> decode_line(RangeDecoder& c, BitReader& golomb, Slice& s, PlaneContext& p, Lines& lines, int bits) {
    const QuantTables& qt = cfg_.tables[static_cast<std::size_t>(p.table)];
    int* cur = lines.cur;
    const int* top = lines.top;
    const int w = lines.width;
    const int mask = (1 << bits) - 1;
    int run_mode = 0;
    int run_count = 0;
    int run_index = s.run_index;
    for (int x = 0; x < w; ++x) {
      int ctx = context_of(qt, cur, top, x);
      bool negate = false;
      if (ctx < 0) {
        ctx = -ctx;
        negate = true;
      }
      if (static_cast<std::size_t>(ctx) >= p.states.size()) return fail(Status::Decode, "invalid FFV1 context");
      int diff;
      if (cfg_.coder != 0) {
        diff = c.symbol(p.states[static_cast<std::size_t>(ctx)].data(), true);
        if (c.overread()) return fail(Status::Decode, "truncated FFV1 slice");
      } else {
        if (ctx == 0 && run_mode == 0) run_mode = 1;
        if (run_mode != 0) {
          if (run_count == 0 && run_mode == 1) {
            if (golomb.bit()) {
              run_count = 1 << kLog2Run[run_index];
              if (x + run_count <= w) run_index = std::min(run_index + 1, 40);
            } else {
              run_count = kLog2Run[run_index] != 0 ? static_cast<int>(golomb.bits(kLog2Run[run_index])) : 0;
              if (run_index > 0) --run_index;
              run_mode = 2;
            }
          }
          --run_count;
          if (run_count < 0) {
            run_mode = 0;
            run_count = 0;
            diff = golomb_symbol(golomb, p.golomb[static_cast<std::size_t>(ctx)], bits);
            if (diff >= 0) ++diff;
          } else {
            diff = 0;
          }
        } else {
          diff = golomb_symbol(golomb, p.golomb[static_cast<std::size_t>(ctx)], bits);
        }
        if (golomb.overrun()) return fail(Status::Decode, "truncated FFV1 slice");
      }
      if (negate) diff = -diff;
      const int pred = mid_pred(cur[x - 1], top[x], cur[x - 1] + top[x] - top[x - 1]);
      cur[x] = (pred + diff) & mask;
    }
    s.run_index = run_index;
    return {};
  }

  static int golomb_symbol(BitReader& br, GolombState& st, int bits) {
    int k = 0;
    for (int i = st.count; i < st.error_sum; i += i) {
      if (++k > 16) break;
    }
    // Signed Rice code with an escape after 12 zero bits.
    int zeros = 0;
    while (zeros < 12 && br.peek(1) == 0) {
      br.skip(1);
      ++zeros;
      if (br.overrun()) return 0;
    }
    int v;
    if (zeros < 12) {
      br.skip(1);
      v = (zeros << k) + static_cast<int>(br.bits(k));
    } else {
      v = static_cast<int>(br.bits(bits)) + 11;
    }
    v = (v >> 1) ^ -(v & 1);
    v ^= (2 * st.drift + st.count) >> 31;
    // fold(v + bias, bits): sign-extend to `bits`
    int folded = v + st.bias;
    if (bits == 8) {
      folded = static_cast<std::int8_t>(folded & 0xFF);
    } else {
      const int m = 1 << bits;
      folded &= m - 1;
      if (folded >= m / 2) folded -= m;
    }
    st.update(v);
    return folded;
  }

  CodecParams params_;
  States default_states_;
  Config cfg_;
  bool have_config_ = false;
  bool had_keyframe_ = false;
  std::vector<Slice> slices_;
  std::optional<VideoFrame> last_;
  std::vector<VideoFrame> output_;
};

// ---- encoder ------------------------------------------------------------------------------------

/// Context quantiser for 8-bit (and scaled for 10-bit) differences.
[[nodiscard]] int quantise_difference(int d, int bits) {
  const int a = std::abs(d) >> (bits - 8);
  int q;
  if (a == 0) q = 0;
  else if (a <= 2) q = 1;
  else if (a <= 6) q = 2;
  else if (a <= 14) q = 3;
  else if (a <= 30) q = 4;
  else q = 5;
  return d < 0 ? -q : q;
}

class Ffv1Encoder final : public SimpleEncoder {
 protected:
  Result<void> on_configure() override {
    states_.set(kDefaultTransition);
    switch (params_.pixel_format) {
      case PixelFormat::RGBA8: format_ = PixelFormat::RGBA8; break;
      case PixelFormat::I422: format_ = PixelFormat::I422; break;
      case PixelFormat::I444: format_ = PixelFormat::I444; break;
      case PixelFormat::I420P10: format_ = PixelFormat::I420P10; break;
      case PixelFormat::I422P10: format_ = PixelFormat::I422P10; break;
      case PixelFormat::I444P10: format_ = PixelFormat::I444P10; break;
      default: format_ = PixelFormat::I420; break;
    }
    rgb_ = format_ == PixelFormat::RGBA8;
    bits_ = ir::bit_depth(format_);
    // Quantisation table: 11 levels over three neighbour differences.
    for (int k = 0; k < 3; ++k) {
      for (int i = 0; i < 256; ++i) {
        const int d = i < 128 ? i : i - 256;
        const int scale = k == 0 ? 1 : (k == 1 ? 11 : 121);
        tables_.q[static_cast<std::size_t>(k)][static_cast<std::size_t>(i)] = static_cast<std::int16_t>(scale * quantise_difference(d, 8));
      }
    }
    tables_.context_count = (11 * 11 * 11 + 1) / 2;
    extradata_ = build_config();
    return {};
  }

  Bytes extradata() const override { return to_cvt_bytes(extradata_); }

  Result<Bytes> encode(const VideoFrame& frame, bool& keyframe) override {
    const std::uint64_t interval = params_.keyframe_interval > 0 ? params_.keyframe_interval : 1;
    keyframe = frame_index_ % interval == 0;
    ColorInfo color = params_.color;
    CVT_TRY_ASSIGN(const VideoFrame f, to_format(frame, format_, color));
    if (keyframe || planes_.empty()) reset_contexts();
    RangeEncoder c(&states_);
    std::uint8_t key_state = 128;
    c.bit(key_state, keyframe);
    // Slice header: one slice covering the picture.
    std::array<std::uint8_t, kContextSize> state{};
    state.fill(128);
    c.symbol(state.data(), 0, false);
    c.symbol(state.data(), 0, false);
    c.symbol(state.data(), 0, false);
    c.symbol(state.data(), 0, false);
    for (int p = 0; p < plane_count(); ++p) c.symbol(state.data(), 0, false);  // quantisation table 0
    c.symbol(state.data(), 3, false);  // progressive
    c.symbol(state.data(), 0, false);  // sample aspect ratio unknown
    c.symbol(state.data(), 0, false);
    const int w = static_cast<int>(params_.width);
    const int h = static_cast<int>(params_.height);
    if (rgb_) {
      encode_rgb(c, f, w, h);
    } else {
      const int hs = ir::chroma_shift_x(format_);
      const int vs = ir::chroma_shift_y(format_);
      encode_plane(c, f, 0, planes_[0], w, h);
      encode_plane(c, f, 1, planes_[1], (w + (1 << hs) - 1) >> hs, (h + (1 << vs) - 1) >> vs);
      encode_plane(c, f, 2, planes_[1], (w + (1 << hs) - 1) >> hs, (h + (1 << vs) - 1) >> vs);
    }
    std::vector<std::uint8_t> slice = c.finish();
    const std::size_t size = slice.size();
    slice.push_back(static_cast<std::uint8_t>(size >> 16));
    slice.push_back(static_cast<std::uint8_t>(size >> 8));
    slice.push_back(static_cast<std::uint8_t>(size));
    slice.push_back(0);  // error status
    const std::uint32_t crc = crc32_msb(slice);
    for (int k = 3; k >= 0; --k) slice.push_back(static_cast<std::uint8_t>(crc >> (8 * k)));
    return to_cvt_bytes(slice);
  }

 private:
  [[nodiscard]] int plane_count() const { return rgb_ ? 2 + (alpha_ ? 1 : 0) : 2; }

  std::vector<std::uint8_t> build_config() {
    alpha_ = false;  // RGB alpha is always coded when the format is RGBA8 (see below)
    if (rgb_) alpha_ = true;
    RangeEncoder c(&states_);
    std::array<std::uint8_t, kContextSize> state{};
    state.fill(128);
    c.symbol(state.data(), 3, false);  // version
    c.symbol(state.data(), 4, false);  // micro version
    c.symbol(state.data(), 1, false);  // range coder, default table
    c.symbol(state.data(), rgb_ ? 1 : 0, false);
    c.symbol(state.data(), bits_, false);
    c.bit(state[0], true);  // chroma planes
    c.symbol(state.data(), rgb_ ? 0 : ir::chroma_shift_x(format_), false);
    c.symbol(state.data(), rgb_ ? 0 : ir::chroma_shift_y(format_), false);
    c.bit(state[0], alpha_);
    c.symbol(state.data(), 0, false);  // one horizontal slice
    c.symbol(state.data(), 0, false);  // one vertical slice
    c.symbol(state.data(), 1, false);  // one quantisation table set
    for (int k = 0; k < 5; ++k) {
      std::array<std::uint8_t, kContextSize> qs{};
      qs.fill(128);
      const auto& table = tables_.q[static_cast<std::size_t>(k)];
      // Run lengths of equal values over indices 0..127.
      int i = 0;
      while (i < 128) {
        int run = 1;
        while (i + run < 128 && table[static_cast<std::size_t>(i + run)] == table[static_cast<std::size_t>(i)]) ++run;
        c.symbol(qs.data(), run - 1, false);
        i += run;
      }
    }
    c.bit(state[0], false);  // no initial states
    c.symbol(state.data(), 1, false);  // error correction (slice CRCs)
    c.symbol(state.data(), 0, false);  // not intra-only
    std::vector<std::uint8_t> out = c.finish();
    const std::uint32_t crc = crc32_msb(out);
    for (int k = 3; k >= 0; --k) out.push_back(static_cast<std::uint8_t>(crc >> (8 * k)));
    return out;
  }

  void reset_contexts() {
    planes_.assign(static_cast<std::size_t>(plane_count()), {});
    for (auto& p : planes_) {
      p.states.assign(static_cast<std::size_t>(tables_.context_count), {});
      for (auto& s : p.states) s.fill(128);
    }
  }

  void code_line(RangeEncoder& c, PlaneContext& p, Lines& lines, const std::vector<int>& values, int bits) {
    int* cur = lines.cur;
    const int* top = lines.top;
    const int mask = (1 << bits) - 1;
    const int half = 1 << (bits - 1);
    for (int x = 0; x < lines.width; ++x) {
      int ctx = context_of(tables_, cur, top, x);
      const int pred = mid_pred(cur[x - 1], top[x], cur[x - 1] + top[x] - top[x - 1]);
      int diff = (values[static_cast<std::size_t>(x)] - pred) & mask;
      if (diff >= half) diff -= 1 << bits;
      if (ctx < 0) {
        ctx = -ctx;
        diff = -diff;
      }
      c.symbol(p.states[static_cast<std::size_t>(ctx)].data(), diff, true);
      cur[x] = values[static_cast<std::size_t>(x)];
    }
  }

  void encode_plane(RangeEncoder& c, const VideoFrame& f, int plane, PlaneContext& p, int w, int h) {
    Lines lines;
    lines.reset(w);
    std::vector<int> values(static_cast<std::size_t>(w));
    const bool wide = bits_ > 8;
    const auto& src = f.planes[static_cast<std::size_t>(plane)];
    for (int y = 0; y < h; ++y) {
      lines.next();
      const std::uint8_t* row = src.data.data() + static_cast<std::size_t>(y) * src.stride;
      for (int x = 0; x < w; ++x) {
        values[static_cast<std::size_t>(x)] = wide ? (row[x * 2] | row[x * 2 + 1] << 8) & 0x3FF : row[x];
      }
      code_line(c, p, lines, values, bits_);
    }
  }

  void encode_rgb(RangeEncoder& c, const VideoFrame& f, int w, int h) {
    const int planes = alpha_ ? 4 : 3;
    std::array<Lines, 4> lines;
    for (int p = 0; p < planes; ++p) lines[static_cast<std::size_t>(p)].reset(w);
    std::array<std::vector<int>, 4> values;
    for (auto& v : values) v.assign(static_cast<std::size_t>(w), 0);
    const int offset = 1 << bits_;
    for (int y = 0; y < h; ++y) {
      const std::uint8_t* row = f.planes[0].data.data() + static_cast<std::size_t>(y) * f.planes[0].stride;
      for (int x = 0; x < w; ++x) {
        const int r = row[x * 4];
        const int g = row[x * 4 + 1];
        const int b = row[x * 4 + 2];
        const int cb = b - g;
        const int cr = r - g;
        values[0][static_cast<std::size_t>(x)] = g + ((cb + cr) >> 2);
        values[1][static_cast<std::size_t>(x)] = cb + offset;
        values[2][static_cast<std::size_t>(x)] = cr + offset;
        values[3][static_cast<std::size_t>(x)] = row[x * 4 + 3];
      }
      for (int p = 0; p < planes; ++p) {
        lines[static_cast<std::size_t>(p)].next();
        code_line(c, planes_[static_cast<std::size_t>((p + 1) / 2)], lines[static_cast<std::size_t>(p)], values[static_cast<std::size_t>(p)], bits_ + 1);
      }
    }
  }

  States states_;
  PixelFormat format_ = PixelFormat::I420;
  bool rgb_ = false;
  bool alpha_ = false;
  int bits_ = 8;
  QuantTables tables_;
  std::vector<PlaneContext> planes_;
  std::vector<std::uint8_t> extradata_;
};

}  // namespace

std::unique_ptr<codec::VideoDecoder> make_ffv1_decoder() { return std::make_unique<Ffv1Decoder>(); }
std::unique_ptr<codec::VideoEncoder> make_ffv1_encoder() { return std::make_unique<Ffv1Encoder>(); }

}  // namespace cvt::legacy::lossless
