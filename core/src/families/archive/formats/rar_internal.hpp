// Convertible core - archive family: internals of the RAR reader (formats/rar*.cpp).
//
//   BitInput     MSB-first bit reader over a block of compressed data
//   LzWindow     the sliding dictionary shared by the RAR 3 and RAR 5 decoders
//   Unpack5      RAR 5 / RAR 7 LZ decoder (filters DELTA, E8, E8E9, ARM)
//   Unpack3      RAR 2.9 / 3.x / 4.x decoder (LZ, PPMd blocks, standard VM filters)
//   Blake2sp     the BLAKE2sp hash of RAR 5 file checksums
//   key derivation for RAR 3 (AES-128) and RAR 5 (AES-256) encryption
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "engine/errors.hpp"
#include "engine/progress.hpp"
#include "families/archive/codecs/huffman.hpp"
#include "families/archive/formats/rar_ppmd.hpp"
#include "util/bytes.hpp"

namespace cvt::archive::rar {

// --------------------------------------------------------------------------- bit input

/// MSB-first bit reader with random access to its position. Bits past the
/// end read as zero; `overrun()` tells when the reader went past the end.
class BitInput {
 public:
  BitInput() = default;
  explicit BitInput(ByteView data) noexcept
      : data_(reinterpret_cast<const std::uint8_t*>(data.data())), size_(data.size()) {}

  /// Up to 25 bits, MSB first.
  [[nodiscard]] std::uint32_t peek(int n) const noexcept {
    const std::size_t at = static_cast<std::size_t>(pos_ >> 3);
    std::uint32_t v;
    if (at + 4 <= size_) {
      v = (std::uint32_t{data_[at]} << 24) | (std::uint32_t{data_[at + 1]} << 16) | (std::uint32_t{data_[at + 2]} << 8) |
          std::uint32_t{data_[at + 3]};
    } else {
      v = 0;
      for (std::size_t i = 0; i < 4; ++i) v = (v << 8) | (at + i < size_ ? std::uint32_t{data_[at + i]} : 0u);
    }
    v <<= (pos_ & 7);
    return n == 0 ? 0 : v >> (32 - n);
  }
  void skip(int n) noexcept { pos_ += static_cast<std::uint64_t>(n); }
  [[nodiscard]] std::uint32_t get(int n) noexcept {
    const std::uint32_t v = peek(n);
    skip(n);
    return v;
  }
  /// Up to 32 bits.
  [[nodiscard]] std::uint32_t get_wide(int n) noexcept {
    if (n <= 16) return get(n);
    const std::uint32_t high = get(n - 16);
    return (high << 16) | get(16);
  }
  void align() noexcept { pos_ = (pos_ + 7) & ~std::uint64_t{7}; }
  [[nodiscard]] std::uint64_t position() const noexcept { return pos_; }
  void seek(std::uint64_t bit) noexcept { pos_ = bit; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool overrun() const noexcept { return pos_ > std::uint64_t{size_} * 8; }
  /// Byte-wise access for the PPMd range decoder (the position must be aligned).
  [[nodiscard]] std::uint8_t byte() noexcept {
    const std::size_t at = static_cast<std::size_t>(pos_ >> 3);
    pos_ += 8;
    return at < size_ ? data_[at] : std::uint8_t{0};
  }

 private:
  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
  std::uint64_t pos_ = 0;
};

// --------------------------------------------------------------------------- window

/// The dictionary: a ring buffer of a power-of-two size holding the most
/// recent output. Positions are absolute (bytes produced since the start of
/// the stream).
class LzWindow {
 public:
  /// Allocate `size` bytes rounded up to a power of two (at least 64 KiB).
  void allocate(std::uint64_t size);
  [[nodiscard]] std::uint64_t capacity() const noexcept { return buf_.size(); }

