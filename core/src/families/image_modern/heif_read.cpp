// Convertible core - image_modern family: HEIF/AVIF/HEIC reading. Items
// (AV1, HEVC and JPEG coded, grids, overlays, identity derivations), alpha
// auxiliaries, transformative properties, EXIF/XMP/ICC, and image sequences.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "codec/ids.hpp"
#include "engine/registry.hpp"
#include "families/codec_av1/av1_api.hpp"
#include "families/codec_hevc/hevc_api.hpp"
#include "families/image/common.hpp"
#include "families/image/image_api.hpp"
#include "families/image_modern/heif_internal.hpp"
#include "util/deflate.hpp"

namespace cvt::image_modern::heif {

namespace {

constexpr int max_derivation_depth = 8;

constexpr std::string_view alpha_urn_mpegb = "urn:mpeg:mpegB:cicp:systems:auxiliary:alpha";
constexpr std::string_view alpha_urn_hevc = "urn:mpeg:hevc:2015:auxid:1";

bool is_alpha_urn(std::string_view urn) { return urn == alpha_urn_mpegb || urn == alpha_urn_hevc; }

std::string_view describe_aux(std::string_view urn) {
  if (urn == "urn:mpeg:hevc:2015:auxid:2" || urn == "urn:mpeg:mpegB:cicp:systems:auxiliary:depth" ||
      urn.find("depth") != std::string_view::npos) {
    return "a depth map";
  }
  if (urn.find("gainmap") != std::string_view::npos || urn.find("hdrgainmap") != std::string_view::npos) {
    return "an HDR gain map";
  }
  if (urn.find("matte") != std::string_view::npos) return "a segmentation matte";
  return "an auxiliary image";
}

/// Properties whose meaning we either apply or can safely ignore.
bool understood_property(std::uint32_t type) {
  for (const char* known : {"ispe", "pixi", "colr", "av1C", "hvcC", "jpgC", "irot", "imir", "clap", "auxC", "clli",
                            "mdcv", "cclv", "amve", "pasp", "a1op", "a1lx", "lsel", "rloc", "udes", "altt", "ccst"}) {
    if (type == fourcc(known)) return true;
  }
  return false;
}

std::uint16_t matrix_code(ir::ColorMatrix matrix) {
  switch (matrix) {
    case ir::ColorMatrix::BT601: return 6;
    case ir::ColorMatrix::BT709: return 1;
    case ir::ColorMatrix::BT2020: return 9;
  }
  return 2;
}

/// Rejects AV1 sequence headers whose colour code points libaom cannot
/// represent in its enums (reserved values in damaged files), before the
/// decoder sees them. `data` is Section 5 OBUs: item data or av1C configOBUs.
Result<void> check_av1_colour(ByteView data) {
  auto obus = av1::split_section5(data);
  if (!obus) return {};  // malformed data is the decoder's to report
  for (const av1::Obu& obu : *obus) {
    if (obu.type != av1::ObuType::SequenceHeader) continue;
    const auto header = av1::parse_sequence_header(obu.payload);
    if (!header) continue;
    const unsigned cp = header->color_primaries;
    if ((cp > 12 && cp != 22) || header->transfer_characteristics > 18 || header->matrix_coefficients > 14) {
      return fail(Status::Decode, "the AV1 sequence header uses reserved colour code points");
    }
  }
  return {};
}

Result<void> check_av1_config(ByteView av1c) {
  if (av1c.size() <= 4) return {};
  return check_av1_colour(av1c.subspan(4));
}

class Reader {
 public:
  Reader(ReadContext& ctx, const BmffFile& file, std::string_view format_name)
      : ctx_(ctx),
        file_(file),
        format_name_(format_name),
        nearest_chroma_(ctx.options.get_enum("chroma_upsampling", "bilinear", {"bilinear", "nearest"}) == "nearest") {}

