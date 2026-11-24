// Convertible core - archive family: LZX decoder (CAB/CHM flavour and the WIM chunk flavour).
//
// LZX is LZ77 over a 2^15..2^21 byte window with Huffman-coded blocks. The
// bitstream is a sequence of little-endian 16-bit words read most significant
// bit first. Blocks are verbatim (main + length trees), aligned-offset (an
// extra 8-symbol tree for the low three offset bits) or uncompressed (raw
// bytes after a 16-bit alignment, preceded by the three repeated offsets).
// Tree code lengths are sent as differences (mod 17) to the previous block's
// lengths through a 20-symbol pretree with run-length codes 17..19.
//
// CAB/CHM (Method::Lzx): the stream starts with the "E8 translation" bit
// (+ 32-bit translation size); every 32 KiB of output forms a frame after
// which the bitstream is re-aligned to 16 bits, and the call-instruction
// translation is undone per frame. Block sizes take 24 bits. In CHM the whole
// state is reset at every reset interval: pass the interval sizes in
// `block_sizes` (entries above 32 KiB, or `flags` bit 0 set, mark them as
// reset intervals; entries of at most 32 KiB without the flag are CAB data
// block sizes, which only supply the total size).
//
// WIM (Method::LzxWim): one call = one chunk, decoded independently. No
// translation header (translation is always on with the size 12000000),
// block sizes are "1 = 32768" or 16 bits (+8 bits when the window is 64 KiB or
// larger), no 32 KiB frames. `window_bits` 0 means 15 (32 KiB chunks).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <array>
#include <vector>

#include "families/archive/codec_impl.hpp"
#include "families/archive/codecs/huffman.hpp"
#include "families/archive/codecs/zip_legacy_common.hpp"

namespace cvt::compression::detail {

namespace {

constexpr std::uint32_t frame_size = 32768;
constexpr std::uint32_t wim_e8_size = 12000000;
constexpr int max_slots = 50;
constexpr int max_main = 256 + max_slots * 8;
constexpr int num_lengths = 249;

struct SlotTables {
  std::array<std::uint32_t, max_slots> base{};
  std::array<std::uint8_t, max_slots> extra{};
};

constexpr SlotTables make_slots() {
  SlotTables t{};
  std::uint32_t b = 0;
  for (int i = 0; i < max_slots; ++i) {
    const int e = i < 4 ? 0 : std::min((i - 2) / 2, 17);
    t.extra[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(e);
    t.base[static_cast<std::size_t>(i)] = b;
    b += 1u << e;
  }
  return t;
}
constexpr SlotTables slots = make_slots();

int slots_for(int window_bits) {
  switch (window_bits) {
    case 15: return 30;
    case 16: return 32;
    case 17: return 34;
    case 18: return 36;
    case 19: return 38;
    case 20: return 42;
    default: return 50;
  }
}

/// MSB-first reader over little-endian 16-bit words (HuffmanTable-compatible).
class WordBits {
 public:
  explicit WordBits(ByteView data) noexcept
      : data_(reinterpret_cast<const std::uint8_t*>(data.data())), size_(data.size()) {}

  [[nodiscard]] std::uint32_t peek(int n) noexcept {
    if (n == 0) return 0;
    if (bits_ < n) refill();
    return static_cast<std::uint32_t>(buf_ >> (64 - n));
  }
  void skip(int n) noexcept {
    if (bits_ < n) refill();
    buf_ <<= n;
    bits_ -= n;
  }
  [[nodiscard]] std::uint32_t get(int n) noexcept {
    if (n <= 16) {
      const std::uint32_t v = peek(n);
      skip(n);
      return v;
    }
    const std::uint32_t hi = get(16);
    return (hi << (n - 16)) | get(n - 16);
  }
  [[nodiscard]] std::uint64_t position() const noexcept { return std::uint64_t{pos_} * 8 - static_cast<std::uint64_t>(bits_); }
  [[nodiscard]] bool overrun() const noexcept { return position() > std::uint64_t{size_} * 8; }
  /// Drop the rest of the current 16-bit word.
  void align16() noexcept { skip(bits_ % 16); }
  /// Continue reading 16-bit words at byte `offset`.
  void restart(std::size_t offset) noexcept {
    pos_ = offset;
    buf_ = 0;
    bits_ = 0;
  }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] const std::uint8_t* bytes() const noexcept { return data_; }

 private:
  void refill() noexcept {
    while (bits_ <= 48) {
      std::uint32_t word = 0;
      if (pos_ < size_) word = data_[pos_];
      if (pos_ + 1 < size_) word |= std::uint32_t{data_[pos_ + 1]} << 8;
      buf_ |= std::uint64_t{word} << (48 - bits_);
      pos_ += 2;
      bits_ += 16;
    }
  }
  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t pos_ = 0;
  std::uint64_t buf_ = 0;
  int bits_ = 0;
};

class LzxDecoder {
 public:
  LzxDecoder(ByteView data, const DecodeOptions& options, bool wim)
      : in_(data), options_(options), wim_(wim) {}

