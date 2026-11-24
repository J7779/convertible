// Convertible core - mesh family: quadric simplification and index optimisation.
//
// simplify(): Garland-Heckbert edge collapses on the welded surface. Each
// position carries the quadric of its faces' planes; the cheapest valid
// collapse is applied first (lazy priority queue). A collapse is rejected
// when it would break manifoldness (link condition), flip or squash a
// neighbouring face, or move a locked vertex. Locked are vertices on open
// borders (when boundaries are preserved) and vertices on attribute seams
// (where one position has several normal/uv/colour variants), so borders
// stay in place and seams never crack. Faces keep their original order and
// face ranges of groups and objects are rebuilt.
//
// optimize_indices(): Tom Forsyth's linear-speed vertex cache optimisation
// per face range, then vertices in first-use order.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <vector>

#include "families/mesh/mesh_ops.hpp"

namespace cvt::mesh {

float quality_to_ratio(int quality) noexcept {
  quality = std::clamp(quality, 1, 100);
  if (quality >= 90) return 1.0f;
  const double t = static_cast<double>(quality - 1) / 89.0;  // 0 at 1, 1 at 90
  return static_cast<float>(0.02 + 0.93 * std::pow(t, 1.5));
}

namespace {

constexpr std::uint32_t none = std::numeric_limits<std::uint32_t>::max();

struct Quadric {
  // Symmetric 4x4: a2 ab ac ad b2 bc bd c2 cd d2
  double q[10] = {};
  static Quadric plane(double a, double b, double c, double d, double weight) {
    Quadric r;
    r.q[0] = a * a * weight;
    r.q[1] = a * b * weight;
    r.q[2] = a * c * weight;
    r.q[3] = a * d * weight;
    r.q[4] = b * b * weight;
    r.q[5] = b * c * weight;
    r.q[6] = b * d * weight;
    r.q[7] = c * c * weight;
    r.q[8] = c * d * weight;
    r.q[9] = d * d * weight;
    return r;
  }
  Quadric& operator+=(const Quadric& o) {
    for (int i = 0; i < 10; ++i) q[i] += o.q[i];
    return *this;
  }
  [[nodiscard]] double error(double x, double y, double z) const {
    return q[0] * x * x + 2 * q[1] * x * y + 2 * q[2] * x * z + 2 * q[3] * x + q[4] * y * y + 2 * q[5] * y * z +
           2 * q[6] * y + q[7] * z * z + 2 * q[8] * z + q[9];
  }
  // Minimiser of the quadric when the system is well conditioned.
  [[nodiscard]] bool optimum(double& x, double& y, double& z) const {
    const double a = q[0], b = q[1], c = q[2], d = q[4], e = q[5], f = q[7];
    const double det = a * (d * f - e * e) - b * (b * f - c * e) + c * (b * e - c * d);
    const double scale = std::abs(a) + std::abs(d) + std::abs(f);
    if (!(std::abs(det) > 1e-12 * scale * scale * scale) || !std::isfinite(det)) return false;
    const double r0 = -q[3], r1 = -q[6], r2 = -q[8];
    x = (r0 * (d * f - e * e) - b * (r1 * f - e * r2) + c * (r1 * e - d * r2)) / det;
    y = (a * (r1 * f - e * r2) - r0 * (b * f - c * e) + c * (b * r2 - r1 * c)) / det;
    z = (a * (d * r2 - r1 * e) - b * (b * r2 - r1 * c) + r0 * (b * e - c * d)) / det;
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
  }
};

struct Candidate {
  double cost;
  std::uint32_t from;  // node collapsed away
  std::uint32_t to;    // node kept
  std::uint32_t version_from;
  std::uint32_t version_to;
  double x, y, z;
  bool operator>(const Candidate& o) const {
    if (cost != o.cost) return cost > o.cost;
    if (from != o.from) return from > o.from;
    return to > o.to;
  }
};

class Simplifier {
 public:
  Simplifier(ir::Mesh& mesh, const SimplifyOptions& options) : mesh_(mesh), options_(options) {}