  Result<ir::Image> read() {
    if (const Track* track = sequence_track(); track != nullptr) return read_sequence(*track);
    if (!file_.has_meta) return fail(Status::Decode, "the {} file has no image items", format_name_);
    const Item* primary = file_.item(file_.primary);
    if (primary == nullptr) {
      // Without a valid 'pitm', use the first visible image item.
      for (const Item& item : file_.items) {
        if (!item.hidden && is_image_type(item.type) && file_.refs_from(item.id, fourcc("thmb")).empty() &&
            file_.refs_from(item.id, fourcc("auxl")).empty()) {
          primary = &item;
          break;
        }
      }
      if (primary == nullptr) return fail(Status::Decode, "the {} file has no primary image", format_name_);
    }
    CVT_TRY_ASSIGN(Raster raster, decode_item(primary->id, false, false, 0));
    attach_auxiliaries(*primary, raster);
    CVT_TRY_ASSIGN(raster, apply_transforms(*primary, std::move(raster)));

    ir::Image image;
    const ByteView icc = icc_of(*primary);
    std::optional<ir::HdrPixels> hdr;
    CVT_TRY_ASSIGN(ir::Frame frame, raster_to_frame(raster, !icc.empty(), &hdr));
    image.frames.push_back(std::move(frame));
    image.hdr = std::move(hdr);
    image.source_bit_depth = static_cast<std::uint8_t>(std::clamp(raster.source_bits, 1, 16));
    image.icc_profile = to_bytes(icc);
    attach_metadata(*primary, image);
    return image;
  }

 private:
  static bool is_image_type(std::uint32_t type) {
    return type == fourcc("av01") || type == fourcc("hvc1") || type == fourcc("grid") || type == fourcc("iovl") ||
           type == fourcc("iden") || type == fourcc("jpeg");
  }

  // ------------------------------------------------------------------------------- items

  Result<Raster> decode_item(std::uint32_t id, bool as_alpha, bool transforms, int depth) {
    if (depth > max_derivation_depth) return fail(Status::Limit, "image items are derived too deeply");
    if (std::find(active_.begin(), active_.end(), id) != active_.end()) {
      return fail(Status::Decode, "image item {} refers to itself", id);
    }
    const Item* item = file_.item(id);
    if (item == nullptr) return fail(Status::Decode, "image item {} does not exist", id);
    for (const auto& [prop, essential] : file_.properties_of(*item)) {
      if (essential && !understood_property(prop->type)) {
        return fail(Status::Decode, "image item {} needs the unsupported property '{}'", id, fourcc_text(prop->type));
      }
    }
    active_.push_back(id);
    Result<Raster> raster = decode_untransformed(*item, as_alpha, depth);
    active_.pop_back();
    if (!raster) return raster;
    if (transforms) return apply_transforms(*item, std::move(*raster));
    return raster;
  }

  /// A coded picture larger than its declared 'ispe' (encoder padding without
  /// a conformance window) is cut to the declared size.
  Raster fit_to_ispe(const Item& item, Raster raster) const {
    const Property* prop = file_.property(item, fourcc("ispe"));
    const auto ispe = prop != nullptr ? parse_ispe(prop->body) : std::nullopt;
    if (!ispe || ispe->width == 0 || ispe->height == 0) return raster;
    if (ispe->width < raster.width || ispe->height < raster.height) {
      const CropRect rect{0, 0, std::min(ispe->width, raster.width), std::min(ispe->height, raster.height)};
      return crop(raster, rect);
    }
    return raster;
  }

  /// Every coded picture decoded for one file counts against a budget, so
  /// that grids or overlays repeating one item many times cannot make the
  /// reader decode without end.
  Result<void> spend(const Raster& raster) {
    decoded_pixels_ += std::uint64_t{raster.width} * raster.height;
    if (decoded_pixels_ / 4 > ctx_.limits.max_pixels) {
      return fail(Status::Limit, "the {} file needs more decoding than the pixel limit allows", format_name_);
    }
    return {};
  }

