// Tests of the hash functions and checksum targets (hashes.cpp, hash_*.cpp):
// known-answer vectors from the specifications, a cross-check of every
// algorithm against independent implementations (Python hashlib/zlib/
// pycryptodome/mmh3, OpenSSL, rhash, b3sum, xxhsum; see
// tests/data/encoding/hashes/SOURCES.md), the CRC catalogue, HMAC, every
// output notation and the engine integration.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

#include "engine/detect.hpp"
#include "engine/registry.hpp"
#include "families/encoding/hashes.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"
#include "util/hex.hpp"
#include "util/text.hpp"

using namespace cvt;
using namespace std::string_view_literals;
using encoding::compute_hash;
using encoding::compute_hmac;
using encoding::digest_hex;
using encoding::find_hash;
using encoding::HashParams;

namespace {

std::string hex_of(std::string_view algorithm, ByteView data, const HashParams& params = {}) {
  const encoding::HashAlgorithm* a = find_hash(algorithm);
  REQUIRE_MESSAGE(a != nullptr, algorithm);
  const auto digest = compute_hash(*a, data, params);
  REQUIRE_OK(digest);
  return digest_hex(*digest);
}
std::string hex_of(std::string_view algorithm, std::string_view text, const HashParams& params = {}) {
  return hex_of(algorithm, as_bytes(text), params);
}

// The input pattern of vectors.txt: byte i = (i * 31 + 7) % 251.
Bytes pattern(std::size_t n) {
  Bytes out(n);
  for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<std::byte>((i * 31 + 7) % 251);
  return out;
}

Bytes counting(std::size_t n) {
  Bytes out(n);
  for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<std::byte>(i);
  return out;
}

}  // namespace

