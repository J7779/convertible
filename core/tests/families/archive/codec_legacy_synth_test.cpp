// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// Legacy decode-only codecs that no available tool can write (PKZIP shrink,
// reduce and implode, Quantum, MS-XCA plain LZ77) and corner cases the real
// fixtures do not reach (Deflate64 length code 285, LZX uncompressed blocks).
// The streams are hand-assembled here by small encoders written from the
// format descriptions (APPNOTE.TXT, [MS-XCA], the Quantum and LZX
// descriptions), independently of the decoders' code.
#include <algorithm>
#include <map>

#include "families/archive/codec_legacy_util.hpp"

using namespace cvt;
namespace cz = cvt::compression;
namespace tc = cvt::test::codec;
namespace tl = cvt::test::legacy;

namespace {

cz::DecodeOptions sized(std::uint64_t size) {
  cz::DecodeOptions d;
  d.output_size = size;
  return d;
}

Bytes decode_ok(cz::Method m, ByteView data, const cz::DecodeOptions& options) {
  auto out = cz::decompress(m, data, options);
  REQUIRE_OK(out);
  return std::move(*out);
}

std::uint8_t u8(ByteView d, std::size_t i) { return std::to_integer<std::uint8_t>(d[i]); }

/// Hash-chain match finder over a whole input (positions with equal 3-byte hashes).
class Matcher {
 public:
  explicit Matcher(ByteView in) : in_(in), prev_(in.size(), -1) {
    std::vector<std::int64_t> head(1u << 16, -1);
    for (std::size_t i = 0; i + 2 < in.size(); ++i) {
      const std::uint32_t h = (u8(in, i) * 2654435761u ^ u8(in, i + 1) * 40503u ^ u8(in, i + 2)) & 0xFFFFu;
      prev_[i] = head[h];
      head[h] = static_cast<std::int64_t>(i);
    }
  }
  /// (distance, length) of the longest earlier match at `pos`.
  [[nodiscard]] std::pair<std::size_t, std::size_t> find(std::size_t pos, std::size_t max_dist,
                                                        std::size_t max_len) const {
    std::size_t best_len = 0;
    std::size_t best_dist = 0;
    const std::size_t limit = std::min(max_len, in_.size() - pos);
    std::int64_t j = pos < prev_.size() ? prev_[pos] : -1;
    for (int steps = 0; j >= 0 && steps < 256; ++steps, j = prev_[static_cast<std::size_t>(j)]) {
      const std::size_t d = pos - static_cast<std::size_t>(j);
      if (d > max_dist) break;
      std::size_t l = 0;
      while (l < limit && in_[pos + l] == in_[pos - d + l]) ++l;
      if (l > best_len) {
        best_len = l;
        best_dist = d;
        if (l == limit) break;
      }
    }
    return {best_dist, best_len};
  }

