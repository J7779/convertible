// Convertible core - vector family: Enhanced Metafile (EMF) reader, after [MS-EMF].
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "families/vector/metafile_internal.hpp"
#include "util/text.hpp"

namespace cvt::vector::metafile {

namespace {

constexpr std::uint32_t emf_signature = 0x464D4520u;  // " EMF"
constexpr std::uint32_t emfplus_comment = 0x2B464D45u;  // "EMF+"

Color color_ref(std::uint32_t v) {
  return Color::rgb8(static_cast<int>(v & 0xFF), static_cast<int>((v >> 8) & 0xFF), static_cast<int>((v >> 16) & 0xFF));
}

/// Bounds-checked field access at fixed offsets of a record.
struct Rec {
  ByteView data;
  [[nodiscard]] std::uint32_t u32(std::size_t at) const {
    util::ByteReader r(data);
    r.seek(at);
    const std::uint32_t v = r.u32le();
    return r.ok() ? v : 0;
  }
  [[nodiscard]] std::int32_t i32(std::size_t at) const { return static_cast<std::int32_t>(u32(at)); }
  [[nodiscard]] double d(std::size_t at) const { return static_cast<double>(i32(at)); }
  [[nodiscard]] float f32(std::size_t at) const { return std::bit_cast<float>(u32(at)); }
  [[nodiscard]] std::uint16_t u16(std::size_t at) const {
    util::ByteReader r(data);
    r.seek(at);
    const std::uint16_t v = r.u16le();
    return r.ok() ? v : 0;
  }
  [[nodiscard]] std::uint8_t u8(std::size_t at) const { return at < data.size() ? static_cast<std::uint8_t>(data[at]) : 0; }
  [[nodiscard]] Point point(std::size_t at) const { return {d(at), d(at + 4)}; }
  [[nodiscard]] Rect rect(std::size_t at) const { return {d(at), d(at + 4), d(at + 8), d(at + 12)}; }
  /// [offset, offset + size) of the record, or empty when out of range.
  [[nodiscard]] ByteView slice(std::uint64_t offset, std::uint64_t size) const {
    if (offset > data.size() || size > data.size() - offset) return {};
    return data.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(size));
  }
  [[nodiscard]] bool has(std::size_t bytes) const { return data.size() >= bytes; }
};

std::vector<Point> points_at(const Rec& rec, std::size_t at, std::uint64_t count, bool small) {
  std::vector<Point> out;
  const std::size_t each = small ? 4 : 8;
  if (at > rec.data.size()) return out;
  count = std::min<std::uint64_t>(count, (rec.data.size() - at) / each);
  out.reserve(static_cast<std::size_t>(count));
  util::ByteReader r(rec.data.subspan(at));
  for (std::uint64_t i = 0; i < count; ++i) {
    if (small) {
      const double x = r.i16le(), y = r.i16le();
      out.push_back({x, y});
    } else {
      const double x = r.i32le(), y = r.i32le();
      out.push_back({x, y});
    }
  }
  return out;
}

std::vector<Rect> region_rects(ByteView rgn) {
  std::vector<Rect> rects;
  util::ByteReader r(rgn);
  const std::uint32_t header = r.u32le();
  r.skip(4);
  const std::uint32_t count = r.u32le();
  if (!r.ok() || header < 32 || header > rgn.size()) return rects;
  r.seek(header);
  const std::uint64_t n = std::min<std::uint64_t>(count, r.remaining() / 16);
  rects.reserve(static_cast<std::size_t>(n));
  for (std::uint64_t i = 0; i < n; ++i) {
    const double l = r.i32le(), t = r.i32le(), rr = r.i32le(), b = r.i32le();
    if (rr > l && b > t) rects.push_back({l, t, rr, b});
  }
  return rects;
}

class EmfReader {
 public:
  EmfReader(const Context& ctx, ByteView data, const ReadOptions& options, int depth)
      : ctx_(ctx), data_(data), options_(options), depth_(depth), gdi_(Flavor::Emf, ctx) {}

