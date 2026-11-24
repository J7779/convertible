// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// PostScript interpreter: operator conformance and bounded execution.
#include <cmath>
#include <string>
#include <vector>

#include "engine/registry.hpp"
#include "families/vector/geom.hpp"
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

std::string eps(std::string_view body) {
  return "%!PS-Adobe-3.0 EPSF-3.0\n%%BoundingBox: 0 0 1000 1000\n%%EndComments\n" + std::string(body) + "\n";
}

void collect(const Group& g, const Matrix& m, std::vector<std::pair<const Node*, Matrix>>& out) {
  const Matrix t = m * g.transform;
  for (const Node& n : g.children) {
    if (const auto* sub = std::get_if<Group>(&n.value)) collect(*sub, t, out);
    else out.emplace_back(&n, t);
  }
}

/// All leaf nodes of the first page with their transform relative to the
/// page root content (device space = PostScript points, y up).
std::vector<std::pair<const Node*, Matrix>> leaves(const Scene& s) {
  std::vector<std::pair<const Node*, Matrix>> out;
  for (const Node& n : s.pages.at(0).root.children) {
    if (const auto* g = std::get_if<Group>(&n.value)) collect(*g, Matrix{}, out);
    else out.emplace_back(&n, Matrix{});
  }
  return out;
}

/// Run `expr`, which must leave one number on the stack, and read it back
/// from the width of a rectangle the program draws.
double eval_number(std::string_view expr) {
  const std::string program = eps(std::string(expr) +
                                  "\n/r exch def initmatrix newpath 0 0 moveto r 0 rlineto 0 1 rlineto -1 r mul 0 rlineto closepath fill");
  auto scene = read_scene("eps", as_bytes(program), test_context());
  if (!scene) return std::nan("");
  const auto items = leaves(*scene);
  if (items.empty()) return std::nan("");
  const auto* s = std::get_if<Shape>(&items[0].first->value);
  if (s == nullptr) return std::nan("");
  const Rect r = bounds(s->path, items[0].second);
  return s->path.points.size() >= 2 ? s->path.points[1].x : r.width();
}

/// Run `expr`, which must leave a boolean, and report it.
bool eval_bool(std::string_view expr) {
  return eval_number(std::string(expr) + " { 1 } { 0 } ifelse") == 1.0;
}

/// Run `expr`, which must leave a string; read it back from the text drawn.
std::string eval_string(std::string_view expr) {
  const std::string program =
      eps("/Helvetica findfont 10 scalefont setfont 10 10 moveto " + std::string(expr) + " show");
  auto scene = read_scene("eps", as_bytes(program), test_context());
  if (!scene) return "<error>";
  for (const auto& [n, m] : leaves(*scene)) {
    if (const auto* t = std::get_if<Text>(&n->value)) return t->content;
  }
  return "<none>";
}

Result<Scene> run(std::string_view body, std::string_view options = {}, const Limits& limits = {}) {
  auto opts = Options::parse(options);
  if (!opts) return fail(opts.error());
  Context ctx = test_context();
  ctx.limits = limits;
  return read_scene("eps", as_bytes(eps(body)), ctx, *opts);
}

}  // namespace

TEST_CASE("vector/ps: arithmetic operators") {
  CHECK_EQ(eval_number("3 4 add"), 7.0);
  CHECK_EQ(eval_number("10 4 sub"), 6.0);
  CHECK_EQ(eval_number("6 7 mul"), 42.0);
  CHECK_APPROX(eval_number("7 2 div"), 3.5, 1e-9);
  CHECK_EQ(eval_number("7 2 idiv"), 3.0);
  CHECK_EQ(eval_number("-7 2 idiv"), -3.0);
  CHECK_EQ(eval_number("7 3 mod"), 1.0);
  CHECK_EQ(eval_number("-7 3 mod"), -1.0);
  CHECK_EQ(eval_number("-5 abs"), 5.0);
  CHECK_EQ(eval_number("5 neg"), -5.0);
  CHECK_EQ(eval_number("3.2 ceiling"), 4.0);
  CHECK_EQ(eval_number("3.7 floor"), 3.0);
  CHECK_EQ(eval_number("2.5 round"), 3.0);
  CHECK_EQ(eval_number("-2.5 round"), -2.0);
  CHECK_EQ(eval_number("-3.7 truncate"), -3.0);
  CHECK_APPROX(eval_number("16 sqrt"), 4.0, 1e-9);
  CHECK_APPROX(eval_number("0 1 atan"), 0.0, 1e-9);
  CHECK_APPROX(eval_number("1 0 atan"), 90.0, 1e-9);
  CHECK_APPROX(eval_number("-1 0 atan"), 270.0, 1e-9);
  CHECK_APPROX(eval_number("60 cos"), 0.5, 1e-9);
  CHECK_APPROX(eval_number("30 sin"), 0.5, 1e-9);
  CHECK_APPROX(eval_number("2 10 exp"), 1024.0, 1e-9);
  CHECK_APPROX(eval_number("100 log"), 2.0, 1e-9);
  CHECK_APPROX(eval_number("1 ln"), 0.0, 1e-9);
  // Integer overflow turns into a real.
  CHECK_APPROX(eval_number("2147483647 1 add 1000000 div"), 2147.483648, 1e-6);
  // Radix numbers and reals.
  CHECK_EQ(eval_number("16#FF"), 255.0);
  CHECK_EQ(eval_number("8#777"), 511.0);
  CHECK_EQ(eval_number("2#1010"), 10.0);
  CHECK_APPROX(eval_number("1.5e2"), 150.0, 1e-9);
  CHECK_APPROX(eval_number(".5 2 mul"), 1.0, 1e-9);
  CHECK_APPROX(eval_number("-.25 -4 mul"), 1.0, 1e-9);
}

