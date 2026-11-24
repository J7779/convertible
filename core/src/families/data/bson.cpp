// Convertible core - data family: BSON (the MongoDB document format).
//
// Every element type: double, string, document, array, binary (with its
// subtype), undefined, ObjectId, boolean, UTC datetime (milliseconds),
// null, regular expression, DBPointer ({"$ref", "$id"}), JavaScript code
// (with scope: {"$code", "$scope"}), symbol, int32, int64, timestamp,
// decimal128 (exact, as $decimal) and min/max keys - using the tagged
// convention, which mirrors MongoDB Extended JSON. A file of concatenated
// documents (mongodump) is read as documents.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "engine/registry.hpp"
#include "families/data/binary_common.hpp"
#include "families/data/data.hpp"
#include "util/bytes.hpp"
#include "util/hex.hpp"

namespace cvt::data {

namespace {

namespace tags = ir::data_tags;

/// decimal128 (IEEE 754-2008 BID) -> decimal text.
Json decimal128_value(std::uint64_t high, std::uint64_t low) {
  const bool negative = (high >> 63) != 0;
  const std::uint64_t combination = (high >> 58) & 0x1F;
  if (combination == 0x1F) return Json(std::numeric_limits<double>::quiet_NaN());
  if (combination == 0x1E) {
    return Json(negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity());
  }
  std::int64_t exponent = 0;
  std::uint64_t coefficient_high = 0;
  if (((high >> 61) & 3) == 3) {
    // Coefficients of this form exceed 10^34 - 1: non-canonical, read as zero.
    exponent = static_cast<std::int64_t>((high >> 47) & 0x3FFF) - 6176;
    return make_decimal(decimal_text(negative, "0", exponent));
  }
  exponent = static_cast<std::int64_t>((high >> 49) & 0x3FFF) - 6176;
  coefficient_high = high & 0x1FFFFFFFFFFFFull;
  Bytes magnitude(16);
  for (int i = 0; i < 8; ++i) {
    magnitude[static_cast<std::size_t>(i)] = static_cast<std::byte>((coefficient_high >> (56 - 8 * i)) & 0xFF);
    magnitude[static_cast<std::size_t>(8 + i)] = static_cast<std::byte>((low >> (56 - 8 * i)) & 0xFF);
  }
  std::string digits = magnitude_to_decimal(magnitude, false);
  if (digits.size() > 34) digits = "0";
  return make_decimal(decimal_text(negative, digits, exponent));
}

/// decimal text -> decimal128 words; nullopt when it does not fit exactly.
std::optional<std::pair<std::uint64_t, std::uint64_t>> to_decimal128(std::string_view text) {
  auto parts = decimal_parts(text);
  if (!parts) return std::nullopt;
  std::string digits = parts->digits;
  std::int64_t exponent = parts->exponent;
  // Trailing zeros may move into the exponent when the exponent is too small.
  while (digits.size() > 1 && digits.back() == '0' && (digits.size() > 34 || exponent < -6176)) {
    digits.pop_back();
    ++exponent;
  }
  // Too large an exponent may be absorbed by adding zeros to the coefficient.
  while (exponent > 6111 && digits.size() < 34 && digits != "0") {
    digits.push_back('0');
    --exponent;
  }
  if (digits == "0") exponent = std::clamp<std::int64_t>(exponent, -6176, 6111);
  if (digits.size() > 34 || exponent < -6176 || exponent > 6111) return std::nullopt;
  bool negative_digits = false;
  const auto magnitude = decimal_to_magnitude(digits, &negative_digits);
  if (!magnitude || magnitude->size() > 16) return std::nullopt;
  std::uint64_t high = 0;
  std::uint64_t low = 0;
  const std::size_t size = magnitude->size();
  for (std::size_t i = 0; i < size; ++i) {
    const auto byte = std::to_integer<std::uint64_t>((*magnitude)[i]);
    const std::size_t position = size - 1 - i;  // 0 = least significant
    if (position < 8) {
      low |= byte << (8 * position);
    } else {
      high |= byte << (8 * (position - 8));
    }
  }
  const auto biased = static_cast<std::uint64_t>(exponent + 6176);
  high |= biased << 49;
  if (parts->negative) high |= std::uint64_t{1} << 63;
  return std::pair{high, low};
}

class BsonReader {
 public:
  BsonReader(ByteView data, const ReadContext& ctx)
      : data_(data), ctx_(ctx), policy_(read_policy(ctx.options)), budget_(data.size()) {}

