// Convertible core - archive family: 7z reader.
//
// The 32-byte signature header points to the (possibly LZMA-compressed and
// AES-encrypted, kEncodedHeader) header; the header describes packed
// streams, folders (graphs of coders joined by bind pairs), the substreams
// that split each folder's output into files, and the per-file properties
// (UTF-16LE names, empty stream / empty file / anti bits, times, Windows
// attributes with the 0x8000 Unix extension carrying st_mode). Every count
// and size is validated against the input and the limits before anything is
// allocated; every folder is decoded with its declared size as the output
// ceiling, charged to the EntrySink, and checked against its CRCs.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "families/archive/archive_common.hpp"
#include "families/archive/compression.hpp"
#include "families/archive/crypto.hpp"
#include "families/archive/formats/sevenzip_internal.hpp"
#include "util/crc.hpp"
#include "util/text.hpp"

namespace cvt::archive::sevenzip {

namespace {

using util::ByteReader;

/// A decoded header larger than this is refused (real headers of 100 000
/// files take a few megabytes).
constexpr std::uint64_t max_header_size = 256ull * 1024 * 1024;

/// Corruption inside the header: one message, the details do not help users.
std::unexpected<Error> damaged(std::string_view what) { return fail(Status::Decode, "7z header is damaged ({})", what); }

// ---------------------------------------------------------------------------- header reader

class Header {
 public:
  explicit Header(ByteView data) : r_(data) {}

  [[nodiscard]] bool ok() const noexcept { return r_.ok(); }
  [[nodiscard]] std::size_t remaining() const noexcept { return r_.remaining(); }

  std::uint8_t byte() noexcept { return r_.u8(); }
  std::uint32_t u32() noexcept { return r_.u32le(); }
  std::uint64_t u64() noexcept { return r_.u64le(); }
  ByteView bytes(std::uint64_t n) noexcept {
    if (n > r_.remaining()) {
      (void)r_.bytes(r_.remaining() + 1);  // sets the failure flag
      return {};
    }
    return r_.bytes(static_cast<std::size_t>(n));
  }

  std::uint64_t number() noexcept {
    const std::uint8_t first = r_.u8();
    std::uint64_t value = 0;
    unsigned mask = 0x80;
    for (int i = 0; i < 8; ++i) {
      if ((first & mask) == 0) {
        const std::uint64_t high = first & (mask - 1);
        return value | (high << (8 * i));
      }
      value |= static_cast<std::uint64_t>(r_.u8()) << (8 * i);
      mask >>= 1;
    }
    return value;
  }

  /// A count that must also fit in the remaining bytes at `min_bytes` each.
  Result<std::size_t> count(std::string_view what, std::uint64_t limit, std::uint64_t min_bytes = 0) {
    const std::uint64_t n = number();
    if (!ok()) return fail(Status::Decode, "7z header is truncated");
    if (n > limit || (min_bytes > 0 && n > remaining() / min_bytes)) {
      return fail(Status::Decode, "7z header is damaged (implausible {} count {})", what, n);
    }
    return static_cast<std::size_t>(n);
  }

  /// Bit vector of `n` items, most significant bit first.
  std::vector<bool> bits(std::size_t n) {
    std::vector<bool> out(n, false);
    std::uint8_t current = 0;
    for (std::size_t i = 0; i < n; ++i) {
      if (i % 8 == 0) current = r_.u8();
      out[i] = (current & (0x80u >> (i % 8))) != 0;
    }
    return out;
  }

  /// "AllAreDefined" byte then (if 0) a bit vector.
  std::vector<bool> defined(std::size_t n) {
    if (byte() != 0) return std::vector<bool>(n, true);
    return bits(n);
  }

  std::vector<std::optional<std::uint32_t>> digests(std::size_t n) {
    const std::vector<bool> have = defined(n);
    std::vector<std::optional<std::uint32_t>> out(n);
    for (std::size_t i = 0; i < n && ok(); ++i) {
      if (have[i]) out[i] = u32();
    }
    return out;
  }

