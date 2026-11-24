// Convertible core - data family: Smile (Jackson's binary JSON, format 1.0).
//
// Reader: all value and key tokens, shared key names and shared string values
// (back references, with the 1024-entry reset rule), 32/64-bit integers,
// BigInteger -> $bigint, float/double, BigDecimal -> $decimal, 7-bit and raw
// binary -> $binary, long strings, and the end-of-content marker. Several
// concatenated documents are read as documents.
// Writer: header with sharing disabled, the shortest token for every value.
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

constexpr std::size_t shared_limit = 1024;

class SmileReader {
 public:
  SmileReader(ByteView data, const ReadContext& ctx)
      : reader_(data), ctx_(ctx), policy_(read_policy(ctx.options)), budget_(data.size()) {}

  Result<std::vector<Json>> read_all() {
    std::vector<Json> documents;
    if (!reader_.starts_with(":)\n")) return fail(Status::Decode, "not a Smile document (missing ':)' header)");
    while (!reader_.at_end()) {
      if (reader_.starts_with(":)\n")) {
        (void)reader_.bytes(3);
        if (reader_.at_end()) return truncated();
        const std::uint8_t flags = reader_.u8();
        if ((flags >> 4) != 0) return fail(Status::Decode, "unsupported Smile version {}", flags >> 4);
        shared_names_ = (flags & 1) != 0;
        shared_values_ = (flags & 2) != 0;
        names_.clear();
        values_.clear();
      }
      if (reader_.at_end()) break;
      if (reader_.peek_u8() == 0xFF) {
        (void)reader_.u8();
        continue;
      }
      CVT_TRY_ASSIGN(Json value, value_of(reader_.u8(), 0));
      documents.push_back(std::move(value));
      CVT_TRY(ctx_.checkpoint(static_cast<double>(reader_.position()) / static_cast<double>(reader_.size())));
    }
    if (documents.empty()) return fail(Status::Decode, "the Smile document holds no value");
    return documents;
  }

 private:
  std::unexpected<Error> truncated() const {
    return fail(Status::Decode, "the Smile data is truncated at byte {}", reader_.position());
  }
  std::unexpected<Error> bad(std::string_view what) const {
    return fail(Status::Decode, "invalid Smile at byte {}: {}", reader_.position(), what);
  }

  Result<std::uint64_t> vint() {
    std::uint64_t value = 0;
    for (int i = 0; i < 10; ++i) {
      if (reader_.at_end()) return truncated();
      const std::uint8_t b = reader_.u8();
      if ((b & 0x80) != 0) return (value << 6) | (b & 0x3F);
      value = (value << 7) | b;
    }
    return bad("variable-length integer too long");
  }

