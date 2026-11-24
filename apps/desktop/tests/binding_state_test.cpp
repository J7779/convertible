// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// Binding tests (real conversions through the core's C API) and view-model
// tests (queue, targets, options from the schema, jobs, saving, settings).
// Everything is written against the dynamic catalogue, so it keeps passing as
// more formats are added to the core.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

#include "app/binding.hpp"
#include "app/state.hpp"
#include "base/text_util.hpp"
#include "platform/fileio.hpp"
#include "support/test.hpp"

using namespace cvd;
using namespace cvd::app;

namespace {

/// A fresh scratch directory below the build tree for one test.
std::string scratch(const std::string& name) {
  const std::string dir = fileio::join(CVD_TEST_TMP_DIR, name);
  fileio::remove_all(dir);
  fileio::make_directories(dir);
  return dir;
}

std::string write(const std::string& dir, const std::string& name, const std::string& content) {
  const std::string p = fileio::join(dir, name);
  fileio::write_text(p, content);
  return p;
}

std::string convert_text(const std::string& in, const std::string& target, const std::string& opts = {},
                         const std::string& source = "txt") {
  std::string out;
  const ConvertOutcome o = convert_bytes(in, "in.txt", source, target, opts, &out);
  REQUIRE_MESSAGE(o.status == kOk, o.message);
  return out;
}

}  // namespace

TEST_CASE("desktop/binding: catalogue is parsed from the core") {
  const Catalog& cat = Catalog::instance();
  REQUIRE(cat.formats().size() > 10u);
  const auto& order = category_order();
  for (const auto& f : cat.formats()) {
    CHECK_FALSE(f.id.empty());
    CHECK_FALSE(f.name.empty());
    CHECK(std::find(order.begin(), order.end(), f.category) != order.end());
    CHECK(f.popularity >= 0);
    CHECK(f.popularity <= 100);
  }
  CHECK(cat.find("txt") != nullptr);
  CHECK(cat.find("base64") != nullptr);
  CHECK(cat.find("no-such-format") == nullptr);
  CHECK(cat.count_writable() >= 2u);
  CHECK(category_label("model3d") == "3D models");
  CHECK_FALSE(core_version().empty());
}

TEST_CASE("desktop/binding: every target and option schema parses") {
  int checked = 0;
  for (const auto& f : Catalog::instance().formats()) {
    const auto targets = targets_for(f.id);
    for (const auto& t : targets) {
      CHECK_FALSE(t.id.empty());
      const auto specs = options_for(f.id, t.id);
      for (const auto& s : specs) {
        CHECK_FALSE(s.key.empty());
        CHECK_FALSE(s.label.empty());
        if (s.type == OptionType::Enum) CHECK_FALSE(s.choices.empty());
        if (s.min && s.max) CHECK(*s.min <= *s.max);
        // Defaults survive coercion unchanged.
        CHECK(coerce_option(s, s.default_value) == s.default_value);
      }
      ++checked;
    }
    if (checked > 400) break;
  }
  CHECK(checked > 0);
  CHECK(targets_for("").empty());
  CHECK(targets_for("no-such-format").empty());
}

TEST_CASE("desktop/binding: text to base64, hex and back") {
  CHECK(convert_text("Hello, world!\n", "base64") == "SGVsbG8sIHdvcmxkIQo=\n");
  CHECK(convert_text("Hello, world!\n", "base64", R"({"padding":false})") == "SGVsbG8sIHdvcmxkIQo\n");
  CHECK(convert_text("abc", "hex") == "616263");
  CHECK(convert_text("abc", "hex", R"({"uppercase":true})") == "616263");
  // Round trips.
  CHECK(convert_text("SGVsbG8sIHdvcmxkIQo=\n", "bin", {}, "base64") == "Hello, world!\n");
  CHECK(convert_text("616263", "bin", {}, "hex") == "abc");
}

TEST_CASE("desktop/binding: checksums match known vectors") {
  const std::string report = convert_text("abc", "checksums");
  CHECK(report.find("900150983cd24fb0d6963f7d28e17f72") != std::string::npos);                          // MD5
  CHECK(report.find("a9993e364706816aba3e25717850c26c9cd0d89d") != std::string::npos);                  // SHA-1
  CHECK(report.find("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") != std::string::npos);  // SHA-256
  CHECK(report.find("352441c2") != std::string::npos);                                                  // CRC-32
}

