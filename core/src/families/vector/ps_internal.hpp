// Convertible core - vector family: the in-house PostScript interpreter.
//
// Private to ps_*.cpp. The interpreter runs a PostScript (or EPS / legacy
// Illustrator) program and records what it paints as a `Scene`:
//
//   ps_object.cpp   objects, virtual memory (arrays, strings, dictionaries,
//                   files) with save/restore journaling
//   ps_scan.cpp     the tokenizer and file sources (eexec, filters)
//   ps_interp.cpp   the execution loop, errors and the language operators
//   ps_graphics.cpp graphics state, paths, painting, clipping, output
//   ps_image.cpp    decode filters and the image operators
//   ps_font.cpp     fonts (Type 1 / 3 / 42 / 0, bundled fallback) and text
//   ps_shading.cpp  functions, shfill and shading / tiling patterns
//   ps_ai.cpp       emulation of the Illustrator procsets for .ai files
//
// Nothing here throws across the module boundary: errors inside the
// interpreter are C++ exceptions caught by `Interpreter::run_program`.
//
// Coordinates: the device space is PostScript's default user space (points,
// y up); the page root group maps it to scene px, y down.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "families/vector/scene.hpp"
#include "util/bytes.hpp"

namespace cvt::vector {
class TrueTypeFont;
}

namespace cvt::vector::ps {

// --------------------------------------------------------------------------- objects

enum class Type : std::uint8_t {
  Null, Integer, Real, Boolean, Name, String, Array, Dict, Operator, Mark, File, Save, FontID, GState
};

[[nodiscard]] std::string_view type_name(Type type) noexcept;

struct Object {
  Type type = Type::Null;
  bool exec = false;
  bool readonly = false;
  bool packed = false;
  std::int32_t ival = 0;     ///< integer, boolean
  std::uint32_t ref = 0;     ///< name id, store index, operator index, save level
  std::uint32_t start = 0;   ///< string / array slice
  std::uint32_t size = 0;
  double rval = 0.0;         ///< real

