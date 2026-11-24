// Convertible core - archive family: RAR archives (read only).
//
// Two container generations share the extension and the reader:
//
//   RAR 5 ('Rar!' 1A 07 01 00, RAR 5.0 and later): headers of variable-length
//   integers, each protected by a CRC-32; main, file, service (comment
//   "CMT", quick open, recovery...), archive encryption and end headers;
//   extra records for encryption, BLAKE2sp hashes, high-precision times,
//   links and owners. Data is stored or compressed with the RAR 5 LZ
//   algorithm (rar_lz5.cpp), optionally AES-256 encrypted per file and with
//   encrypted headers (-hp).
//
//   RAR 2.9-4.x ('Rar!' 1A 07 00): fixed little-endian block headers with a
//   16-bit CRC; file headers with 64-bit sizes, Unicode names, salt and
//   extended times; NEWSUB blocks for comments. Data is stored or compressed
//   with the RAR 3 algorithm (rar_lz3.cpp: LZ, PPMd and the standard VM
//   filters), optionally AES-128 encrypted, headers included.
//
// Solid archives are decoded as one stream. Multi-volume archives are read
// as far as the first volume goes; a file split across volumes fails with a
// precise message. Every byte produced is charged to the EntrySink, and the
// dictionary is only allocated once its size has been checked against the
// remaining decompression budget.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "util/bytes.hpp"
#include "engine/registry.hpp"
#include "families/archive/archive_common.hpp"
#include "families/archive/crypto.hpp"
#include "families/archive/formats/formats.hpp"
#include "families/archive/formats/rar_internal.hpp"
#include "util/crc.hpp"
#include "util/hash.hpp"
#include "util/text.hpp"
#include "util/zip.hpp"

namespace cvt::archive {

namespace {

using rar::FileOutput;

constexpr std::string_view sig_rar5{"Rar!\x1A\x07\x01\x00", 8};
constexpr std::string_view sig_rar4{"Rar!\x1A\x07\x00", 7};
// Room for the largest filter block (RAR 5: 4 MiB, RAR 3: the VM's 256 KiB) plus the longest match.
constexpr std::uint64_t filter_room5 = 0x400000 + 0x10000;
constexpr std::uint64_t filter_room3 = 0x40000 + 0x10000;
constexpr std::uint64_t unknown_size = std::numeric_limits<std::uint64_t>::max();

bool starts_with(ByteView data, std::string_view magic, std::size_t at = 0) {
  return data.size() >= at + magic.size() && as_chars(data.subspan(at, magic.size())) == magic;
}

std::uint64_t saturating_add(std::uint64_t a, std::uint64_t b) {
  return a > unknown_size - b ? unknown_size : a + b;
}

/// One file of either generation, as the headers describe it.
struct Item {
  std::string name;
  std::string raw_name;  // for hard link / file copy targets
  bool directory = false;
  bool symlink = false;
  bool windows_link = false;
  int redirect = 0;  // RAR 5: 4 hard link, 5 file copy
  std::string link_target;
  std::uint64_t unpacked = 0;
  bool size_known = true;
  std::uint64_t data_offset = 0;
  std::uint64_t packed = 0;
  std::uint32_t mode = 0;
  std::int64_t mtime = 0;
  bool has_crc = false;
  std::uint32_t crc = 0;
  bool has_blake = false;
  std::array<std::uint8_t, 32> blake{};
  int method = 0;  // 0 = stored; RAR 5: 1..5; RAR 4: 1..5 (from 0x31..0x35)
  std::uint64_t dictionary = 0;
  bool solid = false;
  int version = 0;  // RAR 5: algorithm version (0, 1); RAR 4: UNP_VER
  bool split_before = false;
  bool split_after = false;
  // encryption
  bool encrypted = false;
  Bytes salt;
  std::array<std::uint8_t, 16> iv{};
  unsigned kdf_count = 0;
  bool has_check = false;
  std::array<std::uint8_t, 8> check{};
  bool use_mac = false;
};

[[nodiscard]] bool needs_decoder(const Item& it) {
  return !it.directory && !it.symlink && !it.windows_link && it.redirect == 0 && it.method != 0;
}

Result<void> split_file(const Item& it) {
  return fail(Status::Decode,
              "'{}' continues in another volume of this multi-volume RAR archive, which is not supported; "
              "extract the whole set with the RAR tools instead",
              it.name);
}

Result<void> later_volume() {
  return fail(Status::Decode,
              "this is not the first volume of a multi-volume RAR archive; open the first volume (.part1.rar or .rar)");
}

/// Window size for decoding `items`: the largest dictionary, but no larger
/// than the output (plus room for a filter block and the longest match).
Result<std::uint64_t> window_for(const std::vector<Item>& items, const EntrySink& sink, std::uint64_t filter_room) {
  std::uint64_t dictionary = 0;
  std::uint64_t total = 0;
  for (const Item& it : items) {
    if (!needs_decoder(it)) continue;
    dictionary = std::max(dictionary, it.dictionary);
    total = saturating_add(total, it.size_known ? it.unpacked : unknown_size);
  }
  const std::uint64_t needed = std::min(dictionary, total);
  if (needed > sink.budget()) {
    return fail(Status::Limit, "the RAR dictionary of {} bytes needs more memory than the remaining limit of {} bytes",
                dictionary, sink.budget());
  }
  return std::min(std::max(dictionary, filter_room), saturating_add(total, 0x10000));
}

// --------------------------------------------------------------------------- shared extraction

struct Extractor {
  ReadContext& ctx;
  EntrySink& sink;
  std::string password;
  std::set<std::string, std::less<>> referenced;                // targets of hard links and file copies
  std::map<std::string, std::size_t, std::less<>> by_raw_name;  // index into contents
  std::vector<Bytes> contents;  // content of the referenced files