 private:
  ByteReader r_;
};

// ---------------------------------------------------------------------------- streams info

struct Streams {
  std::uint64_t pack_pos = 0;
  std::vector<std::uint64_t> pack_sizes;
  std::vector<std::uint64_t> pack_offsets;  ///< absolute, in the input
  std::vector<Folder> folders;
  std::vector<std::size_t> substream_count;  ///< per folder
  std::vector<std::uint64_t> sizes;          ///< per substream, all folders in order
  std::vector<std::optional<std::uint32_t>> crcs;
};

struct Limits7z {
  std::uint64_t input_size = 0;
  std::uint64_t max_entries = 0;
};

Result<void> read_pack_info(Header& h, Streams& s, const Limits7z& lim) {
  s.pack_pos = h.number();
  CVT_TRY_ASSIGN(const std::size_t n, h.count("packed stream", 4 * lim.max_entries + 64, 1));
  s.pack_sizes.assign(n, 0);
  for (;;) {
    const std::uint8_t id = h.byte();
    if (!h.ok()) return damaged("pack info");
    if (id == kEnd) break;
    if (id == kSize) {
      for (std::uint64_t& size : s.pack_sizes) size = h.number();
    } else if (id == kCrc) {
      (void)h.digests(n);  // packed-stream CRCs are optional and never written by 7-Zip
    } else {
      return damaged("unknown pack info property");
    }
  }
  return {};
}

Result<void> read_folder(Header& h, Folder& f) {
  CVT_TRY_ASSIGN(const std::size_t num_coders, h.count("coder", max_coders, 1));
  if (num_coders == 0) return damaged("folder without coders");
  std::uint64_t total_in = 0;
  std::uint64_t total_out = 0;
  f.coders.resize(num_coders);
  for (Coder& c : f.coders) {
    const std::uint8_t flags = h.byte();
    if ((flags & 0x80u) != 0) return fail(Status::Decode, "7z alternative coder methods are not supported");
    const std::size_t id_size = flags & 0x0Fu;
    if (id_size > 8) return damaged("coder id");
    const ByteView id = h.bytes(id_size);
    for (const std::byte b : id) c.id = (c.id << 8) | std::to_integer<std::uint64_t>(b);
    if ((flags & 0x10u) != 0) {
      const std::uint64_t in = h.number();
      const std::uint64_t out = h.number();
      if (in == 0 || out == 0 || in > max_coder_streams || out > max_coder_streams) return damaged("coder streams");
      c.num_in = static_cast<std::uint32_t>(in);
      c.num_out = static_cast<std::uint32_t>(out);
    }
    if ((flags & 0x20u) != 0) {
      const std::uint64_t size = h.number();
      const ByteView props = h.bytes(size);
      c.properties.assign(props.begin(), props.end());
    }
    if (!h.ok()) return fail(Status::Decode, "7z header is truncated");
    total_in += c.num_in;
    total_out += c.num_out;
  }
  if (total_in > max_coder_streams || total_out > max_coder_streams) return damaged("too many coder streams");
  for (const Coder& c : f.coders) {
    if (c.num_out != 1) return fail(Status::Decode, "7z coders with {} outputs are not supported", c.num_out);
  }
  const std::size_t num_binds = static_cast<std::size_t>(total_out - 1);
  if (total_in < num_binds) return damaged("bind pairs");
  std::vector<bool> in_bound(static_cast<std::size_t>(total_in), false);
  std::vector<bool> out_bound(static_cast<std::size_t>(total_out), false);
  f.binds.resize(num_binds);
  for (BindPair& b : f.binds) {
    const std::uint64_t in64 = h.number();
    const std::uint64_t out64 = h.number();
    if (!h.ok() || in64 >= total_in || out64 >= total_out) return damaged("bind pair");
    const auto in = static_cast<std::size_t>(in64);
    const auto out = static_cast<std::size_t>(out64);
    if (in_bound[in] || out_bound[out]) return damaged("bind pair");
    in_bound[in] = true;
    out_bound[out] = true;
    b.in = static_cast<std::uint32_t>(in);
    b.out = static_cast<std::uint32_t>(out);
  }
  const std::size_t num_packed = static_cast<std::size_t>(total_in) - num_binds;
  if (num_packed == 0) return damaged("folder without packed streams");
  if (num_packed == 1) {
    for (std::uint32_t i = 0; i < total_in; ++i) {
      if (!in_bound[i]) {
        f.packed.push_back(i);
        break;
      }
    }
  } else {
    for (std::size_t i = 0; i < num_packed; ++i) {
      const std::uint64_t in64 = h.number();
      if (!h.ok() || in64 >= total_in) return damaged("packed stream index");
      const auto in = static_cast<std::size_t>(in64);
      if (in_bound[in]) return damaged("packed stream index");
      in_bound[in] = true;
      f.packed.push_back(static_cast<std::uint32_t>(in));
    }
  }
  if (f.packed.size() != num_packed) return damaged("packed streams");
  bool found = false;
  for (std::uint32_t i = 0; i < total_out; ++i) {
    if (!out_bound[i]) {
      f.main_out = i;
      found = true;
    }
  }
  if (!found) return damaged("folder output");
  return {};
}

Result<void> read_unpack_info(Header& h, Streams& s, const Limits7z& lim) {
  if (h.byte() != kFolder) return damaged("expected folder list");
  CVT_TRY_ASSIGN(const std::size_t n, h.count("folder", lim.max_entries + 1, 2));
  if (h.byte() != 0) return fail(Status::Decode, "7z external folder lists are not supported");
  s.folders.resize(n);
  for (Folder& f : s.folders) CVT_TRY(read_folder(h, f));
  if (h.byte() != kCodersUnpackSize) return damaged("expected unpack sizes");
  for (Folder& f : s.folders) {
    std::size_t outs = 0;
    for (const Coder& c : f.coders) outs += c.num_out;
    f.unpack_sizes.resize(outs);
    for (std::uint64_t& size : f.unpack_sizes) size = h.number();
  }
  for (;;) {
    const std::uint8_t id = h.byte();
    if (!h.ok()) return fail(Status::Decode, "7z header is truncated");
    if (id == kEnd) break;
    if (id != kCrc) return damaged("unknown unpack info property");
    const auto crcs = h.digests(n);
    for (std::size_t i = 0; i < n; ++i) s.folders[i].crc = crcs[i];
  }
  return {};
}

Result<void> read_substreams(Header& h, Streams& s, const Limits7z& lim) {
  const std::size_t folders = s.folders.size();
  s.substream_count.assign(folders, 1);
  std::uint8_t id = h.byte();
  if (id == kNumUnpackStream) {
    std::uint64_t total = 0;
    for (std::size_t& c : s.substream_count) {
      const std::uint64_t n = h.number();
      if (n > lim.max_entries || total + n > lim.max_entries) {
        return fail(Status::Limit, "7z archive declares more than {} files", lim.max_entries);
      }
      total += n;
      c = static_cast<std::size_t>(n);
    }
    id = h.byte();
  }
  // Sizes: all but the last substream of each folder are stored; the last is the remainder.
  const bool sizes_stored = id == kSize;
  for (std::size_t f = 0; f < folders; ++f) {
    const std::size_t n = s.substream_count[f];
    if (n == 0) continue;
    const std::uint64_t folder_size = s.folders[f].unpack_size();
    std::uint64_t sum = 0;
    for (std::size_t k = 0; k + 1 < n; ++k) {
      const std::uint64_t size = sizes_stored ? h.number() : 0;
      if (!h.ok()) return fail(Status::Decode, "7z header is truncated");
      if (size > folder_size - sum) return damaged("substream sizes exceed the folder");
      sum += size;
      s.sizes.push_back(size);
    }
    s.sizes.push_back(folder_size - sum);
  }
  if (sizes_stored) id = h.byte();

  // CRCs for the substreams not already covered by a folder CRC.
  std::size_t unknown = 0;
  for (std::size_t f = 0; f < folders; ++f) {
    if (!(s.substream_count[f] == 1 && s.folders[f].crc)) unknown += s.substream_count[f];
  }
  s.crcs.assign(s.sizes.size(), std::nullopt);
  for (;;) {
    if (!h.ok()) return fail(Status::Decode, "7z header is truncated");
    if (id == kEnd) break;
    if (id == kCrc) {
      const auto crcs = h.digests(unknown);
      std::size_t k = 0;
      std::size_t pos = 0;
      for (std::size_t f = 0; f < folders; ++f) {
        const std::size_t n = s.substream_count[f];
        if (n == 1 && s.folders[f].crc) {
          s.crcs[pos++] = s.folders[f].crc;
          continue;
        }
        for (std::size_t j = 0; j < n; ++j) s.crcs[pos++] = crcs[k++];
      }
    } else {
      const std::uint64_t size = h.number();
      (void)h.bytes(size);
    }
    id = h.byte();
  }
  // Folders with one substream and a folder CRC keep that CRC even without kCrc here.
  std::size_t pos = 0;
  for (std::size_t f = 0; f < folders; ++f) {
    if (s.substream_count[f] == 1 && s.folders[f].crc) s.crcs[pos] = s.folders[f].crc;
    pos += s.substream_count[f];
  }
  return {};
}

Result<Streams> read_streams(Header& h, const Limits7z& lim) {
  Streams s;
  std::uint8_t id = h.byte();
  if (id == kPackInfo) {
    CVT_TRY(read_pack_info(h, s, lim));
    id = h.byte();
  }
  if (id == kUnpackInfo) {
    CVT_TRY(read_unpack_info(h, s, lim));
    id = h.byte();
  }
  bool have_substreams = false;
  if (id == kSubStreamsInfo) {
    CVT_TRY(read_substreams(h, s, lim));
    have_substreams = true;
    id = h.byte();
  }
  if (!h.ok()) return fail(Status::Decode, "7z header is truncated");
  if (id != kEnd) return damaged("streams info");
  if (!have_substreams) {
    s.substream_count.assign(s.folders.size(), 1);
    for (const Folder& f : s.folders) {
      s.sizes.push_back(f.unpack_size());
      s.crcs.push_back(f.crc);
    }
  }

  // Packed streams: positions inside the input, assigned to folders in order.
  std::uint64_t pos = signature_header_size;
  if (s.pack_pos > lim.input_size - pos) return fail(Status::Decode, "7z packed data lies outside the file (truncated?)");
  pos += s.pack_pos;
  for (const std::uint64_t size : s.pack_sizes) {
    if (size > lim.input_size - pos) return fail(Status::Decode, "7z packed data lies outside the file (truncated?)");
    s.pack_offsets.push_back(pos);
    pos += size;
  }
  std::size_t next = 0;
  for (Folder& f : s.folders) {
    f.first_pack_stream = next;
    if (f.packed.size() > s.pack_sizes.size() - next) return damaged("folders use more packed streams than exist");
    next += f.packed.size();
  }
  return s;
}

// ---------------------------------------------------------------------------- folder decoding

/// Keys derived so far, by (salt, cycles): deriving is deliberately slow.
struct KeyCache {
  std::string password;
  bool have_password = false;
  Progress progress;
  std::map<std::pair<Bytes, unsigned>, std::array<std::uint8_t, 32>> keys;

