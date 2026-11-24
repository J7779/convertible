// Convertible core - image family: JPEG encoder (baseline and progressive).
//
// Quality 1..100 scales the Annex K tables exactly like libjpeg, so the
// numbers mean what users expect. Float AAN forward DCT, box-filtered chroma
// downsampling, optimal (two-pass) Huffman tables, libjpeg's default
// progressive script including successive-approximation refinement scans.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "families/image/image_ops.hpp"
#include "families/image/jpeg_codec.hpp"
#include "util/bytes.hpp"

namespace cvt::image::jpeg {

namespace {

constexpr std::array<double, 8> aan = {1.0,         1.387039845, 1.306562965, 1.175875602,
                                       1.0,         0.785694958, 0.541196100, 0.275899379};

/// Float AAN forward DCT in place (as libjpeg's jfdctflt); output scaled by
/// 8 * aan[row] * aan[col].
void fdct(float* d) {
  for (int pass = 0; pass < 2; ++pass) {
    const int step = pass == 0 ? 1 : 8;     // element step inside a vector
    const int next = pass == 0 ? 8 : 1;     // step between vectors
    for (int v = 0; v < 8; ++v) {
      float* p = d + v * next;
      const float tmp0 = p[0] + p[7 * step];
      const float tmp7 = p[0] - p[7 * step];
      const float tmp1 = p[step] + p[6 * step];
      const float tmp6 = p[step] - p[6 * step];
      const float tmp2 = p[2 * step] + p[5 * step];
      const float tmp5 = p[2 * step] - p[5 * step];
      const float tmp3 = p[3 * step] + p[4 * step];
      const float tmp4 = p[3 * step] - p[4 * step];
      float tmp10 = tmp0 + tmp3;
      const float tmp13 = tmp0 - tmp3;
      float tmp11 = tmp1 + tmp2;
      float tmp12 = tmp1 - tmp2;
      p[0] = tmp10 + tmp11;
      p[4 * step] = tmp10 - tmp11;
      const float z1 = (tmp12 + tmp13) * 0.707106781f;
      p[2 * step] = tmp13 + z1;
      p[6 * step] = tmp13 - z1;
      tmp10 = tmp4 + tmp5;
      tmp11 = tmp5 + tmp6;
      tmp12 = tmp6 + tmp7;
      const float z5 = (tmp10 - tmp12) * 0.382683433f;
      const float z2 = 0.541196100f * tmp10 + z5;
      const float z4 = 1.306562965f * tmp12 + z5;
      const float z3 = tmp11 * 0.707106781f;
      const float z11 = tmp7 + z3;
      const float z13 = tmp7 - z3;
      p[5 * step] = z13 + z2;
      p[3 * step] = z13 - z2;
      p[step] = z11 + z4;
      p[7 * step] = z11 - z4;
    }
  }
}

// --------------------------------------------------------------------------- Huffman

struct Codes {
  std::array<std::uint16_t, 256> code{};
  std::array<std::uint8_t, 256> size{};
};

Codes make_codes(const HuffmanSpec& spec) {
  Codes codes;
  std::uint32_t code = 0;
  std::size_t k = 0;
  for (int length = 1; length <= 16; ++length) {
    for (int i = 0; i < spec.counts[static_cast<std::size_t>(length - 1)]; ++i, ++k) {
      codes.code[spec.symbols[k]] = static_cast<std::uint16_t>(code);
      codes.size[spec.symbols[k]] = static_cast<std::uint8_t>(length);
      ++code;
    }
    code <<= 1;
  }
  return codes;
}

/// libjpeg's jpeg_gen_optimal_table: optimal code lengths limited to 16 bits,
/// with the all-ones code reserved.
HuffmanSpec optimal_table(const std::array<std::uint32_t, 256>& counts) {
  constexpr int max_len = 320;
  std::array<std::uint64_t, 257> freq{};
  for (std::size_t i = 0; i < 256; ++i) freq[i] = counts[i];
  freq[256] = 1;
  std::array<int, 257> codesize{};
  std::array<int, 257> others{};
  others.fill(-1);
  while (true) {
    int c1 = -1;
    std::uint64_t v = ~std::uint64_t{0};
    for (int i = 0; i <= 256; ++i) {
      if (freq[static_cast<std::size_t>(i)] != 0 && freq[static_cast<std::size_t>(i)] <= v) {
        v = freq[static_cast<std::size_t>(i)];
        c1 = i;
      }
    }
    int c2 = -1;
    v = ~std::uint64_t{0};
    for (int i = 0; i <= 256; ++i) {
      if (freq[static_cast<std::size_t>(i)] != 0 && freq[static_cast<std::size_t>(i)] <= v && i != c1) {
        v = freq[static_cast<std::size_t>(i)];
        c2 = i;
      }
    }
    if (c2 < 0) break;
    freq[static_cast<std::size_t>(c1)] += freq[static_cast<std::size_t>(c2)];
    freq[static_cast<std::size_t>(c2)] = 0;
    ++codesize[static_cast<std::size_t>(c1)];
    while (others[static_cast<std::size_t>(c1)] >= 0) {
      c1 = others[static_cast<std::size_t>(c1)];
      ++codesize[static_cast<std::size_t>(c1)];
    }
    others[static_cast<std::size_t>(c1)] = c2;
    ++codesize[static_cast<std::size_t>(c2)];
    while (others[static_cast<std::size_t>(c2)] >= 0) {
      c2 = others[static_cast<std::size_t>(c2)];
      ++codesize[static_cast<std::size_t>(c2)];
    }
  }
  std::vector<int> bits(max_len + 1, 0);
  for (int i = 0; i <= 256; ++i) {
    if (codesize[static_cast<std::size_t>(i)] != 0) ++bits[static_cast<std::size_t>(std::min(codesize[static_cast<std::size_t>(i)], max_len))];
  }
  for (int i = max_len; i > 16; --i) {
    while (bits[static_cast<std::size_t>(i)] > 0) {
      int j = i - 2;
      while (j > 0 && bits[static_cast<std::size_t>(j)] == 0) --j;
      bits[static_cast<std::size_t>(i)] -= 2;
      ++bits[static_cast<std::size_t>(i - 1)];
      bits[static_cast<std::size_t>(j + 1)] += 2;
      --bits[static_cast<std::size_t>(j)];
    }
  }
  int i = 16;
  while (i > 0 && bits[static_cast<std::size_t>(i)] == 0) --i;
  --bits[static_cast<std::size_t>(i)];
  HuffmanSpec spec;
  for (int len = 1; len <= 16; ++len) spec.counts[static_cast<std::size_t>(len - 1)] = static_cast<std::uint8_t>(bits[static_cast<std::size_t>(len)]);
  // Symbols sorted by code length (ties by value), excluding the pseudo-symbol.
  std::vector<std::pair<int, int>> order;
  for (int s = 0; s < 256; ++s) {
    if (codesize[static_cast<std::size_t>(s)] != 0) order.emplace_back(codesize[static_cast<std::size_t>(s)], s);
  }
  std::sort(order.begin(), order.end());
  for (const auto& [len, s] : order) spec.symbols.push_back(static_cast<std::uint8_t>(s));
  return spec;
}

// --------------------------------------------------------------------------- bit output

class BitWriter {
 public:
  explicit BitWriter(Bytes& out) : out_(out) {}
  void put(std::uint32_t value, int size) {
    if (size == 0) return;
    acc_ = (acc_ << size) | (value & ((1u << size) - 1u));
    bits_ += size;
    while (bits_ >= 8) {
      bits_ -= 8;
      const auto byte = static_cast<std::uint8_t>(acc_ >> bits_);
      out_.push_back(std::byte{byte});
      if (byte == 0xFF) out_.push_back(std::byte{0});
    }
    acc_ &= (std::uint64_t{1} << bits_) - 1;
  }
  void flush() {
    if (bits_ > 0) put(0x7F, 8 - bits_);  // pad with 1-bits
  }

