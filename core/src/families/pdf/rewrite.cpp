// Convertible core - pdf family: PDF -> PDF rewriting and compression.
//
// The file is parsed and re-emitted with only the objects reachable from
// the (possibly reduced) page tree, every compressible stream recompressed
// with Flate at the strongest level, non-stream objects packed into object
// streams, a cross-reference stream, thumbnails and (by default) metadata
// removed, page resources pruned to what the content uses, encryption
// dropped, and images optionally downsampled / recompressed.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <deque>
#include <format>
#include <map>
#include <set>

#include "engine/standard_options.hpp"
#include "families/pdf/builder.hpp"
#include "families/pdf/interp.hpp"
#include "families/pdf/pdf.hpp"
#include "families/pdf/pdf_image.hpp"
#include "families/pdf/syntax.hpp"
#include "families/pdf/write_common.hpp"

namespace cvt::pdf {

namespace {

// Records the largest displayed size (in points) of each image XObject.
class ImageUseDevice final : public Device {
 public:
  ImageUseDevice() = default;
  std::map<std::uint32_t, double> max_points;  // object number -> max(width, height) in points
  void image(const ImageEvent& ev, const GState& gs) override {
    if (ev.is_inline || !ev.stream.is_stream()) return;
    const auto* ptr = ev.stream.stream_ptr().get();
    const auto it = by_ptr_.find(ptr);
    if (it == by_ptr_.end()) return;
    const Point u = gs.ctm.apply_vector(Point{1, 0}), v = gs.ctm.apply_vector(Point{0, 1});
    const double size = std::max(std::hypot(u.x, u.y), std::hypot(v.x, v.y));
    double& slot = max_points[it->second];
    slot = std::max(slot, size);
  }
  [[nodiscard]] bool wants_paths() const override { return false; }
  [[nodiscard]] bool execute_type3() const override { return false; }
  void index(std::uint32_t num, const Object& stream) { by_ptr_[stream.stream_ptr().get()] = num; }

