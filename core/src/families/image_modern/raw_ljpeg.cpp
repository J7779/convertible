// Convertible core - image_modern family: lossless JPEG (T.81 process 14) decoder.
// Written from ITU-T T.81 Annex C (Huffman tables), Annex F.2.2 (decoding
// procedures) and Annex H (lossless mode).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "util/bytes.hpp"
#include "families/image_modern/raw_ljpeg.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace cvt::image_modern::raw {

namespace {

constexpr int fast_bits = 9;

struct Huffman {
  bool defined = false;
  std::array<std::uint8_t, 1 << fast_bits> fast_length{};  // 0 = not in the fast table
  std::array<std::uint8_t, 1 << fast_bits> fast_value{};
  std::array<std::int32_t, 18> maxcode{};
  std::array<std::int32_t, 17> mincode{};
  std::array<std::int32_t, 17> valptr{};
  std::array<std::uint8_t, 256> values{};
};

// Annex C: canonical codes from BITS/HUFFVAL.
bool build_huffman(Huffman& h, const std::array<std::uint8_t, 16>& counts, const std::uint8_t* values, int total) {
  h = Huffman{};
  if (total > 256) return false;
  ::cvt::util::copy_bytes(h.values.data(), values, static_cast<std::size_t>(total));
  std::int32_t code = 0;
  int k = 0;
  for (int length = 1; length <= 16; ++length) {
    const int n = counts[static_cast<std::size_t>(length - 1)];
    h.valptr[static_cast<std::size_t>(length)] = k;
    h.mincode[static_cast<std::size_t>(length)] = code;
    for (int i = 0; i < n; ++i, ++k, ++code) {
      if (code >= (1 << length)) return false;  // over-subscribed table
      if (length <= fast_bits) {
        const int shift = fast_bits - length;
        for (int fill = 0; fill < (1 << shift); ++fill) {
          const auto at = static_cast<std::size_t>((code << shift) | fill);
          h.fast_length[at] = static_cast<std::uint8_t>(length);
          h.fast_value[at] = h.values[static_cast<std::size_t>(k)];
        }
      }
    }
    h.maxcode[static_cast<std::size_t>(length)] = n > 0 ? code - 1 : -1;
    code <<= 1;
  }
  h.maxcode[17] = 0x7FFFFFFF;
  h.defined = true;
  return true;
}

class BitReader {
 public:
  BitReader(const std::uint8_t* data, std::size_t size, std::size_t pos) : data_(data), size_(size), pos_(pos) {}

  std::uint32_t peek(int count) {
    if (count_ < count) fill();
    return static_cast<std::uint32_t>(buffer_ >> (64 - count));
  }
  void skip(int count) {
    buffer_ <<= count;
    count_ -= count;
  }
  std::uint32_t bits(int count) {
    if (count == 0) return 0;
    const std::uint32_t v = peek(count);
    skip(count);
    return v;
  }
  // Bits consumed beyond the end of the entropy-coded data.
  [[nodiscard]] bool overrun() const noexcept { return static_cast<std::int64_t>(phantom_) * 8 - count_ > 0; }

  // Drop buffered bits and step over an RSTn marker.
  void restart() {
    buffer_ = 0;
    count_ = 0;
    phantom_ = 0;
    marker_ = false;
    while (pos_ + 1 < size_ && data_[pos_] == 0xFF && data_[pos_ + 1] == 0xFF) ++pos_;
    if (pos_ + 1 < size_ && data_[pos_] == 0xFF && (data_[pos_ + 1] & 0xF8) == 0xD0) pos_ += 2;
  }

