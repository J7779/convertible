// Converting: per-file progress (stepped deterministically through the
// progress gate), results with before/after sizes, saved files verified byte
// for byte with independent decoders and digests, Save / Save all / Convert
// again / Reveal / Open (through the shell seam), cancellation and errors.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <cstdio>

#include "common.hpp"

using namespace e2e;

namespace {

/// The app's size and change formats (as specified for the result row).
std::string format_size(std::uint64_t bytes) {
  if (bytes == 1) return "1 byte";
  if (bytes < 1000) return std::to_string(bytes) + " bytes";
  const char* units[] = {"KB", "MB", "GB", "TB", "PB"};
  double value = static_cast<double>(bytes) / 1000.0;
  int unit = 0;
  while (value >= 999.95 && unit < 4) {
    value /= 1000.0;
    ++unit;
  }
  char buf[32];
  if (value < 9.995) std::snprintf(buf, sizeof buf, "%.2f %s", value, units[unit]);
  else if (value < 99.95) std::snprintf(buf, sizeof buf, "%.1f %s", value, units[unit]);
  else std::snprintf(buf, sizeof buf, "%.0f %s", value, units[unit]);
  return buf;
}

std::string format_change(std::uint64_t before, std::uint64_t after) {
  if (before == 0) return after == 0 ? "0%" : "new";
  const long long r = std::llround((static_cast<double>(after) - static_cast<double>(before)) / static_cast<double>(before) * 100.0);
  if (r == 0) return "0%";
  return r < 0 ? "\xE2\x88\x92" + std::to_string(-r) + "%" : "+" + std::to_string(r) + "%";
}

/// The "Name: value" lines of a checksum report.
std::map<std::string, std::string> report_fields(const std::string& text) {
  std::map<std::string, std::string> out;
  std::size_t start = 0;
  while (start < text.size()) {
    std::size_t nl = text.find('\n', start);
    if (nl == std::string::npos) nl = text.size();
    const std::string line = text.substr(start, nl - start);
    const std::size_t colon = line.find(':');
    if (colon != std::string::npos) {
      std::string v = line.substr(colon + 1);
      while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
      while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
      out[line.substr(0, colon)] = v;
    }
    start = nl + 1;
  }
  return out;
}

void require_targets(const Json& item, std::initializer_list<const char*> ids) {
  for (const char* id : ids) {
    if (!contains(item["targetIds"], id)) SKIP("the catalogue offers no '", id, "' target for ", item["name"].str());
  }
}

}  // namespace

TEST_CASE("e2e/convert/progress is reported per file, never goes back and reaches 100 %") {
  Session s("convert-progress");
  const Bytes big = pseudo_random(3 * 1024 * 1024, 42);
  s.write_input("big.bin", big);
  REQUIRE(s.ok(s.run("convert_progress.json")));

  const Json start = s.state("c-start.json");
  CHECK(at(start, "items.0.status").str() == "converting");
  CHECK(at(start, "automation.gate.parked").as_int() == 1);
  const Json mid = s.state("c-mid.json");
  CHECK(at(mid, "items.0.status").str() == "converting");
  const double p = at(mid, "items.0.progress").as_number();
  CHECK(p > 0.0);
  CHECK(p < 1.0);
  // The progress ring and the action bar report it.
  const Json* ring = node(mid, "Converting big.bin", "progress");
  REQUIRE(ring != nullptr);
  CHECK((*ring)["value"].str() == std::to_string(static_cast<int>(std::lround(p * 100.0))) + "%");
  CHECK(node(mid, "Converting 1 file\xE2\x80\xA6", "label") != nullptr);
  CHECK(node(mid, "Cancel all", "button") != nullptr);

  const Json done = s.state("c-done.json");
  const Json& samples = at(done, "automation.samples.0");
  REQUIRE(samples.size() >= 3u);
  bool intermediate = false;
  for (std::size_t i = 1; i < samples.size(); ++i) {
    INFO("sample ", i);
    CHECK(samples[i].as_number() >= samples[i - 1].as_number());
    intermediate = intermediate || (samples[i].as_number() > 0.0 && samples[i].as_number() < 1.0);
  }
  CHECK(intermediate);
  CHECK(samples[samples.size() - 1].as_number() == 1.0);
  CHECK(at(done, "items.0.status").str() == "done");
  CHECK(at(done, "items.0.progress").as_number() == 1.0);
  const Bytes out = saved(s, "saved/big.b64");
  CHECK(base64_decode(to_string(out)).value_or(Bytes{}) == big);
}