  bool run(const std::function<bool(double)>& keep_going) {
    const std::size_t faces = mesh_.indices.size() / 3;
    if (faces < 2) return true;
    std::size_t target = options_.target_triangles != 0
                             ? options_.target_triangles
                             : static_cast<std::size_t>(std::llround(static_cast<double>(faces) *
                                                                     std::clamp(static_cast<double>(options_.target_ratio), 0.0, 1.0)));
    target = std::max<std::size_t>(target, 1);
    if (target >= faces) return true;
    build();
    const Bounds box = bounds(mesh_);
    const double diagonal = box.diagonal();
    const double max_error = options_.max_relative_error > 0.0f ? options_.max_relative_error * diagonal : -1.0;
    const double max_cost = max_error > 0.0 ? max_error * max_error : std::numeric_limits<double>::infinity();

    for (std::uint32_t f = 0; f < faces; ++f) {
      for (int k = 0; k < 3; ++k) push_edge(node_of(f, k), node_of(f, (k + 1) % 3));
    }
    std::size_t alive = faces;
    std::size_t collapses = 0;
    const std::size_t initial = alive;
    while (alive > target && !queue_.empty()) {
      const Candidate c = queue_.top();
      queue_.pop();
      if (version_[c.from] != c.version_from || version_[c.to] != c.version_to) continue;
      if (removed_[c.from] || removed_[c.to]) continue;
      if (c.cost > max_cost) break;
      const std::size_t gone = collapse(c);
      if (gone == 0) continue;
      alive -= gone;
      if ((++collapses & 1023) == 0 && keep_going) {
        if (!keep_going(static_cast<double>(initial - alive) / static_cast<double>(initial - target))) {
          finish();
          return false;
        }
      }
    }
    finish();
    return true;
  }

 private:
  std::uint32_t node_of(std::uint32_t face, int k) const { return node_[mesh_.indices[face * 3 + k]]; }

