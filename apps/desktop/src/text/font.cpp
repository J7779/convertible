// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "text/font.hpp"

#include <bit>
#include <cstring>

namespace cvd::text {

bool Font::in_range(std::size_t off, std::size_t len) const { return off <= data_.size() && len <= data_.size() - off; }

std::uint16_t Font::u16(std::size_t off) const {
  if (!in_range(off, 2)) return 0;
  return static_cast<std::uint16_t>((data_[off] << 8) | data_[off + 1]);
}

std::int16_t Font::s16(std::size_t off) const { return static_cast<std::int16_t>(u16(off)); }

std::uint32_t Font::u32(std::size_t off) const {
  if (!in_range(off, 4)) return 0;
  return (static_cast<std::uint32_t>(data_[off]) << 24) | (static_cast<std::uint32_t>(data_[off + 1]) << 16) |
         (static_cast<std::uint32_t>(data_[off + 2]) << 8) | data_[off + 3];
}

Font::Table Font::find_table(const char* tag) const {
  const std::uint16_t count = u16(4);
  for (std::uint16_t i = 0; i < count; ++i) {
    const std::size_t rec = 12 + static_cast<std::size_t>(i) * 16;
    if (!in_range(rec, 16)) break;
    if (std::memcmp(data_.data() + rec, tag, 4) == 0) {
      Table t{u32(rec + 8), u32(rec + 12)};
      if (!in_range(t.offset, t.length)) return {};
      return t;
    }
  }
  return {};
}

bool Font::load(std::span<const std::uint8_t> data) {
  data_ = data;
  valid_ = false;
  if (data.size() < 12) return false;
  const std::uint32_t version = u32(0);
  if (version != 0x00010000u && version != 0x74727565u /* 'true' */) return false;
  const Table head = find_table("head");
  const Table hhea = find_table("hhea");
  const Table maxp = find_table("maxp");
  const Table cmap = find_table("cmap");
  glyf_ = find_table("glyf");
  loca_ = find_table("loca");
  hmtx_ = find_table("hmtx");
  kern_ = find_table("kern");
  gpos_ = find_table("GPOS");
  gsub_ = find_table("GSUB");
  if (!head.length || !hhea.length || !maxp.length || !cmap.length || !glyf_.length || !loca_.length || !hmtx_.length) {
    return false;
  }
  upem_ = u16(head.offset + 18);
  if (upem_ < 16 || upem_ > 16384) return false;
  long_loca_ = s16(head.offset + 50) != 0;
  ascender_ = s16(hhea.offset + 4);
  descender_ = s16(hhea.offset + 6);
  line_gap_ = s16(hhea.offset + 8);
  num_hmetrics_ = u16(hhea.offset + 34);
  num_glyphs_ = u16(maxp.offset + 4);
  if (num_hmetrics_ == 0) return false;

  // Prefer a full-Unicode (format 12) subtable, then the BMP (format 4) one.
  const std::uint16_t nsub = u16(cmap.offset + 2);
  std::size_t best = 0;
  int best_score = -1;
  for (std::uint16_t i = 0; i < nsub; ++i) {
    const std::size_t rec = cmap.offset + 4 + static_cast<std::size_t>(i) * 8;
    const std::uint16_t platform = u16(rec);
    const std::uint16_t encoding = u16(rec + 2);
    const std::size_t sub = cmap.offset + u32(rec + 4);
    const std::uint16_t format = u16(sub);
    int score = -1;
    if (format == 12 && (platform == 3 || platform == 0)) score = 3;
    else if (format == 4 && ((platform == 3 && (encoding == 1 || encoding == 10)) || platform == 0)) score = 2;
    if (score > best_score) {
      best_score = score;
      best = sub;
    }
  }
  if (best_score < 0) return false;
  cmap_subtable_ = best;
  cmap_format_ = u16(best);

  if (gpos_.length) {
    gpos_kern_subtables_ = feature_lookup_tables(gpos_.offset, "kern", 2, 9);
  }
  valid_ = true;
  return true;
}

std::uint16_t Font::cmap_lookup(std::uint32_t cp) const {
  const std::size_t s = cmap_subtable_;
  if (cmap_format_ == 4) {
    if (cp > 0xFFFF) return 0;
    const std::uint16_t segx2 = u16(s + 6);
    const std::size_t ends = s + 14;
    const std::size_t starts = ends + segx2 + 2;
    const std::size_t deltas = starts + segx2;
    const std::size_t ranges = deltas + segx2;
    // Binary search the end codes.
    std::size_t lo = 0, hi = segx2 / 2u;
    while (lo < hi) {
      const std::size_t mid = (lo + hi) / 2;
      if (u16(ends + mid * 2) < cp) lo = mid + 1;
      else hi = mid;
    }
    if (lo >= segx2 / 2u) return 0;
    const std::size_t i = lo;
    const std::uint16_t start = u16(starts + i * 2);
    if (cp < start) return 0;
    const std::uint16_t delta = u16(deltas + i * 2);
    const std::uint16_t range = u16(ranges + i * 2);
    if (range == 0) return static_cast<std::uint16_t>((cp + delta) & 0xFFFFu);
    const std::size_t addr = ranges + i * 2 + range + (cp - start) * 2;
    const std::uint16_t g = u16(addr);
    if (g == 0) return 0;
    return static_cast<std::uint16_t>((g + delta) & 0xFFFFu);
  }
  if (cmap_format_ == 12) {
    const std::uint32_t groups = u32(s + 12);
    std::uint32_t lo = 0, hi = groups;
    while (lo < hi) {
      const std::uint32_t mid = (lo + hi) / 2;
      const std::size_t g = s + 16 + static_cast<std::size_t>(mid) * 12;
      const std::uint32_t start = u32(g);
      const std::uint32_t end = u32(g + 4);
      if (cp < start) hi = mid;
      else if (cp > end) lo = mid + 1;
      else return static_cast<std::uint16_t>(u32(g + 8) + (cp - start));
    }
  }
  return 0;
}

std::uint16_t Font::glyph_index(std::uint32_t codepoint) const {
  if (!valid_) return 0;
  return cmap_lookup(codepoint);
}

int Font::advance(std::uint16_t glyph) const {
  if (!valid_) return 0;
  const int idx = glyph < num_hmetrics_ ? glyph : num_hmetrics_ - 1;
  return u16(hmtx_.offset + static_cast<std::size_t>(idx) * 4);
}

int Font::left_side_bearing(std::uint16_t glyph) const {
  if (!valid_) return 0;
  if (glyph < num_hmetrics_) return s16(hmtx_.offset + static_cast<std::size_t>(glyph) * 4 + 2);
  return s16(hmtx_.offset + static_cast<std::size_t>(num_hmetrics_) * 4 +
             static_cast<std::size_t>(glyph - num_hmetrics_) * 2);
}

bool Font::glyph_range(std::uint16_t glyph, std::uint32_t& start, std::uint32_t& end) const {
  if (glyph >= num_glyphs_) return false;
  if (long_loca_) {
    start = u32(loca_.offset + static_cast<std::size_t>(glyph) * 4);
    end = u32(loca_.offset + static_cast<std::size_t>(glyph) * 4 + 4);
  } else {
    start = static_cast<std::uint32_t>(u16(loca_.offset + static_cast<std::size_t>(glyph) * 2)) * 2u;
    end = static_cast<std::uint32_t>(u16(loca_.offset + static_cast<std::size_t>(glyph) * 2 + 2)) * 2u;
  }
  return end > start && end <= glyf_.length;
}

gfx::Path Font::outline(std::uint16_t glyph) const {
  gfx::Path path;
  if (!valid_) return path;
  const float identity[6] = {1, 0, 0, 1, 0, 0};
  append_outline(path, glyph, identity, 0);
  return path;
}

void Font::append_outline(gfx::Path& path, std::uint16_t glyph, const float m[6], int depth) const {
  if (depth > 8) return;
  std::uint32_t start = 0, end = 0;
  if (!glyph_range(glyph, start, end)) return;
  const std::size_t g = glyf_.offset + start;
  const std::size_t gend = glyf_.offset + end;
  const std::int16_t ncontours = s16(g);
  const auto xf = [&](float x, float y) { return Vec2{m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]}; };

