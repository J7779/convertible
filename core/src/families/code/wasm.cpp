// Convertible core - code family: WebAssembly binary -> text.
//
//   wasm -> wat   a disassembler producing the text format in the layout of
//                 wabt's wasm2wat: MVP plus sign-extension, saturating
//                 float-to-int, multi-value, bulk memory and reference types
//   wat           readable as source code (highlighted, to HTML/PDF ...)
//
// Every count, index and length in the binary is checked before use; nesting
// depth and the number of entries are bounded. Custom sections (names,
// producers) are skipped. SIMD, threads, GC and exceptions are reported as
// unsupported rather than guessed at. Assembling text back into a binary is
// not implemented.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <array>
#include <algorithm>
#include <iterator>
#include <bit>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "engine/registry.hpp"
#include "families/code/code.hpp"
#include "families/code/wasm.hpp"
#include "util/bytes.hpp"
#include "util/text.hpp"

namespace cvt::code {

namespace {

constexpr std::size_t max_entries = 1'000'000;
constexpr int max_depth = 1024;

struct FuncType {
  std::vector<std::uint8_t> params, results;
};

std::string_view valtype_name(std::uint8_t t) {
  switch (t) {
    case 0x7F: return "i32";
    case 0x7E: return "i64";
    case 0x7D: return "f32";
    case 0x7C: return "f64";
    case 0x7B: return "v128";
    case 0x70: return "funcref";
    case 0x6F: return "externref";
    default: return {};
  }
}

class Reader {
 public:
  explicit Reader(ByteView data) : data_(data) {}
  [[nodiscard]] bool at_end() const { return pos_ >= data_.size(); }
  [[nodiscard]] std::size_t position() const { return pos_; }
  Result<std::uint8_t> u8() {
    if (pos_ >= data_.size()) return fail(Status::Decode, "unexpected end of the module at offset {}", pos_);
    return std::to_integer<std::uint8_t>(data_[pos_++]);
  }
  Result<std::uint64_t> leb_u(unsigned bits) {
    std::uint64_t value = 0;
    for (unsigned shift = 0;; shift += 7) {
      CVT_TRY_ASSIGN(const std::uint8_t b, u8());
      if (shift >= bits || (shift + 7 > bits && (b & 0x7F) >> (bits - shift) != 0)) {
        return fail(Status::Decode, "LEB128 number too large at offset {}", pos_);
      }
      value |= std::uint64_t{b & 0x7Fu} << shift;
      if ((b & 0x80) == 0) return value;
    }
  }
  Result<std::int64_t> leb_s(unsigned bits) {
    std::int64_t value = 0;
    unsigned shift = 0;
    std::uint8_t b = 0;
    do {
      CVT_TRY_ASSIGN(b, u8());
      if (shift >= bits) return fail(Status::Decode, "LEB128 number too large at offset {}", pos_);
      value |= static_cast<std::int64_t>(std::uint64_t{b & 0x7Fu} << shift);
      shift += 7;
    } while (b & 0x80);
    if (shift < 64 && (b & 0x40)) value |= static_cast<std::int64_t>(~std::uint64_t{0} << shift);
    return value;
  }
  Result<std::uint32_t> u32() {
    CVT_TRY_ASSIGN(const std::uint64_t v, leb_u(32));
    return static_cast<std::uint32_t>(v);
  }
  Result<std::uint32_t> count() {
    CVT_TRY_ASSIGN(const std::uint32_t n, u32());
    if (n > max_entries || n > data_.size() - pos_ + 1) return fail(Status::Decode, "implausible count {} at offset {}", n, pos_);
    return n;
  }
  Result<ByteView> bytes(std::size_t n) {
    if (n > data_.size() - pos_) return fail(Status::Decode, "truncated module at offset {}", pos_);
    const ByteView out = data_.subspan(pos_, n);
    pos_ += n;
    return out;
  }
  Result<std::string> name() {
    CVT_TRY_ASSIGN(const std::uint32_t n, u32());
    CVT_TRY_ASSIGN(const ByteView raw, bytes(n));
    return std::string(as_chars(raw));
  }