  void build() {
    const std::size_t n = mesh_.positions.size();
    const std::size_t faces = mesh_.indices.size() / 3;
    // Nodes: one per distinct position.
    std::vector<std::uint32_t> order(n);
    for (std::size_t i = 0; i < n; ++i) order[i] = static_cast<std::uint32_t>(i);
    const auto key = [&](std::uint32_t i) {
      const ir::Vec3& p = mesh_.positions[i];
      return std::array<float, 3>{p.x == 0 ? 0.0f : p.x, p.y == 0 ? 0.0f : p.y, p.z == 0 ? 0.0f : p.z};
    };
    std::stable_sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) { return key(a) < key(b); });
    node_.assign(n, 0);
    std::uint32_t count = 0;
    for (std::size_t i = 0; i < n; ++i) {
      if (i > 0 && key(order[i]) != key(order[i - 1])) ++count;
      node_[order[i]] = count;
    }
    const std::size_t nodes = n == 0 ? 0 : count + 1;
    position_.assign(nodes, {});
    vertices_of_.assign(nodes, {});
    for (std::size_t i = 0; i < n; ++i) {
      position_[node_[i]] = {mesh_.positions[i].x, mesh_.positions[i].y, mesh_.positions[i].z};
      vertices_of_[node_[i]].push_back(static_cast<std::uint32_t>(i));
    }
    faces_of_.assign(nodes, {});
    face_alive_.assign(faces, true);
    quadric_.assign(nodes, {});
    for (std::uint32_t f = 0; f < faces; ++f) {
      const std::uint32_t a = node_of(f, 0), b = node_of(f, 1), c = node_of(f, 2);
      if (a == b || b == c || a == c) {
        face_alive_[f] = false;  // degenerate already
        continue;
      }
      for (int k = 0; k < 3; ++k) faces_of_[node_of(f, k)].push_back(f);
      const auto& pa = position_[a];
      const auto& pb = position_[b];
      const auto& pc = position_[c];
      const std::array<double, 3> u = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
      const std::array<double, 3> v = {pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]};
      std::array<double, 3> nrm = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
      const double len = std::sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
      if (len > 0) {
        for (double& x : nrm) x /= len;
        const double d = -(nrm[0] * pa[0] + nrm[1] * pa[1] + nrm[2] * pa[2]);
        const Quadric q = Quadric::plane(nrm[0], nrm[1], nrm[2], d, len * 0.5);
        quadric_[a] += q;
        quadric_[b] += q;
        quadric_[c] += q;
      }
    }
    // Locks: attribute seams always; open borders when asked.
    locked_.assign(nodes, false);
    removed_.assign(nodes, false);
    version_.assign(nodes, 0);
    for (std::size_t node = 0; node < nodes; ++node) {
      std::uint32_t first = none;
      for (const std::uint32_t f : faces_of_[node]) {
        for (int k = 0; k < 3; ++k) {
          if (node_of(f, k) != node) continue;
          const std::uint32_t vertex = mesh_.indices[f * 3 + k];
          if (first == none) first = vertex;
          else if (vertex != first) locked_[node] = true;
        }
      }
    }
    // Border edges: used by exactly one face (counted per undirected node pair).
    std::vector<std::pair<std::uint64_t, std::uint32_t>> edges;
    edges.reserve(faces * 3);
    for (std::uint32_t f = 0; f < faces; ++f) {
      if (!face_alive_[f]) continue;
      for (int k = 0; k < 3; ++k) {
        const std::uint32_t a = node_of(f, k), b = node_of(f, (k + 1) % 3);
        edges.emplace_back((static_cast<std::uint64_t>(std::min(a, b)) << 32) | std::max(a, b), f);
      }
    }
    std::sort(edges.begin(), edges.end());
    for (std::size_t i = 0; i < edges.size();) {
      std::size_t j = i;
      while (j < edges.size() && edges[j].first == edges[i].first) ++j;
      if (j - i != 2) {  // border (1) or non-manifold (>2): keep both ends
        const auto a = static_cast<std::uint32_t>(edges[i].first >> 32);
        const auto b = static_cast<std::uint32_t>(edges[i].first & 0xFFFFFFFFu);
        if (j - i > 2 || options_.preserve_boundary) {
          locked_[a] = true;
          locked_[b] = true;
        } else {
          // Unpreserved border: add a perpendicular plane so the border keeps its line.
          border_quadric(a, b, edges[i].second);
        }
      }
      i = j;
    }
  }

  void border_quadric(std::uint32_t a, std::uint32_t b, std::uint32_t face) {
    const auto& pa = position_[a];
    const auto& pb = position_[b];
    std::uint32_t c = none;
    for (int k = 0; k < 3; ++k) {
      if (node_of(face, k) != a && node_of(face, k) != b) c = node_of(face, k);
    }
    if (c == none) return;
    const auto& pc = position_[c];
    const std::array<double, 3> e = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
    const std::array<double, 3> v = {pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]};
    const std::array<double, 3> fn = {e[1] * v[2] - e[2] * v[1], e[2] * v[0] - e[0] * v[2], e[0] * v[1] - e[1] * v[0]};
    std::array<double, 3> p = {e[1] * fn[2] - e[2] * fn[1], e[2] * fn[0] - e[0] * fn[2], e[0] * fn[1] - e[1] * fn[0]};
    const double len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    if (!(len > 0)) return;
    for (double& x : p) x /= len;
    const double d = -(p[0] * pa[0] + p[1] * pa[1] + p[2] * pa[2]);
    const double weight = (e[0] * e[0] + e[1] * e[1] + e[2] * e[2]) * 10.0;
    const Quadric q = Quadric::plane(p[0], p[1], p[2], d, weight);
    quadric_[a] += q;
    quadric_[b] += q;
  }

  void push_edge(std::uint32_t a, std::uint32_t b) {
    if (a == b) return;
    const bool la = locked_[a], lb = locked_[b];
    if (la && lb) return;
    Quadric q = quadric_[a];
    q += quadric_[b];
    // Directions allowed: an unlocked node may move into the other.
    const auto consider = [&](std::uint32_t from, std::uint32_t to) {
      double x = position_[to][0], y = position_[to][1], z = position_[to][2];
      if (!locked_[to] && vertices_of_[to].size() <= 1) {
        double ox = 0, oy = 0, oz = 0;
        if (q.optimum(ox, oy, oz)) {
          // Stay near the edge: the optimum may lie far off for flat areas.
          const auto& p = position_[from];
          const auto& r = position_[to];
          const double ex = r[0] - p[0], ey = r[1] - p[1], ez = r[2] - p[2];
          const double len2 = ex * ex + ey * ey + ez * ez;
          const double dx = ox - (p[0] + r[0]) * 0.5, dy = oy - (p[1] + r[1]) * 0.5, dz = oz - (p[2] + r[2]) * 0.5;
          if (dx * dx + dy * dy + dz * dz <= len2) {
            x = ox;
            y = oy;
            z = oz;
          }
        }
        const double mid[3] = {(position_[from][0] + position_[to][0]) * 0.5, (position_[from][1] + position_[to][1]) * 0.5,
                               (position_[from][2] + position_[to][2]) * 0.5};
        if (q.error(mid[0], mid[1], mid[2]) < q.error(x, y, z)) {
          x = mid[0];
          y = mid[1];
          z = mid[2];
        }
      }
      const double cost = std::max(0.0, q.error(x, y, z));
      queue_.push({cost, from, to, version_[from], version_[to], x, y, z});
    };
    if (!la) consider(a, b);
    if (!lb) consider(b, a);
  }

  // Applies a collapse when it is valid; returns the number of faces removed.
  std::size_t collapse(const Candidate& c) {
    const std::uint32_t u = c.from, v = c.to;
    // Faces shared by u and v, and the link condition.
    shared_.clear();
    for (const std::uint32_t f : faces_of_[u]) {
      if (!face_alive_[f]) continue;
      for (int k = 0; k < 3; ++k) {
        if (node_of(f, k) == v) {
          shared_.push_back(f);
          break;
        }
      }
    }
    if (shared_.empty() || shared_.size() > 2) return 0;
    neighbours(u, link_u_);
    neighbours(v, link_v_);
    std::size_t common = 0;
    for (const std::uint32_t w : link_u_) {
      if (std::binary_search(link_v_.begin(), link_v_.end(), w)) ++common;
    }
    if (common != shared_.size()) return 0;
    // No face may flip or collapse when u (and v) move to the new position.
    const std::array<double, 3> target = {c.x, c.y, c.z};
    for (const std::uint32_t node : {u, v}) {
      for (const std::uint32_t f : faces_of_[node]) {
        if (!face_alive_[f] || std::find(shared_.begin(), shared_.end(), f) != shared_.end()) continue;
        std::array<std::array<double, 3>, 3> before, after;
        for (int k = 0; k < 3; ++k) {
          const std::uint32_t w = node_of(f, k);
          before[k] = position_[w];
          after[k] = (w == u || w == v) ? target : position_[w];
        }
        const auto normal = [](const std::array<std::array<double, 3>, 3>& t) {
          const std::array<double, 3> a = {t[1][0] - t[0][0], t[1][1] - t[0][1], t[1][2] - t[0][2]};
          const std::array<double, 3> b = {t[2][0] - t[0][0], t[2][1] - t[0][1], t[2][2] - t[0][2]};
          return std::array<double, 3>{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        };
        const auto n0 = normal(before);
        const auto n1 = normal(after);
        const double l0 = std::sqrt(n0[0] * n0[0] + n0[1] * n0[1] + n0[2] * n0[2]);
        const double l1 = std::sqrt(n1[0] * n1[0] + n1[1] * n1[1] + n1[2] * n1[2]);
        if (!(l1 > 1e-12 * (l0 + 1e-30))) return 0;
        if ((n0[0] * n1[0] + n0[1] * n1[1] + n0[2] * n1[2]) < 0.25 * l0 * l1) return 0;
      }
    }
    // Attribute vertex of v used by the shared faces (v may be a seam node).
    std::uint32_t keep = none;
    for (const std::uint32_t f : shared_) {
      for (int k = 0; k < 3; ++k) {
        if (node_of(f, k) != v) continue;
        const std::uint32_t vertex = mesh_.indices[f * 3 + k];
        if (keep == none) keep = vertex;
        else if (keep != vertex) return 0;
      }
    }
    if (keep == none) return 0;
    // Interpolate the kept vertex's attributes along the edge when v moves.
    std::uint32_t gone_vertex = none;
    for (const std::uint32_t f : shared_) {
      for (int k = 0; k < 3; ++k) {
        if (node_of(f, k) == u) gone_vertex = mesh_.indices[f * 3 + k];
      }
    }
    const bool moved = target != position_[v];
    if (moved && gone_vertex != none) {
      const auto& p = position_[u];
      const auto& q = position_[v];
      const double ex = q[0] - p[0], ey = q[1] - p[1], ez = q[2] - p[2];
      const double len2 = ex * ex + ey * ey + ez * ez;
      double t = len2 > 0 ? ((target[0] - p[0]) * ex + (target[1] - p[1]) * ey + (target[2] - p[2]) * ez) / len2 : 1.0;
      t = std::clamp(t, 0.0, 1.0);
      const auto mix = [t](float a, float b) { return static_cast<float>(a + (b - a) * t); };
      if (!mesh_.texcoords.empty()) {
        const ir::Vec2 a = mesh_.texcoords[gone_vertex], b = mesh_.texcoords[keep];
        mesh_.texcoords[keep] = {mix(a.x, b.x), mix(a.y, b.y)};
      }
      if (!mesh_.colors.empty()) {
        const ir::Color a = mesh_.colors[gone_vertex], b = mesh_.colors[keep];
        mesh_.colors[keep] = {mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), mix(a.a, b.a)};
      }
      for (ir::VertexAttribute& attribute : mesh_.attributes) {
        attribute.values[keep] = mix(attribute.values[gone_vertex], attribute.values[keep]);
      }
    }
    // Apply.
    for (const std::uint32_t f : shared_) face_alive_[f] = false;
    for (const std::uint32_t f : faces_of_[u]) {
      if (!face_alive_[f]) continue;
      for (int k = 0; k < 3; ++k) {
        if (node_of(f, k) == u) mesh_.indices[f * 3 + k] = keep;
      }
      faces_of_[v].push_back(f);
    }
    faces_of_[u].clear();
    std::erase_if(faces_of_[v], [&](std::uint32_t f) { return !face_alive_[f]; });
    removed_[u] = true;
    position_[v] = target;
    for (const std::uint32_t vertex : vertices_of_[v]) {
      mesh_.positions[vertex] = {static_cast<float>(target[0]), static_cast<float>(target[1]), static_cast<float>(target[2])};
    }
    quadric_[v] += quadric_[u];
    ++version_[u];
    ++version_[v];
    // New candidates around v.
    neighbours(v, link_v_);
    for (const std::uint32_t w : link_v_) {
      ++version_[w];
    }
    for (const std::uint32_t w : link_v_) {
      push_edge(v, w);
      // w's other edges changed cost too (its quadric did not, but versions did).
      neighbours(w, link_u_);
      for (const std::uint32_t x : link_u_) push_edge(w, x);
    }
    return shared_.size();
  }

  void neighbours(std::uint32_t node, std::vector<std::uint32_t>& out) const {
    out.clear();
    for (const std::uint32_t f : faces_of_[node]) {
      if (!face_alive_[f]) continue;
      for (int k = 0; k < 3; ++k) {
        const std::uint32_t w = node_of(f, k);
        if (w != node) out.push_back(w);
      }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
  }

  void finish() {
    const std::size_t faces = mesh_.indices.size() / 3;
    std::vector<std::uint32_t> indices;
    indices.reserve(mesh_.indices.size());
    std::vector<std::size_t> first_new(faces + 1, 0);
    std::size_t kept = 0;
    for (std::size_t f = 0; f < faces; ++f) {
      first_new[f] = kept;
      if (!face_alive_[f]) continue;
      const std::uint32_t a = mesh_.indices[f * 3], b = mesh_.indices[f * 3 + 1], c = mesh_.indices[f * 3 + 2];
      if (a == b || b == c || a == c) continue;
      indices.insert(indices.end(), {a, b, c});
      ++kept;
    }
    first_new[faces] = kept;
    mesh_.indices = std::move(indices);
    remap_face_ranges(mesh_, first_new);
    remove_unused_vertices(mesh_);
  }

  ir::Mesh& mesh_;
  SimplifyOptions options_;
  std::vector<std::uint32_t> node_;
  std::vector<std::array<double, 3>> position_;
  std::vector<std::vector<std::uint32_t>> vertices_of_;
  std::vector<std::vector<std::uint32_t>> faces_of_;
  std::vector<bool> face_alive_;
  std::vector<Quadric> quadric_;
  std::vector<bool> locked_;
  std::vector<bool> removed_;
  std::vector<std::uint32_t> version_;
  std::priority_queue<Candidate, std::vector<Candidate>, std::greater<Candidate>> queue_;
  std::vector<std::uint32_t> shared_, link_u_, link_v_;
};

}  // namespace