  Result<std::array<std::uint8_t, 32>> get(const AesParams& p) {
    auto key = std::make_pair(p.salt, p.cycles_power);
    if (const auto it = keys.find(key); it != keys.end()) return it->second;
    CVT_TRY_ASSIGN(const auto derived, derive_key(password, p.salt, p.cycles_power, progress));
    keys.emplace(std::move(key), derived);
    return derived;
  }
};

std::optional<compression::Filter> branch_filter(std::uint64_t id) {
  using compression::FilterKind;
  switch (id) {
    case id_bcj:
    case id_x86_new: return compression::Filter{FilterKind::X86, 0};
    case id_ppc:
    case id_ppc_new: return compression::Filter{FilterKind::PowerPc, 0};
    case id_ia64:
    case id_ia64_new: return compression::Filter{FilterKind::Ia64, 0};
    case id_arm:
    case id_arm_new: return compression::Filter{FilterKind::Arm, 0};
    case id_armt:
    case id_armt_new: return compression::Filter{FilterKind::ArmThumb, 0};
    case id_sparc:
    case id_sparc_new: return compression::Filter{FilterKind::Sparc, 0};
    case id_arm64: return compression::Filter{FilterKind::Arm64, 0};
    case id_riscv: return compression::Filter{FilterKind::RiscV, 0};
    default: return std::nullopt;
  }
}

std::string method_label(std::uint64_t id) {
  switch (id) {
    case id_copy: return "Copy";
    case id_lzma: return "LZMA";
    case id_lzma2: return "LZMA2";
    case id_ppmd: return "PPMd";
    case id_deflate: return "Deflate";
    case id_deflate64: return "Deflate64";
    case id_bzip2: return "BZip2";
    case id_bcj2: return "BCJ2";
    case id_aes: return "7zAES";
    case id_zstd: return "Zstandard";
    case id_delta: return "Delta";
    default: break;
  }
  if (branch_filter(id)) return "branch filter";
  return std::format("method {:X}", id);
}

class FolderDecoder {
 public:
  FolderDecoder(const Folder& f, const Streams& s, ByteView input, KeyCache& keys, const Progress& progress)
      : f_(f), s_(s), input_(input), keys_(keys), progress_(progress) {
    std::uint32_t in = 0;
    std::uint32_t out = 0;
    for (const Coder& c : f.coders) {
      first_in_.push_back(in);
      first_out_.push_back(out);
      in += c.num_in;
      out += c.num_out;
    }
  }