 private:
  ByteView in_;
  std::vector<std::int64_t> prev_;
};

// ---------------------------------------------------------------------------- MS-XCA plain LZ77

Bytes xpress_plain_encode(ByteView in) {
  Bytes out;
  std::size_t flag_at = 0;
  std::uint32_t flags = 0;
  int nflags = 0;
  std::size_t half = 0;  // position + 1 of a half-used length byte
  const auto put16 = [&](std::uint32_t v) {
    out.push_back(static_cast<std::byte>(v));
    out.push_back(static_cast<std::byte>(v >> 8));
  };
  const auto put_flag = [&](std::uint32_t bit) {
    if (nflags == 0) {
      flag_at = out.size();
      out.resize(out.size() + 4);
    }
    flags = (flags << 1) | bit;
    if (++nflags == 32) {
      for (int k = 0; k < 4; ++k) out[flag_at + static_cast<std::size_t>(k)] = static_cast<std::byte>(flags >> (8 * k));
      nflags = 0;
      flags = 0;
    }
  };
  const Matcher m(in);
  std::size_t pos = 0;
  while (pos < in.size()) {
    const auto [dist, len] = m.find(pos, 8192, 300);
    if (len < 3) {
      put_flag(0);
      out.push_back(in[pos++]);
      continue;
    }
    put_flag(1);
    std::size_t l = len - 3;
    put16(static_cast<std::uint32_t>(((dist - 1) << 3) | std::min<std::size_t>(l, 7)));
    if (l >= 7) {
      l -= 7;
      const auto nibble = static_cast<std::uint32_t>(std::min<std::size_t>(l, 15));
      if (half == 0) {
        half = out.size() + 1;
        out.push_back(static_cast<std::byte>(nibble));
      } else {
        out[half - 1] |= static_cast<std::byte>(nibble << 4);
        half = 0;
      }
      if (l >= 15) {
        l -= 15;
        if (l < 255) {
          out.push_back(static_cast<std::byte>(l));
        } else {
          out.push_back(std::byte{255});
          put16(static_cast<std::uint32_t>(len - 3));
        }
      }
    }
    pos += len;
  }
  if (nflags != 0) {
    flags = (flags << (32 - nflags)) | ((1u << (32 - nflags)) - 1u);
    for (int k = 0; k < 4; ++k) out[flag_at + static_cast<std::size_t>(k)] = static_cast<std::byte>(flags >> (8 * k));
  }
  return out;
}

// ---------------------------------------------------------------------------- Shrink (LZW)

Bytes shrink_encode(ByteView in) {
  tl::LsbWriter w;
  std::map<std::pair<std::uint32_t, std::uint8_t>, std::uint32_t> dict;
  std::uint32_t next = 257;
  int width = 9;
  const auto emit = [&](std::uint32_t code) {
    while (code >= (1u << width)) {
      w.put(256, width);
      w.put(1, width);
      ++width;
    }
    w.put(code, width);
  };
  if (in.empty()) return w.finish();
  std::uint32_t cur = u8(in, 0);
  for (std::size_t i = 1; i < in.size(); ++i) {
    const std::uint8_t c = u8(in, i);
    const auto it = dict.find({cur, c});
    if (it != dict.end()) {
      cur = it->second;
      continue;
    }
    emit(cur);
    if (next < 8192) dict[{cur, c}] = next++;
    cur = c;
  }
  emit(cur);
  return w.finish();
}

// ---------------------------------------------------------------------------- Reduce

Bytes reduce_encode(ByteView in, int factor) {
  // Run/back-reference stage with the escape byte 144.
  const std::uint32_t mask = 0x7Fu >> (factor - 1);
  const std::size_t max_dist = std::size_t{256} << (factor);
  std::vector<std::uint8_t> s;
  const Matcher m(in);
  std::size_t pos = 0;
  while (pos < in.size()) {
    auto [dist, len] = m.find(pos, max_dist, mask + 255 + 3);
    if (len >= 4) {
      const std::size_t l = len - 3;
      const std::size_t hi = (dist - 1) >> 8;
      s.push_back(144);
      if (l < mask) {
        s.push_back(static_cast<std::uint8_t>((hi << (8 - factor)) | l));
      } else {
        s.push_back(static_cast<std::uint8_t>((hi << (8 - factor)) | mask));
        s.push_back(static_cast<std::uint8_t>(l - mask));
      }
      s.push_back(static_cast<std::uint8_t>((dist - 1) & 0xFFu));
      pos += len;
      continue;
    }
    const std::uint8_t c = u8(in, pos++);
    s.push_back(c);
    if (c == 144) s.push_back(0);
  }
  // Follower sets: the (up to) 8 most frequent successors of every byte.
  std::array<std::array<std::uint32_t, 256>, 256> freq{};
  std::uint8_t last = 0;
  for (const std::uint8_t c : s) {
    ++freq[last][c];
    last = c;
  }
  std::array<std::vector<std::uint8_t>, 256> sets;
  for (std::size_t j = 0; j < 256; ++j) {
    std::vector<std::uint8_t> order;
    for (std::size_t c = 0; c < 256; ++c) {
      if (freq[j][c] != 0) order.push_back(static_cast<std::uint8_t>(c));
    }
    std::stable_sort(order.begin(), order.end(), [&](std::uint8_t a, std::uint8_t b) { return freq[j][a] > freq[j][b]; });
    if (order.size() > 8) order.resize(8);
    if (j % 7 == 3) order.clear();  // some empty sets too
    sets[j] = order;
  }
  tl::LsbWriter w;
  for (int j = 255; j >= 0; --j) {
    const auto& set = sets[static_cast<std::size_t>(j)];
    w.put(static_cast<std::uint32_t>(set.size()), 6);
    for (const std::uint8_t c : set) w.put(c, 8);
  }
  last = 0;
  for (const std::uint8_t c : s) {
    const auto& set = sets[last];
    if (set.empty()) {
      w.put(c, 8);
    } else {
      const auto it = std::find(set.begin(), set.end(), c);
      if (it == set.end()) {
        w.put(1, 1);
        w.put(c, 8);
      } else {
        int bits = 1;
        while ((1u << bits) < set.size()) ++bits;
        w.put(0, 1);
        w.put(static_cast<std::uint32_t>(it - set.begin()), bits);
      }
    }
    last = c;
  }
  return w.finish();
}

// ---------------------------------------------------------------------------- Implode

struct SfTree {
  std::vector<std::uint8_t> lengths;
  std::vector<std::uint32_t> code;  // 16-bit Shannon-Fano codes (APPNOTE algorithm)

