// Convertible core - archive family: Quantum decoder (CAB compression type 2).
//
// Quantum (David Stafford) codes LZ77 symbols with a 16-bit arithmetic coder
// and adaptive frequency models, following the published descriptions of
// the format: four 64-symbol literal models, three position-slot models (for
// 3-byte, 4-byte and longer matches), a 27-symbol length-slot model and a
// 7-symbol selector. Every model starts with frequencies n..1, gains 8 per
// coded symbol and is halved once its total passes 3800 (every 50th halving
// re-sorts the symbols by frequency). Extra bits of positions and lengths are
// read raw from the same MSB-first bit stream. The window is 2^10..2^21 bytes
// (`window_bits`).
//
// One call decodes one CAB folder. The output is cut into 32 KiB frames (one
// per CAB data block); the arithmetic coder restarts at every frame and the
// encoder pads each data block with up to four zero bytes. To find the next
// block exactly, pass the compressed size of every data block as little-endian
// 32-bit values in `properties` (with `block_sizes` for the uncompressed
// sizes). Without them the decoder aligns to the next byte and skips zero
// padding bytes, which fails for the rare block whose first byte is zero.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <array>
#include <utility>
#include <vector>

#include "families/archive/codec_impl.hpp"
#include "families/archive/codecs/bitio.hpp"
#include "families/archive/codecs/zip_legacy_common.hpp"

namespace cvt::compression::detail {

namespace {

constexpr std::uint32_t frame_size = 32768;

constexpr std::array<std::uint32_t, 42> position_base = {
    0,      1,      2,      3,      4,      6,       8,       12,      16,      24,     32,
    48,     64,     96,     128,    192,    256,     384,     512,     768,     1024,   1536,
    2048,   3072,   4096,   6144,   8192,   12288,   16384,   24576,   32768,   49152,  65536,
    98304,  131072, 196608, 262144, 393216, 524288,  786432,  1048576, 1572864};
constexpr std::array<std::uint8_t, 42> position_extra = {0,  0,  0,  0,  1,  1,  2,  2,  3,  3,  4,  4,  5,  5,
                                                         6,  6,  7,  7,  8,  8,  9,  9,  10, 10, 11, 11, 12, 12,
                                                         13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18, 19, 19};
constexpr std::array<std::uint16_t, 27> length_base = {0,  1,  2,  3,  4,  5,  6,   8,   10,  12,  14,  18,  22, 26,
                                                       30, 38, 46, 54, 62, 78, 94, 110, 126, 158, 190, 222, 254};
constexpr std::array<std::uint8_t, 27> length_extra = {0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                                       3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};

struct Model {
  struct Entry {
    std::uint16_t sym = 0;
    std::uint32_t cumfreq = 0;
  };
  int entries = 0;
  int shifts_left = 4;
  std::array<Entry, 65> syms{};

  void init(int start, int n) {
    entries = n;
    shifts_left = 4;
    for (int i = 0; i <= n; ++i) {
      syms[static_cast<std::size_t>(i)].sym = static_cast<std::uint16_t>(start + i);
      syms[static_cast<std::size_t>(i)].cumfreq = static_cast<std::uint32_t>(n - i);
    }
  }

  void rescale() {
    const auto at = [&](int i) -> Entry& { return syms[static_cast<std::size_t>(i)]; };
    if (--shifts_left != 0) {
      for (int i = entries - 1; i >= 0; --i) {
        at(i).cumfreq >>= 1;
        if (at(i).cumfreq <= at(i + 1).cumfreq) at(i).cumfreq = at(i + 1).cumfreq + 1;
      }
      return;
    }
    shifts_left = 50;
    // Cumulative counts -> halved frequencies, sorted (selection-swap order), back to cumulative.
    for (int i = 0; i < entries; ++i) {
      at(i).cumfreq -= at(i + 1).cumfreq;
      at(i).cumfreq = (at(i).cumfreq + 1) >> 1;
    }
    for (int i = 0; i < entries - 1; ++i) {
      for (int j = i + 1; j < entries; ++j) {
        if (at(i).cumfreq < at(j).cumfreq) std::swap(at(i), at(j));
      }
    }
    for (int i = entries - 1; i >= 0; --i) at(i).cumfreq += at(i + 1).cumfreq;
  }
};

class QuantumDecoder {
 public:
  QuantumDecoder(ByteView data, const DecodeOptions& options) : in_(data), data_(data), options_(options) {}

