// Convertible core - archive family: LHA and ARJ decoders (Method::Lh, Method::Lzs, Method::Arj).
//
//   -lh4- .. -lh7- (window_bits 12, 13, 15, 16) and ARJ methods 1..3: LZSS
//       with static Huffman blocks: a 16-bit symbol count, then the code
//       lengths of the 19-symbol "pt" code, the 510-symbol literal/length code
//       (lengths sent through the pt code with zero-run codes 0..2) and the
//       position code (np = window_bits + 1 symbols; a position symbol p > 1
//       carries p - 1 extra bits). MSB-first bit order throughout.
//   -lh1- (window_bits 12, flags = 1): adaptive Huffman over 314 symbols
//       (the frequency-sorted tree of LZHUF, rebuilt when the root count
//       reaches 0x8000) and positions as a fixed 3..8-bit code for the upper
//       six bits plus six raw bits; 4 KiB window, matches of 3..60 bytes.
//   -lzs- (window_bits 11) / -lz5- (window_bits 12): LArc's LZSS with an
//       absolute position into a 2 KiB / 4 KiB ring buffer; -lzs- is a bit
//       stream (flag, 11-bit position, 4-bit length), -lz5- a byte stream
//       (flag bytes, 12-bit position + 4-bit length in two bytes) whose ring
//       buffer starts with LArc's fixed initial pattern.
//   ARJ method 4: unary-prefixed lengths and positions, 15 KiB reach.
// LHA's dictionary starts out filled with spaces, which -lh*- streams may
// refer to; ARJ data never refers before its start.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <array>

#include "families/archive/codec_impl.hpp"
#include "families/archive/codecs/bitio.hpp"
#include "families/archive/codecs/huffman.hpp"
#include "families/archive/codecs/zip_legacy_common.hpp"

namespace cvt::compression::detail {

namespace {

constexpr int nc = 510;  // 256 literals + lengths 3..256
constexpr int nt = 19;

/// LZ copy where positions before the start read as spaces (LHA) or are an error (ARJ).
[[nodiscard]] bool copy_spaces(LegacyOutput& out, std::size_t distance, std::size_t length, bool spaces) {
  if (distance <= out.size()) return out.copy(distance, length);
  if (!spaces || distance == 0) return false;
  if (length > out.room()) {
    out.mark_overflow();
    length = static_cast<std::size_t>(out.room());
  }
  for (std::size_t k = 0; k < length; ++k) {
    const std::size_t n = out.size();
    out.put(distance > n ? std::uint8_t{' '} : out.at(n - distance));
  }
  return true;
}

/// A Huffman code that may consist of a single zero-bit symbol.
struct LhCode {
  HuffmanTable table;
  int single = -1;

  template <class Reader>
  [[nodiscard]] int decode(Reader& in) const {
    return single >= 0 ? single : table.decode(in);
  }
};

class StaticLh {
 public:
  StaticLh(ByteView data, LegacyOutput& out, int np, int pbit, bool spaces, std::string_view name,
           const DecodeOptions& options)
      : in_(data), out_(out), np_(np), pbit_(pbit), spaces_(spaces), name_(name), options_(options) {}

  Result<bool> run() {
    std::uint32_t blocks = 0;
    while (!out_.done()) {
      if (block_left_ == 0) {
        if (in_.position() + 16 > in_.byte_size() * 8 && !out_.exact()) return false;  // clean end of data
        block_left_ = in_.get(16);
        if (block_left_ == 0) return fail(Status::Decode, "{} block holds no symbols", name_);
        CVT_TRY(read_pt(pt_, nt, 5, 3));
        CVT_TRY(read_c());
        CVT_TRY(read_pt(p_, np_, pbit_, -1));
        if (in_.overrun()) return fail(Status::Decode, "{} data is truncated", name_);
        if ((++blocks & 3) == 0 && !legacy_progress(options_, in_.position() / 8, in_.byte_size())) {
          return fail(Status::Cancelled, "the conversion was cancelled");
        }
      }
      --block_left_;
      const int c = c_.decode(in_);
      if (c < 0 || c >= nc) return fail(Status::Decode, "{} data is damaged", name_);
      if (c < 256) {
        out_.put(static_cast<std::uint8_t>(c));
      } else {
        const std::size_t length = static_cast<std::size_t>(c - 256 + 3);
        int p = p_.decode(in_);
        if (p < 0) return fail(Status::Decode, "{} data is damaged", name_);
        std::uint32_t position = static_cast<std::uint32_t>(p);
        if (p > 1) position = (1u << (p - 1)) + in_.get(p - 1);
        if (!copy_spaces(out_, std::size_t{position} + 1, length, spaces_)) {
          return fail(Status::Decode, "{} match refers back before the start of the data", name_);
        }
      }
      if (in_.overrun()) return fail(Status::Decode, "{} data is truncated", name_);
    }
    return block_left_ != 0 || in_.position() + 8 <= in_.byte_size() * 8;
  }

