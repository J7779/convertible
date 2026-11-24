// Convertible core - encoding family: hashes, checksums and digest reports.
//
// Targets (all are Blob writers, so every file can be hashed):
//   md5, sha1, sha224, sha256, sha384, sha512, sha3-256, sha3-512, blake2b,
//   blake2s, blake3, crc32, xxh64, xxh3   one digest, in a choice of notations
//   hash                                  any algorithm of the catalogue below
//   checksums                             a "Name: value" text report
//   checksums-json                        the same as a JSON object
// Digest notations: plain hex, a sha256sum-style line, a BSD-style tagged
// line, Subresource Integrity, Base64 and multihash (hex or base58btc).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/encoding/hashes.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/encoding/detail.hpp"
#include "families/encoding/encoding.hpp"
#include "families/encoding/simple.hpp"
#include "util/base64.hpp"
#include "util/hex.hpp"
#include "util/json.hpp"
#include "util/text.hpp"

namespace cvt::encoding {

namespace {

using K = HashKind;

// One row per algorithm. CRC models are appended from crc_models().
constexpr std::size_t no_hmac = 0;
constexpr std::int32_t no_multihash = -1;

struct Row {
  std::string_view id, name;
  HashKind kind;
  std::size_t digest, block;
  bool variable;
  std::size_t max_length;
  bool keyed, seeded;
  std::int32_t multihash;
  std::string_view sri, bsd;
  std::unique_ptr<Hasher> (*make)(const HashParams&);
};

constexpr Row rows[] = {
    {"md2", "MD2", K::Legacy, 16, 16, false, 0, false, false, no_multihash, "", "MD2", make_md2},
    {"md4", "MD4", K::Legacy, 16, 64, false, 0, false, false, 0xd4, "", "MD4", make_md4},
    {"md5", "MD5", K::Legacy, 16, 64, false, 0, false, false, 0xd5, "", "MD5", make_md5},
    {"sha1", "SHA-1", K::Legacy, 20, 64, false, 0, false, false, 0x11, "", "SHA1", make_sha1},
    {"sha224", "SHA-224", K::Cryptographic, 28, 64, false, 0, false, false, 0x1013, "", "SHA224", make_sha224},
    {"sha256", "SHA-256", K::Cryptographic, 32, 64, false, 0, false, false, 0x12, "sha256", "SHA256", make_sha256},
    {"sha384", "SHA-384", K::Cryptographic, 48, 128, false, 0, false, false, 0x20, "sha384", "SHA384", make_sha384},
    {"sha512", "SHA-512", K::Cryptographic, 64, 128, false, 0, false, false, 0x13, "sha512", "SHA512", make_sha512},
    {"sha512-224", "SHA-512/224", K::Cryptographic, 28, 128, false, 0, false, false, 0x1014, "", "SHA512-224",
     make_sha512_224},
    {"sha512-256", "SHA-512/256", K::Cryptographic, 32, 128, false, 0, false, false, 0x1015, "", "SHA512-256",
     make_sha512_256},
    {"sha3-224", "SHA3-224", K::Cryptographic, 28, 144, false, 0, false, false, 0x17, "", "SHA3-224", make_sha3_224},
    {"sha3-256", "SHA3-256", K::Cryptographic, 32, 136, false, 0, false, false, 0x16, "", "SHA3-256", make_sha3_256},
    {"sha3-384", "SHA3-384", K::Cryptographic, 48, 104, false, 0, false, false, 0x15, "", "SHA3-384", make_sha3_384},
    {"sha3-512", "SHA3-512", K::Cryptographic, 64, 72, false, 0, false, false, 0x14, "", "SHA3-512", make_sha3_512},
    {"shake128", "SHAKE128", K::Cryptographic, 32, no_hmac, true, 4096, false, false, 0x18, "", "SHAKE128",
     make_shake128},
    {"shake256", "SHAKE256", K::Cryptographic, 64, no_hmac, true, 4096, false, false, 0x19, "", "SHAKE256",
     make_shake256},
    {"keccak224", "Keccak-224", K::Cryptographic, 28, 144, false, 0, false, false, 0x1a, "", "", make_keccak224},
    {"keccak256", "Keccak-256", K::Cryptographic, 32, 136, false, 0, false, false, 0x1b, "", "", make_keccak256},
    {"keccak384", "Keccak-384", K::Cryptographic, 48, 104, false, 0, false, false, 0x1c, "", "", make_keccak384},
    {"keccak512", "Keccak-512", K::Cryptographic, 64, 72, false, 0, false, false, 0x1d, "", "", make_keccak512},
    {"blake2b", "BLAKE2b", K::Cryptographic, 64, 128, true, 64, true, false, 0xb200, "", "BLAKE2b", make_blake2b},
    {"blake2s", "BLAKE2s", K::Cryptographic, 32, 64, true, 32, true, false, 0xb240, "", "BLAKE2s", make_blake2s},
    {"blake3", "BLAKE3", K::Cryptographic, 32, no_hmac, true, 4096, true, false, 0x1e, "", "BLAKE3", make_blake3},
    {"ripemd160", "RIPEMD-160", K::Cryptographic, 20, 64, false, 0, false, false, 0x1053, "", "RMD160",
     make_ripemd160},
    {"whirlpool", "Whirlpool", K::Cryptographic, 64, 64, false, 0, false, false, no_multihash, "", "WHIRLPOOL",
     make_whirlpool},
    {"sm3", "SM3", K::Cryptographic, 32, 64, false, 0, false, false, 0x534d, "", "SM3", make_sm3},
    {"siphash", "SipHash-2-4", K::Keyed, 8, no_hmac, false, 0, true, false, no_multihash, "", "", make_siphash24},
    {"xxh32", "xxHash32", K::Fast, 4, no_hmac, false, 0, false, true, 0xb3e1, "", "", make_xxh32},
    {"xxh64", "xxHash64", K::Fast, 8, no_hmac, false, 0, false, true, 0xb3e2, "", "", make_xxh64},
    {"xxh3", "XXH3-64", K::Fast, 8, no_hmac, false, 0, false, true, 0xb3e3, "", "", make_xxh3_64},
    {"murmur3", "MurmurHash3 x86_32", K::Fast, 4, no_hmac, false, 0, false, true, 0x23, "", "", make_murmur3_32},
    {"murmur3-128", "MurmurHash3 x64_128", K::Fast, 16, no_hmac, false, 0, false, true, no_multihash, "", "",
     make_murmur3_128},
    {"fnv1-32", "FNV-1 32-bit", K::Fast, 4, no_hmac, false, 0, false, false, no_multihash, "", "", make_fnv1_32},
    {"fnv1a-32", "FNV-1a 32-bit", K::Fast, 4, no_hmac, false, 0, false, false, no_multihash, "", "", make_fnv1a_32},
    {"fnv1-64", "FNV-1 64-bit", K::Fast, 8, no_hmac, false, 0, false, false, no_multihash, "", "", make_fnv1_64},
    {"fnv1a-64", "FNV-1a 64-bit", K::Fast, 8, no_hmac, false, 0, false, false, no_multihash, "", "", make_fnv1a_64},
    {"adler32", "Adler-32", K::Checksum, 4, no_hmac, false, 0, false, false, no_multihash, "", "", make_adler32},
    {"fletcher16", "Fletcher-16", K::Checksum, 2, no_hmac, false, 0, false, false, no_multihash, "", "",
     make_fletcher16},
    {"fletcher32", "Fletcher-32", K::Checksum, 4, no_hmac, false, 0, false, false, no_multihash, "", "",
     make_fletcher32},
    {"fletcher64", "Fletcher-64", K::Checksum, 8, no_hmac, false, 0, false, false, no_multihash, "", "",
     make_fletcher64},
};

// CRC entries need their model; the factory finds it again by id.
std::unique_ptr<Hasher> make_crc_placeholder(const HashParams&) { return nullptr; }

struct Catalogue {
  std::vector<HashAlgorithm> algorithms;
  std::vector<const CrcModel*> crc_of;  // parallel to algorithms; null for non-CRC rows
};

const Catalogue& catalogue() {
  static const Catalogue instance = [] {
    Catalogue c;
    for (const Row& r : rows) {
      c.algorithms.push_back({r.id, r.name, r.kind, r.digest, r.block, r.variable, r.max_length, r.keyed, r.seeded,
                              r.multihash, r.sri, r.bsd, r.make});
      c.crc_of.push_back(nullptr);
    }
    for (const CrcModel& model : crc_models()) {
      c.algorithms.push_back({model.id, model.name, K::Checksum, (model.width + 7) / 8, no_hmac, false, 0, false, false,
                              no_multihash, "", "", make_crc_placeholder});
      c.crc_of.push_back(&model);
    }
    return c;
  }();
  return instance;
}

std::string normalized_id(std::string_view id) {
  std::string out = util::to_lower(util::trim(id));
  std::ranges::replace(out, '_', '-');
  return out;
}

std::unique_ptr<Hasher> instantiate(const HashAlgorithm& algorithm, const HashParams& params) {
  const Catalogue& c = catalogue();
  const auto at = static_cast<std::size_t>(&algorithm - c.algorithms.data());
  if (at < c.crc_of.size() && c.crc_of[at] != nullptr) return make_crc(*c.crc_of[at]);
  return algorithm.make(params);
}

Result<void> check_params(const HashAlgorithm& algorithm, const HashParams& params) {
  if (params.length != 0) {
    if (!algorithm.variable_length) return fail(Status::InvalidArgument, "{} has a fixed output length", algorithm.name);
    if (params.length > algorithm.max_length) {
      return fail(Status::InvalidArgument, "{} output is limited to {} bytes", algorithm.name, algorithm.max_length);
    }
  }
  if (!params.key.empty()) {
    if (!algorithm.keyed) return fail(Status::InvalidArgument, "{} does not take a key", algorithm.name);
    if (algorithm.id == "blake3" && params.key.size() != 32) {
      return fail(Status::InvalidArgument, "a BLAKE3 key must be exactly 32 bytes");
    }
    if (algorithm.id == "siphash" && params.key.size() != 16) {
      return fail(Status::InvalidArgument, "a SipHash key must be exactly 16 bytes");
    }
    if (algorithm.id == "blake2b" && params.key.size() > 64) {
      return fail(Status::InvalidArgument, "a BLAKE2b key is at most 64 bytes");
    }
    if (algorithm.id == "blake2s" && params.key.size() > 32) {
      return fail(Status::InvalidArgument, "a BLAKE2s key is at most 32 bytes");
    }
  }
  return {};
}

// Hash in slices so that progress is reported and cancellation honoured.
Result<Bytes> hash_with_progress(const HashAlgorithm& algorithm, ByteView data, const HashParams& params,
                                 const Context& ctx) {
  CVT_TRY(check_params(algorithm, params));
  std::unique_ptr<Hasher> hasher = instantiate(algorithm, params);
  if (hasher->whole_input()) {
    CVT_TRY(ctx.checkpoint(0.0, "Hashing"));
    return hasher->digest_of(data);
  }
  constexpr std::size_t slice = std::size_t{1} << 20;
  for (std::size_t at = 0; at < data.size(); at += slice) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(at) / static_cast<double>(data.size()), "Hashing"));
    hasher->update(data.subspan(at, std::min(slice, data.size() - at)));
  }
  return hasher->finish();
}

