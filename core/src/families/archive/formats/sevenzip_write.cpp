// Convertible core - archive family: 7z writer.
//
// Layout as 7-Zip writes it: the signature header, the packed streams, the
// packed header, and a small kEncodedHeader record pointing at it. Files with
// data become substreams of one solid folder (`solid`, the default) or of a
// folder each; directories and empty files are "empty streams". Names are
// UTF-16LE, times FILETIME, attributes carry the Unix mode in the high 16
// bits (0x8000 extension), symlinks store their target as content (S_IFLNK),
// exactly as 7-Zip on Unix does. Folders whose data does not shrink are
// stored with the Copy coder.
//
// Encryption (`password`) is 7zAES: AES-256-CBC with a key from 2^19 SHA-256
// rounds over the UTF-16LE password and no salt (7-Zip's own choice), so the
// key is derived once per archive. Each folder's 16-byte IV is the first half
// of SHA-256("7z-aes-iv", derived key, folder number, SHA-256 of the folder's
// content): deterministic, different for different content, and revealing
// nothing that is cheaper to attack than the key derivation itself. With
// `encrypt_headers` (default when a password is set) the header is encrypted
// too, hiding the file names.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <format>
#include <string>
#include <vector>

#include "families/archive/archive_common.hpp"
#include "families/archive/compression.hpp"
#include "families/archive/crypto.hpp"
#include "families/archive/formats/sevenzip_internal.hpp"
#include "util/crc.hpp"
#include "util/hash.hpp"
#include "util/text.hpp"