 private:
  class Reader {
   public:
    explicit Reader(ByteView data) : bits_(data), size_(data.size()) {}
    std::uint32_t peek(int n) { return bits_.peek(n); }
    void skip(int n) { bits_.skip(n); }
    std::uint32_t get(int n) { return n == 0 ? 0 : bits_.get(n); }
    [[nodiscard]] std::uint64_t position() const { return bits_.position(); }
    [[nodiscard]] bool overrun() const { return bits_.overrun(); }
    [[nodiscard]] std::uint64_t byte_size() const { return size_; }

   private:
    MsbBitReader bits_;
    std::size_t size_;
  };

  Result<void> read_pt(LhCode& code, int count, int nbit, int special) {
    std::array<std::uint8_t, 32> len{};
    const int n = static_cast<int>(in_.get(nbit));
    code.single = -1;
    if (n == 0) {
      const int c = static_cast<int>(in_.get(nbit));
      if (c >= count) return fail(Status::Decode, "{} code table is invalid", name_);
      code.single = c;
      return {};
    }
    if (n > count) return fail(Status::Decode, "{} code table is invalid", name_);
    int i = 0;
    while (i < n) {
      int c = static_cast<int>(in_.get(3));
      if (c == 7) {
        while (in_.get(1) != 0) {
          if (++c > 16) return fail(Status::Decode, "{} code length is too long", name_);
        }
      }
      len[static_cast<std::size_t>(i++)] = static_cast<std::uint8_t>(c);
      if (i == special) {
        int zeros = static_cast<int>(in_.get(2));
        while (zeros-- > 0 && i < count) len[static_cast<std::size_t>(i++)] = 0;
      }
    }
    if (!code.table.build(std::span<const std::uint8_t>(len.data(), static_cast<std::size_t>(count)), false)) {
      return fail(Status::Decode, "{} code table is over-subscribed", name_);
    }
    return {};
  }

  Result<void> read_c() {
    std::array<std::uint8_t, nc> len{};
    const int n = static_cast<int>(in_.get(9));
    c_.single = -1;
    if (n == 0) {
      const int c = static_cast<int>(in_.get(9));
      if (c >= nc) return fail(Status::Decode, "{} code table is invalid", name_);
      c_.single = c;
      return {};
    }
    if (n > nc) return fail(Status::Decode, "{} code table is invalid", name_);
    int i = 0;
    while (i < n) {
      const int c = pt_.decode(in_);
      if (c < 0 || in_.overrun()) return fail(Status::Decode, "{} code table is damaged", name_);
      if (c > 2) {
        len[static_cast<std::size_t>(i++)] = static_cast<std::uint8_t>(c - 2);
        continue;
      }
      int zeros = c == 0 ? 1 : c == 1 ? static_cast<int>(in_.get(4)) + 3 : static_cast<int>(in_.get(9)) + 20;
      if (zeros > n - i) return fail(Status::Decode, "{} code table is damaged", name_);
      while (zeros-- > 0) len[static_cast<std::size_t>(i++)] = 0;
    }
    if (!c_.table.build(len, false)) return fail(Status::Decode, "{} code table is over-subscribed", name_);
    return {};
  }

  Reader in_;
  LegacyOutput& out_;
  int np_;
  int pbit_;
  bool spaces_;
  std::string_view name_;
  const DecodeOptions& options_;
  std::uint32_t block_left_ = 0;
  LhCode pt_;
  LhCode c_;
  LhCode p_;
};

// ---------------------------------------------------------------------------- -lh1-

class DynamicLh1 {
 public:
  static constexpr int n_char = 314;
  static constexpr int t = 2 * n_char - 1;
  static constexpr int root = t - 1;
  static constexpr std::uint32_t max_freq = 0x8000;