  Result<Raster> decode_untransformed(const Item& item, bool as_alpha, int depth) {
    if (item.type == fourcc("av01") || item.type == fourcc("hvc1") || item.type == fourcc("jpeg")) {
      Result<Raster> raster = item.type == fourcc("av01")   ? decode_av1(item, as_alpha)
                              : item.type == fourcc("hvc1") ? decode_hevc(item, as_alpha)
                                                            : decode_jpeg(item);
      if (!raster) return raster;
      CVT_TRY(spend(*raster));
      return fit_to_ispe(item, std::move(*raster));
    }
    if (item.type == fourcc("grid")) return decode_grid(item, as_alpha, depth);
    if (item.type == fourcc("iovl")) return decode_overlay(item, as_alpha, depth);
    if (item.type == fourcc("iden")) {
      const std::vector<std::uint32_t> inputs = file_.refs_from(item.id, fourcc("dimg"));
      if (inputs.size() != 1) return fail(Status::Decode, "identity item {} needs exactly one input", item.id);
      return decode_item(inputs.front(), as_alpha, true, depth + 1);
    }
    return fail(Status::Decode, "{} image items of type '{}' are not supported", format_name_, fourcc_text(item.type));
  }

  Result<void> precheck(const Item& item) {
    if (const Property* ispe = file_.property(item, fourcc("ispe")); ispe != nullptr) {
      if (const auto size = parse_ispe(ispe->body)) CVT_TRY(image::check_size(ctx_, size->width, size->height));
    }
    return {};
  }

  std::optional<Cicp> nclx_of(const Item& item) const {
    for (const auto& [prop, essential] : file_.properties_of(item)) {
      if (prop->type != fourcc("colr")) continue;
      if (const auto colr = parse_colr(prop->body); colr && colr->nclx) return colr->nclx;
    }
    return std::nullopt;
  }

  ByteView icc_of(const Item& item) const {
    for (const auto& [prop, essential] : file_.properties_of(item)) {
      if (prop->type != fourcc("colr")) continue;
      if (const auto colr = parse_colr(prop->body); colr && !colr->icc.empty()) return colr->icc;
    }
    if (item.type == fourcc("grid") || item.type == fourcc("iden")) {
      const std::vector<std::uint32_t> inputs = file_.refs_from(item.id, fourcc("dimg"));
      if (!inputs.empty()) {
        if (const Item* first = file_.item(inputs.front()); first != nullptr && first != &item) {
          for (const auto& [prop, essential] : file_.properties_of(*first)) {
            if (prop->type != fourcc("colr")) continue;
            if (const auto colr = parse_colr(prop->body); colr && !colr->icc.empty()) return colr->icc;
          }
        }
      }
    }
    return {};
  }

  Result<Raster> decode_av1(const Item& item, bool as_alpha) {
    const Property* config = file_.property(item, fourcc("av1C"));
    if (config == nullptr) return fail(Status::Decode, "AV1 item {} has no 'av1C' configuration", item.id);
    CVT_TRY(precheck(item));
    CVT_TRY_ASSIGN(const ItemData data, item_data(file_, item));
    av1::StillDecodeOptions options;
    options.max_pixels = ctx_.limits.max_pixels;
    CVT_TRY(check_av1_config(config->body));
    CVT_TRY(check_av1_colour(data.view));
    CVT_TRY_ASSIGN(const av1::Picture picture, av1::decode_still(data.view, config->body, options));
    PictureInfo info;
    info.bit_depth = picture.bit_depth;
    info.monochrome = picture.monochrome;
    info.siting = picture.color.chroma_sample_position == 1   ? ir::ChromaSiting::Left
                  : picture.color.chroma_sample_position == 2 ? ir::ChromaSiting::TopLeft
                                                              : ir::ChromaSiting::Center;
    info.nearest_chroma = nearest_chroma_;
    info.cicp = {picture.color.color_primaries, picture.color.transfer_characteristics,
                 picture.color.matrix_coefficients, picture.color.full_range};
    if (const auto nclx = nclx_of(item)) info.cicp = *nclx;
    return picture_to_raster(picture.frame, info, as_alpha, ctx_);
  }