 private:
  ByteView data_;
  std::size_t pos_ = 0;
};

std::string quoted(std::string_view raw) {
  std::string out = "\"";
  for (const char c : raw) {
    const auto u = static_cast<unsigned char>(c);
    if (u >= 0x20 && u < 0x7F && c != '"' && c != '\\') {
      out.push_back(c);
    } else {
      out += std::format("\\{:02x}", u);
    }
  }
  out.push_back('"');
  return out;
}

std::string float_text(double value, bool is_f32) {
  if (std::isnan(value)) {
    const std::uint64_t bits = is_f32 ? std::bit_cast<std::uint32_t>(static_cast<float>(value)) : std::bit_cast<std::uint64_t>(value);
    const bool negative = is_f32 ? (bits >> 31) != 0 : (bits >> 63) != 0;
    const std::uint64_t payload = is_f32 ? (bits & 0x7FFFFF) : (bits & 0xFFFFFFFFFFFFFULL);
    const std::uint64_t canonical = is_f32 ? 0x400000 : 0x8000000000000ULL;
    return std::string(negative ? "-" : "") + (payload == canonical ? "nan" : std::format("nan:0x{:x}", payload));
  }
  if (std::isinf(value)) return value < 0 ? "-inf" : "inf";
  std::string hex = is_f32 ? std::format("{:a}", static_cast<float>(value)) : std::format("{:a}", value);
  const bool negative = hex.starts_with('-');
  if (negative) hex.erase(0, 1);
  std::string out = std::string(negative ? "-" : "") + "0x" + hex;
  if (value == 0) out = negative ? "-0x0p+0" : "0x0p+0";
  const std::string decimal = is_f32 ? std::format("{}", static_cast<float>(value)) : std::format("{}", value);
  return out + " (;=" + decimal + ";)";
}

class Disassembler {
 public:
  explicit Disassembler(ByteView data, const Context& ctx) : r_(data), ctx_(ctx) {}

  Result<std::string> run() {
    CVT_TRY_ASSIGN(const ByteView magic, r_.bytes(8));
    if (as_chars(magic) != std::string_view("\0asm\x01\0\0\0", 8)) {
      return fail(Status::Decode, "not a WebAssembly module (version 1)");
    }
    while (!r_.at_end()) {
      CVT_TRY(ctx_.checkpoint(0.5, "Disassembling"));
      CVT_TRY_ASSIGN(const std::uint8_t id, r_.u8());
      CVT_TRY_ASSIGN(const std::uint32_t size, r_.u32());
      CVT_TRY_ASSIGN(const ByteView body, r_.bytes(size));
      Reader s(body);
      switch (id) {
        case 0: break;  // custom sections carry no semantics
        case 1: CVT_TRY(types(s)); break;
        case 2: CVT_TRY(imports(s)); break;
        case 3: CVT_TRY(functions(s)); break;
        case 4: CVT_TRY(tables(s)); break;
        case 5: CVT_TRY(memories(s)); break;
        case 6: CVT_TRY(globals(s)); break;
        case 7: CVT_TRY(exports(s)); break;
        case 8: {
          CVT_TRY_ASSIGN(const std::uint32_t start, s.u32());
          start_ = std::format("  (start {})", start);
          break;
        }
        case 9: CVT_TRY(elements(s)); break;
        case 10: CVT_TRY(code(s)); break;
        case 11: CVT_TRY(datas(s)); break;
        case 12: CVT_TRY(s.u32()); break;  // data count: implied by the data section
        default: return fail(Status::Decode, "unsupported section {} (only WebAssembly 2.0 sections are understood)", id);
      }
      if (id != 0 && !s.at_end()) return fail(Status::Decode, "section {} has trailing bytes", id);
    }
    if (declared_.size() != bodies_.size()) return fail(Status::Decode, "function and code sections disagree");
    std::string out = "(module";
    const auto block = [&out](const std::vector<std::string>& lines) {
      for (const std::string& line : lines) out += "\n" + line;
    };
    block(type_lines_);
    block(import_lines_);
    block(bodies_);
    block(table_lines_);
    block(memory_lines_);
    block(global_lines_);
    block(export_lines_);
    if (!start_.empty()) out += "\n" + start_;
    block(elem_lines_);
    block(data_lines_);
    out += ")\n";
    return out;
  }

 private:
  Result<std::uint8_t> valtype(Reader& s) {
    CVT_TRY_ASSIGN(const std::uint8_t t, s.u8());
    if (valtype_name(t).empty()) return fail(Status::Decode, "unknown value type 0x{:02X}", t);
    if (t == 0x7B) return fail(Status::Decode, "SIMD (v128) is not supported");
    return t;
  }

  static std::string signature(const FuncType& type) {
    std::string out;
    if (!type.params.empty()) {
      out += " (param";
      for (const std::uint8_t t : type.params) out += " " + std::string(valtype_name(t));
      out += ")";
    }
    if (!type.results.empty()) {
      out += " (result";
      for (const std::uint8_t t : type.results) out += " " + std::string(valtype_name(t));
      out += ")";
    }
    return out;
  }

