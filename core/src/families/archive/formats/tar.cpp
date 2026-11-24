// Convertible core - archive family: tar (v7, ustar, GNU, pax) and the compressed tar.* formats.
//
// Reading understands v7, ustar (prefix field), GNU (long names and links,
// base-256 numbers, old-GNU sparse files with extension headers) and pax
// (extended and global headers, GNU sparse formats 0.0, 0.1 and 1.0).
// Hard links become copies of the file they point to; devices and FIFOs are
// skipped. Writing produces pax-compatible ustar: plain headers whenever the
// entry fits, pax records only for what ustar cannot hold, owner 0 with no
// owner names, and no access or change times.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/archive/formats/tar.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <format>
#include <map>
#include <optional>

#include "engine/registry.hpp"
#include "families/archive/compression.hpp"
#include "families/archive/formats/formats.hpp"
#include "util/text.hpp"

namespace cvt::archive {

namespace {

constexpr std::size_t block = 512;

std::string_view field(ByteView h, std::size_t at, std::size_t len) {
  std::string_view s = as_chars(h.subspan(at, len));
  const std::size_t nul = s.find('\0');
  return nul == std::string_view::npos ? s : s.substr(0, nul);
}

// Octal (space/NUL padded) or GNU base-256 number.
std::optional<std::uint64_t> number(ByteView h, std::size_t at, std::size_t len) {
  const auto first = std::to_integer<std::uint8_t>(h[at]);
  if (first & 0x80u) {
    if (first == 0xFF) return std::nullopt;  // negative base-256
    std::uint64_t v = first & 0x7Fu;
    for (std::size_t i = 1; i < len; ++i) {
      if (v > (std::numeric_limits<std::uint64_t>::max() >> 8)) return std::nullopt;
      v = (v << 8) | std::to_integer<std::uint64_t>(h[at + i]);
    }
    return v;
  }
  std::uint64_t v = 0;
  bool any = false;
  for (std::size_t i = 0; i < len; ++i) {
    const char c = static_cast<char>(h[at + i]);
    if (c == ' ' && !any) continue;
    if (c == '\0' || c == ' ') break;
    if (c < '0' || c > '7') return std::nullopt;
    if (v > (std::numeric_limits<std::uint64_t>::max() >> 3)) return std::nullopt;
    v = v * 8 + static_cast<std::uint64_t>(c - '0');
    any = true;
  }
  return v;
}

struct Sparse {
  std::vector<std::pair<std::uint64_t, std::uint64_t>> map;  // (offset, length)
  std::uint64_t real_size = 0;
  bool active = false;
  bool map_in_data = false;  // pax GNU.sparse 1.0
};

std::optional<std::uint64_t> parse_u64(std::string_view s) {
  std::uint64_t v = 0;
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc{} || end != s.data() + s.size()) return std::nullopt;
  return v;
}

// Parse pax records ("len key=value\n").
Result<void> parse_pax(ByteView data, std::map<std::string, std::string>& out, Sparse& sparse) {
  std::size_t pos = 0;
  std::optional<std::uint64_t> pending_offset;
  while (pos < data.size()) {
    const std::string_view rest = as_chars(data.subspan(pos));
    const std::size_t space = rest.find(' ');
    if (space == std::string_view::npos || space == 0 || space > 20) {
      if (rest.find_first_not_of(std::string_view("\0", 1)) == std::string_view::npos) break;
      return fail(Status::Decode, "tar pax header is damaged");
    }
    const auto len = parse_u64(rest.substr(0, space));
    if (!len || *len <= space + 1 || *len > rest.size()) return fail(Status::Decode, "tar pax header is damaged");
    const std::string_view record = rest.substr(space + 1, static_cast<std::size_t>(*len) - space - 2);
    const std::size_t eq = record.find('=');
    if (eq == std::string_view::npos) return fail(Status::Decode, "tar pax header is damaged");
    const std::string_view key = record.substr(0, eq);
    const std::string_view value = record.substr(eq + 1);
    if (key == "GNU.sparse.offset") {
      pending_offset = parse_u64(value);
    } else if (key == "GNU.sparse.numbytes" && pending_offset) {
      const auto n = parse_u64(value);
      if (n) sparse.map.emplace_back(*pending_offset, *n);
      pending_offset.reset();
      sparse.active = true;
    } else if (key == "GNU.sparse.map") {
      const auto parts = util::split(value, ',', false);
      for (std::size_t i = 0; i + 1 < parts.size(); i += 2) {
        const auto o = parse_u64(parts[i]);
        const auto n = parse_u64(parts[i + 1]);
        if (!o || !n) return fail(Status::Decode, "tar sparse map is damaged");
        sparse.map.emplace_back(*o, *n);
      }
      sparse.active = true;
    } else if (key == "GNU.sparse.realsize" || key == "GNU.sparse.size") {
      if (const auto v = parse_u64(value)) sparse.real_size = *v;
      sparse.active = true;
    } else if (key == "GNU.sparse.major" && value == "1") {
      sparse.active = true;
      sparse.map_in_data = true;
    } else {
      out[std::string(key)] = std::string(value);
    }
    pos += static_cast<std::size_t>(*len);
  }
  return {};
}

std::int64_t pax_time(const std::string& value) {
  const std::size_t dot = value.find('.');
  const std::string_view whole = std::string_view(value).substr(0, dot);
  std::int64_t v = 0;
  const auto [end, ec] = std::from_chars(whole.data(), whole.data() + whole.size(), v);
  return ec == std::errc{} && end == whole.data() + whole.size() ? v : 0;
}

Result<Bytes> expand_sparse(ByteView stored, const Sparse& sparse, std::uint64_t budget) {
  if (sparse.real_size > budget) return fail(Status::Limit, "sparse tar entry of {} bytes exceeds the limit", sparse.real_size);
  Bytes out(static_cast<std::size_t>(sparse.real_size), std::byte{0});
  std::uint64_t at = 0;
  for (const auto& [offset, length] : sparse.map) {
    if (length > stored.size() - std::min<std::uint64_t>(at, stored.size()) || offset > sparse.real_size ||
        length > sparse.real_size - offset) {
      return fail(Status::Decode, "tar sparse map does not match the data");
    }
    std::memcpy(out.data() + offset, stored.data() + at, static_cast<std::size_t>(length));
    at += length;
  }
  return out;
}

}  // namespace