  static std::string link_key(std::string target) {
    std::replace(target.begin(), target.end(), '\\', '/');
    return target;
  }
  void note_references(const std::vector<Item>& items) {
    for (const Item& it : items) {
      if (it.redirect != 0) referenced.insert(link_key(it.link_target));
    }
  }
  Result<void> add_file(const Item& it, Bytes content) {
    ir::ArchiveEntry e;
    e.path = it.name;
    e.mtime = it.mtime;
    e.unix_mode = it.mode;
    if (referenced.contains(it.raw_name)) {
      by_raw_name[it.raw_name] = contents.size();
      contents.push_back(content);
    }
    e.bytes = std::move(content);
    return sink.add(std::move(e));
  }
  Result<void> add_simple(const Item& it) {
    ir::ArchiveEntry e;
    e.path = it.name;
    e.mtime = it.mtime;
    e.unix_mode = it.mode;
    if (it.directory) {
      e.is_directory = true;
    } else if (it.symlink) {
      e.symlink_target = it.link_target;
      if (e.symlink_target.empty()) return {};
    }
    return sink.add(std::move(e));
  }
  Result<void> add_copy(const Item& it) {
    const auto found = by_raw_name.find(link_key(it.link_target));
    if (found == by_raw_name.end()) {
      ctx.warn(std::format("RAR {} '{}' refers to '{}', which is not in the archive; skipped",
                           it.redirect == 4 ? "hard link" : "file copy", it.name, it.link_target));
      return {};
    }
    const Bytes& source = contents[found->second];
    if (source.size() > sink.budget()) return fail(Status::Limit, "'{}' would exceed the decompression limit", it.name);
    CVT_TRY(sink.charge(source.size()));
    return add_file(it, source);
  }
};

void verify_hint(std::string& message, const Item& it) {
  if (it.encrypted) message = std::format("wrong password for '{}' (or damaged data)", it.name);
}

// --------------------------------------------------------------------------- RAR 5

class VintReader {
 public:
  explicit VintReader(ByteView data) : data_(data) {}
  std::uint64_t vint() {
    std::uint64_t v = 0;
    for (int shift = 0; shift < 70; shift += 7) {
      if (pos_ >= data_.size()) {
        ok_ = false;
        return 0;
      }
      const auto b = std::to_integer<std::uint64_t>(data_[pos_++]);
      if (shift == 63 && (b & 0x7Eu) != 0) ok_ = false;
      v |= (b & 0x7Fu) << shift;
      if ((b & 0x80u) == 0) return v;
    }
    ok_ = false;
    return 0;
  }
  std::uint64_t le(int bytes) {
    if (data_.size() - pos_ < static_cast<std::size_t>(bytes) || pos_ > data_.size()) {
      ok_ = false;
      pos_ = data_.size();
      return 0;
    }
    std::uint64_t v = 0;
    for (int i = bytes; i-- > 0;) v = (v << 8) | std::to_integer<std::uint64_t>(data_[pos_ + static_cast<std::size_t>(i)]);
    pos_ += static_cast<std::size_t>(bytes);
    return v;
  }
  ByteView bytes(std::uint64_t count) {
    if (count > data_.size() - pos_) {
      ok_ = false;
      pos_ = data_.size();
      return {};
    }
    const ByteView v = data_.subspan(pos_, static_cast<std::size_t>(count));
    pos_ += static_cast<std::size_t>(count);
    return v;
  }
  [[nodiscard]] bool ok() const { return ok_; }
  [[nodiscard]] std::size_t position() const { return pos_; }
  [[nodiscard]] std::size_t remaining() const { return data_.size() - pos_; }

 private:
  ByteView data_;
  std::size_t pos_ = 0;
  bool ok_ = true;
};

struct KeyCache5 {
  std::string password;
  std::vector<std::pair<Bytes, rar::Rar5Keys>> entries;
  const rar::Rar5Keys& get(ByteView salt, unsigned count) {
    Bytes key(salt.begin(), salt.end());
    key.push_back(static_cast<std::byte>(count));
    for (const auto& [k, v] : entries) {
      if (k == key) return v;
    }
    entries.emplace_back(std::move(key), rar::rar5_keys(password, salt, count));
    return entries.back().second;
  }
};

bool check_value_valid(ByteView check12) {
  util::Sha256 sha;
  sha.update(check12.first(8));
  const auto digest = sha.finish();
  return ::cvt::util::compare_bytes(digest.data(), check12.data() + 8, 4) == 0;
}

struct Rar5Header {
  std::uint64_t type = 0;
  std::uint64_t flags = 0;
  Bytes storage;      // decrypted copy when headers are encrypted
  ByteView body{};    // type-specific fields followed by the extra area
  ByteView extra{};
  std::uint64_t data_offset = 0;
  std::uint64_t data_size = 0;
  std::uint64_t next = 0;  // offset of the next header
};

class Rar5Parser {
 public:
  Rar5Parser(ByteView data, std::uint64_t start, KeyCache5& keys) : data_(data), pos_(start), keys_(keys) {}

  Result<std::optional<Rar5Header>> next() {
    if (pos_ >= data_.size()) return std::optional<Rar5Header>{};
    Rar5Header h;
    ByteView raw;
    std::uint64_t after = 0;
    if (encrypted_) {
      if (data_.size() - pos_ < 32) return fail(Status::Decode, "RAR 5 encrypted header is truncated");
      Aes::Block iv{};
      ::cvt::util::copy_bytes(iv.data(), data_.data() + pos_, 16);
      Bytes first(data_.begin() + static_cast<std::ptrdiff_t>(pos_ + 16), data_.begin() + static_cast<std::ptrdiff_t>(pos_ + 32));
      Aes::Block chain = iv;
      CVT_TRY(aes_cbc_decrypt(aes_, chain, std::span<std::byte>(first)));
      VintReader probe(ByteView(first).subspan(4));
      const std::uint64_t size = probe.vint();
      if (!probe.ok() || size > 2 * 1024 * 1024) return fail(Status::Decode, "wrong password (the RAR 5 headers do not decrypt)");
      const std::uint64_t total = (4 + probe.position() + size + 15) / 16 * 16;
      if (total > data_.size() - pos_ - 16) return fail(Status::Decode, "wrong password (the RAR 5 headers do not decrypt) or a truncated archive");
      h.storage.assign(data_.begin() + static_cast<std::ptrdiff_t>(pos_ + 16),
                       data_.begin() + static_cast<std::ptrdiff_t>(pos_ + 16 + total));
      chain = iv;
      CVT_TRY(aes_cbc_decrypt(aes_, chain, std::span<std::byte>(h.storage)));
      raw = ByteView(h.storage).first(static_cast<std::size_t>(4 + probe.position() + size));
      after = pos_ + 16 + total;
    } else {
      VintReader r(data_.subspan(static_cast<std::size_t>(pos_)));
      (void)r.le(4);
      const std::uint64_t size = r.vint();
      if (!r.ok() || size > r.remaining()) return fail(Status::Decode, "RAR 5 header is truncated");
      if (size > 2 * 1024 * 1024) return fail(Status::Decode, "RAR 5 header is too large (damaged archive)");
      raw = data_.subspan(static_cast<std::size_t>(pos_), static_cast<std::size_t>(r.position() + size));
      after = pos_ + raw.size();
    }
    VintReader r(raw);
    const auto crc = static_cast<std::uint32_t>(r.le(4));
    if (util::crc32(raw.subspan(4)) != crc) {
      if (encrypted_) return fail(Status::Decode, "wrong password (the RAR 5 headers do not decrypt)");
      return fail(Status::Decode, "RAR 5 header CRC mismatch (damaged archive)");
    }
    const std::uint64_t size = r.vint();
    const std::size_t body_start = r.position();
    h.type = r.vint();
    h.flags = r.vint();
    std::uint64_t extra_size = 0;
    if ((h.flags & 0x1u) != 0) extra_size = r.vint();
    if ((h.flags & 0x2u) != 0) h.data_size = r.vint();
    if (!r.ok()) return fail(Status::Decode, "RAR 5 header is damaged");
    const std::size_t end = body_start + static_cast<std::size_t>(size);
    if (extra_size > end - r.position()) return fail(Status::Decode, "RAR 5 header extra area is out of range");
    h.body = raw.subspan(r.position(), end - r.position());
    h.extra = h.body.last(static_cast<std::size_t>(extra_size));
    h.data_offset = after;
    if (h.data_size > data_.size() - std::min<std::uint64_t>(after, data_.size())) {
      return fail(Status::Decode, "RAR 5 archive is truncated (data runs past the end)");
    }
    h.next = after + h.data_size;
    pos_ = h.next;
    // `body` and `extra` may point into `storage`: moving a vector keeps its buffer.
    return std::optional<Rar5Header>(std::move(h));
  }

