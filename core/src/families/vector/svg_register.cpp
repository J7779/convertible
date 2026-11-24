// Convertible core - vector family: SVG and SVGZ registration.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <string>
#include <utility>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/vector/svg.hpp"
#include "families/vector/vector.hpp"
#include "families/vector/vector_codecs.hpp"
#include "util/deflate.hpp"
#include "util/text.hpp"

namespace cvt::vector {

namespace {

bool is_gzip(ByteView data) {
  return data.size() >= 3 && data[0] == std::byte{0x1F} && data[1] == std::byte{0x8B} && data[2] == std::byte{0x08};
}

// The decoded start of a text file (UTF-8 or UTF-16), for sniffing.
std::string text_head(ByteView data, std::size_t limit) {
  data = data.first(std::min(data.size(), limit));
  if (data.size() >= 2 && ((data[0] == std::byte{0xFF} && data[1] == std::byte{0xFE}) ||
                           (data[0] == std::byte{0xFE} && data[1] == std::byte{0xFF}))) {
    const bool le = data[0] == std::byte{0xFF};
    std::string out;
    for (std::size_t i = 2; i + 1 < data.size(); i += 2) {
      const auto lo = std::to_integer<unsigned>(data[le ? i : i + 1]);
      const auto hi = std::to_integer<unsigned>(data[le ? i + 1 : i]);
      out += hi == 0 ? static_cast<char>(lo) : '?';
    }
    return out;
  }
  return std::string(as_chars(data));
}

int sniff_svg_text(std::string_view head) {
  // Skip BOM, XML declaration, comments and doctype; the root element must be <svg.
  std::size_t i = 0;
  if (head.starts_with("\xEF\xBB\xBF")) i = 3;
  while (i < head.size()) {
    while (i < head.size() && util::is_ascii_space(head[i])) ++i;
    if (head.compare(i, 4, "<!--") == 0) {
      const std::size_t end = head.find("-->", i);
      if (end == std::string_view::npos) return 0;
      i = end + 3;
    } else if (head.compare(i, 2, "<?") == 0) {
      const std::size_t end = head.find("?>", i);
      if (end == std::string_view::npos) return 0;
      i = end + 2;
    } else if (head.compare(i, 9, "<!DOCTYPE") == 0) {
      // May contain an internal subset with '>' inside brackets.
      std::size_t j = i + 9;
      int bracket = 0;
      while (j < head.size()) {
        if (head[j] == '[') ++bracket;
        else if (head[j] == ']') --bracket;
        else if (head[j] == '>' && bracket <= 0) break;
        ++j;
      }
      if (j >= head.size()) return head.find("<svg") != std::string_view::npos ? 70 : 0;
      i = j + 1;
    } else {
      break;
    }
  }
  if (head.compare(i, 4, "<svg") == 0 && i + 4 < head.size() &&
      (util::is_ascii_space(head[i + 4]) || head[i + 4] == '>' || head[i + 4] == '/')) {
    return 95;
  }
  if (head.compare(i, 8, "<svg:svg") == 0) return 95;
  return 0;
}

Result<Bytes> gunzip(ByteView input, const Context& ctx) {
  auto out = util::gzip_decompress(input, ctx.limits.max_decompressed_bytes);
  if (!out) {
    if (out.error().code == Status::Limit) return out;
    return fail(Status::Decode, "the compressed SVG is damaged: {}", out.error().message);
  }
  return out;
}

svg::WriteOptions write_options_from(const Options& o) {
  svg::WriteOptions w;
  w.precision = static_cast<int>(o.get_int("precision", 3, -1, 10));
  w.minify = o.get_bool("minify", false);
  w.text_as_text = o.get_string("svg_text", "text") == "text";
  w.keep_metadata = !options::resolved_strip_metadata(o);
  w.keep_ids = w.keep_metadata;
  return w;
}

std::vector<OptionSpec> writer_options() {
  return {
      OptionSpec::integer("precision", "Precision", 3, -1, 10,
                          "Decimals kept for coordinates; fewer gives smaller files (-1 keeps full precision)."),
      OptionSpec::boolean("minify", "Minify", false,
                          "Write the smallest file: no indentation, shortest path syntax, short ids."),
      OptionSpec::choice("svg_text", "Text", "text", {"text", "outlines"},
                         "text keeps text editable and searchable; outlines draws it as shapes that look the same "
                         "everywhere."),
  };
}

}  // namespace

void register_svg(Registry& registry) {
  for (const bool compressed : {false, true}) {
    VectorFormat f;
    f.info.id = compressed ? "svgz" : "svg";
    f.info.name = compressed ? "Compressed SVG" : "SVG vector image";
    f.info.description = compressed ? "Scalable Vector Graphics compressed with gzip."
                                    : "Scalable Vector Graphics: the web's vector image format.";
    f.info.category = Category::Vector;
    f.info.extensions = {compressed ? "svgz" : "svg"};
    f.info.mime = compressed ? "image/svg+xml-compressed" : "image/svg+xml";
    f.info.popularity = compressed ? 20 : 85;
    if (compressed) {
      f.info.sniff = [](std::span<const std::byte> data, std::string_view filename) -> int {
        if (!is_gzip(data)) return 0;
        // Inflate a little and look for the root element.
        util::Inflater inflater(util::Inflater::Format::Gzip);
        Bytes out;
        std::size_t pos = 0;
        while (pos < data.size() && out.size() < 4096) {
          const std::size_t n = std::min<std::size_t>(2048, data.size() - pos);
          auto progress = inflater.feed(data.subspan(pos, n), out, 4096);
          if (!progress) break;
          pos += n;
          if (progress->finished) break;
        }
        if (sniff_svg_text(text_head(out, 4096)) > 0) return 96;
        return util::iends_with(filename, ".svgz") ? 60 : 0;
      };
    } else {
      f.info.sniff = [](std::span<const std::byte> data, std::string_view /*filename*/) {
        return sniff_svg_text(text_head(data, 4096));
      };
    }
    f.read = [compressed](ReadContext& ctx) -> Result<Scene> {
      svg::ReadOptions options;
      if (compressed || is_gzip(ctx.input)) {
        CVT_TRY_ASSIGN(const Bytes plain, gunzip(ctx.input, ctx));
        return svg::read_svg(plain, ctx, options);
      }
      return svg::read_svg(ctx.input, ctx, options);
    };
    f.write = [compressed](const Scene& scene, WriteContext& ctx) -> Result<Bytes> {
      CVT_TRY_ASSIGN(const std::string text, svg::write_svg(scene, ctx, write_options_from(ctx.options)));
      if (!compressed) return to_bytes(text);
      return util::gzip_compress(as_bytes(text), options::resolved_compression_level(ctx.options, 9), util::GzipHeader{});
    };
    f.write_options = writer_options();
    if (compressed) f.write_options.push_back(options::compression_level(9));
    f.read_cost = 10;
    f.write_cost = 10;
    register_vector_format(registry, std::move(f));
  }
}

}  // namespace cvt::vector