bool simplify(ir::Mesh& mesh, const SimplifyOptions& options, const std::function<bool(double)>& keep_going) {
  if (mesh.point_cloud || !mesh.valid()) return true;
  triangulate(mesh);
  const bool had_normals = !mesh.normals.empty();
  Simplifier simplifier(mesh, options);
  const bool finished = simplifier.run(keep_going);
  if (had_normals) {
    // Normals of moved vertices are stale; recompute with a crease so hard
    // edges stay hard.
    mesh.normals.clear();
    generate_normals(mesh, 60.0f);
  }
  return finished;
}

// --------------------------------------------------------------------------- vertex cache optimisation

namespace {

constexpr int cache_size = 32;

float vertex_score(int cache_position, std::uint32_t remaining) {
  if (remaining == 0) return -1.0f;
  float score = 0.0f;
  if (cache_position >= 0) {
    if (cache_position < 3) {
      score = 0.75f;
    } else {
      const float scaler = 1.0f / (cache_size - 3);
      score = std::pow(1.0f - static_cast<float>(cache_position - 3) * scaler, 1.5f);
    }
  }
  score += 2.0f * std::pow(static_cast<float>(remaining), -0.5f);
  return score;
}

// Forsyth ordering of the triangles [first, first + count).
void forsyth(std::vector<std::uint32_t>& indices, std::size_t first, std::size_t count, std::size_t vertices) {
  if (count < 2) return;
  std::vector<std::uint32_t> remaining(vertices, 0);
  for (std::size_t t = first; t < first + count; ++t) {
    for (int k = 0; k < 3; ++k) ++remaining[indices[t * 3 + k]];
  }
  std::vector<std::uint32_t> offset(vertices + 1, 0);
  for (std::size_t v = 0; v < vertices; ++v) offset[v + 1] = offset[v] + remaining[v];
  std::vector<std::uint32_t> fill(offset.begin(), offset.end() - 1);
  std::vector<std::uint32_t> triangles_of(offset.back());
  for (std::size_t t = first; t < first + count; ++t) {
    for (int k = 0; k < 3; ++k) triangles_of[fill[indices[t * 3 + k]]++] = static_cast<std::uint32_t>(t - first);
  }
  std::vector<int> cache_position(vertices, -1);
  std::vector<float> score(vertices, 0.0f);
  for (std::size_t v = 0; v < vertices; ++v) score[v] = vertex_score(-1, remaining[v]);
  std::vector<float> triangle_score(count, 0.0f);
  std::vector<bool> emitted(count, false);
  const auto tri_score = [&](std::size_t t) {
    return score[indices[(first + t) * 3]] + score[indices[(first + t) * 3 + 1]] + score[indices[(first + t) * 3 + 2]];
  };
  for (std::size_t t = 0; t < count; ++t) triangle_score[t] = tri_score(t);
  std::vector<std::uint32_t> out;
  out.reserve(count * 3);
  std::vector<std::uint32_t> cache;
  std::size_t scan = 0;
  std::size_t best = 0;
  float best_score = -1.0f;
  for (std::size_t t = 0; t < count; ++t) {
    if (triangle_score[t] > best_score) {
      best_score = triangle_score[t];
      best = t;
    }
  }
  for (std::size_t done = 0; done < count; ++done) {
    if (best_score < 0.0f) {  // no candidate from the cache: take the next unemitted triangle
      while (scan < count && emitted[scan]) ++scan;
      best = scan;
    }
    emitted[best] = true;
    std::vector<std::uint32_t> next_cache;
    next_cache.reserve(cache_size + 3);
    for (int k = 0; k < 3; ++k) {
      const std::uint32_t v = indices[(first + best) * 3 + k];
      out.push_back(v);
      --remaining[v];
      next_cache.push_back(v);
      // Remove the triangle from the vertex's list.
      auto begin = triangles_of.begin() + offset[v];
      auto end = begin + static_cast<std::ptrdiff_t>(remaining[v] + 1);
      auto it = std::find(begin, end, static_cast<std::uint32_t>(best));
      if (it != end) std::iter_swap(it, end - 1);
    }
    for (const std::uint32_t v : cache) {
      if (std::find(next_cache.begin(), next_cache.end(), v) == next_cache.end()) next_cache.push_back(v);
    }
    std::vector<std::uint32_t> touched = next_cache;
    for (std::size_t i = 0; i < next_cache.size(); ++i) {
      cache_position[next_cache[i]] = i < cache_size ? static_cast<int>(i) : -1;
    }
    if (next_cache.size() > cache_size) next_cache.resize(cache_size);
    cache = std::move(next_cache);
    for (const std::uint32_t v : touched) score[v] = vertex_score(cache_position[v], remaining[v]);
    best_score = -1.0f;
    for (const std::uint32_t v : touched) {
      for (std::uint32_t i = 0; i < remaining[v]; ++i) {
        const std::uint32_t t = triangles_of[offset[v] + i];
        triangle_score[t] = tri_score(t);
        if (triangle_score[t] > best_score) {
          best_score = triangle_score[t];
          best = t;
        }
      }
    }
  }
  std::copy(out.begin(), out.end(), indices.begin() + static_cast<std::ptrdiff_t>(first * 3));
}

}  // namespace

