// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// PostScript fonts: Type 1 charstrings, Type 3, text operators.
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "engine/registry.hpp"
#include "families/vector/geom.hpp"
#include "families/vector/ps_internal.hpp"
#include "families/vector/vector_codecs.hpp"
#include "support/test_support.hpp"

using namespace cvt;
using namespace cvt::vector;

namespace {

Context test_context() {
  Context ctx;
  ctx.registry = &Registry::instance();
  return ctx;
}

std::string cs_num(int v) {
  std::string s;
  if (v >= -107 && v <= 107) {
    s.push_back(static_cast<char>(v + 139));
  } else if (v >= 108 && v <= 1131) {
    s.push_back(static_cast<char>((v - 108) / 256 + 247));
    s.push_back(static_cast<char>((v - 108) % 256));
  } else {
    s.push_back(static_cast<char>((-v - 108) / 256 + 251));
    s.push_back(static_cast<char>((-v - 108) % 256));
  }
  return s;
}
std::string op(int o) { return std::string(1, static_cast<char>(o)); }
std::string op12(int o) { return std::string("\x0c") + static_cast<char>(o); }

Result<Scene> run(std::string_view body) {
  const std::string program = "%!PS-Adobe-3.0 EPSF-3.0\n%%BoundingBox: 0 0 1000 1000\n" + std::string(body) + "\n";
  return read_scene("eps", as_bytes(program), test_context());
}

}  // namespace

TEST_CASE("vector/ps: Type 1 charstring interpreter") {
  using ps::run_type1_charstring;
  SUBCASE("hsbw, lines, curves and closepath") {
    const std::string cs = cs_num(20) + cs_num(500) + op(13) + cs_num(0) + cs_num(0) + op(21) + cs_num(300) + op(6) +
                           cs_num(100) + cs_num(0) + cs_num(100) + cs_num(100) + cs_num(0) + cs_num(100) + op(8) +
                           op(9) + op(14);
    const auto g = run_type1_charstring(cs, {});
    REQUIRE(g.ok);
    CHECK_EQ(g.advance.x, 500.0);
    const Rect b = bounds(g.path);
    CHECK_EQ(b.x0, 20.0);
    CHECK_EQ(b.x1, 520.0);
    CHECK_EQ(b.y1, 200.0);
  }
  SUBCASE("subroutines, flex and hint replacement") {
    // Subrs 0-3 as in the standard Type 1 boilerplate (flex and hint replacement).
    const std::vector<std::string> subrs = {
        cs_num(3) + cs_num(0) + op12(16) + op12(17) + op12(17) + op12(33) + op(11),  // 0: flex end
        cs_num(0) + cs_num(1) + op12(16) + op(11),                                    // 1: flex start
        cs_num(0) + cs_num(2) + op12(16) + op(11),                                    // 2: flex point
        cs_num(3) + op(11),                                                           // 3: hint replacement
        cs_num(50) + op(6) + op(11),                                                  // 4: 50 hlineto
    };
    std::string cs = cs_num(0) + cs_num(600) + op(13) + cs_num(0) + cs_num(0) + op(21);
    // Hint replacement: 4 1 3 callothersubr pop callsubr
    cs += cs_num(4) + cs_num(1) + cs_num(3) + op12(16) + op12(17) + op(10);
    // Flex from (50,0): reference point and six points, then the end.
    cs += cs_num(1) + op(10);
    const int pts[7][2] = {{25, 5}, {-15, -5}, {15, 0}, {25, 0}, {25, 0}, {25, 0}, {0, 0}};
    for (const auto& pt : pts) cs += cs_num(pt[0]) + cs_num(pt[1]) + op(21) + cs_num(2) + op(10);
    cs += cs_num(50) + cs_num(150) + cs_num(0) + cs_num(0) + op(10);
    cs += op(9) + op(14);
    const auto g = run_type1_charstring(cs, subrs);
    REQUIRE(g.ok);
    // Line to x 50, then two curves ending at x 150, closepath.
    std::size_t cubics = 0;
    for (const Verb v : g.path.verbs) cubics += v == Verb::Cubic ? 1 : 0;
    CHECK_EQ(cubics, 2u);
    const Point end = g.path.points.back();
    CHECK_EQ(end.x, 150.0);
    CHECK_EQ(end.y, 0.0);
  }
  SUBCASE("seac and hostile charstrings") {
    const std::string seac = cs_num(10) + cs_num(500) + op(13) + cs_num(0) + cs_num(100) + cs_num(200) + cs_num(65) +
                             cs_num(194) + op12(6);
    const auto g = run_type1_charstring(seac, {});
    CHECK_EQ(g.seac_base, 65);
    CHECK_EQ(g.seac_accent, 194);
    CHECK_EQ(g.seac_offset.x, 110.0);
    // Recursive subroutine: bounded.
    const std::vector<std::string> loop = {cs_num(0) + op(10)};
    CHECK_FALSE(run_type1_charstring(cs_num(0) + op(10), loop).ok);
    // Truncated numbers and garbage never crash.
    for (int i = 0; i < 256; ++i) (void)run_type1_charstring(std::string(1, static_cast<char>(i)) + "\xff\xff", {});
  }
  CHECK_EQ(ps::decrypt(ps::decrypt("abcd", 4330, 0), 4330, 0).size(), 4u);
}

