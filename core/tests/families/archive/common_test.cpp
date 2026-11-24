// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// Shared archive machinery: path and symlink safety, limits, write policy, bridges.
#include <string>

#include "support/test_support.hpp"

#include "families/archive/archive_common.hpp"
#include "families/archive/bridges.hpp"
#include "families/archive/tree_fixture.hpp"

using namespace cvt;

namespace {

ir::ArchiveEntry file(std::string path, std::string_view text = "x") {
  ir::ArchiveEntry e;
  e.path = std::move(path);
  e.bytes = to_bytes(text);
  return e;
}

ir::ArchiveEntry link(std::string path, std::string target) {
  ir::ArchiveEntry e;
  e.path = std::move(path);
  e.symlink_target = std::move(target);
  return e;
}

}  // namespace

TEST_CASE("archive/common: reader sink sanitises names and enforces symlink safety") {
  Context ctx;
  archive::EntrySink sink(ctx, 1000);
  REQUIRE_OK(sink.add(file("../evil.txt")));      // escapes: skipped
  REQUIRE_OK(sink.add(file("/abs/path.txt")));    // made relative
  REQUIRE_OK(sink.add(file("C:\\win\\x.txt")));   // drive and backslashes
  REQUIRE_OK(sink.add(link("ok", "abs/path.txt")));
  REQUIRE_OK(sink.add(link("sub/up", "../abs")));
  REQUIRE_OK(sink.add(link("bad1", "/etc/passwd")));
  REQUIRE_OK(sink.add(link("bad2", "../../outside")));
  REQUIRE_OK(sink.add(link("dirlink", "abs")));
  REQUIRE_OK(sink.add(file("dirlink/through.txt")));  // written through a link: dropped
  const ir::Archive archive = sink.finish("comment");
  std::vector<std::string> paths;
  for (const auto& e : archive.entries) paths.push_back(e.path);
  CHECK(paths == std::vector<std::string>{"abs/path.txt", "win/x.txt", "ok", "sub/up", "dirlink"});
  CHECK(archive.comment == "comment");
  CHECK_FALSE(ctx.warnings.empty());
}

TEST_CASE("archive/common: decompression budget, totals and ratio guard") {
  Context ctx;
  ctx.limits.max_decompressed_bytes = 1000;
  archive::EntrySink sink(ctx, 10);
  CHECK_EQ(sink.budget(), 1000u);
  REQUIRE_OK(sink.charge(600));
  CHECK_EQ(sink.budget(), 400u);
  CHECK_STATUS(sink.charge(500), Status::Limit);

  Context big;
  archive::EntrySink ratio(big, 100);  // 100 input bytes may expand to at most 64 MiB
  CHECK_EQ(ratio.budget(), archive::ratio_guard_floor);
  CHECK_STATUS(ratio.charge(archive::ratio_guard_floor + 1), Status::Limit);

  Context few;
  few.limits.max_archive_entries = 2;
  archive::EntrySink entries(few, 10);
  REQUIRE_OK(entries.add(file("a")));
  REQUIRE_OK(entries.add(file("b")));
  CHECK_STATUS(entries.add(file("c")), Status::Limit);
}