  Result<std::vector<Json>> read_all() {
    std::vector<Json> documents;
    std::size_t at = 0;
    if (data_.empty()) return fail(Status::Decode, "the BSON data is empty");
    while (at < data_.size()) {
      CVT_TRY_ASSIGN(Json document, read_document(at, false, 0));
      documents.push_back(std::move(document));
      CVT_TRY(ctx_.checkpoint(static_cast<double>(at) / static_cast<double>(data_.size())));
    }
    if (repaired_ > 0) ctx_.warn(std::format("{} string(s) were not valid UTF-8 and were repaired.", repaired_));
    return documents;
  }

 private:
  std::unexpected<Error> bad(std::size_t at, std::string_view what) const {
    return fail(Status::Decode, "invalid BSON at byte {}: {}", at, what);
  }

  Result<std::uint32_t> u32(std::size_t at, std::size_t end) const {
    if (at + 4 > end) return bad(at, "truncated");
    util::ByteReader r(data_.subspan(at, 4));
    return r.u32le();
  }

  Result<std::string> cstring(std::size_t& at, std::size_t end) {
    const std::string_view rest = as_chars(data_.subspan(at, end - at));
    const std::size_t nul = rest.find('\0');
    if (nul == std::string_view::npos) return bad(at, "unterminated name");
    at += nul + 1;
    if (!util::is_valid_utf8(rest.substr(0, nul))) {
      ++repaired_;
      return repair_utf8(rest.substr(0, nul));
    }
    return std::string(rest.substr(0, nul));
  }

  Result<std::string> string(std::size_t& at, std::size_t end) {
    CVT_TRY_ASSIGN(const std::uint32_t size, u32(at, end));
    if (size == 0 || size > end - at - 4) return bad(at, "bad string length");
    const std::string_view text = as_chars(data_.subspan(at + 4, size - 1));
    if (std::to_integer<int>(data_[at + 4 + size - 1]) != 0) return bad(at, "string without terminating NUL");
    at += 4 + size;
    return text_value(text, repaired_).as_string();
  }

  Result<Json> read_document(std::size_t& at, bool array, std::size_t depth) {
    if (depth > policy_.max_depth) {
      return fail(Status::Limit, "the BSON data is nested more than {} levels deep (option max_depth)", policy_.max_depth);
    }
    CVT_TRY_ASSIGN(const std::uint32_t size, u32(at, data_.size()));
    if (size < 5 || size > data_.size() - at) return bad(at, "bad document length");
    const std::size_t end = at + size;
    if (std::to_integer<int>(data_[end - 1]) != 0) return bad(end - 1, "document does not end with NUL");
    std::size_t p = at + 4;
    Json out = array ? Json::array() : Json::object();
    while (p < end - 1) {
      CVT_TRY(budget_.take());
      const auto type = std::to_integer<std::uint8_t>(data_[p++]);
      CVT_TRY_ASSIGN(std::string name, cstring(p, end - 1));
      CVT_TRY_ASSIGN(Json value, element(type, p, end - 1, depth));
      if (array) {
        out.push_back(std::move(value));
      } else {
        CVT_TRY(put_member(out, std::move(name), std::move(value), policy_));
      }
    }
    if (p != end - 1) return bad(p, "element overruns its document");
    at = end;
    return out;
  }