  [[nodiscard]] static Object null() { return {}; }
  [[nodiscard]] static Object integer(std::int64_t v) {
    Object o;
    o.type = Type::Integer;
    o.ival = static_cast<std::int32_t>(v);
    return o;
  }
  [[nodiscard]] static Object real(double v) {
    Object o;
    o.type = Type::Real;
    o.rval = v;
    return o;
  }
  [[nodiscard]] static Object boolean(bool v) {
    Object o;
    o.type = Type::Boolean;
    o.ival = v ? 1 : 0;
    return o;
  }
  [[nodiscard]] static Object mark() {
    Object o;
    o.type = Type::Mark;
    return o;
  }
  [[nodiscard]] bool is_number() const noexcept { return type == Type::Integer || type == Type::Real; }
  [[nodiscard]] double number() const noexcept { return type == Type::Integer ? static_cast<double>(ival) : rval; }
  [[nodiscard]] bool is_proc() const noexcept { return type == Type::Array && exec; }
};

/// Dictionary key: names and strings share the name space; integral reals
/// equal integers.
struct Key {
  std::uint8_t kind = 0;
  std::uint64_t a = 0;
  std::uint64_t b = 0;
  friend bool operator==(const Key&, const Key&) = default;
};
struct KeyHash {
  std::size_t operator()(const Key& k) const noexcept {
    std::uint64_t h = k.a * 0x9E3779B97F4A7C15ull ^ (k.b + 0x632BE59BD9B4E019ull + (h_seed(k.kind) << 6));
    h ^= h >> 29;
    return static_cast<std::size_t>(h);
  }
  static std::uint64_t h_seed(std::uint8_t k) noexcept { return 0x100000001B3ull * (k + 1u); }
};

struct DictStore {
  std::vector<std::pair<Object, Object>> entries;
  std::unordered_map<Key, std::uint32_t, KeyHash> index;
  std::uint32_t capacity = 0;
  bool readonly = false;
  bool global = false;
  std::uint32_t level = 0;  ///< save level at creation
};
struct ArrayStore {
  std::vector<Object> items;
  bool global = false;
  std::uint32_t level = 0;
};
struct StringStore {
  std::string bytes;
  bool global = false;
  std::uint32_t level = 0;
};

/// A readable file: a byte buffer (owned or a view), an eexec decryptor
/// over another file, or a procedure data source. Filters are decoded
/// eagerly into owned buffers.
struct FileStore {
  enum class Kind : std::uint8_t { Buffer, Eexec, Procedure, Deferred };
  Kind kind = Kind::Buffer;
  std::shared_ptr<const std::string> owned;
  std::string_view data;
  std::size_t pos = 0;
  bool closed = false;
  bool writable = false;  ///< %stdout / %stderr: writes are discarded
  bool jpeg = false;      ///< the buffer is a complete JPEG stream (DCTDecode output)
  // eexec
  std::uint32_t source = 0;
  std::uint16_t r = 0;
  bool hex = false;
  int pending = -1;  ///< one byte of look-ahead (peek)
  // procedure source; for a deferred filter, its data source
  Object proc;
  std::string filter_name;
  Object filter_params;
  std::string chunk;
  std::size_t chunk_pos = 0;
  bool eof_seen = false;
};

// --------------------------------------------------------------------------- errors

/// PostScript error names (catchable by `stopped`).
struct PsError {
  std::string name;
  std::string detail;
};
/// Fatal conditions that end the program (not catchable).
struct FatalError {
  Error error;
};
struct StopSignal {};
struct QuitSignal {};

// --------------------------------------------------------------------------- graphics state

struct ClipNode {
  std::shared_ptr<const ClipNode> parent;
  Path path;  ///< device space
  FillRule rule = FillRule::NonZero;
  std::uint64_t id = 0;
};

enum class Family : std::uint8_t { Gray, RGB, CMYK, Indexed, Separation, DeviceN, Pattern, Lab };

struct ColorSpace {
  Family family = Family::Gray;
  int components = 1;
  Object desc;  ///< the PostScript description (array or name), for currentcolorspace
  /// Indexed: base space, hival, lookup bytes (or procedure).
  std::shared_ptr<const ColorSpace> base;
  int hival = 0;
  std::string lookup;
  Object lookup_proc;
  /// Separation / DeviceN: tint transform.
  Object tint;
  bool none = false;  ///< Separation /None paints nothing
  /// Lab / CIE spaces approximated
  std::vector<double> white{0.9505, 1.0, 1.089};
};

struct GState {
  Matrix ctm;
  Path path;  ///< device space
  bool has_point = false;
  Point point;  ///< device space
  Point subpath_start;
  ColorSpace space;
  std::vector<double> comps{0.0};
  Object pattern;  ///< pattern dictionary when painting with a pattern
  Color color{0, 0, 0, 1};
  double line_width = 1.0;
  LineCap cap = LineCap::Butt;
  LineJoin join = LineJoin::Miter;
  double miter = 10.0;
  std::vector<double> dash;
  double dash_offset = 0.0;
  Object dash_array;
  double flat = 1.0;
  bool stroke_adjust = false;
  std::shared_ptr<const ClipNode> clip;
  Object font;
  bool overprint = false;
  double smoothness = 0.02;
};

/// What a gsave stack entry or save object restores.
struct SavedGState {
  GState state;
  bool from_save = false;
};

// --------------------------------------------------------------------------- execution stack

struct Frame {
  enum class Kind : std::uint8_t { Proc, File, For, Repeat, Loop, Forall, Stopped };
  Kind kind = Kind::Proc;
  Object obj;   ///< Proc: the array; File: the file; Forall: the composite
  Object proc;  ///< loop body
  std::uint32_t pos = 0;
  std::int64_t count = 0;
  double cur = 0, inc = 0, limit = 0;
  bool int_loop = false;
};

// --------------------------------------------------------------------------- fonts

struct GlyphOutline {
  Path path;            ///< character space (y up)
  Point advance;        ///< character space
  bool ok = false;
};

struct LoadedFont {
  int type = 1;
  std::shared_ptr<std::string> sfnt;        ///< Type 42 data (kept alive for `ttf`)
  std::shared_ptr<TrueTypeFont> ttf;        ///< Type 42 / fallback
  bool fallback = false;
  bool slant = false;                       ///< synthesize an oblique fallback
  std::vector<std::string> subrs;           ///< Type 1: decrypted Subrs
  int len_iv = 4;
  std::map<std::string, GlyphOutline> cache;
};

// --------------------------------------------------------------------------- interpreter

struct Options {
  bool eps = true;
  bool ai = false;
  std::uint64_t max_operations = 30'000'000;
  std::uint64_t max_vm_bytes = 256ull * 1024 * 1024;
  std::size_t max_pages = 1000;
  int max_nesting = 64;
  /// EPS page box in points (from %%BoundingBox); empty = page size.
  Rect bbox;
  double page_width = 612;  ///< points
  double page_height = 792;
};

class Interpreter {
 public:
  Interpreter(const Context& ctx, Options options);
  ~Interpreter();
  Interpreter(const Interpreter&) = delete;
  Interpreter& operator=(const Interpreter&) = delete;

