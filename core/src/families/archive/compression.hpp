// Convertible core - archive family: general-purpose compression codecs.
//
// The one stable entry point for every format that embeds compressed data:
// fonts (WOFF2 -> Brotli), Parquet and ORC (Snappy, Zstandard, LZ4, gzip,
// Brotli), PDF (LZW, Flate), disk images (zlib, bzip2, LZFSE, ADC, xz), and
// the archive containers of this family. Everything is implemented in-house.
//
//   #include "families/archive/compression.hpp"
//   namespace cz = cvt::compression;
//
//   // one-shot, bounded by the conversion's limits
//   cz::DecodeOptions d;
//   d.max_output = ctx.limits.max_decompressed_bytes;
//   CVT_TRY_ASSIGN(Bytes plain, cz::decompress(cz::Method::Brotli, payload, d));
//
//   cz::EncodeOptions e;
//   e.level = 9;
//   CVT_TRY_ASSIGN(Bytes packed, cz::compress(cz::Method::Zstd, plain, e));
//
//   // streaming: feed input in pieces, collect output in pieces
//   CVT_TRY_ASSIGN(auto decoder, cz::make_decoder(cz::Method::Xz, d));
//   Bytes out;
//   CVT_TRY_ASSIGN(cz::Step step, decoder->decode(chunk, out, /*last=*/false));
//
// Rules shared by every codec:
//   * Decoders never trust their input. Malformed or unsupported data fails
//     with Status::Decode, output beyond `DecodeOptions::max_output` fails
//     with Status::Limit before the excess is allocated, and checksums that
//     the format carries are verified (unless `verify_checksums` is false).
//   * Encoders are deterministic: identical input and options give identical
//     bytes on every platform, independent of how the input was cut into
//     `encode()` calls. Levels run 0 (fastest) .. 9 (smallest, "ultra").
//   * No global state, no threads, no I/O. Everything is re-entrant.
//   * A method may be decode-only; `can_encode()` / `can_decode()` tell.
//
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/errors.hpp"
#include "engine/progress.hpp"
#include "util/bytes.hpp"

