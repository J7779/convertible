// Convertible core - vector family: PostScript tokenizer, files and decode filters.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <utility>

#include "families/vector/ps_internal.hpp"
#include "util/deflate.hpp"
#include "util/text.hpp"

namespace cvt::vector::ps {

namespace {

constexpr bool is_white(int c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == 0; }
constexpr bool is_delim(int c) noexcept {
  return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' ||
         c == '%';
}
constexpr int hex_value(int c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/// Parse a PostScript number token (integer, real, radix). nullopt when it is a name.
std::optional<Object> parse_number(std::string_view t) {
  if (t.empty()) return std::nullopt;
  // radix: base#digits
  const std::size_t hash = t.find('#');
  if (hash != std::string_view::npos && hash > 0 && hash <= 2) {
    const auto base = util::parse_int(t.substr(0, hash));
    if (!base || *base < 2 || *base > 36 || hash + 1 >= t.size()) return std::nullopt;
    std::uint64_t value = 0;
    for (const char ch : t.substr(hash + 1)) {
      int d = -1;
      if (ch >= '0' && ch <= '9') d = ch - '0';
      else if (ch >= 'a' && ch <= 'z') d = ch - 'a' + 10;
      else if (ch >= 'A' && ch <= 'Z') d = ch - 'A' + 10;
      if (d < 0 || d >= *base) return std::nullopt;
      value = value * static_cast<std::uint64_t>(*base) + static_cast<std::uint64_t>(d);
      if (value > 0xFFFFFFFFull) return Object::real(static_cast<double>(value));  // limitcheck in real PS
    }
    // Radix numbers are unsigned 32-bit values reinterpreted as signed.
    return Object::integer(static_cast<std::int32_t>(static_cast<std::uint32_t>(value)));
  }
  std::size_t i = 0;
  if (t[0] == '+' || t[0] == '-') ++i;
  if (i >= t.size()) return std::nullopt;
  bool digits = false, dot = false, exp = false;
  for (std::size_t k = i; k < t.size(); ++k) {
    const char c = t[k];
    if (c >= '0' && c <= '9') {
      digits = true;
    } else if (c == '.' && !dot && !exp) {
      dot = true;
    } else if ((c == 'e' || c == 'E') && digits && !exp) {
      exp = true;
      if (k + 1 < t.size() && (t[k + 1] == '+' || t[k + 1] == '-')) ++k;
      if (k + 1 >= t.size()) return std::nullopt;
    } else {
      return std::nullopt;
    }
  }
  if (!digits) return std::nullopt;
  if (!dot && !exp) {
    std::string_view digits_text = t;
    if (digits_text[0] == '+') digits_text.remove_prefix(1);
    const auto v = util::parse_int(digits_text);
    if (v && *v >= std::numeric_limits<std::int32_t>::min() && *v <= std::numeric_limits<std::int32_t>::max()) {
      return Object::integer(*v);
    }
  }
  std::string text(t);
  if (text[0] == '+') text.erase(0, 1);
  if (!text.empty() && text[0] == '.') text.insert(0, "0");
  if (text.size() > 1 && text[0] == '-' && text[1] == '.') text.insert(1, "0");
  // "1." and "1.e5" are valid PostScript.
  for (std::size_t k = 0; k + 1 < text.size(); ++k) {
    if (text[k] == '.' && (text[k + 1] == 'e' || text[k + 1] == 'E')) text.insert(k + 1, "0");
  }
  if (!text.empty() && text.back() == '.') text += '0';
  const auto v = util::parse_double(text);
  if (!v) return std::nullopt;
  return Object::real(*v);
}

}  // namespace

// --------------------------------------------------------------------------- file access

int Interpreter::file_get(FileStore& f_in) {
  const std::size_t idx = static_cast<std::size_t>(&f_in - files_.data());
  if (idx >= files_.size()) return -1;
  if (files_[idx].kind == FileStore::Kind::Deferred) materialize(idx);
  FileStore* f = &files_[idx];
  if (f->closed) return -1;
  switch (f->kind) {
    case FileStore::Kind::Buffer:
    case FileStore::Kind::Deferred:
      if (f->pos >= f->data.size()) return -1;
      return static_cast<unsigned char>(f->data[f->pos++]);
    case FileStore::Kind::Eexec: {
      if (f->pending >= 0) {
        const int c = f->pending;
        f->pending = -1;
        return c;
      }
      const std::uint32_t source = f->source;
      const bool hex = f->hex;
      if (source >= files_.size() || source == idx) return -1;
      int cipher = -1;
      if (hex) {
        int hi = -1;
        while (true) {
          const int c = file_get(files_[source]);
          if (c < 0) return -1;
          if (is_white(c)) continue;
          const int v = hex_value(c);
          if (v < 0) return -1;
          if (hi < 0) {
            hi = v;
          } else {
            cipher = hi * 16 + v;
            break;
          }
        }
      } else {
        cipher = file_get(files_[source]);
        if (cipher < 0) return -1;
      }
      f = &files_[idx];
      const int plain = cipher ^ (f->r >> 8);
      f->r = static_cast<std::uint16_t>((cipher + f->r) * 52845u + 22719u);
      return plain;
    }
    case FileStore::Kind::Procedure: {
      if (f->pending >= 0) {
        const int c = f->pending;
        f->pending = -1;
        return c;
      }
      while (files_[idx].chunk_pos >= files_[idx].chunk.size()) {
        if (files_[idx].eof_seen) return -1;
        const Object proc = files_[idx].proc;
        call(proc);
        const Object s = pop();
        popped_.clear();
        if (s.type != Type::String) error("typecheck", "data source procedure must return a string");
        FileStore& self = files_[idx];
        self.chunk = std::string(string_view(s));
        self.chunk_pos = 0;
        if (self.chunk.empty()) {
          self.eof_seen = true;
          return -1;
        }
      }
      FileStore& self = files_[idx];
      return static_cast<unsigned char>(self.chunk[self.chunk_pos++]);
    }
  }
  return -1;
}

int Interpreter::file_peek(FileStore& f_in) {
  const std::size_t idx = static_cast<std::size_t>(&f_in - files_.data());
  if (idx >= files_.size()) return -1;
  if (files_[idx].kind == FileStore::Kind::Deferred) materialize(idx);
  FileStore& f = files_[idx];
  if (f.closed) return -1;
  if (f.kind == FileStore::Kind::Buffer) {
    if (f.pos >= f.data.size()) return -1;
    return static_cast<unsigned char>(f.data[f.pos]);
  }
  if (f.pending < 0) {
    const int c = file_get(f);
    files_[idx].pending = c;
  }
  return files_[idx].pending;
}

std::string Interpreter::file_read(const Object& fobj, std::size_t n) {
  std::string out;
  materialize(file(fobj).kind == FileStore::Kind::Deferred ? fobj.ref : files_.size());
  FileStore* f = &file(fobj);
  if (f->kind == FileStore::Kind::Buffer) {
    const std::size_t take = std::min(n, f->data.size() - std::min(f->pos, f->data.size()));
    out.assign(f->data.substr(f->pos, take));
    f->pos += take;
    return out;
  }
  out.reserve(std::min<std::size_t>(n, 1 << 20));
  while (out.size() < n) {
    const int c = file_get(*f);
    if (c < 0) break;
    out.push_back(static_cast<char>(c));
    f = &file(fobj);  // the store may have moved (procedure sources allocate)
  }
  return out;
}

// --------------------------------------------------------------------------- tokenizer

std::optional<Object> Interpreter::scan(const Object& fobj) {
  // Procedures are built with an explicit stack so hostile nesting cannot
  // overflow the C++ stack.
  std::vector<std::vector<Object>> procs;
  auto f = [&]() -> FileStore& { return file(fobj); };
  auto get = [&]() { return file_get(f()); };
  auto peek = [&]() { return file_peek(f()); };

  auto finish = [&](Object o) -> std::optional<Object> {
    if (procs.empty()) return o;
    procs.back().push_back(o);
    return std::nullopt;
  };

  while (true) {
    int c = get();
    if (c < 0) {
      if (!procs.empty()) error("syntaxerror", "unterminated procedure");
      return std::nullopt;
    }
    if (is_white(c)) continue;
    std::optional<Object> token;
    if (c == '%') {
      while (true) {
        const int d = get();
        if (d < 0 || d == '\n' || d == '\r') break;
      }
      continue;
    }
    if (c == '{') {
      if (procs.size() >= 1000) error("limitcheck", "procedures nested too deeply");
      procs.emplace_back();
      continue;
    }
    if (c == '}') {
      if (procs.empty()) error("syntaxerror", "unmatched }");
      std::vector<Object> items = std::move(procs.back());
      procs.pop_back();
      Object arr = make_array(items.size());
      std::copy(items.begin(), items.end(), arrays_[arr.ref].items.begin());
      arr.exec = true;
      token = finish(arr);
      if (token) return token;
      continue;
    }
    if (c == '(') {
      std::string s;
      int depth = 1;
      while (true) {
        int d = get();
        if (d < 0) error("syntaxerror", "unterminated string");
        if (d == '(') {
          ++depth;
        } else if (d == ')') {
          if (--depth == 0) break;
        } else if (d == '\\') {
          d = get();
          if (d < 0) error("syntaxerror", "unterminated string");
          switch (d) {
            case 'n': d = '\n'; break;
            case 'r': d = '\r'; break;
            case 't': d = '\t'; break;
            case 'b': d = '\b'; break;
            case 'f': d = '\f'; break;
            case '\r':
              if (peek() == '\n') get();
              continue;
            case '\n': continue;
            default:
              if (d >= '0' && d <= '7') {
                int v = d - '0';
                for (int k = 0; k < 2; ++k) {
                  const int e = peek();
                  if (e < '0' || e > '7') break;
                  v = v * 8 + (get() - '0');
                }
                d = v & 0xFF;
              }
              break;
          }
        } else if (d == '\r') {
          if (peek() == '\n') get();
          d = '\n';
        }
        if (s.size() >= 65535u * 64u) error("limitcheck", "string too long");
        s.push_back(static_cast<char>(d));
      }
      token = finish(make_string(s));
      if (token) return token;
      continue;
    }
    if (c == '<') {
      const int d = peek();
      if (d == '<') {
        get();
        token = finish(make_name("<<", true));
        if (token) return token;
        continue;
      }
      std::string s;
      if (d == '~') {
        get();
        // ASCII85 string
        std::uint32_t tuple = 0;
        int count = 0;
        while (true) {
          const int e = get();
          if (e < 0) error("syntaxerror", "unterminated ASCII85 string");
          if (is_white(e)) continue;
          if (e == '~') {
            get();  // '>'
            break;
          }
          if (e == 'z' && count == 0) {
            s.append(4, '\0');
            continue;
          }
          if (e < '!' || e > 'u') error("syntaxerror", "bad ASCII85 string");
          tuple = tuple * 85 + static_cast<std::uint32_t>(e - '!');
          if (++count == 5) {
            for (int k = 3; k >= 0; --k) s.push_back(static_cast<char>((tuple >> (8 * k)) & 0xFF));
            tuple = 0;
            count = 0;
          }
        }
        if (count > 1) {
          for (int k = count; k < 5; ++k) tuple = tuple * 85 + 84;
          for (int k = 0; k < count - 1; ++k) s.push_back(static_cast<char>((tuple >> (8 * (3 - k))) & 0xFF));
        }
      } else {
        int hi = -1;
        while (true) {
          const int e = get();
          if (e < 0) error("syntaxerror", "unterminated hex string");
          if (e == '>') break;
          if (is_white(e)) continue;
          const int v = hex_value(e);
          if (v < 0) error("syntaxerror", "bad hex string");
          if (hi < 0) {
            hi = v;
          } else {
            s.push_back(static_cast<char>(hi * 16 + v));
            hi = -1;
          }
          if (s.size() > 65535u * 1024u) error("limitcheck", "string too long");
        }
        if (hi >= 0) s.push_back(static_cast<char>(hi * 16));
      }
      token = finish(make_string(s));
      if (token) return token;
      continue;
    }
    if (c == '>') {
      if (peek() == '>') {
        get();
        token = finish(make_name(">>", true));
        if (token) return token;
        continue;
      }
      error("syntaxerror", "unexpected >");
    }
    if (c == '[' || c == ']') {
      token = finish(make_name(c == '[' ? "[" : "]", true));
      if (token) return token;
      continue;
    }
    if (c == ')') error("syntaxerror", "unexpected )");
    // name or number
    bool literal = false, immediate = false;
    if (c == '/') {
      literal = true;
      if (peek() == '/') {
        get();
        literal = false;
        immediate = true;
      }
      c = -2;
    }
    std::string text;
    if (c >= 0) text.push_back(static_cast<char>(c));
    while (true) {
      const int d = peek();
      if (d < 0 || is_delim(d)) break;
      if (is_white(d)) {
        get();
        if (d == '\r' && peek() == '\n') get();
        break;
      }
      text.push_back(static_cast<char>(get()));
      if (text.size() > 65535) error("limitcheck", "name too long");
    }
    Object o;
    if (literal) {
      o = make_name(text, false);
    } else if (immediate) {
      o = lookup(make_name(text));
    } else if (auto n = parse_number(text)) {
      o = *n;
    } else {
      o = make_name(text, true);
    }
    token = finish(o);
    if (token) return token;
  }
}

// --------------------------------------------------------------------------- filters

namespace {

Bytes png_unpredict(const std::string& data, int predictor, int colors, int bpc, int columns) {
  Bytes out;
  if (predictor < 10) {
    if (predictor == 2 && bpc == 8) {
      // TIFF predictor 2, 8-bit only.
      const std::size_t row = static_cast<std::size_t>(colors) * static_cast<std::size_t>(columns);
      out.resize(data.size());
      for (std::size_t i = 0; i < data.size(); ++i) {
        std::uint8_t v = static_cast<std::uint8_t>(data[i]);
        if (row > 0 && (i % row) >= static_cast<std::size_t>(colors)) {
          v = static_cast<std::uint8_t>(v + static_cast<std::uint8_t>(out[i - static_cast<std::size_t>(colors)]));
        }
        out[i] = std::byte{v};
      }
      return out;
    }
    out.resize(data.size());
    std::memcpy(out.data(), data.data(), data.size());
    return out;
  }
  const std::size_t bpp = std::max<std::size_t>(1, (static_cast<std::size_t>(colors) * static_cast<std::size_t>(bpc) + 7) / 8);
  const std::size_t row =
      (static_cast<std::size_t>(colors) * static_cast<std::size_t>(bpc) * static_cast<std::size_t>(columns) + 7) / 8;
  if (row == 0) return out;
  std::vector<std::uint8_t> prev(row, 0), cur(row, 0);
  std::size_t pos = 0;
  while (pos < data.size()) {
    const int type = static_cast<unsigned char>(data[pos++]);
    const std::size_t take = std::min(row, data.size() - pos);
    std::fill(cur.begin(), cur.end(), 0);
    std::memcpy(cur.data(), data.data() + pos, take);
    pos += take;
    for (std::size_t i = 0; i < row; ++i) {
      const int a = i >= bpp ? cur[i - bpp] : 0;
      const int b = prev[i];
      const int c = i >= bpp ? prev[i - bpp] : 0;
      int v = cur[i];
      switch (type) {
        case 1: v += a; break;
        case 2: v += b; break;
        case 3: v += (a + b) / 2; break;
        case 4: {
          const int p = a + b - c;
          const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
          v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
          break;
        }
        default: break;
      }
      cur[i] = static_cast<std::uint8_t>(v & 0xFF);
    }
    for (const std::uint8_t v : cur) out.push_back(std::byte{v});
    std::swap(prev, cur);
  }
  return out;
}

}  // namespace

Object Interpreter::filter(const Object& source, std::string_view name, const Object& params) {
  if (util::iends_with(name, "Encode")) {
    // Output filters: writes are discarded.
    FileStore fs;
    fs.writable = true;
    return make_file(std::move(fs));
  }
  static constexpr std::string_view known[] = {"ASCIIHexDecode", "AHx", "ASCII85Decode", "A85", "RunLengthDecode",
                                               "RL", "LZWDecode", "LZW", "FlateDecode", "Fl", "DCTDecode", "DCT",
                                               "SubFileDecode", "SFD", "ReusableStreamDecode"};
  if (std::find(std::begin(known), std::end(known), name) == std::end(known)) {
    error("undefined", std::format("unsupported filter {}", name));
  }
  if (source.type != Type::String && source.type != Type::File && !(source.type == Type::Array && source.exec)) {
    error("typecheck", "bad filter data source");
  }
  // Filters are decoded when first read (the data usually follows the
  // operator that consumes it).
  FileStore fs;
  fs.kind = FileStore::Kind::Deferred;
  fs.proc = source;
  fs.filter_name = std::string(name);
  fs.filter_params = params;
  return make_file(std::move(fs));
}

void Interpreter::materialize(std::size_t index) {
  if (index >= files_.size() || files_[index].kind != FileStore::Kind::Deferred) return;
  const Object source = files_[index].proc;
  const std::string name = files_[index].filter_name;
  const Object params = files_[index].filter_params;
  files_[index].kind = FileStore::Kind::Buffer;  // no recursion through itself
  files_[index].data = {};
  if (materialize_depth_ >= 32) error("limitcheck", "filters chained too deeply");
  ++materialize_depth_;
  bool jpeg = false;
  std::string out;
  try {
    out = decode_filter(source, name, params, jpeg);
  } catch (...) {
    --materialize_depth_;
    throw;
  }
  --materialize_depth_;
  charge(out.size());
  FileStore& fs = files_[index];
  fs.owned = std::make_shared<const std::string>(std::move(out));
  fs.data = *fs.owned;
  fs.pos = 0;
  fs.jpeg = jpeg;
  fs.proc = Object::null();
}

std::string Interpreter::decode_filter(const Object& source, std::string_view name, const Object& params, bool& dct) {
  // Resolve the source into a readable file.
  Object src = source;
  if (source.type == Type::String) {
    FileStore fs;
    fs.owned = std::make_shared<const std::string>(string_view(source));
    fs.data = *fs.owned;
    src = make_file(std::move(fs));
  } else if (source.type == Type::Array && source.exec) {
    FileStore fs;
    fs.kind = FileStore::Kind::Procedure;
    fs.proc = source;
    src = make_file(std::move(fs));
  } else if (source.type != Type::File) {
    error("typecheck", "bad filter data source");
  }

  const std::uint64_t cap = std::min<std::uint64_t>(ctx_.limits.max_decompressed_bytes, opts_.max_vm_bytes);
  auto param_int = [&](std::string_view key, std::int64_t fallback) {
    if (params.type != Type::Dict) return fallback;
    const Object* v = dict_find(params, key);
    return v != nullptr && v->is_number() ? static_cast<std::int64_t>(v->number()) : fallback;
  };

  std::string out;
  auto get = [&]() { return file_get(file(src)); };
  auto add = [&](char ch) {
    out.push_back(ch);
    if (out.size() > cap) fatal(Status::Limit, "decoded PostScript data exceeds the decompression limit");
  };
  dct = false;

  if (name == "ASCIIHexDecode" || name == "AHx") {
    int hi = -1;
    while (true) {
      const int c = get();
      if (c < 0 || c == '>') break;
      if (is_white(c)) continue;
      const int v = hex_value(c);
      if (v < 0) continue;  // tolerate stray characters
      if (hi < 0) {
        hi = v;
      } else {
        add(static_cast<char>(hi * 16 + v));
        hi = -1;
      }
    }
    if (hi >= 0) add(static_cast<char>(hi * 16));
  } else if (name == "ASCII85Decode" || name == "A85") {
    std::uint32_t tuple = 0;
    int count = 0;
    while (true) {
      const int c = get();
      if (c < 0) break;
      if (is_white(c)) continue;
      if (c == '~') {
        // Consume the '>' of the end marker.
        if (file_peek(file(src)) == '>') get();
        break;
      }
      if (c == 'z' && count == 0) {
        for (int k = 0; k < 4; ++k) add('\0');
        continue;
      }
      if (c < '!' || c > 'u') error("ioerror", "bad ASCII85 data");
      tuple = tuple * 85 + static_cast<std::uint32_t>(c - '!');
      if (++count == 5) {
        for (int k = 3; k >= 0; --k) add(static_cast<char>((tuple >> (8 * k)) & 0xFF));
        tuple = 0;
        count = 0;
      }
    }
    if (count > 1) {
      for (int k = count; k < 5; ++k) tuple = tuple * 85 + 84;
      for (int k = 0; k < count - 1; ++k) add(static_cast<char>((tuple >> (8 * (3 - k))) & 0xFF));
    }
  } else if (name == "RunLengthDecode" || name == "RL") {
    while (true) {
      const int n = get();
      if (n < 0 || n == 128) break;
      if (n < 128) {
        for (int k = 0; k <= n; ++k) {
          const int c = get();
          if (c < 0) break;
          add(static_cast<char>(c));
        }
      } else {
        const int c = get();
        if (c < 0) break;
        for (int k = 0; k < 257 - n; ++k) add(static_cast<char>(c));
      }
    }
  } else if (name == "LZWDecode" || name == "LZW") {
    const int early = static_cast<int>(param_int("EarlyChange", 1));
    std::vector<std::string> table;
    auto reset = [&] {
      table.clear();
      for (int i = 0; i < 256; ++i) table.emplace_back(1, static_cast<char>(i));
      table.emplace_back();  // 256 clear
      table.emplace_back();  // 257 eod
    };
    reset();
    int bits = 9;
    std::uint32_t buffer = 0;
    int have = 0;
    std::string prev;
    bool first = true;
    while (true) {
      while (have < bits) {
        const int c = get();
        if (c < 0) goto lzw_done;
        buffer = (buffer << 8) | static_cast<std::uint32_t>(c);
        have += 8;
      }
      const std::uint32_t code = (buffer >> (have - bits)) & ((1u << bits) - 1);
      have -= bits;
      if (code == 256) {
        reset();
        bits = 9;
        first = true;
        continue;
      }
      if (code == 257) break;
      std::string entry;
      if (code < table.size() && (code > 257 || code < 256)) {
        entry = table[code];
      } else if (code == table.size() && !first) {
        entry = prev + prev[0];
      } else {
        break;  // corrupt
      }
      for (const char ch : entry) add(ch);
      if (!first && table.size() < 4096) table.push_back(prev + entry[0]);
      prev = std::move(entry);
      first = false;
      const std::size_t next = table.size() + static_cast<std::size_t>(early);
      if (next >= 4096) bits = 12;
      else if (next >= 2048) bits = 12;
      else if (next >= 1024) bits = 11;
      else if (next >= 512) bits = 10;
      else bits = 9;
    }
  lzw_done:;
  } else if (name == "FlateDecode" || name == "Fl") {
    util::Inflater inflater(util::Inflater::Format::Zlib);
    Bytes decoded;
    FileStore& fs = file(src);
    if (fs.kind == FileStore::Kind::Buffer) {
      const ByteView rest = as_bytes(fs.data.substr(std::min(fs.pos, fs.data.size())));
      auto progress = inflater.feed(rest, decoded, cap);
      if (!progress) {
        if (progress.error().code == Status::Limit) fatal(Status::Limit, progress.error().message);
        error("ioerror", "damaged Flate data");
      }
      file(src).pos += progress->consumed;
    } else {
      // Feed byte by byte so nothing after the stream is consumed.
      while (!inflater.finished()) {
        const int c = get();
        if (c < 0) break;
        const std::byte b{static_cast<unsigned char>(c)};
        auto progress = inflater.feed(ByteView(&b, 1), decoded, cap);
        if (!progress) {
          if (progress.error().code == Status::Limit) fatal(Status::Limit, progress.error().message);
          error("ioerror", "damaged Flate data");
        }
      }
    }
    out.assign(reinterpret_cast<const char*>(decoded.data()), decoded.size());
  } else if (name == "DCTDecode" || name == "DCT") {
    // Copy one complete JPEG stream (up to and including EOI).
    int prev = -1;
    bool in_scan = false;
    std::size_t length_left = 0;
    while (true) {
      const int c = get();
      if (c < 0) break;
      add(static_cast<char>(c));
      if (length_left > 0) {
        --length_left;
        prev = -1;
        continue;
      }
      if (prev == 0xFF) {
        if (c == 0xD9) break;
        if (c == 0xDA) {
          in_scan = true;
        } else if (!in_scan && c != 0xD8 && c != 0x01 && !(c >= 0xD0 && c <= 0xD7) && c != 0xFF) {
          // A marker segment with a length: copy it verbatim.
          const int h = get(), l = get();
          if (h < 0 || l < 0) break;
          add(static_cast<char>(h));
          add(static_cast<char>(l));
          const int len = h * 256 + l;
          length_left = len >= 2 ? static_cast<std::size_t>(len - 2) : 0;
          prev = -1;
          continue;
        }
        if (c == 0xFF) {
          prev = 0xFF;
          continue;
        }
        prev = -1;
        continue;
      }
      prev = c;
    }
    dct = true;
  } else if (name == "SubFileDecode" || name == "SFD") {
    std::int64_t count = param_int("EODCount", 0);
    std::string eod;
    if (params.type == Type::Dict) {
      if (const Object* s = dict_find(params, "EODString"); s != nullptr && s->type == Type::String) {
        eod = std::string(string_view(*s));
      }
    } else if (params.type == Type::Array) {
      // [count string] packed by op_filter for the Level 2 form.
      const Object n = array_get(params, 0), s = array_get(params, 1);
      if (n.is_number()) count = static_cast<std::int64_t>(n.number());
      if (s.type == Type::String) eod = std::string(string_view(s));
    }
    if (eod.empty()) {
      for (std::int64_t k = 0; k < count; ++k) {
        const int c = get();
        if (c < 0) break;
        add(static_cast<char>(c));
      }
    } else {
      std::int64_t seen = 0;
      while (true) {
        const int c = get();
        if (c < 0) break;
        add(static_cast<char>(c));
        if (out.size() >= eod.size() && out.compare(out.size() - eod.size(), eod.size(), eod) == 0) {
          if (seen++ >= count) {
            out.resize(out.size() - eod.size());
            break;
          }
        }
      }
    }
  } else if (name == "ReusableStreamDecode" || name == "NullEncode") {
    while (true) {
      const int c = get();
      if (c < 0) break;
      add(static_cast<char>(c));
    }
  } else {
    error("undefined", std::format("unsupported filter {}", name));
  }

  // Predictors (Flate / LZW DecodeParms).
  if ((name == "FlateDecode" || name == "LZWDecode" || name == "Fl" || name == "LZW") && params.type == Type::Dict) {
    const int predictor = static_cast<int>(param_int("Predictor", 1));
    if (predictor > 1) {
      const Bytes un = png_unpredict(out, predictor, static_cast<int>(param_int("Colors", 1)),
                                     static_cast<int>(param_int("BitsPerComponent", 8)),
                                     static_cast<int>(param_int("Columns", 1)));
      out.assign(reinterpret_cast<const char*>(un.data()), un.size());
    }
  }

  return out;
}

std::string Interpreter::read_source(const Object& source, std::size_t want) {
  if (source.type == Type::String) return std::string(string_view(source));
  if (source.type == Type::File) return file_read(source, want);
  if (source.type == Type::Array && source.exec) {
    call(source);
    const Object s = pop();
    popped_.clear();
    if (s.type != Type::String) error("typecheck", "data source procedure must return a string");
    return std::string(string_view(s));
  }
  error("typecheck", "bad data source");
}

}  // namespace cvt::vector::ps