TEST_CASE("vector/ps: bit, relational and boolean operators") {
  CHECK_EQ(eval_number("12 10 and"), 8.0);
  CHECK_EQ(eval_number("12 3 or"), 15.0);
  CHECK_EQ(eval_number("12 10 xor"), 6.0);
  CHECK_EQ(eval_number("0 not"), -1.0);
  CHECK_EQ(eval_number("1 4 bitshift"), 16.0);
  CHECK_EQ(eval_number("256 -4 bitshift"), 16.0);
  CHECK(eval_bool("1 1.0 eq"));
  CHECK(eval_bool("(abc) /abc eq"));
  CHECK(eval_bool("(abc) (abd) lt"));
  CHECK(eval_bool("3 2 gt"));
  CHECK(eval_bool("2 2 ge"));
  CHECK_FALSE(eval_bool("2 3 ge"));
  CHECK(eval_bool("true false or"));
  CHECK_FALSE(eval_bool("true false and"));
  CHECK(eval_bool("true false xor"));
  CHECK(eval_bool("false not"));
  CHECK(eval_bool("[1 2] dup eq"));
  CHECK_FALSE(eval_bool("[1 2] [1 2] eq"));
}

TEST_CASE("vector/ps: stack operators") {
  CHECK_EQ(eval_number("1 2 exch pop"), 2.0);
  CHECK_EQ(eval_number("1 2 3 3 copy add add add add add"), 12.0);
  CHECK_EQ(eval_number("5 6 7 2 index"), 5.0);
  CHECK_EQ(eval_number("1 2 3 3 1 roll pop pop"), 3.0);
  CHECK_EQ(eval_number("1 2 3 3 -1 roll"), 1.0);
  CHECK_EQ(eval_number("1 2 3 count"), 3.0);
  CHECK_EQ(eval_number("9 mark 1 2 3 counttomark exch pop exch pop exch pop exch pop"), 3.0);
  CHECK_EQ(eval_number("9 mark 1 2 3 cleartomark"), 9.0);
  CHECK_EQ(eval_number("1 2 clear 5"), 5.0);
}

TEST_CASE("vector/ps: control flow") {
  CHECK_EQ(eval_number("0 1 1 10 { add } for"), 55.0);
  CHECK_EQ(eval_number("0 10 -2 0 { add } for"), 30.0);
  CHECK_APPROX(eval_number("0 0 0.5 2 { add } for"), 5.0, 1e-9);
  CHECK_EQ(eval_number("1 5 { 2 mul } repeat"), 32.0);
  CHECK_EQ(eval_number("0 { 1 add dup 7 eq { exit } if } loop"), 7.0);
  CHECK_EQ(eval_number("true { 1 } { 2 } ifelse"), 1.0);
  CHECK_EQ(eval_number("false { 1 } { 2 } ifelse"), 2.0);
  CHECK_EQ(eval_number("0 [1 2 3 4] { add } forall"), 10.0);
  CHECK_EQ(eval_number("0 (ABC) { add } forall"), 198.0);
  CHECK_EQ(eval_number("0 << /a 1 /b 2 /c 3 >> { exch pop add } forall"), 6.0);
  CHECK_EQ(eval_number("{ 1 2 3 } exec add add"), 6.0);
  CHECK_EQ(eval_number("(3 4 add) cvx exec"), 7.0);
  // exit leaves the innermost loop only.
  CHECK_EQ(eval_number("0 1 1 3 { pop 1 1 100 { pop 1 add exit } for } for"), 3.0);
  // stop / stopped, and errors caught by stopped.
  CHECK_EQ(eval_number("{ 1 stop 2 } stopped { 10 } { 20 } ifelse add"), 11.0);
  CHECK_EQ(eval_number("{ 1 0 idiv } stopped { pop pop 5 } { 6 } ifelse"), 5.0);
  CHECK_EQ(eval_number("{ undefinedname } stopped { 8 } { 9 } ifelse"), 8.0);
  CHECK(eval_bool("{ /x 1 0 idiv } stopped pop $error /errorname get /undefinedresult eq"));
  CHECK_EQ(eval_number("{ 1 2 } stopped pop add"), 3.0);
}