  Result<Bytes> run() {
    const int wb = options_.window_bits;
    if (wb < 10 || wb > 21) return fail(Status::InvalidArgument, "Quantum window of 2^{} bytes is not supported", wb);
    window_size_ = 1u << wb;
    DecodeOptions inner = options_;
    std::uint64_t total = 0;
    for (const std::uint32_t b : options_.block_sizes) total += b;
    if (!inner.output_size && !options_.block_sizes.empty()) inner.output_size = total;
    if (!options_.properties.empty()) {
      if (options_.properties.size() != options_.block_sizes.size() * 4) {
        return fail(Status::InvalidArgument, "Quantum: properties must hold one 32-bit compressed size per block");
      }
      std::uint64_t at = 0;
      for (std::size_t i = 0; i < options_.block_sizes.size(); ++i) {
        block_starts_.push_back(at);
        at += legacy_le32(reinterpret_cast<const std::uint8_t*>(options_.properties.data()) + 4 * i);
      }
      if (at > data_.size()) return fail(Status::Decode, "Quantum data is truncated");
    }
    CVT_TRY(out_.start(inner, "Quantum"));
    const int i = wb * 2;
    for (int m = 0; m < 4; ++m) literal_[static_cast<std::size_t>(m)].init(m * 64, 64);
    pos3_.init(0, std::min(i, 24));
    pos4_.init(0, std::min(i, 36));
    pos_long_.init(0, i);
    length_.init(0, 27);
    selector_.init(0, 7);

    std::size_t frame = 0;
    while (!out_.done()) {
      // Start of a frame: position the reader and restart the coder.
      if (frame > 0) {
        if (frame < block_starts_.size()) {
          in_ = MsbBitReader(data_.subspan(static_cast<std::size_t>(block_starts_[frame])));
          base_ = static_cast<std::size_t>(block_starts_[frame]);
        } else {
          in_.align();
          std::size_t at = base_ + in_.byte_position();
          for (int k = 0; k < 4 && at < data_.size() && data_[at] == std::byte{0}; ++k) ++at;
          if (at > data_.size()) return fail(Status::Decode, "Quantum data is truncated");
          in_ = MsbBitReader(data_.subspan(at));
          base_ = at;
        }
      }
      high_ = 0xFFFF;
      low_ = 0;
      code_ = in_.get(16);
      const std::uint64_t frame_end = std::uint64_t{frame + 1} * frame_size;
      CVT_TRY(decode_frame(frame_end));
      ++frame;
      if (!legacy_progress(options_, base_ + in_.byte_position(), data_.size())) {
        return fail(Status::Cancelled, "the conversion was cancelled");
      }
    }
    return out_.finish(false);
  }

 private:
  int symbol(Model& m) {
    const std::uint32_t total = m.syms[0].cumfreq;
    std::uint32_t range = ((high_ - low_) & 0xFFFFu) + 1;
    const std::int64_t diff = static_cast<std::int64_t>(code_) - static_cast<std::int64_t>(low_) + 1;
    const auto symf = static_cast<std::uint32_t>((diff * total - 1) / range) & 0xFFFFu;
    int i = 1;
    while (i < m.entries && m.syms[static_cast<std::size_t>(i)].cumfreq > symf) ++i;
    const int sym = m.syms[static_cast<std::size_t>(i - 1)].sym;
    range = ((high_ - low_) & 0xFFFFu) + 1;
    high_ = (low_ + (m.syms[static_cast<std::size_t>(i - 1)].cumfreq * range) / total - 1) & 0xFFFFu;
    low_ = (low_ + (m.syms[static_cast<std::size_t>(i)].cumfreq * range) / total) & 0xFFFFu;
    for (int k = 0; k < i; ++k) m.syms[static_cast<std::size_t>(k)].cumfreq += 8;
    if (m.syms[0].cumfreq > 3800) m.rescale();
    while (true) {
      if ((low_ & 0x8000u) != (high_ & 0x8000u)) {
        if ((low_ & 0x4000u) != 0 && (high_ & 0x4000u) == 0) {
          code_ ^= 0x4000u;
          low_ &= 0x3FFFu;
          high_ |= 0x4000u;
        } else {
          break;
        }
      }
      low_ = (low_ << 1) & 0xFFFFu;
      high_ = ((high_ << 1) | 1u) & 0xFFFFu;
      code_ = ((code_ << 1) | in_.get(1)) & 0xFFFFu;
    }
    return sym;
  }

  Result<void> decode_frame(std::uint64_t frame_end) {
    while (!out_.done() && out_.size() < frame_end) {
      const int sel = symbol(selector_);
      if (sel < 4) {
        out_.put(static_cast<std::uint8_t>(symbol(literal_[static_cast<std::size_t>(sel)])));
      } else {
        std::uint32_t length = 0;
        int slot = 0;
        if (sel == 4) {
          slot = symbol(pos3_);
          length = 3;
        } else if (sel == 5) {
          slot = symbol(pos4_);
          length = 4;
        } else {
          const int ls = symbol(length_);
          length = length_base[static_cast<std::size_t>(ls)] + in_.get(length_extra[static_cast<std::size_t>(ls)]) + 5;
          slot = symbol(pos_long_);
        }
        const std::size_t s = static_cast<std::size_t>(slot);
        const int extra = position_extra[s];
        const std::uint32_t offset = position_base[s] + (extra != 0 ? in_.get(extra) : 0u) + 1;
        if (out_.size() + length > frame_end) return fail(Status::Decode, "Quantum match runs past the end of its frame");
        if (offset > window_size_ || !out_.copy(offset, length)) {
          return fail(Status::Decode, "Quantum match refers back before the start of the data");
        }
      }
      if (in_.overrun()) return fail(Status::Decode, "Quantum data is truncated");
    }
    return {};
  }

  MsbBitReader in_;
  ByteView data_;
  std::size_t base_ = 0;
  const DecodeOptions& options_;
  LegacyOutput out_;
  std::uint32_t window_size_ = 0;
  std::vector<std::uint64_t> block_starts_;
  std::uint32_t high_ = 0;
  std::uint32_t low_ = 0;
  std::uint32_t code_ = 0;
  std::array<Model, 4> literal_{};
  Model pos3_;
  Model pos4_;
  Model pos_long_;
  Model length_;
  Model selector_;
};

Result<Bytes> decode_quantum(ByteView data, const DecodeOptions& options) {
  QuantumDecoder decoder(data, options);
  return decoder.run();
}

constexpr std::array<CodecImpl, 1> table = {{
    {Method::Quantum, decode_quantum, nullptr, nullptr, nullptr, nullptr},
}};

}  // namespace

std::span<const CodecImpl> quantum_codecs() { return table; }

}  // namespace cvt::compression::detail