bool is_tar_header(ByteView h) noexcept {
  if (h.size() < block) return false;
  std::uint64_t unsigned_sum = 0;
  std::int64_t signed_sum = 0;
  bool all_zero = true;
  for (std::size_t i = 0; i < block; ++i) {
    const auto b = (i >= 148 && i < 156) ? std::uint8_t{' '} : std::to_integer<std::uint8_t>(h[i]);
    unsigned_sum += b;
    signed_sum += static_cast<std::int8_t>(b);
    if (h[i] != std::byte{0}) all_zero = false;
  }
  if (all_zero) return false;
  const auto stored = number(h, 148, 8);
  if (!stored) return false;
  return *stored == unsigned_sum || static_cast<std::int64_t>(*stored) == signed_sum;
}

Result<void> read_tar(ByteView data, EntrySink& sink, const Context& ctx, std::string_view prefix) {
  std::size_t pos = 0;
  std::map<std::string, std::string> global;
  std::map<std::string, std::string> local;
  Sparse sparse;
  std::optional<std::string> long_name;
  std::optional<std::string> long_link;
  std::map<std::string, std::size_t, std::less<>> files;  // sanitised path -> index of content holder
  std::vector<Bytes> contents;
  std::size_t steps = 0;
  bool any = false;
  while (pos + block <= data.size()) {
    if ((++steps & 255u) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(pos) / static_cast<double>(data.size())));
    const ByteView h = data.subspan(pos, block);
    if (std::all_of(h.begin(), h.end(), [](std::byte b) { return b == std::byte{0}; })) break;  // end of archive
    if (!is_tar_header(h)) {
      if (!any) return fail(Status::Decode, "not a tar archive (header checksum mismatch)");
      return fail(Status::Decode, "tar header at offset {} is damaged", pos);
    }
    any = true;
    pos += block;
    const char type = static_cast<char>(h[156]);
    const auto size_field = number(h, 124, 12);
    if (!size_field) return fail(Status::Decode, "tar entry size is damaged");
    std::uint64_t size = *size_field;
    const bool ustar_posix = as_chars(h.subspan(257, 6)) == std::string_view("ustar\0", 6);
    const bool gnu = as_chars(h.subspan(257, 8)) == std::string_view("ustar  \0", 8);
    // Old GNU sparse: stored size counts only the data blocks; extension headers follow.
    Sparse header_sparse;
    if (type == 'S' && gnu) {
      header_sparse.active = true;
      for (int i = 0; i < 4; ++i) {
        const auto o = number(h, 386 + static_cast<std::size_t>(i) * 24, 12);
        const auto n = number(h, 398 + static_cast<std::size_t>(i) * 24, 12);
        if (o && n && (*o != 0 || *n != 0)) header_sparse.map.emplace_back(*o, *n);
      }
      header_sparse.real_size = number(h, 483, 12).value_or(0);
      bool extended = h[482] != std::byte{0};
      while (extended) {
        if (pos + block > data.size()) return fail(Status::Decode, "tar sparse header is truncated");
        const ByteView x = data.subspan(pos, block);
        pos += block;
        for (int i = 0; i < 21; ++i) {
          const auto o = number(x, static_cast<std::size_t>(i) * 24, 12);
          const auto n = number(x, static_cast<std::size_t>(i) * 24 + 12, 12);
          if (o && n && (*o != 0 || *n != 0)) header_sparse.map.emplace_back(*o, *n);
        }
        extended = x[504] != std::byte{0};
        if (header_sparse.map.size() > 1000000) return fail(Status::Limit, "tar sparse map is too large");
      }
    }
    // pax overrides the size before we skip the data.
    if (const auto it = local.find("size"); it != local.end()) {
      if (const auto v = parse_u64(it->second)) size = *v;
    }
    if (size > data.size() - pos) return fail(Status::Decode, "tar entry data is truncated");
    const ByteView body = data.subspan(pos, static_cast<std::size_t>(size));
    pos += static_cast<std::size_t>((size + block - 1) / block * block);
    if (pos > data.size()) pos = data.size();

    switch (type) {
      case 'x':
        local.clear();
        CVT_TRY(parse_pax(body, local, sparse));
        continue;
      case 'g': {
        Sparse ignored;
        CVT_TRY(parse_pax(body, global, ignored));
        continue;
      }
      case 'L':
        long_name = std::string(field(body, 0, body.size()));
        continue;
      case 'K':
        long_link = std::string(field(body, 0, body.size()));
        continue;
      case 'V':
      case 'M':
      case 'N':
        local.clear();
        long_name.reset();
        long_link.reset();
        continue;
      default: break;
    }

    // Resolve the name.
    std::string name;
    if (const auto it = local.find("path"); it != local.end()) {
      name = it->second;
    } else if (long_name) {
      name = *long_name;
    } else {
      name = std::string(field(h, 0, 100));
      if (ustar_posix) {
        const std::string_view pre = field(h, 345, 155);
        if (!pre.empty()) name = std::string(pre) + "/" + name;
      }
    }
    std::string link;
    if (const auto it = local.find("linkpath"); it != local.end()) {
      link = it->second;
    } else if (long_link) {
      link = *long_link;
    } else {
      link = std::string(field(h, 157, 100));
    }
    std::int64_t mtime = static_cast<std::int64_t>(number(h, 136, 12).value_or(0));
    if (const auto it = local.find("mtime"); it != local.end()) {
      mtime = pax_time(it->second);
    } else if (const auto g = global.find("mtime"); g != global.end()) {
      mtime = pax_time(g->second);
    }
    const auto mode = static_cast<std::uint32_t>(number(h, 100, 8).value_or(0) & 07777u);
    Sparse entry_sparse = header_sparse.active ? header_sparse : sparse;
    local.clear();
    sparse = Sparse{};
    long_name.reset();
    long_link.reset();

    ir::ArchiveEntry entry;
    entry.path = std::string(prefix) + util::sanitize_utf8(name);
    entry.mtime = mtime;
    entry.unix_mode = mode;
    switch (type) {
      case '5':
      case 'D':
        entry.is_directory = true;
        break;
      case '2':
        if (link.empty()) continue;
        entry.symlink_target = util::sanitize_utf8(link);
        break;
      case '1': {
        // Hard link: a copy of the earlier entry's content.
        const auto target = ir::sanitize_archive_path(std::string(prefix) + util::sanitize_utf8(link));
        const auto found = target ? files.find(*target) : files.end();
        if (found == files.end()) {
          ctx.warn("A hard link to a file that is not in the archive was skipped.");
          continue;
        }
        CVT_TRY(sink.charge(contents[found->second].size()));
        entry.bytes = contents[found->second];
        break;
      }
      case '3':
      case '4':
      case '6':
        ctx.warn("Device files and FIFOs were skipped.");
        continue;
      default: {
        if (entry_sparse.active) {
          ByteView stored = body;
          if (entry_sparse.map_in_data) {
            // GNU sparse 1.0: the map is text at the start of the data, padded to a block.
            std::size_t p = 0;
            const auto line = [&]() -> std::optional<std::uint64_t> {
              const std::size_t nl = as_chars(stored.subspan(p)).find('\n');
              if (nl == std::string_view::npos) return std::nullopt;
              const auto v = parse_u64(as_chars(stored.subspan(p, nl)));
              p += nl + 1;
              return v;
            };
            const auto count = line();
            if (!count || *count > 1000000) return fail(Status::Decode, "tar sparse map is damaged");
            entry_sparse.map.clear();
            for (std::uint64_t i = 0; i < *count; ++i) {
              const auto o = line();
              const auto n = line();
              if (!o || !n) return fail(Status::Decode, "tar sparse map is damaged");
              entry_sparse.map.emplace_back(*o, *n);
            }
            p = (p + block - 1) / block * block;
            stored = p <= stored.size() ? stored.subspan(p) : ByteView{};
          }
          if (entry_sparse.real_size == 0) {
            for (const auto& [o, n] : entry_sparse.map) entry_sparse.real_size = std::max(entry_sparse.real_size, o + n);
          }
          CVT_TRY(sink.charge(entry_sparse.real_size));
          CVT_TRY_ASSIGN(entry.bytes, expand_sparse(stored, entry_sparse, sink.budget() + entry_sparse.real_size));
        } else {
          CVT_TRY(sink.charge(body.size()));
          entry.bytes.assign(body.begin(), body.end());
        }
        break;
      }
    }
    if (entry.is_file()) {
      if (const auto safe = ir::sanitize_archive_path(entry.path)) {
        files[*safe] = contents.size();
        contents.push_back(entry.bytes);
      }
    }
    CVT_TRY(sink.add(std::move(entry)));
  }
  if (!any && !data.empty() && data.size() < block) return fail(Status::Decode, "tar archive is truncated");
  return {};
}

