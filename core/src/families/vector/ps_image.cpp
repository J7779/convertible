// Convertible core - vector family: PostScript image operators.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <format>
#include <unordered_map>
#include <utility>

#include "families/image/image_api.hpp"
#include "families/vector/ps_internal.hpp"

namespace cvt::vector::ps {

namespace {

struct ImageSpec {
  std::int64_t width = 0, height = 0;
  int bpc = 8;
  int ncomp = 1;
  Matrix matrix;
  std::vector<Object> sources;
  bool multi = false;
  std::vector<double> decode;
  bool interpolate = false;
  bool mask = false;      // imagemask
  bool polarity = false;  // imagemask: true paints 1 bits
  // ImageType 3 / 4
  std::vector<Object> mask_sources;
  std::int64_t mask_width = 0, mask_height = 0;
  int mask_bpc = 1;
  bool mask_invert = false;
  int interleave = 0;
  std::vector<double> mask_color;
};

}  // namespace

void Interpreter::do_image(int kind) {
  ImageSpec spec;
  ColorSpace space = gstate_.space;
  need(1);
  if (top().type == Type::Dict) {
    Object d = pop_dict();
    auto num = [&](const Object& dd, std::string_view key, double fallback) {
      const Object* v = dict_find(dd, key);
      return v != nullptr && v->is_number() ? v->number() : fallback;
    };
    const int image_type = static_cast<int>(num(d, "ImageType", 1));
    Object data_dict = d;
    if (image_type == 3) {
      const Object* dd = dict_find(d, "DataDict");
      const Object* md = dict_find(d, "MaskDict");
      if (dd == nullptr || md == nullptr || dd->type != Type::Dict || md->type != Type::Dict) error("typecheck");
      data_dict = *dd;
      spec.interleave = static_cast<int>(num(d, "InterleaveType", 1));
      spec.mask_width = static_cast<std::int64_t>(num(*md, "Width", 0));
      spec.mask_height = static_cast<std::int64_t>(num(*md, "Height", 0));
      spec.mask_bpc = static_cast<int>(num(*md, "BitsPerComponent", 1));
      if (const Object* dec = dict_find(*md, "Decode"); dec != nullptr && dec->type == Type::Array && dec->size >= 1) {
        spec.mask_invert = array_get(*dec, 0).is_number() && array_get(*dec, 0).number() > 0.5;
      }
      if (spec.interleave == 3) {
        if (const Object* src = dict_find(*md, "DataSource")) spec.mask_sources.push_back(*src);
      }
    } else if (image_type == 4) {
      if (const Object* mc = dict_find(d, "MaskColor"); mc != nullptr && mc->type == Type::Array) {
        for (std::uint32_t i = 0; i < mc->size; ++i) {
          const Object e = array_get(*mc, i);
          if (e.is_number()) spec.mask_color.push_back(e.number());
        }
      }
    } else if (image_type != 1) {
      error("rangecheck", std::format("ImageType {} is not supported", image_type));
    }
    spec.width = static_cast<std::int64_t>(num(data_dict, "Width", 0));
    spec.height = static_cast<std::int64_t>(num(data_dict, "Height", 0));
    spec.bpc = static_cast<int>(num(data_dict, "BitsPerComponent", kind == 1 ? 1 : 8));
    if (const Object* m = dict_find(data_dict, "ImageMatrix"); m != nullptr && m->type == Type::Array) spec.matrix = to_matrix(*m);
    else error("undefined", "ImageMatrix");
    if (const Object* dec = dict_find(data_dict, "Decode"); dec != nullptr && dec->type == Type::Array) {
      for (std::uint32_t i = 0; i < dec->size; ++i) {
        const Object e = array_get(*dec, i);
        if (e.is_number()) spec.decode.push_back(e.number());
      }
    }
    if (const Object* ip = dict_find(data_dict, "Interpolate"); ip != nullptr && ip->type == Type::Boolean) {
      spec.interpolate = ip->ival != 0;
    }
    if (const Object* ms = dict_find(data_dict, "MultipleDataSources"); ms != nullptr && ms->type == Type::Boolean) {
      spec.multi = ms->ival != 0;
    }
    const Object* src = dict_find(data_dict, "DataSource");
    if (src == nullptr) error("undefined", "DataSource");
    if (spec.multi && src->type == Type::Array && !src->exec) {
      for (std::uint32_t i = 0; i < src->size; ++i) spec.sources.push_back(array_get(*src, i));
    } else {
      spec.sources.push_back(*src);
    }
    if (kind == 1) {
      spec.mask = true;
      spec.bpc = 1;
      spec.polarity = !spec.decode.empty() && spec.decode[0] > 0.5;
      spec.ncomp = 1;
    } else {
      spec.ncomp = std::max(1, space.components);
      if (space.family == Family::Pattern) error("rangecheck", "image in a Pattern colour space");
    }
  } else if (kind == 1) {
    const Object src = pop();
    spec.matrix = pop_matrix();
    spec.polarity = pop_bool();
    spec.height = pop_int();
    spec.width = pop_int();
    spec.bpc = 1;
    spec.mask = true;
    spec.sources.push_back(src);
  } else if (kind == 2) {
    const std::int64_t ncomp = pop_int();
    const bool multi = pop_bool();
    if (ncomp != 1 && ncomp != 3 && ncomp != 4) error("rangecheck");
    std::vector<Object> srcs;
    for (std::int64_t k = 0; k < (multi ? ncomp : 1); ++k) srcs.insert(srcs.begin(), pop());
    spec.sources = std::move(srcs);
    spec.multi = multi;
    spec.ncomp = static_cast<int>(ncomp);
    spec.matrix = pop_matrix();
    spec.bpc = static_cast<int>(pop_int());
    spec.height = pop_int();
    spec.width = pop_int();
    space = ColorSpace{};
    space.family = ncomp == 1 ? Family::Gray : (ncomp == 3 ? Family::RGB : Family::CMYK);
    space.components = static_cast<int>(ncomp);
  } else {
    const Object src = pop();
    spec.matrix = pop_matrix();
    spec.bpc = static_cast<int>(pop_int());
    spec.height = pop_int();
    spec.width = pop_int();
    spec.sources.push_back(src);
    space = ColorSpace{};
    space.family = Family::Gray;
    space.components = 1;
    spec.ncomp = 1;
  }
  if (spec.width <= 0 || spec.height <= 0) error("rangecheck", "bad image size");
  if (spec.bpc != 1 && spec.bpc != 2 && spec.bpc != 4 && spec.bpc != 8 && spec.bpc != 12 && spec.bpc != 16) {
    error("rangecheck", "bad BitsPerComponent");
  }
  if (auto r = ctx_.limits.check_pixels(static_cast<std::uint64_t>(spec.width), static_cast<std::uint64_t>(spec.height));
      !r) {
    fatal(Status::Limit, r.error().message);
  }
  const std::uint64_t pixels = static_cast<std::uint64_t>(spec.width) * static_cast<std::uint64_t>(spec.height);
  charge(pixels * 4);
  const std::uint32_t w = static_cast<std::uint32_t>(spec.width), h = static_cast<std::uint32_t>(spec.height);

  ir::Frame frame;
  frame.width = w;
  frame.height = h;
  Bytes encoded_jpeg;

  // JPEG data (DCTDecode): decode the stream as a whole.
  if (spec.sources.size() == 1 && spec.sources[0].type == Type::File) materialize(spec.sources[0].ref);
  const bool jpeg_source = spec.sources.size() == 1 && spec.sources[0].type == Type::File &&
                           file(spec.sources[0]).jpeg && !spec.mask;
  if (jpeg_source) {
    FileStore& fs = file(spec.sources[0]);
    const ByteView jpeg = as_bytes(fs.data);
    encoded_jpeg.assign(jpeg.begin(), jpeg.end());
    fs.pos = fs.data.size();
    bool ok = false;
    if (ctx_.registry != nullptr && image::can_decode("jpg", ctx_)) {
      auto img = image::decode("jpg", jpeg, ctx_);
      if (img && !img->frames.empty() && img->frames[0].valid()) {
        frame = std::move(img->frames[0]);
        ok = true;
      }
    }
    if (!ok) {
      warn_once("an embedded JPEG picture could not be decoded");
      frame = ir::Frame::filled(w, h, {128, 128, 128, 255});
      encoded_jpeg.clear();
    }
  } else {
    // Read the samples.
    const int ncomp = spec.mask ? 1 : spec.ncomp;
    const int per_pixel = spec.interleave == 1 ? ncomp + 1 : ncomp;
    const std::size_t row_single = (static_cast<std::size_t>(w) * static_cast<std::size_t>(spec.bpc) + 7) / 8;
    const std::size_t row_all =
        (static_cast<std::size_t>(w) * static_cast<std::size_t>(spec.bpc) * static_cast<std::size_t>(per_pixel) + 7) / 8;
    auto read_all = [&](const Object& src, std::size_t need_bytes) {
      std::string data;
      while (data.size() < need_bytes) {
        std::string chunk = read_source(src, need_bytes - data.size());
        if (chunk.empty()) break;
        data += chunk;
        tick();
      }
      if (data.size() > need_bytes) data.resize(need_bytes);
      return data;
    };
    std::vector<std::string> planes;
    if (spec.multi) {
      for (const Object& src : spec.sources) planes.push_back(read_all(src, row_single * h));
    } else {
      planes.push_back(read_all(spec.sources[0], row_all * h));
    }
    std::string mask_plane;
    if (!spec.mask_sources.empty() && spec.mask_width > 0 && spec.mask_height > 0 &&
        spec.mask_width <= 65536 && spec.mask_height <= 65536) {
      const std::size_t mrow = (static_cast<std::size_t>(spec.mask_width) * static_cast<std::size_t>(spec.mask_bpc) + 7) / 8;
      mask_plane = read_all(spec.mask_sources[0], mrow * static_cast<std::size_t>(spec.mask_height));
    }
    if (planes[0].size() < (spec.multi ? row_single : row_all) * h) {
      warn_once("image data ended early; the rest of the picture is blank");
    }

    const std::uint32_t maxv = (1u << std::min(spec.bpc, 16)) - 1;
    auto sample = [&](const std::string& plane, std::size_t row_bytes, std::uint32_t y, std::size_t index) -> std::uint32_t {
      const std::size_t bit = index * static_cast<std::size_t>(spec.bpc);
      const std::size_t at = static_cast<std::size_t>(y) * row_bytes + bit / 8;
      if (spec.bpc == 16) {
        if (at + 1 >= plane.size()) return 0;
        return static_cast<unsigned char>(plane[at]) * 256u + static_cast<unsigned char>(plane[at + 1]);
      }
      if (spec.bpc == 12) {
        if (at + 1 >= plane.size()) return 0;
        const std::uint32_t v = static_cast<unsigned char>(plane[at]) * 256u + static_cast<unsigned char>(plane[at + 1]);
        return (bit % 8 == 0) ? (v >> 4) : (v & 0xFFF);
      }
      if (at >= plane.size()) return 0;
      const std::uint32_t byte = static_cast<unsigned char>(plane[at]);
      const int shift = 8 - static_cast<int>(bit % 8) - spec.bpc;
      return (byte >> shift) & maxv;
    };

    frame.rgba.assign(static_cast<std::size_t>(w) * h * 4, 0);
    if (spec.mask) {
      const Color c = gstate_.color;
      const std::uint8_t r = static_cast<std::uint8_t>(std::lround(c.r * 255)),
                         g = static_cast<std::uint8_t>(std::lround(c.g * 255)),
                         b = static_cast<std::uint8_t>(std::lround(c.b * 255));
      for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
          const std::uint32_t v = sample(planes[0], row_all, y, x);
          const bool paint = spec.polarity ? v != 0 : v == 0;
          std::uint8_t* px = frame.rgba.data() + frame.offset(x, y);
          px[0] = r;
          px[1] = g;
          px[2] = b;
          px[3] = paint ? 255 : 0;
        }
      }
    } else {
      // Decode arrays.
      std::vector<double> dec = spec.decode;
      if (dec.size() < static_cast<std::size_t>(2 * ncomp)) {
        dec.clear();
        for (int k = 0; k < ncomp; ++k) {
          if (space.family == Family::Indexed) {
            dec.push_back(0);
            dec.push_back(maxv);
          } else if (space.family == Family::Lab) {
            const double lo[3] = {0, -128, -128}, hi[3] = {100, 127, 127};
            dec.push_back(lo[std::min(k, 2)]);
            dec.push_back(hi[std::min(k, 2)]);
          } else {
            dec.push_back(0);
            dec.push_back(1);
          }
        }
      }
      const bool simple = space.family == Family::Gray || space.family == Family::RGB || space.family == Family::CMYK;
      std::unordered_map<std::uint64_t, Color> cache;
      std::vector<double> comps(static_cast<std::size_t>(ncomp));
      std::vector<std::uint32_t> raw(static_cast<std::size_t>(ncomp));
      for (std::uint32_t y = 0; y < h; ++y) {
        if ((y & 63) == 0) tick();
        for (std::uint32_t x = 0; x < w; ++x) {
          std::uint64_t key = 0;
          std::uint32_t alpha_sample = maxv;
          for (int k = 0; k < ncomp; ++k) {
            std::uint32_t v;
            if (spec.multi) {
              v = sample(planes[static_cast<std::size_t>(k) < planes.size() ? static_cast<std::size_t>(k) : 0], row_single, y, x);
            } else if (spec.interleave == 1) {
              if (k == 0) alpha_sample = sample(planes[0], row_all, y, static_cast<std::size_t>(x) * static_cast<std::size_t>(per_pixel));
              v = sample(planes[0], row_all, y, static_cast<std::size_t>(x) * static_cast<std::size_t>(per_pixel) + static_cast<std::size_t>(k) + 1);
            } else {
              v = sample(planes[0], row_all, y, static_cast<std::size_t>(x) * static_cast<std::size_t>(ncomp) + static_cast<std::size_t>(k));
            }
            raw[static_cast<std::size_t>(k)] = v;
            key = key * (static_cast<std::uint64_t>(maxv) + 1) + v;
            comps[static_cast<std::size_t>(k)] =
                dec[static_cast<std::size_t>(2 * k)] +
                v * (dec[static_cast<std::size_t>(2 * k + 1)] - dec[static_cast<std::size_t>(2 * k)]) / maxv;
          }
          Color c;
          if (simple) {
            c = resolve_color(space, comps);
          } else if (auto it = cache.find(key); it != cache.end() && ncomp * spec.bpc <= 48) {
            c = it->second;
          } else {
            c = resolve_color(space, comps);
            if (cache.size() < 65536 && ncomp * spec.bpc <= 48) cache.emplace(key, c);
          }
          std::uint8_t* px = frame.rgba.data() + frame.offset(x, y);
          px[0] = static_cast<std::uint8_t>(std::lround(c.r * 255));
          px[1] = static_cast<std::uint8_t>(std::lround(c.g * 255));
          px[2] = static_cast<std::uint8_t>(std::lround(c.b * 255));
          px[3] = 255;
          if (spec.interleave == 1) {
            const bool shown = spec.mask_invert ? alpha_sample == 0 : alpha_sample != 0;
            px[3] = shown ? 255 : 0;
          }
          if (!spec.mask_color.empty()) {
            bool match = true;
            for (int k = 0; k < ncomp && match; ++k) {
              const std::size_t kk = static_cast<std::size_t>(k);
              if (spec.mask_color.size() == static_cast<std::size_t>(ncomp)) {
                match = std::lround(spec.mask_color[kk]) == static_cast<long>(raw[kk]);
              } else if (spec.mask_color.size() >= static_cast<std::size_t>(2 * ncomp)) {
                match = raw[kk] >= spec.mask_color[2 * kk] && raw[kk] <= spec.mask_color[2 * kk + 1];
              }
            }
            if (match) px[3] = 0;
          }
        }
      }
      if (!mask_plane.empty()) {
        // ImageType 3 separate mask: sample it at the image resolution.
        const std::size_t mrow = (static_cast<std::size_t>(spec.mask_width) * static_cast<std::size_t>(spec.mask_bpc) + 7) / 8;
        for (std::uint32_t y = 0; y < h; ++y) {
          const std::size_t my = static_cast<std::size_t>(y) * static_cast<std::size_t>(spec.mask_height) / h;
          for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t mx = static_cast<std::size_t>(x) * static_cast<std::size_t>(spec.mask_width) / w;
            const std::size_t at = my * mrow + mx / 8;
            const bool bit = at < mask_plane.size() && ((static_cast<unsigned char>(mask_plane[at]) >> (7 - mx % 8)) & 1) != 0;
            const bool shown = spec.mask_invert ? !bit : bit;
            frame.rgba[frame.offset(x, y) + 3] = shown ? frame.rgba[frame.offset(x, y) + 3] : 0;
          }
        }
      }
    }
  }

  if (!spec.matrix.invertible()) return;
  Picture pic;
  pic.bounds = Rect{0, 0, static_cast<double>(w), static_cast<double>(h)};
  pic.smooth = spec.interpolate;
  if (!encoded_jpeg.empty()) {
    pic.encoded = std::move(encoded_jpeg);
    pic.encoded_format = "jpg";
  }
  pic.pixels = std::make_shared<const ir::Frame>(std::move(frame));
  Group g;
  g.transform = gstate_.ctm * spec.matrix.inverted();
  g.children.push_back(Node{std::move(pic)});
  emit_node(Node{std::move(g)});
}

void Interpreter::op_image() { do_image(0); }
void Interpreter::op_imagemask() { do_image(1); }
void Interpreter::op_colorimage() { do_image(2); }

void Interpreter::install_image_operators() {
  define_operator("image", &Interpreter::op_image);
  define_operator("imagemask", &Interpreter::op_imagemask);
  define_operator("colorimage", &Interpreter::op_colorimage);
}

}  // namespace cvt::vector::ps
