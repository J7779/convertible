// Convertible core - archive family: the RAR 2.9-4.x decoder (RAR 3 algorithm).
//
// The stream alternates between LZ blocks and PPMd blocks; every block
// starts byte-aligned with a "PPMd" bit. LZ blocks carry Huffman tables for
// the main (299), distance (60), low-distance (17) and repeat-length (28)
// alphabets as 4-bit deltas against the previous tables, sent through a
// 20-symbol bit-length code. Main symbols are literals, end of block (256),
// a filter program (257), "repeat the last match" (258), one of four recent
// distances (259-262), a short match of length 2 (263-270) or a match
// (271+). Distances above 9 slots take their low 4 bits from the
// low-distance code, which can also repeat the previous low part 16 times.
//
// Filters arrive as small programs for RAR's virtual machine. Only the
// standard programs that RAR itself emits exist in practice; they are
// recognised by length and CRC-32 and executed natively (x86 E8 / E8E9
// call conversion, Itanium branch conversion, byte delta, 24-bit RGB
// prediction and audio prediction). Anything else is refused.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstdlib>
#include <limits>

#include "families/archive/formats/rar_internal.hpp"
#include "util/crc.hpp"

namespace cvt::archive::rar {

namespace {

constexpr std::size_t main_size = 299;
constexpr std::size_t dist_size = 60;
constexpr std::size_t low_size = 17;
constexpr std::size_t rep_size = 28;
constexpr std::size_t table_size = main_size + dist_size + low_size + rep_size;
constexpr std::uint32_t vm_memory = 0x40000;
constexpr std::size_t max_filters = 8192;
constexpr std::uint64_t match_margin = 0x1100;

constexpr std::uint8_t length_base[28] = {0,  1,  2,  3,  4,  5,  6,  7,  8,   10,  12,  14,  16,  20,
                                          24, 28, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224};
constexpr std::uint8_t length_bits[28] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5};
constexpr std::uint8_t short_base[8] = {0, 4, 8, 16, 32, 64, 128, 192};
constexpr std::uint8_t short_bits[8] = {2, 2, 3, 4, 5, 6, 6, 6};

struct DistanceTable {
  std::uint32_t base[dist_size] = {};
  std::uint8_t bits[dist_size] = {};
};
constexpr DistanceTable make_distances() {
  // Number of slots for each count of extra bits: pairs up to 15 bits, 14 slots of
  // 16 bits (up to 1 MiB), then 12 slots of 18 bits (up to the 4 MiB window).
  constexpr std::uint8_t counts[] = {4, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 14, 0, 12};
  DistanceTable t;
  std::uint32_t base = 0;
  std::size_t slot = 0;
  for (std::size_t bits = 0; bits < sizeof(counts) && slot < dist_size; ++bits) {
    for (std::uint8_t j = 0; j < counts[bits] && slot < dist_size; ++j, ++slot) {
      t.base[slot] = base;
      t.bits[slot] = static_cast<std::uint8_t>(bits);
      base += 1u << bits;
    }
  }
  return t;
}
constexpr DistanceTable distances = make_distances();

Result<void> damaged() { return fail(Status::Decode, "RAR compressed data is damaged"); }

/// RAR VM's variable-length number: 2 selector bits, then 4, 8 (or a
/// negative 8-bit value), 16 or 32 bits.
std::uint32_t read_vm_number(BitInput& in) {
  const std::uint32_t v = in.peek(16);
  switch (v & 0xC000u) {
    case 0:
      in.skip(6);
      return (v >> 10) & 0xFu;
    case 0x4000:
      if ((v & 0x3C00u) == 0) {
        in.skip(14);
        return 0xFFFFFF00u | ((v >> 2) & 0xFFu);
      }
      in.skip(10);
      return (v >> 6) & 0xFFu;
    case 0x8000:
      in.skip(2);
      return in.get(16);
    default:
      in.skip(2);
      return in.get_wide(32);
  }
}

std::uint32_t load32(const std::uint8_t* p) noexcept {
  return std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16) | (std::uint32_t{p[3]} << 24);
}
void store32(std::uint8_t* p, std::uint32_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
  p[2] = static_cast<std::uint8_t>(v >> 16);
  p[3] = static_cast<std::uint8_t>(v >> 24);
}