  Result<Raster> decode_hevc(const Item& item, bool as_alpha) {
    const Property* config = file_.property(item, fourcc("hvcC"));
    if (config == nullptr) return fail(Status::Decode, "HEVC item {} has no 'hvcC' configuration", item.id);
    CVT_TRY(precheck(item));
    CVT_TRY_ASSIGN(const hevc::DecoderConfig hvcc, hevc::parse_hvcc(config->body));
    std::optional<hevc::SequenceInfo> sps;
    for (const Bytes& nal : hvcc.nals) {
      if (hevc::nal_type(nal) == static_cast<std::uint8_t>(hevc::NalType::Sps)) {
        CVT_TRY_ASSIGN(sps, hevc::parse_sps(nal));
        break;
      }
    }
    if (!sps) return fail(Status::Decode, "HEVC item {} has no sequence parameter set", item.id);
    CVT_TRY(image::check_size(ctx_, sps->width, sps->height));
    CVT_TRY_ASSIGN(const ItemData data, item_data(file_, item));
    CVT_TRY_ASSIGN(const ir::VideoFrame frame,
                   hevc::decode_picture(config->body, data.view, std::min(ctx_.limits.max_pixels, hevc::max_luma_samples)));
    PictureInfo info;
    info.bit_depth = sps->bit_depth_luma;
    info.monochrome = sps->chroma_format_idc == 0;
    info.siting = sps->chroma_loc_present ? frame.color.siting : ir::ChromaSiting::Center;
    info.nearest_chroma = nearest_chroma_;
    info.cicp = {sps->colour_primaries, sps->transfer_characteristics, sps->matrix_coefficients,
                 sps->video_full_range};
    if (!sps->vui_present) info.cicp.matrix = 2;
    if (const auto nclx = nclx_of(item)) info.cicp = *nclx;
    return picture_to_raster(frame, info, as_alpha, ctx_);
  }

  Result<Raster> decode_jpeg(const Item& item) {
    CVT_TRY(precheck(item));
    CVT_TRY_ASSIGN(const ItemData data, item_data(file_, item));
    Bytes joined;
    ByteView stream = data.view;
    if (const Property* tables = file_.property(item, fourcc("jpgC")); tables != nullptr && !tables->body.empty()) {
      // jpgC holds the header segments (without SOI) shared by the items.
      joined.reserve(tables->body.size() + data.view.size());
      joined.insert(joined.end(), data.view.begin(), data.view.begin() + std::min<std::size_t>(2, data.view.size()));
      joined.insert(joined.end(), tables->body.begin(), tables->body.end());
      if (data.view.size() > 2) joined.insert(joined.end(), data.view.begin() + 2, data.view.end());
      stream = joined;
    }
    CVT_TRY_ASSIGN(const ir::Image decoded, image::decode("jpg", stream, ctx_));
    if (!decoded.valid()) return fail(Status::Decode, "JPEG item {} holds no picture", item.id);
    const ir::Frame& frame = decoded.frames.front();
    CVT_TRY_ASSIGN(Raster raster, make_raster(ctx_, frame.width, frame.height, false));
    raster.p8 = frame.rgba;
    raster.color = {1, 13, 6, true};
    return raster;
  }

