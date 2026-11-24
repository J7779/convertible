// Convertible core - data family: MessagePack.
//
// Every type of the specification: nil, booleans, all integer widths, float
// 32/64, str, bin -> $binary, arrays, maps (non-string keys become their text),
// ext -> {"$ext": type, "$binary": ...}, and the timestamp extension (-1, all
// three sizes) -> $date with nanosecond precision. Concatenated values (a
// MessagePack stream) are read as documents. The writer picks the smallest
// encoding of every value; doubles become float32 only when that is exact.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "engine/registry.hpp"
#include "families/data/binary_common.hpp"
#include "families/data/data.hpp"
#include "util/bytes.hpp"

namespace cvt::data {

namespace {

class MsgpackReader {
 public:
  MsgpackReader(ByteView data, const ReadContext& ctx)
      : reader_(data), ctx_(ctx), policy_(read_policy(ctx.options)), budget_(data.size()) {}

  Result<std::vector<Json>> read_all() {
    std::vector<Json> documents;
    if (reader_.at_end()) return fail(Status::Decode, "the MessagePack data is empty");
    while (!reader_.at_end()) {
      CVT_TRY_ASSIGN(Json value, read_value(0));
      documents.push_back(std::move(value));
      CVT_TRY(ctx_.checkpoint(static_cast<double>(reader_.position()) / static_cast<double>(reader_.size())));
    }
    if (repaired_ > 0) ctx_.warn(std::format("{} string(s) were not valid UTF-8 and were repaired.", repaired_));
    if (keys_converted_ > 0) ctx_.warn(std::format("{} non-string map key(s) were converted to text.", keys_converted_));
    return documents;
  }

 private:
  std::unexpected<Error> truncated() const {
    return fail(Status::Decode, "the MessagePack data is truncated at byte {}", reader_.position());
  }

  Result<ByteView> take(std::uint64_t size) {
    if (size > reader_.remaining()) return truncated();
    return reader_.bytes(static_cast<std::size_t>(size));
  }

  Result<std::uint64_t> length(std::size_t width) {
    if (width > reader_.remaining()) return truncated();
    switch (width) {
      case 1: return reader_.u8();
      case 2: return reader_.u16be();
      default: return reader_.u32be();
    }
  }

  Result<Json> ext(std::int8_t type, ByteView payload) {
    if (type != -1) return make_ext(type, payload);
    util::ByteReader r(payload);
    std::int64_t seconds = 0;
    std::uint32_t nanos = 0;
    if (payload.size() == 4) {
      seconds = r.u32be();
    } else if (payload.size() == 8) {
      const std::uint64_t word = r.u64be();
      nanos = static_cast<std::uint32_t>(word >> 34);
      seconds = static_cast<std::int64_t>(word & 0x3FFFFFFFFull);
    } else if (payload.size() == 12) {
      nanos = r.u32be();
      seconds = r.i64be();
    } else {
      return fail(Status::Decode, "MessagePack timestamp of {} bytes", payload.size());
    }
    if (nanos > 999'999'999u) return fail(Status::Decode, "MessagePack timestamp with {} nanoseconds", nanos);
    return make_date(format_datetime(from_unix(seconds, nanos)));
  }

  Result<Json> container(std::uint64_t count, bool map, std::size_t depth) {
    if (depth >= policy_.max_depth) {
      return fail(Status::Limit, "the MessagePack data is nested more than {} levels deep (option max_depth)",
                  policy_.max_depth);
    }
    // Every element takes at least one byte (two for map entries).
    if (count > reader_.remaining() / (map ? 2 : 1)) return truncated();
    CVT_TRY(budget_.take(count));
    if (!map) {
      Json array = Json::array();
      array.reserve(static_cast<std::size_t>(count));
      for (std::uint64_t i = 0; i < count; ++i) {
        CVT_TRY_ASSIGN(Json value, read_value(depth + 1));
        array.push_back(std::move(value));
      }
      return array;
    }
    Json object = Json::object();
    for (std::uint64_t i = 0; i < count; ++i) {
      CVT_TRY_ASSIGN(Json key, read_value(depth + 1));
      CVT_TRY_ASSIGN(Json value, read_value(depth + 1));
      if (!key.is_string()) ++keys_converted_;
      CVT_TRY(put_member(object, key_from_value(key), std::move(value), policy_));
    }
    return object;
  }

