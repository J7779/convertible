// Convertible core - archive family: Zstandard decoder (RFC 8878) and codec table.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <optional>

#include "families/archive/checksums.hpp"
#include "families/archive/codec_impl.hpp"
#include "families/archive/codecs/zstd_internal.hpp"

namespace cvt::compression::detail {

Result<Bytes> zstd_encode(ByteView data, const EncodeOptions& options);  // zstd_encoder.cpp

namespace zstd {

bool spread_symbols(const std::int16_t* norm, std::size_t symbols, int log, std::vector<std::uint16_t>& cells) {
  const std::uint32_t size = 1u << log;
  cells.assign(size, 0);
  std::uint32_t high = size - 1;
  for (std::size_t s = 0; s < symbols; ++s) {
    if (norm[s] == -1) {
      if (high == 0 && s + 1 < symbols) return false;
      cells[high--] = static_cast<std::uint16_t>(s);
    }
  }
  const std::uint32_t step = (size >> 1) + (size >> 3) + 3;
  const std::uint32_t mask = size - 1;
  std::uint32_t pos = 0;
  for (std::size_t s = 0; s < symbols; ++s) {
    for (int i = 0; i < norm[s]; ++i) {
      cells[pos] = static_cast<std::uint16_t>(s);
      do {
        pos = (pos + step) & mask;
      } while (pos > high);
    }
  }
  return pos == 0;
}

bool build_fse_table(const std::int16_t* norm, std::size_t symbols, int log, std::vector<FseEntry>& table) {
  std::vector<std::uint16_t> cells;
  if (!spread_symbols(norm, symbols, log, cells)) return false;
  const std::uint32_t size = 1u << log;
  std::vector<std::uint32_t> next(symbols);
  for (std::size_t s = 0; s < symbols; ++s) next[s] = norm[s] == -1 ? 1u : static_cast<std::uint32_t>(norm[s]);
  table.assign(size, FseEntry{});
  for (std::uint32_t u = 0; u < size; ++u) {
    const std::uint16_t s = cells[u];
    const std::uint32_t state = next[s]++;
    const int bits = log - (std::bit_width(state) - 1);
    table[u].symbol = s;
    table[u].bits = static_cast<std::uint8_t>(bits);
    table[u].base = static_cast<std::uint16_t>((state << bits) - size);
  }
  return true;
}

}  // namespace zstd

namespace {

using zstd::FseEntry;

// Reads a bitstream backwards from its end: the stream is one little-endian
// number whose highest set bit is a marker; bits are taken from the top.
class BackwardBits {
 public:
  bool init(ByteView data) {
    data_ = reinterpret_cast<const std::uint8_t*>(data.data());
    size_ = data.size();
    if (size_ == 0) return false;
    const std::uint8_t last = data_[size_ - 1];
    if (last == 0) return false;
    left_ = static_cast<std::int64_t>(size_) * 8 - (8 - (std::bit_width(last) - 1));
    return true;
  }
  /// The next `n` bits (n <= 56) without consuming them; bits before the
  /// start of the stream read as zero.
  [[nodiscard]] std::uint64_t peek(int n) const {
    if (n == 0) return 0;
    const std::int64_t from = left_ - n;
    if (from >= 0) return load(static_cast<std::uint64_t>(from), n);
    const std::int64_t have = left_;
    if (have <= 0) return 0;
    return load(0, static_cast<int>(have)) << (n - have);
  }
  void consume(int n) { left_ -= n; }
  std::uint64_t read(int n) {
    const std::uint64_t v = peek(n);
    left_ -= n;
    return v;
  }
  [[nodiscard]] bool overflowed() const noexcept { return left_ < 0; }
  [[nodiscard]] bool finished() const noexcept { return left_ == 0; }
  [[nodiscard]] std::int64_t left() const noexcept { return left_; }

