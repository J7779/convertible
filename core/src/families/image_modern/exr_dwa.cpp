// Convertible core - image_modern family: OpenEXR DWAA/DWAB decompression.
//
// DWA splits the channels of a chunk into three groups by name: colour
// channels are coded lossily (8x8 DCT on perceptually encoded half values,
// RGB triplets first converted to Y'CbCr), alpha is RLE + zlib, everything
// else zlib. The chunk starts with eleven 64-bit counters, then (version 2)
// the classification rules the writer used, then the four packed streams.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "families/image_modern/exr_internal.hpp"

namespace cvt::image_modern::exr {

namespace {

enum class Scheme : std::uint8_t { Unknown = 0, LossyDct = 1, Rle = 2 };

struct Rule {
  std::string suffix;
  Scheme scheme = Scheme::Unknown;
  PixelType type = PixelType::Half;
  int csc = -1;
  bool case_insensitive = false;
};

std::vector<Rule> legacy_rules() {
  std::vector<Rule> rules;
  const auto add = [&](std::string_view suffix, Scheme scheme, int csc, std::initializer_list<PixelType> types) {
    for (const PixelType type : types) rules.push_back({std::string(suffix), scheme, type, csc, true});
  };
  const auto hf = {PixelType::Half, PixelType::Float};
  for (const char* s : {"r", "red"}) add(s, Scheme::LossyDct, 0, hf);
  for (const char* s : {"g", "grn", "green"}) add(s, Scheme::LossyDct, 1, hf);
  for (const char* s : {"b", "blu", "blue"}) add(s, Scheme::LossyDct, 2, hf);
  for (const char* s : {"y", "by", "ry"}) add(s, Scheme::LossyDct, -1, hf);
  add("a", Scheme::Rle, -1, {PixelType::Uint, PixelType::Half, PixelType::Float});
  return rules;
}

bool ascii_iequal(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}

bool rule_matches(const Rule& rule, std::string_view suffix, PixelType type) noexcept {
  if (rule.type != type) return false;
  return rule.case_insensitive ? ascii_iequal(suffix, rule.suffix) : suffix == rule.suffix;
}

Result<std::vector<Rule>> read_rules(ByteView data) {
  std::vector<Rule> rules;
  std::size_t pos = 0;
  while (pos < data.size()) {
    const std::size_t remaining = data.size() - pos;
    if (remaining <= 3) return fail(Status::Decode, "corrupt DWA channel rules");
    const std::string_view text = as_chars(data.subspan(pos));
    const std::size_t end = text.find('\0');
    if (end == std::string_view::npos || end > 128 || end + 3 > remaining) {
      return fail(Status::Decode, "corrupt DWA channel rules");
    }
    Rule rule;
    rule.suffix = std::string(text.substr(0, end));
    const auto value = std::to_integer<std::uint8_t>(data[pos + end + 1]);
    const auto type = std::to_integer<std::uint8_t>(data[pos + end + 2]);
    pos += end + 3;
    rule.csc = static_cast<int>(value >> 4) - 1;
    const int scheme = (value >> 2) & 3;
    if (rule.csc < -1 || rule.csc >= 3 || scheme > 2 || type > 2) return fail(Status::Decode, "corrupt DWA channel rules");
    rule.scheme = static_cast<Scheme>(scheme);
    rule.case_insensitive = (value & 1u) != 0;
    rule.type = static_cast<PixelType>(type);
    rules.push_back(std::move(rule));
  }
  return rules;
}

/// Nonlinear (gamma 2.2 below 1, log above) half -> linear half.
const std::vector<std::uint16_t>& to_linear_table() {
  static const std::vector<std::uint16_t> table = [] {
    std::vector<std::uint16_t> t(65536);
    for (std::uint32_t i = 0; i < 65536; ++i) {
      const auto x = static_cast<std::uint16_t>(i);
      if (x == 0 || (x & 0x7C00u) == 0x7C00u) {
        t[i] = 0;
        continue;
      }
      float f = half_to_float(x);
      const float sign = f < 0.0f ? -1.0f : 1.0f;
      f = std::fabs(f);
      const float px = f <= 1.0f ? f : 9.02501329156f;
      const float py = f <= 1.0f ? 2.2f : f - 1.0f;
      t[i] = float_to_half(sign * std::pow(px, py));
    }
    return t;
  }();
  return table;
}

constexpr std::array<int, 64> kZigzag = {0,  1,  5,  6,  14, 15, 27, 28, 2,  4,  7,  13, 16, 26, 29, 42,
                                         3,  8,  12, 17, 25, 30, 41, 43, 9,  11, 18, 24, 31, 40, 44, 53,
                                         10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60,
                                         21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63};

struct DctConstants {
  float a, b, c, d, e, f, g;
};

const DctConstants& dct_constants() {
  static const DctConstants k = {
      .5f * std::cos(3.14159f / 4.0f),        .5f * std::cos(3.14159f / 16.0f),
      .5f * std::cos(3.14159f / 8.0f),        .5f * std::cos(3.f * 3.14159f / 16.0f),
      .5f * std::cos(5.f * 3.14159f / 16.0f), .5f * std::cos(3.f * 3.14159f / 8.0f),
      .5f * std::cos(7.f * 3.14159f / 16.0f),
  };
  return k;
}

/// Inverse 8x8 DCT in place (the OpenEXR scalar formulation); the last
/// `zeroed_rows` rows are known to be zero.
void inverse_dct(float* data, int zeroed_rows) {
  const DctConstants& k = dct_constants();
  float alpha[4], beta[4], theta[4], gamma[4];
  for (int row = 0; row < 8 - zeroed_rows; ++row) {
    float* r = data + row * 8;
    alpha[0] = k.c * r[2];
    alpha[1] = k.f * r[2];
    alpha[2] = k.c * r[6];
    alpha[3] = k.f * r[6];
    beta[0] = k.b * r[1] + k.d * r[3] + k.e * r[5] + k.g * r[7];
    beta[1] = k.d * r[1] - k.g * r[3] - k.b * r[5] - k.e * r[7];
    beta[2] = k.e * r[1] - k.b * r[3] + k.g * r[5] + k.d * r[7];
    beta[3] = k.g * r[1] - k.e * r[3] + k.d * r[5] - k.b * r[7];
    theta[0] = k.a * (r[0] + r[4]);
    theta[3] = k.a * (r[0] - r[4]);
    theta[1] = alpha[0] + alpha[3];
    theta[2] = alpha[1] - alpha[2];
    gamma[0] = theta[0] + theta[1];
    gamma[1] = theta[3] + theta[2];
    gamma[2] = theta[3] - theta[2];
    gamma[3] = theta[0] - theta[1];
    r[0] = gamma[0] + beta[0];
    r[1] = gamma[1] + beta[1];
    r[2] = gamma[2] + beta[2];
    r[3] = gamma[3] + beta[3];
    r[4] = gamma[3] - beta[3];
    r[5] = gamma[2] - beta[2];
    r[6] = gamma[1] - beta[1];
    r[7] = gamma[0] - beta[0];
  }
  for (int column = 0; column < 8; ++column) {
    float* c = data + column;
    alpha[0] = k.c * c[16];
    alpha[1] = k.f * c[16];
    alpha[2] = k.c * c[48];
    alpha[3] = k.f * c[48];
    beta[0] = k.b * c[8] + k.d * c[24] + k.e * c[40] + k.g * c[56];
    beta[1] = k.d * c[8] - k.g * c[24] - k.b * c[40] - k.e * c[56];
    beta[2] = k.e * c[8] - k.b * c[24] + k.g * c[40] + k.d * c[56];
    beta[3] = k.g * c[8] - k.e * c[24] + k.d * c[40] - k.b * c[56];
    theta[0] = k.a * (c[0] + c[32]);
    theta[3] = k.a * (c[0] - c[32]);
    theta[1] = alpha[0] + alpha[3];
    theta[2] = alpha[1] - alpha[2];
    gamma[0] = theta[0] + theta[1];
    gamma[1] = theta[3] + theta[2];
    gamma[2] = theta[3] - theta[2];
    gamma[3] = theta[0] - theta[1];
    c[0] = gamma[0] + beta[0];
    c[8] = gamma[1] + beta[1];
    c[16] = gamma[2] + beta[2];
    c[24] = gamma[3] + beta[3];
    c[32] = gamma[3] - beta[3];
    c[40] = gamma[2] - beta[2];
    c[48] = gamma[1] - beta[1];
    c[56] = gamma[0] - beta[0];
  }
}

/// Rows (byte offsets into the output chunk) of one channel.
struct Target {
  const ChunkChannel* channel = nullptr;
  std::vector<std::size_t> rows;
};

struct DctStreams {
  const std::vector<std::uint16_t>& ac;
  std::size_t ac_pos = 0;
  const std::vector<std::uint16_t>& dc;
  std::size_t dc_pos = 0;
};

/// Decode one LOSSY_DCT set (1 channel, or 3 colour-converted channels).
Result<void> decode_dct(std::span<Target* const> targets, DctStreams& streams, const std::uint16_t* to_linear,
                        Bytes& out) {
  const std::size_t comps = targets.size();
  const std::int32_t width = targets[0]->channel->width;
  const std::int32_t height = targets[0]->channel->height;
  const std::size_t blocks_x = (static_cast<std::size_t>(width) + 7) / 8;
  const std::size_t blocks_y = (static_cast<std::size_t>(height) + 7) / 8;
  const std::size_t blocks = blocks_x * blocks_y;
  if (streams.dc.size() - streams.dc_pos < comps * blocks) return fail(Status::Decode, "DWA DC data is truncated");
  const std::size_t dc_base = streams.dc_pos;
  streams.dc_pos += comps * blocks;

  std::vector<std::uint16_t> row_block(comps * blocks_x * 64);
  std::array<std::array<float, 64>, 3> dct{};
  std::array<std::uint16_t, 64> zig{};
  std::size_t dc_index = 0;
  for (std::size_t by = 0; by < blocks_y; ++by) {
    for (std::size_t bx = 0; bx < blocks_x; ++bx, ++dc_index) {
      bool constant = true;
      for (std::size_t comp = 0; comp < comps; ++comp) {
        zig.fill(0);
        zig[0] = streams.dc[dc_base + comp * blocks + dc_index];
        int last_non_zero = 0;
        int index = 1;
        while (index < 64) {
          if (streams.ac_pos >= streams.ac.size()) return fail(Status::Decode, "DWA AC data is truncated");
          const std::uint16_t value = streams.ac[streams.ac_pos++];
          if ((value & 0xFF00u) == 0xFF00u) {
            const int count = value & 0xFF;
            index += count == 0 ? 64 : count;
          } else {
            last_non_zero = index;
            zig[static_cast<std::size_t>(index)] = value;
            ++index;
          }
        }
        float* data = dct[comp].data();
        if (last_non_zero == 0) {
          const float v = half_to_float(zig[0]) * 3.535536e-01f * 3.535536e-01f;
          std::fill_n(data, 64, v);
          continue;
        }
        constant = false;
        for (std::size_t i = 0; i < 64; ++i) data[i] = half_to_float(zig[static_cast<std::size_t>(kZigzag[i])]);
        const int zeroed = last_non_zero < 2    ? 7
                           : last_non_zero < 3  ? 6
                           : last_non_zero < 9  ? 5
                           : last_non_zero < 10 ? 4
                           : last_non_zero < 20 ? 3
                           : last_non_zero < 21 ? 2
                           : last_non_zero < 35 ? 1
                                                : 0;
        inverse_dct(data, zeroed);
      }
      if (comps == 3) {
        const std::size_t n = constant ? 1 : 64;
        for (std::size_t i = 0; i < n; ++i) {
          const float y = dct[0][i], cb = dct[1][i], cr = dct[2][i];
          dct[0][i] = y + 1.5747f * cr;
          dct[1][i] = y - 0.1873f * cb - 0.4682f * cr;
          dct[2][i] = y + 1.8556f * cb;
        }
      }
      for (std::size_t comp = 0; comp < comps; ++comp) {
        std::uint16_t* dst = row_block.data() + (comp * blocks_x + bx) * 64;
        if (constant) {
          std::fill_n(dst, 64, float_to_half(dct[comp][0]));
        } else {
          for (std::size_t i = 0; i < 64; ++i) dst[i] = float_to_half(dct[comp][i]);
        }
      }
    }
    // Unblock this row of blocks into the channel rows.
    const std::size_t max_y = std::min<std::size_t>(8, static_cast<std::size_t>(height) - by * 8);
    for (std::size_t comp = 0; comp < comps; ++comp) {
      const Target& target = *targets[comp];
      const bool is_float = target.channel->type == PixelType::Float;
      for (std::size_t yy = 0; yy < max_y; ++yy) {
        std::byte* row = out.data() + target.rows[by * 8 + yy];
        for (std::size_t x = 0; x < static_cast<std::size_t>(width); ++x) {
          std::uint16_t h = row_block[(comp * blocks_x + x / 8) * 64 + yy * 8 + (x & 7)];
          if (to_linear != nullptr) h = to_linear[h];
          if (is_float) {
            store32(row + x * 4, std::bit_cast<std::uint32_t>(half_to_float(h)));
          } else {
            store16(row + x * 2, h);
          }
        }
      }
    }
  }
  return {};
}

}  // namespace

Result<Bytes> dwa_decompress(const ChunkLayout& layout, ByteView packed) {
  constexpr std::size_t kCounters = 11;
  constexpr std::size_t kHeader = kCounters * 8;
  if (packed.size() < kHeader) return fail(Status::Decode, "DWA data is truncated");
  std::uint64_t counter[kCounters];
  for (std::size_t i = 0; i < kCounters; ++i) counter[i] = load64(packed.data() + i * 8);
  const std::uint64_t version = counter[0];
  const std::uint64_t unknown_raw = counter[1];
  const std::uint64_t unknown_packed = counter[2];
  const std::uint64_t ac_packed = counter[3];
  const std::uint64_t dc_packed = counter[4];
  const std::uint64_t rle_packed = counter[5];
  const std::uint64_t rle_unpacked = counter[6];
  const std::uint64_t rle_raw = counter[7];
  const std::uint64_t ac_count = counter[8];
  const std::uint64_t dc_count = counter[9];
  const std::uint64_t ac_compression = counter[10];
  if (version > 2) return fail(Status::Decode, "unsupported DWA version {}", version);
  const std::uint64_t size = packed.size();
  for (const std::uint64_t v : {unknown_packed, ac_packed, dc_packed, rle_packed}) {
    if (v > size) return fail(Status::Decode, "corrupt DWA sizes");
  }
  if (unknown_raw > layout.raw_size || rle_raw > layout.raw_size || rle_unpacked > 2 * layout.raw_size + 64) {
    return fail(Status::Decode, "corrupt DWA sizes");
  }

  std::size_t pos = kHeader;
  std::vector<Rule> rules;
  if (version < 2) {
    rules = legacy_rules();
  } else {
    if (size - pos < 2) return fail(Status::Decode, "DWA data is truncated");
    const std::uint16_t rule_size = load16(packed.data() + pos);
    if (rule_size < 2 || rule_size > size - pos) return fail(Status::Decode, "corrupt DWA channel rules");
    CVT_TRY_ASSIGN(rules, read_rules(packed.subspan(pos + 2, rule_size - 2u)));
    pos += rule_size;
  }
  if (unknown_packed + ac_packed + dc_packed + rle_packed > size - pos) {
    return fail(Status::Decode, "DWA data is truncated");
  }
  const ByteView unknown_data = packed.subspan(pos, static_cast<std::size_t>(unknown_packed));
  pos += static_cast<std::size_t>(unknown_packed);
  const ByteView ac_data = packed.subspan(pos, static_cast<std::size_t>(ac_packed));
  pos += static_cast<std::size_t>(ac_packed);
  const ByteView dc_data = packed.subspan(pos, static_cast<std::size_t>(dc_packed));
  pos += static_cast<std::size_t>(dc_packed);
  const ByteView rle_data = packed.subspan(pos, static_cast<std::size_t>(rle_packed));

  // Classify the channels; the last matching rule wins.
  const std::size_t count = layout.channels.size();
  std::vector<Scheme> scheme(count, Scheme::Unknown);
  struct PrefixSet {
    std::string prefix;
    std::array<int, 3> idx{-1, -1, -1};
  };
  std::vector<PrefixSet> prefixes;
  for (std::size_t c = 0; c < count; ++c) {
    const std::string& name = layout.channels[c].name;
    const std::size_t dot = name.rfind('.');
    const std::size_t split = dot == std::string::npos ? 0 : dot + 1;
    const std::string_view suffix = std::string_view(name).substr(split);
    const std::string prefix = name.substr(0, split);
    auto it = std::find_if(prefixes.begin(), prefixes.end(), [&](const PrefixSet& p) { return p.prefix == prefix; });
    if (it == prefixes.end()) {
      prefixes.push_back({prefix, {-1, -1, -1}});
      it = prefixes.end() - 1;
    }
    for (const Rule& rule : rules) {
      if (!rule_matches(rule, suffix, layout.channels[c].type)) continue;
      scheme[c] = rule.scheme;
      if (rule.csc >= 0) it->idx[static_cast<std::size_t>(rule.csc)] = static_cast<int>(c);
    }
  }
  std::vector<std::array<int, 3>> csc_sets;
  for (const PrefixSet& p : prefixes) {
    if (p.idx[0] < 0 || p.idx[1] < 0 || p.idx[2] < 0) continue;
    const ChunkChannel& r = layout.channels[static_cast<std::size_t>(p.idx[0])];
    const ChunkChannel& g = layout.channels[static_cast<std::size_t>(p.idx[1])];
    const ChunkChannel& b = layout.channels[static_cast<std::size_t>(p.idx[2])];
    if (r.x_sampling != g.x_sampling || r.x_sampling != b.x_sampling || r.y_sampling != g.y_sampling ||
        r.y_sampling != b.y_sampling) {
      continue;
    }
    csc_sets.push_back(p.idx);
  }

  // Sizes the counters must cover.
  std::uint64_t unknown_needed = 0;
  std::uint64_t rle_needed = 0;
  std::uint64_t dct_blocks = 0;
  for (std::size_t c = 0; c < count; ++c) {
    const ChunkChannel& channel = layout.channels[c];
    const std::uint64_t bytes = std::uint64_t(static_cast<std::uint32_t>(channel.width)) *
                                static_cast<std::uint32_t>(channel.height) * pixel_size(channel.type);
    if (scheme[c] == Scheme::Unknown) unknown_needed += bytes;
    if (scheme[c] == Scheme::Rle) rle_needed += bytes;
    if (scheme[c] == Scheme::LossyDct) {
      dct_blocks += ((std::uint64_t(static_cast<std::uint32_t>(channel.width)) + 7) / 8) *
                    ((std::uint64_t(static_cast<std::uint32_t>(channel.height)) + 7) / 8);
    }
  }
  if (unknown_needed > 0 ? (unknown_raw < unknown_needed || unknown_packed == 0) : (unknown_raw | unknown_packed) != 0) {
    return fail(Status::Decode, "corrupt DWA sizes");
  }
  if (rle_needed > 0 ? (rle_raw < rle_needed || rle_packed == 0) : (rle_raw | rle_packed | rle_unpacked) != 0) {
    return fail(Status::Decode, "corrupt DWA sizes");
  }
  if (ac_count > dct_blocks * 63 || dc_count > dct_blocks) return fail(Status::Decode, "corrupt DWA sizes");

  Bytes unknown;
  if (unknown_packed > 0) {
    CVT_TRY_ASSIGN(unknown, zlib_exact(unknown_data, unknown_raw));
  }
  std::vector<std::uint16_t> ac;
  if (ac_packed > 0) {
    ac.resize(static_cast<std::size_t>(ac_count));
    if (ac_compression == 0) {
      CVT_TRY(huf_decompress(ac_data, ac));
    } else if (ac_compression == 1) {
      CVT_TRY_ASSIGN(const Bytes bytes, zlib_exact(ac_data, ac_count * 2));
      for (std::size_t i = 0; i < ac.size(); ++i) ac[i] = load16(bytes.data() + i * 2);
    } else {
      return fail(Status::Decode, "unknown DWA AC compression");
    }
  }
  std::vector<std::uint16_t> dc;
  if (dc_packed > 0) {
    CVT_TRY_ASSIGN(const Bytes zipped, zlib_exact(dc_data, dc_count * 2));
    const Bytes bytes = zip_reconstruct(zipped);
    dc.resize(static_cast<std::size_t>(dc_count));
    for (std::size_t i = 0; i < dc.size(); ++i) dc[i] = load16(bytes.data() + i * 2);
  } else if (dc_count != 0) {
    return fail(Status::Decode, "corrupt DWA sizes");
  }
  Bytes rle;
  if (rle_raw > 0) {
    CVT_TRY_ASSIGN(const Bytes unpacked, zlib_exact(rle_data, rle_unpacked));
    CVT_TRY_ASSIGN(rle, rle_decode(unpacked, rle_raw));
  }

  // Output rows of every channel.
  Bytes out(static_cast<std::size_t>(layout.raw_size));
  std::vector<Target> targets(count);
  {
    std::size_t offset = 0;
    for (std::size_t c = 0; c < count; ++c) targets[c].channel = &layout.channels[c];
    for (std::int32_t line = 0; line < layout.height; ++line) {
      for (std::size_t c = 0; c < count; ++c) {
        if (!ChunkLayout::has_line(layout.channels[c], std::int64_t{layout.y0} + line)) continue;
        targets[c].rows.push_back(offset);
        offset += static_cast<std::size_t>(layout.channels[c].width) * pixel_size(layout.channels[c].type);
      }
    }
  }

  DctStreams streams{ac, 0, dc, 0};
  std::vector<bool> done(count, false);
  const std::uint16_t* linear = to_linear_table().data();
  for (const std::array<int, 3>& set : csc_sets) {
    Target* group[3];
    for (std::size_t k = 0; k < 3; ++k) {
      const auto c = static_cast<std::size_t>(set[k]);
      if (scheme[c] != Scheme::LossyDct) return fail(Status::Decode, "corrupt DWA channel classification");
      group[k] = &targets[c];
      done[c] = true;
    }
    if (group[0]->channel->width > 0 && group[0]->channel->height > 0) {
      CVT_TRY(decode_dct(std::span<Target* const>(group, 3), streams, linear, out));
    }
  }
  std::size_t unknown_pos = 0;
  std::size_t rle_base = 0;
  for (std::size_t c = 0; c < count; ++c) {
    if (done[c]) continue;
    const ChunkChannel& channel = layout.channels[c];
    const auto w = static_cast<std::size_t>(channel.width);
    const auto h = static_cast<std::size_t>(channel.height);
    const std::size_t ps = pixel_size(channel.type);
    if (w == 0 || h == 0) continue;
    switch (scheme[c]) {
      case Scheme::LossyDct: {
        Target* one[1] = {&targets[c]};
        CVT_TRY(decode_dct(std::span<Target* const>(one, 1), streams, channel.p_linear ? nullptr : linear, out));
        break;
      }
      case Scheme::Rle: {
        for (std::size_t row = 0; row < h; ++row) {
          std::byte* dst = out.data() + targets[c].rows[row];
          for (std::size_t x = 0; x < w; ++x) {
            for (std::size_t b = 0; b < ps; ++b) dst[x * ps + b] = rle[rle_base + b * w * h + row * w + x];
          }
        }
        rle_base += w * h * ps;
        break;
      }
      case Scheme::Unknown: {
        const std::size_t row_bytes = w * ps;
        for (std::size_t row = 0; row < h; ++row) {
          std::memcpy(out.data() + targets[c].rows[row], unknown.data() + unknown_pos, row_bytes);
          unknown_pos += row_bytes;
        }
        break;
      }
    }
  }
  return out;
}

}  // namespace cvt::image_modern::exr