  Result<Json> read_value(std::size_t depth) {
    if (reader_.at_end()) return truncated();
    const std::uint8_t b = reader_.u8();
    if (b <= 0x7F) return Json(static_cast<std::int64_t>(b));
    if (b >= 0xE0) return Json(static_cast<std::int64_t>(static_cast<std::int8_t>(b)));
    if (b >= 0x80 && b <= 0x8F) return container(b & 0x0F, true, depth);
    if (b >= 0x90 && b <= 0x9F) return container(b & 0x0F, false, depth);
    if (b >= 0xA0 && b <= 0xBF) {
      CVT_TRY_ASSIGN(const ByteView text, take(b & 0x1F));
      return text_value(as_chars(text), repaired_);
    }
    const auto need = [&](std::size_t n) -> Result<void> {
      if (n > reader_.remaining()) return truncated();
      return {};
    };
    switch (b) {
      case 0xC0: return Json();
      case 0xC2: return Json(false);
      case 0xC3: return Json(true);
      case 0xC4:
      case 0xC5:
      case 0xC6: {
        CVT_TRY_ASSIGN(const std::uint64_t size, length(std::size_t{1} << (b - 0xC4)));
        CVT_TRY_ASSIGN(const ByteView bytes, take(size));
        return make_binary(bytes);
      }
      case 0xC7:
      case 0xC8:
      case 0xC9: {
        CVT_TRY_ASSIGN(const std::uint64_t size, length(std::size_t{1} << (b - 0xC7)));
        CVT_TRY(need(1));
        const auto type = static_cast<std::int8_t>(reader_.u8());
        CVT_TRY_ASSIGN(const ByteView payload, take(size));
        return ext(type, payload);
      }
      case 0xCA: CVT_TRY(need(4)); return Json(static_cast<double>(reader_.f32be()));
      case 0xCB: CVT_TRY(need(8)); return Json(reader_.f64be());
      case 0xCC: CVT_TRY(need(1)); return Json(static_cast<std::int64_t>(reader_.u8()));
      case 0xCD: CVT_TRY(need(2)); return Json(static_cast<std::int64_t>(reader_.u16be()));
      case 0xCE: CVT_TRY(need(4)); return Json(static_cast<std::int64_t>(reader_.u32be()));
      case 0xCF: {
        CVT_TRY(need(8));
        const std::uint64_t v = reader_.u64be();
        if (v <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return Json(static_cast<std::int64_t>(v));
        return Json(v);
      }
      case 0xD0: CVT_TRY(need(1)); return Json(static_cast<std::int64_t>(reader_.i8()));
      case 0xD1: CVT_TRY(need(2)); return Json(static_cast<std::int64_t>(reader_.i16be()));
      case 0xD2: CVT_TRY(need(4)); return Json(static_cast<std::int64_t>(reader_.i32be()));
      case 0xD3: CVT_TRY(need(8)); return Json(reader_.i64be());
      case 0xD4:
      case 0xD5:
      case 0xD6:
      case 0xD7:
      case 0xD8: {
        CVT_TRY(need(1));
        const auto type = static_cast<std::int8_t>(reader_.u8());
        CVT_TRY_ASSIGN(const ByteView payload, take(std::size_t{1} << (b - 0xD4)));
        return ext(type, payload);
      }
      case 0xD9:
      case 0xDA:
      case 0xDB: {
        CVT_TRY_ASSIGN(const std::uint64_t size, length(std::size_t{1} << (b - 0xD9)));
        CVT_TRY_ASSIGN(const ByteView text, take(size));
        return text_value(as_chars(text), repaired_);
      }
      case 0xDC:
      case 0xDD: {
        CVT_TRY_ASSIGN(const std::uint64_t count, length(b == 0xDC ? 2 : 4));
        return container(count, false, depth);
      }
      case 0xDE:
      case 0xDF: {
        CVT_TRY_ASSIGN(const std::uint64_t count, length(b == 0xDE ? 2 : 4));
        return container(count, true, depth);
      }
      default: break;
    }
    return fail(Status::Decode, "invalid MessagePack type byte 0x{:02X} at byte {}", b, reader_.position() - 1);
  }

  util::ByteReader reader_;
  const ReadContext& ctx_;
  ReadPolicy policy_;
  NodeBudget budget_;
  std::size_t repaired_ = 0;
  std::size_t keys_converted_ = 0;
};

Result<Json> parse_msgpack(ByteView input, ReadContext& ctx) {
  MsgpackReader reader(input, ctx);
  CVT_TRY_ASSIGN(std::vector<Json> documents, reader.read_all());
  return select_documents(std::move(documents), ctx.options, "MessagePack");
}

// =========================================================================== writer

class MsgpackWriter {
 public:
  MsgpackWriter(bool shrink_floats, const WriteContext& ctx) : shrink_(shrink_floats), ctx_(ctx) {}

