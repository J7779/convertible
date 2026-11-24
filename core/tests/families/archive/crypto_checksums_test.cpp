// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// Published test vectors for the family's checksums and cryptography.
#include <string>

#include "support/test_support.hpp"

#include "families/archive/checksums.hpp"
#include "families/archive/crypto.hpp"
#include "util/hex.hpp"

using namespace cvt;
using namespace std::string_view_literals;

namespace {

Bytes unhex(std::string_view text) {
  Bytes out;
  for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
    out.push_back(static_cast<std::byte>(std::stoi(std::string(text.substr(i, 2)), nullptr, 16)));
  }
  return out;
}

template <class Digest>
std::string hex_of(const Digest& digest) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  for (const auto b : digest) {
    const auto v = static_cast<unsigned>(b);
    out.push_back(digits[v >> 4]);
    out.push_back(digits[v & 15]);
  }
  return out;
}

}  // namespace

TEST_CASE("archive/checksums: CRC check values of the catalogue") {
  const ByteView check = as_bytes("123456789");
  CHECK_EQ(archive::crc32c(check), 0xE3069283u);
  CHECK_EQ(archive::crc64_xz(check), 0x995DC9BBDF1939FAull);
  CHECK_EQ(archive::crc32_msb(check), 0xFC891918u);
  CHECK_EQ(archive::crc16_arc(check), 0xBB3Du);
  // Running values continue across pieces.
  CHECK_EQ(archive::crc32c(as_bytes("6789"), archive::crc32c(as_bytes("12345"))), 0xE3069283u);
  CHECK_EQ(archive::crc64_xz(as_bytes("6789"), archive::crc64_xz(as_bytes("12345"))), 0x995DC9BBDF1939FAull);
  CHECK_EQ(archive::crc32_msb(as_bytes("6789"), archive::crc32_msb(as_bytes("12345"))), 0xFC891918u);
  CHECK_EQ(archive::crc32c({}), 0u);
}

TEST_CASE("archive/checksums: xxHash reference values") {
  CHECK_EQ(archive::xxh32({}), 0x02CC5D05u);
  CHECK_EQ(archive::xxh64({}), 0xEF46DB3751D8E999ull);
  CHECK_EQ(archive::xxh32(as_bytes("abc")), 0x32D153FFu);
  CHECK_EQ(archive::xxh64(as_bytes("abc")), 0x44BC2CF5AD770999ull);
  // Incremental hashing equals one-shot hashing for every split point.
  const Bytes data = test::random_bytes(200, 7);
  const std::uint32_t whole32 = archive::xxh32(data, 5);
  const std::uint64_t whole64 = archive::xxh64(data, 5);
  for (std::size_t split = 0; split <= data.size(); split += 13) {
    archive::Xxh32 a(5);
    archive::Xxh64 b(5);
    a.update(ByteView(data).first(split));
    b.update(ByteView(data).first(split));
    a.update(ByteView(data).subspan(split));
    b.update(ByteView(data).subspan(split));
    CHECK_EQ(a.digest(), whole32);
    CHECK_EQ(b.digest(), whole64);
  }
}

TEST_CASE("archive/crypto: AES known answers (FIPS-197 appendix C)") {
  const Bytes plain = unhex("00112233445566778899aabbccddeeff");
  struct Vector {
    std::string_view key;
    std::string_view cipher;
  };
  for (const Vector& v : {Vector{"000102030405060708090a0b0c0d0e0f", "69c4e0d86a7b0430d8cdb78070b4c55a"},
                          Vector{"000102030405060708090a0b0c0d0e0f1011121314151617", "dda97ca4864cdfe06eaf70a0ec0d7191"},
                          Vector{"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
                                 "8ea2b7ca516745bfeafc49904b496089"}}) {
    INFO("key ", v.key);
    archive::Aes aes;
    REQUIRE(aes.set_key(unhex(v.key)));
    std::array<std::uint8_t, 16> out{};
    aes.encrypt_block(reinterpret_cast<const std::uint8_t*>(plain.data()), out.data());
    CHECK(hex_of(out) == v.cipher);
    std::array<std::uint8_t, 16> back{};
    aes.decrypt_block(out.data(), back.data());
    CHECK(hex_of(back) == "00112233445566778899aabbccddeeff");
  }
  archive::Aes aes;
  CHECK_FALSE(aes.set_key(unhex("0001020304")));
}

