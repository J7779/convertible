// Convertible core - science family: ASN.1 DER decoding and encoding.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/science/asn1.hpp"

#include <array>
#include <format>

#include "util/hex.hpp"
#include "util/text.hpp"

namespace cvt::science::asn1 {

namespace {

Result<Node> parse_one(ByteView whole, util::ByteReader& reader, int depth) {
  if (depth <= 0) return fail(Status::Limit, "ASN.1 data is nested too deeply");
  const std::size_t start = reader.position();
  if (reader.remaining() < 2) return fail(Status::Decode, "ASN.1 element is truncated");
  Node node;
  const std::uint8_t id = reader.u8();
  node.cls = id >> 6;
  node.constructed = (id & 0x20) != 0;
  node.tag = id & 0x1F;
  if (node.tag == 0x1F) {
    node.tag = 0;
    for (int i = 0;; ++i) {
      if (i > 4 || reader.at_end()) return fail(Status::Decode, "bad ASN.1 tag");
      const std::uint8_t b = reader.u8();
      node.tag = (node.tag << 7) | (b & 0x7F);
      if ((b & 0x80) == 0) break;
    }
  }
  if (reader.at_end()) return fail(Status::Decode, "ASN.1 length is missing");
  const std::uint8_t first = reader.u8();
  std::uint64_t length = 0;
  if (first < 0x80) {
    length = first;
  } else if (first == 0x80) {
    return fail(Status::Decode, "indefinite ASN.1 lengths are not DER");
  } else {
    const int n = first & 0x7F;
    if (n > 8) return fail(Status::Decode, "ASN.1 length is too long");
    for (int i = 0; i < n; ++i) {
      if (reader.at_end()) return fail(Status::Decode, "ASN.1 length is truncated");
      length = (length << 8) | reader.u8();
    }
    if (length < 0x80 || (n > 1 && (length >> (8 * (n - 1))) == 0)) return fail(Status::Decode, "non-minimal DER length");
  }
  if (length > reader.remaining()) return fail(Status::Decode, "ASN.1 element of {} bytes is truncated", length);
  node.value = reader.bytes(static_cast<std::size_t>(length));
  node.header_and_value = whole.subspan(start, reader.position() - start);
  if (node.constructed) {
    util::ByteReader inner(node.value);
    while (!inner.at_end()) {
      CVT_TRY_ASSIGN(Node child, parse_one(node.value, inner, depth - 1));
      node.children.push_back(std::move(child));
      if (node.children.size() > 1'000'000) return fail(Status::Limit, "too many ASN.1 elements");
    }
  }
  return node;
}

constexpr std::pair<std::string_view, std::string_view> oids[] = {
    {"2.5.4.3", "commonName"}, {"2.5.4.4", "surname"}, {"2.5.4.5", "serialNumber"}, {"2.5.4.6", "countryName"},
    {"2.5.4.7", "localityName"}, {"2.5.4.8", "stateOrProvinceName"}, {"2.5.4.9", "streetAddress"}, {"2.5.4.10", "organizationName"},
    {"2.5.4.11", "organizationalUnitName"}, {"2.5.4.12", "title"}, {"2.5.4.42", "givenName"}, {"2.5.4.97", "organizationIdentifier"},
    {"1.2.840.113549.1.9.1", "emailAddress"}, {"0.9.2342.19200300.100.1.25", "domainComponent"}, {"0.9.2342.19200300.100.1.1", "userId"},
    {"1.2.840.113549.1.1.1", "rsaEncryption"}, {"1.2.840.113549.1.1.5", "sha1WithRSAEncryption"},
    {"1.2.840.113549.1.1.10", "rsassaPss"}, {"1.2.840.113549.1.1.11", "sha256WithRSAEncryption"},
    {"1.2.840.113549.1.1.12", "sha384WithRSAEncryption"}, {"1.2.840.113549.1.1.13", "sha512WithRSAEncryption"},
    {"1.2.840.10045.2.1", "ecPublicKey"}, {"1.2.840.10045.4.3.2", "ecdsa-with-SHA256"}, {"1.2.840.10045.4.3.3", "ecdsa-with-SHA384"},
    {"1.2.840.10045.4.3.4", "ecdsa-with-SHA512"}, {"1.2.840.10045.3.1.7", "prime256v1"}, {"1.3.132.0.34", "secp384r1"},
    {"1.3.132.0.35", "secp521r1"}, {"1.3.132.0.10", "secp256k1"}, {"1.3.101.110", "X25519"}, {"1.3.101.111", "X448"},
    {"1.3.101.112", "Ed25519"}, {"1.3.101.113", "Ed448"}, {"1.2.840.10040.4.1", "dsa"},
    {"2.5.29.14", "subjectKeyIdentifier"}, {"2.5.29.15", "keyUsage"}, {"2.5.29.17", "subjectAltName"},
    {"2.5.29.18", "issuerAltName"}, {"2.5.29.19", "basicConstraints"}, {"2.5.29.20", "cRLNumber"}, {"2.5.29.21", "reasonCode"},
    {"2.5.29.31", "cRLDistributionPoints"}, {"2.5.29.32", "certificatePolicies"}, {"2.5.29.35", "authorityKeyIdentifier"},
    {"2.5.29.37", "extKeyUsage"}, {"1.3.6.1.5.5.7.1.1", "authorityInfoAccess"}, {"1.3.6.1.5.5.7.3.1", "serverAuth"},
    {"1.3.6.1.5.5.7.3.2", "clientAuth"}, {"1.3.6.1.5.5.7.3.3", "codeSigning"}, {"1.3.6.1.5.5.7.3.4", "emailProtection"},
    {"1.3.6.1.5.5.7.3.8", "timeStamping"}, {"1.3.6.1.5.5.7.3.9", "OCSPSigning"}, {"1.3.6.1.5.5.7.48.1", "ocsp"},
    {"1.3.6.1.5.5.7.48.2", "caIssuers"}, {"1.2.840.113549.1.7.1", "data"}, {"1.2.840.113549.1.7.2", "signedData"},
    {"1.2.840.113549.1.9.14", "extensionRequest"}, {"1.2.840.113549.1.9.7", "challengePassword"},
    {"1.2.840.113549.1.5.13", "pbes2"}, {"1.2.840.113549.1.5.12", "pbkdf2"}, {"2.16.840.1.101.3.4.1.42", "aes256-CBC"},
    {"2.16.840.1.101.3.4.1.2", "aes128-CBC"}, {"2.16.840.1.101.3.4.2.1", "sha256"}, {"2.16.840.1.101.3.4.2.2", "sha384"},
    {"2.16.840.1.101.3.4.2.3", "sha512"}, {"1.3.14.3.2.26", "sha1"}, {"1.2.840.113549.2.9", "hmacWithSHA256"},
};

}  // namespace

Result<Node> parse(ByteView data, int max_depth) {
  util::ByteReader reader(data);
  CVT_TRY_ASSIGN(Node node, parse_one(data, reader, max_depth));
  if (!reader.at_end()) return fail(Status::Decode, "{} bytes after the ASN.1 element", reader.remaining());
  return node;
}

Result<std::vector<Node>> parse_all(ByteView data, int max_depth) {
  util::ByteReader reader(data);
  std::vector<Node> out;
  while (!reader.at_end()) {
    CVT_TRY_ASSIGN(Node node, parse_one(data, reader, max_depth));
    out.push_back(std::move(node));
  }
  return out;
}

std::string oid_text(ByteView value) {
  std::string out;
  std::uint64_t v = 0;
  bool first = true;
  for (const std::byte b : value) {
    if (v > (std::uint64_t{1} << 56)) return "invalid";
    v = (v << 7) | (static_cast<std::uint8_t>(b) & 0x7F);
    if ((static_cast<std::uint8_t>(b) & 0x80) != 0) continue;
    if (first) {
      const std::uint64_t a = v < 40 ? 0 : v < 80 ? 1 : 2;
      out = std::to_string(a) + "." + std::to_string(v - a * 40);
      first = false;
    } else {
      out += "." + std::to_string(v);
    }
    v = 0;
  }
  return out;
}

std::string oid_name(std::string_view dotted) {
  for (const auto& [o, n] : oids) {
    if (o == dotted) return std::string(n);
  }
  return std::string(dotted);
}

std::string integer_text(ByteView value) {
  if (value.size() <= 8) {
    std::int64_t v = value.empty() ? 0 : (static_cast<std::int8_t>(value[0]) < 0 ? -1 : 0);
    for (const std::byte b : value) v = static_cast<std::int64_t>((static_cast<std::uint64_t>(v) << 8) | static_cast<std::uint8_t>(b));
    return std::to_string(v);
  }
  ByteView v = value;
  while (v.size() > 1 && v[0] == std::byte{0}) v = v.subspan(1);
  std::string hex = util::hex_encode(v);
  std::string out;
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    if (i) out += ':';
    out += hex.substr(i, 2);
  }
  return out;
}