  Result<Bytes> decode() {
    // Every out-stream must fit the remaining budget before anything is decoded.
    return out_stream(f_.main_out, 0);
  }

  std::uint64_t budget = compression::unlimited;

 private:
  Result<Bytes> out_stream(std::uint32_t index, std::size_t depth) {
    if (depth > f_.coders.size()) return damaged("coder graph has a cycle");
    std::size_t coder = f_.coders.size();
    for (std::size_t i = 0; i < f_.coders.size(); ++i) {
      if (first_out_[i] == index) coder = i;
    }
    if (coder == f_.coders.size()) return damaged("coder output");
    const Coder& c = f_.coders[coder];
    const std::uint64_t size = f_.unpack_sizes[index];
    if (size > budget) {
      return fail(Status::Limit, "a 7z folder would decompress to {} bytes, more than the remaining limit", size);
    }
    std::vector<Bytes> owned(c.num_in);
    std::vector<ByteView> inputs(c.num_in);
    for (std::uint32_t k = 0; k < c.num_in; ++k) {
      const std::uint32_t in = first_in_[coder] + k;
      bool bound = false;
      for (const BindPair& b : f_.binds) {
        if (b.in == in) {
          CVT_TRY_ASSIGN(owned[k], out_stream(b.out, depth + 1));
          inputs[k] = owned[k];
          bound = true;
          break;
        }
      }
      if (bound) continue;
      for (std::size_t p = 0; p < f_.packed.size(); ++p) {
        if (f_.packed[p] == in) {
          const std::size_t stream = f_.first_pack_stream + p;
          inputs[k] = input_.subspan(static_cast<std::size_t>(s_.pack_offsets[stream]),
                                     static_cast<std::size_t>(s_.pack_sizes[stream]));
          bound = true;
          break;
        }
      }
      if (!bound) return damaged("unconnected coder input");
    }
    CVT_TRY_ASSIGN(Bytes out, run(c, inputs, size));
    if (out.size() != size) {
      return fail(Status::Decode, "7z {} data decoded to {} bytes instead of {}", method_label(c.id), out.size(), size);
    }
    return out;
  }