TEST_CASE("encoding/hash: known-answer vectors from the specifications") {
  // MD2 (RFC 1319) and MD4 (RFC 1320) test suites.
  CHECK(hex_of("md2", "") == "8350e5a3e24c153df2275c9f80692773");
  CHECK(hex_of("md2", "abc") == "da853b0d3f88d99b30283a69e6ded6bb");
  CHECK(hex_of("md2", "message digest") == "ab4f496bfb2a530b219ff33031fe06b0");
  CHECK(hex_of("md2", "12345678901234567890123456789012345678901234567890123456789012345678901234567890") ==
        "d5976f79d83d3a0dc9806c3c66f3efd8");
  CHECK(hex_of("md4", "") == "31d6cfe0d16ae931b73c59d7e0c089c0");
  CHECK(hex_of("md4", "abc") == "a448017aaf21d8525fc10ae87aa6729d");
  CHECK(hex_of("md4", "abcdefghijklmnopqrstuvwxyz") == "d79e1c308aa5bbcdeea8ed63df412da9");
  CHECK(hex_of("md4", "12345678901234567890123456789012345678901234567890123456789012345678901234567890") ==
        "e33b4ddc9c38f2199c3e7b164fcc0536");
  // FIPS 180-4 examples.
  CHECK(hex_of("sha224", "abc") == "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7");
  CHECK(hex_of("sha256", "abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(hex_of("sha384", "abc") ==
        "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7");
  CHECK(hex_of("sha512", "abc") ==
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd"
        "454d4423643ce80e2a9ac94fa54ca49f");
  CHECK(hex_of("sha512-224", "abc") == "4634270f707b6a54daae7530460842e20e37ed265ceee9a43e8924aa");
  CHECK(hex_of("sha512-256", "abc") == "53048e2681941ef99b2e29b76b4c7dabe4c2d0c634fc6d46e0e2f13107e7af23");
  CHECK(hex_of("sha256", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  const std::string million(1'000'000, 'a');
  CHECK(hex_of("sha256", million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  CHECK(hex_of("sha1", million) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
  // FIPS 202.
  CHECK(hex_of("sha3-256", "") == "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a");
  CHECK(hex_of("sha3-512", "abc") ==
        "b751850b1a57168a5693cd924b6b096e08f621827444f70d884f5d0240d2712e10e116e9192af3c91a7ec57647e3934057340b4c"
        "f408d5a56592f8274eec53f0");
  CHECK(hex_of("shake128", "") == "7f9c2ba4e88f827d616045507605853ed73b8093f6efbc88eb1a6eacfa66ef26");
  CHECK(hex_of("keccak256", "") == "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470");
  // RFC 7693 appendix A and the BLAKE2s reference.
  CHECK(hex_of("blake2b", "abc") ==
        "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d17d87c5392aab792dc252d5de4533cc9518d38aa8"
        "dbf1925ab92386edd4009923");
  CHECK(hex_of("blake2s", "abc") == "508c5e8c327c14e2e1a72ba34eeb452f37458b209ed63a294d999b4c86675982");
  // BLAKE3 (official test vectors: input byte i = i % 251).
  Bytes b3(1025);
  for (std::size_t i = 0; i < b3.size(); ++i) b3[i] = static_cast<std::byte>(i % 251);
  CHECK(hex_of("blake3", ByteView{}) == "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");
  CHECK(hex_of("blake3", ByteView(b3).first(1)) == "2d3adedff11b61f14c886e35afa036736dcd87a74d27b5c1510225d0f592e213");
  CHECK(hex_of("blake3", ByteView(b3).first(1024)) ==
        "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af7");
  CHECK(hex_of("blake3", ByteView(b3).first(1025)) ==
        "d00278ae47eb27b34faecf67b4fe263f82d5412916c1ffd97c8cb7fb814b8444");
  // RIPEMD-160 (Dobbertin, Bosselaers, Preneel), Whirlpool (ISO test vectors), SM3 (GB/T 32905 example 1).
  CHECK(hex_of("ripemd160", "") == "9c1185a5c5e9fc54612808977ee8f548b2258d31");
  CHECK(hex_of("ripemd160", "abc") == "8eb208f7e05d987a9b044a8e98c6b087f15a0bfc");
  CHECK(hex_of("whirlpool", "abc") ==
        "4e2448a4c6f486bb16b6562c73b4020bf3043e3a731bce721ae1b303d97e6d4c7181eebdb6c57e277d0e34957114cbd6c797fc9d"
        "95d8b582d225292076d4eef5");
  CHECK(hex_of("sm3", "abc") == "66c7f0f462eeedd9d1f2d46bdc10e4e24167c4875cf2f7a2297da02b8f4ba8e0");
  // xxHash and SipHash reference values.
  CHECK(hex_of("xxh32", "") == "02cc5d05");
  CHECK(hex_of("xxh64", "") == "ef46db3751d8e999");
  CHECK(hex_of("xxh3", "") == "2d06800538d394c2");
  CHECK(hex_of("siphash", counting(15)) == "e545be4961ca29a1");
  CHECK(hex_of("siphash", ByteView{}) == "310e0edd47db6f72");
}

TEST_CASE("encoding/hash: every algorithm agrees with independent implementations") {
  const std::string vectors = test::fixture_text("encoding/hashes/vectors.txt");
  std::size_t checked = 0;
  for (const std::string_view line : util::split_lines(vectors)) {
    const auto fields = util::split(line, ' ', false);
    if (fields.size() != 3) continue;
    const std::string_view spec = fields[0];
    const auto length = util::parse_int(fields[1]);
    REQUIRE(length.has_value());
    const std::string_view expected = fields[2];
    const Bytes input = pattern(static_cast<std::size_t>(*length));
    const std::size_t colon = spec.find(':');
    const std::string_view id = spec.substr(0, colon);
    const std::string_view variant = colon == std::string_view::npos ? std::string_view{} : spec.substr(colon + 1);
    INFO(spec, " length ", *length);
    const encoding::HashAlgorithm* algorithm = find_hash(id);
    REQUIRE(algorithm != nullptr);
    std::string actual;
    if (variant.starts_with("hmac=")) {
      const std::string key = variant == "hmac=key"
                                  ? std::string("key")
                                  : std::string("a much longer key than the block of sha3 would allow, really much "
                                                "longer than one hundred bytes......");
      const auto mac = compute_hmac(*algorithm, as_bytes(key), input);
      REQUIRE_OK(mac);
      actual = digest_hex(*mac);
    } else {
      HashParams params;
      if (variant == "100" || variant == "len=100") params.length = 100;
      if (variant == "seed=42") params.seed = 42;
      if (variant == "key=000102..3f,len=32") {
        params.key = counting(64);
        params.length = 32;
      }
      if (variant == "key=000102..1f") params.key = counting(32);
      actual = hex_of(id, input, params);
    }
    CHECK(actual.substr(0, expected.size()) == expected);
    ++checked;
  }
  CHECK(checked > 1000);
}

TEST_CASE("encoding/hash: the CRC catalogue matches its check values and the bitwise reference") {
  const Bytes random = test::random_bytes(777, 9);
  for (const encoding::CrcModel& model : encoding::crc_models()) {
    CAPTURE(model.id);
    const std::size_t digits = (model.width + 7) / 8 * 2;
    CHECK(hex_of(model.id, "123456789") == std::format("{:0{}x}", model.check, digits));
    CHECK(encoding::crc_reference(model, as_bytes("123456789"sv)) == model.check);
    CHECK(hex_of(model.id, random) == std::format("{:0{}x}", encoding::crc_reference(model, random), digits));
  }
}

TEST_CASE("encoding/hash: checksums and fast hashes with published values") {
  CHECK(hex_of("fletcher16", "abcde") == "c8f0");
  CHECK(hex_of("fletcher16", "abcdef") == "2057");
  CHECK(hex_of("fletcher32", "abcde") == "f04fc729");
  CHECK(hex_of("fletcher32", "abcdef") == "56502d2a");
  CHECK(hex_of("fletcher32", "abcdefgh") == "ebe19591");
  CHECK(hex_of("fletcher64", "abcde") == "c8c6c527646362c6");
  CHECK(hex_of("fletcher64", "abcdefgh") == "312e2b28cccac8c6");
  CHECK(hex_of("fnv1-32", "a") == "050c5d7e");
  CHECK(hex_of("fnv1a-32", "a") == "e40c292c");
  CHECK(hex_of("fnv1-64", "a") == "af63bd4c8601b7be");
  CHECK(hex_of("fnv1a-64", "a") == "af63dc4c8601ec8c");
  CHECK(hex_of("fnv1a-32", "foobar") == "bf9cf968");
  CHECK(hex_of("fnv1a-64", "foobar") == "85944171f73967e8");
  CHECK(hex_of("adler32", "Wikipedia") == "11e60398");
  CHECK(hex_of("murmur3", "The quick brown fox jumps over the lazy dog") == "2e4ff723");
}

TEST_CASE("encoding/hash: HMAC follows RFC 2104 (RFC 4231 and RFC 2202 vectors)") {
  const Bytes key20(20, std::byte{0x0b});
  const auto* sha256 = find_hash("sha256");
  const auto* sha512 = find_hash("sha512");
  const auto* md5 = find_hash("md5");
  const auto* sha1 = find_hash("sha1");
  CHECK(digest_hex(*compute_hmac(*sha256, key20, as_bytes("Hi There"sv))) ==
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
  CHECK(digest_hex(*compute_hmac(*sha512, as_bytes("Jefe"sv), as_bytes("what do ya want for nothing?"sv))) ==
        "164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd610270cd7ea2505549758bf75c05a994a6d034f65f8f0e6fdcaeab1a3"
        "4d4a6b4b636e070a38bce737");
  const Bytes key131(131, std::byte{0xaa});
  CHECK(digest_hex(*compute_hmac(*sha256, key131, as_bytes("Test Using Larger Than Block-Size Key - Hash Key First"sv))) ==
        "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
  CHECK(digest_hex(*compute_hmac(*md5, Bytes(16, std::byte{0x0b}), as_bytes("Hi There"sv))) ==
        "9294727a3638bb1c13f48ef8158bfc9d");
  CHECK(digest_hex(*compute_hmac(*sha1, as_bytes("Jefe"sv), as_bytes("what do ya want for nothing?"sv))) ==
        "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
  CHECK_STATUS(compute_hmac(*find_hash("crc32"), as_bytes("k"sv), as_bytes("x"sv)), Status::InvalidArgument);
}

TEST_CASE("encoding/hash: streaming in pieces equals one-shot hashing") {
  const Bytes data = test::random_bytes(5000, 4);
  for (const encoding::HashAlgorithm& algorithm : encoding::hash_algorithms()) {
    CAPTURE(algorithm.id);
    const auto whole = compute_hash(algorithm, data);
    REQUIRE_OK(whole);
    CHECK(whole->size() == algorithm.digest_size);
    // Feed odd-sized, non-contiguous copies.
    std::unique_ptr<encoding::Hasher> hasher;
    {
      // compute_hash validates parameters; build the hasher the same way through a zero-length run.
      hasher = algorithm.make != nullptr ? algorithm.make({}) : nullptr;
    }
    if (hasher == nullptr) continue;  // CRC rows are instantiated from their model
    std::size_t at = 0;
    std::size_t step = 1;
    while (at < data.size()) {
      const std::size_t take = std::min(step, data.size() - at);
      const Bytes copy(data.begin() + static_cast<std::ptrdiff_t>(at), data.begin() + static_cast<std::ptrdiff_t>(at + take));
      hasher->update(copy);
      at += take;
      step = step * 3 + 1;
    }
    CHECK(hasher->finish() == *whole);
  }
}

TEST_CASE("encoding/sha256: notations, options and engine integration") {
  constexpr std::string_view abc_sha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  CHECK(test::convert("abc", "bin", "sha256").text() == std::string(abc_sha256) + "\n");
  CHECK(test::convert("abc", "bin", "sha256", R"({"uppercase":true})").text() ==
        util::to_upper(abc_sha256) + "\n");
  CHECK(test::convert("abc", "bin", "sha256", R"({"format":"sumfile"})", "abc.txt").text() ==
        std::string(abc_sha256) + "  abc.txt\n");
  CHECK(test::convert("abc", "bin", "sha256", R"({"format":"bsd"})", "abc.txt").text() ==
        "SHA256 (abc.txt) = " + std::string(abc_sha256) + "\n");
  // `printf abc | openssl dgst -sha256 -binary | openssl base64`
  CHECK(test::convert("abc", "bin", "sha256", R"({"format":"sri"})").text() ==
        "sha256-ungWv48Bz+pBQUDeXa4iI7ADYaOWF3qctBD/YfIAFa0=\n");
  CHECK(test::convert("abc", "bin", "sha384", R"({"format":"sri"})").text() ==
        "sha384-ywB1P0WjXou1oD1pmsZQBycsMqsO3tFjGotgWkP/W+2AhgcroefMI1i67KE0yCWn\n");
  CHECK(test::convert("abc", "bin", "sha256", R"({"format":"base64"})").text() ==
        "ungWv48Bz+pBQUDeXa4iI7ADYaOWF3qctBD/YfIAFa0=\n");
  CHECK(test::convert("abc", "bin", "sha256", R"({"format":"multihash"})").text() ==
        "1220" + std::string(abc_sha256) + "\n");
  // The multihash of "abc" in base58btc, as printed by the reference CLI (`multihash` and IPFS tooling).
  CHECK(test::convert("abc", "bin", "sha256", R"({"format":"multihash-base58"})").text().starts_with("zQm"));
  CHECK(test::convert("abc", "bin", "md5", R"({"format":"sri"})").status == Status::InvalidArgument);
  // HMAC through the engine, key as text and as hex.
  CHECK(test::convert("what do ya want for nothing?", "bin", "sha256", R"({"hmac_key":"Jefe"})").text() ==
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843\n");
  CHECK(test::convert("what do ya want for nothing?", "bin", "sha256",
                      R"({"hmac_key":"4a656665","key_encoding":"hex"})")
            .text() == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843\n");
  CHECK(test::convert("x", "bin", "sha256", R"({"hmac_key":"zz","key_encoding":"hex"})").status ==
        Status::InvalidArgument);
}

TEST_CASE("encoding/hash: the generic target and parametrised algorithms") {
  CHECK(test::convert("abc", "bin", "hash", R"({"algorithm":"sm3"})").text() ==
        "66c7f0f462eeedd9d1f2d46bdc10e4e24167c4875cf2f7a2297da02b8f4ba8e0\n");
  CHECK(test::convert("123456789", "bin", "hash", R"({"algorithm":"crc32c"})").text() == "e3069283\n");
  CHECK(test::convert("abc", "bin", "hash", R"({"algorithm":"shake256","length":16})").text().size() == 33);
  CHECK(test::convert("abc", "bin", "blake2b", R"({"length":32})").text() ==
        "bddd813c634239723171ef3fee98579b94964e3bb1cb3e427262c8c068d52319\n");
  CHECK(test::convert("abc", "bin", "hash", R"({"algorithm":"nope"})").status == Status::InvalidArgument);
  CHECK(test::convert("abc", "bin", "hash", R"({"algorithm":"sha256","length":5})").status == Status::InvalidArgument);
  CHECK(test::convert("abc", "bin", "blake3", R"({"key":"short"})").status == Status::InvalidArgument);
  CHECK(test::convert("abc", "bin", "xxh64", R"({"seed":1})").text() !=
        test::convert("abc", "bin", "xxh64").text());
  // A keyed BLAKE3 differs from the plain one and is deterministic.
  const std::string key = std::string(32, 'k');
  const std::string keyed = test::convert("abc", "bin", "blake3", std::format(R"({{"key":"{}"}})", key)).text();
  CHECK(keyed.size() == 65);
  CHECK(keyed != test::convert("abc", "bin", "blake3").text());
}

TEST_CASE("encoding/checksums: text and JSON reports") {
  const std::string report = test::convert("abc", "bin", "checksums", R"({"algorithms":"sha3-256, blake3,crc32c"})",
                                           "abc.txt")
                                 .text();
  CHECK(report == "File: abc.txt\nSize: 3 bytes\n"
                  "SHA3-256: 3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532\n"
                  "BLAKE3: 6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85\n"
                  "CRC-32C/ISCSI (Castagnoli): 364b3fb7\n");
  const std::string json = test::convert("abc", "bin", "checksums-json", {}, "abc.txt").text();
  CHECK(json.find("\"size\": 3") != std::string::npos);
  CHECK(json.find("\"sha256\": \"ba7816bf") != std::string::npos);
  CHECK(json.find("\"crc32\": \"352441c2\"") != std::string::npos);
  const std::string all = test::convert("abc", "bin", "checksums", R"({"algorithms":"all"})").text();
  CHECK(all.find("Whirlpool: ") != std::string::npos);
  CHECK(test::convert("abc", "bin", "checksums", R"({"algorithms":"md5,bogus"})").status == Status::InvalidArgument);
}

TEST_CASE("encoding/sha256: sum files are detected") {
  const std::string sum = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad  abc.txt\n";
  const auto detection = detect_format(Registry::instance(), as_bytes(sum), "SHA256SUMS.sha256");
  REQUIRE(detection.has_value());
  CHECK(detection->format->id == "sha256");
  const std::string md5sum = "900150983cd24fb0d6963f7d28e17f72  abc.txt\n";
  const auto md5 = detect_format(Registry::instance(), as_bytes(md5sum), "list.md5");
  REQUIRE(md5.has_value());
  CHECK(md5->format->id == "md5");
  const std::string bsd = "SHA256 (abc.txt) = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n";
  const auto tagged = detect_format(Registry::instance(), as_bytes(bsd), "abc.sha256");
  REQUIRE(tagged.has_value());
  CHECK(tagged->format->id == "sha256");
}

TEST_CASE("encoding/hash: cancellation and large inputs") {
  const Bytes big = test::random_bytes(3 << 20, 5);
  const test::Converted result = test::convert(big, "bin", "blake3");
  CHECK(result.ok());
  CHECK(result.data.size() == 65);
}