  Result<std::uint64_t> seven_bit_word(std::size_t bytes) {
    if (bytes > reader_.remaining()) return truncated();
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < bytes; ++i) {
      const std::uint8_t b = reader_.u8();
      if (b > 0x7F) return bad("invalid 7-bit encoded byte");
      value = (value << 7) | b;
    }
    return value;
  }

  /// 7-bit encoded payload of `raw` bytes.
  Result<Bytes> seven_bit_bytes(std::uint64_t raw) {
    const std::uint64_t encoded = raw / 7 * 8 + (raw % 7 == 0 ? 0 : raw % 7 + 1);
    if (raw > reader_.remaining() || encoded > reader_.remaining()) return truncated();
    Bytes out;
    out.reserve(static_cast<std::size_t>(raw));
    std::uint64_t left = raw;
    while (left >= 7) {
      CVT_TRY_ASSIGN(const std::uint64_t bits, seven_bit_word(8));
      for (int shift = 48; shift >= 0; shift -= 8) out.push_back(static_cast<std::byte>((bits >> shift) & 0xFF));
      left -= 7;
    }
    if (left > 0) {
      CVT_TRY_ASSIGN(std::uint64_t bits, seven_bit_word(static_cast<std::size_t>(left)));
      if (reader_.at_end()) return truncated();
      const std::uint8_t last = reader_.u8();
      bits = (bits << left) | (last & ((1u << left) - 1));
      for (std::uint64_t i = left; i-- > 0;) out.push_back(static_cast<std::byte>((bits >> (8 * i)) & 0xFF));
    }
    return out;
  }

  Result<std::string> raw_text(std::size_t size) {
    if (size > reader_.remaining()) return truncated();
    return std::string(reader_.chars(size));
  }

  Result<std::string> until_end_marker() {
    const std::string_view rest = as_chars(reader_.peek_rest());
    const std::size_t end = rest.find('\xFC');
    if (end == std::string_view::npos) return truncated();
    std::string text(rest.substr(0, end));
    (void)reader_.bytes(end + 1);
    return text;
  }

  Json string_value(std::string text) {
    if (!util::is_valid_utf8(text)) text = repair_utf8(text);
    return Json(std::move(text));
  }

  void remember(std::vector<std::string>& table, const std::string& text) {
    if (table.size() >= shared_limit) table.clear();
    table.push_back(text);
  }

  Result<Json> shared_value(std::size_t index) {
    if (!shared_values_ || index >= values_.size()) return bad("shared string reference out of range");
    return Json(values_[index]);
  }

  Result<Json> value_of(std::uint8_t token, std::size_t depth) {
    if (depth > policy_.max_depth) {
      return fail(Status::Limit, "the Smile data is nested more than {} levels deep (option max_depth)", policy_.max_depth);
    }
    CVT_TRY(budget_.take());
    if (token >= 0x01 && token <= 0x1F) return shared_value(token - 1u);
    if (token >= 0x40 && token <= 0xBF) {
      // Tiny / small ASCII and Unicode strings (shared when enabled).
      std::size_t size = 0;
      if (token < 0x60) {
        size = (token & 0x1F) + 1u;
      } else if (token < 0x80) {
        size = (token & 0x1F) + 33u;
      } else if (token < 0xA0) {
        size = (token & 0x1F) + 2u;
      } else {
        size = (token & 0x1F) + 34u;
      }
      CVT_TRY_ASSIGN(std::string text, raw_text(size));
      if (shared_values_) remember(values_, text);
      return string_value(std::move(text));
    }
    if (token >= 0xC0 && token <= 0xDF) return Json(unzigzag(token & 0x1F));
    switch (token) {
      case 0x20: return Json("");
      case 0x21: return Json();
      case 0x22: return Json(false);
      case 0x23: return Json(true);
      case 0x24: {
        CVT_TRY_ASSIGN(const std::uint64_t v, vint());
        if (v > 0xFFFFFFFFu) return bad("32-bit integer out of range");
        return Json(unzigzag(v));
      }
      case 0x25: {
        CVT_TRY_ASSIGN(const std::uint64_t v, vint());
        return Json(unzigzag(v));
      }
      case 0x26: {
        CVT_TRY_ASSIGN(const std::uint64_t size, vint());
        if (size > 4096) return bad("BigInteger too long");
        CVT_TRY_ASSIGN(const Bytes bytes, seven_bit_bytes(size));
        return number_from_literal(twos_complement_to_decimal(bytes));
      }
      case 0x28: {
        CVT_TRY_ASSIGN(const std::uint64_t bits, seven_bit_word(5));
        return Json(static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(bits))));
      }
      case 0x29: {
        CVT_TRY_ASSIGN(const std::uint64_t bits, seven_bit_word(10));
        return Json(std::bit_cast<double>(bits));
      }
      case 0x2A: {
        CVT_TRY_ASSIGN(const std::uint64_t scale_bits, vint());
        const std::int64_t scale = unzigzag(scale_bits);
        CVT_TRY_ASSIGN(const std::uint64_t size, vint());
        if (size > 4096) return bad("BigDecimal too long");
        CVT_TRY_ASSIGN(const Bytes bytes, seven_bit_bytes(size));
        std::string digits = twos_complement_to_decimal(bytes);
        const bool negative = digits.starts_with('-');
        if (negative) digits.erase(0, 1);
        return decimal_value(negative, digits, -scale);
      }
      case 0xE0:
      case 0xE4: {
        CVT_TRY_ASSIGN(std::string text, until_end_marker());
        return string_value(std::move(text));
      }
      case 0xE8: {
        CVT_TRY_ASSIGN(const std::uint64_t size, vint());
        CVT_TRY_ASSIGN(const Bytes bytes, seven_bit_bytes(size));
        return make_binary(bytes);
      }
      case 0xEC:
      case 0xED:
      case 0xEE:
      case 0xEF: {
        if (reader_.at_end()) return truncated();
        return shared_value((static_cast<std::size_t>(token & 3) << 8) | reader_.u8());
      }
      case 0xF8: {
        Json array = Json::array();
        while (true) {
          if (reader_.at_end()) return truncated();
          const std::uint8_t next = reader_.u8();
          if (next == 0xF9) return array;
          CVT_TRY_ASSIGN(Json element, value_of(next, depth + 1));
          array.push_back(std::move(element));
        }
      }
      case 0xFA: return object(depth);
      case 0xFD: {
        CVT_TRY_ASSIGN(const std::uint64_t size, vint());
        if (size > reader_.remaining()) return truncated();
        return make_binary(reader_.bytes(static_cast<std::size_t>(size)));
      }
      default: break;
    }
    return bad(std::format("unexpected token 0x{:02X}", token));
  }

  Result<std::string> key(std::uint8_t token) {
    if (token == 0x20) return std::string();
    if (token >= 0x30 && token <= 0x33) {
      if (reader_.at_end()) return truncated();
      const std::size_t index = (static_cast<std::size_t>(token & 3) << 8) | reader_.u8();
      if (!shared_names_ || index >= names_.size()) return bad("shared key reference out of range");
      return names_[index];
    }
    if (token == 0x34) {
      CVT_TRY_ASSIGN(std::string text, until_end_marker());
      return text;
    }
    if (token >= 0x40 && token <= 0x7F) {
      const std::size_t index = token & 0x3F;
      if (!shared_names_ || index >= names_.size()) return bad("shared key reference out of range");
      return names_[index];
    }
    if (token >= 0x80 && token <= 0xF7) {
      const std::size_t size = token < 0xC0 ? (token & 0x3F) + 1u : (token & 0x3F) + 2u;
      CVT_TRY_ASSIGN(std::string text, raw_text(size));
      if (shared_names_) remember(names_, text);
      return text;
    }
    return bad(std::format("unexpected key token 0x{:02X}", token));
  }

  Result<Json> object(std::size_t depth) {
    Json out = Json::object();
    while (true) {
      if (reader_.at_end()) return truncated();
      const std::uint8_t token = reader_.u8();
      if (token == 0xFB) return out;
      CVT_TRY_ASSIGN(std::string name, key(token));
      if (!util::is_valid_utf8(name)) name = repair_utf8(name);
      if (reader_.at_end()) return truncated();
      CVT_TRY_ASSIGN(Json value, value_of(reader_.u8(), depth + 1));
      CVT_TRY(put_member(out, std::move(name), std::move(value), policy_));
    }
  }

  util::ByteReader reader_;
  const ReadContext& ctx_;
  ReadPolicy policy_;
  NodeBudget budget_;
  bool shared_names_ = false;
  bool shared_values_ = false;
  std::vector<std::string> names_;
  std::vector<std::string> values_;
};

