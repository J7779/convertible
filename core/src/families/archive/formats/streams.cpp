// Convertible core - archive family: single compressed files (gz, bz2, xz, lzma, zst, lz4, br, lz, z, zz, sz).
//
// Each is read as an Archive with exactly one entry, named from the gzip
// header when it carries one and otherwise from the file name without its
// compression extension, so "report.pdf.gz" extracts to "report.pdf".
// Writing takes an Archive with exactly one file (any file becomes one through
// the Blob -> Archive bridge) and compresses it.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <format>

#include "engine/registry.hpp"
#include "families/archive/archive_common.hpp"
#include "families/archive/compression.hpp"
#include "families/archive/formats/formats.hpp"
#include "util/text.hpp"

namespace cvt::archive {

namespace {

struct Stream {
  std::string_view id;
  std::string_view name;
  compression::Method method;
  std::vector<std::string> extensions;
  std::string_view mime;
  int popularity;
  std::vector<Signature> signatures;
};

struct GzipInfo {
  std::string name;
  std::uint32_t mtime = 0;
};

// FNAME and MTIME of the first gzip member.
GzipInfo gzip_info(ByteView data) {
  GzipInfo info;
  if (data.size() < 10) return info;
  const auto flags = std::to_integer<std::uint8_t>(data[3]);
  info.mtime = std::to_integer<std::uint32_t>(data[4]) | (std::to_integer<std::uint32_t>(data[5]) << 8) |
               (std::to_integer<std::uint32_t>(data[6]) << 16) | (std::to_integer<std::uint32_t>(data[7]) << 24);
  std::size_t pos = 10;
  if (flags & 0x04u) {  // FEXTRA
    if (pos + 2 > data.size()) return info;
    const std::size_t xlen = std::to_integer<std::size_t>(data[pos]) | (std::to_integer<std::size_t>(data[pos + 1]) << 8);
    pos += 2 + xlen;
  }
  if ((flags & 0x08u) && pos < data.size()) {  // FNAME, Latin-1
    const std::string_view rest = as_chars(data.subspan(pos));
    const std::size_t nul = rest.find('\0');
    if (nul != std::string_view::npos) info.name = decode_name(rest.substr(0, nul), false, false);
  }
  return info;
}

std::string base_name(std::string_view path) {
  const std::size_t slash = path.find_last_of("/\\");
  return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

void register_stream(Registry& registry, const Stream& s) {
  ArchiveFormat f;
  f.info.id = std::string(s.id);
  f.info.name = std::string(s.name);
  f.info.description = std::format("A single file compressed with {}.", compression::method_name(s.method));
  f.info.category = Category::Archive;
  f.info.extensions = s.extensions;
  f.info.mime = std::string(s.mime);
  f.info.popularity = s.popularity;
  f.info.signatures = s.signatures;
  const compression::Method method = s.method;
  const std::vector<std::string> extensions = s.extensions;
  if (method == compression::Method::LzmaAlone || method == compression::Method::Zlib) {
    // No fixed magic: content checks.
    f.info.sniff = [method](std::span<const std::byte> data, std::string_view) {
      return compression::detect(data) == method ? 60 : 0;
    };
  }
  f.read = [method, extensions](ReadContext& ctx) -> Result<ir::Archive> {
    EntrySink sink(ctx, ctx.input.size());
    compression::DecodeOptions d;
    d.max_output = sink.budget();
    d.progress = ctx.progress;
    CVT_TRY_ASSIGN(Bytes data, compression::decompress(method, ctx.input, d));
    CVT_TRY(sink.charge(data.size()));
    ir::ArchiveEntry entry;
    if (method == compression::Method::Gzip) {
      const GzipInfo info = gzip_info(ctx.input);
      entry.path = base_name(info.name);
      entry.mtime = info.mtime;
    }
    if (entry.path.empty() || !ir::sanitize_archive_path(entry.path)) {
      entry.path = strip_compression_extension(ctx.filename, extensions);
    }
    entry.bytes = std::move(data);
    CVT_TRY(sink.add(std::move(entry)));
    return sink.finish();
  };
  if (compression::can_encode(method)) {
    f.write = [method](const ir::Archive& archive, WriteContext& ctx) -> Result<Bytes> {
      const WritePolicy policy = resolve_write_policy(ctx);
      CVT_TRY_ASSIGN(const auto entries, prepare_entries(archive, policy, ctx, {.symlinks = false, .directories = false}));
      std::size_t files = 0;
      const WriteEntry* file = nullptr;
      for (const WriteEntry& e : entries) {
        if (e.type != EntryType::File) continue;
        ++files;
        file = &e;
      }
      if (files != 1) {
        return fail(Status::Encode,
                    "{} compresses exactly one file, but the input has {}; use a tar.* or zip archive for several files",
                    compression::method_name(method), files);
      }
      compression::EncodeOptions e;
      e.level = policy.level;
      e.progress = ctx.progress;
      if (method == compression::Method::Gzip) {
        const std::string name = base_name(file->path);
        if (util::is_valid_utf8(name)) {
          // gzip names are Latin-1: keep them only when they can be stored faithfully.
          bool latin1 = true;
          std::string encoded;
          for (std::size_t i = 0; i < name.size();) {
            const util::Utf8Decoded cp = util::decode_utf8(name, i);
            i += cp.length;
            if (!cp.ok || cp.code_point > 0xFF || cp.code_point == 0) {
              latin1 = false;
              break;
            }
            encoded.push_back(static_cast<char>(cp.code_point));
          }
          if (latin1) e.original_name = encoded;
        }
        if (!policy.fixed_timestamps && file->source != nullptr && file->source->mtime > 0 &&
            file->source->mtime <= 0xFFFFFFFFll) {
          e.mtime = static_cast<std::uint32_t>(file->source->mtime);
        }
      }
      return compression::compress(method, file->data(), e);
    };
    f.write_options = standard_write_options();
  }
  register_archive_format(registry, std::move(f));
}

}  // namespace

void register_stream_formats(Registry& registry) {
  using compression::Method;
  const std::array<Stream, 11> streams = {{
      {"gz", "Gzip compressed file", Method::Gzip, {"gz", "gzip"}, "application/gzip", 75, {Signature::at(0, "1F 8B 08")}},
      {"bz2", "Bzip2 compressed file", Method::Bzip2, {"bz2", "bzip2"}, "application/x-bzip2", 50, {Signature::at(0, "'BZh' 3?")}},
      {"xz", "XZ compressed file", Method::Xz, {"xz"}, "application/x-xz", 55, {Signature::at(0, "FD '7zXZ' 00")}},
      {"lzma", "LZMA compressed file", Method::LzmaAlone, {"lzma"}, "application/x-lzma", 20, {}},
      {"zst", "Zstandard compressed file", Method::Zstd, {"zst", "zstd"}, "application/zstd", 50, {Signature::at(0, "28 B5 2F FD")}},
      {"lz4", "LZ4 compressed file", Method::Lz4Frame, {"lz4"}, "application/x-lz4", 30,
       {Signature::at(0, "04 22 4D 18"), Signature::at(0, "02 21 4C 18")}},
      {"br", "Brotli compressed file", Method::Brotli, {"br"}, "application/x-brotli", 25, {}},
      {"lz", "Lzip compressed file", Method::Lzip, {"lz"}, "application/x-lzip", 15, {Signature::at(0, "'LZIP' 01")}},
      {"z", "Unix compress file", Method::Lzw, {"z"}, "application/x-compress", 15, {Signature::at(0, "1F 9D")}},
      {"zz", "Zlib compressed data", Method::Zlib, {"zz", "zlib"}, "application/zlib", 15, {}},
      {"sz", "Snappy framed file", Method::SnappyFramed, {"sz"}, "application/x-snappy-framed", 10,
       {Signature::at(0, "FF 06 00 00 'sNaPpY'")}},
  }};
  for (const Stream& s : streams) register_stream(registry, s);
}

}  // namespace cvt::archive
