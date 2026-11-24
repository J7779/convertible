// Convertible core - data family: CBOR (RFC 8949).
//
// All major types with definite and indefinite lengths, half/single/double
// floats, simple values (undefined -> $undefined) and tags:
//   0 date/time string, 1 epoch time, 100 / 1004 dates (RFC 8943) -> $date;
//   2 / 3 bignums -> integer or $bigint; 4 decimal fractions -> number or
//   $decimal; 21-23 encoding hints and 55799 (self-describe) are dropped;
//   every other tag -> {"$tag": n, "$value": v}.
// A CBOR sequence (RFC 8742) is read as documents. The writer uses the
// preferred (shortest) serialisation: smallest integer heads and the
// shortest float that holds a value exactly.
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

class CborReader {
 public:
  CborReader(ByteView data, const ReadContext& ctx)
      : reader_(data), ctx_(ctx), policy_(read_policy(ctx.options)), budget_(data.size()) {}

  Result<std::vector<Json>> read_all() {
    if (reader_.at_end()) return fail(Status::Decode, "the CBOR data is empty");
    std::vector<Json> documents;
    while (!reader_.at_end()) {
      CVT_TRY_ASSIGN(Item item, read_item(0));
      if (item.is_break) return fail(Status::Decode, "unexpected CBOR break code at byte {}", reader_.position() - 1);
      documents.push_back(std::move(item.value));
      CVT_TRY(ctx_.checkpoint(static_cast<double>(reader_.position()) / static_cast<double>(reader_.size())));
    }
    if (repaired_ > 0) ctx_.warn(std::format("{} text string(s) were not valid UTF-8 and were repaired.", repaired_));
    if (keys_converted_ > 0) ctx_.warn(std::format("{} non-text map key(s) were converted to text.", keys_converted_));
    if (simple_ > 0) ctx_.warn(std::format("{} unassigned simple value(s) were read as null.", simple_));
    return documents;
  }

 private:
  struct Item {
    Json value;
    bool is_break = false;
  };

  std::unexpected<Error> truncated() const {
    return fail(Status::Decode, "the CBOR data is truncated at byte {}", reader_.position());
  }

  /// The argument of a head; `indefinite` for additional info 31.
  Result<std::uint64_t> argument(std::uint8_t info, bool& indefinite) {
    indefinite = false;
    if (info < 24) return info;
    const auto need = [&](std::size_t n) -> Result<void> {
      if (n > reader_.remaining()) return truncated();
      return {};
    };
    switch (info) {
      case 24: CVT_TRY(need(1)); return reader_.u8();
      case 25: CVT_TRY(need(2)); return reader_.u16be();
      case 26: CVT_TRY(need(4)); return reader_.u32be();
      case 27: CVT_TRY(need(8)); return reader_.u64be();
      case 31: indefinite = true; return 0;
      default: break;
    }
    return fail(Status::Decode, "reserved CBOR additional information {} at byte {}", info, reader_.position() - 1);
  }

  Result<std::string> string_bytes(std::uint8_t major, std::uint64_t size, bool indefinite, std::size_t depth) {
    if (!indefinite) {
      if (size > reader_.remaining()) return truncated();
      return std::string(as_chars(reader_.bytes(static_cast<std::size_t>(size))));
    }
    std::string out;
    while (true) {
      if (reader_.at_end()) return truncated();
      const std::uint8_t head = reader_.u8();
      if (head == 0xFF) return out;
      if ((head >> 5) != major) return fail(Status::Decode, "invalid chunk in an indefinite-length CBOR string");
      bool chunk_indefinite = false;
      CVT_TRY_ASSIGN(const std::uint64_t chunk, argument(head & 0x1F, chunk_indefinite));
      if (chunk_indefinite) return fail(Status::Decode, "nested indefinite-length CBOR string");
      if (chunk > reader_.remaining()) return truncated();
      out.append(as_chars(reader_.bytes(static_cast<std::size_t>(chunk))));
      (void)depth;
    }
  }

