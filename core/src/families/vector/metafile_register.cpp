// Convertible core - vector family: Windows metafiles (wmf, emf, wmz, emz).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>
#include <utility>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/vector/metafile_internal.hpp"
#include "families/vector/vector.hpp"
#include "families/vector/vector_codecs.hpp"
#include "util/deflate.hpp"
#include "util/text.hpp"

namespace cvt::vector {

namespace {

using namespace metafile;

ReadOptions read_options_from(const Options& options) {
  ReadOptions r;
  r.use_emf_plus = options.get_string("emf_plus", "auto") != "off";
  return r;
}

WriteOptions write_options_from(const Options& options) {
  WriteOptions w;
  w.text_as_fonts = options.get_string("metafile_text", "outlines") == "fonts";
  w.gradient_steps = static_cast<int>(options.get_int("gradient_steps", 64, 2, 1024));
  return w;
}

std::vector<OptionSpec> writer_options(bool emf) {
  std::vector<OptionSpec> specs = {
      OptionSpec::choice("metafile_text", "Text", "outlines", {"outlines", "fonts"},
                         "outlines draws text as shapes (looks identical everywhere); fonts writes real text that "
                         "stays editable but needs the font on the viewing system."),
  };
  if (emf) {
    specs.push_back(OptionSpec::integer("gradient_steps", "Gradient steps", 64, 2, 1024,
                                        "Number of colour bands used to draw each gradient.")
                        .advanced());
  }
  return specs;
}

/// Decompress a .wmz / .emz file within the limits.
Result<Bytes> gunzip(ByteView input, const Context& ctx) {
  auto out = util::gzip_decompress(input, ctx.limits.max_decompressed_bytes);
  if (!out) {
    if (out.error().code == Status::Limit) return out;
    return fail(Status::Decode, "the compressed metafile is damaged: {}", out.error().message);
  }
  return out;
}

/// The first bytes of a gzip stream (for detection), or empty.
Bytes gzip_head(ByteView data, std::size_t want) {
  Bytes out;
  if (data.size() < 18 || data[0] != std::byte{0x1F} || data[1] != std::byte{0x8B}) return out;
  util::Inflater inflater(util::Inflater::Format::Gzip);
  // Feed in small pieces and stop as soon as enough output exists.
  std::size_t pos = 0;
  while (pos < data.size() && out.size() < want) {
    const std::size_t n = std::min<std::size_t>(4096, data.size() - pos);
    auto progress = inflater.feed(data.subspan(pos, n), out, want);
    if (!progress) break;
    pos += n;
    if (progress->finished) break;
  }
  return out;
}

int sniff_compressed(std::span<const std::byte> data, std::string_view filename, bool emf) {
  const Bytes head = gzip_head(data, 128);
  if (head.empty()) return 0;
  const bool inner = emf ? looks_like_emf(head) : looks_like_wmf(head);
  if (inner) return 95;
  // A gzip file named .wmz/.emz whose start we could not inspect.
  if (util::iends_with(filename, emf ? ".emz" : ".wmz") && head.size() < 88) return 70;
  return 0;
}

}  // namespace

void register_metafiles(Registry& registry) {
  const auto emf_reader_options = [] {
    return std::vector<OptionSpec>{
        OptionSpec::choice("emf_plus", "EMF+ records", "auto", {"auto", "off"},
                           "auto draws the EMF+ (GDI+) records of dual metafiles; off always draws the plain GDI "
                           "records.")
            .advanced(),
    };
  };

  // ---- WMF
  {
    VectorFormat f;
    f.info.id = "wmf";
    f.info.name = "Windows Metafile";
    f.info.description = "Windows 3.x vector picture made of GDI drawing records, often with a placeable header.";
    f.info.category = Category::Vector;
    f.info.extensions = {"wmf"};
    f.info.mime = "image/wmf";
    f.info.popularity = 45;
    f.info.signatures = {Signature::at(0, "D7 CD C6 9A"), Signature::at(0, "01 00 09 00 00 03"),
                         Signature::at(0, "02 00 09 00 00 03"), Signature::at(0, "01 00 09 00 00 01")};
    f.read = [](ReadContext& ctx) { return read_wmf(ctx.input, ctx, read_options_from(ctx.options)); };
    f.write = [](const Scene& scene, WriteContext& ctx) {
      return write_wmf(scene, ctx, write_options_from(ctx.options));
    };
    f.write_options = writer_options(false);
    f.lossy_read = true;
    f.lossy_write = true;
    f.read_cost = 15;
    f.write_cost = 20;
    register_vector_format(registry, std::move(f));
  }

  // ---- EMF
  {
    VectorFormat f;
    f.info.id = "emf";
    f.info.name = "Enhanced Metafile";
    f.info.description = "32-bit Windows vector picture (EMF), including EMF+ (GDI+) records.";
    f.info.category = Category::Vector;
    f.info.extensions = {"emf"};
    f.info.mime = "image/emf";
    f.info.popularity = 50;
    f.info.signatures = {Signature::at(0,
                                       "01 00 00 00 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? "
                                       "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 20 45 4D 46")};
    f.read = [](ReadContext& ctx) { return read_emf(ctx.input, ctx, read_options_from(ctx.options)); };
    f.write = [](const Scene& scene, WriteContext& ctx) {
      return write_emf(scene, ctx, write_options_from(ctx.options));
    };
    f.read_options = emf_reader_options();
    f.write_options = writer_options(true);
    f.lossy_read = true;
    f.lossy_write = true;
    f.read_cost = 15;
    f.write_cost = 15;
    register_vector_format(registry, std::move(f));
  }

  // ---- WMZ / EMZ (gzip-compressed)
  for (const bool emf : {false, true}) {
    VectorFormat f;
    f.info.id = emf ? "emz" : "wmz";
    f.info.name = emf ? "Compressed Enhanced Metafile" : "Compressed Windows Metafile";
    f.info.description = emf ? "An EMF picture compressed with gzip (as used by Microsoft Office)."
                             : "A WMF picture compressed with gzip (as used by Microsoft Office).";
    f.info.category = Category::Vector;
    f.info.extensions = {emf ? "emz" : "wmz"};
    f.info.mime = emf ? "image/x-emz" : "image/x-wmz";
    f.info.popularity = 15;
    f.info.sniff = [emf](std::span<const std::byte> data, std::string_view filename) {
      return sniff_compressed(data, filename, emf);
    };
    f.read = [emf](ReadContext& ctx) -> Result<Scene> {
      CVT_TRY_ASSIGN(const Bytes plain, gunzip(ctx.input, ctx));
      if (emf) return read_emf(plain, ctx, read_options_from(ctx.options));
      return read_wmf(plain, ctx, read_options_from(ctx.options));
    };
    f.write = [emf](const Scene& scene, WriteContext& ctx) -> Result<Bytes> {
      const WriteOptions options = write_options_from(ctx.options);
      CVT_TRY_ASSIGN(const Bytes plain, emf ? write_emf(scene, ctx, options) : write_wmf(scene, ctx, options));
      return util::gzip_compress(plain, options::resolved_compression_level(ctx.options, 9), util::GzipHeader{});
    };
    if (emf) f.read_options = emf_reader_options();
    f.write_options = writer_options(emf);
    f.write_options.push_back(options::compression_level(9));
    f.lossy_read = true;
    f.lossy_write = true;
    f.read_cost = 16;
    f.write_cost = emf ? 16 : 21;
    register_vector_format(registry, std::move(f));
  }
}

}  // namespace cvt::vector