  /// Run `program` and return the scene. Errors in the program end it;
  /// whatever was painted so far is kept (with a warning) unless nothing
  /// was, in which case the PostScript error is returned as Status::Decode.
  Result<Scene> run_program(std::string_view program);

  // ---- objects (ps_object.cpp)
  Object make_name(std::string_view text, bool exec = false);
  Object make_string(std::string_view bytes);
  Object make_string(std::size_t size);
  Object make_array(std::size_t size);
  Object make_dict(std::size_t capacity);
  Object make_operator(std::string_view name);
  Object make_file(FileStore file);
  std::string_view name_text(const Object& name) const;
  std::string_view string_view(const Object& str) const;
  std::string& string_bytes(const Object& str);  ///< whole backing store (use start/size)
  std::string object_string(const Object& o) const;  ///< text of a name/string
  Object* array_data(const Object& arr);
  const Object* array_data(const Object& arr) const;
  Object array_get(const Object& arr, std::size_t i) const;
  void array_put(const Object& arr, std::size_t i, const Object& value);
  void string_put(const Object& str, std::size_t i, std::uint8_t value);
  void string_write(const Object& str, std::size_t at, std::string_view bytes);
  DictStore& dict(const Object& d);
  const DictStore& dict(const Object& d) const;
  Key key_of(const Object& k) const;
  const Object* dict_find(const Object& d, const Object& key) const;
  const Object* dict_find(const Object& d, std::string_view key) const;
  void dict_put(const Object& d, const Object& key, const Object& value);
  void dict_put(const Object& d, std::string_view key, const Object& value);
  bool dict_remove(const Object& d, const Object& key);
  FileStore& file(const Object& f);
  void charge(std::uint64_t bytes);
  bool equal(const Object& a, const Object& b) const;
  std::string cvs(const Object& o) const;

  // ---- stacks
  void push(const Object& o);
  void push_int(std::int64_t v) { push(Object::integer(v)); }
  void push_real(double v) { push(Object::real(v)); }
  void push_bool(bool v) { push(Object::boolean(v)); }
  Object pop();
  Object& top(std::size_t depth = 0);
  void need(std::size_t n) const;
  double pop_number();
  std::int64_t pop_int();
  bool pop_bool();
  Object pop_type(Type type);
  Object pop_dict() { return pop_type(Type::Dict); }
  Object pop_string() { return pop_type(Type::String); }
  Object pop_array();  ///< array or packed array
  Object pop_proc();
  Matrix pop_matrix();
  Matrix to_matrix(const Object& arr) const;
  void store_matrix(const Object& arr, const Matrix& m);
  [[noreturn]] void error(std::string_view name, std::string_view detail = {}) const;
  [[noreturn]] void fatal(Status status, std::string message) const;