Result<Json> parse_smile(ByteView input, ReadContext& ctx) {
  SmileReader reader(input, ctx);
  CVT_TRY_ASSIGN(std::vector<Json> documents, reader.read_all());
  return select_documents(std::move(documents), ctx.options, "Smile");
}

// =========================================================================== writer

class SmileWriter {
 public:
  Result<Bytes> run(const Json& value) {
    out_.text(":)\n").u8(0x00);
    CVT_TRY(write(value));
    return out_.take();
  }

 private:
  void vint(std::uint64_t value) {
    // Last byte: 6 bits with the high bit set; before it 7-bit groups.
    std::uint8_t groups[10];
    std::size_t count = 0;
    groups[count++] = static_cast<std::uint8_t>(0x80 | (value & 0x3F));
    value >>= 6;
    while (value != 0) {
      groups[count++] = static_cast<std::uint8_t>(value & 0x7F);
      value >>= 7;
    }
    while (count-- > 0) out_.u8(groups[count]);
  }

  void seven_bit_word(std::uint64_t bits, std::size_t bytes) {
    for (std::size_t i = bytes; i-- > 0;) out_.u8(static_cast<std::uint8_t>((bits >> (7 * i)) & 0x7F));
  }

  void seven_bit_bytes(ByteView data) {
    std::size_t i = 0;
    for (; i + 7 <= data.size(); i += 7) {
      std::uint64_t bits = 0;
      for (std::size_t k = 0; k < 7; ++k) bits = (bits << 8) | std::to_integer<std::uint64_t>(data[i + k]);
      seven_bit_word(bits, 8);
    }
    const std::size_t left = data.size() - i;
    if (left > 0) {
      std::uint64_t bits = 0;
      for (std::size_t k = 0; k < left; ++k) bits = (bits << 8) | std::to_integer<std::uint64_t>(data[i + k]);
      seven_bit_word(bits >> left, left);
      out_.u8(static_cast<std::uint8_t>(bits & ((1u << left) - 1)));
    }
  }

