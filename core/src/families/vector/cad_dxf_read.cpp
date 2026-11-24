// Convertible core - vector family: DXF reader (R12 .. 2018, ASCII and binary).
//
// Tags are grouped into sections, tables, blocks and entities; entities are
// drawn into per-layer groups in drawing units (y up), with INSERTs expanded
// into world coordinates, and the page maps drawing units to CSS px (y down).
// 3D faces (3DFACE, polyface and polygon-mesh POLYLINEs, MESH) are drawn as
// wireframes in top view and collected into Scene::mesh.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "families/vector/cad_dxf.hpp"
#include "families/vector/cad_internal.hpp"
#include "families/vector/font.hpp"
#include "families/vector/geom.hpp"
#include "util/text.hpp"

namespace cvt::vector::cad {

namespace {

constexpr double pi = std::numbers::pi;
constexpr double deg = pi / 180.0;
constexpr int max_block_depth = 32;
constexpr std::uint64_t max_points = 20'000'000;

using Tags = std::span<const DxfTag>;

std::string upper(std::string_view s) { return util::to_upper(util::trim(s)); }

const DxfTag* find(Tags tags, int code) {
  for (const DxfTag& t : tags) {
    if (t.code == code) return &t;
  }
  return nullptr;
}
double get_d(Tags tags, int code, double fallback = 0.0) {
  const DxfTag* t = find(tags, code);
  return t != nullptr ? tag_double(*t) : fallback;
}
std::int64_t get_i(Tags tags, int code, std::int64_t fallback = 0) {
  const DxfTag* t = find(tags, code);
  return t != nullptr ? tag_int(*t) : fallback;
}
std::string_view get_s(Tags tags, int code, std::string_view fallback = {}) {
  const DxfTag* t = find(tags, code);
  if (t == nullptr) return fallback;
  std::string_view s = t->text;
  while (!s.empty() && (s.back() == '\r' || s.back() == '\0')) s.remove_suffix(1);
  return s;
}
Vec3 get_p(Tags tags, int code, Vec3 fallback = {}) {
  const DxfTag* x = find(tags, code);
  if (x == nullptr) return fallback;
  return {tag_double(*x), get_d(tags, code + 10, 0.0), get_d(tags, code + 20, 0.0)};
}
bool finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
Point xy(Vec3 v) { return {v.x, v.y}; }

// --------------------------------------------------------------------------- document model

struct Entity {
  std::string type;  // upper case
  Tags tags;
  std::vector<Entity> children;  // VERTEX / ATTRIB
};

struct Record {
  std::string type;
  Tags tags;
};

struct LayerInfo {
  std::string name;
  int color = 7;
  std::optional<std::uint32_t> rgb;
  bool off = false;
  bool frozen = false;
  std::string linetype = "CONTINUOUS";
  int lineweight = -3;
};

struct BlockDef {
  std::string name;
  Vec3 base;
  bool xref = false;
  std::vector<Entity> entities;
};

struct Document {
  std::map<std::string, Tags, std::less<>> header;
  std::map<std::string, LayerInfo, std::less<>> layers;
  std::vector<std::string> layer_order;
  std::map<std::string, std::vector<double>, std::less<>> linetypes;
  std::map<std::string, DxfTextStyle, std::less<>> styles;
  std::map<std::string, BlockDef, std::less<>> blocks;
  std::vector<Entity> entities;
  DxfStrings strings;
  std::string version;
};

std::vector<Record> split_records(Tags tags) {
  std::vector<Record> out;
  std::size_t i = 0;
  while (i < tags.size()) {
    if (tags[i].code != 0) {
      ++i;
      continue;
    }
    const std::size_t start = i + 1;
    std::size_t end = start;
    while (end < tags.size() && tags[end].code != 0) ++end;
    out.push_back({upper(tags[i].text), tags.subspan(start, end - start)});
    i = end;
  }
  return out;
}

std::vector<Entity> group_entities(std::span<const Record> records) {
  std::vector<Entity> out;
  std::size_t i = 0;
  while (i < records.size()) {
    const Record& r = records[i];
    Entity e{r.type, r.tags, {}};
    ++i;
    if (r.type == "POLYLINE") {
      while (i < records.size() && records[i].type == "VERTEX") {
        e.children.push_back({records[i].type, records[i].tags, {}});
        ++i;
      }
      if (i < records.size() && records[i].type == "SEQEND") ++i;
    } else if (r.type == "INSERT") {
      while (i < records.size() && records[i].type == "ATTRIB") {
        e.children.push_back({records[i].type, records[i].tags, {}});
        ++i;
      }
      if (!e.children.empty() && i < records.size() && records[i].type == "SEQEND") ++i;
    } else if (r.type == "SEQEND" || r.type == "VERTEX" || r.type == "ATTRIB") {
      continue;  // orphans
    }
    out.push_back(std::move(e));
  }
  return out;
}

std::vector<double> linetype_pattern(Tags tags) {
  std::vector<double> pattern;
  for (const DxfTag& t : tags) {
    if (t.code == 49 && pattern.size() < 64) pattern.push_back(tag_double(t));
  }
  return pattern;
}

Result<Document> parse_document(std::span<const DxfTag> all, ReadContext& ctx) {
  Document doc;
  std::size_t i = 0;
  bool any_section = false;
  while (i < all.size()) {
    const DxfTag& t = all[i];
    if (!(t.code == 0 && upper(t.text) == "SECTION")) {
      ++i;
      continue;
    }
    any_section = true;
    std::string name;
    if (i + 1 < all.size() && all[i + 1].code == 2) name = upper(all[i + 1].text);
    std::size_t end = i + 1;
    while (end < all.size() && !(all[end].code == 0 && (upper(all[end].text) == "ENDSEC" || upper(all[end].text) == "SECTION" ||
                                                          upper(all[end].text) == "EOF"))) {
      ++end;
    }
    const std::size_t body = std::min(i + 2, end);
    const Tags section = all.subspan(body, end - body);
    if (name == "HEADER") {
      std::size_t k = 0;
      while (k < section.size()) {
        if (section[k].code != 9) {
          ++k;
          continue;
        }
        const std::string var = upper(section[k].text);
        std::size_t e = k + 1;
        while (e < section.size() && section[e].code != 9 && section[e].code != 0) ++e;
        doc.header[var] = section.subspan(k + 1, e - k - 1);
        k = e;
      }
    } else if (name == "TABLES") {
      for (const Record& r : split_records(section)) {
        if (r.type == "LAYER") {
          LayerInfo layer;
          const std::string_view raw = get_s(r.tags, 2);
          if (raw.empty()) continue;
          layer.name = std::string(raw);
          const auto color = get_i(r.tags, 62, 7);
          layer.off = color < 0;
          layer.color = static_cast<int>(std::clamp<std::int64_t>(color < 0 ? -color : color, 0, 256));
          if (const DxfTag* tc = find(r.tags, 420)) layer.rgb = static_cast<std::uint32_t>(tag_int(*tc)) & 0xFFFFFF;
          layer.frozen = (get_i(r.tags, 70, 0) & 1) != 0;
          layer.linetype = upper(get_s(r.tags, 6, "CONTINUOUS"));
          layer.lineweight = static_cast<int>(std::clamp<std::int64_t>(get_i(r.tags, 370, -3), -3, 211));
          const std::string key = upper(raw);
          if (!doc.layers.contains(key)) doc.layer_order.push_back(key);
          doc.layers[key] = std::move(layer);
        } else if (r.type == "LTYPE") {
          const std::string key = upper(get_s(r.tags, 2));
          if (!key.empty()) doc.linetypes[key] = linetype_pattern(r.tags);
        } else if (r.type == "STYLE") {
          const std::string key = upper(get_s(r.tags, 2));
          DxfTextStyle style;
          style.font = std::string(get_s(r.tags, 3));
          if (const DxfTag* family = find(r.tags, 1000)) style.font = std::string(family->text);
          style.fixed_height = get_d(r.tags, 40, 0.0);
          style.width_factor = get_d(r.tags, 41, 1.0);
          style.oblique = get_d(r.tags, 50, 0.0);
          const auto flags = get_i(r.tags, 1071, 0);
          style.bold = (flags & 0x2000000) != 0;
          style.italic = (flags & 0x1000000) != 0;
          if (!key.empty()) doc.styles[key] = std::move(style);
        }
      }
    } else if (name == "BLOCKS") {
      const std::vector<Record> records = split_records(section);
      std::size_t k = 0;
      while (k < records.size()) {
        if (records[k].type != "BLOCK") {
          ++k;
          continue;
        }
        BlockDef block;
        block.name = std::string(get_s(records[k].tags, 2));
        block.base = get_p(records[k].tags, 10);
        block.xref = (get_i(records[k].tags, 70, 0) & 12) != 0;
        std::size_t e = k + 1;
        while (e < records.size() && records[e].type != "ENDBLK" && records[e].type != "BLOCK") ++e;
        block.entities = group_entities(std::span<const Record>(records).subspan(k + 1, e - k - 1));
        const std::string key = upper(block.name);
        if (!key.empty()) doc.blocks[key] = std::move(block);
        k = e;
      }
    } else if (name == "ENTITIES") {
      const std::vector<Record> records = split_records(section);
      std::vector<Entity> entities = group_entities(records);
      for (Entity& e : entities) doc.entities.push_back(std::move(e));
    }
    i = end + 1;
  }
  if (!any_section) {
    // Some writers emit a bare ENTITIES list without SECTION markers.
    const std::vector<Record> records = split_records(all);
    doc.entities = group_entities(records);
    if (doc.entities.empty()) return fail(Status::Decode, "no DXF sections or entities were found");
  }
  const auto var = [&](std::string_view key, int code) -> std::string_view {
    const auto it = doc.header.find(key);
    return it == doc.header.end() ? std::string_view{} : get_s(it->second, code);
  };
  doc.version = std::string(util::trim(var("$ACADVER", 1)));
  doc.strings.configure(doc.version, var("$DWGCODEPAGE", 3));
  (void)ctx;
  return doc;
}

// --------------------------------------------------------------------------- drawing

struct Props {
  Color color{0, 0, 0, 1};
  std::vector<double> dash;
  double dash_offset = 0.0;
  double width = 0.0;
  bool hairline = false;
  std::string layer;  // effective layer key
};

struct State {
  Affine3 xf;
  int depth = 0;
  Color byblock_color{0, 0, 0, 1};
  std::vector<double> byblock_dash;
  double byblock_width = -1.0;
  std::string insert_layer = "0";
  std::vector<std::string> stack;
};

Affine3 rotation_z(double radians) {
  const double c = std::cos(radians), s = std::sin(radians);
  return {{c, s, 0}, {-s, c, 0}, {0, 0, 1}, {0, 0, 0}};
}
Affine3 translation(Vec3 t) { return {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, t}; }
Affine3 scaling(double sx, double sy, double sz) { return {{sx, 0, 0}, {0, sy, 0}, {0, 0, sz}, {0, 0, 0}}; }
Matrix linear(const Matrix& m) { return {m.a, m.b, m.c, m.d, 0, 0}; }

class Drawer {
 public:
  Drawer(const Document& doc, ReadContext& ctx) : doc_(doc), ctx_(ctx) {
    const auto it = doc.header.find("$INSUNITS");
    insunits_ = it == doc.header.end() ? 0 : static_cast<int>(get_i(it->second, 70, 0));
    ltscale_ = header_d("$LTSCALE", 40, 1.0);
    if (!(ltscale_ > 0) || !std::isfinite(ltscale_)) ltscale_ = 1.0;
    lwdefault_ = static_cast<int>(header_i("$LWDEFAULT", 370, 25));
    if (lwdefault_ < 0 || lwdefault_ > 211) lwdefault_ = 25;
    dimasz_ = header_d("$DIMASZ", 40, 2.5) * std::max(header_d("$DIMSCALE", 40, 1.0), 1e-9);
  }