std::string time_text(const Node& node) {
  const std::string_view t = as_chars(node.value);
  if (node.universal(23) && t.size() >= 12) {  // UTCTime YYMMDDhhmm[ss]Z
    const int yy = (t[0] - '0') * 10 + (t[1] - '0');
    const std::string year = std::to_string(yy >= 50 ? 1900 + yy : 2000 + yy);
    return std::format("{}-{}-{}T{}:{}:{}Z", year, t.substr(2, 2), t.substr(4, 2), t.substr(6, 2), t.substr(8, 2),
                       t.size() >= 13 && t[10] != 'Z' ? t.substr(10, 2) : "00");
  }
  if (node.universal(24) && t.size() >= 14) {
    return std::format("{}-{}-{}T{}:{}:{}Z", t.substr(0, 4), t.substr(4, 2), t.substr(6, 2), t.substr(8, 2), t.substr(10, 2), t.substr(12, 2));
  }
  return util::sanitize_utf8(t);
}

std::string string_text(const Node& node) {
  if (node.universal(30)) {  // BMPString
    std::u16string u;
    for (std::size_t i = 0; i + 1 < node.value.size(); i += 2) {
      u.push_back(static_cast<char16_t>((static_cast<std::uint8_t>(node.value[i]) << 8) | static_cast<std::uint8_t>(node.value[i + 1])));
    }
    return util::utf16_to_utf8(u);
  }
  if (node.universal(28)) {  // UniversalString
    std::u32string u;
    for (std::size_t i = 0; i + 3 < node.value.size(); i += 4) {
      char32_t c = 0;
      for (int k = 0; k < 4; ++k) c = (c << 8) | static_cast<std::uint8_t>(node.value[i + static_cast<std::size_t>(k)]);
      u.push_back(util::is_scalar_value(c) ? c : U'�');
    }
    return util::utf32_to_utf8(u);
  }
  if (node.universal(20)) {  // T61: treat as Latin-1
    return util::decode_text(node.value, util::TextEncoding::Latin1).text;
  }
  return util::sanitize_utf8(as_chars(node.value));
}