TEST_CASE("vector/ps: text operators") {
  auto scene = run("/Helvetica findfont 20 scalefont setfont\n"
                   "10 10 moveto (AB) show currentpoint /y exch def /x exch def\n"
                   "x y moveto (A) stringwidth pop 0 rmoveto (C) show\n"
                   "100 100 moveto 5 0 (abc) ashow\n"
                   "200 200 moveto 10 0 32 (a b) widthshow\n"
                   "300 300 moveto (xy) [30 0 30 0] xyshow\n"
                   "400 400 moveto /Lslash glyphshow\n"
                   "newpath 500 500 moveto (O) true charpath fill\n"
                   "/Times-Italic 12 selectfont 600 600 moveto (it) show\n"
                   "700 700 moveto { pop pop 3 0 rmoveto } (kk) kshow");
  REQUIRE_OK(scene);
  std::vector<const Text*> texts;
  std::vector<Matrix> placements;
  int shapes = 0;
  std::vector<const Group*> todo{&scene->pages[0].root};
  std::vector<Matrix> mats{Matrix{}};
  while (!todo.empty()) {
    const Group* g = todo.back();
    const Matrix m = mats.back() * g->transform;
    todo.pop_back();
    mats.pop_back();
    for (const Node& n : g->children) {
      if (const auto* t = std::get_if<Text>(&n.value)) {
        texts.push_back(t);
        placements.push_back(m);
      } else if (std::holds_alternative<Shape>(n.value)) {
        ++shapes;
      } else if (const auto* sub = std::get_if<Group>(&n.value)) {
        todo.push_back(sub);
        mats.push_back(m);
      }
    }
  }
  std::string all;
  for (const Text* t : texts) all += t->content + "|";
  for (const char* expect : {"AB|", "C|", "abc|", "a b|", "xy|", "\xC5\x81|", "it|", "kk|"}) {
    CHECK_MESSAGE(all.find(expect) != std::string::npos, expect, " in ", all);
  }
  CHECK_EQ(shapes, 1);  // the charpath outline
  for (std::size_t i = 0; i < texts.size(); ++i) {
    if (texts[i]->content == "xy") CHECK_APPROX(texts[i]->advance, 60.0, 1e-6);  // xyshow displacements
    if (texts[i]->content == "it") {
      CHECK(texts[i]->font.italic);
      CHECK(texts[i]->font.family.find("serif") != std::string::npos);
      CHECK_APPROX(texts[i]->font.size, 12.0, 1e-9);
    }
    if (texts[i]->content == "AB") CHECK_APPROX(texts[i]->font.size, 20.0, 1e-9);
  }
  // "C" starts where "AB" plus one "A" ended: stringwidth agrees with show.
  const Text* ab = nullptr;
  const Text* c = nullptr;
  Matrix ab_m, c_m;
  for (std::size_t i = 0; i < texts.size(); ++i) {
    if (texts[i]->content == "AB") {
      ab = texts[i];
      ab_m = placements[i];
    }
    if (texts[i]->content == "C") {
      c = texts[i];
      c_m = placements[i];
    }
  }
  REQUIRE(ab != nullptr);
  REQUIRE(c != nullptr);
  const double ab_end = ab_m.apply({ab->advance, 0}).x;
  const double c_start = c_m.apply({0, 0}).x;
  CHECK_GT(c_start - ab_end, 5.0);
}

TEST_CASE("vector/ps: Type 3 font with BuildGlyph and makefont") {
  auto scene = run("/T3 << /FontType 3 /FontMatrix [0.01 0 0 0.01 0 0] /FontBBox [0 0 100 100]\n"
                   "  /Encoding [ 65 { /.notdef } repeat /square ] /CharProcs << /square { 0 0 80 80 rectfill } >>\n"
                   "  /BuildGlyph { 100 0 0 0 80 80 setcachedevice exch /CharProcs get exch get exec }\n"
                   "  /BuildChar { 1 index /Encoding get exch get 1 index /BuildGlyph get exec } >> definefont\n"
                   "[20 0 0 40 0 0] makefont setfont 100 100 moveto (AA) show");
  REQUIRE_OK(scene);
  std::vector<Rect> squares;
  std::vector<const Group*> todo{&scene->pages[0].root};
  std::string content;
  while (!todo.empty()) {
    const Group* g = todo.back();
    todo.pop_back();
    for (const Node& n : g->children) {
      if (const auto* s = std::get_if<Shape>(&n.value)) squares.push_back(bounds(s->path));
      if (const auto* t = std::get_if<Text>(&n.value)) content += t->content;
      if (const auto* sub = std::get_if<Group>(&n.value)) todo.push_back(sub);
    }
  }
  REQUIRE_EQ(squares.size(), 2u);
  CHECK_EQ(content, "AA");
  // 0.8 em wide (16 pt at 20 pt), 0.8 em tall at 40 pt, advancing 20 pt.
  std::sort(squares.begin(), squares.end(), [](const Rect& a, const Rect& b) { return a.x0 < b.x0; });
  CHECK_APPROX(squares[0].x0, 100.0, 1e-9);
  CHECK_APPROX(squares[0].width(), 16.0, 1e-9);
  CHECK_APPROX(squares[0].height(), 32.0, 1e-9);
  CHECK_APPROX(squares[1].x0, 120.0, 1e-9);
}