  [[nodiscard]] double units_to_px() const {
    switch (insunits_) {
      case 1: return px_per_in;
      case 2: return px_per_in * 12;
      case 3: return px_per_in * 63360;
      case 5: return px_per_mm * 10;
      case 6: return px_per_mm * 1000;
      case 7: return px_per_mm * 1e6;
      case 8: return px_per_in * 1e-6;
      case 9: return px_per_in * 1e-3;
      case 10: return px_per_in * 36;
      case 11: return px_per_mm * 1e-7;
      case 12: return px_per_mm * 1e-6;
      case 13: return px_per_mm * 1e-3;
      case 14: return px_per_mm * 100;
      case 15: return px_per_mm * 1e4;
      case 16: return px_per_mm * 1e5;
      case 17: return px_per_mm * 1e12;
      case 18: return px_per_mm * 1.495978707e14;
      case 21: return px_per_mm * 1000 * 1200.0 / 3937.0 * 1;
      default: return px_per_mm;
    }
  }
  [[nodiscard]] std::string units_name() const {
    switch (insunits_) {
      case 0: return "unitless";
      case 1: return "in";
      case 2: return "ft";
      case 4: return "mm";
      case 5: return "cm";
      case 6: return "m";
      default: return "mm";
    }
  }

  Result<void> draw_top(const Entity& e, bool paper, const std::set<std::string>& filter) {
    const bool in_paper = get_i(e.tags, 67, 0) == 1;
    if (in_paper != paper) return {};
    return draw_top_unchecked(e, filter);
  }

  Result<void> draw_top_unchecked(const Entity& e, const std::set<std::string>& filter) {
    std::string key = upper(get_s(e.tags, 8, "0"));
    if (key.empty()) key = "0";
    if (!filter.empty() && !filter.contains(key)) return {};
    Group& group = layer_group(key);
    State state;
    return draw(e, state, group.children);
  }

  [[nodiscard]] std::vector<Group> take_groups() {
    std::vector<Group> out;
    std::vector<std::size_t> order;
    for (const std::string& key : doc_.layer_order) {
      const auto it = group_index_.find(key);
      if (it != group_index_.end()) order.push_back(it->second);
    }
    for (std::size_t i = 0; i < groups_.size(); ++i) {
      if (std::find(order.begin(), order.end(), i) == order.end()) order.push_back(i);
    }
    for (const std::size_t i : order) out.push_back(std::move(groups_[i]));
    return out;
  }

  [[nodiscard]] const std::vector<std::vector<Vec3>>& faces() const { return faces_; }
  [[nodiscard]] const std::set<std::string>& skipped() const { return skipped_; }

 private:
  [[nodiscard]] double header_d(std::string_view key, int code, double fallback) const {
    const auto it = doc_.header.find(key);
    return it == doc_.header.end() ? fallback : get_d(it->second, code, fallback);
  }
  [[nodiscard]] std::int64_t header_i(std::string_view key, int code, std::int64_t fallback) const {
    const auto it = doc_.header.find(key);
    return it == doc_.header.end() ? fallback : get_i(it->second, code, fallback);
  }

  Group& layer_group(const std::string& key) {
    const auto it = group_index_.find(key);
    if (it != group_index_.end()) return groups_[it->second];
    Group g;
    g.layer = true;
    const LayerInfo* info = layer(key);
    g.layer_name = doc_.strings.decode(info != nullptr ? std::string_view(info->name) : std::string_view(key));
    g.visible = info == nullptr || (!info->off && !info->frozen);
    group_index_[key] = groups_.size();
    groups_.push_back(std::move(g));
    return groups_.back();
  }

  [[nodiscard]] const LayerInfo* layer(std::string_view key) const {
    const auto it = doc_.layers.find(key);
    return it == doc_.layers.end() ? nullptr : &it->second;
  }

  [[nodiscard]] double mm_to_units() const { return px_per_mm / units_to_px(); }

  [[nodiscard]] Color layer_color(const LayerInfo* info) const {
    if (info == nullptr) return aci_color(7);
    if (info->rgb) {
      const std::uint32_t rgb = *info->rgb;
      return Color::rgb8(static_cast<int>((rgb >> 16) & 0xFF), static_cast<int>((rgb >> 8) & 0xFF),
                         static_cast<int>(rgb & 0xFF));
    }
    return aci_color(info->color);
  }

  // DXF dash elements (>0 dash, <0 gap, 0 dot) -> an even dash array starting with a dash.
  void set_dash(std::span<const double> pattern, double scale, double dot, Props& props) const {
    props.dash.clear();
    props.dash_offset = 0;
    std::vector<std::pair<double, bool>> items;
    for (const double v : pattern) {
      if (!std::isfinite(v)) return;
      const bool on = v >= 0;
      double len = std::abs(v) * scale;
      if (v == 0) len = dot;
      if (!items.empty() && items.back().second == on) items.back().first += len;
      else items.push_back({len, on});
    }
    if (items.size() < 2) return;
    double offset = 0;
    if (!items.front().second) {
      offset = items.front().first;
      items.push_back(items.front());
      items.erase(items.begin());
      double total = 0;
      for (const auto& it : items) total += it.first;
      offset = total - offset;
    }
    if (items.size() % 2 == 1) {  // ends with a dash: merge it into the first one
      offset += items.back().first;
      items.front().first += items.back().first;
      items.pop_back();
    }
    double total = 0;
    for (const auto& it : items) total += it.first;
    if (!(total > 0) || items.size() < 2) return;
    for (const auto& it : items) props.dash.push_back(it.first);
    props.dash_offset = std::fmod(offset, total);
  }

  [[nodiscard]] Props resolve(Tags tags, const State& s) const {
    Props p;
    std::string key = upper(get_s(tags, 8, "0"));
    if (key.empty()) key = "0";
    if (key == "0" && s.depth > 0) key = s.insert_layer;
    p.layer = key;
    const LayerInfo* info = layer(key);
    // colour
    if (const DxfTag* tc = find(tags, 420)) {
      const auto rgb = static_cast<std::uint32_t>(tag_int(*tc)) & 0xFFFFFF;
      p.color = Color::rgb8(static_cast<int>((rgb >> 16) & 0xFF), static_cast<int>((rgb >> 8) & 0xFF),
                            static_cast<int>(rgb & 0xFF));
    } else {
      const auto aci = get_i(tags, 62, 256);
      if (aci == 0) p.color = s.byblock_color;
      else if (aci >= 256 || aci <= -256) p.color = layer_color(info);
      else p.color = aci_color(static_cast<int>(aci < 0 ? -aci : aci));
    }
    // transparency (440): 0x02000000 | alpha
    if (const DxfTag* tr = find(tags, 440)) {
      const auto v = static_cast<std::uint32_t>(tag_int(*tr));
      if ((v & 0x02000000u) != 0) p.color.a = static_cast<float>(v & 0xFF) / 255.0f;
    }
    // line weight
    int lw = static_cast<int>(std::clamp<std::int64_t>(get_i(tags, 370, -1), -3, 211));
    double width = -1;
    if (lw == -1) lw = info != nullptr ? info->lineweight : -3;
    if (lw == -2) width = s.byblock_width;
    if (lw == -3 || (lw == -2 && width < 0)) lw = lwdefault_;
    if (width < 0) {
      if (lw <= 0) {
        p.hairline = true;
        width = 0.05 * mm_to_units();
      } else {
        width = lw / 100.0 * mm_to_units();
      }
    }
    p.width = width;
    // line type
    std::string lt = upper(get_s(tags, 6, "BYLAYER"));
    if (lt == "BYLAYER") lt = info != nullptr ? info->linetype : "CONTINUOUS";
    if (lt == "BYBLOCK") {
      p.dash = s.byblock_dash;
    } else if (lt != "CONTINUOUS") {
      const auto it = doc_.linetypes.find(lt);
      if (it != doc_.linetypes.end() && !it->second.empty()) {
        const double scale = ltscale_ * std::max(get_d(tags, 48, 1.0), 1e-9);
        set_dash(it->second, scale, std::max(width, 0.2 * mm_to_units()), p);
      }
    }
    return p;
  }