// Itanium bundles: bit fields at arbitrary bit offsets of a 16-byte bundle.
std::uint32_t bundle_bits(const std::uint8_t* d, std::uint32_t at, std::uint32_t count) noexcept {
  const std::uint32_t byte = at / 8;
  const std::uint32_t v = load32(d + byte) >> (at & 7u);
  return v & (0xFFFFFFFFu >> (32 - count));
}
void set_bundle_bits(std::uint8_t* d, std::uint32_t value, std::uint32_t at, std::uint32_t count) noexcept {
  const std::uint32_t byte = at / 8;
  const std::uint32_t shift = at & 7u;
  std::uint32_t mask = ~((0xFFFFFFFFu >> (32 - count)) << shift);
  value <<= shift;
  for (std::uint32_t i = 0; i < 4; ++i) {
    d[byte + i] = static_cast<std::uint8_t>((d[byte + i] & mask) | value);
    mask = (mask >> 8) | 0xFF000000u;
    value >>= 8;
  }
}

struct BitPpmdInput final : PpmdInput {
  explicit BitPpmdInput(BitInput& in) : bits(&in) {}
  std::uint8_t next() override { return bits->byte(); }
  BitInput* bits;
};

}  // namespace

Unpack3::FilterType identify_vm_filter(std::size_t length, std::uint32_t crc) noexcept {
  struct Known {
    std::size_t length;
    std::uint32_t crc;
    Unpack3::FilterType type;
  };
  constexpr Known known[] = {
      {53, 0xAD576887u, Unpack3::FilterType::E8},       {57, 0x3CD7E57Eu, Unpack3::FilterType::E8E9},
      {120, 0x3769893Fu, Unpack3::FilterType::Itanium}, {29, 0x0E06077Du, Unpack3::FilterType::Delta},
      {149, 0x1C2C5DC8u, Unpack3::FilterType::Rgb},     {216, 0xBC85E701u, Unpack3::FilterType::Audio},
  };
  for (const Known& k : known) {
    if (k.length == length && k.crc == crc) return k.type;
  }
  return Unpack3::FilterType::None;
}

void Unpack3::init(std::uint64_t window) {
  window_.allocate(std::max<std::uint64_t>(window, 1));
  tables_ = false;
  ppm_block_ = false;
  old_table_ = {};
  old_ = {};
  last_dist_ = 0;
  last_length_ = 0;
  esc_char_ = 2;
  reset_filters(true);
  written_ = 0;
  file_start_ = 0;
}

void Unpack3::reset_filters(bool all) {
  if (all) {
    programs_.clear();
    old_lengths_.clear();
    last_filter_ = 0;
  }
  stack_.clear();
}

Result<void> Unpack3::start_ppmd(BitInput& in, std::uint64_t memory_budget) {
  in.align();
  const std::uint32_t flags = in.byte();
  const bool reset = (flags & 0x20u) != 0;
  std::uint32_t megabytes = 0;
  if (reset) megabytes = in.byte();
  if ((flags & 0x40u) != 0) esc_char_ = in.byte();
  if (!ppmd_) ppmd_ = make_ppmd_decoder();
  if (!ppmd_) return fail(Status::Decode, "this RAR archive uses PPMd compression: PPMd blocks are not available in this build");
  if (reset) {
    std::uint32_t order = (flags & 0x1Fu) + 1;
    if (order > 16) order = 16 + (order - 16) * 3;
    if (order == 1) return damaged();
    const std::uint64_t memory = (std::uint64_t{megabytes} + 1) << 20;
    if (memory > memory_budget) {
      return fail(Status::Limit, "the RAR PPMd model needs {} MB, more than the remaining limit", megabytes + 1);
    }
    if (!ppmd_->restart(order, megabytes + 1)) return damaged();
  } else if (!ppmd_->has_model()) {
    return damaged();
  }
  BitPpmdInput input(in);
  ppmd_->init_coder(input);
  ppm_block_ = true;
  return {};
}