  Result<Raster> decode_grid(const Item& item, bool as_alpha, int depth) {
    CVT_TRY_ASSIGN(const ItemData data, item_data(file_, item));
    util::ByteReader r(data.view);
    const std::uint8_t version = r.u8();
    const std::uint8_t flags = r.u8();
    const std::uint32_t rows = r.u8() + 1u;
    const std::uint32_t columns = r.u8() + 1u;
    const std::uint32_t width = (flags & 1u) != 0 ? r.u32be() : r.u16be();
    const std::uint32_t height = (flags & 1u) != 0 ? r.u32be() : r.u16be();
    if (!r.ok() || version != 0) return fail(Status::Decode, "grid item {} is malformed", item.id);
    const std::vector<std::uint32_t> tiles = file_.refs_from(item.id, fourcc("dimg"));
    if (tiles.size() != std::size_t{rows} * columns) {
      return fail(Status::Decode, "grid item {} has {} tiles instead of {}", item.id, tiles.size(), rows * columns);
    }
    CVT_TRY(image::check_size(ctx_, width, height));
    // Tile sizes from their 'ispe' before anything is decoded.
    const Item* first = file_.item(tiles.front());
    if (first == nullptr) return fail(Status::Decode, "grid item {} refers to a missing tile", item.id);
    const Property* first_ispe = file_.property(*first, fourcc("ispe"));
    if (first_ispe != nullptr) {
      if (const auto size = parse_ispe(first_ispe->body)) {
        CVT_TRY(ctx_.limits.check_pixels(size->width, size->height, tiles.size()));
      }
    }

    std::optional<Raster> canvas;
    std::uint32_t tile_width = 0;
    std::uint32_t tile_height = 0;
    for (std::size_t i = 0; i < tiles.size(); ++i) {
      CVT_TRY(ctx_.checkpoint(0.9 * static_cast<double>(i) / static_cast<double>(tiles.size()), "Decoding tiles"));
      CVT_TRY_ASSIGN(Raster tile, decode_item(tiles[i], as_alpha, true, depth + 1));
      if (!canvas) {
        tile_width = tile.width;
        tile_height = tile.height;
        if (std::uint64_t{tile_width} * columns < width || std::uint64_t{tile_height} * rows < height) {
          return fail(Status::Decode, "the tiles of grid item {} do not cover the image", item.id);
        }
        CVT_TRY_ASSIGN(canvas, make_raster(ctx_, width, height, tile.wide));
        canvas->color = tile.color;
        canvas->source_bits = tile.source_bits;
        canvas->identity_matrix = tile.identity_matrix;
      }
      if (tile.width != tile_width || tile.height != tile_height) {
        return fail(Status::Decode, "the tiles of grid item {} differ in size", item.id);
      }
      if (tile.wide && !canvas->wide) promote_to_wide(*canvas);
      if (canvas->wide && !tile.wide) promote_to_wide(tile);
      const std::uint64_t x = std::uint64_t{tile_width} * (i % columns);
      const std::uint64_t y = std::uint64_t{tile_height} * (i / columns);
      blit(*canvas, tile, static_cast<std::int64_t>(x), static_cast<std::int64_t>(y));
    }
    if (const auto nclx = nclx_of(item)) {
      canvas->color.primaries = nclx->primaries;
      canvas->color.transfer = nclx->transfer;
    }
    return std::move(*canvas);
  }

  Result<Raster> decode_overlay(const Item& item, bool as_alpha, int depth) {
    CVT_TRY_ASSIGN(const ItemData data, item_data(file_, item));
    const std::vector<std::uint32_t> inputs = file_.refs_from(item.id, fourcc("dimg"));
    util::ByteReader r(data.view);
    const std::uint8_t version = r.u8();
    const std::uint8_t flags = r.u8();
    std::uint16_t fill[4] = {};
    for (std::uint16_t& f : fill) f = r.u16be();
    const bool large = (flags & 1u) != 0;
    const std::uint32_t width = large ? r.u32be() : r.u16be();
    const std::uint32_t height = large ? r.u32be() : r.u16be();
    if (!r.ok() || version != 0 || inputs.empty()) return fail(Status::Decode, "overlay item {} is malformed", item.id);
    CVT_TRY(image::check_size(ctx_, width, height));
    std::vector<std::pair<std::int64_t, std::int64_t>> offsets;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      const std::int64_t x = large ? r.i32be() : r.i16be();
      const std::int64_t y = large ? r.i32be() : r.i16be();
      offsets.emplace_back(x, y);
    }
    if (!r.ok()) return fail(Status::Decode, "overlay item {} is truncated", item.id);
    CVT_TRY_ASSIGN(Raster canvas, make_raster(ctx_, width, height, false, fill));
    canvas.has_alpha = fill[3] != 65535;
    bool first = true;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      CVT_TRY_ASSIGN(Raster layer, decode_item(inputs[i], as_alpha, true, depth + 1));
      if (const Item* input = file_.item(inputs[i]); input != nullptr && !as_alpha) attach_auxiliaries(*input, layer);
      if (first) {
        canvas.color = layer.color;
        canvas.source_bits = layer.source_bits;
        first = false;
      }
      if (layer.wide && !canvas.wide) {
        promote_to_wide(canvas);
      } else if (canvas.wide && !layer.wide) {
        promote_to_wide(layer);
      }
      if (layer.has_alpha) {
        composite(canvas, layer, offsets[i].first, offsets[i].second);
      } else {
        blit(canvas, layer, offsets[i].first, offsets[i].second);
      }
    }
    return canvas;
  }

