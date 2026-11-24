// Convertible core - archive family: ISO 9660 writer (level 3, Rock Ridge, Joliet).
//
// Layout: system area (16 empty sectors), primary and Joliet volume
// descriptors, terminator, four path tables (L/M for each tree), the
// primary directories, the Rock Ridge continuation areas, the Joliet
// directories, then the file data, each file starting on its own sector.
// Primary names are ISO 9660 identifiers (A-Z 0-9 _, up to 30 characters,
// made unique per directory); Rock Ridge (RRIP 1991A: PX, TF, NM, SL, CE,
// with SP/ER on the root) carries the real names, permissions, times and
// symbolic links; Joliet carries the names (up to 64 UCS-2 characters) for
// Windows. Files of 4 GiB and more use multi-extent records. Directories
// deeper than eight levels are written in place (no RR_MOVED relocation),
// which every current reader accepts. Nothing identifies the machine: the
// system, publisher, preparer and application fields are blank, owners are 0
// and the volume dates are the newest modification time of the content.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "families/archive/formats/iso_internal.hpp"
#include "util/text.hpp"

namespace cvt::archive::iso {

namespace {

constexpr std::uint64_t max_extent = 0xFFFFF800u;  // largest extent of one record, a multiple of the sector

void put_le16(Bytes& b, std::size_t at, std::uint32_t v) {
  b[at] = static_cast<std::byte>(v & 0xFFu);
  b[at + 1] = static_cast<std::byte>((v >> 8) & 0xFFu);
}
void put_be16(Bytes& b, std::size_t at, std::uint32_t v) {
  b[at] = static_cast<std::byte>((v >> 8) & 0xFFu);
  b[at + 1] = static_cast<std::byte>(v & 0xFFu);
}
void put_le32(Bytes& b, std::size_t at, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) b[at + static_cast<std::size_t>(i)] = static_cast<std::byte>((v >> (8 * i)) & 0xFFu);
}
void put_be32(Bytes& b, std::size_t at, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) b[at + static_cast<std::size_t>(3 - i)] = static_cast<std::byte>((v >> (8 * i)) & 0xFFu);
}
void put_both16(Bytes& b, std::size_t at, std::uint32_t v) {
  put_le16(b, at, v);
  put_be16(b, at + 2, v);
}
void put_both32(Bytes& b, std::size_t at, std::uint32_t v) {
  put_le32(b, at, v);
  put_be32(b, at + 4, v);
}
void put_text(Bytes& b, std::size_t at, std::size_t len, std::string_view s) {
  for (std::size_t i = 0; i < len; ++i) b[at + i] = static_cast<std::byte>(i < s.size() ? s[i] : ' ');
}
void put_ucs2(Bytes& b, std::size_t at, std::size_t len, std::u16string_view s) {
  for (std::size_t i = 0; i + 1 < len; i += 2) {
    const char16_t c = i / 2 < s.size() ? s[i / 2] : u' ';
    put_be16(b, at + i, c);
  }
}

std::array<std::byte, 7> record_date(std::int64_t t) {
  const CivilTime c = civil_from_unix(std::clamp<std::int64_t>(t, -2208988800LL, 5932915199LL));  // 1900..2157
  const std::int64_t year = std::clamp<std::int64_t>(c.year - 1900, 0, 255);
  return {static_cast<std::byte>(year),     static_cast<std::byte>(c.month),  static_cast<std::byte>(c.day),
          static_cast<std::byte>(c.hour),   static_cast<std::byte>(c.minute), static_cast<std::byte>(c.second),
          std::byte{0}};
}

std::string decimal_date(std::int64_t t) {
  const CivilTime c = civil_from_unix(std::clamp<std::int64_t>(t, 0, 253402300799LL));
  return std::format("{:04}{:02}{:02}{:02}{:02}{:02}00", c.year, c.month, c.day, c.hour, c.minute, c.second);
}