  Result<void> types(Reader& s) {
    CVT_TRY_ASSIGN(const std::uint32_t n, s.count());
    for (std::uint32_t i = 0; i < n; ++i) {
      CVT_TRY_ASSIGN(const std::uint8_t form, s.u8());
      if (form != 0x60) return fail(Status::Decode, "unsupported type form 0x{:02X}", form);
      FuncType type;
      CVT_TRY_ASSIGN(const std::uint32_t params, s.count());
      for (std::uint32_t k = 0; k < params; ++k) {
        CVT_TRY_ASSIGN(const std::uint8_t t, valtype(s));
        type.params.push_back(t);
      }
      CVT_TRY_ASSIGN(const std::uint32_t results, s.count());
      for (std::uint32_t k = 0; k < results; ++k) {
        CVT_TRY_ASSIGN(const std::uint8_t t, valtype(s));
        type.results.push_back(t);
      }
      type_lines_.push_back(std::format("  (type (;{};) (func{}))", types_.size(), signature(type)));
      types_.push_back(std::move(type));
    }
    return {};
  }

  Result<std::string> limits(Reader& s) {
    CVT_TRY_ASSIGN(const std::uint8_t flags, s.u8());
    if (flags > 3) return fail(Status::Decode, "unsupported limits flags 0x{:02X} (memory64?)", flags);
    CVT_TRY_ASSIGN(const std::uint32_t min, s.u32());
    std::string out = std::to_string(min);
    if (flags & 1) {
      CVT_TRY_ASSIGN(const std::uint32_t max, s.u32());
      out += " " + std::to_string(max);
    }
    if (flags & 2) out += " shared";
    return out;
  }

  Result<std::string> table_type(Reader& s) {
    CVT_TRY_ASSIGN(const std::uint8_t ref, s.u8());
    if (ref != 0x70 && ref != 0x6F) return fail(Status::Decode, "unknown table element type 0x{:02X}", ref);
    CVT_TRY_ASSIGN(const std::string lim, limits(s));
    return lim + " " + std::string(valtype_name(ref));
  }

  Result<std::string> global_type(Reader& s) {
    CVT_TRY_ASSIGN(const std::uint8_t t, valtype(s));
    CVT_TRY_ASSIGN(const std::uint8_t mut, s.u8());
    if (mut > 1) return fail(Status::Decode, "invalid global mutability");
    return mut ? std::format("(mut {})", valtype_name(t)) : std::string(valtype_name(t));
  }

  Result<void> imports(Reader& s) {
    CVT_TRY_ASSIGN(const std::uint32_t n, s.count());
    for (std::uint32_t i = 0; i < n; ++i) {
      CVT_TRY_ASSIGN(const std::string module, s.name());
      CVT_TRY_ASSIGN(const std::string field, s.name());
      CVT_TRY_ASSIGN(const std::uint8_t kind, s.u8());
      std::string desc;
      switch (kind) {
        case 0: {
          CVT_TRY_ASSIGN(const std::uint32_t type, s.u32());
          if (type >= types_.size()) return fail(Status::Decode, "imported function uses unknown type {}", type);
          func_types_.push_back(type);
          desc = std::format("(func (;{};) (type {}))", func_types_.size() - 1, type);
          ++imported_funcs_;
          break;
        }
        case 1: {
          CVT_TRY_ASSIGN(const std::string t, table_type(s));
          desc = std::format("(table (;{};) {})", tables_++, t);
          break;
        }
        case 2: {
          CVT_TRY_ASSIGN(const std::string l, limits(s));
          desc = std::format("(memory (;{};) {})", memories_++, l);
          break;
        }
        case 3: {
          CVT_TRY_ASSIGN(const std::string g, global_type(s));
          desc = std::format("(global (;{};) {})", globals_++, g);
          break;
        }
        default: return fail(Status::Decode, "unsupported import kind {}", kind);
      }
      import_lines_.push_back(std::format("  (import {} {} {})", quoted(module), quoted(field), desc));
    }
    return {};
  }

  Result<void> functions(Reader& s) {
    CVT_TRY_ASSIGN(const std::uint32_t n, s.count());
    for (std::uint32_t i = 0; i < n; ++i) {
      CVT_TRY_ASSIGN(const std::uint32_t type, s.u32());
      if (type >= types_.size()) return fail(Status::Decode, "function {} uses unknown type {}", i, type);
      func_types_.push_back(type);
      declared_.push_back(type);
    }
    return {};
  }

