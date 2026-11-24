// Convertible core - vector family: PostScript execution loop and language operators.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <limits>
#include <numbers>
#include <set>
#include <utility>

#include "families/vector/ps_internal.hpp"
#include "util/text.hpp"

namespace cvt::vector::ps {

namespace {

constexpr double deg = std::numbers::pi / 180.0;

Object int_or_real(double v) {
  if (v >= -2147483648.0 && v <= 2147483647.0 && v == std::floor(v)) return Object::integer(static_cast<std::int64_t>(v));
  return Object::real(v);
}

}  // namespace

// --------------------------------------------------------------------------- setup

Interpreter::Interpreter(const Context& ctx, Options options) : ctx_(ctx), opts_(options) {
  global_alloc_ = true;
  systemdict_ = make_dict(600);
  globaldict_ = make_dict(64);
  dicts_[systemdict_.ref].global = true;
  dicts_[globaldict_.ref].global = true;
  global_alloc_ = false;
  userdict_ = make_dict(200);
  dstack_ = {systemdict_, globaldict_, userdict_};

  install_operators();
  install_graphics_operators();
  install_font_operators();
  install_image_operators();

  errordict_ = make_dict(32);
  dollar_error_ = make_dict(16);
  statusdict_ = make_dict(32);
  fontdir_ = make_dict(64);
  dict_put(systemdict_, "systemdict", systemdict_);
  dict_put(systemdict_, "userdict", userdict_);
  dict_put(systemdict_, "globaldict", globaldict_);
  dict_put(systemdict_, "shareddict", globaldict_);
  dict_put(systemdict_, "errordict", errordict_);
  dict_put(systemdict_, "$error", dollar_error_);
  dict_put(systemdict_, "statusdict", statusdict_);
  dict_put(systemdict_, "FontDirectory", fontdir_);
  dict_put(systemdict_, "GlobalFontDirectory", fontdir_);
  dict_put(systemdict_, "SharedFontDirectory", fontdir_);
  dict_put(dollar_error_, "newerror", Object::boolean(false));
  dict_put(dollar_error_, "errorname", Object::null());
  dict_put(statusdict_, "product", make_string("Convertible"));
  // errordict entries: procedures that stop (the interpreter itself handles errors).
  for (const std::string_view e :
       {"dictfull", "dictstackoverflow", "dictstackunderflow", "execstackoverflow", "handleerror", "interrupt",
        "invalidaccess", "invalidexit", "invalidfileaccess", "invalidfont", "invalidrestore", "ioerror", "limitcheck",
        "nocurrentpoint", "rangecheck", "stackoverflow", "stackunderflow", "syntaxerror", "timeout", "typecheck",
        "undefined", "undefinedfilename", "undefinedresource", "undefinedresult", "unmatchedmark", "unregistered",
        "VMerror", "configurationerror", "undefinedresource"}) {
    Object proc = make_array(1);
    arrays_[proc.ref].items[0] = make_operator("stop");
    proc.exec = true;
    dict_put(errordict_, e, proc);
  }
  init_encodings();
  // Resource categories.
  Object resources = make_dict(32);
  dict_put(systemdict_, ".resources", resources);
  for (const std::string_view cat : {"Font", "Encoding", "ProcSet", "Form", "Pattern", "ColorSpace", "Halftone",
                                     "ColorRendering", "Category", "Generic", "CMap", "CIDFont", "FontSet",
                                     "IdiomSet", "Filter", "ColorSpaceFamily", "Emulator", "IODevice",
                                     "ColorRenderingType", "FMapType", "FontType", "FormType", "HalftoneType",
                                     "ImageType", "PatternType", "FunctionType", "ShadingType", "CIDInit"}) {
    dict_put(resources, cat, make_dict(16));
  }
  // Implicit resources: what the interpreter supports.
  auto implicit = [&](std::string_view cat, std::initializer_list<int> ids, std::initializer_list<std::string_view> names) {
    const Object* d = dict_find(resources, cat);
    if (d == nullptr) return;
    for (const int id : ids) dict_put(*d, Object::integer(id), Object::integer(id));
    for (const std::string_view n : names) dict_put(*d, n, make_name(n));
  };
  implicit("FontType", {0, 1, 3, 42}, {});
  implicit("FunctionType", {0, 2, 3}, {});
  implicit("ShadingType", {1, 2, 3, 4, 5, 6, 7}, {});
  implicit("ImageType", {1, 3, 4}, {});
  implicit("PatternType", {1, 2}, {});
  implicit("FMapType", {2, 9}, {});
  implicit("FormType", {1}, {});
  implicit("HalftoneType", {1, 3, 5, 6, 10, 16}, {});
  implicit("Filter", {}, {"ASCIIHexDecode", "ASCII85Decode", "RunLengthDecode", "LZWDecode", "FlateDecode",
                          "DCTDecode", "SubFileDecode", "ReusableStreamDecode", "NullEncode", "ASCIIHexEncode",
                          "ASCII85Encode"});
  implicit("ColorSpaceFamily", {}, {"DeviceGray", "DeviceRGB", "DeviceCMYK", "Indexed", "Separation", "DeviceN",
                                    "Pattern", "ICCBased", "CIEBasedA", "CIEBasedABC", "CIEBasedDEF",
                                    "CIEBasedDEFG"});
  implicit("Encoding", {}, {});
  if (const Object* enc = dict_find(resources, "Encoding")) {
    dict_put(*enc, "StandardEncoding", standard_encoding_);
    dict_put(*enc, "ISOLatin1Encoding", isolatin1_encoding_);
  }
  if (opts_.ai) install_ai_emulation();
}

Interpreter::~Interpreter() = default;

void Interpreter::define_operator(std::string_view name, void (Interpreter::*fn)()) {
  OperatorDef def;
  def.name = std::string(name);
  def.fn = fn;
  operators_.push_back(std::move(def));
  Object o;
  o.type = Type::Operator;
  o.exec = true;
  o.ref = static_cast<std::uint32_t>(operators_.size() - 1);
  dict_put(systemdict_, name, o);
}

void Interpreter::define(std::string_view name, const Object& value) { dict_put(dstack_.back(), name, value); }

void Interpreter::begin_dict(const Object& d) {
  if (d.type != Type::Dict) error("typecheck");
  if (dstack_.size() >= 1000) error("dictstackoverflow");
  dstack_.push_back(d);
}

void Interpreter::warn_once(const std::string& message) {
  if (std::find(warned_.begin(), warned_.end(), message) != warned_.end()) return;
  warned_.push_back(message);
  ctx_.warn(message);
}

void Interpreter::install_operators() {
  using I = Interpreter;
  const std::pair<std::string_view, void (I::*)()> ops[] = {
      {"pop", &I::op_pop}, {"exch", &I::op_exch}, {"dup", &I::op_dup}, {"copy", &I::op_copy},
      {"index", &I::op_index}, {"roll", &I::op_roll}, {"clear", &I::op_clear}, {"count", &I::op_count},
      {"mark", &I::op_mark}, {"cleartomark", &I::op_cleartomark}, {"counttomark", &I::op_counttomark},
      {"add", &I::op_add}, {"sub", &I::op_sub}, {"mul", &I::op_mul}, {"div", &I::op_div}, {"idiv", &I::op_idiv},
      {"mod", &I::op_mod}, {"abs", &I::op_abs}, {"neg", &I::op_neg}, {"ceiling", &I::op_ceiling},
      {"floor", &I::op_floor}, {"round", &I::op_round}, {"truncate", &I::op_truncate}, {"sqrt", &I::op_sqrt},
      {"atan", &I::op_atan}, {"cos", &I::op_cos}, {"sin", &I::op_sin}, {"exp", &I::op_exp}, {"ln", &I::op_ln},
      {"log", &I::op_log}, {"rand", &I::op_rand}, {"srand", &I::op_srand}, {"rrand", &I::op_rrand},
      {"eq", &I::op_eq}, {"ne", &I::op_ne}, {"ge", &I::op_ge}, {"gt", &I::op_gt}, {"le", &I::op_le},
      {"lt", &I::op_lt}, {"and", &I::op_and}, {"or", &I::op_or}, {"xor", &I::op_xor}, {"not", &I::op_not},
      {"bitshift", &I::op_bitshift}, {"true", &I::op_true}, {"false", &I::op_false}, {"exec", &I::op_exec},
      {"if", &I::op_if}, {"ifelse", &I::op_ifelse}, {"for", &I::op_for}, {"repeat", &I::op_repeat},
      {"loop", &I::op_loop}, {"exit", &I::op_exit}, {"stop", &I::op_stop}, {"stopped", &I::op_stopped},
      {"countexecstack", &I::op_countexecstack}, {"execstack", &I::op_execstack}, {"quit", &I::op_quit},
      {"start", &I::op_start}, {"array", &I::op_array}, {"packedarray", &I::op_packedarray},
      {"length", &I::op_length}, {"get", &I::op_get}, {"put", &I::op_put}, {"getinterval", &I::op_getinterval},
      {"putinterval", &I::op_putinterval}, {"aload", &I::op_aload}, {"astore", &I::op_astore},
      {"forall", &I::op_forall}, {"currentpacking", &I::op_currentpacking}, {"setpacking", &I::op_setpacking},
      {"[", &I::op_mark}, {"]", &I::op_endarray}, {"<<", &I::op_mark}, {">>", &I::op_enddict},
      {"dict", &I::op_dict}, {"maxlength", &I::op_maxlength}, {"begin", &I::op_begin}, {"end", &I::op_end},
      {"def", &I::op_def}, {"load", &I::op_load}, {"store", &I::op_store}, {"undef", &I::op_undef},
      {"known", &I::op_known}, {"where", &I::op_where}, {"currentdict", &I::op_currentdict},
      {"countdictstack", &I::op_countdictstack}, {"dictstack", &I::op_dictstack},
      {"cleardictstack", &I::op_cleardictstack}, {"string", &I::op_string}, {"anchorsearch", &I::op_anchorsearch},
      {"search", &I::op_search}, {"token", &I::op_token}, {"type", &I::op_type}, {"cvlit", &I::op_cvlit},
      {"cvx", &I::op_cvx}, {"xcheck", &I::op_xcheck}, {"executeonly", &I::op_executeonly},
      {"noaccess", &I::op_noaccess}, {"readonly", &I::op_readonly}, {"rcheck", &I::op_rcheck},
      {"wcheck", &I::op_wcheck}, {"cvi", &I::op_cvi}, {"cvn", &I::op_cvn}, {"cvr", &I::op_cvr},
      {"cvrs", &I::op_cvrs}, {"cvs", &I::op_cvs}, {"null", &I::op_null}, {"bind", &I::op_bind},
      {"save", &I::op_save}, {"restore", &I::op_restore}, {"vmstatus", &I::op_vmstatus},
      {"setglobal", &I::op_setglobal}, {"currentglobal", &I::op_currentglobal}, {"gcheck", &I::op_gcheck},
      {"print", &I::op_print}, {"=", &I::op_equals}, {"==", &I::op_eqeq}, {"=only", &I::op_equals},
      {"==only", &I::op_eqeq}, {"stack", &I::op_stack}, {"pstack", &I::op_pstack}, {"flush", &I::op_flush},
      {"currentfile", &I::op_currentfile}, {"file", &I::op_file}, {"closefile", &I::op_closefile},
      {"read", &I::op_read}, {"readhexstring", &I::op_readhexstring}, {"readstring", &I::op_readstring},
      {"readline", &I::op_readline}, {"bytesavailable", &I::op_bytesavailable}, {"flushfile", &I::op_flushfile},
      {"status", &I::op_status}, {"run", &I::op_run}, {"eexec", &I::op_eexec}, {"filter", &I::op_filter},
      {"write", &I::op_write}, {"writestring", &I::op_writestring}, {"writehexstring", &I::op_writehexstring},
      {"usertime", &I::op_usertime}, {"realtime", &I::op_realtime}, {"languagelevel", &I::op_languagelevel},
      {"product", &I::op_product}, {"version", &I::op_version}, {"revision", &I::op_revision},
      {"serialnumber", &I::op_serialnumber}, {"findresource", &I::op_findresource},
      {"defineresource", &I::op_defineresource}, {"undefineresource", &I::op_undefineresource},
      {"resourcestatus", &I::op_resourcestatus}, {"resourceforall", &I::op_resourceforall},
      {"dicttomark", &I::op_enddict}, {".dicttomark", &I::op_enddict}, {"setvmthreshold", &I::op_pop_noop},
      {"vmreclaim", &I::op_pop_noop}, {"handleerror", &I::op_noop}, {"deletefile", &I::op_deletefile},
      {"renamefile", &I::op_renamefile}, {"filenameforall", &I::op_filenameforall},
      {"setfileposition", &I::op_setfileposition}, {"fileposition", &I::op_fileposition},
      {"setobjectformat", &I::op_pop_noop}, {"currentobjectformat", &I::op_currentobjectformat},
      {"setuserparams", &I::op_pop_noop}, {"currentuserparams", &I::op_currentuserparams},
      {"setsystemparams", &I::op_pop_noop}, {"currentsystemparams", &I::op_currentuserparams},
      {"superexec", &I::op_exec}, {"internaldict", &I::op_internaldict}, {"echo", &I::op_pop_noop},
      {"prompt", &I::op_noop}, {"executive", &I::op_noop}, {"setucacheparams", &I::op_cleartomark},
      {"ucachestatus", &I::op_noop}, {"defineuserobject", &I::op_defineuserobject},
      {"execuserobject", &I::op_execuserobject}, {"undefineuserobject", &I::op_pop_noop},
      {"pdfmark", &I::op_cleartomark}, {"cleardictstack", &I::op_cleardictstack},
  };
  for (const auto& [name, fn] : ops) define_operator(name, fn);
}

// --------------------------------------------------------------------------- execution

const Object* Interpreter::find_name(const Object& name) const {
  for (std::size_t i = dstack_.size(); i-- > 0;) {
    if (const Object* v = dict_find(dstack_[i], name)) return v;
  }
  return nullptr;
}

Object Interpreter::lookup(const Object& name) const {
  if (const Object* v = find_name(name)) return *v;
  if (opts_.ai && ai_dummy_.type == Type::Dict && name.type == Type::Name && name_text(name).starts_with("Adobe_")) {
    return ai_dummy_;
  }
  error("undefined", name_text(name));
}

void Interpreter::tick() {
  if (++operations_ > opts_.max_operations) {
    fatal(Status::Limit, std::format("the PostScript program runs more than {} operations", opts_.max_operations));
  }
  if ((operations_ & 0xFFFF) == 0) {
    double fraction = 0.0;
    if (current_file_.type == Type::File && current_file_.ref < files_.size()) {
      const FileStore& f = files_[current_file_.ref];
      if (!f.data.empty()) fraction = static_cast<double>(f.pos) / static_cast<double>(f.data.size());
    }
    if (auto r = ctx_.checkpoint(std::min(0.95, fraction), "Interpreting"); !r) fatal(r.error().code, r.error().message);
  }
}

void Interpreter::push_frame(Frame frame) {
  if (estack_.size() >= 20'000) error("execstackoverflow");
  estack_.push_back(std::move(frame));
}

void Interpreter::call_operator(std::uint32_t index) {
  popped_.clear();
  (this->*operators_[index].fn)();
  popped_.clear();
}

void Interpreter::execute(const Object& o_in) {
  Object o = o_in;
  for (int hops = 0; hops < 64; ++hops) {
    if (!o.exec) {
      push(o);
      return;
    }
    switch (o.type) {
      case Type::Array: {
        if (o.size == 0) return;
        Frame f;
        f.kind = Frame::Kind::Proc;
        f.obj = o;
        push_frame(std::move(f));
        return;
      }
      case Type::Name: {
        if (const Object* v = find_name(o)) {
          o = *v;
          continue;
        }
        if (opts_.ai) {
          if (name_text(o).starts_with("Adobe_") && ai_dummy_.type == Type::Dict) {
            push(ai_dummy_);
          } else {
            warn_once(std::format("unknown Illustrator operator {} was ignored", name_text(o)));
          }
          return;
        }
        error("undefined", name_text(o));
      }
      case Type::Operator: tick(); call_operator(o.ref); return;
      case Type::File: {
        Frame f;
        f.kind = Frame::Kind::File;
        f.obj = o;
        push_frame(std::move(f));
        return;
      }
      case Type::String: {
        FileStore fs;
        fs.owned = std::make_shared<const std::string>(string_view(o));
        fs.data = *fs.owned;
        Frame f;
        f.kind = Frame::Kind::File;
        f.obj = make_file(std::move(fs));
        push_frame(std::move(f));
        return;
      }
      case Type::Null: return;
      default: push(o); return;
    }
  }
  error("limitcheck", "name indirection too deep");
}

void Interpreter::exec_token(const Object& o, bool /*from_proc*/) {
  if (!o.exec || o.type == Type::Array) {
    push(o);
    return;
  }
  if (o.type == Type::Operator) {
    call_operator(o.ref);
    return;
  }
  execute(o);
}

void Interpreter::step_frame() {
  tick();
  const std::size_t idx = estack_.size() - 1;
  Frame& f = estack_[idx];
  switch (f.kind) {
    case Frame::Kind::Proc: {
      if (f.pos >= f.obj.size) {
        estack_.pop_back();
        return;
      }
      const Object o = array_get(f.obj, f.pos++);
      if (f.pos >= f.obj.size) estack_.pop_back();  // tail call
      exec_token(o, true);
      return;
    }
    case Frame::Kind::File: {
      const Object fo = f.obj;
      std::optional<Object> tok = scan(fo);
      if (!tok) {
        if (estack_.size() > idx && estack_[idx].kind == Frame::Kind::File && estack_[idx].obj.ref == fo.ref) {
          estack_.erase(estack_.begin() + static_cast<std::ptrdiff_t>(idx));
        }
        return;
      }
      exec_token(*tok, true);
      return;
    }
    case Frame::Kind::For: {
      const bool done = f.inc >= 0 ? f.cur > f.limit : f.cur < f.limit;
      if (done) {
        estack_.pop_back();
        return;
      }
      const Object proc = f.proc;
      if (f.int_loop) push(Object::integer(static_cast<std::int64_t>(f.cur)));
      else push(Object::real(f.cur));
      f.cur += f.inc;
      execute(proc);
      return;
    }
    case Frame::Kind::Repeat: {
      if (f.count <= 0) {
        estack_.pop_back();
        return;
      }
      --f.count;
      const Object proc = f.proc;
      execute(proc);
      return;
    }
    case Frame::Kind::Loop: {
      const Object proc = f.proc;
      execute(proc);
      return;
    }
    case Frame::Kind::Forall: {
      const Object obj = f.obj;
      const Object proc = f.proc;
      const std::uint32_t pos = f.pos;
      if (obj.type == Type::Array) {
        if (pos >= obj.size) {
          estack_.pop_back();
          return;
        }
        f.pos++;
        push(array_get(obj, pos));
      } else if (obj.type == Type::String) {
        if (pos >= obj.size) {
          estack_.pop_back();
          return;
        }
        f.pos++;
        push_int(static_cast<unsigned char>(string_view(obj)[pos]));
      } else if (obj.type == Type::Dict) {
        const DictStore& d = dict(obj);
        if (pos >= d.entries.size()) {
          estack_.pop_back();
          return;
        }
        f.pos++;
        const auto entry = d.entries[pos];
        push(entry.first);
        push(entry.second);
      } else {
        estack_.pop_back();
        return;
      }
      execute(proc);
      return;
    }
    case Frame::Kind::Stopped:
      estack_.pop_back();
      push_bool(false);
      return;
  }
}

bool Interpreter::unwind_to_stopped(std::size_t base) {
  for (std::size_t i = estack_.size(); i-- > base;) {
    if (estack_[i].kind == Frame::Kind::Stopped) {
      estack_.resize(i);
      push_bool(true);
      return true;
    }
  }
  return false;
}

void Interpreter::handle_error(const PsError& e, std::size_t base) {
  // Restore the operands of the failing operator.
  for (std::size_t i = popped_.size(); i-- > 0;) {
    if (ostack_.size() < 500'000) ostack_.push_back(popped_[i]);
  }
  popped_.clear();
  dict_put(dollar_error_, "newerror", Object::boolean(true));
  dict_put(dollar_error_, "errorname", make_name(e.name));
  dict_put(dollar_error_, "errorinfo", make_string(e.detail));
  if (unwind_to_stopped(base)) return;
  throw e;
}

void Interpreter::run_until(std::size_t base) {
  while (estack_.size() > base) {
    try {
      step_frame();
    } catch (const PsError& e) {
      handle_error(e, base);
    } catch (const StopSignal&) {
      if (!unwind_to_stopped(base)) throw;
    }
  }
}

void Interpreter::call(const Object& proc) {
  if (nesting_ >= opts_.max_nesting) error("limitcheck", "callbacks nested too deeply");
  ++nesting_;
  std::vector<Object> saved = std::move(popped_);
  popped_.clear();
  const std::size_t base = estack_.size();
  bases_.push_back(base);
  try {
    if (proc.type == Type::Array && !proc.exec) {
      // A literal array used as a procedure (some data sources): execute it anyway.
      Object p = proc;
      p.exec = true;
      execute(p);
    } else {
      execute(proc);
    }
    run_until(base);
  } catch (...) {
    --nesting_;
    bases_.pop_back();
    popped_ = std::move(saved);
    throw;
  }
  --nesting_;
  bases_.pop_back();
  popped_ = std::move(saved);
}

// --------------------------------------------------------------------------- stack operators

void Interpreter::op_pop() { pop(); }
void Interpreter::op_exch() {
  need(2);
  std::swap(ostack_[ostack_.size() - 1], ostack_[ostack_.size() - 2]);
}
void Interpreter::op_dup() {
  need(1);
  push(ostack_.back());
}
void Interpreter::op_copy() {
  need(1);
  const Object top_obj = ostack_.back();
  if (top_obj.type == Type::Integer) {
    const std::int64_t n = top_obj.ival;
    if (n < 0) error("rangecheck");
    pop();
    need(static_cast<std::size_t>(n));
    if (ostack_.size() + static_cast<std::size_t>(n) > 500'000) error("stackoverflow");
    const std::size_t from = ostack_.size() - static_cast<std::size_t>(n);
    for (std::size_t i = 0; i < static_cast<std::size_t>(n); ++i) ostack_.push_back(ostack_[from + i]);
    return;
  }
  const Object dst = pop();
  const Object src = pop();
  if (src.type == Type::Array && dst.type == Type::Array) {
    if (src.size > dst.size) error("rangecheck");
    std::vector<Object> items(src.size);
    for (std::uint32_t i = 0; i < src.size; ++i) items[i] = array_get(src, i);
    for (std::uint32_t i = 0; i < src.size; ++i) array_put(dst, i, items[i]);
    Object r = dst;
    r.size = src.size;
    push(r);
  } else if (src.type == Type::String && dst.type == Type::String) {
    if (src.size > dst.size) error("rangecheck");
    const std::string bytes(string_view(src));
    string_write(dst, 0, bytes);
    Object r = dst;
    r.size = src.size;
    push(r);
  } else if (src.type == Type::Dict && dst.type == Type::Dict) {
    const auto entries = dict(src).entries;
    for (const auto& [k, v] : entries) dict_put(dst, k, v);
    push(dst);
  } else if (src.type == Type::GState && dst.type == Type::GState) {
    push(dst);
  } else {
    error("typecheck");
  }
}
void Interpreter::op_index() {
  const std::int64_t n = pop_int();
  if (n < 0 || static_cast<std::size_t>(n) >= ostack_.size()) error("rangecheck");
  push(ostack_[ostack_.size() - 1 - static_cast<std::size_t>(n)]);
}
void Interpreter::op_roll() {
  const std::int64_t j = pop_int();
  const std::int64_t n = pop_int();
  if (n < 0) error("rangecheck");
  need(static_cast<std::size_t>(n));
  if (n == 0) return;
  std::int64_t shift = j % n;
  if (shift < 0) shift += n;
  if (shift == 0) return;
  const auto first = ostack_.end() - static_cast<std::ptrdiff_t>(n);
  std::rotate(first, ostack_.end() - static_cast<std::ptrdiff_t>(shift), ostack_.end());
}
void Interpreter::op_clear() { ostack_.clear(); }
void Interpreter::op_count() { push_int(static_cast<std::int64_t>(ostack_.size())); }
void Interpreter::op_mark() { push(Object::mark()); }
void Interpreter::op_cleartomark() {
  for (std::size_t i = ostack_.size(); i-- > 0;) {
    if (ostack_[i].type == Type::Mark) {
      ostack_.resize(i);
      return;
    }
  }
  error("unmatchedmark");
}
void Interpreter::op_counttomark() {
  for (std::size_t i = ostack_.size(); i-- > 0;) {
    if (ostack_[i].type == Type::Mark) {
      push_int(static_cast<std::int64_t>(ostack_.size() - 1 - i));
      return;
    }
  }
  error("unmatchedmark");
}

// --------------------------------------------------------------------------- arithmetic

void Interpreter::op_add() {
  need(2);
  const Object b = pop(), a = pop();
  if (!a.is_number() || !b.is_number()) error("typecheck");
  if (a.type == Type::Integer && b.type == Type::Integer) {
    const std::int64_t r = static_cast<std::int64_t>(a.ival) + b.ival;
    if (r >= std::numeric_limits<std::int32_t>::min() && r <= std::numeric_limits<std::int32_t>::max()) push_int(r);
    else push_real(static_cast<double>(r));
  } else {
    push_real(a.number() + b.number());
  }
}
void Interpreter::op_sub() {
  need(2);
  const Object b = pop(), a = pop();
  if (!a.is_number() || !b.is_number()) error("typecheck");
  if (a.type == Type::Integer && b.type == Type::Integer) {
    const std::int64_t r = static_cast<std::int64_t>(a.ival) - b.ival;
    if (r >= std::numeric_limits<std::int32_t>::min() && r <= std::numeric_limits<std::int32_t>::max()) push_int(r);
    else push_real(static_cast<double>(r));
  } else {
    push_real(a.number() - b.number());
  }
}
void Interpreter::op_mul() {
  need(2);
  const Object b = pop(), a = pop();
  if (!a.is_number() || !b.is_number()) error("typecheck");
  if (a.type == Type::Integer && b.type == Type::Integer) {
    const std::int64_t r = static_cast<std::int64_t>(a.ival) * b.ival;
    if (r >= std::numeric_limits<std::int32_t>::min() && r <= std::numeric_limits<std::int32_t>::max()) push_int(r);
    else push_real(static_cast<double>(r));
  } else {
    push_real(a.number() * b.number());
  }
}
void Interpreter::op_div() {
  const double b = pop_number(), a = pop_number();
  if (b == 0.0) error("undefinedresult");
  push_real(a / b);
}
void Interpreter::op_idiv() {
  const std::int64_t b = pop_int(), a = pop_int();
  if (b == 0) error("undefinedresult");
  push_int(a / b);
}
void Interpreter::op_mod() {
  const std::int64_t b = pop_int(), a = pop_int();
  if (b == 0) error("undefinedresult");
  push_int(a % b);
}
void Interpreter::op_abs() {
  need(1);
  const Object a = pop();
  if (a.type == Type::Integer) {
    if (a.ival == std::numeric_limits<std::int32_t>::min()) push_real(2147483648.0);
    else push_int(std::abs(a.ival));
  } else if (a.type == Type::Real) {
    push_real(std::abs(a.rval));
  } else {
    error("typecheck");
  }
}
void Interpreter::op_neg() {
  need(1);
  const Object a = pop();
  if (a.type == Type::Integer) {
    if (a.ival == std::numeric_limits<std::int32_t>::min()) push_real(2147483648.0);
    else push_int(-static_cast<std::int64_t>(a.ival));
  } else if (a.type == Type::Real) {
    push_real(-a.rval);
  } else {
    error("typecheck");
  }
}
namespace {
template <class F>
void round_op(Interpreter& in, F f) {
  in.need(1);
  const Object a = in.pop();
  if (a.type == Type::Integer) in.push(a);
  else if (a.type == Type::Real) in.push_real(f(a.rval));
  else in.error("typecheck");
}
}  // namespace
void Interpreter::op_ceiling() { round_op(*this, [](double v) { return std::ceil(v); }); }
void Interpreter::op_floor() { round_op(*this, [](double v) { return std::floor(v); }); }
void Interpreter::op_round() { round_op(*this, [](double v) { return std::floor(v + 0.5); }); }
void Interpreter::op_truncate() { round_op(*this, [](double v) { return std::trunc(v); }); }
void Interpreter::op_sqrt() {
  const double a = pop_number();
  if (a < 0) error("rangecheck");
  push_real(std::sqrt(a));
}
void Interpreter::op_atan() {
  const double den = pop_number(), num = pop_number();
  if (num == 0 && den == 0) error("undefinedresult");
  double a = std::atan2(num, den) / deg;
  if (a < 0) a += 360.0;
  push_real(a);
}
void Interpreter::op_cos() { push_real(std::cos(pop_number() * deg)); }
void Interpreter::op_sin() { push_real(std::sin(pop_number() * deg)); }
void Interpreter::op_exp() {
  const double e = pop_number(), b = pop_number();
  const double r = std::pow(b, e);
  if (!std::isfinite(r)) error("undefinedresult");
  push_real(r);
}
void Interpreter::op_ln() {
  const double a = pop_number();
  if (a <= 0) error("rangecheck");
  push_real(std::log(a));
}
void Interpreter::op_log() {
  const double a = pop_number();
  if (a <= 0) error("rangecheck");
  push_real(std::log10(a));
}
void Interpreter::op_rand() {
  // Deterministic linear congruential generator.
  rand_seed_ = rand_seed_ * 1103515245u + 12345u;
  push_int(static_cast<std::int64_t>((rand_seed_ >> 1) & 0x7FFFFFFF));
}
void Interpreter::op_srand() { rand_seed_ = static_cast<std::uint32_t>(pop_int()); }
void Interpreter::op_rrand() { push_int(static_cast<std::int64_t>(rand_seed_ & 0x7FFFFFFF)); }

// --------------------------------------------------------------------------- relational / boolean

void Interpreter::op_eq() {
  need(2);
  const Object b = pop(), a = pop();
  push_bool(equal(a, b));
}
void Interpreter::op_ne() {
  need(2);
  const Object b = pop(), a = pop();
  push_bool(!equal(a, b));
}
namespace {
int compare_objects(Interpreter& in, const Object& a, const Object& b) {
  if (a.is_number() && b.is_number()) {
    const double x = a.number(), y = b.number();
    return x < y ? -1 : (x > y ? 1 : 0);
  }
  if (a.type == Type::String && b.type == Type::String) {
    const int c = in.string_view(a).compare(in.string_view(b));
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
  }
  in.error("typecheck");
}
}  // namespace
void Interpreter::op_ge() {
  need(2);
  const Object b = pop(), a = pop();
  push_bool(compare_objects(*this, a, b) >= 0);
}
void Interpreter::op_gt() {
  need(2);
  const Object b = pop(), a = pop();
  push_bool(compare_objects(*this, a, b) > 0);
}
void Interpreter::op_le() {
  need(2);
  const Object b = pop(), a = pop();
  push_bool(compare_objects(*this, a, b) <= 0);
}
void Interpreter::op_lt() {
  need(2);
  const Object b = pop(), a = pop();
  push_bool(compare_objects(*this, a, b) < 0);
}
namespace {
template <class FB, class FI>
void logic_op(Interpreter& in, FB fb, FI fi) {
  in.need(2);
  const Object b = in.pop(), a = in.pop();
  if (a.type == Type::Boolean && b.type == Type::Boolean) in.push_bool(fb(a.ival != 0, b.ival != 0));
  else if (a.type == Type::Integer && b.type == Type::Integer) in.push_int(fi(a.ival, b.ival));
  else in.error("typecheck");
}
}  // namespace
void Interpreter::op_and() {
  logic_op(*this, [](bool x, bool y) { return x && y; }, [](std::int32_t x, std::int32_t y) { return x & y; });
}
void Interpreter::op_or() {
  logic_op(*this, [](bool x, bool y) { return x || y; }, [](std::int32_t x, std::int32_t y) { return x | y; });
}
void Interpreter::op_xor() {
  logic_op(*this, [](bool x, bool y) { return x != y; }, [](std::int32_t x, std::int32_t y) { return x ^ y; });
}
void Interpreter::op_not() {
  need(1);
  const Object a = pop();
  if (a.type == Type::Boolean) push_bool(a.ival == 0);
  else if (a.type == Type::Integer) push_int(~a.ival);
  else error("typecheck");
}
void Interpreter::op_bitshift() {
  const std::int64_t shift = pop_int();
  const std::int64_t v = pop_int();
  std::uint32_t u = static_cast<std::uint32_t>(static_cast<std::int32_t>(v));
  if (shift >= 32 || shift <= -32) u = 0;
  else if (shift > 0) u <<= shift;
  else if (shift < 0) u >>= -shift;
  push_int(static_cast<std::int32_t>(u));
}
void Interpreter::op_true() { push_bool(true); }
void Interpreter::op_false() { push_bool(false); }

// --------------------------------------------------------------------------- control

void Interpreter::op_exec() {
  const Object o = pop();
  execute(o);
}
void Interpreter::op_if() {
  const Object proc = pop_proc();
  if (pop_bool()) execute(proc);
}
void Interpreter::op_ifelse() {
  const Object p2 = pop_proc();
  const Object p1 = pop_proc();
  if (pop_bool()) execute(p1);
  else execute(p2);
}
void Interpreter::op_for() {
  const Object proc = pop_proc();
  need(3);
  const Object limit = pop(), inc = pop(), init = pop();
  if (!limit.is_number() || !inc.is_number() || !init.is_number()) error("typecheck");
  Frame f;
  f.kind = Frame::Kind::For;
  f.proc = proc;
  f.cur = init.number();
  f.inc = inc.number();
  f.limit = limit.number();
  f.int_loop = init.type == Type::Integer && inc.type == Type::Integer;
  if (f.inc == 0) {
    // PostScript loops forever; bounded by the operation limit.
  }
  push_frame(std::move(f));
}
void Interpreter::op_repeat() {
  const Object proc = pop_proc();
  const std::int64_t n = pop_int();
  if (n < 0) error("rangecheck");
  Frame f;
  f.kind = Frame::Kind::Repeat;
  f.proc = proc;
  f.count = n;
  push_frame(std::move(f));
}
void Interpreter::op_loop() {
  const Object proc = pop_proc();
  Frame f;
  f.kind = Frame::Kind::Loop;
  f.proc = proc;
  push_frame(std::move(f));
}
void Interpreter::op_exit() {
  const std::size_t floor = bases_.empty() ? 0 : bases_.back();
  for (std::size_t i = estack_.size(); i-- > floor;) {
    const Frame::Kind k = estack_[i].kind;
    if (k == Frame::Kind::Stopped) break;
    if (k == Frame::Kind::For || k == Frame::Kind::Repeat || k == Frame::Kind::Loop || k == Frame::Kind::Forall) {
      estack_.resize(i);
      return;
    }
  }
  error("invalidexit");
}
void Interpreter::op_stop() { throw StopSignal{}; }
void Interpreter::op_stopped() {
  const Object proc = pop();
  Frame f;
  f.kind = Frame::Kind::Stopped;
  push_frame(std::move(f));
  execute(proc);
}
void Interpreter::op_countexecstack() { push_int(static_cast<std::int64_t>(estack_.size())); }
void Interpreter::op_execstack() {
  const Object arr = pop_array();
  const std::size_t n = std::min<std::size_t>(arr.size, estack_.size());
  for (std::size_t i = 0; i < n; ++i) array_put(arr, i, estack_[i].obj);
  Object r = arr;
  r.size = static_cast<std::uint32_t>(n);
  push(r);
}
void Interpreter::op_quit() { throw QuitSignal{}; }
void Interpreter::op_start() {}

// --------------------------------------------------------------------------- arrays

void Interpreter::op_array() {
  const std::int64_t n = pop_int();
  if (n < 0) error("rangecheck");
  push(make_array(static_cast<std::size_t>(n)));
}
void Interpreter::op_packedarray() {
  const std::int64_t n = pop_int();
  if (n < 0) error("rangecheck");
  need(static_cast<std::size_t>(n));
  Object arr = make_array(static_cast<std::size_t>(n));
  const std::size_t from = ostack_.size() - static_cast<std::size_t>(n);
  std::copy(ostack_.begin() + static_cast<std::ptrdiff_t>(from), ostack_.end(), arrays_[arr.ref].items.begin());
  ostack_.resize(from);
  arr.readonly = true;
  arr.packed = true;
  push(arr);
}
void Interpreter::op_endarray() {
  std::size_t mark_at = ostack_.size();
  for (std::size_t i = ostack_.size(); i-- > 0;) {
    if (ostack_[i].type == Type::Mark) {
      mark_at = i;
      break;
    }
  }
  if (mark_at == ostack_.size()) error("unmatchedmark");
  const std::size_t n = ostack_.size() - mark_at - 1;
  Object arr = make_array(n);
  std::copy(ostack_.begin() + static_cast<std::ptrdiff_t>(mark_at + 1), ostack_.end(), arrays_[arr.ref].items.begin());
  ostack_.resize(mark_at);
  push(arr);
}
void Interpreter::op_enddict() {
  std::size_t mark_at = ostack_.size();
  for (std::size_t i = ostack_.size(); i-- > 0;) {
    if (ostack_[i].type == Type::Mark) {
      mark_at = i;
      break;
    }
  }
  if (mark_at == ostack_.size()) error("unmatchedmark");
  const std::size_t n = ostack_.size() - mark_at - 1;
  if (n % 2 != 0) error("rangecheck");
  Object d = make_dict(n / 2);
  for (std::size_t i = mark_at + 1; i + 1 < ostack_.size(); i += 2) dict_put(d, ostack_[i], ostack_[i + 1]);
  ostack_.resize(mark_at);
  push(d);
}
void Interpreter::op_length() {
  const Object o = pop();
  switch (o.type) {
    case Type::Array:
    case Type::String: push_int(o.size); break;
    case Type::Dict: push_int(static_cast<std::int64_t>(dict(o).entries.size())); break;
    case Type::Name: push_int(static_cast<std::int64_t>(name_text(o).size())); break;
    default: error("typecheck");
  }
}
void Interpreter::op_get() {
  need(2);
  const Object key = pop();
  const Object o = pop();
  switch (o.type) {
    case Type::Array: {
      if (key.type != Type::Integer) error("typecheck");
      if (key.ival < 0 || static_cast<std::uint32_t>(key.ival) >= o.size) error("rangecheck");
      push(array_get(o, static_cast<std::size_t>(key.ival)));
      break;
    }
    case Type::String: {
      if (key.type != Type::Integer) error("typecheck");
      if (key.ival < 0 || static_cast<std::uint32_t>(key.ival) >= o.size) error("rangecheck");
      push_int(static_cast<unsigned char>(string_view(o)[static_cast<std::size_t>(key.ival)]));
      break;
    }
    case Type::Dict: {
      const Object* v = dict_find(o, key);
      if (v == nullptr) error("undefined", key.type == Type::Name ? name_text(key) : std::string_view{});
      push(*v);
      break;
    }
    default: error("typecheck");
  }
}
void Interpreter::op_put() {
  need(3);
  const Object value = pop();
  const Object key = pop();
  const Object o = pop();
  switch (o.type) {
    case Type::Array: {
      if (key.type != Type::Integer) error("typecheck");
      if (key.ival < 0 || static_cast<std::uint32_t>(key.ival) >= o.size) error("rangecheck");
      array_put(o, static_cast<std::size_t>(key.ival), value);
      break;
    }
    case Type::String: {
      if (key.type != Type::Integer || value.type != Type::Integer) error("typecheck");
      if (key.ival < 0 || static_cast<std::uint32_t>(key.ival) >= o.size) error("rangecheck");
      string_put(o, static_cast<std::size_t>(key.ival), static_cast<std::uint8_t>(value.ival & 0xFF));
      break;
    }
    case Type::Dict: dict_put(o, key, value); break;
    default: error("typecheck");
  }
}
void Interpreter::op_getinterval() {
  const std::int64_t count = pop_int();
  const std::int64_t index = pop_int();
  const Object o = pop();
  if (o.type != Type::Array && o.type != Type::String) error("typecheck");
  if (index < 0 || count < 0 || static_cast<std::uint64_t>(index) + static_cast<std::uint64_t>(count) > o.size) {
    error("rangecheck");
  }
  Object r = o;
  r.start = o.start + static_cast<std::uint32_t>(index);
  r.size = static_cast<std::uint32_t>(count);
  push(r);
}
void Interpreter::op_putinterval() {
  need(3);
  const Object src = pop();
  const std::int64_t index = pop_int();
  const Object dst = pop();
  if (index < 0) error("rangecheck");
  if (dst.type == Type::Array && src.type == Type::Array) {
    if (static_cast<std::uint64_t>(index) + src.size > dst.size) error("rangecheck");
    std::vector<Object> items(src.size);
    for (std::uint32_t i = 0; i < src.size; ++i) items[i] = array_get(src, i);
    for (std::uint32_t i = 0; i < src.size; ++i) array_put(dst, static_cast<std::size_t>(index) + i, items[i]);
  } else if (dst.type == Type::String && src.type == Type::String) {
    if (static_cast<std::uint64_t>(index) + src.size > dst.size) error("rangecheck");
    const std::string bytes(string_view(src));
    string_write(dst, static_cast<std::size_t>(index), bytes);
  } else {
    error("typecheck");
  }
}
void Interpreter::op_aload() {
  const Object arr = pop_array();
  if (ostack_.size() + arr.size > 500'000) error("stackoverflow");
  for (std::uint32_t i = 0; i < arr.size; ++i) push(array_get(arr, i));
  push(arr);
}
void Interpreter::op_astore() {
  const Object arr = pop_array();
  need(arr.size);
  const std::size_t from = ostack_.size() - arr.size;
  for (std::uint32_t i = 0; i < arr.size; ++i) array_put(arr, i, ostack_[from + i]);
  ostack_.resize(from);
  push(arr);
}
void Interpreter::op_forall() {
  const Object proc = pop_proc();
  const Object obj = pop();
  if (obj.type != Type::Array && obj.type != Type::String && obj.type != Type::Dict) error("typecheck");
  Frame f;
  f.kind = Frame::Kind::Forall;
  f.obj = obj;
  f.proc = proc;
  push_frame(std::move(f));
}
void Interpreter::op_currentpacking() { push_bool(false); }
void Interpreter::op_setpacking() { pop_bool(); }

// --------------------------------------------------------------------------- dictionaries

void Interpreter::op_dict() {
  const std::int64_t n = pop_int();
  if (n < 0) error("rangecheck");
  push(make_dict(static_cast<std::size_t>(std::min<std::int64_t>(n, 65535))));
}
void Interpreter::op_maxlength() {
  const Object d = pop_dict();
  const DictStore& s = dict(d);
  push_int(std::max<std::int64_t>(s.capacity, static_cast<std::int64_t>(s.entries.size()) + 1));
}
void Interpreter::op_begin() { begin_dict(pop_dict()); }
void Interpreter::op_end() {
  if (dstack_.size() <= permanent_dicts_) error("dictstackunderflow");
  dstack_.pop_back();
}
void Interpreter::op_def() {
  need(2);
  const Object value = pop();
  const Object key = pop();
  dict_put(dstack_.back(), key, value);
}
void Interpreter::op_load() {
  const Object key = pop();
  const Object* v = find_name(key);
  if (v == nullptr) error("undefined", key.type == Type::Name ? name_text(key) : std::string_view{});
  push(*v);
}
void Interpreter::op_store() {
  need(2);
  const Object value = pop();
  const Object key = pop();
  for (std::size_t i = dstack_.size(); i-- > 0;) {
    if (dict_find(dstack_[i], key) != nullptr) {
      dict_put(dstack_[i], key, value);
      return;
    }
  }
  dict_put(dstack_.back(), key, value);
}
void Interpreter::op_undef() {
  need(2);
  const Object key = pop();
  const Object d = pop_dict();
  dict_remove(d, key);
}
void Interpreter::op_known() {
  need(2);
  const Object key = pop();
  const Object d = pop_dict();
  push_bool(dict_find(d, key) != nullptr);
}
void Interpreter::op_where() {
  const Object key = pop();
  for (std::size_t i = dstack_.size(); i-- > 0;) {
    if (dict_find(dstack_[i], key) != nullptr) {
      push(dstack_[i]);
      push_bool(true);
      return;
    }
  }
  push_bool(false);
}
void Interpreter::op_currentdict() { push(dstack_.back()); }
void Interpreter::op_countdictstack() { push_int(static_cast<std::int64_t>(dstack_.size())); }
void Interpreter::op_dictstack() {
  const Object arr = pop_array();
  if (arr.size < dstack_.size()) error("rangecheck");
  for (std::size_t i = 0; i < dstack_.size(); ++i) array_put(arr, i, dstack_[i]);
  Object r = arr;
  r.size = static_cast<std::uint32_t>(dstack_.size());
  push(r);
}
void Interpreter::op_cleardictstack() { dstack_.resize(permanent_dicts_); }

// --------------------------------------------------------------------------- strings

void Interpreter::op_string() {
  const std::int64_t n = pop_int();
  if (n < 0) error("rangecheck");
  push(make_string(static_cast<std::size_t>(n)));
}
void Interpreter::op_anchorsearch() {
  const Object seek = pop_string();
  const Object s = pop_string();
  const std::string_view sv = string_view(s), kv = string_view(seek);
  if (sv.starts_with(kv)) {
    Object post = s, match = s;
    post.start = s.start + seek.size;
    post.size = s.size - seek.size;
    match.size = seek.size;
    push(post);
    push(match);
    push_bool(true);
  } else {
    push(s);
    push_bool(false);
  }
}
void Interpreter::op_search() {
  const Object seek = pop_string();
  const Object s = pop_string();
  const std::string_view sv = string_view(s), kv = string_view(seek);
  const std::size_t at = sv.find(kv);
  if (at == std::string_view::npos) {
    push(s);
    push_bool(false);
    return;
  }
  Object pre = s, match = s, post = s;
  pre.size = static_cast<std::uint32_t>(at);
  match.start = s.start + static_cast<std::uint32_t>(at);
  match.size = seek.size;
  post.start = match.start + seek.size;
  post.size = s.size - static_cast<std::uint32_t>(at) - seek.size;
  push(post);
  push(match);
  push(pre);
  push_bool(true);
}
void Interpreter::op_token() {
  const Object src = pop();
  if (src.type == Type::File) {
    std::optional<Object> tok = scan(src);
    if (tok) {
      push(*tok);
      push_bool(true);
    } else {
      push_bool(false);
    }
    return;
  }
  if (src.type != Type::String) error("typecheck");
  FileStore fs;
  fs.owned = std::make_shared<const std::string>(string_view(src));
  fs.data = *fs.owned;
  const Object f = make_file(std::move(fs));
  std::optional<Object> tok = scan(f);
  if (!tok) {
    push_bool(false);
    return;
  }
  const std::size_t used = std::min<std::size_t>(file(f).pos, src.size);
  Object post = src;
  post.start = src.start + static_cast<std::uint32_t>(used);
  post.size = src.size - static_cast<std::uint32_t>(used);
  push(post);
  push(*tok);
  push_bool(true);
}

// --------------------------------------------------------------------------- types / conversion

void Interpreter::op_type() {
  const Object o = pop();
  Object n = make_name(type_name(o.type));
  n.exec = true;
  push(n);
}
void Interpreter::op_cvlit() {
  Object o = pop();
  o.exec = false;
  push(o);
}
void Interpreter::op_cvx() {
  Object o = pop();
  o.exec = true;
  push(o);
}
void Interpreter::op_xcheck() { push_bool(pop().exec); }
void Interpreter::op_executeonly() {
  need(1);
  ostack_.back().readonly = true;
}
void Interpreter::op_noaccess() {
  need(1);
  ostack_.back().readonly = true;
}
void Interpreter::op_readonly() {
  need(1);
  ostack_.back().readonly = true;
}
void Interpreter::op_rcheck() {
  pop();
  push_bool(true);
}
void Interpreter::op_wcheck() {
  const Object o = pop();
  bool writable = !o.readonly;
  if (o.type == Type::Dict) writable = !dict(o).readonly && !o.readonly;
  push_bool(writable);
}
void Interpreter::op_cvi() {
  const Object o = pop();
  double v = 0;
  if (o.is_number()) {
    v = o.number();
  } else if (o.type == Type::String) {
    FileStore fs;
    fs.owned = std::make_shared<const std::string>(string_view(o));
    fs.data = *fs.owned;
    const Object f = make_file(std::move(fs));
    std::optional<Object> tok = scan(f);
    if (!tok || !tok->is_number()) error("typecheck");
    v = tok->number();
  } else {
    error("typecheck");
  }
  v = std::trunc(v);
  if (!(v >= -2147483648.0 && v <= 2147483647.0)) error("rangecheck");
  push_int(static_cast<std::int64_t>(v));
}
void Interpreter::op_cvn() {
  const Object o = pop();
  if (o.type == Type::Name) {
    push(o);
    return;
  }
  if (o.type != Type::String) error("typecheck");
  push(make_name(string_view(o), o.exec));
}
void Interpreter::op_cvr() {
  const Object o = pop();
  if (o.is_number()) {
    push_real(o.number());
  } else if (o.type == Type::String) {
    FileStore fs;
    fs.owned = std::make_shared<const std::string>(string_view(o));
    fs.data = *fs.owned;
    const Object f = make_file(std::move(fs));
    std::optional<Object> tok = scan(f);
    if (!tok || !tok->is_number()) error("typecheck");
    push_real(tok->number());
  } else {
    error("typecheck");
  }
}
void Interpreter::op_cvrs() {
  const Object s = pop_string();
  const std::int64_t radix = pop_int();
  const double num = pop_number();
  if (radix < 2 || radix > 36) error("rangecheck");
  std::string text;
  if (radix == 10) {
    text = cvs(int_or_real(num).type == Type::Integer ? Object::integer(static_cast<std::int64_t>(num)) : Object::real(num));
  } else {
    const std::uint32_t v = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int64_t>(num)));
    char buf[40];
    auto r = std::to_chars(buf, buf + sizeof(buf), v, static_cast<int>(radix));
    text.assign(buf, r.ptr);
    for (char& c : text) c = util::ascii_upper(c);
  }
  if (text.size() > s.size) error("rangecheck");
  string_write(s, 0, text);
  Object r = s;
  r.size = static_cast<std::uint32_t>(text.size());
  push(r);
}
void Interpreter::op_cvs() {
  const Object s = pop_string();
  const Object o = pop();
  const std::string text = cvs(o);
  if (text.size() > s.size) error("rangecheck");
  string_write(s, 0, text);
  Object r = s;
  r.size = static_cast<std::uint32_t>(text.size());
  push(r);
}
void Interpreter::op_null() { push(Object::null()); }

void Interpreter::op_bind() {
  need(1);
  const Object proc = ostack_.back();
  if (proc.type != Type::Array) return;
  std::set<std::uint32_t> visited;
  std::vector<Object> todo{proc};
  std::size_t work = 0;
  while (!todo.empty()) {
    const Object p = todo.back();
    todo.pop_back();
    if (!visited.insert(p.ref).second) continue;
    for (std::uint32_t i = 0; i < p.size; ++i) {
      if (++work > 1'000'000) return;
      const Object e = array_get(p, i);
      if (e.type == Type::Name && e.exec) {
        const Object* v = find_name(e);
        if (v != nullptr && v->type == Type::Operator) array_put(p, i, *v);
      } else if (e.type == Type::Array && e.exec) {
        todo.push_back(e);
      }
    }
  }
}

// --------------------------------------------------------------------------- VM

void Interpreter::op_save() {
  if (saves_.size() >= 200) error("limitcheck", "too many nested saves");
  SaveRecord rec;
  rec.journal_start = journal_.size();
  rec.gstack_depth = gstack_.size();
  rec.state = gstate_;
  saves_.push_back(std::move(rec));
  Object o;
  o.type = Type::Save;
  o.ref = static_cast<std::uint32_t>(saves_.size());
  push(o);
}
void Interpreter::op_restore() {
  const Object s = pop_type(Type::Save);
  if (s.ref == 0 || s.ref > saves_.size()) error("invalidrestore");
  const SaveRecord rec = saves_[s.ref - 1];
  restoring_ = true;
  for (std::size_t i = journal_.size(); i-- > rec.journal_start;) {
    const Journal& j = journal_[i];
    switch (j.kind) {
      case Journal::Kind::ArrayItem:
        if (j.store < arrays_.size() && j.pos < arrays_[j.store].items.size()) arrays_[j.store].items[j.pos] = j.old;
        break;
      case Journal::Kind::StringBytes:
        if (j.store < strings_.size() && j.pos + j.bytes.size() <= strings_[j.store].bytes.size()) {
          strings_[j.store].bytes.replace(j.pos, j.bytes.size(), j.bytes);
        }
        break;
      case Journal::Kind::DictEntry: {
        Object d;
        d.type = Type::Dict;
        d.ref = j.store;
        if (j.existed) dict_put(d, j.key, j.old);
        else dict_remove(d, j.key);
        break;
      }
    }
  }
  restoring_ = false;
  journal_.resize(rec.journal_start);
  saves_.resize(s.ref - 1);
  gstack_.resize(std::min(gstack_.size(), rec.gstack_depth));
  gstate_ = rec.state;
}
void Interpreter::op_vmstatus() {
  push_int(static_cast<std::int64_t>(saves_.size()));
  push_int(static_cast<std::int64_t>(std::min<std::uint64_t>(vm_bytes_, 2'000'000'000)));
  push_int(static_cast<std::int64_t>(std::min<std::uint64_t>(opts_.max_vm_bytes, 2'000'000'000)));
}
void Interpreter::op_setglobal() { global_alloc_ = pop_bool(); }
void Interpreter::op_currentglobal() { push_bool(global_alloc_); }
void Interpreter::op_gcheck() {
  const Object o = pop();
  bool global = false;
  if (o.type == Type::Dict && o.ref < dicts_.size()) global = dicts_[o.ref].global;
  else if (o.type == Type::Array && o.ref < arrays_.size()) global = arrays_[o.ref].global;
  else if (o.type == Type::String && o.ref < strings_.size()) global = strings_[o.ref].global;
  else global = true;
  push_bool(global);
}

// --------------------------------------------------------------------------- output (discarded)

void Interpreter::op_print() { pop_string(); }
void Interpreter::op_equals() { pop(); }
void Interpreter::op_eqeq() { pop(); }
void Interpreter::op_stack() {}
void Interpreter::op_pstack() {}
void Interpreter::op_flush() {}
void Interpreter::op_noop() {}
void Interpreter::op_pop_noop() { pop(); }
void Interpreter::op_usertime() { push_int(static_cast<std::int64_t>(operations_ / 10000)); }
void Interpreter::op_realtime() { push_int(static_cast<std::int64_t>(operations_ / 10000)); }
void Interpreter::op_languagelevel() { push_int(3); }
void Interpreter::op_product() { push(make_string("Convertible")); }
void Interpreter::op_version() { push(make_string("3010")); }
void Interpreter::op_revision() { push_int(1); }
void Interpreter::op_serialnumber() { push_int(0); }
void Interpreter::op_currentobjectformat() { push_int(0); }
void Interpreter::op_currentuserparams() { push(make_dict(4)); }
void Interpreter::op_internaldict() {
  pop_int();
  push(userdict_);
}
void Interpreter::op_defineuserobject() {
  need(2);
  pop();
  pop();
}
void Interpreter::op_execuserobject() {
  pop_int();
  error("undefined", "user objects are not supported");
}

// --------------------------------------------------------------------------- files

void Interpreter::op_currentfile() {
  for (std::size_t i = estack_.size(); i-- > 0;) {
    if (estack_[i].kind == Frame::Kind::File) {
      push(estack_[i].obj);
      return;
    }
  }
  FileStore fs;
  fs.closed = true;
  push(make_file(std::move(fs)));
}
void Interpreter::op_file() {
  const Object access = pop_string();
  const Object name = pop_string();
  const std::string_view n = string_view(name);
  (void)access;
  if (n == "%stdin" || n == "%stdin%") {
    push(current_file_);
    return;
  }
  if (n == "%stdout" || n == "%stderr" || n == "%stdout%" || n == "%stderr%" || n.starts_with("%pipe%") ||
      string_view(access).find('w') != std::string_view::npos) {
    FileStore fs;
    fs.writable = true;
    push(make_file(std::move(fs)));
    return;
  }
  error("undefinedfilename", "file access is not available");
}
void Interpreter::op_closefile() {
  const Object f = pop_type(Type::File);
  FileStore& fs = file(f);
  fs.closed = true;
  fs.pending = -1;
}
void Interpreter::op_read() {
  const Object f = pop_type(Type::File);
  const int c = file_get(file(f));
  if (c < 0) {
    push_bool(false);
  } else {
    push_int(c);
    push_bool(true);
  }
}
void Interpreter::op_readhexstring() {
  const Object s = pop_string();
  const Object f = pop_type(Type::File);
  std::string out;
  out.reserve(s.size);
  int hi = -1;
  while (out.size() < s.size) {
    const int c = file_get(file(f));
    if (c < 0) break;
    int v = -1;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    if (v < 0) continue;
    if (hi < 0) {
      hi = v;
    } else {
      out.push_back(static_cast<char>(hi * 16 + v));
      hi = -1;
    }
  }
  string_write(s, 0, out);
  Object r = s;
  r.size = static_cast<std::uint32_t>(out.size());
  push(r);
  push_bool(out.size() == s.size);
}
void Interpreter::op_readstring() {
  const Object s = pop_string();
  const Object f = pop_type(Type::File);
  const std::string out = file_read(f, s.size);
  string_write(s, 0, out);
  Object r = s;
  r.size = static_cast<std::uint32_t>(out.size());
  push(r);
  push_bool(out.size() == s.size);
}
void Interpreter::op_readline() {
  const Object s = pop_string();
  const Object f = pop_type(Type::File);
  std::string out;
  bool found = false;
  while (true) {
    const int c = file_get(file(f));
    if (c < 0) break;
    if (c == '\n') {
      found = true;
      break;
    }
    if (c == '\r') {
      if (file_peek(file(f)) == '\n') file_get(file(f));
      found = true;
      break;
    }
    if (out.size() >= s.size) error("rangecheck");
    out.push_back(static_cast<char>(c));
  }
  string_write(s, 0, out);
  Object r = s;
  r.size = static_cast<std::uint32_t>(out.size());
  push(r);
  push_bool(found || !out.empty());
}
void Interpreter::op_bytesavailable() {
  const Object f = pop_type(Type::File);
  materialize(f.ref);
  const FileStore& fs = file(f);
  if (fs.kind == FileStore::Kind::Buffer && !fs.closed) {
    push_int(static_cast<std::int64_t>(std::min<std::size_t>(fs.data.size() - std::min(fs.pos, fs.data.size()),
                                                              2'000'000'000)));
  } else {
    push_int(-1);
  }
}
void Interpreter::op_flushfile() {
  const Object f = pop_type(Type::File);
  materialize(f.ref);
  FileStore& fs = file(f);
  if (fs.writable) return;
  if (fs.kind == FileStore::Kind::Buffer) {
    if (f.ref != current_file_.ref) fs.pos = fs.data.size();
    return;
  }
  // Read (and discard) everything that is left.
  std::size_t guard = 0;
  while (file_get(file(f)) >= 0) {
    if (++guard > 512u * 1024u * 1024u) break;
  }
}
void Interpreter::op_status() {
  const Object o = pop();
  if (o.type == Type::File) {
    push_bool(!file(o).closed);
  } else if (o.type == Type::String) {
    push_bool(false);
  } else {
    error("typecheck");
  }
}
void Interpreter::op_run() {
  pop_string();
  error("undefinedfilename", "file access is not available");
}
void Interpreter::op_eexec() {
  const Object src = pop();
  Object source = src;
  if (src.type == Type::String) {
    FileStore fs;
    fs.owned = std::make_shared<const std::string>(string_view(src));
    fs.data = *fs.owned;
    source = make_file(std::move(fs));
  } else if (src.type != Type::File) {
    error("typecheck");
  }
  // Hex or binary? Look at the first four bytes after skipping whitespace.
  materialize(source.ref);
  FileStore& sfs = file(source);
  bool hex = false;
  if (sfs.kind == FileStore::Kind::Buffer) {
    std::size_t p = sfs.pos;
    while (p < sfs.data.size() && (sfs.data[p] == ' ' || sfs.data[p] == '\t' || sfs.data[p] == '\r' ||
                                    sfs.data[p] == '\n')) {
      ++p;
    }
    hex = p + 4 <= sfs.data.size();
    for (std::size_t k = 0; hex && k < 4; ++k) {
      const char c = sfs.data[p + k];
      hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }
    if (hex) sfs.pos = p;
  }
  FileStore fs;
  fs.kind = FileStore::Kind::Eexec;
  fs.source = source.ref;
  fs.r = 55665;
  fs.hex = hex;
  const Object ef = make_file(std::move(fs));
  for (int k = 0; k < 4; ++k) file_get(file(ef));
  begin_dict(systemdict_);
  Frame f;
  f.kind = Frame::Kind::File;
  f.obj = ef;
  push_frame(std::move(f));
}
void Interpreter::op_filter() {
  const Object name = pop_type(Type::Name);
  const std::string n(name_text(name));
  Object params;
  if (n == "SubFileDecode" || n == "SFD") {
    need(1);
    if (top().type == Type::Dict) {
      params = pop();
    } else {
      const Object eod = pop_string();
      const std::int64_t count = pop_int();
      params = make_array(2);
      array_put(params, 0, Object::integer(count));
      array_put(params, 1, eod);
    }
  } else {
    need(1);
    if (top().type == Type::Dict) {
      // Either the parameters or (for an encode filter) nothing; a dict
      // can never be a data source.
      params = pop();
    }
  }
  const Object src = pop();
  push(filter(src, n, params));
}
void Interpreter::op_write() {
  pop_int();
  const Object f = pop_type(Type::File);
  if (!file(f).writable) error("ioerror");
}
void Interpreter::op_writestring() {
  pop_string();
  const Object f = pop_type(Type::File);
  if (!file(f).writable) error("ioerror");
}
void Interpreter::op_writehexstring() { op_writestring(); }
void Interpreter::op_deletefile() {
  pop_string();
  error("undefinedfilename");
}
void Interpreter::op_renamefile() {
  pop_string();
  pop_string();
  error("undefinedfilename");
}
void Interpreter::op_filenameforall() {
  pop_string();
  pop_proc();
  pop_string();
}
void Interpreter::op_setfileposition() {
  const std::int64_t pos = pop_int();
  const Object f = pop_type(Type::File);
  materialize(f.ref);
  FileStore& fs = file(f);
  if (fs.kind != FileStore::Kind::Buffer || pos < 0 || static_cast<std::uint64_t>(pos) > fs.data.size()) {
    error("ioerror");
  }
  fs.pos = static_cast<std::size_t>(pos);
}
void Interpreter::op_fileposition() {
  const Object f = pop_type(Type::File);
  const FileStore& fs = file(f);
  push_int(static_cast<std::int64_t>(std::min<std::size_t>(fs.pos, 2'000'000'000)));
}

// --------------------------------------------------------------------------- resources

void Interpreter::op_findresource() {
  const Object category = pop();
  const Object key = pop();
  const std::string cat = object_string(category);
  if (cat == "Font" || cat == "CIDFont") {
    push(find_font(key));
    return;
  }
  const std::string k = object_string(key);
  if (cat == "Encoding") {
    if (k == "StandardEncoding") {
      push(standard_encoding_);
      return;
    }
    if (k == "ISOLatin1Encoding") {
      push(isolatin1_encoding_);
      return;
    }
    if (k == "SymbolEncoding") {
      push(symbol_encoding_);
      return;
    }
  }
  const Object* resources = dict_find(systemdict_, ".resources");
  if (resources != nullptr) {
    if (const Object* catdict = dict_find(*resources, category)) {
      if (const Object* v = dict_find(*catdict, key)) {
        push(*v);
        return;
      }
    }
  }
  if (cat == "Category") {
    if (resources != nullptr) {
      if (const Object* catdict = dict_find(*resources, key)) {
        push(*catdict);
        return;
      }
    }
  }
  if (cat == "ProcSet" && opts_.ai && ai_dummy_.type == Type::Dict) {
    push(ai_dummy_);
    return;
  }
  error("undefinedresource", k);
}
void Interpreter::op_defineresource() {
  need(3);
  const Object category = pop();
  const Object instance = pop();
  const Object key = pop();
  const std::string cat = object_string(category);
  if (cat == "Font" || cat == "CIDFont") {
    push(key);
    push(instance);
    op_definefont();
    return;
  }
  const Object* resources = dict_find(systemdict_, ".resources");
  if (resources != nullptr) {
    const Object* catdict = dict_find(*resources, category);
    if (catdict == nullptr) {
      const Object d = make_dict(16);
      dict_put(*resources, category, d);
      catdict = dict_find(*resources, category);
    }
    if (catdict != nullptr) dict_put(*catdict, key, instance);
  }
  push(instance);
}
void Interpreter::op_undefineresource() {
  need(2);
  const Object category = pop();
  const Object key = pop();
  if (const Object* resources = dict_find(systemdict_, ".resources")) {
    if (const Object* catdict = dict_find(*resources, category)) dict_remove(*catdict, key);
  }
}
void Interpreter::op_resourcestatus() {
  const Object category = pop();
  const Object key = pop();
  const std::string cat = object_string(category);
  bool found = false;
  if (cat == "Font") {
    found = dict_find(fontdir_, key) != nullptr;
  } else if (cat == "Encoding") {
    const std::string k = object_string(key);
    found = k == "StandardEncoding" || k == "ISOLatin1Encoding";
  }
  if (!found) {
    if (const Object* resources = dict_find(systemdict_, ".resources")) {
      if (const Object* catdict = dict_find(*resources, category)) found = dict_find(*catdict, key) != nullptr;
    }
  }
  if (found) {
    push_int(1);
    push_int(0);
    push_bool(true);
  } else {
    push_bool(false);
  }
}
void Interpreter::op_resourceforall() {
  need(4);
  pop();
  pop();
  pop();
  pop();
}

// --------------------------------------------------------------------------- program

Result<Scene> Interpreter::run_program(std::string_view program) {
  FileStore fs;
  fs.data = program;
  current_file_ = make_file(std::move(fs));
  stdin_file_ = current_file_;
  Frame f;
  f.kind = Frame::Kind::File;
  f.obj = current_file_;
  estack_.push_back(f);
  start_page();

  std::optional<Error> program_error;
  try {
    run_until(0);
  } catch (const PsError& e) {
    program_error = Error{Status::Decode, e.detail.empty()
                                              ? std::format("PostScript error: {}", e.name)
                                              : std::format("PostScript error: {} ({})", e.name, e.detail)};
  } catch (const StopSignal&) {
    // A top-level stop ends the job.
  } catch (const QuitSignal&) {
  } catch (const FatalError& e) {
    if (e.error.code == Status::Cancelled) return fail(e.error);
    program_error = e.error;
  }
  // Make sure no capture is left open after an abort.
  while (captures_.size() > 1) end_capture();
  if (program_error) {
    if (drawn_total_ == 0 || program_error->code == Status::Limit) return fail(*program_error);
    ctx_.warn(std::format("the PostScript program stopped early: {}", program_error->message));
  }
  finish_page(true);
  Scene scene;
  scene.pages = std::move(pages_);
  scene.source_units = "pt";
  scene.metadata = metadata_;
  if (scene.pages.empty()) return fail(Status::Decode, "the PostScript program produced no page");
  return scene;
}

}  // namespace cvt::vector::ps