// ------------------------------------------------------------------------------- writing

namespace {

void put_octal(Bytes& h, std::size_t at, std::size_t len, std::uint64_t v) {
  // len-1 octal digits and a NUL.
  std::string digits(len - 1, '0');
  for (std::size_t i = len - 1; i-- > 0;) {
    digits[i] = static_cast<char>('0' + (v & 7u));
    v >>= 3;
  }
  std::memcpy(h.data() + at, digits.data(), digits.size());
  h[at + len - 1] = std::byte{0};
}

bool fits_octal(std::uint64_t v, std::size_t len) { return len - 1 >= 22 || v < (std::uint64_t{1} << (3 * (len - 1))); }

void put_text(Bytes& h, std::size_t at, std::size_t len, std::string_view s) {
  std::memcpy(h.data() + at, s.data(), std::min(len, s.size()));
}

bool is_ascii(std::string_view s) {
  return std::all_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
}

// Split a path into ustar prefix and name, if possible.
std::optional<std::pair<std::string, std::string>> ustar_split(const std::string& path) {
  if (path.size() <= 100) return std::make_pair(std::string(), path);
  for (std::size_t slash = path.size(); slash-- > 0;) {
    if (path[slash] != '/') continue;
    if (path.size() - slash - 1 > 100) return std::nullopt;
    if (slash <= 155 && slash > 0) return std::make_pair(path.substr(0, slash), path.substr(slash + 1));
  }
  return std::nullopt;
}

std::string pax_record(std::string_view key, std::string_view value) {
  const std::size_t base = key.size() + value.size() + 3;  // ' ' '=' '\n'
  std::size_t len = base + 1;
  while (std::to_string(len).size() + base != len) len = std::to_string(len).size() + base;
  return std::format("{} {}={}\n", len, key, value);
}

Bytes header(std::string_view name, std::string_view prefix, std::uint32_t mode, std::uint64_t size, std::int64_t mtime,
             char type, std::string_view link, bool gnu_magic) {
  Bytes h(block, std::byte{0});
  put_text(h, 0, 100, name);
  put_octal(h, 100, 8, mode);
  put_octal(h, 108, 8, 0);
  put_octal(h, 116, 8, 0);
  if (fits_octal(size, 12)) {
    put_octal(h, 124, 12, size);
  } else {
    put_octal(h, 124, 12, 0);  // the real size is in a pax record
  }
  put_octal(h, 136, 12, mtime < 0 || !fits_octal(static_cast<std::uint64_t>(mtime), 12) ? 0 : static_cast<std::uint64_t>(mtime));
  h[156] = static_cast<std::byte>(type);
  put_text(h, 157, 100, link);
  if (gnu_magic) {
    put_text(h, 257, 8, std::string_view("ustar  \0", 8));
  } else {
    put_text(h, 257, 6, std::string_view("ustar\0", 6));
    put_text(h, 263, 2, "00");
    put_octal(h, 329, 8, 0);
    put_octal(h, 337, 8, 0);
    put_text(h, 345, 155, prefix);
  }
  std::fill(h.begin() + 148, h.begin() + 156, std::byte{' '});
  std::uint32_t sum = 0;
  for (const std::byte b : h) sum += std::to_integer<std::uint32_t>(b);
  std::string chk(6, '0');
  for (std::size_t i = 6; i-- > 0;) {
    chk[i] = static_cast<char>('0' + (sum & 7u));
    sum >>= 3;
  }
  put_text(h, 148, 6, chk);
  h[154] = std::byte{0};
  h[155] = std::byte{' '};
  return h;
}

void append_padded(Bytes& out, ByteView data) {
  append(out, data);
  out.insert(out.end(), (block - data.size() % block) % block, std::byte{0});
}

}  // namespace