  [[nodiscard]] Stroke stroke_of(const Props& p, std::optional<double> width = std::nullopt) const {
    Stroke st;
    st.paint = Paint::solid(p.color);
    st.width = width ? *width : p.width;
    st.hairline = p.hairline && !width;
    st.cap = LineCap::Round;
    st.join = LineJoin::Round;
    st.dash = p.dash;
    st.dash_offset = p.dash_offset;
    return st;
  }

  Result<void> count(const Path& path) {
    points_ += path.points.size() + 1;
    if (points_ > max_points) {
      return fail(Status::Limit, "the DXF drawing expands to more than {} path points", max_points);
    }
    if ((++nodes_ & 0x3FFF) == 0) CVT_TRY(ctx_.checkpoint(0.5));
    return {};
  }

  Result<void> emit_stroke(Path path, const Props& p, std::vector<Node>& out,
                           std::optional<double> width = std::nullopt) {
    if (path.empty()) return {};
    CVT_TRY(count(path));
    Shape shape;
    shape.path = std::move(path);
    shape.stroke = stroke_of(p, width);
    out.push_back(Node{std::move(shape)});
    return {};
  }

  Result<void> emit_fill(Path path, Paint paint, std::vector<Node>& out, FillRule rule = FillRule::NonZero) {
    if (path.empty()) return {};
    CVT_TRY(count(path));
    Shape shape;
    shape.path = std::move(path);
    shape.fill = std::move(paint);
    shape.fill_rule = rule;
    out.push_back(Node{std::move(shape)});
    return {};
  }

  [[nodiscard]] static Matrix plane(const State& s, Tags tags, double elevation) {
    const Vec3 ext = get_p(tags, 210, {0, 0, 1});
    return (s.xf * ocs_to_wcs(finite(ext) ? ext : Vec3{0, 0, 1})).plane(elevation);
  }

  // ------------------------------------------------------------------ entity dispatch

  Result<void> draw(const Entity& e, const State& s, std::vector<Node>& out) {
    const std::string& type = e.type;
    if (s.depth > 0) {
      // Entities on their own (non-0) layer inside blocks follow that layer's visibility.
      std::string key = upper(get_s(e.tags, 8, "0"));
      if (!key.empty() && key != "0") {
        const LayerInfo* info = layer(key);
        if (info != nullptr && (info->off || info->frozen)) return {};
      }
    }
    if ((get_i(e.tags, 60, 0) & 1) != 0) return {};  // invisible entity
    if (type == "LINE") return draw_line(e, s, out);
    if (type == "POINT") return draw_point(e, s, out);
    if (type == "CIRCLE" || type == "ARC") return draw_circle(e, s, out);
    if (type == "ELLIPSE") return draw_ellipse(e, s, out);
    if (type == "LWPOLYLINE") return draw_lwpolyline(e, s, out);
    if (type == "POLYLINE") return draw_polyline(e, s, out);
    if (type == "SPLINE") return draw_spline(e, s, out);
    if (type == "TEXT") return draw_text(e.tags, s, out, false);
    if (type == "ATTRIB") return draw_text(e.tags, s, out, true);
    if (type == "MTEXT") return draw_mtext(e.tags, s, out);
    if (type == "INSERT") return draw_insert(e, s, out);
    if (type == "DIMENSION" || type == "ACAD_TABLE") return draw_block_ref(e, s, out);
    if (type == "LEADER") return draw_leader(e, s, out);
    if (type == "SOLID" || type == "TRACE") return draw_solid(e, s, out);
    if (type == "3DFACE") return draw_3dface(e, s, out);
    if (type == "HATCH") return draw_hatch(e, s, out);
    if (type == "MESH") return draw_mesh(e, s, out);
    if (type == "ATTDEF" || type == "VIEWPORT" || type == "SEQEND" || type == "VERTEX") return {};
    skipped_.insert(type);
    return {};
  }

  Result<void> draw_line(const Entity& e, const State& s, std::vector<Node>& out) {
    const Vec3 a = get_p(e.tags, 10), b = get_p(e.tags, 11);
    if (!finite(a) || !finite(b)) return {};
    Path path;
    path.move_to(xy(s.xf.apply(a)));
    path.line_to(xy(s.xf.apply(b)));
    return emit_stroke(std::move(path), resolve(e.tags, s), out);
  }

  Result<void> draw_point(const Entity& e, const State& s, std::vector<Node>& out) {
    const Vec3 a = get_p(e.tags, 10);
    if (!finite(a)) return {};
    const Props p = resolve(e.tags, s);
    const Point c = xy(s.xf.apply(a));
    const double r = std::max(p.width, 0.25 * mm_to_units()) / 2;
    Path path;
    add_ellipse(path, c.x, c.y, r, r);
    return emit_fill(std::move(path), Paint::solid(p.color), out);
  }

  Result<void> draw_circle(const Entity& e, const State& s, std::vector<Node>& out) {
    const Vec3 c = get_p(e.tags, 10);
    const double r = get_d(e.tags, 40);
    if (!finite(c) || !(r > 0) || !std::isfinite(r)) return {};
    Path local;
    if (e.type == "CIRCLE") {
      add_ellipse(local, c.x, c.y, r, r);
    } else {
      double a0 = get_d(e.tags, 50) * deg, a1 = get_d(e.tags, 51) * deg;
      if (!std::isfinite(a0) || !std::isfinite(a1)) return {};
      a0 = std::fmod(a0, 2 * pi);
      a1 = std::fmod(a1, 2 * pi);
      if (a1 <= a0 + 1e-12) a1 += 2 * pi;
      add_arc(local, {c.x, c.y}, r, r, 0.0, a0, a1, true, false);
    }
    const Props p = resolve(e.tags, s);
    return emit_stroke(transformed(local, plane(s, e.tags, c.z)), p, out);
  }

  Result<void> draw_ellipse(const Entity& e, const State& s, std::vector<Node>& out) {
    const Vec3 c = get_p(e.tags, 10), major = get_p(e.tags, 11);
    const double ratio = get_d(e.tags, 40, 1.0);
    const Vec3 ext = get_p(e.tags, 210, {0, 0, 1});
    if (!finite(c) || !finite(major) || !finite(ext) || !(ratio > 0) || !std::isfinite(ratio)) return {};
    const Vec3 minor = cross3(normalized3(ext), major) * ratio;
    const Point C = xy(s.xf.apply(c)), M = xy(s.xf.apply_vector(major)), N = xy(s.xf.apply_vector(minor));
    double t0 = get_d(e.tags, 41, 0.0), t1 = get_d(e.tags, 42, 2 * pi);
    if (!std::isfinite(t0) || !std::isfinite(t1)) return {};
    Path unit;
    if (std::abs(t1 - t0) >= 2 * pi - 1e-9 || std::abs(t1 - t0) < 1e-12) {
      add_ellipse(unit, 0, 0, 1, 1);
    } else {
      if (t1 < t0) t1 += 2 * pi;
      add_arc(unit, {0, 0}, 1, 1, 0, t0, t1, true, false);
    }
    return emit_stroke(transformed(unit, Matrix{M.x, M.y, N.x, N.y, C.x, C.y}), resolve(e.tags, s), out);
  }

  Result<void> draw_lwpolyline(const Entity& e, const State& s, std::vector<Node>& out) {
    struct V {
      Point p;
      double bulge = 0;
    };
    std::vector<V> vertices;
    for (const DxfTag& t : e.tags) {
      if (t.code == 10) vertices.push_back({{tag_double(t), 0}, 0});
      else if (t.code == 20 && !vertices.empty()) vertices.back().p.y = tag_double(t);
      else if (t.code == 42 && !vertices.empty()) vertices.back().bulge = tag_double(t);
    }
    if (vertices.empty()) return {};
    const bool closed = (get_i(e.tags, 70, 0) & 1) != 0;
    Path local;
    local.move_to(vertices[0].p);
    for (std::size_t i = 1; i < vertices.size(); ++i) bulge_to(local, vertices[i - 1].p, vertices[i].p, vertices[i - 1].bulge);
    if (closed && vertices.size() > 1) {
      bulge_to(local, vertices.back().p, vertices.front().p, vertices.back().bulge);
      local.close();
    }
    const Matrix m = plane(s, e.tags, get_d(e.tags, 38, 0.0));
    const double cw = get_d(e.tags, 43, 0.0);
    const Props p = resolve(e.tags, s);
    if (cw > 0 && std::isfinite(cw)) return emit_stroke(transformed(local, m), p, out, cw * m.mean_scale());
    if (vertices.size() == 1) return draw_dot(vertices[0].p, m, p, out);
    return emit_stroke(transformed(local, m), p, out);
  }

