// Convertible core - office family: XPS and OpenXPS documents (read).
//
// The fixed document sequence is followed to every FixedPage; Glyphs runs
// are placed with their accumulated render transforms, grouped into lines
// and paragraphs in reading order (top to bottom, left to right), larger
// type becomes headings, ImageBrush pictures are placed by position, and
// pages are separated by page breaks. Writing XPS is not supported.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/document/document_api.hpp"
#include "families/office/office.hpp"
#include "families/office/office_common.hpp"
#include "util/text.hpp"

namespace cvt::office {

namespace {

using util::XmlNode;
using Matrix = std::array<double, 6>;  // m11 m12 m21 m22 dx dy

Matrix multiply(const Matrix& a, const Matrix& b) {
  // Apply a, then b.
  return {a[0] * b[0] + a[1] * b[2], a[0] * b[1] + a[1] * b[3], a[2] * b[0] + a[3] * b[2],
          a[2] * b[1] + a[3] * b[3], a[4] * b[0] + a[5] * b[2] + b[4], a[4] * b[1] + a[5] * b[3] + b[5]};
}

Matrix parse_matrix(std::string_view text) {
  Matrix m{1, 0, 0, 1, 0, 0};
  std::size_t k = 0;
  for (std::string_view part : util::split(text, ',', false)) {
    if (k >= 6) break;
    if (auto v = util::parse_double(util::trim(part))) m[k] = *v;
    ++k;
  }
  return k == 6 ? m : Matrix{1, 0, 0, 1, 0, 0};
}

struct Run {
  double x = 0, y = 0, size = 0, width = 0;
  std::string text;
  bool bold = false;
};
struct Picture {
  double x = 0, y = 0;
  ir::EncodedImage image;
};

class XpsReader {
 public:
  XpsReader(const ReadContext& ctx, Package& package) : ctx_(ctx), package_(package) {}
  Result<ir::Document> read();

 private:
  void walk(XmlNode node, const Matrix& m, const std::string& page, int depth);
  ir::Blocks layout();