  // ---- execution (ps_interp.cpp)
  Object lookup(const Object& name) const;
  const Object* find_name(const Object& name) const;
  void execute(const Object& o);  ///< push a frame (or run an operator) for an executable object
  /// Run `proc` to completion now (callbacks from operators).
  void call(const Object& proc);
  void push_frame(Frame frame);
  void tick();
  Object systemdict() const { return systemdict_; }
  Object userdict() const { return userdict_; }
  Object current_dict() const { return dstack_.back(); }
  void define(std::string_view name, const Object& value);  ///< in the current dictionary
  void define_operator(std::string_view name, void (Interpreter::*fn)());
  void begin_dict(const Object& d);

  // ---- scanning (ps_scan.cpp)
  /// Next token from a file: nullopt at end of file.
  std::optional<Object> scan(const Object& file);
  int file_get(FileStore& f);
  int file_peek(FileStore& f);
  /// Up to `n` bytes from a file.
  std::string file_read(const Object& file, std::size_t n);
  Object filter(const Object& source, std::string_view name, const Object& params);
  /// Decode a deferred filter now (on first read).
  void materialize(std::size_t index);
  std::string decode_filter(const Object& source, std::string_view name, const Object& params, bool& jpeg);
  /// Bytes from a data source (string, file or procedure) - for images and functions.
  std::string read_source(const Object& source, std::size_t want);

  // ---- graphics (ps_graphics.cpp)
  GState& gs() { return gstate_; }
  void emit_fill(const Path& device_path, FillRule rule);
  void emit_fill_paint(const Path& device_path, FillRule rule, const Paint& paint);
  void emit_stroke(const Path& device_path);
  void emit_node(Node node);
  /// Start/finish capturing painted output into a separate group (patterns, forms, glyphs).
  void begin_capture();
  Group end_capture();
  void set_color_space(const Object& desc);
  ColorSpace parse_color_space(const Object& desc, int depth = 0);
  Color resolve_color(const ColorSpace& space, const std::vector<double>& comps, int depth = 0);
  void update_color();
  Rect page_rect_device() const;
  Point user_to_device(Point p) const { return gstate_.ctm.apply(p); }
  void finish_page(bool forced);

  // ---- fonts (ps_font.cpp)
  LoadedFont* font_for(const Object& font_dict);
  void show_string(const Object& str, int mode, double ax, double ay, double cx, double cy, int wchar,
                   const Object& proc);
  GlyphOutline glyph(LoadedFont& font, const Object& font_dict, std::uint32_t code, std::string* name_out);
  Object find_font(const Object& name);

  // ---- images / shading
  void do_image(int kind);  // 0 image, 1 imagemask, 2 colorimage
  void do_shfill(const Object& shading, const Matrix& space);
  std::optional<Paint> shading_paint(const Object& shading, const Matrix& space);
  std::vector<double> eval_function(const Object& fn, double t, int depth = 0);

  // ---- state
  const Context& ctx_;
  Options opts_;
  std::vector<std::string> names_;
  std::unordered_map<std::string, std::uint32_t> name_ids_;
  std::vector<ArrayStore> arrays_;
  std::vector<StringStore> strings_;
  std::vector<DictStore> dicts_;
  std::vector<FileStore> files_;
  struct OperatorDef {
    std::string name;
    void (Interpreter::*fn)() = nullptr;
  };
  std::vector<OperatorDef> operators_;
  std::uint64_t vm_bytes_ = 0;
  std::uint64_t operations_ = 0;
  int nesting_ = 0;
  std::uint32_t rand_seed_ = 0;
  std::size_t permanent_dicts_ = 3;
  int materialize_depth_ = 0;
  bool global_alloc_ = false;

  std::vector<Object> ostack_;
  std::vector<Object> dstack_;
  std::vector<Frame> estack_;
  std::vector<Object> popped_;  ///< operands popped by the running operator (restored on error)
  Object systemdict_, userdict_, globaldict_, errordict_, dollar_error_, statusdict_, fontdir_;
  Object stdin_file_, current_file_;

  GState gstate_;
  std::vector<SavedGState> gstack_;