  Result<Json> element(std::uint8_t type, std::size_t& p, std::size_t end, std::size_t depth) {
    const auto need = [&](std::size_t n) -> Result<void> {
      if (n > end - p) return bad(p, "truncated element");
      return {};
    };
    util::ByteReader r;
    switch (type) {
      case 0x01: {
        CVT_TRY(need(8));
        r = util::ByteReader(data_.subspan(p, 8));
        p += 8;
        return Json(r.f64le());
      }
      case 0x02: {
        CVT_TRY_ASSIGN(std::string text, string(p, end));
        return Json(std::move(text));
      }
      case 0x03:
      case 0x04: {
        std::size_t at = p;
        if (p + 4 > end) return bad(p, "truncated element");
        CVT_TRY_ASSIGN(const std::uint32_t size, u32(p, end));
        if (size > end - p) return bad(p, "embedded document overruns its parent");
        CVT_TRY_ASSIGN(Json value, read_document(at, type == 0x04, depth + 1));
        p = at;
        return value;
      }
      case 0x05: {
        CVT_TRY_ASSIGN(const std::uint32_t size, u32(p, end));
        if (size > end - p - 4 || end - p - 4 < 1 + static_cast<std::size_t>(size)) return bad(p, "bad binary length");
        const auto subtype = std::to_integer<std::uint8_t>(data_[p + 4]);
        ByteView bytes = data_.subspan(p + 5, size);
        p += 5 + size;
        if (subtype == 0x02 && bytes.size() >= 4) bytes = bytes.subspan(4);  // old binary: inner length
        return subtype == 0 ? make_binary(bytes) : make_binary_subtype(bytes, subtype);
      }
      case 0x06: return make_undefined();
      case 0x07: {
        CVT_TRY(need(12));
        const ByteView id = data_.subspan(p, 12);
        p += 12;
        return make_object_id(id);
      }
      case 0x08: {
        CVT_TRY(need(1));
        const auto b = std::to_integer<int>(data_[p++]);
        if (b > 1) return bad(p - 1, "invalid boolean");
        return Json(b == 1);
      }
      case 0x09: {
        CVT_TRY(need(8));
        r = util::ByteReader(data_.subspan(p, 8));
        p += 8;
        const std::int64_t ms = r.i64le();
        std::int64_t seconds = ms / 1000;
        std::int64_t rest = ms % 1000;
        if (rest < 0) {
          rest += 1000;
          --seconds;
        }
        return make_date(format_datetime(from_unix(seconds, static_cast<std::uint32_t>(rest) * 1'000'000u)));
      }
      case 0x0A: return Json();
      case 0x0B: {
        CVT_TRY_ASSIGN(std::string pattern, cstring(p, end));
        CVT_TRY_ASSIGN(std::string options, cstring(p, end));
        return make_regex(std::move(pattern), std::move(options));
      }
      case 0x0C: {
        CVT_TRY_ASSIGN(std::string ns, string(p, end));
        CVT_TRY(need(12));
        const ByteView id = data_.subspan(p, 12);
        p += 12;
        return Json::object({{"$ref", std::move(ns)}, {"$id", make_object_id(id)}});
      }
      case 0x0D:
      case 0x0E: {
        CVT_TRY_ASSIGN(std::string text, string(p, end));
        return type == 0x0D ? make_code(std::move(text)) : make_symbol(std::move(text));
      }
      case 0x0F: {
        CVT_TRY_ASSIGN(const std::uint32_t total, u32(p, end));
        if (total < 14 || total > end - p) return bad(p, "bad code-with-scope length");
        const std::size_t stop = p + total;
        std::size_t q = p + 4;
        CVT_TRY_ASSIGN(std::string code, string(q, stop));
        CVT_TRY_ASSIGN(Json scope, read_document(q, false, depth + 1));
        if (q != stop) return bad(p, "bad code-with-scope length");
        p = stop;
        return Json::object({{tags::code, std::move(code)}, {"$scope", std::move(scope)}});
      }
      case 0x10: {
        CVT_TRY(need(4));
        r = util::ByteReader(data_.subspan(p, 4));
        p += 4;
        return Json(static_cast<std::int64_t>(r.i32le()));
      }
      case 0x11: {
        CVT_TRY(need(8));
        r = util::ByteReader(data_.subspan(p, 8));
        p += 8;
        const std::uint32_t increment = r.u32le();
        const std::uint32_t seconds = r.u32le();
        return Json::object({{tags::timestamp, Json::object({{"t", seconds}, {"i", increment}})}});
      }
      case 0x12: {
        CVT_TRY(need(8));
        r = util::ByteReader(data_.subspan(p, 8));
        p += 8;
        return Json(r.i64le());
      }
      case 0x13: {
        CVT_TRY(need(16));
        r = util::ByteReader(data_.subspan(p, 16));
        p += 16;
        const std::uint64_t low = r.u64le();
        const std::uint64_t high = r.u64le();
        return decimal128_value(high, low);
      }
      case 0xFF: return Json::object({{tags::min_key, 1}});
      case 0x7F: return Json::object({{tags::max_key, 1}});
      default: break;
    }
    return bad(p - 1, std::format("unknown element type 0x{:02X}", type));
  }

  ByteView data_;
  const ReadContext& ctx_;
  ReadPolicy policy_;
  NodeBudget budget_;
  std::size_t repaired_ = 0;
};

Result<Json> parse_bson(ByteView input, ReadContext& ctx) {
  BsonReader reader(input, ctx);
  CVT_TRY_ASSIGN(std::vector<Json> documents, reader.read_all());
  return select_documents(std::move(documents), ctx.options, "BSON");
}

// =========================================================================== writer

class BsonWriter {
 public:
  explicit BsonWriter(const WriteContext& ctx) : ctx_(ctx) {}