namespace {

// Depth-first order: every directory is followed by its contents, siblings in
// their original order. GNU tar restores a directory's time as soon as it sees
// a member outside it, so contents listed later would change that time again.
std::vector<const WriteEntry*> depth_first(const std::vector<WriteEntry>& entries) {
  std::map<std::string, std::vector<std::size_t>, std::less<>> children;  // parent path -> child nodes
  std::map<std::string, std::size_t, std::less<>> dir_entry;              // directory path -> entry index
  std::map<std::string, std::vector<std::size_t>, std::less<>> files_at;  // node path -> other entries
  std::map<std::string, bool, std::less<>> known;
  std::vector<std::string> order;                                         // node paths, first seen first
  const auto add_node = [&](const std::string& path) {
    if (known.contains(path)) return;
    known[path] = true;
    const std::size_t slash = path.rfind('/');
    const std::string parent = slash == std::string::npos ? std::string() : path.substr(0, slash);
    order.push_back(path);
    children[parent].push_back(order.size() - 1);
  };
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const std::string& path = entries[i].path;
    for (std::size_t slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1)) {
      add_node(path.substr(0, slash));
    }
    add_node(path);
    if (entries[i].type == EntryType::Directory) {
      dir_entry[path] = i;
    } else {
      files_at[path].push_back(i);
    }
  }
  std::vector<const WriteEntry*> out;
  out.reserve(entries.size());
  std::vector<std::size_t> stack;
  const std::vector<std::size_t>& roots = children[std::string()];
  for (auto it = roots.rbegin(); it != roots.rend(); ++it) stack.push_back(*it);
  while (!stack.empty()) {
    const std::string path = order[stack.back()];
    stack.pop_back();
    if (const auto d = dir_entry.find(path); d != dir_entry.end()) out.push_back(&entries[d->second]);
    if (const auto f = files_at.find(path); f != files_at.end()) {
      for (const std::size_t i : f->second) out.push_back(&entries[i]);
    }
    if (const auto c = children.find(path); c != children.end()) {
      for (auto it = c->second.rbegin(); it != c->second.rend(); ++it) stack.push_back(*it);
    }
  }
  return out;
}

}  // namespace

