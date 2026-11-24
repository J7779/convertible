// Convertible core - vector family: TikZ/PGF picture writer.
//
// Writes a `tikzpicture` (optionally wrapped in a minimal `standalone`
// document): colours as \definecolor, shapes as \fill / \draw / \filldraw
// with `controls` for curves, line width/cap/join/miter/dash, opacity, the
// even-odd rule, clipping with \clip inside scopes, group transforms as
// cm={a,b,c,d,(e,f)}, group opacity as transparency groups, text as \node
// (or as outlines). TikZ is y-up: coordinates are flipped, so the page's
// top-left corner is (0, height). Pictures are dropped with a warning.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <utility>

#include "engine/registry.hpp"
#include "families/vector/geom.hpp"
#include "families/vector/misc_common.hpp"
#include "families/vector/vector_codecs.hpp"
#include "util/text.hpp"

namespace cvt::vector::misc {

namespace {

/// LaTeX-safe text: specials escaped, control characters dropped, UTF-8 kept.
std::string latex_escape(std::string_view text) {
  std::string out;
  for (const char c : text) {
    switch (c) {
      case '\\': out += "\\textbackslash{}"; break;
      case '{': out += "\\{"; break;
      case '}': out += "\\}"; break;
      case '$': out += "\\$"; break;
      case '&': out += "\\&"; break;
      case '#': out += "\\#"; break;
      case '%': out += "\\%"; break;
      case '_': out += "\\_"; break;
      case '~': out += "\\textasciitilde{}"; break;
      case '^': out += "\\textasciicircum{}"; break;
      case '<': out += "\\textless{}"; break;
      case '>': out += "\\textgreater{}"; break;
      case '|': out += "\\textbar{}"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
          if (c == '\t') out += ' ';
        } else {
          out += c;
        }
    }
  }
  return util::sanitize_utf8(out);
}

class TikzWriter {
 public:
  TikzWriter(WriteContext& ctx, int precision, bool centimetres, bool outlines)
      : ctx_(ctx), precision_(precision), unit_(centimetres ? 2.54 / 96.0 : 72.0 / 96.0), centimetres_(centimetres),
        outlines_(outlines) {}

  Result<Bytes> write(const Scene& scene, bool standalone) {
    if (scene.pages.empty()) return fail(Status::Encode, "the drawing has no pages");
    std::string pictures;
    for (std::size_t p = 0; p < scene.pages.size(); ++p) {
      const Page& page = scene.pages[p];
      if (!(page.width > 0) || !(page.height > 0) || !std::isfinite(page.width) || !std::isfinite(page.height)) {
        return fail(Status::Encode, "the drawing has no size");
      }
      height_ = page.height;
      flip_ = Matrix{unit_, 0, 0, -unit_, 0, unit_ * page.height};
      flip_inverse_ = flip_.inverted();
      body_.clear();
      CVT_TRY(write_group(page.root, Matrix{}, 1));
      pictures += std::format("\\begin{{tikzpicture}}[x=1{0},y=1{0}]\n", centimetres_ ? "cm" : "pt");
      pictures += std::format("  \\path[use as bounding box] (0,0) rectangle ({},{});\n", num(page.width * unit_),
                              num(page.height * unit_));
      pictures += body_;
      pictures += "\\end{tikzpicture}\n";
      CVT_TRY(ctx_.limits.check_output(pictures.size()));
    }
    std::string out;
    if (standalone) {
      out += "\\documentclass[tikz]{standalone}\n\\usepackage[utf8]{inputenc}\n\\usepackage[T1]{fontenc}\n";
      out += "\\begin{document}\n";
    } else {
      out += "% Requires \\usepackage{tikz} in the preamble.\n";
    }
    out += colors_;
    out += pictures;
    if (standalone) out += "\\end{document}\n";
    return to_bytes(out);
  }

 private:
  WriteContext& ctx_;
  int precision_;
  double unit_;
  bool centimetres_;
  bool outlines_;
  double height_ = 0;
  Matrix flip_, flip_inverse_;
  std::string body_;
  std::string colors_;
  std::map<std::uint32_t, std::string> color_names_;
  std::size_t nodes_ = 0;
  bool warned_picture_ = false, warned_gradient_ = false, warned_effects_ = false, warned_hidden_ = false;