  explicit SfTree(std::vector<std::uint8_t> l) : lengths(std::move(l)), code(lengths.size()) {
    std::vector<std::size_t> order(lengths.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return lengths[a] < lengths[b]; });
    std::uint32_t c = 0;
    std::uint32_t increment = 0;
    std::uint8_t last = 0;
    for (std::size_t k = order.size(); k-- > 0;) {
      c += increment;
      if (lengths[order[k]] != last) {
        last = lengths[order[k]];
        increment = 1u << (16 - last);
      }
      code[order[k]] = c & 0xFFFFu;
    }
  }
  void put(tl::LsbWriter& w, std::size_t sym) const {
    // The 16-bit code is bit-reversed and sent LSB first: its top bits go first.
    for (int b = 0; b < lengths[sym]; ++b) w.put((code[sym] >> (15 - b)) & 1u, 1);
  }
  void describe(tl::LsbWriter& w) const {
    std::vector<std::uint8_t> bytes;
    std::size_t i = 0;
    while (i < lengths.size()) {
      std::size_t n = 1;
      while (i + n < lengths.size() && lengths[i + n] == lengths[i] && n < 16) ++n;
      bytes.push_back(static_cast<std::uint8_t>(((n - 1) << 4) | (lengths[i] - 1u)));
      i += n;
    }
    w.put(static_cast<std::uint32_t>(bytes.size() - 1), 8);
    for (const std::uint8_t b : bytes) w.put(b, 8);
  }
};

std::vector<std::uint8_t> lengths_for(std::size_t n, std::initializer_list<std::pair<std::size_t, std::uint8_t>> runs) {
  std::vector<std::uint8_t> out;
  for (const auto& [count, len] : runs) out.insert(out.end(), count, len);
  out.resize(n, 8);
  return out;
}

Bytes implode_encode(ByteView in, bool big_window, bool literal_tree) {
  // Complete codes with mixed lengths, so that the code order matters.
  const SfTree lit(lengths_for(256, {{2, 4}, {28, 7}, {110, 8}, {116, 9}}));
  const SfTree len(lengths_for(64, {{2, 2}, {2, 6}, {60, 7}}));
  const SfTree dist(lengths_for(64, {{2, 2}, {2, 6}, {60, 7}}));
  tl::LsbWriter w;
  if (literal_tree) lit.describe(w);
  len.describe(w);
  dist.describe(w);
  const int low_bits = big_window ? 7 : 6;
  const std::size_t min_match = literal_tree ? 3 : 2;
  const std::size_t max_dist = std::size_t{64} << low_bits;
  const Matcher m(in);
  std::size_t pos = 0;
  while (pos < in.size()) {
    auto [d, l] = m.find(pos, max_dist, min_match + 63 + 255);
    if (l >= min_match + 1) {
      w.put(0, 1);
      w.put(static_cast<std::uint32_t>((d - 1) & ((1u << low_bits) - 1u)), low_bits);
      dist.put(w, (d - 1) >> low_bits);
      const std::size_t code = l - min_match;
      if (code >= 63) {
        len.put(w, 63);
        w.put(static_cast<std::uint32_t>(code - 63), 8);
      } else {
        len.put(w, code);
      }
      pos += l;
    } else {
      w.put(1, 1);
      if (literal_tree) {
        lit.put(w, u8(in, pos));
      } else {
        w.put(u8(in, pos), 8);
      }
      ++pos;
    }
  }
  return w.finish();
}

// ---------------------------------------------------------------------------- Quantum