TEST_CASE("vector/ps: arrays and strings") {
  CHECK_EQ(eval_number("[1 2 3] length"), 3.0);
  CHECK_EQ(eval_number("[1 2 3] 1 get"), 2.0);
  CHECK_EQ(eval_number("3 array dup 1 7 put 1 get"), 7.0);
  CHECK_EQ(eval_number("[1 2 3 4 5] 1 3 getinterval 2 get"), 4.0);
  CHECK_EQ(eval_number("[1 2 3 4 5] dup 1 2 getinterval 0 9 put 1 get"), 9.0);  // shared storage
  CHECK_EQ(eval_number("5 array dup 2 [7 8] putinterval 3 get"), 8.0);
  CHECK_EQ(eval_number("[4 5 6] aload pop add add"), 15.0);
  CHECK_EQ(eval_number("1 2 3 3 array astore 2 get"), 3.0);
  CHECK_EQ(eval_number("[1 2] 3 array copy length"), 2.0);
  CHECK_EQ(eval_number("1 2 3 3 packedarray 0 get"), 1.0);
  CHECK_EQ(eval_number("(hello) length"), 5.0);
  CHECK_EQ(eval_number("(hello) 1 get"), 101.0);
  CHECK_EQ(eval_string("(hello) 1 3 getinterval"), "ell");
  CHECK_EQ(eval_string("5 string dup 0 (abcde) putinterval dup 0 72 put"), "Hbcde");
  CHECK_EQ(eval_string("(a\\(b\\)c\\101\\n) 0 5 getinterval"), "a(b)c");
  CHECK_EQ(eval_string("<48656c6C6f>"), "Hello");
  CHECK_EQ(eval_string("<~87cURD]j7BEbo7~>"), "Hello world");
  CHECK_EQ(eval_string("(abcdef) (cd) search pop exch pop exch pop"), "ab");
  CHECK_EQ(eval_string("(abcdef) (cd) search pop pop pop"), "ef");
  CHECK(eval_bool("(abcdef) (ab) anchorsearch exch pop exch pop"));
  CHECK_EQ(eval_number("(  42 rest) token pop exch pop"), 42.0);
  CHECK_EQ(eval_string("(  42 rest) token pop pop"), "rest");
  CHECK_EQ(eval_string("123 10 string cvs"), "123");
  CHECK_EQ(eval_string("2.5 10 string cvs"), "2.5");
  CHECK_EQ(eval_string("/name 10 string cvs"), "name");
  CHECK_EQ(eval_string("255 16 10 string cvrs"), "FF");
  CHECK_EQ(eval_number("(12.7) cvi"), 12.0);
  CHECK_APPROX(eval_number("(1.25) cvr"), 1.25, 1e-9);
  CHECK_EQ(eval_string("(xyz) cvn 10 string cvs"), "xyz");
}

