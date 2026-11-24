// Convertible core - vector family: Type 1 font charstrings (eexec / charstring
// decryption and the Type 1 charstring interpreter, including flex and hint
// replacement other-subroutines and seac accented characters).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cstdint>
#include <string>
#include <vector>

#include "families/vector/ps_internal.hpp"

namespace cvt::vector::ps {

std::string decrypt(std::string_view data, std::uint16_t key, std::size_t skip) {
  std::string out;
  out.reserve(data.size() > skip ? data.size() - skip : 0);
  std::uint16_t r = key;
  for (std::size_t i = 0; i < data.size(); ++i) {
    const auto c = static_cast<unsigned char>(data[i]);
    const auto p = static_cast<unsigned char>(c ^ (r >> 8));
    r = static_cast<std::uint16_t>((c + r) * 52845u + 22719u);
    if (i >= skip) out.push_back(static_cast<char>(p));
  }
  return out;
}

namespace {

struct T1State {
  const std::vector<std::string>* subrs = nullptr;
  std::vector<double> stack;
  std::vector<double> ps_stack;  // results of callothersubr, consumed by `pop`
  Type1Glyph glyph;
  Point cur;
  bool open = false;
  bool flex = false;
  std::vector<Point> flex_points;
  Point flex_start;
  bool done = false;
  int ops = 0;
};

bool run(T1State& s, std::string_view cs, int depth, int depth_limit) {
  if (depth > depth_limit) return false;
  std::size_t i = 0;
  auto pop = [&]() {
    if (s.stack.empty()) return 0.0;
    const double v = s.stack.back();
    s.stack.pop_back();
    return v;
  };
  auto line_to = [&](Point p) {
    if (!s.open) {
      s.glyph.path.move_to(s.cur);
      s.open = true;
    }
    s.glyph.path.line_to(p);
    s.cur = p;
  };
  auto curve_to = [&](Point a, Point b, Point c) {
    if (!s.open) {
      s.glyph.path.move_to(s.cur);
      s.open = true;
    }
    s.glyph.path.cubic_to(a, b, c);
    s.cur = c;
  };
  auto move_to = [&](Point p) {
    if (s.flex) {
      s.flex_points.push_back(p);
      s.cur = p;
      return;
    }
    if (s.open) s.glyph.path.close();
    s.open = false;
    s.cur = p;
  };
  while (i < cs.size() && !s.done) {
    if (++s.ops > 200000) return false;
    const int v = static_cast<unsigned char>(cs[i++]);
    if (v >= 32) {
      double num = 0;
      if (v <= 246) {
        num = v - 139;
      } else if (v <= 250) {
        if (i >= cs.size()) return false;
        num = (v - 247) * 256 + static_cast<unsigned char>(cs[i++]) + 108;
      } else if (v <= 254) {
        if (i >= cs.size()) return false;
        num = -(v - 251) * 256 - static_cast<unsigned char>(cs[i++]) - 108;
      } else {
        if (i + 4 > cs.size()) return false;
        const std::uint32_t u = (static_cast<std::uint32_t>(static_cast<unsigned char>(cs[i])) << 24) |
                                (static_cast<std::uint32_t>(static_cast<unsigned char>(cs[i + 1])) << 16) |
                                (static_cast<std::uint32_t>(static_cast<unsigned char>(cs[i + 2])) << 8) |
                                static_cast<std::uint32_t>(static_cast<unsigned char>(cs[i + 3]));
        i += 4;
        num = static_cast<std::int32_t>(u);
      }
      if (s.stack.size() >= 64) return false;
      s.stack.push_back(num);
      continue;
    }
    switch (v) {
      case 1:   // hstem
      case 3:   // vstem
        s.stack.clear();
        break;
      case 4: {  // vmoveto
        const double dy = pop();
        move_to(s.cur + Point{0, dy});
        s.stack.clear();
        break;
      }
      case 5: {  // rlineto
        const double dy = pop(), dx = pop();
        line_to(s.cur + Point{dx, dy});
        s.stack.clear();
        break;
      }
      case 6: {  // hlineto
        const double dx = pop();
        line_to(s.cur + Point{dx, 0});
        s.stack.clear();
        break;
      }
      case 7: {  // vlineto
        const double dy = pop();
        line_to(s.cur + Point{0, dy});
        s.stack.clear();
        break;
      }
      case 8: {  // rrcurveto
        const double dy3 = pop(), dx3 = pop(), dy2 = pop(), dx2 = pop(), dy1 = pop(), dx1 = pop();
        const Point a = s.cur + Point{dx1, dy1};
        const Point b = a + Point{dx2, dy2};
        const Point c = b + Point{dx3, dy3};
        curve_to(a, b, c);
        s.stack.clear();
        break;
      }
      case 9:  // closepath
        if (s.open) s.glyph.path.close();
        s.open = false;
        s.stack.clear();
        break;
      case 10: {  // callsubr
        const int n = static_cast<int>(pop());
        if (s.subrs == nullptr || n < 0 || static_cast<std::size_t>(n) >= s.subrs->size()) return false;
        if (!run(s, (*s.subrs)[static_cast<std::size_t>(n)], depth + 1, depth_limit)) return false;
        break;
      }
      case 11:  // return
        return true;
      case 12: {
        if (i >= cs.size()) return false;
        const int e = static_cast<unsigned char>(cs[i++]);
        switch (e) {
          case 0:  // dotsection
          case 1:  // vstem3
          case 2:  // hstem3
            s.stack.clear();
            break;
          case 6: {  // seac
            const double achar = pop(), bchar = pop(), ady = pop(), adx = pop(), asb = pop();
            s.glyph.seac_base = static_cast<int>(bchar);
            s.glyph.seac_accent = static_cast<int>(achar);
            s.glyph.seac_offset = {adx - asb + s.glyph.sidebearing.x, ady};
            s.done = true;
            s.stack.clear();
            break;
          }
          case 7: {  // sbw
            const double wy = pop(), wx = pop(), sby = pop(), sbx = pop();
            s.glyph.sidebearing = {sbx, sby};
            s.glyph.advance = {wx, wy};
            s.cur = {sbx, sby};
            s.stack.clear();
            break;
          }
          case 12: {  // div
            const double b = pop(), a = pop();
            s.stack.push_back(b != 0 ? a / b : 0.0);
            break;
          }
          case 16: {  // callothersubr
            const int othersubr = static_cast<int>(pop());
            const int n = static_cast<int>(pop());
            if (n < 0 || n > 16) return false;
            std::vector<double> args;
            for (int k = 0; k < n; ++k) args.push_back(pop());  // args[0] = last pushed
            switch (othersubr) {
              case 0: {  // flex end: flexheight x y
                s.flex = false;
                // flex_points: reference point then 6 curve points.
                if (s.flex_points.size() >= 7) {
                  const auto& f = s.flex_points;
                  s.cur = s.flex_start;
                  curve_to(f[1], f[2], f[3]);
                  curve_to(f[4], f[5], f[6]);
                }
                s.flex_points.clear();
                // Results for the following `pop pop setcurrentpoint`.
                const double y = n >= 1 ? args[0] : s.cur.y;
                const double x = n >= 2 ? args[1] : s.cur.x;
                s.ps_stack.push_back(y);
                s.ps_stack.push_back(x);
                break;
              }
              case 1:  // flex start
                s.flex = true;
                s.flex_points.clear();
                s.flex_start = s.cur;
                break;
              case 2:  // flex point (added by rmoveto)
                break;
              case 3:  // hint replacement: return the subr number
                s.ps_stack.push_back(n >= 1 ? args[0] : 3.0);
                break;
              default:
                for (int k = n - 1; k >= 0; --k) s.ps_stack.push_back(args[static_cast<std::size_t>(k)]);
                break;
            }
            break;
          }
          case 17: {  // pop
            if (!s.ps_stack.empty()) {
              s.stack.push_back(s.ps_stack.back());
              s.ps_stack.pop_back();
            } else {
              s.stack.push_back(0);
            }
            break;
          }
          case 33: {  // setcurrentpoint
            const double y = pop(), x = pop();
            s.cur = {x, y};
            s.stack.clear();
            break;
          }
          default: s.stack.clear(); break;
        }
        break;
      }
      case 13: {  // hsbw
        const double wx = pop(), sbx = pop();
        s.glyph.sidebearing = {sbx, 0};
        s.glyph.advance = {wx, 0};
        s.cur = {sbx, 0};
        s.stack.clear();
        break;
      }
      case 14:  // endchar
        if (s.open) s.glyph.path.close();
        s.open = false;
        s.done = true;
        break;
      case 21: {  // rmoveto
        const double dy = pop(), dx = pop();
        move_to(s.cur + Point{dx, dy});
        s.stack.clear();
        break;
      }
      case 22: {  // hmoveto
        const double dx = pop();
        move_to(s.cur + Point{dx, 0});
        s.stack.clear();
        break;
      }
      case 30: {  // vhcurveto
        const double dx3 = pop(), dy2 = pop(), dx2 = pop(), dy1 = pop();
        const Point a = s.cur + Point{0, dy1};
        const Point b = a + Point{dx2, dy2};
        const Point c = b + Point{dx3, 0};
        curve_to(a, b, c);
        s.stack.clear();
        break;
      }
      case 31: {  // hvcurveto
        const double dy3 = pop(), dy2 = pop(), dx2 = pop(), dx1 = pop();
        const Point a = s.cur + Point{dx1, 0};
        const Point b = a + Point{dx2, dy2};
        const Point c = b + Point{0, dy3};
        curve_to(a, b, c);
        s.stack.clear();
        break;
      }
      default: s.stack.clear(); break;
    }
  }
  return true;
}

}  // namespace

Type1Glyph run_type1_charstring(std::string_view cs, const std::vector<std::string>& subrs, int depth_limit) {
  T1State s;
  s.subrs = &subrs;
  const bool ok = run(s, cs, 0, depth_limit);
  if (s.open) s.glyph.path.close();
  s.glyph.ok = ok;
  return std::move(s.glyph);
}

}  // namespace cvt::vector::ps
