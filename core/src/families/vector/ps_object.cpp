// Convertible core - vector family: PostScript objects and virtual memory.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <format>
#include <utility>

#include "families/vector/ps_internal.hpp"

namespace cvt::vector::ps {

std::string_view type_name(Type type) noexcept {
  switch (type) {
    case Type::Null: return "nulltype";
    case Type::Integer: return "integertype";
    case Type::Real: return "realtype";
    case Type::Boolean: return "booleantype";
    case Type::Name: return "nametype";
    case Type::String: return "stringtype";
    case Type::Array: return "arraytype";
    case Type::Dict: return "dicttype";
    case Type::Operator: return "operatortype";
    case Type::Mark: return "marktype";
    case Type::File: return "filetype";
    case Type::Save: return "savetype";
    case Type::FontID: return "fonttype";
    case Type::GState: return "gstatetype";
  }
  return "nulltype";
}

void Interpreter::charge(std::uint64_t bytes) {
  vm_bytes_ += bytes;
  if (vm_bytes_ > opts_.max_vm_bytes) {
    fatal(Status::Limit, std::format("the PostScript program allocates more than {} bytes", opts_.max_vm_bytes));
  }
}

Object Interpreter::make_name(std::string_view text, bool exec) {
  Object o;
  o.type = Type::Name;
  o.exec = exec;
  const auto it = name_ids_.find(std::string(text));
  if (it != name_ids_.end()) {
    o.ref = it->second;
    return o;
  }
  if (names_.size() >= 4'000'000) fatal(Status::Limit, "too many distinct names in the PostScript program");
  charge(text.size() + 32);
  o.ref = static_cast<std::uint32_t>(names_.size());
  names_.emplace_back(text);
  name_ids_.emplace(std::string(text), o.ref);
  return o;
}

Object Interpreter::make_string(std::string_view bytes) {
  if (bytes.size() > 65535u * 1024u) error("limitcheck", "string too long");
  charge(bytes.size() + 24);
  Object o;
  o.type = Type::String;
  o.ref = static_cast<std::uint32_t>(strings_.size());
  o.size = static_cast<std::uint32_t>(bytes.size());
  StringStore store;
  store.bytes.assign(bytes);
  store.global = global_alloc_;
  store.level = save_level();
  strings_.push_back(std::move(store));
  return o;
}

Object Interpreter::make_string(std::size_t size) {
  if (size > 65535u * 1024u) error("limitcheck", "string too long");
  charge(size + 24);
  Object o;
  o.type = Type::String;
  o.ref = static_cast<std::uint32_t>(strings_.size());
  o.size = static_cast<std::uint32_t>(size);
  StringStore store;
  store.bytes.assign(size, '\0');
  store.global = global_alloc_;
  store.level = save_level();
  strings_.push_back(std::move(store));
  return o;
}

Object Interpreter::make_array(std::size_t size) {
  if (size > 1'000'000) error("limitcheck", "array too long");
  charge(size * sizeof(Object) + 24);
  Object o;
  o.type = Type::Array;
  o.ref = static_cast<std::uint32_t>(arrays_.size());
  o.size = static_cast<std::uint32_t>(size);
  ArrayStore store;
  store.items.resize(size);
  store.global = global_alloc_;
  store.level = save_level();
  arrays_.push_back(std::move(store));
  return o;
}

Object Interpreter::make_dict(std::size_t capacity) {
  if (capacity > 1'000'000) error("limitcheck", "dictionary too large");
  charge(64 + capacity * 8);
  Object o;
  o.type = Type::Dict;
  o.ref = static_cast<std::uint32_t>(dicts_.size());
  DictStore store;
  store.capacity = static_cast<std::uint32_t>(capacity);
  store.global = global_alloc_;
  store.level = save_level();
  dicts_.push_back(std::move(store));
  return o;
}

Object Interpreter::make_operator(std::string_view name) {
  for (std::size_t i = 0; i < operators_.size(); ++i) {
    if (operators_[i].name == name) {
      Object o;
      o.type = Type::Operator;
      o.exec = true;
      o.ref = static_cast<std::uint32_t>(i);
      return o;
    }
  }
  return Object::null();
}

Object Interpreter::make_file(FileStore file) {
  charge(64);
  Object o;
  o.type = Type::File;
  o.exec = false;
  o.ref = static_cast<std::uint32_t>(files_.size());
  files_.push_back(std::move(file));
  return o;
}

std::string_view Interpreter::name_text(const Object& name) const {
  if (name.type != Type::Name || name.ref >= names_.size()) return {};
  return names_[name.ref];
}

std::string_view Interpreter::string_view(const Object& str) const {
  if (str.type != Type::String || str.ref >= strings_.size()) return {};
  const std::string& b = strings_[str.ref].bytes;
  if (static_cast<std::size_t>(str.start) + str.size > b.size()) return {};
  return std::string_view(b).substr(str.start, str.size);
}

std::string& Interpreter::string_bytes(const Object& str) { return strings_.at(str.ref).bytes; }

std::string Interpreter::object_string(const Object& o) const {
  if (o.type == Type::Name) return std::string(name_text(o));
  if (o.type == Type::String) return std::string(string_view(o));
  return {};
}

Object* Interpreter::array_data(const Object& arr) {
  if (arr.type != Type::Array || arr.ref >= arrays_.size()) return nullptr;
  auto& items = arrays_[arr.ref].items;
  if (static_cast<std::size_t>(arr.start) + arr.size > items.size()) return nullptr;
  return items.data() + arr.start;
}

const Object* Interpreter::array_data(const Object& arr) const {
  if (arr.type != Type::Array || arr.ref >= arrays_.size()) return nullptr;
  const auto& items = arrays_[arr.ref].items;
  if (static_cast<std::size_t>(arr.start) + arr.size > items.size()) return nullptr;
  return items.data() + arr.start;
}

Object Interpreter::array_get(const Object& arr, std::size_t i) const {
  const Object* data = array_data(arr);
  if (data == nullptr || i >= arr.size) return Object::null();
  return data[i];
}

void Interpreter::array_put(const Object& arr, std::size_t i, const Object& value) {
  if (arr.type != Type::Array || arr.ref >= arrays_.size() || i >= arr.size) error("rangecheck");
  ArrayStore& store = arrays_[arr.ref];
  const std::size_t at = static_cast<std::size_t>(arr.start) + i;
  if (at >= store.items.size()) error("rangecheck");
  if (!restoring_ && !store.global && store.level < save_level()) {
    Journal j;
    j.kind = Journal::Kind::ArrayItem;
    j.store = arr.ref;
    j.pos = static_cast<std::uint32_t>(at);
    j.old = store.items[at];
    journal_.push_back(std::move(j));
    charge(sizeof(Journal));
  }
  store.items[at] = value;
}

void Interpreter::string_put(const Object& str, std::size_t i, std::uint8_t value) {
  const char c = static_cast<char>(value);
  string_write(str, i, std::string_view(&c, 1));
}

void Interpreter::string_write(const Object& str, std::size_t at, std::string_view bytes) {
  if (str.type != Type::String || str.ref >= strings_.size() || at + bytes.size() > str.size) error("rangecheck");
  StringStore& store = strings_[str.ref];
  const std::size_t pos = static_cast<std::size_t>(str.start) + at;
  if (pos + bytes.size() > store.bytes.size()) error("rangecheck");
  if (!restoring_ && !store.global && store.level < save_level() && !bytes.empty()) {
    Journal j;
    j.kind = Journal::Kind::StringBytes;
    j.store = str.ref;
    j.pos = static_cast<std::uint32_t>(pos);
    j.bytes = store.bytes.substr(pos, bytes.size());
    charge(bytes.size() + sizeof(Journal));
    journal_.push_back(std::move(j));
  }
  std::memcpy(store.bytes.data() + pos, bytes.data(), bytes.size());
}

DictStore& Interpreter::dict(const Object& d) {
  if (d.type != Type::Dict || d.ref >= dicts_.size()) error("typecheck");
  return dicts_[d.ref];
}

const DictStore& Interpreter::dict(const Object& d) const {
  if (d.type != Type::Dict || d.ref >= dicts_.size()) error("typecheck");
  return dicts_[d.ref];
}

Key Interpreter::key_of(const Object& k) const {
  Key key;
  switch (k.type) {
    case Type::Name: key.kind = 1; key.a = k.ref; break;
    case Type::String: {
      const std::string_view text = string_view(k);
      const auto it = name_ids_.find(std::string(text));
      if (it != name_ids_.end()) {
        key.kind = 1;
        key.a = it->second;
      } else {
        // A string that was never a name: hash its text (it cannot collide
        // with a name entry because the kind differs).
        key.kind = 9;
        key.a = std::hash<std::string_view>{}(text);
        key.b = text.size();
      }
      break;
    }
    case Type::Integer: key.kind = 2; key.a = static_cast<std::uint64_t>(static_cast<std::int64_t>(k.ival)); break;
    case Type::Real:
      if (std::isfinite(k.rval) && k.rval == std::floor(k.rval) && std::abs(k.rval) < 2147483648.0) {
        key.kind = 2;
        key.a = static_cast<std::uint64_t>(static_cast<std::int64_t>(k.rval));
      } else {
        key.kind = 3;
        key.a = std::bit_cast<std::uint64_t>(k.rval);
      }
      break;
    case Type::Boolean: key.kind = 4; key.a = static_cast<std::uint64_t>(k.ival); break;
    case Type::Null: key.kind = 5; break;
    default:
      key.kind = static_cast<std::uint8_t>(16 + static_cast<int>(k.type));
      key.a = k.ref;
      key.b = (static_cast<std::uint64_t>(k.start) << 32) | k.size;
      break;
  }
  return key;
}

const Object* Interpreter::dict_find(const Object& d, const Object& key) const {
  if (d.type != Type::Dict || d.ref >= dicts_.size()) return nullptr;
  const DictStore& store = dicts_[d.ref];
  const auto it = store.index.find(key_of(key));
  if (it == store.index.end()) return nullptr;
  return &store.entries[it->second].second;
}

const Object* Interpreter::dict_find(const Object& d, std::string_view key) const {
  if (d.type != Type::Dict || d.ref >= dicts_.size()) return nullptr;
  const auto id = name_ids_.find(std::string(key));
  if (id == name_ids_.end()) return nullptr;
  Key k;
  k.kind = 1;
  k.a = id->second;
  const DictStore& store = dicts_[d.ref];
  const auto it = store.index.find(k);
  if (it == store.index.end()) return nullptr;
  return &store.entries[it->second].second;
}

void Interpreter::dict_put(const Object& d, const Object& key_in, const Object& value) {
  if (key_in.type == Type::Null) error("typecheck", "null key");
  // Strings used as keys become names (PostScript semantics).
  Object key = key_in;
  if (key.type == Type::String) key = make_name(string_view(key_in));
  DictStore& store = dict(d);
  const Key k = key_of(key);
  const auto it = store.index.find(k);
  const bool journal = !restoring_ && !store.global && store.level < save_level();
  if (it != store.index.end()) {
    if (journal) {
      Journal j;
      j.kind = Journal::Kind::DictEntry;
      j.store = d.ref;
      j.key = key;
      j.old = store.entries[it->second].second;
      j.existed = true;
      journal_.push_back(std::move(j));
      charge(sizeof(Journal));
    }
    store.entries[it->second].second = value;
    return;
  }
  if (store.entries.size() >= 2'000'000) error("dictfull");
  if (journal) {
    Journal j;
    j.kind = Journal::Kind::DictEntry;
    j.store = d.ref;
    j.key = key;
    j.existed = false;
    journal_.push_back(std::move(j));
    charge(sizeof(Journal));
  }
  charge(2 * sizeof(Object) + 16);
  store.index.emplace(k, static_cast<std::uint32_t>(store.entries.size()));
  store.entries.emplace_back(key, value);
}

void Interpreter::dict_put(const Object& d, std::string_view key, const Object& value) {
  dict_put(d, make_name(key), value);
}

bool Interpreter::dict_remove(const Object& d, const Object& key) {
  DictStore& store = dict(d);
  const auto it = store.index.find(key_of(key));
  if (it == store.index.end()) return false;
  const std::uint32_t slot = it->second;
  if (!restoring_ && !store.global && store.level < save_level()) {
    Journal j;
    j.kind = Journal::Kind::DictEntry;
    j.store = d.ref;
    j.key = store.entries[slot].first;
    j.old = store.entries[slot].second;
    j.existed = true;
    journal_.push_back(std::move(j));
  }
  store.index.erase(it);
  const std::uint32_t last = static_cast<std::uint32_t>(store.entries.size() - 1);
  if (slot != last) {
    store.entries[slot] = std::move(store.entries[last]);
    store.index[key_of(store.entries[slot].first)] = slot;
  }
  store.entries.pop_back();
  return true;
}

FileStore& Interpreter::file(const Object& f) {
  if (f.type != Type::File || f.ref >= files_.size()) error("typecheck");
  return files_[f.ref];
}

bool Interpreter::equal(const Object& a, const Object& b) const {
  if (a.is_number() && b.is_number()) {
    if (a.type == Type::Integer && b.type == Type::Integer) return a.ival == b.ival;
    return a.number() == b.number();
  }
  const bool a_text = a.type == Type::Name || a.type == Type::String;
  const bool b_text = b.type == Type::Name || b.type == Type::String;
  if (a_text && b_text) {
    if (a.type == Type::Name && b.type == Type::Name) return a.ref == b.ref;
    const std::string_view x = a.type == Type::Name ? name_text(a) : string_view(a);
    const std::string_view y = b.type == Type::Name ? name_text(b) : string_view(b);
    return x == y;
  }
  if (a.type != b.type) return false;
  switch (a.type) {
    case Type::Null:
    case Type::Mark: return true;
    case Type::Boolean: return a.ival == b.ival;
    case Type::Array: return a.ref == b.ref && a.start == b.start && a.size == b.size;
    default: return a.ref == b.ref;
  }
}

namespace {

std::string format_real(double v) {
  if (!std::isfinite(v)) return std::isnan(v) ? "nan" : (v > 0 ? "inf" : "-inf");
  char buf[64];
  // PostScript prints reals with up to 6 significant digits.
  auto r = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::general, 6);
  std::string s(buf, r.ptr);
  if (s.find_first_of(".en") == std::string::npos) s += ".0";
  return s;
}

}  // namespace

