// Convertible core - science family: security and PKI containers, structure
// only. PEM <-> DER for certificates, requests, CRLs and keys (several blocks,
// strict Base64); PKCS#7 certificate bags; X.509 interpretation (names,
// times, extensions) as Data and a readable Document; OpenSSH public keys,
// authorized_keys and known_hosts; OpenSSH <-> SPKI and JWK <-> SPKI
// public-key re-encoding (RSA, ECDSA P-256/384/521, Ed25519); JWT decoding
// (the signature is NOT verified); OpenPGP ASCII armour <-> binary packets
// with a packet dump.
//
// Nothing here generates keys, decrypts, verifies or weakens anything:
// encrypted private keys pass through unchanged, and private key material is
// never printed (summaries give algorithm and size only).
//
// Data model: {"objects":[{"label","headers","der":base64,"summary":{...}}],
//              "keys":[{"type","bits","comment","options","hosts","fingerprint_sha256","blob":base64}]}
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <format>

#include "engine/standard_options.hpp"
#include "families/science/asn1.hpp"
#include "families/science/common.hpp"
#include "families/science/science.hpp"
#include "util/base64.hpp"
#include "util/hash.hpp"
#include "util/hex.hpp"
#include "util/text.hpp"

namespace cvt::science {

namespace {

using asn1::Node;

struct PkiObject {
  std::string label;
  std::vector<std::pair<std::string, std::string>> headers;
  Bytes der;
};

std::string b64(ByteView data) { return util::base64_encode(data); }

std::string b64url(ByteView data) {
  util::Base64Options o;
  o.url_safe = true;
  o.padding = false;
  return util::base64_encode(data, o);
}

Result<Bytes> b64_strict(std::string_view text) {
  std::string compact;
  for (const char c : text) {
    if (util::is_ascii_space(c)) continue;
    if (!(util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '+' || c == '/' || c == '=')) {
      return fail(Status::Decode, "invalid Base64 character '{}'", c);
    }
    compact.push_back(c);
  }
  if (compact.size() % 4 != 0) return fail(Status::Decode, "Base64 data has a bad length");
  return util::base64_decode(compact);
}

Result<Bytes> b64url_decode(std::string_view text) {
  std::string s(text);
  for (char& c : s) {
    if (c == '-') c = '+';
    else if (c == '_') c = '/';
  }
  while (s.size() % 4 != 0) s.push_back('=');
  return util::base64_decode(s);
}

// --------------------------------------------------------------------------- PEM

Result<std::vector<PkiObject>> parse_pem(std::string_view text) {
  std::vector<PkiObject> out;
  const auto lines = lines_of(text);
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const std::string_view line = util::trim(lines[i]);
    if (!line.starts_with("-----BEGIN ") || !line.ends_with("-----")) continue;
    PkiObject obj;
    obj.label = std::string(line.substr(11, line.size() - 16));
    std::string body;
    bool in_headers = true;
    bool closed = false;
    for (++i; i < lines.size(); ++i) {
      const std::string_view l = util::trim(lines[i]);
      if (l == "-----END " + obj.label + "-----") {
        closed = true;
        break;
      }
      if (l.starts_with("-----")) return fail(Status::Decode, "PEM block '{}' is not closed", obj.label);
      if (in_headers) {
        const std::size_t colon = l.find(':');
        if (colon != std::string_view::npos && l.find_first_of("=") == std::string_view::npos) {
          obj.headers.emplace_back(std::string(util::trim(l.substr(0, colon))), std::string(util::trim(l.substr(colon + 1))));
          continue;
        }
        if (l.empty() && !obj.headers.empty()) {
          in_headers = false;
          continue;
        }
        in_headers = false;
      }
      body += l;
    }
    if (!closed) return fail(Status::Decode, "PEM block '{}' has no END line", obj.label);
    CVT_TRY_ASSIGN(obj.der, b64_strict(body));
    out.push_back(std::move(obj));
    if (out.size() > 100000) return fail(Status::Limit, "too many PEM blocks");
  }
  if (out.empty()) return fail(Status::Decode, "no PEM blocks found");
  return out;
}

std::string pem_text(const PkiObject& obj) {
  std::string out = "-----BEGIN " + obj.label + "-----\n";
  for (const auto& [k, v] : obj.headers) out += k + ": " + v + "\n";
  if (!obj.headers.empty()) out += "\n";
  util::Base64Options o;
  o.line_width = 64;
  out += util::base64_encode(obj.der, o) + "\n";
  out += "-----END " + obj.label + "-----\n";
  return out;
}

// --------------------------------------------------------------------------- X.509 interpretation

std::string dn_text(const Node& name, Json* parts) {
  std::string out;
  for (const Node& rdn : name.children) {
    for (const Node& atv : rdn.children) {
      if (atv.children.size() < 2) continue;
      const std::string oid = asn1::oid_text(atv.children[0].value);
      static constexpr std::pair<std::string_view, std::string_view> short_names[] = {
          {"2.5.4.3", "CN"}, {"2.5.4.6", "C"}, {"2.5.4.7", "L"}, {"2.5.4.8", "ST"}, {"2.5.4.10", "O"}, {"2.5.4.11", "OU"},
          {"0.9.2342.19200300.100.1.25", "DC"}, {"1.2.840.113549.1.9.1", "emailAddress"}, {"2.5.4.5", "serialNumber"}};
      std::string key = asn1::oid_name(oid);
      for (const auto& [o, n] : short_names) {
        if (o == oid) key = std::string(n);
      }
      const std::string value = asn1::string_text(atv.children[1]);
      if (!out.empty()) out += ", ";
      out += key + "=" + value;
      if (parts != nullptr) (*parts)[asn1::oid_name(oid)] = value;
    }
  }
  return out;
}

Json algorithm_json(const Node& alg) {
  Json out = Json::object();
  if (alg.children.empty()) return out;
  const std::string oid = asn1::oid_text(alg.children[0].value);
  out["oid"] = oid;
  out["name"] = asn1::oid_name(oid);
  if (alg.children.size() > 1 && alg.children[1].universal(6)) out["parameters"] = asn1::oid_name(asn1::oid_text(alg.children[1].value));
  return out;
}

std::size_t bit_length(ByteView magnitude) {
  while (!magnitude.empty() && magnitude[0] == std::byte{0}) magnitude = magnitude.subspan(1);
  if (magnitude.empty()) return 0;
  std::size_t bits = magnitude.size() * 8;
  for (auto top = static_cast<std::uint8_t>(magnitude[0]); (top & 0x80) == 0; top = static_cast<std::uint8_t>(top << 1)) --bits;
  return bits;
}

Json spki_json(const Node& spki) {
  Json out = Json::object();
  if (spki.children.size() < 2) return out;
  out["algorithm"] = algorithm_json(spki.children[0]);
  const std::string alg = out["algorithm"]["name"].as_string();
  const ByteView key = spki.children[1].value.empty() ? ByteView{} : spki.children[1].value.subspan(1);
  if (alg == "rsaEncryption") {
    if (auto rsa = asn1::parse(key); rsa && rsa->children.size() >= 2) {
      out["bits"] = static_cast<std::uint64_t>(bit_length(rsa->children[0].value));
      out["exponent"] = asn1::integer_text(rsa->children[1].value);
    }
  } else if (alg == "ecPublicKey") {
    out["curve"] = out["algorithm"]["parameters"];
    out["bits"] = static_cast<std::uint64_t>(key.size() > 1 ? (key.size() - 1) / 2 * 8 : 0);
  } else if (alg == "Ed25519" || alg == "X25519") {
    out["bits"] = 256;
  } else if (alg == "Ed448") {
    out["bits"] = 456;
  }
  out["key_sha256"] = util::sha256_hex(spki.header_and_value);
  return out;
}

Json general_names(const Node& seq) {
  Json out = Json::array();
  for (const Node& n : seq.children) {
    if (n.cls != 2) continue;
    std::string text;
    switch (n.tag) {
      case 1: text = "email:" + util::sanitize_utf8(as_chars(n.value)); break;
      case 2: text = "DNS:" + util::sanitize_utf8(as_chars(n.value)); break;
      case 6: text = "URI:" + util::sanitize_utf8(as_chars(n.value)); break;
      case 7:
        if (n.value.size() == 4) {
          text = std::format("IP:{}.{}.{}.{}", static_cast<int>(n.value[0]), static_cast<int>(n.value[1]), static_cast<int>(n.value[2]),
                             static_cast<int>(n.value[3]));
        } else if (n.value.size() == 16) {
          text = "IP:";
          for (std::size_t i = 0; i < 16; i += 2) {
            text += std::format("{}{:x}", i ? ":" : "", (static_cast<int>(n.value[i]) << 8) | static_cast<int>(n.value[i + 1]));
          }
        }
        break;
      case 4:
        if (!n.children.empty()) text = "DirName:" + dn_text(n.children[0], nullptr);
        break;
      default: text = std::format("[{}]", n.tag); break;
    }
    out.push_back(text);
  }
  return out;
}

Json extension_value(const std::string& name, ByteView der) {
  auto node = asn1::parse(der);
  if (!node) return Json(util::hex_encode(der));
  const Node& n = *node;
  if (name == "basicConstraints") {
    Json out = Json::object({{"ca", false}});
    for (const Node& c : n.children) {
      if (c.universal(1)) out["ca"] = !c.value.empty() && c.value[0] != std::byte{0};
      if (c.universal(2)) out["path_length"] = asn1::integer_text(c.value);
    }
    return out;
  }
  if (name == "keyUsage") {
    static constexpr std::string_view bits[] = {"digitalSignature", "nonRepudiation", "keyEncipherment", "dataEncipherment",
                                                "keyAgreement", "keyCertSign", "cRLSign", "encipherOnly", "decipherOnly"};
    Json out = Json::array();
    const ByteView v = n.value.size() > 1 ? n.value.subspan(1) : ByteView{};
    for (std::size_t i = 0; i < 9; ++i) {
      if (i / 8 < v.size() && (static_cast<std::uint8_t>(v[i / 8]) >> (7 - i % 8)) & 1) out.push_back(std::string(bits[i]));
    }
    return out;
  }
  if (name == "extKeyUsage" || name == "certificatePolicies") {
    Json out = Json::array();
    for (const Node& c : n.children) {
      const Node& o = c.universal(6) ? c : (c.children.empty() ? c : c.children[0]);
      if (o.universal(6)) out.push_back(asn1::oid_name(asn1::oid_text(o.value)));
    }
    return out;
  }
  if (name == "subjectAltName" || name == "issuerAltName") return general_names(n);
  if (name == "subjectKeyIdentifier") return Json(util::hex_encode(n.value));
  if (name == "authorityKeyIdentifier") {
    for (const Node& c : n.children) {
      if (c.cls == 2 && c.tag == 0) return Json::object({{"key_id", util::hex_encode(c.value)}});
    }
    return Json::object();
  }
  if (name == "cRLDistributionPoints" || name == "authorityInfoAccess") {
    Json out = Json::array();
    std::vector<const Node*> stack = {&n};
    while (!stack.empty()) {
      const Node* cur = stack.back();
      stack.pop_back();
      if (cur->cls == 2 && cur->tag == 6 && !cur->constructed) out.push_back(util::sanitize_utf8(as_chars(cur->value)));
      for (auto it = cur->children.rbegin(); it != cur->children.rend(); ++it) stack.push_back(&*it);
    }
    return out;
  }
  if (name == "cRLNumber") return Json(asn1::integer_text(n.value));
  return Json(util::hex_encode(der.first(std::min<std::size_t>(der.size(), 128))));
}

Json extensions_json(const Node& exts) {
  Json out = Json::object();
  for (const Node& ext : exts.children) {
    if (ext.children.size() < 2) continue;
    const std::string name = asn1::oid_name(asn1::oid_text(ext.children[0].value));
    const Node& value = ext.children.back();
    bool critical = false;
    if (ext.children.size() == 3 && ext.children[1].universal(1)) critical = !ext.children[1].value.empty() && ext.children[1].value[0] != std::byte{0};
    Json v = extension_value(name, value.value);
    if (critical) v = Json::object({{"critical", true}, {"value", std::move(v)}});
    out[name] = std::move(v);
  }
  return out;
}

Json certificate_json(const Node& cert) {
  Json out = Json::object();
  const Node* tbs = cert.child(0);
  if (tbs == nullptr) return out;
  std::size_t i = 0;
  int version = 1;
  if (tbs->child(0) != nullptr && tbs->children[0].is(2, 0)) {
    if (const Node* v = tbs->children[0].child(0)) version = static_cast<int>(to_int(asn1::integer_text(v->value)).value_or(0)) + 1;
    i = 1;
  }
  out["version"] = version;
  if (const Node* serial = tbs->child(i)) out["serial"] = asn1::integer_text(serial->value);
  if (const Node* alg = cert.child(1)) out["signature_algorithm"] = algorithm_json(*alg)["name"];
  if (const Node* issuer = tbs->child(i + 2)) out["issuer"] = dn_text(*issuer, nullptr);
  if (const Node* validity = tbs->child(i + 3); validity != nullptr && validity->children.size() == 2) {
    out["not_before"] = asn1::time_text(validity->children[0]);
    out["not_after"] = asn1::time_text(validity->children[1]);
  }
  if (const Node* subject = tbs->child(i + 4)) out["subject"] = dn_text(*subject, nullptr);
  if (const Node* spki = tbs->child(i + 5)) out["public_key"] = spki_json(*spki);
  for (std::size_t k = i + 6; k < tbs->children.size(); ++k) {
    if (tbs->children[k].is(2, 3) && !tbs->children[k].children.empty()) out["extensions"] = extensions_json(tbs->children[k].children[0]);
  }
  out["sha256_fingerprint"] = util::sha256_hex(cert.header_and_value);
  return out;
}

Json csr_json(const Node& csr) {
  Json out = Json::object();
  const Node* info = csr.child(0);
  if (info == nullptr) return out;
  if (const Node* subject = info->child(1)) out["subject"] = dn_text(*subject, nullptr);
  if (const Node* spki = info->child(2)) out["public_key"] = spki_json(*spki);
  if (const Node* attrs = info->child(3); attrs != nullptr && attrs->is(2, 0)) {
    for (const Node& attr : attrs->children) {
      if (attr.children.size() < 2) continue;
      const std::string name = asn1::oid_name(asn1::oid_text(attr.children[0].value));
      if (name == "extensionRequest" && !attr.children[1].children.empty()) out["extensions"] = extensions_json(attr.children[1].children[0]);
      if (name == "challengePassword") out["challenge_password"] = "(present, not shown)";
    }
  }
  if (const Node* alg = csr.child(1)) out["signature_algorithm"] = algorithm_json(*alg)["name"];
  return out;
}

Json crl_json(const Node& crl) {
  Json out = Json::object();
  const Node* tbs = crl.child(0);
  if (tbs == nullptr) return out;
  std::size_t i = tbs->child(0) != nullptr && tbs->children[0].universal(2) ? 1 : 0;
  if (const Node* alg = tbs->child(i)) out["signature_algorithm"] = algorithm_json(*alg)["name"];
  if (const Node* issuer = tbs->child(i + 1)) out["issuer"] = dn_text(*issuer, nullptr);
  if (const Node* t = tbs->child(i + 2)) out["this_update"] = asn1::time_text(*t);
  std::size_t k = i + 3;
  if (const Node* t = tbs->child(k); t != nullptr && (t->universal(23) || t->universal(24))) {
    out["next_update"] = asn1::time_text(*t);
    ++k;
  }
  Json revoked = Json::array();
  if (const Node* list = tbs->child(k); list != nullptr && list->universal(16)) {
    for (const Node& entry : list->children) {
      Json r = Json::object();
      if (const Node* s = entry.child(0)) r["serial"] = asn1::integer_text(s->value);
      if (const Node* d = entry.child(1)) r["date"] = asn1::time_text(*d);
      revoked.push_back(std::move(r));
    }
    ++k;
  }
  out["revoked"] = std::move(revoked);
  if (const Node* exts = tbs->child(k); exts != nullptr && exts->is(2, 0) && !exts->children.empty()) out["extensions"] = extensions_json(exts->children[0]);
  return out;
}

/// Guess the PEM label of a DER structure.
std::string infer_label(const Node& n) {
  if (!n.universal(16) || n.children.empty()) return "UNKNOWN";
  const Node& first = n.children[0];
  if (first.universal(6) && asn1::oid_text(first.value) == "1.2.840.113549.1.7.2") return "PKCS7";
  if (first.universal(16) && n.children.size() == 3 && n.children[2].universal(3)) {
    // Certificate, CSR or CRL: look inside the to-be-signed part.
    const Node& tbs = first;
    if (!tbs.children.empty() && tbs.children[0].is(2, 0)) return "CERTIFICATE";
    if (tbs.children.size() >= 6 && tbs.children[0].universal(2)) return "CERTIFICATE";
    if (tbs.children.size() >= 4 && tbs.children[0].universal(2) && tbs.children[3].is(2, 0)) return "CERTIFICATE REQUEST";
    return "X509 CRL";
  }
  if (first.universal(16) && n.children.size() == 2 && n.children[1].universal(3)) return "PUBLIC KEY";
  if (first.universal(16) && n.children.size() == 2 && n.children[1].universal(4)) return "ENCRYPTED PRIVATE KEY";
  if (first.universal(2) && n.children.size() >= 3 && n.children[1].universal(16) && n.children[2].universal(4)) return "PRIVATE KEY";
  if (first.universal(2) && n.children.size() == 9) return "RSA PRIVATE KEY";
  if (first.universal(2) && n.children.size() >= 2 && n.children[1].universal(4)) return "EC PRIVATE KEY";
  if (first.universal(2) && n.children.size() == 2) return "RSA PUBLIC KEY";
  return "UNKNOWN";
}

Json summary_of(const PkiObject& obj) {
  Json out = Json::object();
  for (const auto& [k, v] : obj.headers) {
    if (k == "Proc-Type" && v.find("ENCRYPTED") != std::string::npos) out["encrypted"] = true;
  }
  if (out.contains("encrypted")) {
    out["note"] = "encrypted private key; passed through unchanged";
    return out;
  }
  auto parsed = asn1::parse(obj.der);
  if (!parsed) {
    out["error"] = parsed.error().message;
    return out;
  }
  const Node& n = *parsed;
  const std::string& label = obj.label;
  if (label == "CERTIFICATE" || label == "TRUSTED CERTIFICATE" || label == "X509 CERTIFICATE") return certificate_json(n);
  if (label == "CERTIFICATE REQUEST" || label == "NEW CERTIFICATE REQUEST") return csr_json(n);
  if (label == "X509 CRL") return crl_json(n);
  if (label == "PUBLIC KEY") return spki_json(n);
  if (label == "RSA PUBLIC KEY" && n.children.size() >= 2) {
    return Json::object({{"algorithm", "rsaEncryption"}, {"bits", static_cast<std::uint64_t>(bit_length(n.children[0].value))},
                         {"exponent", asn1::integer_text(n.children[1].value)}});
  }
  if (label == "PRIVATE KEY" && n.children.size() >= 3) {
    out["algorithm"] = algorithm_json(n.children[1]);
    if (out["algorithm"]["name"].as_string() == "rsaEncryption") {
      if (auto rsa = asn1::parse(n.children[2].value); rsa && rsa->children.size() >= 2) {
        out["bits"] = static_cast<std::uint64_t>(bit_length(rsa->children[1].value));
      }
    }
    out["note"] = "private key: values not shown";
    return out;
  }
  if (label == "RSA PRIVATE KEY" && n.children.size() >= 2) {
    return Json::object({{"algorithm", "rsaEncryption"}, {"bits", static_cast<std::uint64_t>(bit_length(n.children[1].value))},
                         {"note", "private key: values not shown"}});
  }
  if (label == "EC PRIVATE KEY") {
    for (const Node& c : n.children) {
      if (c.is(2, 0) && !c.children.empty()) out["curve"] = asn1::oid_name(asn1::oid_text(c.children[0].value));
    }
    out["note"] = "private key: values not shown";
    return out;
  }
  if (label == "ENCRYPTED PRIVATE KEY" && n.children.size() >= 2) {
    out["encryption"] = algorithm_json(n.children[0]);
    if (const Node* params = n.children[0].child(1); params != nullptr && params->children.size() >= 2) {
      out["kdf"] = algorithm_json(params->children[0])["name"];
      out["cipher"] = algorithm_json(params->children[1])["name"];
    }
    out["note"] = "encrypted private key; passed through unchanged";
    return out;
  }
  if (label == "PKCS7") {
    std::size_t certs = 0, crls = 0;
    if (const Node* content = n.child(1); content != nullptr && !content->children.empty()) {
      for (const Node& part : content->children[0].children) {
        if (part.is(2, 0)) certs += part.children.size();
        if (part.is(2, 1)) crls += part.children.size();
      }
    }
    return Json::object({{"content_type", "signedData"}, {"certificates", static_cast<std::uint64_t>(certs)}, {"crls", static_cast<std::uint64_t>(crls)}});
  }
  out["asn1"] = asn1::tree_json(n);
  return out;
}

bool secret(std::string_view label) { return label.find("PRIVATE KEY") != std::string_view::npos; }

Json objects_json(const std::vector<PkiObject>& objects, bool include_tree) {
  Json list = Json::array();
  for (const PkiObject& o : objects) {
    Json item = Json::object();
    item["label"] = o.label;
    if (!o.headers.empty()) {
      Json h = Json::object();
      for (const auto& [k, v] : o.headers) h[k] = v;
      item["headers"] = std::move(h);
    }
    item["der"] = b64(o.der);
    item["summary"] = summary_of(o);
    if (include_tree && !secret(o.label)) {
      if (auto parsed = asn1::parse(o.der)) item["asn1"] = asn1::tree_json(*parsed);
    }
    list.push_back(std::move(item));
  }
  return list;
}

// --------------------------------------------------------------------------- PKCS#7 bags

Result<std::vector<PkiObject>> p7_contents(const PkiObject& bag) {
  CVT_TRY_ASSIGN(const Node n, asn1::parse(bag.der));
  if (infer_label(n) != "PKCS7") return fail(Status::Decode, "not a PKCS#7 signedData structure");
  std::vector<PkiObject> out;
  const Node* content = n.child(1);
  if (content == nullptr || content->children.empty()) return out;
  for (const Node& part : content->children[0].children) {
    if (part.is(2, 0) || part.is(2, 1)) {
      for (const Node& item : part.children) {
        PkiObject o;
        o.label = part.tag == 0 ? "CERTIFICATE" : "X509 CRL";
        o.der.assign(item.header_and_value.begin(), item.header_and_value.end());
        out.push_back(std::move(o));
      }
    }
  }
  return out;
}

Bytes p7_bag(const std::vector<PkiObject>& objects) {
  Bytes certs, crls;
  for (const PkiObject& o : objects) {
    if (o.label == "CERTIFICATE") append(certs, ByteView(o.der));
    if (o.label == "X509 CRL") append(crls, ByteView(o.der));
  }
  std::vector<Bytes> signed_data = {asn1::tlv(0x02, as_bytes(std::string_view("\x01", 1))), asn1::tlv(0x31, {}),
                                    asn1::sequence({asn1::oid("1.2.840.113549.1.7.1")})};
  if (!certs.empty()) signed_data.push_back(asn1::tlv(0xA0, certs));
  if (!crls.empty()) signed_data.push_back(asn1::tlv(0xA1, crls));
  signed_data.push_back(asn1::tlv(0x31, {}));
  return asn1::sequence({asn1::oid("1.2.840.113549.1.7.2"), asn1::tlv(0xA0, asn1::sequence(signed_data))});
}

// --------------------------------------------------------------------------- SSH and JWK keys

class SshReader {
 public:
  explicit SshReader(ByteView data) : r_(data) {}
  Result<ByteView> string() {
    const std::uint32_t n = r_.u32be();
    if (!r_.ok() || n > r_.remaining()) return fail(Status::Decode, "SSH key blob is truncated");
    return r_.bytes(n);
  }
  bool done() const { return r_.at_end(); }