TEST_CASE("vector/ps: dictionaries and types") {
  CHECK_EQ(eval_number("/a 5 def a"), 5.0);
  CHECK_EQ(eval_number("1 dict begin /a 3 def /a load end"), 3.0);
  CHECK_EQ(eval_number("/a 1 def 1 dict begin /a 2 def end a"), 1.0);
  CHECK_EQ(eval_number("/a 1 def 1 dict begin /a 2 store end a"), 2.0);
  CHECK(eval_bool("<< /k 1 >> /k known"));
  CHECK_FALSE(eval_bool("<< /k 1 >> dup /k undef /k known"));
  CHECK(eval_bool("/moveto where { systemdict eq } { false } ifelse"));
  CHECK_FALSE(eval_bool("/nosuchname where"));
  CHECK_EQ(eval_number("<< 1 10 2.0 20 (s) 30 >> 2 get"), 20.0);  // integral real keys equal integers
  CHECK_EQ(eval_number("<< /s 30 >> (s) get"), 30.0);              // string keys are names
  CHECK_EQ(eval_number("<< /a 1 /b 2 >> length"), 2.0);
  CHECK_EQ(eval_number("countdictstack"), 3.0);
  CHECK(eval_bool("currentdict userdict eq"));
  CHECK(eval_bool("1 type /integertype eq"));
  CHECK(eval_bool("1.0 type /realtype eq"));
  CHECK(eval_bool("(s) type /stringtype eq"));
  CHECK(eval_bool("{ } xcheck"));
  CHECK_FALSE(eval_bool("{ } cvlit xcheck"));
  CHECK(eval_bool("/x cvx xcheck"));
  CHECK(eval_bool("[1] readonly wcheck not"));
  CHECK(eval_bool("null type /nulltype eq"));
  CHECK_EQ(eval_number("/f { 2 mul } bind def 21 f"), 42.0);
  CHECK(eval_bool("{ add } bind 0 get type /operatortype eq"));
  CHECK_EQ(eval_number("/v 5 def /w //v def 1 w add"), 6.0);  // immediately evaluated name
}

TEST_CASE("vector/ps: save and restore undo VM changes") {
  CHECK_EQ(eval_number("/a 1 def save /a 2 def restore a"), 1.0);
  CHECK_EQ(eval_number("/arr [1 2 3] def save arr 0 9 put restore arr 0 get"), 1.0);
  CHECK_EQ(eval_number("/s (abc) def save s 0 65 put restore s 0 get"), 97.0);
  CHECK_EQ(eval_number("save /n 7 def restore /n where { pop 1 } { 0 } ifelse"), 0.0);
  CHECK_EQ(eval_number("2 setlinewidth save 9 setlinewidth restore currentlinewidth"), 2.0);
  // Objects created after the save keep their changes until restored.
  CHECK_EQ(eval_number("save /b [0] def b 0 5 put b 0 get exch restore"), 5.0);
}

TEST_CASE("vector/ps: graphics state and matrices") {
  CHECK_EQ(eval_number("2 3 scale 10 0 transform pop"), 20.0);
  CHECK_EQ(eval_number("100 50 translate 0 0 transform exch pop"), 50.0);
  CHECK_APPROX(eval_number("90 rotate 1 0 transform exch pop"), 1.0, 1e-9);
  CHECK_APPROX(eval_number("2 2 scale 10 10 itransform pop"), 5.0, 1e-9);
  CHECK_APPROX(eval_number("3 3 scale 1 1 dtransform pop"), 3.0, 1e-9);
  CHECK_APPROX(eval_number("[2 0 0 2 5 5] matrix invertmatrix 0 get"), 0.5, 1e-9);
  CHECK_APPROX(eval_number("[1 0 0 1 3 0] [2 0 0 2 0 0] matrix concatmatrix 4 get"), 6.0, 1e-9);
  CHECK_EQ(eval_number("matrix currentmatrix 0 get"), 1.0);
  CHECK_EQ(eval_number("gsave 5 setlinewidth grestore currentlinewidth"), 1.0);
  CHECK_EQ(eval_number("3 setlinecap currentlinecap"), 0.0);
  CHECK_EQ(eval_number("1 setlinejoin currentlinejoin"), 1.0);
  CHECK_APPROX(eval_number("0.25 setgray currentgray"), 0.25, 1e-6);
  CHECK_APPROX(eval_number("1 0 0 setrgbcolor currentrgbcolor pop pop"), 1.0, 1e-6);
  CHECK_APPROX(eval_number("0 1 1 sethsbcolor currentrgbcolor pop pop"), 1.0, 1e-6);
  CHECK_APPROX(eval_number("0 0 0 1 setcmykcolor currentgray"), 0.0, 1e-6);
  CHECK_APPROX(eval_number("10 20 moveto 5 5 rlineto currentpoint pop"), 15.0, 1e-9);
  CHECK_APPROX(eval_number("0 0 moveto 10 0 lineto 10 10 lineto pathbbox pop pop pop"), 0.0, 1e-9);
  CHECK_APPROX(eval_number("0 0 moveto 10 0 lineto 10 20 lineto pathbbox pop exch pop exch pop"), 10.0, 1e-9);
  CHECK_EQ(eval_number("[3 1] 2 setdash currentdash"), 2.0);
  CHECK(eval_bool("{ 0 0 lineto } stopped"));  // nocurrentpoint
}

