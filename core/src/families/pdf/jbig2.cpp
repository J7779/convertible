// Convertible core - pdf family: JBIG2 decoding (embedded streams, T.88).
//
// Supported: page information, end of stripe, generic regions (MQ
// arithmetic coding with all four templates, adaptive pixels and typical
// prediction; MMR), symbol dictionaries and text regions with arithmetic
// coding, all combination operators. Not supported (the region is skipped
// and a damaged-data result returned): Huffman-coded symbol dictionaries and
// text regions, refinement, pattern dictionaries and halftone regions.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <map>
#include <memory>

#include "families/pdf/bilevel.hpp"

namespace cvt::pdf::bilevel {

namespace {

struct Qe {
  std::uint16_t qe;
  std::uint8_t nmps, nlps, sw;
};

constexpr Qe kQe[47] = {
    {0x5601, 1, 1, 1},   {0x3401, 2, 6, 0},   {0x1801, 3, 9, 0},   {0x0AC1, 4, 12, 0},  {0x0521, 5, 29, 0},
    {0x0221, 38, 33, 0}, {0x5601, 7, 6, 1},   {0x5401, 8, 14, 0},  {0x4801, 9, 14, 0},  {0x3801, 10, 14, 0},
    {0x3001, 11, 17, 0}, {0x2401, 12, 18, 0}, {0x1C01, 13, 20, 0}, {0x1601, 29, 21, 0}, {0x5601, 15, 14, 1},
    {0x5401, 16, 14, 0}, {0x5101, 17, 15, 0}, {0x4801, 18, 16, 0}, {0x3801, 19, 17, 0}, {0x3401, 20, 18, 0},
    {0x3001, 21, 19, 0}, {0x2801, 22, 19, 0}, {0x2401, 23, 20, 0}, {0x2201, 24, 21, 0}, {0x1C01, 25, 22, 0},
    {0x1801, 26, 23, 0}, {0x1601, 27, 24, 0}, {0x1401, 28, 25, 0}, {0x1201, 29, 26, 0}, {0x1101, 30, 27, 0},
    {0x0AC1, 31, 28, 0}, {0x09C1, 32, 29, 0}, {0x08A1, 33, 30, 0}, {0x0521, 34, 31, 0}, {0x0441, 35, 32, 0},
    {0x02A1, 36, 33, 0}, {0x0221, 37, 34, 0}, {0x0141, 38, 35, 0}, {0x0111, 39, 36, 0}, {0x0085, 40, 37, 0},
    {0x0049, 41, 38, 0}, {0x0025, 42, 39, 0}, {0x0015, 43, 40, 0}, {0x0009, 44, 41, 0}, {0x0005, 45, 42, 0},
    {0x0001, 45, 43, 0}, {0x5601, 46, 46, 0}};

class MqDecoder {
 public:
  MqDecoder(ByteView data, std::size_t start, std::size_t end) : data_(data), bp_(start), end_(std::min(end, data.size())) {
    chigh_ = byte(start);
    clow_ = 0;
    byte_in();
    chigh_ = ((chigh_ << 7) & 0xFFFF) | ((clow_ >> 9) & 0x7F);
    clow_ = (clow_ << 7) & 0xFFFF;
    ct_ -= 7;
    a_ = 0x8000;
  }
  int bit(std::vector<std::uint8_t>& contexts, std::size_t pos) {
    if (pos >= contexts.size()) return 0;
    std::uint32_t index = contexts[pos] >> 1;
    std::uint32_t mps = contexts[pos] & 1;
    if (index >= 47) index = 46;
    const Qe& q = kQe[index];
    std::uint32_t a = a_ - q.qe;
    std::uint32_t d;
    if (chigh_ < q.qe) {
      if (a < q.qe) {
        a = q.qe;
        d = mps;
        index = q.nmps;
      } else {
        a = q.qe;
        d = 1 ^ mps;
        if (q.sw == 1) mps = d;
        index = q.nlps;
      }
    } else {
      chigh_ -= q.qe;
      if ((a & 0x8000) != 0) {
        a_ = a;
        return static_cast<int>(mps);
      }
      if (a < q.qe) {
        d = 1 ^ mps;
        if (q.sw == 1) mps = d;
        index = q.nlps;
      } else {
        d = mps;
        index = q.nmps;
      }
    }
    do {
      if (ct_ == 0) byte_in();
      a <<= 1;
      chigh_ = ((chigh_ << 1) & 0xFFFF) | ((clow_ >> 15) & 1);
      clow_ = (clow_ << 1) & 0xFFFF;
      --ct_;
    } while ((a & 0x8000) == 0);
    a_ = a;
    contexts[pos] = static_cast<std::uint8_t>((index << 1) | mps);
    return static_cast<int>(d);
  }