TEST_CASE("archive/crypto: AES-128-CBC (NIST SP 800-38A F.2.1)") {
  archive::Aes aes;
  REQUIRE(aes.set_key(unhex("2b7e151628aed2a6abf7158809cf4f3c")));
  Bytes data = unhex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51");
  archive::Aes::Block iv{};
  for (std::size_t i = 0; i < 16; ++i) iv[i] = static_cast<std::uint8_t>(i);
  REQUIRE_OK(archive::aes_cbc_encrypt(aes, iv, std::span<std::byte>(data)));
  CHECK(hex_of(data) == "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2");
  for (std::size_t i = 0; i < 16; ++i) iv[i] = static_cast<std::uint8_t>(i);
  REQUIRE_OK(archive::aes_cbc_decrypt(aes, iv, std::span<std::byte>(data)));
  CHECK(hex_of(data) == "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51");
  Bytes odd(15);
  CHECK_STATUS(archive::aes_cbc_decrypt(aes, iv, std::span<std::byte>(odd)), Status::InvalidArgument);
}

TEST_CASE("archive/crypto: HMAC (RFC 2202, RFC 4231)") {
  const Bytes key1(20, std::byte{0x0b});
  CHECK(hex_of(archive::hmac_sha1(key1, as_bytes("Hi There"))) == "b617318655057264e28bc0b6fb378c8ef146be00");
  CHECK(hex_of(archive::hmac_sha1(as_bytes("Jefe"), as_bytes("what do ya want for nothing?"))) ==
        "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
  CHECK(hex_of(archive::hmac_sha256(key1, as_bytes("Hi There"))) ==
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
  CHECK(hex_of(archive::hmac_sha256(as_bytes("Jefe"), as_bytes("what do ya want for nothing?"))) ==
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  // RFC 4231 test 6: a key longer than the block size is hashed first.
  const Bytes long_key(131, std::byte{0xaa});
  CHECK(hex_of(archive::hmac_sha256(long_key, as_bytes("Test Using Larger Than Block-Size Key - Hash Key First"))) ==
        "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

TEST_CASE("archive/crypto: PBKDF2 (RFC 6070 and SHA-256 vectors)") {
  const ByteView password = as_bytes("password");
  const ByteView salt = as_bytes("salt");
  CHECK(hex_of(archive::pbkdf2_hmac_sha1(password, salt, 1, 20)) == "0c60c80f961f0e71f3a9b524af6012062fe037a6");
  CHECK(hex_of(archive::pbkdf2_hmac_sha1(password, salt, 2, 20)) == "ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957");
  CHECK(hex_of(archive::pbkdf2_hmac_sha1(password, salt, 4096, 20)) == "4b007901b765489abead49d926f721d065a429c1");
  CHECK(hex_of(archive::pbkdf2_hmac_sha1(as_bytes("passwordPASSWORDpassword"),
                                         as_bytes("saltSALTsaltSALTsaltSALTsaltSALTsalt"), 4096, 25)) ==
        "3d2eec4fe41c849b80c8d83662c0e44a8b291a964cf2f07038");
  CHECK(hex_of(archive::pbkdf2_hmac_sha256(password, salt, 1, 32)) ==
        "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
  CHECK(hex_of(archive::pbkdf2_hmac_sha256(password, salt, 2, 32)) ==
        "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");
  CHECK(hex_of(archive::pbkdf2_hmac_sha256(password, salt, 4096, 32)) ==
        "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
}