std::string universal_name(std::uint32_t tag) {
  static constexpr std::array<std::string_view, 31> names = {
      "EOC", "BOOLEAN", "INTEGER", "BIT STRING", "OCTET STRING", "NULL", "OBJECT IDENTIFIER", "ObjectDescriptor", "EXTERNAL",
      "REAL", "ENUMERATED", "EMBEDDED PDV", "UTF8String", "RELATIVE-OID", "TIME", "reserved", "SEQUENCE", "SET",
      "NumericString", "PrintableString", "T61String", "VideotexString", "IA5String", "UTCTime", "GeneralizedTime",
      "GraphicString", "VisibleString", "GeneralString", "UniversalString", "CHARACTER STRING", "BMPString"};
  return tag < names.size() ? std::string(names[tag]) : "tag " + std::to_string(tag);
}

Json tree_json(const Node& node, int depth) {
  Json out = Json::object();
  if (node.cls == 0) {
    out["type"] = universal_name(node.tag);
  } else {
    static constexpr std::string_view classes[] = {"universal", "application", "context", "private"};
    out["type"] = std::format("[{}] {}", node.tag, classes[node.cls]);
  }
  if (node.constructed) {
    Json children = Json::array();
    for (const Node& child : node.children) children.push_back(depth < max_nesting ? tree_json(child, depth + 1) : Json("..."));
    out["children"] = std::move(children);
    return out;
  }
  if (node.cls != 0) {
    out["hex"] = util::hex_encode(node.value.first(std::min<std::size_t>(node.value.size(), 64)));
    return out;
  }
  switch (node.tag) {
    case 1: out["value"] = !node.value.empty() && node.value[0] != std::byte{0}; break;
    case 2:
    case 10: out["value"] = integer_text(node.value); break;
    case 5: break;
    case 6: out["value"] = oid_text(node.value); out["name"] = oid_name(oid_text(node.value)); break;
    case 23:
    case 24: out["value"] = time_text(node); break;
    case 12: case 18: case 19: case 20: case 22: case 26: case 27: case 28: case 30: out["value"] = string_text(node); break;
    default:
      out["bytes"] = static_cast<std::uint64_t>(node.value.size());
      out["hex"] = util::hex_encode(node.value.first(std::min<std::size_t>(node.value.size(), 64)));
      break;
  }
  return out;
}