std::string Interpreter::cvs(const Object& o) const {
  switch (o.type) {
    case Type::Integer: return std::to_string(o.ival);
    case Type::Real: return format_real(o.rval);
    case Type::Boolean: return o.ival != 0 ? "true" : "false";
    case Type::String: return std::string(string_view(o));
    case Type::Name: return std::string(name_text(o));
    case Type::Operator: return o.ref < operators_.size() ? operators_[o.ref].name : "--operator--";
    default: return "--nostringval--";
  }
}

// --------------------------------------------------------------------------- stacks

void Interpreter::push(const Object& o) {
  if (ostack_.size() >= 500'000) error("stackoverflow");
  ostack_.push_back(o);
}

void Interpreter::need(std::size_t n) const {
  if (ostack_.size() < n) error("stackunderflow");
}

Object Interpreter::pop() {
  if (ostack_.empty()) error("stackunderflow");
  Object o = ostack_.back();
  ostack_.pop_back();
  popped_.push_back(o);
  return o;
}

Object& Interpreter::top(std::size_t depth) {
  if (ostack_.size() <= depth) error("stackunderflow");
  return ostack_[ostack_.size() - 1 - depth];
}

double Interpreter::pop_number() {
  need(1);
  if (!ostack_.back().is_number()) error("typecheck", "number expected");
  return pop().number();
}

std::int64_t Interpreter::pop_int() {
  need(1);
  const Object& o = ostack_.back();
  if (o.type == Type::Integer) return pop().ival;
  if (o.type == Type::Real && o.rval == std::floor(o.rval) && std::abs(o.rval) < 2147483648.0) {
    return static_cast<std::int64_t>(pop().rval);
  }
  error("typecheck", "integer expected");
}

bool Interpreter::pop_bool() {
  need(1);
  if (ostack_.back().type != Type::Boolean) error("typecheck", "boolean expected");
  return pop().ival != 0;
}

Object Interpreter::pop_type(Type type) {
  need(1);
  if (ostack_.back().type != type) error("typecheck", std::format("{} expected", type_name(type)));
  return pop();
}

Object Interpreter::pop_array() {
  need(1);
  if (ostack_.back().type != Type::Array) error("typecheck", "array expected");
  return pop();
}

Object Interpreter::pop_proc() {
  need(1);
  const Object& o = ostack_.back();
  if (o.type == Type::Array || o.type == Type::Operator || o.type == Type::Name || o.type == Type::File ||
      o.type == Type::String) {
    return pop();
  }
  error("typecheck", "procedure expected");
}

Matrix Interpreter::to_matrix(const Object& arr) const {
  if (arr.type != Type::Array || arr.size != 6) error("rangecheck", "matrix expected");
  double v[6];
  for (std::uint32_t i = 0; i < 6; ++i) {
    const Object e = array_get(arr, i);
    if (!e.is_number()) error("typecheck", "matrix expected");
    v[i] = e.number();
  }
  return {v[0], v[1], v[2], v[3], v[4], v[5]};
}

void Interpreter::store_matrix(const Object& arr, const Matrix& m) {
  if (arr.type != Type::Array || arr.size != 6) error("rangecheck", "matrix expected");
  const double v[6] = {m.a, m.b, m.c, m.d, m.e, m.f};
  for (std::uint32_t i = 0; i < 6; ++i) array_put(arr, i, Object::real(v[i]));
}

Matrix Interpreter::pop_matrix() {
  const Object arr = pop_array();
  return to_matrix(arr);
}

void Interpreter::error(std::string_view name, std::string_view detail) const {
  throw PsError{std::string(name), std::string(detail)};
}

void Interpreter::fatal(Status status, std::string message) const {
  throw FatalError{Error{status, std::move(message)}};
}

}  // namespace cvt::vector::ps