  Result<void> tables(Reader& s) {
    CVT_TRY_ASSIGN(const std::uint32_t n, s.count());
    for (std::uint32_t i = 0; i < n; ++i) {
      CVT_TRY_ASSIGN(const std::string t, table_type(s));
      table_lines_.push_back(std::format("  (table (;{};) {})", tables_++, t));
    }
    return {};
  }

  Result<void> memories(Reader& s) {
    CVT_TRY_ASSIGN(const std::uint32_t n, s.count());
    for (std::uint32_t i = 0; i < n; ++i) {
      CVT_TRY_ASSIGN(const std::string l, limits(s));
      memory_lines_.push_back(std::format("  (memory (;{};) {})", memories_++, l));
    }
    return {};
  }

  // A constant expression: the instructions up to 'end', folded "(i32.const 0)".
  Result<std::string> const_expr(Reader& s) {
    std::vector<std::string> parts;
    for (int guard = 0; guard < 64; ++guard) {
      CVT_TRY_ASSIGN(const std::uint8_t op, s.u8());
      if (op == 0x0B) {
        std::string out;
        for (const std::string& p : parts) out += (out.empty() ? "" : " ") + ("(" + p + ")");
        return out;
      }
      CVT_TRY_ASSIGN(std::string instr, instruction(s, op, 0, nullptr));
      parts.push_back(std::move(instr));
    }
    return fail(Status::Decode, "constant expression too long");
  }

  Result<void> globals(Reader& s) {
    CVT_TRY_ASSIGN(const std::uint32_t n, s.count());
    for (std::uint32_t i = 0; i < n; ++i) {
      CVT_TRY_ASSIGN(const std::string t, global_type(s));
      CVT_TRY_ASSIGN(const std::string init, const_expr(s));
      global_lines_.push_back(std::format("  (global (;{};) {} {})", globals_++, t, init));
    }
    return {};
  }

  Result<void> exports(Reader& s) {
    CVT_TRY_ASSIGN(const std::uint32_t n, s.count());
    constexpr std::array<std::string_view, 4> kinds = {"func", "table", "memory", "global"};
    for (std::uint32_t i = 0; i < n; ++i) {
      CVT_TRY_ASSIGN(const std::string field, s.name());
      CVT_TRY_ASSIGN(const std::uint8_t kind, s.u8());
      CVT_TRY_ASSIGN(const std::uint32_t index, s.u32());
      if (kind > 3) return fail(Status::Decode, "unsupported export kind {}", kind);
      export_lines_.push_back(std::format("  (export {} ({} {}))", quoted(field), kinds[kind], index));
    }
    return {};
  }

  Result<void> elements(Reader& s) {
    CVT_TRY_ASSIGN(const std::uint32_t n, s.count());
    for (std::uint32_t i = 0; i < n; ++i) {
      CVT_TRY_ASSIGN(const std::uint32_t flags, s.u32());
      if (flags > 7) return fail(Status::Decode, "invalid element segment flags {}", flags);
      std::string line = std::format("  (elem (;{};)", i);
      std::uint32_t table = 0;
      const bool passive_or_declarative = (flags & 1) != 0;
      const bool explicit_table = (flags & 2) != 0 && !passive_or_declarative;
      const bool expressions = (flags & 4) != 0;
      if (explicit_table) {
        CVT_TRY_ASSIGN(table, s.u32());
      }
      if (!passive_or_declarative) {
        CVT_TRY_ASSIGN(const std::string offset, const_expr(s));
        if (explicit_table) line += std::format(" (table {})", table);
        line += " " + offset;
      } else if (flags & 2) {
        line += " declare";
      }
      std::string elemtype = "func";
      if ((flags & 3) != 0) {
        CVT_TRY_ASSIGN(const std::uint8_t t, s.u8());
        if (expressions) {
          if (valtype_name(t).empty()) return fail(Status::Decode, "unknown element type");
          elemtype = std::string(valtype_name(t));
        } else if (t != 0) {
          return fail(Status::Decode, "unknown element kind {}", t);
        }
      } else if (expressions) {
        elemtype = "funcref";
      }
      CVT_TRY_ASSIGN(const std::uint32_t items, s.count());
      line += " " + elemtype;
      for (std::uint32_t k = 0; k < items; ++k) {
        if (expressions) {
          CVT_TRY_ASSIGN(const std::string e, const_expr(s));
          line += " " + e;
        } else {
          CVT_TRY_ASSIGN(const std::uint32_t f, s.u32());
          line += " " + std::to_string(f);
        }
      }
      elem_lines_.push_back(line + ")");
    }
    return {};
  }