Result<Bytes> write_tar(const std::vector<WriteEntry>& entries, TarFormat format, const Context& ctx) {
  Bytes out;
  // Progress by bytes (file contents dominate), each entry counted as one
  // block; the output is reserved up front instead of growing by copies.
  std::uint64_t total = 0;
  for (const WriteEntry& e : entries) total += block + (e.type == EntryType::File ? e.data().size() : 0);
  out.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(total + 4 * block, std::uint64_t{1} << 31)));
  const double span = static_cast<double>(std::max<std::uint64_t>(total, 1));
  std::uint64_t done = 0;
  std::size_t index = 0;
  for (const WriteEntry* entry : depth_first(entries)) {
    const WriteEntry& e = *entry;
    if ((++index & 255u) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(done) / span));
    done += block;
    std::string path = e.path;
    if (e.type == EntryType::Directory) path += '/';
    const char type = e.type == EntryType::Directory ? '5' : (e.type == EntryType::Symlink ? '2' : '0');
    const std::string link = e.type == EntryType::Symlink ? e.link_target() : std::string();
    const std::uint64_t size = e.type == EntryType::File ? e.data().size() : 0;
    std::string name = path;
    std::string prefix;
    bool need_path = false;
    if (const auto split = ustar_split(path)) {
      prefix = split->first;
      name = split->second;
    } else {
      need_path = true;
    }
    if (format == TarFormat::Gnu) {
      // GNU: long names in ././@LongLink entries, no prefix field.
      if (path.size() > 100) {
        Bytes data = to_bytes(path);
        data.push_back(std::byte{0});
        append(out, header("././@LongLink", "", 0644, data.size(), 0, 'L', "", true));
        append_padded(out, data);
      }
      if (link.size() > 100) {
        Bytes data = to_bytes(link);
        data.push_back(std::byte{0});
        append(out, header("././@LongLink", "", 0644, data.size(), 0, 'K', "", true));
        append_padded(out, data);
      }
      if (!fits_octal(size, 12)) return fail(Status::Encode, "GNU tar output here cannot hold files of 8 GiB or more");
      append(out, header(std::string_view(path).substr(0, 100), "", e.mode, size, e.mtime, type,
                         std::string_view(link).substr(0, 100), true));
    } else {
      const bool long_link = link.size() > 100;
      const bool big = !fits_octal(size, 12);
      const bool odd_time = e.mtime < 0 || !fits_octal(static_cast<std::uint64_t>(e.mtime), 12);
      if (format == TarFormat::Ustar && (need_path || long_link || big || odd_time)) {
        return fail(Status::Encode, "'{}' does not fit the ustar format (use pax)", e.path);
      }
      const bool non_ascii = !is_ascii(path) || !is_ascii(link);
      if (format == TarFormat::Pax && (need_path || long_link || big || odd_time || non_ascii)) {
        std::string records;
        if (need_path || non_ascii) records += pax_record("path", path);
        if (long_link || (!link.empty() && !is_ascii(link))) records += pax_record("linkpath", link);
        if (big) records += pax_record("size", std::to_string(size));
        if (odd_time) records += pax_record("mtime", std::to_string(e.mtime));
        std::string base = e.path.substr(e.path.rfind('/') == std::string::npos ? 0 : e.path.rfind('/') + 1);
        std::string pax_name = "PaxHeaders/" + base;
        if (pax_name.size() > 100 || !is_ascii(pax_name)) pax_name = "PaxHeaders/entry";
        append(out, header(pax_name, "", 0644, records.size(), e.mtime < 0 ? 0 : e.mtime, 'x', "", false));
        append_padded(out, as_bytes(records));
        if (need_path) {
          name = std::string(std::string_view(path).substr(0, 100));
          prefix.clear();
        }
      }
      append(out, header(name, prefix, e.mode, size, e.mtime, type, std::string_view(link).substr(0, 100), false));
    }
    if (e.type == EntryType::File) {
      const ByteView data = e.data();
      constexpr std::size_t piece = std::size_t{4} << 20;
      for (std::size_t at = 0; at < data.size(); at += piece) {
        CVT_TRY(ctx.checkpoint(static_cast<double>(done + at) / span));
        append(out, data.subspan(at, std::min(piece, data.size() - at)));
      }
      out.insert(out.end(), (block - data.size() % block) % block, std::byte{0});
      done += data.size();
    }
  }
  out.insert(out.end(), 2 * block, std::byte{0});
  const std::size_t record = 20 * block;
  out.insert(out.end(), (record - out.size() % record) % record, std::byte{0});
  return out;
}

