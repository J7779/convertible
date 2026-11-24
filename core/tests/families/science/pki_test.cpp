// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/science/asn1.hpp"
#include "families/science/science_test.hpp"

using namespace cvt;
using test::science::detected;

namespace {

Bytes pki(const std::string& name) { return test::fixture("science/pki/" + name); }

Json read(const std::string& format, const std::string& name, std::string_view options = {}) {
  const auto data = test::read_as<ir::Data>(format, pki(name), options);
  if (!data) {
    FAIL_CHECK(name, ": ", data.error().message);
    return Json();
  }
  return data->value;
}

}  // namespace

TEST_CASE("science/pem: OpenSSL certificate fields, extensions and fingerprint") {
  const Json data = read("pem", "rsa_cert.pem");
  const Json& c = data["objects"][0]["summary"];
  CHECK(data["objects"][0]["label"].as_string() == "CERTIFICATE");
  CHECK(c["version"].as_int64() == 3);
  CHECK(c["serial"].as_string() == "4660");
  CHECK(c["subject"].as_string() == "C=XX, O=Test Fixtures, CN=test.example");
  CHECK(c["issuer"] == c["subject"]);
  CHECK(c["not_before"].as_string() == "2025-01-01T00:00:00Z");
  CHECK(c["not_after"].as_string() == "2035-01-01T00:00:00Z");
  CHECK(c["signature_algorithm"].as_string() == "sha256WithRSAEncryption");
  CHECK(c["public_key"]["bits"].as_int64() == 2048);
  CHECK(c["public_key"]["exponent"].as_string() == "65537");
  CHECK(c["extensions"]["subjectAltName"] == Json::array({"DNS:test.example", "DNS:www.test.example", "IP:192.0.2.1"}));
  CHECK(c["extensions"]["keyUsage"]["critical"].as_bool());
  CHECK(c["extensions"]["keyUsage"]["value"] == Json::array({"digitalSignature", "keyEncipherment"}));
  CHECK(c["extensions"]["extKeyUsage"] == Json::array({"serverAuth", "clientAuth"}));
  // openssl x509 -fingerprint -sha256
  CHECK(c["sha256_fingerprint"].as_string() == "3e233f4c06023cf38552e2667b8bfed570cdb6f3a0c417ddbc17dc149399ebd9");
  CHECK(detected(pki("rsa_cert.pem")) == "pem");
  CHECK(detected(pki("rsa_cert.der"), "c.der") == "der");
}

TEST_CASE("science/pem: requests, CRLs, keys and encrypted keys") {
  const Json csr = read("pem", "ec.csr")["objects"][0];
  CHECK(csr["label"].as_string() == "CERTIFICATE REQUEST");
  CHECK(csr["summary"]["subject"].as_string() == "CN=test.example");
  CHECK(csr["summary"]["public_key"]["curve"].as_string() == "prime256v1");
  CHECK(csr["summary"]["extensions"]["subjectAltName"] == Json::array({"DNS:test.example"}));
  const Json crl = read("pem", "test.crl")["objects"][0]["summary"];
  CHECK(crl["issuer"].as_string() == "CN=ca.test.example");
  CHECK(crl["this_update"].as_string() == "2025-06-01T00:00:00Z");
  CHECK(crl["next_update"].as_string() == "2025-07-01T00:00:00Z");
  CHECK(crl["revoked"].size() == 2);
  CHECK(crl["extensions"]["cRLNumber"].as_string() == "5");
  const Json key = read("pem", "rsa_key.pem")["objects"][0];
  CHECK(key["label"].as_string() == "PRIVATE KEY");
  CHECK(key["summary"]["bits"].as_int64() == 2048);
  CHECK_FALSE(key.contains("asn1"));
  const Json ed = read("pem", "ed_pub.pem")["objects"][0]["summary"];
  CHECK(ed["algorithm"]["name"].as_string() == "Ed25519");
  const Json enc = read("pem", "rsa_key_encrypted.pem")["objects"][0];
  CHECK(enc["label"].as_string() == "ENCRYPTED PRIVATE KEY");
  CHECK(enc["summary"]["kdf"].as_string() == "pbkdf2");
  CHECK(enc["summary"]["cipher"].as_string() == "aes256-CBC");
  // Encrypted keys pass through byte for byte.
  const auto again = test::write_from("pem", ir::Data{read("pem", "rsa_key_encrypted.pem")});
  REQUIRE_OK(again);
  CHECK(test::str(*again) == test::str(pki("rsa_key_encrypted.pem")));
}