  Result<Bytes> run(const Json& value) {
    CVT_TRY(document(value, false));
    if (converted_ > 0) ctx_.warn(std::format("{} value(s) without a BSON type were converted (see the documentation).", converted_));
    return out_.take();
  }

 private:
  Result<void> name(std::string_view key) {
    if (key.find('\0') != std::string_view::npos) return fail(Status::Encode, "BSON keys cannot contain NUL characters");
    out_.text(key).u8(0);
    return {};
  }

  void string(std::string_view text) {
    const std::string valid = util::is_valid_utf8(text) ? std::string(text) : repair_utf8(text);
    out_.u32le(static_cast<std::uint32_t>(valid.size() + 1)).text(valid).u8(0);
  }

  Result<void> document(const Json& value, bool array) {
    const std::size_t start = out_.size();
    out_.u32le(0);
    if (array) {
      for (std::size_t i = 0; i < value.size(); ++i) CVT_TRY(element(std::to_string(i), value[i]));
    } else {
      for (const auto& member : value.items()) CVT_TRY(element(member.key(), member.value()));
    }
    out_.u8(0);
    const std::size_t size = out_.size() - start;
    if (size > 0x7FFFFFFF) return fail(Status::Encode, "the BSON document is larger than 2 GiB");
    out_.patch_u32le(start, static_cast<std::uint32_t>(size));
    return {};
  }

  Result<void> object_id(std::string_view hex) {
    auto bytes = util::hex_decode(hex);
    if (!bytes || bytes->size() != 12) return fail(Status::Encode, "invalid ObjectId '{}'", hex);
    out_.bytes(*bytes);
    return {};
  }

  Result<void> element(std::string_view key, const Json& value) {
    const auto start = [&](std::uint8_t type) -> Result<void> {
      out_.u8(type);
      return name(key);
    };
    switch (tag_kind(value)) {
      case TagKind::None: break;
      case TagKind::Binary:
      case TagKind::Ext: {
        const auto bytes = tagged_bytes(value);
        if (!bytes) return fail(Status::Encode, "invalid Base64 in a binary value");
        CVT_TRY(start(0x05));
        const Json* subtype = value.find(tags::subtype);
        out_.u32le(static_cast<std::uint32_t>(bytes->size())).u8(subtype != nullptr ? static_cast<std::uint8_t>(subtype->as_int64()) : 0);
        out_.bytes(*bytes);
        return {};
      }
      case TagKind::Date: {
        const auto parsed = parse_datetime(tag_text(value));
        if (!parsed || parsed->kind == DateTime::Kind::Time) break;
        const std::int64_t ms = to_unix_seconds(*parsed) * 1000 + parsed->nanos() / 1'000'000;
        if (parsed->nanos() % 1'000'000 != 0) ++converted_;
        CVT_TRY(start(0x09));
        out_.i64le(ms);
        return {};
      }
      case TagKind::Decimal:
      case TagKind::BigInt: {
        const auto words = to_decimal128(tag_text(value));
        if (!words) break;
        CVT_TRY(start(0x13));
        out_.u64le(words->second).u64le(words->first);
        return {};
      }
      case TagKind::Float:
        CVT_TRY(start(0x01));
        out_.f64le(plain_value(value).as_double());
        return {};
      case TagKind::Undefined: CVT_TRY(start(0x06)); return {};
      case TagKind::ObjectId:
        CVT_TRY(start(0x07));
        return object_id(tag_text(value));
      case TagKind::Regex: {
        const std::string& pattern = value[tags::regex].as_string();
        const std::string& options = value[tags::regex_options].as_string();
        if (pattern.find('\0') != std::string::npos || options.find('\0') != std::string::npos) break;
        CVT_TRY(start(0x0B));
        out_.text(pattern).u8(0).text(options).u8(0);
        return {};
      }
      case TagKind::Timestamp: {
        const Json& ts = value[tags::timestamp];
        CVT_TRY(start(0x11));
        out_.u32le(static_cast<std::uint32_t>(ts["i"].as_uint64())).u32le(static_cast<std::uint32_t>(ts["t"].as_uint64()));
        return {};
      }
      case TagKind::MinKey: CVT_TRY(start(0xFF)); return {};
      case TagKind::MaxKey: CVT_TRY(start(0x7F)); return {};
      case TagKind::Code:
        CVT_TRY(start(0x0D));
        string(tag_text(value));
        return {};
      case TagKind::Symbol:
        CVT_TRY(start(0x0E));
        string(tag_text(value));
        return {};
      case TagKind::Uid:
        CVT_TRY(start(0x12));
        out_.i64le(static_cast<std::int64_t>(value[tags::uid].as_uint64()));
        return {};
      case TagKind::CborTag: return element(key, value[tags::tag_value]);
      default: break;
    }
    if (tag_kind(value) != TagKind::None && !value.is_object()) ++converted_;
    switch (value.type()) {
      case Json::Type::Null: CVT_TRY(start(0x0A)); return {};
      case Json::Type::Bool:
        CVT_TRY(start(0x08));
        out_.u8(value.as_bool() ? 1 : 0);
        return {};
      case Json::Type::Int64: {
        const std::int64_t v = value.as_int64();
        if (v >= std::numeric_limits<std::int32_t>::min() && v <= std::numeric_limits<std::int32_t>::max()) {
          CVT_TRY(start(0x10));
          out_.i32le(static_cast<std::int32_t>(v));
        } else {
          CVT_TRY(start(0x12));
          out_.i64le(v);
        }
        return {};
      }
      case Json::Type::UInt64: {
        const auto words = to_decimal128(format_integer(value));
        ++converted_;
        CVT_TRY(start(0x13));
        out_.u64le(words ? words->second : 0).u64le(words ? words->first : 0);
        return {};
      }
      case Json::Type::Double:
        CVT_TRY(start(0x01));
        out_.f64le(value.as_double());
        return {};
      case Json::Type::String:
        CVT_TRY(start(0x02));
        string(value.as_string());
        return {};
      case Json::Type::Array:
        CVT_TRY(start(0x04));
        return document(value, true);
      case Json::Type::Object:
        if (tag_kind(value) != TagKind::None) {
          // A tagged value BSON has no type for (time of day, huge number): its text.
          ++converted_;
          CVT_TRY(start(0x02));
          string(tag_text(value));
          return {};
        }
        CVT_TRY(start(0x03));
        return document(value, false);
    }
    return {};
  }