namespace cvt::archive::sevenzip {

namespace {

using util::ByteWriter;

constexpr unsigned write_cycles_power = 19;

struct MethodChoice {
  std::string name = "lzma2";
  std::uint64_t id = id_lzma2;
  compression::Method codec = compression::Method::Lzma2Raw;
};

MethodChoice method_choice(const std::string& name) {
  using compression::Method;
  if (name == "lzma") return {name, id_lzma, Method::LzmaRaw};
  if (name == "bzip2") return {name, id_bzip2, Method::Bzip2};
  if (name == "deflate") return {name, id_deflate, Method::Deflate};
  if (name == "copy") return {name, id_copy, Method::Store};
  if (name == "ppmd") return {name, id_ppmd, Method::Ppmd7};
  return {};
}

struct Crypto {
  bool enabled = false;
  std::array<std::uint8_t, 32> key{};
};

/// Compress one folder's content; returns the folder (coders, sizes) and its packed bytes.
Result<std::pair<Folder, Bytes>> pack_folder(ByteView content, const MethodChoice& method, int level,
                                             const Crypto& crypto, std::uint64_t folder_number, const Progress& progress) {
  Folder f;
  Coder main;
  Bytes packed;
  bool stored = method.id == id_copy || level <= 0;
  if (!stored) {
    compression::EncodeOptions e;
    e.level = level;
    e.progress = progress;
    CVT_TRY_ASSIGN(main.properties, compression::encoder_properties(method.codec, e, content.size()));
    CVT_TRY_ASSIGN(packed, compression::compress(method.codec, content, e));
    if (packed.size() >= content.size()) stored = true;
  }
  if (stored) {
    main = Coder{};
    main.id = id_copy;
    packed.assign(content.begin(), content.end());
  } else {
    main.id = method.id;
  }
  f.coders.push_back(std::move(main));
  f.unpack_sizes.push_back(content.size());
  if (crypto.enabled) {
    util::Sha256 iv_hash;
    iv_hash.update(as_bytes(std::string_view("7z-aes-iv")));
    iv_hash.update(as_bytes(std::span<const std::uint8_t>(crypto.key.data(), crypto.key.size())));
    ByteWriter number;
    number.u64le(folder_number);
    iv_hash.update(number.view());
    const util::Sha256::Digest content_digest = util::sha256(content);
    iv_hash.update(as_bytes(std::span<const std::uint8_t>(content_digest.data(), content_digest.size())));
    const util::Sha256::Digest digest = iv_hash.finish();
    AesParams params;
    params.cycles_power = write_cycles_power;
    std::copy_n(digest.begin(), params.iv.size(), params.iv.begin());
    Coder aes;
    aes.id = id_aes;
    aes.properties = aes_properties(params);
    f.coders.push_back(std::move(aes));
    f.unpack_sizes.push_back(packed.size());
    f.binds.push_back(BindPair{0, 1});  // the main coder reads what AES produces
    f.packed.push_back(1);
    packed = aes_encrypt(packed, crypto.key, params.iv);
  } else {
    f.packed.push_back(0);
  }
  f.crc = util::crc32(content);
  return std::pair{std::move(f), std::move(packed)};
}

void write_bits(ByteWriter& out, const std::vector<bool>& bits) {
  std::uint8_t current = 0;
  std::size_t i = 0;
  for (; i < bits.size(); ++i) {
    if (bits[i]) current = static_cast<std::uint8_t>(current | (0x80u >> (i % 8)));
    if (i % 8 == 7) {
      out.u8(current);
      current = 0;
    }
  }
  if (i % 8 != 0) out.u8(current);
}

void write_property(ByteWriter& out, std::uint8_t id, ByteView body) {
  out.u8(id);
  write_number(out, body.size());
  out.bytes(body);
}

/// PackInfo + UnpackInfo for `folders` whose packed streams start at `pack_pos`.
void write_pack_and_unpack(ByteWriter& out, std::uint64_t pack_pos, const std::vector<std::uint64_t>& pack_sizes,
                           const std::vector<Folder>& folders, bool folder_crcs) {
  out.u8(kPackInfo);
  write_number(out, pack_pos);
  write_number(out, pack_sizes.size());
  out.u8(kSize);
  for (const std::uint64_t s : pack_sizes) write_number(out, s);
  out.u8(kEnd);
  out.u8(kUnpackInfo);
  out.u8(kFolder);
  write_number(out, folders.size());
  out.u8(0);  // not external
  for (const Folder& f : folders) write_folder(out, f);
  out.u8(kCodersUnpackSize);
  for (const Folder& f : folders) {
    for (const std::uint64_t s : f.unpack_sizes) write_number(out, s);
  }
  if (folder_crcs) {
    out.u8(kCrc);
    out.u8(1);  // all defined
    for (const Folder& f : folders) out.u32le(f.crc.value_or(0));
  }
  out.u8(kEnd);
}

std::uint64_t to_filetime(std::int64_t unix_time) {
  const std::int64_t seconds = unix_time + filetime_epoch_offset;
  return seconds <= 0 ? 0 : static_cast<std::uint64_t>(seconds) * 10000000u;
}

}  // namespace

Result<Bytes> write_7z(const ir::Archive& archive, WriteContext& ctx) {
  const WritePolicy policy = resolve_write_policy(ctx);
  CVT_TRY_ASSIGN(const std::vector<WriteEntry> entries,
                 prepare_entries(archive, policy, ctx, {.symlinks = true, .directories = true}));
  MethodChoice method =
      method_choice(ctx.options.get_enum("method", "lzma2", {"lzma2", "lzma", "bzip2", "deflate", "copy", "ppmd"}));
  if (method.id != id_copy) {
    bool usable = compression::can_encode(method.codec);
    if (usable) {
      // PPMd needs its order/memory properties from the encoder.
      compression::EncodeOptions probe;
      probe.level = policy.level;
      const auto props = compression::encoder_properties(method.codec, probe, 1);
      if (method.id == id_ppmd && (!props || props->size() != 5)) usable = false;
    }
    if (!usable) {
      ctx.warn(std::format("7z method '{}' is not available in this build; using LZMA2 instead", method.name));
      method = MethodChoice{};
    }
  }
  const bool solid = ctx.options.get_bool("solid", true);
  const std::string& password = policy.password;
  Crypto crypto;
  if (!password.empty()) {
    crypto.enabled = true;
    CVT_TRY_ASSIGN(crypto.key, derive_key(password, {}, write_cycles_power, ctx.progress.sub(0.0, 0.05)));
  }
  const bool encrypt_header = crypto.enabled && ctx.options.get_bool("encrypt_headers", true);

  // Which entries carry data, and the folders they go into.
  std::vector<ByteView> contents(entries.size());
  std::vector<std::size_t> with_data;
  std::uint64_t total = 0;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const WriteEntry& e = entries[i];
    if (e.type == EntryType::File) contents[i] = e.data();
    if (e.type == EntryType::Symlink) contents[i] = as_bytes(e.link_target());
    if (!contents[i].empty()) {
      with_data.push_back(i);
      total += contents[i].size();
    }
  }
  std::vector<std::vector<std::size_t>> groups;
  if (solid) {
    if (!with_data.empty()) groups.push_back(with_data);
  } else {
    for (const std::size_t i : with_data) groups.push_back({i});
  }