Result<Bytes> hmac_with_progress(const HashAlgorithm& algorithm, ByteView key, ByteView data, const Context& ctx) {
  if (algorithm.block_size == 0) return fail(Status::InvalidArgument, "HMAC is not defined for {}", algorithm.name);
  Bytes k(key.begin(), key.end());
  if (k.size() > algorithm.block_size) {
    CVT_TRY_ASSIGN(k, hash_with_progress(algorithm, key, {}, ctx));
  }
  k.resize(algorithm.block_size, std::byte{0});
  Bytes inner_pad(k), outer_pad(k);
  for (std::byte& b : inner_pad) b ^= std::byte{0x36};
  for (std::byte& b : outer_pad) b ^= std::byte{0x5c};
  std::unique_ptr<Hasher> inner = instantiate(algorithm, {});
  inner->update(inner_pad);
  constexpr std::size_t slice = std::size_t{1} << 20;
  for (std::size_t at = 0; at < data.size(); at += slice) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(at) / static_cast<double>(data.size()), "Hashing"));
    inner->update(data.subspan(at, std::min(slice, data.size() - at)));
  }
  const Bytes inner_digest = inner->finish();
  std::unique_ptr<Hasher> outer = instantiate(algorithm, {});
  outer->update(outer_pad);
  outer->update(inner_digest);
  return outer->finish();
}