  Result<Bytes> run() {
    window_bits_ = options_.window_bits;
    if (wim_ && window_bits_ == 0) window_bits_ = 15;
    if (window_bits_ < 15 || window_bits_ > 21) {
      return fail(Status::InvalidArgument, "LZX window of 2^{} bytes is not supported (15..21)", window_bits_);
    }
    window_size_ = 1u << window_bits_;
    num_main_ = 256 + slots_for(window_bits_) * 8;

    DecodeOptions inner = options_;
    std::vector<std::uint64_t> resets;
    if (!options_.block_sizes.empty()) {
      bool reset_mode = (options_.flags & 1u) != 0;
      std::uint64_t total = 0;
      for (const std::uint32_t b : options_.block_sizes) {
        if (b > frame_size) reset_mode = true;
        total += b;
      }
      if (!inner.output_size) inner.output_size = total;
      if (reset_mode && !wim_) {
        std::uint64_t at = 0;
        for (const std::uint32_t b : options_.block_sizes) {
          at += b;
          if (at % frame_size != 0 && at != total) {
            return fail(Status::InvalidArgument, "LZX reset intervals must be multiples of 32 KiB");
          }
          resets.push_back(at);
        }
      }
    }
    // A prefix request still decodes whole frames: the call translation of a
    // frame depends on its full length.
    if (inner.stop_after != 0) {
      const std::uint64_t whole = (inner.stop_after + frame_size - 1) / frame_size * frame_size;
      inner.stop_after = wim_ ? 0 : whole;
      if (inner.output_size && inner.stop_after >= *inner.output_size) inner.stop_after = 0;
    }
    CVT_TRY(out_.start(inner, wim_ ? "LZX (WIM)" : "LZX"));
    reset_state();
    std::size_t next_reset = 0;
    std::uint64_t next_frame = wim_ ? ~std::uint64_t{0} : frame_size;
    std::uint32_t steps = 0;

    while (!out_.done()) {
      const std::uint64_t pos = out_.size();
      if (pos == next_frame) {
        if (!raw_mode_) in_.align16();
        next_frame += frame_size;
        if (next_reset < resets.size() && resets[next_reset] == pos) {
          ++next_reset;
          if (block_remaining_ != 0) return fail(Status::Decode, "LZX block runs across a reset point");
          if (raw_mode_) leave_raw_mode();
          reset_state();
        }
      }
      if (!header_read_) {
        header_read_ = true;
        if (wim_) {
          e8_size_ = wim_e8_size;
        } else if (in_.get(1) != 0) {
          e8_size_ = in_.get(32);
        } else {
          e8_size_ = 0;
        }
      }
      if (pos % frame_size == 0) frame_e8_.push_back(e8_size_);
      if (block_remaining_ == 0) {
        if (raw_mode_) leave_raw_mode();
        CVT_TRY(block_header());
      }
      const std::uint64_t frame_left = next_frame - pos;
      const std::uint32_t run = static_cast<std::uint32_t>(std::min<std::uint64_t>(block_remaining_, frame_left));
      if (raw_mode_) {
        CVT_TRY(raw_bytes(run));
      } else {
        CVT_TRY(symbols(run));
      }
      if ((++steps & 7) == 0 && !legacy_progress(options_, in_.position() / 8, in_.size())) {
        return fail(Status::Cancelled, "the conversion was cancelled");
      }
    }
    const bool more = block_remaining_ != 0 || (raw_mode_ ? raw_pos_ < in_.size() : in_.position() + 16 < in_.size() * 8);
    translate();
    CVT_TRY_ASSIGN(Bytes result, out_.finish(more));
    if (options_.stop_after != 0 && result.size() > options_.stop_after) {
      result.resize(static_cast<std::size_t>(options_.stop_after));
    }
    return result;
  }

 private:
  void reset_state() {
    r_ = {1, 1, 1};
    main_len_.fill(0);
    length_len_.fill(0);
    header_read_ = false;
    block_remaining_ = 0;
    block_type_ = 0;
  }

  void leave_raw_mode() {
    if (raw_odd_ && raw_pos_ < in_.size()) ++raw_pos_;  // pad byte after an odd-sized uncompressed block
    raw_mode_ = false;
    raw_odd_ = false;
    in_.restart(raw_pos_);
  }