  Result<void> datas(Reader& s) {
    CVT_TRY_ASSIGN(const std::uint32_t n, s.count());
    for (std::uint32_t i = 0; i < n; ++i) {
      CVT_TRY_ASSIGN(const std::uint32_t flags, s.u32());
      std::string line = std::format("  (data (;{};)", i);
      if (flags == 0 || flags == 2) {
        std::uint32_t memory = 0;
        if (flags == 2) {
          CVT_TRY_ASSIGN(memory, s.u32());
        }
        CVT_TRY_ASSIGN(const std::string offset, const_expr(s));
        if (flags == 2) line += std::format(" (memory {})", memory);
        line += " " + offset;
      } else if (flags != 1) {
        return fail(Status::Decode, "invalid data segment flags {}", flags);
      }
      CVT_TRY_ASSIGN(const std::uint32_t size, s.u32());
      CVT_TRY_ASSIGN(const ByteView raw, s.bytes(size));
      data_lines_.push_back(line + " " + quoted(as_chars(raw)) + ")");
    }
    return {};
  }

  Result<std::string> block_type(Reader& s) {
    CVT_TRY_ASSIGN(const std::int64_t bt, s.leb_s(33));
    if (bt == -64) return std::string();  // 0x40: empty
    if (bt < 0) {
      const auto t = static_cast<std::uint8_t>(bt & 0x7F);
      if (valtype_name(t).empty()) return fail(Status::Decode, "invalid block type");
      return std::format(" (result {})", valtype_name(t));
    }
    if (static_cast<std::uint64_t>(bt) >= types_.size()) return fail(Status::Decode, "block uses unknown type {}", bt);
    return signature(types_[static_cast<std::size_t>(bt)]);
  }

  Result<std::string> memarg(Reader& s, unsigned natural) {
    CVT_TRY_ASSIGN(const std::uint32_t align, s.u32());
    CVT_TRY_ASSIGN(const std::uint64_t offset, s.leb_u(64));
    if (align >= 32) return fail(Status::Decode, "invalid alignment");
    std::string out;
    if (offset != 0) out += std::format(" offset={}", offset);
    if (align != natural) out += std::format(" align={}", std::uint64_t{1} << align);
    return out;
  }

  std::string label(std::uint32_t relative, int depth) const {
    const int target = depth - static_cast<int>(relative);
    return target >= 1 ? std::format("{} (;@{};)", relative, target) : std::to_string(relative);
  }

