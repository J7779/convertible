// Convertible core - DEFLATE (RFC 1951), zlib (RFC 1950) and gzip (RFC 1952).
//
// In-house compressor and decompressor shared by every format that embeds
// DEFLATE data: ZIP containers and the office formats built on them, PNG,
// PDF streams, gzip files, ...
//
// Two levels of interface:
//
//   * one-shot functions (`deflate_raw`, `inflate_raw`, `zlib_*`, `gzip_*`)
//     for data that is already in memory, and
//   * the incremental `Inflater` / `Deflater` classes for data that arrives
//     or leaves in pieces.
//
// Decompression never trusts its input: every malformed stream is rejected
// with Status::Decode, and output is bounded by a caller-supplied limit
// (Status::Limit) that is enforced while decoding, before memory for the
// excess is allocated. Compression is deterministic: the same input and
// level give the same bytes on every platform.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>

#include "engine/errors.hpp"
#include "util/bytes.hpp"

namespace cvt::util {

/// "No limit" value for the `max_output` parameters below. Codecs should pass
/// `ctx.limits.max_decompressed_bytes` instead so decompression bombs are
/// rejected.
inline constexpr std::uint64_t no_output_limit = std::numeric_limits<std::uint64_t>::max();

/// Compression level: 0 = store, 1 = fastest ... 9 = smallest. Values outside
/// 0..9 are clamped.
inline constexpr int default_compression_level = 6;

/// Framing around the DEFLATE data.
enum class DeflateFormat {
  Raw,   ///< bare DEFLATE stream, no header or checksum (RFC 1951)
  Zlib,  ///< 2-byte header, DEFLATE data, Adler-32 (RFC 1950)
  Gzip,  ///< one gzip member: header, DEFLATE data, CRC-32 and length (RFC 1952)
};

/// Optional gzip member header fields.
struct GzipHeader {
  std::string filename;      ///< original file name (Latin-1 per the RFC; ASCII recommended), may be empty
  std::uint32_t mtime = 0;   ///< modification time, Unix seconds; 0 = not recorded
};

// --------------------------------------------------------------------------- one-shot

/// Raw DEFLATE stream (RFC 1951), no header or checksum.
[[nodiscard]] Result<Bytes> deflate_raw(ByteView data, int level = default_compression_level);
/// Inflate a raw DEFLATE stream. Fails with Status::Limit when the output
/// would exceed `max_output` bytes and Status::Decode on corrupt input.
/// `consumed`, when given, receives the number of input bytes used.
[[nodiscard]] Result<Bytes> inflate_raw(ByteView data, std::uint64_t max_output = no_output_limit,
                                        std::size_t* consumed = nullptr);

/// zlib stream (RFC 1950): 2-byte header, DEFLATE data, Adler-32.
[[nodiscard]] Result<Bytes> zlib_compress(ByteView data, int level = default_compression_level);
/// Decompress a zlib stream and verify its Adler-32. Streams that require a
/// preset dictionary are rejected; bytes after the end of the stream are
/// ignored.
[[nodiscard]] Result<Bytes> zlib_decompress(ByteView data, std::uint64_t max_output = no_output_limit);

/// gzip file (RFC 1952) with a single member.
[[nodiscard]] Result<Bytes> gzip_compress(ByteView data, int level = default_compression_level,
                                          const GzipHeader& header = {});
/// Decompress a gzip file; concatenated members are joined, CRC-32 and length
/// trailers are verified, and `max_output` bounds the total across members.
/// All optional header fields (extra data, name, comment, header CRC) are
/// understood, and a header CRC is checked when present. Bytes after a
/// complete member that do not start another member are ignored. `header`,
/// when given, receives the first member's header fields.
[[nodiscard]] Result<Bytes> gzip_decompress(ByteView data, std::uint64_t max_output = no_output_limit,
                                            GzipHeader* header = nullptr);

// --------------------------------------------------------------------------- incremental

/// Incremental decompressor for one DEFLATE, zlib or gzip stream.
///
///   util::Inflater inflater(util::Inflater::Format::Gzip);
///   Bytes out;
///   while (!inflater.finished() && read_more(chunk)) {
///     CVT_TRY_ASSIGN(const auto progress, inflater.feed(chunk, out, limit));
///     // progress.finished: the stream is complete and
///     // chunk.subspan(progress.consumed) is whatever follows it.
///     write(out);
///     out.clear();
///   }
///   if (!inflater.finished()) return fail(Status::Decode, "truncated stream");
///
/// The input may be cut anywhere, down to one byte per call. The output
/// buffer may be emptied (or not) between calls: the 32 KiB of history that
/// DEFLATE needs is kept inside the object, which uses about 50 KiB in all.
class Inflater {
 public:
  using Format = DeflateFormat;

