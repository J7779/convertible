// Convertible core - archive family: branch-converter (BCJ) and delta filters, and 7z BCJ2.
//
// The branch converters turn relative branch targets into absolute ones
// (encode) and back (decode) so that repeated calls to the same function
// compress better. The algorithms are the ones defined by the xz and 7z
// formats for x86, PowerPC, IA-64, ARM, ARM-Thumb, ARM64 and SPARC.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>

#include "families/archive/codec_impl.hpp"

namespace cvt::compression::detail {

namespace {

inline std::uint8_t& at(std::span<std::byte> d, std::size_t i) { return reinterpret_cast<std::uint8_t&>(d[i]); }

bool x86_ms_byte(std::uint8_t b) { return b == 0 || b == 0xFF; }

std::size_t x86(std::span<std::byte> buf, std::uint32_t now, bool encode, FilterState& st) {
  static constexpr std::array<bool, 8> allowed = {true, true, true, false, true, false, false, false};
  static constexpr std::array<std::uint32_t, 8> bit_number = {0, 1, 2, 2, 3, 3, 3, 3};
  const std::size_t size = buf.size();
  if (size < 5) return 0;
  std::uint32_t prev_mask = st.x86_mask;
  std::uint32_t prev_pos = st.history_pos;  // reused as the position of the previous E8/E9
  if (now - prev_pos > 5) prev_pos = now - 5;
  const std::size_t limit = size - 5;
  std::size_t i = 0;
  while (i <= limit) {
    std::uint8_t b = at(buf, i);
    if (b != 0xE8 && b != 0xE9) {
      ++i;
      continue;
    }
    const std::uint32_t offset = now + static_cast<std::uint32_t>(i) - prev_pos;
    prev_pos = now + static_cast<std::uint32_t>(i);
    if (offset > 5) {
      prev_mask = 0;
    } else {
      for (std::uint32_t k = 0; k < offset; ++k) {
        prev_mask &= 0x77;
        prev_mask <<= 1;
      }
    }
    b = at(buf, i + 4);
    if (x86_ms_byte(b) && allowed[(prev_mask >> 1) & 7] && (prev_mask >> 1) < 0x10) {
      std::uint32_t src = (static_cast<std::uint32_t>(b) << 24) | (static_cast<std::uint32_t>(at(buf, i + 3)) << 16) |
                          (static_cast<std::uint32_t>(at(buf, i + 2)) << 8) | at(buf, i + 1);
      std::uint32_t dest = 0;
      while (true) {
        const std::uint32_t here = now + static_cast<std::uint32_t>(i) + 5;
        dest = encode ? src + here : src - here;
        if (prev_mask == 0) break;
        const std::uint32_t idx = bit_number[prev_mask >> 1];
        b = static_cast<std::uint8_t>(dest >> (24 - idx * 8));
        if (!x86_ms_byte(b)) break;
        src = dest ^ ((1u << (32 - idx * 8)) - 1u);
      }
      at(buf, i + 4) = static_cast<std::uint8_t>(~(((dest >> 24) & 1u) - 1u));
      at(buf, i + 3) = static_cast<std::uint8_t>(dest >> 16);
      at(buf, i + 2) = static_cast<std::uint8_t>(dest >> 8);
      at(buf, i + 1) = static_cast<std::uint8_t>(dest);
      i += 5;
      prev_mask = 0;
    } else {
      ++i;
      prev_mask |= 1;
      if (x86_ms_byte(b)) prev_mask |= 0x10;
    }
  }
  st.x86_mask = prev_mask;
  st.history_pos = prev_pos;
  return i;
}

std::size_t arm(std::span<std::byte> buf, std::uint32_t now, bool encode) {
  std::size_t i = 0;
  for (; i + 4 <= buf.size(); i += 4) {
    if (at(buf, i + 3) != 0xEB) continue;
    std::uint32_t src = (static_cast<std::uint32_t>(at(buf, i + 2)) << 16) | (static_cast<std::uint32_t>(at(buf, i + 1)) << 8) |
                        at(buf, i);
    src <<= 2;
    const std::uint32_t here = now + static_cast<std::uint32_t>(i) + 8;
    std::uint32_t dest = encode ? src + here : src - here;
    dest >>= 2;
    at(buf, i + 2) = static_cast<std::uint8_t>(dest >> 16);
    at(buf, i + 1) = static_cast<std::uint8_t>(dest >> 8);
    at(buf, i) = static_cast<std::uint8_t>(dest);
  }
  return i;
}

std::size_t arm_thumb(std::span<std::byte> buf, std::uint32_t now, bool encode) {
  std::size_t i = 0;
  for (; i + 4 <= buf.size(); i += 2) {
    if ((at(buf, i + 1) & 0xF8) != 0xF0 || (at(buf, i + 3) & 0xF8) != 0xF8) continue;
    std::uint32_t src = ((static_cast<std::uint32_t>(at(buf, i + 1)) & 7u) << 19) | (static_cast<std::uint32_t>(at(buf, i)) << 11) |
                        ((static_cast<std::uint32_t>(at(buf, i + 3)) & 7u) << 8) | at(buf, i + 2);
    src <<= 1;
    const std::uint32_t here = now + static_cast<std::uint32_t>(i) + 4;
    std::uint32_t dest = encode ? src + here : src - here;
    dest >>= 1;
    at(buf, i + 1) = static_cast<std::uint8_t>(0xF0 | ((dest >> 19) & 7u));
    at(buf, i) = static_cast<std::uint8_t>(dest >> 11);
    at(buf, i + 3) = static_cast<std::uint8_t>(0xF8 | ((dest >> 8) & 7u));
    at(buf, i + 2) = static_cast<std::uint8_t>(dest);
    i += 2;
  }
  return i;
}

std::size_t powerpc(std::span<std::byte> buf, std::uint32_t now, bool encode) {
  std::size_t i = 0;
  for (; i + 4 <= buf.size(); i += 4) {
    if ((at(buf, i) >> 2) != 0x12 || (at(buf, i + 3) & 3) != 1) continue;
    const std::uint32_t src = ((static_cast<std::uint32_t>(at(buf, i)) & 3u) << 24) | (static_cast<std::uint32_t>(at(buf, i + 1)) << 16) |
                              (static_cast<std::uint32_t>(at(buf, i + 2)) << 8) | (static_cast<std::uint32_t>(at(buf, i + 3)) & ~3u);
    const std::uint32_t here = now + static_cast<std::uint32_t>(i);
    const std::uint32_t dest = encode ? src + here : src - here;
    at(buf, i) = static_cast<std::uint8_t>(0x48 | ((dest >> 24) & 3u));
    at(buf, i + 1) = static_cast<std::uint8_t>(dest >> 16);
    at(buf, i + 2) = static_cast<std::uint8_t>(dest >> 8);
    at(buf, i + 3) = static_cast<std::uint8_t>((at(buf, i + 3) & 3u) | (dest & 0xFCu));
  }
  return i;
}

std::size_t sparc(std::span<std::byte> buf, std::uint32_t now, bool encode) {
  std::size_t i = 0;
  for (; i + 4 <= buf.size(); i += 4) {
    const std::uint8_t b0 = at(buf, i);
    const std::uint8_t b1 = at(buf, i + 1);
    if (!((b0 == 0x40 && (b1 & 0xC0) == 0) || (b0 == 0x7F && (b1 & 0xC0) == 0xC0))) continue;
    std::uint32_t src = (static_cast<std::uint32_t>(b0) << 24) | (static_cast<std::uint32_t>(b1) << 16) |
                        (static_cast<std::uint32_t>(at(buf, i + 2)) << 8) | at(buf, i + 3);
    src <<= 2;
    const std::uint32_t here = now + static_cast<std::uint32_t>(i);
    std::uint32_t dest = encode ? src + here : src - here;
    dest >>= 2;
    dest = (((0u - ((dest >> 22) & 1u)) << 22) & 0x3FFFFFFFu) | (dest & 0x3FFFFFu) | 0x40000000u;
    at(buf, i) = static_cast<std::uint8_t>(dest >> 24);
    at(buf, i + 1) = static_cast<std::uint8_t>(dest >> 16);
    at(buf, i + 2) = static_cast<std::uint8_t>(dest >> 8);
    at(buf, i + 3) = static_cast<std::uint8_t>(dest);
  }
  return i;
}

std::size_t arm64(std::span<std::byte> buf, std::uint32_t now, bool encode) {
  std::size_t i = 0;
  for (; i + 4 <= buf.size(); i += 4) {
    std::uint32_t pc = now + static_cast<std::uint32_t>(i);
    std::uint32_t instr = at(buf, i) | (static_cast<std::uint32_t>(at(buf, i + 1)) << 8) |
                          (static_cast<std::uint32_t>(at(buf, i + 2)) << 16) | (static_cast<std::uint32_t>(at(buf, i + 3)) << 24);
    if ((instr >> 26) == 0x25) {
      const std::uint32_t src = instr;
      instr = 0x94000000u;
      pc >>= 2;
      if (!encode) pc = 0u - pc;
      instr |= (src + pc) & 0x03FFFFFFu;
    } else if ((instr & 0x9F000000u) == 0x90000000u) {
      const std::uint32_t src = ((instr >> 29) & 3u) | ((instr >> 3) & 0x001FFFFCu);
      if (((src + 0x00020000u) & 0x001C0000u) != 0) continue;
      instr &= 0x9000001Fu;
      pc >>= 12;
      if (!encode) pc = 0u - pc;
      const std::uint32_t dest = src + pc;
      instr |= (dest & 3u) << 29;
      instr |= (dest & 0x0003FFFCu) << 3;
      instr |= (0u - (dest & 0x00020000u)) & 0x00E00000u;
    } else {
      continue;
    }
    for (int k = 0; k < 4; ++k) at(buf, i + static_cast<std::size_t>(k)) = static_cast<std::uint8_t>(instr >> (8 * k));
  }
  return i;
}

std::size_t ia64(std::span<std::byte> buf, std::uint32_t now, bool encode) {
  static constexpr std::array<std::uint32_t, 32> branch = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                                           4, 4, 6, 6, 0, 0, 7, 7, 4, 4, 0, 0, 4, 4, 0, 0};
  std::size_t i = 0;
  for (; i + 16 <= buf.size(); i += 16) {
    const std::uint32_t mask = branch[at(buf, i) & 0x1Fu];
    std::uint32_t bit_pos = 5;
    for (std::size_t slot = 0; slot < 3; ++slot, bit_pos += 41) {
      if (((mask >> slot) & 1u) == 0) continue;
      const std::size_t byte_pos = bit_pos >> 3;
      const std::uint32_t bit_res = bit_pos & 7u;
      std::uint64_t instruction = 0;
      for (std::size_t j = 0; j < 6; ++j) instruction |= static_cast<std::uint64_t>(at(buf, i + j + byte_pos)) << (8 * j);
      std::uint64_t norm = instruction >> bit_res;
      if (((norm >> 37) & 0xFu) != 0x5 || ((norm >> 9) & 7u) != 0) continue;
      std::uint32_t src = static_cast<std::uint32_t>((norm >> 13) & 0xFFFFFu);
      src |= static_cast<std::uint32_t>((norm >> 36) & 1u) << 20;
      src <<= 4;
      const std::uint32_t here = now + static_cast<std::uint32_t>(i);
      std::uint32_t dest = encode ? src + here : src - here;
      dest >>= 4;
      norm &= ~(static_cast<std::uint64_t>(0x8FFFFFu) << 13);
      norm |= static_cast<std::uint64_t>(dest & 0xFFFFFu) << 13;
      norm |= static_cast<std::uint64_t>(dest & 0x100000u) << (36 - 20);
      instruction &= (std::uint64_t{1} << bit_res) - 1u;
      instruction |= norm << bit_res;
      for (std::size_t j = 0; j < 6; ++j) at(buf, i + j + byte_pos) = static_cast<std::uint8_t>(instruction >> (8 * j));
    }
  }
  return i;
}

Result<std::size_t> run(const Filter& filter, std::span<std::byte> data, FilterState& st, bool last, bool encode) {
  if (filter.kind == FilterKind::Delta) {
    const std::uint32_t distance = filter.parameter;
    if (distance < 1 || distance > 256) return fail(Status::Decode, "delta filter distance {} is invalid", distance);
    auto pos = static_cast<std::uint8_t>(st.history_pos);
    for (std::byte& b : data) {
      const std::uint8_t v = std::to_integer<std::uint8_t>(b);
      const std::uint8_t prev = st.history[static_cast<std::uint8_t>(distance + pos)];
      if (encode) {
        st.history[pos--] = v;
        b = static_cast<std::byte>(static_cast<std::uint8_t>(v - prev));
      } else {
        const auto out = static_cast<std::uint8_t>(v + prev);
        b = static_cast<std::byte>(out);
        st.history[pos--] = out;
      }
    }
    st.history_pos = pos;
    st.position += data.size();
    return data.size();
  }
  if (st.position == 0 && st.history_pos == 0 && filter.kind == FilterKind::X86) {
    st.history_pos = static_cast<std::uint32_t>(0) - 5u;  // "no previous E8/E9"
  }
  const auto now = static_cast<std::uint32_t>(st.position + filter.parameter);
  std::size_t done = 0;
  switch (filter.kind) {
    case FilterKind::X86: done = x86(data, now, encode, st); break;
    case FilterKind::Arm: done = arm(data, now, encode); break;
    case FilterKind::ArmThumb: done = arm_thumb(data, now, encode); break;
    case FilterKind::PowerPc: done = powerpc(data, now, encode); break;
    case FilterKind::Sparc: done = sparc(data, now, encode); break;
    case FilterKind::Arm64: done = arm64(data, now, encode); break;
    case FilterKind::Ia64: done = ia64(data, now, encode); break;
    case FilterKind::RiscV: return fail(encode ? Status::Encode : Status::Decode, "the RISC-V branch filter is not supported");
    case FilterKind::Delta: break;
  }
  // At the end of the data the unfiltered tail is final as it is.
  if (last) done = data.size();
  st.position += done;
  return done;
}

}  // namespace