  if (ncontours >= 0) {
    const std::size_t end_pts = g + 10;
    const std::uint16_t npoints =
        ncontours > 0 ? static_cast<std::uint16_t>(u16(end_pts + static_cast<std::size_t>(ncontours - 1) * 2) + 1) : 0;
    const std::uint16_t ins_len = u16(end_pts + static_cast<std::size_t>(ncontours) * 2);
    std::size_t p = end_pts + static_cast<std::size_t>(ncontours) * 2 + 2 + ins_len;
    std::vector<std::uint8_t> flags(npoints);
    for (std::size_t i = 0; i < npoints && p < gend;) {
      const std::uint8_t f = data_[p++];
      flags[i++] = f;
      if (f & 8) {
        std::uint8_t rep = p < gend ? data_[p++] : 0;
        while (rep-- > 0 && i < npoints) flags[i++] = f;
      }
    }
    std::vector<Vec2> pts(npoints);
    int v = 0;
    for (std::size_t i = 0; i < npoints; ++i) {
      const std::uint8_t f = flags[i];
      if (f & 2) {
        const int d = p < gend ? data_[p++] : 0;
        v += (f & 16) ? d : -d;
      } else if (!(f & 16)) {
        v += s16(p);
        p += 2;
      }
      pts[i].x = static_cast<float>(v);
    }
    v = 0;
    for (std::size_t i = 0; i < npoints; ++i) {
      const std::uint8_t f = flags[i];
      if (f & 4) {
        const int d = p < gend ? data_[p++] : 0;
        v += (f & 32) ? d : -d;
      } else if (!(f & 32)) {
        v += s16(p);
        p += 2;
      }
      pts[i].y = static_cast<float>(v);
    }
    std::size_t first = 0;
    for (int c = 0; c < ncontours; ++c) {
      const std::size_t last = u16(end_pts + static_cast<std::size_t>(c) * 2);
      if (last >= npoints || last < first) break;
      const std::size_t n = last - first + 1;
      const auto on = [&](std::size_t i) { return (flags[first + (i % n)] & 1) != 0; };
      const auto pt = [&](std::size_t i) { return pts[first + (i % n)]; };
      // Find a starting on-curve point (or synthesise one between two off points).
      std::size_t s = 0;
      Vec2 start_pt;
      bool found = false;
      for (std::size_t i = 0; i < n; ++i) {
        if (on(i)) {
          s = i;
          start_pt = pt(i);
          found = true;
          break;
        }
      }
      if (!found) {
        start_pt = (pt(0) + pt(1)) * 0.5f;
        s = 0;
      }
      path.move_to(xf(start_pt.x, start_pt.y));
      Vec2 ctrl{};
      bool have_ctrl = false;
      const std::size_t begin = found ? s + 1 : 1;
      for (std::size_t k = 0; k < n; ++k) {
        const std::size_t i = begin + k;
        const Vec2 q = pt(i);
        if (on(i)) {
          if (have_ctrl) {
            path.quad_to(xf(ctrl.x, ctrl.y), xf(q.x, q.y));
            have_ctrl = false;
          } else {
            path.line_to(xf(q.x, q.y));
          }
        } else {
          if (have_ctrl) {
            const Vec2 mid = (ctrl + q) * 0.5f;
            path.quad_to(xf(ctrl.x, ctrl.y), xf(mid.x, mid.y));
          }
          ctrl = q;
          have_ctrl = true;
        }
      }
      if (have_ctrl) path.quad_to(xf(ctrl.x, ctrl.y), xf(start_pt.x, start_pt.y));
      path.close();
      first = last + 1;
    }
    return;
  }