  Result<Bytes> run(const Coder& c, const std::vector<ByteView>& in, std::uint64_t size) {
    using compression::Method;
    compression::DecodeOptions d;
    d.max_output = size;
    d.output_size = size;
    d.progress = progress_;
    const auto codec = [&](Method m) -> Result<Bytes> {
      if (!compression::can_decode(m)) {
        return fail(Status::Decode, "this 7z archive uses {}, which this build cannot decode", method_label(c.id));
      }
      d.properties = c.properties;
      return compression::decompress(m, in[0], d);
    };
    switch (c.id) {
      case id_copy:
        if (in[0].size() < size) return fail(Status::Decode, "7z stored data is truncated");
        return Bytes(in[0].begin(), in[0].begin() + static_cast<std::ptrdiff_t>(size));
      case id_lzma:
        if (c.properties.size() != 5) return fail(Status::Decode, "7z LZMA properties are damaged");
        return codec(Method::LzmaRaw);
      case id_lzma2:
        if (c.properties.size() != 1) return fail(Status::Decode, "7z LZMA2 properties are damaged");
        return codec(Method::Lzma2Raw);
      case id_ppmd:
        if (c.properties.size() != 5) return fail(Status::Decode, "7z PPMd properties are damaged");
        return codec(Method::Ppmd7);
      case id_deflate: return codec(Method::Deflate);
      case id_deflate64: return codec(Method::Deflate64);
      case id_bzip2: return codec(Method::Bzip2);
      case id_zstd: return codec(Method::Zstd);
      case id_bcj2:
        if (in.size() != 4) return fail(Status::Decode, "7z BCJ2 needs four input streams");
        return compression::bcj2_decode(in[0], in[1], in[2], in[3], size, size);
      case id_aes: return decrypt(c, in[0], size);
      case id_delta: {
        if (c.properties.size() != 1) return fail(Status::Decode, "7z Delta properties are damaged");
        compression::Filter filter{compression::FilterKind::Delta, std::to_integer<std::uint32_t>(c.properties[0]) + 1};
        return apply_filter(filter, in[0], size);
      }
      default: break;
    }
    if (auto filter = branch_filter(c.id)) {
      if (c.properties.size() == 4) {
        filter->parameter = std::to_integer<std::uint32_t>(c.properties[0]) |
                            (std::to_integer<std::uint32_t>(c.properties[1]) << 8) |
                            (std::to_integer<std::uint32_t>(c.properties[2]) << 16) |
                            (std::to_integer<std::uint32_t>(c.properties[3]) << 24);
      } else if (!c.properties.empty()) {
        return fail(Status::Decode, "7z branch filter properties are damaged");
      }
      return apply_filter(*filter, in[0], size);
    }
    return fail(Status::Decode, "this 7z archive uses {}, which is not supported", method_label(c.id));
  }

  static Result<Bytes> apply_filter(const compression::Filter& filter, ByteView in, std::uint64_t size) {
    if (in.size() != size) return fail(Status::Decode, "7z filter input has the wrong size");
    Bytes data(in.begin(), in.end());
    compression::FilterState state;
    CVT_TRY(compression::filter_decode(filter, std::span<std::byte>(data), state, true));
    return data;
  }

