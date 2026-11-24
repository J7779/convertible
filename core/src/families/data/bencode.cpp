// Convertible core - data family: bencode (BitTorrent .torrent files).
//
// i<integer>e (any size), <length>:<bytes>, l...e lists and d...e
// dictionaries. Byte strings that are valid UTF-8 become strings, others
// (such as the SHA-1 "pieces" of a torrent) become $binary - or always
// $binary with `binary_strings`. The writer sorts dictionary keys as the
// format requires; it has no floats, booleans or null (see the warnings).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "engine/registry.hpp"
#include "families/data/binary_common.hpp"
#include "families/data/data.hpp"
#include "util/base64.hpp"
#include "util/bytes.hpp"

namespace cvt::data {

namespace {

class BencodeReader {
 public:
  BencodeReader(ByteView data, const ReadContext& ctx)
      : text_(as_chars(data)), policy_(read_policy(ctx.options)), budget_(data.size()),
        always_binary_(ctx.options.get_enum("binary_strings", "auto", {"auto", "always"}) == "always") {}

  Result<Json> parse() {
    CVT_TRY_ASSIGN(Json value, read_value(0));
    if (p_ != text_.size()) return bad("unexpected data after the end of the value");
    return value;
  }

 private:
  std::unexpected<Error> bad(std::string_view what) const {
    return fail(Status::Decode, "invalid bencode at byte {}: {}", p_, what);
  }

  Result<std::string_view> bytes() {
    const std::size_t colon = text_.find(':', p_);
    if (colon == std::string_view::npos || colon == p_ || colon - p_ > 18) return bad("bad string length");
    const std::string_view digits = text_.substr(p_, colon - p_);
    if (!std::ranges::all_of(digits, [](char c) { return util::is_ascii_digit(c); }) ||
        (digits.size() > 1 && digits[0] == '0')) {
      return bad("bad string length");
    }
    const auto size = static_cast<std::uint64_t>(util::parse_int(digits).value_or(0));
    if (size > text_.size() - colon - 1) return bad("string runs past the end of the data");
    p_ = colon + 1 + static_cast<std::size_t>(size);
    return text_.substr(colon + 1, static_cast<std::size_t>(size));
  }

  Json string_value(std::string_view raw) const {
    // Text: valid UTF-8 without control characters (hashes and keys stay binary).
    const bool text = util::is_valid_utf8(raw) && std::ranges::none_of(raw, [](char c) {
      const auto u = static_cast<unsigned char>(c);
      return (u < 0x20 && c != '\t' && c != '\n' && c != '\r') || u == 0x7F;
    });
    if (!always_binary_ && text) return Json(std::string(raw));
    return make_binary(as_bytes(raw));
  }

  Result<Json> read_value(std::size_t depth) {
    if (depth > policy_.max_depth) {
      return fail(Status::Limit, "the bencode data is nested more than {} levels deep (option max_depth)", policy_.max_depth);
    }
    CVT_TRY(budget_.take());
    if (p_ >= text_.size()) return bad("unexpected end of data");
    const char c = text_[p_];
    if (c == 'i') {
      const std::size_t end = text_.find('e', p_);
      if (end == std::string_view::npos) return bad("unterminated integer");
      const std::string_view digits = text_.substr(p_ + 1, end - p_ - 1);
      std::string_view body = digits;
      if (body.starts_with('-')) body.remove_prefix(1);
      if (body.empty() || !std::ranges::all_of(body, [](char d) { return util::is_ascii_digit(d); }) ||
          (body.size() > 1 && body[0] == '0') || digits == "-0") {
        return bad("malformed integer");
      }
      p_ = end + 1;
      return number_from_literal(digits);
    }
    if (c == 'l') {
      ++p_;
      Json list = Json::array();
      while (p_ < text_.size() && text_[p_] != 'e') {
        CVT_TRY_ASSIGN(Json element, read_value(depth + 1));
        list.push_back(std::move(element));
      }
      if (p_ >= text_.size()) return bad("unterminated list");
      ++p_;
      return list;
    }
    if (c == 'd') {
      ++p_;
      Json dict = Json::object();
      while (p_ < text_.size() && text_[p_] != 'e') {
        if (!util::is_ascii_digit(text_[p_])) return bad("a dictionary key must be a string");
        CVT_TRY_ASSIGN(const std::string_view key, bytes());
        CVT_TRY_ASSIGN(Json element, read_value(depth + 1));
        const std::string name = util::is_valid_utf8(key) ? std::string(key) : util::base64_encode(as_bytes(key));
        CVT_TRY(put_member(dict, name, std::move(element), policy_));
      }
      if (p_ >= text_.size()) return bad("unterminated dictionary");
      ++p_;
      return dict;
    }
    if (util::is_ascii_digit(c)) {
      CVT_TRY_ASSIGN(const std::string_view raw, bytes());
      return string_value(raw);
    }
    return bad(std::format("unexpected character '{}'", util::is_ascii_alpha(c) ? c : '?'));
  }

