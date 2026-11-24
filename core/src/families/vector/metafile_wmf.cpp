// Convertible core - vector family: Windows Metafile (WMF) reader, after [MS-WMF].
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "families/vector/metafile_internal.hpp"
#include "util/text.hpp"

namespace cvt::vector::metafile {

namespace {

constexpr std::uint32_t placeable_key = 0x9AC6CDD7u;

/// Sequential reader of record parameters.
struct Params {
  util::ByteReader r;
  explicit Params(ByteView body) : r(body) {}
  double s() { return static_cast<double>(r.i16le()); }
  std::uint16_t u() { return r.u16le(); }
  std::uint32_t u32() { return r.u32le(); }
  [[nodiscard]] bool ok() const { return r.ok(); }
  [[nodiscard]] std::size_t remaining() const { return r.remaining(); }
};

std::vector<Point> read_points(Params& p, std::size_t count) {
  std::vector<Point> out;
  count = std::min(count, p.remaining() / 4);
  out.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const double x = p.s();
    const double y = p.s();
    out.push_back({x, y});
  }
  return out;
}

GdiFont read_font(Params& p) {
  GdiFont f;
  f.height = p.s();
  f.width = p.s();
  f.escapement = p.s();
  (void)p.s();  // orientation
  f.weight = static_cast<int>(p.s());
  f.italic = p.r.u8() != 0;
  f.underline = p.r.u8() != 0;
  f.strikeout = p.r.u8() != 0;
  f.charset = p.r.u8();
  p.r.skip(4);  // precision, clip precision, quality, pitch and family
  const ByteView rest = p.r.peek_rest();
  const ByteView name = rest.first(std::min<std::size_t>(32, rest.size()));
  std::size_t len = 0;
  while (len < name.size() && name[len] != std::byte{0}) ++len;
  f.face = charset_to_utf8(name.first(len), 0);
  return f;
}

std::vector<Rect> read_region(Params& p) {
  std::vector<Rect> rects;
  p.r.skip(10);  // next in chain, object type, object count, region size
  const std::uint16_t scans = p.u();
  p.r.skip(2);  // max scan
  const double l = p.s(), t = p.s(), rr = p.s(), b = p.s();
  for (std::uint16_t i = 0; i < scans && p.ok(); ++i) {
    const std::uint16_t count = p.u();
    const double top = p.s(), bottom = p.s();
    for (std::uint16_t k = 0; k + 1 < count && p.ok(); k += 2) {
      const double left = p.s(), right = p.s();
      if (p.ok() && right > left && bottom > top) rects.push_back({left, top, right, bottom});
    }
    (void)p.u();  // count2
  }
  if (rects.empty() && rr > l && b > t) rects.push_back({l, t, rr, b});
  return rects;
}

class WmfReader {
 public:
  WmfReader(const Context& ctx, ByteView data) : ctx_(ctx), data_(data), gdi_(Flavor::Wmf, ctx) {}