  // Composite glyph.
  std::size_t p = g + 10;
  while (p + 4 <= gend) {
    const std::uint16_t flags = u16(p);
    const std::uint16_t component = u16(p + 2);
    p += 4;
    float dx = 0, dy = 0;
    if (flags & 1) {
      dx = s16(p);
      dy = s16(p + 2);
      p += 4;
    } else {
      dx = static_cast<std::int8_t>(data_[p]);
      dy = static_cast<std::int8_t>(data_[p + 1]);
      p += 2;
    }
    float a = 1, b = 0, c = 0, d = 1;
    const auto f2dot14 = [&](std::size_t off) { return static_cast<float>(s16(off)) / 16384.0f; };
    if (flags & 8) {
      a = d = f2dot14(p);
      p += 2;
    } else if (flags & 0x40) {
      a = f2dot14(p);
      d = f2dot14(p + 2);
      p += 4;
    } else if (flags & 0x80) {
      a = f2dot14(p);
      b = f2dot14(p + 2);
      c = f2dot14(p + 4);
      d = f2dot14(p + 6);
      p += 8;
    }
    // Only offsets given as x/y (ARGS_ARE_XY_VALUES) are supported; point matching is rare.
    if (!(flags & 2)) {
      dx = 0;
      dy = 0;
    }
    const float local[6] = {a, b, c, d, dx, dy};
    float combined[6];
    combined[0] = m[0] * local[0] + m[2] * local[1];
    combined[1] = m[1] * local[0] + m[3] * local[1];
    combined[2] = m[0] * local[2] + m[2] * local[3];
    combined[3] = m[1] * local[2] + m[3] * local[3];
    combined[4] = m[0] * local[4] + m[2] * local[5] + m[4];
    combined[5] = m[1] * local[4] + m[3] * local[5] + m[5];
    append_outline(path, component, combined, depth + 1);
    if (!(flags & 0x20)) break;  // MORE_COMPONENTS
  }
}