  const ReadContext& ctx_;
  Package& package_;
  std::vector<Run> runs_;
  std::vector<Picture> pictures_;
  std::size_t nodes_ = 0;
};

void XpsReader::walk(XmlNode node, const Matrix& m, const std::string& page, int depth) {
  if (depth > 64 || ++nodes_ > 2'000'000) return;
  for (XmlNode c : node.elements()) {
    const std::string_view name = c.local_name();
    Matrix local = m;
    std::string_view transform = attr(c, "RenderTransform");
    if (transform.empty()) {
      if (XmlNode t = child(child(c, std::string(name) + ".RenderTransform"), "MatrixTransform")) transform = attr(t, "Matrix");
    }
    if (!transform.empty()) local = multiply(parse_matrix(transform), m);
    if (name == "Canvas") {
      walk(c, local, page, depth + 1);
    } else if (name == "Glyphs") {
      std::string text(attr(c, "UnicodeString"));
      if (text.starts_with("{}")) text.erase(0, 2);
      if (util::trim(text).empty()) continue;
      const double ox = util::parse_double(attr(c, "OriginX")).value_or(0);
      const double oy = util::parse_double(attr(c, "OriginY")).value_or(0);
      const double em = util::parse_double(attr(c, "FontRenderingEmSize")).value_or(12);
      Run run;
      run.x = ox * local[0] + oy * local[2] + local[4];
      run.y = ox * local[1] + oy * local[3] + local[5];
      run.size = em * std::sqrt(std::abs(local[0] * local[3] - local[1] * local[2]));
      run.width = static_cast<double>(util::utf8_length(text)) * run.size * 0.5;
      const std::string font = util::to_lower(attr(c, "FontUri"));
      run.bold = font.find("bold") != std::string::npos || attr(c, "StyleSimulations").find("Bold") != std::string_view::npos;
      run.text = std::move(text);
      runs_.push_back(std::move(run));
    } else if (name == "Path") {
      XmlNode brush;
      for (XmlNode d : c.descendants()) {
        if (d.is_element() && d.local_name() == "ImageBrush") {
          brush = d;
          break;
        }
      }
      if (!brush) continue;
      const std::string source = resolve_part(page, percent_decode(attr(brush, "ImageSource")));
      auto bytes = package_.try_read(source);
      if (!bytes) continue;
      Picture picture;
      std::vector<double> viewport;
      for (std::string_view part : util::split(attr(brush, "Viewport"), ',', false)) {
        if (auto v = util::parse_double(util::trim(part))) viewport.push_back(*v);
      }
      if (viewport.size() >= 2) {
        picture.x = viewport[0] * local[0] + viewport[1] * local[2] + local[4];
        picture.y = viewport[0] * local[1] + viewport[1] * local[3] + local[5];
      }
      picture.image.bytes = std::move(*bytes);
      picture.image.mime = document::sniff_image_mime(picture.image.bytes);
      if (picture.image.mime.empty()) picture.image.mime = mime_for_extension(source);
      pictures_.push_back(std::move(picture));
    }
  }
}

ir::Blocks XpsReader::layout() {
  ir::Blocks out;
  if (runs_.empty() && pictures_.empty()) return out;
  std::vector<double> sizes;
  for (const Run& r : runs_) sizes.push_back(r.size);
  std::ranges::sort(sizes);
  const double body = sizes.empty() ? 12 : sizes[sizes.size() / 2];
  // Lines: runs whose baselines are close.
  std::ranges::stable_sort(runs_, [](const Run& a, const Run& b) { return a.y < b.y; });
  struct Line {
    double y = 0, x = 0, size = 0;
    std::vector<Run> runs;
  };
  std::vector<Line> lines;
  for (Run& r : runs_) {
    if (!lines.empty() && std::abs(lines.back().y - r.y) < std::max(2.0, r.size * 0.4)) {
      lines.back().runs.push_back(std::move(r));
    } else {
      Line line;
      line.y = r.y;
      line.runs.push_back(std::move(r));
      lines.push_back(std::move(line));
    }
  }
  for (Line& line : lines) {
    std::ranges::sort(line.runs, [](const Run& a, const Run& b) { return a.x < b.x; });
    line.x = line.runs.front().x;
    for (const Run& r : line.runs) line.size = std::max(line.size, r.size);
  }
  std::size_t picture = 0;
  std::ranges::sort(pictures_, [](const Picture& a, const Picture& b) { return a.y < b.y; });
  std::string paragraph;
  bool paragraph_bold = true;
  double last_y = -1e9;
  double last_size = body;
  const auto flush = [&] {
    std::string text(util::trim(paragraph));
    paragraph.clear();
    if (text.empty()) return;
    const double ratio = last_size / body;
    if (ratio >= 1.25 && text.size() < 150) {
      out.push_back(ir::Heading{ratio >= 1.8 ? 1 : ratio >= 1.45 ? 2 : 3, {ir::Text{text}}});
    } else if (paragraph_bold && text.size() < 100 && ratio > 1.05) {
      out.push_back(ir::Heading{3, {ir::Text{text}}});
    } else {
      out.push_back(ir::Paragraph{{ir::Text{text}}});
    }
    paragraph_bold = true;
  };
  for (const Line& line : lines) {
    while (picture < pictures_.size() && pictures_[picture].y < line.y) {
      flush();
      out.push_back(ir::ImageBlock{std::move(pictures_[picture].image), {}, {}});
      ++picture;
    }
    std::string text;
    double end = -1e9;
    bool bold = true;
    for (const Run& r : line.runs) {
      if (!text.empty() && r.x - end > r.size * 0.2) text += " ";
      text += r.text;
      end = r.x + r.width;
      bold = bold && r.bold;
    }
    const double gap = line.y - last_y;
    const bool new_paragraph = gap > line.size * 1.7 || std::abs(line.size - last_size) > 0.5;
    if (new_paragraph) flush();
    if (!paragraph.empty()) paragraph += " ";
    paragraph += text;
    paragraph_bold = paragraph_bold && bold;
    last_y = line.y;
    last_size = line.size;
  }
  flush();
  for (; picture < pictures_.size(); ++picture) out.push_back(ir::ImageBlock{std::move(pictures_[picture].image), {}, {}});
  return out;
}

Result<ir::Document> XpsReader::read() {
  std::string sequence;
  for (const auto& rel : package_.relationships("")) {
    if (rel.type.ends_with("/fixedrepresentation")) sequence = rel.target;
  }
  if (sequence.empty()) {
    for (const std::string& name : package_.names()) {
      if (util::iends_with(name, ".fdseq")) sequence = name;
    }
  }
  if (sequence.empty()) return fail(Status::Decode, "the XPS package has no fixed document sequence");
  CVT_TRY_ASSIGN(util::XmlDocument seq, package_.xml(sequence, false));
  std::vector<std::string> pages;
  for (XmlNode ref : seq.root().children_local("DocumentReference")) {
    const std::string fdoc = resolve_part(sequence, percent_decode(attr(ref, "Source")));
    auto doc = package_.xml(fdoc, false);
    if (!doc) continue;
    for (XmlNode content : doc->root().children_local("PageContent")) {
      pages.push_back(resolve_part(fdoc, percent_decode(attr(content, "Source"))));
      if (pages.size() > 100000) break;
    }
  }
  CVT_TRY(ctx_.limits.check_archive_entries(pages.size()));
  ir::Document doc;
  for (std::size_t i = 0; i < pages.size(); ++i) {
    CVT_TRY(ctx_.checkpoint(static_cast<double>(i) / static_cast<double>(pages.size()), "Reading pages"));
    auto page = package_.xml(pages[i], false);
    if (!page) continue;
    runs_.clear();
    pictures_.clear();
    walk(page->root(), Matrix{1, 0, 0, 1, 0, 0}, pages[i], 0);
    if (!doc.blocks.empty()) doc.blocks.push_back(ir::PageBreak{});
    for (ir::Block& b : layout()) doc.blocks.push_back(std::move(b));
  }
  if (auto core = package_.xml("docProps/core.xml", false)) {
    doc.metadata.title = std::string(util::trim(child(core->root(), "title").text()));
  }
  return doc;
}

Result<AnyIR> read_xps(ReadContext& ctx) {
  CVT_TRY_ASSIGN(Package package, Package::open(ctx.input, ctx.limits));
  XpsReader reader(ctx, package);
  CVT_TRY_ASSIGN(ir::Document doc, reader.read());
  finish_metadata(doc, read_settings(ctx.options));
  return AnyIR(std::move(doc));
}

}  // namespace

void register_xps(Registry& registry) {
  for (const bool open : {false, true}) {
    FormatInfo info;
    info.id = open ? "oxps" : "xps";
    info.name = open ? "OpenXPS document" : "XPS document";
    info.description = "Fixed-layout document in the XML Paper Specification.";
    info.category = Category::Document;
    info.extensions = {info.id};
    info.mime = open ? "application/oxps" : "application/vnd.ms-xpsdocument";
    info.popularity = open ? 10 : 20;
    info.sniff = [open](ByteView data, std::string_view filename) {
      if (!zip_mentions(data, ".fdseq") && !zip_mentions(data, "FixedDocumentSequence")) return 0;
      const bool oxps_name = util::iends_with(filename, ".oxps");
      // Content alone cannot tell XPS from OpenXPS cheaply; the name decides.
      if (open) return oxps_name ? 95 : 0;
      return oxps_name ? 60 : 94;
    };
    registry.add_format(std::move(info));
    Codec codec;
    codec.format_id = open ? "oxps" : "xps";
    codec.kind = Kind::Document;
    codec.read = read_xps;
    codec.read_cost = 25;
    codec.lossy = true;
    codec.read_options = {options::strip_metadata()};
    registry.add_codec(std::move(codec));
  }
}

}  // namespace cvt::office
