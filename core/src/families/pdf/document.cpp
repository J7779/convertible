// Convertible core - pdf family: file structure, objects, security, pages.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/pdf/document.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <functional>
#include <span>

#include "families/pdf/crypto.hpp"
#include "families/pdf/syntax.hpp"
#include "util/text.hpp"

namespace cvt::pdf {

namespace {

constexpr std::array<std::uint8_t, 32> kPadding = {0x28, 0xBF, 0x4E, 0x5E, 0x4E, 0x75, 0x8A, 0x41, 0x64, 0x00, 0x4E,
                                                   0x56, 0xFF, 0xFA, 0x01, 0x08, 0x2E, 0x2E, 0x00, 0xB6, 0xD0, 0x68,
                                                   0x3E, 0x80, 0x2F, 0x0C, 0xA9, 0xFE, 0x64, 0x53, 0x69, 0x7A};

std::string_view as_view(ByteView data) { return as_chars(data); }

std::size_t rfind_text(ByteView data, std::string_view needle, std::size_t from_end_limit) {
  const std::string_view hay = as_view(data);
  const std::size_t start = hay.size() > from_end_limit ? hay.size() - from_end_limit : 0;
  const std::size_t pos = hay.substr(start).rfind(needle);
  return pos == std::string_view::npos ? std::string_view::npos : start + pos;
}

std::span<const std::uint8_t> u8span(const std::string& s) {
  return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

std::string to_str(std::span<const std::uint8_t> bytes) {
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

}  // namespace

enum class CryptMethod : std::uint8_t { Identity, Rc4, AesV2, AesV3 };

struct PdfFile::Security {
  int v = 0;
  int r = 0;
  int key_length = 5;
  std::string o, u, oe, ue;
  std::int32_t p = 0;
  bool encrypt_metadata = true;
  std::string id0;
  std::map<std::string, CryptMethod, std::less<>> filters;
  CryptMethod stream_method = CryptMethod::Rc4;
  CryptMethod string_method = CryptMethod::Rc4;
  std::string key;
};

PdfFile::PdfFile(ByteView data, OpenOptions options) : data_(data), options_(std::move(options)) {}

void PdfFile::add_warning(std::string message) {
  if (warnings_.size() < 20 && std::find(warnings_.begin(), warnings_.end(), message) == warnings_.end()) {
    warnings_.push_back(std::move(message));
  }
}

Result<std::shared_ptr<PdfFile>> PdfFile::open(ByteView data, const OpenOptions& options) {
  std::shared_ptr<PdfFile> file(new PdfFile(data, options));
  CVT_TRY(file->load());
  return file;
}

// --------------------------------------------------------------------------- xref loading

bool PdfFile::load_xref_table(std::size_t offset, Dict& trailer_out) {
  Lexer lexer(data_, offset);
  Token t = lexer.next();
  if (t.kind != Token::Kind::Keyword || t.text != "xref") return false;
  std::uint64_t entries = 0;
  while (true) {
    const std::size_t before = lexer.position();
    Token first = lexer.next();
    if (first.kind == Token::Kind::Keyword && first.text == "trailer") break;
    if (first.kind != Token::Kind::Integer) {
      lexer.seek(before);
      // Some files lack the trailer keyword; look for it nearby.
      const std::string_view rest = as_view(data_).substr(before, 4096);
      const std::size_t tr = rest.find("trailer");
      if (tr == std::string_view::npos) return entries > 0;
      lexer.seek(before + tr + 7);
      break;
    }
    Token count = lexer.next();
    if (count.kind != Token::Kind::Integer || first.integer < 0 || count.integer < 0) return false;
    std::int64_t start = first.integer;
    for (std::int64_t i = 0; i < count.integer; ++i) {
      if (++entries > options_.max_objects) return false;
      const std::size_t entry_pos = lexer.position();
      Token off = lexer.next();
      Token gen = lexer.next();
      Token kind = lexer.next();
      if (off.kind != Token::Kind::Integer || gen.kind != Token::Kind::Integer || kind.kind != Token::Kind::Keyword ||
          (kind.text != "n" && kind.text != "f")) {
        lexer.seek(entry_pos);
        break;
      }
      // Classic off-by-one: a subsection "1 n" whose first entry is the free head.
      if (i == 0 && start == 1 && kind.text == "f" && off.integer == 0 && gen.integer == 65535) start = 0;
      const auto num = static_cast<std::uint64_t>(start + i);
      if (num > 0x7FFFFFFF) continue;
      const auto key = static_cast<std::uint32_t>(num);
      if (xref_.contains(key)) continue;  // newer section wins
      XrefEntry entry;
      if (kind.text == "n" && off.integer > 0) {
        entry.kind = XrefEntry::Kind::Offset;
        entry.offset = static_cast<std::uint64_t>(off.integer);
        entry.gen = static_cast<std::uint32_t>(std::clamp<std::int64_t>(gen.integer, 0, 65535));
      }
      xref_.emplace(key, entry);
    }
  }
  bool ok = true;
  Object trailer = parse_object(lexer, 32, &ok);
  if (!trailer.is_dict()) return entries > 0;
  trailer_out = trailer.as_dict();
  return true;
}

bool PdfFile::load_xref_stream(std::size_t offset, Dict& trailer_out) {
  bool ok = true;
  Object obj = parse_indirect_at(offset, Ref{}, &ok);
  const Stream* stream = obj.as_stream();
  if (stream == nullptr) return false;
  const Dict& dict = stream->dict;
  if (!dict.get("Type").is_name("XRef")) return false;
  FilterEnv env;
  env.max_output = std::min<std::uint64_t>(options_.limits.max_decompressed_bytes, 1ull << 30);
  auto decoded = apply_filters(stream->data, dict.get("Filter"), dict.get("DecodeParms"), env);
  if (!decoded) return false;
  const Bytes& bytes = decoded->data;
  const Array& w = dict.get("W").as_array();
  if (w.size() < 3) return false;
  std::array<int, 3> widths{};
  for (std::size_t i = 0; i < 3; ++i) {
    widths[i] = static_cast<int>(w[i].as_int(-1));
    if (widths[i] < 0 || widths[i] > 8) return false;
  }
  const std::size_t row = static_cast<std::size_t>(widths[0] + widths[1] + widths[2]);
  if (row == 0) return false;
  std::vector<std::int64_t> index;
  for (const Object& o : dict.get("Index").as_array()) index.push_back(o.as_int(-1));
  if (index.empty()) {
    index = {0, dict.get("Size").as_int(0)};
  }
  std::size_t pos = 0;
  std::uint64_t total = 0;
  const auto field = [&](int width, std::uint64_t fallback) {
    if (width == 0) return fallback;
    std::uint64_t value = 0;
    for (int k = 0; k < width; ++k) value = (value << 8) | std::to_integer<std::uint8_t>(bytes[pos++]);
    return value;
  };
  for (std::size_t s = 0; s + 1 < index.size(); s += 2) {
    const std::int64_t start = index[s];
    const std::int64_t count = index[s + 1];
    if (start < 0 || count < 0) break;
    for (std::int64_t i = 0; i < count; ++i) {
      if (pos + row > bytes.size()) break;
      if (++total > options_.max_objects) return false;
      const std::uint64_t type = field(widths[0], 1);
      const std::uint64_t f2 = field(widths[1], 0);
      const std::uint64_t f3 = field(widths[2], 0);
      const std::uint64_t num = static_cast<std::uint64_t>(start + i);
      if (num > 0x7FFFFFFF) continue;
      const auto key = static_cast<std::uint32_t>(num);
      if (xref_.contains(key)) continue;
      XrefEntry entry;
      if (type == 1) {
        entry.kind = XrefEntry::Kind::Offset;
        entry.offset = f2;
        entry.gen = static_cast<std::uint32_t>(std::min<std::uint64_t>(f3, 65535));
      } else if (type == 2) {
        entry.kind = XrefEntry::Kind::Compressed;
        entry.offset = f2;
        entry.index = static_cast<std::uint32_t>(std::min<std::uint64_t>(f3, 0xFFFFFFFF));
      } else if (type != 0) {
        continue;  // unknown types are treated as null references
      }
      xref_.emplace(key, entry);
    }
  }
  trailer_out = dict;
  return true;
}

bool PdfFile::load_xref_chain(std::size_t start) {
  std::set<std::size_t> visited;
  std::size_t offset = start;
  bool any = false;
  bool first = true;
  for (int guard = 0; guard < 512; ++guard) {
    if (offset >= data_.size() || visited.contains(offset)) break;
    visited.insert(offset);
    Dict section_trailer;
    Lexer probe(data_, offset);
    probe.skip_whitespace();
    std::size_t at = probe.position();
    bool loaded = false;
    if (as_view(data_).substr(at, 4) == "xref") {
      loaded = load_xref_table(at, section_trailer);
    } else {
      loaded = load_xref_stream(at, section_trailer);
    }
    if (!loaded && header_offset_ > 0 && offset + header_offset_ < data_.size()) {
      // Offsets measured from the %PDF header when junk precedes it.
      Lexer shifted(data_, offset + header_offset_);
      shifted.skip_whitespace();
      at = shifted.position();
      loaded = as_view(data_).substr(at, 4) == "xref" ? load_xref_table(at, section_trailer)
                                                       : load_xref_stream(at, section_trailer);
    }
    if (!loaded) {
      if (!first || !any) return any;
      break;
    }
    any = true;
    // Merge trailer entries: newer ones win.
    for (const auto& [key, value] : section_trailer) {
      if (key == "Prev" || key == "XRefStm" || key == "W" || key == "Index" || key == "Filter" ||
          key == "DecodeParms" || key == "Length" || key == "Type") {
        continue;
      }
      if (!trailer_.contains(key)) trailer_.set(key, value);
    }
    // Hybrid file: the cross-reference stream of this section.
    const Object xrefstm = section_trailer.get("XRefStm");
    if (xrefstm.is_int() && xrefstm.as_int() > 0) {
      Dict ignored;
      const auto pos = static_cast<std::size_t>(xrefstm.as_int());
      if (!visited.contains(pos)) {
        visited.insert(pos);
        // Entries the table left free may be defined by the stream.
        std::vector<std::uint32_t> free_numbers;
        for (const auto& [num, entry] : xref_) {
          if (entry.kind == XrefEntry::Kind::Free) free_numbers.push_back(num);
        }
        for (const std::uint32_t num : free_numbers) xref_.erase(num);
        load_xref_stream(pos, ignored);
      }
    }
    const Object prev = section_trailer.get("Prev");
    if (!prev.is_number() || prev.as_int() <= 0) break;
    offset = static_cast<std::size_t>(prev.as_int());
    first = false;
  }
  return any;
}

void PdfFile::reconstruct() {
  reconstructed_ = true;
  const std::string_view text = as_view(data_);
  std::size_t pos = 0;
  std::uint64_t count = 0;
  while (true) {
    pos = text.find("obj", pos);
    if (pos == std::string_view::npos) break;
    const std::size_t obj_pos = pos;
    pos += 3;
    if (obj_pos + 3 < text.size() && is_pdf_regular(static_cast<std::uint8_t>(text[obj_pos + 3]))) continue;
    // Walk back: whitespace, generation digits, whitespace, number digits.
    std::size_t i = obj_pos;
    const auto skip_space_back = [&]() {
      while (i > 0 && is_pdf_space(static_cast<std::uint8_t>(text[i - 1]))) --i;
    };
    const auto digits_back = [&]() {
      const std::size_t end = i;
      while (i > 0 && util::is_ascii_digit(text[i - 1]) && end - i < 10) --i;
      return end - i;
    };
    skip_space_back();
    const std::size_t gen_end = i;
    if (digits_back() == 0) continue;
    const std::size_t gen_start = i;
    const std::size_t before_gen = i;
    skip_space_back();
    if (i == before_gen) continue;
    const std::size_t num_end = i;
    if (digits_back() == 0) continue;
    const std::size_t num_start = i;
    if (num_start > 0 && is_pdf_regular(static_cast<std::uint8_t>(text[num_start - 1]))) continue;
    const auto num = util::parse_int(text.substr(num_start, num_end - num_start));
    const auto gen = util::parse_int(text.substr(gen_start, gen_end - gen_start));
    if (!num || !gen || *num <= 0 || *num > 0x7FFFFFFF || *gen < 0 || *gen > 65535) continue;
    if (++count > options_.max_objects) break;
    XrefEntry entry;
    entry.kind = XrefEntry::Kind::Offset;
    entry.offset = num_start;
    entry.gen = static_cast<std::uint32_t>(*gen);
    recovered_[static_cast<std::uint32_t>(*num)] = entry;  // later definitions win
  }
  // Objects inside object streams that have no direct definition.
  std::vector<std::uint32_t> objstms;
  for (const auto& [num, entry] : recovered_) {
    const Ref ref{num, entry.gen};
    bool ok = true;
    // Cheap check: does the object dictionary mention ObjStm?
    const std::string_view head = text.substr(static_cast<std::size_t>(entry.offset), 512);
    if (head.find("ObjStm") == std::string_view::npos) continue;
    Object obj = parse_indirect_at(static_cast<std::size_t>(entry.offset), ref, &ok);
    if (obj.as_dict().get("Type").is_name("ObjStm")) objstms.push_back(num);
  }
  for (const std::uint32_t stm : objstms) {
    Object stream_obj = get(Ref{stm, recovered_[stm].gen});
    const Stream* stream = stream_obj.as_stream();
    if (stream == nullptr) continue;
    auto decoded = decode(stream_obj);
    if (!decoded) continue;
    Lexer lexer(decoded->data);
    const std::int64_t n = std::min<std::int64_t>(stream->dict.get("N").as_int(0), 1'000'000);
    for (std::int64_t k = 0; k < n; ++k) {
      Token num = lexer.next();
      Token off = lexer.next();
      if (num.kind != Token::Kind::Integer || off.kind != Token::Kind::Integer) break;
      if (num.integer <= 0 || num.integer > 0x7FFFFFFF) continue;
      const auto key = static_cast<std::uint32_t>(num.integer);
      if (recovered_.contains(key)) continue;
      XrefEntry entry;
      entry.kind = XrefEntry::Kind::Compressed;
      entry.offset = stm;
      entry.index = static_cast<std::uint32_t>(k);
      recovered_[key] = entry;
    }
  }
}

void PdfFile::ensure_reconstructed() {
  if (!reconstructed_) reconstruct();
}

// --------------------------------------------------------------------------- objects

std::size_t PdfFile::find_stream_end(std::size_t data_start, std::size_t declared_length, bool length_known) {
  const std::string_view text = as_view(data_);
  if (length_known && declared_length <= data_.size() - data_start) {
    std::size_t end = data_start + declared_length;
    std::size_t probe = end;
    while (probe < text.size() && is_pdf_space(static_cast<std::uint8_t>(text[probe])) && probe < end + 64) ++probe;
    if (text.substr(probe, 9) == "endstream") return end;
  }
  // Search for "endstream" and drop the end-of-line before it.
  const std::size_t found = text.find("endstream", data_start);
  std::size_t end = found == std::string_view::npos ? text.size() : found;
  if (end > data_start && text[end - 1] == '\n') --end;
  if (end > data_start && text[end - 1] == '\r') --end;
  return end;
}

Object PdfFile::parse_indirect_at(std::size_t offset, Ref expected, bool* ok) {
  *ok = false;
  if (offset >= data_.size()) return {};
  Lexer lexer(data_, offset);
  const Token num = lexer.next();
  const Token gen = lexer.next();
  const Token obj = lexer.next();
  if (num.kind != Token::Kind::Integer || gen.kind != Token::Kind::Integer || obj.kind != Token::Kind::Keyword ||
      obj.text != "obj") {
    return {};
  }
  if (expected.num != 0 && static_cast<std::uint64_t>(num.integer) != expected.num) return {};
  bool parsed_ok = true;
  Object value = parse_object(lexer, 64, &parsed_ok);
  const std::size_t after_value = lexer.position();
  const Token next = lexer.next();
  if (value.is_dict() && next.kind == Token::Kind::Keyword && next.text == "stream") {
    std::size_t data_start = lexer.position();
    const std::string_view text = as_view(data_);
    if (data_start < text.size() && text[data_start] == '\r') ++data_start;
    if (data_start < text.size() && text[data_start] == '\n') ++data_start;
    const Object length_obj = value.as_dict().get("Length");
    std::int64_t length = -1;
    if (length_obj.is_int()) {
      length = length_obj.as_int();
    } else if (length_obj.is_ref()) {
      const Ref lr = length_obj.as_ref();
      if (lr.num != static_cast<std::uint64_t>(num.integer)) length = resolve(length_obj).as_int(-1);
    }
    const bool known = length >= 0 && static_cast<std::uint64_t>(length) <= data_.size();
    const std::size_t end = find_stream_end(data_start, known ? static_cast<std::size_t>(length) : 0, known);
    Stream stream;
    stream.dict = value.as_dict();
    if (end > data_start) stream.data.assign(data_.begin() + static_cast<std::ptrdiff_t>(data_start),
                                              data_.begin() + static_cast<std::ptrdiff_t>(end));
    *ok = true;
    return Object(std::move(stream));
  }
  (void)after_value;
  *ok = parsed_ok || !value.is_null();
  return value;
}

Object PdfFile::load_from_object_stream(std::uint32_t stream_num, std::uint32_t index, Ref ref) {
  if (loading_.contains(stream_num)) return {};
  auto it = objstm_index_.find(stream_num);
  Object stream_obj = get(Ref{stream_num, 0});
  const Stream* stream = stream_obj.as_stream();
  if (stream == nullptr) return {};
  auto decoded = decode(stream_obj);
  if (!decoded) return {};
  const Bytes& bytes = decoded->data;
  const std::int64_t first = stream->dict.get("First").as_int(0);
  if (first < 0 || static_cast<std::uint64_t>(first) > bytes.size()) return {};
  if (it == objstm_index_.end()) {
    std::vector<std::pair<std::uint32_t, std::size_t>> entries;
    Lexer lexer(bytes);
    const std::int64_t n = std::min<std::int64_t>(stream->dict.get("N").as_int(0), 1'000'000);
    for (std::int64_t k = 0; k < n; ++k) {
      const Token num = lexer.next();
      const Token off = lexer.next();
      if (num.kind != Token::Kind::Integer || off.kind != Token::Kind::Integer || off.integer < 0) break;
      entries.emplace_back(static_cast<std::uint32_t>(std::clamp<std::int64_t>(num.integer, 0, 0x7FFFFFFF)),
                           static_cast<std::size_t>(first + off.integer));
    }
    it = objstm_index_.emplace(stream_num, std::move(entries)).first;
  }
  const auto& entries = it->second;
  std::size_t at = 0;
  bool found = false;
  if (index < entries.size() && entries[index].first == ref.num) {
    at = entries[index].second;
    found = true;
  } else {
    for (const auto& [num, off] : entries) {
      if (num == ref.num) {
        at = off;
        found = true;
        break;
      }
    }
  }
  if (!found || at >= bytes.size()) return {};
  Lexer lexer(bytes, at);
  bool ok = true;
  Object value = parse_object(lexer, 64, &ok);
  return value;
}

Object PdfFile::load_object(Ref ref) {
  const auto try_entry = [&](const XrefEntry& entry, bool* ok) -> Object {
    *ok = false;
    if (entry.kind == XrefEntry::Kind::Offset) {
      Object obj = parse_indirect_at(static_cast<std::size_t>(entry.offset), ref, ok);
      if (!*ok && header_offset_ > 0) obj = parse_indirect_at(static_cast<std::size_t>(entry.offset) + header_offset_, ref, ok);
      if (*ok && security_ && ref.num != encrypt_ref_.num) decrypt_object(obj, ref, 0);
      return obj;
    }
    if (entry.kind == XrefEntry::Kind::Compressed) {
      if (entry.offset > 0x7FFFFFFF) return {};
      Object obj = load_from_object_stream(static_cast<std::uint32_t>(entry.offset), entry.index, ref);
      *ok = !obj.is_null();
      return obj;
    }
    return {};
  };
  bool ok = false;
  Object result;
  if (const auto it = xref_.find(ref.num); it != xref_.end()) {
    if (it->second.kind == XrefEntry::Kind::Free) return {};
    result = try_entry(it->second, &ok);
  }
  if (!ok) {
    ensure_reconstructed();
    if (const auto it = recovered_.find(ref.num); it != recovered_.end()) {
      result = try_entry(it->second, &ok);
      if (ok && xref_.contains(ref.num)) add_warning("the cross-reference table is damaged; objects were recovered by scanning");
    }
  }
  return ok ? result : Object{};
}

Object PdfFile::get(Ref ref) {
  if (ref.num == 0) return {};
  if (const auto it = cache_.find(ref.num); it != cache_.end()) return it->second;
  if (loading_.contains(ref.num)) return {};  // reference cycle
  if (loading_.size() > 64) return {};        // runaway nesting
  loading_.insert(ref.num);
  Object value = load_object(ref);
  loading_.erase(ref.num);
  if (cache_.size() < options_.max_objects) cache_.emplace(ref.num, value);
  return value;
}

Object PdfFile::resolve(const Object& object) {
  Object current = object;
  for (int i = 0; i < 32 && current.is_ref(); ++i) current = get(current.as_ref());
  return current.is_ref() ? Object{} : current;
}

Result<DecodedData> PdfFile::decode(const Object& stream_ref, bool stop_at_images) {
  const Object obj = resolve(stream_ref);
  const Stream* stream = obj.as_stream();
  if (stream == nullptr) return fail(Status::Decode, "expected a PDF stream");
  FilterEnv env;
  const std::uint64_t remaining = options_.limits.max_decompressed_bytes > decompressed_total_
                                      ? options_.limits.max_decompressed_bytes - decompressed_total_
                                      : 0;
  env.max_output = remaining;
  env.max_pixels = options_.limits.max_pixels;
  env.stop_at_dct = stop_at_images;
  env.stop_at_jpx = stop_at_images;
  env.resolve = [this](const Object& o) { return resolve(o); };
  env.decode_stream = [this](const Object& o) -> Result<Bytes> {
    CVT_TRY_ASSIGN(DecodedData d, decode(o));
    return std::move(d.data);
  };
  Object filter = resolve(stream->dict.get("Filter"));
  Object params = resolve(stream->dict.get("DecodeParms"));
  if (filter.is_null()) filter = resolve(stream->dict.get("F"));  // never valid for embedded streams, but seen
  if (params.is_null()) params = resolve(stream->dict.get("DP"));
  auto result = apply_filters(stream->data, filter, params, env);
  if (result) {
    decompressed_total_ += result->data.size();
    if (result->damaged) add_warning("a compressed stream is damaged; its readable part was used");
  }
  return result;
}

Bytes PdfFile::decode_or_empty(const Object& stream) {
  auto decoded = decode(stream);
  if (!decoded || !decoded->pending_filter.empty()) return {};
  return std::move(decoded->data);
}

std::vector<Ref> PdfFile::object_refs() const {
  std::vector<Ref> refs;
  for (const auto& [num, entry] : xref_) {
    if (entry.kind != XrefEntry::Kind::Free) refs.push_back(Ref{num, entry.kind == XrefEntry::Kind::Offset ? entry.gen : 0});
  }
  std::sort(refs.begin(), refs.end());
  return refs;
}

// --------------------------------------------------------------------------- security

namespace {

std::string compute_r6_hash(const std::string& password, const std::string& salt, const std::string& udata, int r) {
  std::string input = password + salt + udata;
  const auto first = crypto::sha256(u8span(input));
  std::string k = to_str(first);
  if (r < 6) return k;
  for (int round = 0;; ++round) {
    std::string k1;
    const std::string unit = password + k + udata;
    k1.reserve(unit.size() * 64);
    for (int i = 0; i < 64; ++i) k1 += unit;
    const std::string key16 = k.substr(0, 16);
    const std::string iv = k.substr(16, 16);
    const auto e = crypto::aes_cbc_encrypt_nopad(u8span(key16), u8span(iv), u8span(k1));
    if (e.size() < 16) return k.substr(0, 32);
    int sum = 0;
    for (int i = 0; i < 16; ++i) sum += e[static_cast<std::size_t>(i)];
    switch (sum % 3) {
      case 0: k = to_str(crypto::sha256(e)); break;
      case 1: k = to_str(crypto::sha384(e)); break;
      default: k = to_str(crypto::sha512(e)); break;
    }
    if (round >= 63 && static_cast<int>(e.back()) <= round + 1 - 32) break;
    if (round > 2000) break;
  }
  return k.substr(0, 32);
}

std::string pad_password(const std::string& password) {
  std::string out = password.substr(0, 32);
  const std::size_t used = out.size();
  for (std::size_t i = used; i < 32; ++i) out.push_back(static_cast<char>(kPadding[i - used]));
  return out;
}

}  // namespace

Result<void> PdfFile::setup_security(const std::string& password) {
  const Object encrypt_obj = trailer_.get("Encrypt");
  if (encrypt_obj.is_ref()) encrypt_ref_ = encrypt_obj.as_ref();
  const Object encrypt = resolve(encrypt_obj);
  if (!encrypt.is_dict()) return {};
  const Dict& dict = encrypt.as_dict();
  const std::string filter = dict.get("Filter").as_name();
  if (filter != "Standard") {
    return fail(Status::Decode, "the PDF uses the /{} security handler, which is not supported",
                filter.empty() ? std::string("unknown") : filter);
  }
  auto sec = std::make_shared<Security>();
  sec->v = static_cast<int>(dict.get("V").as_int(0));
  sec->r = static_cast<int>(dict.get("R").as_int(2));
  sec->key_length = static_cast<int>(std::clamp<std::int64_t>(dict.get("Length").as_int(40), 40, 256) / 8);
  if (sec->v == 1) sec->key_length = 5;
  sec->o = resolve(dict.get("O")).as_string();
  sec->u = resolve(dict.get("U")).as_string();
  sec->oe = resolve(dict.get("OE")).as_string();
  sec->ue = resolve(dict.get("UE")).as_string();
  sec->p = static_cast<std::int32_t>(static_cast<std::uint32_t>(dict.get("P").as_int(0)));
  sec->encrypt_metadata = dict.get("EncryptMetadata").as_bool(true);
  const Array& id = resolve(trailer_.get("ID")).as_array();
  if (!id.empty()) sec->id0 = resolve(id[0]).as_string();
  const auto method_of = [](const std::string& cfm) {
    if (cfm == "V2") return CryptMethod::Rc4;
    if (cfm == "AESV2") return CryptMethod::AesV2;
    if (cfm == "AESV3") return CryptMethod::AesV3;
    return CryptMethod::Identity;
  };
  if (sec->v >= 4) {
    const Dict& cf = resolve(dict.get("CF")).as_dict();
    for (const auto& [name, value] : cf) {
      const Dict& f = resolve(value).as_dict();
      sec->filters[name] = method_of(resolve(f.get("CFM")).as_name());
      if (sec->v == 4 && f.contains("Length")) {
        const std::int64_t len = f.get("Length").as_int(16);
        sec->key_length = static_cast<int>(len > 32 ? len / 8 : len);
      }
    }
    const auto pick = [&](std::string_view key) {
      const std::string name = dict.get(key).as_name();
      if (name.empty() || name == "Identity") return CryptMethod::Identity;
      const auto it = sec->filters.find(name);
      return it == sec->filters.end() ? CryptMethod::Identity : it->second;
    };
    sec->stream_method = pick("StmF");
    sec->string_method = pick("StrF");
    if (sec->v == 4) sec->key_length = std::clamp(sec->key_length, 5, 16);
  } else {
    sec->stream_method = CryptMethod::Rc4;
    sec->string_method = CryptMethod::Rc4;
    sec->key_length = std::clamp(sec->key_length, 5, 16);
  }

  if (sec->r >= 5) {
    // AES-256: SHA-256 (R5) or the iterated hash of ISO 32000-2 (R6).
    std::string pw = util::sanitize_utf8(password).substr(0, 127);
    if (sec->u.size() < 48 || sec->o.size() < 48) return fail(Status::Decode, "the PDF encryption dictionary is damaged");
    const std::string u_hash = sec->u.substr(0, 32), u_vsalt = sec->u.substr(32, 8), u_ksalt = sec->u.substr(40, 8);
    const std::string o_hash = sec->o.substr(0, 32), o_vsalt = sec->o.substr(32, 8), o_ksalt = sec->o.substr(40, 8);
    const std::string u48 = sec->u.substr(0, 48);
    std::string key;
    if (compute_r6_hash(pw, o_vsalt, u48, sec->r) == o_hash && sec->oe.size() >= 32) {
      const std::string kek = compute_r6_hash(pw, o_ksalt, u48, sec->r);
      key = to_str(crypto::aes_cbc_decrypt_zero_iv(u8span(kek), u8span(sec->oe.substr(0, 32))));
    } else if (compute_r6_hash(pw, u_vsalt, "", sec->r) == u_hash && sec->ue.size() >= 32) {
      const std::string kek = compute_r6_hash(pw, u_ksalt, "", sec->r);
      key = to_str(crypto::aes_cbc_decrypt_zero_iv(u8span(kek), u8span(sec->ue.substr(0, 32))));
    } else {
      return fail(Status::InvalidArgument,
                  password.empty() ? "the PDF is protected by a password; set the 'password' option"
                                   : "the password does not open this PDF");
    }
    if (key.size() != 32) return fail(Status::Decode, "the PDF encryption dictionary is damaged");
    sec->key = key;
    sec->key_length = 32;
  } else {
    const auto file_key = [&](const std::string& user_padded) {
      std::string input = user_padded + sec->o.substr(0, 32);
      const auto p = static_cast<std::uint32_t>(sec->p);
      for (int i = 0; i < 4; ++i) input.push_back(static_cast<char>((p >> (8 * i)) & 0xFF));
      input += sec->id0;
      if (sec->r >= 4 && !sec->encrypt_metadata) input += "\xFF\xFF\xFF\xFF";
      auto hash = crypto::md5(u8span(input));
      const auto n = static_cast<std::size_t>(sec->key_length);
      if (sec->r >= 3) {
        for (int i = 0; i < 50; ++i) hash = crypto::md5(std::span<const std::uint8_t>(hash.data(), n));
      }
      return std::string(reinterpret_cast<const char*>(hash.data()), n);
    };
    const auto check_user = [&](const std::string& key) {
      if (sec->r == 2) {
        const auto u = crypto::rc4(u8span(key), kPadding);
        return to_str(u) == sec->u.substr(0, 32);
      }
      std::string input(reinterpret_cast<const char*>(kPadding.data()), 32);
      input += sec->id0;
      const auto hash = crypto::md5(u8span(input));
      auto value = crypto::rc4(u8span(key), hash);
      for (int i = 1; i <= 19; ++i) {
        std::string k = key;
        for (char& c : k) c = static_cast<char>(c ^ i);
        value = crypto::rc4(u8span(k), value);
      }
      return sec->u.size() >= 16 && to_str(value).substr(0, 16) == sec->u.substr(0, 16);
    };
    std::string key = file_key(pad_password(password));
    if (!check_user(key)) {
      // Treat the password as the owner password.
      auto hash = crypto::md5(u8span(pad_password(password)));
      const auto n = static_cast<std::size_t>(sec->key_length);
      if (sec->r >= 3) {
        for (int i = 0; i < 50; ++i) hash = crypto::md5(std::span<const std::uint8_t>(hash.data(), 16));
      }
      const std::string rc4_key(reinterpret_cast<const char*>(hash.data()), sec->r == 2 ? 5 : n);
      std::vector<std::uint8_t> user(sec->o.begin(), sec->o.end());
      user.resize(32);
      if (sec->r == 2) {
        user = crypto::rc4(u8span(rc4_key), user);
      } else {
        for (int i = 19; i >= 0; --i) {
          std::string k = rc4_key;
          for (char& c : k) c = static_cast<char>(c ^ i);
          user = crypto::rc4(u8span(k), user);
        }
      }
      key = file_key(to_str(user));
      if (!check_user(key)) {
        return fail(Status::InvalidArgument,
                    password.empty() ? "the PDF is protected by a password; set the 'password' option"
                                     : "the password does not open this PDF");
      }
    }
    sec->key = key;
  }
  security_storage_ = sec;
  security_ = 1;
  return {};
}

std::string PdfFile::decrypt_string(const std::string& bytes, Ref ref, bool is_stream, const std::string& crypt_filter) {
  const Security& sec = *security_storage_;
  CryptMethod method = is_stream ? sec.stream_method : sec.string_method;
  if (!crypt_filter.empty()) {
    if (crypt_filter == "Identity") return bytes;
    const auto it = sec.filters.find(crypt_filter);
    if (it != sec.filters.end()) method = it->second;
  }
  if (method == CryptMethod::Identity) return bytes;
  std::string key;
  if (method == CryptMethod::AesV3) {
    key = sec.key;
  } else {
    std::string input = sec.key;
    input.push_back(static_cast<char>(ref.num & 0xFF));
    input.push_back(static_cast<char>((ref.num >> 8) & 0xFF));
    input.push_back(static_cast<char>((ref.num >> 16) & 0xFF));
    input.push_back(static_cast<char>(ref.gen & 0xFF));
    input.push_back(static_cast<char>((ref.gen >> 8) & 0xFF));
    if (method == CryptMethod::AesV2) input += "sAlT";
    const auto hash = crypto::md5(u8span(input));
    key.assign(reinterpret_cast<const char*>(hash.data()), std::min<std::size_t>(sec.key.size() + 5, 16));
  }
  if (method == CryptMethod::Rc4) return to_str(crypto::rc4(u8span(key), u8span(bytes)));
  return to_str(crypto::aes_cbc_decrypt_pdf(u8span(key), u8span(bytes)));
}

void PdfFile::decrypt_object(Object& object, Ref ref, int depth) {
  if (depth > 64) return;
  switch (object.type()) {
    case Object::Type::String: {
      const String* s = object.string_object();
      object = String{decrypt_string(s->bytes, ref, false, {}), s->hex};
      break;
    }
    case Object::Type::Array: {
      Array copy = object.as_array();
      for (Object& item : copy) decrypt_object(item, ref, depth + 1);
      object = Object(std::move(copy));
      break;
    }
    case Object::Type::Dict: {
      Dict copy;
      for (const auto& [key, value] : object.as_dict()) {
        Object v = value;
        decrypt_object(v, ref, depth + 1);
        copy.set(key, std::move(v));
      }
      object = Object(std::move(copy));
      break;
    }
    case Object::Type::Stream: {
      const Stream* s = object.as_stream();
      Stream copy;
      const Dict& dict = s->dict;
      const std::string type = dict.get("Type").as_name();
      for (const auto& [key, value] : dict) {
        Object v = value;
        decrypt_object(v, ref, depth + 1);
        copy.dict.set(key, std::move(v));
      }
      bool skip = type == "XRef" || (type == "Metadata" && !security_storage_->encrypt_metadata);
      std::string crypt_filter;
      const Object filter = dict.get("Filter");
      const auto is_crypt = [](const Object& f) { return f.is_name("Crypt"); };
      if (is_crypt(filter) || (filter.is_array() && !filter.as_array().empty() && is_crypt(filter.as_array()[0]))) {
        const Object parms = dict.get("DecodeParms");
        const Dict& p = parms.is_array() && !parms.as_array().empty() ? parms.as_array()[0].as_dict() : parms.as_dict();
        crypt_filter = p.get("Name").as_name();
        if (crypt_filter.empty()) crypt_filter = "Identity";
      }
      if (skip) {
        copy.data = s->data;
      } else {
        const std::string plain = decrypt_string(to_string(s->data), ref, true, crypt_filter);
        copy.data = to_bytes(plain);
      }
      object = Object(std::move(copy));
      break;
    }
    default: break;
  }
}

// --------------------------------------------------------------------------- loading

Result<void> PdfFile::load() {
  const std::string_view text = as_view(data_);
  std::size_t header = text.substr(0, 1024).find("%PDF-");
  if (header == std::string_view::npos && options_.allow_fdf) header = text.substr(0, 1024).find("%FDF-");
  if (header == std::string_view::npos) {
    if (text.find("%PDF-") == std::string_view::npos) return fail(Status::Decode, "not a PDF file (no %PDF header)");
  } else {
    header_offset_ = header;
    std::size_t v = header + 5;
    std::string version;
    while (v < text.size() && version.size() < 4 && (util::is_ascii_digit(text[v]) || text[v] == '.')) version.push_back(text[v++]);
    if (!version.empty()) version_ = version;
  }
  bool loaded = false;
  const std::size_t sx = rfind_text(data_, "startxref", 1 << 16);
  if (sx != std::string_view::npos) {
    Lexer lexer(data_, sx + 9);
    const Token offset = lexer.next();
    if (offset.kind == Token::Kind::Integer && offset.integer >= 0) {
      loaded = load_xref_chain(static_cast<std::size_t>(offset.integer));
    }
  }
  if (!loaded || !trailer_.contains("Root")) {
    if (sx == std::string_view::npos) add_warning("the PDF has no cross-reference table; it was rebuilt by scanning");
    else add_warning("the PDF cross-reference table is damaged; it was rebuilt by scanning");
    reconstruct();
    for (const auto& [num, entry] : recovered_) {
      if (!xref_.contains(num)) xref_.emplace(num, entry);
    }
    // Trailer dictionaries anywhere in the file (the last one wins).
    std::size_t pos = 0;
    while ((pos = text.find("trailer", pos)) != std::string_view::npos) {
      Lexer lexer(data_, pos + 7);
      bool ok = true;
      Object t = parse_object(lexer, 16, &ok);
      for (const auto& [key, value] : t.as_dict()) {
        if (key == "Root" || key == "Info" || key == "Encrypt" || key == "ID" || key == "Size") trailer_.set(key, value);
      }
      pos += 7;
    }
    if (!trailer_.contains("Root") || !resolve(trailer_.get("Root")).is_dict()) {
      // Find a catalog by type. Cross-reference streams carry trailer keys too.
      std::vector<std::uint32_t> numbers;
      for (const auto& [num, entry] : recovered_) numbers.push_back(num);
      std::sort(numbers.begin(), numbers.end());
      for (const std::uint32_t num : numbers) {
        const Object obj = get(Ref{num, recovered_[num].gen});
        const Dict& d = obj.as_dict();
        if (d.get("Type").is_name("XRef")) {
          for (const auto& [key, value] : d) {
            if ((key == "Root" || key == "Info" || key == "Encrypt" || key == "ID") && !trailer_.contains(key)) {
              trailer_.set(key, value);
            }
          }
        }
        if (d.get("Type").is_name("Catalog") && d.contains("Pages")) {
          trailer_.set("Root", Ref{num, recovered_[num].gen});
        }
      }
    }
    cache_.clear();
  }
  if (!trailer_.contains("Root")) return fail(Status::Decode, "the PDF has no document catalog");
  if (trailer_.contains("Encrypt")) {
    cache_.clear();
    CVT_TRY(setup_security(options_.password));
    cache_.clear();
    objstm_index_.clear();
  }
  load_pages();
  if (pages_.empty()) {
    // Last resort: collect page objects by type.
    ensure_reconstructed();
    std::vector<std::uint32_t> numbers;
    for (const auto& [num, entry] : recovered_) numbers.push_back(num);
    std::sort(numbers.begin(), numbers.end());
    for (const std::uint32_t num : numbers) {
      const Object obj = get(Ref{num, 0});
      if (obj.as_dict().get("Type").is_name("Page") && pages_.size() < options_.max_pages) {
        Page page;
        page.ref = Ref{num, 0};
        page.dict = obj.as_dict();
        page.resources = resolve(page.dict.get("Resources"));
        pages_.push_back(std::move(page));
      }
    }
    if (!pages_.empty()) add_warning("the page tree is damaged; pages were recovered by scanning");
  }
  return {};
}

namespace {

std::optional<Rect> rect_from(const Object& o, PdfFile& file) {
  const Array& a = file.resolve(o).as_array();
  if (a.size() < 4) return std::nullopt;
  Rect r{file.resolve(a[0]).as_number(), file.resolve(a[1]).as_number(), file.resolve(a[2]).as_number(),
         file.resolve(a[3]).as_number()};
  r = r.normalized();
  if (r.empty()) return std::nullopt;
  return r;
}

}  // namespace

void PdfFile::load_pages() {
  const Object catalog_obj = catalog();
  const Object root = resolve(catalog_obj.as_dict().get("Pages"));
  if (!root.is_dict()) return;
  struct Inherited {
    Object resources;
    std::optional<Rect> media;
    std::optional<Rect> crop;
    int rotate = 0;
  };
  std::set<std::uint32_t> visited;
  std::function<void(const Object&, const Object&, Inherited, int)> walk;
  walk = [&](const Object& node_ref, const Object& node, Inherited inherited, int depth) {
    if (depth > 64 || pages_.size() >= options_.max_pages) return;
    const Dict& dict = node.as_dict();
    if (dict.contains("Resources")) inherited.resources = resolve(dict.get("Resources"));
    if (auto m = rect_from(dict.get("MediaBox"), *this)) inherited.media = m;
    if (auto c = rect_from(dict.get("CropBox"), *this)) inherited.crop = c;
    if (dict.contains("Rotate")) inherited.rotate = static_cast<int>(resolve(dict.get("Rotate")).as_int(0));
    const Object kids = resolve(dict.get("Kids"));
    const bool is_page = dict.get("Type").is_name("Page") || (!kids.is_array() && !dict.get("Type").is_name("Pages"));
    if (is_page) {
      Page page;
      page.ref = node_ref.as_ref();
      page.dict = dict;
      page.resources = inherited.resources;
      page.media_box = inherited.media.value_or(Rect{0, 0, 612, 792});
      page.crop_box = inherited.crop ? inherited.crop->intersect(page.media_box) : page.media_box;
      if (page.crop_box.empty()) page.crop_box = page.media_box;
      int rotate = ((inherited.rotate % 360) + 360) % 360;
      page.rotate = (rotate / 90) * 90;
      page.user_unit = std::clamp(resolve(dict.get("UserUnit")).as_number(1.0), 0.01, 100.0);
      pages_.push_back(std::move(page));
      return;
    }
    for (const Object& kid : kids.as_array()) {
      if (kid.is_ref()) {
        if (visited.contains(kid.as_ref().num)) continue;
        visited.insert(kid.as_ref().num);
      }
      const Object child = resolve(kid);
      if (child.is_dict()) walk(kid, child, inherited, depth + 1);
    }
  };
  const Object root_ref = catalog_obj.as_dict().get("Pages");
  if (root_ref.is_ref()) visited.insert(root_ref.as_ref().num);
  walk(root_ref, root, Inherited{}, 0);
}

std::string pdf_date_to_iso(std::string_view text) {
  if (text.starts_with("D:")) text.remove_prefix(2);
  std::string digits;
  std::size_t i = 0;
  while (i < text.size() && util::is_ascii_digit(text[i]) && digits.size() < 14) digits.push_back(text[i++]);
  if (digits.size() < 4) return {};
  std::string out = digits.substr(0, 4);
  if (digits.size() >= 6) out += "-" + digits.substr(4, 2);
  if (digits.size() >= 8) out += "-" + digits.substr(6, 2);
  if (digits.size() >= 12) {
    out += "T" + digits.substr(8, 2) + ":" + digits.substr(10, 2);
    out += ":" + (digits.size() >= 14 ? digits.substr(12, 2) : std::string("00"));
    if (i < text.size()) {
      const char tz = text[i];
      if (tz == 'Z') {
        out += "Z";
      } else if (tz == '+' || tz == '-') {
        std::string h, m;
        std::size_t j = i + 1;
        while (j < text.size() && util::is_ascii_digit(text[j]) && h.size() < 2) h.push_back(text[j++]);
        while (j < text.size() && !util::is_ascii_digit(text[j])) ++j;
        while (j < text.size() && util::is_ascii_digit(text[j]) && m.size() < 2) m.push_back(text[j++]);
        if (h.size() == 2) out += std::string(1, tz) + h + ":" + (m.size() == 2 ? m : std::string("00"));
      }
    }
  }
  return out;
}

}  // namespace cvt::pdf
