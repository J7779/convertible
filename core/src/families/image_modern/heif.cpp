// Convertible core - image_modern family: registration of the HEIF-based
// formats (AVIF, HEIC and generic HEIF).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/image_modern/heif.hpp"

#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/image/common.hpp"
#include "families/image_modern/heif_internal.hpp"

namespace cvt::image_modern {

namespace {

OptionSpec effort_option() {
  return OptionSpec::choice("effort", "Encoder effort", "balanced", {"fast", "balanced", "best"},
                            "Encoding speed against file size: 'best' searches longer for a smaller file at the "
                            "same quality.")
      .advanced();
}

std::vector<OptionSpec> heic_options() {
  std::vector<OptionSpec> specs = {
      options::quality(),
      options::preset(),
      effort_option(),
      OptionSpec::boolean("keep_alpha", "Keep transparency", false,
                          "Store transparency as an auxiliary 4:2:0 HEVC image. libheif-based viewers show it, but "
                          "Apple's decoder cannot open such files; when off, transparent areas are blended onto "
                          "the background colour.")
          .advanced(),
      image::background_option(),
  };
  for (OptionSpec& spec : image::size_options()) specs.push_back(std::move(spec));
  specs.push_back(options::strip_metadata());
  return specs;
}

std::vector<OptionSpec> avif_options() {
  std::vector<OptionSpec> specs = {
      options::quality(),
      options::preset(),
      options::lossless(),
      OptionSpec::choice("bit_depth", "Bit depth", "auto", {"auto", "8", "10"},
                         "Bits per sample. 'auto' keeps 10 bits for sources with more than 8 bits (16-bit PNG, "
                         "10-bit HEIC/AVIF) and uses 8 otherwise.")
          .advanced(),
      OptionSpec::choice("chroma", "Chroma subsampling", "420", {"420", "444"},
                         "4:2:0 halves the colour resolution (smaller files); 4:4:4 keeps it (sharper colour "
                         "edges). Lossless output is always 4:4:4.")
          .advanced(),
      effort_option(),
  };
  for (OptionSpec& spec : image::size_options()) specs.push_back(std::move(spec));
  specs.push_back(options::strip_metadata());
  return specs;
}

std::vector<OptionSpec> read_options() {
  return {OptionSpec::choice("chroma_upsampling", "Chroma upsampling", "bilinear", {"bilinear", "nearest"},
                             "How subsampled (4:2:0/4:2:2) colour is brought to full resolution: interpolated at the "
                             "signalled sample position, or each sample repeated (as libheif does).")
              .advanced()};
}

template <heif::Flavor flavor>
Result<ir::Image> read_fn(ReadContext& ctx) {
  constexpr std::string_view name = flavor == heif::Flavor::Avif ? "AVIF" : flavor == heif::Flavor::Heic ? "HEIC" : "HEIF";
  return heif::read_heif(ctx, name);
}

template <heif::Flavor flavor>
Result<Bytes> write_fn(const ir::Image& image, WriteContext& ctx) {
  return heif::write_heif(image, ctx, flavor);
}

}  // namespace

void register_heif(Registry& registry) {
  {
    FormatInfo info;
    info.id = "avif";
    info.name = "AVIF image";
    info.description = "AV1-coded image in the HEIF container: small files, alpha, 10-bit, animation.";
    info.category = Category::Image;
    info.extensions = {"avif", "avifs"};
    info.mime = "image/avif";
    info.popularity = 65;
    registry.add_format(std::move(info));

    image::ImageCodecSpec spec;
    spec.format_id = "avif";
    spec.read = image::reader(read_fn<heif::Flavor::Avif>);
    spec.write = image::writer(write_fn<heif::Flavor::Avif>);
    spec.write_options = avif_options();
    spec.read_options = read_options();
    spec.lossy_write = true;
    spec.read_cost = 20;
    spec.write_cost = 30;
    image::add_image_codec(registry, std::move(spec));
  }
  {
    FormatInfo info;
    info.id = "heic";
    info.name = "HEIC image";
    info.description = "HEVC-coded image in the HEIF container, the iPhone photo format.";
    info.category = Category::Image;
    info.extensions = {"heic", "heics", "hif"};
    info.mime = "image/heic";
    info.popularity = 75;
    registry.add_format(std::move(info));

    image::ImageCodecSpec spec;
    spec.format_id = "heic";
    spec.read = image::reader(read_fn<heif::Flavor::Heic>);
    spec.write = image::writer(write_fn<heif::Flavor::Heic>);
    spec.write_options = heic_options();
    spec.read_options = read_options();
    spec.lossy_write = true;
    spec.read_cost = 20;
    spec.write_cost = 30;
    image::add_image_codec(registry, std::move(spec));
  }
  {
    FormatInfo info;
    info.id = "heif";
    info.name = "HEIF image";
    info.description = "High Efficiency Image File with the generic 'mif1' brand (HEVC-coded when written).";
    info.category = Category::Image;
    info.extensions = {"heif", "heifs"};
    info.mime = "image/heif";
    info.popularity = 30;
    info.sniff = [](std::span<const std::byte> data, std::string_view) { return heif::sniff_generic_heif(data); };
    registry.add_format(std::move(info));

    image::ImageCodecSpec spec;
    spec.format_id = "heif";
    spec.read = image::reader(read_fn<heif::Flavor::Heif>);
    spec.write = image::writer(write_fn<heif::Flavor::Heif>);
    spec.write_options = heic_options();
    spec.read_options = read_options();
    spec.lossy_write = true;
    spec.read_cost = 20;
    spec.write_cost = 30;
    image::add_image_codec(registry, std::move(spec));
  }
}

}  // namespace cvt::image_modern