  /// One instruction (without the block structure, which `body` handles).
  Result<std::string> instruction(Reader& s, std::uint8_t op, int depth, const std::vector<std::uint8_t>*) {
    static constexpr std::string_view numeric[] = {
        "i32.eqz", "i32.eq", "i32.ne", "i32.lt_s", "i32.lt_u", "i32.gt_s", "i32.gt_u", "i32.le_s", "i32.le_u", "i32.ge_s",
        "i32.ge_u", "i64.eqz", "i64.eq", "i64.ne", "i64.lt_s", "i64.lt_u", "i64.gt_s", "i64.gt_u", "i64.le_s", "i64.le_u",
        "i64.ge_s", "i64.ge_u", "f32.eq", "f32.ne", "f32.lt", "f32.gt", "f32.le", "f32.ge", "f64.eq", "f64.ne", "f64.lt",
        "f64.gt", "f64.le", "f64.ge", "i32.clz", "i32.ctz", "i32.popcnt", "i32.add", "i32.sub", "i32.mul", "i32.div_s",
        "i32.div_u", "i32.rem_s", "i32.rem_u", "i32.and", "i32.or", "i32.xor", "i32.shl", "i32.shr_s", "i32.shr_u",
        "i32.rotl", "i32.rotr", "i64.clz", "i64.ctz", "i64.popcnt", "i64.add", "i64.sub", "i64.mul", "i64.div_s",
        "i64.div_u", "i64.rem_s", "i64.rem_u", "i64.and", "i64.or", "i64.xor", "i64.shl", "i64.shr_s", "i64.shr_u",
        "i64.rotl", "i64.rotr", "f32.abs", "f32.neg", "f32.ceil", "f32.floor", "f32.trunc", "f32.nearest", "f32.sqrt",
        "f32.add", "f32.sub", "f32.mul", "f32.div", "f32.min", "f32.max", "f32.copysign", "f64.abs", "f64.neg", "f64.ceil",
        "f64.floor", "f64.trunc", "f64.nearest", "f64.sqrt", "f64.add", "f64.sub", "f64.mul", "f64.div", "f64.min",
        "f64.max", "f64.copysign", "i32.wrap_i64", "i32.trunc_f32_s", "i32.trunc_f32_u", "i32.trunc_f64_s",
        "i32.trunc_f64_u", "i64.extend_i32_s", "i64.extend_i32_u", "i64.trunc_f32_s", "i64.trunc_f32_u",
        "i64.trunc_f64_s", "i64.trunc_f64_u", "f32.convert_i32_s", "f32.convert_i32_u", "f32.convert_i64_s",
        "f32.convert_i64_u", "f32.demote_f64", "f64.convert_i32_s", "f64.convert_i32_u", "f64.convert_i64_s",
        "f64.convert_i64_u", "f64.promote_f32", "i32.reinterpret_f32", "i64.reinterpret_f64", "f32.reinterpret_i32",
        "f64.reinterpret_i64", "i32.extend8_s", "i32.extend16_s", "i64.extend8_s", "i64.extend16_s", "i64.extend32_s"};
    static constexpr std::string_view memory_ops[] = {
        "i32.load", "i64.load", "f32.load", "f64.load", "i32.load8_s", "i32.load8_u", "i32.load16_s", "i32.load16_u",
        "i64.load8_s", "i64.load8_u", "i64.load16_s", "i64.load16_u", "i64.load32_s", "i64.load32_u", "i32.store",
        "i64.store", "f32.store", "f64.store", "i32.store8", "i32.store16", "i64.store8", "i64.store16", "i64.store32"};
    static constexpr unsigned natural_align[] = {2, 3, 2, 3, 0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 2, 3, 2, 3, 0, 1, 0, 1, 2};
    static_assert(std::size(numeric) == 0xC4 - 0x45 + 1 && std::size(memory_ops) == 23 && std::size(natural_align) == 23);
    if (op >= 0x45 && op <= 0xC4) return std::string(numeric[op - 0x45]);
    if (op >= 0x28 && op <= 0x3E) {
      CVT_TRY_ASSIGN(const std::string arg, memarg(s, natural_align[op - 0x28]));
      return std::string(memory_ops[op - 0x28]) + arg;
    }
    switch (op) {
      case 0x00: return std::string("unreachable");
      case 0x01: return std::string("nop");
      case 0x0C: case 0x0D: {
        CVT_TRY_ASSIGN(const std::uint32_t l, s.u32());
        return std::string(op == 0x0C ? "br " : "br_if ") + label(l, depth);
      }
      case 0x0E: {
        CVT_TRY_ASSIGN(const std::uint32_t n, s.count());
        std::string out = "br_table";
        for (std::uint32_t k = 0; k <= n; ++k) {
          CVT_TRY_ASSIGN(const std::uint32_t l, s.u32());
          out += " " + label(l, depth);
        }
        return out;
      }
      case 0x0F: return std::string("return");
      case 0x10: {
        CVT_TRY_ASSIGN(const std::uint32_t f, s.u32());
        return std::format("call {}", f);
      }
      case 0x11: {
        CVT_TRY_ASSIGN(const std::uint32_t type, s.u32());
        CVT_TRY_ASSIGN(const std::uint32_t table, s.u32());
        return table == 0 ? std::format("call_indirect (type {})", type) : std::format("call_indirect {} (type {})", table, type);
      }
      case 0x1A: return std::string("drop");
      case 0x1B: return std::string("select");
      case 0x1C: {
        CVT_TRY_ASSIGN(const std::uint32_t n, s.count());
        std::string out = "select (result";
        for (std::uint32_t k = 0; k < n; ++k) {
          CVT_TRY_ASSIGN(const std::uint8_t t, valtype(s));
          out += " " + std::string(valtype_name(t));
        }
        return out + ")";
      }
      case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: {
        constexpr std::array<std::string_view, 7> names = {"local.get", "local.set", "local.tee", "global.get",
                                                           "global.set", "table.get", "table.set"};
        CVT_TRY_ASSIGN(const std::uint32_t idx, s.u32());
        return std::format("{} {}", names[op - 0x20], idx);
      }
      case 0x3F: case 0x40: {
        CVT_TRY_ASSIGN(const std::uint8_t mem, s.u8());
        if (mem != 0) return fail(Status::Decode, "multiple memories are not supported");
        return std::string(op == 0x3F ? "memory.size" : "memory.grow");
      }
      case 0x41: {
        CVT_TRY_ASSIGN(const std::int64_t v, s.leb_s(32));
        return std::format("i32.const {}", static_cast<std::int32_t>(v));
      }
      case 0x42: {
        CVT_TRY_ASSIGN(const std::int64_t v, s.leb_s(64));
        return std::format("i64.const {}", v);
      }
      case 0x43: {
        CVT_TRY_ASSIGN(const ByteView raw, s.bytes(4));
        std::uint32_t bits = 0;
        for (int k = 3; k >= 0; --k) bits = (bits << 8) | std::to_integer<std::uint32_t>(raw[static_cast<std::size_t>(k)]);
        return "f32.const " + float_text(std::bit_cast<float>(bits), true);
      }
      case 0x44: {
        CVT_TRY_ASSIGN(const ByteView raw, s.bytes(8));
        std::uint64_t bits = 0;
        for (int k = 7; k >= 0; --k) bits = (bits << 8) | std::to_integer<std::uint64_t>(raw[static_cast<std::size_t>(k)]);
        return "f64.const " + float_text(std::bit_cast<double>(bits), false);
      }
      case 0xD0: {
        CVT_TRY_ASSIGN(const std::uint8_t t, s.u8());
        if (t != 0x70 && t != 0x6F) return fail(Status::Decode, "invalid ref.null type");
        return std::string(t == 0x70 ? "ref.null func" : "ref.null extern");
      }
      case 0xD1: return std::string("ref.is_null");
      case 0xD2: {
        CVT_TRY_ASSIGN(const std::uint32_t f, s.u32());
        return std::format("ref.func {}", f);
      }
      case 0xFC: {
        CVT_TRY_ASSIGN(const std::uint32_t sub, s.u32());
        constexpr std::array<std::string_view, 8> sat = {"i32.trunc_sat_f32_s", "i32.trunc_sat_f32_u", "i32.trunc_sat_f64_s",
                                                         "i32.trunc_sat_f64_u", "i64.trunc_sat_f32_s", "i64.trunc_sat_f32_u",
                                                         "i64.trunc_sat_f64_s", "i64.trunc_sat_f64_u"};
        if (sub < 8) return std::string(sat[sub]);
        switch (sub) {
          case 8: {
            CVT_TRY_ASSIGN(const std::uint32_t data, s.u32());
            CVT_TRY_ASSIGN(const std::uint8_t mem, s.u8());
            (void)mem;
            return std::format("memory.init {}", data);
          }
          case 9: {
            CVT_TRY_ASSIGN(const std::uint32_t data, s.u32());
            return std::format("data.drop {}", data);
          }
          case 10: {
            CVT_TRY(s.u8());
            CVT_TRY(s.u8());
            return std::string("memory.copy");
          }
          case 11: {
            CVT_TRY(s.u8());
            return std::string("memory.fill");
          }
          case 12: {
            CVT_TRY_ASSIGN(const std::uint32_t elem, s.u32());
            CVT_TRY_ASSIGN(const std::uint32_t table, s.u32());
            return std::format("table.init {} {}", table, elem);
          }
          case 13: {
            CVT_TRY_ASSIGN(const std::uint32_t elem, s.u32());
            return std::format("elem.drop {}", elem);
          }
          case 14: {
            CVT_TRY_ASSIGN(const std::uint32_t dst, s.u32());
            CVT_TRY_ASSIGN(const std::uint32_t src, s.u32());
            return std::format("table.copy {} {}", dst, src);
          }
          case 15: case 16: case 17: {
            CVT_TRY_ASSIGN(const std::uint32_t table, s.u32());
            constexpr std::array<std::string_view, 3> names = {"table.grow", "table.size", "table.fill"};
            return std::format("{} {}", names[sub - 15], table);
          }
          default: return fail(Status::Decode, "unknown 0xFC instruction {}", sub);
        }
      }
      case 0xFD: return fail(Status::Decode, "SIMD instructions are not supported");
      default: return fail(Status::Decode, "unknown or unsupported opcode 0x{:02X} at offset {}", op, s.position());
    }
  }