Result<void> Unpack3::read_tables(BitInput& in, std::uint64_t memory_budget) {
  in.align();
  if (in.peek(1) != 0) {
    CVT_TRY(start_ppmd(in, memory_budget));
    tables_ = true;
    return {};
  }
  ppm_block_ = false;
  prev_low_ = 0;
  low_rep_ = 0;
  if (in.peek(2) == 0) old_table_ = {};  // bit 2 clear: lengths are not deltas
  in.skip(2);
  std::array<std::uint8_t, 20> bit_lengths{};
  for (std::size_t i = 0; i < bit_lengths.size();) {
    const auto length = static_cast<std::uint8_t>(in.get(4));
    if (length == 15) {
      std::uint32_t zeros = in.get(4);
      if (zeros != 0) {
        for (zeros += 2; zeros > 0 && i < bit_lengths.size(); --zeros) bit_lengths[i++] = 0;
        continue;
      }
    }
    bit_lengths[i++] = length;
  }
  compression::detail::HuffmanTable bits;
  if (!bits.build(bit_lengths, false)) return damaged();
  std::array<std::uint8_t, table_size> table{};
  for (std::size_t i = 0; i < table_size;) {
    const int sym = bits.decode(in);
    if (sym < 0) return damaged();
    if (sym < 16) {
      table[i] = static_cast<std::uint8_t>((sym + old_table_[i]) & 0xF);
      ++i;
    } else if (sym < 18) {
      if (i == 0) return damaged();
      std::uint32_t n = sym == 16 ? in.get(3) + 3 : in.get(7) + 11;
      for (; n > 0 && i < table_size; --n, ++i) table[i] = table[i - 1];
    } else {
      std::uint32_t n = sym == 18 ? in.get(3) + 3 : in.get(7) + 11;
      for (; n > 0 && i < table_size; --n) table[i++] = 0;
    }
    if (in.overrun()) return fail(Status::Decode, "RAR compressed data is truncated");
  }
  const std::span<const std::uint8_t> all(table);
  if (!main_.build(all.subspan(0, main_size), false) || !dist_.build(all.subspan(main_size, dist_size), false) ||
      !low_.build(all.subspan(main_size + dist_size, low_size), false) ||
      !rep_.build(all.subspan(main_size + dist_size + low_size, rep_size), false)) {
    return damaged();
  }
  old_table_ = table;
  tables_ = true;
  return {};
}

Result<bool> Unpack3::read_end_of_block(BitInput& in, std::uint64_t memory_budget) {
  bool new_table = false;
  bool new_file = false;
  if (in.get(1) != 0) {
    new_table = true;
  } else {
    new_file = true;
    new_table = in.get(1) != 0;
  }
  tables_ = !new_table;
  if (new_file) return false;
  CVT_TRY(read_tables(in, memory_budget));
  return true;
}

Result<void> Unpack3::read_vm_code(BitInput& in) {
  const std::uint32_t first = in.get(8);
  std::uint32_t length = (first & 7u) + 1;
  if (length == 7) {
    length = in.get(8) + 7;
  } else if (length == 8) {
    length = in.get(16);
  }
  if (length == 0) return damaged();
  Bytes code(length);
  for (std::byte& b : code) b = static_cast<std::byte>(in.get(8));
  if (in.overrun()) return fail(Status::Decode, "RAR compressed data is truncated");
  return add_vm_code(first, code);
}

Result<void> Unpack3::read_vm_code_ppm() {
  BitPpmdInput input(*ppm_in_);
  const auto next = [&]() { return ppmd_->decode(input); };
  const int first = next();
  if (first < 0) return damaged();
  std::uint32_t length = (static_cast<std::uint32_t>(first) & 7u) + 1;
  if (length == 7) {
    const int b = next();
    if (b < 0) return damaged();
    length = static_cast<std::uint32_t>(b) + 7;
  } else if (length == 8) {
    const int hi = next();
    const int lo = next();
    if (hi < 0 || lo < 0) return damaged();
    length = static_cast<std::uint32_t>(hi) * 256 + static_cast<std::uint32_t>(lo);
  }
  if (length == 0) return damaged();
  Bytes code(length);
  for (std::byte& b : code) {
    const int c = next();
    if (c < 0) return damaged();
    b = static_cast<std::byte>(c);
  }
  return add_vm_code(static_cast<std::uint32_t>(first), code);
}

