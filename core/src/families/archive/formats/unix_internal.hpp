// Convertible core - archive family: shared pieces of the Unix archive formats (cpio, ar, deb, rpm, shar, warc, xar).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/context.hpp"
#include "families/archive/archive_common.hpp"
#include "util/bytes.hpp"

namespace cvt::archive::unixfmt {

// ---- cpio (unix_cpio.cpp)

/// Read every entry of a cpio stream (newc, crc, odc, old binary in either
/// byte order) into `sink`, up to the `TRAILER!!!` record.
[[nodiscard]] Result<void> read_cpio(ByteView data, EntrySink& sink, const Context& ctx);
/// Serialise prepared entries as a newc ("070701") cpio stream padded to 512 bytes.
[[nodiscard]] Result<Bytes> write_cpio(const std::vector<WriteEntry>& entries, const Context& ctx);
/// True when `data` starts with a plausible cpio header of any variant.
[[nodiscard]] bool is_cpio(ByteView data) noexcept;

// ---- ar (unix_ar.cpp)

struct ArMember {
  std::string name;  ///< as stored (GNU '/' terminator and BSD '#1/' prefix removed)
  ByteView data;
  std::int64_t mtime = 0;
  std::uint32_t mode = 0;
};

/// Split a GNU or BSD ar archive into its members; symbol tables and the
/// GNU long-name table are consumed, thin archives are refused.
[[nodiscard]] Result<std::vector<ArMember>> parse_ar(ByteView data, const Context& ctx);
[[nodiscard]] Result<ir::Archive> read_ar(ReadContext& ctx);
[[nodiscard]] Result<Bytes> write_ar(const ir::Archive& archive, WriteContext& ctx);

// ---- packages (unix_pkg.cpp)

[[nodiscard]] Result<ir::Archive> read_deb(ReadContext& ctx);
[[nodiscard]] Result<ir::Archive> read_rpm(ReadContext& ctx);

// ---- shar (unix_shar.cpp)

[[nodiscard]] Result<Bytes> write_shar(const ir::Archive& archive, WriteContext& ctx);

// ---- warc (unix_warc.cpp)

[[nodiscard]] Result<ir::Archive> read_warc(ReadContext& ctx);
/// True when `data` is a WARC file, plain or gzip-compressed per record.
[[nodiscard]] int sniff_warc(ByteView data) noexcept;

// ---- xar (unix_xar.cpp)

[[nodiscard]] Result<ir::Archive> read_xar(ReadContext& ctx);

// ---- helpers (unix.cpp)

/// Seconds since the epoch for an ISO 8601 / RFC 3339 UTC time
/// ("2021-06-15T12:00:00Z", fractional seconds and a missing 'Z' allowed).
[[nodiscard]] std::optional<std::int64_t> parse_iso_time(std::string_view text) noexcept;
/// Unsigned number in `base` (8, 10 or 16) from a fixed-width, space- or
/// NUL-padded field; nullopt for anything else.
[[nodiscard]] std::optional<std::uint64_t> parse_field(std::string_view field, int base) noexcept;

}  // namespace cvt::archive::unixfmt
