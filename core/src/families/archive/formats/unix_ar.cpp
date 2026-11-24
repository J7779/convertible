// Convertible core - archive family: ar archives (GNU and BSD variants).
//
// "!<arch>\n" followed by members, each a 60-byte text header
//   name[16] mtime[12] uid[6] gid[6] mode[8] (octal) size[10] "`\n"
// and its data padded to an even size with '\n'.
//   GNU: names end with '/'; "/" (and "/SYM64/") is the symbol table, "//"
//        holds long names ("name/\n" each) referenced as "/offset".
//   BSD: "#1/len" means the name is the first `len` bytes of the data;
//        "__.SYMDEF" members are symbol tables.
// Thin archives ("!<thin>\n") only reference files outside the archive and
// are refused. ar has no directories: the writer stores each file under its
// base name (with a warning when that drops directories) and leaves out
// directories and symbolic links.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstring>
#include <format>
#include <map>

#include "families/archive/formats/unix_internal.hpp"
#include "util/text.hpp"

namespace cvt::archive::unixfmt {

namespace {

constexpr std::size_t header_size = 60;

std::string_view trim_spaces(std::string_view s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.remove_suffix(1);
  return s;
}

}  // namespace

Result<std::vector<ArMember>> parse_ar(ByteView data, const Context& ctx) {
  if (data.size() >= 8 && as_chars(data.first(8)) == "!<thin>\n") {
    return fail(Status::Decode, "thin ar archives only refer to files outside the archive and cannot be read");
  }
  if (data.size() < 8 || as_chars(data.first(8)) != "!<arch>\n") return fail(Status::Decode, "not an ar archive");
  std::vector<ArMember> members;
  std::string_view long_names;
  std::size_t pos = 8;
  std::size_t steps = 0;
  while (pos < data.size()) {
    if ((++steps & 255u) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(pos) / static_cast<double>(data.size())));
    // Some writers pad with an extra newline.
    if (data[pos] == std::byte{'\n'} && data.size() - pos < header_size) break;
    if (data.size() - pos < header_size) return fail(Status::Decode, "ar archive is truncated");
    const std::string_view h = as_chars(data.subspan(pos, header_size));
    if (h.substr(58, 2) != "`\n") return fail(Status::Decode, "ar member header at offset {} is damaged", pos);
    const auto size = parse_field(h.substr(48, 10), 10);
    if (!size) return fail(Status::Decode, "ar member size is damaged");
    pos += header_size;
    if (*size > data.size() - pos) return fail(Status::Decode, "ar member data is truncated");
    ByteView body = data.subspan(pos, static_cast<std::size_t>(*size));
    pos += static_cast<std::size_t>(*size);
    if ((*size & 1u) != 0 && pos < data.size()) ++pos;

    std::string_view raw = trim_spaces(h.substr(0, 16));
    std::string name;
    if (raw == "/" || raw == "/SYM64/" || raw == "__.SYMDEF" || raw == "__.SYMDEF SORTED" || raw == "__.SYMDEF_64" ||
        raw == "__.SYMDEF_64 SORTED") {
      continue;  // symbol tables
    }
    if (raw == "//") {
      long_names = as_chars(body);
      continue;
    }
    if (raw.starts_with("#1/")) {
      const auto len = parse_field(raw.substr(3), 10);
      if (!len || *len > body.size()) return fail(Status::Decode, "BSD ar long name is damaged");
      std::string_view stored = as_chars(body.first(static_cast<std::size_t>(*len)));
      if (const std::size_t nul = stored.find('\0'); nul != std::string_view::npos) stored = stored.substr(0, nul);
      name = std::string(stored);
      body = body.subspan(static_cast<std::size_t>(*len));
      if (name == "__.SYMDEF" || name == "__.SYMDEF SORTED" || name == "__.SYMDEF_64" || name == "__.SYMDEF_64 SORTED") {
        continue;
      }
    } else if (raw.size() > 1 && raw.front() == '/' && util::is_ascii_digit(raw[1])) {
      const auto offset = parse_field(raw.substr(1), 10);
      if (!offset || *offset >= long_names.size()) return fail(Status::Decode, "GNU ar long name reference is damaged");
      std::string_view stored = long_names.substr(static_cast<std::size_t>(*offset));
      const std::size_t end = stored.find('\n');
      stored = stored.substr(0, end);
      if (stored.ends_with('/')) stored.remove_suffix(1);
      name = std::string(stored);
    } else {
      if (raw.ends_with('/')) raw.remove_suffix(1);
      name = std::string(raw);
    }
    ArMember m;
    m.name = std::move(name);
    m.data = body;
    m.mtime = static_cast<std::int64_t>(parse_field(h.substr(16, 12), 10).value_or(0));
    m.mode = static_cast<std::uint32_t>(parse_field(h.substr(40, 8), 8).value_or(0) & 07777u);
    CVT_TRY(ctx.limits.check_archive_entries(members.size() + 1));
    members.push_back(std::move(m));
  }
  return members;
}

