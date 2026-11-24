// Convertible core - encoding family: the text encoding report.
//
// Targets registered here:
//   charset-detect   a report of the plausible encodings of any file
// The charsets themselves (util/charsets.hpp) are offered through the `txt`
// reader and writer (options source_encoding / encoding, see txt.cpp).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "engine/registry.hpp"
#include "families/encoding/charsets.hpp"
#include "families/encoding/encoding.hpp"
#include "families/encoding/simple.hpp"

namespace cvt::encoding {

namespace {

int sniff_report(ByteView data, std::string_view) {
  const std::string_view text = as_chars(data).substr(0, 64);
  return text.starts_with("Most likely encoding: ") ? 85 : 0;
}

}  // namespace

void register_charsets(Registry& registry) {
  add_binary_encoding(registry, {
      .id = "charset-detect", .name = "Text encoding report",
      .description = "The most likely character encodings of a text file, with a confidence for each.",
      .extensions = {"txt", "charset"}, .popularity = 20,
      .encode = [](const ir::Blob& blob, const WriteContext& ctx) -> Result<std::string> {
        CVT_TRY(ctx.checkpoint(0.0, "Detecting"));
        const std::vector<CharsetGuess> guesses = detect_charsets(blob.bytes);
        std::string out = std::format("Most likely encoding: {} ({}%)\n", guesses.front().id, guesses.front().confidence);
        for (std::size_t i = 1; i < guesses.size(); ++i) out += std::format("Also possible: {} ({}%)\n", guesses[i].id, guesses[i].confidence);
        return out;
      },
      .expansion = 0,
      .sniff = sniff_report,
  });
}

}  // namespace cvt::encoding