void append_varint(Bytes& out, std::uint64_t value) {
  while (value >= 0x80) {
    out.push_back(static_cast<std::byte>((value & 0x7F) | 0x80));
    value >>= 7;
  }
  out.push_back(static_cast<std::byte>(value));
}

// ---------------------------------------------------------------------------- options

const std::vector<std::string>& notation_choices() {
  static const std::vector<std::string> choices = {"hex",    "sumfile",   "bsd",
                                                    "sri",    "base64",    "multihash",
                                                    "multihash-base58"};
  return choices;
}

Result<Bytes> key_bytes(const Options& options, std::string_view key, std::string_view encoding_key) {
  const std::string value = options.get_string(key);
  if (value.empty()) return Bytes{};
  if (options.get_enum(encoding_key, "utf-8", {"utf-8", "hex"}) == "hex") {
    auto decoded = parse_hex_text(value);
    if (!decoded) return fail(Status::InvalidArgument, "option '{}' is not valid hexadecimal", key);
    return std::move(*decoded);
  }
  return to_bytes(value);
}

std::vector<OptionSpec> options_for(const HashAlgorithm* algorithm, bool generic) {
  std::vector<OptionSpec> specs;
  if (generic) {
    std::vector<std::string> ids;
    for (const HashAlgorithm& a : hash_algorithms()) ids.emplace_back(a.id);
    specs.push_back(OptionSpec::choice("algorithm", "Algorithm", "sha256", std::move(ids), "Hash or checksum to compute."));
  }
  specs.push_back(OptionSpec::choice(
      "format", "Notation", "hex", notation_choices(),
      "hex digits; a sha256sum-style line; a BSD-style tagged line; Subresource Integrity (sha256/384/512); Base64; "
      "multihash in hex or base58btc."));
  specs.push_back(OptionSpec::boolean("uppercase", "Uppercase digits", false, "Write hexadecimal digits A-F in upper case."));
  const bool any = generic || algorithm == nullptr;
  if (any || algorithm->block_size != 0) {
    specs.push_back(OptionSpec::text("hmac_key", "HMAC key", "",
                                     "When set, compute the RFC 2104 HMAC with this key instead of the plain digest."));
  }
  if (any || algorithm->keyed) {
    specs.push_back(OptionSpec::text("key", "Key", "",
                                     "Key of keyed algorithms (BLAKE2 up to 64/32 bytes, BLAKE3 32 bytes, SipHash 16 bytes); "
                                     "empty = unkeyed (SipHash: the reference key 00 01 .. 0F)."));
  }
  if (any || algorithm->block_size != 0 || algorithm->keyed) {
    specs.push_back(OptionSpec::choice("key_encoding", "Key encoding", "utf-8", {"utf-8", "hex"},
                                       "How the key options are written: UTF-8 text or hexadecimal digits.")
                        .advanced());
  }
  if (any || algorithm->variable_length) {
    specs.push_back(OptionSpec::integer("length", "Output length", 0, 0, 4096,
                                        "Digest length in bytes for SHAKE, BLAKE2 and BLAKE3; 0 = the default."));
  }
  if (any || algorithm->seeded) {
    specs.push_back(OptionSpec::integer("seed", "Seed", 0, 0, 0xFFFFFFFFLL, "Seed of xxHash and MurmurHash3.").advanced());
  }
  return specs;
}

