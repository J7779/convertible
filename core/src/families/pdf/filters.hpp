// Convertible core - pdf family: stream filters.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "engine/errors.hpp"
#include "families/pdf/object.hpp"
#include "util/bytes.hpp"

namespace cvt::pdf {

struct FilterEnv {
  /// Upper bound for every intermediate and final result.
  std::uint64_t max_output = 1ull << 31;
  /// Stop before these image codecs and hand the data over still encoded
  /// (the image layer decodes them via the registry or in-house).
  bool stop_at_dct = true;
  bool stop_at_jpx = true;
  /// Resolve indirect references inside DecodeParms (may be null).
  std::function<Object(const Object&)> resolve;
  /// Decode another stream (JBIG2Globals) fully (may be null).
  std::function<Result<Bytes>(const Object&)> decode_stream;
  /// Cap on decoded pixels for JBIG2 pages.
  std::uint64_t max_pixels = 268'435'456ull;
};

struct DecodedData {
  Bytes data;
  /// Set when decoding stopped before an image filter ("DCTDecode",
  /// "JPXDecode"); `data` is then that filter's input and `params` its
  /// DecodeParms.
  std::string pending_filter;
  Dict params;
  /// True when a filter met damaged data and returned what it could.
  bool damaged = false;
};

/// Apply the /Filter chain (name or array) with /DecodeParms (dict, array
/// or null) to `raw`. Long names and the inline-image abbreviations are
/// accepted. Unknown filters fail with Status::Decode.
[[nodiscard]] Result<DecodedData> apply_filters(ByteView raw, const Object& filter, const Object& params,
                                               const FilterEnv& env);

/// Single filters, exposed for tests and the writer.
[[nodiscard]] Result<Bytes> flate_decode_tolerant(ByteView data, std::uint64_t max_output, bool* damaged);
[[nodiscard]] Result<Bytes> lzw_decode(ByteView data, bool early_change, std::uint64_t max_output);
[[nodiscard]] Result<Bytes> ascii85_decode(ByteView data, std::uint64_t max_output);
[[nodiscard]] Result<Bytes> asciihex_decode(ByteView data, std::uint64_t max_output);
[[nodiscard]] Result<Bytes> runlength_decode(ByteView data, std::uint64_t max_output);
/// Undo a TIFF (2) or PNG (10-15) predictor.
[[nodiscard]] Result<Bytes> apply_predictor(Bytes data, int predictor, int colors, int bpc, int columns);

[[nodiscard]] Bytes ascii85_encode(ByteView data);
[[nodiscard]] Bytes runlength_encode(ByteView data);
/// PNG-predict rows (choosing the best filter per row) for FlateDecode with
/// /Predictor 15.
[[nodiscard]] Bytes png_predict(ByteView data, int colors, int bpc, int columns);

}  // namespace cvt::pdf
