// Convertible core - pdf family: functions and colour spaces.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/pdf/color.hpp"

#include <algorithm>
#include <cmath>

#include "families/pdf/syntax.hpp"

namespace cvt::pdf {

namespace {

double clamp01(double v) { return std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 0.0; }

std::vector<double> numbers(PdfFile& file, const Object& o) {
  std::vector<double> out;
  for (const Object& item : file.resolve(o).as_array()) {
    out.push_back(file.resolve(item).as_number());
    if (out.size() > 4096) break;
  }
  return out;
}

// ------------------------------------------------------------------- functions

class SampledFunction final : public Function {
 public:
  std::vector<double> domain, range, encode, decode;
  std::vector<int> size;
  std::vector<double> samples;  // normalised 0..1 then decoded on output

  void eval(const double* in, double* out) const override {
    const int m = inputs_, n = outputs_;
    // Multilinear interpolation over up to 2^m corners (m capped at 8 when loading).
    std::vector<double> e(static_cast<std::size_t>(m));
    std::vector<int> i0(static_cast<std::size_t>(m)), i1(static_cast<std::size_t>(m));
    std::vector<double> frac(static_cast<std::size_t>(m));
    for (int k = 0; k < m; ++k) {
      const double d0 = domain[2 * static_cast<std::size_t>(k)], d1 = domain[2 * static_cast<std::size_t>(k) + 1];
      double x = std::clamp(std::isfinite(in[k]) ? in[k] : d0, std::min(d0, d1), std::max(d0, d1));
      const double e0 = encode[2 * static_cast<std::size_t>(k)], e1 = encode[2 * static_cast<std::size_t>(k) + 1];
      double v = d1 != d0 ? e0 + (x - d0) * (e1 - e0) / (d1 - d0) : e0;
      v = std::clamp(v, 0.0, static_cast<double>(size[static_cast<std::size_t>(k)] - 1));
      i0[static_cast<std::size_t>(k)] = static_cast<int>(std::floor(v));
      i1[static_cast<std::size_t>(k)] = std::min(i0[static_cast<std::size_t>(k)] + 1, size[static_cast<std::size_t>(k)] - 1);
      frac[static_cast<std::size_t>(k)] = v - i0[static_cast<std::size_t>(k)];
    }
    for (int j = 0; j < n; ++j) out[j] = 0;
    const int corners = 1 << m;
    for (int c = 0; c < corners; ++c) {
      double weight = 1;
      std::size_t index = 0, stride = 1;
      for (int k = 0; k < m; ++k) {
        const bool hi = (c >> k) & 1;
        weight *= hi ? frac[static_cast<std::size_t>(k)] : 1 - frac[static_cast<std::size_t>(k)];
        index += static_cast<std::size_t>(hi ? i1[static_cast<std::size_t>(k)] : i0[static_cast<std::size_t>(k)]) * stride;
        stride *= static_cast<std::size_t>(size[static_cast<std::size_t>(k)]);
      }
      if (weight == 0) continue;
      for (int j = 0; j < n; ++j) {
        const std::size_t at = index * static_cast<std::size_t>(n) + static_cast<std::size_t>(j);
        out[j] += weight * (at < samples.size() ? samples[at] : 0);
      }
    }
    for (int j = 0; j < n; ++j) {
      const double d0 = decode[2 * static_cast<std::size_t>(j)], d1 = decode[2 * static_cast<std::size_t>(j) + 1];
      double v = d0 + out[j] * (d1 - d0);
      if (!range.empty()) v = std::clamp(v, std::min(range[2 * static_cast<std::size_t>(j)], range[2 * static_cast<std::size_t>(j) + 1]),
                                         std::max(range[2 * static_cast<std::size_t>(j)], range[2 * static_cast<std::size_t>(j) + 1]));
      out[j] = v;
    }
  }
  void set_io(int in, int out) {
    inputs_ = in;
    outputs_ = out;
  }
};

class ExponentialFunction final : public Function {
 public:
  std::vector<double> c0, c1, domain, range;
  double exponent = 1;
  void eval(const double* in, double* out) const override {
    double x = std::isfinite(in[0]) ? in[0] : 0;
    if (domain.size() >= 2) x = std::clamp(x, domain[0], domain[1]);
    const double t = (x < 0 && exponent != std::floor(exponent)) ? 0 : std::pow(x, exponent);
    for (int j = 0; j < outputs_; ++j) {
      const auto k = static_cast<std::size_t>(j);
      double v = c0[k] + t * (c1[k] - c0[k]);
      if (range.size() >= 2 * k + 2) v = std::clamp(v, range[2 * k], range[2 * k + 1]);
      out[j] = std::isfinite(v) ? v : 0;
    }
  }
  void set_io(int in, int out) {
    inputs_ = in;
    outputs_ = out;
  }
};

class StitchingFunction final : public Function {
 public:
  std::vector<std::shared_ptr<const Function>> functions;
  std::vector<double> bounds, encode, domain;
  void eval(const double* in, double* out) const override {
    double x = std::isfinite(in[0]) ? in[0] : 0;
    x = std::clamp(x, domain[0], domain[1]);
    std::size_t k = 0;
    while (k < bounds.size() && x >= bounds[k]) ++k;
    if (k >= functions.size()) k = functions.size() - 1;
    const double lo = k == 0 ? domain[0] : bounds[k - 1];
    const double hi = k < bounds.size() ? bounds[k] : domain[1];
    const double e0 = encode[2 * k], e1 = encode[2 * k + 1];
    const double t = hi != lo ? e0 + (x - lo) * (e1 - e0) / (hi - lo) : e0;
    std::vector<double> tmp(static_cast<std::size_t>(std::max(outputs_, functions[k]->outputs())), 0.0);
    functions[k]->eval(&t, tmp.data());
    for (int j = 0; j < outputs_; ++j) out[j] = tmp[static_cast<std::size_t>(j)];
  }
  void set_io(int in, int out) {
    inputs_ = in;
    outputs_ = out;
  }
};

// PostScript calculator (type 4).
enum class PsOp : std::uint8_t {
  Push, Abs, Add, Atan, Ceiling, Cos, Cvi, Cvr, Div, Exp, Floor, Idiv, Ln, Log, Mod, Mul, Neg, Round, Sin, Sqrt,
  Sub, Truncate, And, Bitshift, Eq, False, Ge, Gt, Le, Lt, Ne, Not, Or, True, Xor, Copy, Dup, Exch, Index, Pop,
  Roll, If, IfElse, Jump
};

struct PsInstr {
  PsOp op = PsOp::Push;
  double value = 0;
  int target = 0;  // for If/IfElse/Jump
};

class PostScriptFunction final : public Function {
 public:
  std::vector<PsInstr> code;
  std::vector<double> domain, range;
  void eval(const double* in, double* out) const override {
    std::vector<double> st;
    st.reserve(64);
    for (int k = 0; k < inputs_; ++k) {
      double v = std::isfinite(in[k]) ? in[k] : 0;
      if (domain.size() >= 2 * static_cast<std::size_t>(k) + 2) v = std::clamp(v, domain[2 * static_cast<std::size_t>(k)], domain[2 * static_cast<std::size_t>(k) + 1]);
      st.push_back(v);
    }
    const auto pop = [&]() {
      if (st.empty()) return 0.0;
      const double v = st.back();
      st.pop_back();
      return v;
    };
    std::size_t steps = 0;
    for (std::size_t pc = 0; pc < code.size();) {
      if (++steps > 100000 || st.size() > 1000) break;
      const PsInstr& ins = code[pc++];
      switch (ins.op) {
        case PsOp::Push: st.push_back(ins.value); break;
        case PsOp::Abs: st.push_back(std::abs(pop())); break;
        case PsOp::Add: { const double b = pop(), a = pop(); st.push_back(a + b); break; }
        case PsOp::Sub: { const double b = pop(), a = pop(); st.push_back(a - b); break; }
        case PsOp::Mul: { const double b = pop(), a = pop(); st.push_back(a * b); break; }
        case PsOp::Div: { const double b = pop(), a = pop(); st.push_back(b != 0 ? a / b : 0); break; }
        case PsOp::Idiv: {
          const auto b = static_cast<long long>(pop()), a = static_cast<long long>(pop());
          st.push_back(b != 0 ? static_cast<double>(a / b) : 0);
          break;
        }
        case PsOp::Mod: {
          const auto b = static_cast<long long>(pop()), a = static_cast<long long>(pop());
          st.push_back(b != 0 ? static_cast<double>(a % b) : 0);
          break;
        }
        case PsOp::Atan: {
          const double b = pop(), a = pop();
          double deg = std::atan2(a, b) * 180.0 / 3.14159265358979323846;
          if (deg < 0) deg += 360;
          st.push_back(deg);
          break;
        }
        case PsOp::Ceiling: st.push_back(std::ceil(pop())); break;
        case PsOp::Floor: st.push_back(std::floor(pop())); break;
        case PsOp::Round: st.push_back(std::floor(pop() + 0.5)); break;
        case PsOp::Truncate: st.push_back(std::trunc(pop())); break;
        case PsOp::Cvi: st.push_back(std::trunc(pop())); break;
        case PsOp::Cvr: break;
        case PsOp::Cos: st.push_back(std::cos(pop() * 3.14159265358979323846 / 180.0)); break;
        case PsOp::Sin: st.push_back(std::sin(pop() * 3.14159265358979323846 / 180.0)); break;
        case PsOp::Exp: { const double e = pop(), b = pop(); const double v = std::pow(b, e); st.push_back(std::isfinite(v) ? v : 0); break; }
        case PsOp::Ln: { const double a = pop(); st.push_back(a > 0 ? std::log(a) : 0); break; }
        case PsOp::Log: { const double a = pop(); st.push_back(a > 0 ? std::log10(a) : 0); break; }
        case PsOp::Neg: st.push_back(-pop()); break;
        case PsOp::Sqrt: st.push_back(std::sqrt(std::max(0.0, pop()))); break;
        case PsOp::And: { const auto b = static_cast<long long>(pop()), a = static_cast<long long>(pop()); st.push_back(static_cast<double>(a & b)); break; }
        case PsOp::Or: { const auto b = static_cast<long long>(pop()), a = static_cast<long long>(pop()); st.push_back(static_cast<double>(a | b)); break; }
        case PsOp::Xor: { const auto b = static_cast<long long>(pop()), a = static_cast<long long>(pop()); st.push_back(static_cast<double>(a ^ b)); break; }
        case PsOp::Not: { const double a = pop(); st.push_back(a == 0 ? 1 : (a == 1 ? 0 : static_cast<double>(~static_cast<long long>(a)))); break; }
        case PsOp::Bitshift: {
          const auto s = static_cast<long long>(pop()), a = static_cast<long long>(pop());
          st.push_back(s >= 0 ? static_cast<double>(s < 63 ? a << s : 0) : static_cast<double>(-s < 63 ? a >> -s : 0));
          break;
        }
        case PsOp::Eq: { const double b = pop(), a = pop(); st.push_back(a == b); break; }
        case PsOp::Ne: { const double b = pop(), a = pop(); st.push_back(a != b); break; }
        case PsOp::Ge: { const double b = pop(), a = pop(); st.push_back(a >= b); break; }
        case PsOp::Gt: { const double b = pop(), a = pop(); st.push_back(a > b); break; }
        case PsOp::Le: { const double b = pop(), a = pop(); st.push_back(a <= b); break; }
        case PsOp::Lt: { const double b = pop(), a = pop(); st.push_back(a < b); break; }
        case PsOp::True: st.push_back(1); break;
        case PsOp::False: st.push_back(0); break;
        case PsOp::Dup: { const double a = st.empty() ? 0 : st.back(); st.push_back(a); break; }
        case PsOp::Pop: pop(); break;
        case PsOp::Exch: { const double b = pop(), a = pop(); st.push_back(b); st.push_back(a); break; }
        case PsOp::Copy: {
          const auto n = static_cast<long long>(pop());
          if (n > 0 && n <= static_cast<long long>(st.size())) {
            const std::size_t start = st.size() - static_cast<std::size_t>(n);
            for (std::size_t i = 0; i < static_cast<std::size_t>(n); ++i) st.push_back(st[start + i]);
          }
          break;
        }
        case PsOp::Index: {
          const auto n = static_cast<long long>(pop());
          st.push_back(n >= 0 && n < static_cast<long long>(st.size()) ? st[st.size() - 1 - static_cast<std::size_t>(n)] : 0);
          break;
        }
        case PsOp::Roll: {
          const auto j = static_cast<long long>(pop());
          const auto n = static_cast<long long>(pop());
          if (n > 0 && n <= static_cast<long long>(st.size())) {
            const long long shift = ((j % n) + n) % n;
            std::rotate(st.end() - n, st.end() - shift, st.end());
          }
          break;
        }
        case PsOp::If:
          if (pop() == 0) pc = static_cast<std::size_t>(ins.target);
          break;
        case PsOp::IfElse:
          if (pop() == 0) pc = static_cast<std::size_t>(ins.target);
          break;
        case PsOp::Jump: pc = static_cast<std::size_t>(ins.target); break;
      }
    }
    // Outputs are the top `outputs_` values.
    for (int j = outputs_ - 1; j >= 0; --j) {
      double v = pop();
      const auto k = static_cast<std::size_t>(j);
      if (range.size() >= 2 * k + 2) v = std::clamp(v, range[2 * k], range[2 * k + 1]);
      out[j] = std::isfinite(v) ? v : 0;
    }
  }
  void set_io(int in, int out) {
    inputs_ = in;
    outputs_ = out;
  }
};

bool compile_ps(Lexer& lexer, std::vector<PsInstr>& code, int depth) {
  if (depth > 32 || code.size() > 100000) return false;
  static constexpr std::pair<std::string_view, PsOp> kOps[] = {
      {"abs", PsOp::Abs}, {"add", PsOp::Add}, {"atan", PsOp::Atan}, {"ceiling", PsOp::Ceiling}, {"cos", PsOp::Cos},
      {"cvi", PsOp::Cvi}, {"cvr", PsOp::Cvr}, {"div", PsOp::Div}, {"exp", PsOp::Exp}, {"floor", PsOp::Floor},
      {"idiv", PsOp::Idiv}, {"ln", PsOp::Ln}, {"log", PsOp::Log}, {"mod", PsOp::Mod}, {"mul", PsOp::Mul},
      {"neg", PsOp::Neg}, {"round", PsOp::Round}, {"sin", PsOp::Sin}, {"sqrt", PsOp::Sqrt}, {"sub", PsOp::Sub},
      {"truncate", PsOp::Truncate}, {"and", PsOp::And}, {"bitshift", PsOp::Bitshift}, {"eq", PsOp::Eq},
      {"false", PsOp::False}, {"ge", PsOp::Ge}, {"gt", PsOp::Gt}, {"le", PsOp::Le}, {"lt", PsOp::Lt},
      {"ne", PsOp::Ne}, {"not", PsOp::Not}, {"or", PsOp::Or}, {"true", PsOp::True}, {"xor", PsOp::Xor},
      {"copy", PsOp::Copy}, {"dup", PsOp::Dup}, {"exch", PsOp::Exch}, {"index", PsOp::Index}, {"pop", PsOp::Pop},
      {"roll", PsOp::Roll}};
  // Pending procedure blocks: we compile `{a} if` and `{a} {b} ifelse`.
  std::vector<std::vector<PsInstr>> blocks;
  while (true) {
    Token t = lexer.next();
    if (t.kind == Token::Kind::End) return depth == 0;
    if (t.kind == Token::Kind::BraceClose) return true;
    if (t.kind == Token::Kind::BraceOpen) {
      std::vector<PsInstr> block;
      if (!compile_ps(lexer, block, depth + 1)) return false;
      blocks.push_back(std::move(block));
      continue;
    }
    if (t.kind == Token::Kind::Integer || t.kind == Token::Kind::Real) {
      code.push_back(PsInstr{PsOp::Push, t.real, 0});
      continue;
    }
    if (t.kind != Token::Kind::Keyword) continue;
    if (t.text == "if" && !blocks.empty()) {
      std::vector<PsInstr> body = std::move(blocks.back());
      blocks.pop_back();
      const std::size_t at = code.size();
      code.push_back(PsInstr{PsOp::If, 0, 0});
      const std::size_t base = code.size();
      for (PsInstr ins : body) {
        if (ins.op == PsOp::If || ins.op == PsOp::IfElse || ins.op == PsOp::Jump) ins.target += static_cast<int>(base);
        code.push_back(ins);
      }
      code[at].target = static_cast<int>(code.size());
      continue;
    }
    if (t.text == "ifelse" && blocks.size() >= 2) {
      std::vector<PsInstr> no = std::move(blocks.back());
      blocks.pop_back();
      std::vector<PsInstr> yes = std::move(blocks.back());
      blocks.pop_back();
      const std::size_t at = code.size();
      code.push_back(PsInstr{PsOp::IfElse, 0, 0});
      std::size_t base = code.size();
      for (PsInstr ins : yes) {
        if (ins.op == PsOp::If || ins.op == PsOp::IfElse || ins.op == PsOp::Jump) ins.target += static_cast<int>(base);
        code.push_back(ins);
      }
      const std::size_t jump = code.size();
      code.push_back(PsInstr{PsOp::Jump, 0, 0});
      code[at].target = static_cast<int>(code.size());
      base = code.size();
      for (PsInstr ins : no) {
        if (ins.op == PsOp::If || ins.op == PsOp::IfElse || ins.op == PsOp::Jump) ins.target += static_cast<int>(base);
        code.push_back(ins);
      }
      code[jump].target = static_cast<int>(code.size());
      continue;
    }
    for (const auto& [name, op] : kOps) {
      if (t.text == name) {
        code.push_back(PsInstr{op, 0, 0});
        break;
      }
    }
  }
}

class ArrayFunction final : public Function {
 public:
  std::vector<std::shared_ptr<const Function>> parts;
  void eval(const double* in, double* out) const override {
    for (std::size_t i = 0; i < parts.size(); ++i) {
      double tmp[32] = {};
      parts[i]->eval(in, tmp);
      out[i] = tmp[0];
    }
  }
  void set_io(int in, int out) {
    inputs_ = in;
    outputs_ = out;
  }
};

}  // namespace

std::shared_ptr<const Function> load_function(PdfFile& file, const Object& object, int depth) {
  if (depth > 8) return nullptr;
  const Object obj = file.resolve(object);
  if (obj.is_array()) {
    auto f = std::make_shared<ArrayFunction>();
    for (const Object& item : obj.as_array()) {
      auto part = load_function(file, item, depth + 1);
      if (!part || part->outputs() < 1) return nullptr;
      f->parts.push_back(std::move(part));
      if (f->parts.size() > 32) return nullptr;
    }
    if (f->parts.empty()) return nullptr;
    f->set_io(f->parts[0]->inputs(), static_cast<int>(f->parts.size()));
    return f;
  }
  if (!obj.is_dict()) return nullptr;
  const Dict& d = obj.as_dict();
  const int type = static_cast<int>(file.get(d, "FunctionType").as_int(-1));
  std::vector<double> domain = numbers(file, d.get("Domain"));
  std::vector<double> range = numbers(file, d.get("Range"));
  if (domain.size() < 2) domain = {0, 1};
  const int m = static_cast<int>(domain.size() / 2);
  if (type == 0) {
    if (!obj.is_stream() || m > 8 || range.size() < 2) return nullptr;
    auto f = std::make_shared<SampledFunction>();
    const int n = static_cast<int>(range.size() / 2);
    if (n > 32) return nullptr;
    f->set_io(m, n);
    f->domain = domain;
    f->range = range;
    std::uint64_t total = static_cast<std::uint64_t>(n);
    for (const double s : numbers(file, d.get("Size"))) {
      const int si = static_cast<int>(std::clamp(s, 1.0, 1e7));
      f->size.push_back(si);
      total *= static_cast<std::uint64_t>(si);
      if (total > 50'000'000) return nullptr;
    }
    if (static_cast<int>(f->size.size()) != m) return nullptr;
    f->encode = numbers(file, d.get("Encode"));
    if (f->encode.size() < 2 * static_cast<std::size_t>(m)) {
      f->encode.clear();
      for (int k = 0; k < m; ++k) {
        f->encode.push_back(0);
        f->encode.push_back(f->size[static_cast<std::size_t>(k)] - 1);
      }
    }
    f->decode = numbers(file, d.get("Decode"));
    if (f->decode.size() < 2 * static_cast<std::size_t>(n)) f->decode = range;
    const int bps = static_cast<int>(file.get(d, "BitsPerSample").as_int(8));
    if (bps != 1 && bps != 2 && bps != 4 && bps != 8 && bps != 12 && bps != 16 && bps != 24 && bps != 32) return nullptr;
    const Bytes data = file.decode_or_empty(obj);
    f->samples.reserve(static_cast<std::size_t>(total));
    std::uint64_t bit = 0;
    const double max_value = std::pow(2.0, bps) - 1;
    for (std::uint64_t i = 0; i < total; ++i) {
      std::uint64_t v = 0;
      for (int b = 0; b < bps; ++b, ++bit) {
        const std::size_t byte = static_cast<std::size_t>(bit / 8);
        const int value = byte < data.size() ? std::to_integer<int>(data[byte]) : 0;
        v = (v << 1) | static_cast<std::uint64_t>((value >> (7 - bit % 8)) & 1);
      }
      f->samples.push_back(static_cast<double>(v) / max_value);
    }
    return f;
  }
  if (type == 2) {
    auto f = std::make_shared<ExponentialFunction>();
    f->c0 = numbers(file, d.get("C0"));
    f->c1 = numbers(file, d.get("C1"));
    if (f->c0.empty()) f->c0 = {0};
    if (f->c1.empty()) f->c1 = {1};
    if (f->c0.size() != f->c1.size() || f->c0.size() > 32) return nullptr;
    f->exponent = file.get(d, "N").as_number(1);
    f->domain = domain;
    f->range = range;
    f->set_io(1, static_cast<int>(f->c0.size()));
    return f;
  }
  if (type == 3) {
    auto f = std::make_shared<StitchingFunction>();
    for (const Object& item : file.get(d, "Functions").as_array()) {
      auto part = load_function(file, item, depth + 1);
      if (!part) return nullptr;
      f->functions.push_back(std::move(part));
      if (f->functions.size() > 256) return nullptr;
    }
    if (f->functions.empty()) return nullptr;
    f->bounds = numbers(file, d.get("Bounds"));
    f->encode = numbers(file, d.get("Encode"));
    f->domain = domain;
    if (f->bounds.size() + 1 != f->functions.size()) return nullptr;
    if (f->encode.size() < 2 * f->functions.size()) {
      f->encode.clear();
      for (std::size_t i = 0; i < f->functions.size(); ++i) {
        f->encode.push_back(0);
        f->encode.push_back(1);
      }
    }
    int outputs = f->functions[0]->outputs();
    for (const auto& part : f->functions) outputs = std::max(outputs, part->outputs());
    f->set_io(1, outputs);
    return f;
  }
  if (type == 4) {
    if (!obj.is_stream() || range.size() < 2) return nullptr;
    auto f = std::make_shared<PostScriptFunction>();
    const Bytes program = file.decode_or_empty(obj);
    Lexer lexer(program);
    const Token open = lexer.next();
    if (open.kind != Token::Kind::BraceOpen) return nullptr;
    if (!compile_ps(lexer, f->code, 1)) return nullptr;
    f->domain = domain;
    f->range = range;
    f->set_io(m, static_cast<int>(range.size() / 2));
    if (f->outputs() > 32 || f->inputs() > 32) return nullptr;
    return f;
  }
  return nullptr;
}

// ------------------------------------------------------------------- colour spaces

std::vector<double> ColorSpace::initial_color() const { return std::vector<double>(static_cast<std::size_t>(n_), 0.0); }

std::array<double, 2> ColorSpace::decode_range(int, int) const { return {0.0, 1.0}; }

namespace {

class GraySpace final : public ColorSpace {
 public:
  GraySpace() {
    kind_ = Kind::Gray;
    n_ = 1;
  }
  Rgb to_rgb(const double* c) const override {
    const double v = clamp01(c[0]);
    return {v, v, v};
  }
};

class RgbSpace final : public ColorSpace {
 public:
  RgbSpace() {
    kind_ = Kind::Rgb;
    n_ = 3;
  }
  Rgb to_rgb(const double* c) const override { return {clamp01(c[0]), clamp01(c[1]), clamp01(c[2])}; }
};

class CmykSpace final : public ColorSpace {
 public:
  CmykSpace() {
    kind_ = Kind::Cmyk;
    n_ = 4;
  }
  std::vector<double> initial_color() const override { return {0, 0, 0, 1}; }
  Rgb to_rgb(const double* c) const override {
    const double k = clamp01(c[3]);
    return {1 - std::min(1.0, clamp01(c[0]) + k), 1 - std::min(1.0, clamp01(c[1]) + k),
            1 - std::min(1.0, clamp01(c[2]) + k)};
  }
};

double srgb_gamma(double linear) {
  linear = clamp01(linear);
  return linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1 / 2.4) - 0.055;
}

class LabSpace final : public ColorSpace {
 public:
  std::array<double, 3> white{0.9505, 1.0, 1.089};
  std::array<double, 4> range{-100, 100, -100, 100};
  LabSpace() {
    kind_ = Kind::Lab;
    n_ = 3;
  }
  std::vector<double> initial_color() const override {
    return {0, std::clamp(0.0, range[0], range[1]), std::clamp(0.0, range[2], range[3])};
  }
  std::array<double, 2> decode_range(int i, int) const override {
    if (i == 0) return {0, 100};
    return {range[static_cast<std::size_t>(2 * (i - 1))], range[static_cast<std::size_t>(2 * (i - 1) + 1)]};
  }
  Rgb to_rgb(const double* c) const override {
    const double l = std::clamp(c[0], 0.0, 100.0);
    const double a = std::clamp(c[1], range[0], range[1]);
    const double b = std::clamp(c[2], range[2], range[3]);
    const double fy = (l + 16) / 116, fx = fy + a / 500, fz = fy - b / 200;
    const auto g = [](double t) { return t >= 6.0 / 29 ? t * t * t : 108.0 / 841 * (t - 4.0 / 29); };
    // Bradford-free approximation: scale to D65 white.
    const double x = g(fx) * white[0] / 0.9505 * 0.9505, y = g(fy) * white[1], z = g(fz) * white[2] / 1.089 * 1.089;
    const double r = 3.2406 * x - 1.5372 * y - 0.4986 * z;
    const double gg = -0.9689 * x + 1.8758 * y + 0.0415 * z;
    const double bb = 0.0557 * x - 0.2040 * y + 1.0570 * z;
    return {srgb_gamma(r), srgb_gamma(gg), srgb_gamma(bb)};
  }
};

class IndexedSpace final : public ColorSpace {
 public:
  std::shared_ptr<const ColorSpace> base_space;
  int hival = 0;
  std::vector<std::uint8_t> lookup;
  IndexedSpace() {
    kind_ = Kind::Indexed;
    n_ = 1;
  }
  std::array<double, 2> decode_range(int, int bpc) const override { return {0, std::pow(2.0, bpc) - 1}; }
  Rgb to_rgb(const double* c) const override {
    const int index = std::clamp(static_cast<int>(std::lround(std::isfinite(c[0]) ? c[0] : 0)), 0, hival);
    const int n = base_space->components();
    double comps[32] = {};
    for (int k = 0; k < n && k < 32; ++k) {
      const std::size_t at = static_cast<std::size_t>(index * n + k);
      const double v = at < lookup.size() ? lookup[at] / 255.0 : 0;
      const auto range = base_space->decode_range(k, 8);
      comps[k] = range[0] + v * (range[1] - range[0]);
    }
    return base_space->to_rgb(comps);
  }
};

class TintSpace final : public ColorSpace {
 public:
  std::shared_ptr<const ColorSpace> alternate;
  std::shared_ptr<const Function> function;
  bool none = false;  // Separation /None: paints nothing (treated as white)
  bool all = false;
  explicit TintSpace(int n, bool separation) {
    kind_ = separation ? Kind::Separation : Kind::DeviceN;
    n_ = n;
  }
  std::vector<double> initial_color() const override { return std::vector<double>(static_cast<std::size_t>(n_), 1.0); }
  Rgb to_rgb(const double* c) const override {
    if (!function || !alternate) {
      const double v = 1 - clamp01(c[0]);
      return {v, v, v};
    }
    double out[32] = {};
    function->eval(c, out);
    return alternate->to_rgb(out);
  }
};

class PatternSpace final : public ColorSpace {
 public:
  std::shared_ptr<const ColorSpace> under;
  PatternSpace() {
    kind_ = Kind::Pattern;
    n_ = 0;
  }
  std::vector<double> initial_color() const override { return {}; }
  Rgb to_rgb(const double* c) const override {
    if (under) return under->to_rgb(c);
    return {0, 0, 0};
  }
  std::shared_ptr<const ColorSpace> base() const override { return under; }
  void set_components(int n) { n_ = n; }
};

}  // namespace