 private:
  void fill() {
    while (count_ <= 56) {
      std::uint64_t byte = 0;
      if (!marker_ && pos_ < size_) {
        byte = data_[pos_];
        if (byte == 0xFF) {
          const std::uint8_t next = pos_ + 1 < size_ ? data_[pos_ + 1] : 0xD9;
          if (next == 0x00) {
            pos_ += 2;
          } else {
            marker_ = true;  // a marker ends the entropy-coded segment
            byte = 0;
            ++phantom_;
          }
        } else {
          ++pos_;
        }
      } else {
        ++phantom_;
      }
      buffer_ |= byte << (56 - count_);
      count_ += 8;
    }
  }

  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t pos_;
  std::uint64_t buffer_ = 0;
  int count_ = 0;
  std::uint32_t phantom_ = 0;
  bool marker_ = false;
};

inline int decode_symbol(BitReader& bits, const Huffman& h) {
  const std::uint32_t look = bits.peek(fast_bits);
  if (const int length = h.fast_length[look]; length != 0) {
    bits.skip(length);
    return h.fast_value[look];
  }
  const std::uint32_t code16 = bits.peek(16);
  for (int length = fast_bits + 1; length <= 16; ++length) {
    const auto code = static_cast<std::int32_t>(code16 >> (16 - length));
    if (code <= h.maxcode[static_cast<std::size_t>(length)]) {
      bits.skip(length);
      const std::int32_t index = h.valptr[static_cast<std::size_t>(length)] + code - h.mincode[static_cast<std::size_t>(length)];
      return (index >= 0 && index < 256) ? h.values[static_cast<std::size_t>(index)] : -1;
    }
  }
  return -1;
}

inline std::int32_t decode_difference(BitReader& bits, int ssss) {
  if (ssss == 0) return 0;
  if (ssss == 16) return 32768;
  const auto v = static_cast<std::int32_t>(bits.bits(ssss));
  return v < (1 << (ssss - 1)) ? v - (1 << ssss) + 1 : v;
}

struct Header {
  LjpegFrame frame;
  std::array<int, 4> component_ids{};
  std::array<int, 4> table_of{};  // Huffman table per frame component
  std::array<Huffman, 4> tables{};
  int point_transform = 0;
  std::uint32_t restart_interval = 0;
  std::size_t scan_data = 0;  // position of the entropy-coded data
};

std::uint32_t be16(const std::uint8_t* p) { return (std::uint32_t{p[0]} << 8) | p[1]; }

// Parse markers up to the first SOS. With `info_only`, stop after SOF3.
Result<Header> parse_header(ByteView bytes, bool info_only) {
  const auto* p = reinterpret_cast<const std::uint8_t*>(bytes.data());
  const std::size_t n = bytes.size();
  if (n < 4 || p[0] != 0xFF || p[1] != 0xD8) return fail(Status::Decode, "lossless JPEG data does not start with SOI");
  Header h;
  bool have_frame = false;
  std::size_t pos = 2;
  for (int guard = 0; guard < 1000; ++guard) {
    while (pos < n && p[pos] != 0xFF) ++pos;  // tolerate garbage between segments
    while (pos < n && p[pos] == 0xFF) ++pos;
    if (pos >= n) break;
    const std::uint8_t marker = p[pos++];
    if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) continue;
    if (marker == 0xD9) break;
    if (pos + 2 > n) break;
    const std::uint32_t length = be16(p + pos);
    if (length < 2 || pos + length > n) return fail(Status::Decode, "lossless JPEG segment is truncated");
    const std::uint8_t* s = p + pos + 2;
    const std::uint32_t body = length - 2;
    switch (marker) {
      case 0xC3: {
        if (body < 6) return fail(Status::Decode, "lossless JPEG frame header is truncated");
        h.frame.precision = s[0];
        h.frame.height = be16(s + 1);
        h.frame.width = be16(s + 3);
        h.frame.components = s[5];
        if (h.frame.precision < 2 || h.frame.precision > 16) {
          return fail(Status::Decode, "lossless JPEG precision {} is invalid", h.frame.precision);
        }
        if (h.frame.components < 1 || h.frame.components > 4 || body < 6u + 3u * static_cast<std::uint32_t>(h.frame.components)) {
          return fail(Status::Decode, "lossless JPEG with {} components is not supported", h.frame.components);
        }
        for (int c = 0; c < h.frame.components; ++c) {
          h.component_ids[static_cast<std::size_t>(c)] = s[6 + c * 3];
          if (s[7 + c * 3] != 0x11) return fail(Status::Decode, "subsampled lossless JPEG is not supported");
        }
        if (h.frame.width == 0 || h.frame.height == 0) return fail(Status::Decode, "lossless JPEG frame has no samples");
        have_frame = true;
        if (info_only) return h;
        break;
      }
      case 0xC0: case 0xC1: case 0xC2: case 0xC5: case 0xC6: case 0xC7: case 0xC9: case 0xCA: case 0xCB:
      case 0xCD: case 0xCE: case 0xCF:
        return fail(Status::Decode, "not a lossless Huffman JPEG stream");
      case 0xC4: {
        std::uint32_t at = 0;
        while (at + 17 <= body) {
          const int tc = s[at] >> 4;
          const int th = s[at] & 15;
          std::array<std::uint8_t, 16> counts{};
          int total = 0;
          for (int i = 0; i < 16; ++i) {
            counts[static_cast<std::size_t>(i)] = s[at + 1 + static_cast<std::uint32_t>(i)];
            total += counts[static_cast<std::size_t>(i)];
          }
          if (tc != 0 || th > 3 || at + 17 + static_cast<std::uint32_t>(total) > body) {
            return fail(Status::Decode, "invalid lossless JPEG Huffman table");
          }
          if (!build_huffman(h.tables[static_cast<std::size_t>(th)], counts, s + at + 17, total)) {
            return fail(Status::Decode, "invalid lossless JPEG Huffman table");
          }
          at += 17 + static_cast<std::uint32_t>(total);
        }
        break;
      }
      case 0xDD:
        if (body >= 2) h.restart_interval = be16(s);
        break;
      case 0xDA: {
        if (!have_frame) return fail(Status::Decode, "lossless JPEG scan before the frame header");
        if (body < 1) return fail(Status::Decode, "lossless JPEG scan header is truncated");
        const int ns = s[0];
        if (ns != h.frame.components || body < 1u + 2u * static_cast<std::uint32_t>(ns) + 3u) {
          return fail(Status::Decode, "lossless JPEG with several scans is not supported");
        }
        for (int i = 0; i < ns; ++i) {
          const int id = s[1 + i * 2];
          const int table = s[2 + i * 2] >> 4;
          int index = -1;
          for (int c = 0; c < h.frame.components; ++c) {
            if (h.component_ids[static_cast<std::size_t>(c)] == id) index = c;
          }
          if (index < 0) index = i;  // tolerate mismatched ids
          if (table > 3 || !h.tables[static_cast<std::size_t>(table)].defined) {
            return fail(Status::Decode, "lossless JPEG scan uses an undefined Huffman table");
          }
          h.table_of[static_cast<std::size_t>(index)] = table;
        }
        const std::uint8_t* tail = s + 1 + 2 * ns;
        h.frame.predictor = tail[0];
        h.point_transform = tail[2] & 15;
        if (h.frame.predictor < 1 || h.frame.predictor > 7) {
          return fail(Status::Decode, "lossless JPEG predictor {} is not supported", h.frame.predictor);
        }
        if (h.point_transform >= h.frame.precision) return fail(Status::Decode, "invalid lossless JPEG point transform");
        h.scan_data = pos + length;
        return h;
      }
      default:
        break;  // APPn, COM, DNL ... skipped
    }
    pos += length;
  }
  return fail(Status::Decode, have_frame ? "lossless JPEG stream has no scan" : "lossless JPEG stream has no SOF3 frame");
}

}  // namespace