 private:
  std::uint32_t byte(std::size_t i) const { return i < end_ ? std::to_integer<std::uint32_t>(data_[i]) : 0xFF; }
  void byte_in() {
    if (byte(bp_) == 0xFF) {
      if (byte(bp_ + 1) > 0x8F) {
        clow_ += 0xFF00;
        ct_ = 8;
      } else {
        ++bp_;
        clow_ += byte(bp_) << 9;
        ct_ = 7;
      }
    } else {
      ++bp_;
      clow_ += bp_ < end_ ? byte(bp_) << 8 : 0xFF00;
      ct_ = 8;
    }
    if (clow_ > 0xFFFF) {
      chigh_ += clow_ >> 16;
      clow_ &= 0xFFFF;
    }
  }
  ByteView data_;
  std::size_t bp_, end_;
  std::uint32_t chigh_ = 0, clow_ = 0, a_ = 0;
  int ct_ = 0;
};

struct Contexts {
  std::map<std::string, std::vector<std::uint8_t>> sets;
  std::vector<std::uint8_t>& get(const std::string& name, std::size_t size) {
    auto& v = sets[name];
    if (v.size() < size) v.resize(size, 0);
    return v;
  }
};

// Integer arithmetic decoding (A.2); nullopt = OOB.
std::optional<std::int64_t> decode_int(Contexts& cx, const char* name, MqDecoder& d) {
  auto& contexts = cx.get(name, 512);
  std::uint32_t prev = 1;
  const auto bits = [&](int n) {
    std::uint64_t v = 0;
    for (int i = 0; i < n; ++i) {
      const int b = d.bit(contexts, prev);
      prev = prev < 256 ? (prev << 1) | static_cast<std::uint32_t>(b) : (((prev << 1) | static_cast<std::uint32_t>(b)) & 511) | 256;
      v = (v << 1) | static_cast<std::uint64_t>(b);
    }
    return v;
  };
  const std::uint64_t sign = bits(1);
  std::int64_t value;
  if (!bits(1)) value = static_cast<std::int64_t>(bits(2));
  else if (!bits(1)) value = static_cast<std::int64_t>(bits(4)) + 4;
  else if (!bits(1)) value = static_cast<std::int64_t>(bits(6)) + 20;
  else if (!bits(1)) value = static_cast<std::int64_t>(bits(8)) + 84;
  else if (!bits(1)) value = static_cast<std::int64_t>(bits(12)) + 340;
  else value = static_cast<std::int64_t>(bits(32)) + 4436;
  if (sign == 0) return value;
  if (value > 0) return -value;
  return std::nullopt;
}

std::uint32_t decode_iaid(Contexts& cx, MqDecoder& d, int length) {
  auto& contexts = cx.get("IAID", static_cast<std::size_t>(1) << (length + 1));
  std::uint32_t prev = 1;
  for (int i = 0; i < length; ++i) prev = (prev << 1) | static_cast<std::uint32_t>(d.bit(contexts, prev));
  return length < 31 ? prev & ((1u << length) - 1) : prev & 0x7FFFFFFF;
}

struct Bitmap1 {
  std::uint32_t w = 0, h = 0;
  std::vector<std::uint8_t> px;  // 0/1
  [[nodiscard]] int at(std::int64_t x, std::int64_t y) const {
    if (x < 0 || y < 0 || x >= w || y >= h) return 0;
    return px[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)];
  }
};

struct Pt {
  int x, y;
};

constexpr std::array<std::array<Pt, 12>, 4> kTemplates = {{
    {{{-1, -2}, {0, -2}, {1, -2}, {-2, -1}, {-1, -1}, {0, -1}, {1, -1}, {2, -1}, {-4, 0}, {-3, 0}, {-2, 0}, {-1, 0}}},
    {{{-1, -2}, {0, -2}, {1, -2}, {2, -2}, {-2, -1}, {-1, -1}, {0, -1}, {1, -1}, {2, -1}, {-3, 0}, {-2, 0}, {-1, 0}}},
    {{{-1, -2}, {0, -2}, {1, -2}, {-2, -1}, {-1, -1}, {0, -1}, {1, -1}, {-2, 0}, {-1, 0}, {0, 0}, {0, 0}, {0, 0}}},
    {{{-3, -1}, {-2, -1}, {-1, -1}, {0, -1}, {1, -1}, {-4, 0}, {-3, 0}, {-2, 0}, {-1, 0}, {0, 0}, {0, 0}, {0, 0}}},
}};
constexpr int kTemplateSize[4] = {12, 12, 9, 9};
constexpr std::uint32_t kReused[4] = {0x9B25, 0x0795, 0x00E5, 0x0195};

Result<Bitmap1> decode_generic(MqDecoder& d, Contexts& cx, std::uint32_t w, std::uint32_t h, int templ, bool tpgdon,
                               const std::vector<Pt>& at, std::uint64_t max_pixels) {
  if (static_cast<std::uint64_t>(w) * h > max_pixels) return fail(Status::Limit, "JBIG2 region is too large");
  Bitmap1 bm;
  bm.w = w;
  bm.h = h;
  bm.px.assign(static_cast<std::size_t>(w) * h, 0);
  std::vector<Pt> tpl(kTemplates[static_cast<std::size_t>(templ)].begin(),
                      kTemplates[static_cast<std::size_t>(templ)].begin() + kTemplateSize[templ]);
  tpl.insert(tpl.end(), at.begin(), at.end());
  std::stable_sort(tpl.begin(), tpl.end(), [](const Pt& a, const Pt& b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
  auto& contexts = cx.get("GB", static_cast<std::size_t>(1) << 16);
  int ltp = 0;
  for (std::uint32_t y = 0; y < h; ++y) {
    if (tpgdon) {
      ltp ^= d.bit(contexts, kReused[templ]);
      if (ltp) {
        if (y > 0) std::copy_n(bm.px.begin() + static_cast<std::ptrdiff_t>((y - 1) * w), w, bm.px.begin() + static_cast<std::ptrdiff_t>(y * w));
        continue;
      }
    }
    for (std::uint32_t x = 0; x < w; ++x) {
      std::uint32_t label = 0;
      for (const Pt& p : tpl) label = (label << 1) | static_cast<std::uint32_t>(bm.at(static_cast<std::int64_t>(x) + p.x, static_cast<std::int64_t>(y) + p.y));
      bm.px[static_cast<std::size_t>(y) * w + x] = static_cast<std::uint8_t>(d.bit(contexts, label & 0xFFFF));
    }
  }
  return bm;
}

Result<Bitmap1> decode_mmr(ByteView data, std::uint32_t w, std::uint32_t h) {
  CcittParams p;
  p.k = -1;
  p.columns = w;
  p.rows = h;
  p.black_is_1 = true;
  p.end_of_block = true;
  CVT_TRY_ASSIGN(Bytes packed, ccitt_decode(data, p, static_cast<std::uint64_t>(w + 7) / 8 * h + 16));
  Bitmap1 bm;
  bm.w = w;
  bm.h = h;
  bm.px.assign(static_cast<std::size_t>(w) * h, 0);
  const std::size_t row = (w + 7) / 8;
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      const std::size_t at = y * row + x / 8;
      if (at < packed.size()) bm.px[static_cast<std::size_t>(y) * w + x] = (std::to_integer<int>(packed[at]) >> (7 - x % 8)) & 1;
    }
  }
  return bm;
}

void compose(Bitmap1& dst, const Bitmap1& src, std::int64_t ox, std::int64_t oy, int op) {
  for (std::uint32_t y = 0; y < src.h; ++y) {
    const std::int64_t ty = oy + y;
    if (ty < 0 || ty >= dst.h) continue;
    for (std::uint32_t x = 0; x < src.w; ++x) {
      const std::int64_t tx = ox + x;
      if (tx < 0 || tx >= dst.w) continue;
      std::uint8_t& d = dst.px[static_cast<std::size_t>(ty) * dst.w + static_cast<std::size_t>(tx)];
      const std::uint8_t s = src.px[static_cast<std::size_t>(y) * src.w + x];
      switch (op) {
        case 0: d = d | s; break;
        case 1: d = d & s; break;
        case 2: d = d ^ s; break;
        case 3: d = static_cast<std::uint8_t>(1 - (d ^ s)); break;
        default: d = s; break;
      }
    }
  }
}

struct Segment {
  std::uint32_t number = 0;
  int type = 0;
  std::vector<std::uint32_t> referred;
  ByteView data;
};

std::uint32_t be32(ByteView d, std::size_t o) {
  if (o + 4 > d.size()) return 0;
  return (std::to_integer<std::uint32_t>(d[o]) << 24) | (std::to_integer<std::uint32_t>(d[o + 1]) << 16) |
         (std::to_integer<std::uint32_t>(d[o + 2]) << 8) | std::to_integer<std::uint32_t>(d[o + 3]);
}
std::uint16_t be16(ByteView d, std::size_t o) {
  if (o + 2 > d.size()) return 0;
  return static_cast<std::uint16_t>((std::to_integer<int>(d[o]) << 8) | std::to_integer<int>(d[o + 1]));
}

bool parse_segments(ByteView data, std::vector<Segment>& out) {
  std::size_t p = 0;
  while (p + 11 <= data.size() && out.size() < 100000) {
    Segment s;
    s.number = be32(data, p);
    const int flags = std::to_integer<int>(data[p + 4]);
    s.type = flags & 63;
    p += 5;
    if (p >= data.size()) return false;
    int count = std::to_integer<int>(data[p]) >> 5;
    std::size_t retain_bytes = 1;
    if (count == 7) {
      if (p + 4 > data.size()) return false;
      count = static_cast<int>(be32(data, p) & 0x1FFFFFFF);
      retain_bytes = 4 + (static_cast<std::size_t>(count) + 8) / 8;
      if (count > 100000) return false;
    }
    p += retain_bytes;
    const int ref_size = s.number <= 256 ? 1 : (s.number <= 65536 ? 2 : 4);
    for (int i = 0; i < count; ++i) {
      if (p + static_cast<std::size_t>(ref_size) > data.size()) return false;
      std::uint32_t r = ref_size == 1 ? std::to_integer<std::uint32_t>(data[p]) : (ref_size == 2 ? be16(data, p) : be32(data, p));
      s.referred.push_back(r);
      p += static_cast<std::size_t>(ref_size);
    }
    p += (flags & 64) ? 4 : 1;
    if (p + 4 > data.size()) return false;
    std::uint32_t length = be32(data, p);
    p += 4;
    if (length == 0xFFFFFFFF || length > data.size() - std::min(p, data.size())) {
      length = static_cast<std::uint32_t>(data.size() - std::min(p, data.size()));
    }
    s.data = data.subspan(std::min(p, data.size()), length);
    p += length;
    out.push_back(std::move(s));
    if (out.back().type == 51) break;
  }
  return true;
}

struct RegionInfo {
  std::uint32_t w = 0, h = 0, x = 0, y = 0;
  int op = 0;
};

RegionInfo region_info(ByteView d) {
  RegionInfo r;
  r.w = be32(d, 0);
  r.h = be32(d, 4);
  r.x = be32(d, 8);
  r.y = be32(d, 12);
  r.op = d.size() > 16 ? std::to_integer<int>(d[16]) & 7 : 0;
  return r;
}

}  // namespace