  Result<void> draw_dot(Point local, const Matrix& m, const Props& p, std::vector<Node>& out) {
    const Point c = m.apply(local);
    Path path;
    const double r = std::max(p.width, 0.25 * mm_to_units()) / 2;
    add_ellipse(path, c.x, c.y, r, r);
    return emit_fill(std::move(path), Paint::solid(p.color), out);
  }

  Result<void> draw_polyline(const Entity& e, const State& s, std::vector<Node>& out) {
    const auto flags = get_i(e.tags, 70, 0);
    if ((flags & 64) != 0) return draw_polyface(e, s, out);
    if ((flags & 16) != 0) return draw_polymesh(e, s, out);
    const bool closed = (flags & 1) != 0;
    const bool spline_fit = (flags & 4) != 0;
    const bool is3d = (flags & 8) != 0;
    struct V {
      Vec3 p;
      double bulge = 0;
    };
    std::vector<V> vertices;
    for (const Entity& v : e.children) {
      const auto vflags = get_i(v.tags, 70, 0);
      if (spline_fit && (vflags & 16) != 0) continue;  // spline frame control point
      const Vec3 p = get_p(v.tags, 10);
      if (!finite(p)) continue;
      vertices.push_back({p, is3d ? 0.0 : get_d(v.tags, 42, 0.0)});
    }
    if (vertices.empty()) return {};
    const Props p = resolve(e.tags, s);
    Path path;
    if (is3d) {
      path.move_to(xy(s.xf.apply(vertices[0].p)));
      for (std::size_t i = 1; i < vertices.size(); ++i) path.line_to(xy(s.xf.apply(vertices[i].p)));
      if (closed) path.close();
      return emit_stroke(std::move(path), p, out);
    }
    path.move_to(xy(vertices[0].p));
    for (std::size_t i = 1; i < vertices.size(); ++i) bulge_to(path, xy(vertices[i - 1].p), xy(vertices[i].p), vertices[i - 1].bulge);
    if (closed && vertices.size() > 1) {
      bulge_to(path, xy(vertices.back().p), xy(vertices.front().p), vertices.back().bulge);
      path.close();
    }
    const double elevation = get_p(e.tags, 10).z;
    const Matrix m = plane(s, e.tags, std::isfinite(elevation) ? elevation : 0.0);
    const double w0 = get_d(e.tags, 40, 0.0), w1 = get_d(e.tags, 41, 0.0);
    const double w = std::max(w0, w1);
    if (vertices.size() == 1) return draw_dot(xy(vertices[0].p), m, p, out);
    if (w > 0 && std::isfinite(w)) return emit_stroke(transformed(path, m), p, out, w * m.mean_scale());
    return emit_stroke(transformed(path, m), p, out);
  }

  Result<void> add_face(std::vector<Vec3> face, const State& s, const Props& p, std::vector<Node>& out,
                        std::uint32_t invisible_edges = 0) {
    // Drop repeated corners (a triangle stored as a quad).
    std::vector<Vec3> clean;
    for (const Vec3& v : face) {
      if (!finite(v)) return {};
      const Vec3 w = s.xf.apply(v);
      if (clean.empty() || std::abs(w.x - clean.back().x) + std::abs(w.y - clean.back().y) + std::abs(w.z - clean.back().z) > 1e-12) {
        clean.push_back(w);
      }
    }
    while (clean.size() > 1 && std::abs(clean.front().x - clean.back().x) + std::abs(clean.front().y - clean.back().y) +
                                       std::abs(clean.front().z - clean.back().z) <= 1e-12) {
      clean.pop_back();
    }
    if (clean.size() < 3) return {};
    if (faces_.size() >= 5'000'000) return fail(Status::Limit, "the DXF has more than 5000000 3D faces");
    Path path;
    bool pen = false;
    for (std::size_t i = 0; i < clean.size(); ++i) {
      const Point a = xy(clean[i]), b = xy(clean[(i + 1) % clean.size()]);
      if (i < 32 && (invisible_edges & (1u << i)) != 0) {
        pen = false;
        continue;
      }
      if (!pen) path.move_to(a);
      path.line_to(b);
      pen = true;
    }
    faces_.push_back(std::move(clean));
    return emit_stroke(std::move(path), p, out);
  }

  Result<void> draw_3dface(const Entity& e, const State& s, std::vector<Node>& out) {
    std::vector<Vec3> face = {get_p(e.tags, 10), get_p(e.tags, 11), get_p(e.tags, 12), get_p(e.tags, 13, get_p(e.tags, 12))};
    const auto invisible = static_cast<std::uint32_t>(get_i(e.tags, 70, 0) & 15);
    return add_face(std::move(face), s, resolve(e.tags, s), out, invisible);
  }

  Result<void> draw_polyface(const Entity& e, const State& s, std::vector<Node>& out) {
    std::vector<Vec3> positions;
    const Props p = resolve(e.tags, s);
    for (const Entity& v : e.children) {
      const auto vflags = get_i(v.tags, 70, 0);
      if ((vflags & 128) != 0 && (vflags & 64) != 0) {
        positions.push_back(get_p(v.tags, 10));
      } else if ((vflags & 128) != 0) {
        std::vector<Vec3> face;
        std::uint32_t invisible = 0;
        for (int k = 0; k < 4; ++k) {
          const auto index = get_i(v.tags, 71 + k, 0);
          if (index == 0) break;
          const std::int64_t i = (index < 0 ? -index : index) - 1;
          if (i < 0 || static_cast<std::uint64_t>(i) >= positions.size()) {
            face.clear();
            break;
          }
          if (index < 0) invisible |= 1u << k;
          face.push_back(positions[static_cast<std::size_t>(i)]);
        }
        if (face.size() >= 3) CVT_TRY(add_face(std::move(face), s, p, out, invisible));
      }
    }
    return {};
  }

  Result<void> draw_polymesh(const Entity& e, const State& s, std::vector<Node>& out) {
    const auto flags = get_i(e.tags, 70, 0);
    const auto m = get_i(e.tags, 71, 0), n = get_i(e.tags, 72, 0);
    std::vector<Vec3> grid;
    for (const Entity& v : e.children) grid.push_back(get_p(v.tags, 10));
    if (m < 2 || n < 2 || static_cast<std::uint64_t>(m) * static_cast<std::uint64_t>(n) > grid.size()) return {};
    const bool closed_m = (flags & 1) != 0, closed_n = (flags & 32) != 0;
    const Props p = resolve(e.tags, s);
    const auto at = [&](std::int64_t i, std::int64_t j) { return grid[static_cast<std::size_t>((i % m) * n + (j % n))]; };
    for (std::int64_t i = 0; i < (closed_m ? m : m - 1); ++i) {
      for (std::int64_t j = 0; j < (closed_n ? n : n - 1); ++j) {
        CVT_TRY(add_face({at(i, j), at(i, j + 1), at(i + 1, j + 1), at(i + 1, j)}, s, p, out));
      }
    }
    return {};
  }

  Result<void> draw_mesh(const Entity& e, const State& s, std::vector<Node>& out) {
    // AcDbSubDMesh: 92 vertex count, 10/20/30 vertices, 93 face list size, 90 entries.
    std::vector<Vec3> vertices;
    std::vector<std::int64_t> list;
    std::size_t i = 0;
    const Tags t = e.tags;
    while (i < t.size() && t[i].code != 92) ++i;
    if (i >= t.size()) return {};
    const auto vcount = tag_int(t[i++]);
    while (i < t.size() && t[i].code == 10 && static_cast<std::int64_t>(vertices.size()) < vcount) {
      Vec3 v{tag_double(t[i]), 0, 0};
      if (i + 1 < t.size() && t[i + 1].code == 20) v.y = tag_double(t[i + 1]);
      if (i + 2 < t.size() && t[i + 2].code == 30) v.z = tag_double(t[i + 2]);
      vertices.push_back(v);
      i += 3;
    }
    while (i < t.size() && t[i].code != 93) ++i;
    if (i >= t.size()) return {};
    const auto lcount = tag_int(t[i++]);
    while (i < t.size() && t[i].code == 90 && static_cast<std::int64_t>(list.size()) < lcount) list.push_back(tag_int(t[i++]));
    const Props p = resolve(e.tags, s);
    std::size_t k = 0;
    while (k < list.size()) {
      const auto count = list[k++];
      if (count < 3 || count > 1024 || k + static_cast<std::size_t>(count) > list.size()) break;
      std::vector<Vec3> face;
      for (std::int64_t j = 0; j < count; ++j) {
        const auto index = list[k++];
        if (index < 0 || static_cast<std::uint64_t>(index) >= vertices.size()) {
          face.clear();
          k += static_cast<std::size_t>(count - j - 1);
          break;
        }
        face.push_back(vertices[static_cast<std::size_t>(index)]);
      }
      if (face.size() >= 3) CVT_TRY(add_face(std::move(face), s, p, out));
    }
    return {};
  }