 private:
  std::map<const Stream*, std::uint32_t> by_ptr_;
};

// Names used by a content stream, per resource category.
std::map<std::string, std::set<std::string>> used_resources(ByteView content, bool& ok) {
  std::map<std::string, std::set<std::string>> used;
  Lexer lexer(content);
  std::vector<Token> operands;
  ok = true;
  std::size_t guard = 0;
  while (++guard < 50'000'000) {
    Token t = lexer.next();
    if (t.kind == Token::Kind::End) break;
    if (t.kind == Token::Kind::Keyword) {
      const std::string& op = t.text;
      const auto last_name = [&]() -> std::string {
        for (auto it = operands.rbegin(); it != operands.rend(); ++it) {
          if (it->kind == Token::Kind::Name) return it->text;
        }
        return {};
      };
      if (op == "Tf" && operands.size() >= 2 && operands[operands.size() - 2].kind == Token::Kind::Name) {
        used["Font"].insert(operands[operands.size() - 2].text);
      } else if (op == "Do") {
        used["XObject"].insert(last_name());
      } else if (op == "gs") {
        used["ExtGState"].insert(last_name());
      } else if (op == "cs" || op == "CS") {
        used["ColorSpace"].insert(last_name());
      } else if (op == "scn" || op == "SCN") {
        used["Pattern"].insert(last_name());
      } else if (op == "sh") {
        used["Shading"].insert(last_name());
      } else if (op == "BDC" || op == "DP") {
        used["Properties"].insert(last_name());
      } else if (op == "BI") {
        // Inline image: its /CS may name a colour space; skip to EI.
        while (true) {
          Token k = lexer.next();
          if (k.kind == Token::Kind::End) return used;
          if (k.kind == Token::Kind::Name && (k.text == "CS" || k.text == "ColorSpace")) {
            Token v = lexer.next();
            if (v.kind == Token::Kind::Name) used["ColorSpace"].insert(v.text);
          }
          if (k.kind == Token::Kind::Keyword && k.text == "ID") break;
        }
        const std::string_view text = as_chars(content);
        const std::size_t ei = text.find("EI", lexer.position());
        if (ei == std::string_view::npos) break;
        lexer.seek(ei + 2);
      } else if (op == "true" || op == "false" || op == "null") {
        operands.push_back(t);
        continue;
      }
      operands.clear();
      continue;
    }
    if (t.kind == Token::Kind::Error) continue;
    operands.push_back(std::move(t));
    if (operands.size() > 64) operands.erase(operands.begin());
  }
  return used;
}

ir::Frame downsample(const DecodedImage& img, std::uint32_t w, std::uint32_t h) {
  ir::Frame out = ir::Frame::filled(w, h);
  for (std::uint32_t y = 0; y < h; ++y) {
    const double sy0 = static_cast<double>(y) * img.height / h, sy1 = static_cast<double>(y + 1) * img.height / h;
    for (std::uint32_t x = 0; x < w; ++x) {
      const double sx0 = static_cast<double>(x) * img.width / w, sx1 = static_cast<double>(x + 1) * img.width / w;
      double acc[4] = {0, 0, 0, 0}, total = 0;
      for (auto yy = static_cast<std::uint32_t>(sy0); yy < std::min<double>(img.height, std::ceil(sy1)); ++yy) {
        const double wy = std::min<double>(yy + 1, sy1) - std::max<double>(yy, sy0);
        for (auto xx = static_cast<std::uint32_t>(sx0); xx < std::min<double>(img.width, std::ceil(sx1)); ++xx) {
          const double wx = std::min<double>(xx + 1, sx1) - std::max<double>(xx, sx0);
          const double wgt = wx * wy;
          const std::uint8_t* p = img.rgba.data() + (static_cast<std::size_t>(yy) * img.width + xx) * 4;
          const double a = p[3] / 255.0;
          for (int c = 0; c < 3; ++c) acc[c] += p[c] * a * wgt;
          acc[3] += p[3] * wgt;
          total += wgt;
        }
      }
      std::uint8_t* d = out.rgba.data() + (static_cast<std::size_t>(y) * w + x) * 4;
      if (total > 0 && acc[3] > 0) {
        for (int c = 0; c < 3; ++c) d[c] = static_cast<std::uint8_t>(std::clamp(acc[c] / acc[3] * 255.0, 0.0, 255.0));
        d[3] = static_cast<std::uint8_t>(std::clamp(acc[3] / total, 0.0, 255.0));
      }
    }
  }
  return out;
}

class Rewriter {
 public:
  Rewriter(PdfFile& file, WriteContext& ctx) : file_(file), ctx_(ctx), builder_(9) {}

