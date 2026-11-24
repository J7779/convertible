// Convertible core - detection-only database of the world's file formats.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
//
// This database lets `cvt_detect` and the format list name any file, even
// one no family can convert (yet). It never overrides a family: when a family
// registers the same id, the family's description wins and this database
// only fills the fields the family left empty; once a family says how to
// recognise a format (signatures or a sniff function), the database's
// detection for that id is dropped.
//
// CANONICAL IDS (families must use the same spelling for the same format):
//   * lower case, [a-z0-9._+-], at most 32 characters;
//   * the format's dominant file extension without the dot: "jpg" not
//     "jpeg", "tiff" not "tif", "mp4", "html", "md", "yaml", "ttf", "mid";
//     every other common extension is listed as an alias (first = default);
//   * compound formats get compound ids: "tar.gz", "tar.xz", "nii.gz",
//     "ome.tiff", "pkg.tar.zst";
//   * when two genuinely different formats share an extension, the more
//     common one keeps the plain id and the other gets a descriptive one
//     ("ts" is TypeScript, the MPEG transport stream is "mpegts"; "vcf" is
//     vCard, the genomics format is "vcf-variant"; "asc" is ASCII-armoured
//     OpenPGP, the ESRI grid is "asc-grid");
//   * variants that are the same format under another name stay one id
//     (m4b, m4p, m4r are "m4a"; jfif is "jpg"; aif/aifc are "aiff"), while
//     variants with their own structure get their own id (docm, dotx, xlsb,
//     m2ts, bigtiff, apng);
//   * ids that existed before this policy keep their spelling: "macho",
//     "exe", "elf", "sqlite", "mpg", "mid", "aiff", "ics", "vcf", "ts".
//
// EACH ROW (see known_formats_table.hpp):
//   id, human name, category, extensions (space separated, lower case, the
//   first is the default for output files), MIME type (the IANA registration
//   where one exists, otherwise the type in common use, usually
//   application/x-...; empty for application/octet-stream), popularity 0..100
//   (how often ordinary users meet the format: png/pdf/zip ~95, everyday
//   formats 60-85, specialist 20-50, rare 1-20), a one-line description, up
//   to 8 signatures and an optional sniffer.
//
// SIGNATURES come from the format's specification or were checked against
// files written by independent software (see core/tests/data/detect/). A
// format without reliable magic has no signature: it is recognised by a
// structured sniffer (sniffers.hpp) or by extension only. Formats that share
// a container (ZIP, OLE2, ISO BMFF, RIFF, EBML, Ogg, TIFF, XML, JSON, gzip)
// are told apart by the sniffers, never by a bare container magic that would
// make every ZIP look like the most popular ZIP-based format.
//
// Two formats may list the same extension only if at most one of them has no
// content test (Registry::validate() checks this); the tests in
// core/tests/engine/known_formats_test.cpp check every row.

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/known_formats_sniff_util.hpp"
#include "engine/known_formats_table.hpp"
#include "engine/registry.hpp"
#include "engine/sniffers.hpp"
#include "families/families.hpp"
#include "util/text.hpp"

namespace cvt {

namespace known {

std::span<const std::span<const KnownFormat>> all_parts() noexcept {
  static const std::array<std::span<const KnownFormat>, 7> parts = {
      image_formats(), media_formats(), document_formats(), data_formats(),
      archive_formats(), system_formats(), code_formats(),
  };
  return parts;
}

}  // namespace known

namespace {

// The sniff function of one row: the row's classifiers identify the format,
// or (for rows marked `named`) a valid container plus a matching extension.
struct RowSniffer {
  std::string id;
  std::vector<std::string> extensions;
  known::KnownSniff how;
  // What a matching name is worth. Source code is weaker than other
  // formats: the name of a .h, .py or .js file says less than the content
  // tests of the encoding family (byte-array literals in those languages).
  int name_confidence = sniff::named_confidence;

  [[nodiscard]] bool name_matches(std::string_view filename) const {
    return std::ranges::any_of(extensions, [&](const std::string& e) { return sniff::detail::has_ext(filename, e); });
  }

  int operator()(ByteView data, std::string_view filename) const {
    int best = 0;
    for (const sniff::Family family : {how.family, how.also}) {
      if (family == sniff::Family::None) continue;
      const sniff::Verdict verdict = sniff::classify(family, data, filename, id);
      int confidence = 0;
      if (verdict.id == id) confidence = verdict.confidence;
      // The content is a plain member of the family and the name says which.
      if (how.named && verdict.container && (verdict.id.empty() || verdict.base) && name_matches(filename)) {
        confidence = std::max(confidence, name_confidence);
      }
      best = std::max(best, confidence);
    }
    return best;
  }
};

}  // namespace

void register_known_formats(Registry& registry) {
  for (const std::span<const known::KnownFormat> part : known::all_parts()) {
    for (const known::KnownFormat& row : part) {
      FormatInfo info;
      info.id = std::string(row.id);
      info.name = std::string(row.name);
      info.description = std::string(row.description);
      info.category = row.category;
      for (const std::string_view extension : util::split(row.extensions, ' ', false)) {
        info.extensions.emplace_back(extension);
      }
      info.mime = std::string(row.mime);
      info.popularity = row.popularity;
      for (const known::KnownSignature& signature : row.signatures) {
        if (!signature.pattern.text.empty()) info.signatures.push_back(Signature::at(signature.offset, signature.pattern));
      }
      if (row.sniff.family != sniff::Family::None) {
        info.sniff = RowSniffer{info.id, info.extensions, row.sniff,
                                row.category == Category::Code ? sniff::weak_named_confidence : sniff::named_confidence};
      }
      registry.add_known_format(std::move(info));
    }
  }
}

}  // namespace cvt