  /// Outcome of one `feed()` call.
  struct Progress {
    /// Bytes of this call's `input` that were used. Always `input.size()`
    /// unless the stream ended inside this input; then the rest,
    /// `input.subspan(consumed)`, is data that follows the stream.
    std::size_t consumed = 0;
    /// Bytes appended to `output` by this call.
    std::size_t produced = 0;
    /// True once the stream is complete, including the zlib or gzip trailer
    /// and its checksum. A raw DEFLATE stream ends inside a byte; that last,
    /// partly used byte counts as consumed.
    bool finished = false;
  };

  explicit Inflater(Format format = Format::Raw);
  ~Inflater();
  Inflater(Inflater&&) noexcept;
  Inflater& operator=(Inflater&&) noexcept;

  /// Decode as much of `input` as possible, appending the result to
  /// `output`.
  ///
  /// `max_total_output` bounds the bytes produced by this object over all
  /// calls since construction or `reset()`: the call fails with
  /// Status::Limit as soon as one more byte would be needed, and `output`
  /// never grows past what the limit allows. Corrupt input fails with
  /// Status::Decode. After a failure `output` holds the bytes decoded up to
  /// the error, and every later call returns the same error.
  ///
  /// When the input ran out before the end of the stream the result has
  /// `finished == false`; call again with the bytes that follow. A caller
  /// with no more input must treat that as a truncated stream. Once the
  /// stream has finished, further calls consume nothing.
  [[nodiscard]] Result<Progress> feed(ByteView input, Bytes& output,
                                      std::uint64_t max_total_output = no_output_limit);

  /// True once the end of the stream has been decoded and verified.
  [[nodiscard]] bool finished() const noexcept;
  /// Total input bytes consumed / output bytes produced so far.
  [[nodiscard]] std::uint64_t total_in() const noexcept;
  [[nodiscard]] std::uint64_t total_out() const noexcept;
  /// Header fields of a gzip stream; complete once the first output byte has
  /// been produced or the stream has finished. Empty for other formats.
  [[nodiscard]] const GzipHeader& gzip_header() const noexcept;

  /// Forget everything and get ready for a new stream of the same format
  /// (for example the next member of a multi-member gzip file).
  void reset();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Incremental compressor producing one DEFLATE, zlib or gzip stream.
///
///   util::Deflater deflater(util::Deflater::Format::Zlib, 6);
///   Bytes out;
///   while (read_more(chunk)) CVT_TRY(deflater.feed(chunk, out));
///   CVT_TRY(deflater.finish(out));
///
/// The compressed bytes depend only on the data, the level and the header,
/// never on how the data was cut into `feed()` calls: feeding a buffer in
/// pieces gives exactly the output of the matching one-shot function.
/// Working memory grows with the data up to about 2 MiB (much less for
/// streams shorter than 32 KiB, and next to nothing at level 0).
class Deflater {
 public:
  using Format = DeflateFormat;

  /// `level` is clamped to 0..9. `header` is used by Format::Gzip only.
  explicit Deflater(Format format = Format::Raw, int level = default_compression_level,
                    const GzipHeader& header = {});
  ~Deflater();
  Deflater(Deflater&&) noexcept;
  Deflater& operator=(Deflater&&) noexcept;

  /// Compress more data. Whatever compressed bytes are ready are appended to
  /// `output`; that may be nothing, because input is buffered internally (up
  /// to a few hundred KiB) to find matches and choose block boundaries.
  /// Fails with Status::InvalidArgument after `finish()`.
  [[nodiscard]] Result<void> feed(ByteView input, Bytes& output);
  /// Compress everything still buffered, terminate the stream and append the
  /// trailer. Must be called exactly once; fails with
  /// Status::InvalidArgument when called again.
  [[nodiscard]] Result<void> finish(Bytes& output);

  [[nodiscard]] bool finished() const noexcept;
  /// Total input bytes accepted / output bytes produced so far.
  [[nodiscard]] std::uint64_t total_in() const noexcept;
  [[nodiscard]] std::uint64_t total_out() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cvt::util