  Result<Bytes> run(const Json& value) {
    CVT_TRY(write(value));
    if (lossy_ > 0) {
      ctx_.warn(std::format("{} number(s) beyond MessagePack's 64-bit range were written as text or rounded.", lossy_));
    }
    return out_.take();
  }

 private:
  void header(std::uint64_t size, std::uint8_t fix_base, std::uint64_t fix_max, std::uint8_t b8, std::uint8_t b16,
              std::uint8_t b32) {
    if (fix_base != 0 && size <= fix_max) {
      out_.u8(static_cast<std::uint8_t>(fix_base | size));
    } else if (b8 != 0 && size <= 0xFF) {
      out_.u8(b8).u8(static_cast<std::uint8_t>(size));
    } else if (size <= 0xFFFF) {
      out_.u8(b16).u16be(static_cast<std::uint16_t>(size));
    } else {
      out_.u8(b32).u32be(static_cast<std::uint32_t>(size));
    }
  }

  Result<void> check_size(std::uint64_t size) const {
    if (size > 0xFFFFFFFFull) return fail(Status::Encode, "MessagePack cannot hold more than 2^32-1 items or bytes");
    return {};
  }

  Result<void> string(std::string_view text) {
    const std::string valid = util::is_valid_utf8(text) ? std::string(text) : repair_utf8(text);
    CVT_TRY(check_size(valid.size()));
    header(valid.size(), 0xA0, 31, 0xD9, 0xDA, 0xDB);
    out_.text(valid);
    return {};
  }

  void integer(std::int64_t v) {
    if (v >= 0) {
      unsigned_integer(static_cast<std::uint64_t>(v));
    } else if (v >= -32) {
      out_.u8(static_cast<std::uint8_t>(v));
    } else if (v >= -128) {
      out_.u8(0xD0).i8(static_cast<std::int8_t>(v));
    } else if (v >= -32768) {
      out_.u8(0xD1).i16be(static_cast<std::int16_t>(v));
    } else if (v >= std::numeric_limits<std::int32_t>::min()) {
      out_.u8(0xD2).i32be(static_cast<std::int32_t>(v));
    } else {
      out_.u8(0xD3).i64be(v);
    }
  }

  void unsigned_integer(std::uint64_t v) {
    if (v <= 0x7F) {
      out_.u8(static_cast<std::uint8_t>(v));
    } else if (v <= 0xFF) {
      out_.u8(0xCC).u8(static_cast<std::uint8_t>(v));
    } else if (v <= 0xFFFF) {
      out_.u8(0xCD).u16be(static_cast<std::uint16_t>(v));
    } else if (v <= 0xFFFFFFFFull) {
      out_.u8(0xCE).u32be(static_cast<std::uint32_t>(v));
    } else {
      out_.u8(0xCF).u64be(v);
    }
  }

  void floating(double d) {
    if (shrink_ && exact_float(d)) {
      out_.u8(0xCA).f32be(static_cast<float>(d));
    } else {
      out_.u8(0xCB).f64be(d);
    }
  }

  void ext(std::int8_t type, ByteView payload) {
    const std::size_t size = payload.size();
    if (size == 1 || size == 2 || size == 4 || size == 8 || size == 16) {
      const std::uint8_t code = size == 1 ? 0xD4 : size == 2 ? 0xD5 : size == 4 ? 0xD6 : size == 8 ? 0xD7 : 0xD8;
      out_.u8(code);
    } else {
      header(size, 0, 0, 0xC7, 0xC8, 0xC9);
    }
    out_.i8(type);
    out_.bytes(payload);
  }