Result<void> Unpack3::add_vm_code(std::uint32_t first, ByteView code) {
  BitInput v(code);
  std::size_t index = 0;
  if ((first & 0x80u) != 0) {
    index = read_vm_number(v);
    if (index == 0) {
      reset_filters(true);
    } else {
      --index;
    }
  } else {
    index = last_filter_;
  }
  if (index > programs_.size() || index > old_lengths_.size()) return damaged();
  last_filter_ = index;
  const bool fresh = index == programs_.size();
  if (fresh) {
    if (index >= max_filters) return damaged();
    programs_.push_back({});
    old_lengths_.push_back(0);
  }
  Pending p;
  p.program = index;
  std::uint32_t start = read_vm_number(v);
  if ((first & 0x40u) != 0) start += 258;
  p.start = window_.pos() + start;
  if ((first & 0x20u) != 0) {
    p.length = read_vm_number(v);
    old_lengths_[index] = p.length;
  } else {
    p.length = old_lengths_[index];
  }
  if ((first & 0x10u) != 0) {
    const std::uint32_t mask = v.get(7);
    for (std::size_t i = 0; i < 7; ++i) {
      if ((mask & (1u << i)) != 0) p.regs[i] = read_vm_number(v);
    }
  }
  if (fresh) {
    const std::uint32_t size = read_vm_number(v);
    if (size == 0 || size >= 0x10000 || v.position() / 8 + size > code.size()) return damaged();
    Bytes program(size);
    for (std::byte& b : program) b = static_cast<std::byte>(v.get(8));
    const FilterType type = identify_vm_filter(size, util::crc32(program));
    if (type == FilterType::None) {
      return fail(Status::Decode, "unsupported RAR VM filter program (length {}, CRC-32 {:08X})", size, util::crc32(program));
    }
    programs_[index].type = type;
  }
  if ((first & 0x08u) != 0) {
    const std::uint32_t size = read_vm_number(v);
    if (size > 0x2000 - 0x40) return damaged();  // global data, unused by the standard filters
  }
  if (v.overrun()) return damaged();
  if (p.length > vm_memory) return damaged();
  if (stack_.size() >= max_filters) return damaged();
  stack_.push_back(p);
  return {};
}