  Result<Scene> run() {
    const Rec h{data_};
    if (!looks_like_emf(data_)) return fail(Status::Decode, "not an enhanced metafile");
    const std::uint32_t header_size = h.u32(4);
    if (header_size < 88 || header_size > data_.size()) return fail(Status::Decode, "malformed EMF header");
    const Rect bounds = h.rect(8);
    const Rect frame = h.rect(24);
    const std::uint16_t handles = h.u16(48);
    const double dev_w = h.d(72), dev_h = h.d(76), mm_w = h.d(80), mm_h = h.d(84);
    double um_w = 0, um_h = 0;
    const std::uint32_t off_desc = h.u32(64);
    if (header_size >= 108 && (off_desc == 0 || off_desc >= 108)) {
      um_w = h.u32(100);
      um_h = h.u32(104);
    }
    // Device pixels per millimetre of the reference device.
    double ppmm_x = 96.0 / 25.4, ppmm_y = 96.0 / 25.4;
    if (dev_w > 0 && dev_h > 0) {
      if (um_w > 0 && um_h > 0) {
        ppmm_x = dev_w / (um_w / 1000.0);
        ppmm_y = dev_h / (um_h / 1000.0);
      } else if (mm_w > 0 && mm_h > 0) {
        ppmm_x = dev_w / mm_w;
        ppmm_y = dev_h / mm_h;
      }
    }
    if (!std::isfinite(ppmm_x) || ppmm_x <= 0 || !std::isfinite(ppmm_y) || ppmm_y <= 0) {
      ppmm_x = ppmm_y = 96.0 / 25.4;
    }
    gdi_.set_device_resolution(ppmm_x, ppmm_y);
    const double sx = px_per_mm / ppmm_x, sy = px_per_mm / ppmm_y;
    double page_w = 0, page_h = 0;
    bool have_page = false;
    if (frame.x1 > frame.x0 && frame.y1 > frame.y0) {
      page_w = (frame.x1 - frame.x0) / 100.0 * px_per_mm;
      page_h = (frame.y1 - frame.y0) / 100.0 * px_per_mm;
      gdi_.set_device_to_page(Matrix{sx, 0, 0, sy, -frame.x0 / 100.0 * px_per_mm, -frame.y0 / 100.0 * px_per_mm});
      have_page = true;
    } else if (bounds.x1 >= bounds.x0 && bounds.y1 >= bounds.y0 && (bounds.x1 > bounds.x0 || bounds.y1 > bounds.y0)) {
      page_w = (bounds.x1 - bounds.x0 + 1) * sx;
      page_h = (bounds.y1 - bounds.y0 + 1) * sy;
      gdi_.set_device_to_page(Matrix{sx, 0, 0, sy, -bounds.x0 * sx, -bounds.y0 * sy});
      have_page = true;
    } else {
      gdi_.set_device_to_page(Matrix::scale(sx, sy));
    }
    gdi_.ensure_table(std::size_t{handles} + 1);
    if (options_.use_emf_plus) plus_ = std::make_unique<EmfPlusPlayer>(gdi_, options_, depth_);

    std::size_t pos = header_size;
    std::size_t records = 0;
    bool ended = false;
    while (data_.size() - pos >= 8) {
      const Rec head{data_.subspan(pos)};
      const std::uint32_t type = head.u32(0);
      const std::uint32_t size = head.u32(4);
      if (size < 8 || size > data_.size() - pos) {
        return fail(Status::Decode, "EMF record {} is truncated or malformed", records + 1);
      }
      const Rec rec{data_.subspan(pos, size)};
      if (type == 14) {
        ended = true;
        break;
      }
      CVT_TRY(play(type, rec));
      pos += size;
      if ((++records & 1023) == 0) CVT_TRY(ctx_.checkpoint(static_cast<double>(pos) / static_cast<double>(data_.size())));
    }
    if (!ended) gdi_.note("The EMF file has no end record; it may be truncated.");

    CVT_TRY_ASSIGN(Group root, gdi_.finish());
    Scene scene;
    scene.source_units = "mm";
    Page page;
    if (have_page && std::isfinite(page_w) && std::isfinite(page_h) && page_w > 0 && page_h > 0) {
      page.width = page_w;
      page.height = page_h;
    } else {
      Rect b = drawing_bounds(root);
      if (b.empty() || !std::isfinite(b.width()) || !std::isfinite(b.height())) b = Rect::xywh(0, 0, 1, 1);
      page.width = std::max(1.0, b.width());
      page.height = std::max(1.0, b.height());
      root.transform = Matrix::translate(-b.x0, -b.y0);
    }
    page.root = std::move(root);
    scene.pages.push_back(std::move(page));
    return scene;
  }