  static Bytes twos_complement(std::string_view digits) {
    bool negative = false;
    Bytes magnitude = decimal_to_magnitude(digits, &negative).value_or(Bytes{});
    magnitude.insert(magnitude.begin(), std::byte{0});  // room for the sign bit
    if (negative) {
      for (std::byte& b : magnitude) b = ~b;
      for (std::size_t i = magnitude.size(); i-- > 0;) {
        const unsigned v = std::to_integer<unsigned>(magnitude[i]) + 1;
        magnitude[i] = static_cast<std::byte>(v & 0xFF);
        if (v <= 0xFF) break;
      }
    }
    // Drop redundant sign bytes.
    while (magnitude.size() > 1) {
      const auto first = std::to_integer<unsigned>(magnitude[0]);
      const auto second = std::to_integer<unsigned>(magnitude[1]);
      if ((first == 0 && (second & 0x80) == 0) || (first == 0xFF && (second & 0x80) != 0)) {
        magnitude.erase(magnitude.begin());
      } else {
        break;
      }
    }
    return magnitude;
  }

  void big_integer(std::string_view digits) {
    const Bytes bytes = twos_complement(digits);
    out_.u8(0x26);
    vint(bytes.size());
    seven_bit_bytes(bytes);
  }

  void string(std::string_view text) {
    const std::string valid = util::is_valid_utf8(text) ? std::string(text) : repair_utf8(text);
    const std::size_t size = valid.size();
    const bool ascii = std::ranges::all_of(valid, [](char c) { return static_cast<unsigned char>(c) < 0x80; });
    if (size == 0) {
      out_.u8(0x20);
      return;
    }
    if (ascii && size <= 32) {
      out_.u8(static_cast<std::uint8_t>(0x40 + size - 1));
    } else if (ascii && size <= 64) {
      out_.u8(static_cast<std::uint8_t>(0x60 + size - 33));
    } else if (!ascii && size >= 2 && size <= 33) {
      out_.u8(static_cast<std::uint8_t>(0x80 + size - 2));
    } else if (!ascii && size >= 34 && size <= 65) {
      out_.u8(static_cast<std::uint8_t>(0xA0 + size - 34));
    } else {
      out_.u8(ascii ? 0xE0 : 0xE4).text(valid).u8(0xFC);
      return;
    }
    out_.text(valid);
  }

  void key(std::string_view name) {
    const std::string valid = util::is_valid_utf8(name) ? std::string(name) : repair_utf8(name);
    const std::size_t size = valid.size();
    const bool ascii = std::ranges::all_of(valid, [](char c) { return static_cast<unsigned char>(c) < 0x80; });
    if (size == 0) {
      out_.u8(0x20);
    } else if (ascii && size <= 64) {
      out_.u8(static_cast<std::uint8_t>(0x80 + size - 1)).text(valid);
    } else if (!ascii && size >= 2 && size <= 57) {
      out_.u8(static_cast<std::uint8_t>(0xC0 + size - 2)).text(valid);
    } else {
      out_.u8(0x34).text(valid).u8(0xFC);
    }
  }

