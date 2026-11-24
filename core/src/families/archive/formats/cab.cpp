// Convertible core - archive family: Microsoft Cabinet (.cab) reader, and the registration of CAB and CHM.
//
// Reads CFHEADER (with reserved areas and the names of neighbouring
// cabinets), CFFOLDER and CFFILE records and the CFDATA blocks of each
// folder, verifying their checksums. Folders are stored, MSZIP, Quantum or
// LZX compressed; the codecs come from compression.hpp, one call per folder
// with the CFDATA sizes as `block_sizes`. File names are UTF-8 when the
// file's 0x80 attribute says so, otherwise Latin-1; DOS times are read as
// UTC. In a cabinet of a multi-cabinet set, files that begin in the previous
// cabinet or continue into the next one cannot be complete: they are skipped
// and named in a warning, and everything else is extracted.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "engine/registry.hpp"
#include "families/archive/archive_common.hpp"
#include "families/archive/compression.hpp"
#include "families/archive/formats/cab_internal.hpp"
#include "families/archive/formats/formats.hpp"
#include "util/text.hpp"

namespace cvt::archive {

namespace cab {

namespace {

constexpr std::uint16_t continued_from_prev = 0xFFFD;
constexpr std::uint16_t continued_to_next = 0xFFFE;
constexpr std::uint16_t continued_both = 0xFFFF;

struct Folder {
  std::uint32_t data_offset = 0;
  std::uint16_t blocks = 0;
  std::uint16_t type = 0;
};

struct File {
  std::uint32_t size = 0;
  std::uint32_t offset = 0;
  std::uint16_t folder = 0;
  std::uint16_t date = 0;
  std::uint16_t time = 0;
  std::uint16_t attributes = 0;
  std::string name;
};

// CAB checksum: 32-bit little-endian words XORed together, the tail bytes packed high to low.
std::uint32_t checksum(ByteView data, std::uint32_t seed) {
  std::uint32_t sum = seed;
  std::size_t i = 0;
  for (; i + 4 <= data.size(); i += 4) {
    sum ^= std::to_integer<std::uint32_t>(data[i]) | (std::to_integer<std::uint32_t>(data[i + 1]) << 8) |
           (std::to_integer<std::uint32_t>(data[i + 2]) << 16) | (std::to_integer<std::uint32_t>(data[i + 3]) << 24);
  }
  std::uint32_t tail = 0;
  for (; i < data.size(); ++i) tail = (tail << 8) | std::to_integer<std::uint32_t>(data[i]);
  return sum ^ tail;
}

std::int64_t dos_time(std::uint16_t date, std::uint16_t time) {
  if (date == 0) return 0;
  const std::int64_t year = 1980 + (date >> 9);
  const unsigned month = (date >> 5) & 15u;
  const unsigned day = date & 31u;
  if (month < 1 || month > 12 || day < 1) return 0;
  const auto days = [](std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    return era * 146097 + static_cast<std::int64_t>(yoe * 365 + yoe / 4 - yoe / 100 + doy) - 719468;
  };
  return days(year, month, day) * 86400 + (time >> 11) * 3600 + ((time >> 5) & 63u) * 60 + (time & 31u) * 2;
}

std::string_view method_label(std::uint16_t type) {
  switch (type & 0x0Fu) {
    case 0: return "stored";
    case 1: return "MSZIP";
    case 2: return "Quantum";
    case 3: return "LZX";
    default: return "unknown";
  }
}

struct Header {
  std::uint16_t flags = 0;
  std::size_t folder_reserve = 0;
  std::size_t data_reserve = 0;
  std::string previous;
  std::string next;
  std::uint32_t files_offset = 0;
  std::uint16_t folder_count = 0;
  std::uint16_t file_count = 0;
  std::size_t folders_at = 0;
};

Result<Header> parse_header(ByteView data) {
  util::ByteReader r(data);
  if (r.chars(4) != "MSCF") return fail(Status::Decode, "not a cabinet file");
  Header h;
  (void)r.u32le();
  const std::uint32_t cabinet_size = r.u32le();
  (void)r.u32le();
  h.files_offset = r.u32le();
  (void)r.u32le();
  const std::uint8_t minor = r.u8();
  const std::uint8_t major = r.u8();
  h.folder_count = r.u16le();
  h.file_count = r.u16le();
  h.flags = r.u16le();
  (void)r.u16le();  // set id
  (void)r.u16le();  // index in the set
  if (!r.ok()) return fail(Status::Decode, "cabinet header is truncated");
  if (major != 1 || minor != 3) return fail(Status::Decode, "cabinet version {}.{} is not supported", major, minor);
  if (cabinet_size < 36) return fail(Status::Decode, "cabinet header is damaged");
  if ((h.flags & 0x0004u) != 0) {
    const std::uint16_t header_reserve = r.u16le();
    h.folder_reserve = r.u8();
    h.data_reserve = r.u8();
    (void)r.skip(header_reserve);
  }
  const auto cstring = [&r]() -> std::optional<std::string> {
    std::string s;
    while (r.ok() && !r.at_end()) {
      const char c = static_cast<char>(r.u8());
      if (c == '\0') return s;
      if (s.size() >= 256) return std::nullopt;
      s += c;
    }
    return std::nullopt;
  };
  if ((h.flags & 0x0001u) != 0) {
    const auto name = cstring();
    const auto disk = cstring();
    if (!name || !disk) return fail(Status::Decode, "cabinet header is truncated");
    h.previous = decode_name(*name, false, false);
  }
  if ((h.flags & 0x0002u) != 0) {
    const auto name = cstring();
    const auto disk = cstring();
    if (!name || !disk) return fail(Status::Decode, "cabinet header is truncated");
    h.next = decode_name(*name, false, false);
  }
  if (!r.ok()) return fail(Status::Decode, "cabinet header is truncated");
  h.folders_at = r.position();
  return h;
}

// Decompress one folder (or, for the last folder of a cabinet continued in
// the next one, its complete blocks).
Result<Bytes> read_folder(ByteView data, const Folder& folder, const Header& h, EntrySink& sink, const Context& ctx,
                          bool& partial) {
  std::size_t pos = folder.data_offset;
  Bytes packed;
  std::vector<std::uint32_t> sizes;
  std::uint64_t total = 0;
  partial = false;
  for (std::uint32_t i = 0; i < folder.blocks; ++i) {
    if (pos > data.size() || data.size() - pos < 8 + h.data_reserve) return fail(Status::Decode, "cabinet data block is truncated");
    const ByteView head = data.subspan(pos, 8 + h.data_reserve);
    util::ByteReader r(head);
    const std::uint32_t stored = r.u32le();
    const std::uint16_t compressed = r.u16le();
    const std::uint16_t uncompressed = r.u16le();
    pos += head.size();
    if (data.size() - pos < compressed) return fail(Status::Decode, "cabinet data block is truncated");
    const ByteView payload = data.subspan(pos, compressed);
    pos += compressed;
    if (stored != 0 && checksum(head.subspan(4), checksum(payload, 0)) != stored) {
      return fail(Status::Decode, "cabinet data block {} has a bad checksum", i);
    }
    if (uncompressed == 0) {
      // The block continues in the next cabinet.
      partial = true;
      break;
    }
    if (uncompressed > 32768 + 6144) return fail(Status::Decode, "cabinet data block is too large");
    append(packed, payload);
    sizes.push_back(uncompressed);
    total += uncompressed;
  }
  if (total > sink.budget()) return fail(Status::Limit, "cabinet folder of {} bytes exceeds the limit", total);
  const unsigned kind = folder.type & 0x0Fu;
  if (kind == 0) {
    if (packed.size() != total) return fail(Status::Decode, "stored cabinet block sizes disagree");
    CVT_TRY(sink.charge(total));
    return packed;
  }
  compression::Method method = compression::Method::Mszip;
  compression::DecodeOptions d;
  d.max_output = total;
  d.output_size = total;
  d.block_sizes = sizes;
  d.progress = ctx.progress;
  if (kind == 1) {
    method = compression::Method::Mszip;
  } else if (kind == 2) {
    method = compression::Method::Quantum;
    d.window_bits = (folder.type >> 8) & 0x1F;
  } else if (kind == 3) {
    method = compression::Method::Lzx;
    d.window_bits = (folder.type >> 8) & 0x1F;
  } else {
    return fail(Status::Decode, "cabinet compression type {} is not known", kind);
  }
  if (!compression::can_decode(method)) {
    return fail(Status::Decode, "{} compressed cabinets cannot be read by this build", method_label(folder.type));
  }
  if (total == 0) return Bytes{};
  CVT_TRY_ASSIGN(Bytes out, compression::decompress(method, packed, d));
  if (out.size() != total) return fail(Status::Decode, "cabinet folder decompressed to the wrong size");
  CVT_TRY(sink.charge(out.size()));
  return out;
}

Result<ir::Archive> read_cab(ReadContext& ctx) {
  const ByteView data = ctx.input;
  CVT_TRY_ASSIGN(const Header h, parse_header(data));
  util::ByteReader r(data);
  (void)r.seek(h.folders_at);
  std::vector<Folder> folders;
  for (std::uint16_t i = 0; i < h.folder_count; ++i) {
    Folder f;
    f.data_offset = r.u32le();
    f.blocks = r.u16le();
    f.type = r.u16le();
    (void)r.skip(h.folder_reserve);
    if (!r.ok()) return fail(Status::Decode, "cabinet folder table is truncated");
    folders.push_back(f);
  }
  if (!r.seek(h.files_offset)) return fail(Status::Decode, "cabinet file table is truncated");
  CVT_TRY(ctx.limits.check_archive_entries(h.file_count));
  std::vector<File> files;
  for (std::uint16_t i = 0; i < h.file_count; ++i) {
    File f;
    f.size = r.u32le();
    f.offset = r.u32le();
    f.folder = r.u16le();
    f.date = r.u16le();
    f.time = r.u16le();
    f.attributes = r.u16le();
    std::string raw;
    while (r.ok() && !r.at_end()) {
      const char c = static_cast<char>(r.u8());
      if (c == '\0') break;
      raw += c;
    }
    if (!r.ok() || r.at_end()) return fail(Status::Decode, "cabinet file table is truncated");
    f.name = decode_name(raw, (f.attributes & 0x80u) != 0, false);
    files.push_back(std::move(f));
  }

  EntrySink sink(ctx, data.size());
  std::vector<std::string> from_previous;
  std::vector<std::string> to_next;
  std::map<std::uint16_t, Bytes> decoded;
  std::map<std::uint16_t, bool> partial_folder;
  for (std::size_t i = 0; i < files.size(); ++i) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(files.size())));
    const File& f = files[i];
    std::uint16_t index = f.folder;
    if (index == continued_from_prev || index == continued_both) {
      from_previous.push_back(f.name);
      continue;
    }
    if (index == continued_to_next) index = static_cast<std::uint16_t>(folders.empty() ? 0 : folders.size() - 1);
    if (index >= folders.size()) return fail(Status::Decode, "cabinet file '{}' names a missing folder", f.name);
    // The first folder of a cabinet that continues a set starts in the previous cabinet.
    if (index == 0 && (h.flags & 0x0001u) != 0 && f.folder != continued_to_next &&
        std::any_of(files.begin(), files.end(), [](const File& x) { return x.folder == continued_from_prev || x.folder == continued_both; })) {
      from_previous.push_back(f.name);
      continue;
    }
    auto found = decoded.find(index);
    if (found == decoded.end()) {
      bool partial = false;
      CVT_TRY_ASSIGN(Bytes folder, read_folder(data, folders[index], h, sink, ctx, partial));
      partial_folder[index] = partial;
      found = decoded.emplace(index, std::move(folder)).first;
    }
    const Bytes& folder = found->second;
    if (static_cast<std::uint64_t>(f.offset) + f.size > folder.size()) {
      if (f.folder == continued_to_next || partial_folder[index]) {
        to_next.push_back(f.name);
        continue;
      }
      return fail(Status::Decode, "cabinet file '{}' lies beyond the end of its folder", f.name);
    }
    ir::ArchiveEntry entry;
    entry.path = f.name;
    entry.mtime = dos_time(f.date, f.time);
    entry.unix_mode = (f.attributes & 0x40u) != 0 ? 0755u : 0644u;
    if ((f.attributes & 0x01u) != 0) entry.unix_mode &= ~0222u;
    entry.bytes.assign(folder.begin() + f.offset, folder.begin() + f.offset + f.size);
    CVT_TRY(sink.add(std::move(entry)));
  }
  const auto list = [](const std::vector<std::string>& names) {
    std::string out;
    for (std::size_t i = 0; i < names.size() && i < 10; ++i) out += (i == 0 ? "'" : ", '") + names[i] + "'";
    if (names.size() > 10) out += std::format(" and {} more", names.size() - 10);
    return out;
  };
  if (!from_previous.empty()) {
    ctx.warn(std::format("{} file(s) begin in the previous cabinet{} and were skipped: {}.", from_previous.size(),
                         h.previous.empty() ? std::string() : " '" + h.previous + "'", list(from_previous)));
  }
  if (!to_next.empty()) {
    ctx.warn(std::format("{} file(s) continue in the next cabinet{} and were skipped: {}.", to_next.size(),
                         h.next.empty() ? std::string() : " '" + h.next + "'", list(to_next)));
  }
  return sink.finish();
}