  Result<void> block_header() {
    block_type_ = static_cast<int>(in_.get(3));
    std::uint32_t size = 0;
    if (wim_) {
      if (in_.get(1) != 0) {
        size = frame_size;
      } else {
        size = in_.get(16);
        if (window_bits_ >= 16) size = (size << 8) | in_.get(8);
      }
    } else {
      const std::uint32_t hi = in_.get(16);
      size = (hi << 8) | in_.get(8);
    }
    if (in_.overrun()) return fail(Status::Decode, "LZX data is truncated");
    if (size == 0) return fail(Status::Decode, "LZX block of size zero");
    block_remaining_ = size;
    switch (block_type_) {
      case 2: {
        std::array<std::uint8_t, 8> al{};
        for (std::uint8_t& l : al) l = static_cast<std::uint8_t>(in_.get(3));
        if (!aligned_.build(al, false)) return fail(Status::Decode, "LZX aligned offset tree is invalid");
        [[fallthrough]];
      }
      case 1: {
        CVT_TRY(read_lengths(main_len_.data(), 0, 256));
        CVT_TRY(read_lengths(main_len_.data(), 256, static_cast<std::uint32_t>(num_main_)));
        CVT_TRY(read_lengths(length_len_.data(), 0, num_lengths));
        if (!main_.build(std::span<const std::uint8_t>(main_len_.data(), static_cast<std::size_t>(num_main_)), false) ||
            !length_.build(length_len_, false)) {
          return fail(Status::Decode, "LZX Huffman tree is over-subscribed");
        }
        if (in_.overrun()) return fail(Status::Decode, "LZX data is truncated");
        return {};
      }
      case 3: {
        // Align to 16 bits; CAB streams always pad (1..16 bits), WIM only when needed.
        const int used = static_cast<int>(in_.position() % 16);
        if (used != 0) {
          in_.align16();
        } else if (!wim_) {
          in_.skip(16);
        }
        const std::uint64_t at = in_.position() / 8;
        if (in_.overrun() || at + 12 > in_.size()) return fail(Status::Decode, "LZX data is truncated");
        const std::uint8_t* p = in_.bytes() + at;
        for (std::size_t i = 0; i < 3; ++i) r_[i] = legacy_le32(p + 4 * i);
        for (const std::uint32_t r : r_) {
          if (r == 0 || r > window_size_) return fail(Status::Decode, "LZX uncompressed block has invalid offsets");
        }
        raw_pos_ = static_cast<std::size_t>(at + 12);
        raw_mode_ = true;
        raw_odd_ = (size & 1u) != 0;
        return {};
      }
      default:
        return fail(Status::Decode, "LZX block has the invalid type {}", block_type_);
    }
  }

  Result<void> read_lengths(std::uint8_t* lens, std::uint32_t first, std::uint32_t last) {
    std::array<std::uint8_t, 20> pre_len{};
    for (std::uint8_t& l : pre_len) l = static_cast<std::uint8_t>(in_.get(4));
    if (!pre_.build(pre_len, false)) return fail(Status::Decode, "LZX pretree is invalid");
    std::uint32_t x = first;
    while (x < last) {
      int z = pre_.decode(in_);
      if (z < 0 || in_.overrun()) return fail(Status::Decode, "LZX code lengths are damaged");
      std::uint32_t run = 1;
      std::uint8_t value = 0;
      if (z == 17) {
        run = 4 + in_.get(4);
      } else if (z == 18) {
        run = 20 + in_.get(5);
      } else if (z == 19) {
        run = 4 + in_.get(1);
        z = pre_.decode(in_);
        if (z < 0 || z > 16) return fail(Status::Decode, "LZX code lengths are damaged");
        value = static_cast<std::uint8_t>((lens[x] + 17 - z) % 17);
      } else {
        value = static_cast<std::uint8_t>((lens[x] + 17 - z) % 17);
      }
      if (run > last - x) return fail(Status::Decode, "LZX code length run overflows the tree");
      for (std::uint32_t k = 0; k < run; ++k) lens[x++] = value;
    }
    return {};
  }

  Result<void> raw_bytes(std::uint32_t run) {
    const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(run, out_.room()));
    if (n > in_.size() - std::min(raw_pos_, in_.size())) return fail(Status::Decode, "LZX data is truncated");
    out_.append_raw(in_.bytes() + raw_pos_, n);
    raw_pos_ += n;
    block_remaining_ -= static_cast<std::uint32_t>(n);
    return {};
  }