  Result<void> draw_spline(const Entity& e, const State& s, std::vector<Node>& out) {
    Nurbs curve;
    curve.degree = static_cast<int>(std::clamp<std::int64_t>(get_i(e.tags, 71, 3), 1, 11));
    const auto flags = get_i(e.tags, 70, 0);
    std::vector<Point> fit;
    for (const DxfTag& t : e.tags) {
      if (t.code == 40 && curve.knots.size() < 1'000'000) curve.knots.push_back(tag_double(t));
      else if (t.code == 41 && curve.weights.size() < 1'000'000) curve.weights.push_back(tag_double(t));
    }
    // Control and fit points: x on 10/11, then y, z.
    std::vector<Vec3> cps, fps;
    for (std::size_t i = 0; i < e.tags.size(); ++i) {
      const DxfTag& t = e.tags[i];
      if (t.code == 10 && cps.size() < 1'000'000) cps.push_back({tag_double(t), 0, 0});
      else if (t.code == 20 && !cps.empty()) cps.back().y = tag_double(t);
      else if (t.code == 30 && !cps.empty()) cps.back().z = tag_double(t);
      else if (t.code == 11 && fps.size() < 1'000'000) fps.push_back({tag_double(t), 0, 0});
      else if (t.code == 21 && !fps.empty()) fps.back().y = tag_double(t);
      else if (t.code == 31 && !fps.empty()) fps.back().z = tag_double(t);
    }
    for (const Vec3& c : cps) curve.control.push_back(xy(s.xf.apply(c)));
    for (const Vec3& f : fps) fit.push_back(xy(s.xf.apply(f)));
    if (curve.weights.size() != curve.control.size()) curve.weights.clear();
    const std::size_t n = curve.control.size();
    const auto p = static_cast<std::size_t>(curve.degree);
    if (n > p && curve.knots.size() != n + p + 1) {
      // Missing or inconsistent knots: clamped uniform.
      curve.knots.clear();
      for (std::size_t i = 0; i < n + p + 1; ++i) {
        curve.knots.push_back(i <= p ? 0.0 : i >= n ? static_cast<double>(n - p) : static_cast<double>(i - p));
      }
    }
    Path path;
    const Props props = resolve(e.tags, s);
    const double tolerance = std::max(props.width * 0.05, 1e-6);
    bool ok = n > p && append_nurbs(path, curve, tolerance, false, max_points / 4);
    if (!ok && fit.size() >= 2) {
      std::optional<Point> t0, t1;
      if (find(e.tags, 12) != nullptr) t0 = xy(s.xf.apply_vector(get_p(e.tags, 12)));
      if (find(e.tags, 13) != nullptr) t1 = xy(s.xf.apply_vector(get_p(e.tags, 13)));
      path.clear();
      ok = append_fit_spline(path, fit, t0, t1, false);
    }
    if (!ok) {
      // Unusable definition: fall back to the control polygon.
      path.clear();
      const std::vector<Point>& pts = curve.control.empty() ? fit : curve.control;
      for (std::size_t i = 0; i < pts.size(); ++i) {
        if (i == 0) path.move_to(pts[i]);
        else path.line_to(pts[i]);
      }
    }
    if ((flags & 1) != 0 && !path.empty()) path.close();
    return emit_stroke(std::move(path), props, out);
  }

  // ------------------------------------------------------------------ text

  [[nodiscard]] const DxfTextStyle* style_of(Tags tags) const {
    const auto it = doc_.styles.find(upper(get_s(tags, 7, "STANDARD")));
    return it == doc_.styles.end() ? nullptr : &it->second;
  }

  [[nodiscard]] static FontSpec font_for(std::string_view font_name, double cap_height, bool bold, bool italic) {
    FontSpec font;
    std::string name(font_name);
    if (const auto dot = name.rfind('.'); dot != std::string::npos) name.resize(dot);
    const std::string lower = util::to_lower(name);
    font.family = lower.find("mono") != std::string::npos || lower.find("cour") != std::string::npos ? "monospace"
                  : lower.empty() || lower == "txt" || lower.starts_with("roman") || lower.starts_with("simplex") ||
                          lower == "isocp" || lower == "iso"
                      ? "sans-serif"
                      : name;
    font.size = cap_height / cap_height_ratio;
    font.weight = bold ? 700 : 400;
    font.italic = italic;
    return font;
  }

  Result<void> emit_text(std::string_view content, const FontSpec& font, Point origin, const Matrix& placement,
                         Color color, std::vector<Node>& out) {
    if (content.empty() || !(font.size > 0) || !std::isfinite(font.size)) return {};
    Text text = make_text(content, font, origin, Paint::solid(color));
    CVT_TRY(count(text.outline));
    Group g;
    g.transform = placement;
    g.children.push_back(Node{std::move(text)});
    out.push_back(Node{std::move(g)});
    return {};
  }

  Result<void> draw_text(Tags tags, const State& s, std::vector<Node>& out, bool attrib) {
    if (attrib && (get_i(tags, 70, 0) & 1) != 0) return {};
    const std::string content = expand_percent_codes(doc_.strings.decode(get_s(tags, 1)));
    if (util::trim(content).empty()) return {};
    const DxfTextStyle* style = style_of(tags);
    double height = get_d(tags, 40, 0.0);
    if (!(height > 0) && style != nullptr) height = style->fixed_height;
    if (!(height > 0) || !std::isfinite(height)) height = 2.5;
    double wf = get_d(tags, 41, style != nullptr ? style->width_factor : 1.0);
    if (!(wf > 0) || !std::isfinite(wf)) wf = 1.0;
    const double oblique = get_d(tags, 51, style != nullptr ? style->oblique : 0.0);
    double rotation = get_d(tags, 50, 0.0) * deg;
    const auto gen = get_i(tags, 71, 0);
    const auto h = attrib ? get_i(tags, 72, 0) : get_i(tags, 72, 0);
    const auto v = attrib ? get_i(tags, 74, 0) : get_i(tags, 73, 0);
    const Vec3 p1 = get_p(tags, 10);
    const Vec3 p2 = find(tags, 11) != nullptr ? get_p(tags, 11) : p1;
    if (!finite(p1) || !finite(p2)) return {};
    const FontSpec font = font_for(style != nullptr ? std::string_view(style->font) : std::string_view{}, height,
                                   style != nullptr && style->bold, style != nullptr && style->italic);
    const TextLayout layout = layout_text(content, font);
    double width = layout.advance * wf;
    double sx = wf, sy = 1.0;
    Point anchor = xy(p1);
    double dx = 0, baseline = 0;  // offsets in y-up text space
    if (h != 0 || v != 0) anchor = xy(p2);
    if (h == 3 || h == 5) {  // aligned / fit between the two points
      const Point d = xy(p2) - xy(p1);
      const double target = length(d);
      if (target > 1e-12 && layout.advance > 0) {
        rotation = std::atan2(d.y, d.x);
        const double k = target / layout.advance;
        sx = k;
        if (h == 3) sy = k;  // aligned: the height follows; fit: only the width
        width = target;
      }
      anchor = xy(p1);
    } else if (h == 1 || h == 4) {
      dx = -width / 2;
    } else if (h == 2) {
      dx = -width;
    }
    if (h == 4) baseline = -height / 2;
    else if (v == 1) baseline = layout.descent * (sy);
    else if (v == 2) baseline = -height / 2 * sy;
    else if (v == 3) baseline = -height * sy;
    const double mx = (gen & 2) != 0 ? -1.0 : 1.0, my = (gen & 4) != 0 ? -1.0 : 1.0;
    const Matrix up = plane(s, tags, p1.z) * Matrix::translate(anchor.x, anchor.y) * Matrix::rotate(rotation) *
                      Matrix::scale(mx, my) * Matrix::translate(dx, baseline) * Matrix::skew_x(oblique * deg) *
                      Matrix::scale(sx, sy) * Matrix::scale(1, -1);
    return emit_text(content, font, {0, 0}, up, resolve(tags, s).color, out);
  }

  Result<void> draw_mtext(Tags tags, const State& s, std::vector<Node>& out) {
    std::string raw;
    for (const DxfTag& t : tags) {
      if (t.code == 3) raw += get_s(std::span<const DxfTag>(&t, 1), 3);
    }
    raw += get_s(tags, 1);
    const std::string decoded = doc_.strings.decode(raw);
    const DxfTextStyle* style = style_of(tags);
    double height = get_d(tags, 40, 0.0);
    if (!(height > 0) && style != nullptr) height = style->fixed_height;
    if (!(height > 0) || !std::isfinite(height)) height = 2.5;
    const Props props = resolve(tags, s);
    MTextRun base;
    base.color = props.color;
    base.font = style != nullptr ? style->font : std::string{};
    base.bold = style != nullptr && style->bold;
    base.italic = style != nullptr && style->italic;
    const std::vector<MTextParagraph> paragraphs = parse_mtext(decoded, height, base);
    const double wrap = get_d(tags, 41, 0.0);
    const auto attachment = std::clamp<std::int64_t>(get_i(tags, 71, 1), 1, 9);
    const double spacing = std::clamp(get_d(tags, 44, 1.0), 0.25, 4.0);
    // Lay out lines: words wrapped at the reference width.
    struct Piece {
      std::string text;
      const MTextRun* run = nullptr;
      double width = 0;
    };
    struct Line {
      std::vector<Piece> pieces;
      double width = 0;
      double height = 0;
    };
    std::vector<Line> lines;
    for (const MTextParagraph& para : paragraphs) {
      lines.emplace_back();
      for (const MTextRun& run : para.runs) {
        const FontSpec font = font_for(run.font, run.height, run.bold, run.italic);
        std::size_t start = 0;
        while (start < run.text.size()) {
          std::size_t end = run.text.find(' ', start);
          end = end == std::string::npos ? run.text.size() : end + 1;
          std::string word = run.text.substr(start, end - start);
          start = end;
          const double w = layout_text(word, font).advance;
          Line& line = lines.back();
          if (wrap > 0 && line.width > 0 && line.width + layout_text(util::trim_right(word), font).advance > wrap) {
            lines.emplace_back();
          }
          Line& target = lines.back();
          if (!target.pieces.empty() && target.pieces.back().run == &run) {
            target.pieces.back().text += word;
            target.pieces.back().width += w;
          } else {
            target.pieces.push_back({std::move(word), &run, w});
          }
          target.width += w;
          target.height = std::max(target.height, run.height);
        }
        if (lines.back().height == 0) lines.back().height = run.height;
      }
      if (lines.back().height == 0) lines.back().height = height;
      if (lines.size() > 100000) return fail(Status::Limit, "MTEXT has too many lines");
    }
    // Vertical placement.
    std::vector<double> baselines;
    double y = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
      y -= i == 0 ? lines[i].height : lines[i].height * 5.0 / 3.0 * spacing;
      baselines.push_back(y);
    }
    const double total = lines.empty() ? 0 : -baselines.back() + height * 0.0;
    const auto row = (attachment - 1) / 3, col = (attachment - 1) % 3;
    const double shift = row == 0 ? 0 : row == 1 ? total / 2 : total;
    // Placement: WCS insertion point, x axis from 11/21/31 or the rotation.
    const Vec3 ins = get_p(tags, 10);
    if (!finite(ins)) return {};
    double rotation = get_d(tags, 50, 0.0) * deg;
    if (find(tags, 11) != nullptr) {
      const Vec3 xdir = get_p(tags, 11);
      if (finite(xdir) && std::hypot(xdir.x, xdir.y) > 1e-12) rotation = std::atan2(xdir.y, xdir.x);
    }
    const Matrix m = s.xf.plane(0);
    const Point origin = xy(s.xf.apply(ins));
    const Matrix placement = Matrix::translate(origin.x, origin.y) * linear(m) * Matrix::rotate(rotation);
    for (std::size_t i = 0; i < lines.size(); ++i) {
      const Line& line = lines[i];
      double x = col == 0 ? 0 : col == 1 ? -line.width / 2 : -line.width;
      for (const Piece& piece : line.pieces) {
        const MTextRun& run = *piece.run;
        const FontSpec font = font_for(run.font, run.height, run.bold, run.italic);
        const std::string_view text = util::trim_right(piece.text);
        if (!util::trim(text).empty()) {
          const Matrix at = placement * Matrix::translate(x, baselines[i] + shift) * Matrix::scale(1, -1);
          CVT_TRY(emit_text(text, font, {0, 0}, at, run.color.value_or(props.color), out));
          if (run.underline) {
            Path u;
            const double uy = baselines[i] + shift - run.height * 0.2;
            u.move_to({x, uy});
            u.line_to({x + piece.width, uy});
            CVT_TRY(emit_stroke(transformed(u, placement), props, out, run.height * 0.07));
          }
        }
        x += piece.width;
      }
    }
    return {};
  }