TEST_CASE("desktop/binding: text re-encoding to UTF-16 with a BOM") {
  const std::string out = convert_text("h\xC3\xA9", "txt", R"({"encoding":"utf-16le","bom":true})");
  CHECK(out == std::string("\xFF\xFE" "h\0\xE9\0", 6));
  const std::string crlf = convert_text("a\nb\n", "txt", R"({"line_endings":"crlf"})");
  CHECK(crlf == "a\r\nb\r\n");
}

TEST_CASE("desktop/binding: file conversion, cancellation and error mapping") {
  const std::string dir = scratch("binding-file");
  const std::string in = write(dir, "notes.txt", std::string(200000, 'x'));
  ConvertRequest req;
  req.input_path = in;
  req.output_path = fileio::join(dir, "notes.b64");
  req.target = "base64";
  double last = -1.0;
  bool monotonic = true;
  const ConvertOutcome ok = convert_file(req, [&](double f, std::string_view) {
    monotonic = monotonic && f >= last;
    last = f;
    return false;
  });
  CHECK(ok.status == kOk);
  CHECK(monotonic);
  CHECK(ok.output_size > 266000u);
  CHECK(fileio::file_size(req.output_path).value_or(0) == ok.output_size);
  CHECK(ok.extension == "b64");

  ConvertRequest cancel = req;
  cancel.output_path = fileio::join(dir, "cancelled.b64");
  const ConvertOutcome c = convert_file(cancel, [](double, std::string_view) { return true; });
  CHECK(c.status == kCancelled);
  CHECK_FALSE(fileio::exists(cancel.output_path));  // nothing left behind

  ConvertRequest bad = req;
  bad.target = "no-such-format";
  bad.output_path = fileio::join(dir, "bad.out");
  const ConvertOutcome b = convert_file(bad, {});
  CHECK(b.status != kOk);
  CHECK_FALSE(b.message.empty());

  ConvertRequest missing = req;
  missing.input_path = fileio::join(dir, "does-not-exist.txt");
  CHECK(convert_file(missing, {}).status == kIo);

  // Every status maps to a friendly title, and failures get a hint.
  for (int s = 0; s <= 9; ++s) {
    CHECK_FALSE(status_title(s).empty());
    if (s != kOk) CHECK_FALSE(status_hint(s).empty());
  }
  CHECK(status_title(kDecode).find("damaged") != std::string::npos);
}

TEST_CASE("desktop/binding: detection of files by content and name") {
  const std::string dir = scratch("binding-detect");
  CHECK(detect_file(write(dir, "a.txt", "plain words\n")) == "txt");
  CHECK(detect_file(fileio::join(dir, "missing.bin")).empty());
}

// ---------------------------------------------------------------------------
// View-model
// ---------------------------------------------------------------------------

TEST_CASE("desktop/state: options from a schema, presets and coercion") {
  OptionSpec quality{"quality", "Quality", OptionType::Int, Json(85), 1.0, 100.0, {}, "", false};
  OptionSpec preset{"preset", "Preset", OptionType::Enum, Json("balanced"), std::nullopt, std::nullopt,
                    {"high", "balanced", "small"}, "", false};
  OptionSpec strip{"strip_metadata", "Strip", OptionType::Bool, Json(false), std::nullopt, std::nullopt, {}, "", true};
  OptionSpec name{"title", "Title", OptionType::String, Json("x"), std::nullopt, std::nullopt, {}, "", true};
  const std::vector<OptionSpec> specs = {quality, preset, strip, name};
  CHECK(has_preset(specs));
  CHECK_FALSE(has_preset({quality}));
  Settings s;
  s.default_preset = 2;
  s.strip_metadata = true;
  const Json values = default_option_values(specs, s);
  CHECK(values["quality"].as_int() == 85);
  CHECK(values["preset"].str() == "small");  // the user's default preset
  CHECK(values["strip_metadata"].as_bool());  // the user's default
  CHECK(preset_index(values) == 2);
  CHECK(coerce_option(quality, Json(250)).as_int() == 100);
  CHECK(coerce_option(quality, Json(0.4)).as_int() == 1);
  CHECK(coerce_option(quality, Json(42.6)).as_int() == 43);
  CHECK(coerce_option(quality, Json("oops")).as_int() == 85);
  CHECK(coerce_option(preset, Json("ultra")).str() == "balanced");
  CHECK(coerce_option(strip, Json(1)).as_bool() == false);
  CHECK(coerce_option(name, Json("t")).str() == "t");
}