Bytes tlv(std::uint8_t identifier, ByteView value) {
  util::ByteWriter out(value.size() + 6);
  out.u8(identifier);
  const std::size_t n = value.size();
  if (n < 0x80) {
    out.u8(static_cast<std::uint8_t>(n));
  } else {
    std::array<std::uint8_t, 8> digits{};
    int count = 0;
    for (std::size_t v = n; v != 0; v >>= 8) digits[static_cast<std::size_t>(count++)] = static_cast<std::uint8_t>(v & 0xFF);
    out.u8(static_cast<std::uint8_t>(0x80 | count));
    for (int i = count; i-- > 0;) out.u8(digits[static_cast<std::size_t>(i)]);
  }
  out.bytes(value);
  return out.take();
}

Bytes sequence(const std::vector<Bytes>& items) {
  Bytes body;
  for (const Bytes& item : items) append(body, ByteView(item));
  return tlv(0x30, body);
}

Bytes oid(std::string_view dotted) {
  std::vector<std::uint64_t> parts;
  for (const auto p : util::split(dotted, '.')) parts.push_back(static_cast<std::uint64_t>(util::parse_int(p).value_or(0)));
  Bytes body;
  const auto put = [&body](std::uint64_t v) {
    std::array<std::uint8_t, 10> tmp{};
    int n = 0;
    do {
      tmp[static_cast<std::size_t>(n++)] = static_cast<std::uint8_t>(v & 0x7F);
      v >>= 7;
    } while (v != 0);
    for (int i = n; i-- > 0;) body.push_back(static_cast<std::byte>(tmp[static_cast<std::size_t>(i)] | (i ? 0x80 : 0)));
  };
  if (parts.size() >= 2) put(parts[0] * 40 + parts[1]);
  for (std::size_t i = 2; i < parts.size(); ++i) put(parts[i]);
  return tlv(0x06, body);
}

Bytes integer_unsigned(ByteView magnitude) {
  while (magnitude.size() > 1 && magnitude[0] == std::byte{0}) magnitude = magnitude.subspan(1);
  Bytes body;
  if (magnitude.empty() || (static_cast<std::uint8_t>(magnitude[0]) & 0x80) != 0) body.push_back(std::byte{0});
  append(body, magnitude);
  return tlv(0x02, body);
}

Bytes bit_string(ByteView bytes) {
  Bytes body{std::byte{0}};
  append(body, bytes);
  return tlv(0x03, body);
}

}  // namespace cvt::science::asn1