  Result<Bytes> run() {
    const auto& pages = file_.pages();
    CVT_TRY_ASSIGN(const std::vector<int> selected,
                   parse_page_ranges(ctx_.options.get_string("pages", ""), static_cast<int>(pages.size())));
    full_ = selected.size() == pages.size();
    const options::Preset preset = options::resolved_preset(ctx_.options);
    quality_ = options::resolved_quality(ctx_.options);
    level_ = std::max(1, options::resolved_compression_level(ctx_.options, 9));
    strip_ = options::resolved_strip_metadata(ctx_.options);
    recompress_ = ctx_.options.get_bool("recompress_images", true);
    max_dpi_ = static_cast<double>(ctx_.options.get_int("max_image_dpi", 0, 0, 2400));
    if (max_dpi_ == 0) {
      max_dpi_ = preset == options::Preset::Small ? 120 : (preset == options::Preset::Balanced ? 200 : 0);
      if (ctx_.options.was_set("quality") && quality_ < 60) max_dpi_ = 150;
    }
    // Old page objects: selected ones get new numbers, the others become null.
    std::set<std::uint32_t> selected_nums;
    for (const int i : selected) selected_nums.insert(pages[static_cast<std::size_t>(i)].ref.num);
    for (const Page& p : pages) {
      if (p.ref.num != 0 && !selected_nums.contains(p.ref.num)) dropped_.insert(p.ref.num);
    }
    if (max_dpi_ > 0 || quality_ < 90) measure_images(selected);
    pages_root_ = builder_.reserve();
    Array kids;
    for (std::size_t k = 0; k < selected.size(); ++k) {
      CVT_TRY(ctx_.checkpoint(0.8 * static_cast<double>(k) / static_cast<double>(selected.size()), "Rewriting"));
      const Page& page = pages[static_cast<std::size_t>(selected[k])];
      kids.push_back(copy_page(page));
    }
    CVT_TRY(drain());
    Dict pages_dict;
    pages_dict.set("Type", Object::name("Pages"));
    pages_dict.set("Count", static_cast<std::int64_t>(kids.size()));
    pages_dict.set("Kids", Object(std::move(kids)));
    builder_.set(pages_root_, Object(std::move(pages_dict)));
    // Catalog.
    const Dict& old_catalog = file_.catalog().as_dict();
    Dict catalog;
    catalog.set("Type", Object::name("Catalog"));
    catalog.set("Pages", pages_root_);
    static constexpr std::string_view kKeep[] = {"Lang", "ViewerPreferences", "PageLayout", "PageMode", "MarkInfo",
                                                 "AcroForm", "OCProperties", "Names", "Dests"};
    static constexpr std::string_view kKeepFull[] = {"Outlines", "PageLabels", "StructTreeRoot", "OpenAction"};
    for (const std::string_view key : kKeep) {
      if (old_catalog.contains(key)) catalog.set(key, copy(old_catalog.get(key)));
    }
    if (full_) {
      for (const std::string_view key : kKeepFull) {
        if (old_catalog.contains(key)) catalog.set(key, copy(old_catalog.get(key)));
      }
    }
    if (!strip_ && old_catalog.contains("Metadata")) catalog.set("Metadata", copy(old_catalog.get("Metadata")));
    CVT_TRY(drain());
    const Ref root = builder_.add(Object(std::move(catalog)));
    Dict info;
    const Dict& old_info = file_.info().as_dict();
    if (!strip_) {
      for (const auto& [key, value] : old_info) info.set(key, copy(value));
      CVT_TRY(drain());
    } else if (old_info.contains("Title")) {
      info.set("Title", copy(old_info.get("Title")));
    }
    info.set("Producer", Object::string("Convertible"));
    const Ref info_ref = builder_.add(Object(std::move(info)));
    CVT_TRY(ctx_.checkpoint(0.9, "Writing"));
    std::string version = file_.version();
    if (version < "1.5" || version.size() != 3) version = "1.7";
    CVT_TRY_ASSIGN(Bytes out, builder_.finish(root, info_ref, version));
    // Self-check: the result must parse with the same page count.
    auto check = PdfFile::open(out);
    if (!check || (*check)->pages().size() != selected.size()) {
      return fail(Status::Internal, "the rewritten PDF failed its consistency check");
    }
    if (full_ && !file_.encrypted() && out.size() >= file_.data().size()) {
      ctx_.warn("The PDF is already compact; it was kept unchanged.");
      return Bytes(file_.data().begin(), file_.data().end());
    }
    CVT_TRY(ctx_.limits.check_output(out.size()));
    return out;
  }

 private:
  void measure_images(const std::vector<int>& selected) {
    ImageUseDevice device;
    for (const Ref ref : file_.object_refs()) {
      const Object obj = file_.get(ref);
      if (obj.is_stream() && file_.get(obj.as_dict(), "Subtype").is_name("Image")) device.index(ref.num, obj);
    }
    FontCache fonts;
    for (const int i : selected) {
      Interpreter interp(file_, device, fonts, &ctx_);
      interp.limits.max_operators = 5'000'000;
      interp.run_page(file_.pages()[static_cast<std::size_t>(i)], Matrix{});
    }
    image_points_ = std::move(device.max_points);
  }