Result<LjpegFrame> ljpeg_info(ByteView data) {
  CVT_TRY_ASSIGN(Header h, parse_header(data, true));
  return h.frame;
}

Result<LjpegFrame> decode_ljpeg(ByteView data, std::uint64_t max_samples) {
  CVT_TRY_ASSIGN(Header h, parse_header(data, false));
  LjpegFrame& frame = h.frame;
  const auto comps = static_cast<std::uint32_t>(frame.components);
  const std::uint64_t line = std::uint64_t{frame.width} * comps;
  const std::uint64_t total = line * frame.height;
  if (total > max_samples) return fail(Status::Limit, "lossless JPEG frame of {} samples is too large", total);
  // Each sample needs at least one bit unless every difference is zero with a
  // one-bit code; reject frames far larger than the data could describe.
  if (total / 64 > data.size() + 64) return fail(Status::Decode, "lossless JPEG frame is larger than its data");
  frame.samples.resize(static_cast<std::size_t>(total));

  const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
  BitReader bits(p, data.size(), h.scan_data);
  const int pt = h.point_transform;
  const std::int32_t initial = 1 << (frame.precision - pt - 1);
  const int predictor = frame.predictor;
  std::array<const Huffman*, 4> tables{};
  for (std::uint32_t c = 0; c < comps; ++c) tables[c] = &h.tables[static_cast<std::size_t>(h.table_of[c])];

  std::uint16_t* out = frame.samples.data();
  const auto stride = static_cast<std::size_t>(line);
  std::uint32_t mcus_left = h.restart_interval;
  bool first_line = true;  // first line of the image or of a restart interval
  bool reset_sample = true;
  for (std::uint32_t y = 0; y < frame.height; ++y) {
    std::uint16_t* row = out + std::size_t{y} * stride;
    const std::uint16_t* above = y > 0 ? row - stride : nullptr;
    bool restarted_in_row = false;
    for (std::uint32_t x = 0; x < frame.width; ++x) {
      if (h.restart_interval != 0) {
        if (mcus_left == 0) {
          bits.restart();
          mcus_left = h.restart_interval;
          reset_sample = true;
          first_line = true;
          restarted_in_row = x != 0;
        }
        --mcus_left;
      }
      for (std::uint32_t c = 0; c < comps; ++c) {
        const std::size_t i = std::size_t{x} * comps + c;
        std::int32_t pred = 0;
        if (reset_sample) {
          pred = initial;
        } else if (first_line || above == nullptr) {
          pred = x > 0 ? row[i - comps] : initial;
        } else if (x == 0) {
          pred = above[i];
        } else {
          const std::int32_t ra = row[i - comps];
          const std::int32_t rb = above[i];
          const std::int32_t rc = above[i - comps];
          switch (predictor) {
            case 1: pred = ra; break;
            case 2: pred = rb; break;
            case 3: pred = rc; break;
            case 4: pred = ra + rb - rc; break;
            case 5: pred = ra + ((rb - rc) >> 1); break;
            case 6: pred = rb + ((ra - rc) >> 1); break;
            default: pred = (ra + rb) >> 1; break;
          }
        }
        const int ssss = decode_symbol(bits, *tables[c]);
        if (ssss < 0 || ssss > 16) return fail(Status::Decode, "corrupt lossless JPEG data");
        const std::int32_t diff = decode_difference(bits, ssss);
        row[i] = static_cast<std::uint16_t>(static_cast<std::uint32_t>(pred + diff) & 0xFFFFu);
      }
      reset_sample = false;
    }
    if (!restarted_in_row) first_line = false;
    if (bits.overrun()) return fail(Status::Decode, "lossless JPEG data is truncated");
  }
  if (pt > 0) {
    for (std::uint16_t& v : frame.samples) v = static_cast<std::uint16_t>(v << pt);
  }
  return frame;
}

}  // namespace cvt::image_modern::raw