std::string file_label(const ir::Blob& blob, const WriteContext& ctx) {
  std::string name = blob.filename.empty() ? ctx.source_filename : blob.filename;
  name = util::sanitize_utf8(name);
  std::ranges::replace(name, '\n', ' ');
  std::ranges::replace(name, '\r', ' ');
  return name.empty() ? "-" : name;
}

Result<std::string> encode_digest(const HashAlgorithm& algorithm, const ir::Blob& blob, const WriteContext& ctx) {
  HashParams params;
  params.length = static_cast<std::size_t>(ctx.options.get_int("length", 0, 0, 4096));
  params.seed = static_cast<std::uint64_t>(ctx.options.get_int("seed", 0, 0, 0xFFFFFFFFLL));
  CVT_TRY_ASSIGN(params.key, key_bytes(ctx.options, "key", "key_encoding"));
  CVT_TRY_ASSIGN(const Bytes hmac_key, key_bytes(ctx.options, "hmac_key", "key_encoding"));
  const bool hmac = !ctx.options.get_string("hmac_key").empty();
  Bytes digest;
  if (hmac) {
    CVT_TRY_ASSIGN(digest, hmac_with_progress(algorithm, hmac_key, blob.bytes, ctx));
  } else {
    CVT_TRY_ASSIGN(digest, hash_with_progress(algorithm, blob.bytes, params, ctx));
  }
  const bool upper = ctx.options.get_bool("uppercase", false);
  const std::string notation = ctx.options.get_enum("format", "hex", notation_choices());
  const std::string hex = digest_hex(digest, upper);
  if (notation == "sumfile") return std::format("{}  {}\n", hex, file_label(blob, ctx));
  if (notation == "bsd") {
    std::string tag(algorithm.bsd_tag.empty() ? util::to_upper(algorithm.id) : std::string(algorithm.bsd_tag));
    if (hmac) tag = "HMAC-" + tag;
    return std::format("{} ({}) = {}\n", tag, file_label(blob, ctx), hex);
  }
  if (notation == "sri") {
    if (algorithm.sri_name.empty() || hmac || params.length != 0) {
      return fail(Status::InvalidArgument, "Subresource Integrity is only defined for plain SHA-256, SHA-384 and SHA-512");
    }
    return digest_sri(algorithm, digest) + "\n";
  }
  if (notation == "base64") return util::base64_encode(digest) + "\n";
  if (notation == "multihash" || notation == "multihash-base58") {
    if (hmac) return fail(Status::InvalidArgument, "multihash has no code for HMAC values");
    CVT_TRY_ASSIGN(const Bytes multihash, digest_multihash(algorithm, digest));
    if (notation == "multihash") return digest_hex(multihash, upper) + "\n";
    CVT_TRY_ASSIGN(const std::string base58, base58_encode(multihash));
    return "z" + base58 + "\n";  // multibase prefix for base58btc
  }
  return hex + "\n";
}