  Result<void> code(Reader& s) {
    CVT_TRY_ASSIGN(const std::uint32_t n, s.count());
    if (n != declared_.size()) return fail(Status::Decode, "the code section has {} bodies for {} functions", n, declared_.size());
    for (std::uint32_t i = 0; i < n; ++i) {
      if ((i & 63) == 0) CVT_TRY(ctx_.checkpoint(static_cast<double>(i) / n, "Disassembling"));
      CVT_TRY_ASSIGN(const std::uint32_t size, s.u32());
      CVT_TRY_ASSIGN(const ByteView raw, s.bytes(size));
      Reader b(raw);
      const std::uint32_t index = static_cast<std::uint32_t>(imported_funcs_ + i);
      const FuncType& type = types_[declared_[i]];
      std::string out = std::format("  (func (;{};) (type {}){}", index, declared_[i], signature(type));
      CVT_TRY_ASSIGN(const std::uint32_t groups, b.count());
      std::string locals;
      std::uint64_t total = 0;
      for (std::uint32_t g = 0; g < groups; ++g) {
        CVT_TRY_ASSIGN(const std::uint32_t count, b.u32());
        CVT_TRY_ASSIGN(const std::uint8_t t, valtype(b));
        total += count;
        if (total > 50000) return fail(Status::Decode, "function {} declares too many locals", index);
        for (std::uint32_t k = 0; k < count; ++k) locals += " " + std::string(valtype_name(t));
      }
      if (!locals.empty()) out += "\n    (local" + locals + ")";
      int depth = 0;
      bool ended = false;
      while (!b.at_end()) {
        CVT_TRY_ASSIGN(const std::uint8_t op, b.u8());
        const std::string indent(static_cast<std::size_t>(4 + 2 * std::max(0, depth - (op == 0x0B || op == 0x05 ? 1 : 0))), ' ');
        if (op == 0x02 || op == 0x03 || op == 0x04) {
          CVT_TRY_ASSIGN(const std::string bt, block_type(b));
          if (++depth > max_depth) return fail(Status::Decode, "blocks nested too deeply");
          constexpr std::array<std::string_view, 3> names = {"block", "loop", "if"};
          out += std::format("\n{}{}{}  ;; label = @{}", indent, names[op - 0x02], bt, depth);
        } else if (op == 0x05) {
          out += "\n" + indent + "else";
        } else if (op == 0x0B) {
          if (depth == 0) {
            ended = true;
            if (!b.at_end()) return fail(Status::Decode, "function {} has bytes after its final 'end'", index);
            break;
          }
          --depth;
          out += "\n" + indent + "end";
        } else {
          CVT_TRY_ASSIGN(const std::string instr, instruction(b, op, depth, nullptr));
          out += "\n" + indent + instr;
        }
      }
      if (!ended) return fail(Status::Decode, "function {} has no final 'end'", index);
      bodies_.push_back(out + ")");
    }
    return {};
  }

