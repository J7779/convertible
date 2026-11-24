// Convertible core - archive family: 7z internals shared by the reader, the writer and the 7zAES code.
//
// Written from the 7z format description that ships with the LZMA SDK
// (7zFormat.txt, public domain) and the method list (Methods.txt). All
// numbers in a 7z header are "7z UINT64": the count of leading one bits of
// the first byte says how many little-endian bytes follow.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "engine/progress.hpp"
#include "ir/archive.hpp"
#include "util/bytes.hpp"

namespace cvt::archive::sevenzip {

inline constexpr std::array<std::uint8_t, 6> signature = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C};
inline constexpr std::size_t signature_header_size = 32;

// ---------------------------------------------------------------------------- property ids (7zFormat.txt)

enum Prop : std::uint8_t {
  kEnd = 0x00,
  kHeader = 0x01,
  kArchiveProperties = 0x02,
  kAdditionalStreamsInfo = 0x03,
  kMainStreamsInfo = 0x04,
  kFilesInfo = 0x05,
  kPackInfo = 0x06,
  kUnpackInfo = 0x07,
  kSubStreamsInfo = 0x08,
  kSize = 0x09,
  kCrc = 0x0A,
  kFolder = 0x0B,
  kCodersUnpackSize = 0x0C,
  kNumUnpackStream = 0x0D,
  kEmptyStream = 0x0E,
  kEmptyFile = 0x0F,
  kAnti = 0x10,
  kName = 0x11,
  kCTime = 0x12,
  kATime = 0x13,
  kMTime = 0x14,
  kWinAttributes = 0x15,
  kComment = 0x16,
  kEncodedHeader = 0x17,
  kStartPos = 0x18,
  kDummy = 0x19,
};

// ---------------------------------------------------------------------------- method ids (Methods.txt), big-endian packed

inline constexpr std::uint64_t id_copy = 0x00;
inline constexpr std::uint64_t id_delta = 0x03;
inline constexpr std::uint64_t id_x86_new = 0x04;
inline constexpr std::uint64_t id_ppc_new = 0x05;
inline constexpr std::uint64_t id_ia64_new = 0x06;
inline constexpr std::uint64_t id_arm_new = 0x07;
inline constexpr std::uint64_t id_armt_new = 0x08;
inline constexpr std::uint64_t id_sparc_new = 0x09;
inline constexpr std::uint64_t id_arm64 = 0x0A;
inline constexpr std::uint64_t id_riscv = 0x0B;
inline constexpr std::uint64_t id_lzma2 = 0x21;
inline constexpr std::uint64_t id_lzma = 0x030101;
inline constexpr std::uint64_t id_bcj = 0x03030103;
inline constexpr std::uint64_t id_bcj2 = 0x0303011B;
inline constexpr std::uint64_t id_ppc = 0x03030205;
inline constexpr std::uint64_t id_ia64 = 0x03030401;
inline constexpr std::uint64_t id_arm = 0x03030501;
inline constexpr std::uint64_t id_armt = 0x03030701;
inline constexpr std::uint64_t id_sparc = 0x03030805;
inline constexpr std::uint64_t id_ppmd = 0x030401;
inline constexpr std::uint64_t id_deflate = 0x040108;
inline constexpr std::uint64_t id_deflate64 = 0x040109;
inline constexpr std::uint64_t id_bzip2 = 0x040202;
inline constexpr std::uint64_t id_zstd = 0x04F71101;
inline constexpr std::uint64_t id_aes = 0x06F10701;

// ---------------------------------------------------------------------------- Windows attributes

inline constexpr std::uint32_t attr_readonly = 0x01;
inline constexpr std::uint32_t attr_directory = 0x10;
inline constexpr std::uint32_t attr_archive = 0x20;
inline constexpr std::uint32_t attr_unix_extension = 0x8000;  // high 16 bits = st_mode

/// Seconds between 1601-01-01 (FILETIME epoch) and 1970-01-01.
inline constexpr std::int64_t filetime_epoch_offset = 11644473600;

// ---------------------------------------------------------------------------- folders

/// Structural limits (7-Zip itself uses 64 coders / 64 streams per folder).
inline constexpr std::size_t max_coders = 64;
inline constexpr std::size_t max_coder_streams = 64;

struct Coder {
  std::uint64_t id = 0;
  Bytes properties;
  std::uint32_t num_in = 1;   ///< packed side (inputs when decoding)
  std::uint32_t num_out = 1;  ///< unpacked side
};

struct BindPair {
  std::uint32_t in = 0;   ///< global in-stream index
  std::uint32_t out = 0;  ///< global out-stream index that feeds it
};

struct Folder {
  std::vector<Coder> coders;
  std::vector<BindPair> binds;
  std::vector<std::uint32_t> packed;       ///< in-stream index of each packed stream
  std::vector<std::uint64_t> unpack_sizes;  ///< one per out-stream
  std::optional<std::uint32_t> crc;
  std::uint32_t main_out = 0;          ///< the out-stream not consumed by a bind pair
  std::size_t first_pack_stream = 0;   ///< index into StreamsInfo::pack_sizes

  [[nodiscard]] std::uint64_t unpack_size() const noexcept {
    return main_out < unpack_sizes.size() ? unpack_sizes[main_out] : 0;
  }
  [[nodiscard]] bool encrypted() const noexcept {
    for (const Coder& c : coders) {
      if (c.id == id_aes) return true;
    }
    return false;
  }
};

/// Writes a folder description (coders, bind pairs, packed indices) as the
/// writer and the tests need it.
void write_folder(util::ByteWriter& out, const Folder& folder);
/// Writes a 7z UINT64.
void write_number(util::ByteWriter& out, std::uint64_t value);

// ---------------------------------------------------------------------------- 7zAES

/// The parameters stored in the 7zAES coder properties.
struct AesParams {
  unsigned cycles_power = 19;  ///< 2^n SHA-256 rounds; 0x3F = no hashing
  Bytes salt;                  ///< 0..16 bytes
  std::array<std::uint8_t, 16> iv{};
};

[[nodiscard]] Result<AesParams> parse_aes_properties(ByteView props);
[[nodiscard]] Bytes aes_properties(const AesParams& params);

/// SHA-256 over 2^cycles_power repetitions of salt + UTF-16LE password + a
/// 64-bit little-endian counter (cycles_power 0x3F: salt and password copied
/// into the key). Powers above 24 are refused as hostile (7-Zip writes 19).
[[nodiscard]] Result<std::array<std::uint8_t, 32>> derive_key(std::string_view password, ByteView salt,
                                                              unsigned cycles_power, const Progress& progress);

/// AES-256-CBC over `data`, which is zero-padded to a multiple of 16 first.
[[nodiscard]] Bytes aes_encrypt(ByteView data, const std::array<std::uint8_t, 32>& key,
                                const std::array<std::uint8_t, 16>& iv);
/// AES-256-CBC decryption; `data.size()` must be a multiple of 16.
[[nodiscard]] Result<Bytes> aes_decrypt(ByteView data, const std::array<std::uint8_t, 32>& key,
                                        const std::array<std::uint8_t, 16>& iv);

// ---------------------------------------------------------------------------- reader / writer

[[nodiscard]] Result<ir::Archive> read_7z(ReadContext& ctx);
[[nodiscard]] Result<Bytes> write_7z(const ir::Archive& archive, WriteContext& ctx);

}  // namespace cvt::archive::sevenzip