  Ref copy_page(const Page& page) {
    const Ref ref = map_ref(page.ref.num != 0 ? page.ref : Ref{0, 0});
    Dict d;
    for (const auto& [key, value] : page.dict) {
      if (key == "Parent" || key == "Resources" || key == "MediaBox" || key == "CropBox" || key == "Rotate" ||
          key == "Thumb" || key == "PieceInfo" || key == "LastModified" || key == "B" || key == "Type") {
        continue;
      }
      d.set(key, copy(value));
    }
    d.set("Type", Object::name("Page"));
    d.set("Parent", pages_root_);
    d.set("MediaBox", Array{Object(page.media_box.x0), Object(page.media_box.y0), Object(page.media_box.x1),
                            Object(page.media_box.y1)});
    if (page.crop_box.x0 != page.media_box.x0 || page.crop_box.y0 != page.media_box.y0 ||
        page.crop_box.x1 != page.media_box.x1 || page.crop_box.y1 != page.media_box.y1) {
      d.set("CropBox", Array{Object(page.crop_box.x0), Object(page.crop_box.y0), Object(page.crop_box.x1),
                             Object(page.crop_box.y1)});
    }
    if (page.rotate != 0) d.set("Rotate", page.rotate);
    // Resources pruned to the names the content uses.
    const Bytes content = page_content(file_, page);
    bool ok = true;
    const auto used = used_resources(content, ok);
    const Dict& res = page.resources.as_dict();
    Dict pruned;
    for (const auto& [category, value] : res) {
      const Object v = file_.resolve(value);
      static constexpr std::string_view kPrunable[] = {"Font", "XObject", "ExtGState", "ColorSpace", "Pattern",
                                                       "Shading", "Properties"};
      const bool prunable = ok && std::find(std::begin(kPrunable), std::end(kPrunable), category) != std::end(kPrunable);
      if (!prunable || !v.is_dict()) {
        pruned.set(category, copy(value));
        continue;
      }
      const auto it = used.find(category);
      Dict sub;
      for (const auto& [name, entry] : v.as_dict()) {
        if (it != used.end() && it->second.contains(name)) sub.set(name, copy(entry));
      }
      if (!sub.empty()) pruned.set(category, Object(std::move(sub)));
    }
    d.set("Resources", Object(std::move(pruned)));
    builder_.set(ref, Object(std::move(d)));
    return ref;
  }

  Ref map_ref(Ref old) {
    if (old.num == 0) return builder_.reserve();
    const auto it = mapping_.find(old.num);
    if (it != mapping_.end()) return it->second;
    const Ref fresh = builder_.reserve();
    mapping_[old.num] = fresh;
    return fresh;
  }

  Object copy(const Object& o, int depth = 0) {
    if (depth > 64) return {};
    switch (o.type()) {
      case Object::Type::Ref: {
        const Ref r = o.as_ref();
        if (dropped_.contains(r.num)) return {};
        if (const auto it = mapping_.find(r.num); it != mapping_.end()) return it->second;
        const Object target = file_.get(r);
        if (target.is_null()) return {};
        const Ref fresh = map_ref(r);
        queue_.push_back(r);
        return fresh;
      }
      case Object::Type::Array: {
        Array a;
        for (const Object& item : o.as_array()) a.push_back(copy(item, depth + 1));
        return a;
      }
      case Object::Type::Dict: {
        Dict d;
        for (const auto& [key, value] : o.as_dict()) {
          if (key == "Thumb" || key == "PieceInfo") continue;
          if (strip_ && key == "Metadata") continue;
          Object v = copy(value, depth + 1);
          if (!v.is_null()) d.set(key, std::move(v));
        }
        return d;
      }
      case Object::Type::Stream: return {};  // streams are always indirect
      default: return o;
    }
  }

