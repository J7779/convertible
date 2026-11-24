// Convertible core - codec_legacy family: HuffYUV ('HFYU').
//
// Extradata (the BITMAPINFOHEADER tail): byte 0 = predictor (0 left, 1 plane,
// 2 median) | 0x40 when RGB channels are decorrelated (G subtracted from B
// and R), byte 1 = bits per pixel (16 = YUY2, 24 = RGB, 32 = RGBA), byte 2 =
// 0x10 interlaced / 0x20 progressive (| 0x40 per-frame tables), byte 3 = 0,
// then three Huffman tables (Y U V or B G R) as run-length coded code
// lengths. Codes are assigned from the longest length down. The bitstream is
// read MSB first from 32-bit little-endian words. YUY2 is decoded to I422,
// RGB to RGBA8 (stored bottom-up). Files without tables in extradata (the
// original 1.x "classic" tables) are not supported.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <queue>
#include <vector>

#include "families/codec_legacy/common/bitstream.hpp"
#include "families/codec_legacy/common/codec_base.hpp"
#include "families/codec_legacy/common/vlc.hpp"
#include "families/codec_legacy/lossless/lossless.hpp"

namespace cvt::legacy::lossless {

namespace {

constexpr int kLeft = 0;
constexpr int kPlane = 1;
constexpr int kMedian = 2;

[[nodiscard]] std::uint8_t mid_pred(int a, int b, int c) {
  return static_cast<std::uint8_t>(std::max(std::min(a, b), std::min(std::max(a, b), c)));
}

/// Reads 256 code lengths (run-length coded).
bool read_lengths(BitReader& br, std::array<std::uint8_t, 256>& lengths) {
  for (int i = 0; i < 256;) {
    int repeat = static_cast<int>(br.bits(3));
    const int value = static_cast<int>(br.bits(5));
    if (repeat == 0) repeat = static_cast<int>(br.bits(8));
    if (i + repeat > 256 || br.overrun()) return false;
    while (repeat-- > 0) lengths[static_cast<std::size_t>(i++)] = static_cast<std::uint8_t>(value);
  }
  return true;
}

void write_lengths(BitWriter& w, const std::array<std::uint8_t, 256>& lengths) {
  for (int i = 0; i < 256;) {
    int run = 1;
    while (i + run < 256 && lengths[static_cast<std::size_t>(i + run)] == lengths[static_cast<std::size_t>(i)] && run < 255) ++run;
    if (run < 8) {
      w.put(3, static_cast<std::uint32_t>(run));
      w.put(5, lengths[static_cast<std::size_t>(i)]);
    } else {
      w.put(3, 0);
      w.put(5, lengths[static_cast<std::size_t>(i)]);
      w.put(8, static_cast<std::uint32_t>(run));
    }
    i += run;
  }
}

/// Code words for code lengths: assigned from the longest length down,
/// in symbol order within one length. False when the lengths do not form
/// a complete prefix code.
bool make_codes(const std::array<std::uint8_t, 256>& lengths, std::array<std::uint32_t, 256>& codes) {
  std::uint64_t bits = 0;
  for (int length = 32; length > 0; --length) {
    for (std::size_t i = 0; i < 256; ++i) {
      if (lengths[i] == length) codes[i] = static_cast<std::uint32_t>(bits++);
    }
    if ((bits & 1) != 0) return false;
    bits >>= 1;
  }
  return true;
}

/// Huffman code lengths (limited to 24 bits) for 256 symbol counts.
std::array<std::uint8_t, 256> huffman_lengths(const std::array<std::uint64_t, 256>& counts) {
  std::array<std::uint64_t, 256> weight = counts;
  for (int attempt = 0; attempt < 32; ++attempt) {
    struct Node {
      std::uint64_t weight;
      int index;
    };
    std::vector<int> parent(512, -1);
    const auto cmp = [](const Node& a, const Node& b) { return a.weight > b.weight || (a.weight == b.weight && a.index > b.index); };
    std::priority_queue<Node, std::vector<Node>, decltype(cmp)> queue(cmp);
    for (int i = 0; i < 256; ++i) queue.push({std::max<std::uint64_t>(1, weight[static_cast<std::size_t>(i)]), i});
    int next = 256;
    while (queue.size() > 1) {
      const Node a = queue.top();
      queue.pop();
      const Node b = queue.top();
      queue.pop();
      parent[static_cast<std::size_t>(a.index)] = next;
      parent[static_cast<std::size_t>(b.index)] = next;
      queue.push({a.weight + b.weight, next++});
    }
    std::array<std::uint8_t, 256> lengths{};
    int longest = 0;
    for (int i = 0; i < 256; ++i) {
      int depth = 0;
      for (int n = i; parent[static_cast<std::size_t>(n)] >= 0; n = parent[static_cast<std::size_t>(n)]) ++depth;
      lengths[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(depth);
      longest = std::max(longest, depth);
    }
    if (longest <= 24) return lengths;
    for (auto& v : weight) v = (v + 1) / 2 + 1;  // flatten and retry
  }
  std::array<std::uint8_t, 256> flat{};
  flat.fill(8);
  return flat;
}

struct Tables {
  std::array<std::array<std::uint8_t, 256>, 3> lengths{};
  std::array<Vlc, 3> vlc;
};

bool build_vlc(Tables& t) {
  for (std::size_t k = 0; k < 3; ++k) {
    std::array<std::uint32_t, 256> codes{};
    if (!make_codes(t.lengths[k], codes)) return false;
    std::vector<VlcCode> list;
    for (std::size_t i = 0; i < 256; ++i) {
      if (t.lengths[k][i] == 0) continue;
      list.push_back({codes[i], t.lengths[k][i], static_cast<std::int32_t>(i)});
    }
    if (!t.vlc[k].build(list, 11)) return false;
  }
  return true;
}

/// 32-bit word byte swap of a packet into a readable buffer.
std::vector<std::uint8_t> swap_words(ByteView data) {
  std::vector<std::uint8_t> out((data.size() + 3) / 4 * 4, 0);
  const auto in = as_u8(data);
  std::copy(in.begin(), in.end(), out.begin());
  for (std::size_t i = 0; i < out.size(); i += 4) {
    std::swap(out[i], out[i + 3]);
    std::swap(out[i + 1], out[i + 2]);
  }
  return out;
}

class HuffyuvDecoder final : public SimpleDecoder {
 protected:
  Result<void> on_configure() override {
    if (params_.width == 0 || params_.height == 0) return fail(Status::Decode, "HuffYUV needs the picture size");
    const auto e = as_u8(ByteView(params_.extradata));
    if (e.size() < 4) return fail(Status::Decode, "HuffYUV without Huffman tables (version 1.x) is not supported");
    predictor_ = e[0] & 0x3F;
    decorrelate_ = (e[0] & 0x40) != 0;
    bpp_ = e[1] == 0 ? (params_.bits_per_sample != 0 ? params_.bits_per_sample : 16) : e[1];
    interlaced_ = (e[2] & 0x10) != 0;
    per_frame_tables_ = (e[2] & 0x40) != 0;
    if (bpp_ != 16 && bpp_ != 24 && bpp_ != 32) return fail(Status::Decode, "unsupported HuffYUV format ({} bits)", bpp_);
    if (predictor_ > kMedian) return fail(Status::Decode, "unknown HuffYUV predictor {}", predictor_);
    if (bpp_ != 16 && predictor_ == kMedian) return fail(Status::Decode, "HuffYUV RGB with median prediction is not defined");
    if (bpp_ == 16 && (params_.width & 1) != 0) return fail(Status::Decode, "HuffYUV 4:2:2 needs an even width");
    BitReader br(e.subspan(4));
    for (auto& l : tables_.lengths) {
      if (!read_lengths(br, l)) return fail(Status::Decode, "damaged HuffYUV tables");
    }
    if (!build_vlc(tables_)) return fail(Status::Decode, "invalid HuffYUV tables");
    return {};
  }

  Result<VideoFrame> decode(const Packet& packet) override {
    const std::vector<std::uint8_t> swapped = swap_words(packet.bytes);
    BitReader br{std::span<const std::uint8_t>(swapped)};
    if (per_frame_tables_) {
      for (auto& l : tables_.lengths) {
        if (!read_lengths(br, l)) return fail(Status::Decode, "damaged HuffYUV tables");
      }
      if (!build_vlc(tables_)) return fail(Status::Decode, "invalid HuffYUV tables");
    }
    if (bpp_ == 16) return decode_yuv(br);
    return decode_rgb(br);
  }

 private:
  Result<VideoFrame> decode_yuv(BitReader& br) {
    const std::uint32_t w = params_.width;
    const std::uint32_t h = params_.height;
    VideoFrame f = VideoFrame::allocate(PixelFormat::I422, w, h);
    f.color = {h >= 720 ? ir::ColorMatrix::BT709 : ir::ColorMatrix::BT601, ir::ColorRange::Limited, ir::ChromaSiting::Left};
    std::uint8_t* py = f.planes[0].data.data();
    std::uint8_t* pu = f.planes[1].data.data();
    std::uint8_t* pv = f.planes[2].data.data();
    const std::size_t sy = f.planes[0].stride;
    const std::size_t sc = f.planes[1].stride;
    const std::uint32_t w2 = w / 2;
    std::vector<std::uint8_t> dy(w);
    std::vector<std::uint8_t> du(w2);
    std::vector<std::uint8_t> dv(w2);
    // Residuals of `count` luma samples (and count / 2 of each chroma).
    const auto read = [&](std::uint32_t count) {
      for (std::uint32_t i = 0; i < count / 2; ++i) {
        dy[2 * i] = static_cast<std::uint8_t>(tables_.vlc[0].decode(br));
        du[i] = static_cast<std::uint8_t>(tables_.vlc[1].decode(br));
        dy[2 * i + 1] = static_cast<std::uint8_t>(tables_.vlc[0].decode(br));
        dv[i] = static_cast<std::uint8_t>(tables_.vlc[2].decode(br));
      }
    };
    const auto left = [](std::uint8_t* dst, const std::uint8_t* diff, std::uint32_t n, int acc) {
      for (std::uint32_t i = 0; i < n; ++i) dst[i] = static_cast<std::uint8_t>(acc = (acc + diff[i]) & 0xFF);
      return acc;
    };
    const auto median = [](std::uint8_t* dst, const std::uint8_t* top, const std::uint8_t* diff, std::uint32_t n, int& l, int& lt) {
      for (std::uint32_t i = 0; i < n; ++i) {
        const int pred = mid_pred(l, top[i], (l + top[i] - lt) & 0xFF);
        lt = top[i];
        l = dst[i] = static_cast<std::uint8_t>((pred + diff[i]) & 0xFF);
      }
    };
    // First pixel pair raw (V Y1 U Y0 after the word swap).
    int lv = pv[0] = static_cast<std::uint8_t>(br.bits(8));
    int ly = py[1] = static_cast<std::uint8_t>(br.bits(8));
    int lu = pu[0] = static_cast<std::uint8_t>(br.bits(8));
    py[0] = static_cast<std::uint8_t>(br.bits(8));
    read(w - 2);
    ly = left(py + 2, dy.data(), w - 2, ly);
    lu = left(pu + 1, du.data(), w2 - 1, lu);
    lv = left(pv + 1, dv.data(), w2 - 1, lv);
    const std::size_t fake_y = interlaced_ ? 2 * sy : sy;
    const std::size_t fake_c = interlaced_ ? 2 * sc : sc;
    std::uint32_t y = 1;
    if (predictor_ != kMedian) {
      for (; y < h; ++y) {
        read(w);
        std::uint8_t* ry = py + y * sy;
        std::uint8_t* ru = pu + y * sc;
        std::uint8_t* rv = pv + y * sc;
        ly = left(ry, dy.data(), w, ly);
        lu = left(ru, du.data(), w2, lu);
        lv = left(rv, dv.data(), w2, lv);
        if (predictor_ == kPlane && y > (interlaced_ ? 1u : 0u)) {
          for (std::uint32_t x = 0; x < w; ++x) ry[x] = static_cast<std::uint8_t>(ry[x] + ry[x - fake_y]);
          for (std::uint32_t x = 0; x < w2; ++x) {
            ru[x] = static_cast<std::uint8_t>(ru[x] + ru[x - fake_c]);
            rv[x] = static_cast<std::uint8_t>(rv[x] + rv[x - fake_c]);
          }
        }
        if (br.overrun()) return fail(Status::Decode, "truncated HuffYUV frame");
      }
      return f;
    }
    // Median: the second line of an interlaced picture is left predicted.
    if (interlaced_ && h > 1) {
      read(w);
      ly = left(py + sy, dy.data(), w, ly);
      lu = left(pu + sc, du.data(), w2, lu);
      lv = left(pv + sc, dv.data(), w2, lv);
      ++y;
    }
    if (y >= h) return f;
    // The next 4 pixels are left predicted, the rest of that line median.
    read(4);
    ly = left(py + fake_y, dy.data(), 4, ly);
    lu = left(pu + fake_c, du.data(), 2, lu);
    lv = left(pv + fake_c, dv.data(), 2, lv);
    int lty = py[3];
    int ltu = pu[1];
    int ltv = pv[1];
    read(w - 4);
    median(py + fake_y + 4, py + 4, dy.data(), w - 4, ly, lty);
    median(pu + fake_c + 2, pu + 2, du.data(), w2 - 2, lu, ltu);
    median(pv + fake_c + 2, pv + 2, dv.data(), w2 - 2, lv, ltv);
    for (++y; y < h; ++y) {
      read(w);
      std::uint8_t* ry = py + y * sy;
      median(ry, ry - fake_y, dy.data(), w, ly, lty);
      median(pu + y * sc, pu + y * sc - fake_c, du.data(), w2, lu, ltu);
      median(pv + y * sc, pv + y * sc - fake_c, dv.data(), w2, lv, ltv);
      if (br.overrun()) return fail(Status::Decode, "truncated HuffYUV frame");
    }
    return f;
  }

  Result<VideoFrame> decode_rgb(BitReader& br) {
    const std::uint32_t w = params_.width;
    const std::uint32_t h = params_.height;
    const bool alpha = bpp_ == 32;
    VideoFrame f = VideoFrame::allocate(PixelFormat::RGBA8, w, h);
    const std::size_t stride = f.planes[0].stride;
    std::uint8_t* base = f.planes[0].data.data();
    std::vector<std::array<std::uint8_t, 4>> diff(w);  // r g b a residuals
    const auto read = [&](std::uint32_t count) {
      for (std::uint32_t i = 0; i < count; ++i) {
        auto& d = diff[i];
        if (decorrelate_) {
          const int g = tables_.vlc[1].decode(br);
          d[1] = static_cast<std::uint8_t>(g);
          d[2] = static_cast<std::uint8_t>(tables_.vlc[0].decode(br) + g);
          d[0] = static_cast<std::uint8_t>(tables_.vlc[2].decode(br) + g);
        } else {
          d[2] = static_cast<std::uint8_t>(tables_.vlc[0].decode(br));
          d[1] = static_cast<std::uint8_t>(tables_.vlc[1].decode(br));
          d[0] = static_cast<std::uint8_t>(tables_.vlc[2].decode(br));
        }
        d[3] = alpha ? static_cast<std::uint8_t>(tables_.vlc[2].decode(br)) : 0;
      }
    };
    std::array<int, 4> acc{};
    std::uint8_t* last = base + static_cast<std::size_t>(h - 1) * stride;
    if (alpha) acc[3] = last[3] = static_cast<std::uint8_t>(br.bits(8));
    acc[0] = last[0] = static_cast<std::uint8_t>(br.bits(8));
    acc[1] = last[1] = static_cast<std::uint8_t>(br.bits(8));
    acc[2] = last[2] = static_cast<std::uint8_t>(br.bits(8));
    if (!alpha) {
      last[3] = 255;
      br.skip(8);
    }
    const auto left = [&](std::uint8_t* row, std::uint32_t n) {
      for (std::uint32_t i = 0; i < n; ++i) {
        for (std::size_t c = 0; c < 4; ++c) {
          acc[c] = (acc[c] + diff[i][c]) & 0xFF;
          row[i * 4 + c] = static_cast<std::uint8_t>(acc[c]);
        }
        if (!alpha) row[i * 4 + 3] = 255;
      }
    };
    read(w - 1);
    left(last + 4, w - 1);
    const std::uint32_t lap = interlaced_ ? 2 : 1;
    for (std::int64_t y = static_cast<std::int64_t>(h) - 2; y >= 0; --y) {
      read(w);
      std::uint8_t* row = base + static_cast<std::size_t>(y) * stride;
      left(row, w);
      if (predictor_ == kPlane) {
        if (!alpha) acc[3] = 0;
        const bool odd_row = interlaced_ && (y & 1) != 0;
        if (!odd_row && y + lap < static_cast<std::int64_t>(h)) {
          const std::uint8_t* below = row + lap * stride;
          for (std::uint32_t i = 0; i < w * 4; ++i) {
            if (!alpha && (i & 3) == 3) continue;
            row[i] = static_cast<std::uint8_t>(row[i] + below[i]);
          }
        }
      }
      if (br.overrun()) return fail(Status::Decode, "truncated HuffYUV frame");
    }
    return f;
  }

  int predictor_ = kLeft;
  bool decorrelate_ = false;
  int bpp_ = 16;
  bool interlaced_ = false;
  bool per_frame_tables_ = false;
  Tables tables_;
};

/// HuffYUV encoder: 4:2:2 (YUY2) with median prediction, or RGB/RGBA with
/// decorrelation and plane prediction when the configured pixel format is
/// RGBA8 (alpha kept). Lossless; I420 input is upsampled to 4:2:2.
class HuffyuvEncoder final : public SimpleEncoder {
 protected:
  Result<void> on_configure() override {
    rgb_ = params_.pixel_format == PixelFormat::RGBA8;
    if (!rgb_ && (params_.width & 1) != 0) return fail(Status::InvalidArgument, "HuffYUV 4:2:2 needs an even width");
    if (!rgb_ && params_.width < 4) return fail(Status::InvalidArgument, "HuffYUV needs a width of at least 4");
    predictor_ = rgb_ ? kPlane : (params_.preset == EncodePreset::Fast ? kLeft : kMedian);
    interlaced_ = false;
    // Fixed tables tuned for small residuals (as every HuffYUV writer does).
    std::array<std::uint64_t, 256> counts{};
    for (int j = 0; j < 256; ++j) {
      const std::uint64_t d = static_cast<std::uint64_t>(std::min(j, 256 - j));
      counts[static_cast<std::size_t>(j)] = 100000000ull / (d * d + 1);
    }
    for (auto& l : tables_.lengths) l = huffman_lengths(counts);
    for (std::size_t k = 0; k < 3; ++k) make_codes(tables_.lengths[k], codes_[k]);
    BitWriter w;
    w.put(8, static_cast<std::uint32_t>(predictor_ | (rgb_ ? 0x40 : 0)));
    w.put(8, rgb_ ? 32 : 16);
    w.put(8, 0x20);
    w.put(8, 0);
    for (const auto& l : tables_.lengths) write_lengths(w, l);
    extradata_ = w.take();
    return {};
  }

  Bytes extradata() const override { return to_cvt_bytes(extradata_); }

  Result<Bytes> encode(const VideoFrame& frame, bool& keyframe) override {
    keyframe = true;
    BitWriter w;
    if (rgb_) {
      CVT_TRY_ASSIGN(const VideoFrame f, to_format(frame, PixelFormat::RGBA8, {}));
      encode_rgb(w, f);
    } else {
      ColorInfo color = params_.color;
      color.siting = ir::ChromaSiting::Left;
      CVT_TRY_ASSIGN(const VideoFrame f, to_format(frame, PixelFormat::I422, color));
      encode_yuv(w, f);
    }
    // Pad to whole 32-bit words and swap them into file order.
    while (w.bit_count() % 32 != 0) w.put(1, 0);
    std::vector<std::uint8_t> bytes = w.take();
    for (std::size_t i = 0; i + 3 < bytes.size(); i += 4) {
      std::swap(bytes[i], bytes[i + 3]);
      std::swap(bytes[i + 1], bytes[i + 2]);
    }
    return to_cvt_bytes(bytes);
  }

 private:
  void put(BitWriter& w, std::size_t table, std::uint8_t symbol) {
    w.put(tables_.lengths[table][symbol], codes_[table][symbol]);
  }

  void encode_yuv(BitWriter& w, const VideoFrame& f) {
    const std::uint32_t width = params_.width;
    const std::uint32_t h = params_.height;
    const std::uint32_t w2 = width / 2;
    const std::uint8_t* py = f.planes[0].data.data();
    const std::uint8_t* pu = f.planes[1].data.data();
    const std::uint8_t* pv = f.planes[2].data.data();
    const std::size_t sy = f.planes[0].stride;
    const std::size_t sc = f.planes[1].stride;
    std::vector<std::uint8_t> ry(width);
    std::vector<std::uint8_t> ru(w2);
    std::vector<std::uint8_t> rv(w2);
    const auto emit = [&](std::uint32_t count) {
      for (std::uint32_t i = 0; i < count / 2; ++i) {
        put(w, 0, ry[2 * i]);
        put(w, 1, ru[i]);
        put(w, 0, ry[2 * i + 1]);
        put(w, 2, rv[i]);
      }
    };
    const auto left = [](std::uint8_t* out, const std::uint8_t* src, std::uint32_t n, int prev) {
      for (std::uint32_t i = 0; i < n; ++i) {
        out[i] = static_cast<std::uint8_t>(src[i] - prev);
        prev = src[i];
      }
      return prev;
    };
    const auto median = [](std::uint8_t* out, const std::uint8_t* src, const std::uint8_t* top, std::uint32_t n, int& l, int& lt) {
      for (std::uint32_t i = 0; i < n; ++i) {
        const int pred = mid_pred(l, top[i], (l + top[i] - lt) & 0xFF);
        out[i] = static_cast<std::uint8_t>(src[i] - pred);
        lt = top[i];
        l = src[i];
      }
    };
    w.put(8, pv[0]);
    w.put(8, py[1]);
    w.put(8, pu[0]);
    w.put(8, py[0]);
    int ly = left(ry.data(), py + 2, width - 2, py[1]);
    int lu = left(ru.data(), pu + 1, w2 - 1, pu[0]);
    int lv = left(rv.data(), pv + 1, w2 - 1, pv[0]);
    emit(width - 2);
    if (h < 2) return;
    // Median: 4 left-predicted pixels, then median for the rest.
    ly = left(ry.data(), py + sy, 4, ly);
    lu = left(ru.data(), pu + sc, 2, lu);
    lv = left(rv.data(), pv + sc, 2, lv);
    if (predictor_ == kMedian) {
      emit(4);
      int lty = py[3];
      int ltu = pu[1];
      int ltv = pv[1];
      median(ry.data(), py + sy + 4, py + 4, width - 4, ly, lty);
      median(ru.data(), pu + sc + 2, pu + 2, w2 - 2, lu, ltu);
      median(rv.data(), pv + sc + 2, pv + 2, w2 - 2, lv, ltv);
      emit(width - 4);
      for (std::uint32_t y = 2; y < h; ++y) {
        median(ry.data(), py + y * sy, py + (y - 1) * sy, width, ly, lty);
        median(ru.data(), pu + y * sc, pu + (y - 1) * sc, w2, lu, ltu);
        median(rv.data(), pv + y * sc, pv + (y - 1) * sc, w2, lv, ltv);
        emit(width);
      }
      return;
    }
    // Left prediction for every following line (continuing the accumulator).
    int acc_y = py[width - 1];
    int acc_u = pu[w2 - 1];
    int acc_v = pv[w2 - 1];
    for (std::uint32_t y = 1; y < h; ++y) {
      acc_y = left(ry.data(), py + y * sy, width, acc_y);
      acc_u = left(ru.data(), pu + y * sc, w2, acc_u);
      acc_v = left(rv.data(), pv + y * sc, w2, acc_v);
      emit(width);
    }
  }

  void encode_rgb(BitWriter& w, const VideoFrame& f) {
    const std::uint32_t width = params_.width;
    const std::uint32_t h = params_.height;
    const std::size_t stride = f.planes[0].stride;
    const std::uint8_t* base = f.planes[0].data.data();
    std::vector<std::uint8_t> work(static_cast<std::size_t>(width) * 4);
    std::array<int, 4> prev{};
    const auto emit_row = [&](const std::uint8_t* row, std::uint32_t from, std::uint32_t n, const std::uint8_t* below) {
      for (std::uint32_t i = from; i < from + n; ++i) {
        std::array<std::uint8_t, 4> v{};
        for (std::size_t c = 0; c < 4; ++c) {
          const int value = below != nullptr ? (row[i * 4 + c] - below[i * 4 + c]) & 0xFF : row[i * 4 + c];
          v[c] = static_cast<std::uint8_t>(value - prev[c]);
          prev[c] = value;
        }
        // Residuals: G, then B - G, R - G, then A.
        put(w, 1, v[1]);
        put(w, 0, static_cast<std::uint8_t>(v[2] - v[1]));
        put(w, 2, static_cast<std::uint8_t>(v[0] - v[1]));
        put(w, 2, v[3]);
      }
    };
    const std::uint8_t* last = base + static_cast<std::size_t>(h - 1) * stride;
    w.put(8, last[3]);
    w.put(8, last[0]);
    w.put(8, last[1]);
    w.put(8, last[2]);
    prev = {last[0], last[1], last[2], last[3]};
    emit_row(last, 1, width - 1, nullptr);
    for (std::int64_t y = static_cast<std::int64_t>(h) - 2; y >= 0; --y) {
      const std::uint8_t* row = base + static_cast<std::size_t>(y) * stride;
      // Plane prediction: left prediction of (row - row below); the
      // accumulators continue across rows exactly as the decoder's do.
      emit_row(row, 0, width, row + stride);
    }
  }

  bool rgb_ = false;
  int predictor_ = kMedian;
  bool interlaced_ = false;
  Tables tables_;
  std::array<std::array<std::uint32_t, 256>, 3> codes_{};
  std::vector<std::uint8_t> extradata_;
};

}  // namespace

std::unique_ptr<codec::VideoDecoder> make_huffyuv_decoder() { return std::make_unique<HuffyuvDecoder>(); }
std::unique_ptr<codec::VideoEncoder> make_huffyuv_encoder() { return std::make_unique<HuffyuvEncoder>(); }

}  // namespace cvt::legacy::lossless