  DynamicLh1(ByteView data, LegacyOutput& out, const DecodeOptions& options) : in_(data), size_(data.size()), out_(out), options_(options) {
    for (int i = 0; i < n_char; ++i) {
      freq_[static_cast<std::size_t>(i)] = 1;
      son_[static_cast<std::size_t>(i)] = i + t;
      prnt_[static_cast<std::size_t>(i + t)] = i;
    }
    for (int i = 0, j = n_char; j <= root; i += 2, ++j) {
      freq_[static_cast<std::size_t>(j)] = freq_[static_cast<std::size_t>(i)] + freq_[static_cast<std::size_t>(i + 1)];
      son_[static_cast<std::size_t>(j)] = i;
      prnt_[static_cast<std::size_t>(i)] = j;
      prnt_[static_cast<std::size_t>(i + 1)] = j;
    }
    freq_[t] = 0xFFFFFFFFu;
    prnt_[root] = 0;
  }

  Result<bool> run() {
    // Upper six position bits: 1 code of 3 bits, 3 of 4, 8 of 5, 12 of 6, 24 of 7, 16 of 8.
    std::array<std::uint8_t, 64> plen{};
    for (std::size_t i = 0; i < 64; ++i) plen[i] = i < 1 ? 3 : i < 4 ? 4 : i < 12 ? 5 : i < 24 ? 6 : i < 48 ? 7 : 8;
    HuffmanTable ptable;
    (void)ptable.build(plen, false);
    std::uint32_t steps = 0;
    while (!out_.done()) {
      if (!out_.exact() && in_.position() >= std::uint64_t{size_} * 8) return false;
      int c = son_[root];
      int guard = 0;
      while (c < t) {
        c = son_[static_cast<std::size_t>(c + static_cast<int>(in_.get(1)))];
        if (++guard > t) return fail(Status::Decode, "-lh1- Huffman tree is damaged");
      }
      c -= t;
      update(c);
      if (c < 256) {
        out_.put(static_cast<std::uint8_t>(c));
      } else {
        const std::size_t length = static_cast<std::size_t>(c - 256 + 3);
        const int upper = ptable.decode(in_);
        const std::uint32_t position = (static_cast<std::uint32_t>(upper) << 6) | in_.get(6);
        if (!copy_spaces(out_, std::size_t{position} + 1, length, true)) {
          return fail(Status::Decode, "-lh1- match refers back before the start of the data");
        }
      }
      if (in_.overrun()) return fail(Status::Decode, "-lh1- data is truncated");
      if ((++steps & 0xFFFF) == 0 && !legacy_progress(options_, in_.position() / 8, size_)) {
        return fail(Status::Cancelled, "the conversion was cancelled");
      }
    }
    return in_.position() + 8 <= std::uint64_t{size_} * 8;
  }

 private:
  void reconstruct() {
    // Collect the leaves in the first half with halved counts.
    int j = 0;
    for (int i = 0; i < t; ++i) {
      if (son_[static_cast<std::size_t>(i)] >= t) {
        freq_[static_cast<std::size_t>(j)] = (freq_[static_cast<std::size_t>(i)] + 1) / 2;
        son_[static_cast<std::size_t>(j)] = son_[static_cast<std::size_t>(i)];
        ++j;
      }
    }
    // Rebuild the inner nodes, keeping the nodes sorted by count.
    for (int i = 0, k = n_char; k < t; i += 2, ++k) {
      const std::uint32_t f = freq_[static_cast<std::size_t>(i)] + freq_[static_cast<std::size_t>(i + 1)];
      int at = k - 1;
      while (f < freq_[static_cast<std::size_t>(at)]) --at;
      ++at;
      for (int m = k; m > at; --m) {
        freq_[static_cast<std::size_t>(m)] = freq_[static_cast<std::size_t>(m - 1)];
        son_[static_cast<std::size_t>(m)] = son_[static_cast<std::size_t>(m - 1)];
      }
      freq_[static_cast<std::size_t>(at)] = f;
      son_[static_cast<std::size_t>(at)] = i;
    }
    for (int i = 0; i < t; ++i) {
      const int k = son_[static_cast<std::size_t>(i)];
      prnt_[static_cast<std::size_t>(k)] = i;
      if (k < t) prnt_[static_cast<std::size_t>(k + 1)] = i;
    }
  }