Result<void> Unpack3::run_filter(const Pending& p, std::vector<std::uint8_t>& data, std::uint64_t file_offset) const {
  const auto size = static_cast<std::uint32_t>(data.size());
  std::uint8_t* d = data.data();
  const FilterType type = programs_[p.program].type;
  switch (type) {
    case FilterType::E8:
    case FilterType::E8E9: {
      if (size < 4) return {};
      constexpr std::uint32_t file_size = 0x1000000;
      const auto base = static_cast<std::uint32_t>(file_offset);
      for (std::uint32_t pos = 0; pos < size - 4;) {
        const std::uint8_t op = d[pos++];
        if (op == 0xE8 || (type == FilterType::E8E9 && op == 0xE9)) {
          const std::uint32_t offset = pos + base;
          const std::uint32_t addr = load32(d + pos);
          if ((addr & 0x80000000u) != 0) {
            if (((addr + offset) & 0x80000000u) == 0) store32(d + pos, addr + file_size);
          } else if (((addr - file_size) & 0x80000000u) != 0) {
            store32(d + pos, addr - offset);
          }
          pos += 4;
        }
      }
      return {};
    }
    case FilterType::Itanium: {
      if (size < 21) return {};
      constexpr std::uint8_t masks[16] = {4, 4, 6, 6, 0, 0, 7, 7, 4, 4, 0, 0, 4, 4, 0, 0};
      auto bundle = static_cast<std::uint32_t>(file_offset >> 4);
      for (std::uint32_t pos = 0; pos < size - 21; pos += 16, ++bundle) {
        const int tmpl = static_cast<int>(d[pos] & 0x1Fu) - 0x10;
        if (tmpl < 0) continue;
        const std::uint8_t mask = masks[tmpl];
        if (mask == 0) continue;
        for (std::uint32_t slot = 0; slot <= 2; ++slot) {
          if ((mask & (1u << slot)) == 0) continue;
          const std::uint32_t at = slot * 41 + 5;
          if (bundle_bits(d + pos, at + 37, 4) == 5) {
            const std::uint32_t offset = bundle_bits(d + pos, at + 13, 20);
            set_bundle_bits(d + pos, (offset - bundle) & 0xFFFFFu, at + 13, 20);
          }
        }
      }
      return {};
    }
    case FilterType::Delta: {
      const std::uint32_t channels = p.regs[0];
      if (size > vm_memory / 2 || channels == 0) return damaged();
      std::vector<std::uint8_t> out(size);
      std::size_t src = 0;
      for (std::uint32_t ch = 0; ch < channels; ++ch) {
        std::uint8_t prev = 0;
        for (std::uint32_t i = ch; i < size; i += channels) {
          prev = static_cast<std::uint8_t>(prev - d[src++]);
          out[i] = prev;
        }
      }
      data.swap(out);
      return {};
    }
    case FilterType::Rgb: {
      const std::uint32_t width = p.regs[0] - 3;
      const std::uint32_t red = p.regs[1];
      if (size > vm_memory / 2 || size < 3 || width > size || red > 2 || p.regs[0] < 3) return damaged();
      std::vector<std::uint8_t> out(size);
      std::size_t src = 0;
      for (std::uint32_t ch = 0; ch < 3; ++ch) {
        int prev = 0;
        for (std::uint32_t i = ch; i < size; i += 3) {
          int predicted = prev;
          if (i >= width + 3) {
            const int up = out[i - width];
            const int up_left = out[i - width - 3];
            const int estimate = prev + up - up_left;
            const int pa = std::abs(estimate - prev);
            const int pb = std::abs(estimate - up);
            const int pc = std::abs(estimate - up_left);
            predicted = pa <= pb && pa <= pc ? prev : (pb <= pc ? up : up_left);
          }
          prev = (predicted - d[src++]) & 0xFF;
          out[i] = static_cast<std::uint8_t>(prev);
        }
      }
      for (std::uint32_t i = red; i + 2 < size; i += 3) {
        const std::uint8_t g = out[i + 1];
        out[i] = static_cast<std::uint8_t>(out[i] + g);
        out[i + 2] = static_cast<std::uint8_t>(out[i + 2] + g);
      }
      data.swap(out);
      return {};
    }
    case FilterType::Audio: {
      const std::uint32_t channels = p.regs[0];
      if (size > vm_memory / 2 || channels == 0) return damaged();
      std::vector<std::uint8_t> out(size);
      std::size_t src = 0;
      for (std::uint32_t ch = 0; ch < channels; ++ch) {
        int prev = 0;
        int prev_delta = 0;
        int d1 = 0;
        int d2 = 0;
        int d3 = 0;
        int k1 = 0;
        int k2 = 0;
        int k3 = 0;
        std::uint32_t dif[7] = {};
        std::uint32_t count = 0;
        for (std::uint32_t i = ch; i < size; i += channels, ++count) {
          d3 = d2;
          d2 = prev_delta - d1;
          d1 = prev_delta;
          const int predicted = ((8 * prev + k1 * d1 + k2 * d2 + k3 * d3) >> 3) & 0xFF;
          const int cur = d[src++];
          const int value = (predicted - cur) & 0xFF;
          out[i] = static_cast<std::uint8_t>(value);
          prev_delta = static_cast<std::int8_t>(static_cast<std::uint8_t>(value - prev));
          prev = value;
          const int e = static_cast<std::int8_t>(static_cast<std::uint8_t>(cur)) * 8;
          dif[0] += static_cast<std::uint32_t>(std::abs(e));
          dif[1] += static_cast<std::uint32_t>(std::abs(e - d1));
          dif[2] += static_cast<std::uint32_t>(std::abs(e + d1));
          dif[3] += static_cast<std::uint32_t>(std::abs(e - d2));
          dif[4] += static_cast<std::uint32_t>(std::abs(e + d2));
          dif[5] += static_cast<std::uint32_t>(std::abs(e - d3));
          dif[6] += static_cast<std::uint32_t>(std::abs(e + d3));
          if ((count & 0x1Fu) == 0) {
            std::uint32_t best = dif[0];
            int which = 0;
            dif[0] = 0;
            for (int j = 1; j < 7; ++j) {
              if (dif[j] < best) {
                best = dif[j];
                which = j;
              }
              dif[j] = 0;
            }
            switch (which) {
              case 1: if (k1 >= -16) --k1; break;
              case 2: if (k1 < 16) ++k1; break;
              case 3: if (k2 >= -16) --k2; break;
              case 4: if (k2 < 16) ++k2; break;
              case 5: if (k3 >= -16) --k3; break;
              case 6: if (k3 < 16) ++k3; break;
              default: break;
            }
          }
        }
      }
      data.swap(out);
      return {};
    }
    case FilterType::None:
      break;
  }
  return damaged();
}

void Unpack3::emit(FileOutput& out, const std::uint8_t* data, std::size_t count) {
  const auto* p = reinterpret_cast<const std::byte*>(data);
  out.bytes->insert(out.bytes->end(), p, p + count);
}