 private:
  Bytes& out_;
  std::uint64_t acc_ = 0;
  int bits_ = 0;
};

inline int bit_count(int value) {
  int n = 0;
  auto v = static_cast<unsigned>(value < 0 ? -value : value);
  while (v != 0) {
    ++n;
    v >>= 1;
  }
  return n;
}

/// Symbol and bit sink for one pass over a scan: either gathers symbol
/// statistics or emits codes.
struct Sink {
  bool counting = true;
  std::array<std::array<std::uint32_t, 256>, 2> dc_counts{};
  std::array<std::array<std::uint32_t, 256>, 2> ac_counts{};
  std::array<Codes, 2> dc_codes{};
  std::array<Codes, 2> ac_codes{};
  BitWriter* writer = nullptr;

  void dc(int table, int symbol) {
    if (counting) {
      ++dc_counts[static_cast<std::size_t>(table)][static_cast<std::size_t>(symbol)];
    } else {
      const Codes& c = dc_codes[static_cast<std::size_t>(table)];
      writer->put(c.code[static_cast<std::size_t>(symbol)], c.size[static_cast<std::size_t>(symbol)]);
    }
  }
  void ac(int table, int symbol) {
    if (counting) {
      ++ac_counts[static_cast<std::size_t>(table)][static_cast<std::size_t>(symbol)];
    } else {
      const Codes& c = ac_codes[static_cast<std::size_t>(table)];
      writer->put(c.code[static_cast<std::size_t>(symbol)], c.size[static_cast<std::size_t>(symbol)]);
    }
  }
  void bits(std::uint32_t value, int count) {
    if (!counting) writer->put(value, count);
  }
};

// --------------------------------------------------------------------------- components

struct EncComponent {
  int id = 1;
  int h = 1;
  int v = 1;
  int tq = 0;
  int table = 0;  // 0 luma, 1 chroma Huffman tables
  std::uint32_t blocks_w = 0;
  std::uint32_t blocks_h = 0;
  std::uint32_t real_blocks_w = 0;
  std::uint32_t real_blocks_h = 0;
  std::vector<std::int16_t> coefs;
};

struct ScanSpec {
  std::vector<int> comps;
  int ss = 0, se = 63, ah = 0, al = 0;
};

struct Layout {
  std::uint32_t width = 0, height = 0;
  int max_h = 1, max_v = 1;
  std::uint32_t mcus_x = 0, mcus_y = 0;
};

/// Run one scan through `sink`.
void run_scan(const ScanSpec& scan, std::vector<EncComponent>& comps, const Layout& layout, Sink& sink) {
  const bool single = scan.comps.size() == 1;
  std::array<int, 4> last_dc{};
  std::uint32_t eobrun = 0;
  std::vector<std::uint8_t> correction;  // buffered refinement bits (BE)
  const auto emit_eobrun = [&](int table) {
    if (eobrun == 0) return;
    int nbits = 0;
    for (std::uint32_t t = eobrun; t >>= 1;) ++nbits;
    sink.ac(table, nbits << 4);
    if (nbits != 0) sink.bits(eobrun, nbits);
    eobrun = 0;
    for (const std::uint8_t bit : correction) sink.bits(bit, 1);
    correction.clear();
  };
  const auto encode_block = [&](EncComponent& c, std::size_t index, int ci) {
    const std::int16_t* block = c.coefs.data() + index * 64;
    const int dct = c.table;
    if (scan.ss == 0) {
      if (scan.ah == 0) {
        const int value = block[0] >> scan.al;  // arithmetic shift of the point transform
        const int diff = value - last_dc[static_cast<std::size_t>(ci)];
        last_dc[static_cast<std::size_t>(ci)] = value;
        const int nbits = bit_count(diff);
        sink.dc(dct, nbits);
        if (nbits != 0) sink.bits(static_cast<std::uint32_t>(diff < 0 ? diff - 1 : diff), nbits);
      } else {
        sink.bits(static_cast<std::uint32_t>((block[0] >> scan.al) & 1), 1);
      }
      if (scan.se == 0) return;
    }
    const int first = std::max(scan.ss, 1);
    if (scan.ah == 0) {
      int run = 0;
      for (int k = first; k <= scan.se; ++k) {
        int temp = block[zigzag[static_cast<std::size_t>(k)]];
        int temp2 = 0;
        if (temp < 0) {
          temp = (-temp) >> scan.al;
          temp2 = ~temp;
        } else {
          temp >>= scan.al;
          temp2 = temp;
        }
        if (temp == 0) {
          ++run;
          continue;
        }
        emit_eobrun(dct);
        while (run > 15) {
          sink.ac(dct, 0xF0);
          run -= 16;
        }
        const int nbits = bit_count(temp);
        sink.ac(dct, (run << 4) + nbits);
        sink.bits(static_cast<std::uint32_t>(temp2), nbits);
        run = 0;
      }
      if (run > 0) {
        if (scan.ss == 0 || !single) {
          sink.ac(dct, 0x00);  // sequential: plain EOB
        } else {
          ++eobrun;
          if (eobrun == 0x7FFF) emit_eobrun(dct);
        }
      }
      return;
    }
    // AC refinement (G.1.2.3).
    std::array<int, 64> absolute{};
    int eob = 0;
    for (int k = scan.ss; k <= scan.se; ++k) {
      int temp = block[zigzag[static_cast<std::size_t>(k)]];
      if (temp < 0) temp = -temp;
      temp >>= scan.al;
      absolute[static_cast<std::size_t>(k)] = temp;
      if (temp == 1) eob = k;
    }
    int run = 0;
    std::vector<std::uint8_t> pending;  // correction bits of this block (BR)
    for (int k = scan.ss; k <= scan.se; ++k) {
      const int temp = absolute[static_cast<std::size_t>(k)];
      if (temp == 0) {
        ++run;
        continue;
      }
      while (run > 15 && k <= eob) {
        emit_eobrun(dct);
        sink.ac(dct, 0xF0);
        run -= 16;
        for (const std::uint8_t bit : pending) sink.bits(bit, 1);
        pending.clear();
      }
      if (temp > 1) {
        pending.push_back(static_cast<std::uint8_t>(temp & 1));
        continue;
      }
      emit_eobrun(dct);
      sink.ac(dct, (run << 4) + 1);
      sink.bits(block[zigzag[static_cast<std::size_t>(k)]] < 0 ? 0u : 1u, 1);
      for (const std::uint8_t bit : pending) sink.bits(bit, 1);
      pending.clear();
      run = 0;
    }
    if (run > 0 || !pending.empty()) {
      ++eobrun;
      correction.insert(correction.end(), pending.begin(), pending.end());
      if (eobrun == 0x7FFF || correction.size() > 1000 - 64 + 1) emit_eobrun(dct);
    }
  };
  if (single) {
    EncComponent& c = comps[static_cast<std::size_t>(scan.comps[0])];
    for (std::uint32_t by = 0; by < c.real_blocks_h; ++by) {
      for (std::uint32_t bx = 0; bx < c.real_blocks_w; ++bx) {
        encode_block(c, static_cast<std::size_t>(by) * c.blocks_w + bx, scan.comps[0]);
      }
    }
    emit_eobrun(c.table);
  } else {
    for (std::uint32_t my = 0; my < layout.mcus_y; ++my) {
      for (std::uint32_t mx = 0; mx < layout.mcus_x; ++mx) {
        for (const int ci : scan.comps) {
          EncComponent& c = comps[static_cast<std::size_t>(ci)];
          for (int v = 0; v < c.v; ++v) {
            for (int h = 0; h < c.h; ++h) {
              const std::size_t by = my * static_cast<std::uint32_t>(c.v) + static_cast<std::uint32_t>(v);
              const std::size_t bx = mx * static_cast<std::uint32_t>(c.h) + static_cast<std::uint32_t>(h);
              encode_block(c, by * c.blocks_w + bx, ci);
            }
          }
        }
      }
    }
  }
}

// --------------------------------------------------------------------------- markers

void marker(util::ByteWriter& out, std::uint8_t code) { out.u8(0xFF).u8(code); }

void segment(util::ByteWriter& out, std::uint8_t code, ByteView body) {
  marker(out, code);
  out.u16be(static_cast<std::uint16_t>(body.size() + 2));
  out.bytes(body);
}

void write_dht(util::ByteWriter& out, int tc, int th, const HuffmanSpec& spec) {
  util::ByteWriter body;
  body.u8(static_cast<std::uint8_t>(tc << 4 | th));
  for (const std::uint8_t c : spec.counts) body.u8(c);
  body.bytes(std::span<const std::uint8_t>(spec.symbols));
  segment(out, 0xC4, body.view());
}

void write_metadata(util::ByteWriter& out, const EncodeOptions& options) {
  {
    util::ByteWriter jfif;
    jfif.text(std::string_view("JFIF\0", 5)).u8(1).u8(1);
    if (options.dpi_x > 0 && options.dpi_y > 0 && options.dpi_x < 65536 && options.dpi_y < 65536) {
      jfif.u8(1).u16be(static_cast<std::uint16_t>(std::lround(options.dpi_x)))
          .u16be(static_cast<std::uint16_t>(std::lround(options.dpi_y)));
    } else {
      jfif.u8(0).u16be(1).u16be(1);
    }
    jfif.u8(0).u8(0);
    segment(out, 0xE0, jfif.view());
  }
  if (!options.exif.empty() && options.exif.size() <= 65533 - 6) {
    util::ByteWriter body;
    body.text(std::string_view("Exif\0\0", 6)).bytes(options.exif);
    segment(out, 0xE1, body.view());
  }
  constexpr std::string_view xmp_id("http://ns.adobe.com/xap/1.0/\0", 29);
  if (!options.xmp.empty() && options.xmp.size() <= 65533 - xmp_id.size()) {
    util::ByteWriter body;
    body.text(xmp_id).text(options.xmp);
    segment(out, 0xE1, body.view());
  }
  if (!options.icc.empty()) {
    constexpr std::size_t chunk = 65519;
    const std::size_t count = (options.icc.size() + chunk - 1) / chunk;
    if (count <= 255) {
      for (std::size_t i = 0; i < count; ++i) {
        util::ByteWriter body;
        body.text(std::string_view("ICC_PROFILE\0", 12)).u8(static_cast<std::uint8_t>(i + 1)).u8(static_cast<std::uint8_t>(count));
        body.bytes(ByteView(options.icc).subspan(i * chunk, std::min(chunk, options.icc.size() - i * chunk)));
        segment(out, 0xE2, body.view());
      }
    }
  }
  if (!options.comment.empty()) {
    const std::string_view text = std::string_view(options.comment).substr(0, 65533);
    segment(out, 0xFE, as_bytes(text));
  }
}

/// A source plane for one component at its own sampled resolution.
struct SourcePlane {
  const std::uint8_t* data = nullptr;
  std::size_t stride = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

Result<Bytes> encode_components(std::vector<SourcePlane> sources, std::vector<EncComponent> comps,
                                const Layout& layout, const EncodeOptions& options, const Context& ctx) {
  const int quality = std::clamp(options.quality, 1, 100);
  const std::array<std::array<std::uint16_t, 64>, 2> quant = {scaled_quant(luma_quant, quality),
                                                             scaled_quant(chroma_quant, quality)};
  // Float divisors including the AAN scaling.
  std::array<std::array<float, 64>, 2> divisors{};
  for (std::size_t t = 0; t < 2; ++t) {
    for (std::size_t r = 0; r < 8; ++r) {
      for (std::size_t c = 0; c < 8; ++c) {
        divisors[t][r * 8 + c] = static_cast<float>(1.0 / (quant[t][r * 8 + c] * aan[r] * aan[c] * 8.0));
      }
    }
  }
  // Forward DCT + quantisation of every block (edge pixels replicated into the padding).
  for (std::size_t ci = 0; ci < comps.size(); ++ci) {
    EncComponent& c = comps[ci];
    const SourcePlane& src = sources[ci];
    c.coefs.assign(static_cast<std::size_t>(c.blocks_w) * c.blocks_h * 64, 0);
    const auto& div = divisors[static_cast<std::size_t>(c.tq)];
    float block[64];
    for (std::uint32_t by = 0; by < c.blocks_h; ++by) {
      if ((by & 15) == 0) CVT_TRY(ctx.checkpoint(0.6 * by / c.blocks_h, "Transforming"));
      for (std::uint32_t bx = 0; bx < c.blocks_w; ++bx) {
        for (std::uint32_t r = 0; r < 8; ++r) {
          const std::uint32_t y = std::min(by * 8 + r, src.height - 1);
          const std::uint8_t* row = src.data + static_cast<std::size_t>(y) * src.stride;
          for (std::uint32_t k = 0; k < 8; ++k) {
            const std::uint32_t x = std::min(bx * 8 + k, src.width - 1);
            block[r * 8 + k] = static_cast<float>(row[x]) - 128.0f;
          }
        }
        fdct(block);
        std::int16_t* out = c.coefs.data() + (static_cast<std::size_t>(by) * c.blocks_w + bx) * 64;
        for (std::size_t i = 0; i < 64; ++i) {
          const float q = block[i] * div[i];
          out[i] = static_cast<std::int16_t>(static_cast<int>(q + 16384.5f) - 16384);
        }
      }
    }
  }

  util::ByteWriter out;
  marker(out, 0xD8);
  write_metadata(out, options);
  // DQT
  {
    util::ByteWriter body;
    const std::size_t tables = comps.size() > 1 ? 2 : 1;
    for (std::size_t t = 0; t < tables; ++t) {
      body.u8(static_cast<std::uint8_t>(t));
      for (std::size_t k = 0; k < 64; ++k) body.u8(static_cast<std::uint8_t>(quant[t][zigzag[k]]));
    }
    segment(out, 0xDB, body.view());
  }
  // SOF
  {
    util::ByteWriter body;
    body.u8(8).u16be(static_cast<std::uint16_t>(layout.height)).u16be(static_cast<std::uint16_t>(layout.width));
    body.u8(static_cast<std::uint8_t>(comps.size()));
    for (const EncComponent& c : comps) {
      body.u8(static_cast<std::uint8_t>(c.id)).u8(static_cast<std::uint8_t>(c.h << 4 | c.v)).u8(static_cast<std::uint8_t>(c.tq));
    }
    segment(out, options.progressive ? 0xC2 : 0xC0, body.view());
  }

  // The scan script.
  std::vector<ScanSpec> scans;
  const int n = static_cast<int>(comps.size());
  if (!options.progressive) {
    ScanSpec all;
    for (int i = 0; i < n; ++i) all.comps.push_back(i);
    scans.push_back(all);
  } else {
    ScanSpec dc;
    for (int i = 0; i < n; ++i) dc.comps.push_back(i);
    dc.ss = 0;
    dc.se = 0;
    dc.ah = 0;
    dc.al = 1;
    scans.push_back(dc);
    scans.push_back({{0}, 1, 5, 0, 2});
    if (n == 3) {
      scans.push_back({{2}, 1, 63, 0, 1});
      scans.push_back({{1}, 1, 63, 0, 1});
    }
    scans.push_back({{0}, 6, 63, 0, 2});
    scans.push_back({{0}, 1, 63, 2, 1});
    ScanSpec dc_refine = dc;
    dc_refine.ah = 1;
    dc_refine.al = 0;
    scans.push_back(dc_refine);
    if (n == 3) {
      scans.push_back({{2}, 1, 63, 1, 0});
      scans.push_back({{1}, 1, 63, 1, 0});
    }
    scans.push_back({{0}, 1, 63, 1, 0});
  }

  const bool optimize = options.optimize || options.progressive;
  std::array<HuffmanSpec, 2> std_dc = {standard_dc_luma(), standard_dc_chroma()};
  std::array<HuffmanSpec, 2> std_ac = {standard_ac_luma(), standard_ac_chroma()};
  Bytes entropy;
  for (std::size_t s = 0; s < scans.size(); ++s) {
    CVT_TRY(ctx.checkpoint(0.6 + 0.4 * static_cast<double>(s) / static_cast<double>(scans.size()), "Encoding"));
    const ScanSpec& scan = scans[s];
    Sink sink;
    std::array<HuffmanSpec, 2> dc_spec = std_dc;
    std::array<HuffmanSpec, 2> ac_spec = std_ac;
    if (optimize) {
      sink.counting = true;
      run_scan(scan, comps, layout, sink);
      for (std::size_t t = 0; t < 2; ++t) {
        dc_spec[t] = optimal_table(sink.dc_counts[t]);
        ac_spec[t] = optimal_table(sink.ac_counts[t]);
      }
    }
    // Tables used by this scan.
    std::array<bool, 2> uses_dc{};
    std::array<bool, 2> uses_ac{};
    for (const int ci : scan.comps) {
      const auto t = static_cast<std::size_t>(comps[static_cast<std::size_t>(ci)].table);
      if (scan.ss == 0 && scan.ah == 0) uses_dc[t] = true;
      if (scan.se > 0) uses_ac[t] = true;
    }
    for (std::size_t t = 0; t < 2; ++t) {
      if (uses_dc[t] && (optimize || s == 0)) write_dht(out, 0, static_cast<int>(t), dc_spec[t]);
      if (uses_ac[t] && (optimize || s == 0)) write_dht(out, 1, static_cast<int>(t), ac_spec[t]);
      sink.dc_codes[t] = make_codes(dc_spec[t]);
      sink.ac_codes[t] = make_codes(ac_spec[t]);
    }
    {
      util::ByteWriter body;
      body.u8(static_cast<std::uint8_t>(scan.comps.size()));
      for (const int ci : scan.comps) {
        const EncComponent& c = comps[static_cast<std::size_t>(ci)];
        body.u8(static_cast<std::uint8_t>(c.id)).u8(static_cast<std::uint8_t>(c.table << 4 | c.table));
      }
      body.u8(static_cast<std::uint8_t>(scan.ss)).u8(static_cast<std::uint8_t>(scan.se));
      body.u8(static_cast<std::uint8_t>(scan.ah << 4 | scan.al));
      segment(out, 0xDA, body.view());
    }
    entropy.clear();
    BitWriter writer(entropy);
    sink.counting = false;
    sink.writer = &writer;
    run_scan(scan, comps, layout, sink);
    writer.flush();
    out.bytes(entropy);
  }
  marker(out, 0xD9);
  CVT_TRY(ctx.limits.check_output(out.size()));
  return out.take();
}

Layout make_layout(std::uint32_t width, std::uint32_t height, std::vector<EncComponent>& comps) {
  Layout layout;
  layout.width = width;
  layout.height = height;
  for (const EncComponent& c : comps) {
    layout.max_h = std::max(layout.max_h, c.h);
    layout.max_v = std::max(layout.max_v, c.v);
  }
  layout.mcus_x = (width + 8u * static_cast<std::uint32_t>(layout.max_h) - 1) / (8u * static_cast<std::uint32_t>(layout.max_h));
  layout.mcus_y = (height + 8u * static_cast<std::uint32_t>(layout.max_v) - 1) / (8u * static_cast<std::uint32_t>(layout.max_v));
  for (EncComponent& c : comps) {
    c.blocks_w = layout.mcus_x * static_cast<std::uint32_t>(c.h);
    c.blocks_h = layout.mcus_y * static_cast<std::uint32_t>(c.v);
    const std::uint32_t cw = (width * static_cast<std::uint32_t>(c.h) + static_cast<std::uint32_t>(layout.max_h) - 1) /
                             static_cast<std::uint32_t>(layout.max_h);
    const std::uint32_t ch = (height * static_cast<std::uint32_t>(c.v) + static_cast<std::uint32_t>(layout.max_v) - 1) /
                             static_cast<std::uint32_t>(layout.max_v);
    c.real_blocks_w = (cw + 7) / 8;
    c.real_blocks_h = (ch + 7) / 8;
  }
  return layout;
}

std::vector<EncComponent> component_set(bool grey, Subsampling subsampling) {
  std::vector<EncComponent> comps;
  EncComponent y;
  y.id = 1;
  y.tq = 0;
  y.table = 0;
  if (grey) {
    comps.push_back(y);
    return comps;
  }
  y.h = subsampling == Subsampling::S444 ? 1 : 2;
  y.v = subsampling == Subsampling::S420 ? 2 : 1;
  comps.push_back(y);
  for (int id = 2; id <= 3; ++id) {
    EncComponent c;
    c.id = id;
    c.tq = 1;
    c.table = 1;
    comps.push_back(c);
  }
  return comps;
}

Result<void> check_dimensions(std::uint32_t width, std::uint32_t height) {
  if (width == 0 || height == 0) return fail(Status::Encode, "the image has no pixels");
  if (width > 65535 || height > 65535) {
    return fail(Status::Encode, "JPEG pictures are limited to 65535x65535 pixels ({}x{} given)", width, height);
  }
  return {};
}

}  // namespace

Result<Bytes> encode(const ir::Frame& frame, const EncodeOptions& options, const Context& ctx) {
  if (!frame.valid()) return fail(Status::Encode, "the image has no pixels");
  CVT_TRY(check_dimensions(frame.width, frame.height));
  const bool grey = options.grayscale || is_grayscale(frame);
  Subsampling subsampling = options.subsampling;
  if (subsampling == Subsampling::Auto) subsampling = options.quality >= 90 ? Subsampling::S444 : Subsampling::S420;
  const std::uint32_t w = frame.width;
  const std::uint32_t h = frame.height;
  const std::size_t pixels = static_cast<std::size_t>(w) * h;
  // Colour conversion (JFIF full-range BT.601).
  std::vector<std::uint8_t> y_plane(pixels);
  std::vector<float> cb_full;
  std::vector<float> cr_full;
  if (grey && !options.grayscale) {
    for (std::size_t i = 0; i < pixels; ++i) y_plane[i] = frame.rgba[i * 4];
  } else if (grey) {
    for (std::size_t i = 0; i < pixels; ++i) {
      const float v = 0.299f * frame.rgba[i * 4] + 0.587f * frame.rgba[i * 4 + 1] + 0.114f * frame.rgba[i * 4 + 2];
      y_plane[i] = static_cast<std::uint8_t>(std::clamp(v + 0.5f, 0.0f, 255.0f));
    }
  } else {
    cb_full.resize(pixels);
    cr_full.resize(pixels);
    for (std::size_t i = 0; i < pixels; ++i) {
      const float r = frame.rgba[i * 4];
      const float g = frame.rgba[i * 4 + 1];
      const float b = frame.rgba[i * 4 + 2];
      const float yy = 0.299f * r + 0.587f * g + 0.114f * b;
      y_plane[i] = static_cast<std::uint8_t>(std::clamp(yy + 0.5f, 0.0f, 255.0f));
      cb_full[i] = -0.168735892f * r - 0.331264108f * g + 0.5f * b + 128.0f;
      cr_full[i] = 0.5f * r - 0.418687589f * g - 0.081312411f * b + 128.0f;
    }
  }
  CVT_TRY(ctx.checkpoint(0.1, "Converting colours"));
  std::vector<EncComponent> comps = component_set(grey, subsampling);
  const Layout layout = make_layout(w, h, comps);
  std::vector<SourcePlane> sources;
  sources.push_back({y_plane.data(), w, w, h});
  std::vector<std::uint8_t> cb_plane;
  std::vector<std::uint8_t> cr_plane;
  if (!grey) {
    // Box-filter the chroma down to its sampled size.
    const std::uint32_t fx = static_cast<std::uint32_t>(layout.max_h);
    const std::uint32_t fy = static_cast<std::uint32_t>(layout.max_v);
    const std::uint32_t cw = (w + fx - 1) / fx;
    const std::uint32_t ch = (h + fy - 1) / fy;
    cb_plane.resize(static_cast<std::size_t>(cw) * ch);
    cr_plane.resize(cb_plane.size());
    for (std::uint32_t y = 0; y < ch; ++y) {
      for (std::uint32_t x = 0; x < cw; ++x) {
        float sb = 0, sr = 0;
        int count = 0;
        for (std::uint32_t dy = 0; dy < fy; ++dy) {
          const std::uint32_t sy = std::min(y * fy + dy, h - 1);
          for (std::uint32_t dx = 0; dx < fx; ++dx) {
            const std::uint32_t sx = std::min(x * fx + dx, w - 1);
            sb += cb_full[static_cast<std::size_t>(sy) * w + sx];
            sr += cr_full[static_cast<std::size_t>(sy) * w + sx];
            ++count;
          }
        }
        cb_plane[static_cast<std::size_t>(y) * cw + x] = static_cast<std::uint8_t>(std::clamp(sb / static_cast<float>(count) + 0.5f, 0.0f, 255.0f));
        cr_plane[static_cast<std::size_t>(y) * cw + x] = static_cast<std::uint8_t>(std::clamp(sr / static_cast<float>(count) + 0.5f, 0.0f, 255.0f));
      }
    }
    cb_full = {};
    cr_full = {};
    sources.push_back({cb_plane.data(), cw, cw, ch});
    sources.push_back({cr_plane.data(), cw, cw, ch});
  }
  return encode_components(std::move(sources), std::move(comps), layout, options, ctx);
}

Result<Bytes> encode_planes(const PlanarInput& input, const EncodeOptions& options, const Context& ctx) {
  CVT_TRY(check_dimensions(input.width, input.height));
  if (input.y == nullptr || input.y_stride < input.width) return fail(Status::InvalidArgument, "missing luma plane");
  const bool grey = input.cb == nullptr || input.cr == nullptr;
  Subsampling subsampling = input.subsampling == Subsampling::Auto ? Subsampling::S420 : input.subsampling;
  std::vector<EncComponent> comps = component_set(grey, subsampling);
  const Layout layout = make_layout(input.width, input.height, comps);
  std::vector<SourcePlane> sources;
  sources.push_back({input.y, input.y_stride, input.width, input.height});
  if (!grey) {
    const std::uint32_t cw = (input.width + static_cast<std::uint32_t>(layout.max_h) - 1) / static_cast<std::uint32_t>(layout.max_h);
    const std::uint32_t ch = (input.height + static_cast<std::uint32_t>(layout.max_v) - 1) / static_cast<std::uint32_t>(layout.max_v);
    if (input.cb_stride < cw || input.cr_stride < cw) return fail(Status::InvalidArgument, "chroma planes are too small");
    sources.push_back({input.cb, input.cb_stride, cw, ch});
    sources.push_back({input.cr, input.cr_stride, cw, ch});
  }
  return encode_components(std::move(sources), std::move(comps), layout, options, ctx);
}

}  // namespace cvt::image::jpeg