  void update(int symbol) {
    if (freq_[root] == max_freq) reconstruct();
    int c = prnt_[static_cast<std::size_t>(symbol + t)];
    do {
      const std::uint32_t k = ++freq_[static_cast<std::size_t>(c)];
      int l = c + 1;
      if (k > freq_[static_cast<std::size_t>(l)]) {
        while (k > freq_[static_cast<std::size_t>(++l)]) {
        }
        --l;
        freq_[static_cast<std::size_t>(c)] = freq_[static_cast<std::size_t>(l)];
        freq_[static_cast<std::size_t>(l)] = k;
        const int i = son_[static_cast<std::size_t>(c)];
        prnt_[static_cast<std::size_t>(i)] = l;
        if (i < t) prnt_[static_cast<std::size_t>(i + 1)] = l;
        const int j = son_[static_cast<std::size_t>(l)];
        son_[static_cast<std::size_t>(l)] = i;
        prnt_[static_cast<std::size_t>(j)] = c;
        if (j < t) prnt_[static_cast<std::size_t>(j + 1)] = c;
        son_[static_cast<std::size_t>(c)] = j;
        c = l;
      }
      c = prnt_[static_cast<std::size_t>(c)];
    } while (c != 0);
  }

  MsbBitReader in_;
  std::size_t size_;
  LegacyOutput& out_;
  const DecodeOptions& options_;
  std::array<std::uint32_t, t + 1> freq_{};
  std::array<int, t + n_char> prnt_{};
  std::array<int, t> son_{};
};

// ---------------------------------------------------------------------------- -lzs- / -lz5-

Result<Bytes> decode_lzs(ByteView data, const DecodeOptions& options) {
  const int bits = options.window_bits;
  if (bits != 11 && bits != 12) return fail(Status::InvalidArgument, "-lzs-/-lz5- need window_bits 11 or 12");
  LegacyOutput out;
  CVT_TRY(out.start(options, bits == 11 ? "-lzs-" : "-lz5-"));
  std::array<std::uint8_t, 4096> ring{};
  const auto* in = reinterpret_cast<const std::uint8_t*>(data.data());
  const std::size_t size = data.size();
  bool more = false;
  if (bits == 11) {
    ring.fill(' ');
    std::uint32_t w = 2048 - 17;
    MsbBitReader r(data);
    while (!out.done()) {
      if (!out.exact() && r.position() + 9 > std::uint64_t{size} * 8) break;
      if (r.get(1) != 0) {
        const auto c = static_cast<std::uint8_t>(r.get(8));
        out.put(c);
        ring[w] = c;
        w = (w + 1) & 2047u;
      } else {
        std::uint32_t pos = r.get(11);
        const std::uint32_t len = r.get(4) + 2;
        for (std::uint32_t k = 0; k < len && !out.done(); ++k) {
          const std::uint8_t c = ring[pos];
          out.put(c);
          ring[w] = c;
          w = (w + 1) & 2047u;
          pos = (pos + 1) & 2047u;
        }
      }
      if (r.overrun()) return fail(Status::Decode, "-lzs- data is truncated");
    }
    more = r.position() + 8 <= std::uint64_t{size} * 8;
  } else {
    // LArc's initial ring buffer: 13 copies of every byte value, 0..255,
    // 255..0, 128 zeros and spaces up to the start position 4096 - 18.
    std::size_t at = 0;
    for (int v = 0; v < 256; ++v) {
      for (int k = 0; k < 13; ++k) ring[at++] = static_cast<std::uint8_t>(v);
    }
    for (int v = 0; v < 256; ++v) ring[at++] = static_cast<std::uint8_t>(v);
    for (int v = 255; v >= 0; --v) ring[at++] = static_cast<std::uint8_t>(v);
    for (int k = 0; k < 128; ++k) ring[at++] = 0;
    while (at < 4096) ring[at++] = ' ';
    std::uint32_t w = 4096 - 18;
    std::size_t pos = 0;
    std::uint32_t flags = 0;
    int nflags = 0;
    while (!out.done()) {
      if (nflags == 0) {
        if (pos >= size) break;
        flags = in[pos++];
        nflags = 8;
      }
      --nflags;
      const bool literal = (flags & 1u) != 0;
      flags >>= 1;
      if (literal) {
        if (pos >= size) break;
        const std::uint8_t c = in[pos++];
        out.put(c);
        ring[w] = c;
        w = (w + 1) & 4095u;
      } else {
        if (size - pos < 2) {
          if (pos >= size) break;
          return fail(Status::Decode, "-lz5- data is truncated");
        }
        std::uint32_t p = in[pos] | ((in[pos + 1] & 0xF0u) << 4);
        const std::uint32_t len = (in[pos + 1] & 0x0Fu) + 3;
        pos += 2;
        for (std::uint32_t k = 0; k < len && !out.done(); ++k) {
          const std::uint8_t c = ring[p];
          out.put(c);
          ring[w] = c;
          w = (w + 1) & 4095u;
          p = (p + 1) & 4095u;
        }
      }
    }
    more = pos < size;
  }
  return out.finish(more);
}

// ---------------------------------------------------------------------------- ARJ method 4

Result<Bytes> decode_arj4(ByteView data, LegacyOutput& out, const DecodeOptions& options) {
  MsbBitReader r(data);
  const std::uint64_t total_bits = std::uint64_t{data.size()} * 8;
  std::uint32_t steps = 0;
  while (!out.done()) {
    if (!out.exact() && r.position() + 9 > total_bits) break;
    // Length: up to seven 1 bits (stopped by a 0 bit) select a width.
    std::uint32_t plus = 0;
    int width = 0;
    for (std::uint32_t pwr = 1; width < 7 && r.get(1) != 0; ++width, pwr <<= 1) plus += pwr;
    const std::uint32_t c = (width != 0 ? r.get(width) : 0u) + plus;
    if (c == 0) {
      out.put(static_cast<std::uint8_t>(r.get(8)));
    } else {
      const std::size_t length = c - 1 + 3;
      std::uint32_t pplus = 0;
      int pwidth = 9;
      for (std::uint32_t pwr = 1u << 9; pwidth < 13 && r.get(1) != 0; ++pwidth, pwr <<= 1) pplus += pwr;
      const std::uint32_t ptr = r.get(pwidth) + pplus;
      if (!out.copy(std::size_t{ptr} + 1, length)) {
        return fail(Status::Decode, "ARJ match refers back before the start of the data");
      }
    }
    if (r.overrun()) return fail(Status::Decode, "ARJ data is truncated");
    if ((++steps & 0xFFFF) == 0 && !legacy_progress(options, r.position() / 8, data.size())) {
      return fail(Status::Cancelled, "the conversion was cancelled");
    }
  }
  return out.finish(r.position() + 8 <= total_bits);
}

// ---------------------------------------------------------------------------- entry points

Result<Bytes> decode_lh(ByteView data, const DecodeOptions& options) {
  const int bits = options.window_bits;
  if (options.flags == 1) {
    if (bits != 12 && bits != 0) return fail(Status::InvalidArgument, "-lh1- needs window_bits 12");
    LegacyOutput out;
    CVT_TRY(out.start(options, "-lh1-"));
    DynamicLh1 decoder(data, out, options);
    CVT_TRY_ASSIGN(const bool more, decoder.run());
    return out.finish(more);
  }
  if (bits != 12 && bits != 13 && bits != 15 && bits != 16) {
    return fail(Status::InvalidArgument, "-lhN- needs window_bits 12, 13, 15 or 16 (lh4..lh7)");
  }
  LegacyOutput out;
  CVT_TRY(out.start(options, "LHA"));
  StaticLh decoder(data, out, bits + 1, bits <= 13 ? 4 : 5, true, "LHA", options);
  CVT_TRY_ASSIGN(const bool more, decoder.run());
  return out.finish(more);
}

Result<Bytes> decode_arj(ByteView data, const DecodeOptions& options) {
  LegacyOutput out;
  CVT_TRY(out.start(options, "ARJ"));
  if (options.flags == 4) return decode_arj4(data, out, options);
  if (options.flags < 1 || options.flags > 3) {
    return fail(Status::InvalidArgument, "ARJ method {} is not a compression method (1..4)", options.flags);
  }
  StaticLh decoder(data, out, 17, 5, false, "ARJ", options);
  CVT_TRY_ASSIGN(const bool more, decoder.run());
  return out.finish(more);
}

constexpr std::array<CodecImpl, 3> table = {{
    {Method::Lh, decode_lh, nullptr, nullptr, nullptr, nullptr},
    {Method::Lzs, decode_lzs, nullptr, nullptr, nullptr, nullptr},
    {Method::Arj, decode_arj, nullptr, nullptr, nullptr, nullptr},
}};

}  // namespace

std::span<const CodecImpl> lzh_codecs() { return table; }

}  // namespace cvt::compression::detail