// ------------------------------------------------------------------------------- formats

namespace {

TarFormat tar_format(const Options& options) {
  const std::string f = options.get_enum("tar_format", "pax", {"pax", "gnu", "ustar"});
  return f == "gnu" ? TarFormat::Gnu : (f == "ustar" ? TarFormat::Ustar : TarFormat::Pax);
}

OptionSpec tar_format_option() {
  return OptionSpec::choice("tar_format", "Tar format", "pax", {"pax", "gnu", "ustar"},
                            "Header flavour: pax (POSIX, default), GNU, or plain ustar (fails for names it cannot hold).")
      .advanced();
}

Result<ir::Archive> read_plain_tar(ReadContext& ctx) {
  EntrySink sink(ctx, ctx.input.size());
  CVT_TRY(read_tar(ctx.input, sink, ctx));
  return sink.finish();
}

Result<Bytes> write_plain_tar(const ir::Archive& archive, WriteContext& ctx) {
  const WritePolicy policy = resolve_write_policy(ctx);
  CVT_TRY_ASSIGN(const auto entries, prepare_entries(archive, policy, ctx, {.symlinks = true, .directories = true}));
  return write_tar(entries, tar_format(ctx.options), ctx);
}

struct Compound {
  std::string_view id;
  std::string_view name;
  compression::Method method;
  std::vector<std::string> extensions;
  std::string_view mime;
  int popularity;
};

// Map the archive policy level 0..9 onto the stream codec.
compression::EncodeOptions encode_options(const WritePolicy& policy, const Context& ctx) {
  compression::EncodeOptions e;
  e.level = policy.level;
  e.progress = ctx.progress;
  return e;
}

int sniff_compound(compression::Method method, ByteView data) {
  if (!compression::can_decode(method)) return 0;
  compression::DecodeOptions d;
  d.stop_after = block;
  d.max_output = 64u << 20;
  d.verify_checksums = false;
  const auto head = compression::decompress(method, data.first(std::min<std::size_t>(data.size(), 1u << 20)), d);
  if (!head || head->size() < block) return 0;
  return is_tar_header(*head) ? 93 : 0;
}

void register_compound(Registry& registry, const Compound& c) {
  ArchiveFormat f;
  f.info.id = std::string(c.id);
  f.info.name = std::string(c.name);
  f.info.description = std::format("A tar archive compressed with {}.", compression::method_name(c.method));
  f.info.category = Category::Archive;
  f.info.extensions = c.extensions;
  f.info.mime = std::string(c.mime);
  f.info.popularity = c.popularity;
  const compression::Method method = c.method;
  const std::vector<std::string> exts = c.extensions;
  f.info.sniff = [method, exts](std::span<const std::byte> data, std::string_view filename) {
    // The stream header first: decoding arbitrary data as LZMA, Brotli, ...
    // just to find out it is not a tar would cost every detection dearly.
    if (compression::detect(data) != method && method != compression::Method::Brotli) return 0;
    if (const int score = sniff_compound(method, data); score != 0) return score;
    // Too short (or not decodable here) to look inside: trust a matching name over the bare stream format.
    if (compression::detect(data) != method) return 0;
    for (const std::string& ext : exts) {
      if (filename.size() > ext.size() && util::iends_with(filename, "." + ext)) return 85;
    }
    return 0;
  };
  f.read = [method](ReadContext& ctx) -> Result<ir::Archive> {
    EntrySink sink(ctx, ctx.input.size());
    compression::DecodeOptions d;
    d.max_output = sink.budget();
    d.progress = ctx.progress.sub(0.0, 0.7);
    CVT_TRY_ASSIGN(const Bytes tar, compression::decompress(method, ctx.input, d));
    CVT_TRY(sink.charge(0));
    CVT_TRY(read_tar(tar, sink, ctx));
    return sink.finish();
  };
  f.read_options = {};
  if (compression::can_encode(method)) {
    f.write = [method](const ir::Archive& archive, WriteContext& ctx) -> Result<Bytes> {
      const WritePolicy policy = resolve_write_policy(ctx);
      CVT_TRY_ASSIGN(const auto entries, prepare_entries(archive, policy, ctx, {.symlinks = true, .directories = true}));
      CVT_TRY_ASSIGN(const Bytes tar, write_tar(entries, tar_format(ctx.options), ctx));
      return compression::compress(method, tar, encode_options(policy, ctx));
    };
    f.write_options = standard_write_options();
    f.write_options.push_back(tar_format_option());
  }
  f.read_cost = 12;
  f.write_cost = 14;
  register_archive_format(registry, std::move(f));
}

}  // namespace