  Result<Bytes> decrypt(const Coder& c, ByteView in, std::uint64_t size) {
    if (!keys_.have_password) return fail(Status::Decode, "password required: this 7z archive is encrypted");
    CVT_TRY_ASSIGN(const AesParams params, parse_aes_properties(c.properties));
    if (in.size() < size || in.size() % 16 != 0) return fail(Status::Decode, "wrong password or damaged archive");
    CVT_TRY_ASSIGN(const auto key, keys_.get(params));
    CVT_TRY_ASSIGN(Bytes plain, aes_decrypt(in, key, params.iv));
    plain.resize(static_cast<std::size_t>(size));
    return plain;
  }

  const Folder& f_;
  const Streams& s_;
  ByteView input_;
  KeyCache& keys_;
  Progress progress_;
  std::vector<std::uint32_t> first_in_;
  std::vector<std::uint32_t> first_out_;
};

/// Decode a folder and check its CRC. Errors inside encrypted folders become
/// "wrong password or damaged archive" (a wrong key yields garbage that fails
/// to decompress or to match the CRC).
Result<Bytes> decode_folder(const Folder& f, const Streams& s, ByteView input, KeyCache& keys, std::uint64_t budget,
                            const Progress& progress, bool verify) {
  FolderDecoder decoder(f, s, input, keys, progress);
  decoder.budget = budget;
  auto out = decoder.decode();
  const bool encrypted = f.encrypted();
  if (!out) {
    const Error& e = out.error();
    if (encrypted && keys.have_password && e.code == Status::Decode &&
        e.message.find("password required") == std::string::npos) {
      return fail(Status::Decode, "wrong password or damaged archive");
    }
    if (encrypted && keys.have_password && e.code == Status::Limit &&
        e.message.find("remaining limit") == std::string::npos) {
      return fail(Status::Decode, "wrong password or damaged archive");
    }
    return std::unexpected(std::move(out).error());
  }
  if (verify && f.crc && util::crc32(*out) != *f.crc) {
    if (encrypted) return fail(Status::Decode, "wrong password or damaged archive");
    return fail(Status::Decode, "CRC mismatch in a 7z folder");
  }
  return out;
}

// ---------------------------------------------------------------------------- files info

struct FileRecord {
  std::string name;
  bool has_stream = true;
  bool is_dir = false;
  bool anti = false;
  std::optional<std::uint64_t> mtime;  ///< FILETIME
  std::optional<std::uint32_t> attrib;
};

Result<std::vector<FileRecord>> read_files(Header& h, const Limits7z& lim, const Context& ctx) {
  const std::uint64_t n64 = h.number();
  if (!h.ok()) return fail(Status::Decode, "7z header is truncated");
  CVT_TRY(ctx.limits.check_archive_entries(n64));
  if (n64 > lim.max_entries) return fail(Status::Limit, "too many 7z entries");
  const auto n = static_cast<std::size_t>(n64);
  std::vector<FileRecord> files(n);
  std::vector<bool> empty_stream(n, false);
  std::size_t num_empty = 0;
  std::vector<bool> empty_file;
  std::vector<bool> anti;
  for (;;) {
    const std::uint64_t type = h.number();
    if (!h.ok()) return fail(Status::Decode, "7z header is truncated");
    if (type == kEnd) break;
    const std::uint64_t size = h.number();
    const ByteView body = h.bytes(size);
    if (!h.ok()) return fail(Status::Decode, "7z file property runs past the header");
    Header p(body);
    switch (type) {
      case kEmptyStream:
        empty_stream = p.bits(n);
        num_empty = static_cast<std::size_t>(std::ranges::count(empty_stream, true));
        empty_file.assign(num_empty, false);
        anti.assign(num_empty, false);
        break;
      case kEmptyFile: empty_file = p.bits(num_empty); break;
      case kAnti: anti = p.bits(num_empty); break;
      case kName: {
        if (p.byte() != 0) return fail(Status::Decode, "7z external name lists are not supported");
        for (FileRecord& f : files) {
          std::u16string name;
          for (;;) {
            if (p.remaining() < 2) return damaged("file names");
            const std::uint32_t lo = p.byte();
            const std::uint32_t hi = p.byte();
            const auto unit = static_cast<char16_t>(lo | (hi << 8));
            if (unit == 0) break;
            name.push_back(unit);
          }
          f.name = util::utf16_to_utf8(name);
        }
        break;
      }
      case kMTime: {
        const std::vector<bool> have = p.defined(n);
        if (p.byte() != 0) return fail(Status::Decode, "7z external time lists are not supported");
        for (std::size_t i = 0; i < n; ++i) {
          if (have[i]) files[i].mtime = p.u64();
        }
        break;
      }
      case kWinAttributes: {
        const std::vector<bool> have = p.defined(n);
        if (p.byte() != 0) return fail(Status::Decode, "7z external attribute lists are not supported");
        for (std::size_t i = 0; i < n; ++i) {
          if (have[i]) files[i].attrib = p.u32();
        }
        break;
      }
      default: break;  // kCTime, kATime, kStartPos, kDummy, kComment and unknown properties
    }
    if (!p.ok()) return damaged("file property");
  }
  std::size_t e = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (!empty_stream[i]) continue;
    files[i].has_stream = false;
    files[i].is_dir = !empty_file[e];
    files[i].anti = anti[e];
    ++e;
  }
  return files;
}

// ---------------------------------------------------------------------------- top level

struct SignatureHeader {
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
  std::uint32_t crc = 0;
};

Result<SignatureHeader> read_signature(ByteView data) {
  if (data.size() < signature_header_size ||
      !std::equal(signature.begin(), signature.end(), data.begin(),
                  [](std::uint8_t a, std::byte b) { return std::byte{a} == b; })) {
    return fail(Status::Decode, "not a 7z archive");
  }
  ByteReader r(data.subspan(6, signature_header_size - 6));
  const std::uint8_t major = r.u8();
  (void)r.u8();  // minor
  const std::uint32_t start_crc = r.u32le();
  SignatureHeader s;
  s.offset = r.u64le();
  s.size = r.u64le();
  s.crc = r.u32le();
  if (major != 0) return fail(Status::Decode, "7z format version {} is not supported", major);
  if (util::crc32(data.subspan(12, 20)) != start_crc) return fail(Status::Decode, "7z start header is damaged (CRC mismatch)");
  return s;
}

}  // namespace