  Result<void> enable_encryption(const Rar5Header& h, std::string_view password) {
    VintReader r(h.body);
    const std::uint64_t version = r.vint();
    const std::uint64_t flags = r.vint();
    const auto count = static_cast<unsigned>(r.le(1));
    const ByteView salt = r.bytes(16);
    ByteView check;
    if ((flags & 1u) != 0) check = r.bytes(12);
    if (!r.ok()) return fail(Status::Decode, "RAR 5 archive encryption header is damaged");
    if (version != 0) return fail(Status::Decode, "RAR 5 encryption version {} is not supported", version);
    if (count > 24) return fail(Status::Decode, "RAR 5 key derivation count 2^{} is out of range", count);
    if (password.empty()) return fail(Status::Decode, "password required: the RAR archive has encrypted headers");
    const rar::Rar5Keys& keys = keys_.get(salt, count);
    if (!check.empty() && check_value_valid(check) && ::cvt::util::compare_bytes(check.data(), keys.check.data(), 8) != 0) {
      return fail(Status::Decode, "wrong password for the RAR archive");
    }
    if (!aes_.set_key(std::span<const std::uint8_t>(keys.key.data(), keys.key.size()))) {
      return fail(Status::Internal, "AES key setup failed");
    }
    encrypted_ = true;
    return {};
  }

