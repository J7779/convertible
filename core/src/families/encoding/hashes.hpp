// Convertible core - encoding family: hash functions and checksums.
//
// Every algorithm is written here from its specification and exposed through
// one streaming interface (`Hasher`) and one catalogue (`hash_algorithms()`),
// which drives everything else: the per-algorithm targets (`sha256`, `md5`,
// `crc32`, ...), the generic `hash` target, the `checksums` reports and HMAC.
//
// Adding an algorithm: implement a `Hasher`, add a factory declaration below,
// and add one row to the table in hashes.cpp.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "engine/errors.hpp"
#include "util/bytes.hpp"

namespace cvt {
class Registry;
}  // namespace cvt

namespace cvt::encoding {

/// Incremental hashing: any number of `update` calls, then one `finish`.
class Hasher {
 public:
  Hasher() = default;
  Hasher(const Hasher&) = delete;
  Hasher& operator=(const Hasher&) = delete;
  virtual ~Hasher() = default;
  virtual void update(ByteView data) = 0;
  /// The digest. Integer checksums (CRC, Adler, FNV, xxHash ...) are returned
  /// big-endian, i.e. in the order their hexadecimal form is usually written.
  [[nodiscard]] virtual Bytes finish() = 0;
  /// True for algorithms that are computed over the whole input at once
  /// (XXH3, MurmurHash3, SipHash): `update` then has to copy, so callers that
  /// hold all the data should use `digest_of` instead.
  [[nodiscard]] virtual bool whole_input() const noexcept { return false; }
  /// update(data) + finish(), without a copy for whole-input algorithms.
  [[nodiscard]] virtual Bytes digest_of(ByteView data) {
    update(data);
    return finish();
  }
};

/// Parameters some algorithms accept; ignored by the others.
struct HashParams {
  std::size_t length = 0;  ///< output bytes for XOFs and BLAKE2/3; 0 = the algorithm's default
  Bytes key;               ///< BLAKE2/BLAKE3 key, SipHash key (16 bytes; empty = 00 01 .. 0F)
  std::uint64_t seed = 0;  ///< xxHash and MurmurHash3 seed
};

enum class HashKind {
  Cryptographic,  ///< collision-resistant digest; usable with HMAC
  Legacy,         ///< broken cryptographic digest (MD2, MD4, MD5, SHA-1); still usable with HMAC
  Keyed,          ///< keyed PRF such as SipHash
  Fast,           ///< non-cryptographic hash (xxHash, MurmurHash3, FNV)
  Checksum,       ///< error-detecting code (CRC, Adler, Fletcher)
};

struct HashAlgorithm {
  std::string_view id;          ///< canonical lower-case id, e.g. "sha512-256"
  std::string_view name;        ///< display name, e.g. "SHA-512/256"
  HashKind kind;
  std::size_t digest_size;      ///< default output size in bytes
  std::size_t block_size;       ///< HMAC block size in bytes; 0 = HMAC not offered
  bool variable_length;         ///< accepts HashParams::length
  std::size_t max_length;       ///< largest HashParams::length (when variable)
  bool keyed;                   ///< accepts HashParams::key
  bool seeded;                  ///< accepts HashParams::seed
  std::int32_t multihash_code;  ///< multicodec code, or -1 when multihash has none
  std::string_view sri_name;    ///< Subresource Integrity prefix ("sha256"), or empty
  std::string_view bsd_tag;     ///< tag of BSD-style sum lines ("SHA256"), or empty
  std::unique_ptr<Hasher> (*make)(const HashParams& params);
};

/// Every algorithm, in display order.
[[nodiscard]] std::span<const HashAlgorithm> hash_algorithms() noexcept;
/// By id (ASCII case-insensitive; '_' and '-' are interchangeable).
[[nodiscard]] const HashAlgorithm* find_hash(std::string_view id) noexcept;

/// One-shot digest. Fails with InvalidArgument for unsupported parameters.
[[nodiscard]] Result<Bytes> compute_hash(const HashAlgorithm& algorithm, ByteView data, const HashParams& params = {});
/// RFC 2104 HMAC over a cryptographic algorithm (block_size != 0).
[[nodiscard]] Result<Bytes> compute_hmac(const HashAlgorithm& algorithm, ByteView key, ByteView data);

// -- output formats ---------------------------------------------------------------

[[nodiscard]] std::string digest_hex(ByteView digest, bool uppercase = false);
/// "sha384-<base64>" (W3C Subresource Integrity); empty when the algorithm has no SRI name.
[[nodiscard]] std::string digest_sri(const HashAlgorithm& algorithm, ByteView digest);
/// Multihash: varint(code) varint(length) digest. Fails when the algorithm has no code.
[[nodiscard]] Result<Bytes> digest_multihash(const HashAlgorithm& algorithm, ByteView digest);

// -- the individual algorithms (hash_*.cpp) ------------------------------------------

std::unique_ptr<Hasher> make_md2(const HashParams&);
std::unique_ptr<Hasher> make_md4(const HashParams&);
std::unique_ptr<Hasher> make_md5(const HashParams&);
std::unique_ptr<Hasher> make_sha1(const HashParams&);
std::unique_ptr<Hasher> make_sha224(const HashParams&);
std::unique_ptr<Hasher> make_sha256(const HashParams&);
std::unique_ptr<Hasher> make_sha384(const HashParams&);
std::unique_ptr<Hasher> make_sha512(const HashParams&);
std::unique_ptr<Hasher> make_sha512_224(const HashParams&);
std::unique_ptr<Hasher> make_sha512_256(const HashParams&);
/// SHA-512/t for any t that is a multiple of 8 below 512 (FIPS 180-4 5.3.6), t = 8 * bytes.
std::unique_ptr<Hasher> make_sha512_t(std::size_t bytes);
std::unique_ptr<Hasher> make_sha3_224(const HashParams&);
std::unique_ptr<Hasher> make_sha3_256(const HashParams&);
std::unique_ptr<Hasher> make_sha3_384(const HashParams&);
std::unique_ptr<Hasher> make_sha3_512(const HashParams&);
std::unique_ptr<Hasher> make_shake128(const HashParams&);
std::unique_ptr<Hasher> make_shake256(const HashParams&);
std::unique_ptr<Hasher> make_keccak224(const HashParams&);
std::unique_ptr<Hasher> make_keccak256(const HashParams&);
std::unique_ptr<Hasher> make_keccak384(const HashParams&);
std::unique_ptr<Hasher> make_keccak512(const HashParams&);
std::unique_ptr<Hasher> make_blake2b(const HashParams&);
std::unique_ptr<Hasher> make_blake2s(const HashParams&);
std::unique_ptr<Hasher> make_blake3(const HashParams&);
std::unique_ptr<Hasher> make_ripemd160(const HashParams&);
std::unique_ptr<Hasher> make_whirlpool(const HashParams&);
std::unique_ptr<Hasher> make_sm3(const HashParams&);
std::unique_ptr<Hasher> make_xxh32(const HashParams&);
std::unique_ptr<Hasher> make_xxh64(const HashParams&);
std::unique_ptr<Hasher> make_xxh3_64(const HashParams&);
std::unique_ptr<Hasher> make_murmur3_32(const HashParams&);
std::unique_ptr<Hasher> make_murmur3_128(const HashParams&);
std::unique_ptr<Hasher> make_fnv1_32(const HashParams&);
std::unique_ptr<Hasher> make_fnv1a_32(const HashParams&);
std::unique_ptr<Hasher> make_fnv1_64(const HashParams&);
std::unique_ptr<Hasher> make_fnv1a_64(const HashParams&);
std::unique_ptr<Hasher> make_siphash24(const HashParams&);
std::unique_ptr<Hasher> make_adler32(const HashParams&);
std::unique_ptr<Hasher> make_fletcher16(const HashParams&);
std::unique_ptr<Hasher> make_fletcher32(const HashParams&);
std::unique_ptr<Hasher> make_fletcher64(const HashParams&);

/// A CRC from the Rocksoft parameter model (width 1..64).
struct CrcModel {
  std::string_view id;
  std::string_view name;
  unsigned width;
  std::uint64_t poly;
  std::uint64_t init;
  bool refin;
  bool refout;
  std::uint64_t xorout;
  std::uint64_t check;  ///< CRC of the ASCII string "123456789" (catalogue check value)
};
[[nodiscard]] std::span<const CrcModel> crc_models() noexcept;
[[nodiscard]] std::unique_ptr<Hasher> make_crc(const CrcModel& model);
/// Bit-at-a-time reference computation, independent of the table-driven one.
[[nodiscard]] std::uint64_t crc_reference(const CrcModel& model, ByteView data) noexcept;

}  // namespace cvt::encoding
