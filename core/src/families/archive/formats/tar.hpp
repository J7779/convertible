// Convertible core - archive family: tar reading and writing, shared with deb, rpm and the compound formats.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <vector>

#include "engine/context.hpp"
#include "families/archive/archive_common.hpp"
#include "util/bytes.hpp"

namespace cvt::archive {

enum class TarFormat { Pax, Gnu, Ustar };

/// Read every entry of an uncompressed tar stream into `sink`. `prefix`
/// is prepended to every path (used by deb for control files).
[[nodiscard]] Result<void> read_tar(ByteView data, EntrySink& sink, const Context& ctx, std::string_view prefix = {});

/// Serialise prepared entries as a tar stream (two zero blocks, padded to 10 KiB).
[[nodiscard]] Result<Bytes> write_tar(const std::vector<WriteEntry>& entries, TarFormat format, const Context& ctx);

/// True when `block` (at least 512 bytes) is a tar header with a valid checksum.
[[nodiscard]] bool is_tar_header(ByteView block) noexcept;

}  // namespace cvt::archive