  std::string_view text_;
  ReadPolicy policy_;
  NodeBudget budget_;
  bool always_binary_ = false;
  std::size_t p_ = 0;
};

Result<Json> parse_bencode(ByteView input, ReadContext& ctx) {
  BencodeReader reader(input, ctx);
  return reader.parse();
}

class BencodeWriter {
 public:
  Result<Bytes> run(const Json& value, const WriteContext& ctx) {
    CVT_TRY(write(value));
    if (converted_ > 0) {
      ctx.warn(std::format("bencode has only integers, strings, lists and dictionaries; {} value(s) were "
                           "converted (floats and dates to text, booleans to 0/1) or left out (null).", converted_));
    }
    return out_.take();
  }

 private:
  void bytes(std::string_view raw) {
    out_.text(std::to_string(raw.size()));
    out_.u8(':');
    out_.text(raw);
  }

  Result<void> write(const Json& value) {
    switch (tag_kind(value)) {
      case TagKind::None: break;
      case TagKind::Binary:
      case TagKind::Ext: {
        const auto data = tagged_bytes(value);
        if (!data) return fail(Status::Encode, "invalid Base64 in a binary value");
        bytes(as_chars(*data));
        return {};
      }
      case TagKind::BigInt:
        out_.text("i").text(tag_text(value)).text("e");
        return {};
      default: {
        const Json plain = plain_value(value);
        ++converted_;
        if (plain.is_string()) {
          bytes(plain.as_string());
          return {};
        }
        if (plain.is_structured()) return write(plain);
        return write(plain.is_number() ? Json(format_double(plain.as_double())) : plain);
      }
    }
    switch (value.type()) {
      case Json::Type::Null: ++converted_; return {};
      case Json::Type::Bool:
        ++converted_;
        out_.text(value.as_bool() ? "i1e" : "i0e");
        return {};
      case Json::Type::Int64:
      case Json::Type::UInt64: out_.text("i" + format_integer(value) + "e"); return {};
      case Json::Type::Double: {
        const double d = value.as_double();
        if (std::isfinite(d) && d == std::floor(d) && std::abs(d) < 9.2e18) {
          out_.text("i" + std::to_string(static_cast<std::int64_t>(d)) + "e");
        } else {
          bytes(format_double(d));
        }
        ++converted_;
        return {};
      }
      case Json::Type::String: bytes(value.as_string()); return {};
      case Json::Type::Array:
        out_.u8('l');
        for (const Json& element : value.elements()) {
          if (element.is_null()) {
            ++converted_;
            continue;
          }
          CVT_TRY(write(element));
        }
        out_.u8('e');
        return {};
      case Json::Type::Object: {
        out_.u8('d');
        for (const Json::Member* member : members_of(value, true)) {  // keys must be sorted
          if (member->value().is_null()) {
            ++converted_;
            continue;
          }
          bytes(member->key());
          CVT_TRY(write(member->value()));
        }
        out_.u8('e');
        return {};
      }
    }
    return {};
  }

  util::ByteWriter out_;
  std::size_t converted_ = 0;
};

Result<Bytes> write_bencode(const Json& value, WriteContext& ctx) {
  BencodeWriter writer;
  return writer.run(value, ctx);
}

int sniff_bencode(ByteView data, std::string_view filename) {
  const std::string_view head = as_chars(data).substr(0, 16);
  if (has_extension(filename, {"bencode", "torrent"})) return 75;
  if (head.size() > 3 && head[0] == 'd' && util::is_ascii_digit(head[1])) {
    const std::size_t colon = head.find(':');
    if (colon != std::string_view::npos && colon < 6) return 45;
  }
  return 0;
}

}  // namespace

void register_bencode(Registry& registry) {
  registry.add_format(FormatInfo{
      .id = "bencode",
      .name = "Bencode",
      .description = "The encoding of BitTorrent metadata.",
      .category = Category::Data,
      .extensions = {"bencode"},
      .mime = "application/x-bencode",
      .popularity = 10,
      .signatures = {},
      .sniff = sniff_bencode,
  });
  std::vector<OptionSpec> read_options = common_read_options(true, false);
  read_options.push_back(OptionSpec::choice("binary_strings", "Byte strings", "auto", {"auto", "always"},
                                            "Byte strings that are valid UTF-8 become text (auto) or every byte "
                                            "string is kept as binary data."));
  for (const std::string_view id : {"bencode", "torrent"}) {
    add_codec(registry, {.format_id = std::string(id),
                         .read = binary_reader(parse_bencode),
                         .write = binary_writer(write_bencode, "bencode"),
                         .read_options = read_options});
  }
  registry.add_format(FormatInfo{
      .id = "torrent",
      .name = "BitTorrent metainfo",
      .description = "A .torrent file: tracker, file names and piece hashes in bencode.",
      .category = Category::Data,
      .extensions = {"torrent"},
      .mime = "application/x-bittorrent",
      .popularity = 30,
      .signatures = {Signature::at(0, "'d8:announce'"), Signature::at(0, "'d4:info'"),
                     Signature::at(0, "'d13:announce-list'"), Signature::at(0, "'d10:created by'")},
      .sniff = nullptr,
  });
}

}  // namespace cvt::data
