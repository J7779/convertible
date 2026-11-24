// Convertible core - science family: FITS (Flexible Image Transport System).
// Primary HDU plus IMAGE, ASCII TABLE and BINTABLE extensions.
//
//   Data   every HDU as one array (images: physical values after BSCALE/BZERO,
//          C order [NAXISn..NAXIS1]; tables: record arrays), header cards as
//          the array's attributes.
//   Table  the same through the array model (tables keep their columns).
//   Image  the selected image HDU rendered with an intensity stretch; cubes
//          give one frame per plane.
// Writers: Image -> 8-bit or 16-bit FITS image (RGB as a 3-plane cube),
// Data/Table -> primary image + IMAGE/BINTABLE extensions.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

#include "util/bytes.hpp"
#include "engine/standard_options.hpp"
#include "families/science/arrays.hpp"
#include "families/science/imaging.hpp"
#include "families/science/science.hpp"
#include "util/text.hpp"

namespace cvt::science::arrays {

namespace {

constexpr std::size_t block = 2880;

struct Hdu {
  std::string type;  // PRIMARY, IMAGE, TABLE, BINTABLE
  Json header = Json::object();
  Array array;
  bool has_data = false;
};

Json card_value(std::string_view text) {
  text = util::trim(text);
  if (text.starts_with('\'')) {
    std::string out;
    for (std::size_t i = 1; i < text.size(); ++i) {
      if (text[i] == '\'') {
        if (i + 1 < text.size() && text[i + 1] == '\'') {
          out.push_back('\'');
          ++i;
          continue;
        }
        break;
      }
      out.push_back(text[i]);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return Json(out);
  }
  // Strip the comment.
  if (const std::size_t slash = text.find('/'); slash != std::string_view::npos) text = util::trim(text.substr(0, slash));
  if (text == "T") return Json(true);
  if (text == "F") return Json(false);
  if (text.empty()) return Json();
  if (const auto i = util::parse_int(text)) return Json(*i);
  std::string normalized(text);
  std::replace(normalized.begin(), normalized.end(), 'D', 'E');  // Fortran exponent
  if (const auto d = to_double(normalized)) return json_number(*d);
  return Json(std::string(text));
}

Result<Json> parse_header(util::ByteReader& reader) {
  Json header = Json::object();
  while (true) {
    if (reader.remaining() < block) return fail(Status::Decode, "FITS header is truncated (no END card)");
    const std::string_view chunk = reader.chars(block);
    for (std::size_t at = 0; at < block; at += 80) {
      const std::string_view card = chunk.substr(at, 80);
      const std::string key(util::trim(card.substr(0, 8)));
      if (key == "END") {
        return header;
      }
      if (key.empty()) continue;
      if (key == "COMMENT" || key == "HISTORY") {
        Json& list = header[key];
        if (!list.is_array()) list = Json::array();
        list.push_back(util::sanitize_utf8(util::trim_right(card.substr(8))));
        continue;
      }
      if (key == "CONTINUE") {
        // Long-string continuation of the previous string value.
        auto items = header.items();
        if (!items.empty() && items.back().value().is_string()) {
          std::string prev = items.back().value().as_string();
          if (prev.ends_with('&')) prev.pop_back();
          prev += card_value(card.substr(8)).as_string();
          items.back().value() = prev;
        }
        continue;
      }
      if (card.size() >= 10 && card.substr(8, 2) == "= ") {
        header[util::sanitize_utf8(key)] = card_value(card.substr(10));
      }
    }
    if (header.size() > 100000) return fail(Status::Limit, "FITS header has too many cards");
  }
}

std::int64_t hint(const Json& header, std::string_view key, std::int64_t fallback = 0) {
  const Json* v = header.find(key);
  return v == nullptr ? fallback : v->as_int64(fallback);
}

double hdouble(const Json& header, std::string_view key, double fallback) {
  const Json* v = header.find(key);
  return v == nullptr || !v->is_number() ? fallback : v->as_double(fallback);
}

std::optional<DType> bitpix_dtype(std::int64_t bitpix) {
  switch (bitpix) {
    case 8: return DType{ScalarKind::UInt, 1};
    case 16: return DType{ScalarKind::Int, 2};
    case 32: return DType{ScalarKind::Int, 4};
    case 64: return DType{ScalarKind::Int, 8};
    case -32: return DType{ScalarKind::Float, 4};
    case -64: return DType{ScalarKind::Float, 8};
    default: return std::nullopt;
  }
}

/// Image data: raw big-endian -> physical values.
Result<Array> image_array(const Json& header, ByteView raw, const Context& ctx) {
  const auto dtype = bitpix_dtype(hint(header, "BITPIX"));
  if (!dtype) return fail(Status::Decode, "bad FITS BITPIX {}", hint(header, "BITPIX"));
  const std::int64_t naxis = hint(header, "NAXIS");
  Array a;
  a.dtype = *dtype;
  for (std::int64_t i = naxis; i >= 1; --i) {
    const std::int64_t n = hint(header, "NAXIS" + std::to_string(i), -1);
    if (n < 0) return fail(Status::Decode, "missing FITS NAXIS{}", i);
    a.shape.push_back(static_cast<std::uint64_t>(n));
  }
  CVT_TRY_ASSIGN(const std::uint64_t n, shape_count(a.shape));
  CVT_TRY(check_array_bytes(ctx, n, 8));
  if (raw.size() < n * dtype->size) return fail(Status::Decode, "FITS image data is truncated");
  a.data.assign(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(n * dtype->size));
  swap_elements(a);
  const double bscale = hdouble(header, "BSCALE", 1.0);
  const double bzero = hdouble(header, "BZERO", 0.0);
  const Json* blank = header.find("BLANK");
  if (bscale == 1.0 && bzero == 0.0 && blank == nullptr) return a;
  // Unsigned integers stored with the conventional offset.
  if (bscale == 1.0 && blank == nullptr && dtype->kind == ScalarKind::Int &&
      bzero == std::ldexp(1.0, static_cast<int>(dtype->size * 8 - 1))) {
    for (std::uint64_t i = 0; i < n; ++i) {
      std::byte* p = a.data.data() + i * dtype->size;
      p[dtype->size - 1] ^= std::byte{0x80};  // flip the sign bit (little-endian now)
    }
    a.dtype.kind = ScalarKind::UInt;
    return a;
  }
  if (bscale == 1.0 && bzero == -128.0 && dtype->kind == ScalarKind::UInt && dtype->size == 1 && blank == nullptr) {
    for (std::uint64_t i = 0; i < n; ++i) a.data[static_cast<std::size_t>(i)] ^= std::byte{0x80};
    a.dtype.kind = ScalarKind::Int;
    return a;
  }
  Array physical;
  physical.shape = a.shape;
  physical.dtype = DType{ScalarKind::Float, 8};
  physical.data.assign(static_cast<std::size_t>(n * 8), std::byte{0});
  const std::int64_t blank_value = blank != nullptr ? blank->as_int64() : 0;
  for (std::uint64_t i = 0; i < n; ++i) {
    const double raw_value = a.number(i);
    double v = bzero + bscale * raw_value;
    if (blank != nullptr && dtype->kind != ScalarKind::Float && raw_value == static_cast<double>(blank_value)) v = NAN;
    store_double(physical.dtype, physical.data.data() + i * 8, v);
  }
  return physical;
}

struct ColumnFormat {
  std::uint64_t repeat = 1;
  char code = 'E';
  std::uint32_t width = 0;  // ASCII tables
};

ColumnFormat parse_tform(std::string_view text, bool ascii) {
  ColumnFormat f;
  text = util::trim(text);
  std::size_t i = 0;
  while (i < text.size() && util::is_ascii_digit(text[i])) ++i;
  if (ascii) {
    f.code = text.empty() ? 'A' : util::ascii_upper(text[0]);
    const std::string_view rest = text.substr(std::min<std::size_t>(1, text.size()));
    f.width = static_cast<std::uint32_t>(to_int(rest.substr(0, rest.find('.'))).value_or(0));
    return f;
  }
  f.repeat = i == 0 ? 1 : static_cast<std::uint64_t>(to_int(text.substr(0, i)).value_or(1));
  f.code = i < text.size() ? util::ascii_upper(text[i]) : 'E';
  return f;
}

std::optional<DType> bintable_dtype(char code) {
  switch (code) {
    case 'L': return DType{ScalarKind::Bool, 1};
    case 'B': return DType{ScalarKind::UInt, 1};
    case 'I': return DType{ScalarKind::Int, 2};
    case 'J': return DType{ScalarKind::Int, 4};
    case 'K': return DType{ScalarKind::Int, 8};
    case 'E': return DType{ScalarKind::Float, 4};
    case 'D': return DType{ScalarKind::Float, 8};
    case 'C': return DType{ScalarKind::Complex, 8};
    case 'M': return DType{ScalarKind::Complex, 16};
    default: return std::nullopt;
  }
}

/// Binary or ASCII table -> record array (vector columns become name_0, name_1...).
Result<Array> table_array(const Json& header, ByteView raw, bool ascii, const Context& ctx) {
  const std::uint64_t row_bytes = static_cast<std::uint64_t>(std::max<std::int64_t>(0, hint(header, "NAXIS1")));
  const std::uint64_t rows = static_cast<std::uint64_t>(std::max<std::int64_t>(0, hint(header, "NAXIS2")));
  const std::int64_t fields = hint(header, "TFIELDS");
  if (fields < 0 || fields > 999) return fail(Status::Decode, "bad FITS TFIELDS");
  if (row_bytes != 0 && rows > raw.size() / row_bytes) return fail(Status::Decode, "FITS table data is truncated");
  const std::uint64_t heap_start = static_cast<std::uint64_t>(hint(header, "THEAP", static_cast<std::int64_t>(row_bytes * rows)));
  struct Column {
    std::string name;
    ColumnFormat format;
    std::uint64_t offset = 0;
    DType out;
    double scale = 1, zero = 0;
  };
  std::vector<Column> columns;
  std::uint64_t at = 0;
  for (std::int64_t c = 1; c <= fields; ++c) {
    const std::string n = std::to_string(c);
    Column col;
    col.name = header["TTYPE" + n].as_string();
    if (col.name.empty()) col.name = "col" + n;
    col.format = parse_tform(header["TFORM" + n].as_string(), ascii);
    col.scale = hdouble(header, "TSCAL" + n, 1.0);
    col.zero = hdouble(header, "TZERO" + n, 0.0);
    if (ascii) {
      col.offset = static_cast<std::uint64_t>(std::max<std::int64_t>(1, hint(header, "TBCOL" + n, 1)) - 1);
      const char code = col.format.code;
      col.out = code == 'A' ? DType{ScalarKind::Bytes, std::max<std::uint32_t>(1, col.format.width)}
                : code == 'I' ? DType{ScalarKind::Int, 8}
                              : DType{ScalarKind::Float, 8};
    } else {
      col.offset = at;
      const char code = col.format.code;
      std::uint64_t width = 0;
      if (code == 'A') {
        width = col.format.repeat;
        col.out = DType{ScalarKind::Bytes, static_cast<std::uint32_t>(std::max<std::uint64_t>(1, width))};
      } else if (code == 'X') {
        width = (col.format.repeat + 7) / 8;
        col.out = DType{ScalarKind::UInt, static_cast<std::uint32_t>(std::min<std::uint64_t>(8, std::max<std::uint64_t>(1, width)))};
      } else if (code == 'P' || code == 'Q') {
        width = code == 'P' ? 8 : 16;
        col.out = DType{ScalarKind::Unicode, 4 * 256};  // rendered as text
      } else if (const auto t = bintable_dtype(code)) {
        width = col.format.repeat * t->size;
        col.out = *t;
        if ((col.scale != 1.0 || col.zero != 0.0) && t->kind == ScalarKind::Int) {
          if (col.scale == 1.0 && col.zero == std::ldexp(1.0, static_cast<int>(t->size * 8 - 1))) {
            col.out = DType{ScalarKind::UInt, t->size};
          } else {
            col.out = DType{ScalarKind::Float, 8};
          }
        }
      } else {
        return fail(Status::Decode, "unsupported FITS column format '{}'", header["TFORM" + n].as_string());
      }
      at += width;
    }
    columns.push_back(std::move(col));
  }
  // Expand vector columns into one field per element.
  Array a;
  a.shape = {rows};
  std::uint32_t offset = 0;
  struct Plan {
    std::size_t column;
    std::uint64_t element;
    Field field;
  };
  std::vector<Plan> plans;
  for (std::size_t c = 0; c < columns.size(); ++c) {
    const Column& col = columns[c];
    const bool vector = !ascii && col.format.repeat > 1 && col.format.code != 'A' && col.format.code != 'X';
    const std::uint64_t count = vector ? std::min<std::uint64_t>(col.format.repeat, 4096) : 1;
    for (std::uint64_t e = 0; e < count; ++e) {
      Field f{vector ? col.name + "_" + std::to_string(e) : col.name, col.out, offset};
      offset += col.out.size;
      plans.push_back({c, e, f});
      a.fields.push_back(f);
    }
  }
  a.dtype = DType{ScalarKind::Record, offset};
  CVT_TRY(check_array_bytes(ctx, rows, offset));
  CVT_TRY(check_cells(ctx, rows * plans.size()));
  a.data.assign(static_cast<std::size_t>(rows * offset), std::byte{0});
  for (std::uint64_t r = 0; r < rows; ++r) {
    if ((r & 1023) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(r) / static_cast<double>(rows)));
    const std::byte* row = raw.data() + r * row_bytes;
    std::byte* out = a.data.data() + r * offset;
    for (const Plan& plan : plans) {
      const Column& col = columns[plan.column];
      std::byte* dst = out + plan.field.offset;
      if (ascii) {
        if (col.offset + col.format.width > row_bytes) return fail(Status::Decode, "FITS ASCII column outside the row");
        const std::string_view text = as_chars(ByteView(row + col.offset, col.format.width));
        if (col.out.kind == ScalarKind::Bytes) {
          const std::string_view trimmed = util::trim_right(text);
          ::cvt::util::copy_bytes(dst, trimmed.data(), std::min<std::size_t>(trimmed.size(), col.out.size));
        } else {
          std::string normalized(util::trim(text));
          std::replace(normalized.begin(), normalized.end(), 'D', 'E');
          const double v = to_double(normalized).value_or(NAN);
          store_double(col.out, dst, col.zero + col.scale * v);
        }
        continue;
      }
      const char code = col.format.code;
      if (code == 'A') {
        const std::uint64_t w = col.format.repeat;
        if (col.offset + w > row_bytes) return fail(Status::Decode, "FITS column outside the row");
        ::cvt::util::copy_bytes(dst, row + col.offset, static_cast<std::size_t>(std::min<std::uint64_t>(w, col.out.size)));
        continue;
      }
      if (code == 'X') {
        const std::uint64_t w = (col.format.repeat + 7) / 8;
        if (col.offset + w > row_bytes) return fail(Status::Decode, "FITS column outside the row");
        std::uint64_t bits = 0;
        for (std::uint64_t k = 0; k < std::min<std::uint64_t>(w, 8); ++k) bits = (bits << 8) | static_cast<std::uint8_t>(row[col.offset + k]);
        store_double(col.out, dst, static_cast<double>(bits));
        continue;
      }
      if (code == 'P' || code == 'Q') {
        util::ByteReader desc(ByteView(row + col.offset, code == 'P' ? 8 : 16));
        const std::uint64_t n = code == 'P' ? desc.u32be() : desc.u64be();
        const std::uint64_t off = code == 'P' ? desc.u32be() : desc.u64be();
        // Element type follows the P/Q letter.
        const std::string tform = header["TFORM" + std::to_string(plan.column + 1)].as_string();
        const std::size_t letter = tform.find_first_of("PQpq");
        const char inner = letter + 1 < tform.size() ? util::ascii_upper(tform[letter + 1]) : 'B';
        std::string text;
        const std::uint64_t base = heap_start + off;
        if (inner == 'A') {
          if (base > raw.size() || n > raw.size() - base) return fail(Status::Decode, "FITS heap reference out of range");
          text = util::sanitize_utf8(as_chars(raw.subspan(static_cast<std::size_t>(base), static_cast<std::size_t>(n))));
        } else if (const auto t = bintable_dtype(inner)) {
          if (base > raw.size() || n > (raw.size() - base) / t->size) return fail(Status::Decode, "FITS heap reference out of range");
          Array tmp;
          tmp.dtype = *t;
          tmp.shape = {n};
          tmp.data.assign(raw.begin() + static_cast<std::ptrdiff_t>(base), raw.begin() + static_cast<std::ptrdiff_t>(base + n * t->size));
          swap_elements(tmp);
          for (std::uint64_t k = 0; k < std::min<std::uint64_t>(n, 64); ++k) text += (k ? " " : "") + fmt_num(tmp.number(k));
          if (n > 64) text += " ...";
        }
        const std::u32string u = util::utf8_to_utf32(text);
        for (std::size_t k = 0; k < u.size() && k < 256; ++k) {
          for (int b = 0; b < 4; ++b) dst[k * 4 + static_cast<std::size_t>(b)] = static_cast<std::byte>((u[k] >> (8 * b)) & 0xFF);
        }
        continue;
      }
      const DType raw_type = *bintable_dtype(code);
      const std::uint64_t src = col.offset + plan.element * raw_type.size;
      if (src + raw_type.size > row_bytes) return fail(Status::Decode, "FITS column outside the row");
      Array tmp;
      tmp.dtype = raw_type;
      tmp.shape = {1};
      tmp.data.assign(row + src, row + src + raw_type.size);
      swap_elements(tmp);
      if (raw_type.kind == ScalarKind::Bool) {
        dst[0] = static_cast<char>(tmp.data[0]) == 'T' ? std::byte{1} : std::byte{0};
      } else if (col.out == raw_type) {
        ::cvt::util::copy_bytes(dst, tmp.data.data(), raw_type.size);
      } else if (col.out.kind == ScalarKind::UInt && raw_type.kind == ScalarKind::Int) {
        ::cvt::util::copy_bytes(dst, tmp.data.data(), raw_type.size);
        dst[raw_type.size - 1] ^= std::byte{0x80};
      } else {
        store_double(col.out, dst, col.zero + col.scale * tmp.number(0));
      }
    }
  }
  return a;
}

Result<std::vector<Hdu>> read_hdus(ByteView input, const Context& ctx) {
  if (input.size() < block || !as_chars(input.first(9)).starts_with("SIMPLE  =")) {
    return fail(Status::Decode, "not a FITS file");
  }
  util::ByteReader reader(input);
  std::vector<Hdu> hdus;
  while (reader.remaining() >= block) {
    if (hdus.size() >= 4096) return fail(Status::Limit, "too many FITS extensions");
    const std::string_view first = as_chars(reader.peek_rest().first(80));
    if (!hdus.empty() && !first.starts_with("XTENSION")) break;  // trailing special records
    CVT_TRY_ASSIGN(Json header, parse_header(reader));
    Hdu hdu;
    hdu.type = hdus.empty() ? "PRIMARY" : header["XTENSION"].as_string();
    const std::int64_t bitpix = hint(header, "BITPIX");
    const std::int64_t naxis = hint(header, "NAXIS");
    if (naxis < 0 || naxis > 999) return fail(Status::Decode, "bad FITS NAXIS");
    std::uint64_t elements = naxis == 0 ? 0 : 1;
    for (std::int64_t i = 1; i <= naxis; ++i) {
      const std::int64_t n = hint(header, "NAXIS" + std::to_string(i), -1);
      if (n < 0) return fail(Status::Decode, "bad FITS NAXIS{}", i);
      if (n != 0 && elements > (std::uint64_t{1} << 50) / static_cast<std::uint64_t>(n)) return fail(Status::Limit, "FITS data is too large");
      elements *= static_cast<std::uint64_t>(n);
    }
    const std::int64_t pcount = hint(header, "PCOUNT", 0);
    const std::int64_t gcount = hint(header, "GCOUNT", 1);
    if (pcount < 0 || gcount < 0 || gcount > 1'000'000) return fail(Status::Decode, "bad FITS PCOUNT/GCOUNT");
    const std::uint64_t bytes = static_cast<std::uint64_t>(std::abs(bitpix)) / 8 * static_cast<std::uint64_t>(gcount) *
                                (static_cast<std::uint64_t>(pcount) + elements);
    if (bytes > reader.remaining()) return fail(Status::Decode, "FITS data unit is truncated");
    const ByteView raw = reader.bytes(static_cast<std::size_t>(bytes));
    (void)reader.bytes(std::min<std::size_t>(reader.remaining(), (block - bytes % block) % block));
    if (header["GROUPS"].as_bool(false)) return fail(Status::Decode, "random-groups FITS files are not supported");
    if (elements > 0) {
      if (hdu.type == "PRIMARY" || hdu.type == "IMAGE") {
        CVT_TRY_ASSIGN(hdu.array, image_array(header, raw, ctx));
      } else if (hdu.type == "BINTABLE" || hdu.type == "TABLE" || hdu.type == "A3DTABLE") {
        CVT_TRY_ASSIGN(hdu.array, table_array(header, raw, hdu.type == "TABLE", ctx));
      } else {
        ctx.warn("FITS extension '" + hdu.type + "' was skipped.");
      }
      hdu.has_data = !hdu.array.data.empty() || hdu.array.dtype.kind == ScalarKind::Record;
    }
    std::string name = header["EXTNAME"].as_string();
    if (name.empty()) name = hdus.empty() ? "PRIMARY" : "HDU" + std::to_string(hdus.size());
    hdu.array.name = name;
    hdu.header = std::move(header);
    hdus.push_back(std::move(hdu));
    CVT_TRY(ctx.checkpoint(static_cast<double>(reader.position()) / static_cast<double>(reader.size())));
  }
  if (hdus.empty()) return fail(Status::Decode, "no FITS HDUs");
  return hdus;
}

Result<Collection> read_fits_collection(ReadContext& ctx) {
  CVT_TRY_ASSIGN(auto hdus, read_hdus(ctx.input, ctx));
  Collection c;
  for (std::size_t i = 0; i < hdus.size(); ++i) {
    Hdu& hdu = hdus[i];
    if (!hdu.has_data) {
      if (i == 0) c.attributes = hdu.header;  // header-only primary
      continue;
    }
    hdu.array.attributes = std::move(hdu.header);
    c.arrays.push_back(std::move(hdu.array));
  }
  if (c.arrays.empty()) {
    c.arrays.push_back(Array{"PRIMARY", DType{ScalarKind::Float, 8}, {}, {0}, {}, c.attributes});
  }
  return c;
}

Result<AnyIR> read_fits_image(ReadContext& ctx) {
  CVT_TRY_ASSIGN(auto hdus, read_hdus(ctx.input, ctx));
  const std::int64_t wanted = ctx.options.get_int("fits_hdu", -1);
  const Hdu* chosen = nullptr;
  for (std::size_t i = 0; i < hdus.size(); ++i) {
    const Hdu& h = hdus[i];
    const bool image = h.has_data && (h.type == "PRIMARY" || h.type == "IMAGE") && h.array.shape.size() >= 2;
    if (wanted >= 0 && static_cast<std::size_t>(wanted) == i) {
      if (!image) return fail(Status::Decode, "FITS HDU {} is not an image", i);
      chosen = &h;
      break;
    }
    if (wanted < 0 && image && chosen == nullptr) chosen = &h;
  }
  if (chosen == nullptr) return fail(Status::Decode, "the FITS file has no 2-D image");
  imaging::Window window = imaging::window_from_options(ctx.options);
  CVT_TRY_ASSIGN(ir::Image image, imaging::array_to_image(chosen->array, window, ctx.options.get_bool("flip_vertical", true), ctx, true));
  if (!options::resolved_strip_metadata(ctx.options)) {
    for (const auto& [key, value] : chosen->header.items()) {
      if (!value.is_structured()) image.metadata["FITS:" + key] = value.is_string() ? value.as_string() : util::dump_json(value);
    }
  }
  return image;
}

// --------------------------------------------------------------------------- writing

std::string card(std::string_view key, const Json& value, std::string_view comment = {}) {
  std::string out(key.substr(0, 8));
  out.resize(8, ' ');
  std::string text;
  if (value.is_bool()) {
    text = std::string(19, ' ') + (value.as_bool() ? "T" : "F");
  } else if (value.is_integer()) {
    const std::string n = value.is_unsigned() ? std::to_string(value.as_uint64()) : std::to_string(value.as_int64());
    text = std::string(n.size() < 20 ? 20 - n.size() : 0, ' ') + n;
  } else if (value.is_number()) {
    std::string n = fmt_num(value.as_double(), 15);
    if (n.find_first_of(".eEn") == std::string::npos) n += ".";
    for (char& ch : n) ch = util::ascii_upper(ch);
    text = std::string(n.size() < 20 ? 20 - n.size() : 0, ' ') + n;
  } else {
    std::string s = util::replace_all(value.is_string() ? value.as_string() : util::dump_json(value), "'", "''");
    for (char& ch : s) {
      if (static_cast<unsigned char>(ch) < 32 || static_cast<unsigned char>(ch) > 126) ch = '?';
    }
    s = s.substr(0, 66);
    if (s.size() < 8) s.resize(8, ' ');
    text = "'" + s + "'";
  }
  out += "= " + text;
  if (!comment.empty() && out.size() + 3 + comment.size() <= 80) out += " / " + std::string(comment);
  out.resize(80, ' ');
  return out.substr(0, 80);
}

std::string commentary(std::string_view key, std::string_view text) {
  std::string out(key);
  out.resize(8, ' ');
  std::string body(text.substr(0, 72));
  for (char& ch : body) {
    if (static_cast<unsigned char>(ch) < 32 || static_cast<unsigned char>(ch) > 126) ch = '?';
  }
  out += body;
  out.resize(80, ' ');
  return out;
}

bool structural(std::string_view key) {
  static constexpr std::string_view fixed[] = {"SIMPLE", "BITPIX", "NAXIS", "EXTEND", "XTENSION", "PCOUNT", "GCOUNT",
                                               "TFIELDS", "BSCALE", "BZERO", "BLANK", "END", "THEAP", "GROUPS"};
  for (const auto f : fixed) {
    if (key == f) return true;
  }
  for (const std::string_view prefix : {"NAXIS", "TTYPE", "TFORM", "TBCOL", "TSCAL", "TZERO", "TDIM", "TNULL"}) {
    if (key.starts_with(prefix) && key.size() > prefix.size() && util::is_ascii_digit(key[prefix.size()])) return true;
  }
  return false;
}

bool identifying(std::string_view key) {
  for (const std::string_view k : {"OBSERVER", "AUTHOR", "ORIGIN", "DATE", "CREATOR", "PROGRAM", "USER", "USERNAME",
                                   "HOSTNAME", "COMPUTER", "FILENAME", "IRAF-TLM", "REFERENC", "PROPOSER", "PI_NAME"}) {
    if (key == k) return true;
  }
  return false;
}

void append_header(std::string& out, const Json& extra, bool strip) {
  for (const auto& [key, value] : extra.items()) {
    if (key.empty() || key.starts_with('_') || structural(key) || (strip && identifying(key))) continue;
    if (key == "COMMENT" || key == "HISTORY") {
      if (strip && key == "HISTORY") continue;
      for (const Json& line : value.elements()) out += commentary(key, line.as_string());
      continue;
    }
    if (value.is_null() || value.is_structured()) continue;
    std::string k = util::to_upper(key);
    if (k.size() > 8) continue;
    out += card(k, value);
  }
}

void finish_block(std::string& text, char fill) {
  text.append((block - text.size() % block) % block, fill);
}

Result<void> append_image_hdu(Bytes& out, const Array& source, bool primary, const Json& extra, bool strip, bool more) {
  Array a = source;
  if (a.dtype.kind == ScalarKind::Complex) return fail(Status::Encode, "FITS images cannot hold complex numbers");
  std::int64_t bitpix = 0;
  double bzero = 0;
  DType stored = a.dtype;
  switch (a.dtype.kind) {
    case ScalarKind::Bool: bitpix = 8; stored = {ScalarKind::UInt, 1}; break;
    case ScalarKind::UInt:
      if (a.dtype.size == 1) {
        bitpix = 8;
      } else {
        bitpix = a.dtype.size * 8;
        bzero = std::ldexp(1.0, static_cast<int>(a.dtype.size * 8 - 1));
        stored = {ScalarKind::Int, a.dtype.size};
      }
      break;
    case ScalarKind::Int:
      if (a.dtype.size == 1) {
        bitpix = 8;
        bzero = -128;
        stored = {ScalarKind::UInt, 1};
      } else {
        bitpix = a.dtype.size * 8;
      }
      break;
    case ScalarKind::Float: bitpix = a.dtype.size == 8 ? -64 : -32; stored = {ScalarKind::Float, a.dtype.size == 8 ? 8u : 4u}; break;
    default: return fail(Status::Encode, "'{}' cannot be stored as a FITS image", a.name);
  }
  // Convert to the stored representation (sign-bit flips for offset types).
  Array s;
  s.dtype = stored;
  s.shape = a.shape;
  s.data.assign(static_cast<std::size_t>(a.count() * stored.size), std::byte{0});
  for (std::uint64_t i = 0; i < a.count(); ++i) {
    std::byte* p = s.data.data() + i * stored.size;
    if (a.dtype.kind == ScalarKind::UInt && a.dtype.size > 1) {
      ::cvt::util::copy_bytes(p, a.element(i), stored.size);
      p[stored.size - 1] ^= std::byte{0x80};
    } else if (a.dtype.kind == ScalarKind::Int && a.dtype.size == 1) {
      p[0] = a.element(i)[0] ^ std::byte{0x80};
    } else if (stored == a.dtype) {
      ::cvt::util::copy_bytes(p, a.element(i), stored.size);
    } else {
      store_double(stored, p, a.number(i));
    }
  }
  swap_elements(s);  // to big-endian
  std::string header;
  header += primary ? card("SIMPLE", Json(true), "conforms to FITS standard") : card("XTENSION", Json("IMAGE"), "image extension");
  header += card("BITPIX", Json(bitpix));
  header += card("NAXIS", Json(static_cast<std::int64_t>(a.shape.size())));
  for (std::size_t d = 0; d < a.shape.size(); ++d) {
    header += card("NAXIS" + std::to_string(d + 1), Json(a.shape[a.shape.size() - 1 - d]));
  }
  if (primary) {
    if (more) header += card("EXTEND", Json(true));
  } else {
    header += card("PCOUNT", Json(0)) + card("GCOUNT", Json(1));
  }
  if (bzero != 0) header += card("BZERO", Json(static_cast<std::int64_t>(bzero))) + card("BSCALE", Json(1));
  if (!primary && !a.name.empty()) header += card("EXTNAME", Json(a.name));
  Json filtered = extra;
  filtered.erase("EXTNAME");
  append_header(header, filtered, strip);
  header += commentary("END", "");
  finish_block(header, ' ');
  append(out, as_bytes(header));
  append(out, ByteView(s.data));
  out.resize(out.size() + (block - s.data.size() % block) % block, std::byte{0});
  return {};
}

Result<void> append_table_hdu(Bytes& out, const Array& a, const Json& extra, bool strip) {
  std::vector<Field> fields = a.fields;
  if (a.dtype.kind != ScalarKind::Record) fields = {Field{a.name.empty() ? "value" : a.name, a.dtype, 0}};
  std::string header = card("XTENSION", Json("BINTABLE"), "binary table extension") + card("BITPIX", Json(8)) +
                       card("NAXIS", Json(2));
  std::uint64_t row = 0;
  std::vector<std::pair<char, std::uint32_t>> formats;
  for (const Field& f : fields) {
    char code = 'D';
    std::uint32_t width = 8;
    switch (f.type.kind) {
      case ScalarKind::Bool: code = 'L'; width = 1; break;
      case ScalarKind::UInt: code = f.type.size == 1 ? 'B' : 'K'; width = f.type.size == 1 ? 1 : 8; break;
      case ScalarKind::Int: code = f.type.size == 2 ? 'I' : f.type.size == 4 ? 'J' : 'K'; width = f.type.size <= 2 ? 2 : f.type.size; break;
      case ScalarKind::Float: code = f.type.size == 4 ? 'E' : 'D'; width = f.type.size == 4 ? 4 : 8; break;
      case ScalarKind::Complex: code = f.type.size == 8 ? 'C' : 'M'; width = f.type.size; break;
      case ScalarKind::Bytes: code = 'A'; width = f.type.size; break;
      case ScalarKind::Unicode: code = 'A'; width = f.type.size; break;  // UTF-8 bytes, up to 4 per character
      default: return fail(Status::Encode, "nested records cannot be stored in FITS");
    }
    if (f.type.kind == ScalarKind::Int && f.type.size == 1) {
      code = 'I';
      width = 2;
    }
    formats.emplace_back(code, width);
    row += width;
  }
  header += card("NAXIS1", Json(row)) + card("NAXIS2", Json(a.count())) + card("PCOUNT", Json(0)) +
            card("GCOUNT", Json(1)) + card("TFIELDS", Json(static_cast<std::uint64_t>(fields.size())));
  for (std::size_t i = 0; i < fields.size(); ++i) {
    const std::string n = std::to_string(i + 1);
    header += card("TTYPE" + n, Json(fields[i].name));
    const auto [code, width] = formats[i];
    header += card("TFORM" + n, Json(code == 'A' ? std::to_string(width) + "A" : std::string(1, code)));
  }
  if (!a.name.empty()) header += card("EXTNAME", Json(a.name));
  Json filtered = extra;
  filtered.erase("EXTNAME");
  append_header(header, filtered, strip);
  header += commentary("END", "");
  finish_block(header, ' ');
  append(out, as_bytes(header));
  util::ByteWriter data(static_cast<std::size_t>(row * a.count()));
  for (std::uint64_t r = 0; r < a.count(); ++r) {
    for (std::size_t i = 0; i < fields.size(); ++i) {
      const Field& f = fields[i];
      const std::byte* p = a.element(r) + (a.dtype.kind == ScalarKind::Record ? f.offset : 0);
      const auto [code, width] = formats[i];
      if (code == 'A') {
        std::string s = scalar_json(f.type, p).as_string();
        s.resize(width, '\0');
        data.text(s);
      } else if (code == 'L') {
        data.u8(p[0] != std::byte{0} ? 'T' : 'F');
      } else if (code == 'B') {
        data.u8(static_cast<std::uint8_t>(p[0]));
      } else if (code == 'C' || code == 'M') {
        Array tmp;
        tmp.dtype = f.type;
        tmp.shape = {1};
        tmp.data.assign(p, p + f.type.size);
        swap_elements(tmp);
        data.bytes(tmp.data);
      } else {
        const DType t = code == 'I' ? DType{ScalarKind::Int, 2} : code == 'J' ? DType{ScalarKind::Int, 4}
                        : code == 'K' ? DType{ScalarKind::Int, 8} : code == 'E' ? DType{ScalarKind::Float, 4}
                                                                       : DType{ScalarKind::Float, 8};
        Array tmp;
        tmp.dtype = t;
        tmp.shape = {1};
        tmp.data.resize(t.size);
        if (t == f.type) ::cvt::util::copy_bytes(tmp.data.data(), p, t.size);
        else store_double(t, tmp.data.data(), scalar_double(f.type, p));
        swap_elements(tmp);
        data.bytes(tmp.data);
      }
    }
  }
  append(out, data.view());
  out.resize(out.size() + (block - data.size() % block) % block, std::byte{0});
  return {};
}

Result<Bytes> write_fits_collection(const Collection& c, WriteContext& ctx) {
  const bool strip = options::resolved_strip_metadata(ctx.options);
  Bytes out;
  std::size_t first_data = 0;
  const auto is_image = [](const Array& a) {
    return a.dtype.kind != ScalarKind::Record && a.dtype.kind != ScalarKind::Bytes && a.dtype.kind != ScalarKind::Unicode &&
           a.dtype.kind != ScalarKind::Complex && !a.shape.empty() && a.count() > 0;
  };
  if (!c.arrays.empty() && is_image(c.arrays.front())) {
    CVT_TRY(append_image_hdu(out, c.arrays.front(), true, c.arrays.front().attributes, strip, c.arrays.size() > 1));
    first_data = 1;
  } else {
    std::string header = card("SIMPLE", Json(true), "conforms to FITS standard") + card("BITPIX", Json(8)) +
                         card("NAXIS", Json(0)) + card("EXTEND", Json(true));
    append_header(header, c.attributes, strip);
    header += commentary("END", "");
    finish_block(header, ' ');
    append(out, as_bytes(header));
  }
  for (std::size_t i = first_data; i < c.arrays.size(); ++i) {
    const Array& a = c.arrays[i];
    if (a.count() == 0 && a.dtype.kind != ScalarKind::Record) continue;
    if (is_image(a)) {
      CVT_TRY(append_image_hdu(out, a, false, a.attributes, strip, false));
    } else {
      CVT_TRY(append_table_hdu(out, a, a.attributes, strip));
    }
  }
  return out;
}

Result<Bytes> write_fits_image(const AnyIR& value, WriteContext& ctx) {
  const ir::Image& image = value.get<ir::Image>();
  if (!image.valid()) return fail(Status::Encode, "the image has no pixels");
  const ir::Frame& f = image.frames.front();
  if (image.animated()) ctx.warn("Only the first frame was written to FITS.");
  const bool grey = imaging::is_grey(f);
  const std::string depth = ctx.options.get_string("fits_bitpix", "8");
  Array a;
  a.name = "PRIMARY";
  const std::uint32_t planes = grey ? 1 : 3;
  a.shape = grey ? std::vector<std::uint64_t>{f.height, f.width} : std::vector<std::uint64_t>{3, f.height, f.width};
  a.dtype = depth == "16" ? DType{ScalarKind::UInt, 2} : depth == "-32" ? DType{ScalarKind::Float, 4} : DType{ScalarKind::UInt, 1};
  a.data.assign(static_cast<std::size_t>(a.count() * a.dtype.size), std::byte{0});
  const bool flip = ctx.options.get_bool("flip_vertical", true);
  for (std::uint32_t c = 0; c < planes; ++c) {
    for (std::uint32_t y = 0; y < f.height; ++y) {
      const std::uint32_t sy = flip ? f.height - 1 - y : y;  // FITS rows run bottom-up
      for (std::uint32_t x = 0; x < f.width; ++x) {
        const std::size_t at = f.offset(x, sy);
        const double v = grey ? f.rgba[at] : f.rgba[at + c];
        const double scaled = depth == "16" ? v * 257.0 : depth == "-32" ? v / 255.0 : v;
        store_double(a.dtype, a.data.data() + ((std::uint64_t{c} * f.height + y) * f.width + x) * a.dtype.size, scaled);
      }
    }
  }
  Collection c;
  c.arrays.push_back(std::move(a));
  return write_fits_collection(c, ctx);
}

int sniff_fits(ByteView data, std::string_view /*name*/) {
  const std::string_view head = head_text(data, 80);
  if (!head.starts_with("SIMPLE  =")) return 0;
  return head.size() >= 30 && head[29] == 'T' ? 97 : 80;
}

}  // namespace
}  // namespace cvt::science::arrays