TEST_CASE("archive/common: write policy normalises modes and times") {
  ir::Archive in;
  auto a = file("a.sh");
  a.unix_mode = 04755;  // setuid is never kept when stripping
  a.mtime = 1600000000;
  in.entries.push_back(a);
  auto b = file("dir/b.txt");
  b.unix_mode = 0600;
  in.entries.push_back(b);  // mtime unknown
  in.entries.push_back(link("l", "a.sh"));
  in.entries.push_back(file("a.sh", "second"));  // duplicate: last wins

  WriteContext ctx;
  archive::WritePolicy policy;
  policy.strip_metadata = true;
  auto prepared = archive::prepare_entries(in, policy, ctx, {.symlinks = true, .directories = true, .add_parent_directories = true});
  REQUIRE_OK(prepared);
  REQUIRE_EQ(prepared->size(), 4u);  // a.sh, dir, dir/b.txt, l
  CHECK((*prepared)[0].path == "a.sh");
  CHECK_EQ((*prepared)[0].mode, 0644u);  // the duplicate (mode 0) replaced it
  CHECK(to_string((*prepared)[0].data()) == "second");
  CHECK((*prepared)[1].path == "dir");
  CHECK((*prepared)[1].type == archive::EntryType::Directory);
  CHECK_EQ((*prepared)[2].mode, 0644u);
  CHECK_EQ((*prepared)[2].mtime, archive::fixed_epoch);
  CHECK((*prepared)[3].type == archive::EntryType::Symlink);
  CHECK_FALSE(ctx.warnings.empty());

  policy.strip_metadata = false;
  in.entries.pop_back();
  auto kept = archive::prepare_entries(in, policy, ctx, {.symlinks = false});
  REQUIRE_OK(kept);
  REQUIRE_EQ(kept->size(), 2u);
  CHECK_EQ((*kept)[0].mode, 04755u);
  CHECK_EQ((*kept)[0].mtime, 1600000000);
  CHECK_EQ((*kept)[1].mode, 0600u);

  CHECK_EQ(archive::normalized_mode(archive::EntryType::File, 0, true), 0644u);
  CHECK_EQ(archive::normalized_mode(archive::EntryType::Directory, 0700, true), 0755u);
  CHECK_EQ(archive::normalized_mode(archive::EntryType::Directory, 0700, false), 0700u);
}

TEST_CASE("archive/common: name helpers") {
  CHECK(archive::decode_name("caf\x82", false) == "caf\xC3\xA9");     // CP437 0x82 = e acute
  CHECK(archive::decode_name("caf\xE9", false, false) == "caf\xC3\xA9");  // Latin-1
  CHECK(archive::decode_name("ok", false) == "ok");
  CHECK(archive::encode_cp437("caf\xC3\xA9") == std::optional<std::string>("caf\x82"));
  CHECK_FALSE(archive::encode_cp437("\xE5\x90\x8D").has_value());
  CHECK(archive::strip_compression_extension("photo.PNG.gz", {"gz"}) == "photo.PNG");
  CHECK(archive::strip_compression_extension("backup.tgz", {"tgz", "tar.gz"}, ".tar") == "backup.tar");
  CHECK(archive::strip_compression_extension("", {"gz"}) == "data");
  CHECK(archive::strip_compression_extension("noext", {"gz"}) == "noext.out");
}

TEST_CASE("archive/bridges: wrap a file and take it out again") {
  ir::Blob blob;
  blob.filename = "dir/report.pdf";
  blob.bytes = to_bytes("%PDF");
  auto wrapped = archive::wrap_blob(blob, nullptr);
  REQUIRE_OK(wrapped);
  REQUIRE_EQ(wrapped->entries.size(), 1u);
  CHECK(wrapped->entries[0].path == "report.pdf");

  ir::Blob nameless;
  nameless.format_id = "png";
  auto named = archive::wrap_blob(nameless, &Registry::instance());
  REQUIRE_OK(named);
  CHECK(named->entries[0].path.starts_with("data"));

  auto back = archive::extract_entry(*wrapped, "");
  REQUIRE_OK(back);
  CHECK(back->filename == "report.pdf");
  CHECK(to_string(back->bytes) == "%PDF");

  ir::Archive two = test::tree::archive();
  CHECK_STATUS(archive::extract_entry(two, ""), Status::Encode);
  auto picked = archive::extract_entry(two, "dir/nested/deep/file.txt");
  REQUIRE_OK(picked);
  CHECK(to_string(picked->bytes) == "deep file\n");
  CHECK_STATUS(archive::extract_entry(two, "missing.txt"), Status::Encode);
  CHECK_STATUS(archive::extract_entry(two, "../x"), Status::InvalidArgument);
}

TEST_CASE("archive/bridges: registered, and Blob targets still see the raw archive") {
  bool wrap = false;
  bool extract = false;
  for (const Bridge& b : Registry::instance().bridges()) {
    if (b.from == Kind::Blob && b.to == Kind::Archive) wrap = true;
    if (b.from == Kind::Archive && b.to == Kind::Blob) extract = true;
  }
  CHECK(wrap);
  CHECK(extract);
  CHECK(Registry::instance().validate().empty());
}