  Result<void> drain() {
    std::size_t done = 0;
    while (!queue_.empty()) {
      if ((++done & 1023) == 0) CVT_TRY(ctx_.checkpoint(0.8, "Rewriting"));
      const Ref old = queue_.front();
      queue_.pop_front();
      const Ref fresh = mapping_.at(old.num);
      const Object obj = file_.get(old);
      if (obj.is_stream()) {
        write_stream(old, fresh, obj);
      } else {
        builder_.set(fresh, copy(obj));
      }
    }
    return {};
  }

  void write_stream(Ref old, Ref fresh, const Object& obj) {
    const Stream& s = *obj.as_stream();
    Dict dict;
    for (const auto& [key, value] : s.dict) {
      if (key == "Length" || key == "Filter" || key == "DecodeParms" || key == "DL") continue;
      Object v = copy(value);
      if (!v.is_null()) dict.set(key, std::move(v));
    }
    const bool image = file_.get(s.dict, "Subtype").is_name("Image");
    if (image && try_image(old, fresh, obj, dict)) return;
    auto decoded = file_.decode(obj, true);
    if (!decoded || !decoded->pending_filter.empty() || decoded->damaged) {
      // Keep the original encoding (image codecs, unsupported or damaged data).
      Dict keep = dict;
      for (const char* key : {"Filter", "DecodeParms", "DL"}) {
        if (s.dict.contains(key)) keep.set(key, copy(s.dict.get(key)));
      }
      builder_.set_stream_raw(fresh, std::move(keep), s.data);
      return;
    }
    // Recompressed with Flate (images keep a PNG predictor).
    if (image) {
      const int bpc = static_cast<int>(file_.get(s.dict, "BitsPerComponent").as_int(8));
      const auto space = load_color_space(file_, s.dict.get("ColorSpace"), nullptr);
      const int colors = space ? std::max(1, space->components()) : 1;
      const int width = static_cast<int>(file_.get(s.dict, "Width").as_int(0));
      if (bpc == 8 && width > 0 && !file_.get(s.dict, "ImageMask").as_bool(false) &&
          decoded->data.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(colors) *
                                      static_cast<std::size_t>(std::max<std::int64_t>(0, file_.get(s.dict, "Height").as_int(0)))) {
        const Bytes predicted = png_predict(decoded->data, colors, 8, width);
        Bytes packed = flate_encode(predicted, level_);
        Bytes plain = flate_encode(decoded->data, level_);
        if (packed.size() < plain.size()) {
          Dict parms;
          parms.set("Predictor", 15);
          parms.set("Colors", colors);
          parms.set("BitsPerComponent", 8);
          parms.set("Columns", width);
          dict.set("DecodeParms", Object(std::move(parms)));
          dict.set("Filter", Object::name("FlateDecode"));
          builder_.set_stream_raw(fresh, std::move(dict), std::move(packed));
        } else {
          dict.set("Filter", Object::name("FlateDecode"));
          builder_.set_stream_raw(fresh, std::move(dict), std::move(plain));
        }
        return;
      }
    }
    Bytes compressed = flate_encode(decoded->data, level_);
    if (compressed.size() + 8 < decoded->data.size()) {
      dict.set("Filter", Object::name("FlateDecode"));
      builder_.set_stream_raw(fresh, std::move(dict), std::move(compressed));
    } else {
      builder_.set_stream_raw(fresh, std::move(dict), std::move(decoded->data));
    }
  }