  // save / restore journal
  struct Journal {
    enum class Kind : std::uint8_t { ArrayItem, StringBytes, DictEntry };
    Kind kind = Kind::ArrayItem;
    std::uint32_t store = 0;
    std::uint32_t pos = 0;
    Object old;
    bool existed = false;
    Object key;
    std::string bytes;
  };
  struct SaveRecord {
    std::size_t journal_start = 0;
    std::size_t gstack_depth = 0;
    GState state;
    bool valid = true;
  };
  std::vector<Journal> journal_;
  bool restoring_ = false;
  std::vector<std::size_t> bases_;  ///< execution stack bases of nested calls
  Object ai_dummy_;                 ///< stand-in for Illustrator procset dictionaries
  std::vector<SaveRecord> saves_;
  [[nodiscard]] std::uint32_t save_level() const { return static_cast<std::uint32_t>(saves_.size()); }

  // output
  struct OpenGroup {
    Group group;
    std::uint64_t clip_id = 0;
  };
  struct Capture {
    std::vector<OpenGroup> stack;
    std::size_t base = 1;  ///< groups below this index (root, layers) are not clip groups
  };
  std::vector<GState> gstate_objects_;
  std::vector<std::shared_ptr<const Group>> pattern_groups_;
  Paint current_paint();
  void sync_clip(Capture& capture);
  void close_top(Capture& capture);
  void path_moveto(Point device);
  void path_lineto(Point device);
  void path_curveto(Point c1, Point c2, Point p);
  void arc_user(double cx, double cy, double r, double a1, double a2, bool ccw);
  Stroke current_stroke(Matrix* group_transform, bool* use_group);
  void set_device_color(Family family, std::vector<double> comps);
  void page_size_changed(double w, double h);
  std::vector<Capture> captures_;
  std::vector<Page> pages_;
  std::size_t drawn_on_page_ = 0;
  std::size_t drawn_total_ = 0;
  std::uint64_t clip_counter_ = 0;
  Matrix page_transform_;
  double page_w_px_ = 0, page_h_px_ = 0;
  double media_w_ = 612, media_h_ = 792;  ///< current page device size (points)
  Rect media_box_;                        ///< device space region of the page
  bool page_size_set_ = false;
  std::map<std::string, std::string> metadata_;
  std::vector<std::string> warned_;
  void warn_once(const std::string& message);
  void start_page();

  // fonts
  std::map<std::uint32_t, std::unique_ptr<LoadedFont>> fonts_;  ///< by FontID / dict store index
  Object standard_encoding_, isolatin1_encoding_, symbol_encoding_;
  Object fallback_charstrings_;
  std::uint32_t font_id_counter_ = 0;
  int text_mode_ = 0;  ///< 0 normal, 1 charpath stroke, 2 charpath fill-outline, 3 stringwidth
  Point text_width_;
  bool in_build_char_ = false;
  Point build_char_width_;

  // AI emulation layers
  std::vector<Group> layer_stack_;

  // initialisation
  void install_operators();
  void install_graphics_operators();
  void install_font_operators();
  void install_image_operators();
  void install_ai_emulation();
  void init_encodings();
  void run_until(std::size_t base);
  void handle_error(const PsError& e, std::size_t base);
  bool unwind_to_stopped(std::size_t base);
  void step_frame();
  void exec_token(const Object& o, bool from_proc);
  void call_operator(std::uint32_t index);