struct QModel {
  struct Entry {
    int sym = 0;
    std::uint32_t cf = 0;
  };
  std::vector<Entry> s;
  int shifts = 4;
  QModel(int start, int n) : s(static_cast<std::size_t>(n) + 1) {
    for (int i = 0; i <= n; ++i) s[static_cast<std::size_t>(i)] = {start + i, static_cast<std::uint32_t>(n - i)};
  }
  [[nodiscard]] int n() const { return static_cast<int>(s.size()) - 1; }
  void bump(std::size_t upto) {
    for (std::size_t k = 0; k <= upto; ++k) s[k].cf += 8;
    if (s[0].cf <= 3800) return;
    if (--shifts != 0) {
      for (int i = n() - 1; i >= 0; --i) {
        auto& e = s[static_cast<std::size_t>(i)];
        e.cf >>= 1;
        if (e.cf <= s[static_cast<std::size_t>(i) + 1].cf) e.cf = s[static_cast<std::size_t>(i) + 1].cf + 1;
      }
      return;
    }
    shifts = 50;
    for (std::size_t i = 0; i < static_cast<std::size_t>(n()); ++i) s[i].cf = (s[i].cf - s[i + 1].cf + 1) >> 1;
    for (std::size_t i = 0; i + 1 < static_cast<std::size_t>(n()); ++i) {
      for (std::size_t j = i + 1; j < static_cast<std::size_t>(n()); ++j) {
        if (s[i].cf < s[j].cf) std::swap(s[i], s[j]);
      }
    }
    for (std::size_t i = static_cast<std::size_t>(n()); i-- > 0;) s[i].cf += s[i + 1].cf;
  }
};

class QuantumEncoder {
 public:
  void symbol(QModel& m, int sym) {
    std::size_t i = 0;
    while (m.s[i].sym != sym) ++i;
    const std::uint32_t total = m.s[0].cf;
    const std::uint32_t range = high_ - low_ + 1;
    high_ = low_ + (m.s[i].cf * range) / total - 1;
    low_ = low_ + (m.s[i + 1].cf * range) / total;
    m.bump(i);
    while (true) {
      if (((low_ ^ high_) & 0x8000u) == 0) {
        bit(high_ >> 15);
      } else if ((low_ & 0x4000u) != 0 && (high_ & 0x4000u) == 0) {
        ++pending_;
        low_ &= 0x3FFFu;
        high_ |= 0x4000u;
      } else {
        break;
      }
      ++shifts_;
      low_ = (low_ << 1) & 0xFFFFu;
      high_ = ((high_ << 1) | 1u) & 0xFFFFu;
    }
  }
  void raw(std::uint32_t value, int n) {
    // The decoder reads raw bits after the 16 code bits it holds: they go
    // in front of code bit 16 + (renormalisation shifts so far).
    for (int b = n - 1; b >= 0; --b) inserts_.push_back({16 + shifts_, (value >> b) & 1u});
  }
  /// Finish the frame: the code bits of `low` resolve every decision; then interleave.
  void flush(tl::MsbWriter& w) {
    bit(low_ >> 15);
    for (int b = 14; b >= 0; --b) code_.push_back((low_ >> b) & 1u);
    std::size_t next = 0;
    for (std::size_t k = 0; k <= code_.size(); ++k) {
      while (next < inserts_.size() && inserts_[next].first == k) w.put(inserts_[next++].second, 1);
      if (k < code_.size()) w.put(code_[k], 1);
    }
    code_.clear();
    inserts_.clear();
    low_ = 0;
    high_ = 0xFFFF;
    pending_ = 0;
    shifts_ = 0;
  }

 private:
  void bit(std::uint32_t b) {
    code_.push_back(b);
    for (; pending_ > 0; --pending_) code_.push_back(b ^ 1u);
  }
  std::uint32_t low_ = 0;
  std::uint32_t high_ = 0xFFFF;
  int pending_ = 0;
  std::size_t shifts_ = 0;
  std::vector<std::uint32_t> code_;
  std::vector<std::pair<std::size_t, std::uint32_t>> inserts_;
};

constexpr std::array<std::uint32_t, 42> q_pos_base = {
    0,     1,     2,     3,      4,      6,      8,      12,     16,     24,      32,      48,     64,     96,
    128,   192,   256,   384,    512,    768,    1024,   1536,   2048,   3072,    4096,    6144,   8192,   12288,
    16384, 24576, 32768, 49152,  65536,  98304,  131072, 196608, 262144, 393216,  524288,  786432, 1048576, 1572864};
