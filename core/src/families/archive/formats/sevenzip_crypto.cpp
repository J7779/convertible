// Convertible core - archive family: 7z AES-256 (method 06F10701) key derivation and ciphering, and header encoders.
//
// 7zAES, as described with the LZMA SDK: the key is SHA-256 over 2^n
// repetitions of (salt, UTF-16LE password, 64-bit little-endian round
// counter); data is AES-256-CBC with the stored IV (zero-extended to 16
// bytes) and zero-padded to whole blocks, the coder's unpack size giving the
// real length. The properties byte 0 holds n (bits 0..5) and "salt present"
// / "IV present" flags (bits 7 / 6); byte 1 the remaining salt and IV sizes.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstring>

#include "util/bytes.hpp"
#include "families/archive/crypto.hpp"
#include "families/archive/formats/sevenzip_internal.hpp"
#include "util/hash.hpp"
#include "util/text.hpp"

namespace cvt::archive::sevenzip {

namespace {
constexpr unsigned max_cycles_power = 24;
constexpr unsigned no_hash_power = 0x3F;
}  // namespace

void write_number(util::ByteWriter& out, std::uint64_t value) {
  // 7z UINT64: the first byte carries (8 - n) high bits of the value behind n
  // leading one bits; n little-endian bytes follow.
  for (int n = 0; n < 8; ++n) {
    const unsigned bits = 7u - static_cast<unsigned>(n);  // value bits left in the first byte
    if (value < (std::uint64_t{1} << (8 * n + static_cast<int>(bits)))) {
      const auto high = static_cast<std::uint8_t>(value >> (8 * n));
      const auto prefix = static_cast<std::uint8_t>(0xFF00u >> n);
      out.u8(static_cast<std::uint8_t>(prefix | high));
      for (int i = 0; i < n; ++i) out.u8(static_cast<std::uint8_t>(value >> (8 * i)));
      return;
    }
  }
  out.u8(0xFF);
  out.u64le(value);
}

void write_folder(util::ByteWriter& out, const Folder& folder) {
  write_number(out, folder.coders.size());
  for (const Coder& c : folder.coders) {
    std::uint8_t id_bytes[8] = {};
    std::size_t id_size = 0;
    for (std::uint64_t v = c.id; v != 0; v >>= 8) ++id_size;
    if (id_size == 0) id_size = 1;
    for (std::size_t i = 0; i < id_size; ++i) id_bytes[i] = static_cast<std::uint8_t>(c.id >> (8 * (id_size - 1 - i)));
    const bool complex = c.num_in != 1 || c.num_out != 1;
    std::uint8_t flags = static_cast<std::uint8_t>(id_size);
    if (complex) flags |= 0x10;
    if (!c.properties.empty()) flags |= 0x20;
    out.u8(flags);
    for (std::size_t i = 0; i < id_size; ++i) out.u8(id_bytes[i]);
    if (complex) {
      write_number(out, c.num_in);
      write_number(out, c.num_out);
    }
    if (!c.properties.empty()) {
      write_number(out, c.properties.size());
      out.bytes(c.properties);
    }
  }
  for (const BindPair& b : folder.binds) {
    write_number(out, b.in);
    write_number(out, b.out);
  }
  if (folder.packed.size() > 1) {
    for (const std::uint32_t p : folder.packed) write_number(out, p);
  }
}

Result<AesParams> parse_aes_properties(ByteView props) {
  if (props.empty()) return fail(Status::Decode, "7z AES properties are missing");
  AesParams p;
  const unsigned b0 = std::to_integer<unsigned>(props[0]);
  p.cycles_power = b0 & 0x3Fu;
  if ((b0 & 0xC0u) == 0) {
    if (props.size() != 1) return fail(Status::Decode, "7z AES properties are damaged");
    return p;
  }
  if (props.size() < 2) return fail(Status::Decode, "7z AES properties are damaged");
  const unsigned b1 = std::to_integer<unsigned>(props[1]);
  const std::size_t salt_size = ((b0 >> 7) & 1u) + (b1 >> 4);
  const std::size_t iv_size = ((b0 >> 6) & 1u) + (b1 & 0x0Fu);
  if (props.size() != 2 + salt_size + iv_size) return fail(Status::Decode, "7z AES properties are damaged");
  const ByteView salt = props.subspan(2, salt_size);
  p.salt.assign(salt.begin(), salt.end());
  for (std::size_t i = 0; i < iv_size; ++i) p.iv[i] = std::to_integer<std::uint8_t>(props[2 + salt_size + i]);
  return p;
}

Bytes aes_properties(const AesParams& params) {
  util::ByteWriter w;
  const std::size_t salt = params.salt.size();
  const std::size_t iv = params.iv.size();
  std::uint8_t b0 = static_cast<std::uint8_t>(params.cycles_power & 0x3Fu);
  if (salt > 0) b0 |= 0x80;
  if (iv > 0) b0 |= 0x40;
  w.u8(b0);
  if (salt > 0 || iv > 0) {
    const auto b1 = static_cast<std::uint8_t>(((salt > 0 ? salt - 1 : 0) << 4) | (iv > 0 ? iv - 1 : 0));
    w.u8(b1);
    w.bytes(params.salt);
    w.bytes(std::span<const std::uint8_t>(params.iv.data(), params.iv.size()));
  }
  return w.take();
}

Result<std::array<std::uint8_t, 32>> derive_key(std::string_view password, ByteView salt, unsigned cycles_power,
                                                const Progress& progress) {
  const std::u16string wide = util::utf8_to_utf16(password);
  Bytes pw;
  pw.reserve(wide.size() * 2);
  for (const char16_t c : wide) {
    pw.push_back(static_cast<std::byte>(c & 0xFFu));
    pw.push_back(static_cast<std::byte>((c >> 8) & 0xFFu));
  }
  std::array<std::uint8_t, 32> key{};
  if (cycles_power == no_hash_power) {
    std::size_t pos = 0;
    for (const std::byte b : salt) {
      if (pos < key.size()) key[pos++] = std::to_integer<std::uint8_t>(b);
    }
    for (const std::byte b : pw) {
      if (pos < key.size()) key[pos++] = std::to_integer<std::uint8_t>(b);
    }
    return key;
  }
  if (cycles_power > max_cycles_power) {
    return fail(Status::Decode, "7z AES key derivation asks for 2^{} rounds, which is refused", cycles_power);
  }
  // Hash many rounds per update: a buffer of `batch` units, counters patched in place.
  const std::size_t unit = salt.size() + pw.size() + 8;
  constexpr std::size_t batch = 64;
  Bytes buffer(unit * batch);
  for (std::size_t k = 0; k < batch; ++k) {
    std::byte* at = buffer.data() + k * unit;
    if (!salt.empty()) ::cvt::util::copy_bytes(at, salt.data(), salt.size());
    if (!pw.empty()) ::cvt::util::copy_bytes(at + salt.size(), pw.data(), pw.size());
  }
  util::Sha256 sha;
  const std::uint64_t rounds = std::uint64_t{1} << cycles_power;
  std::uint64_t counter = 0;
  while (counter < rounds) {
    const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(batch, rounds - counter));
    for (std::size_t k = 0; k < n; ++k) {
      std::byte* c = buffer.data() + k * unit + salt.size() + pw.size();
      const std::uint64_t value = counter + k;
      for (int i = 0; i < 8; ++i) c[i] = static_cast<std::byte>((value >> (8 * i)) & 0xFFu);
    }
    sha.update(ByteView(buffer).first(n * unit));
    counter += n;
    if ((counter & 0xFFFFu) == 0 && !progress.report(static_cast<double>(counter) / static_cast<double>(rounds))) {
      return fail(Status::Cancelled, "cancelled");
    }
  }
  const util::Sha256::Digest digest = sha.finish();
  std::copy(digest.begin(), digest.end(), key.begin());
  secure_zero(std::span<std::byte>(buffer));
  secure_zero(std::span<std::byte>(pw));
  return key;
}

Bytes aes_encrypt(ByteView data, const std::array<std::uint8_t, 32>& key, const std::array<std::uint8_t, 16>& iv) {
  Bytes out(data.begin(), data.end());
  out.resize((out.size() + 15) / 16 * 16, std::byte{0});
  Aes aes;
  (void)aes.set_key(std::span<const std::uint8_t>(key.data(), key.size()));
  Aes::Block chain = iv;
  (void)aes_cbc_encrypt(aes, chain, std::span<std::byte>(out));
  return out;
}

Result<Bytes> aes_decrypt(ByteView data, const std::array<std::uint8_t, 32>& key, const std::array<std::uint8_t, 16>& iv) {
  if (data.size() % 16 != 0) return fail(Status::Decode, "7z AES data is not a whole number of blocks");
  Bytes out(data.begin(), data.end());
  Aes aes;
  (void)aes.set_key(std::span<const std::uint8_t>(key.data(), key.size()));
  Aes::Block chain = iv;
  CVT_TRY(aes_cbc_decrypt(aes, chain, std::span<std::byte>(out)));
  return out;
}

}  // namespace cvt::archive::sevenzip
