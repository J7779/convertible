// Convertible core - archive family: xar archives (and macOS .pkg / .xip, which are xar), read only.
//
// Layout (xar file format as documented by the xar project): a big-endian
// header
//   magic "xar!", header size u16, version u16 (1), TOC compressed length u64,
//   TOC uncompressed length u64, checksum algorithm u32 (0 none, 1 SHA-1,
//   2 MD5, 3 named: the name follows in the header, e.g. "sha256")
// then the zlib-compressed XML table of contents, then the heap. The TOC's
// own checksum is stored in the heap at <toc><checksum><offset>/<size> and
// covers the compressed TOC. Every <file> has a name, type (file, directory,
// symlink, hardlink...), mode, mtime and, for content, a <data> element with
// offset/length (in the heap), size (extracted), an encoding style (none,
// zlib "application/x-gzip", bzip2, xz, lzma) and checksums of the archived
// and the extracted bytes, which are verified.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <format>
#include <map>

#include "families/archive/compression.hpp"
#include "families/archive/formats/unix_internal.hpp"
#include "util/base64.hpp"
#include "util/hash.hpp"
#include "util/text.hpp"
#include "util/xml.hpp"

namespace cvt::archive::unixfmt {

namespace {

enum class Hash { None, Sha1, Md5, Sha256, Unknown };

Hash hash_from_name(std::string_view name) {
  const std::string n = util::to_lower(util::trim(name));
  if (n == "sha1") return Hash::Sha1;
  if (n == "md5") return Hash::Md5;
  if (n == "sha256") return Hash::Sha256;
  if (n == "none" || n.empty()) return Hash::None;
  return Hash::Unknown;
}

std::size_t digest_size(Hash h) {
  switch (h) {
    case Hash::Sha1: return 20;
    case Hash::Md5: return 16;
    case Hash::Sha256: return 32;
    case Hash::None:
    case Hash::Unknown: return 0;
  }
  return 0;
}

Bytes digest(Hash h, ByteView data) {
  const auto to = [](const auto& d) {
    Bytes out(d.size());
    for (std::size_t i = 0; i < d.size(); ++i) out[i] = static_cast<std::byte>(d[i]);
    return out;
  };
  switch (h) {
    case Hash::Sha1: return to(util::sha1(data));
    case Hash::Md5: return to(util::md5(data));
    case Hash::Sha256: return to(util::sha256(data));
    case Hash::None:
    case Hash::Unknown: return {};
  }
  return {};
}

std::string hex(ByteView data) {
  static constexpr std::string_view digits = "0123456789abcdef";
  std::string out;
  for (const std::byte b : data) {
    out += digits[std::to_integer<unsigned>(b) >> 4];
    out += digits[std::to_integer<unsigned>(b) & 15u];
  }
  return out;
}

// Check `data` against a checksum element (<archived-checksum style="sha1">hex</...>).
// Returns false on a mismatch; unknown styles pass.
bool checksum_ok(util::XmlNode element, ByteView data) {
  if (!element) return true;
  const Hash h = hash_from_name(element.attribute("style").value());
  if (h == Hash::None || h == Hash::Unknown) return true;
  return util::to_lower(util::trim(element.text())) == hex(digest(h, data));
}

std::optional<compression::Method> method_for(std::string_view style) {
  using compression::Method;
  if (style.empty() || style == "application/octet-stream") return Method::Store;
  if (style == "application/x-gzip" || style == "application/zlib") return Method::Zlib;  // xar's "gzip" is zlib
  if (style == "application/x-bzip2") return Method::Bzip2;
  if (style == "application/x-xz") return Method::Xz;
  if (style == "application/x-lzma") return Method::LzmaAlone;
  return std::nullopt;
}

std::uint64_t number(util::XmlNode node) {
  return parse_field(util::trim(node.text()), 10).value_or(0);
}

}  // namespace

Result<ir::Archive> read_xar(ReadContext& ctx) {
  const ByteView input = ctx.input;
  util::ByteReader r(input);
  if (!r.consume("xar!")) return fail(Status::Decode, "not a xar archive");
  const std::uint16_t header_size = r.u16be();
  const std::uint16_t version = r.u16be();
  const std::uint64_t toc_packed = r.u64be();
  const std::uint64_t toc_size = r.u64be();
  const std::uint32_t algorithm = r.u32be();
  if (!r.ok() || header_size < 28 || header_size > input.size()) return fail(Status::Decode, "xar header is damaged");
  if (version != 1) return fail(Status::Decode, "unsupported xar version {}", version);
  if (toc_packed > input.size() - header_size) return fail(Status::Decode, "xar table of contents is truncated");
  Hash toc_hash = Hash::None;
  if (algorithm == 1) {
    toc_hash = Hash::Sha1;
  } else if (algorithm == 2) {
    toc_hash = Hash::Md5;
  } else if (algorithm == 3) {
    const std::string_view named = as_chars(input.subspan(28, header_size - 28u));
    toc_hash = hash_from_name(named.substr(0, named.find('\0')));
  } else if (algorithm != 0) {
    toc_hash = Hash::Unknown;
  }
  EntrySink sink(ctx, input.size());
  const std::uint64_t toc_cap = std::min<std::uint64_t>(sink.budget(), 256u << 20);
  if (toc_size > toc_cap) return fail(Status::Limit, "xar table of contents of {} bytes is too large", toc_size);
  const ByteView packed = input.subspan(header_size, static_cast<std::size_t>(toc_packed));
  const ByteView heap = input.subspan(header_size + static_cast<std::size_t>(toc_packed));
  compression::DecodeOptions d;
  d.max_output = toc_size;
  CVT_TRY_ASSIGN(const Bytes toc_bytes, compression::decompress(compression::Method::Zlib, packed, d));
  if (toc_bytes.size() != toc_size) return fail(Status::Decode, "xar table of contents has the wrong size");
  util::XmlParseOptions xml_options;
  xml_options.max_depth = 512;
  CVT_TRY_ASSIGN(const util::XmlDocument doc, util::parse_xml(ByteView(toc_bytes), xml_options));
  const util::XmlNode toc = doc.root().child("toc");
  if (doc.root().name() != "xar" || !toc) return fail(Status::Decode, "xar table of contents is not a xar TOC");

  // The TOC checksum, stored in the heap.
  if (toc_hash == Hash::Unknown) {
    ctx.warn("The xar table of contents uses an unknown checksum; it was not verified.");
  } else if (toc_hash != Hash::None) {
    const util::XmlNode c = toc.child("checksum");
    const std::uint64_t offset = number(c.child("offset"));
    const std::uint64_t size = number(c.child("size"));
    if (!c || size != digest_size(toc_hash) || offset > heap.size() || size > heap.size() - offset) {
      return fail(Status::Decode, "xar table of contents checksum is missing");
    }
    if (!std::ranges::equal(heap.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(size)), digest(toc_hash, packed))) {
      return fail(Status::Decode, "xar table of contents checksum mismatch");
    }
  }