  Result<Json> tagged(std::uint64_t tag, Json value) {
    switch (tag) {
      case 0: {
        if (!value.is_string()) break;
        const auto parsed = parse_datetime(value.as_string());
        if (!parsed) break;
        return make_date(format_datetime(*parsed));
      }
      case 1: {
        if (value.is_integer() && value.get_int64()) return make_date(format_datetime(from_unix(*value.get_int64())));
        if (value.is_float() && std::isfinite(value.as_double()) && std::abs(value.as_double()) < 1e15) {
          const double d = value.as_double();
          const double whole = std::floor(d);
          auto nanos = static_cast<std::int64_t>(std::llround((d - whole) * 1e9));
          auto seconds = static_cast<std::int64_t>(whole);
          if (nanos >= 1'000'000'000) {
            nanos -= 1'000'000'000;
            ++seconds;
          }
          return make_date(format_datetime(from_unix(seconds, static_cast<std::uint32_t>(nanos))));
        }
        break;
      }
      case 100:
        if (value.is_integer() && value.get_int64() && std::abs(*value.get_int64()) < 1'000'000'000) {
          DateTime date = from_unix(*value.get_int64() * 86400);
          date.kind = DateTime::Kind::Date;
          return make_date(format_datetime(date));
        }
        break;
      case 1004:
        if (value.is_string()) {
          if (const auto parsed = parse_datetime(value.as_string()); parsed && parsed->kind == DateTime::Kind::Date) {
            return make_date(format_datetime(*parsed));
          }
        }
        break;
      case 2:
      case 3: {
        if (tag_kind(value) != TagKind::Binary) break;
        const auto bytes = tagged_bytes(value);
        if (!bytes || bytes->size() > 4096) break;
        // Tag 3 is -1 - n.
        std::string digits = magnitude_to_decimal(*bytes, false);
        if (tag == 3) {
          // -1 - n: add one to the magnitude.
          Bytes plus(bytes->begin(), bytes->end());
          plus.insert(plus.begin(), std::byte{0});
          for (std::size_t i = plus.size(); i-- > 0;) {
            const unsigned v = std::to_integer<unsigned>(plus[i]) + 1;
            plus[i] = static_cast<std::byte>(v & 0xFF);
            if (v <= 0xFF) break;
          }
          digits = magnitude_to_decimal(plus, true);
        }
        return number_from_literal(digits);
      }
      case 4: {
        if (!value.is_array() || value.size() != 2 || !value[0].is_integer()) break;
        const Json& mantissa = value[1];
        std::string digits;
        if (mantissa.is_integer()) {
          digits = format_integer(mantissa);
        } else if (tag_kind(mantissa) == TagKind::BigInt) {
          digits = std::string(tag_text(mantissa));
        } else {
          break;
        }
        const bool negative = digits.starts_with('-');
        if (negative) digits.erase(0, 1);
        const std::int64_t exponent = value[0].as_int64();
        if (std::abs(exponent) > 1'000'000'000) break;
        return decimal_value(negative, digits, exponent);
      }
      case 21:
      case 22:
      case 23:
      case 55799: return value;
      default: break;
    }
    return make_cbor_tag(tag, std::move(value));
  }

  Result<Item> read_item(std::size_t depth) {
    if (depth > policy_.max_depth) {
      return fail(Status::Limit, "the CBOR data is nested more than {} levels deep (option max_depth)", policy_.max_depth);
    }
    if (reader_.at_end()) return truncated();
    CVT_TRY(budget_.take());
    const std::uint8_t head = reader_.u8();
    const std::uint8_t major = head >> 5;
    const std::uint8_t info = head & 0x1F;
    if (head == 0xFF) return Item{Json(), true};
    bool indefinite = false;
    if (major == 7) {
      switch (info) {
        case 20: return Item{Json(false)};
        case 21: return Item{Json(true)};
        case 22: return Item{Json()};
        case 23: return Item{make_undefined()};
        case 24: {
          if (reader_.at_end()) return truncated();
          const std::uint8_t simple = reader_.u8();
          if (simple < 32) return fail(Status::Decode, "invalid two-byte CBOR simple value {}", simple);
          ++simple_;
          return Item{Json()};
        }
        case 25:
          if (reader_.remaining() < 2) return truncated();
          return Item{Json(half_to_double(reader_.u16be()))};
        case 26:
          if (reader_.remaining() < 4) return truncated();
          return Item{Json(static_cast<double>(reader_.f32be()))};
        case 27:
          if (reader_.remaining() < 8) return truncated();
          return Item{Json(reader_.f64be())};
        default:
          if (info < 20) {
            ++simple_;
            return Item{Json()};
          }
          return fail(Status::Decode, "reserved CBOR simple value at byte {}", reader_.position() - 1);
      }
    }
    CVT_TRY_ASSIGN(const std::uint64_t arg, argument(info, indefinite));
    switch (major) {
      case 0:
        if (indefinite) break;
        if (arg <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
          return Item{Json(static_cast<std::int64_t>(arg))};
        }
        return Item{Json(arg)};
      case 1:
        if (indefinite) break;
        if (arg <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
          return Item{Json(-1 - static_cast<std::int64_t>(arg))};
        }
        {
          Bytes magnitude(8);
          for (int i = 0; i < 8; ++i) magnitude[static_cast<std::size_t>(i)] = static_cast<std::byte>((arg >> (56 - 8 * i)) & 0xFF);
          // -1 - arg = -(arg + 1)
          magnitude.insert(magnitude.begin(), std::byte{0});
          for (std::size_t i = magnitude.size(); i-- > 0;) {
            const unsigned v = std::to_integer<unsigned>(magnitude[i]) + 1;
            magnitude[i] = static_cast<std::byte>(v & 0xFF);
            if (v <= 0xFF) break;
          }
          return Item{make_bigint(magnitude_to_decimal(magnitude, true))};
        }
      case 2: {
        CVT_TRY_ASSIGN(const std::string bytes, string_bytes(2, arg, indefinite, depth));
        return Item{make_binary(as_bytes(bytes))};
      }
      case 3: {
        CVT_TRY_ASSIGN(const std::string text, string_bytes(3, arg, indefinite, depth));
        return Item{text_value(text, repaired_)};
      }
      case 4: {
        Json array = Json::array();
        if (!indefinite && arg > reader_.remaining()) return truncated();
        for (std::uint64_t i = 0; indefinite || i < arg; ++i) {
          CVT_TRY_ASSIGN(Item element, read_item(depth + 1));
          if (element.is_break) {
            if (!indefinite) return fail(Status::Decode, "unexpected CBOR break code inside a definite array");
            break;
          }
          array.push_back(std::move(element.value));
        }
        return Item{std::move(array)};
      }
      case 5: {
        Json object = Json::object();
        if (!indefinite && arg > reader_.remaining() / 2) return truncated();
        for (std::uint64_t i = 0; indefinite || i < arg; ++i) {
          CVT_TRY_ASSIGN(Item key, read_item(depth + 1));
          if (key.is_break) {
            if (!indefinite) return fail(Status::Decode, "unexpected CBOR break code inside a definite map");
            break;
          }
          CVT_TRY_ASSIGN(Item value, read_item(depth + 1));
          if (value.is_break) return fail(Status::Decode, "a CBOR map key has no value");
          if (!key.value.is_string()) ++keys_converted_;
          CVT_TRY(put_member(object, key_from_value(key.value), std::move(value.value), policy_));
        }
        return Item{std::move(object)};
      }
      case 6: {
        if (indefinite) break;
        CVT_TRY_ASSIGN(Item inner, read_item(depth + 1));
        if (inner.is_break) return fail(Status::Decode, "a CBOR tag has no content");
        CVT_TRY_ASSIGN(Json value, tagged(arg, std::move(inner.value)));
        return Item{std::move(value)};
      }
      default: break;
    }
    return fail(Status::Decode, "invalid indefinite length for CBOR major type {} at byte {}", major, reader_.position() - 1);
  }

  util::ByteReader reader_;
  const ReadContext& ctx_;
  ReadPolicy policy_;
  NodeBudget budget_;
  std::size_t repaired_ = 0;
  std::size_t keys_converted_ = 0;
  std::size_t simple_ = 0;
};

Result<Json> parse_cbor(ByteView input, ReadContext& ctx) {
  CborReader reader(input, ctx);
  CVT_TRY_ASSIGN(std::vector<Json> documents, reader.read_all());
  return select_documents(std::move(documents), ctx.options, "CBOR");
}

// =========================================================================== writer

class CborWriter {
 public:
  CborWriter(bool shortest_floats, bool epoch_dates, const WriteContext& ctx)
      : shortest_(shortest_floats), epoch_dates_(epoch_dates), ctx_(ctx) {}