 private:
  Result<FramePtr> bitmap(const Rec& rec, std::size_t off_bmi, std::size_t off_bits, std::uint32_t usage,
                          bool keep_alpha = false) {
    const ByteView info = rec.slice(rec.u32(off_bmi), rec.u32(off_bmi + 4));
    const ByteView bits = rec.slice(rec.u32(off_bits), rec.u32(off_bits + 4));
    if (info.empty()) return FramePtr{};
    DibRequest req;
    req.info = info;
    req.bits = bits;
    req.palette_indices = usage == 1;
    req.palette = gdi_.dc().palette ? &gdi_.dc().palette->colors : nullptr;
    req.keep_alpha = keep_alpha;
    req.premultiplied = keep_alpha;
    if (bits.empty()) return FramePtr{};
    auto frame = decode_dib(req, ctx_, gdi_.pixels());
    if (!frame) {
      if (frame.error().code == Status::Limit || frame.error().code == Status::Cancelled) return frame;
      gdi_.note("Some bitmaps could not be decoded and were left out.");
      return FramePtr{};
    }
    return frame;
  }

  /// Crop the source rectangle; `dib_origin` = y measured from the bottom of bottom-up DIBs.
  static FramePtr crop(const FramePtr& frame, ByteView info, double xs, double ys, double ws, double hs,
                       bool dib_origin) {
    if (!frame) return frame;
    double top = ys;
    if (dib_origin && dib_bottom_up(info)) top = frame->height - ys - std::abs(hs);
    return crop_frame(frame, static_cast<std::int64_t>(xs), static_cast<std::int64_t>(top),
                      static_cast<std::int64_t>(std::abs(ws)), static_cast<std::int64_t>(std::abs(hs)));
  }

  /// Make pixels transparent where a monochrome mask is black.
  Result<FramePtr> apply_mask(const FramePtr& frame, const Rec& rec, std::size_t off_bmi, std::size_t off_bits,
                              double xm, double ym) {
    if (!frame) return frame;
    CVT_TRY_ASSIGN(FramePtr mask, bitmap(rec, off_bmi, off_bits, 0));
    if (!mask) return frame;
    auto copy = std::make_shared<ir::Frame>(*frame);
    for (std::uint32_t y = 0; y < copy->height; ++y) {
      for (std::uint32_t x = 0; x < copy->width; ++x) {
        const std::int64_t mx = static_cast<std::int64_t>(xm) + x, my = static_cast<std::int64_t>(ym) + y;
        if (mx < 0 || my < 0 || mx >= mask->width || my >= mask->height) continue;
        const std::size_t m = mask->offset(static_cast<std::uint32_t>(mx), static_cast<std::uint32_t>(my));
        if (mask->rgba[m] < 128) copy->rgba[copy->offset(x, y) + 3] = 0;
      }
    }
    return FramePtr(std::move(copy));
  }

  Result<void> text(const Rec& rec, std::size_t at, bool wide, double scale_x) {
    GdiPlayer::TextRun run;
    run.reference = rec.point(at);
    const std::uint32_t count = rec.u32(at + 8);
    const std::uint32_t off_string = rec.u32(at + 12);
    run.options = rec.u32(at + 16);
    std::size_t next = at + 20;
    if ((run.options & 0x100) == 0) {
      const Rect r = rec.rect(next);
      if (r.x1 != r.x0 && r.y1 != r.y0) run.rect = r;
      next += 16;
    }
    const std::uint32_t off_dx = rec.u32(next);
    const ByteView chars = rec.slice(off_string, std::uint64_t{count} * (wide ? 2 : 1));
    if (chars.empty() && count > 0) return {};
    if (wide) {
      std::u16string u16;
      util::ByteReader r(chars);
      for (std::uint32_t i = 0; i < count; ++i) u16.push_back(static_cast<char16_t>(r.u16le()));
      run.chars = util::utf8_to_utf32(util::utf16_to_utf8(u16));
    } else {
      const int charset = gdi_.dc().font.charset;
      if (!charset_supported(charset)) gdi_.note("Text in East Asian double-byte character sets is not decoded.");
      run.chars = util::utf8_to_utf32(charset_to_utf8(chars, charset));
    }
    const bool pdy = (run.options & 0x2000) != 0;
    if (off_dx != 0) {
      const ByteView dx = rec.slice(off_dx, std::uint64_t{count} * (pdy ? 8 : 4));
      if (!dx.empty()) {
        util::ByteReader r(dx);
        for (std::uint32_t i = 0; i < count; ++i) {
          run.dx.push_back(static_cast<double>(r.i32le()));
          if (pdy) run.dy.push_back(static_cast<double>(r.i32le()));
        }
      }
    }
    if (run.dx.size() != run.chars.size()) {
      run.dx.clear();
      run.dy.clear();
    }
    run.scale_x = scale_x;
    return gdi_.text_out(run);
  }