constexpr std::array<std::uint32_t, 27> q_len_base = {0,  1,  2,  3,  4,  5,  6,   8,   10,  12,  14,  18,  22, 26,
                                                      30, 38, 46, 54, 62, 78, 94, 110, 126, 158, 190, 222, 254};

int q_slot(std::uint32_t v, const std::uint32_t* base, int n) {
  int s = 0;
  while (s + 1 < n && base[s + 1] <= v) ++s;
  return s;
}
int q_extra_pos(int slot) { return slot < 4 ? 0 : slot / 2 - 1; }
int q_extra_len(int slot) { return slot < 6 || slot == 26 ? 0 : (slot - 2) / 4; }

/// Quantum folder with frames of 32 KiB; returns the data and the per-frame compressed sizes.
std::pair<Bytes, std::vector<std::uint32_t>> quantum_encode(ByteView in, int window_bits) {
  const int i2 = window_bits * 2;
  std::array<QModel, 4> lit = {QModel(0, 64), QModel(64, 64), QModel(128, 64), QModel(192, 64)};
  QModel pos3(0, std::min(i2, 24));
  QModel pos4(0, std::min(i2, 36));
  QModel pos_long(0, i2);
  QModel lengths(0, 27);
  QModel sel(0, 7);
  QuantumEncoder enc;
  const Matcher matcher(in);
  Bytes out;
  std::vector<std::uint32_t> sizes;
  std::size_t pos = 0;
  const std::size_t window = std::size_t{1} << window_bits;
  while (pos < in.size()) {
    const std::size_t frame_end = std::min(in.size(), (pos / 32768 + 1) * 32768);
    tl::MsbWriter w;
    while (pos < frame_end) {
      auto [d, l] = matcher.find(pos, window, std::min<std::size_t>(259, frame_end - pos));
      QModel* pm = nullptr;
      if (l >= 5) {
        enc.symbol(sel, 6);
        const auto v = static_cast<std::uint32_t>(l - 5);
        const int ls = q_slot(v, q_len_base.data(), 27);
        enc.symbol(lengths, ls);
        enc.raw(v - q_len_base[static_cast<std::size_t>(ls)], q_extra_len(ls));
        pm = &pos_long;
      } else if (l == 4 || (l == 3 && d <= 4096)) {
        l = l == 4 ? 4 : 3;
        enc.symbol(sel, l == 3 ? 4 : 5);
        pm = l == 3 ? &pos3 : &pos4;
      }
      if (pm == nullptr || q_slot(static_cast<std::uint32_t>(d - 1), q_pos_base.data(), 42) >= pm->n()) {
        if (pm != nullptr) return {};  // cannot happen with the window sizes used here
        const std::uint8_t c = u8(in, pos);
        enc.symbol(sel, c >> 6);
        enc.symbol(lit[c >> 6], c);
        ++pos;
        continue;
      }
      const auto v = static_cast<std::uint32_t>(d - 1);
      const int ps = q_slot(v, q_pos_base.data(), 42);
      enc.symbol(*pm, ps);
      enc.raw(v - q_pos_base[static_cast<std::size_t>(ps)], q_extra_pos(ps));
      pos += l;
    }
    enc.flush(w);
    Bytes frame = w.finish();
    frame.resize(frame.size() + (frame.size() % 3));  // a few zero padding bytes, like the CAB encoder
    sizes.push_back(static_cast<std::uint32_t>(frame.size()));
    append(out, frame);
  }
  return {out, sizes};
}

// ---------------------------------------------------------------------------- LZX words

class WordWriter {
 public:
  void put(std::uint32_t value, int n) {
    for (int i = n - 1; i >= 0; --i) {
      acc_ = (acc_ << 1) | ((value >> i) & 1u);
      if (++count_ == 16) {
        out_.push_back(static_cast<std::byte>(acc_ & 0xFFu));
        out_.push_back(static_cast<std::byte>(acc_ >> 8));
        acc_ = 0;
        count_ = 0;
      }
    }
  }
  void align(bool always) {
    if (count_ != 0) {
      put(0, 16 - count_);
    } else if (always) {
      put(0, 16);
    }
  }
  void bytes(ByteView b) { append(out_, b); }
  void le32(std::uint32_t v) {
    for (int k = 0; k < 4; ++k) out_.push_back(static_cast<std::byte>(v >> (8 * k)));
  }
  Bytes& data() { return out_; }