  Result<void> write(const Json& value) {
    switch (tag_kind(value)) {
      case TagKind::None: break;
      case TagKind::Binary: {
        const auto bytes = tagged_bytes(value);
        if (!bytes) return fail(Status::Encode, "invalid Base64 in a binary value");
        CVT_TRY(check_size(bytes->size()));
        header(bytes->size(), 0, 0, 0xC4, 0xC5, 0xC6);
        out_.bytes(*bytes);
        return {};
      }
      case TagKind::Ext: {
        const auto bytes = tagged_bytes(value);
        if (!bytes) return fail(Status::Encode, "invalid Base64 in an extension value");
        CVT_TRY(check_size(bytes->size()));
        ext(static_cast<std::int8_t>(value[ir::data_tags::ext].as_int64()), *bytes);
        return {};
      }
      case TagKind::Date: {
        const auto parsed = parse_datetime(tag_text(value));
        if (!parsed || parsed->kind == DateTime::Kind::Time) return string(tag_text(value));
        const std::int64_t seconds = to_unix_seconds(*parsed);
        const std::uint32_t nanos = parsed->nanos();
        util::ByteWriter payload;
        if (nanos == 0 && seconds >= 0 && seconds <= 0xFFFFFFFFll) {
          payload.u32be(static_cast<std::uint32_t>(seconds));
        } else if (seconds >= 0 && seconds < (std::int64_t{1} << 34)) {
          payload.u64be((static_cast<std::uint64_t>(nanos) << 34) | static_cast<std::uint64_t>(seconds));
        } else {
          payload.u32be(nanos).i64be(seconds);
        }
        ext(-1, payload.view());
        return {};
      }
      case TagKind::BigInt:
        ++lossy_;
        return string(tag_text(value));
      case TagKind::Decimal:
        ++lossy_;
        out_.u8(0xCB).f64be(util::parse_double(tag_text(value)).value_or(0.0));
        return {};
      case TagKind::Float: floating(plain_value(value).as_double()); return {};
      case TagKind::Uid: unsigned_integer(value[ir::data_tags::uid].as_uint64()); return {};
      case TagKind::Undefined: out_.u8(0xC0); return {};
      default: break;  // other tags stay maps
    }
    switch (value.type()) {
      case Json::Type::Null: out_.u8(0xC0); return {};
      case Json::Type::Bool: out_.u8(value.as_bool() ? 0xC3 : 0xC2); return {};
      case Json::Type::Int64: integer(value.as_int64()); return {};
      case Json::Type::UInt64: unsigned_integer(value.as_uint64()); return {};
      case Json::Type::Double: floating(value.as_double()); return {};
      case Json::Type::String: return string(value.as_string());
      case Json::Type::Array:
        CVT_TRY(check_size(value.size()));
        header(value.size(), 0x90, 15, 0, 0xDC, 0xDD);
        for (const Json& element : value.elements()) CVT_TRY(write(element));
        return {};
      case Json::Type::Object:
        CVT_TRY(check_size(value.size()));
        header(value.size(), 0x80, 15, 0, 0xDE, 0xDF);
        for (const auto& member : value.items()) {
          CVT_TRY(string(member.key()));
          CVT_TRY(write(member.value()));
        }
        return {};
    }
    return {};
  }

  bool shrink_ = false;
  const WriteContext& ctx_;
  util::ByteWriter out_;
  std::size_t lossy_ = 0;
};

Result<Bytes> write_msgpack(const Json& value, WriteContext& ctx) {
  const std::string floats = ctx.options.get_enum("float_size", "auto", {"auto", "64"});
  MsgpackWriter writer(floats == "auto", ctx);
  return writer.run(value);
}

}  // namespace

void register_msgpack(Registry& registry) {
  registry.add_format(FormatInfo{
      .id = "msgpack",
      .name = "MessagePack",
      .description = "Compact binary serialisation of JSON-like data.",
      .category = Category::Data,
      .extensions = {"msgpack", "mpk", "mpack"},
      .mime = "application/vnd.msgpack",
      .popularity = 40,
      .signatures = {},
      .sniff = [](ByteView, std::string_view filename) { return has_extension(filename, {"msgpack", "mpk", "mpack"}) ? 75 : 0; },
  });
  std::vector<OptionSpec> read_options = common_read_options(true, false);
  read_options.push_back(documents_option());
  read_options.push_back(document_index_option());
  add_codec(registry, {.format_id = "msgpack",
                       .read = binary_reader(parse_msgpack),
                       .write = binary_writer(write_msgpack, "MessagePack"),
                       .read_options = std::move(read_options),
                       .write_options = {OptionSpec::choice("float_size", "Floating-point size", "auto", {"auto", "64"},
                                                            "auto writes a 32-bit float wherever that holds the value "
                                                            "exactly; 64 always writes doubles.")
                                             .advanced()}});
}

}  // namespace cvt::data
