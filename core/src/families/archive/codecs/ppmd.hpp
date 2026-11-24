// Convertible core - archive family: PPMd var.H and var.I (private interface).
//
// PPMd ("prediction by partial matching", Dmitry Shkarin's variants H and I)
// models each byte with the statistics of the contexts that preceded it and
// codes it with a range coder. Three users share the code:
//
//   * Method::Ppmd7  - var.H with the 7z range coder (7z method 03 04 01).
//                      Decoder and encoder (ppmd_h.cpp).
//   * RAR 1.5-4.x    - var.H with the original carry-less range coder and
//                      RAR's own escape protocol on top (RarPpmdDecoder
//                      below; the RAR format code drives it).
//   * Method::Ppmd8  - var.I revision 1 as in ZIP method 98 (ppmd_i.cpp).
//
// The models are written from the published descriptions of the algorithm and
// follow the reference memory layout exactly (12-byte units, the same
// sub-allocator and the same restart rules): a PPMd stream can only be
// decoded by a model that runs out of memory at precisely the same symbol as
// the encoder's did.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>

#include "families/archive/codec_impl.hpp"

namespace cvt::compression::detail {

// ---------------------------------------------------------------- shared tables

/// Unit-count <-> size-class tables of the PPMd sub-allocator (identical in
/// var.H and var.I): 38 classes of 1,2,3,4, 6,8,10,12, 15,18,21,24, 28,32,...,128 units.
struct PpmdUnitTables {
  std::array<std::uint8_t, 38> indx2units{};
  std::array<std::uint8_t, 128> units2indx{};
};

[[nodiscard]] constexpr PpmdUnitTables make_ppmd_unit_tables() noexcept {
  PpmdUnitTables t;
  unsigned k = 0;
  for (unsigned i = 0; i < 38; ++i) {
    unsigned step = i >= 12 ? 4 : (i >> 2) + 1;
    while (step-- != 0) t.units2indx[k++] = static_cast<std::uint8_t>(i);
    t.indx2units[i] = static_cast<std::uint8_t>(k);
  }
  return t;
}

inline constexpr PpmdUnitTables ppmd_units = make_ppmd_unit_tables();

/// Escape estimate after a binary-context miss, indexed by probability >> 10.
inline constexpr std::array<std::uint8_t, 16> ppmd_exp_escape = {25, 14, 9, 7, 5, 5, 4, 4, 4, 3, 3, 3, 2, 2, 2, 2};
/// Initial binary-context escape estimates.
inline constexpr std::array<std::uint16_t, 8> ppmd_init_bin_esc = {0x3CDD, 0x1F3F, 0x59BF, 0x48F3,
                                                                   0x64A1, 0x5ABC, 0x6632, 0x6051};

struct PpmdHeapFree {
  void operator()(std::uint8_t* p) const noexcept { std::free(p); }
};
using PpmdHeap = std::unique_ptr<std::uint8_t, PpmdHeapFree>;

/// Zeroed model memory. calloc hands large blocks over as untouched zero
/// pages, so a model costs only the memory it actually uses.
[[nodiscard]] inline PpmdHeap ppmd_alloc(std::size_t bytes) {
  return PpmdHeap(static_cast<std::uint8_t*>(std::calloc(bytes, 1)));
}

// ---------------------------------------------------------------- RAR interface

/// Supplies the bytes of a RAR PPMd block to RarPpmdDecoder. RAR interleaves
/// PPMd blocks with LZ blocks in one stream, so the RAR decoder owns the input
/// and hands bytes over one at a time.
class PpmdByteSource {
 public:
  virtual ~PpmdByteSource() = default;
  /// The next input byte. At the end of the input return 0 and remember that
  /// the stream overran; the caller reports the truncation (the range coder
  /// reads up to four bytes ahead, so an overrun is only an error if decoding
  /// continues to need them).
  virtual std::uint8_t next_byte() = 0;
};

/// PPMd var.H as used by RAR 2.9/3.x/4.x ("-mc<order>:<memory>t+", file
/// format 2.9): Shkarin's original carry-less range coder (Subbotin style,
/// TOP = 2^24, BOT = 2^15) on top of the same model as Method::Ppmd7.
///
/// How the RAR decoder drives it, block by block:
///
///   RarPpmdDecoder ppm;                       // keep one per RAR stream (solid archives reuse it)
///   // PPM block header: flags byte, [memory byte if flags & 0x20], [escape char if flags & 0x40]
///   if (flags & 0x20) {                       // "reset": new model
///     int order = (flags & 0x1F) + 1;
///     if (order > 16) order = 16 + (order - 16) * 3;
///     CVT_TRY(ppm.restart(order, (memory_byte + 1) << 20));
///   } else if (!ppm.has_model()) {
///     return fail(Status::Decode, ...);       // continuing a model that never existed
///   }
///   CVT_TRY(ppm.init_range_coder(source));    // after the header bytes; reads 4 bytes
///   for (;;) {
///     CVT_TRY_ASSIGN(const std::uint8_t c, ppm.decode_symbol());
///     ... RAR's escape protocol (escape char, then 0 = end of PPM block,
///         2 = end of file, 3 = VM code, 4 = match, 5 = run, other = literal) ...
///   }
///
/// `restart` keeps the allocation when the memory size is unchanged. A model
/// that is not reset by the next PPM block keeps all its statistics; only the
/// range coder is re-initialised. The object is not copyable; it holds up to
/// 256 MiB.
class RarPpmdDecoder {
 public:
  RarPpmdDecoder();
  ~RarPpmdDecoder();
  RarPpmdDecoder(const RarPpmdDecoder&) = delete;
  RarPpmdDecoder& operator=(const RarPpmdDecoder&) = delete;

  /// Start a fresh model: `max_order` 2..64, `memory_bytes` 2 KiB..256 MiB
  /// (RAR stores (MiB - 1) in a byte). Status::InvalidArgument outside those
  /// ranges, Status::Limit when the memory cannot be allocated.
  [[nodiscard]] Result<void> restart(int max_order, std::uint32_t memory_bytes);
  /// True once `restart` succeeded (a model exists to continue).
  [[nodiscard]] bool has_model() const noexcept;
  /// (Re)start the range decoder on `source` (reads four bytes). The source
  /// must outlive every following decode_symbol() call.
  [[nodiscard]] Result<void> init_range_coder(PpmdByteSource& source);
  /// Decode one byte. Status::Decode when the data is inconsistent (also for
  /// the model's own "escape below order 0", which RAR never writes).
  [[nodiscard]] Result<std::uint8_t> decode_symbol();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------- codec entry points

Result<Bytes> ppmd7_decode(ByteView data, const DecodeOptions& options);
Result<Bytes> ppmd7_encode(ByteView data, const EncodeOptions& options);
Result<Bytes> ppmd7_properties(const EncodeOptions& options, std::uint64_t input_size);
Result<Bytes> ppmd8_decode(ByteView data, const DecodeOptions& options);

/// The decoders first run the model in a heap sized for the output (1 MiB +
/// 64 bytes per output byte) when the stream declares more, and fall back to
/// the declared size the moment the small heap could make a different
/// decision (its free gap or text area runs out). These entry points fix the
/// size of that first run (0 = automatic) so tests can exercise the fallback.
Result<Bytes> ppmd7_decode_probed(ByteView data, const DecodeOptions& options, std::uint32_t probe_bytes);
Result<Bytes> ppmd8_decode_probed(ByteView data, const DecodeOptions& options, std::uint32_t probe_bytes);

}  // namespace cvt::compression::detail