  ByteWriter out(static_cast<std::size_t>(std::min<std::uint64_t>(total / 2 + 4096, 64u << 20)));
  out.zeros(signature_header_size);
  std::vector<Folder> folders;
  std::vector<std::uint64_t> pack_sizes;
  std::vector<std::size_t> substreams;
  std::uint64_t done = 0;
  const double span = static_cast<double>(std::max<std::uint64_t>(total, 1));
  for (std::size_t g = 0; g < groups.size(); ++g) {
    CVT_TRY(ctx.checkpoint(0.05 + 0.9 * static_cast<double>(done) / span));
    Bytes joined;
    std::uint64_t size = 0;
    for (const std::size_t i : groups[g]) size += contents[i].size();
    ByteView content;
    if (groups[g].size() == 1) {
      content = contents[groups[g][0]];
    } else {
      joined.reserve(static_cast<std::size_t>(size));
      for (const std::size_t i : groups[g]) append(joined, contents[i]);
      content = joined;
    }
    const Progress progress = ctx.progress.sub(0.05 + 0.9 * static_cast<double>(done) / span,
                                               0.05 + 0.9 * static_cast<double>(done + size) / span);
    const int level = groups[g].size() == 1 && entries[groups[g][0]].type == EntryType::Symlink ? 0 : policy.level;
    CVT_TRY_ASSIGN(auto packed, pack_folder(content, method, level, crypto, g, progress));
    pack_sizes.push_back(packed.second.size());
    out.bytes(packed.second);
    folders.push_back(std::move(packed.first));
    substreams.push_back(groups[g].size());
    done += size;
  }

  // ---- the header
  ByteWriter h;
  if (!entries.empty()) {
    h.u8(kHeader);
    if (!folders.empty()) {
      h.u8(kMainStreamsInfo);
      write_pack_and_unpack(h, 0, pack_sizes, folders, false);
      h.u8(kSubStreamsInfo);
      if (std::ranges::any_of(substreams, [](std::size_t n) { return n != 1; })) {
        h.u8(kNumUnpackStream);
        for (const std::size_t n : substreams) write_number(h, n);
        h.u8(kSize);
        for (const auto& group : groups) {
          for (std::size_t k = 0; k + 1 < group.size(); ++k) write_number(h, contents[group[k]].size());
        }
      }
      h.u8(kCrc);
      h.u8(1);
      for (const auto& group : groups) {
        for (const std::size_t i : group) h.u32le(util::crc32(contents[i]));
      }
      h.u8(kEnd);
      h.u8(kEnd);
    }
    h.u8(kFilesInfo);
    write_number(h, entries.size());
    std::vector<bool> empty_stream(entries.size(), false);
    std::vector<bool> empty_file;
    for (std::size_t i = 0; i < entries.size(); ++i) {
      if (!contents[i].empty()) continue;
      empty_stream[i] = true;
      empty_file.push_back(entries[i].type != EntryType::Directory);
    }
    if (!empty_file.empty()) {
      ByteWriter bits;
      write_bits(bits, empty_stream);
      write_property(h, kEmptyStream, bits.view());
      if (std::ranges::any_of(empty_file, [](bool b) { return b; })) {
        ByteWriter files;
        write_bits(files, empty_file);
        write_property(h, kEmptyFile, files.view());
      }
    }
    ByteWriter names;
    names.u8(0);  // not external
    for (const WriteEntry& e : entries) {
      for (const char16_t c : util::utf8_to_utf16(e.path)) names.u16le(static_cast<std::uint16_t>(c));
      names.u16le(0);
    }
    write_property(h, kName, names.view());
    ByteWriter times;
    times.u8(1).u8(0);  // all defined, not external
    for (const WriteEntry& e : entries) times.u64le(to_filetime(e.mtime));
    write_property(h, kMTime, times.view());
    ByteWriter attributes;
    attributes.u8(1).u8(0);
    for (const WriteEntry& e : entries) {
      std::uint32_t type = 0100000u;
      std::uint32_t win = attr_archive;
      if (e.type == EntryType::Directory) {
        type = 0040000u;
        win = attr_directory;
      } else if (e.type == EntryType::Symlink) {
        type = 0120000u;
      }
      attributes.u32le(win | attr_unix_extension | ((type | (e.mode & 07777u)) << 16));
    }
    write_property(h, kWinAttributes, attributes.view());
    h.u8(kEnd);
    h.u8(kEnd);
  }