int sniff_cab(std::span<const std::byte> data, std::string_view) {
  if (data.size() < 36 || as_chars(data.first(4)) != "MSCF") return 0;
  const bool reserved_zero = std::all_of(data.begin() + 4, data.begin() + 8, [](std::byte b) { return b == std::byte{0}; });
  return reserved_zero && data[24] == std::byte{3} && data[25] == std::byte{1} ? 95 : 0;
}

int sniff_chm(std::span<const std::byte> data, std::string_view) {
  if (data.size() < 0x58 || as_chars(data.first(4)) != "ITSF") return 0;
  const auto version = std::to_integer<int>(data[4]);
  return (version == 2 || version == 3) ? 95 : 0;
}

}  // namespace

}  // namespace cab

void register_cab_formats(Registry& registry) {
  ArchiveFormat f;
  f.info.id = "cab";
  f.info.name = "Microsoft Cabinet archive";
  f.info.description = "Windows installer cabinet (stored, MSZIP, Quantum or LZX compressed). Read only.";
  f.info.category = Category::Archive;
  f.info.extensions = {"cab"};
  f.info.mime = "application/vnd.ms-cab-compressed";
  f.info.popularity = 25;
  f.info.signatures = {Signature::at(0, "'MSCF' 00 00 00 00")};
  f.info.sniff = cab::sniff_cab;
  f.read = cab::read_cab;
  register_archive_format(registry, std::move(f));

  ArchiveFormat chm;
  chm.info.id = "chm";
  chm.info.name = "Compiled HTML Help";
  chm.info.description = "Windows help file (ITSF): converts to documents or extracts its pages and images as files.";
  chm.info.category = Category::Ebook;
  chm.info.extensions = {"chm"};
  chm.info.mime = "application/vnd.ms-htmlhelp";
  chm.info.popularity = 15;
  chm.info.signatures = {Signature::at(0, "'ITSF'")};
  chm.info.sniff = cab::sniff_chm;
  chm.read = cab::read_chm;
  register_archive_format(registry, std::move(chm));
}

}  // namespace cvt::archive
