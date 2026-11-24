// The synthetic file tree that the archive fixtures are made from
// (core/tests/data/archive/make_tree.py builds it on disk; this header builds
// the same content in memory), and a checker that compares a decoded archive
// with it entry by entry: path, kind, exact bytes, mode, mtime, link target.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "support/test.hpp"

#include "ir/archive.hpp"
#include "util/bytes.hpp"

namespace cvt::test::tree {

inline constexpr std::int64_t mtime = 1623758400;  // 2021-06-15T12:00:00Z

inline const std::string& lorem_paragraph() {
  static const std::string text =
      "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore "
      "et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut "
      "aliquip ex ea commodo consequat. Duis aute irure dolor in reprehenderit in voluptate velit esse cillum "
      "dolore eu fugiat nulla pariatur. Excepteur sint occaecat cupidatat non proident, sunt in culpa qui "
      "officia deserunt mollit anim id est laborum.\n";
  return text;
}

/// The xorshift32 noise of make_tree.py.
inline Bytes noise(std::size_t count, std::uint32_t seed = 2463534242u) {
  Bytes out(count);
  std::uint32_t state = seed;
  for (std::byte& b : out) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    b = static_cast<std::byte>(state & 0xFFu);
  }
  return out;
}

inline const std::string& long_name() {
  static const std::string name = "long_file_name_" + std::string(105, 'x') + ".txt";
  return name;
}

inline const std::string& unicode_name() {
  static const std::string name = "unicode/\xC3\xBCn\xC3\xAF" "c\xC3\xB6" "d\xC3\xA9-\xE5\x90\x8D\xE5\x89\x8D.txt";
  return name;
}

struct File {
  std::string path;
  Bytes bytes;
  std::uint32_t mode = 0644;
};

inline std::vector<File> files() {
  std::string lorem;
  for (int i = 0; i < 8; ++i) lorem += lorem_paragraph();
  return {
      {"hello.txt", to_bytes("Hello, archive!\n"), 0644},
      {"lorem.txt", to_bytes(lorem), 0644},
      {"noise.bin", noise(4096), 0644},
      {"empty.txt", {}, 0644},
      {"exec.sh", to_bytes("#!/bin/sh\necho hello\n"), 0755},
      {"dir/nested/deep/file.txt", to_bytes("deep file\n"), 0644},
      {unicode_name(), to_bytes("unicode name\n"), 0644},
      {"long/" + long_name(), to_bytes("long name\n"), 0644},
  };
}

inline std::vector<std::string> directories() {
  return {"dir", "dir/nested", "dir/nested/deep", "dir/empty_dir", "unicode", "long"};
}

inline constexpr std::string_view link_path = "link";
inline constexpr std::string_view link_target = "hello.txt";

/// The tree as an ir::Archive (directories first, then files, then the link),
/// for writer round trips.
inline ir::Archive archive(bool with_symlink = true) {
  ir::Archive out;
  for (const std::string& dir : directories()) {
    ir::ArchiveEntry e;
    e.path = dir;
    e.is_directory = true;
    e.mtime = mtime;
    e.unix_mode = 0755;
    out.entries.push_back(std::move(e));
  }
  for (File& f : files()) {
    ir::ArchiveEntry e;
    e.path = std::move(f.path);
    e.bytes = std::move(f.bytes);
    e.mtime = mtime;
    e.unix_mode = f.mode;
    out.entries.push_back(std::move(e));
  }
  if (with_symlink) {
    ir::ArchiveEntry e;
    e.path = std::string(link_path);
    e.symlink_target = std::string(link_target);
    e.mtime = mtime;
    e.unix_mode = 0777;
    out.entries.push_back(std::move(e));
  }
  return out;
}

/// What a format can carry; everything the format carries is compared exactly.
struct Check {
  bool symlink = true;        ///< `link` -> hello.txt must be present as a symlink
  bool empty_dir = true;      ///< dir/empty_dir must be present as a directory
  bool modes = true;          ///< permission bits (0644 files, 0755 exec.sh and dirs)
  bool mtimes = true;         ///< file mtimes == tree::mtime (+- mtime_tolerance)
  std::int64_t mtime_tolerance = 0;
  bool unicode = true;        ///< the unicode-named file is present (exact name)
  bool long_name = true;      ///< the 124-byte name is present
  bool exec = true;           ///< exec.sh is present
  bool noise = true;          ///< noise.bin is present
  bool allow_extra = false;   ///< extra *files* in the archive are tolerated
};

/// Compare a decoded archive with the tree. Reports every difference.
inline void check(const ir::Archive& archive, const Check& c = {}) {
  std::map<std::string, const ir::ArchiveEntry*, std::less<>> by_path;
  for (const ir::ArchiveEntry& e : archive.entries) by_path[e.path] = &e;

  std::size_t expected_files = 0;
  for (const File& f : files()) {
    if (!c.unicode && f.path == unicode_name()) continue;
    if (!c.long_name && f.path == "long/" + long_name()) continue;
    if (!c.exec && f.path == "exec.sh") continue;
    if (!c.noise && f.path == "noise.bin") continue;
    ++expected_files;
    INFO("entry ", f.path);
    const auto found = by_path.find(f.path);
    CHECK_MESSAGE(found != by_path.end(), "missing file ", f.path);
    if (found == by_path.end()) continue;
    const ir::ArchiveEntry& e = *found->second;
    CHECK(e.is_file());
    CHECK(e.bytes.size() == f.bytes.size());
    CHECK(e.bytes == f.bytes);
    if (c.modes) CHECK_EQ(e.unix_mode & 07777u, f.mode);
    if (c.mtimes) CHECK_LE(std::abs(e.mtime - mtime), c.mtime_tolerance);
  }
  if (c.empty_dir) {
    const auto found = by_path.find("dir/empty_dir");
    CHECK_MESSAGE(found != by_path.end(), "missing empty directory dir/empty_dir");
    if (found != by_path.end()) {
      CHECK(found->second->is_directory);
      if (c.modes) CHECK_EQ(found->second->unix_mode & 07777u, 0755u);
    }
  }
  if (c.symlink) {
    const auto found = by_path.find(link_path);
    CHECK_MESSAGE(found != by_path.end(), "missing symlink 'link'");
    if (found != by_path.end()) {
      CHECK(found->second->is_symlink());
      CHECK(found->second->symlink_target == link_target);
      CHECK(found->second->bytes.empty());
    }
  }
  std::size_t actual_files = 0;
  for (const ir::ArchiveEntry& e : archive.entries) {
    if (e.is_file()) ++actual_files;
  }
  if (!c.allow_extra) CHECK_EQ(actual_files, expected_files);
}

}  // namespace cvt::test::tree