  Result<Raster> apply_transforms(const Item& item, Raster raster) {
    for (const auto& [prop, essential] : file_.properties_of(item)) {
      if (prop->type == fourcc("clap")) {
        const auto clap = parse_clap(prop->body);
        const auto rect = clap ? clap_rect(*clap, raster.width, raster.height) : std::nullopt;
        if (!rect) {
          ctx_.warn("An invalid clean-aperture crop was ignored.");
          continue;
        }
        if (rect->width != raster.width || rect->height != raster.height) raster = crop(raster, *rect);
      } else if (prop->type == fourcc("irot")) {
        if (prop->body.empty()) continue;
        const int angle = std::to_integer<int>(prop->body[0]) & 3;
        if (angle != 0) raster = rotate_ccw(raster, angle);
      } else if (prop->type == fourcc("imir")) {
        if (prop->body.empty()) continue;
        raster = mirror(raster, std::to_integer<int>(prop->body[0]) & 1);
      }
    }
    return raster;
  }

  // -------------------------------------------------------------------------- auxiliaries

  void attach_auxiliaries(const Item& master, Raster& color) {
    const std::vector<std::uint32_t> premultiplied = file_.refs_from(master.id, fourcc("prem"));
    for (const std::uint32_t aux_id : file_.refs_to(master.id, fourcc("auxl"))) {
      const Item* aux = file_.item(aux_id);
      if (aux == nullptr) continue;
      const Property* auxc = file_.property(*aux, fourcc("auxC"));
      const std::string urn = auxc != nullptr ? parse_aux_type(auxc->body) : std::string{};
      if (!is_alpha_urn(urn)) {
        ctx_.warn(std::format("The {} file contains {}, which was ignored.", format_name_, describe_aux(urn)));
        continue;
      }
      if (color.has_alpha) continue;
      Result<Raster> alpha = decode_item(aux_id, true, false, 1);
      if (!alpha) {
        ctx_.warn(std::format("The transparency of the {} image could not be decoded ({}); it was dropped.",
                              format_name_, alpha.error().message));
        continue;
      }
      if (alpha->width != color.width || alpha->height != color.height) {
        // Scale the mask to the colour picture (nearest sample).
        Raster scaled = color;
        scaled.has_alpha = false;
        for (std::uint32_t y = 0; y < color.height; ++y) {
          const std::uint32_t sy = static_cast<std::uint32_t>(std::uint64_t{y} * alpha->height / color.height);
          for (std::uint32_t x = 0; x < color.width; ++x) {
            const std::uint32_t sx = static_cast<std::uint32_t>(std::uint64_t{x} * alpha->width / color.width);
            const std::size_t from = (std::size_t{sy} * alpha->width + sx) * 4;
            const std::size_t to = (std::size_t{y} * color.width + x) * 4;
            if (scaled.wide) {
              scaled.p16[to] = alpha->wide ? alpha->p16[from] : static_cast<std::uint16_t>(alpha->p8[from] * 257u);
            } else {
              scaled.p8[to] = alpha->wide ? static_cast<std::uint8_t>((alpha->p16[from] * 255u + 32767u) / 65535u)
                                          : alpha->p8[from];
            }
          }
        }
        *alpha = std::move(scaled);
      }
      const bool prem = std::find(premultiplied.begin(), premultiplied.end(), aux_id) != premultiplied.end();
      attach_alpha(color, *alpha, prem);
    }
  }

  // ----------------------------------------------------------------------------- metadata