 private:
  Bytes out_;
  std::uint32_t acc_ = 0;
  int count_ = 0;
};

}  // namespace

TEST_CASE("archive/xpress: plain LZ77 streams assembled from [MS-XCA]") {
  for (const Bytes& input : {tc::lorem(), tc::text_corpus(30000), Bytes(5000, std::byte{'z'}), tc::mixed()}) {
    const Bytes packed = xpress_plain_encode(input);
    CHECK_LT(packed.size(), input.size() + input.size() / 8 + 8);
    CHECK(decode_ok(cz::Method::XpressPlain, packed, sized(input.size())) == input);
    CHECK(decode_ok(cz::Method::XpressPlain, packed, {}) == input);  // ends with the data
  }
  const Bytes packed = xpress_plain_encode(tc::lorem());
  cz::DecodeOptions limited;
  limited.max_output = 100;
  CHECK_STATUS(cz::decompress(cz::Method::XpressPlain, packed, limited), Status::Limit);
  tc::hostile(cz::Method::XpressPlain, packed, sized(tc::lorem().size()));
  CHECK_STATUS(cz::decompress(cz::Method::XpressHuffman, packed, {}), Status::InvalidArgument);
}

TEST_CASE("archive/shrink: LZW with code-size increments and a partial clear") {
  // Hand-assembled: 'a' 'b' 257 'a' <clear> 258 (KwKwK) 257 <wider> 259.
  tl::LsbWriter w;
  for (const std::uint32_t code : {97u, 98u, 257u, 97u, 256u, 2u, 258u, 257u, 256u, 1u}) w.put(code, 9);
  w.put(259, 10);
  const Bytes stream = w.finish();
  CHECK(decode_ok(cz::Method::Shrink, stream, sized(12)) == to_bytes("ababaaaabaaa"));
  for (const Bytes& input : {tc::lorem(), tc::text_corpus(200000), test::random_bytes(20000, 5)}) {
    const Bytes packed = shrink_encode(input);
    CHECK(decode_ok(cz::Method::Shrink, packed, sized(input.size())) == input);
  }
  const Bytes packed = shrink_encode(tc::lorem());
  CHECK_STATUS(cz::decompress(cz::Method::Shrink, packed, {}), Status::InvalidArgument);
  CHECK_STATUS(cz::decompress(cz::Method::Shrink, packed, sized(tc::lorem().size() + 5)), Status::Decode);
  tc::hostile(cz::Method::Shrink, packed, sized(tc::lorem().size()));
}

TEST_CASE("archive/reduce: factors 1..4 with follower sets") {
  const Bytes text = tc::text_corpus(20000);
  Bytes with_escapes = tc::lorem();
  for (std::size_t i = 0; i < with_escapes.size(); i += 37) with_escapes[i] = std::byte{144};
  for (int factor = 1; factor <= 4; ++factor) {
    INFO("factor ", factor);
    for (const Bytes* input : {&text, static_cast<const Bytes*>(&with_escapes)}) {
      const Bytes packed = reduce_encode(*input, factor);
      cz::DecodeOptions d = sized(input->size());
      d.level = factor;
      CHECK(decode_ok(cz::Method::Reduce, packed, d) == *input);
    }
  }
  cz::DecodeOptions d = sized(text.size());
  d.level = 5;
  CHECK_STATUS(cz::decompress(cz::Method::Reduce, reduce_encode(text, 2), d), Status::InvalidArgument);
  d.level = 2;
  tc::hostile(cz::Method::Reduce, reduce_encode(text, 2), d);
}

TEST_CASE("archive/implode: 4K/8K windows with two and three Shannon-Fano trees") {
  const Bytes text = tc::text_corpus(30000);
  for (const std::uint32_t flags : {0u, 2u, 4u, 6u}) {
    INFO("flags ", flags);
    const Bytes packed = implode_encode(text, (flags & 2u) != 0, (flags & 4u) != 0);
    cz::DecodeOptions d = sized(text.size());
    d.flags = flags;
    CHECK(decode_ok(cz::Method::Implode, packed, d) == text);
    CHECK_LT(packed.size(), text.size());
    tc::hostile(cz::Method::Implode, packed, d);
  }
  cz::DecodeOptions d = sized(text.size());
  d.flags = 4;
  CHECK_STATUS(cz::decompress(cz::Method::Implode, implode_encode(text, false, false), d), Status::Decode);
}