// ---------------------------------------------------------------------------
// OpenType layout
// ---------------------------------------------------------------------------

int Font::coverage_index(std::size_t cov, std::uint16_t glyph) const {
  const std::uint16_t format = u16(cov);
  if (format == 1) {
    const std::uint16_t count = u16(cov + 2);
    int lo = 0, hi = count - 1;
    while (lo <= hi) {
      const int mid = (lo + hi) / 2;
      const std::uint16_t g = u16(cov + 4 + static_cast<std::size_t>(mid) * 2);
      if (g == glyph) return mid;
      if (g < glyph) lo = mid + 1;
      else hi = mid - 1;
    }
  } else if (format == 2) {
    const std::uint16_t count = u16(cov + 2);
    int lo = 0, hi = count - 1;
    while (lo <= hi) {
      const int mid = (lo + hi) / 2;
      const std::size_t r = cov + 4 + static_cast<std::size_t>(mid) * 6;
      const std::uint16_t start = u16(r);
      const std::uint16_t end = u16(r + 2);
      if (glyph < start) hi = mid - 1;
      else if (glyph > end) lo = mid + 1;
      else return u16(r + 4) + (glyph - start);
    }
  }
  return -1;
}

int Font::class_of(std::size_t cd, std::uint16_t glyph) const {
  const std::uint16_t format = u16(cd);
  if (format == 1) {
    const std::uint16_t start = u16(cd + 2);
    const std::uint16_t count = u16(cd + 4);
    if (glyph >= start && glyph < start + count) return u16(cd + 6 + static_cast<std::size_t>(glyph - start) * 2);
  } else if (format == 2) {
    const std::uint16_t count = u16(cd + 2);
    int lo = 0, hi = count - 1;
    while (lo <= hi) {
      const int mid = (lo + hi) / 2;
      const std::size_t r = cd + 4 + static_cast<std::size_t>(mid) * 6;
      if (glyph < u16(r)) hi = mid - 1;
      else if (glyph > u16(r + 2)) lo = mid + 1;
      else return u16(r + 4);
    }
  }
  return 0;
}