 private:
  ByteView data_;
  std::uint64_t pos_;
  KeyCache5& keys_;
  bool encrypted_ = false;
  Aes aes_;
};

std::int64_t filetime_to_unix(std::uint64_t ft) {
  return static_cast<std::int64_t>(ft / 10000000u) - 11644473600;
}

Result<Item> parse_rar5_file(const Rar5Header& h) {
  VintReader r(h.body.first(h.body.size() - h.extra.size()));
  Item it;
  const std::uint64_t flags = r.vint();
  it.unpacked = r.vint();
  const std::uint64_t attributes = r.vint();
  if ((flags & 0x2u) != 0) it.mtime = static_cast<std::int64_t>(r.le(4));
  if ((flags & 0x4u) != 0) {
    it.has_crc = true;
    it.crc = static_cast<std::uint32_t>(r.le(4));
  }
  const std::uint64_t info = r.vint();
  const std::uint64_t host = r.vint();
  const std::uint64_t name_size = r.vint();
  const ByteView name = r.bytes(name_size);
  if (!r.ok()) return fail(Status::Decode, "RAR 5 file header is damaged");
  it.raw_name = util::sanitize_utf8(as_chars(name));
  it.name = it.raw_name;
  it.directory = (flags & 0x1u) != 0;
  it.size_known = (flags & 0x8u) == 0;
  it.version = static_cast<int>(info & 0x3Fu);
  it.solid = (info & 0x40u) != 0;
  it.method = static_cast<int>((info >> 7) & 7u);
  const unsigned dict_bits = static_cast<unsigned>((info >> 10) & 0x1Fu);
  const unsigned fraction = static_cast<unsigned>((info >> 15) & 0x1Fu);
  if (it.version == 1 && (info & 0x100000u) != 0) it.version = 0;  // RAR 7 archive, RAR 5 algorithm
  if (dict_bits > 23) return fail(Status::Decode, "RAR 5 file header of '{}' has an invalid dictionary size", it.name);
  it.dictionary = (std::uint64_t{128 * 1024} << dict_bits);
  if (it.version >= 1) it.dictionary += (it.dictionary / 32) * fraction;
  it.split_before = (h.flags & 0x8u) != 0;
  it.split_after = (h.flags & 0x10u) != 0;
  it.data_offset = h.data_offset;
  it.packed = h.data_size;
  if (host == 1) {  // Unix: st_mode
    const auto type = static_cast<std::uint32_t>(attributes & 0170000u);
    it.mode = static_cast<std::uint32_t>(attributes & 07777u);
    if (type == 0040000u) it.directory = true;
    if (type == 0120000u) it.symlink = true;
  } else if (host == 0) {  // Windows attributes
    if ((attributes & 0x10u) != 0) it.directory = true;
  }

  VintReader x(h.extra);
  while (x.remaining() > 0) {
    const std::uint64_t size = x.vint();
    if (!x.ok() || size == 0 || size > x.remaining()) return fail(Status::Decode, "RAR 5 extra area of '{}' is damaged", it.name);
    VintReader rec(x.bytes(size));
    const std::uint64_t type = rec.vint();
    switch (type) {
      case 1: {  // encryption
        const std::uint64_t version = rec.vint();
        const std::uint64_t eflags = rec.vint();
        it.kdf_count = static_cast<unsigned>(rec.le(1));
        const ByteView salt = rec.bytes(16);
        const ByteView iv = rec.bytes(16);
        if (!rec.ok()) return fail(Status::Decode, "RAR 5 encryption record of '{}' is damaged", it.name);
        if (version != 0) return fail(Status::Decode, "'{}' uses RAR 5 encryption version {}, which is not supported", it.name, version);
        it.encrypted = true;
        it.salt.assign(salt.begin(), salt.end());
        ::cvt::util::copy_bytes(it.iv.data(), iv.data(), 16);
        if ((eflags & 1u) != 0) {
          const ByteView check = rec.bytes(12);
          if (rec.ok() && check_value_valid(check)) {
            it.has_check = true;
            ::cvt::util::copy_bytes(it.check.data(), check.data(), 8);
          }
        }
        it.use_mac = (eflags & 2u) != 0;
        break;
      }
      case 2: {  // hash
        const std::uint64_t hash_type = rec.vint();
        const ByteView digest = rec.bytes(32);
        if (rec.ok() && hash_type == 0) {
          it.has_blake = true;
          ::cvt::util::copy_bytes(it.blake.data(), digest.data(), 32);
        }
        break;
      }
      case 3: {  // times
        const std::uint64_t tflags = rec.vint();
        if ((tflags & 0x2u) != 0) {
          if ((tflags & 0x1u) != 0) {
            it.mtime = static_cast<std::int64_t>(rec.le(4));
          } else {
            const std::uint64_t ft = rec.le(8);
            if (rec.ok()) it.mtime = filetime_to_unix(ft);
          }
        }
        break;
      }
      case 5: {  // redirection
        const std::uint64_t kind = rec.vint();
        (void)rec.vint();  // flags (directory link)
        const std::uint64_t length = rec.vint();
        const ByteView target = rec.bytes(length);
        if (!rec.ok()) return fail(Status::Decode, "RAR 5 link record of '{}' is damaged", it.name);
        it.link_target = util::sanitize_utf8(as_chars(target));
        if (kind == 1) {
          it.symlink = true;
        } else if (kind == 2 || kind == 3) {
          it.windows_link = true;
        } else if (kind == 4 || kind == 5) {
          it.redirect = static_cast<int>(kind);
        }
        break;
      }
      default:
        break;  // version, owner, service data
    }
  }
  if (it.directory) it.symlink = false;
  return it;
}

struct Rar5Decoder {
  rar::Unpack5 unpack;
  bool ready = false;
};

Result<Bytes> unpack_rar5(Extractor& ex, const Item& it, ByteView packed, Rar5Decoder& decoder, KeyCache5& keys,
                          const Progress& progress) {
  Bytes decrypted;
  const rar::Rar5Keys* k = nullptr;
  if (it.encrypted) {
    if (ex.password.empty()) return fail(Status::Decode, "password required: '{}' is encrypted", it.name);
    if (it.kdf_count > 24) return fail(Status::Decode, "RAR 5 key derivation count of '{}' is out of range", it.name);
    k = &keys.get(it.salt, it.kdf_count);
    if (it.has_check && ::cvt::util::compare_bytes(it.check.data(), k->check.data(), 8) != 0) {
      return fail(Status::Decode, "wrong password for '{}'", it.name);
    }
    if (packed.size() % 16 != 0) return fail(Status::Decode, "encrypted RAR data of '{}' is damaged", it.name);
    Aes aes;
    if (!aes.set_key(std::span<const std::uint8_t>(k->key.data(), k->key.size()))) return fail(Status::Internal, "AES key setup failed");
    decrypted.assign(packed.begin(), packed.end());
    Aes::Block iv = it.iv;
    CVT_TRY(aes_cbc_decrypt(aes, iv, std::span<std::byte>(decrypted)));
    packed = decrypted;
  }
  if (it.size_known && it.unpacked > ex.sink.budget()) {
    return fail(Status::Limit, "'{}' would decompress to {} bytes, more than the remaining limit", it.name, it.unpacked);
  }
  Bytes content;
  if (it.method == 0 || (it.size_known && it.unpacked == 0 && packed.empty())) {
    if (!it.size_known || packed.size() < it.unpacked) return fail(Status::Decode, "stored RAR data of '{}' is truncated", it.name);
    content.assign(packed.begin(), packed.begin() + static_cast<std::ptrdiff_t>(it.unpacked));
  } else {
    if (it.version > 1) {
      return fail(Status::Decode, "'{}' uses RAR compression algorithm version {}, which is not supported", it.name, it.version);
    }
    if (!decoder.ready) return fail(Status::Internal, "RAR decoder is not initialised");
    FileOutput out;
    out.bytes = &content;
    out.size = it.unpacked;
    out.size_known = it.size_known;
    out.budget = ex.sink.budget();
    out.progress = progress;
    auto r = decoder.unpack.decode(packed, it.solid, it.version == 1, out);
    if (!r) {
      std::string message = r.error().message;
      if (r.error().code == Status::Decode) verify_hint(message, it);
      return fail(r.error().code, "{}", message);
    }
  }
  // Checksums (converted through HMAC-SHA256 when the archive hides them).
  if (it.has_crc) {
    std::uint32_t crc = util::crc32(content);
    if (k != nullptr && it.use_mac) {
      std::uint8_t raw[4] = {static_cast<std::uint8_t>(crc), static_cast<std::uint8_t>(crc >> 8),
                             static_cast<std::uint8_t>(crc >> 16), static_cast<std::uint8_t>(crc >> 24)};
      const auto digest = hmac_sha256(ByteView(reinterpret_cast<const std::byte*>(k->hash_key.data()), 32),
                                      ByteView(reinterpret_cast<const std::byte*>(raw), 4));
      crc = 0;
      for (std::size_t i = 0; i < digest.size(); ++i) crc ^= std::uint32_t{digest[i]} << (8 * (i & 3));
    }
    if (crc != it.crc) {
      if (it.encrypted) return fail(Status::Decode, "wrong password for '{}' (CRC mismatch)", it.name);
      return fail(Status::Decode, "CRC mismatch in '{}'", it.name);
    }
  }
  if (it.has_blake) {
    rar::Blake2sp hash;
    hash.update(content);
    auto digest = hash.finish();
    if (k != nullptr && it.use_mac) {
      const auto mac = hmac_sha256(ByteView(reinterpret_cast<const std::byte*>(k->hash_key.data()), 32),
                                   ByteView(reinterpret_cast<const std::byte*>(digest.data()), 32));
      std::copy(mac.begin(), mac.end(), digest.begin());
    }
    if (digest != it.blake) {
      if (it.encrypted) return fail(Status::Decode, "wrong password for '{}' (BLAKE2 mismatch)", it.name);
      return fail(Status::Decode, "BLAKE2sp checksum mismatch in '{}'", it.name);
    }
  }
  return content;
}

Result<ir::Archive> read_rar5(ReadContext& ctx, ByteView data, std::uint64_t start) {
  EntrySink sink(ctx, data.size());
  KeyCache5 keys;
  keys.password = ctx.options.get_string("password", "");
  Rar5Parser parser(data, start, keys);
  std::vector<Item> items;
  std::vector<Item> comments;
  bool ended = false;
  bool volume = false;
  while (!ended) {
    CVT_TRY_ASSIGN(std::optional<Rar5Header> h, parser.next());
    if (!h) break;
    switch (h->type) {
      case 1: {  // main archive header
        VintReader r(h->body);
        const std::uint64_t flags = r.vint();
        if (!r.ok()) return fail(Status::Decode, "RAR 5 main header is damaged");
        volume = (flags & 0x1u) != 0;
        if ((flags & 0x2u) != 0) {
          const std::uint64_t number = r.vint();
          if (number > 0) CVT_TRY(later_volume());
        }
        break;
      }
      case 4:
        CVT_TRY(parser.enable_encryption(*h, keys.password));
        break;
      case 2:
      case 3: {
        CVT_TRY_ASSIGN(Item it, parse_rar5_file(*h));
        if (h->type == 2) {
          CVT_TRY(ctx.limits.check_archive_entries(items.size() + 1));
          items.push_back(std::move(it));
        } else if (it.name == "CMT") {
          comments.push_back(std::move(it));
        }
        break;
      }
      case 5:
        ended = true;
        break;
      default:
        if ((h->flags & 0x4u) == 0) ctx.warn(std::format("unknown RAR 5 header type {} skipped", h->type));
        break;
    }
  }
  if (!ended && !volume) ctx.warn("the RAR archive has no end header (it may be truncated)");

  Extractor ex{ctx, sink, keys.password, {}, {}, {}};
  ex.note_references(items);
  Rar5Decoder decoder;
  std::uint64_t total = 0;
  for (const Item& it : items) total = saturating_add(total, it.packed);
  std::uint64_t done = 0;
  for (const Item& it : items) {
    const double span = static_cast<double>(std::max<std::uint64_t>(total, 1));
    CVT_TRY(ctx.checkpoint(static_cast<double>(done) / span));
    if (it.split_before || it.split_after) CVT_TRY(split_file(it));
    if (it.windows_link) {
      ctx.warn(std::format("'{}' is a Windows symbolic link or junction, which is not extracted", it.name));
      continue;
    }
    if (it.directory || it.symlink) {
      CVT_TRY(ex.add_simple(it));
      continue;
    }
    if (it.redirect != 0) {
      CVT_TRY(ex.add_copy(it));
      continue;
    }
    if (needs_decoder(it) && !decoder.ready) {
      CVT_TRY_ASSIGN(const std::uint64_t window, window_for(items, sink, filter_room5));
      decoder.unpack.init(window);
      decoder.ready = true;
    }
    const ByteView packed = data.subspan(static_cast<std::size_t>(it.data_offset), static_cast<std::size_t>(it.packed));
    CVT_TRY_ASSIGN(Bytes content,
                   unpack_rar5(ex, it, packed, decoder, keys,
                               ctx.progress.sub(static_cast<double>(done) / span,
                                                static_cast<double>(saturating_add(done, it.packed)) / span)));
    CVT_TRY(sink.charge(content.size()));
    CVT_TRY(ex.add_file(it, std::move(content)));
    done = saturating_add(done, it.packed);
  }

  std::string comment;
  for (const Item& c : comments) {
    if (c.unpacked > 1024 * 1024) continue;
    Rar5Decoder local;
    if (needs_decoder(c)) {
      local.unpack.init(std::max<std::uint64_t>(std::min(c.dictionary, c.unpacked + 0x10000), 0x10000));
      local.ready = true;
    }
    Item copy = c;
    copy.solid = false;
    const ByteView packed = data.subspan(static_cast<std::size_t>(c.data_offset), static_cast<std::size_t>(c.packed));
    auto text = unpack_rar5(ex, copy, packed, local, keys, Progress{});
    if (!text) {
      ctx.warn("the RAR archive comment could not be read: " + text.error().message);
      continue;
    }
    comment = util::sanitize_utf8(as_chars(*text));
  }
  return sink.finish(std::move(comment));
}

// --------------------------------------------------------------------------- RAR 2.9 - 4.x

struct KeyCache3 {
  std::string password;
  std::vector<std::pair<Bytes, rar::Rar3Key>> entries;
  const rar::Rar3Key& get(ByteView salt) {
    for (const auto& [k, v] : entries) {
      if (ByteView(k).size() == salt.size() && std::equal(k.begin(), k.end(), salt.begin())) return v;
    }
    entries.emplace_back(Bytes(salt.begin(), salt.end()), rar::rar3_key(password, salt));
    return entries.back().second;
  }
};

std::uint32_t le32(ByteView v, std::size_t at) {
  return std::to_integer<std::uint32_t>(v[at]) | (std::to_integer<std::uint32_t>(v[at + 1]) << 8) |
         (std::to_integer<std::uint32_t>(v[at + 2]) << 16) | (std::to_integer<std::uint32_t>(v[at + 3]) << 24);
}
std::uint16_t le16(ByteView v, std::size_t at) {
  return static_cast<std::uint16_t>(std::to_integer<unsigned>(v[at]) | (std::to_integer<unsigned>(v[at + 1]) << 8));
}

/// RAR's compact Unicode name encoding: a high byte, then 2-bit opcodes that
/// pick a byte, a byte with the high byte, a full 16-bit unit, or a run
/// copied from the ASCII name (optionally with a correction).
std::string decode_rar_unicode(ByteView ascii, ByteView enc) {
  std::u16string out;
  std::size_t pos = 0;
  if (enc.empty()) return {};
  const unsigned high = std::to_integer<unsigned>(enc[pos++]);
  unsigned flags = 0;
  int flag_bits = 0;
  const auto byte = [&]() -> unsigned { return pos < enc.size() ? std::to_integer<unsigned>(enc[pos++]) : 0u; };
  while (pos < enc.size() && out.size() < 0x10000) {
    if (flag_bits == 0) {
      flags = byte();
      flag_bits = 8;
    }
    flag_bits -= 2;
    switch ((flags >> flag_bits) & 3u) {
      case 0:
        if (pos >= enc.size()) return util::utf16_to_utf8(out);
        out.push_back(static_cast<char16_t>(byte()));
        break;
      case 1:
        if (pos >= enc.size()) return util::utf16_to_utf8(out);
        out.push_back(static_cast<char16_t>(byte() | (high << 8)));
        break;
      case 2: {
        if (pos + 2 > enc.size()) return util::utf16_to_utf8(out);
        const unsigned lo = byte();
        out.push_back(static_cast<char16_t>(lo | (byte() << 8)));
        break;
      }
      default: {
        unsigned length = byte();
        if ((length & 0x80u) != 0) {
          const unsigned correction = byte();
          for (length = (length & 0x7Fu) + 2; length > 0 && out.size() < ascii.size(); --length) {
            const unsigned c = (std::to_integer<unsigned>(ascii[out.size()]) + correction) & 0xFFu;
            out.push_back(static_cast<char16_t>(c | (high << 8)));
          }
        } else {
          for (length += 2; length > 0 && out.size() < ascii.size(); --length) {
            out.push_back(static_cast<char16_t>(std::to_integer<unsigned>(ascii[out.size()])));
          }
        }
        break;
      }
    }
  }
  return util::utf16_to_utf8(out);
}

struct Rar4Block {
  std::uint8_t type = 0;
  std::uint16_t flags = 0;
  Bytes storage;
  ByteView header{};       // the whole header (CRC included)
  std::uint64_t data_offset = 0;
  std::uint64_t data_size = 0;
};

class Rar4Parser {
 public:
  Rar4Parser(ByteView data, std::uint64_t start, KeyCache3& keys) : data_(data), pos_(start), keys_(keys) {}