  // operators: the language (ps_interp.cpp)
  void op_pop(); void op_exch(); void op_dup(); void op_copy(); void op_index(); void op_roll(); void op_clear();
  void op_count(); void op_mark(); void op_cleartomark(); void op_counttomark();
  void op_add(); void op_sub(); void op_mul(); void op_div(); void op_idiv(); void op_mod(); void op_abs(); void op_neg();
  void op_ceiling(); void op_floor(); void op_round(); void op_truncate(); void op_sqrt(); void op_atan(); void op_cos();
  void op_sin(); void op_exp(); void op_ln(); void op_log(); void op_rand(); void op_srand(); void op_rrand();
  void op_eq(); void op_ne(); void op_ge(); void op_gt(); void op_le(); void op_lt(); void op_and(); void op_or();
  void op_xor(); void op_not(); void op_bitshift(); void op_true(); void op_false();
  void op_exec(); void op_if(); void op_ifelse(); void op_for(); void op_repeat(); void op_loop(); void op_exit();
  void op_stop(); void op_stopped(); void op_countexecstack(); void op_execstack(); void op_quit(); void op_start();
  void op_array(); void op_packedarray(); void op_length(); void op_get(); void op_put(); void op_getinterval();
  void op_putinterval(); void op_aload(); void op_astore(); void op_forall(); void op_currentpacking();
  void op_setpacking(); void op_endarray(); void op_enddict();
  void op_dict(); void op_maxlength(); void op_begin(); void op_end(); void op_def(); void op_load(); void op_store();
  void op_undef(); void op_known(); void op_where(); void op_currentdict(); void op_countdictstack();
  void op_dictstack(); void op_cleardictstack(); void op_string(); void op_anchorsearch(); void op_search();
  void op_token(); void op_type(); void op_cvlit(); void op_cvx(); void op_xcheck(); void op_executeonly();
  void op_noaccess(); void op_readonly(); void op_rcheck(); void op_wcheck(); void op_cvi(); void op_cvn(); void op_cvr();
  void op_cvrs(); void op_cvs(); void op_null(); void op_bind(); void op_save(); void op_restore(); void op_vmstatus();
  void op_setglobal(); void op_currentglobal(); void op_gcheck(); void op_print(); void op_equals(); void op_eqeq();
  void op_stack(); void op_pstack(); void op_flush(); void op_currentfile(); void op_file(); void op_closefile();
  void op_read(); void op_readhexstring(); void op_readstring(); void op_readline(); void op_bytesavailable();
  void op_flushfile(); void op_status(); void op_run(); void op_eexec(); void op_filter(); void op_write();
  void op_writestring(); void op_writehexstring(); void op_noop(); void op_pop_noop(); void op_usertime();
  void op_realtime(); void op_languagelevel(); void op_product(); void op_version(); void op_revision();
  void op_serialnumber(); void op_findresource(); void op_defineresource(); void op_undefineresource();
  void op_resourcestatus(); void op_resourceforall(); void op_execuserobject();
  void op_defineuserobject(); void op_dicttomark();
  void op_deletefile(); void op_renamefile(); void op_filenameforall();
  void op_setfileposition(); void op_fileposition();
  void op_currentobjectformat(); void op_currentuserparams();
  void op_internaldict();
 

  // operators: graphics (ps_graphics.cpp)
  void op_gsave(); void op_grestore(); void op_grestoreall(); void op_initgraphics(); void op_gstate();
  void op_currentgstate(); void op_setgstate(); void op_setlinewidth(); void op_currentlinewidth();
  void op_setlinecap(); void op_currentlinecap(); void op_setlinejoin(); void op_currentlinejoin();
  void op_setmiterlimit(); void op_currentmiterlimit(); void op_setdash(); void op_currentdash(); void op_setflat();
  void op_currentflat(); void op_setstrokeadjust(); void op_currentstrokeadjust(); void op_setgray();
  void op_currentgray(); void op_setrgbcolor(); void op_currentrgbcolor(); void op_sethsbcolor();
  void op_currenthsbcolor(); void op_setcmykcolor(); void op_currentcmykcolor(); void op_setcolorspace();
  void op_currentcolorspace(); void op_setcolor(); void op_currentcolor(); void op_setpattern(); void op_matrix();
  void op_initmatrix(); void op_identmatrix(); void op_defaultmatrix(); void op_currentmatrix(); void op_setmatrix();
  void op_translate(); void op_scale(); void op_rotate(); void op_concat(); void op_concatmatrix(); void op_transform();
  void op_itransform(); void op_dtransform(); void op_idtransform(); void op_invertmatrix(); void op_newpath();
  void op_currentpoint(); void op_moveto(); void op_rmoveto(); void op_lineto(); void op_rlineto(); void op_arc();
  void op_arcn(); void op_arct(); void op_arcto(); void op_curveto(); void op_rcurveto(); void op_closepath();
  void op_flattenpath(); void op_reversepath(); void op_strokepath(); void op_clippath(); void op_pathbbox();
  void op_pathforall(); void op_initclip(); void op_clip(); void op_eoclip(); void op_rectclip(); void op_fill();
  void op_eofill(); void op_stroke(); void op_rectfill(); void op_rectstroke(); void op_erasepage();
  void op_showpage(); void op_copypage(); void op_setpagedevice(); void op_currentpagedevice(); void op_ufill();
  void op_ueofill(); void op_ustroke(); void op_uappend(); void op_upath(); void op_setbbox(); void op_ucache();
  void op_infill(); void op_instroke(); void op_inufill(); void op_setoverprint(); void op_currentoverprint();
  void op_settransfer(); void op_currenttransfer(); void op_sethalftone(); void op_currenthalftone();
  void op_setscreen(); void op_currentscreen(); void op_setcolortransfer(); void op_currentcolortransfer();
  void op_setblackgeneration(); void op_currentblackgeneration(); void op_setundercolorremoval();
  void op_currentundercolorremoval(); void op_setcolorrendering(); void op_currentcolorrendering();
  void op_setsmoothness(); void op_currentsmoothness(); void op_execform(); void op_makepattern();
  void op_shfill(); void op_wtranslation(); void op_sethalftonephase(); void op_nulldevice();
  void op_currentdevparams(); void op_setcachedevice(); void op_setcachedevice2(); void op_setcharwidth();
  void op_setcachelimit(); void op_cachestatus(); void op_ai_layer_begin(); void op_ai_layer_name();
  void op_ai_layer_end(); void op_ai_image();

