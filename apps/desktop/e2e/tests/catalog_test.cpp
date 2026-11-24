// Catalogue-conditional conversions: they run only when the core offers the
// formats (images, archives, audio) and skip with a message otherwise. The
// outputs are checked structurally by code written here (JPEG markers, the
// ZIP central directory, audio container magic).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>

#include "common.hpp"

using namespace e2e;

namespace {

Json catalogue(Session& s) {
  REQUIRE(s.ok(s.run_inline(script(R"JS([{"op": "dump", "path": "catalog.json", "catalog": true}])JS"))));
  return s.state("catalog.json")["catalog"];
}

const Json* format(const Json& cat, const std::string& id) {
  for (const Json& f : cat.items()) {
    if (f["id"].str() == id) return &f;
  }
  return nullptr;
}

/// Converts `input` (already in the session's input folder) to `target`
/// with the given setOption steps; returns the saved bytes.
Bytes convert(Session& s, const std::string& input, const std::string& target, const Json& option_steps,
              const std::string& tag) {
  Json steps = Json::array();
  Json files = Json::array();
  files.push_back(s.in(input));
  steps.push_back(step({{"op", "open"}, {"files", files}}));
  steps.push_back(step({{"op", "setTarget"}, {"index", 0}, {"target", target}}));
  for (const Json& o : option_steps.items()) steps.push_back(o);
  steps.push_back(step({{"op", "convertAll"}}));
  steps.push_back(step({{"op", "waitIdle"}}));
  steps.push_back(step({{"op", "saveAll"}, {"dir", tag}, {"expect", 1}}));
  steps.push_back(step({{"op", "dump"}, {"path", tag + ".json"}}));
  REQUIRE(s.ok(s.run_inline(steps)));
  const Json st = s.state(tag + ".json");
  REQUIRE_MESSAGE(at(st, "items.0.status").str() == "done", at(st, "items.0.errorMessage").str());
  const auto names = list_dir(s.out(tag));
  REQUIRE(names.size() == 1u);
  return saved(s, tag + "/" + names[0]);
}

}  // namespace

TEST_CASE("e2e/catalog/PNG to JPEG starts with the JPEG markers and lower quality is smaller") {
  Session s("catalog-jpeg");
  const Json cat = catalogue(s);
  const Json* png = format(cat, "png");
  if (!png || !(*png)["readable"].as_bool()) SKIP("no PNG reader in this catalogue (image formats not present)");
  s.write_input("photo.png", make_png(160, 120, 21));
  const Discovery d = discover(s, s.in("photo.png"), "png");
  std::string jpeg;
  for (const auto& t : d.targets) {
    if (t == "jpg" || t == "jpeg") jpeg = t;
  }
  if (jpeg.empty()) SKIP("PNG cannot become JPEG in this catalogue");
  // Quality through the preset when offered, else a numeric "quality" option.
  const Json& item = d.by_target.at(jpeg);
  Json high = Json::array();
  Json low = Json::array();
  if (item["hasPreset"].as_bool()) {
    high.push_back(step({{"op", "setPreset"}, {"index", 0}, {"preset", "high"}}));
    low.push_back(step({{"op", "setPreset"}, {"index", 0}, {"preset", "small"}}));
  }
  for (const Json& spec : item["optionSpecs"].items()) {
    if (spec["key"].str() == "quality" && spec.contains("min") && spec.contains("max")) {
      high.push_back(step({{"op", "setOption"}, {"index", 0}, {"key", "quality"}, {"value", spec["max"].as_number()}}));
      low.push_back(step({{"op", "setOption"}, {"index", 0}, {"key", "quality"}, {"value", std::max(spec["min"].as_number(), 5.0)}}));
    }
  }
  if (high.size() == 0) SKIP("the JPEG target offers no quality control");
  const Bytes hq = convert(s, "photo.png", jpeg, high, "high");
  const Bytes lq = convert(s, "photo.png", jpeg, low, "low");
  for (const Bytes* b : {&hq, &lq}) {
    REQUIRE(b->size() > 4u);
    CHECK((*b)[0] == 0xFF);
    CHECK((*b)[1] == 0xD8);
    CHECK((*b)[2] == 0xFF);
    CHECK((*b)[b->size() - 2] == 0xFF);
    CHECK((*b)[b->size() - 1] == 0xD9);
  }
  CHECK(lq.size() < hq.size());
}