  Result<Bytes> run(const Json& value) {
    CVT_TRY(write(value));
    if (downgraded_ > 0) {
      ctx_.warn(std::format("{} date(s) without a time zone or date were written as text.", downgraded_));
    }
    return out_.take();
  }

 private:
  void head(std::uint8_t major, std::uint64_t value) {
    const auto m = static_cast<std::uint8_t>(major << 5);
    if (value < 24) {
      out_.u8(static_cast<std::uint8_t>(m | value));
    } else if (value <= 0xFF) {
      out_.u8(m | 24).u8(static_cast<std::uint8_t>(value));
    } else if (value <= 0xFFFF) {
      out_.u8(m | 25).u16be(static_cast<std::uint16_t>(value));
    } else if (value <= 0xFFFFFFFFull) {
      out_.u8(m | 26).u32be(static_cast<std::uint32_t>(value));
    } else {
      out_.u8(m | 27).u64be(value);
    }
  }

  void text(std::string_view s) {
    const std::string valid = util::is_valid_utf8(s) ? std::string(s) : repair_utf8(s);
    head(3, valid.size());
    out_.text(valid);
  }

  void integer(std::int64_t v) {
    if (v >= 0) {
      head(0, static_cast<std::uint64_t>(v));
    } else {
      head(1, static_cast<std::uint64_t>(-(v + 1)));
    }
  }