std::shared_ptr<const ColorSpace> device_gray() {
  static const std::shared_ptr<const ColorSpace> space = std::make_shared<GraySpace>();
  return space;
}
std::shared_ptr<const ColorSpace> device_rgb() {
  static const std::shared_ptr<const ColorSpace> space = std::make_shared<RgbSpace>();
  return space;
}
std::shared_ptr<const ColorSpace> device_cmyk() {
  static const std::shared_ptr<const ColorSpace> space = std::make_shared<CmykSpace>();
  return space;
}

std::shared_ptr<const ColorSpace> load_color_space(PdfFile& file, const Object& object, const Dict* resources,
                                                   int depth) {
  if (depth > 8) return nullptr;
  const Object obj = file.resolve(object);
  if (obj.is_name()) {
    const std::string& name = obj.as_name();
    if (name == "DeviceGray" || name == "G" || name == "CalGray") return device_gray();
    if (name == "DeviceRGB" || name == "RGB" || name == "CalRGB") return device_rgb();
    if (name == "DeviceCMYK" || name == "CMYK") return device_cmyk();
    if (name == "Pattern") return std::make_shared<PatternSpace>();
    if (resources != nullptr) {
      const Dict& spaces = file.get(*resources, "ColorSpace").as_dict();
      const Object named = spaces.get(name);
      if (!named.is_null()) return load_color_space(file, named, nullptr, depth + 1);
    }
    if (name == "DefaultGray") return device_gray();
    if (name == "DefaultRGB") return device_rgb();
    if (name == "DefaultCMYK") return device_cmyk();
    return nullptr;
  }
  if (!obj.is_array() || obj.as_array().empty()) return nullptr;
  const Array& a = obj.as_array();
  const std::string family = file.resolve(a[0]).as_name();
  const auto arg = [&](std::size_t i) { return i < a.size() ? file.resolve(a[i]) : Object{}; };
  if (family == "DeviceGray" || family == "CalGray" || family == "G") return device_gray();
  if (family == "DeviceRGB" || family == "CalRGB" || family == "RGB") return device_rgb();
  if (family == "DeviceCMYK" || family == "CMYK") return device_cmyk();
  if (family == "ICCBased") {
    const Object stream = arg(1);
    const Dict& d = stream.as_dict();
    const auto alternate = load_color_space(file, d.get("Alternate"), resources, depth + 1);
    const int n = static_cast<int>(file.get(d, "N").as_int(alternate ? alternate->components() : 3));
    if (alternate && alternate->components() == n) return alternate;
    if (n == 1) return device_gray();
    if (n == 4) return device_cmyk();
    return device_rgb();
  }
  if (family == "Lab") {
    auto lab = std::make_shared<LabSpace>();
    const Dict& d = arg(1).as_dict();
    const auto white = numbers(file, d.get("WhitePoint"));
    if (white.size() == 3) lab->white = {white[0], white[1], white[2]};
    const auto range = numbers(file, d.get("Range"));
    if (range.size() == 4) lab->range = {range[0], range[1], range[2], range[3]};
    return lab;
  }
  if (family == "Indexed" || family == "I") {
    auto indexed = std::make_shared<IndexedSpace>();
    indexed->base_space = load_color_space(file, a.size() > 1 ? a[1] : Object{}, resources, depth + 1);
    if (!indexed->base_space || indexed->base_space->kind() == ColorSpace::Kind::Pattern ||
        indexed->base_space->kind() == ColorSpace::Kind::Indexed) {
      indexed->base_space = device_rgb();
    }
    indexed->hival = static_cast<int>(std::clamp<std::int64_t>(arg(2).as_int(0), 0, 255));
    const Object lookup = arg(3);
    std::string bytes;
    if (lookup.is_string()) {
      bytes = lookup.as_string();
    } else if (lookup.is_stream()) {
      bytes = to_string(file.decode_or_empty(lookup));
    }
    indexed->lookup.assign(bytes.begin(), bytes.end());
    return indexed;
  }
  if (family == "Separation" || family == "DeviceN") {
    const bool separation = family == "Separation";
    const Object names = arg(1);
    int n = 1;
    bool none = false, all = false;
    if (separation) {
      none = names.is_name("None");
      all = names.is_name("All");
    } else {
      n = static_cast<int>(std::clamp<std::size_t>(names.as_array().size(), 1, 32));
    }
    auto space = std::make_shared<TintSpace>(n, separation);
    space->alternate = load_color_space(file, a.size() > 2 ? a[2] : Object{}, resources, depth + 1);
    space->function = load_function(file, a.size() > 3 ? a[3] : Object{});
    space->none = none;
    space->all = all;
    if (space->function && space->alternate && space->function->outputs() < space->alternate->components()) {
      space->function = nullptr;
    }
    return space;
  }
  if (family == "Pattern") {
    auto pattern = std::make_shared<PatternSpace>();
    if (a.size() > 1) {
      pattern->under = load_color_space(file, a[1], resources, depth + 1);
      if (pattern->under) pattern->set_components(pattern->under->components());
    }
    return pattern;
  }
  return nullptr;
}

}  // namespace cvt::pdf