TEST_CASE("e2e/convert/results, Save, Reveal, Open, Save all and Convert again with verified bytes") {
  Session s("convert-results");
  const Bytes hello = to_bytes("Hello, world!\n");
  const Bytes fox = to_bytes("The quick brown fox jumps over the lazy dog");
  const Bytes data = pseudo_random(5000, 9);
  const std::string crlf = "Line one\r\nL\xC3\xADne tw\xC3\xB6 \xE2\x9C\x93\r\n";
  s.write_input("hello.txt", hello);
  s.write_input("fox.txt", fox);
  s.write_input("data.bin", data);
  s.write_input("crlf.txt", crlf);
  make_dirs(s.out("one"));  // a native save dialog only returns existing folders
  REQUIRE(s.ok(s.run("convert_results.json")));

  const Json done = s.state("r-done.json");
  REQUIRE(done["items"].size() == 4u);
  for (const Json& it : done["items"].items()) {
    INFO(it["name"].str());
    require_targets(it, {"base64"});
    CHECK(it["status"].str() == "done");
    // Before/after sizes and the percentage on the card.
    const auto before = static_cast<std::uint64_t>(it["size"].as_number());
    const auto after = static_cast<std::uint64_t>(it["outputSize"].as_number());
    const std::string label = format_size(before) + " to " + format_size(after) + " (" + format_change(before, after) + ")";
    CHECK(node(done, label, "label") != nullptr);
  }
  CHECK(node(done, "4 files converted", "label") != nullptr);

  // Save (one file, through the dialog seam), then Reveal and Open go through the shell seam.
  const Json saved_state = s.state("r-saved.json");
  const std::string one = s.out("one/hello-saved.b64");
  CHECK(same_path(at(saved_state, "items.0.savedPath").str(), one));
  CHECK(at(saved_state, "automation.dialogs.0.kind").str() == "save");
  CHECK(at(saved_state, "automation.dialogs.0.name").str() == "hello.b64");
  CHECK(node(saved_state, "Saved", "button") != nullptr);
  CHECK(at(saved_state, "toasts.0").str() == "Saved hello-saved.b64");
  CHECK(base64_decode(to_string(saved(s, "one/hello-saved.b64"))).value_or(Bytes{}) == hello);
  CHECK(to_string(saved(s, "one/hello-saved.b64")) == "SGVsbG8sIHdvcmxkIQo=\n");
  const Json shell = s.state("r-shell.json");
  REQUIRE(at(shell, "automation.shell").size() == 2u);
  CHECK(at(shell, "automation.shell.0.action").str() == "reveal");
  CHECK(same_path(at(shell, "automation.shell.0.path").str(), one));
  CHECK(at(shell, "automation.shell.0.exists").as_bool());
  CHECK(at(shell, "automation.shell.1.action").str() == "open");
  CHECK(same_path(at(shell, "automation.shell.1.path").str(), one));

  // Save all writes every result; the bytes are what the inputs encode to.
  const Json all = s.state("r-all.json");
  CHECK(at(all, "automation.dialogs.1.kind").str() == "folder");
  const auto names = list_dir(s.out("all"));
  CHECK(names.size() == 4u);
  const std::string out_b64 = to_string(saved(s, "all/" + at(done, "items.0.outputName").str()));
  CHECK(base64_decode(out_b64).value_or(Bytes{}) == hello);
  const std::string out_hex = to_string(saved(s, "all/" + at(done, "items.1.outputName").str()));
  CHECK(hex_decode(out_hex).value_or(Bytes{}) == fox);
  CHECK(out_hex == hex_lower(fox.data(), fox.size()));
  // Checksums equal independently computed digests (and the digests match published vectors).
  CHECK(sha256_hex(to_bytes("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(md5_hex(to_bytes("abc")) == "900150983cd24fb0d6963f7d28e17f72");
  CHECK(crc32(to_bytes("123456789")) == 0xCBF43926u);
  const auto fields = report_fields(to_string(saved(s, "all/" + at(done, "items.2.outputName").str())));
  REQUIRE(fields.count("SHA-256") == 1u);
  CHECK(fields.at("SHA-256") == sha256_hex(data));
  if (fields.count("SHA-1")) CHECK(fields.at("SHA-1") == sha1_hex(data));
  if (fields.count("MD5")) CHECK(fields.at("MD5") == md5_hex(data));
  if (fields.count("CRC-32")) {
    char crc[16];
    std::snprintf(crc, sizeof crc, "%08x", crc32(data));
    CHECK(fields.at("CRC-32") == crc);
  }
  // UTF-16LE with a BOM and LF line endings.
  Bytes expected_utf16 = {0xFF, 0xFE};
  const Bytes body = utf8_to_utf16le("Line one\nL\xC3\xADne tw\xC3\xB6 \xE2\x9C\x93\n").value();
  expected_utf16.insert(expected_utf16.end(), body.begin(), body.end());
  CHECK(saved(s, "all/" + at(done, "items.3.outputName").str()) == expected_utf16);

  // Convert again with another option: the card returns to ready, the new output differs.
  CHECK(at(s.state("r-again-ready.json"), "items.0.status").str() == "ready");
  const Json again = s.state("r-again.json");
  CHECK(at(again, "items.0.status").str() == "done");
  const std::string rewrapped = to_string(saved(s, "again/hello.b64"));
  CHECK(rewrapped != out_b64);
  CHECK(rewrapped.find('\n') <= 8u);
  CHECK(base64_decode(rewrapped).value_or(Bytes{}) == hello);
}

TEST_CASE("e2e/convert/Save all numbers colliding names and never overwrites") {
  Session s("convert-collisions");
  s.write_input("first/a.txt", "first a\n");
  s.write_input("second/a.txt", "second a\n");
  make_dirs(s.out("coll"));
  write_text(s.out("coll/a.b64"), "already here\n");
  REQUIRE(s.ok(s.run("convert_collisions.json")));
  CHECK(read_text(s.out("coll/a.b64")) == "already here\n");
  const auto names = list_dir(s.out("coll"));
  CHECK(names == std::vector<std::string>{"a (2).b64", "a (3).b64", "a (4).b64", "a (5).b64", "a.b64"});
  CHECK(base64_decode(read_text(s.out("coll/a (2).b64"))).value_or(Bytes{}) == to_bytes("first a\n"));
  CHECK(base64_decode(read_text(s.out("coll/a (3).b64"))).value_or(Bytes{}) == to_bytes("second a\n"));
  CHECK(at(s.state("coll-1.json"), "toasts.0").str() == "Saved 2 files to coll");
}

TEST_CASE("e2e/convert/saving through the in-house browser never replaces the source file") {
  Session s("convert-save-browser");
  const std::string text = "Notes to re-encode.\n";
  s.write_input("notes.txt", text);
  REQUIRE(s.ok(s.run("convert_save_browser.json")));
  const Json browser = s.state("s-browser.json");
  REQUIRE(browser["browser"]["open"].as_bool());
  CHECK(browser["browser"]["mode"].str() == "save");
  CHECK(same_path(browser["browser"]["directory"].str(), s.in()));
  const std::string suggested = browser["browser"]["fileName"].str();
  CHECK(suggested != "notes.txt");  // re-encoding .txt to .txt must not suggest the source itself
  CHECK(read_text(s.in("notes.txt")) == text);
  const Json after = s.state("s-saved.json");
  CHECK_FALSE(after["browser"]["open"].as_bool());
  CHECK(same_path(at(after, "items.0.savedPath").str(), s.in(suggested)));
  Bytes expected = utf8_to_utf16le(text).value();
  CHECK(read_bytes(s.in(suggested)).value_or(Bytes{}) == expected);
}

TEST_CASE("e2e/cancel/cancel one, cancel all, then convert again") {
  Session s("cancel");
  const Bytes a = pseudo_random(2 * 1024 * 1024, 1);
  const Bytes b = pseudo_random(1024 * 1024, 2);
  s.write_input("big1.bin", a);
  s.write_input("big2.bin", b);
  REQUIRE(s.ok(s.run("cancel.json")));
  const Json busy = s.state("k-busy.json");
  CHECK(at(busy, "items.0.status").str() == "converting");
  CHECK(at(busy, "items.1.status").str() == "converting");
  CHECK(node(busy, "Converting 2 files\xE2\x80\xA6", "label") != nullptr);
  const Json one = s.state("k-one.json");
  CHECK(at(one, "items.0.status").str() == "cancelled");
  CHECK(at(one, "items.1.status").str() == "converting");
  const Json all = s.state("k-all.json");
  for (const Json& it : all["items"].items()) {
    CHECK(it["status"].str() == "cancelled");
    CHECK(it["outputPath"].str().empty());  // the partial output is gone
  }
  CHECK(nodes_with_role(all, "button").size() > 0u);
  CHECK(node(all, "Convert again", "button") != nullptr);
  const Json ready = s.state("k-ready.json");
  CHECK(at(ready, "items.0.status").str() == "ready");
  CHECK(at(ready, "items.1.status").str() == "ready");
  CHECK(at(ready, "items.0.target").str() == "base64");  // target kept
  const Json done = s.state("k-done.json");
  CHECK(at(done, "items.0.status").str() == "done");
  CHECK(at(done, "items.1.status").str() == "done");
  CHECK(base64_decode(read_text(s.out("saved/big1.b64"))).value_or(Bytes{}) == a);
  CHECK(hex_decode(read_text(s.out("saved/big2.hex"))).value_or(Bytes{}) == b);
}

TEST_CASE("e2e/errors/a failed conversion has a friendly title and the queue stays intact") {
  Session probe("errors-probe");
  const std::string bad = probe.write_input("bad.b64", "Zm9v!!!***");
  const Discovery d = discover(probe, bad, "bad");
  if (d.item["source"].str() != "base64" || !contains(d.item["targetIds"], "bin")) {
    SKIP("the catalogue cannot decode Base64 to raw bytes here");
  }
  Session s("errors");
  s.write_input("bad.b64", "Zm9v!!!***");
  s.write_input("good.txt", "fine\n");
  REQUIRE(s.ok(s.run("errors.json", {{"DECODE_TARGET", "bin"}})));
  const Json failed = s.state("e-failed.json");
  REQUIRE(failed["items"].size() == 2u);
  const Json& item = failed["items"][0];
  CHECK(item["status"].str() == "failed");
  const std::string title = item["errorTitle"].str();
  CHECK_FALSE(title.empty());
  // Friendly: a sentence, not a status code or an exception name.
  CHECK(title.find_first_of("0123456789") == std::string::npos);
  CHECK(title.find("cvt") == std::string::npos);
  CHECK(title.find("error") == std::string::npos);
  CHECK(std::isupper(static_cast<unsigned char>(title[0])));
  CHECK_FALSE(item["errorMessage"].str().empty());
  bool described = false;
  for (const Json& n : failed["accessibility"].items()) described = described || n["label"].str().rfind(title + ".", 0) == 0;
  CHECK(described);
  CHECK(node(failed, "Try again", "button") != nullptr);
  CHECK(node(failed, "1 file converted \xC2\xB7 1 failed", "label") != nullptr);
  CHECK(at(failed, "items.1.status").str() == "done");
  // Try again returns the card to ready with its target kept.
  const Json retry = s.state("e-retry.json");
  CHECK(at(retry, "items.0.status").str() == "ready");
  CHECK(at(retry, "items.0.target").str() == "bin");
  CHECK(at(retry, "items.1.status").str() == "done");
  // A save that cannot be written and an Open the system refuses both explain themselves.
  const Json save_fail = s.state("e-savefail.json");
  CHECK(at(save_fail, "toasts.0").str().find("Couldn\xE2\x80\x99t save to x.b64") == 0u);
  CHECK(at(save_fail, "items.1.savedPath").str().empty());
  const Json shell = s.state("e-shell.json");
  CHECK(contains(shell["toasts"], "No app is set up to open this kind of file."));
}