  // operators: fonts & images
  void op_findfont(); void op_definefont(); void op_undefinefont(); void op_scalefont(); void op_makefont();
  void op_setfont(); void op_currentfont(); void op_rootfont(); void op_selectfont(); void op_show(); void op_ashow();
  void op_widthshow(); void op_awidthshow(); void op_kshow(); void op_xshow(); void op_yshow(); void op_xyshow();
  void op_glyphshow(); void op_cshow(); void op_stringwidth(); void op_charpath(); void op_findencoding();
  void op_image(); void op_imagemask(); void op_colorimage(); void op_composefont();

  // text helpers
  void finish_text_run(const Object& font_dict, const std::string& content, const Path& node_outline,
                       const Matrix& node_to_device, double size, double advance, bool invisible);
};

/// Standard glyph-name tables (ps_encodings.cpp).
[[nodiscard]] const std::array<std::string_view, 256>& standard_encoding();
[[nodiscard]] const std::array<std::string_view, 256>& isolatin1_encoding();
/// Unicode value of a glyph name (Adobe Glyph List subset, uniXXXX, uXXXX); 0 when unknown.
[[nodiscard]] char32_t glyph_unicode(std::string_view name);
/// Glyph name for a Unicode value in the Standard / ISO Latin-1 sets; empty when none.
[[nodiscard]] std::string_view unicode_glyph_name(char32_t cp);

/// Type 1 charstring decryption / interpretation (ps_type1.cpp).
[[nodiscard]] std::string decrypt(std::string_view data, std::uint16_t key, std::size_t skip);
struct Type1Glyph {
  Path path;
  Point advance;
  Point sidebearing;
  bool ok = false;
  /// seac: accent composed of two standard-encoding glyphs.
  int seac_base = -1, seac_accent = -1;
  Point seac_offset;
};
/// Interpret a decrypted Type 1 charstring. `subrs` decrypted; `othersubr`
/// handled internally (flex, hint replacement).
[[nodiscard]] Type1Glyph run_type1_charstring(std::string_view cs, const std::vector<std::string>& subrs,
                                              int depth_limit = 10);
/// Type 2 (CFF) charstrings are not supported; Type 1 only.

/// Is this an Illustrator emulation-worthy file (legacy AI comments without procsets)?
[[nodiscard]] bool needs_ai_emulation(std::string_view program);
/// The PostScript source of the Illustrator procset emulation.
[[nodiscard]] std::string_view ai_emulation_source();

}  // namespace cvt::vector::ps