  void encrypt_headers() { encrypted_ = true; }

  Result<std::optional<Rar4Block>> next() {
    if (pos_ >= data_.size()) return std::optional<Rar4Block>{};
    Rar4Block b;
    std::uint64_t at = pos_;
    if (encrypted_) {
      if (data_.size() - at < 8 + 16) {
        if (data_.size() - at < 8) return std::optional<Rar4Block>{};
        return fail(Status::Decode, "RAR encrypted header is truncated");
      }
      const ByteView salt = data_.subspan(static_cast<std::size_t>(at), 8);
      const rar::Rar3Key& key = keys_.get(salt);
      Aes aes;
      if (!aes.set_key(std::span<const std::uint8_t>(key.key.data(), 16))) return fail(Status::Internal, "AES key setup failed");
      at += 8;
      Bytes first(data_.begin() + static_cast<std::ptrdiff_t>(at), data_.begin() + static_cast<std::ptrdiff_t>(at + 16));
      Aes::Block iv = key.iv;
      CVT_TRY(aes_cbc_decrypt(aes, iv, std::span<std::byte>(first)));
      const std::uint16_t size = le16(first, 5);
      if (size < 7) return fail(Status::Decode, "wrong password (the RAR headers do not decrypt)");
      const std::uint64_t total = (std::uint64_t{size} + 15) / 16 * 16;
      if (total > data_.size() - at) return fail(Status::Decode, "wrong password (the RAR headers do not decrypt) or a truncated archive");
      b.storage.assign(data_.begin() + static_cast<std::ptrdiff_t>(at), data_.begin() + static_cast<std::ptrdiff_t>(at + total));
      iv = key.iv;
      CVT_TRY(aes_cbc_decrypt(aes, iv, std::span<std::byte>(b.storage)));
      b.header = ByteView(b.storage).first(size);
      b.data_offset = at + total;
    } else {
      if (data_.size() - at < 7) return fail(Status::Decode, "RAR block header is truncated");
      const std::uint16_t size = le16(data_, static_cast<std::size_t>(at) + 5);
      if (size < 7 || size > data_.size() - at) return fail(Status::Decode, "RAR block header is truncated or damaged");
      b.header = data_.subspan(static_cast<std::size_t>(at), size);
      b.data_offset = at + size;
    }
    b.type = std::to_integer<std::uint8_t>(b.header[2]);
    b.flags = le16(b.header, 3);
    const std::uint16_t crc = le16(b.header, 0);
    const bool checked = b.type == 0x73 || b.type == 0x74 || b.type == 0x7A || b.type == 0x7B;
    if (checked && (util::crc32(b.header.subspan(2)) & 0xFFFFu) != crc) {
      // An end block may be shorter than the CRC covers in old archives.
      if (encrypted_) return fail(Status::Decode, "wrong password (the RAR headers do not decrypt)");
      if (b.type != 0x7B) return fail(Status::Decode, "RAR header CRC mismatch (damaged archive)");
    }
    if (b.type == 0x74 || b.type == 0x7A || (b.flags & 0x8000u) != 0) {
      if (b.header.size() < 11) return fail(Status::Decode, "RAR block header is truncated");
      b.data_size = le32(b.header, 7);
      if ((b.type == 0x74 || b.type == 0x7A) && (b.flags & 0x100u) != 0 && b.header.size() >= 36) {
        b.data_size |= std::uint64_t{le32(b.header, 32)} << 32;
      }
    }
    if (b.data_size > data_.size() - std::min<std::uint64_t>(b.data_offset, data_.size())) {
      return fail(Status::Decode, "RAR archive is truncated (data runs past the end)");
    }
    pos_ = b.data_offset + b.data_size;
    return std::optional<Rar4Block>(std::move(b));
  }

