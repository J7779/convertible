// Convertible core - image_modern family: camera RAW format registration,
// detection and the shared reader entry point.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/image/common.hpp"
#include "families/image_modern/raw.hpp"
#include "families/image_modern/raw_readers.hpp"
#include "util/text.hpp"

namespace cvt::image_modern {

namespace {

using raw::DevelopOptions;

raw::Request request_from(const Options& options) {
  raw::Request r;
  r.preview = options.get_bool("raw_preview", false);
  const std::string demosaic = options.get_string("raw_demosaic", "adaptive");
  r.develop.demosaic = demosaic == "malvar"     ? DevelopOptions::Demosaic::Malvar
                       : demosaic == "bilinear" ? DevelopOptions::Demosaic::Bilinear
                                                : DevelopOptions::Demosaic::Adaptive;
  const std::string wb = options.get_string("raw_white_balance", "camera");
  r.develop.white_balance = wb == "auto"   ? DevelopOptions::WhiteBalance::Auto
                            : wb == "none" ? DevelopOptions::WhiteBalance::None
                                           : DevelopOptions::WhiteBalance::Camera;
  const std::string tone = options.get_string("raw_tone", "clip");
  r.develop.tone = tone == "filmic"     ? DevelopOptions::Tone::Filmic
                   : tone == "reinhard" ? DevelopOptions::Tone::Reinhard
                                        : DevelopOptions::Tone::Clip;
  r.develop.exposure = options.get_float("raw_exposure", 0.0, -8.0, 8.0);
  return r;
}

Result<ir::Image> read_raw(ReadContext& ctx) {
  const raw::Request request = request_from(ctx.options);
  // The content decides how to read; the requested id only fills in when the
  // structure is not recognised (e.g. an unusual TIFF-based RAW).
  const raw::Identified content = raw::identify(ctx.input, ctx.filename);
  std::string id = content.id.empty() ? ctx.format_id : content.id;
  if (id == "dng" || id == "gpr") {
    const auto tree = raw::parse_tree(ctx.input);
    if (!tree) return fail(Status::Decode, "not a DNG file (no TIFF structure)");
    if (tree->ifds.front().has(0xC612)) return raw::read_dng(*tree, request, ctx);
    id = "tiff-raw";
  }
  return raw::read_maker(id, ctx.input, request, ctx);
}

std::vector<OptionSpec> read_options() {
  return {
      OptionSpec::boolean("raw_preview", "Use embedded preview", false,
                          "Return the camera's embedded JPEG preview instead of developing the RAW data (much faster)."),
      OptionSpec::choice("raw_white_balance", "White balance", "camera", {"camera", "auto", "none"},
                         "As shot by the camera, estimated from the picture (grey world), or none."),
      OptionSpec::number("raw_exposure", "Exposure", 0.0, -8.0, 8.0, "Exposure adjustment in stops."),
      OptionSpec::choice("raw_tone", "Tone mapping", "clip", {"clip", "filmic", "reinhard"},
                         "How linear sensor values become the 8-bit picture: clip highlights, a filmic curve, "
                         "or Reinhard compression.")
          .advanced(),
      OptionSpec::choice("raw_demosaic", "Demosaicing", "adaptive", {"adaptive", "malvar", "bilinear"},
                         "Colour filter interpolation: edge-adaptive (best), Malvar-He-Cutler, or bilinear (fastest).")
          .advanced(),
  };
}

struct RawFormat {
  std::string_view id;
  std::string_view name;
  std::string_view description;
  std::string_view extensions;  // space separated
  std::string_view mime;
  int popularity;
  int certain_confidence;  // sniff result when the structure was recognised
};

constexpr RawFormat formats[] = {
    {"dng", "Digital Negative", "Adobe's open TIFF-based raw image format, also used by phones.", "dng",
     "image/x-adobe-dng", 25, 95},
    {"gpr", "GoPro RAW image", "DNG-based raw photo from GoPro cameras.", "gpr", "image/x-gopro-gpr", 3, 95},
    {"cr2", "Canon RAW 2 image", "TIFF-based raw sensor data from Canon cameras (until 2018).", "cr2",
     "image/x-canon-cr2", 30, 97},
    {"cr3", "Canon RAW 3 image", "ISO-media-based raw sensor data from current Canon cameras.", "cr3",
     "image/x-canon-cr3", 25, 96},
    {"crw", "Canon RAW image (CIFF)", "Raw sensor data from early Canon digital cameras.", "crw", "image/x-canon-crw",
     5, 96},
    {"nef", "Nikon RAW image", "TIFF-based raw sensor data from Nikon cameras.", "nef", "image/x-nikon-nef", 30, 92},
    {"nrw", "Nikon Coolpix RAW image", "Raw sensor data from Nikon Coolpix compacts.", "nrw", "image/x-nikon-nrw", 5,
     93},
    {"arw", "Sony RAW image", "TIFF-based raw sensor data from Sony cameras.", "arw", "image/x-sony-arw", 25, 92},
    {"sr2", "Sony RAW 2 image", "Raw sensor data from early Sony cameras.", "sr2", "image/x-sony-sr2", 3, 93},
    {"srf", "Sony RAW format image", "Raw sensor data from the Sony DSC-R1.", "srf", "image/x-sony-srf", 2, 93},
    {"raf", "Fujifilm RAW image", "Raw sensor data from Fujifilm cameras.", "raf", "image/x-fuji-raf", 15, 96},
    {"orf", "Olympus RAW image", "Raw sensor data from Olympus / OM System cameras.", "orf ori",
     "image/x-olympus-orf", 12, 95},
    {"rw2", "Panasonic RAW image", "Raw sensor data from Panasonic Lumix cameras.", "rw2 raw",
     "image/x-panasonic-rw2", 12, 95},
    {"rwl", "Leica RAW image", "Raw sensor data from Leica compacts (Panasonic-made).", "rwl", "image/x-leica-rwl", 2,
     96},
    {"pef", "Pentax RAW image", "Raw sensor data from Pentax cameras.", "pef", "image/x-pentax-pef", 6, 92},
    {"srw", "Samsung RAW image", "Raw sensor data from Samsung cameras.", "srw", "image/x-samsung-srw", 3, 92},
    {"3fr", "Hasselblad RAW image", "Raw sensor data from Hasselblad cameras.", "3fr", "image/x-hasselblad-3fr", 3,
     92},
    {"fff", "Hasselblad/Imacon RAW image", "Raw data from Hasselblad and Imacon scanners and backs.", "fff",
     "image/x-hasselblad-fff", 2, 93},
    {"erf", "Epson RAW image", "Raw sensor data from the Epson R-D1.", "erf", "image/x-epson-erf", 1, 92},
    {"dcr", "Kodak DCR RAW image", "Raw sensor data from Kodak professional cameras.", "dcr", "image/x-kodak-dcr", 2,
     92},
    {"kdc", "Kodak RAW image", "Raw sensor data from Kodak compacts.", "kdc", "image/x-kodak-kdc", 2, 93},
    {"mef", "Mamiya RAW image", "Raw sensor data from Mamiya medium-format cameras.", "mef", "image/x-mamiya-mef", 1,
     92},
    {"mos", "Leaf RAW image", "Raw sensor data from Leaf digital backs.", "mos", "image/x-leaf-mos", 1, 92},
    {"iiq", "Phase One RAW image", "Raw sensor data from Phase One digital backs.", "iiq", "image/x-phaseone-iiq", 2,
     92},
};

bool extension_matches(std::string_view filename, std::string_view extensions) {
  const std::size_t dot = filename.find_last_of('.');
  if (dot == std::string_view::npos) return false;
  const std::string ext = util::to_lower(filename.substr(dot + 1));
  for (const std::string_view e : util::split(extensions, ' ')) {
    if (e == ext) return true;
  }
  return false;
}

std::vector<Signature> signatures_for(std::string_view id) {
  if (id == "cr2") return {Signature::at(0, "'II' 2A 00 10 00 00 00 'CR' 02")};
  if (id == "orf") return {Signature::at(0, "'IIRO' 08 00"), Signature::at(0, "'IIRS' 08 00"), Signature::at(0, "'MMOR' 00 00")};
  if (id == "rw2") return {Signature::at(0, "'IIU' 00")};
  if (id == "raf") return {Signature::at(0, "'FUJIFILMCCD-RAW '")};
  if (id == "cr3") return {Signature::at(0, "?? ?? ?? ?? 'ftypcrx '")};
  if (id == "crw") return {Signature::at(0, "'II' 1A 00 00 00 'HEAPCCDR'")};
  return {};
}

}  // namespace

void register_raw(Registry& registry) {
  for (const RawFormat& f : formats) {
    FormatInfo info;
    info.id = std::string(f.id);
    info.name = std::string(f.name);
    info.description = std::string(f.description);
    info.category = Category::Image;
    for (const std::string_view e : util::split(f.extensions, ' ')) info.extensions.emplace_back(e);
    info.mime = std::string(f.mime);
    info.popularity = f.popularity;
    info.signatures = signatures_for(f.id);
    const std::string id(f.id);
    const std::string extensions(f.extensions);
    const int certain = f.certain_confidence;
    // The `tiff` format owns the plain TIFF magic: a RAW id wins only when
    // the structure is recognised (or the maker matches and the extension
    // agrees).
    info.sniff = [id, extensions, certain](std::span<const std::byte> data, std::string_view filename) -> int {
      const raw::Identified found = raw::identify(data, filename);
      if (found.id != id) return 0;
      if (found.certain) return certain;
      return extension_matches(filename, extensions) ? 72 : 0;
    };
    registry.add_format(std::move(info));

    image::ImageCodecSpec spec;
    spec.format_id = std::string(f.id);
    spec.read = image::reader(read_raw);
    spec.read_options = read_options();
    spec.read_cost = 30;
    image::add_image_codec(registry, std::move(spec));
  }
}

}  // namespace cvt::image_modern