  void attach_metadata(const Item& primary, ir::Image& image) {
    // Items that describe the primary image ('cdsc'); when nothing refers to
    // it, metadata items that describe nothing in particular are taken.
    std::vector<std::uint32_t> described = file_.refs_to(primary.id, fourcc("cdsc"));
    if (described.empty()) {
      for (const Item& item : file_.items) {
        if ((item.type == fourcc("Exif") || item.type == fourcc("mime")) && file_.refs_from(item.id, fourcc("cdsc")).empty()) {
          described.push_back(item.id);
        }
      }
    }
    for (const std::uint32_t id : described) {
      const Item* item = file_.item(id);
      if (item == nullptr) continue;
      Result<ItemData> data = item_data(file_, *item);
      if (!data) continue;
      if (item->type == fourcc("Exif") && image.exif.empty()) {
        util::ByteReader r(data->view);
        const std::uint32_t offset = r.u32be();
        if (!r.ok() || offset > r.remaining()) continue;
        ByteView tiff = data->view.subspan(4 + offset);
        if (tiff.size() >= 6 && as_chars(tiff.first(6)) == image::exif_app1_prefix) tiff = tiff.subspan(6);
        const auto info = image::parse_exif(tiff);
        if (!info) continue;
        // irot/imir are normative in HEIF and were applied above; the EXIF
        // orientation is informative and must not rotate the picture again.
        image.exif = to_bytes(tiff);
        image::reset_exif_orientation(image.exif);
        for (const auto& [key, value] : info->text) image.metadata.emplace(key, value);
        if (image.dpi_x <= 0 && info->dpi_x > 0) {
          image.dpi_x = info->dpi_x;
          image.dpi_y = info->dpi_y;
        }
      } else if (item->type == fourcc("mime") && image.xmp.empty() &&
                 (item->content_type == "application/rdf+xml" || item->content_type == "application/xmp+xml")) {
        if (item->content_encoding.empty()) {
          image.xmp = std::string(as_chars(data->view));
        } else if (item->content_encoding == "deflate") {
          auto inflated = util::zlib_decompress(data->view, ctx_.limits.max_decompressed_bytes);
          if (!inflated) inflated = util::inflate_raw(data->view, ctx_.limits.max_decompressed_bytes);
          if (inflated) image.xmp = std::string(as_chars(*inflated));
        }
      }
    }
  }

  // ---------------------------------------------------------------------------- sequences

  const Track* sequence_track() const {
    for (const Track& track : file_.tracks) {
      const bool visual = track.handler == fourcc("pict") || track.handler == fourcc("vide");
      const bool coded = track.codec == fourcc("av01") || track.codec == fourcc("hvc1") || track.codec == fourcc("hev1");
      if (!visual || !coded || track.samples.empty() || !track.auxl.empty()) continue;
      // A single-sample track next to a still image adds nothing.
      if (file_.has_meta && file_.item(file_.primary) != nullptr && track.samples.size() < 2) continue;
      return &track;
    }
    return nullptr;
  }

  const Track* alpha_track(const Track& color) const {
    for (const Track& track : file_.tracks) {
      if (&track == &color || track.samples.size() != color.samples.size()) continue;
      if (std::find(track.auxl.begin(), track.auxl.end(), color.id) == track.auxl.end()) continue;
      if (track.aux_type.empty() || is_alpha_urn(track.aux_type)) return &track;
    }
    return nullptr;
  }

  /// Decodes every sample of a track into rasters, calling `use` for each.
  template <class Use>
  Result<void> decode_track(const Track& track, bool as_alpha, Use&& use) {
    const std::string_view codec_id = track.codec == fourcc("av01") ? codec::id::av1 : codec::id::hevc;
    if (ctx_.registry == nullptr) return fail(Status::Decode, "no codec registry available");
    std::unique_ptr<codec::VideoDecoder> decoder = ctx_.registry->media().make_video_decoder(codec_id);
    if (!decoder) return fail(Status::Decode, "no {} decoder is available for the image sequence", codec_id);
    codec::CodecParams params;
    params.codec = std::string(codec_id);
    params.extradata = to_bytes(track.config);
    params.timescale = track.timescale == 0 ? 1000 : track.timescale;
    params.width = track.width;
    params.height = track.height;
    if (track.codec == fourcc("av01")) CVT_TRY(check_av1_config(track.config));
    CVT_TRY(decoder->configure(params));
    std::size_t delivered = 0;
    const auto drain = [&]() -> Result<void> {
      while (true) {
        CVT_TRY_ASSIGN(std::optional<ir::VideoFrame> frame, decoder->receive_frame());
        if (!frame) return {};
        if (delivered >= track.samples.size()) continue;
        PictureInfo info;
        info.bit_depth = ir::bit_depth(frame->format);
        info.siting = frame->color.siting;
        info.nearest_chroma = nearest_chroma_;
        info.cicp = {2, 2, matrix_code(frame->color.matrix), frame->color.range == ir::ColorRange::Full};
        if (track.nclx) info.cicp = *track.nclx;
        CVT_TRY(image::check_size(ctx_, frame->width, frame->height));
        CVT_TRY_ASSIGN(Raster raster, picture_to_raster(*frame, info, as_alpha, ctx_));
        CVT_TRY(use(delivered, std::move(raster)));
        ++delivered;
      }
    };
    std::int64_t pts = 0;
    for (std::size_t i = 0; i < track.samples.size(); ++i) {
      CVT_TRY(ctx_.checkpoint(0.95 * static_cast<double>(i) / static_cast<double>(track.samples.size()),
                              "Decoding frames"));
      const Sample& sample = track.samples[i];
      ir::Packet packet;
      packet.bytes = to_bytes(file_.data.subspan(static_cast<std::size_t>(sample.offset), sample.size));
      packet.pts = pts;
      packet.dts = pts;
      packet.duration = sample.duration;
      packet.keyframe = sample.sync;
      if (track.codec == fourcc("av01")) CVT_TRY(check_av1_colour(packet.bytes));
      pts += sample.duration;
      CVT_TRY(decoder->send_packet(packet));
      CVT_TRY(drain());
    }
    CVT_TRY(decoder->flush());
    CVT_TRY(drain());
    if (delivered == 0) return fail(Status::Decode, "the image sequence holds no decodable frames");
    return {};
  }

