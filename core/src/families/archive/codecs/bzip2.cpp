// Convertible core - archive family: bzip2 decoder and encoder (Method::Bzip2).
//
// Stream: "BZh" + block-size digit, blocks (magic 314159265359, CRC, BWT origin,
// symbol map, Huffman tables, MTF/RLE2 symbols), end marker 177245385090 +
// combined CRC. Blocks hold up to 100k * level bytes after the initial
// run-length encoding. The encoder sorts rotations by prefix doubling (exact
// cyclic order, O(n log n) even for degenerate input) and chooses up to six
// Huffman tables with iterative refinement like the reference encoder.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <memory>
#include <numeric>
#include <optional>

#include "util/bytes.hpp"
#include "families/archive/checksums.hpp"
#include "families/archive/codec_impl.hpp"
#include "families/archive/codecs/bitio.hpp"
#include "families/archive/codecs/huffman.hpp"

namespace cvt::compression::detail {

namespace {

constexpr std::uint64_t block_magic = 0x314159265359ull;
constexpr std::uint64_t end_magic = 0x177245385090ull;
constexpr int max_groups = 6;
constexpr int group_size = 50;
constexpr int max_alpha = 258;
constexpr int max_selectors = 18002;
constexpr int max_code_len = 20;

std::uint32_t crc_update(std::uint32_t crc, ByteView data) {
  // checksums.hpp keeps the running value pre-inverted; bzip2 starts at ~0.
  return archive::crc32_msb(data, crc);
}

// ---------------------------------------------------------------------------- decoder

struct BlockDecoder {
  std::vector<std::uint32_t> tt;
};

class Bzip2Decoder {
 public:
  explicit Bzip2Decoder(const DecodeOptions& options) : options_(options) {}

  // Decode a whole buffer (one or more concatenated streams).
  Result<Bytes> run(ByteView data) {
    Bytes out;
    std::size_t offset = 0;
    bool any = false;
    while (offset < data.size()) {
      const ByteView rest = data.subspan(offset);
      if (rest.size() < 4 || as_chars(rest.first(3)) != "BZh") {
        if (!any) return fail(Status::Decode, "not a bzip2 stream");
        break;  // trailing garbage after a complete stream is ignored, like bzip2 -d
      }
      CVT_TRY_ASSIGN(const std::size_t used, stream(rest, out));
      any = true;
      offset += used;
      if (stopped_) break;
    }
    return out;
  }

 private:
  bool limit_reached(std::uint64_t total) const {
    return options_.stop_after != 0 && total >= options_.stop_after;
  }

  Result<std::size_t> stream(ByteView data, Bytes& out) {
    const int level = std::to_integer<int>(data[3]) - '0';
    if (level < 1 || level > 9) return fail(Status::Decode, "bzip2 header has an invalid block size");
    const std::uint32_t max_block = static_cast<std::uint32_t>(level) * 100000u;
    MsbBitReader in(data);
    in.skip(32);
    std::uint32_t combined = 0;
    while (true) {
      const std::uint64_t magic = (std::uint64_t{in.get(24)} << 24) | in.get(24);
      const std::uint32_t stored_crc = in.get(32);
      if (in.overrun()) return fail(Status::Decode, "bzip2 stream is truncated");
      if (magic == end_magic) {
        if (options_.verify_checksums && stored_crc != combined) {
          return fail(Status::Decode, "bzip2 stream CRC mismatch");
        }
        in.align();
        return in.byte_position();
      }
      if (magic != block_magic) return fail(Status::Decode, "bzip2 block header is damaged");
      const std::size_t before = out.size();
      CVT_TRY(block(in, max_block, out));
      if (stopped_) return in.byte_position();
      const std::uint32_t crc = crc_update(0, ByteView(out).subspan(before));
      if (options_.verify_checksums && crc != stored_crc) return fail(Status::Decode, "bzip2 block CRC mismatch");
      combined = ((combined << 1) | (combined >> 31)) ^ crc;
      if (!options_.progress.report(static_cast<double>(in.position() / 8) / static_cast<double>(data.size()))) {
        return fail(Status::Cancelled, "the conversion was cancelled");
      }
    }
  }

