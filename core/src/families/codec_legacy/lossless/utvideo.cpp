// Convertible core - codec_legacy family: Ut Video decoder.
//
// Formats: ULY0 (4:2:0), ULY2 (4:2:2), ULY4 (4:4:4), ULH0/ULH2/ULH4 (the
// same with BT.709), ULRG (RGB), ULRA (RGBA). The container passes the
// fourcc's colour layout through `bits_per_sample` (24 RGB, 32 RGBA, 12
// 4:2:0, 16 4:2:2, 48 4:4:4 as in the BITMAPINFOHEADER) and the 16-byte
// codec private data (encoder version, original format, frame info size,
// flags with the slice count in the top byte) as extradata.
//
// Each plane: 256 code lengths (0 = the only symbol, 255 = unused), slice
// end offsets, then the slices; codes are canonical with longer codes and,
// within one length, higher symbols first. Bitstream read MSB first from
// 32-bit little-endian words. Prediction (frame info after the planes):
// none, left (running over each slice), gradient or median.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "families/codec_legacy/common/bitstream.hpp"
#include "families/codec_legacy/common/codec_base.hpp"
#include "families/codec_legacy/common/vlc.hpp"
#include "families/codec_legacy/lossless/lossless.hpp"

namespace cvt::legacy::lossless {

namespace {

[[nodiscard]] std::uint8_t mid(int a, int b, int c) {
  return static_cast<std::uint8_t>(std::max(std::min(a, b), std::min(std::max(a, b), c)));
}

class UtvideoDecoder final : public SimpleDecoder {
 protected:
  Result<void> on_configure() override {
    if (params_.width == 0 || params_.height == 0) return fail(Status::Decode, "Ut Video needs the picture size");
    const auto e = as_u8(ByteView(params_.extradata));
    if (e.size() < 16) return fail(Status::Decode, "Ut Video needs its 16-byte codec private data");
    const std::uint32_t flags = static_cast<std::uint32_t>(e[12]) | static_cast<std::uint32_t>(e[13]) << 8 |
                                static_cast<std::uint32_t>(e[14]) << 16 | static_cast<std::uint32_t>(e[15]) << 24;
    slices_ = static_cast<int>(flags >> 24) + 1;
    if ((flags & 0x800) != 0) return fail(Status::Decode, "interlaced Ut Video is not supported");
    if ((flags & 1) == 0) return fail(Status::Decode, "uncompressed Ut Video frames are not supported");
    // The layout from the original format recorded by the encoder, unless the container says otherwise.
    const std::string_view original(reinterpret_cast<const char*>(e.data() + 4), 4);
    int bpp = params_.bits_per_sample;
    if (bpp == 0) {
      if (original == "YV12" || original == "NV12" || original == "I420") bpp = 12;
      else if (original == "YUY2" || original == "UYVY" || original == "YV16") bpp = 16;
      else if (original == "YV24") bpp = 48;
      else if (e[4] == 0 && e[5] == 0 && e[6] == 2 && e[7] == 0x18) bpp = 24;
      else if (e[4] == 0 && e[5] == 0 && e[6] == 2 && e[7] == 0x20) bpp = 32;
      else bpp = 24;
    }
    switch (bpp) {
      case 12: format_ = PixelFormat::I420; planes_ = 3; break;
      case 16: format_ = PixelFormat::I422; planes_ = 3; break;
      case 48: format_ = PixelFormat::I444; planes_ = 3; break;
      case 24: format_ = PixelFormat::RGBA8; planes_ = 3; break;
      case 32: format_ = PixelFormat::RGBA8; planes_ = 4; break;
      default: return fail(Status::Decode, "unsupported Ut Video layout ({} bits)", bpp);
    }
    if (format_ != PixelFormat::RGBA8 && ((params_.width & 1) != 0 || (format_ == PixelFormat::I420 && (params_.height & 1) != 0))) {
      return fail(Status::Decode, "Ut Video with odd dimensions is not supported");
    }
    return {};
  }