TEST_CASE("vector/ps: path construction and painting") {
  auto scene = run("newpath 100 100 50 0 90 arc 100 100 lineto closepath 1 0 0 setrgbcolor fill\n"
                   "0 0 1 setrgbcolor 3 setlinewidth 1 setlinecap 200 200 moveto 50 0 rlineto 0 50 rlineto stroke\n"
                   "newpath 300 300 moveto 400 300 400 400 300 400 curveto closepath eofill\n"
                   "newpath 500 500 moveto 600 500 600 600 30 arcto 4 { pop } repeat stroke\n"
                   "[10 20 30 40] rectfill 0.5 setgray [50 60 70 80 90 100 10 10] rectstroke");
  REQUIRE_OK(scene);
  const auto items = leaves(*scene);
  REQUIRE_EQ(items.size(), 6u);
  const auto* pie = std::get_if<Shape>(&items[0].first->value);
  REQUIRE(pie != nullptr);
  const Rect pb = bounds(pie->path);
  CHECK_APPROX(pb.x0, 100.0, 0.01);
  CHECK_APPROX(pb.x1, 150.0, 0.01);
  CHECK_APPROX(pb.y1, 150.0, 0.01);
  CHECK_EQ(pie->fill.color.r, 1.0f);
  const auto* line = std::get_if<Shape>(&items[1].first->value);
  REQUIRE(line != nullptr);
  CHECK(line->fill.type == Paint::Type::None);
  CHECK_EQ(line->stroke.width, 3.0);
  CHECK(line->stroke.cap == LineCap::Round);
  CHECK_EQ(line->stroke.paint.color.b, 1.0f);
  const auto* eo = std::get_if<Shape>(&items[2].first->value);
  REQUIRE(eo != nullptr);
  CHECK(eo->fill_rule == FillRule::EvenOdd);
  const auto* rect = std::get_if<Shape>(&items[4].first->value);
  REQUIRE(rect != nullptr);
  const Rect rb = bounds(rect->path);
  CHECK_EQ(rb, (Rect{10, 20, 40, 60}));
  const auto* rects = std::get_if<Shape>(&items[5].first->value);
  REQUIRE(rects != nullptr);
  CHECK_APPROX(rects->stroke.paint.color.g, 0.5f, 1e-6);
}

TEST_CASE("vector/ps: clipping nests groups and restores with grestore") {
  auto scene = run("gsave 0 0 100 100 rectclip 10 10 50 50 rectfill\n"
                   "gsave newpath 50 50 20 0 360 arc eoclip 20 20 30 30 rectfill grestore\n"
                   "30 30 5 5 rectfill grestore 200 200 10 10 rectfill initclip");
  REQUIRE_OK(scene);
  const Group& root = scene->pages[0].root;
  REQUIRE_EQ(root.children.size(), 2u);
  const auto* clip_group = std::get_if<Group>(&root.children[0].value);
  REQUIRE(clip_group != nullptr);
  REQUIRE(clip_group->clip != nullptr);
  CHECK_EQ(bounds(clip_group->clip->items[0].path), (Rect{0, 0, 100, 100}));
  REQUIRE_EQ(clip_group->children.size(), 3u);
  const auto* inner = std::get_if<Group>(&clip_group->children[1].value);
  REQUIRE(inner != nullptr);
  REQUIRE(inner->clip != nullptr);
  CHECK(inner->clip->items[0].rule == FillRule::EvenOdd);
  CHECK(std::holds_alternative<Shape>(root.children[1].value));
}

TEST_CASE("vector/ps: colour spaces") {
  auto scene = run("[/Indexed /DeviceRGB 1 <ff000000ff00>] setcolorspace 1 setcolor 0 0 10 10 rectfill\n"
                   "[/Separation /Spot /DeviceCMYK { 0 exch 0 0 }] setcolorspace 1 setcolor 20 0 10 10 rectfill\n"
                   "/DeviceGray setcolorspace 0.5 setcolor 40 0 10 10 rectfill\n"
                   "[/ICCBased << /N 3 >>] setcolorspace 0 0 1 setcolor 60 0 10 10 rectfill\n"
                   "[/Lab << /WhitePoint [0.9505 1 1.089] >>] setcolorspace 100 0 0 setcolor 80 0 10 10 rectfill\n"
                   "[/Separation /None /DeviceGray {}] setcolorspace 1 setcolor 100 0 10 10 rectfill");
  REQUIRE_OK(scene);
  const auto items = leaves(*scene);
  REQUIRE_EQ(items.size(), 5u);  // the /None separation paints nothing
  const auto color = [&](std::size_t i) { return std::get<Shape>(items[i].first->value).fill.color; };
  CHECK_EQ(color(0), (Color{0, 1, 0, 1}));
  CHECK_EQ(color(1), (Color{1, 0, 1, 1}));  // full magenta through the tint transform
  CHECK_APPROX(color(2).r, 0.5f, 1e-6);
  CHECK_EQ(color(3), (Color{0, 0, 1, 1}));
  CHECK_APPROX(color(4).r, 1.0f, 0.01);
  CHECK_APPROX(color(4).g, 1.0f, 0.01);
}