  Result<void> block(MsbBitReader& in, std::uint32_t max_block, Bytes& out) {
    if (in.bit()) {
      return fail(Status::Decode, "randomised bzip2 blocks (bzip2 0.9.0) are not supported");
    }
    const std::uint32_t orig_ptr = in.get(24);
    // Symbol map.
    std::array<std::uint8_t, 256> seq_to_unseq{};
    int in_use = 0;
    const std::uint32_t used16 = in.get(16);
    for (int i = 0; i < 16; ++i) {
      if (used16 & (0x8000u >> i)) {
        const std::uint32_t bits = in.get(16);
        for (int j = 0; j < 16; ++j) {
          if (bits & (0x8000u >> j)) seq_to_unseq[static_cast<std::size_t>(in_use++)] = static_cast<std::uint8_t>(i * 16 + j);
        }
      }
    }
    if (in_use == 0) return fail(Status::Decode, "bzip2 block uses no symbols");
    const int alpha = in_use + 2;
    const int groups = static_cast<int>(in.get(3));
    if (groups < 2 || groups > max_groups) return fail(Status::Decode, "bzip2 block has an invalid table count");
    const int selectors_count = static_cast<int>(in.get(15));
    if (selectors_count < 1) return fail(Status::Decode, "bzip2 block has no selectors");
    std::vector<std::uint8_t> selectors(static_cast<std::size_t>(std::min(selectors_count, max_selectors)));
    std::array<std::uint8_t, max_groups> mtf{};
    for (int i = 0; i < groups; ++i) mtf[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
    for (int i = 0; i < selectors_count; ++i) {
      int j = 0;
      while (in.bit()) {
        if (++j >= groups) return fail(Status::Decode, "bzip2 selector is out of range");
        if (in.overrun()) return fail(Status::Decode, "bzip2 stream is truncated");
      }
      const std::uint8_t v = mtf[static_cast<std::size_t>(j)];
      for (int k = j; k > 0; --k) mtf[static_cast<std::size_t>(k)] = mtf[static_cast<std::size_t>(k - 1)];
      mtf[0] = v;
      if (i < max_selectors) selectors[static_cast<std::size_t>(i)] = v;
    }
    std::array<HuffmanTable, max_groups> tables;
    std::array<std::uint8_t, max_alpha> lengths{};
    for (int t = 0; t < groups; ++t) {
      int curr = static_cast<int>(in.get(5));
      for (int i = 0; i < alpha; ++i) {
        while (true) {
          if (curr < 1 || curr > max_code_len) return fail(Status::Decode, "bzip2 code length is out of range");
          if (in.overrun()) return fail(Status::Decode, "bzip2 stream is truncated");
          if (!in.bit()) break;
          curr += in.bit() ? -1 : 1;
        }
        lengths[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(curr);
      }
      if (!tables[static_cast<std::size_t>(t)].build(std::span(lengths.data(), static_cast<std::size_t>(alpha)), false)) {
        return fail(Status::Decode, "bzip2 Huffman table is invalid");
      }
    }
    // MTF / RLE2 symbols.
    std::array<std::uint8_t, 256> order{};
    for (int i = 0; i < 256; ++i) order[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
    std::array<std::uint32_t, 256> counts{};
    tt_.resize(max_block);
    std::uint32_t n = 0;
    const int eob = in_use + 1;
    int group_left = 0;
    std::size_t selector_index = 0;
    const HuffmanTable* table = nullptr;
    std::uint32_t run = 0;
    std::uint32_t run_weight = 1;
    const auto flush_run = [&]() -> Result<void> {
      if (run == 0) return {};
      const std::uint8_t byte = seq_to_unseq[order[0]];
      if (run > max_block - n) return fail(Status::Decode, "bzip2 block is larger than its declared size");
      counts[byte] += run;
      std::fill_n(tt_.begin() + n, run, byte);
      n += run;
      run = 0;
      run_weight = 1;
      return {};
    };
    while (true) {
      if (group_left == 0) {
        if (selector_index >= selectors.size()) return fail(Status::Decode, "bzip2 block runs out of selectors");
        const std::uint8_t sel = selectors[selector_index++];
        if (sel >= groups) return fail(Status::Decode, "bzip2 selector is out of range");
        table = &tables[sel];
        group_left = group_size;
      }
      --group_left;
      const int sym = table->decode(in);
      if (sym < 0 || in.overrun()) return fail(Status::Decode, "bzip2 data is damaged or truncated");
      if (sym <= 1) {
        if (run_weight > (1u << 21)) return fail(Status::Decode, "bzip2 run is too long");
        run += run_weight << sym;
        run_weight <<= 1;
        continue;
      }
      CVT_TRY(flush_run());
      if (sym == eob) break;
      const int index = sym - 1;
      if (index >= in_use) return fail(Status::Decode, "bzip2 symbol is out of range");
      const std::uint8_t v = order[static_cast<std::size_t>(index)];
      ::cvt::util::move_bytes(order.data() + 1, order.data(), static_cast<std::size_t>(index));
      order[0] = v;
      if (n >= max_block) return fail(Status::Decode, "bzip2 block is larger than its declared size");
      const std::uint8_t byte = seq_to_unseq[v];
      ++counts[byte];
      tt_[n++] = byte;
    }
    if (orig_ptr >= n) return fail(Status::Decode, "bzip2 block origin is out of range");
    // Inverse BWT.
    std::array<std::uint32_t, 256> start{};
    std::uint32_t sum = 0;
    for (int i = 0; i < 256; ++i) {
      start[static_cast<std::size_t>(i)] = sum;
      sum += counts[static_cast<std::size_t>(i)];
    }
    for (std::uint32_t i = 0; i < n; ++i) {
      const std::uint32_t byte = tt_[i] & 0xFFu;
      tt_[start[byte]++] |= i << 8;
    }
    // Undo the initial run-length encoding while walking the cycle.
    std::uint32_t pos = tt_[orig_ptr] >> 8;
    int last = -1;
    int same = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
      const std::uint32_t entry = tt_[pos];
      const auto byte = static_cast<int>(entry & 0xFFu);
      pos = entry >> 8;
      if (same == 4) {
        CVT_TRY(emit(out, static_cast<std::uint8_t>(last), static_cast<std::uint32_t>(byte)));
        same = 0;
        last = -1;
        if (stopped_) return {};
        continue;
      }
      if (byte == last) {
        ++same;
      } else {
        last = byte;
        same = 1;
      }
      CVT_TRY(emit(out, static_cast<std::uint8_t>(byte), 1));
      if (stopped_) return {};
    }
    return {};
  }

  Result<void> emit(Bytes& out, std::uint8_t byte, std::uint32_t count) {
    if (count == 0) return {};
    CVT_TRY(check_output_room(out.size(), count, options_.max_output));
    out.insert(out.end(), count, static_cast<std::byte>(byte));
    if (limit_reached(out.size())) stopped_ = true;
    return {};
  }

  DecodeOptions options_;
  std::vector<std::uint32_t> tt_;
  bool stopped_ = false;
};

// ---------------------------------------------------------------------------- encoder

// Sorted order of the cyclic rotations of `block` (prefix doubling with
// counting sorts). Returns the suffix-array-like order of rotation starts.
std::vector<std::uint32_t> sort_rotations(std::span<const std::uint8_t> block) {
  const auto n = static_cast<std::uint32_t>(block.size());
  std::vector<std::uint32_t> sa(n), tmp(n), rank(n), next_rank(n);
  std::vector<std::uint32_t> count(std::max<std::uint32_t>(n, 256) + 1);
  for (std::uint32_t i = 0; i < n; ++i) ++count[block[i]];
  for (std::uint32_t i = 1; i < 256; ++i) count[i] += count[i - 1];
  for (std::uint32_t i = n; i-- > 0;) sa[--count[block[i]]] = i;
  std::uint32_t classes = 1;
  rank[sa[0]] = 0;
  for (std::uint32_t i = 1; i < n; ++i) {
    if (block[sa[i]] != block[sa[i - 1]]) ++classes;
    rank[sa[i]] = classes - 1;
  }
  for (std::uint32_t k = 1; k < n && classes < n; k <<= 1) {
    // Order by the second half: rotation (i - k) sorted by rank of i.
    for (std::uint32_t i = 0; i < n; ++i) tmp[i] = sa[i] >= k ? sa[i] - k : sa[i] + n - k;
    std::fill(count.begin(), count.begin() + classes + 1, 0u);
    for (std::uint32_t i = 0; i < n; ++i) ++count[rank[tmp[i]]];
    for (std::uint32_t i = 1; i < classes; ++i) count[i] += count[i - 1];
    for (std::uint32_t i = n; i-- > 0;) sa[--count[rank[tmp[i]]]] = tmp[i];
    next_rank[sa[0]] = 0;
    std::uint32_t c = 1;
    for (std::uint32_t i = 1; i < n; ++i) {
      const std::uint32_t a = sa[i - 1];
      const std::uint32_t b = sa[i];
      const std::uint32_t a2 = a + k < n ? a + k : a + k - n;
      const std::uint32_t b2 = b + k < n ? b + k : b + k - n;
      if (rank[a] != rank[b] || rank[a2] != rank[b2]) ++c;
      next_rank[b] = c - 1;
    }
    rank.swap(next_rank);
    classes = c;
  }
  return sa;
}

// MSB-first bit output whose partial byte survives between calls.
struct BitSink {
  std::uint64_t acc = 0;
  int count = 0;
  void put(Bytes& out, std::uint32_t value, int n) {
    acc = (acc << n) | (value & (n == 32 ? 0xFFFFFFFFu : ((1u << n) - 1u)));
    count += n;
    while (count >= 8) {
      count -= 8;
      out.push_back(static_cast<std::byte>((acc >> count) & 0xFFu));
    }
    acc &= (std::uint64_t{1} << count) - 1;
  }
  void put48(Bytes& out, std::uint64_t value) {
    put(out, static_cast<std::uint32_t>(value >> 24), 24);
    put(out, static_cast<std::uint32_t>(value & 0xFFFFFFu), 24);
  }
  void flush(Bytes& out) {
    if (count > 0) out.push_back(static_cast<std::byte>((acc << (8 - count)) & 0xFFu));
    acc = 0;
    count = 0;
  }
};

class Bzip2Encoder final : public Encoder {
 public:
  explicit Bzip2Encoder(const EncodeOptions& options)
      : level_(std::clamp(options.level, 1, 9)), progress_(options.progress) {
    block_limit_ = static_cast<std::uint32_t>(level_) * 100000u - 19u;
    block_.reserve(block_limit_ + 8);
  }

  Result<void> encode(ByteView input, Bytes& output) override {
    if (finished_) return fail(Status::InvalidArgument, "the encoder has already finished");
    begin(output);
    for (const std::byte b : input) {
      const auto byte = std::to_integer<std::uint8_t>(b);
      if (run_length_ > 0 && byte == run_byte_ && run_length_ < 255) {
        ++run_length_;
        continue;
      }
      CVT_TRY(flush_run(output));
      run_byte_ = byte;
      run_length_ = 1;
    }
    total_in_ += input.size();
    total_out_ += 0;
    return {};
  }

  Result<void> finish(Bytes& output) override {
    if (finished_) return fail(Status::InvalidArgument, "the encoder has already finished");
    begin(output);
    CVT_TRY(flush_run(output));
    if (!block_.empty()) CVT_TRY(write_block(output));
    bits_.put48(output, end_magic);
    bits_.put(output, combined_crc_, 32);
    bits_.flush(output);
    finished_ = true;
    total_out_ = output.size();
    return {};
  }

  std::uint64_t total_in() const noexcept override { return total_in_; }
  std::uint64_t total_out() const noexcept override { return total_out_; }

 private:
  void begin(Bytes& output) {
    if (started_) return;
    started_ = true;
    append(output, as_bytes("BZh"));
    output.push_back(static_cast<std::byte>('0' + level_));
  }

  Result<void> flush_run(Bytes& output) {
    if (run_length_ == 0) return {};
    const std::uint32_t needed = run_length_ >= 4 ? 5 : run_length_;
    if (block_.size() + needed > block_limit_) CVT_TRY(write_block(output));
    const std::array<std::uint8_t, 4> copies = {run_byte_, run_byte_, run_byte_, run_byte_};
    for (std::uint32_t i = 0; i < std::min<std::uint32_t>(run_length_, 4); ++i) block_.push_back(run_byte_);
    if (run_length_ >= 4) block_.push_back(static_cast<std::uint8_t>(run_length_ - 4));
    // CRC of the original bytes of this run.
    std::uint32_t left = run_length_;
    while (left > 0) {
      const std::uint32_t take = std::min<std::uint32_t>(left, 4);
      block_crc_ = crc_update(block_crc_, as_bytes(std::span<const std::uint8_t>(copies.data(), take)));
      left -= take;
    }
    run_length_ = 0;
    return {};
  }

  Result<void> write_block(Bytes& output) {
    if (progress_.cancelled()) return fail(Status::Cancelled, "the conversion was cancelled");
    const auto n = static_cast<std::uint32_t>(block_.size());
    const std::vector<std::uint32_t> sa = sort_rotations(block_);
    std::uint32_t orig_ptr = 0;
    std::vector<std::uint8_t> last(n);
    for (std::uint32_t i = 0; i < n; ++i) {
      if (sa[i] == 0) orig_ptr = i;
      last[i] = block_[sa[i] == 0 ? n - 1 : sa[i] - 1];
    }
    // Symbol map.
    std::array<bool, 256> used{};
    for (const std::uint8_t b : block_) used[b] = true;
    std::array<std::uint8_t, 256> unseq_to_seq{};
    int in_use = 0;
    for (int i = 0; i < 256; ++i) {
      if (used[static_cast<std::size_t>(i)]) unseq_to_seq[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(in_use++);
    }
    const int alpha = in_use + 2;
    const int eob = in_use + 1;
    // MTF + RLE2.
    std::vector<std::uint16_t> symbols;
    symbols.reserve(n + 1);
    std::array<std::uint8_t, 256> order{};
    for (int i = 0; i < in_use; ++i) order[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
    std::array<std::uint32_t, max_alpha> freq{};
    std::uint32_t zeros = 0;
    const auto flush_zeros = [&]() {
      if (zeros == 0) return;
      std::uint32_t z = zeros - 1;
      while (true) {
        const std::uint16_t sym = (z & 1u) ? 1 : 0;
        symbols.push_back(sym);
        ++freq[sym];
        if (z < 2) break;
        z = (z - 2) / 2;
      }
      zeros = 0;
    };
    for (std::uint32_t i = 0; i < n; ++i) {
      const std::uint8_t s = unseq_to_seq[last[i]];
      if (order[0] == s) {
        ++zeros;
        continue;
      }
      flush_zeros();
      int j = 1;
      std::uint8_t tmp = order[1];
      order[1] = order[0];
      while (tmp != s) {
        ++j;
        std::swap(tmp, order[static_cast<std::size_t>(j)]);
      }
      order[0] = s;
      const auto sym = static_cast<std::uint16_t>(j + 1);
      symbols.push_back(sym);
      ++freq[sym];
    }
    flush_zeros();
    symbols.push_back(static_cast<std::uint16_t>(eob));
    ++freq[static_cast<std::size_t>(eob)];

    // Huffman tables.
    const std::size_t mtf_count = symbols.size();
    const int groups = mtf_count < 200 ? 2 : mtf_count < 600 ? 3 : mtf_count < 1200 ? 4 : mtf_count < 2400 ? 5 : 6;
    std::array<std::array<std::uint8_t, max_alpha>, max_groups> len{};
    {
      // Initial partition of the alphabet into ranges of similar total frequency.
      int remaining = static_cast<int>(mtf_count);
      int gs = 0;
      for (int part = groups; part > 0; --part) {
        const int target = remaining / part;
        int ge = gs - 1;
        int acc = 0;
        while (acc < target && ge < alpha - 1) {
          ++ge;
          acc += static_cast<int>(freq[static_cast<std::size_t>(ge)]);
        }
        if (ge > gs && part != groups && part != 1 && ((groups - part) % 2 == 1)) {
          acc -= static_cast<int>(freq[static_cast<std::size_t>(ge)]);
          --ge;
        }
        for (int v = 0; v < alpha; ++v) {
          len[static_cast<std::size_t>(part - 1)][static_cast<std::size_t>(v)] = (v >= gs && v <= ge) ? 0 : 15;
        }
        gs = ge + 1;
        remaining -= acc;
      }
    }
    const std::size_t group_count = (mtf_count + group_size - 1) / group_size;
    std::vector<std::uint8_t> selectors(group_count);
    for (int iteration = 0; iteration < 4; ++iteration) {
      std::array<std::array<std::uint32_t, max_alpha>, max_groups> rfreq{};
      for (std::size_t g = 0; g < group_count; ++g) {
        const std::size_t begin = g * group_size;
        const std::size_t end = std::min(mtf_count, begin + group_size);
        int best = 0;
        std::uint32_t best_cost = 0xFFFFFFFFu;
        for (int t = 0; t < groups; ++t) {
          std::uint32_t cost = 0;
          for (std::size_t i = begin; i < end; ++i) cost += len[static_cast<std::size_t>(t)][symbols[i]];
          if (cost < best_cost) {
            best_cost = cost;
            best = t;
          }
        }
        selectors[g] = static_cast<std::uint8_t>(best);
        for (std::size_t i = begin; i < end; ++i) ++rfreq[static_cast<std::size_t>(best)][symbols[i]];
      }
      for (int t = 0; t < groups; ++t) {
        // Every symbol needs a code (the decoder table covers the whole alphabet).
        std::array<std::uint32_t, max_alpha> f{};
        for (int v = 0; v < alpha; ++v) {
          f[static_cast<std::size_t>(v)] = rfreq[static_cast<std::size_t>(t)][static_cast<std::size_t>(v)] * 2 + 1;
        }
        huffman_lengths(std::span(f.data(), static_cast<std::size_t>(alpha)), 17,
                        std::span(len[static_cast<std::size_t>(t)].data(), static_cast<std::size_t>(alpha)));
      }
    }
    std::array<std::array<std::uint32_t, max_alpha>, max_groups> codes{};
    for (int t = 0; t < groups; ++t) {
      canonical_codes(std::span(len[static_cast<std::size_t>(t)].data(), static_cast<std::size_t>(alpha)),
                      std::span(codes[static_cast<std::size_t>(t)].data(), static_cast<std::size_t>(alpha)));
    }

    // Write the block.
    Bytes& o = output;
    BitSink& w = bits_;
    w.put48(o, block_magic);
    w.put(o, block_crc_, 32);
    w.put(o, 0, 1);  // not randomised
    w.put(o, orig_ptr, 24);
    std::uint32_t used16 = 0;
    for (int i = 0; i < 16; ++i) {
      for (int j = 0; j < 16; ++j) {
        if (used[static_cast<std::size_t>(i * 16 + j)]) used16 |= 0x8000u >> i;
      }
    }
    w.put(o, used16, 16);
    for (int i = 0; i < 16; ++i) {
      if (!(used16 & (0x8000u >> i))) continue;
      std::uint32_t bits = 0;
      for (int j = 0; j < 16; ++j) {
        if (used[static_cast<std::size_t>(i * 16 + j)]) bits |= 0x8000u >> j;
      }
      w.put(o, bits, 16);
    }
    w.put(o, static_cast<std::uint32_t>(groups), 3);
    w.put(o, static_cast<std::uint32_t>(group_count), 15);
    std::array<std::uint8_t, max_groups> mtf{};
    for (int i = 0; i < groups; ++i) mtf[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
    for (const std::uint8_t sel : selectors) {
      int j = 0;
      while (mtf[static_cast<std::size_t>(j)] != sel) ++j;
      for (int k = j; k > 0; --k) mtf[static_cast<std::size_t>(k)] = mtf[static_cast<std::size_t>(k - 1)];
      mtf[0] = sel;
      for (int k = 0; k < j; ++k) w.put(o, 1, 1);
      w.put(o, 0, 1);
    }
    for (int t = 0; t < groups; ++t) {
      int curr = len[static_cast<std::size_t>(t)][0];
      w.put(o, static_cast<std::uint32_t>(curr), 5);
      for (int v = 0; v < alpha; ++v) {
        const int target = len[static_cast<std::size_t>(t)][static_cast<std::size_t>(v)];
        while (curr < target) {
          w.put(o, 2, 2);
          ++curr;
        }
        while (curr > target) {
          w.put(o, 3, 2);
          --curr;
        }
        w.put(o, 0, 1);
      }
    }
    for (std::size_t i = 0; i < mtf_count; ++i) {
      const auto t = static_cast<std::size_t>(selectors[i / group_size]);
      const std::uint16_t s = symbols[i];
      w.put(o, codes[t][s], len[t][s]);
    }
    combined_crc_ = ((combined_crc_ << 1) | (combined_crc_ >> 31)) ^ block_crc_;
    block_crc_ = 0;
    block_.clear();
    return {};
  }

  BitSink bits_;

  int level_;
  Progress progress_;
  std::uint32_t block_limit_ = 0;
  std::vector<std::uint8_t> block_;
  std::uint32_t block_crc_ = 0;
  std::uint32_t combined_crc_ = 0;
  std::uint8_t run_byte_ = 0;
  std::uint32_t run_length_ = 0;
  bool started_ = false;
  bool finished_ = false;
  std::uint64_t total_in_ = 0;
  std::uint64_t total_out_ = 0;
};

Result<Bytes> decode_bzip2(ByteView data, const DecodeOptions& options) {
  Bzip2Decoder decoder(options);
  CVT_TRY_ASSIGN(Bytes out, decoder.run(data));
  if (options.output_size && options.stop_after == 0 && out.size() != *options.output_size) {
    return fail(Status::Decode, "bzip2 data decodes to {} bytes, expected {}", out.size(), *options.output_size);
  }
  return out;
}

Result<Bytes> encode_bzip2(ByteView data, const EncodeOptions& options) {
  Bzip2Encoder encoder(options);
  Bytes out;
  // Fed in pieces so that progress advances (and a cancellation is seen)
  // at least once per block sort; the output does not depend on the cut.
  constexpr std::size_t piece = std::size_t{256} << 10;
  for (std::size_t at = 0; at < data.size(); at += piece) {
    if (!options.progress.report(static_cast<double>(at) / static_cast<double>(data.size()))) {
      return fail(Status::Cancelled, "the conversion was cancelled");
    }
    CVT_TRY(encoder.encode(data.subspan(at, std::min(piece, data.size() - at)), out));
  }
  CVT_TRY(encoder.finish(out));
  return out;
}

std::unique_ptr<Encoder> make_bzip2_encoder(const EncodeOptions& options) {
  return std::make_unique<Bzip2Encoder>(options);
}

constexpr std::array<CodecImpl, 1> table = {{
    {Method::Bzip2, decode_bzip2, encode_bzip2, nullptr, make_bzip2_encoder, nullptr},
}};

}  // namespace

std::span<const CodecImpl> bzip2_codecs() { return table; }

}  // namespace cvt::compression::detail