  Result<VideoFrame> decode(const Packet& packet) override {
    const auto data = as_u8(ByteView(packet.bytes));
    const int w = static_cast<int>(params_.width);
    const int h = static_cast<int>(params_.height);
    if (format_ == PixelFormat::RGBA8 && params_.bits_per_sample == 0) {
      // ULRG and ULRA share the codec private data: count the planes.
      std::size_t probe = 0;
      int count = 0;
      while (count < 4 && probe + 256 + static_cast<std::size_t>(slices_) * 4 + 4 <= data.size()) {
        const std::size_t last = probe + 256 + static_cast<std::size_t>(slices_ - 1) * 4;
        const std::uint32_t total = static_cast<std::uint32_t>(data[last]) | static_cast<std::uint32_t>(data[last + 1]) << 8 |
                                    static_cast<std::uint32_t>(data[last + 2]) << 16 | static_cast<std::uint32_t>(data[last + 3]) << 24;
        probe = last + 4 + total;
        ++count;
        if (probe + 4 == data.size()) break;
      }
      planes_ = count == 4 ? 4 : 3;
    }
    // Planes as decoded (residuals, then restored in place).
    std::array<std::vector<std::uint8_t>, 4> planes;
    std::array<int, 4> pw{};
    std::array<int, 4> ph{};
    for (int p = 0; p < planes_; ++p) {
      const bool chroma = format_ != PixelFormat::RGBA8 && p > 0;
      pw[static_cast<std::size_t>(p)] = chroma ? (format_ == PixelFormat::I444 ? w : w / 2) : w;
      ph[static_cast<std::size_t>(p)] = chroma && format_ == PixelFormat::I420 ? h / 2 : h;
      planes[static_cast<std::size_t>(p)].assign(static_cast<std::size_t>(pw[static_cast<std::size_t>(p)]) * static_cast<std::size_t>(ph[static_cast<std::size_t>(p)]), 0);
    }
    // The frame info follows the planes: find the prediction mode first.
    std::size_t at = 0;
    std::array<std::size_t, 4> plane_start{};
    for (int p = 0; p < planes_; ++p) {
      plane_start[static_cast<std::size_t>(p)] = at;
      if (at + 256 + static_cast<std::size_t>(slices_) * 4 > data.size()) return fail(Status::Decode, "truncated Ut Video frame");
      const std::size_t offsets = at + 256;
      const std::size_t last = offsets + static_cast<std::size_t>(slices_ - 1) * 4;
      const std::uint32_t total = static_cast<std::uint32_t>(data[last]) | static_cast<std::uint32_t>(data[last + 1]) << 8 |
                                  static_cast<std::uint32_t>(data[last + 2]) << 16 | static_cast<std::uint32_t>(data[last + 3]) << 24;
      at = offsets + static_cast<std::size_t>(slices_) * 4;
      if (total > data.size() - at) return fail(Status::Decode, "truncated Ut Video frame");
      at += total;
    }
    if (at + 4 > data.size()) return fail(Status::Decode, "Ut Video frame info is missing");
    const int prediction = (data[at + 1]) & 3;
    for (int p = 0; p < planes_; ++p) {
      const bool luma420 = format_ == PixelFormat::I420 && p == 0;
      CVT_TRY(decode_plane(data, plane_start[static_cast<std::size_t>(p)], planes[static_cast<std::size_t>(p)],
                           pw[static_cast<std::size_t>(p)], ph[static_cast<std::size_t>(p)], prediction == 1, luma420));
      if (prediction == 2) restore_gradient(planes[static_cast<std::size_t>(p)], pw[static_cast<std::size_t>(p)], ph[static_cast<std::size_t>(p)], luma420);
      if (prediction == 3) restore_median(planes[static_cast<std::size_t>(p)], pw[static_cast<std::size_t>(p)], ph[static_cast<std::size_t>(p)], luma420);
    }
    VideoFrame f = VideoFrame::allocate(format_, params_.width, params_.height);
    if (format_ == PixelFormat::RGBA8) {
      // Coded planes: G, B - G + 0x80, R - G + 0x80 (, A).
      for (int y = 0; y < h; ++y) {
        std::uint8_t* row = f.planes[0].data.data() + static_cast<std::size_t>(y) * f.planes[0].stride;
        for (int x = 0; x < w; ++x) {
          const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
          const int g = planes[0][i];
          row[x * 4] = static_cast<std::uint8_t>(planes[2][i] + g - 0x80);
          row[x * 4 + 1] = static_cast<std::uint8_t>(g);
          row[x * 4 + 2] = static_cast<std::uint8_t>(planes[1][i] + g - 0x80);
          row[x * 4 + 3] = planes_ == 4 ? planes[3][i] : 255;
        }
      }
    } else {
      for (int p = 0; p < 3; ++p) {
        auto& out = f.planes[static_cast<std::size_t>(p)];
        for (int y = 0; y < ph[static_cast<std::size_t>(p)]; ++y) {
          std::memcpy(out.data.data() + static_cast<std::size_t>(y) * out.stride,
                      planes[static_cast<std::size_t>(p)].data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(pw[static_cast<std::size_t>(p)]),
                      static_cast<std::size_t>(pw[static_cast<std::size_t>(p)]));
        }
      }
      f.color = {ir::ColorMatrix::BT601, ir::ColorRange::Limited, ir::ChromaSiting::Center};
    }
    return f;
  }