 private:
  util::ByteReader r_;
};

void ssh_string(util::ByteWriter& w, ByteView s) {
  w.u32be(static_cast<std::uint32_t>(s.size()));
  w.bytes(s);
}

void ssh_mpint(util::ByteWriter& w, ByteView magnitude) {
  while (!magnitude.empty() && magnitude[0] == std::byte{0}) magnitude = magnitude.subspan(1);
  Bytes v;
  if (!magnitude.empty() && (static_cast<std::uint8_t>(magnitude[0]) & 0x80)) v.push_back(std::byte{0});
  append(v, magnitude);
  ssh_string(w, v);
}

ByteView unsigned_magnitude(ByteView v) {
  while (v.size() > 1 && v[0] == std::byte{0}) v = v.subspan(1);
  return v;
}

struct Curve {
  std::string_view ssh;
  std::string_view jwk;
  std::string_view oid;
  std::size_t coordinate_bytes;
};
constexpr Curve curves[] = {{"nistp256", "P-256", "1.2.840.10045.3.1.7", 32},
                            {"nistp384", "P-384", "1.3.132.0.34", 48},
                            {"nistp521", "P-521", "1.3.132.0.35", 66}};

/// SSH public key blob -> DER SubjectPublicKeyInfo.
Result<Bytes> ssh_to_spki(ByteView blob) {
  SshReader r(blob);
  CVT_TRY_ASSIGN(const ByteView type_bytes, r.string());
  const std::string_view type = as_chars(type_bytes);
  if (type == "ssh-rsa") {
    CVT_TRY_ASSIGN(const ByteView e, r.string());
    CVT_TRY_ASSIGN(const ByteView n, r.string());
    const Bytes rsa = asn1::sequence({asn1::integer_unsigned(n), asn1::integer_unsigned(e)});
    return asn1::sequence({asn1::sequence({asn1::oid("1.2.840.113549.1.1.1"), asn1::tlv(0x05, {})}), asn1::bit_string(rsa)});
  }
  if (type == "ssh-ed25519") {
    CVT_TRY_ASSIGN(const ByteView key, r.string());
    if (key.size() != 32) return fail(Status::Decode, "bad Ed25519 key length");
    return asn1::sequence({asn1::sequence({asn1::oid("1.3.101.112")}), asn1::bit_string(key)});
  }
  if (type.starts_with("ecdsa-sha2-")) {
    CVT_TRY_ASSIGN(const ByteView curve_name, r.string());
    CVT_TRY_ASSIGN(const ByteView point, r.string());
    for (const Curve& c : curves) {
      if (as_chars(curve_name) == c.ssh) {
        return asn1::sequence({asn1::sequence({asn1::oid("1.2.840.10045.2.1"), asn1::oid(c.oid)}), asn1::bit_string(point)});
      }
    }
    return fail(Status::Decode, "unsupported SSH curve '{}'", as_chars(curve_name));
  }
  return fail(Status::Decode, "SSH key type '{}' has no SPKI equivalent here", type);
}

/// DER SubjectPublicKeyInfo -> SSH blob and type name.
Result<std::pair<std::string, Bytes>> spki_to_ssh(ByteView der) {
  CVT_TRY_ASSIGN(const Node spki, asn1::parse(der));
  if (spki.children.size() < 2 || spki.children[0].children.empty()) return fail(Status::Encode, "not a SubjectPublicKeyInfo");
  const std::string oid = asn1::oid_text(spki.children[0].children[0].value);
  const ByteView key = spki.children[1].value.empty() ? ByteView{} : spki.children[1].value.subspan(1);
  util::ByteWriter w(key.size() + 64);
  if (oid == "1.2.840.113549.1.1.1") {
    CVT_TRY_ASSIGN(const Node rsa, asn1::parse(key));
    if (rsa.children.size() < 2) return fail(Status::Encode, "bad RSA public key");
    ssh_string(w, as_bytes(std::string_view("ssh-rsa")));
    ssh_mpint(w, rsa.children[1].value);
    ssh_mpint(w, rsa.children[0].value);
    return std::pair{std::string("ssh-rsa"), w.take()};
  }
  if (oid == "1.3.101.112") {
    ssh_string(w, as_bytes(std::string_view("ssh-ed25519")));
    ssh_string(w, key);
    return std::pair{std::string("ssh-ed25519"), w.take()};
  }
  if (oid == "1.2.840.10045.2.1" && spki.children[0].children.size() > 1) {
    const std::string curve = asn1::oid_text(spki.children[0].children[1].value);
    for (const Curve& c : curves) {
      if (curve == c.oid) {
        const std::string type = "ecdsa-sha2-" + std::string(c.ssh);
        ssh_string(w, as_bytes(type));
        ssh_string(w, as_bytes(c.ssh));
        ssh_string(w, key);
        return std::pair{type, w.take()};
      }
    }
    return fail(Status::Encode, "curve {} has no OpenSSH name", asn1::oid_name(curve));
  }
  return fail(Status::Encode, "{} keys cannot be written as OpenSSH keys", asn1::oid_name(oid));
}

std::string ssh_fingerprint(ByteView blob) {
  const auto digest = util::sha256(blob);
  util::Base64Options o;
  o.padding = false;
  return "SHA256:" + util::base64_encode(as_bytes(std::span<const std::uint8_t>(digest)), o);
}

Json ssh_key_json(std::string_view type, ByteView blob) {
  Json k = Json::object();
  k["type"] = std::string(type);
  if (auto spki = ssh_to_spki(blob)) {
    if (auto parsed = asn1::parse(*spki)) {
      const Json info = spki_json(*parsed);
      if (const Json* bits = info.find("bits")) k["bits"] = *bits;
    }
  }
  k["fingerprint_sha256"] = ssh_fingerprint(blob);
  k["blob"] = b64(blob);
  return k;
}

/// One authorized_keys / known_hosts / .pub line.
std::optional<Json> parse_ssh_line(std::string_view line, bool known_hosts) {
  line = util::trim(line);
  if (line.empty() || line.starts_with('#')) return std::nullopt;
  // Split respecting double quotes (authorized_keys options).
  std::vector<std::string> fields;
  std::string cur;
  bool quoted = false;
  for (const char c : line) {
    if (c == '"') quoted = !quoted;
    if (!quoted && (c == ' ' || c == '\t')) {
      if (!cur.empty()) fields.push_back(std::move(cur));
      cur.clear();
      continue;
    }
    cur.push_back(c);
  }
  if (!cur.empty()) fields.push_back(std::move(cur));
  std::size_t t = 0;
  while (t < fields.size() && !(fields[t].starts_with("ssh-") || fields[t].starts_with("ecdsa-") || fields[t].starts_with("sk-"))) ++t;
  if (t >= fields.size() || t + 1 >= fields.size()) return std::nullopt;
  auto blob = util::base64_decode(fields[t + 1]);
  if (!blob) return std::nullopt;
  SshReader r(*blob);
  auto inner = r.string();
  if (!inner || as_chars(*inner) != fields[t]) return std::nullopt;
  Json k = ssh_key_json(fields[t], *blob);
  std::string comment;
  for (std::size_t i = t + 2; i < fields.size(); ++i) comment += (comment.empty() ? "" : " ") + fields[i];
  k["comment"] = comment;
  if (t > 0) {
    std::size_t first = 0;
    if (fields[0] == "@cert-authority" || fields[0] == "@revoked") {
      k["marker"] = fields[0].substr(1);
      first = 1;
    }
    if (known_hosts) {
      Json hosts = Json::array();
      for (std::size_t i = first; i < t; ++i) {
        for (const auto h : util::split(fields[i], ',')) hosts.push_back(std::string(h));
      }
      k["hosts"] = std::move(hosts);
    } else {
      Json options = Json::array();
      for (std::size_t i = first; i < t; ++i) {
        for (const auto o : util::split(fields[i], ',')) options.push_back(std::string(o));
      }
      k["options"] = std::move(options);
    }
  }
  return k;
}

Result<Json> read_ssh_keys(std::string_view text, bool known_hosts) {
  Json keys = Json::array();
  for (const auto line : lines_of(text)) {
    if (auto k = parse_ssh_line(line, known_hosts)) keys.push_back(std::move(*k));
    if (keys.size() > 100000) return fail(Status::Limit, "too many keys");
  }
  if (keys.empty()) return fail(Status::Decode, "no OpenSSH public keys found");
  return keys;
}

/// Public keys of a Data value as SPKI DER (objects and ssh/jwk keys).
Result<std::vector<Bytes>> public_keys(const Json& data, const Context& ctx) {
  std::vector<Bytes> out;
  for (const Json& o : data["objects"].elements()) {
    CVT_TRY_ASSIGN(const Bytes der, util::base64_decode(o["der"].as_string()));
    const std::string label = o["label"].as_string();
    if (label == "PUBLIC KEY") {
      out.push_back(der);
    } else if (label == "CERTIFICATE") {
      CVT_TRY_ASSIGN(const Node cert, asn1::parse(der));
      const Node* tbs = cert.child(0);
      if (tbs == nullptr) continue;
      const std::size_t i = !tbs->children.empty() && tbs->children[0].is(2, 0) ? 1 : 0;
      if (const Node* spki = tbs->child(i + 5)) out.emplace_back(spki->header_and_value.begin(), spki->header_and_value.end());
    } else if (secret(label)) {
      ctx.warn("Private keys are not converted; only public keys are written.");
    }
  }
  for (const Json& k : data["keys"].elements()) {
    if (k.contains("blob")) {
      CVT_TRY_ASSIGN(const Bytes blob, util::base64_decode(k["blob"].as_string()));
      auto spki = ssh_to_spki(blob);
      if (spki) out.push_back(std::move(*spki));
      else ctx.warn(spki.error().message);
    } else if (k.contains("spki")) {
      CVT_TRY_ASSIGN(const Bytes der, util::base64_decode(k["spki"].as_string()));
      out.push_back(der);
    }
  }
  if (out.empty()) return fail(Status::Encode, "the data holds no public keys");
  return out;
}

/// JWK object -> SPKI DER.
Result<Bytes> jwk_to_spki(const Json& jwk) {
  const std::string kty = jwk["kty"].as_string();
  const auto field = [&jwk](std::string_view k) { return b64url_decode(jwk[k].as_string()); };
  if (kty == "RSA") {
    CVT_TRY_ASSIGN(const Bytes n, field("n"));
    CVT_TRY_ASSIGN(const Bytes e, field("e"));
    if (n.empty() || e.empty()) return fail(Status::Decode, "RSA JWK without n/e");
    const Bytes rsa = asn1::sequence({asn1::integer_unsigned(n), asn1::integer_unsigned(e)});
    return asn1::sequence({asn1::sequence({asn1::oid("1.2.840.113549.1.1.1"), asn1::tlv(0x05, {})}), asn1::bit_string(rsa)});
  }
  if (kty == "EC") {
    const std::string crv = jwk["crv"].as_string();
    for (const Curve& c : curves) {
      if (crv != c.jwk) continue;
      CVT_TRY_ASSIGN(const Bytes x, field("x"));
      CVT_TRY_ASSIGN(const Bytes y, field("y"));
      if (x.size() != c.coordinate_bytes || y.size() != c.coordinate_bytes) return fail(Status::Decode, "bad EC JWK coordinates");
      Bytes point{std::byte{4}};
      append(point, ByteView(x));
      append(point, ByteView(y));
      return asn1::sequence({asn1::sequence({asn1::oid("1.2.840.10045.2.1"), asn1::oid(c.oid)}), asn1::bit_string(point)});
    }
    return fail(Status::Decode, "unsupported JWK curve '{}'", crv);
  }
  if (kty == "OKP" && jwk["crv"].as_string() == "Ed25519") {
    CVT_TRY_ASSIGN(const Bytes x, field("x"));
    if (x.size() != 32) return fail(Status::Decode, "bad Ed25519 JWK");
    return asn1::sequence({asn1::sequence({asn1::oid("1.3.101.112")}), asn1::bit_string(x)});
  }
  return fail(Status::Decode, "unsupported JWK key type '{}'", kty);
}

Result<Json> spki_to_jwk(ByteView der) {
  CVT_TRY_ASSIGN(const Node spki, asn1::parse(der));
  if (spki.children.size() < 2 || spki.children[0].children.empty()) return fail(Status::Encode, "not a SubjectPublicKeyInfo");
  const std::string oid = asn1::oid_text(spki.children[0].children[0].value);
  const ByteView key = spki.children[1].value.empty() ? ByteView{} : spki.children[1].value.subspan(1);
  Json jwk = Json::object();
  if (oid == "1.2.840.113549.1.1.1") {
    CVT_TRY_ASSIGN(const Node rsa, asn1::parse(key));
    if (rsa.children.size() < 2) return fail(Status::Encode, "bad RSA key");
    jwk["kty"] = "RSA";
    jwk["n"] = b64url(unsigned_magnitude(rsa.children[0].value));
    jwk["e"] = b64url(unsigned_magnitude(rsa.children[1].value));
  } else if (oid == "1.3.101.112") {
    jwk["kty"] = "OKP";
    jwk["crv"] = "Ed25519";
    jwk["x"] = b64url(key);
  } else if (oid == "1.2.840.10045.2.1" && spki.children[0].children.size() > 1) {
    const std::string curve = asn1::oid_text(spki.children[0].children[1].value);
    const Curve* c = nullptr;
    for (const Curve& k : curves) {
      if (curve == k.oid) c = &k;
    }
    if (c == nullptr || key.size() != 1 + 2 * c->coordinate_bytes || key[0] != std::byte{4}) {
      return fail(Status::Encode, "unsupported EC key for JWK");
    }
    jwk["kty"] = "EC";
    jwk["crv"] = std::string(c->jwk);
    jwk["x"] = b64url(key.subspan(1, c->coordinate_bytes));
    jwk["y"] = b64url(key.subspan(1 + c->coordinate_bytes, c->coordinate_bytes));
  } else {
    return fail(Status::Encode, "{} keys cannot be written as JWK", asn1::oid_name(oid));
  }
  return jwk;
}

// --------------------------------------------------------------------------- PGP

std::uint32_t crc24(ByteView data) {
  std::uint32_t crc = 0xB704CE;
  for (const std::byte b : data) {
    crc ^= std::uint32_t{static_cast<std::uint8_t>(b)} << 16;
    for (int i = 0; i < 8; ++i) {
      crc <<= 1;
      if (crc & 0x1000000) crc ^= 0x1864CFB;
    }
  }
  return crc & 0xFFFFFF;
}

std::string_view packet_name(int tag) {
  switch (tag) {
    case 1: return "Public-Key Encrypted Session Key";
    case 2: return "Signature";
    case 3: return "Symmetric-Key Encrypted Session Key";
    case 4: return "One-Pass Signature";
    case 5: return "Secret-Key";
    case 6: return "Public-Key";
    case 7: return "Secret-Subkey";
    case 8: return "Compressed Data";
    case 9: return "Symmetrically Encrypted Data";
    case 10: return "Marker";
    case 11: return "Literal Data";
    case 12: return "Trust";
    case 13: return "User ID";
    case 14: return "Public-Subkey";
    case 17: return "User Attribute";
    case 18: return "Sym. Encrypted and Integrity Protected Data";
    case 19: return "Modification Detection Code";
    default: return "Unknown";
  }
}

std::string_view pgp_algorithm(int id) {
  switch (id) {
    case 1: case 2: case 3: return "RSA";
    case 16: return "Elgamal";
    case 17: return "DSA";
    case 18: return "ECDH";
    case 19: return "ECDSA";
    case 22: return "EdDSA (legacy)";
    case 25: return "X25519";
    case 27: return "Ed25519";
    default: return "unknown";
  }
}

std::string iso_time(std::uint32_t t) {
  // Civil-from-days (Howard Hinnant), no time zone involved.
  const std::int64_t days = t / 86400;
  const std::uint32_t secs = t % 86400;
  std::int64_t z = days + 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const std::int64_t doe = z - era * 146097;
  const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  std::int64_t y = yoe + era * 400;
  const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const std::int64_t mp = (5 * doy + 2) / 153;
  const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
  const std::int64_t m = mp < 10 ? mp + 3 : mp - 9;
  if (m <= 2) ++y;
  return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", y, m, d, secs / 3600, secs / 60 % 60, secs % 60);
}

Result<Json> pgp_packets(ByteView data) {
  Json packets = Json::array();
  util::ByteReader r(data);
  while (!r.at_end()) {
    if (packets.size() > 100000) return fail(Status::Limit, "too many OpenPGP packets");
    const std::uint8_t head = r.u8();
    if ((head & 0x80) == 0) return fail(Status::Decode, "bad OpenPGP packet header");
    int tag = 0;
    std::uint64_t length = 0;
    bool partial = false;
    if (head & 0x40) {  // new format
      tag = head & 0x3F;
      const std::uint8_t b0 = r.u8();
      if (b0 < 192) {
        length = b0;
      } else if (b0 < 224) {
        length = ((std::uint64_t{b0} - 192) << 8) + r.u8() + 192;
      } else if (b0 == 255) {
        length = r.u32be();
      } else {
        length = std::uint64_t{1} << (b0 & 0x1F);
        partial = true;
      }
    } else {
      tag = (head >> 2) & 0x0F;
      const int type = head & 3;
      if (type == 0) length = r.u8();
      else if (type == 1) length = r.u16be();
      else if (type == 2) length = r.u32be();
      else length = r.remaining();
    }
    if (!r.ok() || length > r.remaining()) return fail(Status::Decode, "OpenPGP packet is truncated");
    const ByteView body = r.bytes(static_cast<std::size_t>(length));
    Json p = Json::object({{"tag", tag}, {"type", std::string(packet_name(tag))}, {"length", length}});
    if (partial) p["partial_length"] = true;
    util::ByteReader b(body);
    if (tag == 6 || tag == 14 || tag == 5 || tag == 7) {
      const std::uint8_t version = b.u8();
      p["version"] = version;
      if (version >= 4) {
        p["created"] = iso_time(b.u32be());
        if (version >= 5) (void)b.u32be();
        const std::uint8_t alg = b.u8();
        p["algorithm"] = std::string(pgp_algorithm(alg));
        if (alg <= 3 && b.remaining() >= 2) p["bits"] = b.u16be();
      }
      if (version == 4 && (tag == 6 || tag == 14)) {
        // v4 fingerprint: SHA-1 over 0x99, length, body.
        util::ByteWriter w(body.size() + 3);
        w.u8(0x99).u16be(static_cast<std::uint16_t>(body.size())).bytes(body);
        const std::string fp = util::sha1_hex(w.view());
        p["fingerprint"] = util::to_upper(fp);
        p["key_id"] = util::to_upper(fp.substr(fp.size() - 16));
      }
      if (tag == 5 || tag == 7) p["note"] = "secret key material not shown or decrypted";
    } else if (tag == 13) {
      p["user_id"] = util::sanitize_utf8(as_chars(body));
    } else if (tag == 2) {
      const std::uint8_t version = b.u8();
      p["version"] = version;
      if (version == 4) {
        p["signature_type"] = std::format("0x{:02x}", b.u8());
        p["algorithm"] = std::string(pgp_algorithm(b.u8()));
        p["hash"] = b.u8();
        const std::uint16_t hashed = b.u16be();
        Bytes areas;
        append(areas, b.bytes(std::min<std::size_t>(hashed, b.remaining())));
        const std::uint16_t unhashed = b.u16be();
        append(areas, b.bytes(std::min<std::size_t>(unhashed, b.remaining())));
        util::ByteReader sub(areas);
        while (sub.remaining() >= 2) {
          std::uint32_t len = sub.u8();
          if (len >= 192 && len < 255) len = ((len - 192) << 8) + sub.u8() + 192;
          else if (len == 255) len = sub.u32be();
          if (len == 0 || len > sub.remaining()) break;
          util::ByteReader item(sub.bytes(len));
          const std::uint8_t type = item.u8() & 0x7F;
          if (type == 2 && item.remaining() >= 4) p["created"] = iso_time(item.u32be());
          if (type == 16 && item.remaining() >= 8) p["issuer"] = util::to_upper(util::hex_encode(item.bytes(8)));
          if (type == 33 && item.remaining() >= 1) {
            (void)item.u8();
            p["issuer_fingerprint"] = util::to_upper(util::hex_encode(item.rest()));
          }
        }
      }
    } else if (tag == 11) {
      p["format"] = std::string(1, static_cast<char>(b.u8()));
      const std::uint8_t n = b.u8();
      p["filename"] = util::sanitize_utf8(b.chars(std::min<std::size_t>(n, b.remaining())));
    }
    packets.push_back(std::move(p));
  }
  return packets;
}

Result<Json> read_pgp_armor(std::string_view text) {
  const auto lines = lines_of(text);
  std::size_t i = 0;
  while (i < lines.size() && !util::trim(lines[i]).starts_with("-----BEGIN PGP ")) ++i;
  if (i >= lines.size()) return fail(Status::Decode, "no OpenPGP armour found");
  const std::string_view begin = util::trim(lines[i]);
  const std::string type(begin.substr(15, begin.size() - 20));
  Json headers = Json::object();
  for (++i; i < lines.size() && !util::trim(lines[i]).empty(); ++i) {
    const std::string_view l = util::trim(lines[i]);
    const std::size_t colon = l.find(": ");
    if (colon == std::string_view::npos) break;
    headers[std::string(l.substr(0, colon))] = std::string(l.substr(colon + 2));
  }
  std::string body;
  std::optional<std::uint32_t> crc;
  for (++i; i < lines.size(); ++i) {
    const std::string_view l = util::trim(lines[i]);
    if (l.starts_with("-----END")) break;
    if (l.starts_with('=') && l.size() == 5) {
      CVT_TRY_ASSIGN(const Bytes c, util::base64_decode(l.substr(1)));
      if (c.size() == 3) crc = (std::uint32_t{static_cast<std::uint8_t>(c[0])} << 16) | (std::uint32_t{static_cast<std::uint8_t>(c[1])} << 8) | static_cast<std::uint8_t>(c[2]);
      continue;
    }
    body += l;
  }
  CVT_TRY_ASSIGN(const Bytes data, b64_strict(body));
  if (crc && *crc != crc24(data)) return fail(Status::Decode, "OpenPGP armour checksum mismatch");
  CVT_TRY_ASSIGN(Json packets, pgp_packets(data));
  return Json::object({{"armor", type}, {"headers", std::move(headers)}, {"data", b64(data)}, {"packets", std::move(packets)}});
}

std::string pgp_armor(const Json& data, ByteView bytes, bool strip) {
  std::string type = data["armor"].as_string();
  if (type.empty()) {
    // From the first packet.
    const int tag = bytes.empty() ? 0 : ((static_cast<std::uint8_t>(bytes[0]) & 0x40) ? (static_cast<std::uint8_t>(bytes[0]) & 0x3F)
                                                                                         : ((static_cast<std::uint8_t>(bytes[0]) >> 2) & 0x0F));
    type = tag == 6 ? "PUBLIC KEY BLOCK" : tag == 5 ? "PRIVATE KEY BLOCK" : tag == 2 ? "SIGNATURE" : "MESSAGE";
  }
  std::string out = "-----BEGIN PGP " + type + "-----\n";
  if (!strip) {
    for (const auto& [k, v] : data["headers"].items()) out += k + ": " + v.as_string() + "\n";
  }
  out += "\n";
  util::Base64Options o;
  o.line_width = 64;
  out += util::base64_encode(bytes, o) + "\n";
  const std::uint32_t c = crc24(bytes);
  const std::uint8_t crc_bytes[] = {static_cast<std::uint8_t>(c >> 16), static_cast<std::uint8_t>(c >> 8), static_cast<std::uint8_t>(c)};
  out += "=" + util::base64_encode(as_bytes(std::span<const std::uint8_t>(crc_bytes))) + "\n";
  out += "-----END PGP " + type + "-----\n";
  return out;
}

// --------------------------------------------------------------------------- documents

ir::Document pki_document(const Json& data, std::string_view title) {
  ir::Document doc = data_document(title, data);
  return doc;
}

// --------------------------------------------------------------------------- codecs

Result<std::vector<PkiObject>> objects_from_data(const Json& data) {
  std::vector<PkiObject> out;
  for (const Json& o : data["objects"].elements()) {
    PkiObject obj;
    obj.label = o["label"].as_string();
    for (const auto& [k, v] : o["headers"].items()) obj.headers.emplace_back(k, v.as_string());
    CVT_TRY_ASSIGN(obj.der, util::base64_decode(o["der"].as_string()));
    out.push_back(std::move(obj));
  }
  return out;
}

Result<std::vector<PkiObject>> read_any_pki(ByteView input, std::string_view format) {
  if (format == "der") {
    CVT_TRY_ASSIGN(const Node n, asn1::parse(input));
    PkiObject o{infer_label(n), {}, Bytes(input.begin(), input.end())};
    return std::vector<PkiObject>{std::move(o)};
  }
  if (format == "p7b") {
    PkiObject bag;
    const std::string_view head = head_text(input, 64);
    if (head.find("-----BEGIN") != std::string_view::npos) {
      CVT_TRY_ASSIGN(auto blocks, parse_pem(input_text(input)));
      bag = std::move(blocks.front());
    } else {
      bag.der.assign(input.begin(), input.end());
    }
    return p7_contents(bag);
  }
  CVT_TRY_ASSIGN(auto blocks, parse_pem(input_text(input)));
  // PKCS#7 blocks inside a PEM file are expanded.
  std::vector<PkiObject> out;
  for (auto& b : blocks) {
    if (b.label == "PKCS7" || b.label == "CERTIFICATE CHAIN") {
      auto inner = p7_contents(b);
      if (inner) {
        for (auto& i : *inner) out.push_back(std::move(i));
        continue;
      }
    }
    out.push_back(std::move(b));
  }
  return out;
}

Result<Bytes> write_pki(const Json& data, std::string_view format, WriteContext& ctx) {
  CVT_TRY_ASSIGN(std::vector<PkiObject> objects, objects_from_data(data));
  if (data.contains("keys") || (objects.empty() && data.contains("kty"))) {
    // SSH / JWK keys become PUBLIC KEY objects.
    Json keys_only = Json::object({{"keys", data["keys"]}});
    if (auto keys = public_keys(keys_only, ctx)) {
      for (Bytes& der : *keys) objects.push_back(PkiObject{"PUBLIC KEY", {}, std::move(der)});
    }
  }
  if (objects.empty()) return fail(Status::Encode, "the data holds no certificates or keys");
  if (format == "der") {
    if (objects.size() > 1) ctx.warn("DER holds one object; only the first was written (use PEM or PKCS#7 for several).");
    if (!objects.front().headers.empty()) return fail(Status::Encode, "legacy-encrypted PEM keys have no DER form");
    return objects.front().der;
  }
  if (format == "p7b") {
    std::size_t certs = 0;
    for (const auto& o : objects) certs += o.label == "CERTIFICATE" || o.label == "X509 CRL" ? 1 : 0;
    if (certs == 0) return fail(Status::Encode, "a PKCS#7 bag holds certificates and CRLs only");
    if (certs < objects.size()) ctx.warn("Objects other than certificates and CRLs were left out of the PKCS#7 bag.");
    const Bytes bag = p7_bag(objects);
    if (ctx.options.get_string("p7b_encoding", "der") == "pem") return to_bytes(pem_text(PkiObject{"PKCS7", {}, bag}));
    return bag;
  }
  std::string out;
  for (const auto& o : objects) out += pem_text(o);
  return to_bytes(out);
}

int sniff_pem(ByteView data, std::string_view /*name*/) {
  const std::string_view head = head_text(data, 8192);
  const std::size_t at = head.find("-----BEGIN ");
  if (at == std::string_view::npos || head.find("-----BEGIN PGP") != std::string_view::npos) return 0;
  if (head.find("-----BEGIN PKCS7") != std::string_view::npos) return 60;
  return 93;
}

int sniff_der(ByteView data, std::string_view name) {
  if (data.size() < 4 || data[0] != std::byte{0x30}) return 0;
  auto n = asn1::parse(data, 12);
  if (!n) return 0;
  const std::string label = infer_label(*n);
  if (label == "UNKNOWN") return 0;
  if (label == "PKCS7") return has_extension(name, "p7b") || has_extension(name, "p7c") ? 0 : 60;
  return 88;
}

void register_pki_format(Registry& registry, const std::string& id, std::vector<OptionSpec> write_options) {
  add_codec(
      registry, id, Kind::Data,
      [id](ReadContext& ctx) -> Result<AnyIR> {
        CVT_TRY_ASSIGN(const auto objects, read_any_pki(ctx.input, id));
        return ir::Data{Json::object({{"objects", objects_json(objects, ctx.options.get_bool("asn1_tree", false))}})};
      },
      [id](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> { return write_pki(value.get<ir::Data>().value, id, ctx); },
      {OptionSpec::boolean("asn1_tree", "ASN.1 tree", false, "Also include the generic ASN.1 structure of public objects.")},
      std::move(write_options), 10, 10);
  add_codec(
      registry, id, Kind::Document,
      [id](ReadContext& ctx) -> Result<AnyIR> {
        CVT_TRY_ASSIGN(const auto objects, read_any_pki(ctx.input, id));
        Json summary = Json::array();
        for (const auto& o : objects) summary.push_back(Json::object({{"label", o.label}, {"summary", summary_of(o)}}));
        return pki_document(Json::object({{"objects", std::move(summary)}}), "Certificates and keys");
      },
      nullptr, {}, {}, 15, 10, true);
}

}  // namespace

void register_pki(Registry& registry) {
  add_format(registry, {.id = "pem", .name = "PEM certificate/key", .description = "Base64 PEM blocks: certificates, requests, CRLs, keys.",
                        .category = Category::Encoding, .extensions = {"pem", "crt", "cer", "key", "csr", "pub", "crl"},
                        .mime = "application/x-pem-file", .popularity = 35, .sniff = sniff_pem});
  register_pki_format(registry, "pem", {});
  add_format(registry, {.id = "der", .name = "DER certificate/key", .description = "Binary ASN.1 DER certificate, request, CRL or key.",
                        .category = Category::Encoding, .extensions = {"der", "cer", "crt"}, .mime = "application/pkix-cert",
                        .popularity = 25, .sniff = sniff_der});
  register_pki_format(registry, "der", {});
  add_format(registry, {.id = "p7b", .name = "PKCS#7 certificate bag", .description = "Certificates and CRLs in a degenerate PKCS#7 SignedData.",
                        .category = Category::Encoding, .extensions = {"p7b", "p7c", "spc"}, .mime = "application/x-pkcs7-certificates",
                        .popularity = 15,
                        .sniff = [](ByteView d, std::string_view name) {
                          if (head_text(d, 64).find("-----BEGIN PKCS7") != std::string_view::npos) return 95;
                          if (d.size() > 16 && d[0] == std::byte{0x30}) {
                            if (auto n = asn1::parse(d, 12); n && infer_label(*n) == "PKCS7") return 92;
                          }
                          return has_extension(name, "p7b") ? 40 : 0;
                        }});
  register_pki_format(registry, "p7b", {OptionSpec::choice("p7b_encoding", "Encoding", "der", {"der", "pem"}, "Binary DER or a PEM 'PKCS7' block.")});

  // OpenSSH public keys.
  const auto ssh_read = [](bool known_hosts) {
    return [known_hosts](ReadContext& ctx) -> Result<AnyIR> {
      CVT_TRY_ASSIGN(Json keys, read_ssh_keys(input_text(ctx.input), known_hosts));
      return ir::Data{Json::object({{"keys", std::move(keys)}})};
    };
  };
  const auto ssh_write = [](bool known_hosts) {
    return [known_hosts](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
      const Json& data = value.get<ir::Data>().value;
      std::string out;
      const bool keep_comment = !options::resolved_strip_metadata(ctx.options);
      for (const Json& k : data["keys"].elements()) {
        std::string prefix;
        if (known_hosts) {
          for (const Json& h : k["hosts"].elements()) prefix += (prefix.empty() ? "" : ",") + h.as_string();
          if (prefix.empty()) return fail(Status::Encode, "known_hosts entries need host names");
          prefix += " ";
        } else {
          for (const Json& o : k["options"].elements()) prefix += (prefix.empty() ? "" : ",") + o.as_string();
          if (!prefix.empty()) prefix += " ";
        }
        if (const Json* marker = k.find("marker")) prefix = "@" + marker->as_string() + " " + prefix;
        const std::string comment = keep_comment ? k["comment"].as_string() : std::string();
        out += prefix + k["type"].as_string() + " " + k["blob"].as_string() + (comment.empty() ? "" : " " + comment) + "\n";
      }
      if (data["keys"].empty()) {
        CVT_TRY_ASSIGN(const auto spkis, public_keys(data, ctx));
        for (const Bytes& spki : spkis) {
          CVT_TRY_ASSIGN(const auto ssh, spki_to_ssh(spki));
          out += ssh.first + " " + b64(ssh.second) + "\n";
        }
      }
      if (out.empty()) return fail(Status::Encode, "no public keys to write");
      return to_bytes(out);
    };
  };
  const std::vector<OptionSpec> ssh_opts = {OptionSpec::boolean("strip_metadata", "Remove comments", true,
                                                                 "Drop key comments (often user@host).")};
  add_format(registry, {.id = "ssh-pub", .name = "OpenSSH public key", .description = "OpenSSH public key line (id_*.pub).",
                        .category = Category::Encoding, .extensions = {"pub"}, .mime = "text/plain", .popularity = 20,
                        .sniff = [](ByteView d, std::string_view) {
                          const std::string_view h = head_text(d, 64);
                          return h.starts_with("ssh-rsa AAAA") || h.starts_with("ssh-ed25519 AAAA") || h.starts_with("ecdsa-sha2-") ||
                                         h.starts_with("ssh-dss AAAA") || h.starts_with("sk-")
                                     ? 95
                                     : 0;
                        }});
  add_codec(registry, "ssh-pub", Kind::Data, ssh_read(false), ssh_write(false), {}, ssh_opts);
  add_format(registry, {.id = "authorized-keys", .name = "OpenSSH authorized_keys", .description = "Authorised public keys with options.",
                        .category = Category::Encoding, .extensions = {"authorized_keys"}, .mime = "text/plain", .popularity = 8,
                        .sniff = [](ByteView, std::string_view name) { return name == "authorized_keys" || name == "authorized_keys2" ? 90 : 0; }});
  add_codec(registry, "authorized-keys", Kind::Data, ssh_read(false), ssh_write(false), {}, ssh_opts);
  add_format(registry, {.id = "known-hosts", .name = "OpenSSH known_hosts", .description = "Known host keys.",
                        .category = Category::Encoding, .extensions = {"known_hosts"}, .mime = "text/plain", .popularity = 8,
                        .sniff = [](ByteView, std::string_view name) { return name == "known_hosts" ? 90 : 0; }});
  add_codec(registry, "known-hosts", Kind::Data, ssh_read(true), ssh_write(true), {}, ssh_opts);

  // JWK / JWKS.
  add_format(registry, {.id = "jwk", .name = "JSON Web Key", .description = "JWK or JWK Set (public keys).",
                        .category = Category::Encoding, .extensions = {"jwk", "jwks"}, .mime = "application/jwk+json", .popularity = 12,
                        .sniff = [](ByteView d, std::string_view) {
                          const std::string_view h = head_text(d, 2048);
                          return h.find("\"kty\"") != std::string_view::npos ? 80 : 0;
                        }});
  add_codec(
      registry, "jwk", Kind::Data,
      [](ReadContext& ctx) -> Result<AnyIR> {
        CVT_TRY_ASSIGN(const Json json, util::parse_json(input_text(ctx.input)));
        std::vector<const Json*> list;
        if (json.contains("keys")) {
          for (const Json& k : json["keys"].elements()) list.push_back(&k);
        } else {
          list.push_back(&json);
        }
        std::vector<PkiObject> objects;
        for (const Json* k : list) {
          if (k->contains("d")) ctx.warn("Private JWK parameters were ignored; only the public key is kept.");
          CVT_TRY_ASSIGN(Bytes spki, jwk_to_spki(*k));
          objects.push_back(PkiObject{"PUBLIC KEY", {}, std::move(spki)});
        }
        Json data = Json::object({{"objects", objects_json(objects, false)}});
        return ir::Data{std::move(data)};
      },
      [](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
        CVT_TRY_ASSIGN(const auto spkis, public_keys(value.get<ir::Data>().value, ctx));
        Json keys = Json::array();
        for (const Bytes& spki : spkis) {
          CVT_TRY_ASSIGN(Json jwk, spki_to_jwk(spki));
          keys.push_back(std::move(jwk));
        }
        const Json out = keys.size() == 1 ? keys[0] : Json::object({{"keys", std::move(keys)}});
        return to_bytes(util::dump_json(out, 2) + "\n");
      });

  // JWT (decode only).
  add_format(registry, {.id = "jwt", .name = "JSON Web Token", .description = "Compact JWT/JWS/JWE; decoded without verification.",
                        .category = Category::Encoding, .extensions = {"jwt"}, .mime = "application/jwt", .popularity = 15,
                        .sniff = [](ByteView d, std::string_view) {
                          const std::string_view h = util::trim(head_text(d, 64));
                          return h.starts_with("eyJ") && h.find('.') != std::string_view::npos ? 85 : 0;
                        }});
  add_codec(registry, "jwt", Kind::Data,
            [](ReadContext& ctx) -> Result<AnyIR> {
              const std::string text(util::trim(input_text(ctx.input)));
              const auto parts = util::split(text, '.');
              if (parts.size() != 3 && parts.size() != 5) return fail(Status::Decode, "a JWT has 3 (JWS) or 5 (JWE) parts");
              Json out = Json::object();
              CVT_TRY_ASSIGN(const Bytes header, b64url_decode(parts[0]));
              CVT_TRY_ASSIGN(out["header"], util::parse_json(as_chars(header)));
              if (parts.size() == 5) {
                out["note"] = "JWE: the payload is encrypted and was not decrypted";
                return ir::Data{std::move(out)};
              }
              CVT_TRY_ASSIGN(const Bytes payload, b64url_decode(parts[1]));
              auto json = util::parse_json(as_chars(payload));
              out["payload"] = json ? std::move(*json) : Json(util::sanitize_utf8(as_chars(payload)));
              Json times = Json::object();
              for (const std::string_view claim : {"iat", "nbf", "exp"}) {
                if (const Json* v = out["payload"].find(claim); v != nullptr && v->is_number() && v->as_double() >= 0 && v->as_double() < 4e9) {
                  times[claim] = iso_time(static_cast<std::uint32_t>(v->as_double()));
                }
              }
              if (!times.empty()) out["times"] = std::move(times);
              out["signature"] = std::string(parts[2]);
              out["signature_verified"] = false;
              out["warning"] = "The signature was NOT verified; do not trust these claims.";
              return ir::Data{std::move(out)};
            },
            nullptr);

  // OpenPGP.
  add_format(registry, {.id = "asc", .name = "OpenPGP armoured data", .description = "ASCII-armoured OpenPGP keys, signatures or messages.",
                        .category = Category::Encoding, .extensions = {"asc"}, .mime = "application/pgp-keys", .popularity = 15,
                        .sniff = [](ByteView d, std::string_view) { return head_text(d, 1024).find("-----BEGIN PGP ") != std::string_view::npos ? 95 : 0; }});
  add_format(registry, {.id = "gpg", .name = "OpenPGP binary data", .description = "Binary OpenPGP packets (keys, signatures, messages).",
                        .category = Category::Encoding, .extensions = {"gpg", "pgp", "sig"}, .mime = "application/pgp-encrypted", .popularity = 12,
                        .sniff = [](ByteView d, std::string_view name) {
                          if (d.size() < 3 || (static_cast<std::uint8_t>(d[0]) & 0x80) == 0) return 0;
                          const auto tag = (static_cast<std::uint8_t>(d[0]) & 0x40) ? (static_cast<std::uint8_t>(d[0]) & 0x3F)
                                                                                     : ((static_cast<std::uint8_t>(d[0]) >> 2) & 0x0F);
                          const bool plausible = tag == 1 || tag == 2 || tag == 3 || tag == 4 || tag == 5 || tag == 6 || tag == 8 || tag == 18;
                          if (!plausible) return 0;
                          return has_extension(name, "gpg") || has_extension(name, "pgp") || has_extension(name, "sig") ? 70 : 0;
                        }});
  add_codec(registry, "asc", Kind::Data,
            [](ReadContext& ctx) -> Result<AnyIR> {
              CVT_TRY_ASSIGN(Json data, read_pgp_armor(input_text(ctx.input)));
              return ir::Data{std::move(data)};
            },
            [](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
              const Json& data = value.get<ir::Data>().value;
              CVT_TRY_ASSIGN(const Bytes bytes, util::base64_decode(data["data"].as_string()));
              if (bytes.empty()) return fail(Status::Encode, "no OpenPGP data");
              return to_bytes(pgp_armor(data, bytes, options::resolved_strip_metadata(ctx.options)));
            },
            {}, {options::strip_metadata()});
  add_codec(registry, "gpg", Kind::Data,
            [](ReadContext& ctx) -> Result<AnyIR> {
              CVT_TRY_ASSIGN(Json packets, pgp_packets(ctx.input));
              return ir::Data{Json::object({{"data", b64(ctx.input)}, {"packets", std::move(packets)}})};
            },
            [](const AnyIR& value, WriteContext&) -> Result<Bytes> {
              CVT_TRY_ASSIGN(Bytes bytes, util::base64_decode(value.get<ir::Data>().value["data"].as_string()));
              if (bytes.empty()) return fail(Status::Encode, "no OpenPGP data");
              CVT_TRY(pgp_packets(bytes));
              return bytes;
            });
}

}  // namespace cvt::science