std::vector<std::size_t> Font::feature_lookup_tables(std::size_t table, std::string_view feature,
                                                     std::uint16_t want_type, std::uint16_t ext_type) const {
  std::vector<std::size_t> subtables;
  const std::size_t features = table + u16(table + 6);
  const std::size_t lookups = table + u16(table + 8);
  const std::uint16_t nfeat = u16(features);
  std::vector<std::uint16_t> lookup_indices;
  for (std::uint16_t i = 0; i < nfeat; ++i) {
    const std::size_t rec = features + 2 + static_cast<std::size_t>(i) * 6;
    if (!in_range(rec, 6) || std::memcmp(data_.data() + rec, feature.data(), 4) != 0) continue;
    const std::size_t f = features + u16(rec + 4);
    const std::uint16_t n = u16(f + 2);
    for (std::uint16_t k = 0; k < n; ++k) {
      const std::uint16_t li = u16(f + 4 + static_cast<std::size_t>(k) * 2);
      if (std::find(lookup_indices.begin(), lookup_indices.end(), li) == lookup_indices.end()) {
        lookup_indices.push_back(li);
      }
    }
  }
  std::sort(lookup_indices.begin(), lookup_indices.end());
  const std::uint16_t nlookups = u16(lookups);
  for (const std::uint16_t li : lookup_indices) {
    if (li >= nlookups) continue;
    const std::size_t lookup = lookups + u16(lookups + 2 + static_cast<std::size_t>(li) * 2);
    const std::uint16_t type = u16(lookup);
    const std::uint16_t nsub = u16(lookup + 4);
    for (std::uint16_t s = 0; s < nsub; ++s) {
      std::size_t sub = lookup + u16(lookup + 6 + static_cast<std::size_t>(s) * 2);
      if (type == ext_type) {
        if (u16(sub + 2) != want_type) continue;
        sub = sub + u32(sub + 4);
      } else if (type != want_type) {
        continue;
      }
      subtables.push_back(sub);
    }
  }
  return subtables;
}

bool Font::gpos_pair(std::size_t sub, std::uint16_t left, std::uint16_t right, int& value) const {
  const std::uint16_t format = u16(sub);
  const int ci = coverage_index(sub + u16(sub + 2), left);
  if (ci < 0) return false;
  const std::uint16_t vf1 = u16(sub + 4);
  const std::uint16_t vf2 = u16(sub + 6);
  const int size1 = std::popcount(static_cast<unsigned>(vf1)) * 2;
  const int size2 = std::popcount(static_cast<unsigned>(vf2)) * 2;
  const int xadv_off = std::popcount(static_cast<unsigned>(vf1 & 3u)) * 2;
  const bool has_xadv = (vf1 & 4u) != 0;
  if (format == 1) {
    const std::uint16_t nsets = u16(sub + 8);
    if (ci >= nsets) return false;
    const std::size_t set = sub + u16(sub + 10 + static_cast<std::size_t>(ci) * 2);
    const std::uint16_t count = u16(set);
    const std::size_t rec_size = 2 + static_cast<std::size_t>(size1 + size2);
    int lo = 0, hi = count - 1;
    while (lo <= hi) {
      const int mid = (lo + hi) / 2;
      const std::size_t rec = set + 2 + static_cast<std::size_t>(mid) * rec_size;
      const std::uint16_t g = u16(rec);
      if (g == right) {
        value = has_xadv ? s16(rec + 2 + static_cast<std::size_t>(xadv_off)) : 0;
        return true;
      }
      if (g < right) lo = mid + 1;
      else hi = mid - 1;
    }
    return false;
  }
  if (format == 2) {
    const std::size_t cd1 = sub + u16(sub + 8);
    const std::size_t cd2 = sub + u16(sub + 10);
    const std::uint16_t c1count = u16(sub + 12);
    const std::uint16_t c2count = u16(sub + 14);
    const int c1 = class_of(cd1, left);
    const int c2 = class_of(cd2, right);
    if (c1 >= c1count || c2 >= c2count) return false;
    const std::size_t rec_size = static_cast<std::size_t>(size1 + size2);
    const std::size_t rec = sub + 16 + (static_cast<std::size_t>(c1) * c2count + static_cast<std::size_t>(c2)) * rec_size;
    value = has_xadv ? s16(rec + static_cast<std::size_t>(xadv_off)) : 0;
    return true;
  }
  return false;
}