 private:
  /// First row of slice `s` (rows aligned to 2 for 4:2:0 luma).
  [[nodiscard]] int slice_row(int s, int h, bool even) const {
    const int row = h * s / slices_;
    return even ? row & ~1 : row;
  }

  Result<void> decode_plane(std::span<const std::uint8_t> data, std::size_t at, std::vector<std::uint8_t>& plane, int w, int h,
                            bool left, bool even_rows) {
    const auto lengths = data.subspan(at, 256);
    int only = -1;
    std::vector<VlcCode> codes;
    {
      // Canonical codes: longer first, within a length the higher symbol first.
      std::vector<std::pair<int, int>> entries;  // (length, symbol)
      for (int s = 0; s < 256; ++s) {
        const int len = lengths[static_cast<std::size_t>(s)];
        if (len == 0) {
          only = s;
          break;
        }
        if (len == 255) continue;
        if (len > 32) return fail(Status::Decode, "invalid Ut Video code length");
        entries.emplace_back(len, s);
      }
      if (only < 0) {
        if (entries.empty()) return fail(Status::Decode, "empty Ut Video code table");
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
          return a.first != b.first ? a.first > b.first : a.second > b.second;
        });
        std::uint64_t code = 0;  // left-aligned in 32 bits
        for (const auto& [len, sym] : entries) {
          if (code >> 32 != 0) return fail(Status::Decode, "invalid Ut Video code table");
          codes.push_back({static_cast<std::uint32_t>(code >> (32 - len)), static_cast<std::uint8_t>(len), sym});
          code += std::uint64_t{1} << (32 - len);
        }
      }
    }
    Vlc vlc;
    if (only < 0 && !vlc.build(codes, 11)) return fail(Status::Decode, "invalid Ut Video code table");
    const std::size_t offsets = at + 256;
    const std::size_t base = offsets + static_cast<std::size_t>(slices_) * 4;
    std::uint32_t start = 0;
    for (int s = 0; s < slices_; ++s) {
      const std::size_t o = offsets + static_cast<std::size_t>(s) * 4;
      const std::uint32_t end = static_cast<std::uint32_t>(data[o]) | static_cast<std::uint32_t>(data[o + 1]) << 8 |
                                static_cast<std::uint32_t>(data[o + 2]) << 16 | static_cast<std::uint32_t>(data[o + 3]) << 24;
      if (end < start || base + end > data.size()) return fail(Status::Decode, "damaged Ut Video slice offsets");
      const int y0 = slice_row(s, h, even_rows);
      const int y1 = slice_row(s + 1, h, even_rows);
      if (only >= 0) {
        for (int y = y0; y < y1; ++y) std::fill_n(plane.begin() + static_cast<std::ptrdiff_t>(y) * w, w, static_cast<std::uint8_t>(only));
      } else {
        // Swap 32-bit words into reading order.
        std::vector<std::uint8_t> bits((end - start + 3) / 4 * 4, 0);
        std::copy(data.begin() + static_cast<std::ptrdiff_t>(base + start), data.begin() + static_cast<std::ptrdiff_t>(base + end), bits.begin());
        for (std::size_t i = 0; i < bits.size(); i += 4) {
          std::swap(bits[i], bits[i + 3]);
          std::swap(bits[i + 1], bits[i + 2]);
        }
        BitReader br{std::span<const std::uint8_t>(bits)};
        int prev = 0x80;
        for (int y = y0; y < y1; ++y) {
          std::uint8_t* row = plane.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
          for (int x = 0; x < w; ++x) {
            int v = vlc.decode(br);
            if (v < 0) return fail(Status::Decode, "damaged Ut Video slice");
            if (left) {
              prev = (prev + v) & 0xFF;
              v = prev;
            }
            row[x] = static_cast<std::uint8_t>(v);
          }
          if (br.overrun()) return fail(Status::Decode, "truncated Ut Video slice");
        }
      }
      start = end;
    }
    if (only >= 0 && left) {
      // A single repeated residual still has to be integrated per slice.
      for (int s = 0; s < slices_; ++s) {
        int prev = 0x80;
        for (int y = slice_row(s, h, even_rows); y < slice_row(s + 1, h, even_rows); ++y) {
          for (int x = 0; x < w; ++x) {
            std::uint8_t& p = plane[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)];
            prev = (prev + p) & 0xFF;
            p = static_cast<std::uint8_t>(prev);
          }
        }
      }
    }
    return {};
  }

  void restore_median(std::vector<std::uint8_t>& plane, int w, int h, bool even_rows) const {
    for (int s = 0; s < slices_; ++s) {
      const int y0 = slice_row(s, h, even_rows);
      const int y1 = slice_row(s + 1, h, even_rows);
      if (y1 <= y0) continue;
      std::uint8_t* row = plane.data() + static_cast<std::size_t>(y0) * static_cast<std::size_t>(w);
      // First line: left prediction from 0x80.
      int acc = 0x80;
      for (int x = 0; x < w; ++x) row[x] = static_cast<std::uint8_t>(acc = (acc + row[x]) & 0xFF);
      if (y1 - y0 <= 1) continue;
      // Following lines: continuous median prediction; the first sample of
      // the second line is predicted from above.
      std::uint8_t* cur = row + w;
      int c = cur[-w];
      cur[0] = static_cast<std::uint8_t>(cur[0] + c);
      int a = cur[0];
      for (int x = 1; x < w; ++x) {
        const int b = cur[x - w];
        cur[x] = static_cast<std::uint8_t>(cur[x] + mid(a, b, (a + b - c) & 0xFF));
        c = b;
        a = cur[x];
      }
      for (int y = y0 + 2; y < y1; ++y) {
        cur += w;
        for (int x = 0; x < w; ++x) {
          const int b = cur[x - w];
          cur[x] = static_cast<std::uint8_t>(cur[x] + mid(a, b, (a + b - c) & 0xFF));
          c = b;
          a = cur[x];
        }
      }
    }
  }

  void restore_gradient(std::vector<std::uint8_t>& plane, int w, int h, bool even_rows) const {
    for (int s = 0; s < slices_; ++s) {
      const int y0 = slice_row(s, h, even_rows);
      const int y1 = slice_row(s + 1, h, even_rows);
      if (y1 <= y0) continue;
      std::uint8_t* row = plane.data() + static_cast<std::size_t>(y0) * static_cast<std::size_t>(w);
      int acc = 0x80;
      for (int x = 0; x < w; ++x) row[x] = static_cast<std::uint8_t>(acc = (acc + row[x]) & 0xFF);
      for (int y = y0 + 1; y < y1; ++y) {
        std::uint8_t* cur = plane.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
        cur[0] = static_cast<std::uint8_t>(cur[0] + cur[-w]);
        for (int x = 1; x < w; ++x) cur[x] = static_cast<std::uint8_t>(cur[x - w] - cur[x - w - 1] + cur[x - 1] + cur[x]);
      }
    }
  }

  PixelFormat format_ = PixelFormat::I420;
  int planes_ = 3;
  int slices_ = 1;
};

}  // namespace

std::unique_ptr<codec::VideoDecoder> make_utvideo_decoder() { return std::make_unique<UtvideoDecoder>(); }

}  // namespace cvt::legacy::lossless
