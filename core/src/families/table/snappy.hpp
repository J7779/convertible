// Convertible core - table family: Snappy block compression (raw format,
// as used inside Parquet pages, ORC and Avro), written from the format
// description: a varint uncompressed length followed by literal and copy
// elements. Not the framed ".sz" stream format.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>

#include "engine/errors.hpp"
#include "util/bytes.hpp"

namespace cvt::table {

/// Decompress one Snappy block. Status::Decode for malformed input,
/// Status::Limit when the declared size exceeds `max_output`.
[[nodiscard]] Result<Bytes> snappy_decompress(ByteView data, std::uint64_t max_output);
/// Compress one Snappy block (greedy matcher over 4-byte hashes).
[[nodiscard]] Bytes snappy_compress(ByteView data);

}  // namespace cvt::table