  std::string num(double v) const { return format_number(v, precision_); }
  static std::string pad(int depth) { return std::string(static_cast<std::size_t>(depth) * 2, ' '); }
  void warn_once(bool& flag, std::string_view message) {
    if (flag) return;
    flag = true;
    ctx_.warn(std::string(message));
  }

  std::string point(Point p) const {
    const Point q = flip_.apply(p);
    return std::format("({},{})", num(q.x), num(q.y));
  }

  std::string color_name(const Color& c) {
    const auto byte = [](float v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    const std::uint32_t key = byte(c.r) << 16 | byte(c.g) << 8 | byte(c.b);
    if (const auto it = color_names_.find(key); it != color_names_.end()) return it->second;
    std::string name = std::format("cvtc{}", color_names_.size() + 1);
    // Names must be letters only for some TeX setups; map digits to letters.
    for (char& ch : name) {
      if (ch >= '0' && ch <= '9') ch = static_cast<char>('A' + (ch - '0'));
    }
    colors_ += std::format("\\definecolor{{{}}}{{RGB}}{{{},{},{}}}\n", name, key >> 16, (key >> 8) & 255, key & 255);
    color_names_.emplace(key, name);
    return name;
  }

  /// Solid colour and alpha for a paint.
  std::pair<Color, double> solid(const Paint& paint) {
    Color c = paint.color;
    if (paint.type == Paint::Type::Gradient && paint.gradient) {
      warn_once(warned_gradient_, "Gradients and patterns were replaced by their average colour.");
      c = gradient_average(*paint.gradient);
    } else if (paint.type == Paint::Type::Pattern) {
      warn_once(warned_gradient_, "Gradients and patterns were replaced by their average colour.");
      c = Color{0.5f, 0.5f, 0.5f, 1.0f};
    }
    return {c, std::clamp(paint.opacity * c.a, 0.0, 1.0)};
  }

  std::string path_text(const Path& path, int depth) {
    std::string out;
    std::size_t line_start = 0;
    const auto add = [&](std::string piece) {
      if (out.size() - line_start > 90) {
        out += "\n" + pad(depth + 2);
        line_start = out.size();
      } else if (!out.empty()) {
        out += ' ';
      }
      out += piece;
    };
    Point cur{}, start{};
    bool open = false;
    std::size_t i = 0;
    for (const Verb verb : path.verbs) {
      const std::size_t n = verb == Verb::Move || verb == Verb::Line ? 1 : verb == Verb::Quad ? 2 : verb == Verb::Cubic ? 3 : 0;
      if (i + n > path.points.size()) break;
      const auto ensure_open = [&] {
        if (!open) {
          add(point(cur));
          start = cur;
          open = true;
        }
      };
      switch (verb) {
        case Verb::Move:
          cur = start = path.points[i];
          add(point(cur));
          open = true;
          break;
        case Verb::Line:
          ensure_open();
          cur = path.points[i];
          add("-- " + point(cur));
          break;
        case Verb::Quad: {
          ensure_open();
          const Point c = path.points[i], p = path.points[i + 1];
          add(std::format(".. controls {} and {} .. {}", point(cur + (c - cur) * (2.0 / 3.0)),
                          point(p + (c - p) * (2.0 / 3.0)), point(p)));
          cur = p;
          break;
        }
        case Verb::Cubic:
          ensure_open();
          add(std::format(".. controls {} and {} .. {}", point(path.points[i]), point(path.points[i + 1]),
                          point(path.points[i + 2])));
          cur = path.points[i + 2];
          break;
        case Verb::Close:
          if (open) add("-- cycle");
          cur = start;
          open = false;
          break;
      }
      i += n;
    }
    return out;
  }

  Result<void> write_group(const Group& group, const Matrix& total, int depth) {
    if (!group.visible) {
      warn_once(warned_hidden_, "Hidden layers were left out.");
      return {};
    }
    if (depth > 400) return fail(Status::Encode, "the drawing is nested too deeply");
    if (group.mask || group.filter || group.blend != BlendMode::Normal) {
      warn_once(warned_effects_, "Masks, filters and blend modes cannot be expressed in TikZ and were dropped.");
    }
    const Matrix here = total * group.transform;
    std::vector<std::string> options;
    if (!group.transform.is_identity()) {
      const Matrix m = flip_ * group.transform * flip_inverse_;
      options.push_back(std::format("cm={{{},{},{},{},({},{})}}", format_number(m.a, 6), format_number(m.b, 6),
                                    format_number(m.c, 6), format_number(m.d, 6), num(m.e), num(m.f)));
    }
    if (group.opacity < 1.0) {
      options.push_back(std::format("transparency group,opacity={}", format_number(std::max(group.opacity, 0.0), 4)));
    }
    const bool scope = !options.empty() || group.clip != nullptr;
    int inner = depth;
    if (scope) {
      std::string joined;
      for (const std::string& o : options) joined += (joined.empty() ? "" : ",") + o;
      body_ += pad(depth) + "\\begin{scope}" + (joined.empty() ? "" : "[" + joined + "]") + "\n";
      inner = depth + 1;
      const ClipPath* clip = group.clip.get();
      for (int guard = 0; clip != nullptr && guard < 64; ++guard) {
        Path combined;
        bool even_odd = false;
        for (const ClipPath::Item& item : clip->items) {
          append(combined, item.path);
          even_odd = even_odd || item.rule == FillRule::EvenOdd;
        }
        if (combined.empty()) {
          // An empty clip hides everything.
          body_ += pad(inner) + "\\clip (0,0) rectangle (0,0);\n";
        } else {
          body_ += pad(inner) + (even_odd ? "\\clip[even odd rule] " : "\\clip ") + path_text(combined, inner) + ";\n";
        }
        clip = clip->clip.get();
      }
    }
    for (const Node& node : group.children) {
      if (++nodes_ % 512 == 0) CVT_TRY(ctx_.checkpoint(0.5, "Writing TikZ"));
      if (const auto* shape = std::get_if<Shape>(&node.value)) {
        write_shape(*shape, here, inner);
      } else if (const auto* text = std::get_if<Text>(&node.value)) {
        write_text(*text, here, inner);
      } else if (std::holds_alternative<Picture>(node.value)) {
        warn_once(warned_picture_, "Raster pictures cannot be written as TikZ paths and were dropped.");
      } else if (const auto* child = std::get_if<Group>(&node.value)) {
        CVT_TRY(write_group(*child, here, inner));
      }
    }
    if (scope) body_ += pad(depth) + "\\end{scope}\n";
    return {};
  }

  void write_shape(const Shape& shape, const Matrix& total, int depth) {
    const bool fill = shape.fill.visible();
    const bool stroke = shape.stroke.paint.visible() && (shape.stroke.width > 0 || shape.stroke.hairline);
    if ((!fill && !stroke) || shape.path.empty()) return;
    std::vector<std::string> options;
    if (fill) {
      const auto [c, alpha] = solid(shape.fill);
      options.push_back("fill=" + color_name(c));
      if (alpha < 1.0) options.push_back("fill opacity=" + format_number(alpha, 4));
      if (shape.fill_rule == FillRule::EvenOdd) options.push_back("even odd rule");
    }
    if (stroke) {
      const Stroke& s = shape.stroke;
      const auto [c, alpha] = solid(s.paint);
      options.push_back("draw=" + color_name(c));
      if (alpha < 1.0) options.push_back("draw opacity=" + format_number(alpha, 4));
      // Line widths are not affected by cm: scale them here (always in pt).
      const double scale = total.mean_scale() * 0.75;
      const double width = s.hairline ? 0.4 : s.width * scale;
      options.push_back("line width=" + format_number(width, std::max(precision_, 3)) + "pt");
      if (s.cap == LineCap::Round) options.push_back("line cap=round");
      else if (s.cap == LineCap::Square) options.push_back("line cap=rect");
      if (s.join == LineJoin::Round) options.push_back("line join=round");
      else if (s.join == LineJoin::Bevel) options.push_back("line join=bevel");
      if (s.join != LineJoin::Round && s.join != LineJoin::Bevel && std::abs(s.miter_limit - 10.0) > 1e-9) {
        options.push_back("miter limit=" + format_number(s.miter_limit, 4));
      }
      if (!s.dash.empty() && s.dash.size() % 2 == 0) {
        std::string pattern = "dash pattern=";
        for (std::size_t k = 0; k < s.dash.size(); ++k) {
          pattern += std::format("{}{} {}pt", k == 0 ? "" : " ", k % 2 == 0 ? "on" : "off",
                                 format_number(std::max(0.0, s.dash[k] * scale), 3));
        }
        options.push_back(pattern);
        if (s.dash_offset != 0.0) options.push_back("dash phase=" + format_number(s.dash_offset * scale, 3) + "pt");
      }
    }
    const char* command = fill && stroke ? "\\filldraw" : fill ? "\\fill" : "\\draw";
    std::string joined;
    for (const std::string& o : options) joined += (joined.empty() ? "" : ",") + o;
    body_ += std::format("{}{}[{}] {};\n", pad(depth), command, joined, path_text(shape.path, depth));
  }

  void write_text(const Text& text, const Matrix& total, int depth) {
    if (outlines_ || text.content.empty()) {
      Shape shape;
      shape.path = text.outline;
      shape.fill = text.fill;
      shape.stroke = text.stroke;
      write_shape(shape, total, depth);
      return;
    }
    if (!text.fill.visible()) return;
    const auto [c, alpha] = solid(text.fill);
    const double size = text.font.size * 0.75;  // pt, before the group transforms
    std::string font = std::format("\\fontsize{{{}}}{{{}}}\\selectfont", format_number(size, 2), format_number(size * 1.2, 2));
    const std::string family = util::to_lower(text.font.family);
    if (family.find("mono") != std::string::npos || family.find("courier") != std::string::npos) font += "\\ttfamily";
    else if (family.find("serif") != std::string::npos && family.find("sans") == std::string::npos) font += "\\rmfamily";
    else font += "\\sffamily";
    if (text.font.weight >= 600) font += "\\bfseries";
    if (text.font.italic) font += "\\itshape";
    std::string options = std::format("anchor=base west,inner sep=0pt,outer sep=0pt,transform shape,font={},text={}",
                                      font, color_name(c));
    if (alpha < 1.0) options += ",text opacity=" + format_number(alpha, 4);
    (void)total;
    body_ += std::format("{}\\node[{}] at {} {{{}}};\n", pad(depth), options, point(text.origin),
                         latex_escape(text.content));
  }
};

Result<Bytes> write_tikz(const Scene& scene, WriteContext& ctx) {
  const int precision = static_cast<int>(ctx.options.get_int("precision", 3, 0, 8));
  const bool cm = ctx.options.get_string("units", "pt") == "cm";
  const bool outlines = ctx.options.get_string("text", "nodes") == "outlines";
  return TikzWriter(ctx, precision, cm, outlines).write(scene, ctx.options.get_bool("standalone", false));
}

}  // namespace

void register_tikz(Registry& registry) {
  VectorFormat format;
  format.info.id = "tikz";
  format.info.name = "TikZ picture";
  format.info.description = "LaTeX TikZ/PGF drawing commands for including vector graphics in TeX documents.";
  format.info.category = Category::Vector;
  format.info.extensions = {"tikz"};
  format.info.mime = "application/x-tex";
  format.info.popularity = 15;
  format.write = write_tikz;
  format.write_options = {
      OptionSpec::boolean("standalone", "Standalone document", false,
                          "Wrap the picture in a minimal LaTeX document (standalone class) that compiles on its own."),
      OptionSpec::choice("units", "Units", "pt", {"pt", "cm"}, "Unit of the coordinates in the TikZ code."),
      OptionSpec::choice("text", "Text", "nodes", {"nodes", "outlines"},
                         "Write text as TikZ nodes (editable, typeset by TeX) or as filled glyph outlines."),
      OptionSpec::integer("precision", "Precision", 3, 0, 8, "Decimal places of coordinates.").advanced(),
  };
  format.lossy_write = true;
  format.can_embed_images = false;
  format.document_reader = false;
  register_vector_format(registry, std::move(format));
}

}  // namespace cvt::vector::misc