namespace cvt::science {

void register_fits(Registry& registry) {
  using namespace arrays;
  add_format(registry, {.id = "fits",
                        .name = "FITS image/table",
                        .description = "Flexible Image Transport System: astronomical images, cubes and tables.",
                        .category = Category::Image,
                        .extensions = {"fits", "fit", "fts", "fz"},
                        .mime = "image/fits",
                        .popularity = 25,
                        .signatures = {Signature::at(0, "'SIMPLE  ='")},
                        .sniff = sniff_fits});
  std::vector<OptionSpec> image_read = imaging::window_options("linear", 0.5);
  image_read.push_back(OptionSpec::integer("fits_hdu", "HDU", -1, -1, 4095, "Index of the HDU to render; -1 = first image."));
  image_read.push_back(OptionSpec::boolean("flip_vertical", "Flip vertically", true,
                                           "FITS stores the bottom row first; flip so north is up."));
  image_read.push_back(options::strip_metadata());
  add_codec(registry, "fits", Kind::Image, read_fits_image, write_fits_image, image_read,
            {OptionSpec::choice("fits_bitpix", "Sample type", "8", {"8", "16", "-32"},
                                "8-bit or 16-bit integers, or 32-bit floats in 0..1."),
             OptionSpec::boolean("flip_vertical", "Flip vertically", true, "Store the bottom row first (FITS convention)."),
             options::strip_metadata()},
            20, 15, true);
  add_collection_codecs(registry, "fits", read_fits_collection, write_fits_collection, {}, {options::strip_metadata()});
}

}  // namespace cvt::science
