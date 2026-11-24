// Helpers shared by the CAD format tests (dxf, hpgl, cgm).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cmath>
#include <cstdlib>
#include <string>
#include <variant>
#include <vector>

#include "engine/registry.hpp"
#include "families/vector/geom.hpp"
#include "families/vector/scene.hpp"
#include "families/vector/vector_codecs.hpp"

namespace cvt::cadtest {

using namespace cvt::vector;

struct Placed {
  const Node* node = nullptr;
  Matrix total;          ///< node space -> parent of the page root (page px)
  Matrix local;          ///< node space -> page root space (drawing units for DXF)
  const Group* layer = nullptr;
};

inline void collect(const Group& group, const Matrix& page, const Matrix& local, const Group* layer,
                    std::vector<Placed>& out) {
  const Matrix p = page * group.transform;
  for (const Node& node : group.children) {
    if (const auto* g = std::get_if<Group>(&node.value)) {
      const Group* l = g->layer ? g : layer;
      collect(*g, p, local * g->transform, l, out);
    } else {
      out.push_back({&node, p, local, layer});
    }
  }
}

/// Every leaf of page `index`; `local` maps into the root group's own space.
inline std::vector<Placed> leaves(const Scene& scene, std::size_t index = 0) {
  std::vector<Placed> out;
  const Group& root = scene.pages.at(index).root;
  for (const Node& node : root.children) {
    if (const auto* g = std::get_if<Group>(&node.value)) {
      collect(*g, root.transform * g->transform, g->transform, g->layer ? g : nullptr, out);
    } else {
      out.push_back({&node, root.transform, Matrix{}, nullptr});
    }
  }
  return out;
}

inline const Group* layer_group(const Scene& scene, std::string_view name) {
  for (const Node& node : scene.pages.at(0).root.children) {
    if (const auto* g = std::get_if<Group>(&node.value); g != nullptr && g->layer && g->layer_name == name) return g;
  }
  return nullptr;
}

inline bool near(double a, double b, double tol = 1e-6) { return std::abs(a - b) <= tol; }

inline bool rgb_is(const Color& c, int r, int g, int b) {
  const auto byte = [](float v) { return static_cast<int>(std::lround(v * 255.0f)); };
  return std::abs(byte(c.r) - r) <= 1 && std::abs(byte(c.g) - g) <= 1 && std::abs(byte(c.b) - b) <= 1;
}

inline Context test_context() {
  Context ctx;
  ctx.registry = &Registry::instance();
  return ctx;
}

/// Smallest distance from `p` to the flattened path.
inline double distance_to(const Path& path, Point p) {
  double best = 1e300;
  for (const Polyline& pl : flatten(path, 1e-4)) {
    for (std::size_t i = 0; i + 1 < pl.points.size(); ++i) {
      const Point a = pl.points[i], b = pl.points[i + 1];
      const Point d = b - a;
      const double len2 = dot(d, d);
      double t = len2 > 0 ? dot(p - a, d) / len2 : 0.0;
      t = std::clamp(t, 0.0, 1.0);
      best = std::min(best, length(a + d * t - p));
    }
    if (pl.points.size() == 1) best = std::min(best, length(pl.points[0] - p));
  }
  return best;
}

/// All text runs of page 0 in drawing order.
inline std::vector<const Text*> texts(const Scene& scene) {
  std::vector<const Text*> out;
  for (const Placed& p : leaves(scene)) {
    if (const auto* t = std::get_if<Text>(&p.node->value)) out.push_back(t);
  }
  return out;
}

inline std::string all_text(const Scene& scene) {
  std::string s;
  for (const Text* t : texts(scene)) s += t->content + "|";
  return s;
}

}  // namespace cvt::cadtest