  // ------------------------------------------------------------------ blocks

  Result<void> expand_block(const BlockDef& block, const State& child, std::vector<Node>& out) {
    for (const Entity& be : block.entities) {
      CVT_TRY(draw(be, child, out));
    }
    return {};
  }

  [[nodiscard]] State child_state(const State& s, const Props& p, const std::string& block_key) const {
    State child;
    child.depth = s.depth + 1;
    child.byblock_color = p.color;
    child.byblock_dash = p.dash;
    child.byblock_width = p.width;
    child.insert_layer = p.layer;
    child.stack = s.stack;
    child.stack.push_back(block_key);
    return child;
  }

  const BlockDef* enter_block(std::string_view name, const State& s) {
    const std::string key = upper(name);
    const auto it = doc_.blocks.find(key);
    if (it == doc_.blocks.end()) {
      if (!key.empty()) ctx_.warn(std::format("DXF block '{}' is referenced but not defined.", doc_.strings.decode(name)));
      return nullptr;
    }
    if (it->second.xref) {
      ctx_.warn("Externally referenced (xref) blocks cannot be drawn and were skipped.");
      return nullptr;
    }
    if (s.depth + 1 > max_block_depth || std::find(s.stack.begin(), s.stack.end(), key) != s.stack.end()) {
      ctx_.warn("DXF blocks are nested too deeply or recursively; the innermost references were skipped.");
      return nullptr;
    }
    return &it->second;
  }

  Result<void> draw_insert(const Entity& e, const State& s, std::vector<Node>& out) {
    const std::string_view name = get_s(e.tags, 2);
    const BlockDef* block = enter_block(name, s);
    Group group;
    if (block != nullptr) {
      const Props p = resolve(e.tags, s);
      const Vec3 ins = get_p(e.tags, 10);
      double sx = get_d(e.tags, 41, 1.0), sy = get_d(e.tags, 42, 1.0), sz = get_d(e.tags, 43, 1.0);
      const double rot = get_d(e.tags, 50, 0.0) * deg;
      const auto cols = std::max<std::int64_t>(1, get_i(e.tags, 70, 1));
      const auto rows = std::max<std::int64_t>(1, get_i(e.tags, 71, 1));
      const double cs = get_d(e.tags, 44, 0.0), rs = get_d(e.tags, 45, 0.0);
      if (!finite(ins) || !std::isfinite(sx) || !std::isfinite(sy) || !std::isfinite(sz) || !std::isfinite(rot)) return {};
      if (sx == 0) sx = 1;
      if (sy == 0) sy = 1;
      if (sz == 0) sz = 1;
      const auto copies = static_cast<std::uint64_t>(cols) * static_cast<std::uint64_t>(rows);
      expansions_ += copies;
      // Block expansions count like container entries (default limit: one million).
      const std::uint64_t max_expansions = std::max<std::uint64_t>(ctx_.limits.max_archive_entries, 1000) * 10;
      if (copies > max_expansions || expansions_ > max_expansions) {
        return fail(Status::Limit, "DXF block references expand to more than {} copies", max_expansions);
      }
      const Vec3 ext = get_p(e.tags, 210, {0, 0, 1});
      const Affine3 outer = s.xf * ocs_to_wcs(finite(ext) ? ext : Vec3{0, 0, 1});
      const Affine3 tail = scaling(sx, sy, sz) * translation(block->base * -1.0);
      State child = child_state(s, p, upper(name));
      for (std::int64_t r = 0; r < rows; ++r) {
        for (std::int64_t c = 0; c < cols; ++c) {
          child.xf = outer * translation(ins) * rotation_z(rot) *
                     translation({static_cast<double>(c) * cs, static_cast<double>(r) * rs, 0}) * tail;
          CVT_TRY(expand_block(*block, child, group.children));
        }
      }
    }
    for (const Entity& attrib : e.children) CVT_TRY(draw_text(attrib.tags, s, group.children, true));
    if (!group.children.empty()) out.push_back(Node{std::move(group)});
    return {};
  }

  Result<void> draw_block_ref(const Entity& e, const State& s, std::vector<Node>& out) {
    const BlockDef* block = enter_block(get_s(e.tags, 2), s);
    if (block == nullptr) return {};
    const Props p = resolve(e.tags, s);
    State child = child_state(s, p, upper(get_s(e.tags, 2)));
    child.xf = s.xf;
    if (e.type == "ACAD_TABLE") {
      const Vec3 ins = get_p(e.tags, 10);
      if (finite(ins)) child.xf = s.xf * translation(ins);
    }
    Group group;
    CVT_TRY(expand_block(*block, child, group.children));
    if (!group.children.empty()) out.push_back(Node{std::move(group)});
    return {};
  }

  Result<void> draw_leader(const Entity& e, const State& s, std::vector<Node>& out) {
    std::vector<Point> pts;
    for (std::size_t i = 0; i < e.tags.size(); ++i) {
      if (e.tags[i].code == 10) {
        Vec3 v{tag_double(e.tags[i]), 0, 0};
        if (i + 1 < e.tags.size() && e.tags[i + 1].code == 20) v.y = tag_double(e.tags[i + 1]);
        if (i + 2 < e.tags.size() && e.tags[i + 2].code == 30) v.z = tag_double(e.tags[i + 2]);
        if (finite(v)) pts.push_back(xy(s.xf.apply(v)));
      }
    }
    if (pts.size() < 2) return {};
    const Props p = resolve(e.tags, s);
    Path path;
    path.move_to(pts[0]);
    for (std::size_t i = 1; i < pts.size(); ++i) path.line_to(pts[i]);
    CVT_TRY(emit_stroke(std::move(path), p, out));
    if (get_i(e.tags, 71, 1) != 0) {
      const Point d = pts[1] - pts[0];
      const double len = length(d);
      const double size = dimasz_ * s.xf.plane(0).mean_scale();
      if (len > 1e-12 && size > 0 && std::isfinite(size)) {
        const Point u = d * (1.0 / len), n{-u.y, u.x};
        Path arrow;
        arrow.move_to(pts[0]);
        arrow.line_to(pts[0] + u * size + n * (size / 6));
        arrow.line_to(pts[0] + u * size - n * (size / 6));
        arrow.close();
        CVT_TRY(emit_fill(std::move(arrow), Paint::solid(p.color), out));
      }
    }
    return {};
  }

  Result<void> draw_solid(const Entity& e, const State& s, std::vector<Node>& out) {
    const Vec3 a = get_p(e.tags, 10), b = get_p(e.tags, 11), c = get_p(e.tags, 12);
    const Vec3 d = find(e.tags, 13) != nullptr ? get_p(e.tags, 13) : c;
    if (!finite(a) || !finite(b) || !finite(c) || !finite(d)) return {};
    Path local;
    local.move_to(xy(a));
    local.line_to(xy(b));
    local.line_to(xy(d));
    local.line_to(xy(c));
    local.close();
    const Props p = resolve(e.tags, s);
    return emit_fill(transformed(local, plane(s, e.tags, a.z)), Paint::solid(p.color), out);
  }

  // ------------------------------------------------------------------ hatch