TEST_CASE("science/pem: PEM <-> DER and PKCS#7 bags") {
  // DER -> PEM reproduces OpenSSL's PEM exactly.
  const auto pem = test::convert(pki("rsa_cert.der"), "der", "pem");
  REQUIRE(pem.ok());
  CHECK(pem.text() == test::str(pki("rsa_cert.pem")));
  const auto der = test::convert(pki("rsa_cert.pem"), "pem", "der");
  REQUIRE(der.ok());
  CHECK(der.data == pki("rsa_cert.der"));
  const auto pub = test::convert(pki("rsa_pub.der"), "der", "pem");
  REQUIRE(pub.ok());
  CHECK(pub.text().starts_with("-----BEGIN PUBLIC KEY-----\n"));
  // p7b from openssl crl2pkcs7 -> PEM bundle equals the input bundle.
  const auto bundle = test::convert(pki("bundle.p7b"), "p7b", "pem");
  REQUIRE(bundle.ok());
  CHECK(bundle.text() == test::str(pki("bundle.pem")));
  const auto bag = test::convert(pki("bundle.pem"), "pem", "p7b");
  REQUIRE(bag.ok());
  CHECK(bag.data == pki("bundle.p7b"));
  CHECK(detected(pki("bundle.p7b"), "b.p7b") == "p7b");
  // Strict Base64.
  std::string broken = test::str(pki("rsa_cert.pem"));
  broken[100] = '*';
  CHECK_STATUS(test::read_as<ir::Data>("pem", as_bytes(broken)), Status::Decode);
}

TEST_CASE("science/ssh: OpenSSH keys, authorized_keys, known_hosts and SPKI re-encoding") {
  const Json ed = read("ssh-pub", "ssh_ed25519.pub")["keys"][0];
  CHECK(ed["type"].as_string() == "ssh-ed25519");
  CHECK(ed["bits"].as_int64() == 256);
  CHECK(ed["comment"].as_string().empty());
  // ssh-keygen -l -E sha256
  CHECK(ed["fingerprint_sha256"].as_string() == "SHA256:HWEmHEBaRozHAeNeFHJ86KOOoyDQtmbfZI/XL/rqgrw");
  CHECK(read("ssh-pub", "ssh_rsa.pub")["keys"][0]["bits"].as_int64() == 2048);
  CHECK(read("ssh-pub", "ssh_ecdsa.pub")["keys"][0]["bits"].as_int64() == 384);
  const Json auth = read("authorized-keys", "authorized_keys")["keys"];
  REQUIRE(auth.size() == 2);
  CHECK(auth[0]["options"] == Json::array({"from=\"192.0.2.0/24\"", "no-pty"}));
  const Json hosts = read("known-hosts", "known_hosts")["keys"];
  REQUIRE(hosts.size() == 2);
  CHECK(hosts[0]["hosts"] == Json::array({"test.example", "192.0.2.1"}));
  CHECK(hosts[1]["marker"].as_string() == "cert-authority");
  // OpenSSH -> SPKI matches ssh-keygen -e -m PKCS8 / cryptography.
  for (const char* name : {"ssh_ed25519", "ssh_rsa", "ssh_ecdsa"}) {
    INFO(name);
    const auto spki = test::convert(pki(std::string(name) + ".pub"), "ssh-pub", "pem");
    REQUIRE(spki.ok());
    CHECK(spki.text() == test::str(pki(std::string(name) + "_spki.pem")));
    const auto back = test::convert(pki(std::string(name) + "_spki.pem"), "pem", "ssh-pub");
    REQUIRE(back.ok());
    std::string original = test::str(pki(std::string(name) + ".pub"));
    while (!original.empty() && (original.back() == '\n' || original.back() == ' ')) original.pop_back();
    CHECK(back.text() == original + "\n");
  }
  CHECK(detected(pki("ssh_ed25519.pub")) == "ssh-pub");
  CHECK(detected(pki("known_hosts"), "known_hosts") == "known-hosts");
}

TEST_CASE("science/jwk: jwcrypto keys <-> PEM, JWT decoding") {
  for (const char* name : {"ec_pub", "ed_pub"}) {
    INFO(name);
    const auto pem = test::convert(pki(std::string(name) + ".jwk"), "jwk", "pem");
    REQUIRE(pem.ok());
    CHECK(pem.text() == test::str(pki(std::string(name) + ".pem")));
    const auto jwk = test::convert(pki(std::string(name) + ".pem"), "pem", "jwk");
    REQUIRE(jwk.ok());
    const auto ours = util::parse_json(jwk.text());
    const auto theirs = util::parse_json(test::str(pki(std::string(name) + ".jwk")));
    REQUIRE_OK(ours);
    REQUIRE_OK(theirs);
    for (const char* field : {"kty", "crv", "x", "y"}) CHECK((*ours)[field] == (*theirs)[field]);
  }
  const Json jwt = read("jwt", "token.jwt");
  CHECK(jwt["header"]["alg"].as_string() == "HS256");
  CHECK(jwt["header"]["kid"].as_string() == "k1");
  CHECK(jwt["payload"]["sub"].as_string() == "test-subject");
  CHECK(jwt["times"]["iat"].as_string() == "2023-11-14T22:13:20Z");
  CHECK(jwt["signature_verified"].as_bool(true) == false);
  CHECK(detected(pki("token.jwt")) == "jwt");
}