int Font::legacy_kern(std::uint16_t left, std::uint16_t right) const {
  if (!kern_.length) return 0;
  const std::size_t t = kern_.offset;
  const std::uint16_t ntables = u16(t + 2);
  std::size_t sub = t + 4;
  for (std::uint16_t i = 0; i < ntables; ++i) {
    const std::uint16_t length = u16(sub + 2);
    const std::uint16_t coverage = u16(sub + 4);
    if ((coverage >> 8) == 0 && (coverage & 1)) {
      const std::uint16_t npairs = u16(sub + 6);
      const std::uint32_t key = (static_cast<std::uint32_t>(left) << 16) | right;
      int lo = 0, hi = npairs - 1;
      while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const std::size_t rec = sub + 14 + static_cast<std::size_t>(mid) * 6;
        const std::uint32_t k = u32(rec);
        if (k == key) return s16(rec + 4);
        if (k < key) lo = mid + 1;
        else hi = mid - 1;
      }
    }
    sub += length;
  }
  return 0;
}

int Font::kerning(std::uint16_t left, std::uint16_t right) const {
  if (!valid_) return 0;
  const std::uint32_t key = (static_cast<std::uint32_t>(left) << 16) | right;
  {
    std::lock_guard lock(cache_mutex_);
    const auto it = kern_cache_.find(key);
    if (it != kern_cache_.end()) return it->second;
  }
  int value = 0;
  bool found = false;
  for (const std::size_t sub : gpos_kern_subtables_) {
    if (gpos_pair(sub, left, right, value)) {
      found = true;
      break;
    }
  }
  if (!found) value = legacy_kern(left, right);
  std::lock_guard lock(cache_mutex_);
  if (kern_cache_.size() > 65536) kern_cache_.clear();
  kern_cache_[key] = value;
  return value;
}

std::uint16_t Font::substitute(std::uint16_t glyph, std::string_view feature) const {
  if (!valid_ || !gsub_.length || feature.size() != 4) return glyph;
  const std::uint64_t key = (static_cast<std::uint64_t>(feature[0]) << 56) | (static_cast<std::uint64_t>(feature[1]) << 48) |
                            (static_cast<std::uint64_t>(feature[2]) << 40) | (static_cast<std::uint64_t>(feature[3]) << 32) |
                            glyph;
  {
    std::lock_guard lock(cache_mutex_);
    const auto it = subst_cache_.find(key);
    if (it != subst_cache_.end()) return it->second;
  }
  std::uint16_t out = glyph;
  for (const std::size_t sub : feature_lookup_tables(gsub_.offset, feature, 1, 7)) {
    const int ci = coverage_index(sub + u16(sub + 2), glyph);
    if (ci < 0) continue;
    const std::uint16_t format = u16(sub);
    if (format == 1) {
      out = static_cast<std::uint16_t>((glyph + s16(sub + 4)) & 0xFFFF);
    } else if (format == 2) {
      if (ci < u16(sub + 4)) out = u16(sub + 6 + static_cast<std::size_t>(ci) * 2);
    }
    break;
  }
  std::lock_guard lock(cache_mutex_);
  subst_cache_[key] = out;
  return out;
}

}  // namespace cvd::text