Result<Bytes> jbig2_decode(ByteView data, ByteView globals, std::uint64_t max_pixels, std::uint32_t* width_out,
                           std::uint32_t* height_out) {
  std::vector<Segment> segments;
  if (!globals.empty()) parse_segments(globals, segments);
  if (!parse_segments(data, segments) && segments.empty()) return fail(Status::Decode, "damaged JBIG2 data");
  Bitmap1 page;
  bool have_page = false;
  bool unknown_height = false;
  int page_default_op = 0;
  std::map<std::uint32_t, std::vector<std::shared_ptr<Bitmap1>>> dictionaries;
  bool damaged = false;
  std::uint32_t stripe_end = 0;
  for (const Segment& s : segments) {
    const ByteView d = s.data;
    switch (s.type) {
      case 48: {  // page information
        if (d.size() < 19) return fail(Status::Decode, "damaged JBIG2 page information");
        std::uint32_t w = be32(d, 0), h = be32(d, 4);
        const int flags = std::to_integer<int>(d[16]);
        unknown_height = h == 0xFFFFFFFF;
        if (unknown_height) h = 0;
        if (w == 0 || w > 1'000'000 || h > 1'000'000) return fail(Status::Decode, "bad JBIG2 page size");
        if (static_cast<std::uint64_t>(w) * std::max<std::uint32_t>(h, 1) > max_pixels) return fail(Status::Limit, "JBIG2 page is too large");
        page.w = w;
        page.h = h;
        page.px.assign(static_cast<std::size_t>(w) * h, (flags & 4) ? 1 : 0);
        page_default_op = (flags >> 3) & 3;
        have_page = true;
        break;
      }
      case 50: {  // end of stripe
        stripe_end = be32(d, 0) + 1;
        if (unknown_height && stripe_end > page.h && have_page) {
          if (static_cast<std::uint64_t>(page.w) * stripe_end > max_pixels || stripe_end > 1'000'000) {
            return fail(Status::Limit, "JBIG2 page is too large");
          }
          page.px.resize(static_cast<std::size_t>(page.w) * stripe_end, 0);
          page.h = stripe_end;
        }
        break;
      }
      case 36: case 38: case 39: {  // generic region
        if (!have_page || d.size() < 18) {
          damaged = true;
          break;
        }
        const RegionInfo ri = region_info(d);
        const int flags = std::to_integer<int>(d[17]);
        const bool mmr = flags & 1;
        const int templ = (flags >> 1) & 3;
        const bool tpgdon = (flags >> 3) & 1;
        std::size_t p = 18;
        std::vector<Pt> at;
        if (!mmr) {
          const int n = templ == 0 ? 4 : 1;
          for (int i = 0; i < n; ++i) {
            if (p + 2 > d.size()) break;
            at.push_back(Pt{static_cast<std::int8_t>(std::to_integer<std::uint8_t>(d[p])),
                            static_cast<std::int8_t>(std::to_integer<std::uint8_t>(d[p + 1]))});
            p += 2;
          }
        }
        if (ri.w == 0 || ri.h == 0 || ri.w > 1'000'000 || ri.h > 1'000'000) {
          damaged = true;
          break;
        }
        // Unknown-height pages grow with their regions.
        if (unknown_height && static_cast<std::uint64_t>(ri.y) + ri.h > page.h) {
          const std::uint64_t nh = static_cast<std::uint64_t>(ri.y) + ri.h;
          if (nh * page.w > max_pixels) return fail(Status::Limit, "JBIG2 page is too large");
          page.px.resize(static_cast<std::size_t>(page.w) * nh, 0);
          page.h = static_cast<std::uint32_t>(nh);
        }
        Result<Bitmap1> bm = mmr ? decode_mmr(d.subspan(std::min(p, d.size())), ri.w, ri.h) : Result<Bitmap1>(Bitmap1{});
        if (!mmr) {
          MqDecoder dec(d, std::min(p, d.size()), d.size());
          Contexts cx;
          bm = decode_generic(dec, cx, ri.w, ri.h, templ, tpgdon, at, max_pixels);
        }
        if (!bm) {
          if (bm.error().code == Status::Limit) return fail(bm.error());
          damaged = true;
          break;
        }
        compose(page, *bm, ri.x, ri.y, (s.type == 36) ? page_default_op : ri.op);
        break;
      }
      case 0: {  // symbol dictionary
        if (d.size() < 2) {
          damaged = true;
          break;
        }
        const int flags = be16(d, 0);
        const bool huff = flags & 1, refagg = (flags >> 1) & 1;
        const int templ = (flags >> 10) & 3;
        std::size_t p = 2;
        std::vector<Pt> at;
        if (!huff) {
          const int n = templ == 0 ? 4 : 1;
          for (int i = 0; i < n; ++i) {
            if (p + 2 > d.size()) break;
            at.push_back(Pt{static_cast<std::int8_t>(std::to_integer<std::uint8_t>(d[p])),
                            static_cast<std::int8_t>(std::to_integer<std::uint8_t>(d[p + 1]))});
            p += 2;
          }
        }
        if (refagg && !((flags >> 12) & 1)) p += 4;
        const std::uint32_t num_ex = be32(d, p), num_new = be32(d, p + 4);
        p += 8;
        if (huff || refagg || num_new > 65536 || num_ex > 65536) {
          damaged = true;
          break;
        }
        std::vector<std::shared_ptr<Bitmap1>> input;
        for (const std::uint32_t r : s.referred) {
          const auto it = dictionaries.find(r);
          if (it != dictionaries.end()) input.insert(input.end(), it->second.begin(), it->second.end());
        }
        MqDecoder dec(d, std::min(p, d.size()), d.size());
        Contexts cx;
        std::vector<std::shared_ptr<Bitmap1>> fresh;
        std::int64_t height = 0;
        bool ok = true;
        std::uint64_t pixels = 0;
        while (fresh.size() < num_new && ok) {
          const auto dh = decode_int(cx, "IADH", dec);
          if (!dh) {
            ok = false;
            break;
          }
          height += *dh;
          std::int64_t width = 0;
          for (int guard = 0; guard < 65536; ++guard) {
            const auto dw = decode_int(cx, "IADW", dec);
            if (!dw) break;
            width += *dw;
            if (width <= 0 || height <= 0 || width > 65536 || height > 65536 || fresh.size() >= num_new) {
              ok = false;
              break;
            }
            pixels += static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
            if (pixels > max_pixels) return fail(Status::Limit, "JBIG2 symbols are too large");
            auto bm = decode_generic(dec, cx, static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
                                     templ, false, at, max_pixels);
            if (!bm) {
              ok = false;
              break;
            }
            fresh.push_back(std::make_shared<Bitmap1>(std::move(*bm)));
          }
        }
        if (!ok) damaged = true;
        // Export flags.
        std::vector<std::shared_ptr<Bitmap1>> exported;
        const std::size_t total = input.size() + fresh.size();
        std::size_t index = 0;
        bool flag = false;
        for (int guard = 0; index < total && guard < 200000; ++guard) {
          const auto run = decode_int(cx, "IAEX", dec);
          if (!run || *run < 0 || static_cast<std::uint64_t>(*run) > total) break;
          for (std::int64_t k = 0; k < *run && index < total; ++k, ++index) {
            if (flag) exported.push_back(index < input.size() ? input[index] : fresh[index - input.size()]);
          }
          flag = !flag;
        }
        dictionaries[s.number] = std::move(exported);
        break;
      }
      case 4: case 6: case 7: {  // text region
        if (!have_page || d.size() < 19) {
          damaged = true;
          break;
        }
        const RegionInfo ri = region_info(d);
        const int flags = be16(d, 17);
        std::size_t p = 19;
        const bool huff = flags & 1, refine = (flags >> 1) & 1;
        const int log_strips = (flags >> 2) & 3;
        const int ref_corner = (flags >> 4) & 3;
        const bool transposed = (flags >> 6) & 1;
        const int combop = (flags >> 7) & 3;
        const int def_pixel = (flags >> 9) & 1;
        int ds_offset = (flags >> 10) & 31;
        if (ds_offset > 15) ds_offset -= 32;
        if (huff || refine) {
          damaged = true;
          break;
        }
        const std::uint32_t instances = be32(d, p);
        p += 4;
        std::vector<std::shared_ptr<Bitmap1>> symbols;
        for (const std::uint32_t r : s.referred) {
          const auto it = dictionaries.find(r);
          if (it != dictionaries.end()) symbols.insert(symbols.end(), it->second.begin(), it->second.end());
        }
        if (symbols.empty() || ri.w == 0 || ri.h == 0 || static_cast<std::uint64_t>(ri.w) * ri.h > max_pixels) {
          damaged = true;
          break;
        }
        int code_len = 0;
        while ((static_cast<std::size_t>(1) << code_len) < symbols.size()) ++code_len;
        Bitmap1 region;
        region.w = ri.w;
        region.h = ri.h;
        region.px.assign(static_cast<std::size_t>(ri.w) * ri.h, static_cast<std::uint8_t>(def_pixel));
        MqDecoder dec(d, std::min(p, d.size()), d.size());
        Contexts cx;
        const std::int64_t strip = std::int64_t{1} << log_strips;
        std::int64_t strip_t = -decode_int(cx, "IADT", dec).value_or(0) * strip;
        std::int64_t first_s = 0;
        std::uint32_t placed = 0;
        bool ok = true;
        while (placed < instances && ok) {
          const auto dt = decode_int(cx, "IADT", dec);
          if (!dt) break;
          strip_t += *dt * strip;
          const auto dfs = decode_int(cx, "IAFS", dec);
          if (!dfs) break;
          first_s += *dfs;
          std::int64_t cur_s = first_s;
          for (int guard = 0; guard < 100000; ++guard) {
            std::int64_t cur_t = 0;
            if (strip > 1) cur_t = decode_int(cx, "IAIT", dec).value_or(0);
            const std::int64_t t = strip_t + cur_t;
            const std::uint32_t id = decode_iaid(cx, dec, code_len);
            if (id >= symbols.size()) {
              ok = false;
              break;
            }
            const Bitmap1& sym = *symbols[id];
            const std::int64_t sw = sym.w, sh = sym.h;
            // 6.4.5 steps 3c ix-xii (REFCORNER: 0 bottom-left, 1 top-left, 2 bottom-right, 3 top-right).
            if (!transposed && (ref_corner == 2 || ref_corner == 3)) cur_s += sw - 1;
            if (transposed && (ref_corner == 0 || ref_corner == 2)) cur_s += sh - 1;
            const std::int64_t si = cur_s;
            const bool right = ref_corner == 2 || ref_corner == 3;
            const bool top = ref_corner == 1 || ref_corner == 3;
            std::int64_t x = 0, y = 0;
            if (!transposed) {
              x = right ? si - sw + 1 : si;
              y = top ? t : t - sh + 1;
            } else {
              x = right ? t - sw + 1 : t;
              y = top ? si : si - sh + 1;
            }
            compose(region, sym, x, y, combop);
            if (!transposed && (ref_corner == 0 || ref_corner == 1)) cur_s += sw - 1;
            if (transposed && (ref_corner == 1 || ref_corner == 3)) cur_s += sh - 1;
            ++placed;
            if (placed >= instances) break;
            const auto ds = decode_int(cx, "IADS", dec);
            if (!ds) break;
            cur_s += *ds + ds_offset;
          }
        }
        if (!ok) damaged = true;
        if (unknown_height && static_cast<std::uint64_t>(ri.y) + ri.h > page.h) {
          const std::uint64_t nh = static_cast<std::uint64_t>(ri.y) + ri.h;
          if (nh * page.w > max_pixels) return fail(Status::Limit, "JBIG2 page is too large");
          page.px.resize(static_cast<std::size_t>(page.w) * nh, 0);
          page.h = static_cast<std::uint32_t>(nh);
        }
        compose(page, region, ri.x, ri.y, s.type == 4 ? page_default_op : ri.op);
        break;
      }
      case 49: case 51: case 52: case 53: case 62: break;
      default:
        damaged = true;  // refinement, pattern and halftone regions are not supported
        break;
    }
  }
  if (!have_page || page.h == 0) return fail(Status::Decode, "the JBIG2 data has no page");
  const std::size_t row = (page.w + 7) / 8;
  Bytes out(row * page.h, std::byte{0});
  for (std::uint32_t y = 0; y < page.h; ++y) {
    for (std::uint32_t x = 0; x < page.w; ++x) {
      if (page.px[static_cast<std::size_t>(y) * page.w + x]) out[y * row + x / 8] |= static_cast<std::byte>(0x80 >> (x % 8));
    }
  }
  if (width_out != nullptr) *width_out = page.w;
  if (height_out != nullptr) *height_out = page.h;
  (void)damaged;
  return out;
}

}  // namespace cvt::pdf::bilevel
