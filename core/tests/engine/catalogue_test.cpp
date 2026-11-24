// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
//
// The catalogue policy (top of src/engine/known_formats.cpp and core/README.md,
// "Format ids"): one canonical id per format across the detection database
// and every family. These tests fail on any future disagreement, so a family
// that registers a second spelling of a known format, or a database row that
// duplicates a family's format, is caught when it is added.
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "engine/known_formats_table.hpp"
#include "engine/planner.hpp"
#include "engine/registry.hpp"
#include "support/test_support.hpp"
#include "util/text.hpp"

using namespace cvt;

namespace {

bool has_content_test(const FormatInfo& f) { return !f.signatures.empty() || static_cast<bool>(f.sniff); }

// Pairs of different ids that legitimately share a name (none today). Keep
// this list empty unless two genuinely different formats are known by the
// same words.
const std::set<std::pair<std::string_view, std::string_view>> same_name_allowed = {};

// A database row whose id is listed as an extension of a family format of the
// same category is normally the same format under a second id ("dcm" next to
// a family's "dicom"). These rows are different formats that happen to use
// that extension, each with its own content test.
const std::set<std::pair<std::string_view, std::string_view>> extension_id_allowed = {
    {"ar", "lib"},                    // a Windows import library is not a Unix ar archive in general
    {"tiff", "geotiff"},              // GeoTIFF: TIFF tags of its own, detected by them
    {"tiff", "bigtiff"},              // BigTIFF: 64-bit offsets, a structure of its own
    {"h5", "hdf"},                    // .hdf is also HDF4, a different file format
    {"r", "rmd"},                     // R Markdown: Markdown with R chunks
    {"usda", "usd"},                  // .usd may be text or the binary crate format
    {"nc", "cdf"},                    // NASA's Common Data Format is not NetCDF
    {"arrow", "feather"},             // Feather v1 predates the Arrow IPC file
    {"squashfs", "snap"},             // a Snap package: SquashFS with metadata of its own
    {"xar", "pkg"},                   // .pkg is also a PlayStation package
    {"openstep-plist", "strings"},    // .strings files are also UTF-16 binary property lists
    {"vb", "vbs"},                    // VBScript and BASIC are languages of their own
    {"vb", "bas"},
    {"ini", "inf"},                   // Windows setup information: INI syntax, own semantics
    {"mpegts", "m2ts"},               // 192-byte packets (Blu-ray, AVCHD): a structure of its own
    {"mid", "rmi"},                   // MIDI wrapped in RIFF
    {"wk1", "wks"},                   // Lotus 1-2-3 release 1A: an older worksheet structure
};

}  // namespace

TEST_CASE("catalogue: the shared registry validates") {
  const auto problems = Registry::instance().validate();
  for (const std::string& problem : problems) MESSAGE(problem);
  CHECK(problems.empty());
}

TEST_CASE("catalogue: one name, one id") {
  std::map<std::string, std::string> by_name;
  for (const FormatInfo* format : Registry::instance().formats()) {
    const std::string key = util::to_lower(format->name);
    const auto [it, inserted] = by_name.emplace(key, format->id);
    if (inserted) continue;
    INFO("'", format->name, "' is the name of '", it->second, "' and '", format->id, "'");
    CHECK(same_name_allowed.contains({it->second, format->id}));
  }
}

TEST_CASE("catalogue: an extension never belongs to two formats without content tests") {
  std::map<std::string, std::string> untested;
  for (const FormatInfo* format : Registry::instance().formats()) {
    if (format->output_only || has_content_test(*format)) continue;
    for (const std::string& extension : format->extensions) {
      const auto [it, inserted] = untested.emplace(extension, format->id);
      INFO("extension '", extension, "' of '", it->second, "' and '", format->id, "'");
      CHECK(inserted);
    }
  }
}

TEST_CASE("catalogue: aliases are extensions, never second ids") {
  const Registry& registry = Registry::instance();
  for (const FormatInfo* family : registry.formats()) {
    if (!registry.described_by_family(family->id)) continue;
    for (const std::string& extension : family->extensions) {
      if (extension == family->id) continue;
      const FormatInfo* other = registry.find(extension);
      if (other == nullptr || registry.described_by_family(other->id) || other->category != family->category) continue;
      INFO("database row '", other->id, "' is an extension of family format '", family->id, "'");
      CHECK(extension_id_allowed.contains({family->id, other->id}));
    }
  }
}

TEST_CASE("catalogue: no format is recognised by a bare shared container magic") {
  // Formats that share a container are told apart by sniff::classify (or a
  // family sniff that looks inside); a bare container magic as a signature
  // would make every ZIP look like a JAR, every MP4 like an M4A.
  struct Container {
    std::size_t offset;
    std::string_view magic;
    std::string_view owner;  // the container format itself may use it
  };
  using namespace std::string_view_literals;
  const Container containers[] = {
      {0, "PK\x03\x04"sv, "zip"},
      {0, "PK\x05\x06"sv, "zip"},
      {0, "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1"sv, ""},
      {4, "ftyp"sv, ""},
      {0, "\x30\x26\xB2\x75\x8E\x66\xCF\x11"sv, ""},
      {0, "RIFF"sv, ""},
      {0, "\x1A\x45\xDF\xA3"sv, ""},
      {0, "OggS"sv, ""},
      {0, "\x1F\x8B"sv, "gz"},
      {0, "SQLite format 3\x00"sv, ""},
      {0, "II*\x00"sv, "tiff"},
      {0, "MM\x00*"sv, "tiff"},
  };
  for (const FormatInfo* format : Registry::instance().formats()) {
    for (const Signature& signature : format->signatures) {
      for (const Container& c : containers) {
        if (signature.offset != c.offset || signature.bytes.size() > c.magic.size() || format->id == c.owner) continue;
        // The signature constrains nothing beyond (a prefix of) the container magic.
        bool inside = true;
        for (std::size_t i = 0; i < signature.bytes.size(); ++i) {
          const std::byte mask = signature.mask.empty() ? std::byte{0xFF} : signature.mask[i];
          inside = inside && (signature.bytes[i] & mask) == (static_cast<std::byte>(c.magic[i]) & mask);
        }
        INFO("format '", format->id, "' uses the bare container magic at offset ", c.offset);
        CHECK_FALSE(inside);
      }
    }
  }
}

TEST_CASE("catalogue: output-only formats are targets only") {
  const Registry& registry = Registry::instance();
  std::size_t count = 0;
  for (const FormatInfo* format : registry.formats()) {
    if (!format->output_only) continue;
    ++count;
    INFO(format->id);
    CHECK(registry.has_writer(format->id));
    CHECK(list_targets(registry, format->id).empty());
  }
  CHECK(count >= 5);  // barcodes, minified/pretty code, csv-semicolon
}
