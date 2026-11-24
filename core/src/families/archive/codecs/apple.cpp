// Convertible core - archive family: Apple codecs table (Method::Lzfse,
// Method::Adc) and the ADC decoder.
//
// ADC (Apple Data Compression, DMG chunk type 0x80000004) is a byte-oriented
// LZ77 with three element kinds, chosen by the first byte b:
//   1xxxxxxx                     literal run of (b & 0x7F) + 1 bytes
//   01xxxxxx hi lo               copy (b & 0x3F) + 4 bytes from distance (hi << 8 | lo) + 1
//   00LLLLhh lo                  copy ((b >> 2) & 0x0F) + 3 bytes from distance (hh << 8 | lo) + 1
// There is no header and no end marker; a chunk ends with its input.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>

#include "families/archive/codecs/apple_codecs.hpp"

namespace cvt::compression::detail {

Result<Bytes> adc_decode(ByteView data, const DecodeOptions& options) {
  const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
  const std::size_t n = data.size();
  const bool sized = options.output_size.has_value();
  if (sized) CVT_TRY(check_output_room(0, *options.output_size, options.max_output));
  const std::uint64_t cap = sized ? *options.output_size : options.max_output;
  const auto room = [&](std::size_t have, std::size_t len) -> Result<void> {
    if (len <= cap - have) return {};
    if (sized) return fail(Status::Decode, "ADC data is longer than the declared {} bytes", cap);
    return fail(Status::Limit, "decompressed data exceeds the limit of {} bytes", cap);
  };
  Bytes out;
  if (sized) out.reserve(static_cast<std::size_t>(cap));
  std::size_t i = 0;
  while (i < n) {
    if (options.stop_after != 0 && out.size() >= options.stop_after) return out;
    if (sized && out.size() == cap) break;
    if ((i & 0xFFFFF) == 0xFFFFF && !options.progress.report(static_cast<double>(i) / static_cast<double>(n))) {
      return fail(Status::Cancelled, "the conversion was cancelled");
    }
    const unsigned b = p[i];
    std::size_t len = 0;
    std::size_t dist = 0;
    if ((b & 0x80u) != 0) {
      len = (b & 0x7Fu) + 1;
      if (len > n - i - 1) return fail(Status::Decode, "ADC literal run is truncated");
      CVT_TRY(room(out.size(), len));
      out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(i + 1),
                 data.begin() + static_cast<std::ptrdiff_t>(i + 1 + len));
      i += 1 + len;
      continue;
    }
    if ((b & 0x40u) != 0) {
      if (n - i < 3) return fail(Status::Decode, "ADC copy is truncated");
      len = (b & 0x3Fu) + 4;
      dist = ((static_cast<std::size_t>(p[i + 1]) << 8) | p[i + 2]) + 1;
      i += 3;
    } else {
      if (n - i < 2) return fail(Status::Decode, "ADC copy is truncated");
      len = ((b >> 2) & 0x0Fu) + 3;
      dist = ((static_cast<std::size_t>(b & 3u) << 8) | p[i + 1]) + 1;
      i += 2;
    }
    if (dist > out.size()) return fail(Status::Decode, "ADC copy reaches before the start of the data");
    CVT_TRY(room(out.size(), len));
    const std::size_t from = out.size() - dist;
    for (std::size_t k = 0; k < len; ++k) out.push_back(out[from + k]);
  }
  if (options.stop_after != 0 && out.size() >= options.stop_after) return out;
  if (sized && out.size() != cap) {
    return fail(Status::Decode, "ADC data decodes to {} bytes instead of {}", out.size(), cap);
  }
  return out;
}

namespace {

const std::array<CodecImpl, 2> table = {{
    {Method::Lzfse, lzfse_decode, nullptr, nullptr, nullptr, nullptr},
    {Method::Adc, adc_decode, nullptr, nullptr, nullptr, nullptr},
}};

}  // namespace

std::span<const CodecImpl> apple_codecs() { return table; }

}  // namespace cvt::compression::detail