TEST_CASE("desktop/state: target grouping, ordering and search") {
  std::vector<TargetInfo> targets = {
      {"base64", "Base64 text", "encoding", {"b64"}, "", false, false},
      {"txt", "Plain text", "document", {"txt"}, "", false, true},
      {"hex", "Hexadecimal text", "encoding", {"hex"}, "", false, false},
      {"md", "Markdown", "document", {"md", "markdown"}, "", false, false},
      {"zip", "ZIP archive", "archive", {"zip"}, "", false, false},
  };
  const auto groups = group_targets(targets, "document", "");
  REQUIRE(groups.size() == 3u);
  CHECK(groups[0].category == "document");  // same category first
  CHECK(groups[0].targets[0]->id == "txt");
  CHECK(groups[1].category == "encoding");  // then in the core's relevance order
  CHECK(groups[1].targets[1]->id == "hex");
  const auto found = group_targets(targets, "document", "MARK");
  REQUIRE(found.size() == 1u);
  CHECK(found[0].targets[0]->id == "md");
  CHECK(group_targets(targets, "document", ".b64").at(0).targets[0]->id == "base64");
  CHECK(group_targets(targets, "document", "zzz").empty());
  CHECK(default_target(targets) == "base64");  // the first non-compress entry
  CHECK(archive_targets(targets).size() == 1u);
  CHECK(output_file_name("photo.jpeg", "png", false) == "photo.png");
  CHECK(output_file_name("photo.png", "png", true) == "photo (compressed).png");
  CHECK(output_file_name("backup.tar.gz", "zip", false) == "backup.zip");
}

TEST_CASE("desktop/state: queue operations and shared-format control") {
  const std::string dir = scratch("state-queue");
  AppState st(fileio::join(dir, "tmp"), {}, 2);
  const std::string a = write(dir, "a.txt", "alpha\n");
  const std::string b = write(dir, "b.txt", "beta\n");
  const std::string c = write(dir, "c.bin", std::string("\x00\x01\x02\xFF", 4));
  CHECK(st.add_files({a, b, c, a}) == 3);  // duplicates are skipped
  CHECK(st.add_files({dir}) == 0);         // folders are refused with a notice
  CHECK(st.add_files({fileio::join(dir, "nope.txt")}) == 0);
  CHECK(st.notices.size() == 2u);
  REQUIRE(st.items().size() == 3u);
  const QueueItem& first = st.items()[0];
  CHECK(first.source == "txt");
  CHECK(first.size == 6u);
  CHECK_FALSE(first.targets.empty());
  CHECK_FALSE(first.is_compress());
  CHECK(first.target == default_target(first.targets));
  CHECK(st.shared_sources() == std::vector<std::string>{"txt"});
  CHECK(st.set_target_for_source("txt", "hex") == 2);
  CHECK(st.items()[0].target == "hex");
  CHECK(st.items()[1].target == "hex");
  CHECK_FALSE(st.set_target(st.items()[0].id, "no-such-target"));
  // The unknown-ish binary file still gets the universal targets.
  CHECK_FALSE(st.items()[2].targets.empty());
  // Options follow the target's schema.
  REQUIRE(st.set_target(st.items()[0].id, "base64"));
  CHECK(st.items()[0].values.contains("line_width"));
  CHECK(st.set_option(st.items()[0].id, "line_width", Json(5000)));
  CHECK(st.items()[0].values["line_width"].as_int() == 1024);  // clamped to the schema max
  CHECK_FALSE(st.set_option(st.items()[0].id, "not-an-option", Json(1)));
  st.reset_options(st.items()[0].id);
  CHECK(st.items()[0].values["line_width"].as_int() == 76);
  const std::uint64_t id_b = st.items()[1].id;
  st.remove(id_b);
  CHECK(st.items().size() == 2u);
  CHECK(st.find(id_b) == nullptr);
  st.clear();
  CHECK(st.empty());
}

TEST_CASE("desktop/state: converting, saving and converting again") {
  const std::string dir = scratch("state-convert");
  AppState st(fileio::join(dir, "tmp"), {}, 2);
  REQUIRE(st.add_files({write(dir, "hello.txt", "Hello, world!\n"), write(dir, "abc.txt", "abc")}) == 2);
  REQUIRE(st.set_target(st.items()[0].id, "base64"));
  REQUIRE(st.set_target(st.items()[1].id, "hex"));
  CHECK(st.start_all() == 2);
  CHECK(st.any_busy());
  st.wait_idle();
  st.poll(1.0);
  CHECK_FALSE(st.any_busy());
  REQUIRE(st.items()[0].status == ItemStatus::Done);
  REQUIRE(st.items()[1].status == ItemStatus::Done);
  CHECK(st.items()[0].output_name == "hello.b64");
  CHECK(st.items()[0].output_size == 21u);
  CHECK(st.count(ItemStatus::Done) == 2);

  const std::string out = fileio::join(dir, "out");
  CHECK(st.save_all_to(out) == 2);
  CHECK(fileio::read_text(fileio::join(out, "hello.b64")).value_or("") == "SGVsbG8sIHdvcmxkIQo=\n");
  CHECK(fileio::read_text(fileio::join(out, "abc.hex")).value_or("") == "616263");
  // Saving again never overwrites: unique names are chosen.
  CHECK(st.save_all_to(out) == 2);
  CHECK(fileio::exists(fileio::join(out, "hello (2).b64")));
  CHECK(st.items()[0].saved_path == fileio::join(out, "hello (2).b64"));

  st.convert_again(st.items()[0].id);
  CHECK(st.items()[0].status == ItemStatus::Ready);
  CHECK(st.items()[0].target == "base64");
}

