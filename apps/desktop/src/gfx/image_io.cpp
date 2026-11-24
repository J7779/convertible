// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "gfx/image_io.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

#include "base/text_util.hpp"
#include "platform/fileio.hpp"

namespace cvd::gfx {

std::uint32_t crc32(const std::uint8_t* data, std::size_t size, std::uint32_t crc) {
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  crc = ~crc;
  for (std::size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
  return ~crc;
}

namespace {

class BitWriter {
 public:
  explicit BitWriter(std::vector<std::uint8_t>& out) : out_(out) {}
  void bits(std::uint32_t value, int count) {
    acc_ |= static_cast<std::uint64_t>(value) << n_;
    n_ += count;
    while (n_ >= 8) {
      out_.push_back(static_cast<std::uint8_t>(acc_ & 0xFFu));
      acc_ >>= 8;
      n_ -= 8;
    }
  }
  /// Huffman codes are sent most significant bit first.
  void code(std::uint32_t value, int count) {
    std::uint32_t rev = 0;
    for (int i = 0; i < count; ++i) rev |= ((value >> i) & 1u) << (count - 1 - i);
    bits(rev, count);
  }
  void flush() {
    if (n_ > 0) out_.push_back(static_cast<std::uint8_t>(acc_ & 0xFFu));
    acc_ = 0;
    n_ = 0;
  }

 private:
  std::vector<std::uint8_t>& out_;
  std::uint64_t acc_ = 0;
  int n_ = 0;
};

void put_literal(BitWriter& bw, int lit) {
  if (lit < 144) bw.code(static_cast<std::uint32_t>(0x30 + lit), 8);
  else bw.code(static_cast<std::uint32_t>(0x190 + lit - 144), 9);
}

void put_symbol(BitWriter& bw, int sym) {
  if (sym < 280) bw.code(static_cast<std::uint32_t>(sym - 256), 7);
  else bw.code(static_cast<std::uint32_t>(0xC0 + sym - 280), 8);
}

constexpr std::array<int, 29> kLenBase{3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                       31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<int, 29> kLenExtra{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<int, 30> kDistBase{1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                        193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<int, 30> kDistExtra{0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void put_match(BitWriter& bw, int length, int distance) {
  int li = 28;
  while (kLenBase[static_cast<std::size_t>(li)] > length) --li;
  put_symbol(bw, 257 + li);
  if (kLenExtra[static_cast<std::size_t>(li)]) {
    bw.bits(static_cast<std::uint32_t>(length - kLenBase[static_cast<std::size_t>(li)]), kLenExtra[static_cast<std::size_t>(li)]);
  }
  int di = 29;
  while (kDistBase[static_cast<std::size_t>(di)] > distance) --di;
  bw.code(static_cast<std::uint32_t>(di), 5);
  if (kDistExtra[static_cast<std::size_t>(di)]) {
    bw.bits(static_cast<std::uint32_t>(distance - kDistBase[static_cast<std::size_t>(di)]), kDistExtra[static_cast<std::size_t>(di)]);
  }
}

/// zlib stream: one fixed-Huffman block with greedy LZ77 (single-candidate hash).
std::vector<std::uint8_t> zlib_compress(const std::vector<std::uint8_t>& in) {
  std::vector<std::uint8_t> out{0x78, 0x01};
  BitWriter bw(out);
  bw.bits(1, 1);  // final block
  bw.bits(1, 2);  // fixed Huffman
  constexpr int kHashBits = 16;
  std::vector<int> head(1u << kHashBits, -1);
  const auto hash = [&](std::size_t i) {
    const std::uint32_t v = static_cast<std::uint32_t>(in[i]) | (static_cast<std::uint32_t>(in[i + 1]) << 8) |
                            (static_cast<std::uint32_t>(in[i + 2]) << 16);
    return (v * 2654435761u) >> (32 - kHashBits);
  };
  const std::size_t n = in.size();
  std::size_t i = 0;
  while (i < n) {
    int best_len = 0;
    int best_dist = 0;
    if (i + 3 <= n) {
      const std::uint32_t h = hash(i);
      const int cand = head[h];
      head[h] = static_cast<int>(i);
      if (cand >= 0 && i - static_cast<std::size_t>(cand) <= 32768) {
        const std::size_t c = static_cast<std::size_t>(cand);
        std::size_t len = 0;
        const std::size_t maxlen = std::min<std::size_t>(258, n - i);
        while (len < maxlen && in[c + len] == in[i + len]) ++len;
        if (len >= 3) {
          best_len = static_cast<int>(len);
          best_dist = static_cast<int>(i - c);
        }
      }
    }
    if (best_len >= 3) {
      put_match(bw, best_len, best_dist);
      const std::size_t end = i + static_cast<std::size_t>(best_len);
      for (std::size_t k = i + 1; k < end && k + 3 <= n; k += 1) head[hash(k)] = static_cast<int>(k);
      i = end;
    } else {
      put_literal(bw, in[i]);
      ++i;
    }
  }
  put_symbol(bw, 256);
  bw.flush();
  std::uint32_t a = 1, b = 0;
  for (const std::uint8_t v : in) {
    a = (a + v) % 65521u;
    b = (b + a) % 65521u;
  }
  const std::uint32_t adler = (b << 16) | a;
  for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<std::uint8_t>((adler >> s) & 0xFFu));
  return out;
}

void put_be32(std::vector<std::uint8_t>& out, std::uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<std::uint8_t>((v >> s) & 0xFFu));
}

void put_chunk(std::vector<std::uint8_t>& out, const char* type, const std::vector<std::uint8_t>& data) {
  put_be32(out, static_cast<std::uint32_t>(data.size()));
  const std::size_t start = out.size();
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(type[i]));
  out.insert(out.end(), data.begin(), data.end());
  put_be32(out, crc32(out.data() + start, out.size() - start));
}

}  // namespace

std::vector<std::uint8_t> encode_png(const Bitmap& bitmap) {
  const std::vector<std::uint8_t> rgba = to_rgba8(bitmap);
  const std::size_t stride = static_cast<std::size_t>(bitmap.width) * 4;
  std::vector<std::uint8_t> raw;
  raw.reserve((stride + 1) * static_cast<std::size_t>(bitmap.height));
  std::vector<std::uint8_t> sub(stride), up(stride);
  for (int y = 0; y < bitmap.height; ++y) {
    const std::uint8_t* row = rgba.data() + static_cast<std::size_t>(y) * stride;
    const std::uint8_t* prev = y > 0 ? row - stride : nullptr;
    long cost_sub = 0, cost_up = 0;
    for (std::size_t i = 0; i < stride; ++i) {
      sub[i] = static_cast<std::uint8_t>(row[i] - (i >= 4 ? row[i - 4] : 0));
      up[i] = static_cast<std::uint8_t>(row[i] - (prev ? prev[i] : 0));
      cost_sub += std::abs(static_cast<int>(static_cast<std::int8_t>(sub[i])));
      cost_up += std::abs(static_cast<int>(static_cast<std::int8_t>(up[i])));
    }
    if (cost_up < cost_sub) {
      raw.push_back(2);
      raw.insert(raw.end(), up.begin(), up.end());
    } else {
      raw.push_back(1);
      raw.insert(raw.end(), sub.begin(), sub.end());
    }
  }
  std::vector<std::uint8_t> out{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<std::uint8_t> ihdr;
  put_be32(ihdr, static_cast<std::uint32_t>(bitmap.width));
  put_be32(ihdr, static_cast<std::uint32_t>(bitmap.height));
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
  put_chunk(out, "IHDR", ihdr);
  put_chunk(out, "IDAT", zlib_compress(raw));
  put_chunk(out, "IEND", {});
  return out;
}

std::vector<std::uint8_t> encode_bmp(const Bitmap& bitmap) {
  const std::vector<std::uint8_t> rgba = to_rgba8(bitmap);
  const std::uint32_t data_size = static_cast<std::uint32_t>(rgba.size());
  std::vector<std::uint8_t> out;
  const auto le16 = [&](std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
  };
  const auto le32 = [&](std::uint32_t v) {
    for (int s = 0; s < 32; s += 8) out.push_back(static_cast<std::uint8_t>((v >> s) & 0xFF));
  };
  out.push_back('B');
  out.push_back('M');
  le32(14 + 40 + data_size);
  le32(0);
  le32(14 + 40);
  le32(40);
  le32(static_cast<std::uint32_t>(bitmap.width));
  le32(static_cast<std::uint32_t>(-bitmap.height));  // top-down
  le16(1);
  le16(32);
  le32(0);
  le32(data_size);
  le32(2835);
  le32(2835);
  le32(0);
  le32(0);
  for (std::size_t i = 0; i < rgba.size(); i += 4) {
    out.push_back(rgba[i + 2]);
    out.push_back(rgba[i + 1]);
    out.push_back(rgba[i + 0]);
    out.push_back(rgba[i + 3]);
  }
  return out;
}

bool write_image(const Bitmap& bitmap, const std::string& path_utf8) {
  const std::string ext = path_extension(path_utf8);
  const std::vector<std::uint8_t> data = ext == "bmp" ? encode_bmp(bitmap) : encode_png(bitmap);
  return fileio::write_file(path_utf8, data.data(), data.size());
}

}  // namespace cvd::gfx