  const WriteContext& ctx_;
  util::ByteWriter out_;
  std::size_t converted_ = 0;
};

Result<Bytes> write_bson(const Json& value, WriteContext& ctx) {
  BsonWriter writer(ctx);
  if (value.is_object() && tag_kind(value) == TagKind::None) return writer.run(value);
  if (value.is_array() && ctx.options.get_bool("split_documents", true) &&
      std::ranges::all_of(value.elements(), [](const Json& e) { return e.is_object() && tag_kind(e) == TagKind::None; })) {
    Bytes out;
    for (const Json& document : value.elements()) {
      BsonWriter one(ctx);
      CVT_TRY_ASSIGN(const Bytes bytes, one.run(document));
      out.insert(out.end(), bytes.begin(), bytes.end());
    }
    return out;
  }
  ctx.warn("A BSON document is an object; the data was placed under the key \"value\".");
  return writer.run(Json::object({{"value", value}}));
}

int sniff_bson(ByteView data, std::string_view filename) {
  if (has_extension(filename, {"bson"})) return 75;
  // The declared length equals the file size and the document ends with NUL.
  if (data.size() >= 5 && data.size() < 0x7FFFFFFF) {
    util::ByteReader r(data);
    const std::uint32_t size = r.u32le();
    if (size == data.size() && std::to_integer<int>(data.back()) == 0) {
      const auto type = std::to_integer<int>(data[4]);
      if ((type >= 1 && type <= 0x13) || size == 5) return 40;
    }
  }
  return 0;
}

}  // namespace

void register_bson(Registry& registry) {
  registry.add_format(FormatInfo{
      .id = "bson",
      .name = "BSON",
      .description = "Binary JSON, the document format of MongoDB.",
      .category = Category::Data,
      .extensions = {"bson"},
      .mime = "application/bson",
      .popularity = 30,
      .signatures = {},
      .sniff = sniff_bson,
  });
  std::vector<OptionSpec> read_options = common_read_options(true, false);
  read_options.push_back(documents_option());
  read_options.push_back(document_index_option());
  add_codec(registry, {.format_id = "bson",
                       .read = binary_reader(parse_bson),
                       .write = binary_writer(write_bson, "BSON"),
                       .read_options = std::move(read_options),
                       .write_options = {OptionSpec::boolean(
                           "split_documents", "One document per item", true,
                           "Write a top-level list of objects as consecutive documents (like mongodump).")}});
}

}  // namespace cvt::data
