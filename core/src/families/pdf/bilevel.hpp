// Convertible core - pdf family: bi-level image decoders (CCITT fax and JBIG2).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <vector>

#include "engine/errors.hpp"
#include "util/bytes.hpp"

namespace cvt::pdf::bilevel {

/// A 1-bit image, one byte per pixel for simplicity: 1 = black, 0 = white.
struct Bitmap {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> pixels;  ///< width * height, row-major
};

/// Parameters of the CCITTFaxDecode filter (PDF 32000-1, table 11).
struct CcittParams {
  int k = 0;                       ///< <0 pure 2-D (G4), 0 pure 1-D (G3 MH), >0 mixed (G3 MR)
  bool end_of_line = false;
  bool encoded_byte_align = false;
  std::uint32_t columns = 1728;
  std::uint32_t rows = 0;          ///< 0 = until the data ends
  bool end_of_block = true;
  bool black_is_1 = false;
  int damaged_rows_before_error = 0;
};

/// Decode CCITT Group 3/4 data into the PDF filter's OUTPUT bytes: packed
/// rows of `columns` bits, MSB first, each row padded to a byte, where a 0
/// bit is black unless `black_is_1` (exactly what the filter must produce).
/// Damaged data: the rows decoded so far are returned (the rest of the image
/// white) unless nothing could be decoded. `max_output` bounds the result.
[[nodiscard]] Result<Bytes> ccitt_decode(ByteView data, const CcittParams& params, std::uint64_t max_output);

/// Decode an embedded JBIG2 stream (PDF JBIG2Decode: no file header, page
/// information segment + region segments, optional JBIG2Globals segments
/// decoded first) into the filter's output: packed rows, MSB first, 1 = black
/// (the PDF convention for JBIG2Decode output is 1 = black as well; the
/// caller applies the /Decode array). Supports generic regions (arithmetic
/// MQ coder with all templates, TPGDON, AT pixels; MMR), generic refinement
/// regions, symbol dictionaries and text regions (Huffman and arithmetic),
/// pattern dictionaries and halftone regions as far as implemented, end of
/// stripe and striped pages with unknown height. `max_pixels` bounds the page.
[[nodiscard]] Result<Bytes> jbig2_decode(ByteView data, ByteView globals, std::uint64_t max_pixels,
                                         std::uint32_t* width_out = nullptr, std::uint32_t* height_out = nullptr);

}  // namespace cvt::pdf::bilevel