// ISO 9660 identifier comparison (ECMA-119 9.3): name, then extension, both space padded.
bool iso_less(const std::string& a, const std::string& b) {
  const auto split = [](const std::string& s) {
    const std::size_t semi = s.find(';');
    const std::string_view base = std::string_view(s).substr(0, semi);
    const std::size_t dot = base.find('.');
    return std::make_pair(base.substr(0, dot), dot == std::string_view::npos ? std::string_view() : base.substr(dot + 1));
  };
  const auto padded_less = [](std::string_view x, std::string_view y) {
    const std::size_t n = std::max(x.size(), y.size());
    for (std::size_t i = 0; i < n; ++i) {
      const char cx = i < x.size() ? x[i] : ' ';
      const char cy = i < y.size() ? y[i] : ' ';
      if (cx != cy) return static_cast<unsigned char>(cx) < static_cast<unsigned char>(cy);
    }
    return false;
  };
  const auto [an, ae] = split(a);
  const auto [bn, be] = split(b);
  if (an != bn) return padded_less(an, bn);
  return padded_less(ae, be);
}

std::string d_chars(std::string_view s) {
  std::string out;
  for (const char c : s) {
    const char u = util::ascii_upper(c);
    out += ((u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '_') ? u : '_';
  }
  return out;
}

// Primary identifier candidates: "NAME.EXT" (files, <= 30 chars) or "NAME" (directories, <= 31).
std::pair<std::string, std::string> iso_parts(const std::string& name, bool directory) {
  if (directory) return {d_chars(name).substr(0, 31), std::string()};
  const std::size_t dot = name.rfind('.');
  std::string base = d_chars(dot == std::string::npos || dot == 0 ? name : name.substr(0, dot));
  std::string ext = dot == std::string::npos || dot == 0 ? std::string() : d_chars(name.substr(dot + 1)).substr(0, 8);
  const std::size_t room = 30 - ext.size() - 1;
  if (base.size() > room) base.resize(room);
  if (base.empty() && ext.empty()) base = "_";
  return {base, ext};
}

std::u16string joliet_chars(const std::string& name) {
  std::u16string out;
  for (const char32_t c : util::utf8_to_utf32(name)) {
    if (c > 0xFFFF || c < 0x20 || c == u'*' || c == u'/' || c == u':' || c == u';' || c == u'?' || c == u'\\') {
      out.push_back(u'_');
    } else {
      out.push_back(static_cast<char16_t>(c));
    }
  }
  return out;
}

struct Node {
  std::string name;          // original UTF-8 name (Rock Ridge)
  std::string identifier;    // primary identifier with version for files
  std::u16string joliet;     // Joliet identifier
  const WriteEntry* entry = nullptr;  // nullptr: the root
  EntryType type = EntryType::Directory;
  std::size_t parent = 0;
  std::vector<std::size_t> children;   // sorted by primary identifier
  std::vector<std::size_t> jchildren;  // sorted by Joliet identifier (no symlinks)
  std::int64_t mtime = fixed_epoch;
  std::uint32_t mode = 0755;
  std::uint32_t subdirs = 0;
  // layout
  std::uint32_t extent = 0;
  std::uint32_t size = 0;
  std::uint32_t jextent = 0;
  std::uint32_t jsize = 0;
  std::uint32_t data = 0;
  std::uint16_t number = 0;
  std::uint16_t jnumber = 0;
};

// Rock Ridge continuation areas, packed into consecutive sectors.
struct Continuations {
  std::uint32_t base = 0;  // first sector (known in the second pass)
  std::vector<Bytes> sectors;
  // Place `area` (<= 2048 bytes); returns (absolute sector, offset).
  std::pair<std::uint32_t, std::uint32_t> place(const Bytes& area) {
    if (sectors.empty() || sectors.back().size() + area.size() > sector) sectors.emplace_back();
    Bytes& s = sectors.back();
    const auto offset = static_cast<std::uint32_t>(s.size());
    append(s, area);
    return {base + static_cast<std::uint32_t>(sectors.size() - 1), offset};
  }
};

Bytes su_entry(std::string_view sig, std::uint8_t version, ByteView body) {
  Bytes e;
  append(e, sig);
  e.push_back(static_cast<std::byte>(4 + body.size()));
  e.push_back(static_cast<std::byte>(version));
  append(e, body);
  return e;
}

Bytes ce_entry(std::uint32_t block, std::uint32_t offset, std::uint32_t length) {
  Bytes body(24);
  put_both32(body, 0, block);
  put_both32(body, 8, offset);
  put_both32(body, 16, length);
  return su_entry("CE", 1, body);
}

Bytes px_entry(std::uint32_t mode, std::uint32_t links) {
  Bytes body(32);
  put_both32(body, 0, mode);
  put_both32(body, 8, links);
  put_both32(body, 16, 0);
  put_both32(body, 24, 0);
  return su_entry("PX", 1, body);
}

Bytes tf_entry(std::int64_t mtime) {
  Bytes body;
  body.push_back(std::byte{0x0E});  // modify, access, attributes
  const auto d = record_date(mtime);
  for (int i = 0; i < 3; ++i) body.insert(body.end(), d.begin(), d.end());
  return su_entry("TF", 1, body);
}

void nm_entries(const std::string& name, std::vector<Bytes>& out) {
  std::size_t pos = 0;
  do {
    const std::size_t take = std::min<std::size_t>(250, name.size() - pos);
    Bytes body;
    body.push_back(pos + take < name.size() ? std::byte{1} : std::byte{0});
    append(body, std::string_view(name).substr(pos, take));
    out.push_back(su_entry("NM", 1, body));
    pos += take;
  } while (pos < name.size());
}

void sl_entries(const std::string& target, std::vector<Bytes>& out) {
  // Component records: flags, length, content.
  std::vector<Bytes> components;
  std::size_t pos = 0;
  if (!target.empty() && target.front() == '/') {
    components.push_back({std::byte{0x08}, std::byte{0}});
    pos = 1;
  }
  while (pos <= target.size()) {
    std::size_t slash = target.find('/', pos);
    if (slash == std::string::npos) slash = target.size();
    const std::string_view part = std::string_view(target).substr(pos, slash - pos);
    if (part == ".") {
      components.push_back({std::byte{0x02}, std::byte{0}});
    } else if (part == "..") {
      components.push_back({std::byte{0x04}, std::byte{0}});
    } else if (!part.empty()) {
      for (std::size_t at = 0; at < part.size(); at += 240) {
        const std::size_t take = std::min<std::size_t>(240, part.size() - at);
        Bytes c{at + take < part.size() ? std::byte{0x01} : std::byte{0}, static_cast<std::byte>(take)};
        append(c, part.substr(at, take));
        components.push_back(std::move(c));
      }
    }
    pos = slash + 1;
  }
  Bytes body{std::byte{0}};
  for (const Bytes& c : components) {
    if (body.size() + c.size() > 250) {
      body[0] = std::byte{1};  // continues in the next SL entry
      out.push_back(su_entry("SL", 1, body));
      body = {std::byte{0}};
    }
    append(body, c);
  }
  out.push_back(su_entry("SL", 1, body));
}

Bytes er_entry() {
  static constexpr std::string_view id = "RRIP_1991A";
  static constexpr std::string_view des = "THE ROCK RIDGE INTERCHANGE PROTOCOL PROVIDES SUPPORT FOR POSIX FILE SYSTEM SEMANTICS";
  static constexpr std::string_view src =
      "PLEASE CONTACT DISC PUBLISHER FOR SPECIFICATION SOURCE.  SEE PUBLISHER IDENTIFIER IN PRIMARY VOLUME "
      "DESCRIPTOR FOR CONTACT INFORMATION.";
  Bytes body{static_cast<std::byte>(id.size()), static_cast<std::byte>(des.size()), static_cast<std::byte>(src.size()),
             std::byte{1}};
  append(body, id);
  append(body, des);
  append(body, src);
  return su_entry("ER", 1, body);
}

class Writer {
 public:
  Writer(const std::vector<WriteEntry>& entries, const WriteContext& ctx, std::string label)
      : entries_(entries), ctx_(ctx), label_(std::move(label)) {}

  Result<Bytes> run() {
    CVT_TRY(build_tree());
    // Pass 1: sizes. Pass 2: final bytes.
    number_directories();
    for (int pass = 0; pass < 2; ++pass) {
      ce_ = Continuations{};
      ce_.base = ce_base_;
      for (const std::size_t d : order_) nodes_[d].size = static_cast<std::uint32_t>(primary_directory(d).size());
      for (const std::size_t d : jorder_) nodes_[d].jsize = static_cast<std::uint32_t>(joliet_directory(d).size());
      CVT_TRY(layout());
    }
    return assemble();
  }

 private:
  Result<void> build_tree() {
    nodes_.emplace_back();  // root
    std::map<std::string, std::size_t, std::less<>> by_path;
    std::size_t skipped = 0;
    for (const WriteEntry& e : entries_) {
      const std::size_t slash = e.path.rfind('/');
      std::size_t parent = 0;
      if (slash != std::string::npos) {
        const auto found = by_path.find(std::string_view(e.path).substr(0, slash));
        if (found == by_path.end() || nodes_[found->second].type != EntryType::Directory) {
          ++skipped;
          continue;
        }
        parent = found->second;
      }
      Node n;
      n.name = slash == std::string::npos ? e.path : e.path.substr(slash + 1);
      n.entry = &e;
      n.type = e.type;
      n.parent = parent;
      n.mtime = e.mtime;
      n.mode = e.mode;
      by_path[e.path] = nodes_.size();
      nodes_[parent].children.push_back(nodes_.size());
      nodes_.push_back(std::move(n));
    }
    if (skipped != 0) ctx_.warn(std::format("{} entr{} below a file instead of a directory were left out.", skipped, skipped == 1 ? "y" : "ies"));
    newest_ = fixed_epoch;
    bool any = false;
    for (const Node& n : nodes_) {
      if (n.entry == nullptr) continue;
      newest_ = any ? std::max(newest_, n.mtime) : n.mtime;
      any = true;
    }
    nodes_[0].mtime = newest_;
    // Identifiers, unique per directory, and sorted child lists.
    for (Node& dir : nodes_) {
      if (dir.type != EntryType::Directory) continue;
      std::set<std::string> used;
      std::set<std::u16string> jused;
      for (const std::size_t c : dir.children) {
        Node& child = nodes_[c];
        const bool is_dir = child.type == EntryType::Directory;
        if (is_dir) ++dir.subdirs;
        auto [base, ext] = iso_parts(child.name, is_dir);
        std::string id = ext.empty() ? base : base + "." + ext;
        for (int k = 1; used.contains(id); ++k) {
          const std::string suffix = std::format("_{}", k);
          const std::size_t limit = is_dir ? 31 : 30 - (ext.empty() ? 0 : ext.size() + 1);
          std::string b = base.substr(0, std::min(base.size(), limit - suffix.size()));
          id = b + suffix + (ext.empty() ? "" : "." + ext);
        }
        used.insert(id);
        child.identifier = is_dir ? id : (ext.empty() ? id + ".;1" : id + ";1");
        if (child.type == EntryType::Symlink) continue;
        std::u16string j = joliet_chars(child.name);
        if (j.size() > 64) j.resize(64);
        for (int k = 1; jused.contains(j); ++k) {
          const std::string suffix = std::format("~{}", k);
          std::u16string t = j.substr(0, 64 - suffix.size());
          for (const char c : suffix) t.push_back(static_cast<char16_t>(c));
          j = std::move(t);
        }
        jused.insert(j);
        child.joliet = std::move(j);
        dir.jchildren.push_back(c);
      }
      std::sort(dir.children.begin(), dir.children.end(),
                [&](std::size_t a, std::size_t b) { return iso_less(nodes_[a].identifier, nodes_[b].identifier); });
      std::sort(dir.jchildren.begin(), dir.jchildren.end(),
                [&](std::size_t a, std::size_t b) { return nodes_[a].joliet < nodes_[b].joliet; });
    }
    return {};
  }

  // Breadth-first order = path table order (level, parent number, name).
  void number_directories() {
    order_ = {0};
    for (std::size_t i = 0; i < order_.size(); ++i) {
      for (const std::size_t c : nodes_[order_[i]].children) {
        if (nodes_[c].type == EntryType::Directory) order_.push_back(c);
      }
    }
    jorder_ = {0};
    for (std::size_t i = 0; i < jorder_.size(); ++i) {
      for (const std::size_t c : nodes_[jorder_[i]].jchildren) {
        if (nodes_[c].type == EntryType::Directory) jorder_.push_back(c);
      }
    }
    for (std::size_t i = 0; i < order_.size(); ++i) nodes_[order_[i]].number = static_cast<std::uint16_t>(i + 1);
    for (std::size_t i = 0; i < jorder_.size(); ++i) nodes_[jorder_[i]].jnumber = static_cast<std::uint16_t>(i + 1);
  }

  Bytes path_table(bool joliet, bool big_endian) const {
    Bytes out;
    for (const std::size_t d : joliet ? jorder_ : order_) {
      const Node& n = nodes_[d];
      Bytes id;
      if (d == 0) {
        id = {std::byte{0}};
      } else if (joliet) {
        for (const char16_t c : n.joliet) {
          id.push_back(static_cast<std::byte>(c >> 8));
          id.push_back(static_cast<std::byte>(c & 0xFFu));
        }
      } else {
        append(id, n.identifier);
      }
      Bytes r(8);
      r[0] = static_cast<std::byte>(id.size());
      const std::uint32_t extent = joliet ? n.jextent : n.extent;
      const std::uint32_t parent = d == 0 ? 1 : (joliet ? nodes_[n.parent].jnumber : nodes_[n.parent].number);
      if (big_endian) {
        put_be32(r, 2, extent);
        put_be16(r, 6, parent);
      } else {
        put_le32(r, 2, extent);
        put_le16(r, 6, parent);
      }
      append(r, id);
      if ((id.size() & 1u) != 0) r.push_back(std::byte{0});
      append(out, r);
    }
    return out;
  }

  static std::uint32_t sectors_for(std::uint64_t bytes) { return static_cast<std::uint32_t>((bytes + sector - 1) / sector); }

  // A directory record; `su` entries that do not fit go to continuation areas.
  Bytes record(ByteView id, std::uint32_t extent, std::uint32_t size, std::uint8_t flags, std::int64_t mtime,
               const std::vector<Bytes>& su) {
    Bytes r(33);
    put_both32(r, 2, extent);
    put_both32(r, 10, size);
    const auto date = record_date(mtime);
    std::copy(date.begin(), date.end(), r.begin() + 18);
    r[25] = static_cast<std::byte>(flags);
    put_both16(r, 28, 1);
    r[32] = static_cast<std::byte>(id.size());
    append(r, id);
    if ((id.size() & 1u) == 0) r.push_back(std::byte{0});
    std::size_t total = 0;
    for (const Bytes& e : su) total += e.size();
    if (r.size() + total <= 255) {
      for (const Bytes& e : su) append(r, e);
    } else {
      std::size_t i = 0;
      while (i < su.size() && r.size() + su[i].size() + 28 <= 255) append(r, su[i++]);
      // The rest in continuation areas of at most one sector each, chained by CE.
      std::vector<Bytes> areas{Bytes{}};
      for (; i < su.size(); ++i) {
        if (areas.back().size() + su[i].size() + 28 > sector) areas.emplace_back();
        append(areas.back(), su[i]);
      }
      // Place from the last so that each area can point to the next.
      std::vector<std::pair<std::uint32_t, std::uint32_t>> where(areas.size());
      std::vector<Bytes> finished(areas.size());
      // Reserve positions in order (CE entries make every non-last area 28 bytes longer).
      for (std::size_t a = 0; a < areas.size(); ++a) {
        Bytes placeholder(areas[a].size() + (a + 1 < areas.size() ? 28 : 0));
        where[a] = ce_.place(placeholder);
      }
      for (std::size_t a = areas.size(); a-- > 0;) {
        finished[a] = areas[a];
        if (a + 1 < areas.size()) {
          append(finished[a], ce_entry(where[a + 1].first, where[a + 1].second, static_cast<std::uint32_t>(finished[a + 1].size())));
        }
        Bytes& target = ce_.sectors[where[a].first - ce_.base];
        std::copy(finished[a].begin(), finished[a].end(), target.begin() + where[a].second);
      }
      append(r, ce_entry(where[0].first, where[0].second, static_cast<std::uint32_t>(finished[0].size())));
    }
    r[0] = static_cast<std::byte>(r.size());
    return r;
  }

  std::uint32_t posix_mode(const Node& n) const {
    const std::uint32_t type = n.type == EntryType::Directory ? 040000u : (n.type == EntryType::Symlink ? 0120000u : 0100000u);
    return type | (n.mode & 07777u);
  }

  std::vector<Bytes> rock_ridge(const Node& n, bool with_name) const {
    std::vector<Bytes> su;
    su.push_back(px_entry(posix_mode(n), n.type == EntryType::Directory ? 2 + n.subdirs : 1));
    su.push_back(tf_entry(n.mtime));
    if (with_name) nm_entries(n.name, su);
    if (n.type == EntryType::Symlink) sl_entries(n.entry->link_target(), su);
    return su;
  }

  // Append a record to a directory listing, never across a sector boundary.
  static void put_record(Bytes& listing, const Bytes& r) {
    const std::size_t used = listing.size() % sector;
    if (used + r.size() > sector) listing.insert(listing.end(), sector - used, std::byte{0});
    append(listing, r);
  }

  static void pad_sector(Bytes& b) { b.insert(b.end(), (sector - b.size() % sector) % sector, std::byte{0}); }

  Bytes primary_directory(std::size_t d) {
    const Node& dir = nodes_[d];
    const Node& parent = nodes_[d == 0 ? 0 : dir.parent];
    Bytes listing;
    std::vector<Bytes> dot_su;
    if (d == 0) {
      dot_su.push_back(su_entry("SP", 1, Bytes{std::byte{0xBE}, std::byte{0xEF}, std::byte{0}}));
      dot_su.push_back(er_entry());  // too long for the record: goes to a continuation area
    }
    for (Bytes& e : rock_ridge(dir, false)) dot_su.insert(dot_su.end() - (d == 0 ? 1 : 0), std::move(e));
    put_record(listing, record(Bytes{std::byte{0}}, dir.extent, dir.size, 0x02, dir.mtime, dot_su));
    put_record(listing, record(Bytes{std::byte{1}}, parent.extent, parent.size, 0x02, parent.mtime, rock_ridge(parent, false)));
    for (const std::size_t c : dir.children) {
      const Node& n = nodes_[c];
      const ByteView id = as_bytes(n.identifier);
      const std::vector<Bytes> su = rock_ridge(n, true);
      if (n.type == EntryType::Directory) {
        put_record(listing, record(id, n.extent, n.size, 0x02, n.mtime, su));
      } else if (n.type == EntryType::Symlink) {
        put_record(listing, record(id, 0, 0, 0x00, n.mtime, su));
      } else {
        file_records(listing, id, n, su);
      }
    }
    pad_sector(listing);
    return listing;
  }

  void file_records(Bytes& listing, ByteView id, const Node& n, const std::vector<Bytes>& su) {
    const std::uint64_t size = n.entry->data().size();
    if (size == 0) {
      put_record(listing, record(id, 0, 0, 0x00, n.mtime, su));
      return;
    }
    std::uint64_t done = 0;
    std::uint32_t extent = n.data;
    while (done < size) {
      const std::uint64_t part = std::min(max_extent, size - done);
      const bool last = done + part == size;
      put_record(listing, record(id, extent, static_cast<std::uint32_t>(part), last ? 0x00 : 0x80, n.mtime, su));
      extent += sectors_for(part);
      done += part;
    }
  }

  Bytes joliet_directory(std::size_t d) {
    const Node& dir = nodes_[d];
    const Node& parent = nodes_[d == 0 ? 0 : dir.parent];
    Bytes listing;
    put_record(listing, record(Bytes{std::byte{0}}, dir.jextent, dir.jsize, 0x02, dir.mtime, {}));
    put_record(listing, record(Bytes{std::byte{1}}, parent.jextent, parent.jsize, 0x02, parent.mtime, {}));
    for (const std::size_t c : dir.jchildren) {
      const Node& n = nodes_[c];
      Bytes id;
      for (const char16_t ch : n.joliet) {
        id.push_back(static_cast<std::byte>(ch >> 8));
        id.push_back(static_cast<std::byte>(ch & 0xFFu));
      }
      if (n.type == EntryType::Directory) {
        put_record(listing, record(id, n.jextent, n.jsize, 0x02, n.mtime, {}));
      } else {
        file_records(listing, id, n, {});
      }
    }
    pad_sector(listing);
    return listing;
  }

  Result<void> layout() {
    if (order_.size() > 65535 || jorder_.size() > 65535) {
      return fail(Status::Encode, "ISO 9660 holds at most 65535 directories");
    }
    path_size_ = static_cast<std::uint32_t>(path_table(false, false).size());
    jpath_size_ = static_cast<std::uint32_t>(path_table(true, false).size());
    std::uint64_t next = 19;
    path_at_[0] = static_cast<std::uint32_t>(next);
    next += sectors_for(path_size_);
    path_at_[1] = static_cast<std::uint32_t>(next);
    next += sectors_for(path_size_);
    path_at_[2] = static_cast<std::uint32_t>(next);
    next += sectors_for(jpath_size_);
    path_at_[3] = static_cast<std::uint32_t>(next);
    next += sectors_for(jpath_size_);
    for (const std::size_t d : order_) {
      nodes_[d].extent = static_cast<std::uint32_t>(next);
      next += sectors_for(nodes_[d].size);
    }
    ce_base_ = static_cast<std::uint32_t>(next);
    next += ce_.sectors.size();
    for (const std::size_t d : jorder_) {
      nodes_[d].jextent = static_cast<std::uint32_t>(next);
      next += sectors_for(nodes_[d].jsize);
    }
    for (Node& n : nodes_) {
      if (n.type != EntryType::File || n.entry->data().empty()) continue;
      n.data = static_cast<std::uint32_t>(next);
      next += sectors_for(n.entry->data().size());
      if (next > 0xFFFFFFFFu) return fail(Status::Encode, "the ISO image would exceed 8 TiB");
    }
    total_ = next;
    CVT_TRY(ctx_.limits.check_output(total_ * sector));
    if (total_ * sector > std::numeric_limits<std::size_t>::max()) return fail(Status::Limit, "the ISO image is too large for this platform");
    return {};
  }

  Bytes descriptor(bool joliet) const {
    Bytes d(sector, std::byte{0});
    d[0] = static_cast<std::byte>(joliet ? 2 : 1);
    put_text(d, 1, 5, "CD001");
    d[6] = std::byte{1};
    if (joliet) {
      put_ucs2(d, 8, 32, u"");
      std::u16string label = joliet_chars(label_);
      if (label.size() > 16) label.resize(16);
      put_ucs2(d, 40, 32, label);
      d[88] = std::byte{'%'};
      d[89] = std::byte{'/'};
      d[90] = std::byte{'E'};
      for (const std::size_t at : {std::size_t{190}, std::size_t{318}, std::size_t{446}, std::size_t{574}}) put_ucs2(d, at, 128, u"");
      for (const std::size_t at : {std::size_t{702}, std::size_t{739}, std::size_t{776}}) put_ucs2(d, at, 37, u"");
    } else {
      put_text(d, 8, 32, "");
      std::string label = d_chars(label_).substr(0, 32);
      put_text(d, 40, 32, label);
      for (const std::size_t at : {std::size_t{190}, std::size_t{318}, std::size_t{446}, std::size_t{574}}) put_text(d, at, 128, "");
      for (const std::size_t at : {std::size_t{702}, std::size_t{739}, std::size_t{776}}) put_text(d, at, 37, "");
    }
    put_both32(d, 80, static_cast<std::uint32_t>(total_));
    put_both16(d, 120, 1);
    put_both16(d, 124, 1);
    put_both16(d, 128, static_cast<std::uint32_t>(sector));
    put_both32(d, 132, joliet ? jpath_size_ : path_size_);
    put_le32(d, 140, path_at_[joliet ? 2 : 0]);
    put_be32(d, 148, path_at_[joliet ? 3 : 1]);
    const Node& root = nodes_[0];
    Bytes r(34);
    r[0] = std::byte{34};
    put_both32(r, 2, joliet ? root.jextent : root.extent);
    put_both32(r, 10, joliet ? root.jsize : root.size);
    const auto date = record_date(root.mtime);
    std::copy(date.begin(), date.end(), r.begin() + 18);
    r[25] = std::byte{0x02};
    put_both16(r, 28, 1);
    r[32] = std::byte{1};
    std::copy(r.begin(), r.end(), d.begin() + 156);
    const std::string when = decimal_date(newest_);
    put_text(d, 813, 16, when);
    put_text(d, 830, 16, when);
    put_text(d, 847, 16, "0000000000000000");
    put_text(d, 864, 16, "0000000000000000");
    d[881] = std::byte{1};
    return d;
  }

  Result<Bytes> assemble() {
    Bytes out;
    out.reserve(static_cast<std::size_t>(total_ * sector));
    out.resize(16 * sector, std::byte{0});
    append(out, descriptor(false));
    append(out, descriptor(true));
    Bytes term(sector, std::byte{0});
    term[0] = std::byte{255};
    put_text(term, 1, 5, "CD001");
    term[6] = std::byte{1};
    append(out, term);
    for (int t = 0; t < 4; ++t) {
      Bytes table = path_table(t >= 2, (t & 1) != 0);
      pad_sector(table);
      append(out, table);
    }
    ce_ = Continuations{};
    ce_.base = ce_base_;
    for (const std::size_t d : order_) append(out, primary_directory(d));
    for (Bytes& s : ce_.sectors) {
      pad_sector(s);
      append(out, s);
    }
    for (const std::size_t d : jorder_) append(out, joliet_directory(d));
    std::size_t done = 0;
    for (const Node& n : nodes_) {
      if (n.type != EntryType::File || n.entry->data().empty()) continue;
      if (out.size() != static_cast<std::size_t>(n.data) * sector) return fail(Status::Internal, "ISO layout mismatch");
      append(out, n.entry->data());
      pad_sector(out);
      if ((++done & 255u) == 0) CVT_TRY(ctx_.checkpoint(static_cast<double>(out.size()) / static_cast<double>(total_ * sector)));
    }
    if (out.size() != total_ * sector) return fail(Status::Internal, "ISO layout mismatch");
    return out;
  }

  const std::vector<WriteEntry>& entries_;
  const WriteContext& ctx_;
  std::string label_;
  std::vector<Node> nodes_;
  std::vector<std::size_t> order_;
  std::vector<std::size_t> jorder_;
  Continuations ce_;
  std::uint32_t ce_base_ = 0;
  std::uint32_t path_size_ = 0;
  std::uint32_t jpath_size_ = 0;
  std::array<std::uint32_t, 4> path_at_{};
  std::uint64_t total_ = 0;
  std::int64_t newest_ = fixed_epoch;
};

}  // namespace

Result<Bytes> write_iso(const ir::Archive& archive, WriteContext& ctx) {
  const WritePolicy policy = resolve_write_policy(ctx);
  CVT_TRY_ASSIGN(const auto entries,
                 prepare_entries(archive, policy, ctx, {.symlinks = true, .directories = true, .add_parent_directories = true}));
  std::string label = ctx.options.get_string("volume_label", "CDROM");
  if (label.empty()) label = "CDROM";
  Writer writer(entries, ctx, std::move(label));
  return writer.run();
}

}  // namespace cvt::archive::iso