// ---------------------------------------------------------------------------- reports

constexpr std::string_view default_report = "crc32,adler32,md5,sha1,sha256";

Result<std::vector<const HashAlgorithm*>> report_algorithms(const Options& options) {
  std::vector<const HashAlgorithm*> out;
  std::string list = options.get_string("algorithms", default_report);
  if (util::trim(list).empty()) list = std::string(default_report);
  if (util::iequals(util::trim(list), "all")) {
    for (const HashAlgorithm& a : hash_algorithms()) out.push_back(&a);
    return out;
  }
  for (const std::string_view part : util::split(list, ',', false)) {
    const HashAlgorithm* algorithm = find_hash(part);
    if (algorithm == nullptr) return fail(Status::InvalidArgument, "unknown hash algorithm '{}'", util::trim(part));
    out.push_back(algorithm);
  }
  return out;
}

Result<std::vector<std::pair<const HashAlgorithm*, std::string>>> report_values(const ir::Blob& blob,
                                                                                const WriteContext& ctx) {
  CVT_TRY_ASSIGN(const std::vector<const HashAlgorithm*> algorithms, report_algorithms(ctx.options));
  const bool upper = ctx.options.get_bool("uppercase", false);
  std::vector<std::pair<const HashAlgorithm*, std::string>> values;
  for (std::size_t i = 0; i < algorithms.size(); ++i) {
    const HashAlgorithm* algorithm = algorithms[i];
    // Each algorithm gets its own share of the progress range: progress never
    // runs backwards, so restarting at 0 would hide every later report (and
    // with it the user's cancellation) until the last algorithm.
    Context part = ctx;
    const double n = static_cast<double>(algorithms.size());
    part.progress = ctx.progress.sub(static_cast<double>(i) / n, static_cast<double>(i + 1) / n);
    CVT_TRY_ASSIGN(const Bytes digest, hash_with_progress(*algorithm, blob.bytes, {}, part));
    values.emplace_back(algorithm, digest_hex(digest, upper));
  }
  return values;
}

int sniff_checksums(ByteView data, std::string_view /*filename*/) {
  const std::string_view text = as_chars(data).substr(0, 1024);
  return text.find("Size: ") != std::string_view::npos && text.find(" bytes\n") != std::string_view::npos &&
                 (text.find("\nCRC-32: ") != std::string_view::npos || text.find("\nSHA-256: ") != std::string_view::npos ||
                  text.find("\nMD5: ") != std::string_view::npos)
             ? 85
             : 0;
}