  void integer(std::int64_t v) {
    if (v >= -16 && v <= 15) {
      out_.u8(static_cast<std::uint8_t>(0xC0 + zigzag(v)));
    } else if (v >= std::numeric_limits<std::int32_t>::min() && v <= std::numeric_limits<std::int32_t>::max()) {
      out_.u8(0x24);
      vint(zigzag(v));
    } else {
      out_.u8(0x25);
      vint(zigzag(v));
    }
  }

  Result<void> write(const Json& value) {
    switch (tag_kind(value)) {
      case TagKind::None: break;
      case TagKind::Binary:
      case TagKind::Ext: {
        const auto bytes = tagged_bytes(value);
        if (!bytes) return fail(Status::Encode, "invalid Base64 in a binary value");
        out_.u8(0xE8);
        vint(bytes->size());
        seven_bit_bytes(*bytes);
        return {};
      }
      case TagKind::BigInt: big_integer(tag_text(value)); return {};
      case TagKind::Decimal: {
        const auto parts = decimal_parts(tag_text(value));
        if (!parts || parts->exponent < std::numeric_limits<std::int32_t>::min() + 1 ||
            parts->exponent > std::numeric_limits<std::int32_t>::max()) {
          string(tag_text(value));
          return {};
        }
        out_.u8(0x2A);
        vint(zigzag(-parts->exponent));
        const Bytes bytes = twos_complement((parts->negative ? "-" : "") + parts->digits);
        vint(bytes.size());
        seven_bit_bytes(bytes);
        return {};
      }
      case TagKind::Float: return write(plain_value(value));
      case TagKind::Date: string(tag_text(value)); return {};
      case TagKind::Undefined: out_.u8(0x21); return {};
      default: break;
    }
    switch (value.type()) {
      case Json::Type::Null: out_.u8(0x21); return {};
      case Json::Type::Bool: out_.u8(value.as_bool() ? 0x23 : 0x22); return {};
      case Json::Type::Int64: integer(value.as_int64()); return {};
      case Json::Type::UInt64: big_integer(format_integer(value)); return {};
      case Json::Type::Double:
        out_.u8(0x29);
        seven_bit_word(std::bit_cast<std::uint64_t>(value.as_double()), 10);
        return {};
      case Json::Type::String: string(value.as_string()); return {};
      case Json::Type::Array:
        out_.u8(0xF8);
        for (const Json& element : value.elements()) CVT_TRY(write(element));
        out_.u8(0xF9);
        return {};
      case Json::Type::Object:
        out_.u8(0xFA);
        for (const auto& member : value.items()) {
          key(member.key());
          CVT_TRY(write(member.value()));
        }
        out_.u8(0xFB);
        return {};
    }
    return {};
  }

  util::ByteWriter out_;
};

Result<Bytes> write_smile(const Json& value, WriteContext&) {
  SmileWriter writer;
  return writer.run(value);
}

}  // namespace

void register_smile(Registry& registry) {
  registry.add_format(FormatInfo{
      .id = "smile",
      .name = "Smile",
      .description = "Jackson's binary JSON format.",
      .category = Category::Data,
      .extensions = {"sml", "smile"},
      .mime = "application/x-jackson-smile",
      .popularity = 8,
      .signatures = {Signature::at(0, "':)' 0A 0?")},
      .sniff = nullptr,
  });
  std::vector<OptionSpec> read_options = common_read_options(true, false);
  read_options.push_back(documents_option());
  read_options.push_back(document_index_option());
  add_codec(registry, {.format_id = "smile",
                       .read = binary_reader(parse_smile),
                       .write = binary_writer(write_smile, "Smile"),
                       .read_options = std::move(read_options)});
}

}  // namespace cvt::data