TEST_CASE("vector/ps: images with filters") {
  // 2x2 RGB through ASCIIHex, RunLength and string / procedure sources.
  using namespace std::string_literals;
  auto scene = run("gsave 0 0 translate 20 20 scale 2 2 8 [2 0 0 -2 0 2] currentfile /ASCIIHexDecode filter false 3 "
                   "colorimage\nff0000 00ff00\n0000ff ffffff>\ngrestore\n"
                   "gsave 30 0 translate 20 20 scale 2 2 8 [2 0 0 2 0 0] {<00ff80c0>} image grestore\n"
                   "gsave 60 0 translate 20 20 scale 1 0 0 setrgbcolor 8 1 true [8 0 0 1 0 0] {<a5>} imagemask grestore\n"
                   "gsave 90 0 translate 20 20 scale /DeviceGray setcolorspace\n"
                   "<< /ImageType 1 /Width 4 /Height 1 /BitsPerComponent 8 /Decode [1 0] /ImageMatrix [4 0 0 1 0 0]\n"
                   "   /DataSource currentfile /RunLengthDecode filter >> image\n"
                   "\003\000\100\200\377\200 grestore"s);
  REQUIRE_OK(scene);
  std::vector<const Picture*> pics;
  std::vector<Matrix> placements;
  for (const auto& [n, m] : leaves(*scene)) {
    if (const auto* p = std::get_if<Picture>(&n->value)) {
      pics.push_back(p);
      placements.push_back(m);
    }
  }
  REQUIRE_EQ(pics.size(), 4u);
  const ir::Frame& rgb = *pics[0]->pixels;
  REQUIRE_EQ(rgb.width, 2u);
  CHECK_EQ(rgb.rgba[0], 255);
  CHECK_EQ(rgb.rgba[5], 255);  // green
  CHECK_EQ(rgb.rgba[rgb.offset(0, 1) + 2], 255);  // blue on the second row
  // First row of the image is drawn at the top of the 20x20 square.
  const Point top_left = placements[0].apply({0, 0});
  CHECK_APPROX(top_left.y, 20.0, 1e-9);
  const ir::Frame& gray = *pics[1]->pixels;
  CHECK_EQ(gray.rgba[0], 0);
  CHECK_EQ(gray.rgba[4], 255);
  const ir::Frame& mask = *pics[2]->pixels;
  CHECK_EQ(mask.rgba[3], 255);  // 1 bit painted (polarity true)
  CHECK_EQ(mask.rgba[7], 0);
  CHECK_EQ(mask.rgba[0], 255);  // red paint
  const ir::Frame& rl = *pics[3]->pixels;
  REQUIRE_EQ(rl.width, 4u);
  CHECK_EQ(rl.rgba[0], 255);  // decode [1 0] inverts 0x00
  CHECK_EQ(rl.rgba[12], 0);   // 0xFF -> 0
}

TEST_CASE("vector/ps: Flate and LZW filters through readstring") {
  // "Hello" Flate-compressed (zlib) and LZW-coded, as ASCIIHex.
  CHECK_EQ(eval_string("currentfile /ASCIIHexDecode filter /FlateDecode filter 5 string readstring\n"
                       "789cf348cdc9c90700058c01f5> pop"),
           "Hello");
  CHECK_EQ(eval_string("currentfile /ASCIIHexDecode filter /LZWDecode filter 5 string readstring\n"
                       "80120ca6c361be02> pop"),
           "Hello");
  CHECK_EQ(eval_string("currentfile 0 (%END) /SubFileDecode filter 20 string readstring\nabc%END pop"), "abc");
}