Result<std::size_t> filter_decode_impl(const Filter& filter, std::span<std::byte> data, FilterState& state, bool last) {
  return run(filter, data, state, last, false);
}

Result<std::size_t> filter_encode_impl(const Filter& filter, std::span<std::byte> data, FilterState& state, bool last) {
  return run(filter, data, state, last, true);
}

Result<Bytes> bcj2_decode_impl(ByteView main_stream, ByteView call_stream, ByteView jump_stream, ByteView range_stream,
                               std::uint64_t output_size, std::uint64_t max_output) {
  if (output_size > max_output) return fail(Status::Limit, "decompressed data exceeds the limit of {} bytes", max_output);
  if (output_size > main_stream.size() + 4 * (call_stream.size() + jump_stream.size()) + 16) {
    return fail(Status::Decode, "BCJ2 output size does not match its streams");
  }
  Bytes out;
  out.reserve(static_cast<std::size_t>(output_size));
  std::array<std::uint16_t, 258> probs{};
  probs.fill(1024);
  const auto* rc = reinterpret_cast<const std::uint8_t*>(range_stream.data());
  std::size_t rc_pos = 0;
  if (range_stream.size() < 5) return fail(Status::Decode, "BCJ2 range stream is truncated");
  std::uint32_t code = 0;
  std::uint32_t range = 0xFFFFFFFFu;
  for (; rc_pos < 5; ++rc_pos) code = (code << 8) | rc[rc_pos];
  std::size_t in_pos = 0;
  std::size_t call_pos = 0;
  std::size_t jump_pos = 0;
  std::uint8_t prev = 0;
  const auto is_jump = [](std::uint8_t b0, std::uint8_t b1) {
    return (b1 & 0xFEu) == 0xE8u || (b0 == 0x0F && (b1 & 0xF0u) == 0x80u);
  };
  while (out.size() < output_size) {
    // Copy plain bytes up to and including the next branch opcode.
    bool found = false;
    while (in_pos < main_stream.size() && out.size() < output_size) {
      const std::uint8_t b = std::to_integer<std::uint8_t>(main_stream[in_pos++]);
      out.push_back(static_cast<std::byte>(b));
      if (is_jump(prev, b)) {
        found = true;
        prev = b;
        break;
      }
      prev = b;
    }
    if (!found || out.size() == output_size) break;
    const std::uint8_t b = prev;
    std::uint16_t& prob = b == 0xE8 ? probs[std::to_integer<std::uint8_t>(out.size() >= 2 ? out[out.size() - 2] : std::byte{0})]
                                    : (b == 0xE9 ? probs[256] : probs[257]);
    // The context of E8 is the byte before it.
    const std::uint32_t bound = (range >> 11) * prob;
    bool converted;
    if (code < bound) {
      range = bound;
      prob = static_cast<std::uint16_t>(prob + ((2048u - prob) >> 5));
      converted = false;
    } else {
      range -= bound;
      code -= bound;
      prob = static_cast<std::uint16_t>(prob - (prob >> 5));
      converted = true;
    }
    if (range < (1u << 24)) {
      if (rc_pos >= range_stream.size()) return fail(Status::Decode, "BCJ2 range stream is truncated");
      range <<= 8;
      code = (code << 8) | rc[rc_pos++];
    }
    if (!converted) continue;
    ByteView src;
    if (b == 0xE8) {
      if (call_pos + 4 > call_stream.size()) return fail(Status::Decode, "BCJ2 call stream is truncated");
      src = call_stream.subspan(call_pos, 4);
      call_pos += 4;
    } else {
      if (jump_pos + 4 > jump_stream.size()) return fail(Status::Decode, "BCJ2 jump stream is truncated");
      src = jump_stream.subspan(jump_pos, 4);
      jump_pos += 4;
    }
    const std::uint32_t abs = (std::to_integer<std::uint32_t>(src[0]) << 24) | (std::to_integer<std::uint32_t>(src[1]) << 16) |
                              (std::to_integer<std::uint32_t>(src[2]) << 8) | std::to_integer<std::uint32_t>(src[3]);
    const std::uint32_t dest = abs - static_cast<std::uint32_t>(out.size() + 4);
    for (int k = 0; k < 4 && out.size() < output_size; ++k) out.push_back(static_cast<std::byte>((dest >> (8 * k)) & 0xFFu));
    prev = static_cast<std::uint8_t>(dest >> 24);
  }
  if (out.size() != output_size) return fail(Status::Decode, "BCJ2 data is truncated");
  return out;
}

}  // namespace cvt::compression::detail