  void put(std::uint8_t b) noexcept {
    buf_[static_cast<std::size_t>(pos_) & mask_] = b;
    ++pos_;
  }
  /// Copy `length` bytes from `distance` back. False when the distance
  /// reaches before the start of the stream or beyond the window.
  [[nodiscard]] bool copy(std::uint64_t distance, std::uint32_t length) noexcept;
  /// Copy `count` bytes starting at absolute position `from` out of the window.
  void read(std::uint64_t from, std::size_t count, std::uint8_t* out) const noexcept;

  [[nodiscard]] std::uint64_t pos() const noexcept { return pos_; }
  /// Matches may not reach before the current position from now on (a new,
  /// non-solid stream begins).
  void restart() noexcept { origin_ = pos_; }

 private:
  std::vector<std::uint8_t> buf_;
  std::size_t mask_ = 0;
  std::uint64_t pos_ = 0;
  std::uint64_t origin_ = 0;
};

/// Where a decoder writes one file's bytes.
struct FileOutput {
  Bytes* bytes = nullptr;
  /// The declared unpacked size; when `size_known` is false, output may
  /// grow up to `budget` bytes.
  std::uint64_t size = 0;
  bool size_known = true;
  std::uint64_t budget = 0;
  Progress progress;
};

// --------------------------------------------------------------------------- RAR 5

class Unpack5 {
 public:
  /// `window` = dictionary bytes to keep (already checked against limits).
  /// `extended` = RAR 7 distance alphabet (algorithm version 1).
  void init(std::uint64_t window);
  /// Decode one file: `solid` continues the state of the previous file.
  [[nodiscard]] Result<void> decode(ByteView packed, bool solid, bool extended, FileOutput& out);

 private:
  struct Filter {
    std::uint64_t start = 0;
    std::uint32_t length = 0;
    std::uint8_t type = 0;
    std::uint8_t channels = 0;
  };
  struct Block {
    std::uint64_t end_bit = 0;  // first bit after the block
    bool last = false;
  };
  [[nodiscard]] Result<void> read_block_header(BitInput& in, Block& block);
  [[nodiscard]] Result<void> read_tables(BitInput& in, bool extended);
  [[nodiscard]] Result<void> read_filter(BitInput& in);
  [[nodiscard]] Result<void> flush(FileOutput& out, bool final);
  [[nodiscard]] static Result<void> emit(FileOutput& out, const std::uint8_t* data, std::size_t count);
  void apply_filter(const Filter& f, std::vector<std::uint8_t>& block, std::uint64_t file_offset) const;

  LzWindow window_;
  compression::detail::HuffmanTable main_, dist_, low_, rep_;
  bool tables_ = false;
  bool extended_ = false;
  std::array<std::uint64_t, 4> old_{};
  std::uint32_t last_length_ = 0;
  std::vector<Filter> filters_;
  std::uint64_t written_ = 0;     // absolute position up to which output was emitted
  std::uint64_t file_start_ = 0;  // absolute position of the current file
  std::vector<std::uint8_t> scratch_;
};

// --------------------------------------------------------------------------- RAR 3

class Unpack3 {
 public:
  void init(std::uint64_t window);
  [[nodiscard]] Result<void> decode(ByteView packed, bool solid, FileOutput& out, std::uint64_t memory_budget);

  enum class FilterType : std::uint8_t { None, E8, E8E9, Itanium, Delta, Rgb, Audio };

 private:
  struct Program {
    FilterType type = FilterType::None;
  };
  struct Pending {
    std::size_t program = 0;
    std::uint64_t start = 0;
    std::uint32_t length = 0;
    std::array<std::uint32_t, 7> regs{};
  };
  [[nodiscard]] Result<void> read_tables(BitInput& in, std::uint64_t memory_budget);
  [[nodiscard]] Result<bool> read_end_of_block(BitInput& in, std::uint64_t memory_budget);
  [[nodiscard]] Result<void> read_vm_code(BitInput& in);
  [[nodiscard]] Result<void> read_vm_code_ppm();
  [[nodiscard]] Result<void> add_vm_code(std::uint32_t first, ByteView code);
  [[nodiscard]] Result<void> flush(FileOutput& out);
  void emit(FileOutput& out, const std::uint8_t* data, std::size_t count);
  [[nodiscard]] Result<void> run_filter(const Pending& p, std::vector<std::uint8_t>& data, std::uint64_t file_offset) const;
  [[nodiscard]] Result<void> start_ppmd(BitInput& in, std::uint64_t memory_budget);
  [[nodiscard]] Result<int> decode_ppmd_lz(BitInput& in, std::uint64_t memory_budget, std::uint64_t file_end);
  void reset_filters(bool all);