// A file of "<hex>  <name>" lines whose digests have `hex_digits` digits.
int sniff_sumfile(ByteView data, std::string_view filename, std::string_view extension, std::size_t hex_digits) {
  const std::string_view text = as_chars(data).substr(0, 4096);
  const std::size_t end = text.find('\n');
  const std::string_view line = text.substr(0, end);
  const bool extension_match = util::iends_with(filename, std::string(".") + std::string(extension));
  // BSD style: "TAG (name) = hex"
  const std::size_t equals = line.rfind(" = ");
  std::string_view digits = line.substr(0, std::min(line.size(), hex_digits));
  if (equals != std::string_view::npos && line.find(" (") != std::string_view::npos) {
    digits = line.substr(equals + 3);
    if (digits.size() != hex_digits) return 0;
  } else if (line.size() < hex_digits + 2 || (line[hex_digits] != ' ' && line[hex_digits] != '\t')) {
    return 0;
  }
  for (const char c : digits) {
    if (util::hex_digit_value(c) < 0) return 0;
  }
  return extension_match ? 92 : 40;
}

}  // namespace

std::span<const HashAlgorithm> hash_algorithms() noexcept { return catalogue().algorithms; }

const HashAlgorithm* find_hash(std::string_view id) noexcept {
  const std::string wanted = normalized_id(id);
  for (const HashAlgorithm& algorithm : hash_algorithms()) {
    if (algorithm.id == wanted) return &algorithm;
  }
  // A few common spellings.
  if (wanted == "sha-1") return find_hash("sha1");
  if (wanted == "sha-256" || wanted == "sha2-256") return find_hash("sha256");
  if (wanted == "sha-512" || wanted == "sha2-512") return find_hash("sha512");
  if (wanted == "sha-384") return find_hash("sha384");
  if (wanted == "sha-224") return find_hash("sha224");
  if (wanted == "ripemd-160" || wanted == "rmd160") return find_hash("ripemd160");
  if (wanted == "xxh3-64" || wanted == "xxhash3") return find_hash("xxh3");
  if (wanted == "crc-32") return find_hash("crc32");
  return nullptr;
}

Result<Bytes> compute_hash(const HashAlgorithm& algorithm, ByteView data, const HashParams& params) {
  CVT_TRY(check_params(algorithm, params));
  return instantiate(algorithm, params)->digest_of(data);
}

Result<Bytes> compute_hmac(const HashAlgorithm& algorithm, ByteView key, ByteView data) {
  return hmac_with_progress(algorithm, key, data, Context{});
}

std::string digest_hex(ByteView digest, bool uppercase) {
  std::string out;
  out.reserve(digest.size() * 2);
  for (const std::byte b : digest) detail::append_hex_byte(out, std::to_integer<std::uint8_t>(b), uppercase);
  return out;
}

std::string digest_sri(const HashAlgorithm& algorithm, ByteView digest) {
  if (algorithm.sri_name.empty()) return {};
  return std::string(algorithm.sri_name) + "-" + util::base64_encode(digest);
}

Result<Bytes> digest_multihash(const HashAlgorithm& algorithm, ByteView digest) {
  if (algorithm.multihash_code < 0) return fail(Status::InvalidArgument, "multihash has no code for {}", algorithm.name);
  std::uint64_t code = static_cast<std::uint64_t>(algorithm.multihash_code);
  // BLAKE2 codes encode the digest length: blake2b-8 = 0xb201 ... blake2b-512 = 0xb240.
  if (algorithm.id == "blake2b" || algorithm.id == "blake2s") code += digest.size();
  Bytes out;
  append_varint(out, code);
  append_varint(out, digest.size());
  out.insert(out.end(), digest.begin(), digest.end());
  return out;
}