  Result<Scene> run() {
    util::ByteReader r(data_);
    bool placeable = false;
    double page_w = 0, page_h = 0;
    if (r.u32le() == placeable_key) {
      placeable = true;
      r.skip(2);
      const double l = r.i16le(), t = r.i16le(), rr = r.i16le(), b = r.i16le();
      double inch = r.u16le();
      r.skip(6);
      if (!r.ok()) return fail(Status::Decode, "truncated WMF header");
      if (inch == 0) inch = 1440;
      const Rect bbox{std::min(l, rr), std::min(t, b), std::max(l, rr), std::max(t, b)};
      if (bbox.width() > 0 && bbox.height() > 0) {
        page_w = bbox.width() / inch * px_per_in;
        page_h = bbox.height() / inch * px_per_in;
        gdi_.set_wmf_frame(bbox, page_w, page_h);
      } else {
        placeable = false;
      }
    } else {
      r.seek(0);
    }
    const std::uint16_t type = r.u16le();
    const std::uint16_t header_words = r.u16le();
    const std::uint16_t version = r.u16le();
    r.skip(4);
    const std::uint16_t objects = r.u16le();
    r.skip(6);
    if (!r.ok()) return fail(Status::Decode, "truncated WMF header");
    if ((type != 1 && type != 2) || header_words != 9 || (version != 0x0100 && version != 0x0300)) {
      return fail(Status::Decode, "not a Windows metafile (bad header)");
    }
    gdi_.ensure_table(objects);

    std::size_t records = 0;
    bool ended = false;
    while (r.remaining() >= 6) {
      const std::size_t start = r.position();
      const std::uint32_t words = r.u32le();
      const std::uint16_t function = r.u16le();
      if (function == 0) {
        ended = true;
        break;
      }
      if (words < 3 || std::uint64_t{words} * 2 > data_.size() - start) {
        return fail(Status::Decode, "WMF record {} is truncated or malformed", records);
      }
      const ByteView body = data_.subspan(start + 6, std::size_t{words} * 2 - 6);
      CVT_TRY(play(function, body, words));
      r.seek(start + std::size_t{words} * 2);
      if ((++records & 1023) == 0) {
        CVT_TRY(ctx_.checkpoint(static_cast<double>(start) / static_cast<double>(data_.size())));
      }
    }
    if (!ended && records == 0) return fail(Status::Decode, "the WMF file has no records");
    if (!ended) gdi_.note("The WMF file has no end record; it may be truncated.");

    CVT_TRY_ASSIGN(Group root, gdi_.finish());
    Scene scene;
    scene.source_units = placeable ? "in" : "px";
    Page page;
    if (gdi_.has_wmf_frame()) {
      const auto [w, h] = gdi_.wmf_page_size();
      page.width = w;
      page.height = h;
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
  Color color(std::uint32_t v) {
    const DcState& dc = gdi_.dc();
    if (((v >> 24) & 0xFF) == 0x01 && dc.palette) {
      const std::size_t index = v & 0xFFFF;
      if (index < dc.palette->colors.size()) return dc.palette->colors[index];
    }
    return Color::rgb8(static_cast<int>(v & 0xFF), static_cast<int>((v >> 8) & 0xFF), static_cast<int>((v >> 16) & 0xFF));
  }

  Result<FramePtr> dib(ByteView packed, bool palette_indices) {
    DibRequest req;
    req.info = packed;
    req.palette_indices = palette_indices;
    req.palette = gdi_.dc().palette ? &gdi_.dc().palette->colors : nullptr;
    auto frame = decode_dib(req, ctx_, gdi_.pixels());
    if (!frame) {
      if (frame.error().code == Status::Limit || frame.error().code == Status::Cancelled) return frame;
      gdi_.note("Some bitmaps could not be decoded and were left out.");
      return FramePtr{};
    }
    return frame;
  }

  /// The source rectangle of a DIB (x, y measured from its first stored row).
  static FramePtr crop_dib(const FramePtr& frame, ByteView info, double xs, double ys, double ws, double hs) {
    if (!frame) return frame;
    double top = ys;
    if (dib_bottom_up(info)) top = frame->height - ys - std::abs(hs);
    return crop_frame(frame, static_cast<std::int64_t>(xs), static_cast<std::int64_t>(top),
                      static_cast<std::int64_t>(std::abs(ws)), static_cast<std::int64_t>(std::abs(hs)));
  }

  Result<void> text(Params& p, bool extended) {
    double x = 0, y = 0;
    std::uint16_t length = 0, options = 0;
    std::optional<Rect> rect;
    if (extended) {
      y = p.s();
      x = p.s();
      length = p.u();
      options = p.u();
      if ((options & 0x0006) != 0) {
        const double l = p.s(), t = p.s(), rr = p.s(), b = p.s();
        rect = Rect{l, t, rr, b};
      }
    } else {
      length = p.u();
    }
    const ByteView rest = p.r.peek_rest();
    const ByteView bytes = rest.first(std::min<std::size_t>(length, rest.size()));
    p.r.skip(std::min<std::size_t>((static_cast<std::size_t>(length) + 1) & ~std::size_t{1}, p.remaining()));
    GdiPlayer::TextRun run;
    if (!extended) {
      y = p.s();
      x = p.s();
    } else {
      for (std::size_t i = 0; i < bytes.size() && p.remaining() >= 2; ++i) run.dx.push_back(p.s());
    }
    if (!p.ok() && !extended) return {};
    const int charset = gdi_.dc().font.charset;
    if (!charset_supported(charset)) {
      gdi_.note("Text in East Asian double-byte character sets is not decoded.");
      run.dx.clear();
    }
    run.chars = util::utf8_to_utf32(charset_to_utf8(bytes, charset));
    if (run.dx.size() != run.chars.size()) run.dx.clear();
    run.reference = {x, y};
    run.options = options;
    run.rect = rect;
    return gdi_.text_out(run);
  }

  Result<void> play(std::uint16_t function, ByteView body, std::uint32_t words);

  const Context& ctx_;
  ByteView data_;
  GdiPlayer gdi_;
};

Result<void> WmfReader::play(std::uint16_t function, ByteView body, std::uint32_t words) {
  Params p(body);
  DcState& dc = gdi_.dc();
  const auto rect_ltrb = [&p]() {
    const double b = p.s(), r = p.s(), t = p.s(), l = p.s();
    return Rect{l, t, r, b};
  };
  const bool no_bitmap = words == static_cast<std::uint32_t>((function >> 8) + 3);
  switch (function) {
    case 0x0102: dc.bk_mode = p.u(); break;                                      // SETBKMODE
    case 0x0103: gdi_.set_map_mode(p.u()); break;                                // SETMAPMODE
    case 0x0104: dc.rop2 = p.u(); break;                                         // SETROP2
    case 0x0106: dc.poly_fill = p.u(); break;                                    // SETPOLYFILLMODE
    case 0x0107: case 0x0105: case 0x0231: case 0x014C: case 0x020A: break;      // stretch mode, relabs, mapper, layout, justification
    case 0x0108: dc.char_extra = p.s(); break;                                   // SETTEXTCHAREXTRA
    case 0x0201: dc.bk_color = color(p.u32()); break;                            // SETBKCOLOR
    case 0x0209: dc.text_color = color(p.u32()); break;                          // SETTEXTCOLOR
    case 0x012E: dc.text_align = p.u(); break;                                   // SETTEXTALIGN
    case 0x020B: { const double y = p.s(), x = p.s(); gdi_.set_window_org({x, y}); break; }
    case 0x020C: { const double y = p.s(), x = p.s(); gdi_.set_window_ext({x, y}); break; }
    case 0x020F: { const double y = p.s(), x = p.s(); gdi_.offset_window_org({x, y}); break; }
    case 0x0410: {                                                               // SCALEWINDOWEXT
      const double yd = p.s(), yn = p.s(), xd = p.s(), xn = p.s();
      gdi_.scale_window_ext(xn, xd, yn, yd);
      break;
    }
    case 0x020D: case 0x020E: case 0x0211: case 0x0412:                          // viewport records
      gdi_.note("WMF viewport records were ignored; the window defines the page.");
      break;
    case 0x001E: gdi_.save_dc(); break;
    case 0x0127: gdi_.restore_dc(static_cast<std::int16_t>(p.u())); break;
    case 0x0213: { const double y = p.s(), x = p.s(); return gdi_.line_to({x, y}); }
    case 0x0214: { const double y = p.s(), x = p.s(); return gdi_.move_to({x, y}); }
    case 0x0325: { const auto pts = read_points(p, p.u()); return gdi_.polyline(pts); }
    case 0x0324: { const auto pts = read_points(p, p.u()); return gdi_.polygon(pts); }
    case 0x0538: {                                                               // POLYPOLYGON
      const std::uint16_t n = p.u();
      std::vector<std::uint16_t> counts;
      for (std::uint16_t i = 0; i < n && p.ok(); ++i) counts.push_back(p.u());
      std::vector<std::vector<Point>> polys;
      for (const std::uint16_t c : counts) polys.push_back(read_points(p, c));
      return gdi_.poly_polygon(polys);
    }
    case 0x041B: return gdi_.rectangle(rect_ltrb());
    case 0x0418: return gdi_.ellipse(rect_ltrb());
    case 0x061C: {
      const double h = p.s(), w = p.s();
      return gdi_.round_rect(rect_ltrb(), w, h);
    }
    case 0x0817: case 0x081A: case 0x0830: {                                     // ARC, PIE, CHORD
      const double ye = p.s(), xe = p.s(), ys = p.s(), xs = p.s();
      const Rect box = rect_ltrb();
      const auto kind = function == 0x0817   ? GdiPlayer::ArcKind::Arc
                        : function == 0x081A ? GdiPlayer::ArcKind::Pie
                                             : GdiPlayer::ArcKind::Chord;
      return gdi_.arc(box, {xs, ys}, {xe, ye}, kind);
    }
    case 0x041F: {                                                               // SETPIXEL
      const Color c = color(p.u32());
      const double y = p.s(), x = p.s();
      return gdi_.set_pixel({x, y}, c);
    }
    case 0x0521: return text(p, false);
    case 0x0A32: return text(p, true);
    case 0x02FA: {                                                               // CREATEPENINDIRECT
      GdiPen pen;
      pen.style = p.u();
      pen.width = p.s();
      (void)p.s();
      pen.color = color(p.u32());
      return gdi_.create_object(std::move(pen));
    }
    case 0x02FC: {                                                               // CREATEBRUSHINDIRECT
      GdiBrush brush;
      brush.style = p.u();
      brush.color = color(p.u32());
      brush.hatch = p.u();
      if (brush.style == 3 || brush.style >= 5) brush.style = 0;  // patterns need a bitmap; fall back to solid
      return gdi_.create_object(std::move(brush));
    }
    case 0x02FB: return gdi_.create_object(read_font(p));                        // CREATEFONTINDIRECT
    case 0x00F7: {                                                               // CREATEPALETTE
      GdiPalette pal;
      p.r.skip(2);
      const std::uint16_t n = p.u();
      for (std::uint16_t i = 0; i < n && p.remaining() >= 4; ++i) {
        const int r = p.r.u8(), g = p.r.u8(), b = p.r.u8();
        p.r.skip(1);
        pal.colors.push_back(Color::rgb8(r, g, b));
      }
      return gdi_.create_object(std::move(pal));
    }
    case 0x0037: {                                                               // SETPALENTRIES
      if (!dc.palette) break;
      const std::uint16_t start = p.u(), n = p.u();
      auto pal = std::make_shared<GdiPalette>(*dc.palette);
      for (std::uint32_t i = 0; i < n && p.remaining() >= 4; ++i) {
        const int r = p.r.u8(), g = p.r.u8(), b = p.r.u8();
        p.r.skip(1);
        if (start + i < pal->colors.size()) pal->colors[start + i] = Color::rgb8(r, g, b);
      }
      dc.palette = std::move(pal);
      break;
    }
    case 0x0035: case 0x0139: case 0x0436: break;                                // palette housekeeping
    case 0x0142: {                                                               // DIBCREATEPATTERNBRUSH
      const std::uint16_t style = p.u(), usage = p.u();
      CVT_TRY_ASSIGN(FramePtr frame, dib(p.r.peek_rest(), usage == 1));
      GdiBrush brush;
      brush.style = frame ? 5 : 0;
      brush.color = {0.5f, 0.5f, 0.5f, 1};
      brush.pattern = frame;
      (void)style;
      return gdi_.create_object(std::move(brush));
    }
    case 0x01F9: {                                                               // CREATEPATTERNBRUSH (Bitmap16)
      p.r.skip(2);
      const std::int16_t w = static_cast<std::int16_t>(p.u()), h = static_cast<std::int16_t>(p.u());
      const std::uint16_t width_bytes = p.u();
      p.r.skip(1);
      const std::uint8_t bits_pixel = p.r.u8();
      p.r.skip(4 + 18);
      GdiBrush brush;
      brush.color = {0.5f, 0.5f, 0.5f, 1};
      if (p.ok() && bits_pixel == 1 && w > 0 && h > 0 && w <= 256 && h <= 256 &&
          p.remaining() >= std::size_t{width_bytes} * static_cast<std::size_t>(h)) {
        auto frame = std::make_shared<ir::Frame>(ir::Frame::filled(static_cast<std::uint32_t>(w),
                                                                   static_cast<std::uint32_t>(h)));
        const ByteView bits = p.r.peek_rest();
        for (int y = 0; y < h; ++y) {
          for (int x = 0; x < w; ++x) {
            const auto byte = static_cast<std::uint8_t>(bits[static_cast<std::size_t>(y) * width_bytes + static_cast<std::size_t>(x / 8)]);
            const bool set = ((byte >> (7 - x % 8)) & 1) != 0;
            const Color c = set ? dc.bk_color : dc.text_color;
            const std::size_t at = frame->offset(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
            frame->rgba[at] = static_cast<std::uint8_t>(c.r * 255 + 0.5f);
            frame->rgba[at + 1] = static_cast<std::uint8_t>(c.g * 255 + 0.5f);
            frame->rgba[at + 2] = static_cast<std::uint8_t>(c.b * 255 + 0.5f);
            frame->rgba[at + 3] = 255;
          }
        }
        brush.style = 3;
        brush.pattern = std::move(frame);
      } else {
        gdi_.note("Pattern brushes with colour bitmaps were approximated by grey.");
      }
      return gdi_.create_object(std::move(brush));
    }
    case 0x06FF: {                                                               // CREATEREGION
      GdiRegion region;
      region.rects = read_region(p);
      return gdi_.create_object(std::move(region));
    }
    case 0x01F0: gdi_.delete_object(p.u()); break;
    case 0x012D: case 0x0234: gdi_.select_object(p.u()); break;                  // SELECTOBJECT, SELECTPALETTE
    case 0x012C: {                                                               // SELECTCLIPREGION
      const GdiObject* obj = gdi_.object(p.u());
      const auto* region = obj != nullptr ? std::get_if<GdiRegion>(obj) : nullptr;
      gdi_.select_clip_rects(region != nullptr ? region->rects : std::vector<Rect>{}, false, 5);
      break;
    }
    case 0x0416: gdi_.intersect_clip_rect(rect_ltrb()); break;
    case 0x0415: gdi_.exclude_clip_rect(rect_ltrb()); break;
    case 0x0220: { const double y = p.s(), x = p.s(); gdi_.offset_clip({x, y}); break; }
    case 0x0228: case 0x012B: case 0x0429: {                                     // FILLREGION, PAINTREGION, FRAMEREGION
      const GdiObject* obj = gdi_.object(p.u());
      const auto* region = obj != nullptr ? std::get_if<GdiRegion>(obj) : nullptr;
      if (region == nullptr) break;
      GdiBrush brush = dc.brush;
      if (function != 0x012B) {
        const GdiObject* bobj = gdi_.object(p.u());
        const auto* b = bobj != nullptr ? std::get_if<GdiBrush>(bobj) : nullptr;
        if (b == nullptr) break;
        brush = *b;
      }
      if (function == 0x0429) {
        const double h = p.s(), w = p.s();
        return gdi_.frame_rects(region->rects, false, brush, w, h);
      }
      return gdi_.fill_rects(region->rects, false, brush);
    }
    case 0x012A: gdi_.note("Inverting raster operations were left out."); break;  // INVERTREGION
    case 0x061D: {                                                               // PATBLT
      const std::uint32_t rop = p.u32();
      const double h = p.s(), w = p.s(), y = p.s(), x = p.s();
      return gdi_.pat_blt(Rect{x, y, w, h}, rop);
    }
    case 0x0940: case 0x0922: {                                                  // DIBBITBLT, BITBLT
      const std::uint32_t rop = p.u32();
      if (no_bitmap) {
        p.r.skip(2);
        p.r.skip(4);  // source origin
        const double h = p.s(), w = p.s(), y = p.s(), x = p.s();
        return gdi_.pat_blt(Rect{x, y, w, h}, rop);
      }
      const double ys = p.s(), xs = p.s(), h = p.s(), w = p.s(), yd = p.s(), xd = p.s();
      if (function == 0x0922) {
        gdi_.note("Device-dependent bitmaps (WMF BitBlt) are not supported and were left out.");
        break;
      }
      const ByteView info = p.r.peek_rest();
      CVT_TRY_ASSIGN(FramePtr frame, dib(info, false));
      return gdi_.draw_bitmap(crop_dib(frame, info, xs, ys, w, h), Rect{xd, yd, w, h}, rop);
    }
    case 0x0B41: case 0x0B23: {                                                  // DIBSTRETCHBLT, STRETCHBLT
      const std::uint32_t rop = p.u32();
      if (no_bitmap) {
        p.r.skip(2);
        p.r.skip(8);  // source rectangle
        const double h = p.s(), w = p.s(), y = p.s(), x = p.s();
        return gdi_.pat_blt(Rect{x, y, w, h}, rop);
      }
      const double hs = p.s(), ws = p.s(), ys = p.s(), xs = p.s();
      const double h = p.s(), w = p.s(), yd = p.s(), xd = p.s();
      if (function == 0x0B23) {
        gdi_.note("Device-dependent bitmaps (WMF StretchBlt) are not supported and were left out.");
        break;
      }
      const ByteView info = p.r.peek_rest();
      CVT_TRY_ASSIGN(FramePtr frame, dib(info, false));
      return gdi_.draw_bitmap(crop_dib(frame, info, xs, ys, ws, hs), Rect{xd, yd, w, h}, rop);
    }
    case 0x0F43: {                                                               // STRETCHDIB
      const std::uint32_t rop = p.u32();
      const std::uint16_t usage = p.u();
      const double hs = p.s(), ws = p.s(), ys = p.s(), xs = p.s();
      const double h = p.s(), w = p.s(), yd = p.s(), xd = p.s();
      if (!p.ok()) break;
      if (GdiPlayer::classify_rop(rop) == RopEffect::Pattern) return gdi_.pat_blt(Rect{xd, yd, w, h}, rop);
      const ByteView info = p.r.peek_rest();
      CVT_TRY_ASSIGN(FramePtr frame, dib(info, usage == 1));
      return gdi_.draw_bitmap(crop_dib(frame, info, xs, ys, ws, hs), Rect{xd, yd, w, h}, rop);
    }
    case 0x0D33: {                                                               // SETDIBTODEV
      const std::uint16_t usage = p.u();
      p.r.skip(4);  // scan count, start scan
      const double ys = p.s(), xs = p.s(), h = p.s(), w = p.s(), yd = p.s(), xd = p.s();
      if (!p.ok()) break;
      const ByteView info = p.r.peek_rest();
      CVT_TRY_ASSIGN(FramePtr frame, dib(info, usage == 1));
      return gdi_.draw_bitmap(crop_dib(frame, info, xs, ys, w, h), Rect{xd, yd, w, h}, 0x00CC0020);
    }
    case 0x0626: break;                                                          // ESCAPE (comments, embedded EMF)
    case 0x0419: case 0x0548:
      gdi_.note("Flood fills are not supported and were left out.");
      break;
    default:
      gdi_.note("Unknown WMF records were skipped.");
      break;
  }
  return {};
}

}  // namespace

bool looks_like_wmf(ByteView data) noexcept {
  util::ByteReader r(data);
  if (r.u32le() == placeable_key) return data.size() >= 40;
  r.seek(0);
  const std::uint16_t type = r.u16le(), header = r.u16le(), version = r.u16le();
  return r.ok() && (type == 1 || type == 2) && header == 9 && (version == 0x0100 || version == 0x0300);
}

Result<Scene> read_wmf(ByteView data, const Context& ctx, const ReadOptions& /*options*/, int /*depth*/) {
  WmfReader reader(ctx, data);
  return reader.run();
}

Result<Scene> read_any_metafile(ByteView data, const Context& ctx, const ReadOptions& options, int depth) {
  if (depth > max_metafile_nesting) return fail(Status::Limit, "metafiles are nested too deeply");
  if (looks_like_emf(data)) return read_emf(data, ctx, options, depth);
  if (looks_like_wmf(data)) return read_wmf(data, ctx, options, depth);
  return fail(Status::Decode, "the embedded metafile is not a WMF or EMF file");
}

}  // namespace cvt::vector::metafile