  Reader r_;
  const Context& ctx_;
  std::vector<FuncType> types_;
  std::vector<std::uint32_t> func_types_, declared_;
  std::size_t imported_funcs_ = 0, tables_ = 0, memories_ = 0, globals_ = 0;
  std::vector<std::string> type_lines_, import_lines_, bodies_, table_lines_, memory_lines_, global_lines_, export_lines_,
      elem_lines_, data_lines_;
  std::string start_;
};

}  // namespace

Result<std::string> disassemble_wasm(ByteView module, const Context& ctx) { return Disassembler(module, ctx).run(); }

void register_wasm(Registry& registry) {
  FormatInfo wasm;
  wasm.id = "wasm";
  wasm.name = "WebAssembly module";
  wasm.description = "A compiled WebAssembly binary; converts to its text format (WAT) and to documents.";
  wasm.category = Category::Code;
  wasm.extensions = {"wasm"};
  wasm.mime = "application/wasm";
  wasm.popularity = 40;
  wasm.signatures = {Signature::at(0, "00 'asm' 01 00 00 00")};
  registry.add_format(std::move(wasm));

  Codec text;
  text.format_id = "wasm";
  text.kind = Kind::PlainText;
  text.read_cost = 20;
  text.read = [](ReadContext& ctx) -> Result<AnyIR> {
    CVT_TRY_ASSIGN(std::string wat, disassemble_wasm(ctx.input, ctx));
    CVT_TRY(ctx.limits.check_decompressed(wat.size()));
    ir::PlainText out;
    out.text = std::move(wat);
    return out;
  };
  registry.add_codec(std::move(text));
  Codec document;
  document.format_id = "wasm";
  document.kind = Kind::Document;
  document.read_cost = 20;
  document.read = [](ReadContext& ctx) -> Result<AnyIR> {
    CVT_TRY_ASSIGN(std::string wat, disassemble_wasm(ctx.input, ctx));
    if (wat.ends_with('\n')) wat.pop_back();
    ir::Document doc;
    doc.metadata.title = ctx.filename;
    doc.blocks.push_back(ir::CodeBlock{"wat", std::move(wat)});
    return doc;
  };
  registry.add_codec(std::move(document));

  // The text format is a source language (languages.cpp); writing it is identity on text.
  Codec wat;
  wat.format_id = "wat";
  wat.kind = Kind::PlainText;
  wat.write = [](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
    const std::string& t = value.get<ir::PlainText>().text;
    CVT_TRY(ctx.limits.check_output(t.size()));
    return to_bytes(t);
  };
  registry.add_codec(std::move(wat));
}

}  // namespace cvt::code