TEST_CASE("e2e/catalog/a file compressed to ZIP is a valid archive holding the input") {
  Session s("catalog-zip");
  const Json cat = catalogue(s);
  const Json* zip = format(cat, "zip");
  if (!zip || !(*zip)["writable"].as_bool()) SKIP("no ZIP writer in this catalogue (archive formats not present)");
  struct Candidate {
    std::string name;
    Bytes data;
  };
  const std::vector<Candidate> candidates = {{"notes.txt", to_bytes("Archive me.\nTwice: Archive me.\n")},
                                             {"blob.bin", pseudo_random(20000, 77)},
                                             {"photo.png", make_png(48, 32, 4)}};
  const Candidate* chosen = nullptr;
  for (const auto& c : candidates) {
    s.write_input(c.name, c.data);
    const Discovery d = discover(s, s.in(c.name), c.name);
    if (contains(d.item["targetIds"], "zip")) {
      chosen = &c;
      break;
    }
  }
  if (!chosen) SKIP("none of the generated inputs can be compressed to ZIP");
  INFO("input ", chosen->name);
  const Bytes out = convert(s, chosen->name, "zip", Json::array(), "zip");
  REQUIRE(out.size() > 22u);
  CHECK(out[0] == 'P');
  CHECK(out[1] == 'K');
  const auto entries = parse_zip(out);
  REQUIRE_MESSAGE(entries.has_value(), "the ZIP central directory does not parse");
  REQUIRE_FALSE(entries->empty());
  const ZipEntry* hit = nullptr;
  for (const auto& e : *entries) {
    if (e.uncompressed == chosen->data.size() && e.crc == crc32(chosen->data)) hit = &e;
  }
  REQUIRE_MESSAGE(hit != nullptr, "no entry with the input's size and CRC-32");
  CHECK((hit->method == 0 || hit->method == 8));
  CHECK_FALSE(hit->name.empty());
  if (hit->method == 0) {
    // Stored: the bytes after the local header are the input itself.
    const std::size_t p = static_cast<std::size_t>(hit->local_offset);
    const std::size_t start = p + 30 + (out[p + 26] | (out[p + 27] << 8)) + (out[p + 28] | (out[p + 29] << 8));
    CHECK(Bytes(out.begin() + static_cast<std::ptrdiff_t>(start),
                out.begin() + static_cast<std::ptrdiff_t>(start + chosen->data.size())) == chosen->data);
  }
}

TEST_CASE("e2e/catalog/the suite's own ZIP and PNG writers and readers are sound") {
  // The ZIP check above is only as good as parse_zip: feed it a stored
  // archive built here byte by byte, then a damaged copy.
  const Bytes body = to_bytes("zip body\n");
  const std::uint32_t crc = crc32(body);
  Bytes z;
  const auto le16 = [&](std::uint32_t v) {
    z.push_back(static_cast<std::uint8_t>(v));
    z.push_back(static_cast<std::uint8_t>(v >> 8));
  };
  const auto le32 = [&](std::uint32_t v) {
    le16(v & 0xFFFF);
    le16(v >> 16);
  };
  const std::string name = "a.txt";
  le32(0x04034b50); le16(20); le16(0); le16(0); le16(0); le16(0);
  le32(crc); le32(static_cast<std::uint32_t>(body.size())); le32(static_cast<std::uint32_t>(body.size()));
  le16(static_cast<std::uint32_t>(name.size())); le16(0);
  z.insert(z.end(), name.begin(), name.end());
  z.insert(z.end(), body.begin(), body.end());
  const auto cd = static_cast<std::uint32_t>(z.size());
  le32(0x02014b50); le16(20); le16(20); le16(0); le16(0); le16(0); le16(0);
  le32(crc); le32(static_cast<std::uint32_t>(body.size())); le32(static_cast<std::uint32_t>(body.size()));
  le16(static_cast<std::uint32_t>(name.size())); le16(0); le16(0); le16(0); le16(0); le32(0); le32(0);
  z.insert(z.end(), name.begin(), name.end());
  const auto cd_size = static_cast<std::uint32_t>(z.size()) - cd;
  le32(0x06054b50); le16(0); le16(0); le16(1); le16(1); le32(cd_size); le32(cd); le16(0);
  const auto entries = parse_zip(z);
  REQUIRE(entries.has_value());
  REQUIRE(entries->size() == 1u);
  CHECK((*entries)[0].name == "a.txt");
  CHECK((*entries)[0].crc == crc);
  CHECK((*entries)[0].uncompressed == body.size());
  Bytes broken = z;
  broken[cd] = 'X';  // central directory signature
  CHECK_FALSE(parse_zip(broken).has_value());
  CHECK_FALSE(parse_zip(Bytes(z.begin(), z.end() - 5)).has_value());

  // The generated PNG is what the app reads as PNG (when the catalogue has it).
  Session s("catalog-selftest");
  const Json cat = catalogue(s);
  if (format(cat, "png")) {
    s.write_input("gen.png", make_png(20, 10, 1));
    const Discovery d = discover(s, s.in("gen.png"), "gen");
    CHECK(d.item["source"].str() == "png");
  }
}

TEST_CASE("e2e/catalog/a WAV recording converts to another audio container") {
  Session s("catalog-audio");
  const Json cat = catalogue(s);
  const Json* wav = format(cat, "wav");
  if (!wav || !(*wav)["readable"].as_bool()) SKIP("no WAV reader in this catalogue (audio formats not present)");
  s.write_input("tone.wav", make_wav(16000, 1.0, 440.0));
  const Discovery d = discover(s, s.in("tone.wav"), "wav");
  std::string target;
  for (const auto& t : d.targets) {
    const Json* f = format(cat, t);
    if (target.empty() && f && (*f)["category"].str() == "audio" && t != "wav") target = t;
  }
  if (target.empty()) SKIP("WAV has no other audio target in this catalogue");
  INFO("target ", target);
  const Bytes out = convert(s, "tone.wav", target, Json::array(), "audio");
  REQUIRE(out.size() > 16u);
  const std::string head(out.begin(), out.begin() + 4);
  if (target == "flac") CHECK(head == "fLaC");
  else if (target == "ogg" || target == "opus") CHECK(head == "OggS");
  else if (target == "aiff") CHECK(head == "FORM");
  else if (target == "mp3") CHECK((head.rfind("ID3", 0) == 0 || (out[0] == 0xFF && (out[1] & 0xE0) == 0xE0)));
  else MESSAGE("no container magic known for ", target, "; checked that it converted");
}
