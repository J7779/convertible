// Convertible core - vector family: legacy (PostScript-based) Adobe Illustrator files.
//
// Native .ai files up to Illustrator 8 are PostScript programs that rely on
// Adobe procsets which the files only reference (%%IncludeResource). This is
// a small emulation of those procsets' drawing operators (paths, separate
// fill and stroke colours, compound paths, clip groups, layers, point text)
// installed below userdict, so files that do include the real procsets keep
// using them. Unknown Illustrator operators are skipped with a warning.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string_view>

#include "families/vector/ps_internal.hpp"

namespace cvt::vector::ps {

namespace {

constexpr std::string_view emulation = R"PS(
/_fcol {0 setgray} def
/_scol {0 setgray} def
/_eo false def
/_cmp false def
/_clip false def
/_pp {} def
/_trm 0 def
/_lead 0 def
/_tlc 0 def
/_tm0 matrix def
/g { [ exch /setgray cvx ] cvx /_fcol exch def } bind def
/G { [ exch /setgray cvx ] cvx /_scol exch def } bind def
/k { [ 5 1 roll /setcmykcolor cvx ] cvx /_fcol exch def } bind def
/K { [ 5 1 roll /setcmykcolor cvx ] cvx /_scol exch def } bind def
/Xa { [ 4 1 roll /setrgbcolor cvx ] cvx /_fcol exch def } bind def
/XA { [ 4 1 roll /setrgbcolor cvx ] cvx /_scol exch def } bind def
/_tint4 { /_t exch def pop 4 { 1 _t sub mul 4 1 roll } repeat } bind def
/_tint3 { /_t exch def pop 3 { dup 1 exch sub _t mul add 3 1 roll } repeat } bind def
/x { _tint4 k } bind def
/X { _tint4 K } bind def
/Xx { 0 eq { _tint4 k } { _tint3 Xa } ifelse } bind def
/XX { 0 eq { _tint4 K } { _tint3 XA } ifelse } bind def
/p { 11 { pop } repeat /_fcol {0.5 setgray} def } bind def
/P { 11 { pop } repeat /_scol {0.5 setgray} def } bind def
/O { pop } bind def
/R { pop } bind def
/XR { 1 eq /_eo exch def } bind def
/D { pop } bind def
/m { moveto } bind def
/l { lineto } bind def
/L { lineto } bind def
/c { curveto } bind def
/C { curveto } bind def
/v { currentpoint 6 2 roll curveto } bind def
/V { currentpoint 6 2 roll curveto } bind def
/y { 2 copy curveto } bind def
/Y { 2 copy curveto } bind def
/_doclip { _clip { _eo { eoclip } { clip } ifelse /_clip false def } if } bind def
/_fill { gsave _fcol _eo { eofill } { fill } ifelse grestore } bind def
/_stroke { gsave _scol stroke grestore } bind def
/_paint { _cmp { /_pp exch def } { exec _doclip newpath } ifelse } bind def
/F { { _fill } _paint } bind def
/f { closepath F } bind def
/S { { _stroke } _paint } bind def
/s { closepath S } bind def
/B { { _fill _stroke } _paint } bind def
/b { closepath B } bind def
/N { { } _paint } bind def
/n { closepath N } bind def
/H { N } bind def
/h { n } bind def
/*u { /_cmp true def /_pp { } def } bind def
/*U { /_cmp false def _pp _doclip newpath } bind def
/W { /_clip true def } bind def
/u { } def
/U { } def
/q { gsave } bind def
/Q { grestore } bind def
/w { setlinewidth } bind def
/j { setlinejoin } bind def
/J { setlinecap } bind def
/M { setmiterlimit } bind def
/d { setdash } bind def
/i { setflat } bind def
/A { pop } bind def
/Lb { .ai_layer_begin } bind def
/Ln { .ai_layer_name } bind def
/LB { .ai_layer_end } bind def
/To { pop gsave /_tm0 matrix currentmatrix def /_tlc 0 def } bind def
/TO { grestore } bind def
/Tp { pop 6 array astore _tm0 setmatrix concat 0 0 moveto /_tlc 0 def } bind def
/TP { } def
/Tm { 6 array astore _tm0 setmatrix concat 0 0 moveto /_tlc 0 def } bind def
/Tf { exch findfont exch scalefont setfont } bind def
/Tr { /_trm exch def } bind def
/_ta 0 def
/_talign { _ta 1 eq { dup stringwidth pop -0.5 mul 0 rmoveto } if _ta 2 eq { dup stringwidth pop neg 0 rmoveto } if } bind def
/_tshow { _talign _trm 3 eq { stringwidth rmoveto } { gsave _fcol currentpoint 3 -1 roll show grestore moveto } ifelse } bind def
/_tadv { stringwidth rmoveto } bind def
/Tx { dup _tshow _ta 0 eq { _tadv } { pop } ifelse } bind def
/Tj { dup _tshow _ta 0 eq { _tadv } { pop } ifelse } bind def
/TX { _tadv } bind def
/Tl { pop /_lead exch def } bind def
/T* { /_tlc _tlc 1 add def 0 _tlc _lead mul neg moveto } bind def
/Tz { pop } bind def
/Tt { pop } bind def
/TA { pop } bind def
/TC { pop pop pop } bind def
/TW { pop pop pop } bind def
/Ti { pop pop pop } bind def
/Ta { /_ta exch def } bind def
/Tq { pop } bind def
/Tc { pop } bind def
/Tw { pop } bind def
/Ts { pop } bind def
/Tk { pop pop } bind def
/Tv { pop } bind def
/TE { cleartomark } bind def
/TZ { ] dup length 2 ge { dup 0 get exch 1 get findfont definefont pop } { pop } ifelse } bind def
/annotatepage { } def
/XI { cleartomark } bind def
)PS";

}  // namespace

std::string_view ai_emulation_source() { return emulation; }

bool needs_ai_emulation(std::string_view program) {
  const std::string_view head = program.substr(0, std::min<std::size_t>(program.size(), 65536));
  return head.find("%AI5_") != std::string_view::npos || head.find("%AI3_") != std::string_view::npos ||
         head.find("%AI7_") != std::string_view::npos || head.find("%AI8_") != std::string_view::npos ||
         head.find("%%Creator: Adobe Illustrator") != std::string_view::npos;
}

void Interpreter::install_ai_emulation() {
  ai_dummy_ = make_dict(4);
  Object empty = make_array(0);
  empty.exec = true;
  dict_put(ai_dummy_, "initialize", empty);
  dict_put(ai_dummy_, "terminate", empty);
  Object ai = make_dict(160);
  dict_put(systemdict_, ".aiemulation", ai);
  begin_dict(ai);
  FileStore fs;
  fs.data = ai_emulation_source();
  Frame f;
  f.kind = Frame::Kind::File;
  f.obj = make_file(std::move(fs));
  estack_.push_back(f);
  run_until(0);
  dstack_.pop_back();
  // systemdict, globaldict, the emulation, userdict.
  dstack_ = {systemdict_, globaldict_, ai, userdict_};
  permanent_dicts_ = 4;
}

}  // namespace cvt::vector::ps
