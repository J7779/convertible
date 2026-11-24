// Every target the planner offers for a representative set of real files
// converts, with the option schema's default values - the product's core
// promise: a target that is offered must work. Files are fixtures written by
// independent software (tests/data/detect/, see its SOURCES.md).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "engine/detect.hpp"
#include "engine/planner.hpp"
#include "integration/integration_support.hpp"
#include "util/json.hpp"

using namespace cvt;
using namespace cvt::test::integration;

namespace {

// The representative sources of the catalogue review (one per row of the
// brief's list, plus a few neighbours).
constexpr std::string_view sources[] = {
    "detect/text/plain.txt",       "detect/text/notes.md",           "detect/office/document.html",
    "office/libreoffice/sample.docx", "detect/office/document.pdf",  "detect/image/pattern.png",
    "detect/image/pattern.jpg",    "detect/image/anim.gif",          "vector/svg/paint.svg",
    "detect/audio/tone.wav",       "detect/audio/tone.mp3",          "detect/audio/tone.flac",
    "video/h264_aac.mp4",          "video/mkvmerge_h264.mkv",        "detect/text/table.csv",
    "detect/office/spreadsheet.xlsx", "detect/text/data.json",       "detect/text/config.yaml",
    "detect/archive/archive.zip",  "detect/archive/archive.tar.gz",  "detect/archive/hello.txt.gz",
    "detect/text/cube.obj",        "detect/text/part.stl",           "detect/text/subtitles.srt",
    "detect/text/event.ics",       "detect/text/contact.vcf",        "detect/font/sample.ttf",
    "detect/text/tool.py",         "detect/text/drawing.dxf",        "office/libreoffice/sample_impress.pptx",
    "office/pandoc/sample.epub",
};

// Conversions that depend on content these particular files do not have.
// They are offered because files of the source format often have it: a
// Markdown file with a table converts to a spreadsheet, a DXF with 3D faces
// to a mesh, text that holds digits to an EAN-13 barcode. Matched on the
// source category and the error message, each with its reason.
struct ContentDependent {
  std::string_view message;  // part of the error message
  std::string_view reason;
};
constexpr ContentDependent content_dependent[] = {
    {"no table was found", "a document without a table has nothing to put into a spreadsheet"},
    {"contains no tables", "a document without a table has nothing to put into a spreadsheet"},
    {"no 3D faces", "a 2D drawing has no mesh; DXF files with 3DFACE, polyface or MESH entities convert"},
    {"takes digits only", "EAN-13 encodes digits"},
    {"needs 11 digits", "UPC-A encodes 11 or 12 digits"},
    {"is not a colour", "the colour tool explains a colour value"},
    {"not an RFC 2822 date", "the date tool explains a date"},
    {"neither a UUID nor a ULID", "the UUID tool explains an identifier"},
    {"is not allowed in a host name", "IDNA converts host names"},
    {"starts or ends with '-'", "IDNA converts host names"},
    {"is not numeric", "a spectrum is a table of numbers"},
    {"no numeric array", "a volume is an array of numbers"},
    {"is not a field number", "Protocol Buffers data has numbered fields"},
    {"must be a scalar", "an .editorconfig holds flat settings"},
    {"nested objects cannot be written", "an .npmrc holds flat settings"},
    {"multiple of 4 bytes", "Z85 (ZeroMQ) encodes data in whole 4-byte groups"},
    {"meant for short payloads", "Bech32 is for short payloads (addresses, keys)"},
    {"no opaque pixels", "a palette needs visible colours; this drawing renders as thin lines on transparency"},
    {"no pictures to put into the comic", "a document without pictures has no comic pages"},
};

// Encodes to video formats take seconds each: a sample per source.
bool slow(const FormatInfo& target) { return target.category == Category::Video; }

// Options of one route share one flat namespace: a key declared by two steps
// with different meanings would hand one of them the other's default.
std::string option_clash(const Plan& plan) {
  std::vector<const std::vector<OptionSpec>*> steps;
  if (plan.reader != nullptr) steps.push_back(&plan.reader->read_options);
  for (const Bridge* bridge : plan.bridges) steps.push_back(&bridge->options);
  if (plan.writer != nullptr) steps.push_back(&plan.writer->write_options);
  std::map<std::string, const OptionSpec*> seen;
  for (const auto* specs : steps) {
    for (const OptionSpec& spec : *specs) {
      const auto [it, inserted] = seen.emplace(spec.key, &spec);
      if (inserted) continue;
      const OptionSpec& first = *it->second;
      // The same concept may be declared by two steps with different defaults
      // (a background colour); a different type or choice list is a clash.
      if (first.type != spec.type || first.choices != spec.choices) {
        return "option '" + spec.key + "' means two things on this route";
      }
    }
  }
  return {};
}

}  // namespace

TEST_CASE("integration/targets: every offered target of real files converts with default options") {
  const Registry& registry = Registry::instance();
  std::vector<std::string> failures;
  std::size_t tried = 0;
  std::size_t content = 0;
  for (const std::string_view file : sources) {
    const Bytes data = test::fixture(std::string(file));
    const std::string name(file.substr(file.rfind('/') + 1));
    const auto detected = detect_format(registry, data, name);
    REQUIRE(detected.has_value());
    const FormatInfo& source = *detected->format;
    std::size_t video = 0;
    for (const TargetInfo& target : list_targets(registry, source.id)) {
      if (slow(*target.format) && ++video > 2) continue;
      const auto plan = plan_conversion(registry, source.id, target.format->id);
      REQUIRE_OK(plan);
      if (const std::string clash = option_clash(*plan); !clash.empty()) {
        failures.push_back(source.id + " -> " + target.format->id + ": " + clash);
      }
      // The schema's defaults, exactly as an app's form would send them.
      Json options = Json::object();
      for (const OptionSpec& spec : plan->option_specs()) options[spec.key] = spec.default_value;
      const test::Converted out = test::convert(data, source.id, target.format->id, util::dump_json(options), name);
      ++tried;
      if (out.ok() && !out.data.empty()) continue;
      const bool expected = std::ranges::any_of(content_dependent, [&](const ContentDependent& c) {
        return out.message.find(c.message) != std::string::npos;
      });
      if (expected) {
        ++content;
        continue;
      }
      failures.push_back(source.id + " -> " + target.format->id + ": " +
                        (out.ok() ? std::string("empty output") : std::string(status_name(out.status)) + ", " + out.message));
    }
  }
  for (const std::string& failure : failures) MESSAGE(failure);
  MESSAGE(tried, " conversions, ", content, " depend on content these files lack, ", failures.size(), " failed");
  CHECK(failures.empty());
}