TEST_CASE("vector/ps: smooth shading becomes gradients") {
  auto scene = run("<< /ShadingType 2 /ColorSpace /DeviceRGB /Coords [0 0 100 0] /Extend [true true]\n"
                   "   /Function << /FunctionType 3 /Domain [0 1] /Bounds [0.5] /Encode [0 1 0 1] /Functions [\n"
                   "     << /FunctionType 2 /Domain [0 1] /C0 [1 0 0] /C1 [0 1 0] /N 1 >>\n"
                   "     << /FunctionType 2 /Domain [0 1] /C0 [0 1 0] /C1 [0 0 1] /N 1 >> ] >> >> shfill\n"
                   "<< /PatternType 2 /Shading << /ShadingType 3 /ColorSpace /DeviceGray /Coords [50 50 0 50 50 40]\n"
                   "   /Function << /FunctionType 2 /Domain [0 1] /C0 [1] /C1 [0] /N 1 >> >> >> matrix makepattern\n"
                   "setpattern 0 0 100 100 rectfill");
  REQUIRE_OK(scene);
  const auto items = leaves(*scene);
  REQUIRE_EQ(items.size(), 2u);
  const Shape& lin = std::get<Shape>(items[0].first->value);
  REQUIRE(lin.fill.type == Paint::Type::Gradient);
  const Gradient& g = *lin.fill.gradient;
  CHECK(g.type == Gradient::Type::Linear);
  CHECK_EQ(g.p2.x, 100.0);
  bool has_mid = false;
  for (const GradientStop& s : g.stops) {
    if (std::abs(s.offset - 0.5) < 1e-9) has_mid = s.color.g > 0.99f;
  }
  CHECK(has_mid);
  CHECK_EQ(g.stops.front().color.r, 1.0f);
  CHECK_EQ(g.stops.back().color.b, 1.0f);
  const Shape& rad = std::get<Shape>(items[1].first->value);
  REQUIRE(rad.fill.type == Paint::Type::Gradient);
  CHECK(rad.fill.gradient->type == Gradient::Type::Radial);
  CHECK_EQ(rad.fill.gradient->radius, 40.0);
  CHECK_EQ(rad.fill.gradient->stops.front().color.r, 1.0f);
}

TEST_CASE("vector/ps: tiling patterns and forms") {
  auto scene = run("<< /PatternType 1 /PaintType 1 /TilingType 1 /BBox [0 0 10 10] /XStep 10 /YStep 10\n"
                   "   /PaintProc { pop 1 0 0 setrgbcolor 0 0 5 5 rectfill } >> matrix makepattern setpattern\n"
                   "0 0 100 100 rectfill\n"
                   "<< /FormType 1 /BBox [0 0 50 50] /Matrix [1 0 0 1 200 200]\n"
                   "   /PaintProc { pop 0 0 1 setrgbcolor 0 0 100 100 rectfill } >> execform");
  REQUIRE_OK(scene);
  const auto items = leaves(*scene);
  REQUIRE_EQ(items.size(), 2u);
  const Shape& pat = std::get<Shape>(items[0].first->value);
  REQUIRE(pat.fill.type == Paint::Type::Pattern);
  CHECK_EQ(pat.fill.pattern->tile, (Rect{0, 0, 10, 10}));
  REQUIRE(pat.fill.pattern->content != nullptr);
  CHECK_EQ(pat.fill.pattern->content->children.size(), 1u);
  // The form is clipped to its bounding box and placed by its matrix.
  const Rect fb = bounds(std::get<Shape>(items[1].first->value).path, items[1].second);
  CHECK_EQ(fb, (Rect{200, 200, 300, 300}));
}

TEST_CASE("vector/ps: one page per showpage in PostScript documents") {
  const std::string doc = "%!PS-Adobe-3.0\n%%Pages: 3\n%%BoundingBox: 0 0 100 100\n"
                          "0 0 10 10 rectfill showpage\n<< /PageSize [200 300] >> setpagedevice 0 0 20 20 rectfill "
                          "showpage\n0 0 30 30 rectfill showpage\n";
  auto scene = read_scene("ps", as_bytes(doc), test_context());
  REQUIRE_OK(scene);
  REQUIRE_EQ(scene->pages.size(), 3u);
  CHECK_APPROX(scene->pages[1].width, 200 * 96.0 / 72.0, 1e-6);
  CHECK_APPROX(scene->pages[1].height, 300 * 96.0 / 72.0, 1e-6);
  // EPS: everything lands on the single page.
  auto single = read_scene("eps", as_bytes(eps("0 0 10 10 rectfill showpage 20 20 5 5 rectfill showpage")),
                           test_context());
  REQUIRE_OK(single);
  CHECK_EQ(single->pages.size(), 1u);
  CHECK_EQ(single->pages[0].root.children.size(), 2u);
}

