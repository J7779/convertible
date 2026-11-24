// Convertible core - archive family: helpers shared by the legacy DOS-era archive readers (LHA, ARJ, ARC).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "ir/archive.hpp"
#include "util/bytes.hpp"

namespace cvt::archive::legacy {

[[nodiscard]] inline std::uint16_t le16(ByteView d, std::size_t at) noexcept {
  return static_cast<std::uint16_t>(std::to_integer<unsigned>(d[at]) | (std::to_integer<unsigned>(d[at + 1]) << 8));
}
[[nodiscard]] inline std::uint32_t le32(ByteView d, std::size_t at) noexcept {
  return static_cast<std::uint32_t>(le16(d, at)) | (static_cast<std::uint32_t>(le16(d, at + 2)) << 16);
}
[[nodiscard]] inline std::uint8_t u8(ByteView d, std::size_t at) noexcept { return std::to_integer<std::uint8_t>(d[at]); }

/// MS-DOS date/time word pair (local time, read as UTC) -> Unix seconds; 0 when unset.
[[nodiscard]] std::int64_t dos_time(std::uint16_t date, std::uint16_t time) noexcept;

// Readers (Archive IR). Each validates its own signature.
[[nodiscard]] Result<ir::Archive> read_lha(ReadContext& ctx);
[[nodiscard]] Result<ir::Archive> read_arj(ReadContext& ctx);
[[nodiscard]] Result<ir::Archive> read_arc(ReadContext& ctx);

// Content sniffers: confidence 0..100.
[[nodiscard]] int sniff_lha(std::span<const std::byte> data, std::string_view filename);
[[nodiscard]] int sniff_arj(std::span<const std::byte> data, std::string_view filename);
[[nodiscard]] int sniff_arc(std::span<const std::byte> data, std::string_view filename);

// ARC's own methods, exposed for tests of the decoders in isolation.
/// Undo ARC's run-length encoding (0x90 = repeat marker), output at most `limit` bytes.
[[nodiscard]] Result<Bytes> arc_unrle(ByteView data, std::uint64_t limit);
/// Method 4: Huffman ("squeezed") followed by run-length decoding.
[[nodiscard]] Result<Bytes> arc_unsqueeze(ByteView data, std::uint64_t limit);
/// Methods 5, 6, 7: 12-bit LZW with a hashed string table ("crunched", the
/// first versions); 6 and 7 add run-length decoding, 7 uses the faster hash.
[[nodiscard]] Result<Bytes> arc_uncrunch_old(ByteView data, int method, std::uint64_t limit);

}  // namespace cvt::archive::legacy