TEST_CASE("science/pgp: GnuPG armour and packets") {
  const Json armored = read("pgp-armor", "pubkey.asc");
  CHECK(armored["armor"].as_string() == "PUBLIC KEY BLOCK");
  const Json& packets = armored["packets"];
  REQUIRE(packets.size() == 3);
  CHECK(packets[0]["type"].as_string() == "Public-Key");
  CHECK(packets[0]["algorithm"].as_string() == "EdDSA (legacy)");
  CHECK(packets[0]["key_id"].as_string() == "45400F307C40605D");  // gpg --list-packets
  CHECK(packets[1]["user_id"].as_string() == "Test Key <test@test.example>");
  CHECK(packets[2]["issuer"].as_string() == "45400F307C40605D");
  const Json binary = read("pgp", "pubkey.gpg");
  CHECK(binary["packets"] == packets);
  // Armour <-> binary.
  const auto bin = test::convert(pki("pubkey.asc"), "pgp-armor", "pgp");
  REQUIRE(bin.ok());
  CHECK(bin.data == pki("pubkey.gpg"));
  const auto asc = test::convert(pki("pubkey.gpg"), "pgp", "pgp-armor");
  REQUIRE(asc.ok());
  const auto back = test::read_as<ir::Data>("pgp-armor", asc.data);
  REQUIRE_OK(back);
  CHECK(back->value["data"] == binary["data"]);
  CHECK(read("pgp-armor", "msg.txt.asc")["packets"][0]["type"].as_string() == "Signature");
  CHECK(detected(pki("pubkey.asc")) == "pgp-armor");
  CHECK(detected(pki("pubkey.gpg"), "k.gpg") == "pgp");
}

TEST_CASE("science/asn1: torture inputs") {
  using namespace std::string_view_literals;
  CHECK_STATUS(science::asn1::parse(as_bytes("\x30\x80\x00\x00"sv)), Status::Decode);          // indefinite length
  CHECK_STATUS(science::asn1::parse(as_bytes("\x30\x81\x05\x01\x00\x00\x00\x00"sv)), Status::Decode);  // non-minimal
  CHECK_STATUS(science::asn1::parse(as_bytes("\x30\x05\x02\x01"sv)), Status::Decode);          // truncated
  CHECK_STATUS(science::asn1::parse(as_bytes("\x02\x01\x00\x00"sv)), Status::Decode);          // trailing byte
  CHECK_STATUS(science::asn1::parse(as_bytes("\x30\x89\xff\xff\xff\xff\xff\xff\xff\xff\xff"sv)), Status::Decode);
  std::string deep;
  for (int i = 0; i < 200; ++i) deep += "\x30\x80";
  CHECK(!science::asn1::parse(as_bytes(deep)).has_value());
  // Nested 100 levels with definite lengths: refused by the depth limit.
  Bytes nested = science::asn1::tlv(0x05, {});
  for (int i = 0; i < 100; ++i) nested = science::asn1::tlv(0x30, nested);
  CHECK_STATUS(science::asn1::parse(nested), Status::Limit);
  const Bytes encoded = science::asn1::oid("1.2.840.113549.1.1.11");
  const auto oid = science::asn1::parse(encoded);
  REQUIRE_OK(oid);
  CHECK(science::asn1::oid_text(oid->value) == "1.2.840.113549.1.1.11");
  for (const auto& [format, name] : std::vector<std::pair<std::string, std::string>>{
           {"pem", "rsa_cert.pem"}, {"der", "rsa_cert.der"}, {"p7b", "bundle.p7b"}, {"pem", "test.crl"}, {"pem", "ec.csr"},
           {"ssh-pub", "ssh_rsa.pub"}, {"known-hosts", "known_hosts"}, {"jwk", "ec_pub.jwk"}, {"jwt", "token.jwt"},
           {"pgp-armor", "pubkey.asc"}, {"pgp", "pubkey.gpg"}}) {
    CHECK(test::fuzz_reader<ir::Data>(format, pki(name), 256) > 0);
  }
}