void register_tar_formats(Registry& registry) {
  ArchiveFormat tar;
  tar.info.id = "tar";
  tar.info.name = "Tar archive";
  tar.info.description = "Unix tape archive (ustar, GNU or pax), uncompressed.";
  tar.info.category = Category::Archive;
  tar.info.extensions = {"tar"};
  tar.info.mime = "application/x-tar";
  tar.info.popularity = 75;
  tar.info.signatures = {Signature::at(257, "'ustar' 00"), Signature::at(257, "'ustar  ' 00")};
  tar.info.sniff = [](std::span<const std::byte> data, std::string_view) { return is_tar_header(data) ? 80 : 0; };
  tar.read = read_plain_tar;
  tar.write = write_plain_tar;
  tar.write_options = standard_write_options();
  tar.write_options.push_back(tar_format_option());
  register_archive_format(registry, std::move(tar));

  using compression::Method;
  const std::array<Compound, 10> compounds = {{
      {"tar.gz", "Gzip-compressed tar archive", Method::Gzip, {"tar.gz", "tgz"}, "application/gzip", 72},
      {"tar.bz2", "Bzip2-compressed tar archive", Method::Bzip2, {"tar.bz2", "tbz2", "tbz", "tb2"}, "application/x-bzip2", 50},
      {"tar.xz", "XZ-compressed tar archive", Method::Xz, {"tar.xz", "txz"}, "application/x-xz", 55},
      {"tar.zst", "Zstandard-compressed tar archive", Method::Zstd, {"tar.zst", "tzst"}, "application/zstd", 45},
      {"tar.lz4", "LZ4-compressed tar archive", Method::Lz4Frame, {"tar.lz4"}, "application/x-lz4", 25},
      {"tar.br", "Brotli-compressed tar archive", Method::Brotli, {"tar.br"}, "application/x-brotli", 10},
      {"tar.lz", "Lzip-compressed tar archive", Method::Lzip, {"tar.lz"}, "application/x-lzip", 15},
      {"tar.lzma", "LZMA-compressed tar archive", Method::LzmaAlone, {"tar.lzma", "tlz"}, "application/x-lzma", 15},
      {"tar.z", "Compress (.Z) tar archive", Method::Lzw, {"tar.z", "taz"}, "application/x-compress", 12},
      {"tar.sz", "Snappy-compressed tar archive", Method::SnappyFramed, {"tar.sz"}, "application/x-snappy-framed", 5},
  }};
  for (const Compound& c : compounds) register_compound(registry, c);
}

}  // namespace cvt::archive
