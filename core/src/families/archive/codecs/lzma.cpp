// Convertible core - archive family: the LZMA family of containers.
//
//   LzmaRaw     raw LZMA1 data, 5-byte properties (7z, zip method 14)
//   LzmaAlone   ".lzma": properties, dictionary, uncompressed size, data
//   Lzma2Raw    LZMA2 chunks, 1-byte dictionary property (7z, xz)
//   Xz          .xz streams: blocks with filter chains, index, checks
//   Lzip        .lz members: "LZIP", version, coded dictionary, LZMA, trailer
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstring>
#include <optional>

#include "families/archive/checksums.hpp"
#include "families/archive/codec_impl.hpp"
#include "families/archive/codecs/lzma_internal.hpp"
#include "util/bytes.hpp"
#include "util/crc.hpp"
#include "util/hash.hpp"

namespace cvt::compression::detail {

namespace {

using lzma::Properties;

std::uint32_t le32(ByteView d, std::size_t at) {
  return std::to_integer<std::uint32_t>(d[at]) | (std::to_integer<std::uint32_t>(d[at + 1]) << 8) |
         (std::to_integer<std::uint32_t>(d[at + 2]) << 16) | (std::to_integer<std::uint32_t>(d[at + 3]) << 24);
}
std::uint64_t le64(ByteView d, std::size_t at) {
  return static_cast<std::uint64_t>(le32(d, at)) | (static_cast<std::uint64_t>(le32(d, at + 4)) << 32);
}
void put32(Bytes& out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
}
void put64(Bytes& out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
}

std::uint64_t ceiling(const DecodeOptions& o) {
  std::uint64_t limit = o.max_output;
  if (o.stop_after != 0) limit = std::min(limit, o.stop_after);
  return limit;
}

Result<Bytes> finish_decode(Bytes out, const DecodeOptions& o, std::string_view what) {
  if (o.stop_after != 0 && out.size() > o.stop_after) out.resize(static_cast<std::size_t>(o.stop_after));
  if (o.output_size && o.stop_after == 0 && out.size() != *o.output_size) {
    return fail(Status::Decode, "{} data decodes to {} bytes, expected {}", what, out.size(), *o.output_size);
  }
  return out;
}

// A prefix decode that hit its ceiling is a success, not a limit error.
bool prefix_done(const Error& e, const DecodeOptions& o) { return o.stop_after != 0 && e.code == Status::Limit; }

// ---------------------------------------------------------------------------- raw LZMA

Result<Properties> parse_props5(ByteView props) {
  if (props.size() < 5) return fail(Status::Decode, "LZMA properties are missing");
  Properties p;
  if (!Properties::from_byte(std::to_integer<std::uint8_t>(props[0]), p)) {
    return fail(Status::Decode, "LZMA properties are invalid");
  }
  p.dict_size = std::max<std::uint32_t>(le32(props, 1), 1u << 12);
  return p;
}

// Decode one LZMA1 stream; `size` = known size or nullopt (end marker required).
Result<std::size_t> lzma1_decode(ByteView data, const Properties& p, std::optional<std::uint64_t> size, Bytes& out,
                                 const DecodeOptions& o) {
  lzma::Decoder dec;
  dec.set_properties(p);
  dec.reset_dictionary(out.size());
  const bool prefix = o.stop_after != 0;
  const std::uint64_t target = size ? out.size() + *size : unlimited;
  if (!prefix && size && target > o.max_output) {
    return fail(Status::Limit, "decompressed data exceeds the limit of {} bytes", o.max_output);
  }
  // A prefix decode may overshoot by one match before it is cut back.
  const std::uint64_t max = prefix ? std::min(o.max_output, o.stop_after + 512) : o.max_output;
  auto r = dec.decode(data, out, target, true, max, o.progress);
  if (!r) return std::unexpected(r.error());
  if (!size && !r->end_marker) {
    return fail(Status::Decode, "LZMA data is truncated (no end marker)");
  }
  return r->consumed;
}

Result<Bytes> decode_lzma_raw(ByteView data, const DecodeOptions& o) {
  CVT_TRY_ASSIGN(const Properties p, parse_props5(o.properties));
  Bytes out;
  auto r = lzma1_decode(data, p, o.output_size, out, o);
  if (!r && !prefix_done(r.error(), o)) return std::unexpected(r.error());
  return finish_decode(std::move(out), o, "LZMA");
}

Result<Bytes> props_lzma_raw(const EncodeOptions& e, std::uint64_t input_size) {
  const lzma::EncoderParams params = lzma::preset(e.level, input_size, e.dictionary_size);
  Bytes props;
  props.push_back(static_cast<std::byte>(params.props.byte()));
  put32(props, params.props.dict_size);
  return props;
}

Result<Bytes> encode_lzma1(ByteView data, const EncodeOptions& e, bool end_marker) {
  const lzma::EncoderParams params = lzma::preset(e.level, data.size(), e.dictionary_size);
  lzma::Encoder enc(data, params);
  Bytes out;
  CVT_TRY(enc.encode(out, data.size(), 0, end_marker, e.progress));
  return out;
}

Result<Bytes> encode_lzma_raw(ByteView data, const EncodeOptions& e) { return encode_lzma1(data, e, !e.content_size); }

// ---------------------------------------------------------------------------- .lzma

Result<Bytes> decode_lzma_alone(ByteView data, const DecodeOptions& o) {
  if (data.size() < 13) return fail(Status::Decode, ".lzma header is truncated");
  CVT_TRY_ASSIGN(Properties p, parse_props5(data.first(5)));
  const std::uint64_t size = le64(data, 5);
  std::optional<std::uint64_t> known;
  if (size != ~0ull) known = size;
  Bytes out;
  auto r = lzma1_decode(data.subspan(13), p, known, out, o);
  if (!r && !prefix_done(r.error(), o)) return std::unexpected(r.error());
  if (known && o.stop_after == 0 && out.size() != *known) return fail(Status::Decode, ".lzma data is truncated");
  return finish_decode(std::move(out), o, ".lzma");
}

Result<Bytes> encode_lzma_alone(ByteView data, const EncodeOptions& e) {
  const lzma::EncoderParams params = lzma::preset(e.level, data.size(), e.dictionary_size);
  Bytes out;
  out.push_back(static_cast<std::byte>(params.props.byte()));
  put32(out, params.props.dict_size);
  put64(out, e.content_size ? data.size() : ~0ull);
  lzma::Encoder enc(data, params);
  CVT_TRY(enc.encode(out, data.size(), 0, !e.content_size, e.progress));
  return out;
}

// ---------------------------------------------------------------------------- LZMA2

std::uint32_t lzma2_dict_from_byte(std::uint8_t b) {
  if (b >= 40) return 0xFFFFFFFFu;
  return (2u | (b & 1u)) << (b / 2 + 11);
}
std::uint8_t lzma2_byte_for_dict(std::uint32_t dict) {
  for (std::uint8_t b = 0; b < 40; ++b) {
    if (lzma2_dict_from_byte(b) >= dict) return b;
  }
  return 40;
}

// Decode LZMA2 chunks up to the end marker, appending to `out`.
Result<std::size_t> lzma2_decode(ByteView data, std::uint32_t dict_size, Bytes& out, const DecodeOptions& o) {
  lzma::Decoder dec;
  Properties props;
  props.dict_size = dict_size;
  bool need_dict_reset = true;
  bool need_props = true;
  std::size_t pos = 0;
  const std::uint64_t cap = ceiling(o);
  while (true) {
    if (pos >= data.size()) return fail(Status::Decode, "LZMA2 data is truncated");
    const std::uint8_t control = std::to_integer<std::uint8_t>(data[pos++]);
    if (control == 0x00) return pos;
    if (o.stop_after != 0 && out.size() >= o.stop_after) return pos;
    if (control == 0x01 || control == 0x02) {
      if (pos + 2 > data.size()) return fail(Status::Decode, "LZMA2 data is truncated");
      const std::size_t size = ((std::to_integer<std::size_t>(data[pos]) << 8) | std::to_integer<std::size_t>(data[pos + 1])) + 1;
      pos += 2;
      if (control == 0x01) {
        dec.reset_dictionary(out.size());
        need_dict_reset = false;
      } else if (need_dict_reset) {
        return fail(Status::Decode, "LZMA2 data does not start with a dictionary reset");
      }
      if (pos + size > data.size()) return fail(Status::Decode, "LZMA2 data is truncated");
      CVT_TRY(check_output_room(out.size(), size, cap == unlimited ? unlimited : std::max(cap, o.max_output)));
      if (out.size() + size > o.max_output) return fail(Status::Limit, "decompressed data exceeds the limit of {} bytes", o.max_output);
      out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(pos), data.begin() + static_cast<std::ptrdiff_t>(pos + size));
      pos += size;
      continue;
    }
    if (control < 0x80) return fail(Status::Decode, "LZMA2 chunk header is invalid");
    if (pos + 4 > data.size()) return fail(Status::Decode, "LZMA2 data is truncated");
    const std::size_t unpacked = ((static_cast<std::size_t>(control & 0x1Fu) << 16) |
                                  (std::to_integer<std::size_t>(data[pos]) << 8) | std::to_integer<std::size_t>(data[pos + 1])) +
                                 1;
    const std::size_t packed = ((std::to_integer<std::size_t>(data[pos + 2]) << 8) | std::to_integer<std::size_t>(data[pos + 3])) + 1;
    pos += 4;
    const int mode = (control >> 5) & 3;
    if (mode == 3) {
      dec.reset_dictionary(out.size());
      need_dict_reset = false;
    } else if (need_dict_reset) {
      return fail(Status::Decode, "LZMA2 data does not start with a dictionary reset");
    }
    if (mode >= 2) {
      if (pos >= data.size()) return fail(Status::Decode, "LZMA2 data is truncated");
      Properties p;
      if (!Properties::from_byte(std::to_integer<std::uint8_t>(data[pos++]), p) || p.lc + p.lp > 4) {
        return fail(Status::Decode, "LZMA2 properties are invalid");
      }
      p.dict_size = dict_size;
      props = p;
      dec.set_properties(props);
      need_props = false;
    } else if (need_props) {
      return fail(Status::Decode, "LZMA2 chunk lacks properties");
    } else if (mode == 1) {
      dec.reset_state();
    }
    if (pos + packed > data.size()) return fail(Status::Decode, "LZMA2 data is truncated");
    if (out.size() + unpacked > o.max_output && !(o.stop_after != 0 && out.size() + unpacked > o.stop_after)) {
      return fail(Status::Limit, "decompressed data exceeds the limit of {} bytes", o.max_output);
    }
    const std::uint64_t target = out.size() + unpacked;
    auto r = dec.decode(data.subspan(pos, packed), out, target, false, std::max<std::uint64_t>(target, 0), o.progress);
    if (!r) return std::unexpected(r.error());
    if (out.size() != target) return fail(Status::Decode, "LZMA2 chunk is damaged");
    if (r->consumed != packed) return fail(Status::Decode, "LZMA2 chunk size does not match its data");
    pos += packed;
  }
}

Result<Bytes> decode_lzma2_raw(ByteView data, const DecodeOptions& o) {
  if (o.properties.empty()) return fail(Status::Decode, "LZMA2 properties are missing");
  const std::uint8_t b = std::to_integer<std::uint8_t>(o.properties[0]);
  if (b > 40) return fail(Status::Decode, "LZMA2 dictionary property is invalid");
  Bytes out;
  auto r = lzma2_decode(data, lzma2_dict_from_byte(b), out, o);
  if (!r && !prefix_done(r.error(), o)) return std::unexpected(r.error());
  return finish_decode(std::move(out), o, "LZMA2");
}

Result<void> lzma2_encode(ByteView data, const EncodeOptions& e, Bytes& out, std::uint8_t* dict_byte) {
  lzma::EncoderParams params = lzma::preset(e.level, data.size(), e.dictionary_size);
  params.props.lc = 3;
  params.props.lp = 0;
  params.props.pb = 2;
  if (dict_byte != nullptr) *dict_byte = lzma2_byte_for_dict(params.props.dict_size);
  lzma::Encoder enc(data, params);
  bool need_dict_reset = true;
  bool need_props = true;
  bool need_state_reset = false;
  constexpr std::size_t max_unpacked = std::size_t{1} << 21;
  constexpr std::size_t max_packed = std::size_t{1} << 16;
  Bytes chunk;
  while (!enc.at_end()) {
    const std::size_t start = enc.position();
    chunk.clear();
    CVT_TRY_ASSIGN(const std::size_t consumed, enc.encode(chunk, max_unpacked, max_packed, false, e.progress));
    if (consumed == 0) return fail(Status::Internal, "LZMA2 encoder made no progress");
    if (chunk.size() >= consumed || chunk.size() > max_packed) {
      // Store: incompressible (or oversized) chunk; the coder state must be reset afterwards.
      for (std::size_t at = 0; at < consumed; at += max_packed) {
        const std::size_t n = std::min(max_packed, consumed - at);
        out.push_back(std::byte{static_cast<std::uint8_t>(need_dict_reset ? 0x01 : 0x02)});
        out.push_back(static_cast<std::byte>(((n - 1) >> 8) & 0xFFu));
        out.push_back(static_cast<std::byte>((n - 1) & 0xFFu));
        append(out, data.subspan(start + at, n));
        need_dict_reset = false;
      }
      enc.reset_state();
      need_state_reset = true;
      continue;
    }
    const int mode = need_dict_reset ? 3 : (need_props ? 2 : (need_state_reset ? 1 : 0));
    const std::size_t u = consumed - 1;
    const std::size_t pk = chunk.size() - 1;
    out.push_back(static_cast<std::byte>(0x80u | (static_cast<unsigned>(mode) << 5) | ((u >> 16) & 0x1Fu)));
    out.push_back(static_cast<std::byte>((u >> 8) & 0xFFu));
    out.push_back(static_cast<std::byte>(u & 0xFFu));
    out.push_back(static_cast<std::byte>((pk >> 8) & 0xFFu));
    out.push_back(static_cast<std::byte>(pk & 0xFFu));
    if (mode >= 2) out.push_back(static_cast<std::byte>(params.props.byte()));
    append(out, chunk);
    need_dict_reset = need_props = need_state_reset = false;
  }
  out.push_back(std::byte{0x00});
  return {};
}

Result<Bytes> encode_lzma2_raw(ByteView data, const EncodeOptions& e) {
  Bytes out;
  CVT_TRY(lzma2_encode(data, e, out, nullptr));
  return out;
}

Result<Bytes> props_lzma2_raw(const EncodeOptions& e, std::uint64_t input_size) {
  const lzma::EncoderParams params = lzma::preset(e.level, input_size, e.dictionary_size);
  return Bytes{static_cast<std::byte>(lzma2_byte_for_dict(params.props.dict_size))};
}

// ---------------------------------------------------------------------------- xz

constexpr std::array<std::uint8_t, 6> xz_magic = {0xFD, '7', 'z', 'X', 'Z', 0x00};

std::size_t check_size(std::uint8_t check) {
  if (check == 0) return 0;
  if (check <= 3) return 4;
  if (check <= 6) return 8;
  if (check <= 9) return 16;
  if (check <= 12) return 32;
  return 64;
}

bool read_vli(ByteView d, std::size_t& pos, std::size_t end, std::uint64_t& value) {
  value = 0;
  for (int i = 0; i < 9; ++i) {
    if (pos >= end) return false;
    const std::uint8_t b = std::to_integer<std::uint8_t>(d[pos++]);
    value |= static_cast<std::uint64_t>(b & 0x7Fu) << (7 * i);
    if ((b & 0x80u) == 0) return i == 0 || b != 0;
  }
  return false;
}
void put_vli(Bytes& out, std::uint64_t v) {
  while (v >= 0x80) {
    out.push_back(static_cast<std::byte>((v & 0x7Fu) | 0x80u));
    v >>= 7;
  }
  out.push_back(static_cast<std::byte>(v));
}

struct XzFilter {
  std::uint64_t id = 0;
  Bytes props;
};

Result<Filter> to_filter(const XzFilter& f) {
  Filter out;
  switch (f.id) {
    case 0x03:
      if (f.props.size() != 1) return fail(Status::Decode, "xz delta filter properties are invalid");
      out.kind = FilterKind::Delta;
      out.parameter = std::to_integer<std::uint32_t>(f.props[0]) + 1u;
      return out;
    case 0x04: out.kind = FilterKind::X86; break;
    case 0x05: out.kind = FilterKind::PowerPc; break;
    case 0x06: out.kind = FilterKind::Ia64; break;
    case 0x07: out.kind = FilterKind::Arm; break;
    case 0x08: out.kind = FilterKind::ArmThumb; break;
    case 0x09: out.kind = FilterKind::Sparc; break;
    case 0x0A: out.kind = FilterKind::Arm64; break;
    case 0x0B: out.kind = FilterKind::RiscV; break;
    default: return fail(Status::Decode, "xz filter 0x{:x} is not supported", f.id);
  }
  if (f.props.size() == 4) {
    out.parameter = le32(f.props, 0);
  } else if (!f.props.empty()) {
    return fail(Status::Decode, "xz branch filter properties are invalid");
  }
  return out;
}

Result<void> verify_check(std::uint8_t check, ByteView block, ByteView stored) {
  switch (check) {
    case 0x01:
      if (util::crc32(block) != le32(stored, 0)) return fail(Status::Decode, "xz block CRC32 mismatch");
      break;
    case 0x04:
      if (archive::crc64_xz(block) != le64(stored, 0)) return fail(Status::Decode, "xz block CRC64 mismatch");
      break;
    case 0x0A: {
      const auto digest = util::sha256(block);
      if (::cvt::util::compare_bytes(digest.data(), stored.data(), 32) != 0) return fail(Status::Decode, "xz block SHA-256 mismatch");
      break;
    }
    default: break;  // unknown check types are skipped, as xz does with a warning
  }
  return {};
}

// Decode one xz stream starting at data[0]; returns the bytes it used.
Result<std::size_t> xz_stream(ByteView data, Bytes& out, const DecodeOptions& o) {
  if (data.size() < 24) return fail(Status::Decode, "xz stream is truncated");
  if (::cvt::util::compare_bytes(data.data(), xz_magic.data(), 6) != 0) return fail(Status::Decode, "not an xz stream");
  if (util::crc32(data.subspan(6, 2)) != le32(data, 8)) return fail(Status::Decode, "xz stream header CRC mismatch");
  if (std::to_integer<int>(data[6]) != 0 || (std::to_integer<int>(data[7]) & 0xF0) != 0) {
    return fail(Status::Decode, "xz stream flags are not supported");
  }
  const std::uint8_t check = std::to_integer<std::uint8_t>(data[7]) & 0x0Fu;
  const std::size_t csize = check_size(check);
  std::size_t pos = 12;
  struct Record {
    std::uint64_t unpadded = 0;
    std::uint64_t uncompressed = 0;
  };
  std::vector<Record> records;
  while (true) {
    if (pos >= data.size()) return fail(Status::Decode, "xz stream is truncated");
    const std::uint8_t size_byte = std::to_integer<std::uint8_t>(data[pos]);
    if (size_byte == 0) break;  // index
    const std::size_t header_size = (static_cast<std::size_t>(size_byte) + 1) * 4;
    if (pos + header_size > data.size()) return fail(Status::Decode, "xz block header is truncated");
    const ByteView header = data.subspan(pos, header_size);
    if (util::crc32(header.first(header_size - 4)) != le32(header, header_size - 4)) {
      return fail(Status::Decode, "xz block header CRC mismatch");
    }
    const std::uint8_t flags = std::to_integer<std::uint8_t>(header[1]);
    if ((flags & 0x3Cu) != 0) return fail(Status::Decode, "xz block flags are not supported");
    const int nfilters = (flags & 3) + 1;
    std::size_t hp = 2;
    const std::size_t hend = header_size - 4;
    std::optional<std::uint64_t> compressed_size;
    std::optional<std::uint64_t> uncompressed_size;
    std::uint64_t v = 0;
    if (flags & 0x40u) {
      if (!read_vli(header, hp, hend, v) || v == 0) return fail(Status::Decode, "xz block header is damaged");
      compressed_size = v;
    }
    if (flags & 0x80u) {
      if (!read_vli(header, hp, hend, v)) return fail(Status::Decode, "xz block header is damaged");
      uncompressed_size = v;
    }
    std::vector<XzFilter> filters;
    for (int i = 0; i < nfilters; ++i) {
      XzFilter f;
      std::uint64_t psize = 0;
      if (!read_vli(header, hp, hend, f.id) || !read_vli(header, hp, hend, psize) || psize > hend - hp) {
        return fail(Status::Decode, "xz block header is damaged");
      }
      f.props.assign(header.begin() + static_cast<std::ptrdiff_t>(hp), header.begin() + static_cast<std::ptrdiff_t>(hp + psize));
      hp += static_cast<std::size_t>(psize);
      filters.push_back(std::move(f));
    }
    for (std::size_t k = hp; k < hend; ++k) {
      if (header[k] != std::byte{0}) return fail(Status::Decode, "xz block header padding is not zero");
    }
    if (filters.back().id != 0x21 || filters.back().props.size() != 1) {
      return fail(Status::Decode, "xz block does not end with LZMA2 (filter 0x{:x})", filters.back().id);
    }
    for (std::size_t i = 0; i + 1 < filters.size(); ++i) {
      if (filters[i].id == 0x21) return fail(Status::Decode, "xz filter chain is invalid");
    }
    const std::uint8_t dict_byte = std::to_integer<std::uint8_t>(filters.back().props[0]);
    if (dict_byte > 40) return fail(Status::Decode, "xz LZMA2 dictionary property is invalid");
    pos += header_size;
    const std::size_t block_start = out.size();
    DecodeOptions inner = o;
    inner.stop_after = o.stop_after == 0 ? 0 : (o.stop_after > out.size() ? o.stop_after : out.size() + 1);
    std::size_t used = 0;
    {
      auto r = lzma2_decode(data.subspan(pos), lzma2_dict_from_byte(dict_byte), out, inner);
      if (!r) return std::unexpected(r.error());
      used = *r;
    }
    if (o.stop_after != 0 && out.size() >= o.stop_after) {
      // Prefix mode: apply the filters to what we have and stop.
      for (std::size_t i = filters.size() - 1; i-- > 0;) {
        CVT_TRY_ASSIGN(const Filter f, to_filter(filters[i]));
        FilterState fs;
        CVT_TRY(filter_decode_impl(f, std::span<std::byte>(out).subspan(block_start), fs, true));
      }
      return data.size();
    }
    if (compressed_size && *compressed_size != used) return fail(Status::Decode, "xz block compressed size mismatch");
    if (uncompressed_size && *uncompressed_size != out.size() - block_start) {
      return fail(Status::Decode, "xz block uncompressed size mismatch");
    }
    for (std::size_t i = filters.size() - 1; i-- > 0;) {
      CVT_TRY_ASSIGN(const Filter f, to_filter(filters[i]));
      FilterState fs;
      CVT_TRY(filter_decode_impl(f, std::span<std::byte>(out).subspan(block_start), fs, true));
    }
    pos += used;
    for (std::size_t pad = (4 - used % 4) % 4; pad > 0; --pad) {
      if (pos >= data.size() || data[pos] != std::byte{0}) return fail(Status::Decode, "xz block padding is damaged");
      ++pos;
    }
    if (pos + csize > data.size()) return fail(Status::Decode, "xz stream is truncated");
    if (o.verify_checksums) CVT_TRY(verify_check(check, ByteView(out).subspan(block_start), data.subspan(pos, csize)));
    pos += csize;
    records.push_back({header_size + used + csize, out.size() - block_start});
    if (!o.progress.report(static_cast<double>(pos) / static_cast<double>(data.size()))) {
      return fail(Status::Cancelled, "the conversion was cancelled");
    }
  }
  // Index.
  const std::size_t index_start = pos;
  ++pos;
  std::uint64_t count = 0;
  if (!read_vli(data, pos, data.size(), count) || count != records.size()) {
    return fail(Status::Decode, "xz index does not match the blocks");
  }
  for (const Record& r : records) {
    std::uint64_t unpadded = 0;
    std::uint64_t uncompressed = 0;
    if (!read_vli(data, pos, data.size(), unpadded) || !read_vli(data, pos, data.size(), uncompressed) ||
        unpadded != r.unpadded || uncompressed != r.uncompressed) {
      return fail(Status::Decode, "xz index does not match the blocks");
    }
  }
  while ((pos - index_start) % 4 != 0) {
    if (pos >= data.size() || data[pos] != std::byte{0}) return fail(Status::Decode, "xz index padding is damaged");
    ++pos;
  }
  if (pos + 4 + 12 > data.size()) return fail(Status::Decode, "xz stream is truncated");
  if (util::crc32(data.subspan(index_start, pos - index_start)) != le32(data, pos)) {
    return fail(Status::Decode, "xz index CRC mismatch");
  }
  pos += 4;
  const ByteView footer = data.subspan(pos, 12);
  if (util::crc32(footer.subspan(4, 6)) != le32(footer, 0)) return fail(Status::Decode, "xz stream footer CRC mismatch");
  if (as_chars(footer.subspan(10, 2)) != "YZ") return fail(Status::Decode, "xz stream footer is damaged");
  if ((static_cast<std::uint64_t>(le32(footer, 4)) + 1) * 4 != pos - index_start) {
    return fail(Status::Decode, "xz stream footer does not match the index");
  }
  if (footer[8] != data[6] || footer[9] != data[7]) return fail(Status::Decode, "xz stream flags mismatch");
  return pos + 12;
}

Result<Bytes> decode_xz(ByteView data, const DecodeOptions& o) {
  Bytes out;
  std::size_t pos = 0;
  bool first = true;
  while (pos < data.size()) {
    if (!first) {
      // Stream padding: groups of four zero bytes.
      std::size_t zeros = 0;
      while (pos + zeros < data.size() && data[pos + zeros] == std::byte{0}) ++zeros;
      if (zeros % 4 != 0) return fail(Status::Decode, "xz stream padding is damaged");
      pos += zeros;
      if (pos >= data.size()) break;
    }
    auto used = xz_stream(data.subspan(pos), out, o);
    if (!used) {
      if (prefix_done(used.error(), o)) break;
      return std::unexpected(used.error());
    }
    pos += *used;
    first = false;
    if (o.stop_after != 0 && out.size() >= o.stop_after) break;
  }
  if (first) return fail(Status::Decode, "not an xz stream");
  return finish_decode(std::move(out), o, "xz");
}

// ---------------------------------------------------------------------------- lzip

Result<Bytes> decode_lzip(ByteView data, const DecodeOptions& o) {
  Bytes out;
  std::size_t pos = 0;
  bool any = false;
  while (pos < data.size()) {
    const ByteView member = data.subspan(pos);
    if (member.size() < 6 || as_chars(member.first(4)) != "LZIP") {
      if (!any) return fail(Status::Decode, "not an lzip file");
      break;  // trailing data
    }
    if (std::to_integer<int>(member[4]) != 1) return fail(Status::Decode, "lzip version {} is not supported", std::to_integer<int>(member[4]));
    const std::uint8_t ds = std::to_integer<std::uint8_t>(member[5]);
    const int exp = ds & 0x1F;
    if (exp < 12 || exp > 29) return fail(Status::Decode, "lzip dictionary size is invalid");
    const std::uint32_t base = 1u << exp;
    const std::uint32_t dict = base - (base / 16) * ((ds >> 5) & 7u);
    Properties p;
    p.lc = 3;
    p.lp = 0;
    p.pb = 2;
    p.dict_size = dict;
    const std::size_t start = out.size();
    auto used = lzma1_decode(member.subspan(6), p, std::nullopt, out, o);
    if (!used) {
      if (prefix_done(used.error(), o)) break;
      return std::unexpected(used.error());
    }
    if (o.stop_after != 0 && out.size() >= o.stop_after) break;
    const std::size_t trailer = 6 + *used;
    if (trailer + 20 > member.size()) return fail(Status::Decode, "lzip member is truncated");
    if (o.verify_checksums && util::crc32(ByteView(out).subspan(start)) != le32(member, trailer)) {
      return fail(Status::Decode, "lzip CRC mismatch");
    }
    if (le64(member, trailer + 4) != out.size() - start) return fail(Status::Decode, "lzip data size mismatch");
    if (le64(member, trailer + 12) != trailer + 20) return fail(Status::Decode, "lzip member size mismatch");
    pos += trailer + 20;
    any = true;
  }
  if (!any && out.empty() && !(o.stop_after != 0)) return fail(Status::Decode, "not an lzip file");
  return finish_decode(std::move(out), o, "lzip");
}

Result<Bytes> encode_lzip(ByteView data, const EncodeOptions& e) {
  lzma::EncoderParams params = lzma::preset(e.level, data.size(), e.dictionary_size);
  params.props.lc = 3;
  params.props.lp = 0;
  params.props.pb = 2;
  // lzip codes the dictionary as 2^n - k * 2^n / 16; use the next power of two.
  int exp = 12;
  while (exp < 29 && (1u << exp) < params.props.dict_size) ++exp;
  params.props.dict_size = 1u << exp;
  Bytes out = to_bytes("LZIP");
  out.push_back(std::byte{1});
  out.push_back(static_cast<std::byte>(exp));
  lzma::Encoder enc(data, params);
  CVT_TRY(enc.encode(out, data.size(), 0, true, e.progress));
  put32(out, util::crc32(data));
  put64(out, data.size());
  put64(out, out.size() + 8);
  return out;
}

Result<Bytes> encode_xz_full(ByteView data, const EncodeOptions& e) {
  const auto check = static_cast<std::uint8_t>(e.check);
  if (check != 0 && check != 1 && check != 4 && check != 10) return fail(Status::InvalidArgument, "unsupported xz check");
  if (e.filters.size() > 3) return fail(Status::InvalidArgument, "xz allows at most three filters before LZMA2");
  Bytes filtered;
  ByteView payload = data;
  if (!e.filters.empty()) {
    filtered.assign(data.begin(), data.end());
    for (const Filter& f : e.filters) {
      FilterState fs;
      CVT_TRY(filter_encode_impl(f, std::span<std::byte>(filtered), fs, true));
    }
    payload = filtered;
  }
  Bytes out(xz_magic.size());
  ::cvt::util::copy_bytes(out.data(), xz_magic.data(), xz_magic.size());
  const std::array<std::byte, 2> flags = {std::byte{0}, static_cast<std::byte>(check)};
  append(out, ByteView(flags));
  put32(out, util::crc32(ByteView(flags)));

  Bytes compressed;
  std::uint8_t dict_byte = 0;
  CVT_TRY(lzma2_encode(payload, e, compressed, &dict_byte));

  Bytes header;
  header.push_back(std::byte{0});
  header.push_back(static_cast<std::byte>(0xC0u | static_cast<unsigned>(e.filters.size())));
  put_vli(header, compressed.size());
  put_vli(header, data.size());
  for (const Filter& f : e.filters) {
    std::uint64_t id = 0;
    switch (f.kind) {
      case FilterKind::Delta: id = 0x03; break;
      case FilterKind::X86: id = 0x04; break;
      case FilterKind::PowerPc: id = 0x05; break;
      case FilterKind::Ia64: id = 0x06; break;
      case FilterKind::Arm: id = 0x07; break;
      case FilterKind::ArmThumb: id = 0x08; break;
      case FilterKind::Sparc: id = 0x09; break;
      case FilterKind::Arm64: id = 0x0A; break;
      case FilterKind::RiscV: id = 0x0B; break;
    }
    put_vli(header, id);
    if (f.kind == FilterKind::Delta) {
      put_vli(header, 1);
      header.push_back(static_cast<std::byte>(std::clamp<std::uint32_t>(f.parameter, 1, 256) - 1));
    } else if (f.parameter != 0) {
      put_vli(header, 4);
      put32(header, f.parameter);
    } else {
      put_vli(header, 0);
    }
  }
  put_vli(header, 0x21);
  put_vli(header, 1);
  header.push_back(static_cast<std::byte>(dict_byte));
  while (header.size() % 4 != 0) header.push_back(std::byte{0});
  header[0] = static_cast<std::byte>((header.size() + 4) / 4 - 1);
  put32(header, util::crc32(header));
  append(out, header);
  append(out, compressed);
  std::size_t padding = (4 - compressed.size() % 4) % 4;
  out.insert(out.end(), padding, std::byte{0});
  std::size_t check_bytes = 0;
  if (check == 1) {
    put32(out, util::crc32(data));
    check_bytes = 4;
  } else if (check == 4) {
    put64(out, archive::crc64_xz(data));
    check_bytes = 8;
  } else if (check == 10) {
    const auto digest = util::sha256(data);
    append(out, as_bytes(std::span<const std::uint8_t>(digest)));
    check_bytes = 32;
  }
  Bytes index;
  index.push_back(std::byte{0});
  put_vli(index, 1);
  put_vli(index, header.size() + compressed.size() + check_bytes);
  put_vli(index, data.size());
  while (index.size() % 4 != 0) index.push_back(std::byte{0});
  put32(index, util::crc32(index));
  append(out, index);
  Bytes footer;
  put32(footer, static_cast<std::uint32_t>(index.size() / 4 - 1));
  append(footer, ByteView(flags));
  Bytes final_footer;
  put32(final_footer, util::crc32(footer));
  append(final_footer, footer);
  append(final_footer, as_bytes("YZ"));
  append(out, final_footer);
  return out;
}

constexpr std::array<CodecImpl, 5> table = {{
    {Method::Xz, decode_xz, encode_xz_full, nullptr, nullptr, nullptr},
    {Method::LzmaAlone, decode_lzma_alone, encode_lzma_alone, nullptr, nullptr, nullptr},
    {Method::LzmaRaw, decode_lzma_raw, encode_lzma_raw, nullptr, nullptr, props_lzma_raw},
    {Method::Lzma2Raw, decode_lzma2_raw, encode_lzma2_raw, nullptr, nullptr, props_lzma2_raw},
    {Method::Lzip, decode_lzip, encode_lzip, nullptr, nullptr, nullptr},
}};

}  // namespace

std::span<const CodecImpl> lzma_codecs() { return table; }

}  // namespace cvt::compression::detail