  Result<void> play(std::uint32_t type, const Rec& rec);

  const Context& ctx_;
  ByteView data_;
  ReadOptions options_;
  int depth_;
  GdiPlayer gdi_;
  std::unique_ptr<EmfPlusPlayer> plus_;
};

Result<void> EmfReader::play(std::uint32_t type, const Rec& rec) {
  DcState& dc = gdi_.dc();
  // Poly records: bounds, count, points.
  const auto poly = [&](bool small) { return points_at(rec, 28, rec.u32(24), small); };
  const auto polypoly = [&](bool small) {
    std::vector<std::vector<Point>> out;
    const std::uint32_t n = rec.u32(24);
    const std::uint64_t total = rec.u32(28);
    const std::uint64_t counts_end = 32 + std::uint64_t{n} * 4;
    if (counts_end > rec.data.size()) return out;
    std::uint64_t at = counts_end;
    std::uint64_t used = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
      const std::uint32_t c = rec.u32(32 + std::size_t{i} * 4);
      if (used + c > total) break;
      out.push_back(points_at(rec, static_cast<std::size_t>(at), c, small));
      at += std::uint64_t{c} * (small ? 4 : 8);
      used += c;
      if (at > rec.data.size()) break;
    }
    return out;
  };
  switch (type) {
    // ---- state
    case 9: gdi_.set_window_ext({rec.d(8), rec.d(12)}); break;
    case 10: gdi_.set_window_org(rec.point(8)); break;
    case 11: gdi_.set_viewport_ext({rec.d(8), rec.d(12)}); break;
    case 12: gdi_.set_viewport_org(rec.point(8)); break;
    case 31: gdi_.scale_viewport_ext(rec.d(8), rec.d(12), rec.d(16), rec.d(20)); break;
    case 32: gdi_.scale_window_ext(rec.d(8), rec.d(12), rec.d(16), rec.d(20)); break;
    case 17: gdi_.set_map_mode(static_cast<int>(rec.u32(8))); break;
    case 18: dc.bk_mode = static_cast<int>(rec.u32(8)); break;
    case 19: dc.poly_fill = static_cast<int>(rec.u32(8)); break;
    case 20: dc.rop2 = static_cast<int>(rec.u32(8)); break;
    case 22: dc.text_align = rec.u32(8); break;
    case 24: dc.text_color = color_ref(rec.u32(8)); break;
    case 25: dc.bk_color = color_ref(rec.u32(8)); break;
    case 33: gdi_.save_dc(); break;
    case 34: gdi_.restore_dc(rec.i32(8)); break;
    case 35:
      gdi_.set_world(Matrix{rec.f32(8), rec.f32(12), rec.f32(16), rec.f32(20), rec.f32(24), rec.f32(28)});
      break;
    case 36:
      gdi_.modify_world(Matrix{rec.f32(8), rec.f32(12), rec.f32(16), rec.f32(20), rec.f32(24), rec.f32(28)},
                        rec.u32(32));
      break;
    case 57: dc.arc_clockwise = rec.u32(8) == 2; break;
    case 58: {
      const std::uint32_t raw = rec.u32(8);
      double limit = raw;
      if (raw > 1000) limit = std::bit_cast<float>(raw);  // some writers store a float
      if (std::isfinite(limit) && limit >= 1) dc.miter_limit = limit;
      break;
    }
    case 13: case 16: case 21: case 23: case 28: case 98: case 99: case 100: case 101: case 104: case 115:
    case 119: case 120: case 121: case 122: case 111: case 112: case 113:
      break;  // brush origin, mapper flags, stretch mode, colour adjustment, meta region, ICM, layout ...

    // ---- objects
    case 37: gdi_.select_object(rec.u32(8)); break;
    case 40: gdi_.delete_object(rec.u32(8)); break;
    case 48: gdi_.select_object(rec.u32(8)); break;
    case 38: {
      GdiPen pen;
      pen.style = rec.u32(12);
      pen.width = rec.d(16);
      pen.color = color_ref(rec.u32(24));
      gdi_.create_object_at(rec.u32(8), std::move(pen));
      break;
    }
    case 95: {
      GdiPen pen;
      const std::uint32_t style = rec.u32(28);
      pen.style = style & 0xFFFF;
      pen.extended = true;
      pen.cosmetic = (style & 0xF0000) == 0;
      pen.width = pen.cosmetic ? 0.0 : static_cast<double>(rec.u32(32));
      const std::uint32_t brush_style = rec.u32(36);
      pen.color = color_ref(rec.u32(40));
      if (brush_style == 1) pen.style = (pen.style & ~0x0Fu) | 5;
      const std::uint32_t entries = std::min<std::uint32_t>(rec.u32(48), 32);
      for (std::uint32_t i = 0; i < entries && rec.has(52 + std::size_t{i} * 4 + 4); ++i) {
        pen.user_dash.push_back(rec.u32(52 + std::size_t{i} * 4));
      }
      if (pen.cosmetic) {
        // Cosmetic user styles count in device pixels; geometric ones in logical units.
      } else if ((pen.style & 0x0F) == 7 && pen.width > 0) {
        for (double& d : pen.user_dash) d /= pen.width;
      }
      gdi_.create_object_at(rec.u32(8), std::move(pen));
      break;
    }
    case 39: {
      GdiBrush brush;
      brush.style = rec.u32(12);
      brush.color = color_ref(rec.u32(16));
      brush.hatch = rec.u32(20);
      if (brush.style != 0 && brush.style != 1 && brush.style != 2) brush.style = 0;
      gdi_.create_object_at(rec.u32(8), std::move(brush));
      break;
    }
    case 93: case 94: {
      GdiBrush brush;
      CVT_TRY_ASSIGN(FramePtr frame, bitmap(rec, 16, 24, rec.u32(12)));
      brush.style = frame ? 5 : 0;
      brush.color = {0.5f, 0.5f, 0.5f, 1};
      brush.pattern = std::move(frame);
      gdi_.create_object_at(rec.u32(8), std::move(brush));
      break;
    }
    case 82: {
      GdiFont f;
      f.height = rec.d(12);
      f.width = rec.d(16);
      f.escapement = rec.d(20);
      f.weight = rec.i32(28);
      f.italic = rec.u8(32) != 0;
      f.underline = rec.u8(33) != 0;
      f.strikeout = rec.u8(34) != 0;
      f.charset = rec.u8(35);
      std::u16string face;
      for (std::size_t i = 0; i < 32; ++i) {
        const std::uint16_t c = rec.u16(40 + i * 2);
        if (c == 0) break;
        face.push_back(static_cast<char16_t>(c));
      }
      f.face = util::utf16_to_utf8(face);
      gdi_.create_object_at(rec.u32(8), std::move(f));
      break;
    }
    case 49: {
      GdiPalette pal;
      const std::uint16_t n = rec.u16(14);
      for (std::uint16_t i = 0; i < n && rec.has(16 + std::size_t{i} * 4 + 4); ++i) {
        const std::size_t at = 16 + std::size_t{i} * 4;
        pal.colors.push_back(Color::rgb8(rec.u8(at), rec.u8(at + 1), rec.u8(at + 2)));
      }
      gdi_.create_object_at(rec.u32(8), std::move(pal));
      break;
    }
    case 50: case 51: case 52: break;  // palette entries, resize, realize

    // ---- drawing
    case 27: return gdi_.move_to(rec.point(8));
    case 54: return gdi_.line_to(rec.point(8));
    case 2: return gdi_.polybezier(poly(false));
    case 85: return gdi_.polybezier(poly(true));
    case 3: return gdi_.polygon(poly(false));
    case 86: return gdi_.polygon(poly(true));
    case 4: return gdi_.polyline(poly(false));
    case 87: return gdi_.polyline(poly(true));
    case 5: return gdi_.polybezier_to(poly(false));
    case 88: return gdi_.polybezier_to(poly(true));
    case 6: return gdi_.polyline_to(poly(false));
    case 89: return gdi_.polyline_to(poly(true));
    case 7: return gdi_.poly_polyline(polypoly(false));
    case 90: return gdi_.poly_polyline(polypoly(true));
    case 8: return gdi_.poly_polygon(polypoly(false));
    case 91: return gdi_.poly_polygon(polypoly(true));
    case 56: case 92: {
      const bool small = type == 92;
      const std::uint32_t n = rec.u32(24);
      const auto pts = points_at(rec, 28, n, small);
      const std::size_t types_at = 28 + pts.size() * (small ? 4 : 8);
      const ByteView types = rec.slice(types_at, pts.size());
      return gdi_.poly_draw(pts, as_u8(types));
    }
    case 43: return gdi_.rectangle(rec.rect(8));
    case 42: return gdi_.ellipse(rec.rect(8));
    case 44: return gdi_.round_rect(rec.rect(8), rec.d(24), rec.d(28));
    case 45: return gdi_.arc(rec.rect(8), rec.point(24), rec.point(32), GdiPlayer::ArcKind::Arc);
    case 55: return gdi_.arc(rec.rect(8), rec.point(24), rec.point(32), GdiPlayer::ArcKind::ArcTo);
    case 46: return gdi_.arc(rec.rect(8), rec.point(24), rec.point(32), GdiPlayer::ArcKind::Chord);
    case 47: return gdi_.arc(rec.rect(8), rec.point(24), rec.point(32), GdiPlayer::ArcKind::Pie);
    case 41: return gdi_.angle_arc(rec.point(8), rec.u32(16), rec.f32(20), rec.f32(24));
    case 15: return gdi_.set_pixel(rec.point(8), color_ref(rec.u32(16)));
    case 53: gdi_.note("Flood fills are not supported and were left out."); break;

    // ---- paths
    case 59: gdi_.begin_path(); break;
    case 60: gdi_.end_path(); break;
    case 61: gdi_.close_figure(); break;
    case 62: return gdi_.fill_path();
    case 63: return gdi_.stroke_and_fill_path();
    case 64: return gdi_.stroke_path();
    case 65: break;  // FLATTENPATH: curves are kept
    case 66: gdi_.note("WidenPath was approximated (the path is used as is)."); break;
    case 67: gdi_.select_clip_path(static_cast<int>(rec.u32(8))); break;
    case 68: gdi_.abort_path(); break;

    // ---- clipping and regions
    case 29: gdi_.exclude_clip_rect(rec.rect(8)); break;
    case 30: gdi_.intersect_clip_rect(rec.rect(8)); break;
    case 26: gdi_.offset_clip(rec.point(8)); break;
    case 75: {
      const std::uint32_t size = rec.u32(8);
      const auto mode = static_cast<int>(rec.u32(12));
      const ByteView rgn = rec.slice(16, size);
      gdi_.select_clip_rects(size == 0 ? std::vector<Rect>{} : region_rects(rgn), true, mode);
      break;
    }
    case 71: case 72: case 74: {
      const std::uint32_t size = rec.u32(24);
      GdiBrush brush = dc.brush;
      std::size_t data_at = 28;
      if (type != 74) {
        const GdiObject* obj = gdi_.object(rec.u32(28));
        const std::uint32_t ih = rec.u32(28);
        if ((ih & 0x80000000u) == 0) {
          const auto* b = obj != nullptr ? std::get_if<GdiBrush>(obj) : nullptr;
          if (b == nullptr) break;
          brush = *b;
        } else {
          const GdiBrush saved = dc.brush;
          gdi_.select_object(ih);
          brush = dc.brush;
          dc.brush = saved;
        }
        data_at = type == 71 ? 32 : 40;
      }
      const auto rects = region_rects(rec.slice(data_at, size));
      if (type == 72) return gdi_.frame_rects(rects, true, brush, rec.d(32), rec.d(36));
      return gdi_.fill_rects(rects, true, brush);
    }
    case 73: gdi_.note("Inverting raster operations were left out."); break;

    // ---- text
    case 84: return text(rec, 36, true, 1.0);
    case 83: return text(rec, 36, false, 1.0);
    case 96: case 97: {
      const std::uint32_t n = rec.u32(36);
      std::size_t at = 40;
      for (std::uint32_t i = 0; i < n && rec.has(at + 40); ++i) {
        CVT_TRY(text(rec, at, type == 97, 1.0));
        at += (rec.u32(at + 16) & 0x100) != 0 ? 24 : 40;
      }
      break;
    }
    case 108: {  // SMALLTEXTOUT
      GdiPlayer::TextRun run;
      run.reference = rec.point(8);
      const std::uint32_t count = rec.u32(16);
      run.options = rec.u32(20);
      std::size_t at = 36;
      if ((run.options & 0x100) == 0) {
        run.rect = rec.rect(36);
        at += 16;
      }
      const bool small_chars = (run.options & 0x200) != 0;
      const ByteView chars = rec.slice(at, std::uint64_t{count} * (small_chars ? 1 : 2));
      if (small_chars) {
        run.chars = util::utf8_to_utf32(charset_to_utf8(chars, dc.font.charset));
      } else {
        std::u16string u16;
        util::ByteReader r(chars);
        for (std::uint32_t i = 0; i < count && r.remaining() >= 2; ++i) u16.push_back(static_cast<char16_t>(r.u16le()));
        run.chars = util::utf8_to_utf32(util::utf16_to_utf8(u16));
      }
      return gdi_.text_out(run);
    }

    // ---- bitmaps
    case 76: case 77: {  // BITBLT, STRETCHBLT
      const double xd = rec.d(24), yd = rec.d(28), wd = rec.d(32), hd = rec.d(36);
      const std::uint32_t rop = rec.u32(40);
      if (rec.u32(88) == 0 || !rec.has(100)) return gdi_.pat_blt(Rect{xd, yd, wd, hd}, rop);
      CVT_TRY_ASSIGN(FramePtr frame, bitmap(rec, 84, 92, rec.u32(80)));
      double ws = wd, hs = hd;
      if (type == 77) {
        ws = rec.d(100);
        hs = rec.d(104);
      }
      const ByteView info = rec.slice(rec.u32(84), rec.u32(88));
      return gdi_.draw_bitmap(crop(frame, info, rec.d(44), rec.d(48), ws, hs, false), Rect{xd, yd, wd, hd}, rop);
    }
    case 81: {  // STRETCHDIBITS
      const double xd = rec.d(24), yd = rec.d(28);
      const double xs = rec.d(32), ys = rec.d(36), ws = rec.d(40), hs = rec.d(44);
      const std::uint32_t rop = rec.u32(68);
      const double wd = rec.d(72), hd = rec.d(76);
      if (GdiPlayer::classify_rop(rop) == RopEffect::Pattern || rec.u32(52) == 0) {
        return gdi_.pat_blt(Rect{xd, yd, wd, hd}, rop);
      }
      CVT_TRY_ASSIGN(FramePtr frame, bitmap(rec, 48, 56, rec.u32(64)));
      const ByteView info = rec.slice(rec.u32(48), rec.u32(52));
      return gdi_.draw_bitmap(crop(frame, info, xs, ys, ws, hs, true), Rect{xd, yd, wd, hd}, rop);
    }
    case 80: {  // SETDIBITSTODEVICE
      const double xd = rec.d(24), yd = rec.d(28);
      const double xs = rec.d(32), ys = rec.d(36), ws = rec.d(40), hs = rec.d(44);
      CVT_TRY_ASSIGN(FramePtr frame, bitmap(rec, 48, 56, rec.u32(64)));
      const ByteView info = rec.slice(rec.u32(48), rec.u32(52));
      return gdi_.draw_bitmap(crop(frame, info, xs, ys, ws, hs, true), Rect{xd, yd, ws, hs}, 0x00CC0020);
    }
    case 114: case 116: {  // ALPHABLEND, TRANSPARENTBLT
      const double xd = rec.d(24), yd = rec.d(28), wd = rec.d(32), hd = rec.d(36);
      const std::uint32_t blend = rec.u32(40);
      const bool alpha = type == 114 && ((blend >> 24) & 1) != 0;
      CVT_TRY_ASSIGN(FramePtr frame, bitmap(rec, 84, 92, rec.u32(80), alpha));
      if (!frame) break;
      const ByteView info = rec.slice(rec.u32(84), rec.u32(88));
      frame = crop(frame, info, rec.d(44), rec.d(48), rec.d(100), rec.d(104), false);
      if (!frame) break;
      auto copy = std::make_shared<ir::Frame>(*frame);
      if (type == 114) {
        const std::uint32_t constant = (blend >> 16) & 0xFF;
        if (constant != 255) {
          for (std::size_t i = 3; i < copy->rgba.size(); i += 4) {
            copy->rgba[i] = static_cast<std::uint8_t>((copy->rgba[i] * constant + 127) / 255);
          }
        }
      } else {
        const std::uint32_t key = blend;
        for (std::size_t i = 0; i < copy->rgba.size(); i += 4) {
          if (copy->rgba[i] == (key & 0xFF) && copy->rgba[i + 1] == ((key >> 8) & 0xFF) &&
              copy->rgba[i + 2] == ((key >> 16) & 0xFF)) {
            copy->rgba[i + 3] = 0;
          }
        }
      }
      return gdi_.draw_bitmap(FramePtr(std::move(copy)), Rect{xd, yd, wd, hd}, 0x00CC0020);
    }
    case 78: {  // MASKBLT
      const double xd = rec.d(24), yd = rec.d(28), wd = rec.d(32), hd = rec.d(36);
      const std::uint32_t rop = rec.u32(40);
      if (rec.u32(88) == 0) return gdi_.pat_blt(Rect{xd, yd, wd, hd}, rop & 0x00FFFFFF);
      CVT_TRY_ASSIGN(FramePtr frame, bitmap(rec, 84, 92, rec.u32(80)));
      const ByteView info = rec.slice(rec.u32(84), rec.u32(88));
      frame = crop(frame, info, rec.d(44), rec.d(48), wd, hd, false);
      if (rec.u32(116) != 0) {
        CVT_TRY_ASSIGN(frame, apply_mask(frame, rec, 112, 120, rec.d(100), rec.d(104)));
      }
      return gdi_.draw_bitmap(frame, Rect{xd, yd, wd, hd}, (rop & 0x00FFFFFF) | 0x00000000u);
    }
    case 79: {  // PLGBLT
      const Point p0 = rec.point(24), p1 = rec.point(32), p2 = rec.point(40);
      CVT_TRY_ASSIGN(FramePtr frame, bitmap(rec, 96, 104, rec.u32(92)));
      const ByteView info = rec.slice(rec.u32(96), rec.u32(100));
      frame = crop(frame, info, rec.d(48), rec.d(52), rec.d(56), rec.d(60), false);
      if (rec.u32(128) != 0) {
        CVT_TRY_ASSIGN(frame, apply_mask(frame, rec, 124, 132, rec.d(112), rec.d(116)));
      }
      return gdi_.draw_bitmap(frame, p0, p1, p2, 0x00CC0020);
    }
    case 118: {  // GRADIENTFILL
      const std::uint32_t nv = rec.u32(24), nt = rec.u32(28), mode = rec.u32(32);
      if (std::uint64_t{nv} * 16 + 36 > rec.data.size()) break;
      std::vector<Point> pts;
      std::vector<Color> cols;
      for (std::uint32_t i = 0; i < nv; ++i) {
        const std::size_t at = 36 + std::size_t{i} * 16;
        pts.push_back(rec.point(at));
        cols.push_back(Color{static_cast<float>(rec.u16(at + 8)) / 65280.0f, static_cast<float>(rec.u16(at + 10)) / 65280.0f,
                             static_cast<float>(rec.u16(at + 12)) / 65280.0f, 1.0f});
        Color& c = cols.back();
        c.r = std::min(c.r, 1.0f);
        c.g = std::min(c.g, 1.0f);
        c.b = std::min(c.b, 1.0f);
      }
      const std::size_t idx_at = 36 + std::size_t{nv} * 16;
      const std::size_t each = mode == 2 ? 12 : 8;
      for (std::uint32_t i = 0; i < nt && rec.has(idx_at + (std::size_t{i} + 1) * each); ++i) {
        const std::size_t at = idx_at + std::size_t{i} * each;
        if (mode == 2) {
          const std::uint32_t a = rec.u32(at), b = rec.u32(at + 4), c = rec.u32(at + 8);
          if (a >= nv || b >= nv || c >= nv) continue;
          const Point tri[3] = {pts[a], pts[b], pts[c]};
          const Color tc[3] = {cols[a], cols[b], cols[c]};
          CVT_TRY(gdi_.gradient_triangle(tri, tc));
        } else {
          const std::uint32_t a = rec.u32(at), b = rec.u32(at + 4);
          if (a >= nv || b >= nv) continue;
          CVT_TRY(gdi_.gradient_rect(Rect{pts[a].x, pts[a].y, pts[b].x, pts[b].y}, cols[a], cols[b], mode == 1));
        }
      }
      break;
    }

    // ---- comments (EMF+)
    case 70: {
      const std::uint32_t size = rec.u32(8);
      const ByteView body = rec.slice(12, size);
      if (body.size() >= 4 && Rec{body}.u32(0) == emfplus_comment && plus_) return plus_->play(body.subspan(4));
      break;
    }
    case 1: break;  // a second header
    default:
      gdi_.note("Unknown EMF records were skipped.");
      break;
  }
  return {};
}

}  // namespace

bool looks_like_emf(ByteView data) noexcept {
  if (data.size() < 88) return false;
  const Rec r{data};
  return r.u32(0) == 1 && r.u32(40) == emf_signature && r.u32(4) >= 88;
}

Result<Scene> read_emf(ByteView data, const Context& ctx, const ReadOptions& options, int depth) {
  EmfReader reader(ctx, data, options, depth);
  return reader.run();
}

}  // namespace cvt::vector::metafile