TEST_CASE("archive/quantum: frames with restarts, raw extra bits and model re-sorting") {
  const Bytes text = tc::text_corpus(70000);  // three frames; enough symbols to re-sort the models
  for (const int bits : {12, 16, 21}) {
    INFO("window bits ", bits);
    const auto [packed, sizes] = quantum_encode(text, bits);
    REQUIRE_EQ(sizes.size(), std::size_t{3});
    std::vector<std::uint32_t> blocks = {32768, 32768, 70000 - 65536};
    Bytes props;
    for (const std::uint32_t s : sizes) {
      for (int k = 0; k < 4; ++k) props.push_back(static_cast<std::byte>(s >> (8 * k)));
    }
    cz::DecodeOptions d;
    d.window_bits = bits;
    d.block_sizes = blocks;
    d.properties = props;
    CHECK(decode_ok(cz::Method::Quantum, packed, d) == text);
    CHECK_LT(packed.size(), text.size() / 3);
    // Without the compressed sizes the zero padding is skipped heuristically.
    cz::DecodeOptions no_sizes = d;
    no_sizes.properties = {};
    const auto guessed = cz::decompress(cz::Method::Quantum, packed, no_sizes);
    if (guessed) CHECK(*guessed == text);
    if (bits == 16) {
      cz::DecodeOptions first = sized(32768);
      first.window_bits = bits;
      tc::hostile(cz::Method::Quantum, ByteView(packed).first(sizes[0]), first);
    }
  }
  cz::DecodeOptions bad;
  bad.window_bits = 9;
  CHECK_STATUS(cz::decompress(cz::Method::Quantum, tc::lorem(), bad), Status::InvalidArgument);
}

TEST_CASE("archive/deflate64: length code 285 carries 16 extra bits (MSZIP: 258)") {
  const auto stream = [](bool mszip) {
    tl::LsbWriter w;
    if (mszip) {
      w.put('C', 8);
      w.put('K', 8);
    }
    w.put(1, 1);                   // final
    w.put(1, 2);                   // fixed Huffman
    w.put_msb_first(0x30 + 'a', 8);  // literal 'a'
    w.put_msb_first(0xC5, 8);        // length code 285
    if (!mszip) w.put(1000, 16);
    w.put_msb_first(0, 5);  // distance 1
    w.put_msb_first(0, 7);  // end of block
    return w.finish();
  };
  CHECK(decode_ok(cz::Method::Deflate64, stream(false), {}) == Bytes(1004, std::byte{'a'}));
  CHECK(decode_ok(cz::Method::Mszip, stream(true), {}) == Bytes(259, std::byte{'a'}));
  // Distance codes 30/31 (beyond 32 KiB) from 7-Zip: two copies of a 40 kB text.
  Bytes twice = tl::text40k();
  append(twice, tl::text40k());
  CHECK(decode_ok(cz::Method::Deflate64, tl::fixture("twice.deflate64"), sized(twice.size())) == twice);
}

TEST_CASE("archive/lzx: uncompressed blocks with odd sizes (CAB and WIM layouts)") {
  // CAB/CHM: E8 bit, 24-bit block sizes, 1..16 alignment bits.
  WordWriter cab;
  cab.put(0, 1);
  cab.put(3, 3);
  cab.put(5, 24);
  cab.align(true);
  for (int k = 0; k < 3; ++k) cab.le32(1);
  cab.bytes(as_bytes("hello"));
  cab.bytes(as_bytes(std::string_view("\0", 1)));  // pad to 16 bits
  WordWriter second;
  second.put(3, 3);
  second.put(6, 24);
  second.align(true);
  append(cab.data(), second.data());
  for (int k = 0; k < 3; ++k) cab.le32(1);
  cab.bytes(as_bytes(" world"));
  cz::DecodeOptions d = sized(11);
  d.window_bits = 15;
  CHECK(decode_ok(cz::Method::Lzx, cab.data(), d) == to_bytes("hello world"));
  // WIM: default-size flag + 16-bit size, alignment only when needed.
  WordWriter wim;
  wim.put(3, 3);
  wim.put(0, 1);
  wim.put(7, 16);
  wim.align(false);
  for (int k = 0; k < 3; ++k) wim.le32(1);
  wim.bytes(as_bytes("chunk!!"));
  CHECK(decode_ok(cz::Method::LzxWim, wim.data(), sized(7)) == to_bytes("chunk!!"));
  CHECK_STATUS(cz::decompress(cz::Method::LzxWim, wim.data(), sized(9)), Status::Decode);
}

