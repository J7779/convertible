// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/detect.hpp"

#include <algorithm>

#include "util/text.hpp"

namespace cvt {

namespace {

// The extension is all we can go on (the format declares no content test).
// Deliberately above a 4-byte magic (75): files such as .cbz, .kmz or .gpx
// are ZIP or XML underneath, and the specific format should win over the
// generic container. Longer magics still beat it.
constexpr int extension_only_confidence = 78;
// The extension matches but the format's own content tests all failed.
constexpr int mismatched_extension_confidence = 25;
constexpr int text_confidence = 30;
constexpr int extension_bonus = 5;

// 60 for a one-byte magic, +5 per further constrained byte, capped at 95
// (2 bytes 65, 4 bytes 75, 8 or more bytes 95).
int signature_confidence(const Signature& signature) {
  const auto bytes = static_cast<int>(signature.significant_bits() / 8);
  return std::min(95, 55 + 5 * std::max(bytes, 1));
}

}  // namespace

std::vector<std::string> filename_extensions(std::string_view filename) {
  std::vector<std::string> out;
  // Only the last path component matters.
  if (const std::size_t slash = filename.find_last_of("/\\"); slash != std::string_view::npos) {
    filename.remove_prefix(slash + 1);
  }
  // Leading dots belong to the name (".gitignore" has no extension).
  std::size_t start = 0;
  while (start < filename.size() && filename[start] == '.') ++start;
  for (std::size_t dot = filename.find('.', start); dot != std::string_view::npos; dot = filename.find('.', dot + 1)) {
    const std::string_view extension = filename.substr(dot + 1);
    if (!extension.empty() && extension.size() <= 16 && extension.find(' ') == std::string_view::npos) {
      out.push_back(util::to_lower(extension));
    }
  }
  return out;
}

std::vector<Detection> detect_candidates(const Registry& registry, ByteView data, std::string_view filename) {
  const std::vector<std::string> extensions = filename_extensions(filename);
  std::vector<Detection> candidates;

  for (const FormatInfo* format : registry.formats()) {
    int content = 0;
    for (const Signature& signature : format->signatures) {
      if (signature.matches(data)) content = std::max(content, signature_confidence(signature));
    }
    if (format->sniff && !data.empty()) {
      content = std::max(content, std::clamp(format->sniff(data, filename), 0, 100));
    }

    // Longer (compound) extensions are more specific: "tar.gz" beats "gz".
    int extension_rank = 0;
    for (std::size_t i = 0; i < extensions.size(); ++i) {
      if (std::ranges::find(format->extensions, extensions[i]) != format->extensions.end()) {
        extension_rank = static_cast<int>(extensions.size() - i);
        break;
      }
    }

    int confidence = 0;
    if (content > 0) {
      confidence = std::min(100, content + (extension_rank > 0 ? extension_bonus + extension_rank - 1 : 0));
    } else if (extension_rank > 0) {
      const bool has_content_test = !format->signatures.empty() || static_cast<bool>(format->sniff);
      confidence = (has_content_test && !data.empty() ? mismatched_extension_confidence : extension_only_confidence) +
                   extension_rank - 1;
    }
    if (confidence > 0) candidates.push_back({format, confidence});
  }

  if (!data.empty() && util::looks_like_text(data)) {
    if (const FormatInfo* text = registry.find("txt")) {
      const auto existing = std::ranges::find(candidates, text, &Detection::format);
      if (existing == candidates.end()) {
        candidates.push_back({text, text_confidence});
      } else {
        existing->confidence = std::max(existing->confidence, text_confidence);
      }
    }
  }

  std::ranges::stable_sort(candidates, [&registry](const Detection& a, const Detection& b) {
    if (a.confidence != b.confidence) return a.confidence > b.confidence;
    // A format a family implements beats a detection-only database entry.
    const bool a_family = registry.described_by_family(a.format->id);
    const bool b_family = registry.described_by_family(b.format->id);
    if (a_family != b_family) return a_family;
    if (a.format->popularity != b.format->popularity) return a.format->popularity > b.format->popularity;
    return a.format->id < b.format->id;
  });
  return candidates;
}

std::optional<Detection> detect_format(const Registry& registry, ByteView data, std::string_view filename) {
  const std::vector<Detection> candidates = detect_candidates(registry, data, filename);
  if (candidates.empty()) return std::nullopt;
  return candidates.front();
}

}  // namespace cvt