Result<void> Unpack3::flush(FileOutput& out) {
  const std::uint64_t file_end = file_start_ + out.size;
  const std::uint64_t pos = window_.pos();
  while (true) {
    const std::uint64_t limit = std::min(pos, file_end);
    if (stack_.empty() || stack_.front().start >= pos) {
      if (limit > written_) {
        const auto n = static_cast<std::size_t>(limit - written_);
        scratch_.resize(n);
        window_.read(written_, n, scratch_.data());
        emit(out, scratch_.data(), n);
        written_ = limit;
      }
      return {};
    }
    const Pending p = stack_.front();
    if (p.start < written_) return damaged();
    if (written_ < p.start) {
      const std::uint64_t to = std::min(p.start, limit);
      if (to <= written_) return {};
      const auto n = static_cast<std::size_t>(to - written_);
      scratch_.resize(n);
      window_.read(written_, n, scratch_.data());
      emit(out, scratch_.data(), n);
      written_ = to;
      continue;
    }
    if (pos < p.start + p.length) return {};
    scratch_.resize(p.length);
    window_.read(p.start, p.length, scratch_.data());
    const std::uint64_t offset = p.start - file_start_;
    CVT_TRY(run_filter(p, scratch_, offset));
    stack_.erase(stack_.begin());
    // Further filters on the same block apply to the filtered output.
    while (!stack_.empty() && stack_.front().start == p.start && stack_.front().length == scratch_.size()) {
      CVT_TRY(run_filter(stack_.front(), scratch_, offset));
      stack_.erase(stack_.begin());
    }
    const std::uint64_t keep = file_end > written_ ? std::min<std::uint64_t>(scratch_.size(), file_end - written_) : 0;
    emit(out, scratch_.data(), static_cast<std::size_t>(keep));
    written_ = p.start + p.length;
  }
}

Result<int> Unpack3::decode_ppmd_lz(BitInput& in, std::uint64_t memory_budget, std::uint64_t /*file_end*/) {
  BitPpmdInput input(in);
  const int c = ppmd_->decode(input);
  if (c < 0) return fail(Status::Decode, "RAR PPMd data is damaged");
  if (c != esc_char_) {
    window_.put(static_cast<std::uint8_t>(c));
    return 0;
  }
  const int next = ppmd_->decode(input);
  if (next < 0) return fail(Status::Decode, "RAR PPMd data is damaged");
  switch (next) {
    case 0:
      CVT_TRY(read_tables(in, memory_budget));
      return 0;
    case 2:
      return 1;  // end of file
    case 3:
      ppm_in_ = &in;
      CVT_TRY(read_vm_code_ppm());
      return 0;
    case 4: {
      std::uint32_t distance = 0;
      std::uint32_t length = 0;
      for (int i = 0; i < 4; ++i) {
        const int b = ppmd_->decode(input);
        if (b < 0) return fail(Status::Decode, "RAR PPMd data is damaged");
        if (i == 3) {
          length = static_cast<std::uint32_t>(b);
        } else {
          distance = (distance << 8) | static_cast<std::uint32_t>(b);
        }
      }
      if (!window_.copy(std::uint64_t{distance} + 2, length + 32)) return fail(Status::Decode, "RAR PPMd data is damaged");
      return 0;
    }
    case 5: {
      const int length = ppmd_->decode(input);
      if (length < 0) return fail(Status::Decode, "RAR PPMd data is damaged");
      if (!window_.copy(1, static_cast<std::uint32_t>(length) + 4)) return fail(Status::Decode, "RAR PPMd data is damaged");
      return 0;
    }
    default:
      window_.put(static_cast<std::uint8_t>(c));
      return 0;
  }
}