TEST_CASE("desktop/state: cancelling a running conversion") {
  const std::string dir = scratch("state-cancel");
  AppState st(fileio::join(dir, "tmp"), {}, 1);
  // Large enough that cancellation lands while it runs (or before it starts).
  REQUIRE(st.add_files({write(dir, "big.txt", std::string(40'000'000, 'q'))}) == 1);
  REQUIRE(st.set_target(st.items()[0].id, "hex"));
  REQUIRE(st.start(st.items()[0].id));
  st.cancel(st.items()[0].id);
  st.wait_idle();
  st.poll(1.0);
  const QueueItem& item = st.items()[0];
  CHECK(item.status == ItemStatus::Cancelled);
  CHECK(item.error_status == kCancelled);
  CHECK(item.output_path.empty());
}

TEST_CASE("desktop/state: failures are mapped to friendly errors") {
  const std::string dir = scratch("state-fail");
  AppState st(fileio::join(dir, "tmp"), {}, 1);
  const std::string path = write(dir, "gone.txt", "temporary");
  REQUIRE(st.add_files({path}) == 1);
  fileio::remove_all(path);  // the source disappears before conversion
  REQUIRE(st.start(st.items()[0].id));
  st.wait_idle();
  st.poll(1.0);
  const QueueItem& item = st.items()[0];
  CHECK(item.status == ItemStatus::Failed);
  CHECK(item.error_status == kIo);
  CHECK_FALSE(status_title(item.error_status).empty());
}

TEST_CASE("desktop/settings: JSON round trip and persistence") {
  Settings s;
  s.theme = ThemeMode::Light;
  s.default_preset = 0;
  s.strip_metadata = false;
  s.motion = MotionMode::Reduced;
  s.text_scale_percent = 130;
  CHECK(Settings::from_json(s.to_json()) == s);
  const std::string dir = scratch("settings");
  CHECK(load_settings(dir) == Settings{});  // missing file -> defaults
  CHECK(save_settings(fileio::join(dir, "nested"), s));
  CHECK(load_settings(fileio::join(dir, "nested")) == s);
  fileio::write_text(fileio::join(dir, "settings.json"), "{broken");
  CHECK(load_settings(dir) == Settings{});
  const Json odd = Json::parse(R"({"theme":"purple","defaultPreset":"huge","textScale":121})").value();
  const Settings parsed = Settings::from_json(odd);
  CHECK(parsed.theme == ThemeMode::System);
  CHECK(parsed.default_preset == 1);
  CHECK(parsed.text_scale_percent == 115);
}

TEST_CASE("desktop/fileio: unique names and directory listing") {
  const std::string dir = scratch("fileio");
  write(dir, "a.txt", "1");
  write(dir, "a (2).txt", "2");
  write(dir, "x.tar.gz", "3");
  CHECK(fileio::unique_path(dir, "b.txt") == fileio::join(dir, "b.txt"));
  CHECK(fileio::unique_path(dir, "a.txt") == fileio::join(dir, "a (3).txt"));
  CHECK(fileio::unique_path(dir, "x.tar.gz") == fileio::join(dir, "x (2).tar.gz"));
  fileio::make_directories(fileio::join(dir, "sub"));
  const auto entries = fileio::list_directory(dir);
  CHECK(entries.size() == 4u);
  CHECK(std::count_if(entries.begin(), entries.end(), [](const auto& e) { return e.is_dir; }) == 1);
  CHECK(fileio::read_file(fileio::join(dir, "a.txt"), 0).value().empty());
  // UTF-8 names work on every platform.
  const std::string uni = write(dir, "r\xC3\xA9sum\xC3\xA9 \xE2\x82\xAC.txt", "u");
  CHECK(fileio::exists(uni));
  CHECK(detect_file(uni) == "txt");
}