 private:
  std::uint64_t load(std::uint64_t bit, int n) const {
    const std::uint64_t byte = bit >> 3;
    const int shift = static_cast<int>(bit & 7u);
    std::uint64_t v = 0;
    const std::uint64_t avail = std::min<std::uint64_t>(8, size_ - byte);
    for (std::uint64_t i = 0; i < avail; ++i) v |= static_cast<std::uint64_t>(data_[byte + i]) << (8 * i);
    return (v >> shift) & ((std::uint64_t{1} << n) - 1u);
  }
  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
  std::int64_t left_ = 0;
};

// Forward little-endian bit reader over a span (FSE table descriptions).
class ForwardBits {
 public:
  explicit ForwardBits(ByteView d) : data_(d) {}
  std::uint32_t peek(int n) const {
    std::uint64_t v = 0;
    const std::size_t byte = pos_ >> 3;
    for (std::size_t i = 0; i < 5; ++i) {
      if (byte + i < data_.size()) v |= std::to_integer<std::uint64_t>(data_[byte + i]) << (8 * i);
    }
    return static_cast<std::uint32_t>((v >> (pos_ & 7)) & ((std::uint64_t{1} << n) - 1u));
  }
  void skip(int n) { pos_ += static_cast<std::size_t>(n); }
  [[nodiscard]] std::size_t bytes_used() const noexcept { return (pos_ + 7) >> 3; }
  [[nodiscard]] bool overrun() const noexcept { return pos_ > data_.size() * 8; }