  struct TagCursor {
    Tags tags;
    std::size_t pos = 0;
    bool ok = true;
    [[nodiscard]] bool at(int code) const { return pos < tags.size() && tags[pos].code == code; }
    double need(int code) {
      if (!at(code)) {
        ok = false;
        return 0.0;
      }
      return tag_double(tags[pos++]);
    }
    std::int64_t need_i(int code) {
      if (!at(code)) {
        ok = false;
        return 0;
      }
      return tag_int(tags[pos++]);
    }
    std::optional<double> maybe(int code) {
      if (!at(code)) return std::nullopt;
      return tag_double(tags[pos++]);
    }
  };

  static void start_or_connect(Path& path, Point p, bool& open) {
    if (!open) {
      path.move_to(p);
      open = true;
    } else if (length(current_point(path) - p) > 1e-9) {
      path.line_to(p);
    }
  }

  // One boundary path of a HATCH; appends a closed subpath in OCS.
  bool read_boundary(TagCursor& c, Path& path) {
    const auto flags = c.need_i(92);
    if (!c.ok) return false;
    bool open = false;
    if ((flags & 2) != 0) {
      const bool has_bulge = c.need_i(72) != 0;
      (void)c.need_i(73);
      const auto n = c.need_i(93);
      if (!c.ok || n < 0 || n > 10'000'000) return false;
      std::vector<std::pair<Point, double>> v;
      for (std::int64_t i = 0; i < n && c.ok; ++i) {
        Point p{c.need(10), c.need(20)};
        const double b = has_bulge ? c.maybe(42).value_or(0.0) : c.maybe(42).value_or(0.0);
        v.push_back({p, b});
      }
      if (!c.ok) return false;
      if (!v.empty()) {
        path.move_to(v[0].first);
        for (std::size_t i = 1; i < v.size(); ++i) bulge_to(path, v[i - 1].first, v[i].first, v[i - 1].second);
        if (v.size() > 1) bulge_to(path, v.back().first, v.front().first, v.back().second);
        path.close();
      }
    } else {
      const auto edges = c.need_i(93);
      if (!c.ok || edges < 0 || edges > 10'000'000) return false;
      for (std::int64_t i = 0; i < edges && c.ok; ++i) {
        const auto type = c.need_i(72);
        if (type == 1) {
          const Point a{c.need(10), c.need(20)};
          const Point b{c.need(11), c.need(21)};
          start_or_connect(path, a, open);
          path.line_to(b);
        } else if (type == 2 || type == 3) {
          const Point center{c.need(10), c.need(20)};
          Point major{1, 0};
          if (type == 3) major = {c.need(11), c.need(21)};
          const double r = c.need(40);
          const double a0 = c.need(50) * deg, a1 = c.need(51) * deg;
          const bool ccw = c.need_i(73) != 0;
          if (!c.ok) return false;
          double rx = r, ry = r, rot = 0;
          if (type == 3) {
            rx = length(major);
            ry = rx * r;
            rot = std::atan2(major.y, major.x);
          }
          if (!(rx > 0) || !(ry > 0) || !std::isfinite(rx) || !std::isfinite(ry)) continue;
          double s0 = ccw ? a0 : -a0, s1 = ccw ? a1 : -a1;
          if (ccw && s1 <= s0 + 1e-12) s1 += 2 * pi;
          if (!ccw && s1 >= s0 - 1e-12) s1 -= 2 * pi;
          const bool was_open = open;
          add_arc(path, center, rx, ry, rot, s0, s1, ccw, was_open);
          open = true;
        } else if (type == 4) {
          Nurbs curve;
          curve.degree = static_cast<int>(std::clamp<std::int64_t>(c.need_i(94), 1, 11));
          const bool rational = c.need_i(73) != 0;
          (void)c.need_i(74);
          const auto nk = c.need_i(95), nc = c.need_i(96);
          if (!c.ok || nk < 0 || nc < 0 || nk > 1'000'000 || nc > 1'000'000) return false;
          for (std::int64_t k = 0; k < nk && c.ok; ++k) curve.knots.push_back(c.need(40));
          for (std::int64_t k = 0; k < nc && c.ok; ++k) {
            curve.control.push_back({c.need(10), c.need(20)});
            if (const auto w = c.maybe(42)) curve.weights.push_back(*w);
          }
          if (!rational || curve.weights.size() != curve.control.size()) curve.weights.clear();
          std::vector<Point> fit;
          if (const auto nf = c.maybe(97)) {
            for (std::int64_t k = 0; k < static_cast<std::int64_t>(*nf) && k < 1'000'000 && c.ok; ++k) {
              fit.push_back({c.need(11), c.need(21)});
            }
            if (c.at(12)) (void)c.need(12), (void)c.need(22);
            if (c.at(13)) (void)c.need(13), (void)c.need(23);
          }
          if (!c.ok) return false;
          Path piece;
          if (!append_nurbs(piece, curve, 1e-3, false, 100000) &&
              !(fit.size() >= 2 && append_fit_spline(piece, fit, std::nullopt, std::nullopt, false))) {
            continue;
          }
          if (!piece.points.empty()) {
            start_or_connect(path, piece.points.front(), open);
            for (std::size_t v = 1; v < piece.verbs.size(); ++v) path.verbs.push_back(piece.verbs[v]);
            path.points.insert(path.points.end(), piece.points.begin() + 1, piece.points.end());
          }
        } else {
          return false;
        }
      }
      if (open) path.close();
    }
    // Source boundary object references.
    if (const auto n = c.maybe(97)) {
      for (std::int64_t i = 0; i < static_cast<std::int64_t>(*n) && c.at(330); ++i) ++c.pos;
    }
    return c.ok;
  }

  Result<void> hatch_lines(const Path& boundary, std::span<const std::vector<double>> families, const Props& p,
                           std::vector<Node>& out) {
    const Rect box = control_bounds(boundary);
    if (box.empty()) return {};
    const double diag = std::hypot(box.width(), box.height());
    const std::vector<Polyline> polys = flatten(boundary, std::max(diag / 4000, 1e-9));
    std::vector<std::pair<Point, Point>> edges;
    for (const Polyline& pl : polys) {
      for (std::size_t i = 0; i + 1 < pl.points.size(); ++i) edges.push_back({pl.points[i], pl.points[i + 1]});
      if (!pl.points.empty()) edges.push_back({pl.points.back(), pl.points.front()});
    }
    Path lines;
    std::uint64_t budget = 200'000;
    for (const std::vector<double>& f : families) {
      // f: angle(deg), base x, base y, offset x, offset y, dashes...
      if (f.size() < 5) continue;
      const double angle = f[0] * deg;
      const Point d{std::cos(angle), std::sin(angle)}, n{-d.y, d.x};
      const Point base{f[1], f[2]}, offset{f[3], f[4]};
      const double step = dot(offset, n);
      if (!(std::abs(step) > diag * 1e-5) || !std::isfinite(step)) continue;
      double lo = 1e300, hi = -1e300;
      for (const Point corner : {Point{box.x0, box.y0}, Point{box.x1, box.y0}, Point{box.x0, box.y1}, Point{box.x1, box.y1}}) {
        const double t = (dot(corner - base, n)) / step;
        lo = std::min(lo, t);
        hi = std::max(hi, t);
      }
      const double first = std::ceil(lo), last = std::floor(hi);
      if (last - first > 20000) return fail(Status::Limit, "a DXF hatch pattern is too dense to draw");
      std::vector<double> dashes(f.begin() + 5, f.end());
      double period = 0;
      for (const double v : dashes) period += std::abs(v);
      for (double k = first; k <= last; k += 1) {
        const Point origin = base + offset * k;
        std::vector<double> hits;
        for (const auto& [a, b] : edges) {
          const double da = dot(a - origin, n), db = dot(b - origin, n);
          if ((da > 0) == (db > 0)) continue;
          const double t = da / (da - db);
          hits.push_back(dot(a + (b - a) * t - origin, d));
        }
        std::sort(hits.begin(), hits.end());
        for (std::size_t h = 0; h + 1 < hits.size(); h += 2) {
          const double t0 = hits[h], t1 = hits[h + 1];
          if (period <= 0 || dashes.empty()) {
            lines.move_to(origin + d * t0);
            lines.line_to(origin + d * t1);
            if (--budget == 0) return fail(Status::Limit, "a DXF hatch pattern is too dense to draw");
            continue;
          }
          double start = std::floor(t0 / period) * period;
          while (start < t1) {
            double pos = start;
            for (const double v : dashes) {
              const double len = std::abs(v);
              const double a = std::max(pos, t0), b = std::min(pos + (v == 0 ? 0 : len), t1);
              if (v >= 0 && b >= a && a <= t1) {
                lines.move_to(origin + d * a);
                lines.line_to(origin + d * (v == 0 ? a + period * 1e-3 : b));
                if (--budget == 0) return fail(Status::Limit, "a DXF hatch pattern is too dense to draw");
              }
              pos += len;
            }
            start += period;
          }
        }
      }
    }
    if (lines.empty()) return {};
    Props thin = p;
    thin.dash.clear();
    return emit_stroke(std::move(lines), thin, out);
  }