  LzWindow window_;
  compression::detail::HuffmanTable main_, dist_, low_, rep_;
  std::array<std::uint8_t, 404> old_table_{};
  bool tables_ = false;
  bool ppm_block_ = false;
  std::array<std::uint64_t, 4> old_{};
  std::uint64_t last_dist_ = 0;
  std::uint32_t last_length_ = 0;
  std::uint32_t prev_low_ = 0;
  std::uint32_t low_rep_ = 0;
  int esc_char_ = 2;
  std::unique_ptr<PpmdDecoder> ppmd_;
  BitInput* ppm_in_ = nullptr;
  std::vector<Program> programs_;
  std::vector<std::uint32_t> old_lengths_;
  std::size_t last_filter_ = 0;
  std::vector<Pending> stack_;
  std::uint64_t written_ = 0;
  std::uint64_t file_start_ = 0;
  std::vector<std::uint8_t> scratch_;
};

/// Name of the standard RAR 3 VM filter whose code has this length and
/// CRC-32, or FilterType::None.
[[nodiscard]] Unpack3::FilterType identify_vm_filter(std::size_t length, std::uint32_t crc) noexcept;

// --------------------------------------------------------------------------- BLAKE2

/// BLAKE2s-256 (RFC 7693) with tree parameters, and BLAKE2sp (8 leaves).
class Blake2s {
 public:
  /// Plain BLAKE2s-256 without key.
  Blake2s() noexcept : Blake2s(1, 1, 0, 0, 0, false) {}
  Blake2s(std::uint8_t fanout, std::uint8_t depth, std::uint32_t node_offset, std::uint8_t node_depth,
          std::uint8_t inner_length, bool last_node) noexcept;
  void update(const std::uint8_t* data, std::size_t size) noexcept;
  void update(ByteView data) noexcept { update(reinterpret_cast<const std::uint8_t*>(data.data()), data.size()); }
  [[nodiscard]] std::array<std::uint8_t, 32> finish() noexcept;

 private:
  void compress(const std::uint8_t* block, bool last) noexcept;
  std::array<std::uint32_t, 8> h_{};
  std::array<std::uint8_t, 64> buf_{};
  std::size_t buffered_ = 0;
  std::uint64_t counter_ = 0;
  bool last_node_ = false;
};

class Blake2sp {
 public:
  Blake2sp() noexcept;
  void update(ByteView data) noexcept;
  [[nodiscard]] std::array<std::uint8_t, 32> finish() noexcept;

 private:
  std::array<Blake2s, 8> leaves_;
  std::array<std::uint8_t, 8 * 64> buf_{};
  std::size_t buffered_ = 0;
};

// --------------------------------------------------------------------------- keys

struct Rar5Keys {
  std::array<std::uint8_t, 32> key{};
  std::array<std::uint8_t, 32> hash_key{};
  std::array<std::uint8_t, 8> check{};
};
/// PBKDF2-HMAC-SHA256 with 2^log2_count rounds, continued 16 and 32 rounds
/// further for the checksum key and the password check value.
[[nodiscard]] Rar5Keys rar5_keys(std::string_view password, ByteView salt, unsigned log2_count);

struct Rar3Key {
  std::array<std::uint8_t, 16> key{};
  std::array<std::uint8_t, 16> iv{};
};
/// RAR 3 key derivation: SHA-1 over 2^18 rounds of UTF-16LE password + salt + counter.
[[nodiscard]] Rar3Key rar3_key(std::string_view password, ByteView salt);

}  // namespace cvt::archive::rar