 private:
  ByteView data_;
  std::size_t pos_ = 0;
};

// FSE table description (RFC 8878 4.1.1). Returns the bytes consumed.
Result<std::size_t> read_fse_description(ByteView data, int max_log, std::size_t max_symbols,
                                         std::vector<std::int16_t>& norm, int& log) {
  ForwardBits in(data);
  log = static_cast<int>(in.peek(4)) + 5;
  in.skip(4);
  if (log > max_log) return fail(Status::Decode, "Zstandard FSE accuracy log {} is too large", log);
  norm.assign(max_symbols, 0);
  std::int32_t remaining = (1 << log) + 1;
  std::int32_t threshold = 1 << log;
  int bits = log + 1;
  std::size_t symbol = 0;
  bool previous_zero = false;
  while (remaining > 1 && symbol < max_symbols) {
    if (previous_zero) {
      std::size_t n0 = symbol;
      while (in.peek(16) == 0xFFFFu) {
        n0 += 24;
        in.skip(16);
        if (in.overrun()) return fail(Status::Decode, "Zstandard FSE table is truncated");
      }
      while (in.peek(2) == 3) {
        n0 += 3;
        in.skip(2);
      }
      n0 += in.peek(2);
      in.skip(2);
      if (n0 > max_symbols) return fail(Status::Decode, "Zstandard FSE table has too many symbols");
      symbol = n0;
      if (symbol >= max_symbols) break;
    }
    const std::int32_t max = 2 * threshold - 1 - remaining;
    std::int32_t count;
    const auto low = static_cast<std::int32_t>(in.peek(bits - 1));
    if (low < max) {
      count = low;
      in.skip(bits - 1);
    } else {
      count = static_cast<std::int32_t>(in.peek(bits));
      if (count >= threshold) count -= max;
      in.skip(bits);
    }
    --count;
    remaining -= count < 0 ? -count : count;
    norm[symbol++] = static_cast<std::int16_t>(count);
    previous_zero = count == 0;
    while (remaining < threshold) {
      --bits;
      threshold >>= 1;
    }
    if (in.overrun()) return fail(Status::Decode, "Zstandard FSE table is truncated");
  }
  if (remaining != 1 || in.overrun()) return fail(Status::Decode, "Zstandard FSE table is invalid");
  norm.resize(symbol);
  return in.bytes_used();
}

struct HuffTable {
  int max_bits = 0;
  std::vector<std::uint8_t> symbol;  // 1 << max_bits entries
  std::vector<std::uint8_t> bits;
  bool valid = false;
};

Result<std::size_t> read_huffman(ByteView data, HuffTable& table) {
  if (data.empty()) return fail(Status::Decode, "Zstandard Huffman description is missing");
  const std::uint8_t header = std::to_integer<std::uint8_t>(data[0]);
  std::vector<std::uint8_t> weights;
  std::size_t used = 0;
  if (header >= 128) {
    const std::size_t count = header - 127u;
    const std::size_t bytes = (count + 1) / 2;
    if (1 + bytes > data.size()) return fail(Status::Decode, "Zstandard Huffman description is truncated");
    for (std::size_t i = 0; i < count; ++i) {
      const std::uint8_t b = std::to_integer<std::uint8_t>(data[1 + i / 2]);
      weights.push_back(i % 2 == 0 ? b >> 4 : b & 15u);
    }
    used = 1 + bytes;
  } else {
    if (1u + header > data.size()) return fail(Status::Decode, "Zstandard Huffman description is truncated");
    const ByteView body = data.subspan(1, header);
    std::vector<std::int16_t> norm;
    int log = 0;
    CVT_TRY_ASSIGN(const std::size_t desc, read_fse_description(body, 6, 256, norm, log));
    std::vector<FseEntry> fse;
    if (!zstd::build_fse_table(norm.data(), norm.size(), log, fse)) return fail(Status::Decode, "Zstandard Huffman weights table is invalid");
    BackwardBits bits;
    if (desc >= body.size() || !bits.init(body.subspan(desc))) return fail(Status::Decode, "Zstandard Huffman weights are damaged");
    std::uint32_t s1 = static_cast<std::uint32_t>(bits.read(log));
    std::uint32_t s2 = static_cast<std::uint32_t>(bits.read(log));
    while (true) {
      if (weights.size() >= 255) return fail(Status::Decode, "Zstandard Huffman weights are damaged");
      weights.push_back(static_cast<std::uint8_t>(fse[s1].symbol));
      s1 = fse[s1].base + static_cast<std::uint32_t>(bits.read(fse[s1].bits));
      if (bits.overflowed()) {
        weights.push_back(static_cast<std::uint8_t>(fse[s2].symbol));
        break;
      }
      weights.push_back(static_cast<std::uint8_t>(fse[s2].symbol));
      s2 = fse[s2].base + static_cast<std::uint32_t>(bits.read(fse[s2].bits));
      if (bits.overflowed()) {
        weights.push_back(static_cast<std::uint8_t>(fse[s1].symbol));
        break;
      }
    }
    used = 1 + header;
  }
  // The last weight is implied.
  std::uint32_t total = 0;
  for (const std::uint8_t w : weights) {
    if (w > 11) return fail(Status::Decode, "Zstandard Huffman weight is too large");
    if (w > 0) total += 1u << (w - 1);
  }
  if (total == 0 || weights.size() > 255) return fail(Status::Decode, "Zstandard Huffman weights are invalid");
  const int max_bits = std::bit_width(total);
  const std::uint32_t rest = (1u << max_bits) - total;
  if (!std::has_single_bit(rest) || max_bits > 11) return fail(Status::Decode, "Zstandard Huffman weights are invalid");
  weights.push_back(static_cast<std::uint8_t>(std::bit_width(rest)));
  table.max_bits = max_bits;
  table.symbol.assign(std::size_t{1} << max_bits, 0);
  table.bits.assign(std::size_t{1} << max_bits, 0);
  std::uint32_t pos = 0;
  for (int w = 1; w <= max_bits; ++w) {
    for (std::size_t s = 0; s < weights.size(); ++s) {
      if (weights[s] != w) continue;
      const std::uint32_t span = 1u << (w - 1);
      for (std::uint32_t k = 0; k < span; ++k) {
        table.symbol[pos + k] = static_cast<std::uint8_t>(s);
        table.bits[pos + k] = static_cast<std::uint8_t>(max_bits + 1 - w);
      }
      pos += span;
    }
  }
  if (pos != (1u << max_bits)) return fail(Status::Decode, "Zstandard Huffman weights are invalid");
  table.valid = true;
  return used;
}

Result<void> huffman_stream(ByteView data, const HuffTable& t, std::byte* out, std::size_t count) {
  BackwardBits bits;
  if (!bits.init(data)) return fail(Status::Decode, "Zstandard literal stream is damaged");
  for (std::size_t i = 0; i < count; ++i) {
    const auto v = static_cast<std::uint32_t>(bits.peek(t.max_bits));
    bits.consume(t.bits[v]);
    out[i] = static_cast<std::byte>(t.symbol[v]);
  }
  if (!bits.finished()) return fail(Status::Decode, "Zstandard literal stream is damaged");
  return {};
}

struct SeqTable {
  std::vector<FseEntry> table;
  int log = 0;
  bool valid = false;
};

struct FrameState {
  HuffTable huff;
  SeqTable ll, of, ml;
  std::array<std::uint32_t, 3> reps = {1, 4, 8};
};

std::uint32_t le_n(ByteView d, std::size_t at, std::size_t n) {
  std::uint32_t v = 0;
  for (std::size_t i = 0; i < n; ++i) v |= std::to_integer<std::uint32_t>(d[at + i]) << (8 * i);
  return v;
}

template <std::size_t N>
bool build_default(const std::array<std::int16_t, N>& norm, int log, SeqTable& t) {
  t.log = log;
  t.valid = zstd::build_fse_table(norm.data(), N, log, t.table);
  return t.valid;
}

Result<std::size_t> read_seq_table(ByteView data, int mode, int max_log, std::size_t max_symbols, SeqTable& t,
                                   int which) {
  switch (mode) {
    case 0:
      if (which == 0) build_default(zstd::ll_default, zstd::ll_default_log, t);
      if (which == 1) build_default(zstd::of_default, zstd::of_default_log, t);
      if (which == 2) build_default(zstd::ml_default, zstd::ml_default_log, t);
      return std::size_t{0};
    case 1: {
      if (data.empty()) return fail(Status::Decode, "Zstandard sequence section is truncated");
      const std::uint8_t sym = std::to_integer<std::uint8_t>(data[0]);
      if (sym >= max_symbols) return fail(Status::Decode, "Zstandard RLE symbol is out of range");
      t.table.assign(1, FseEntry{sym, 0, 0});
      t.log = 0;
      t.valid = true;
      return std::size_t{1};
    }
    case 2: {
      std::vector<std::int16_t> norm;
      int log = 0;
      CVT_TRY_ASSIGN(const std::size_t used, read_fse_description(data, max_log, max_symbols, norm, log));
      if (!zstd::build_fse_table(norm.data(), norm.size(), log, t.table)) {
        return fail(Status::Decode, "Zstandard FSE table is invalid");
      }
      t.log = log;
      t.valid = true;
      return used;
    }
    default:
      if (!t.valid) return fail(Status::Decode, "Zstandard block repeats a table that does not exist");
      return std::size_t{0};
  }
}

// Decode one compressed block, appending to `out`; `frame_start` bounds back-references.
Result<void> compressed_block(ByteView block, FrameState& fs, Bytes& out, std::size_t frame_start,
                              std::uint64_t max_output) {
  if (block.empty()) return fail(Status::Decode, "Zstandard block is empty");
  // ---- literals
  const std::uint8_t b0 = std::to_integer<std::uint8_t>(block[0]);
  const int type = b0 & 3;
  const int format = (b0 >> 2) & 3;
  std::size_t pos = 0;
  std::size_t regen = 0;
  std::size_t comp = 0;
  int streams = 1;
  if (type < 2) {
    if (format == 0 || format == 2) {
      regen = b0 >> 3;
      pos = 1;
    } else if (format == 1) {
      if (block.size() < 2) return fail(Status::Decode, "Zstandard literals header is truncated");
      regen = le_n(block, 0, 2) >> 4;
      pos = 2;
    } else {
      if (block.size() < 3) return fail(Status::Decode, "Zstandard literals header is truncated");
      regen = le_n(block, 0, 3) >> 4;
      pos = 3;
    }
  } else {
    const std::size_t header_len = format < 2 ? 3 : (format == 2 ? 4 : 5);
    if (block.size() < header_len) return fail(Status::Decode, "Zstandard literals header is truncated");
    const std::uint64_t h = header_len == 5 ? (static_cast<std::uint64_t>(le_n(block, 0, 4)) |
                                               (std::to_integer<std::uint64_t>(block[4]) << 32))
                                            : le_n(block, 0, header_len);
    const int field = format < 2 ? 10 : (format == 2 ? 14 : 18);
    regen = static_cast<std::size_t>((h >> 4) & ((std::uint64_t{1} << field) - 1u));
    comp = static_cast<std::size_t>((h >> (4 + field)) & ((std::uint64_t{1} << field) - 1u));
    streams = format == 0 ? 1 : 4;
    pos = header_len;
  }
  if (regen > zstd::block_max) return fail(Status::Decode, "Zstandard literals are too large");
  Bytes literals(regen);
  if (type == 0) {
    if (pos + regen > block.size()) return fail(Status::Decode, "Zstandard literals are truncated");
    if (regen != 0) std::memcpy(literals.data(), block.data() + pos, regen);
    pos += regen;
  } else if (type == 1) {
    if (pos >= block.size()) return fail(Status::Decode, "Zstandard literals are truncated");
    std::fill(literals.begin(), literals.end(), block[pos]);
    pos += 1;
  } else {
    if (pos + comp > block.size()) return fail(Status::Decode, "Zstandard literals are truncated");
    ByteView lit = block.subspan(pos, comp);
    pos += comp;
    if (type == 2) {
      CVT_TRY_ASSIGN(const std::size_t used, read_huffman(lit, fs.huff));
      lit = lit.subspan(used);
    } else if (!fs.huff.valid) {
      return fail(Status::Decode, "Zstandard block repeats a Huffman table that does not exist");
    }
    if (streams == 1) {
      CVT_TRY(huffman_stream(lit, fs.huff, literals.data(), regen));
    } else {
      if (lit.size() < 6) return fail(Status::Decode, "Zstandard literal jump table is truncated");
      const std::size_t s1 = le_n(lit, 0, 2), s2 = le_n(lit, 2, 2), s3 = le_n(lit, 4, 2);
      if (6 + s1 + s2 + s3 > lit.size()) return fail(Status::Decode, "Zstandard literal streams are damaged");
      const std::size_t s4 = lit.size() - 6 - s1 - s2 - s3;
      const std::size_t part = (regen + 3) / 4;
      if (part * 3 > regen) return fail(Status::Decode, "Zstandard literal streams are damaged");
      std::size_t off = 6;
      const std::array<std::size_t, 4> sizes = {s1, s2, s3, s4};
      for (std::size_t k = 0; k < 4; ++k) {
        const std::size_t n = k < 3 ? part : regen - 3 * part;
        CVT_TRY(huffman_stream(lit.subspan(off, sizes[k]), fs.huff, literals.data() + k * part, n));
        off += sizes[k];
      }
    }
  }
  // ---- sequences
  if (pos >= block.size()) return fail(Status::Decode, "Zstandard sequence section is missing");
  std::size_t nseq = std::to_integer<std::size_t>(block[pos++]);
  if (nseq >= 128) {
    if (nseq < 255) {
      if (pos >= block.size()) return fail(Status::Decode, "Zstandard sequence section is truncated");
      nseq = ((nseq - 128) << 8) + std::to_integer<std::size_t>(block[pos++]);
    } else {
      if (pos + 2 > block.size()) return fail(Status::Decode, "Zstandard sequence section is truncated");
      nseq = le_n(block, pos, 2) + 0x7F00;
      pos += 2;
    }
  }
  const std::size_t block_out_start = out.size();
  const auto put = [&](const std::byte* p, std::size_t n) -> Result<void> {
    CVT_TRY(check_output_room(out.size(), n, max_output));
    out.insert(out.end(), p, p + n);
    return {};
  };
  if (nseq == 0) {
    if (pos != block.size()) return fail(Status::Decode, "Zstandard block has trailing data");
    return put(literals.data(), literals.size());
  }
  if (pos >= block.size()) return fail(Status::Decode, "Zstandard sequence section is truncated");
  const std::uint8_t modes = std::to_integer<std::uint8_t>(block[pos++]);
  if ((modes & 3u) != 0) return fail(Status::Decode, "Zstandard sequence modes are invalid");
  CVT_TRY_ASSIGN(std::size_t used, read_seq_table(block.subspan(pos), modes >> 6, 9, 36, fs.ll, 0));
  pos += used;
  CVT_TRY_ASSIGN(used, read_seq_table(block.subspan(pos), (modes >> 4) & 3, 8, 32, fs.of, 1));
  pos += used;
  CVT_TRY_ASSIGN(used, read_seq_table(block.subspan(pos), (modes >> 2) & 3, 9, 53, fs.ml, 2));
  pos += used;
  if (pos > block.size()) return fail(Status::Decode, "Zstandard sequence section is truncated");
  BackwardBits bits;
  if (!bits.init(block.subspan(pos))) return fail(Status::Decode, "Zstandard sequence bitstream is damaged");
  std::uint32_t ll_state = static_cast<std::uint32_t>(bits.read(fs.ll.log));
  std::uint32_t of_state = static_cast<std::uint32_t>(bits.read(fs.of.log));
  std::uint32_t ml_state = static_cast<std::uint32_t>(bits.read(fs.ml.log));
  std::size_t lit_pos = 0;
  auto& reps = fs.reps;
  for (std::size_t i = 0; i < nseq; ++i) {
    const FseEntry& le = fs.ll.table[ll_state];
    const FseEntry& oe = fs.of.table[of_state];
    const FseEntry& me = fs.ml.table[ml_state];
    if (oe.symbol > 31 || le.symbol > 35 || me.symbol > 52) {
      return fail(Status::Decode, "Zstandard sequence code is invalid");
    }
    const std::uint32_t of_value = (1u << oe.symbol) + static_cast<std::uint32_t>(bits.read(oe.symbol));
    const std::uint32_t ml = zstd::ml_base[me.symbol] + static_cast<std::uint32_t>(bits.read(zstd::ml_bits[me.symbol]));
    const std::uint32_t ll = zstd::ll_base[le.symbol] + static_cast<std::uint32_t>(bits.read(zstd::ll_bits[le.symbol]));
    std::uint32_t offset;
    if (of_value > 3) {
      offset = of_value - 3;
      reps = {offset, reps[0], reps[1]};
    } else {
      const std::uint32_t index = of_value - 1 + (ll == 0 ? 1u : 0u);
      if (index == 0) {
        offset = reps[0];
      } else {
        offset = index == 3 ? reps[0] - 1 : reps[index];
        if (index >= 2) reps[2] = reps[1];
        reps[1] = reps[0];
        reps[0] = offset;
      }
    }
    if (i + 1 < nseq) {
      ll_state = le.base + static_cast<std::uint32_t>(bits.read(le.bits));
      ml_state = me.base + static_cast<std::uint32_t>(bits.read(me.bits));
      of_state = oe.base + static_cast<std::uint32_t>(bits.read(oe.bits));
    }
    if (bits.overflowed()) return fail(Status::Decode, "Zstandard sequence bitstream is truncated");
    if (ll > literals.size() - lit_pos) return fail(Status::Decode, "Zstandard sequence uses more literals than exist");
    CVT_TRY(put(literals.data() + lit_pos, ll));
    lit_pos += ll;
    if (offset == 0 || offset > out.size() - frame_start) return fail(Status::Decode, "Zstandard match offset is out of range");
    CVT_TRY(check_output_room(out.size(), ml, max_output));
    const std::size_t from = out.size() - offset;
    if (offset >= ml) {
      out.insert(out.end(), out.begin() + static_cast<std::ptrdiff_t>(from),
                 out.begin() + static_cast<std::ptrdiff_t>(from + ml));
    } else {
      for (std::uint32_t k = 0; k < ml; ++k) out.push_back(out[from + k]);
    }
    if (out.size() - block_out_start > zstd::block_max) return fail(Status::Decode, "Zstandard block is too large");
  }
  if (!bits.finished()) return fail(Status::Decode, "Zstandard sequence bitstream is damaged");
  CVT_TRY(put(literals.data() + lit_pos, literals.size() - lit_pos));
  if (out.size() - block_out_start > zstd::block_max) return fail(Status::Decode, "Zstandard block is too large");
  return {};
}

// Decode one frame (or skip a skippable frame); returns the bytes used.
Result<std::size_t> decode_frame(ByteView data, Bytes& out, const DecodeOptions& o) {
  if (data.size() < 4) return fail(Status::Decode, "Zstandard frame is truncated");
  const std::uint32_t magic = le_n(data, 0, 4);
  if ((magic & 0xFFFFFFF0u) == 0x184D2A50u) {
    if (data.size() < 8) return fail(Status::Decode, "Zstandard skippable frame is truncated");
    const std::uint64_t size = le_n(data, 4, 4);
    if (size > data.size() - 8) return fail(Status::Decode, "Zstandard skippable frame is truncated");
    return static_cast<std::size_t>(8 + size);
  }
  if (magic != zstd::frame_magic) return fail(Status::Decode, "not a Zstandard frame");
  std::size_t pos = 4;
  if (pos >= data.size()) return fail(Status::Decode, "Zstandard frame header is truncated");
  const std::uint8_t fhd = std::to_integer<std::uint8_t>(data[pos++]);
  const int fcs_flag = fhd >> 6;
  const bool single = ((fhd >> 5) & 1u) != 0;
  if (fhd & 0x08u) return fail(Status::Decode, "Zstandard frame header uses a reserved bit");
  const bool checksum = ((fhd >> 2) & 1u) != 0;
  const int dict_flag = fhd & 3;
  std::uint64_t window = 0;
  if (!single) {
    if (pos >= data.size()) return fail(Status::Decode, "Zstandard frame header is truncated");
    const std::uint8_t wd = std::to_integer<std::uint8_t>(data[pos++]);
    const std::uint64_t base = std::uint64_t{1} << (10 + (wd >> 3));
    window = base + (base / 8) * (wd & 7u);
  }
  static constexpr std::array<std::size_t, 4> dict_sizes = {0, 1, 2, 4};
  const std::size_t dsize = dict_sizes[static_cast<std::size_t>(dict_flag)];
  if (pos + dsize > data.size()) return fail(Status::Decode, "Zstandard frame header is truncated");
  if (dsize != 0 && le_n(data, pos, dsize) != 0) {
    return fail(Status::Decode, "this Zstandard frame needs a dictionary (id {}), which is not available",
                le_n(data, pos, dsize));
  }
  pos += dsize;
  const std::size_t fsize = fcs_flag == 0 ? (single ? 1 : 0) : (std::size_t{1} << fcs_flag);
  if (pos + fsize > data.size()) return fail(Status::Decode, "Zstandard frame header is truncated");
  std::optional<std::uint64_t> content_size;
  if (fsize == 1) content_size = std::to_integer<std::uint64_t>(data[pos]);
  if (fsize == 2) content_size = le_n(data, pos, 2) + 256u;
  if (fsize == 4) content_size = le_n(data, pos, 4);
  if (fsize == 8) {
    content_size = static_cast<std::uint64_t>(le_n(data, pos, 4)) | (static_cast<std::uint64_t>(le_n(data, pos + 4, 4)) << 32);
  }
  pos += fsize;
  if (single) window = content_size.value_or(0);
  const std::size_t frame_start = out.size();
  if (content_size && o.stop_after == 0 && *content_size > o.max_output - std::min<std::uint64_t>(o.max_output, out.size())) {
    return fail(Status::Limit, "decompressed data exceeds the limit of {} bytes", o.max_output);
  }
  const std::uint64_t block_limit = std::min<std::uint64_t>(std::max<std::uint64_t>(window, 1), zstd::block_max);
  const std::uint64_t max = o.stop_after != 0 ? std::min(o.max_output, o.stop_after + zstd::block_max) : o.max_output;
  FrameState fs;
  while (true) {
    if (pos + 3 > data.size()) return fail(Status::Decode, "Zstandard block header is truncated");
    const std::uint32_t bh = le_n(data, pos, 3);
    pos += 3;
    const bool last = (bh & 1u) != 0;
    const int btype = static_cast<int>((bh >> 1) & 3u);
    const std::size_t bsize = bh >> 3;
    if (btype == 3) return fail(Status::Decode, "Zstandard block type is reserved");
    if (btype == 1) {
      if (bsize > zstd::block_max) return fail(Status::Decode, "Zstandard block is too large");
      if (pos >= data.size()) return fail(Status::Decode, "Zstandard block is truncated");
      CVT_TRY(check_output_room(out.size(), bsize, max));
      out.insert(out.end(), bsize, data[pos]);
      pos += 1;
    } else {
      if (bsize > zstd::block_max || (btype == 2 && bsize > block_limit)) {
        return fail(Status::Decode, "Zstandard block is larger than the window allows");
      }
      if (pos + bsize > data.size()) return fail(Status::Decode, "Zstandard block is truncated");
      if (btype == 0) {
        CVT_TRY(check_output_room(out.size(), bsize, max));
        out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(pos),
                   data.begin() + static_cast<std::ptrdiff_t>(pos + bsize));
      } else {
        CVT_TRY(compressed_block(data.subspan(pos, bsize), fs, out, frame_start, max));
      }
      pos += bsize;
    }
    if (o.stop_after != 0 && out.size() >= o.stop_after) return data.size();
    if (!o.progress.report(static_cast<double>(pos) / static_cast<double>(data.size()))) {
      return fail(Status::Cancelled, "the conversion was cancelled");
    }
    if (last) break;
  }
  if (content_size && out.size() - frame_start != *content_size) {
    return fail(Status::Decode, "Zstandard frame size does not match its header");
  }
  if (checksum) {
    if (pos + 4 > data.size()) return fail(Status::Decode, "Zstandard frame checksum is truncated");
    if (o.verify_checksums &&
        static_cast<std::uint32_t>(archive::xxh64(ByteView(out).subspan(frame_start))) != le_n(data, pos, 4)) {
      return fail(Status::Decode, "Zstandard content checksum mismatch");
    }
    pos += 4;
  }
  return pos;
}

Result<Bytes> decode_zstd(ByteView data, const DecodeOptions& o) {
  Bytes out;
  std::size_t pos = 0;
  bool any = false;
  while (pos < data.size()) {
    CVT_TRY_ASSIGN(const std::size_t used, decode_frame(data.subspan(pos), out, o));
    pos += used;
    any = true;
    if (o.stop_after != 0 && out.size() >= o.stop_after) {
      out.resize(static_cast<std::size_t>(o.stop_after));
      return out;
    }
  }
  if (!any) return fail(Status::Decode, "not a Zstandard stream");
  if (o.output_size && out.size() != *o.output_size) {
    return fail(Status::Decode, "Zstandard data decodes to {} bytes, expected {}", out.size(), *o.output_size);
  }
  return out;
}

constexpr std::array<CodecImpl, 1> table = {{
    {Method::Zstd, decode_zstd, zstd_encode, nullptr, nullptr, nullptr},
}};

}  // namespace

std::span<const CodecImpl> zstd_codecs() { return table; }

}  // namespace cvt::compression::detail