 private:
  ByteView data_;
  std::uint64_t pos_;
  KeyCache3& keys_;
  bool encrypted_ = false;
};

Result<Item> parse_rar4_file(const Rar4Block& b) {
  const ByteView h = b.header;
  if (h.size() < 32) return fail(Status::Decode, "RAR file header is truncated");
  Item it;
  const std::uint16_t flags = b.flags;
  std::uint64_t unpacked = le32(h, 11);
  const std::uint8_t host = std::to_integer<std::uint8_t>(h[15]);
  it.crc = le32(h, 16);
  it.has_crc = true;
  const std::uint32_t ftime = le32(h, 20);
  it.version = std::to_integer<int>(h[24]);
  const int method = std::to_integer<int>(h[25]);
  const std::size_t name_size = le16(h, 26);
  const std::uint32_t attributes = le32(h, 28);
  std::size_t pos = 32;
  if ((flags & 0x100u) != 0) {
    if (h.size() < 40) return fail(Status::Decode, "RAR file header is truncated");
    unpacked |= std::uint64_t{le32(h, 36)} << 32;
    pos = 40;
  }
  if (name_size > h.size() - pos) return fail(Status::Decode, "RAR file header is truncated");
  const ByteView name = h.subspan(pos, name_size);
  pos += name_size;
  if ((flags & 0x200u) != 0) {
    const auto zero = std::find(name.begin(), name.end(), std::byte{0});
    if (zero == name.end()) {
      it.raw_name = util::sanitize_utf8(as_chars(name));
    } else {
      const auto ascii_size = static_cast<std::size_t>(zero - name.begin());
      it.raw_name = decode_rar_unicode(name.first(ascii_size), name.subspan(ascii_size + 1));
    }
  } else {
    it.raw_name = decode_name(as_chars(name), false, host != 3);
  }
  std::replace(it.raw_name.begin(), it.raw_name.end(), '\\', '/');
  it.name = it.raw_name;
  if ((flags & 0x400u) != 0) {
    if (h.size() - pos < 8) return fail(Status::Decode, "RAR file header is truncated");
    const ByteView salt = h.subspan(pos, 8);
    it.salt.assign(salt.begin(), salt.end());
    pos += 8;
  }
  it.mtime = util::dos_to_unix_time(static_cast<std::uint16_t>(ftime >> 16), static_cast<std::uint16_t>(ftime & 0xFFFFu));
  if ((flags & 0x1000u) != 0 && h.size() - pos >= 2) {
    const unsigned tflags = le16(h, pos);
    pos += 2;
    for (int i = 0; i < 4; ++i) {
      const unsigned mode = (tflags >> ((3 - i) * 4)) & 0xFu;
      if ((mode & 8u) == 0) continue;
      if (i != 0) {
        if (h.size() - pos < 4) break;
        pos += 4;
      }
      const std::size_t count = mode & 3u;
      if (h.size() - pos < count) break;
      pos += count;
      if (i == 0 && (mode & 4u) != 0) it.mtime += 1;
    }
  }
  it.unpacked = unpacked;
  it.packed = b.data_size;
  it.data_offset = b.data_offset;
  it.split_before = (flags & 0x01u) != 0;
  it.split_after = (flags & 0x02u) != 0;
  it.encrypted = (flags & 0x04u) != 0;
  it.solid = (flags & 0x10u) != 0;
  it.directory = (flags & 0xE0u) == 0xE0u;
  it.dictionary = std::uint64_t{64 * 1024} << (it.directory ? 0u : ((flags >> 5) & 7u));
  if (method < 0x30 || method > 0x35) return fail(Status::Decode, "'{}' uses unknown RAR method 0x{:02X}", it.name, method);
  it.method = method - 0x30;
  if (host == 3) {  // Unix
    const std::uint32_t type = attributes & 0170000u;
    it.mode = attributes & 07777u;
    if (type == 0040000u) it.directory = true;
    if (type == 0120000u) it.symlink = true;
  } else if ((attributes & 0x10u) != 0) {
    it.directory = true;
  }
  return it;
}

struct Rar3Decoder {
  rar::Unpack3 unpack;
  bool ready = false;
};

Result<Bytes> unpack_rar4(Extractor& ex, const Item& it, ByteView packed, Rar3Decoder& decoder, KeyCache3& keys,
                          const Progress& progress) {
  Bytes decrypted;
  if (it.encrypted) {
    if (it.version < 29) {
      return fail(Status::Decode, "'{}' uses RAR {} encryption, which is not supported", it.name,
                  it.version < 20 ? "1.5" : "2.0");
    }
    if (ex.password.empty()) return fail(Status::Decode, "password required: '{}' is encrypted", it.name);
    if (packed.size() % 16 != 0) return fail(Status::Decode, "encrypted RAR data of '{}' is damaged", it.name);
    const rar::Rar3Key& key = keys.get(it.salt);
    Aes aes;
    if (!aes.set_key(std::span<const std::uint8_t>(key.key.data(), 16))) return fail(Status::Internal, "AES key setup failed");
    decrypted.assign(packed.begin(), packed.end());
    Aes::Block iv = key.iv;
    CVT_TRY(aes_cbc_decrypt(aes, iv, std::span<std::byte>(decrypted)));
    packed = decrypted;
  }
  if (it.unpacked > ex.sink.budget()) {
    return fail(Status::Limit, "'{}' would decompress to {} bytes, more than the remaining limit", it.name, it.unpacked);
  }
  Bytes content;
  if (it.method == 0 || (it.unpacked == 0 && packed.empty())) {
    if (packed.size() < it.unpacked) return fail(Status::Decode, "stored RAR data of '{}' is truncated", it.name);
    content.assign(packed.begin(), packed.begin() + static_cast<std::ptrdiff_t>(it.unpacked));
  } else {
    if (it.version < 20) {
      return fail(Status::Decode, "'{}' uses RAR 1.5 compression, which is not supported", it.name);
    }
    if (it.version < 29) {
      return fail(Status::Decode, "'{}' uses RAR 2.0 compression, which is not supported", it.name);
    }
    if (it.version != 29 && it.version != 36) {
      return fail(Status::Decode, "'{}' needs RAR version {}.{} to unpack, which is not supported", it.name,
                  it.version / 10, it.version % 10);
    }
    if (!decoder.ready) return fail(Status::Internal, "RAR decoder is not initialised");
    FileOutput out;
    out.bytes = &content;
    out.size = it.unpacked;
    out.budget = ex.sink.budget();
    out.progress = progress;
    auto r = decoder.unpack.decode(packed, it.solid, out, ex.sink.budget());
    if (!r) {
      std::string message = r.error().message;
      if (r.error().code == Status::Decode) verify_hint(message, it);
      // RAR 3 has no password check value: with a wrong key the decrypted
      // garbage may also ask for absurd resources.
      if (r.error().code == Status::Limit && it.encrypted) {
        return fail(Status::Decode, "wrong password for '{}' (or damaged data: {})", it.name, message);
      }
      return fail(r.error().code, "{}", message);
    }
  }
  if (util::crc32(content) != it.crc) {
    if (it.encrypted) return fail(Status::Decode, "wrong password for '{}' (CRC mismatch)", it.name);
    return fail(Status::Decode, "CRC mismatch in '{}'", it.name);
  }
  return content;
}

Result<ir::Archive> read_rar4(ReadContext& ctx, ByteView data, std::uint64_t start) {
  EntrySink sink(ctx, data.size());
  KeyCache3 keys;
  keys.password = ctx.options.get_string("password", "");
  Rar4Parser parser(data, start, keys);
  std::vector<Item> items;
  std::vector<Item> comments;
  bool main_seen = false;
  bool volume = false;
  bool ended = false;
  while (!ended) {
    CVT_TRY_ASSIGN(std::optional<Rar4Block> b, parser.next());
    if (!b) break;
    switch (b->type) {
      case 0x73: {
        main_seen = true;
        volume = (b->flags & 0x01u) != 0;  // later volumes show up through their continued files
        if ((b->flags & 0x02u) != 0) ctx.warn("the old-style RAR archive comment is not read");
        if ((b->flags & 0x80u) != 0) {
          if (keys.password.empty()) return fail(Status::Decode, "password required: the RAR archive has encrypted headers");
          parser.encrypt_headers();
        }
        break;
      }
      case 0x74:
      case 0x7A: {
        if (!main_seen) return fail(Status::Decode, "RAR archive header is missing");
        CVT_TRY_ASSIGN(Item it, parse_rar4_file(*b));
        if (b->type == 0x74) {
          CVT_TRY(ctx.limits.check_archive_entries(items.size() + 1));
          if (it.split_before && items.empty() && volume) CVT_TRY(later_volume());
          items.push_back(std::move(it));
        } else if (it.name == "CMT") {
          comments.push_back(std::move(it));
        }
        break;
      }
      case 0x7B:
        ended = true;
        break;
      default:
        break;  // old comments, authenticity, recovery records, old sub blocks
    }
  }
  if (!ended && !volume) ctx.warn("the RAR archive has no end block (it may be truncated)");

  Extractor ex{ctx, sink, keys.password, {}, {}, {}};
  Rar3Decoder decoder;
  std::uint64_t total = 0;
  for (const Item& it : items) total = saturating_add(total, it.packed);
  std::uint64_t done = 0;
  for (const Item& it : items) {
    const double span = static_cast<double>(std::max<std::uint64_t>(total, 1));
    CVT_TRY(ctx.checkpoint(static_cast<double>(done) / span));
    if (it.split_before || it.split_after) CVT_TRY(split_file(it));
    if (it.directory) {
      CVT_TRY(ex.add_simple(it));
      continue;
    }
    if (needs_decoder(it) || (it.symlink && it.method != 0)) {
      if (!decoder.ready) {
        CVT_TRY_ASSIGN(const std::uint64_t window, window_for(items, sink, filter_room3));
        decoder.unpack.init(window);
        decoder.ready = true;
      }
    }
    const ByteView packed = data.subspan(static_cast<std::size_t>(it.data_offset), static_cast<std::size_t>(it.packed));
    CVT_TRY_ASSIGN(Bytes content,
                   unpack_rar4(ex, it, packed, decoder, keys,
                               ctx.progress.sub(static_cast<double>(done) / span,
                                                static_cast<double>(saturating_add(done, it.packed)) / span)));
    CVT_TRY(sink.charge(content.size()));
    done = saturating_add(done, it.packed);
    if (it.symlink) {
      Item link = it;
      link.link_target = util::sanitize_utf8(as_chars(content));
      CVT_TRY(ex.add_simple(link));
      continue;
    }
    CVT_TRY(ex.add_file(it, std::move(content)));
  }

  std::string comment;
  for (const Item& c : comments) {
    if (c.unpacked > 1024 * 1024) continue;
    Rar3Decoder local;
    if (c.method != 0) {
      local.unpack.init(c.unpacked + 0x10000);
      local.ready = true;
    }
    Item copy = c;
    copy.solid = false;
    const ByteView packed = data.subspan(static_cast<std::size_t>(c.data_offset), static_cast<std::size_t>(c.packed));
    auto text = unpack_rar4(ex, copy, packed, local, keys, Progress{});
    if (!text) {
      ctx.warn("the RAR archive comment could not be read: " + text.error().message);
      continue;
    }
    comment = decode_name(as_chars(*text), false, true);
  }
  return sink.finish(std::move(comment));
}

// --------------------------------------------------------------------------- entry point

Result<ir::Archive> read_rar(ReadContext& ctx) {
  const ByteView data = ctx.input;
  if (starts_with(data, sig_rar5)) return read_rar5(ctx, data, sig_rar5.size());
  if (starts_with(data, sig_rar4)) return read_rar4(ctx, data, sig_rar4.size());
  if (starts_with(data, "RE~^")) return fail(Status::Decode, "RAR 1.4 archives are not supported");
  // A self-extracting archive: the archive follows the executable stub.
  const std::size_t limit = std::min<std::size_t>(data.size(), 4 * 1024 * 1024);
  const std::string_view text = as_chars(data.first(limit));
  for (std::size_t at = text.find("Rar!\x1A\x07"); at != std::string_view::npos; at = text.find("Rar!\x1A\x07", at + 1)) {
    if (starts_with(data, sig_rar5, at)) return read_rar5(ctx, data, at + sig_rar5.size());
    if (starts_with(data, sig_rar4, at)) return read_rar4(ctx, data, at + sig_rar4.size());
  }
  return fail(Status::Decode, "not a RAR archive (no RAR signature)");
}

}  // namespace

void register_rar_format(Registry& registry) {
  ArchiveFormat f;
  f.info.id = "rar";
  f.info.name = "RAR archive";
  f.info.description =
      "A proprietary archive format (RAR 2.9 to 7) with strong compression, solid mode and AES encryption. "
      "Read only.";
  f.info.category = Category::Archive;
  f.info.extensions = {"rar"};
  f.info.mime = "application/vnd.rar";
  f.info.popularity = 75;
  f.info.signatures = {Signature::at(0, "'Rar!' 1A 07 00"), Signature::at(0, "'Rar!' 1A 07 01 00")};
  f.read = read_rar;
  f.read_options = {option_password()};
  f.read_cost = 20;
  register_archive_format(registry, std::move(f));
}

}  // namespace cvt::archive