Result<void> Unpack3::decode(ByteView packed, bool solid, FileOutput& out, std::uint64_t memory_budget) {
  if (!solid) {
    tables_ = false;
    ppm_block_ = false;
    old_table_ = {};
    old_ = {};
    last_dist_ = 0;
    last_length_ = 0;
    esc_char_ = 2;
    reset_filters(true);
    window_.restart();
    written_ = window_.pos();
  } else {
    reset_filters(false);
  }
  file_start_ = written_;
  out.bytes->reserve(static_cast<std::size_t>(out.size));
  const std::uint64_t file_end = file_start_ + out.size;

  BitInput in(packed);
  if (!tables_) CVT_TRY(read_tables(in, memory_budget));
  const std::uint64_t capacity = window_.capacity();
  std::uint32_t tick = 0;
  while (true) {
    if (in.overrun()) {
      if (window_.pos() >= file_end) break;
      return fail(Status::Decode, "RAR compressed data is truncated");
    }
    if (window_.pos() - written_ >= capacity - match_margin) {
      CVT_TRY(flush(out));
      if (window_.pos() - written_ >= capacity - match_margin) {
        return fail(Status::Decode, "RAR data is longer than its declared size or uses an oversized filter");
      }
    }
    if ((++tick & 0xFFFFu) == 0) {
      const double done =
          out.size != 0 ? static_cast<double>(window_.pos() - file_start_) / static_cast<double>(out.size) : 0.0;
      if (!out.progress.report(std::min(done, 1.0))) return fail(Status::Cancelled, "the conversion was cancelled");
    }
    if (ppm_block_) {
      CVT_TRY_ASSIGN(const int state, decode_ppmd_lz(in, memory_budget, file_end));
      if (state == 1) break;
      continue;
    }

    const int sym = main_.decode(in);
    if (sym < 0) return damaged();
    if (sym < 256) {
      window_.put(static_cast<std::uint8_t>(sym));
      continue;
    }
    if (sym >= 271) {
      const auto slot = static_cast<std::size_t>(sym - 271);
      if (slot >= 28) return damaged();
      std::uint32_t length = length_base[slot] + 3u;
      if (length_bits[slot] > 0) length += in.get(length_bits[slot]);
      const int dslot = dist_.decode(in);
      if (dslot < 0) return damaged();
      std::uint64_t distance = distances.base[dslot] + 1u;
      const int bits = distances.bits[dslot];
      if (bits > 0) {
        if (dslot > 9) {
          if (bits > 4) distance += std::uint64_t{in.get(bits - 4)} << 4;
          if (low_rep_ > 0) {
            --low_rep_;
            distance += prev_low_;
          } else {
            const int low = low_.decode(in);
            if (low < 0) return damaged();
            if (low == 16) {
              low_rep_ = 15;
              distance += prev_low_;
            } else {
              distance += static_cast<std::uint64_t>(low);
              prev_low_ = static_cast<std::uint32_t>(low);
            }
          }
        } else {
          distance += in.get(bits);
        }
      }
      if (distance >= 0x2000) {
        ++length;
        if (distance >= 0x40000) ++length;
      }
      old_[3] = old_[2];
      old_[2] = old_[1];
      old_[1] = old_[0];
      old_[0] = distance;
      last_dist_ = distance;
      last_length_ = length;
      if (!window_.copy(distance, length)) return damaged();
      continue;
    }
    if (sym == 256) {
      CVT_TRY_ASSIGN(const bool more, read_end_of_block(in, memory_budget));
      if (!more) break;
      continue;
    }
    if (sym == 257) {
      CVT_TRY(read_vm_code(in));
      continue;
    }
    if (sym == 258) {
      if (last_length_ != 0 && !window_.copy(last_dist_, last_length_)) return damaged();
      continue;
    }
    if (sym < 263) {
      const auto index = static_cast<std::size_t>(sym - 259);
      const std::uint64_t distance = old_[index];
      for (std::size_t i = index; i > 0; --i) old_[i] = old_[i - 1];
      old_[0] = distance;
      const int lslot = rep_.decode(in);
      if (lslot < 0 || lslot >= 28) return damaged();
      std::uint32_t length = length_base[lslot] + 2u;
      if (length_bits[lslot] > 0) length += in.get(length_bits[lslot]);
      last_dist_ = distance;
      last_length_ = length;
      if (!window_.copy(distance, length)) return damaged();
      continue;
    }
    const auto index = static_cast<std::size_t>(sym - 263);
    std::uint64_t distance = short_base[index] + 1u;
    distance += in.get(short_bits[index]);
    old_[3] = old_[2];
    old_[2] = old_[1];
    old_[1] = old_[0];
    old_[0] = distance;
    last_dist_ = distance;
    last_length_ = 2;
    if (!window_.copy(distance, 2)) return damaged();
  }
  CVT_TRY(flush(out));
  written_ = window_.pos();  // a final match may run past the end; that output is dropped
  if (out.bytes->size() != out.size) {
    return fail(Status::Decode, "RAR data decompressed to {} bytes instead of {}", out.bytes->size(), out.size);
  }
  return {};
}

}  // namespace cvt::archive::rar
