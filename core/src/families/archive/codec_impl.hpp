// Convertible core - archive family: how individual codecs plug into compression.hpp.
//
// Each codec source file under codecs/ exposes one function returning a
// constant table of `CodecImpl` entries (one per Method it implements).
// compression.cpp looks methods up in these tables; a table may be empty
// while a codec is not available in a build.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <memory>
#include <span>

#include "families/archive/compression.hpp"

namespace cvt::compression::detail {

struct CodecImpl {
  Method method = Method::Store;
  /// One-shot functions; nullptr = direction not supported.
  Result<Bytes> (*decode)(ByteView data, const DecodeOptions& options) = nullptr;
  Result<Bytes> (*encode)(ByteView data, const EncodeOptions& options) = nullptr;
  /// Native incremental implementations (optional). Without them,
  /// make_decoder / make_encoder fall back to buffering the whole stream and
  /// running the one-shot function.
  std::unique_ptr<Decoder> (*make_decoder)(const DecodeOptions& options) = nullptr;
  std::unique_ptr<Encoder> (*make_encoder)(const EncodeOptions& options) = nullptr;
  /// Raw coder properties for `encoder_properties` (LzmaRaw, Lzma2Raw).
  Result<Bytes> (*properties)(const EncodeOptions& options, std::uint64_t input_size) = nullptr;
};

// One table per codec source file (codecs/<name>.cpp).
std::span<const CodecImpl> deflate_codecs();     // Store, Deflate, Zlib, Gzip (wrapping util/deflate)
std::span<const CodecImpl> bzip2_codecs();       // Bzip2
std::span<const CodecImpl> lzma_codecs();        // Xz, LzmaAlone, LzmaRaw, Lzma2Raw, Lzip
std::span<const CodecImpl> zstd_codecs();        // Zstd
std::span<const CodecImpl> lz4_codecs();         // Lz4Frame, Lz4Block
std::span<const CodecImpl> brotli_codecs();      // Brotli
std::span<const CodecImpl> snappy_codecs();      // Snappy, SnappyFramed
std::span<const CodecImpl> lzw_codecs();         // Lzw
std::span<const CodecImpl> lzo_codecs();         // Lzo1x
std::span<const CodecImpl> deflate64_codecs();   // Deflate64, Mszip
std::span<const CodecImpl> lzx_codecs();         // Lzx, LzxWim
std::span<const CodecImpl> quantum_codecs();     // Quantum
std::span<const CodecImpl> xpress_codecs();      // XpressPlain, XpressHuffman, Lzms
std::span<const CodecImpl> lzh_codecs();         // Lh, Lzs, Arj
std::span<const CodecImpl> zip_legacy_codecs();  // Implode, Shrink, Reduce
std::span<const CodecImpl> ppmd_codecs();        // Ppmd7, Ppmd8
std::span<const CodecImpl> apple_codecs();       // Lzfse, Adc

// Filters (codecs/filters.cpp).
Result<std::size_t> filter_decode_impl(const Filter& filter, std::span<std::byte> data, FilterState& state, bool last);
Result<std::size_t> filter_encode_impl(const Filter& filter, std::span<std::byte> data, FilterState& state, bool last);
Result<Bytes> bcj2_decode_impl(ByteView main_stream, ByteView call_stream, ByteView jump_stream, ByteView range_stream,
                               std::uint64_t output_size, std::uint64_t max_output);

/// Grow `out` by `extra` bytes while honouring the caller's ceiling:
/// Status::Limit when `already + extra > max_output`.
[[nodiscard]] Result<void> check_output_room(std::uint64_t already, std::uint64_t extra, std::uint64_t max_output);

}  // namespace cvt::compression::detail
