// Helpers shared by the legacy (decode-only) codec tests of the archive family.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <string>
#include <vector>

#include "families/archive/codec_test_util.hpp"

namespace cvt::test::legacy {

namespace cz = cvt::compression;

inline Bytes fixture(std::string_view name) {
  return test::fixture(std::string("archive/legacy_codecs/") + std::string(name));
}

/// The 40 000-byte text of the fixtures (codec::text_corpus).
inline Bytes text40k() { return codec::text_corpus(40000); }

/// x86-like data full of E8 (CALL rel32) instructions, as made by the
/// fixture generator (see legacy_codecs/SOURCES.md).
inline Bytes x86_like(std::size_t size) {
  Bytes out;
  std::uint32_t state = 1;
  while (out.size() < size) {
    state = state * 1103515245u + 12345u;
    const std::uint32_t target = (state >> 8) % 30000u;
    const std::uint32_t rel = target - static_cast<std::uint32_t>(out.size() + 5);
    const std::uint8_t bytes[10] = {0xE8,
                                    static_cast<std::uint8_t>(rel),
                                    static_cast<std::uint8_t>(rel >> 8),
                                    static_cast<std::uint8_t>(rel >> 16),
                                    static_cast<std::uint8_t>(rel >> 24),
                                    0x83,
                                    0xC4,
                                    static_cast<std::uint8_t>((state >> 4) & 0x0Cu),
                                    0x55,
                                    0x8B};
    for (const std::uint8_t b : bytes) out.push_back(std::byte{b});
    out.push_back(std::byte{0xEC});
  }
  out.resize(size);
  return out;
}

inline std::uint32_t le16(ByteView d, std::size_t at) {
  return std::to_integer<std::uint32_t>(d[at]) | (std::to_integer<std::uint32_t>(d[at + 1]) << 8);
}
inline std::uint32_t le32(ByteView d, std::size_t at) { return le16(d, at) | (le16(d, at + 2) << 16); }

/// LSB-first bit writer for hand-assembled streams.
class LsbWriter {
 public:
  void put(std::uint32_t value, int n) {
    for (int i = 0; i < n; ++i) {
      acc_ |= ((value >> i) & 1u) << count_;
      if (++count_ == 8) flush_byte();
    }
  }
  /// Write the `n` low bits of `code` most significant bit first (Huffman codes).
  void put_msb_first(std::uint32_t code, int n) {
    for (int i = n - 1; i >= 0; --i) put((code >> i) & 1u, 1);
  }
  Bytes finish() {
    if (count_ != 0) flush_byte();
    return std::move(out_);
  }

 private:
  void flush_byte() {
    out_.push_back(static_cast<std::byte>(acc_));
    acc_ = 0;
    count_ = 0;
  }
  Bytes out_;
  std::uint32_t acc_ = 0;
  int count_ = 0;
};

/// MSB-first bit writer.
class MsbWriter {
 public:
  void put(std::uint32_t value, int n) {
    for (int i = n - 1; i >= 0; --i) {
      acc_ = (acc_ << 1) | ((value >> i) & 1u);
      if (++count_ == 8) {
        out_.push_back(static_cast<std::byte>(acc_));
        acc_ = 0;
        count_ = 0;
      }
    }
  }
  Bytes finish() {
    while (count_ != 0) put(0, 1);
    return std::move(out_);
  }

 private:
  Bytes out_;
  std::uint32_t acc_ = 0;
  int count_ = 0;
};

}  // namespace cvt::test::legacy