  // Downsample / recompress an image when that is asked for and helps.
  bool try_image(Ref old, Ref fresh, const Object& obj, Dict& dict) {
    if (!recompress_) return false;
    const Dict& sd = obj.as_dict();
    if (file_.get(sd, "ImageMask").as_bool(false) || sd.contains("Mask")) return false;
    const std::int64_t w = file_.get(sd, "Width").as_int(0), h = file_.get(sd, "Height").as_int(0);
    if (w <= 0 || h <= 0) return false;
    double target_scale = 1.0;
    if (max_dpi_ > 0) {
      const auto it = image_points_.find(old.num);
      if (it != image_points_.end() && it->second > 1) {
        const double dpi = static_cast<double>(std::max(w, h)) / (it->second / 72.0);
        if (dpi > max_dpi_ * 1.15) target_scale = max_dpi_ / dpi;
      }
    }
    const Object filter = file_.get(sd, "Filter");
    const bool is_dct = filter.is_name("DCTDecode") ||
                        (filter.is_array() && !filter.as_array().empty() && file_.resolve(filter.as_array().back()).is_name("DCTDecode"));
    const bool lossy_ok = quality_ < 90;
    if (target_scale >= 1.0 && !(is_dct && lossy_ok)) return false;
    // Only plain colour images whose look survives a re-encode.
    const auto space = load_color_space(file_, sd.get("ColorSpace"), nullptr);
    if (!space || (space->kind() != ColorSpace::Kind::Gray && space->kind() != ColorSpace::Kind::Rgb &&
                   space->kind() != ColorSpace::Kind::Indexed && space->kind() != ColorSpace::Kind::Cmyk)) {
      return false;
    }
    if (file_.get(sd, "BitsPerComponent").as_int(8) != 8 && space->kind() != ColorSpace::Kind::Indexed) return false;
    ImageEvent ev;
    ev.dict = &sd;
    ev.stream = obj;
    auto decoded = decode_image(file_, ev, &ctx_);
    if (!decoded) return false;
    DecodedImage img = std::move(*decoded);
    // The soft mask stays as it is (it is copied as /SMask).
    for (std::size_t i = 3; i < img.rgba.size(); i += 4) img.rgba[i] = 255;
    ir::Frame frame;
    if (target_scale < 1.0) {
      const auto nw = static_cast<std::uint32_t>(std::max(1.0, std::round(static_cast<double>(w) * target_scale)));
      const auto nh = static_cast<std::uint32_t>(std::max(1.0, std::round(static_cast<double>(h) * target_scale)));
      frame = downsample(img, nw, nh);
    } else {
      frame.width = img.width;
      frame.height = img.height;
      frame.rgba = std::move(img.rgba);
    }
    PdfBuilder scratch(level_);
    ImageEncoding enc;
    enc.quality = quality_;
    enc.lossless = !(is_dct || lossy_ok);
    enc.compression_level = level_;
    // Encode into a scratch builder to compare sizes, then transplant.
    auto embedded = embed_image(scratch, frame, enc, ctx_);
    if (!embedded) return false;
    auto bytes = scratch.finish(*embedded, std::nullopt, "1.7", false);
    if (!bytes) return false;
    // Parse the scratch result back to get the stream dictionary and data.
    auto parsed = PdfFile::open(*bytes);
    if (!parsed) return false;
    const Object enc_obj = (*parsed)->get(*embedded);
    const Stream* es = enc_obj.as_stream();
    if (es == nullptr || es->data.size() >= obj.as_stream()->data.size()) return false;
    Dict out = dict;
    for (const char* key : {"Filter", "DecodeParms", "Width", "Height", "ColorSpace", "BitsPerComponent"}) {
      if (es->dict.contains(key)) out.set(key, es->dict.get(key));
    }
    out.erase("Decode");
    builder_.set_stream_raw(fresh, std::move(out), es->data);
    return true;
  }

  PdfFile& file_;
  WriteContext& ctx_;
  PdfBuilder builder_;
  Ref pages_root_{};
  std::map<std::uint32_t, Ref> mapping_;
  std::set<std::uint32_t> dropped_;
  std::deque<Ref> queue_;
  std::map<std::uint32_t, double> image_points_;
  bool full_ = true;
  bool strip_ = true;
  bool recompress_ = true;
  int quality_ = 90;
  int level_ = 9;
  double max_dpi_ = 0;
};

}  // namespace

Result<Bytes> rewrite_pdf(PdfFile& file, WriteContext& ctx) {
  Rewriter rewriter(file, ctx);
  CVT_TRY_ASSIGN(Bytes out, rewriter.run());
  for (const std::string& w : file.warnings()) ctx.warn(w);
  return out;
}

}  // namespace cvt::pdf