Result<ir::Archive> read_ar(ReadContext& ctx) {
  EntrySink sink(ctx, ctx.input.size());
  CVT_TRY_ASSIGN(const std::vector<ArMember> members, parse_ar(ctx.input, ctx));
  for (const ArMember& m : members) {
    ir::ArchiveEntry e;
    e.path = decode_name(m.name, false, false);
    e.mtime = m.mtime;
    e.unix_mode = m.mode;
    CVT_TRY(sink.charge(m.data.size()));
    e.bytes.assign(m.data.begin(), m.data.end());
    CVT_TRY(sink.add(std::move(e)));
  }
  return sink.finish();
}

// ------------------------------------------------------------------------------- writing

namespace {

void put_field(std::string& h, std::string_view value, std::size_t width) {
  h += value.substr(0, width);
  h.append(width - std::min(width, value.size()), ' ');
}

}  // namespace

Result<Bytes> write_ar(const ir::Archive& archive, WriteContext& ctx) {
  const WritePolicy policy = resolve_write_policy(ctx);
  CVT_TRY_ASSIGN(const auto entries, prepare_entries(archive, policy, ctx, {.symlinks = false, .directories = false}));
  struct Member {
    std::string name;
    const WriteEntry* entry;
  };
  std::vector<Member> members;
  std::string long_names;
  std::size_t flattened = 0;
  for (const WriteEntry& e : entries) {
    std::string base = e.path.substr(e.path.rfind('/') == std::string::npos ? 0 : e.path.rfind('/') + 1);
    if (base.size() != e.path.size()) ++flattened;
    members.push_back({std::move(base), &e});
  }
  if (flattened != 0) {
    ctx.warn(std::format("ar archives have no directories: {} file(s) were stored under their base names.", flattened));
  }
  // Names that do not fit "name/" in 16 bytes (or contain spaces) go to the "//" table.
  std::vector<std::string> fields;
  for (const Member& m : members) {
    if (m.name.size() <= 15 && m.name.find(' ') == std::string::npos) {
      fields.push_back(m.name + "/");
    } else {
      fields.push_back("/" + std::to_string(long_names.size()));
      long_names += m.name + "/\n";
    }
  }
  Bytes out = to_bytes("!<arch>\n");
  if (!long_names.empty()) {
    if (long_names.size() % 2 != 0) long_names += '\n';
    std::string h;
    put_field(h, "//", 48);
    put_field(h, std::to_string(long_names.size()), 10);
    h += "`\n";
    append(out, h);
    append(out, long_names);
  }
  std::size_t index = 0;
  for (std::size_t i = 0; i < members.size(); ++i) {
    if ((++index & 255u) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(index) / static_cast<double>(members.size())));
    const WriteEntry& e = *members[i].entry;
    const ByteView body = e.data();
    if (body.size() > 9999999999ull) return fail(Status::Encode, "'{}' is too large for an ar archive", e.path);
    std::string h;
    put_field(h, fields[i], 16);
    put_field(h, std::to_string(std::clamp<std::int64_t>(e.mtime, 0, 999999999999ll)), 12);
    put_field(h, "0", 6);
    put_field(h, "0", 6);
    put_field(h, std::format("{:o}", 0100000u | (e.mode & 07777u)), 8);
    put_field(h, std::to_string(body.size()), 10);
    h += "`\n";
    append(out, h);
    append(out, body);
    if (body.size() % 2 != 0) out.push_back(std::byte{'\n'});
  }
  return out;
}

}  // namespace cvt::archive::unixfmt