  // Walk the <file> tree without recursion.
  struct Item {
    util::XmlNode node;
    std::string parent;
  };
  struct Pending {
    std::size_t entry;
    std::string original;  // xar id of the file holding the data
  };
  std::vector<ir::ArchiveEntry> entries;
  std::map<std::string, std::size_t, std::less<>> by_id;
  std::vector<Pending> hardlinks;
  std::vector<Item> stack;
  for (const util::XmlNode f : toc.children("file")) stack.push_back({f, {}});
  std::reverse(stack.begin(), stack.end());
  std::size_t steps = 0;
  std::size_t skipped_special = 0;
  while (!stack.empty()) {
    if ((++steps & 255u) == 0) CVT_TRY(ctx.checkpoint(0.5));
    const Item item = std::move(stack.back());
    stack.pop_back();
    const util::XmlNode f = item.node;
    const util::XmlNode name_node = f.child("name");
    std::string name = name_node.text();
    if (name_node.attribute("enctype").value() == "base64") {
      auto decoded = util::base64_decode(name);
      name = decoded ? decode_name(as_chars(*decoded), false, false) : std::string();
    } else {
      name = util::sanitize_utf8(name);
    }
    const std::string path = item.parent.empty() ? name : item.parent + "/" + name;
    // Children first in document order.
    std::vector<Item> children;
    for (const util::XmlNode c : f.children("file")) children.push_back({c, path});
    for (auto it = children.rbegin(); it != children.rend(); ++it) stack.push_back(std::move(*it));
    if (stack.size() > 1000000) return fail(Status::Limit, "xar table of contents has too many files");

    const util::XmlNode type_node = f.child("type");
    const std::string type(util::trim(type_node.text()));
    ir::ArchiveEntry entry;
    entry.path = path;
    entry.unix_mode = static_cast<std::uint32_t>(parse_field(util::trim(f.child("mode").text()), 8).value_or(0) & 07777u);
    entry.mtime = parse_iso_time(f.child("mtime").text()).value_or(0);
    const util::XmlNode data = f.child("data");
    bool has_data = false;
    if (type == "directory") {
      entry.is_directory = true;
    } else if (type == "symlink") {
      entry.symlink_target = util::sanitize_utf8(f.child("link").text());
      if (entry.symlink_target.empty()) continue;
    } else if (type == "file" || type == "hardlink") {
      if (data) {
        const std::uint64_t offset = number(data.child("offset"));
        const std::uint64_t length = number(data.child("length"));
        const std::uint64_t size = number(data.child("size"));
        if (offset > heap.size() || length > heap.size() - offset) return fail(Status::Decode, "xar file data is out of range");
        const ByteView archived = heap.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(length));
        if (!checksum_ok(data.child("archived-checksum"), archived)) {
          return fail(Status::Decode, "xar checksum mismatch for '{}'", path);
        }
        const std::string_view style = data.child("encoding").attribute("style").value();
        const auto method = method_for(style);
        if (!method) return fail(Status::Decode, "xar encoding '{}' is not supported", util::sanitize_utf8(style));
        if (size > sink.budget()) return fail(Status::Limit, "xar entry '{}' of {} bytes exceeds the limit", path, size);
        if (*method == compression::Method::Store) {
          entry.bytes = to_bytes(archived);
        } else {
          if (!compression::can_decode(*method)) {
            return fail(Status::Decode, "xar entry '{}' uses {}, which this build cannot decompress", path,
                        compression::method_name(*method));
          }
          compression::DecodeOptions dd;
          dd.max_output = std::min(sink.budget(), size);
          CVT_TRY_ASSIGN(entry.bytes, compression::decompress(*method, archived, dd));
        }
        if (entry.bytes.size() != size) return fail(Status::Decode, "xar entry '{}' has the wrong size", path);
        if (!checksum_ok(data.child("extracted-checksum"), entry.bytes)) {
          return fail(Status::Decode, "xar checksum mismatch for '{}'", path);
        }
        CVT_TRY(sink.charge(entry.bytes.size()));
        has_data = true;
      }
      if (type == "hardlink" && !has_data) {
        const std::string_view original = type_node.attribute("link").value();
        if (original != "original") hardlinks.push_back({entries.size(), std::string(original)});
      }
    } else {
      ++skipped_special;
      continue;
    }
    CVT_TRY(ctx.limits.check_archive_entries(entries.size() + 1));
    by_id.emplace(std::string(f.attribute("id").value()), entries.size());
    entries.push_back(std::move(entry));
  }
  for (const Pending& p : hardlinks) {
    const auto found = by_id.find(p.original);
    if (found == by_id.end() || !entries[found->second].is_file()) {
      ctx.warn("A xar hard link to a file that is not in the archive was left empty.");
      continue;
    }
    CVT_TRY(sink.charge(entries[found->second].bytes.size()));
    entries[p.entry].bytes = entries[found->second].bytes;
  }
  if (skipped_special != 0) ctx.warn(std::format("{} device file(s), FIFO(s) or socket(s) were skipped.", skipped_special));
  for (ir::ArchiveEntry& e : entries) CVT_TRY(sink.add(std::move(e)));
  return sink.finish();
}

}  // namespace cvt::archive::unixfmt