void register_hashes(Registry& registry) {
  // ---- one target per popular algorithm ------------------------------------------
  struct Target {
    std::string_view id;
    std::string_view extension;
    int popularity;
  };
  constexpr Target targets[] = {
      {"md5", "md5", 45},          {"sha1", "sha1", 40},         {"sha224", "sha224", 15},   {"sha256", "sha256", 55},
      {"sha384", "sha384", 20},    {"sha512", "sha512", 35},     {"sha3-256", "sha3-256", 15},
      {"sha3-512", "sha3-512", 12}, {"blake2b", "b2", 15},        {"blake2s", "blake2s", 8},  {"blake3", "b3", 18},
      {"crc32", "crc32", 30},      {"xxh64", "xxh64", 12},       {"xxh3", "xxh3", 12},
  };
  for (const Target& target : targets) {
    const HashAlgorithm* algorithm = find_hash(target.id);
    if (algorithm == nullptr) continue;
    const std::size_t hex_digits = algorithm->digest_size * 2;
    const std::string extension(target.extension);
    add_binary_encoding(registry, {
        .id = std::string(target.id),
        .name = std::string(algorithm->name) + " checksum",
        .description = std::format("The {} digest of the file, as hex, a {}sum-style line, Base64 or another notation.",
                                   algorithm->name, target.id),
        .extensions = {extension},
        .popularity = target.popularity,
        .encode = [algorithm](const ir::Blob& blob, const WriteContext& ctx) { return encode_digest(*algorithm, blob, ctx); },
        .encode_options = options_for(algorithm, false),
        .expansion = 0,
        .sniff = [extension, hex_digits](ByteView data, std::string_view filename) {
          return sniff_sumfile(data, filename, extension, hex_digits);
        },
    });
  }

  // ---- any algorithm ------------------------------------------------------------------
  add_binary_encoding(registry, {
      .id = "hash", .name = "Hash or checksum",
      .description = "Any of about 70 hash functions and checksums (SHA-2/3, BLAKE2/3, RIPEMD, CRC, xxHash, ...), with HMAC.",
      .extensions = {"hash"}, .popularity = 30,
      .encode = [](const ir::Blob& blob, const WriteContext& ctx) -> Result<std::string> {
        const std::string id = ctx.options.get_string("algorithm", "sha256");
        const HashAlgorithm* algorithm = find_hash(id);
        if (algorithm == nullptr) return fail(Status::InvalidArgument, "unknown hash algorithm '{}'", id);
        return encode_digest(*algorithm, blob, ctx);
      },
      .encode_options = options_for(nullptr, true),
      .expansion = 0,
  });

  // ---- reports ---------------------------------------------------------------------------
  const OptionSpec algorithms_option = OptionSpec::text(
      "algorithms", "Algorithms", std::string(default_report),
      "Comma-separated algorithm ids (e.g. md5,sha256,blake3,crc32c), or 'all'.");
  add_binary_encoding(registry, {
      .id = "checksums", .name = "Checksum report",
      .description = "Size and checksums of the file (by default CRC-32, Adler-32, MD5, SHA-1 and SHA-256).",
      .extensions = {"txt", "checksums"}, .popularity = 55,
      .encode = [](const ir::Blob& blob, const WriteContext& ctx) -> Result<std::string> {
        CVT_TRY_ASSIGN(const auto values, report_values(blob, ctx));
        std::string out;
        if (!blob.filename.empty()) out += "File: " + util::sanitize_utf8(blob.filename) + "\n";
        out += std::format("Size: {} bytes\n", blob.bytes.size());
        for (const auto& [algorithm, hex] : values) out += std::format("{}: {}\n", algorithm->name, hex);
        return out;
      },
      .encode_options = {algorithms_option,
                         OptionSpec::boolean("uppercase", "Uppercase digits", false,
                                             "Write hexadecimal digits A-F in upper case.")},
      .expansion = 0,
      .sniff = sniff_checksums,
  });
  add_binary_encoding(registry, {
      .id = "checksums-json", .name = "Checksum report (JSON)",
      .description = "Size and checksums of the file as a JSON object keyed by algorithm id.",
      .extensions = {"json"}, .mime = "application/json", .popularity = 25,
      .encode = [](const ir::Blob& blob, const WriteContext& ctx) -> Result<std::string> {
        CVT_TRY_ASSIGN(const auto values, report_values(blob, ctx));
        Json root = Json::object();
        if (!blob.filename.empty()) root["file"] = util::sanitize_utf8(blob.filename);
        root["size"] = static_cast<std::uint64_t>(blob.bytes.size());
        for (const auto& [algorithm, hex] : values) root[algorithm->id] = hex;
        return util::dump_json(root, 2) + "\n";
      },
      .encode_options = {algorithms_option,
                         OptionSpec::boolean("uppercase", "Uppercase digits", false,
                                             "Write hexadecimal digits A-F in upper case.")},
      .expansion = 0,
      .sniff = [](ByteView data, std::string_view) {
        const std::string_view text = as_chars(data).substr(0, 512);
        return util::trim_left(text).starts_with("{") && text.find("\"size\": ") != std::string_view::npos &&
                       (text.find("\"sha256\": \"") != std::string_view::npos ||
                        text.find("\"crc32\": \"") != std::string_view::npos ||
                        text.find("\"md5\": \"") != std::string_view::npos)
                   ? 70
                   : 0;
      },
  });
}

}  // namespace cvt::encoding
