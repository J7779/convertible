// Convertible core - vector family: Windows metafiles (WMF, EMF, EMF+), internal declarations.
//
// The readers decode records ([MS-WMF], [MS-EMF], [MS-EMFPLUS]) and drive
// one GDI state machine (`GdiPlayer`) that turns drawing calls into scene
// nodes in page space (CSS px, y down). EMF+ records are played by
// `EmfPlusPlayer`, which shares the player's output and device mapping.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "families/vector/scene.hpp"
#include "ir/image.hpp"
#include "util/bytes.hpp"

namespace cvt::vector::metafile {

// --------------------------------------------------------------------------- text

/// 8-bit text in a GDI font character set (ANSI_CHARSET, RUSSIAN_CHARSET,
/// SYMBOL_CHARSET ...) as UTF-8. Double-byte East Asian sets are not mapped
/// (their non-ASCII characters become U+FFFD).
[[nodiscard]] std::string charset_to_utf8(ByteView text, int charset);
[[nodiscard]] bool charset_supported(int charset);
/// The Unicode character of a byte in the Adobe Symbol encoding.
[[nodiscard]] char32_t symbol_to_unicode(std::uint8_t byte);

// --------------------------------------------------------------------------- bitmaps

using FramePtr = std::shared_ptr<const ir::Frame>;

/// Running total of decoded bitmap pixels, checked against ctx.limits.
struct PixelBudget {
  std::uint64_t used = 0;
};

struct DibRequest {
  /// BITMAPINFO (header + masks + colour table).
  ByteView info;
  /// The pixel bits; empty when `info` is a packed DIB (bits follow the table).
  ByteView bits;
  /// DIB_PAL_COLORS: the colour table holds 16-bit indices into `palette`.
  bool palette_indices = false;
  const std::vector<Color>* palette = nullptr;
  /// Keep the alpha byte of 32-bit pixels (AlphaBlend); otherwise opaque.
  bool keep_alpha = false;
  /// Alpha of 32-bit BI_RGB pixels is premultiplied (AlphaBlend AC_SRC_ALPHA).
  bool premultiplied = false;
};

/// Decode a device-independent bitmap (core, info, V4 and V5 headers;
/// 1/2/4/8/16/24/32 bits; BI_RGB, BI_RLE4, BI_RLE8, BI_BITFIELDS; BI_PNG and
/// BI_JPEG through the image family). Rows top to bottom.
[[nodiscard]] Result<FramePtr> decode_dib(const DibRequest& request, const Context& ctx, PixelBudget& budget);
/// Width and height a DIB header declares (height positive), or nullopt.
[[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>> dib_size(ByteView info);
/// True when the DIB's rows are stored bottom-up (the usual case).
[[nodiscard]] bool dib_bottom_up(ByteView info);

/// Pixels of `frame` inside the source rectangle (clamped); nullptr when empty.
[[nodiscard]] FramePtr crop_frame(const FramePtr& frame, std::int64_t x, std::int64_t y, std::int64_t w,
                                  std::int64_t h);

// --------------------------------------------------------------------------- clipping

using ClipRef = std::shared_ptr<const ClipPath>;

/// A clip region as GDI and GDI+ combine them. `infinite` (no clip) is the
/// identity for intersections.
struct Region {
  bool infinite = true;
  ClipRef clip;  ///< when !infinite; nullptr = empty region
};

/// A rectangle far larger than any page (for complements).
[[nodiscard]] Path huge_rect_path();
[[nodiscard]] ClipRef make_clip(Path path, FillRule rule, ClipRef parent = nullptr);
/// A clip that hides everything.
[[nodiscard]] ClipRef empty_clip();
/// current AND path.
[[nodiscard]] ClipRef clip_intersect(const ClipRef& current, Path path, FillRule rule);
/// current minus path (path must be non-self-overlapping, e.g. disjoint rectangles).
[[nodiscard]] ClipRef clip_exclude(const ClipRef& current, Path path);
/// current OR path (exact when `current` is a single level; approximated otherwise).
[[nodiscard]] ClipRef clip_union(const ClipRef& current, Path path, FillRule rule, bool& approximated);
/// Combine with the RGN_* / CombineMode semantics: 1 AND, 2 OR, 3 XOR, 4 DIFF (current - new), 5 COPY,
/// 6 complement (new - current). `path_clip` null = the empty region.
[[nodiscard]] ClipRef combine_clip(const ClipRef& current, const ClipRef& incoming, int mode, bool& approximated);
/// Every point of a clip chain mapped through `m`.
[[nodiscard]] ClipRef transform_clip(const ClipRef& clip, const Matrix& m);

// --------------------------------------------------------------------------- output

/// Tight bounds of everything drawn in `group` (in its parent's space).
[[nodiscard]] Rect drawing_bounds(const Group& group);

/// Collects nodes in page space, grouping consecutive nodes that share a clip.
class SceneBuilder {
 public:
  SceneBuilder() = default;
  /// Add a node drawn under `clip` (nullptr = unclipped).
  Result<void> add(Node node, const ClipRef& clip);
  Result<void> add_shape(Shape shape, const ClipRef& clip);
  /// Count points against the internal ceiling (Status::Limit when exceeded).
  Result<void> charge_points(std::size_t count);
  [[nodiscard]] Group take_root();
  [[nodiscard]] std::size_t node_count() const noexcept { return nodes_; }
  [[nodiscard]] Rect content_bounds() const;

 private:
  Group root_;
  ClipRef open_clip_;
  bool group_open_ = false;
  std::size_t nodes_ = 0;
  std::size_t points_ = 0;
};

// --------------------------------------------------------------------------- GDI

enum class Flavor : std::uint8_t { Wmf, Emf };

struct GdiPen {
  std::uint32_t style = 0;  ///< PS_* (low nibble) | caps | joins | PS_GEOMETRIC
  double width = 0.0;       ///< logical units; 0 = one device pixel
  Color color;
  std::vector<double> user_dash;  ///< PS_USERSTYLE lengths
  bool cosmetic = false;          ///< ExtCreatePen PS_COSMETIC
  bool extended = false;          ///< created by ExtCreatePen (caps/joins honoured)
};

struct GdiBrush {
  std::uint32_t style = 0;  ///< BS_*
  Color color{1, 1, 1, 1};
  std::uint32_t hatch = 0;
  FramePtr pattern;
  /// Pattern pixels whose colour comes from the text/background colours (monochrome BS_PATTERN).
  bool monochrome = false;
};

struct GdiFont {
  double height = 0.0;  ///< logical units; < 0 = em height, > 0 = cell height
  double width = 0.0;
  double escapement = 0.0;  ///< tenths of a degree
  int weight = 400;
  bool italic = false;
  bool underline = false;
  bool strikeout = false;
  int charset = 1;
  std::string face;
};

struct GdiPalette {
  std::vector<Color> colors;
};

struct GdiRegion {
  std::vector<Rect> rects;  ///< logical (WMF) units
};

using GdiObject = std::variant<std::monostate, GdiPen, GdiBrush, GdiFont, GdiPalette, GdiRegion>;

struct DcState {
  GdiPen pen;
  GdiBrush brush;
  GdiFont font;
  std::shared_ptr<const GdiPalette> palette;
  Color text_color{0, 0, 0, 1};
  Color bk_color{1, 1, 1, 1};
  int bk_mode = 2;    ///< 1 TRANSPARENT, 2 OPAQUE
  int poly_fill = 1;  ///< 1 ALTERNATE, 2 WINDING
  int rop2 = 13;      ///< R2_COPYPEN
  std::uint32_t text_align = 0;
  double char_extra = 0.0;
  int map_mode = 1;  ///< MM_TEXT
  Point window_org{0, 0}, window_ext{1, 1};
  Point viewport_org{0, 0}, viewport_ext{1, 1};
  bool window_ext_set = false;
  Matrix world;
  bool arc_clockwise = false;
  double miter_limit = 10.0;
  ClipRef clip;
  Point position{0, 0};  ///< current position, logical
};

/// What a raster operation does with the source bitmap.
enum class RopEffect : std::uint8_t { Source, SourceAnd, SourcePaint, SourceInvert, Pattern, Black, White, Nothing };

class GdiPlayer {
 public:
  GdiPlayer(Flavor flavor, const Context& ctx);

  [[nodiscard]] const Context& ctx() const noexcept { return ctx_; }
  [[nodiscard]] SceneBuilder& out() noexcept { return out_; }
  [[nodiscard]] DcState& dc() noexcept { return dc_; }
  [[nodiscard]] Flavor flavor() const noexcept { return flavor_; }
  [[nodiscard]] PixelBudget& pixels() noexcept { return pixels_; }
  /// Note a non-fatal approximation once per distinct message.
  void note(std::string_view message);

  // ---- frame and mapping
  /// EMF: device pixels -> page px (from the header). WMF: identity.
  void set_device_to_page(const Matrix& m) { device_to_page_ = m; recompute(); }
  [[nodiscard]] const Matrix& device_to_page() const noexcept { return device_to_page_; }
  /// EMF: device pixels per millimetre (for the metric mapping modes).
  void set_device_resolution(double px_per_mm_x, double px_per_mm_y);
  /// WMF: the logical rectangle shown on the page (placeable header or first window) and the page size.
  void set_wmf_frame(Rect logical, double page_width, double page_height);
  [[nodiscard]] bool has_wmf_frame() const noexcept { return wmf_frame_set_; }
  [[nodiscard]] std::pair<double, double> wmf_page_size() const noexcept { return {wmf_page_w_, wmf_page_h_}; }
  void set_map_mode(int mode);
  void set_window_org(Point p);
  void set_window_ext(Point p);
  void set_viewport_org(Point p);
  void set_viewport_ext(Point p);
  void offset_window_org(Point d);
  void offset_viewport_org(Point d);
  void scale_window_ext(double xn, double xd, double yn, double yd);
  void scale_viewport_ext(double xn, double xd, double yn, double yd);
  void set_world(const Matrix& m);
  void modify_world(const Matrix& m, std::uint32_t mode);
  /// Logical -> page.
  [[nodiscard]] const Matrix& to_page() const noexcept { return to_page_; }
  /// Logical -> device pixels.
  [[nodiscard]] Matrix to_device() const;

  // ---- objects
  void ensure_table(std::size_t size);
  /// WMF: put `object` in the lowest free slot.
  Result<void> create_object(GdiObject object);
  /// EMF: put `object` at `index`.
  void create_object_at(std::uint32_t index, GdiObject object);
  void select_object(std::uint32_t index);
  void delete_object(std::uint32_t index);
  [[nodiscard]] const GdiObject* object(std::uint32_t index) const;
  void save_dc();
  void restore_dc(int relative);

  // ---- drawing (logical coordinates)
  Result<void> move_to(Point p);
  Result<void> line_to(Point p);
  Result<void> polyline(std::span<const Point> points);
  Result<void> polygon(std::span<const Point> points);
  Result<void> poly_polygon(const std::vector<std::vector<Point>>& polygons);
  Result<void> poly_polyline(const std::vector<std::vector<Point>>& lines);
  Result<void> polybezier(std::span<const Point> points);
  Result<void> polybezier_to(std::span<const Point> points);
  Result<void> polyline_to(std::span<const Point> points);
  Result<void> poly_draw(std::span<const Point> points, std::span<const std::uint8_t> types);
  Result<void> rectangle(Rect r);
  Result<void> round_rect(Rect r, double corner_w, double corner_h);
  Result<void> ellipse(Rect r);
  enum class ArcKind : std::uint8_t { Arc, ArcTo, Pie, Chord };
  Result<void> arc(Rect box, Point start, Point end, ArcKind kind);
  Result<void> angle_arc(Point center, double radius, double start_deg, double sweep_deg);
  Result<void> set_pixel(Point p, Color c);

  // ---- paths
  void begin_path();
  void end_path();
  void close_figure();
  void abort_path();
  Result<void> fill_path();
  Result<void> stroke_path();
  Result<void> stroke_and_fill_path();
  void select_clip_path(int mode);

  // ---- clipping
  void intersect_clip_rect(Rect logical);
  void exclude_clip_rect(Rect logical);
  /// Region given as device rectangles (EMF RGNDATA) or logical ones (WMF); empty = null region.
  void select_clip_rects(const std::vector<Rect>& rects, bool device_units, int mode);
  void set_clip(ClipRef clip) { dc_.clip = std::move(clip); }
  void offset_clip(Point logical_delta);

  // ---- regions and fills
  Result<void> fill_rects(const std::vector<Rect>& rects, bool device_units, const GdiBrush& brush);
  Result<void> frame_rects(const std::vector<Rect>& rects, bool device_units, const GdiBrush& brush, double w, double h);
  Result<void> gradient_rect(Rect logical, Color c0, Color c1, bool vertical);
  Result<void> gradient_triangle(std::span<const Point> tri, std::span<const Color> colors);

  // ---- text
  struct TextRun {
    Point reference;             ///< logical
    std::u32string chars;
    std::vector<double> dx;      ///< logical advances (may be empty)
    std::vector<double> dy;      ///< ETO_PDY vertical advances (may be empty)
    std::uint32_t options = 0;   ///< ETO_*
    std::optional<Rect> rect;    ///< logical (ETO_OPAQUE / ETO_CLIPPED)
    double scale_x = 1.0;        ///< extra horizontal scale (EMF exScale in GM_COMPATIBLE is ignored)
  };
  Result<void> text_out(const TextRun& run);

  // ---- bitmaps
  /// Draw `frame` onto the parallelogram whose corners are `p0` (top-left),
  /// `p1` (top-right) and `p2` (bottom-left), in logical units.
  Result<void> draw_bitmap(const FramePtr& frame, Point p0, Point p1, Point p2, std::uint32_t rop);
  /// Same with a rectangle (x, y, w, h) in logical units; negative extents mirror.
  Result<void> draw_bitmap(const FramePtr& frame, Rect dest_xywh, std::uint32_t rop);
  Result<void> pat_blt(Rect dest_xywh, std::uint32_t rop);
  [[nodiscard]] static RopEffect classify_rop(std::uint32_t rop);

  // ---- shared with EMF+
  /// Emit a page-space path with an explicit paint/stroke under the current clip.
  Result<void> emit(Path page_path, const Paint& fill, FillRule rule, const Stroke& stroke, const ClipRef& clip);
  [[nodiscard]] Paint brush_paint(const GdiBrush& brush);
  /// An 8x8 device-pixel hatch tile (HS_* styles 0..5).
  [[nodiscard]] Paint hatch_paint(std::uint32_t hatch, Color color, bool opaque_bk, Color bk);
  [[nodiscard]] Paint picture_pattern(const FramePtr& frame);
  /// Draw pending LineTo segments now (before nodes added directly to `out()`).
  Result<void> flush() { return flush_pending(); }
  [[nodiscard]] Stroke pen_stroke();
  /// Length in page px of one device pixel.
  [[nodiscard]] double device_pixel() const;

  [[nodiscard]] bool drawing_enabled() const noexcept { return drawing_enabled_; }
  Result<void> set_drawing_enabled(bool on);

  /// Finish: draw what is pending and hand over the page group.
  [[nodiscard]] Result<Group> finish();

 private:
  void recompute();
  Result<void> draw_closed(Path logical, bool fill, bool stroke);
  Result<void> draw_open(Path logical);
  [[nodiscard]] FillRule fill_rule() const noexcept { return dc_.poly_fill == 2 ? FillRule::NonZero : FillRule::EvenOdd; }
  [[nodiscard]] bool pen_visible() const;
  [[nodiscard]] Color rop2_color(Color c);
  void add_to_path(const Path& logical);
  Result<void> flush_pending();
  [[nodiscard]] Path arc_path(Rect box, Point start, Point end, ArcKind kind, bool& ok);

  Flavor flavor_;
  const Context& ctx_;
  SceneBuilder out_;
  DcState dc_;
  std::vector<DcState> saved_;
  std::vector<GdiObject> objects_;
  std::size_t first_free_ = 0;
  std::vector<std::string> notes_;
  Matrix device_to_page_;
  Matrix to_page_;
  double dev_px_per_mm_x_ = 96.0 / 25.4;
  double dev_px_per_mm_y_ = 96.0 / 25.4;
  // WMF frame
  bool wmf_frame_set_ = false;
  Rect wmf_frame_;
  double wmf_page_w_ = 0.0, wmf_page_h_ = 0.0;
  // path bracket (page space)
  bool in_path_ = false;
  Path path_;
  // consecutive LineTo calls merge into one polyline
  Path pending_line_;
  Stroke pending_stroke_;
  ClipRef pending_clip_;
  bool drawing_enabled_ = true;
  PixelBudget pixels_;
};

// --------------------------------------------------------------------------- readers

/// Maximum nesting of metafiles embedded in metafiles (EMF+ images, GDI comments).
inline constexpr int max_metafile_nesting = 4;

struct ReadOptions {
  /// "auto": draw the EMF+ records of dual files; "off": always the GDI records.
  bool use_emf_plus = true;
};

/// A metafile read into a single page.
[[nodiscard]] Result<Scene> read_wmf(ByteView data, const Context& ctx, const ReadOptions& options, int depth = 0);
[[nodiscard]] Result<Scene> read_emf(ByteView data, const Context& ctx, const ReadOptions& options, int depth = 0);
/// Either format, by signature (used for embedded metafiles).
[[nodiscard]] Result<Scene> read_any_metafile(ByteView data, const Context& ctx, const ReadOptions& options,
                                              int depth);

[[nodiscard]] bool looks_like_wmf(ByteView data) noexcept;
[[nodiscard]] bool looks_like_emf(ByteView data) noexcept;

/// The EMF+ interpreter (metafile_emfplus.cpp). One instance per EMF.
class EmfPlusPlayer {
 public:
  EmfPlusPlayer(GdiPlayer& gdi, const ReadOptions& options, int depth);
  ~EmfPlusPlayer();
  EmfPlusPlayer(const EmfPlusPlayer&) = delete;
  EmfPlusPlayer& operator=(const EmfPlusPlayer&) = delete;
  /// The records of one EMR_COMMENT_EMFPLUS comment.
  Result<void> play(ByteView records);
  /// True once an EMF+ header record was seen (GDI drawing is then suppressed
  /// except between GetDC and the next EMF+ record).
  [[nodiscard]] bool active() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// --------------------------------------------------------------------------- writers

struct WriteOptions {
  bool text_as_fonts = false;  ///< ExtTextOutW/TextOut instead of outlines
  int gradient_steps = 64;
};

[[nodiscard]] Result<Bytes> write_emf(const Scene& scene, const Context& ctx, const WriteOptions& options);
[[nodiscard]] Result<Bytes> write_wmf(const Scene& scene, const Context& ctx, const WriteOptions& options);

}  // namespace cvt::vector::metafile