  Result<ir::Image> read_sequence(const Track& track) {
    if (track.width > 0 && track.height > 0) {
      CVT_TRY(ctx_.limits.check_pixels(track.width, track.height, track.samples.size()));
    }
    std::vector<Raster> rasters;
    CVT_TRY(decode_track(track, false, [&](std::size_t, Raster raster) -> Result<void> {
      if (!rasters.empty() && (raster.width != rasters.front().width || raster.height != rasters.front().height)) {
        return fail(Status::Decode, "the frames of the image sequence differ in size");
      }
      CVT_TRY(ctx_.limits.check_pixels(raster.width, raster.height, rasters.size() + 1));
      rasters.push_back(std::move(raster));
      return {};
    }));
    if (const Track* alpha = alpha_track(track); alpha != nullptr) {
      Result<void> attached = decode_track(*alpha, true, [&](std::size_t index, Raster mask) -> Result<void> {
        if (index < rasters.size()) attach_alpha(rasters[index], mask, false);
        return {};
      });
      if (!attached) ctx_.warn("The transparency of the image sequence could not be decoded; it was dropped.");
    }
    ir::Image image;
    image.loop_count = track.plays;
    const bool single = rasters.size() == 1;
    for (std::size_t i = 0; i < rasters.size(); ++i) {
      std::optional<ir::HdrPixels> hdr;
      CVT_TRY_ASSIGN(ir::Frame frame, raster_to_frame(rasters[i], !track.icc.empty(), single ? &hdr : nullptr));
      const std::uint32_t ticks = i < track.samples.size() ? track.samples[i].duration : 0;
      const std::uint32_t timescale = track.timescale == 0 ? 1000 : track.timescale;
      frame.delay_ms = static_cast<std::uint32_t>((std::uint64_t{ticks} * 1000 + timescale / 2) / timescale);
      image.frames.push_back(std::move(frame));
      if (single) image.hdr = std::move(hdr);
    }
    image.source_bit_depth = static_cast<std::uint8_t>(std::clamp(rasters.front().source_bits, 1, 16));
    image.icc_profile = to_bytes(track.icc);
    if (const Item* primary = file_.item(file_.primary); primary != nullptr) {
      if (image.icc_profile.empty()) image.icc_profile = to_bytes(icc_of(*primary));
      attach_metadata(*primary, image);
    }
    return image;
  }

  ReadContext& ctx_;
  const BmffFile& file_;
  std::string_view format_name_;
  bool nearest_chroma_ = false;
  std::uint64_t decoded_pixels_ = 0;
  std::vector<std::uint32_t> active_;
};

}  // namespace

Result<ir::Image> read_heif(ReadContext& ctx, std::string_view format_name) {
  CVT_TRY_ASSIGN(const BmffFile file, parse_file(ctx.input, ctx.limits));
  Reader reader(ctx, file, format_name);
  return reader.read();
}

}  // namespace cvt::image_modern::heif