  Result<void> symbols(std::uint32_t run) {
    const bool aligned = block_type_ == 2;
    while (run > 0 && !out_.done()) {
      const int sym = main_.decode(in_);
      if (sym < 256) {
        if (sym < 0) return fail(Status::Decode, "LZX data is damaged");
        out_.put(static_cast<std::uint8_t>(sym));
        --run;
        --block_remaining_;
        continue;
      }
      const std::uint32_t s = static_cast<std::uint32_t>(sym - 256);
      std::uint32_t length = s & 7u;
      const std::uint32_t slot = s >> 3;
      if (length == 7) {
        const int extra = length_.decode(in_);
        if (extra < 0) return fail(Status::Decode, "LZX data is damaged");
        length += static_cast<std::uint32_t>(extra);
      }
      length += 2;
      std::uint32_t offset = 0;
      if (slot == 0) {
        offset = r_[0];
      } else if (slot == 1) {
        offset = r_[1];
        r_[1] = r_[0];
        r_[0] = offset;
      } else if (slot == 2) {
        offset = r_[2];
        r_[2] = r_[0];
        r_[0] = offset;
      } else {
        const int extra = slots.extra[slot];
        const std::uint32_t base = slots.base[slot];
        if (aligned && extra >= 3) {
          const std::uint32_t verbatim = in_.get(extra - 3) << 3;
          const int low = aligned_.decode(in_);
          if (low < 0) return fail(Status::Decode, "LZX data is damaged");
          offset = base + verbatim + static_cast<std::uint32_t>(low) - 2;
        } else {
          offset = base + in_.get(extra) - 2;
        }
        r_[2] = r_[1];
        r_[1] = r_[0];
        r_[0] = offset;
      }
      if (in_.overrun()) return fail(Status::Decode, "LZX data is truncated");
      if (length > run) return fail(Status::Decode, "LZX match runs past the end of its block or frame");
      if (offset > window_size_ || !out_.copy(offset, length)) {
        return fail(Status::Decode, "LZX match refers back before the start of the data");
      }
      run -= length;
      block_remaining_ -= length;
    }
    if (in_.overrun()) return fail(Status::Decode, "LZX data is truncated");
    return {};
  }

  // Undo the E8 (x86 CALL) translation frame by frame.
  void translate() {
    const std::size_t total = out_.size();
    std::uint8_t* data = out_.data();
    for (std::size_t f = 0; f < frame_e8_.size(); ++f) {
      const std::uint32_t e8 = frame_e8_[f];
      if (e8 == 0 || f >= 32768) continue;
      const std::size_t start = f * frame_size;
      if (start >= total) break;
      std::size_t len = std::min<std::size_t>(frame_size, total - start);
      if (wim_) len = total;
      if (len <= 10) continue;
      std::uint8_t* p = data + start;
      for (std::size_t i = 0; i < len - 10;) {
        if (p[i] != 0xE8) {
          ++i;
          continue;
        }
        const std::int64_t cur = static_cast<std::int64_t>(wim_ ? i : start + i);
        const std::int64_t abs = static_cast<std::int32_t>(legacy_le32(p + i + 1));
        if (abs >= -cur && abs < static_cast<std::int64_t>(e8)) {
          const std::int64_t rel = abs >= 0 ? abs - cur : abs + static_cast<std::int64_t>(e8);
          const auto v = static_cast<std::uint32_t>(rel);
          p[i + 1] = static_cast<std::uint8_t>(v);
          p[i + 2] = static_cast<std::uint8_t>(v >> 8);
          p[i + 3] = static_cast<std::uint8_t>(v >> 16);
          p[i + 4] = static_cast<std::uint8_t>(v >> 24);
        }
        i += 5;
      }
    }
  }

  WordBits in_;
  const DecodeOptions& options_;
  bool wim_;
  LegacyOutput out_;
  int window_bits_ = 0;
  std::uint32_t window_size_ = 0;
  int num_main_ = 0;
  std::array<std::uint32_t, 3> r_{1, 1, 1};
  std::array<std::uint8_t, max_main> main_len_{};
  std::array<std::uint8_t, num_lengths> length_len_{};
  HuffmanTable pre_;
  HuffmanTable main_;
  HuffmanTable length_;
  HuffmanTable aligned_;
  bool header_read_ = false;
  std::uint32_t e8_size_ = 0;
  std::vector<std::uint32_t> frame_e8_;
  std::uint32_t block_remaining_ = 0;
  int block_type_ = 0;
  bool raw_mode_ = false;
  bool raw_odd_ = false;
  std::size_t raw_pos_ = 0;
};

Result<Bytes> decode_lzx(ByteView data, const DecodeOptions& options) {
  LzxDecoder decoder(data, options, false);
  return decoder.run();
}

Result<Bytes> decode_lzx_wim(ByteView data, const DecodeOptions& options) {
  LzxDecoder decoder(data, options, true);
  return decoder.run();
}

constexpr std::array<CodecImpl, 2> table = {{
    {Method::Lzx, decode_lzx, nullptr, nullptr, nullptr, nullptr},
    {Method::LzxWim, decode_lzx_wim, nullptr, nullptr, nullptr, nullptr},
}};

}  // namespace

std::span<const CodecImpl> lzx_codecs() { return table; }

}  // namespace cvt::compression::detail