  void floating(double d) {
    if (shortest_) {
      if (const auto half = exact_half(d)) {
        out_.u8(0xF9).u16be(*half);
        return;
      }
      if (exact_float(d)) {
        out_.u8(0xFA).f32be(static_cast<float>(d));
        return;
      }
    }
    out_.u8(0xFB).f64be(d);
  }

  /// Integer from decimal digits (with '-'): a head when it fits 64 bits, else a bignum.
  void big_integer(std::string_view digits) {
    bool negative = false;
    auto magnitude = decimal_to_magnitude(digits, &negative);
    if (!magnitude) {
      text(digits);
      return;
    }
    if (negative) {
      // Tag 3 holds n where the value is -1 - n: subtract one from the magnitude.
      for (std::size_t i = magnitude->size(); i-- > 0;) {
        const unsigned v = std::to_integer<unsigned>((*magnitude)[i]);
        if (v > 0) {
          (*magnitude)[i] = static_cast<std::byte>(v - 1);
          break;
        }
        (*magnitude)[i] = std::byte{0xFF};
      }
      const auto first = std::ranges::find_if(*magnitude, [](std::byte b) { return b != std::byte{0}; });
      magnitude->erase(magnitude->begin(), first);
    }
    if (magnitude->size() <= 8) {
      std::uint64_t v = 0;
      for (const std::byte b : *magnitude) v = (v << 8) | std::to_integer<std::uint64_t>(b);
      head(negative ? 1 : 0, v);
      return;
    }
    head(6, negative ? 3 : 2);
    head(2, magnitude->size());
    out_.bytes(*magnitude);
  }

