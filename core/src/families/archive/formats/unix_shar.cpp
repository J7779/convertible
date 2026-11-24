// Convertible core - archive family: shell archives (shar), write only.
//
// The output is a POSIX sh script that recreates the files in the current
// directory when run with `sh archive.shar`:
//   * directories: mkdir -p (and chmod when the mode is not 755);
//   * text files (no NUL bytes, ending with a newline, lines of at most
//     4000 bytes): a here-document with a quoted delimiter (so the shell
//     expands nothing) whose lines carry an 'X' prefix that
//     `LC_ALL=C sed 's/^X//'` removes, so no line can end the document early;
//   * other files: base64 in a here-document, decoded with `base64 -d`;
//   * empty files: `: > file`; symbolic links: `ln -sf`;
//   * permissions with chmod. Modification times are not restored.
// Every path is single-quoted and starts with "./", so no name can be taken
// for an option or expanded. The script is deterministic.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <format>
#include <set>

#include "families/archive/formats/unix_internal.hpp"
#include "util/base64.hpp"

namespace cvt::archive::unixfmt {

namespace {

constexpr std::string_view delimiter = "SHAR_EOF";

std::string quote(std::string_view s) {
  std::string out = "'";
  for (const char c : s) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += '\'';
  return out;
}

std::string quote_path(std::string_view path) { return quote(std::string("./") + std::string(path)); }

bool is_text(ByteView data) {
  if (data.empty() || data.back() != std::byte{'\n'}) return false;
  std::size_t line = 0;
  for (const std::byte b : data) {
    if (b == std::byte{0}) return false;
    if (b == std::byte{'\n'}) {
      line = 0;
    } else if (++line > 4000) {
      return false;
    }
  }
  return true;
}

}  // namespace

Result<Bytes> write_shar(const ir::Archive& archive, WriteContext& ctx) {
  const WritePolicy policy = resolve_write_policy(ctx);
  CVT_TRY_ASSIGN(const auto entries, prepare_entries(archive, policy, ctx, {.symlinks = true, .directories = true}));
  std::string out =
      "#!/bin/sh\n"
      "# This is a shell archive. Run it with \"sh <file>\" in an empty directory to extract its files.\n"
      "# It needs only a POSIX shell, sed, base64, mkdir, chmod and ln.\n";
  std::set<std::string, std::less<>> made;
  const auto ensure_parent = [&](std::string_view path) {
    const std::size_t slash = path.rfind('/');
    if (slash == std::string_view::npos) return;
    const std::string_view parent = path.substr(0, slash);
    if (made.contains(parent)) return;
    out += std::format("mkdir -p {}\n", quote_path(parent));
    // Every ancestor exists now.
    for (std::size_t s = parent.find('/'); s != std::string_view::npos; s = parent.find('/', s + 1)) {
      made.emplace(parent.substr(0, s));
    }
    made.emplace(parent);
  };
  std::size_t index = 0;
  for (const WriteEntry& e : entries) {
    if ((++index & 255u) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(index) / static_cast<double>(entries.size())));
    const std::string path = quote_path(e.path);
    switch (e.type) {
      case EntryType::Directory:
        ensure_parent(e.path);
        if (!made.contains(e.path)) out += std::format("mkdir -p {}\n", path);
        made.emplace(e.path);
        if (e.mode != 0755) out += std::format("chmod {:o} {}\n", e.mode, path);
        break;
      case EntryType::Symlink:
        ensure_parent(e.path);
        out += std::format("ln -sf {} {}\n", quote(e.link_target()), path);
        break;
      case EntryType::File: {
        ensure_parent(e.path);
        const ByteView data = e.data();
        if (data.empty()) {
          out += std::format(": > {}\n", path);
        } else if (is_text(data)) {
          out += std::format("LC_ALL=C sed 's/^X//' > {} << '{}'\n", path, delimiter);
          const std::string_view text = as_chars(data);
          std::size_t start = 0;
          while (start < text.size()) {
            const std::size_t end = text.find('\n', start);
            out += 'X';
            out += text.substr(start, end - start + 1);
            start = end + 1;
          }
          out += delimiter;
          out += '\n';
        } else {
          out += std::format("base64 -d > {} << '{}'\n", path, delimiter);
          out += util::base64_encode(data, {.url_safe = false, .padding = true, .line_width = 76, .newline = "\n"});
          out += '\n';
          out += delimiter;
          out += '\n';
        }
        out += std::format("chmod {:o} {}\n", e.mode, path);
        break;
      }
    }
  }
  out += "exit 0\n";
  return to_bytes(out);
}

}  // namespace cvt::archive::unixfmt
