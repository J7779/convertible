// Convertible core - archive family: ZIP writer.
//
// Output is deterministic: entries in the order given, sizes and CRCs known
// up front (no data descriptors), Zip64 records only for the entries and
// fields that need them, Unix "made by" with the mode in the external
// attributes (files, directories with a trailing '/', symlinks whose target
// is the content), the UTF-8 flag on non-ASCII names, an extended timestamp
// (0x5455, modification time only) unless `timestamps` is "fixed", and never
// owner ids or names. Data that does not shrink is stored. Encryption is
// WinZip AES (AE-1, or AE-2 below 20 bytes as WinZip does) or traditional
// ZipCrypto; salts and encryption headers come from `entry_nonce`, so the
// same input always gives the same archive.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <format>
#include <limits>

#include "families/archive/compression.hpp"
#include "families/archive/formats/zip_internal.hpp"
#include "util/crc.hpp"
#include "util/zip.hpp"

namespace cvt::archive::zip {

using util::ByteReader;
using util::ByteWriter;

namespace {

constexpr std::uint32_t u32_max = 0xFFFFFFFFu;
constexpr auto made_by = static_cast<std::uint16_t>((host_unix << 8) | 63);  // Unix, APPNOTE 6.3

struct Record {
  std::string name;
  std::uint16_t needed = 10;
  std::uint16_t flags = 0;
  std::uint16_t method = 0;
  util::DosDateTime dos;
  std::uint32_t crc = 0;
  std::uint64_t compressed = 0;
  std::uint64_t uncompressed = 0;
  std::uint64_t offset = 0;
  std::uint32_t external = 0;
  Bytes extra;  // shared by the local and central headers (timestamp, AES)
};

struct MethodChoice {
  std::uint16_t zip_method = method_deflate;
  compression::Method codec = compression::Method::Deflate;
  std::uint16_t needed = 20;
};

MethodChoice method_choice(const std::string& name) {
  using compression::Method;
  if (name == "bzip2") return {method_bzip2, Method::Bzip2, 46};
  if (name == "lzma") return {method_lzma, Method::LzmaRaw, 63};
  if (name == "zstd") return {method_zstd, Method::Zstd, 63};
  if (name == "xz") return {method_xz, Method::Xz, 63};
  if (name == "store") return {method_store, Method::Store, 10};
  return {};
}

bool is_ascii(std::string_view s) {
  return std::ranges::all_of(s, [](char c) { return static_cast<unsigned char>(c) < 0x80; });
}

void put_extra(Bytes& out, std::uint16_t id, ByteView value) {
  ByteWriter w;
  w.u16le(id).u16le(static_cast<std::uint16_t>(value.size())).bytes(value);
  append(out, w.view());
}

// Compress `data`; returns the ZIP method used and the payload (stored when nothing is gained).
Result<std::pair<std::uint16_t, Bytes>> pack(ByteView data, const MethodChoice& choice, int level, const Progress& progress,
                                             std::uint16_t& flags, std::uint16_t& needed) {
  if (data.empty() || level <= 0 || choice.zip_method == method_store) return std::pair{method_store, Bytes()};
  if (!compression::can_encode(choice.codec)) {
    return fail(Status::Encode, "ZIP method {} is not available in this build", method_description(choice.zip_method));
  }
  compression::EncodeOptions e;
  e.level = level;
  e.progress = progress;
  Bytes packed;
  if (choice.zip_method == method_lzma) {
    e.content_size = false;  // write the end-of-stream marker (flag bit 1)
    CVT_TRY_ASSIGN(const Bytes props, compression::encoder_properties(compression::Method::LzmaRaw, e, data.size()));
    // LZMA SDK version 9.20, property size, properties.
    ByteWriter w;
    w.u8(9).u8(20).u16le(static_cast<std::uint16_t>(props.size())).bytes(props);
    packed = w.take();
    CVT_TRY_ASSIGN(const Bytes body, compression::compress(compression::Method::LzmaRaw, data, e));
    append(packed, body);
  } else {
    CVT_TRY_ASSIGN(packed, compression::compress(choice.codec, data, e));
  }
  if (packed.size() >= data.size()) return std::pair{method_store, Bytes()};
  if (choice.zip_method == method_lzma) flags |= flag_lzma_eos;
  needed = std::max(needed, choice.needed);
  return std::pair{choice.zip_method, std::move(packed)};
}

void write_local(ByteWriter& out, const Record& r) {
  const bool big = r.compressed >= u32_max || r.uncompressed >= u32_max;
  Bytes extra;
  if (big) {
    ByteWriter z;
    z.u64le(r.uncompressed).u64le(r.compressed);
    put_extra(extra, extra_zip64, z.view());
  }
  append(extra, r.extra);
  out.u32le(sig_local)
      .u16le(big ? std::max<std::uint16_t>(r.needed, 45) : r.needed)
      .u16le(r.flags)
      .u16le(r.method)
      .u16le(r.dos.time)
      .u16le(r.dos.date)
      .u32le(r.crc)
      .u32le(big ? u32_max : static_cast<std::uint32_t>(r.compressed))
      .u32le(big ? u32_max : static_cast<std::uint32_t>(r.uncompressed))
      .u16le(static_cast<std::uint16_t>(r.name.size()))
      .u16le(static_cast<std::uint16_t>(extra.size()))
      .text(r.name)
      .bytes(extra);
}

void write_central(ByteWriter& out, const Record& r) {
  ByteWriter z;
  if (r.uncompressed >= u32_max) z.u64le(r.uncompressed);
  if (r.compressed >= u32_max) z.u64le(r.compressed);
  if (r.offset >= u32_max) z.u64le(r.offset);
  Bytes extra;
  if (z.size() != 0) put_extra(extra, extra_zip64, z.view());
  append(extra, r.extra);
  const std::uint16_t needed = z.size() != 0 ? std::max<std::uint16_t>(r.needed, 45) : r.needed;
  out.u32le(sig_central)
      .u16le(made_by)
      .u16le(needed)
      .u16le(r.flags)
      .u16le(r.method)
      .u16le(r.dos.time)
      .u16le(r.dos.date)
      .u32le(r.crc)
      .u32le(static_cast<std::uint32_t>(std::min<std::uint64_t>(r.compressed, u32_max)))
      .u32le(static_cast<std::uint32_t>(std::min<std::uint64_t>(r.uncompressed, u32_max)))
      .u16le(static_cast<std::uint16_t>(r.name.size()))
      .u16le(static_cast<std::uint16_t>(extra.size()))
      .u16le(0)  // comment
      .u16le(0)  // disk
      .u16le(0)  // internal attributes
      .u32le(r.external)
      .u32le(static_cast<std::uint32_t>(std::min<std::uint64_t>(r.offset, u32_max)))
      .text(r.name)
      .bytes(extra);
}

std::string clip_comment(const std::string& comment) {
  if (comment.size() <= 65535) return comment;
  std::size_t cut = 65535;
  while (cut > 0 && (static_cast<unsigned char>(comment[cut]) & 0xC0u) == 0x80u) --cut;
  return comment.substr(0, cut);
}

}  // namespace

Result<Bytes> write_zip(const ir::Archive& archive, WriteContext& ctx) {
  const WritePolicy policy = resolve_write_policy(ctx);
  CVT_TRY_ASSIGN(const std::vector<WriteEntry> entries,
                 prepare_entries(archive, policy, ctx, {.symlinks = true, .directories = true}));
  const MethodChoice choice =
      method_choice(ctx.options.get_enum("method", "deflate", {"deflate", "store", "bzip2", "lzma", "zstd", "xz"}));
  const std::string encryption = ctx.options.get_enum("encryption", "aes256", {"aes256", "aes128", "zipcrypto"});
  const std::string& password = policy.password;

  std::uint64_t total = 0;
  for (const WriteEntry& e : entries) total += e.data().size();
  ByteWriter out(static_cast<std::size_t>(std::min<std::uint64_t>(total + entries.size() * 128, 64u << 20)));
  std::vector<Record> records;
  records.reserve(entries.size());
  std::uint64_t done = 0;
  for (const WriteEntry& e : entries) {
    const double span = static_cast<double>(std::max<std::uint64_t>(total, 1));
    CVT_TRY(ctx.checkpoint(static_cast<double>(done) / span));
    Record r;
    r.name = e.type == EntryType::Directory ? e.path + "/" : e.path;
    if (r.name.size() > 65535) return fail(Status::Encode, "the name '{}...' is too long for ZIP", r.name.substr(0, 64));
    if (!is_ascii(r.name)) r.flags |= flag_utf8;
    r.dos = util::unix_to_dos_time(e.mtime);
    std::uint32_t type_bits = 0100000u;
    if (e.type == EntryType::Directory) type_bits = 0040000u;
    if (e.type == EntryType::Symlink) type_bits = 0120000u;
    r.external = ((type_bits | (e.mode & 07777u)) << 16) | (e.type == EntryType::Directory ? 0x10u : 0u);
    r.needed = e.type == EntryType::Directory ? 20 : 10;
    if (!policy.fixed_timestamps && e.mtime >= std::numeric_limits<std::int32_t>::min() &&
        e.mtime <= std::numeric_limits<std::int32_t>::max()) {
      ByteWriter ut;
      ut.u8(1).i32le(static_cast<std::int32_t>(e.mtime));
      put_extra(r.extra, extra_timestamp, ut.view());
    }

    const ByteView content = e.type == EntryType::File      ? e.data()
                             : e.type == EntryType::Symlink ? as_bytes(e.link_target())
                                                            : ByteView{};
    r.crc = util::crc32(content);
    r.uncompressed = content.size();
    Bytes payload;
    if (e.type != EntryType::Directory) {
      const Progress progress = ctx.progress.sub(static_cast<double>(done) / span,
                                                 static_cast<double>(done + content.size()) / span);
      const int level = e.type == EntryType::Symlink ? 0 : policy.level;
      CVT_TRY_ASSIGN(auto packed, pack(content, choice, level, progress, r.flags, r.needed));
      r.method = packed.first;
      payload = packed.first == method_store ? Bytes(content.begin(), content.end()) : std::move(packed.second);

      if (!password.empty()) {
        r.flags |= flag_encrypted;
        if (encryption == "zipcrypto") {
          const auto nonce = entry_nonce("zipcrypto", password, r.name, r.crc, content);
          Bytes sealed(zipcrypto_header_size + payload.size());
          for (std::size_t i = 0; i + 1 < zipcrypto_header_size; ++i) sealed[i] = static_cast<std::byte>(nonce[i]);
          sealed[zipcrypto_header_size - 1] = static_cast<std::byte>(r.crc >> 24);
          std::ranges::copy(payload, sealed.begin() + zipcrypto_header_size);
          ZipCrypto cipher(password);
          cipher.encrypt(sealed);
          payload = std::move(sealed);
          r.needed = std::max<std::uint16_t>(r.needed, 20);
        } else {
          const std::uint8_t strength = encryption == "aes128" ? 1 : 3;
          const auto nonce = entry_nonce(encryption, password, r.name, r.crc, content);
          const ByteView salt = as_bytes(std::span<const std::uint8_t>(nonce.data(), aes_salt_size(strength)));
          payload = aes_encrypt(payload, password, strength, salt);
          const std::uint16_t vendor_version = content.size() < 20 ? 2 : 1;
          ByteWriter aes;
          aes.u16le(vendor_version).text("AE").u8(strength).u16le(r.method);
          put_extra(r.extra, extra_aes, aes.view());
          r.method = method_aes;
          if (vendor_version == 2) r.crc = 0;
          r.needed = std::max<std::uint16_t>(r.needed, 51);
        }
      }
    }
    r.compressed = payload.size();
    r.offset = out.size();
    write_local(out, r);
    out.bytes(payload);
    done += content.size();
    records.push_back(std::move(r));
  }

  const std::uint64_t cd_offset = out.size();
  for (const Record& r : records) write_central(out, r);
  const std::uint64_t cd_size = out.size() - cd_offset;
  const std::uint64_t count = records.size();
  if (count >= 0xFFFFu || cd_size >= u32_max || cd_offset >= u32_max) {
    const std::uint64_t z_offset = out.size();
    out.u32le(sig_zip64_eocd)
        .u64le(44)
        .u16le(made_by)
        .u16le(45)
        .u32le(0)
        .u32le(0)
        .u64le(count)
        .u64le(count)
        .u64le(cd_size)
        .u64le(cd_offset);
    out.u32le(sig_zip64_locator).u32le(0).u64le(z_offset).u32le(1);
  }
  const std::string comment = policy.strip_metadata ? std::string() : clip_comment(archive.comment);
  out.u32le(sig_eocd)
      .u16le(0)
      .u16le(0)
      .u16le(static_cast<std::uint16_t>(std::min<std::uint64_t>(count, 0xFFFFu)))
      .u16le(static_cast<std::uint16_t>(std::min<std::uint64_t>(count, 0xFFFFu)))
      .u32le(static_cast<std::uint32_t>(std::min<std::uint64_t>(cd_size, u32_max)))
      .u32le(static_cast<std::uint32_t>(std::min<std::uint64_t>(cd_offset, u32_max)))
      .u16le(static_cast<std::uint16_t>(comment.size()))
      .text(comment);
  CVT_TRY(ctx.checkpoint(1.0));
  return out.take();
}

}  // namespace cvt::archive::zip