  Result<void> write(const Json& value) {
    switch (tag_kind(value)) {
      case TagKind::None: break;
      case TagKind::Binary:
      case TagKind::Ext: {
        const auto bytes = tagged_bytes(value);
        if (!bytes) return fail(Status::Encode, "invalid Base64 in a binary value");
        head(2, bytes->size());
        out_.bytes(*bytes);
        return {};
      }
      case TagKind::Date: {
        const std::string iso(tag_text(value));
        const auto parsed = parse_datetime(iso);
        if (parsed && parsed->kind == DateTime::Kind::Offset) {
          if (epoch_dates_) {
            head(6, 1);
            const std::int64_t seconds = to_unix_seconds(*parsed);
            if (parsed->nanos() == 0) {
              integer(seconds);
            } else {
              out_.u8(0xFB).f64be(static_cast<double>(seconds) + parsed->nanos() / 1e9);
            }
          } else {
            head(6, 0);
            text(format_datetime(*parsed));
          }
          return {};
        }
        if (parsed && parsed->kind == DateTime::Kind::Date) {
          head(6, 1004);
          text(iso);
          return {};
        }
        ++downgraded_;
        text(iso);
        return {};
      }
      case TagKind::BigInt: big_integer(tag_text(value)); return {};
      case TagKind::Decimal: {
        const auto parts = decimal_parts(tag_text(value));
        if (!parts) {
          text(tag_text(value));
          return {};
        }
        head(6, 4);
        head(4, 2);
        integer(parts->exponent);
        big_integer((parts->negative ? "-" : "") + parts->digits);
        return {};
      }
      case TagKind::Float: floating(plain_value(value).as_double()); return {};
      case TagKind::Undefined: out_.u8(0xF7); return {};
      case TagKind::Uid: head(0, value[ir::data_tags::uid].as_uint64()); return {};
      case TagKind::CborTag:
        head(6, value[ir::data_tags::tag].as_uint64());
        return write(value[ir::data_tags::tag_value]);
      default: break;
    }
    switch (value.type()) {
      case Json::Type::Null: out_.u8(0xF6); return {};
      case Json::Type::Bool: out_.u8(value.as_bool() ? 0xF5 : 0xF4); return {};
      case Json::Type::Int64: integer(value.as_int64()); return {};
      case Json::Type::UInt64: head(0, value.as_uint64()); return {};
      case Json::Type::Double: floating(value.as_double()); return {};
      case Json::Type::String: text(value.as_string()); return {};
      case Json::Type::Array:
        head(4, value.size());
        for (const Json& element : value.elements()) CVT_TRY(write(element));
        return {};
      case Json::Type::Object:
        head(5, value.size());
        for (const auto& member : value.items()) {
          text(member.key());
          CVT_TRY(write(member.value()));
        }
        return {};
    }
    return {};
  }

  bool shortest_ = true;
  bool epoch_dates_ = false;
  const WriteContext& ctx_;
  util::ByteWriter out_;
  std::size_t downgraded_ = 0;
};

Result<Bytes> write_cbor(const Json& value, WriteContext& ctx) {
  CborWriter writer(ctx.options.get_enum("float_size", "auto", {"auto", "64"}) == "auto",
                    ctx.options.get_enum("cbor_dates", "string", {"string", "epoch"}) == "epoch", ctx);
  util::ByteWriter prefix;
  CVT_TRY_ASSIGN(Bytes body, writer.run(value));
  if (ctx.options.get_bool("self_describe", false)) {
    prefix.u8(0xD9).u8(0xD9).u8(0xF7);
    prefix.bytes(body);
    return prefix.take();
  }
  return body;
}

}  // namespace

void register_cbor(Registry& registry) {
  registry.add_format(FormatInfo{
      .id = "cbor",
      .name = "CBOR",
      .description = "Concise Binary Object Representation (RFC 8949).",
      .category = Category::Data,
      .extensions = {"cbor"},
      .mime = "application/cbor",
      .popularity = 30,
      .signatures = {Signature::at(0, "D9 D9 F7")},
      .sniff = [](ByteView, std::string_view filename) { return has_extension(filename, {"cbor"}) ? 75 : 0; },
  });
  std::vector<OptionSpec> read_options = common_read_options(true, false);
  read_options.push_back(documents_option());
  read_options.push_back(document_index_option());
  add_codec(registry,
            {.format_id = "cbor",
             .read = binary_reader(parse_cbor),
             .write = binary_writer(write_cbor, "CBOR"),
             .read_options = std::move(read_options),
             .write_options = {
                 OptionSpec::choice("float_size", "Floating-point size", "auto", {"auto", "64"},
                                    "auto writes the shortest float (half, single, double) that holds each value "
                                    "exactly; 64 always writes doubles.")
                     .advanced(),
                 OptionSpec::choice("cbor_dates", "Dates", "string", {"string", "epoch"},
                                    "Date-times as RFC 3339 text (tag 0, keeps the time zone) or as seconds since "
                                    "1970 (tag 1)."),
                 OptionSpec::boolean("self_describe", "Self-describe tag", false,
                                     "Start the file with the 55799 tag that marks it as CBOR.")
                     .advanced(),
             }});
}

}  // namespace cvt::data