Result<ir::Archive> read_7z(ReadContext& ctx) {
  const ByteView input = ctx.input;
  CVT_TRY_ASSIGN(const SignatureHeader sig, read_signature(input));
  EntrySink sink(ctx, input.size());
  KeyCache keys;
  keys.password = ctx.options.get_string("password", "");
  keys.have_password = !keys.password.empty();
  keys.progress = ctx.progress.sub(0.0, 0.05);
  const bool verify = true;
  // What a folder may expand to: the sink's remaining budget, and never more
  // than the ratio guard would accept for this input (checked before decoding,
  // so a tiny archive cannot make us allocate gigabytes first).
  const std::uint64_t ratio_cap =
      std::max<std::uint64_t>(ratio_guard_floor, static_cast<std::uint64_t>(input.size()) * max_compression_ratio);
  const auto budget = [&] { return std::min(sink.budget(), ratio_cap); };
  if (sig.size == 0) return sink.finish();  // an empty archive

  const std::uint64_t body = input.size() - signature_header_size;
  if (sig.offset > body || sig.size > body - sig.offset) {
    return fail(Status::Decode, "7z archive is truncated (the header lies beyond the end of the file)");
  }
  const ByteView first_header =
      input.subspan(static_cast<std::size_t>(signature_header_size + sig.offset), static_cast<std::size_t>(sig.size));
  if (util::crc32(first_header) != sig.crc) return fail(Status::Decode, "7z header is damaged (CRC mismatch)");

  Limits7z lim;
  lim.input_size = input.size();
  lim.max_entries = ctx.limits.max_archive_entries;

  // Unwrap encoded headers (normally exactly one level).
  Bytes decoded;
  ByteView header = first_header;
  for (int level = 0;; ++level) {
    Header h(header);
    const std::uint8_t id = h.byte();
    if (id == kHeader) break;
    if (id != kEncodedHeader || level >= 4) return damaged("unknown header type");
    CVT_TRY_ASSIGN(const Streams s, read_streams(h, lim));
    if (s.folders.empty()) return damaged("encoded header without data");
    const Folder& f = s.folders[0];
    const std::uint64_t header_budget = std::min(budget(), max_header_size);
    if (f.unpack_size() > header_budget) {
      return fail(Status::Limit, "the 7z header would decompress to {} bytes, more than the limit", f.unpack_size());
    }
    auto next = decode_folder(f, s, input, keys, header_budget, keys.progress, verify);
    if (!next) {
      if (next.error().message.find("wrong password") != std::string::npos) {
        return fail(Status::Decode, "wrong password or damaged archive");
      }
      return std::unexpected(std::move(next).error());
    }
    decoded = std::move(*next);
    header = decoded;
    if (f.encrypted() && (header.empty() || header[0] != std::byte{kHeader}) &&
        (header.empty() || header[0] != std::byte{kEncodedHeader})) {
      return fail(Status::Decode, "wrong password or damaged archive");
    }
  }

  // The plain header.
  Header h(header);
  (void)h.byte();  // kHeader
  Streams main;
  std::vector<FileRecord> files;
  std::uint8_t id = h.byte();
  if (id == kArchiveProperties) {
    for (;;) {
      const std::uint64_t type = h.number();
      if (!h.ok()) return fail(Status::Decode, "7z header is truncated");
      if (type == kEnd) break;
      (void)h.bytes(h.number());
    }
    id = h.byte();
  }
  if (id == kAdditionalStreamsInfo) {
    CVT_TRY_ASSIGN(const Streams ignored, read_streams(h, lim));
    (void)ignored;
    id = h.byte();
  }
  if (id == kMainStreamsInfo) {
    CVT_TRY_ASSIGN(main, read_streams(h, lim));
    id = h.byte();
  }
  if (id == kFilesInfo) {
    CVT_TRY_ASSIGN(files, read_files(h, lim, ctx));
    id = h.byte();
  }
  if (!h.ok()) return fail(Status::Decode, "7z header is truncated");
  if (id != kEnd) return damaged("unexpected header property");

  std::size_t streams_needed = 0;
  for (const FileRecord& f : files) {
    if (f.has_stream) ++streams_needed;
  }
  if (streams_needed != main.sizes.size()) {
    return damaged("the number of files with data does not match the streams");
  }

  // Walk the files; decode each folder when its first substream is needed.
  std::uint64_t total_packed = 0;
  for (const std::uint64_t p : main.pack_sizes) total_packed += p;
  std::size_t folder = 0;
  std::size_t in_folder = 0;  // substreams of `folder` used so far
  std::size_t stream = 0;     // global substream index
  std::uint64_t offset = 0;   // position inside the decoded folder
  Bytes content;
  bool loaded = false;
  std::uint64_t packed_done = 0;
  for (FileRecord& f : files) {
    if (f.anti) continue;  // deletion markers of update archives
    ir::ArchiveEntry entry;
    entry.path = f.name;
    if (f.mtime && *f.mtime != 0) {
      entry.mtime = static_cast<std::int64_t>(*f.mtime / 10000000u) - filetime_epoch_offset;
    }
    std::uint32_t type_bits = 0;
    if (f.attrib && (*f.attrib & attr_unix_extension) != 0) {
      const std::uint32_t mode = *f.attrib >> 16;
      entry.unix_mode = mode & 07777u;
      type_bits = mode & 0170000u;
    }
    if (!f.has_stream) {
      entry.is_directory = f.is_dir || type_bits == 0040000u;
      CVT_TRY(sink.add(std::move(entry)));
      continue;
    }
    // Next substream: skip folders without any.
    while (folder < main.folders.size() && in_folder >= main.substream_count[folder]) {
      ++folder;
      in_folder = 0;
      offset = 0;
      loaded = false;
    }
    if (folder >= main.folders.size()) return damaged("substreams");
    if (!loaded) {
      const Folder& fo = main.folders[folder];
      CVT_TRY(ctx.checkpoint(static_cast<double>(packed_done) / static_cast<double>(std::max<std::uint64_t>(total_packed, 1))));
      std::uint64_t folder_packed = 0;
      for (std::size_t p = 0; p < fo.packed.size(); ++p) folder_packed += main.pack_sizes[fo.first_pack_stream + p];
      const double span = static_cast<double>(std::max<std::uint64_t>(total_packed, 1));
      const Progress progress = ctx.progress.sub(0.05 + 0.95 * static_cast<double>(packed_done) / span,
                                                 0.05 + 0.95 * static_cast<double>(packed_done + folder_packed) / span);
      content.clear();
      content.shrink_to_fit();
      CVT_TRY_ASSIGN(content, decode_folder(fo, main, input, keys, budget(), progress, verify));
      CVT_TRY(sink.charge(content.size()));
      packed_done += folder_packed;
      loaded = true;
    }
    const std::uint64_t size = main.sizes[stream];
    if (size > content.size() - offset) return damaged("substream sizes");
    const ByteView piece = ByteView(content).subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(size));
    if (verify && main.crcs[stream] && util::crc32(piece) != *main.crcs[stream]) {
      if (main.folders[folder].encrypted()) return fail(Status::Decode, "wrong password or damaged archive");
      return fail(Status::Decode, "CRC mismatch in '{}'", util::sanitize_utf8(f.name));
    }
    if (type_bits == 0120000u) {
      entry.symlink_target = util::sanitize_utf8(as_chars(piece));
      if (entry.symlink_target.empty()) entry.bytes.assign(piece.begin(), piece.end());
    } else {
      entry.bytes.assign(piece.begin(), piece.end());
    }
    offset += size;
    ++in_folder;
    ++stream;
    CVT_TRY(sink.add(std::move(entry)));
  }
  CVT_TRY(ctx.checkpoint(1.0));
  return sink.finish();
}

}  // namespace cvt::archive::sevenzip