namespace cvt::compression {

/// "No limit" for `DecodeOptions::max_output`. Callers inside a conversion
/// should pass `ctx.limits.max_decompressed_bytes` instead.
inline constexpr std::uint64_t unlimited = std::numeric_limits<std::uint64_t>::max();

/// Every algorithm the family knows. Containers ("framed" methods) carry
/// their own header, sizes and checksums; raw methods need the parameters
/// documented next to them in `DecodeOptions`.
enum class Method : std::uint8_t {
  // ---- encode + decode -----------------------------------------------------
  Store,         ///< identity (no compression)
  Deflate,       ///< raw DEFLATE, RFC 1951
  Zlib,          ///< zlib stream, RFC 1950 (".zz", PDF FlateDecode)
  Gzip,          ///< gzip file, RFC 1952; concatenated members are joined
  Bzip2,         ///< bzip2 stream ("BZh"); concatenated streams are joined
  Xz,            ///< .xz container: LZMA2 + BCJ/delta filters, CRC32/CRC64/SHA-256 checks
  LzmaAlone,     ///< ".lzma" (LZMA_Alone): 13-byte header + LZMA1 data
  LzmaRaw,       ///< raw LZMA1 data; `properties` = 5 bytes (lc/lp/pb byte + LE32 dictionary size)
  Lzma2Raw,      ///< raw LZMA2 chunks; `properties` = 1 byte (dictionary size code)
  Lzip,          ///< .lz container (LZMA1 member(s) + CRC32 + sizes)
  Zstd,          ///< Zstandard frames, RFC 8878; skippable frames are skipped
  Lz4Frame,      ///< LZ4 frame format (magic 04 22 4D 18) with optional checksums
  Lz4Block,      ///< one raw LZ4 block; decoding needs `output_size` or `max_output`
  Brotli,        ///< Brotli stream, RFC 7932
  Snappy,        ///< raw Snappy block (varint length + elements)
  SnappyFramed,  ///< Snappy framing format ("sNaPpY" stream identifier, CRC-32C)
  Lzw,           ///< Unix `compress` (.Z, magic 1F 9D)
  // ---- decode only ----------------------------------------------------------
  Deflate64,      ///< "enhanced deflate" (ZIP method 9), raw
  Mszip,          ///< CAB MSZIP: "CK"-prefixed deflate blocks sharing history; one call = one CAB folder,
                  ///  `block_sizes` lists the uncompressed size of each CFDATA block
  Lzx,            ///< LZX as in CAB/CHM; `window_bits` 15..21; `block_sizes` = CFDATA/reset-interval frames
  LzxWim,         ///< LZX as in WIM (32 KiB chunks, window 2^15, no E8 translation offset header)
  Quantum,        ///< CAB Quantum; `window_bits` 10..21
  XpressPlain,    ///< MS-XCA "plain LZ77"
  XpressHuffman,  ///< MS-XCA "LZ77+Huffman" (WIM XPRESS); `output_size` required
  Lzms,           ///< WIM LZMS (decode only, optional; may report unsupported)
  Lzo1x,          ///< raw LZO1X-1..999 block; `output_size` or `max_output`
  Ppmd7,          ///< PPMd var.H as in 7z; `properties` = order byte + LE32 memory; `output_size` required
  Ppmd8,          ///< PPMd var.I rev.1 as in ZIP method 98; the 2-byte ZIP parameter word comes first in the data
  Implode,        ///< PKZIP implode (method 6); `flags` = general-purpose bits (bit 1: 8K window, bit 2: 3 trees)
  Shrink,         ///< PKZIP shrink (method 1), dynamic LZW
  Reduce,         ///< PKZIP reduce (methods 2..5); `level` field of DecodeOptions = factor 1..4
  Lh,             ///< LHA "-lhN-" (N = `window_bits`: lh1=12 dynamic Huffman (`flags`=1), lh4..lh7 static Huffman 12..16)
  Lzs,            ///< LHA -lzs- / -lz5- (`window_bits` 11 or 12)
  Arj,            ///< ARJ methods 1..3 (like -lh6-, 26-bit window) and 4 (`flags` = method)
  Lzfse,          ///< Apple LZFSE (bvx1/bvx2/bvxn/bvx- blocks)
  Adc,            ///< Apple Data Compression (DMG)
};

/// Short lower-case name ("zstd", "lzma2-raw", ...), stable for logs and options.
[[nodiscard]] std::string_view method_name(Method method) noexcept;
/// Inverse of `method_name` (ASCII case-insensitive); also accepts common
/// aliases ("gz", "bz2", "zst", "lz4", "br", "lzma", "lz", "z", "sz").
[[nodiscard]] std::optional<Method> parse_method(std::string_view name) noexcept;

/// True when this build can decode / encode the method.
[[nodiscard]] bool can_decode(Method method) noexcept;
[[nodiscard]] bool can_encode(Method method) noexcept;

// --------------------------------------------------------------------------- options

/// Integrity check stored by the xz container.
enum class XzCheck : std::uint8_t { None = 0, Crc32 = 1, Crc64 = 4, Sha256 = 10 };

/// Branch-converter and delta filters (xz, 7z, RAR). They make executable
/// code and fixed-stride data more compressible; the decode direction is
/// what readers need, the encode direction is offered to writers.
enum class FilterKind : std::uint8_t {
  X86,       ///< BCJ x86 (E8/E9 relative calls/jumps)
  PowerPc,   ///< big-endian PowerPC branches
  Ia64,      ///< Itanium bundles
  Arm,       ///< ARM BL (little endian)
  ArmThumb,  ///< ARM Thumb BL
  Arm64,     ///< ARM64 BL and ADRP
  Sparc,     ///< SPARC call
  RiscV,     ///< RISC-V JAL/AUIPC (xz 5.6)
  Delta,     ///< byte-wise delta with `distance` 1..256
};

struct Filter {
  FilterKind kind = FilterKind::X86;
  /// Delta: distance 1..256. Branch filters: start offset (normally 0).
  std::uint32_t parameter = 0;
};

/// Parameters for decoding. Fields a method does not use are ignored.
struct DecodeOptions {
  /// Hard ceiling for the total decompressed output; exceeding it is
  /// Status::Limit and is detected before memory for the excess is taken.
  std::uint64_t max_output = unlimited;
  /// When non-zero: stop successfully as soon as this many bytes have been
  /// produced and return exactly that prefix (used for content sniffing;
  /// checksums of the incomplete stream are then not verified).
  std::uint64_t stop_after = 0;
  /// Exact decompressed size, for raw methods whose data does not mark its
  /// own end (LzmaRaw without end marker, Lz4Block, Lzo1x, Ppmd7, LZX,
  /// Quantum, XPRESS, Lh, Implode, ...). Also checked when the format stores
  /// a size of its own.
  std::optional<std::uint64_t> output_size;
  /// Coder properties as stored by the container (see `Method`).
  ByteView properties{};
  /// Window size in bits for LZX / Quantum / Lh / Lzs.
  int window_bits = 0;
  /// Codec-specific flags (see `Method`).
  std::uint32_t flags = 0;
  /// Reduce factor 1..4 (Method::Reduce).
  int level = 0;
  /// Uncompressed size of each block, where a container splits one stream
  /// into independently framed blocks (CAB CFDATA, CHM reset intervals).
  std::span<const std::uint32_t> block_sizes{};
  /// Verify CRCs / hashes that the stream carries.
  bool verify_checksums = true;
  /// Optional progress and cancellation (fraction = input consumed).
  Progress progress{};
};

/// Parameters for encoding. Fields a method does not use are ignored.
struct EncodeOptions {
  /// 0 = fastest (Store for methods that have a stored mode) .. 9 = smallest
  /// ("ultra": the slowest settings the method offers). Clamped.
  int level = 6;
  /// Override of the dictionary / window size in bytes (LZMA, LZMA2, xz,
  /// lzip, Zstandard, Brotli); 0 = chosen from the level and the input size.
  std::uint32_t dictionary_size = 0;
  /// xz integrity check.
  XzCheck check = XzCheck::Crc64;
  /// xz / 7z pre-filters applied before LZMA2, in order.
  std::vector<Filter> filters;
  /// LZ4 frame block size in bytes (65536, 262144, 1048576 or 4194304; 0 = 4 MiB).
  std::uint32_t block_size = 0;
  /// Store a checksum of the content (Zstandard, LZ4 frame).
  bool content_checksum = true;
  /// Record the uncompressed size in the header (Zstandard, LZ4 frame, LzmaAlone).
  bool content_size = true;
  /// gzip header fields (FNAME and MTIME); empty / 0 = not recorded.
  std::string original_name;
  std::uint32_t mtime = 0;
  /// Optional progress and cancellation (fraction = input consumed).
  Progress progress{};
};

// --------------------------------------------------------------------------- one-shot

/// Decompress a whole buffer.
[[nodiscard]] Result<Bytes> decompress(Method method, ByteView data, const DecodeOptions& options = {});
/// Compress a whole buffer.
[[nodiscard]] Result<Bytes> compress(Method method, ByteView data, const EncodeOptions& options = {});

/// The coder properties a raw encoder writes for these options and an input
/// of `input_size` bytes (LzmaRaw: 5 bytes, Lzma2Raw: 1 byte, others: empty).
/// Containers such as 7z store them next to the data.
[[nodiscard]] Result<Bytes> encoder_properties(Method method, const EncodeOptions& options, std::uint64_t input_size);

// --------------------------------------------------------------------------- streaming

/// Outcome of one `Decoder::decode` call.
struct Step {
  /// Bytes of the call's input that were used. Less than the input only when
  /// the stream ended inside it; the rest is data that follows the stream.
  std::size_t consumed = 0;
  /// Bytes appended to the output by this call.
  std::size_t produced = 0;
  /// True once the stream (including its trailer/checksum) is complete.
  bool finished = false;
};

/// Incremental decoder. The input may be cut anywhere. Some methods decode
/// block by block and keep up to one block of input buffered (bzip2: 900 kB,
/// Zstandard: 128 KiB, LZ4: 4 MiB, Brotli: one meta-block, raw methods
/// without block structure: all of it); memory stays bounded by the method's
/// block size and window except where noted.
class Decoder {
 public:
  virtual ~Decoder() = default;
  /// Decode as much as possible, appending to `output`. `last` = no input
  /// follows this call; a stream still unfinished then is Status::Decode
  /// ("truncated"). Totals are bounded by `DecodeOptions::max_output`.
  /// After a failure every later call returns the same error.
  [[nodiscard]] virtual Result<Step> decode(ByteView input, Bytes& output, bool last) = 0;
  [[nodiscard]] virtual bool finished() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t total_in() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t total_out() const noexcept = 0;
};

/// Incremental encoder. Output depends only on the data and the options,
/// never on how the data was split between `encode` calls.
class Encoder {
 public:
  virtual ~Encoder() = default;
  /// Accept more input; compressed bytes that are ready are appended.
  [[nodiscard]] virtual Result<void> encode(ByteView input, Bytes& output) = 0;
  /// Flush everything and terminate the stream. Call exactly once.
  [[nodiscard]] virtual Result<void> finish(Bytes& output) = 0;
  [[nodiscard]] virtual std::uint64_t total_in() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t total_out() const noexcept = 0;
};

[[nodiscard]] Result<std::unique_ptr<Decoder>> make_decoder(Method method, const DecodeOptions& options = {});
[[nodiscard]] Result<std::unique_ptr<Encoder>> make_encoder(Method method, const EncodeOptions& options = {});

// --------------------------------------------------------------------------- filters

/// Undo a branch-converter / delta filter in place. `stream_offset` is the
/// position of `data[0]` in the filtered stream (call repeatedly over
/// consecutive buffers with `state` kept between calls). Returns the number
/// of leading bytes fully processed; a branch filter may leave up to 4..15
/// trailing bytes untouched until more data (or `last`) arrives.
struct FilterState {
  std::uint64_t position = 0;  ///< bytes processed so far
  std::uint32_t x86_mask = 0;  ///< BCJ x86 state
  std::array<std::uint8_t, 256> history{};  ///< delta history
  std::uint32_t history_pos = 0;
};
[[nodiscard]] Result<std::size_t> filter_decode(const Filter& filter, std::span<std::byte> data, FilterState& state,
                                                bool last);
/// The forward direction, used by xz / 7z writers.
[[nodiscard]] Result<std::size_t> filter_encode(const Filter& filter, std::span<std::byte> data, FilterState& state,
                                                bool last);

/// 7z BCJ2: combine the main, call, jump and range-coded streams into the
/// original x86 code of `output_size` bytes.
[[nodiscard]] Result<Bytes> bcj2_decode(ByteView main_stream, ByteView call_stream, ByteView jump_stream,
                                        ByteView range_stream, std::uint64_t output_size,
                                        std::uint64_t max_output = unlimited);

// --------------------------------------------------------------------------- detection

/// The framed method whose magic number starts `data` (gzip, bzip2, xz,
/// LzmaAlone (heuristic), lzip, Zstandard (also skippable frames), LZ4 frame,
/// Snappy framed, Unix compress, zlib (header check)), or nullopt.
[[nodiscard]] std::optional<Method> detect(ByteView data) noexcept;

}  // namespace cvt::compression