TEST_CASE("vector/ps: bounded execution of hostile programs") {
  // Endless loops end at the operation limit.
  CHECK_STATUS(run("{ } loop", R"({"max_operations": 100000})"), Status::Limit);
  CHECK_STATUS(run("/a { a } def a", R"({"max_operations": 100000})"), Status::Limit);
  CHECK_STATUS(run("0 0 1 { pop } for", R"({"max_operations": 100000})"), Status::Limit);
  // Unbounded recursion overflows the execution stack.
  auto recursion = run("/a { a 1 } def a");
  REQUIRE_FALSE(recursion);
  CHECK((recursion.error().code == Status::Decode || recursion.error().code == Status::Limit));
  // Huge allocations are refused.
  CHECK_FALSE(run("100000000 array"));
  CHECK_FALSE(run("2000000000 string"));
  CHECK_OK(run("100000000 dict pop 0 0 1 1 rectfill"));  // the capacity is only a hint
  Limits small;
  small.max_decompressed_bytes = 4u * 1024u * 1024u;
  auto vm = run("/l [ 0 1 100000 { pop 1000 string } for ] def", {}, small);
  REQUIRE_FALSE(vm);
  CHECK((vm.error().code == Status::Limit || vm.error().code == Status::Decode));
  // Operand stack growth.
  CHECK_FALSE(run("{ 1 } loop", R"({"max_operations": 2000000})"));
  // Deeply nested procedures and dictionaries.
  CHECK_FALSE(run(std::string(5000, '{')));
  CHECK_FALSE(run("{ 1 dict begin } loop", R"({"max_operations": 1000000})"));
  // A Type 3 font whose BuildChar shows text with itself.
  auto nested = run("/F << /FontType 3 /FontMatrix [1 0 0 1 0 0] /FontBBox [0 0 1 1] /Encoding StandardEncoding\n"
                    "/BuildChar { pop pop 1 0 setcharwidth 0 0 moveto (a) show } >> definefont pop\n"
                    "/F findfont setfont 0 0 moveto (a) show");
  CHECK_FALSE(nested);
  // A huge image declared with little data is bounded by the pixel limit.
  Limits pixels;
  pixels.max_pixels = 1000;
  CHECK_STATUS(run("100 100 8 [100 0 0 100 0 0] {<00>} image", {}, pixels), Status::Limit);
  // Errors at the top level without any drawing fail with Decode.
  CHECK_STATUS(run("nosuchoperator"), Status::Decode);
}

TEST_CASE("vector/ps: errors after drawing keep the drawing with a warning") {
  std::vector<std::string> warnings;
  const std::string program = eps("0 0 10 10 rectfill nosuchoperator 20 20 10 10 rectfill");
  auto data = test::read_as<ir::Data>("eps", as_bytes(program), {}, &warnings);
  REQUIRE_OK(data);
  bool warned = false;
  for (const std::string& w : warnings) warned = warned || w.find("stopped early") != std::string::npos;
  CHECK(warned);
}

TEST_CASE("vector/ps: eexec decryption and currentfile reading") {
  // `eexec` over a hex-encrypted section that draws a rectangle and closes
  // the file; the plain program continues afterwards.
  // Encrypt "0 0 7 7 rectfill currentfile closefile " with r = 55665 and 4 random bytes.
  const std::string plain = "0000 0 0 7 7 rectfill mark currentfile closefile\n";
  std::string hex;
  std::uint16_t r = 55665;
  for (const char ch : plain) {
    const auto p = static_cast<unsigned char>(ch);
    const auto c = static_cast<unsigned char>(p ^ (r >> 8));
    r = static_cast<std::uint16_t>((c + r) * 52845u + 22719u);
    static const char* digits = "0123456789abcdef";
    hex.push_back(digits[c >> 4]);
    hex.push_back(digits[c & 15]);
  }
  auto scene = run("currentfile eexec\n" + hex + "\n" + std::string(64, '0') + "\ncleartomark 20 20 3 3 rectfill");
  REQUIRE_OK(scene);
  const auto items = leaves(*scene);
  REQUIRE_EQ(items.size(), 2u);
  CHECK_EQ(bounds(std::get<Shape>(items[0].first->value).path), (Rect{0, 0, 7, 7}));
  // readline / readhexstring from currentfile.
  CHECK_EQ(eval_string("currentfile 80 string readline\nLorem ipsum\npop"), "Lorem ipsum");
  CHECK_EQ(eval_string("currentfile 3 string readhexstring\n41 42\n43 pop"), "ABC");
}
