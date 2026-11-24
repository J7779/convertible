// Convertible core - pdf family: content stream interpreter.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "engine/context.hpp"
#include "families/pdf/color.hpp"
#include "families/pdf/document.hpp"
#include "families/pdf/geom.hpp"
#include "families/pdf/pdf_font.hpp"
#include "families/pdf/raster.hpp"

namespace cvt::pdf {

enum class BlendMode : std::uint8_t {
  Normal, Multiply, Screen, Overlay, Darken, Lighten, ColorDodge, ColorBurn, HardLight, SoftLight, Difference,
  Exclusion, Hue, Saturation, Color, Luminosity
};

/// Opaque per-device state that follows the graphics state (soft masks).
struct DeviceSoftMask {
  virtual ~DeviceSoftMask() = default;
};

struct GState {
  Matrix ctm;
  std::shared_ptr<const ColorSpace> fill_space = device_gray();
  std::shared_ptr<const ColorSpace> stroke_space = device_gray();
  std::vector<double> fill_color{0.0};
  std::vector<double> stroke_color{0.0};
  Object fill_pattern;    ///< resolved pattern object when fill_space is Pattern
  Object stroke_pattern;
  raster::StrokeStyle stroke;
  double fill_alpha = 1.0;
  double stroke_alpha = 1.0;
  BlendMode blend = BlendMode::Normal;
  bool alpha_is_shape = false;
  std::shared_ptr<const DeviceSoftMask> soft_mask;
  // text state
  double char_spacing = 0, word_spacing = 0, h_scale = 1, leading = 0, font_size = 0, rise = 0;
  int render_mode = 0;
  std::shared_ptr<PdfFont> font;
};

struct GlyphEvent {
  GlyphCode code;
  std::u32string unicode;
  Matrix trm;           ///< text rendering matrix: glyph space (1 = font size em) to device/user space
  double advance = 0;   ///< advance along the baseline in text space units (scaled by font size, Tz, spacing)
  double width = 0;     ///< glyph width in text space units (font size applied), no spacing
  bool space_code = false;
  PdfFont* font = nullptr;
};

struct ImageEvent {
  const Dict* dict = nullptr;       ///< image dictionary (XObject stream dict or expanded inline dict)
  Object stream;                    ///< XObject stream (null for inline images)
  Bytes inline_data;                ///< raw inline image data (still filtered)
  const Dict* resources = nullptr;  ///< for named colour spaces
  bool is_inline = false;
};

struct GroupInfo {
  Rect bbox;                 ///< in form space (CTM applies)
  bool isolated = false;
  bool knockout = false;
  std::shared_ptr<const ColorSpace> space;
};

class Interpreter;

class Device {
 public:
  virtual ~Device() = default;
  virtual void save() {}
  virtual void restore() {}
  virtual void fill(const Path& /*user_path*/, raster::FillRule /*rule*/, const GState& /*gs*/) {}
  virtual void stroke(const Path& /*user_path*/, const GState& /*gs*/) {}
  virtual void clip(const Path& /*user_path*/, raster::FillRule /*rule*/, const GState& /*gs*/) {}
  /// One glyph of a show-text operator. Type 3 glyphs are executed by the
  /// interpreter (when `execute_type3()`) after this call.
  virtual void glyph(const GlyphEvent& /*event*/, const GState& /*gs*/) {}
  /// End of a text object (text clipping is applied here).
  virtual void end_text(const GState& /*gs*/) {}
  virtual void image(const ImageEvent& /*event*/, const GState& /*gs*/) {}
  virtual void shading(const Object& /*shading*/, const GState& /*gs*/) {}
  /// Transparency groups around form XObjects. `begin_group` returns false
  /// to have the content drawn directly.
  virtual bool begin_group(const GroupInfo& /*group*/, const GState& /*gs*/) { return false; }
  virtual void end_group(const GState& /*gs*/) {}
  /// ExtGState /SMask: build device state for the mask (null = no mask).
  virtual std::shared_ptr<const DeviceSoftMask> make_soft_mask(const Dict& /*smask*/, const GState& /*gs*/) {
    return nullptr;
  }
  virtual void begin_marked(const std::string& /*tag*/, const Dict& /*properties*/) {}
  virtual void end_marked() {}
  [[nodiscard]] virtual bool execute_type3() const { return true; }
  [[nodiscard]] virtual bool wants_paths() const { return true; }
  [[nodiscard]] virtual bool wants_images() const { return true; }
  /// Polled by the interpreter; true stops the page (work budget exhausted).
  [[nodiscard]] virtual bool exhausted() const { return false; }
};

struct InterpLimits {
  int max_depth = 24;                       ///< nested forms / patterns / Type 3 glyphs
  std::uint64_t max_operators = 20'000'000; ///< per page, all nesting levels
  std::uint64_t max_invocations = 20'000;   ///< form XObjects, patterns and Type 3 glyphs run per page
};

class Interpreter {
 public:
  Interpreter(PdfFile& file, Device& device, FontCache& fonts, const Context* ctx = nullptr);

  /// Run a content stream (decoded bytes) with the given resources.
  void run(ByteView content, const Object& resources, GState& gs, int depth = 0);
  /// Run a form XObject (matrix, bbox clip, group) with `gs` as the state.
  void run_form(const Object& form, GState gs, int depth, const Object& fallback_resources);
  /// Run a page's content (all streams of /Contents concatenated).
  void run_page(const Page& page, const Matrix& base);
  /// Total operators executed so far.
  [[nodiscard]] std::uint64_t operators() const noexcept { return operators_; }
  [[nodiscard]] bool aborted() const noexcept { return aborted_; }
  [[nodiscard]] PdfFile& file() noexcept { return file_; }
  [[nodiscard]] FontCache& fonts() noexcept { return fonts_; }
  InterpLimits limits;

  /// Fonts are cached per interpreter keyed by object.
  std::shared_ptr<PdfFont> font_for(const Object& font_ref);

 private:
  struct State;
  void show_text(State& st, std::string_view bytes, const Dict& resources, int depth);

  PdfFile& file_;
  Device& device_;
  FontCache& fonts_;
  const Context* ctx_ = nullptr;
  std::uint64_t operators_ = 0;
  std::uint64_t invocations_ = 0;
  bool aborted_ = false;
  std::map<std::uint32_t, std::shared_ptr<PdfFont>> font_cache_;
  std::map<const void*, std::shared_ptr<PdfFont>> direct_font_cache_;
  std::map<const void*, std::shared_ptr<const Bytes>> form_content_cache_;
};

/// Concatenate the page content streams.
[[nodiscard]] Bytes page_content(PdfFile& file, const Page& page);

/// Matrix from PDF user space of the page to a raster of `scale` pixels per
/// point (crop box origin, rotation, y flipped), and the resulting size.
struct PageTransform {
  Matrix matrix;
  double width = 0;   ///< in pixels (or points when scale = 1)
  double height = 0;
};
[[nodiscard]] PageTransform page_transform(const Page& page, double scale);

}  // namespace cvt::pdf