  std::uint64_t next_offset = 0;
  Bytes next;
  if (!entries.empty()) {
    // The header itself, LZMA-compressed (and encrypted), described by a kEncodedHeader record.
    const ByteView plain = h.view();
    compression::EncodeOptions e;
    e.level = 7;
    Coder lzma;
    lzma.id = id_lzma;
    CVT_TRY_ASSIGN(lzma.properties, compression::encoder_properties(compression::Method::LzmaRaw, e, plain.size()));
    CVT_TRY_ASSIGN(Bytes packed, compression::compress(compression::Method::LzmaRaw, plain, e));
    Folder f;
    f.coders.push_back(std::move(lzma));
    f.unpack_sizes.push_back(plain.size());
    if (encrypt_header) {
      util::Sha256 iv_hash;
      iv_hash.update(as_bytes(std::string_view("7z-aes-header-iv")));
      iv_hash.update(as_bytes(std::span<const std::uint8_t>(crypto.key.data(), crypto.key.size())));
      iv_hash.update(plain);
      const util::Sha256::Digest digest = iv_hash.finish();
      AesParams params;
      params.cycles_power = write_cycles_power;
      std::copy_n(digest.begin(), params.iv.size(), params.iv.begin());
      Coder aes;
      aes.id = id_aes;
      aes.properties = aes_properties(params);
      f.coders.push_back(std::move(aes));
      f.unpack_sizes.push_back(packed.size());
      f.binds.push_back(BindPair{0, 1});
      f.packed.push_back(1);
      packed = aes_encrypt(packed, crypto.key, params.iv);
    } else {
      f.packed.push_back(0);
    }
    f.crc = util::crc32(plain);
    const std::uint64_t pack_pos = out.size() - signature_header_size;
    out.bytes(packed);
    ByteWriter encoded;
    encoded.u8(kEncodedHeader);
    write_pack_and_unpack(encoded, pack_pos, {packed.size()}, {f}, true);
    encoded.u8(kEnd);
    next_offset = out.size() - signature_header_size;
    next = encoded.take();
    out.bytes(next);
  }

  // ---- the signature header
  Bytes result = out.take();
  ByteWriter start;
  start.u64le(next_offset).u64le(next.size()).u32le(util::crc32(next));
  ByteWriter sig;
  sig.bytes(std::span<const std::uint8_t>(signature.data(), signature.size())).u8(0).u8(4);
  sig.u32le(util::crc32(start.view())).bytes(start.view());
  std::copy(sig.data().begin(), sig.data().end(), result.begin());
  CVT_TRY(ctx.checkpoint(1.0));
  return result;
}

}  // namespace cvt::archive::sevenzip