void optimize_indices(ir::Mesh& mesh) {
  if (!mesh.face_sizes.empty() || mesh.indices.empty() || !mesh.valid()) return;
  const std::size_t faces = mesh.indices.size() / 3;
  // Ranges that must stay intact: boundaries of groups and objects.
  std::vector<std::size_t> cuts = {0, faces};
  for (const ir::MeshGroup& g : mesh.groups) {
    cuts.push_back(std::min(g.first_face, faces));
    cuts.push_back(std::min(g.first_face + g.face_count, faces));
  }
  for (const ir::MeshObject& o : mesh.objects) {
    cuts.push_back(std::min(o.first_face, faces));
    cuts.push_back(std::min(o.first_face + o.face_count, faces));
  }
  std::sort(cuts.begin(), cuts.end());
  cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
  for (std::size_t i = 0; i + 1 < cuts.size(); ++i) forsyth(mesh.indices, cuts[i], cuts[i + 1] - cuts[i], mesh.positions.size());
  // Vertices in first-use order.
  std::vector<std::uint32_t> old_to_new(mesh.positions.size(), none);
  std::uint32_t next = 0;
  for (const std::uint32_t index : mesh.indices) {
    if (old_to_new[index] == none) old_to_new[index] = next++;
  }
  for (std::uint32_t& v : old_to_new) {
    if (v == none) v = next++;  // unused vertices keep a place at the end
  }
  remap_vertices(mesh, old_to_new, mesh.positions.size());
}

}  // namespace cvt::mesh