  Result<void> draw_hatch(const Entity& e, const State& s, std::vector<Node>& out) {
    TagCursor c{e.tags, 0, true};
    while (c.pos < c.tags.size() && c.tags[c.pos].code != 91) ++c.pos;
    if (c.pos >= c.tags.size()) return {};
    const auto count = c.need_i(91);
    Path boundary;
    for (std::int64_t i = 0; i < count && i < 100000; ++i) {
      if (!read_boundary(c, boundary)) {
        ctx_.warn("A DXF HATCH boundary could not be read; the hatch was skipped.");
        return {};
      }
    }
    if (boundary.empty()) return {};
    const Tags head = e.tags.subspan(0, std::min(e.tags.size(), c.pos));
    const bool solid = get_i(head, 70, 0) == 1;
    // Pattern lines and gradient after the boundaries.
    std::vector<std::vector<double>> families;
    bool gradient = false;
    std::string gradient_name;
    double gradient_angle = 0;
    std::vector<Color> gradient_colors;
    const Tags rest = e.tags.subspan(c.pos);
    for (std::size_t i = 0; i < rest.size(); ++i) {
      const DxfTag& t = rest[i];
      if (t.code == 53) {
        families.push_back({tag_double(t)});
      } else if ((t.code == 43 || t.code == 44 || t.code == 45 || t.code == 46 || t.code == 49) && !families.empty()) {
        if (families.back().size() < 256) families.back().push_back(tag_double(t));
      } else if (t.code == 450) {
        gradient = tag_int(t) != 0;
      } else if (t.code == 470) {
        gradient_name = upper(t.text);
      } else if (t.code == 460) {
        gradient_angle = tag_double(t);
      } else if (t.code == 421) {
        const auto rgb = static_cast<std::uint32_t>(tag_int(t)) & 0xFFFFFF;
        gradient_colors.push_back(Color::rgb8(static_cast<int>((rgb >> 16) & 0xFF), static_cast<int>((rgb >> 8) & 0xFF),
                                              static_cast<int>(rgb & 0xFF)));
      }
    }
    const double elevation = get_p(head, 10).z;
    const Matrix m = plane(s, e.tags, std::isfinite(elevation) ? elevation : 0.0);
    const Props p = resolve(e.tags, s);
    Path world = transformed(boundary, m);
    if (gradient && !gradient_colors.empty()) {
      if (gradient_colors.size() == 1) gradient_colors.push_back(Color{1, 1, 1, 1});
      const Rect box = bounds(world);
      auto g = std::make_shared<Gradient>();
      const bool inverted = gradient_name.starts_with("INV");
      const Color c0 = inverted ? gradient_colors[1] : gradient_colors[0];
      const Color c1 = inverted ? gradient_colors[0] : gradient_colors[1];
      const Point center{(box.x0 + box.x1) / 2, (box.y0 + box.y1) / 2};
      if (gradient_name.find("SPHER") != std::string::npos || gradient_name.find("HEMI") != std::string::npos ||
          gradient_name.find("CURVED") != std::string::npos) {
        g->type = Gradient::Type::Radial;
        g->center = g->focal = center;
        g->radius = std::max(std::hypot(box.width(), box.height()) / 2, 1e-9);
        g->stops = {{0, c0}, {1, c1}};
      } else {
        const Point dir{std::cos(gradient_angle), std::sin(gradient_angle)};
        const double half = (std::abs(dir.x) * box.width() + std::abs(dir.y) * box.height()) / 2;
        g->p1 = center - dir * half;
        g->p2 = center + dir * half;
        if (gradient_name.find("CYLINDER") != std::string::npos) g->stops = {{0, c0}, {0.5, c1}, {1, c0}};
        else g->stops = {{0, c0}, {1, c1}};
      }
      Paint paint;
      paint.type = Paint::Type::Gradient;
      paint.gradient = std::move(g);
      return emit_fill(std::move(world), paint, out, FillRule::EvenOdd);
    }
    if (solid || families.empty()) {
      return emit_fill(std::move(world), Paint::solid(p.color), out, FillRule::EvenOdd);
    }
    // Pattern hatch: line families in OCS, clipped to the boundary.
    Group g;
    CVT_TRY(hatch_lines(boundary, families, p, g.children));
    for (Node& node : g.children) {
      if (auto* shape = std::get_if<Shape>(&node.value)) shape->path = transformed(shape->path, m);
    }
    for (Node& node : g.children) out.push_back(std::move(node));
    return {};
  }

  const Document& doc_;
  ReadContext& ctx_;
  int insunits_ = 0;
  double ltscale_ = 1.0;
  int lwdefault_ = 25;
  double dimasz_ = 2.5;
  std::vector<Group> groups_;
  std::map<std::string, std::size_t, std::less<>> group_index_;
  std::vector<std::vector<Vec3>> faces_;
  std::set<std::string> skipped_;
  std::uint64_t points_ = 0;
  std::uint64_t nodes_ = 0;
  std::uint64_t expansions_ = 0;
};

ir::Mesh build_mesh(const std::vector<std::vector<Vec3>>& faces) {
  ir::Mesh mesh;
  std::map<std::array<float, 3>, std::uint32_t> index;
  for (const std::vector<Vec3>& face : faces) {
    mesh.face_sizes.push_back(static_cast<std::uint32_t>(face.size()));
    for (const Vec3& v : face) {
      // DXF is +Z up; the Mesh IR is +Y up.
      const std::array<float, 3> key{static_cast<float>(v.x), static_cast<float>(v.z), static_cast<float>(-v.y)};
      auto [it, inserted] = index.try_emplace(key, static_cast<std::uint32_t>(mesh.positions.size()));
      if (inserted) mesh.positions.push_back({key[0], key[1], key[2]});
      mesh.indices.push_back(it->second);
    }
  }
  if (mesh.is_triangulated()) mesh.face_sizes.clear();
  return mesh;
}

Result<Scene> read_impl(ReadContext& ctx, bool for_mesh) {
  CVT_TRY_ASSIGN(const std::vector<DxfTag> tags, tokenize_dxf(ctx.input, ctx));
  CVT_TRY_ASSIGN(const Document doc, parse_document(tags, ctx));
  const bool paper = !for_mesh && ctx.options.get_enum("space", "model", {"model", "paper"}) == "paper";
  std::set<std::string> filter;
  if (!for_mesh) {
    const std::string layers = ctx.options.get_string("dxf_layers", "");  // split() returns views into it
    for (const std::string_view name : util::split(layers, ',', false)) {
      const std::string key = upper(name);
      if (!key.empty()) filter.insert(key);
    }
  }
  Drawer drawer(doc, ctx);
  std::size_t done = 0;
  bool any = false;
  for (const Entity& e : doc.entities) {
    if ((++done & 0xFFF) == 0) CVT_TRY(ctx.checkpoint(0.3 + 0.6 * static_cast<double>(done) / doc.entities.size()));
    const bool in_paper = get_i(e.tags, 67, 0) == 1;
    if (in_paper == paper) any = true;
    CVT_TRY(drawer.draw_top(e, paper, filter));
  }
  if (paper && !any) {
    const auto it = doc.blocks.find("*PAPER_SPACE");
    if (it != doc.blocks.end()) {
      for (const Entity& e : it->second.entities) CVT_TRY(drawer.draw_top_unchecked(e, filter));
    }
  }
  if (paper) ctx.warn("Paper space viewports are not rendered; only paper space entities were drawn.");
  if (!drawer.skipped().empty()) {
    std::string list;
    for (const std::string& type : drawer.skipped()) list += (list.empty() ? "" : ", ") + type;
    ctx.warn(std::format("Unsupported DXF entities were skipped: {}.", list));
  }

  Scene scene;
  scene.source_units = drawer.units_name();
  if (!drawer.faces().empty()) {
    scene.mesh = build_mesh(drawer.faces());
    ctx.warn("3D faces were converted from DXF's +Z up axes to +Y up.");
  }
  Page page;
  Group root;
  for (Group& g : drawer.take_groups()) root.children.push_back(Node{std::move(g)});
  Rect box = content_bounds(root, Matrix{});
  if (box.empty()) {
    const auto mn = doc.header.find("$EXTMIN"), mx = doc.header.find("$EXTMAX");
    if (mn != doc.header.end() && mx != doc.header.end()) {
      const Vec3 a = get_p(mn->second, 10), b = get_p(mx->second, 10);
      if (finite(a) && finite(b) && b.x > a.x && b.y > a.y && b.x - a.x < 1e12) box = {a.x, a.y, b.x, b.y};
    }
    if (box.empty()) box = {0, 0, 100, 100};
  }
  double k = drawer.units_to_px();
  double w = std::max(box.width() * k, 1.0), h = std::max(box.height() * k, 1.0);
  // The fit only applies when rendering (the Image reader declares "dpi") or
  // when asked for explicitly: vector conversions keep the true scale.
  const bool rendering = ctx.options.contains("dpi") || ctx.options.was_set("max_page_size");
  const double limit = rendering ? ctx.options.get_float("max_page_size", 4096.0, 0.0, 1e7) : 0.0;
  if (limit > 0 && std::max(w, h) > limit && std::isfinite(w) && std::isfinite(h)) {
    const double f = limit / std::max(w, h);
    k *= f;
    w *= f;
    h *= f;
    ctx.warn(std::format("The drawing was scaled to fit a {} px page; set max_page_size to 0 for true scale.",
                         number(limit, 0)));
  }
  if (!std::isfinite(w) || !std::isfinite(h) || !std::isfinite(k) || !(k > 0)) {
    return fail(Status::Decode, "the DXF drawing extents are not finite");
  }
  root.transform = Matrix{k, 0, 0, -k, -box.x0 * k, box.y1 * k};
  page.width = w;
  page.height = h;
  page.root = std::move(root);
  page.name = paper ? "Paper space" : "Model space";
  scene.pages.push_back(std::move(page));
  return scene;
}

}  // namespace

Result<Scene> read_dxf(ReadContext& ctx) { return read_impl(ctx, false); }
Result<Scene> read_dxf_for_mesh(ReadContext& ctx) { return read_impl(ctx, true); }

}  // namespace cvt::vector::cad