TEST_CASE("archive/legacy codecs: cancellation, declared sizes beyond the limit and bombs") {
  struct Case {
    cz::Method method;
    int window_bits;
    std::uint32_t flags;
    int level;
  };
  const Case cases[] = {
      {cz::Method::Deflate64, 0, 0, 0},     {cz::Method::Mszip, 0, 0, 0},     {cz::Method::Lzx, 16, 0, 0},
      {cz::Method::LzxWim, 0, 0, 0},        {cz::Method::Quantum, 16, 0, 0},  {cz::Method::XpressPlain, 0, 0, 0},
      {cz::Method::XpressHuffman, 0, 0, 0}, {cz::Method::Lzo1x, 0, 0, 0},     {cz::Method::Implode, 0, 6, 0},
      {cz::Method::Shrink, 0, 0, 0},        {cz::Method::Reduce, 0, 0, 3},    {cz::Method::Lh, 13, 0, 0},
      {cz::Method::Lh, 12, 1, 0},           {cz::Method::Lzs, 11, 0, 0},      {cz::Method::Lzs, 12, 0, 0},
      {cz::Method::Arj, 0, 1, 0},           {cz::Method::Arj, 0, 4, 0},
  };
  const Bytes garbage = test::random_bytes(3000, 11);
  for (const Case& c : cases) {
    INFO(cz::method_name(c.method), " flags ", c.flags);
    REQUIRE(cz::can_decode(c.method));
    CHECK_FALSE(cz::can_encode(c.method));
    cz::DecodeOptions d;
    d.window_bits = c.window_bits;
    d.flags = c.flags;
    d.level = c.level;
    d.output_size = std::uint64_t{1} << 40;
    d.max_output = std::uint64_t{1} << 20;
    CHECK_STATUS(cz::decompress(c.method, garbage, d), Status::Limit);
    d.output_size = 4096;
    d.max_output = cz::unlimited;
    Progress p([](double, std::string_view) { return false; });
    p.cancel();
    d.progress = p;
    CHECK_STATUS(cz::decompress(c.method, garbage, d), Status::Cancelled);
    // Garbage with a large declared size fails or stays within it, without huge allocations.
    cz::DecodeOptions g;
    g.window_bits = c.window_bits;
    g.flags = c.flags;
    g.level = c.level;
    g.output_size = std::uint64_t{1} << 30;
    const auto r = cz::decompress(c.method, garbage, g);
    if (r) CHECK_LE(r->size(), std::uint64_t{1} << 30);
  }
  CHECK_FALSE(cz::can_decode(cz::Method::Lzms));
  // XPRESS: a literal and one match of 2^31 bytes, in fifteen bytes.
  const Bytes bomb = {std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0x7F}, std::byte{'a'},
                      std::byte{7},    std::byte{0},    std::byte{15},   std::byte{255},  std::byte{0},
                      std::byte{0},    std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0x7F}};
  cz::DecodeOptions limited;
  limited.max_output = std::uint64_t{1} << 20;
  CHECK_STATUS(cz::decompress(cz::Method::XpressPlain, bomb, limited), Status::Limit);
  cz::DecodeOptions prefix;
  prefix.stop_after = 10;
  CHECK(decode_ok(cz::Method::XpressPlain, bomb, prefix) == Bytes(10, std::byte{'a'}));
  // LZO1X: a literal and 255 kB matches, repeated.
  Bytes lzo = {std::byte{18}, std::byte{'a'}};
  for (int k = 0; k < 8; ++k) {
    lzo.push_back(std::byte{0x20});
    lzo.insert(lzo.end(), 1000, std::byte{0});
    lzo.push_back(std::byte{1});
    lzo.push_back(std::byte{0});
    lzo.push_back(std::byte{0});
  }
  CHECK_STATUS(cz::decompress(cz::Method::Lzo1x, lzo, limited), Status::Limit);
}
