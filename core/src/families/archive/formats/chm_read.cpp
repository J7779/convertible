// Convertible core - archive family: Compiled HTML Help (.chm, ITSF) reader.
//
// The ITSF header points at the ITSP directory, whose PMGL listing chunks
// name every file with its section, offset and length (ENCINT numbers).
// Section 0 is stored; section 1 ("MSCompressed") is one LZX stream that is
// reset at fixed intervals: its ControlData gives the window and the reset
// interval, its ResetTable the compressed offset of every 32 KiB frame, so
// each reset interval is decoded on its own (Method::Lzx, frame sizes as
// `block_sizes`) and only the intervals a file touches are decoded. The
// internal files ("::DataSpace/...", "/#SYSTEM", "/$OBJINST"...) are left out.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <bit>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "families/archive/archive_common.hpp"
#include "families/archive/compression.hpp"
#include "families/archive/formats/cab_internal.hpp"
#include "util/text.hpp"

namespace cvt::archive::cab {

namespace {

constexpr std::string_view control_name = "::DataSpace/Storage/MSCompressed/ControlData";
constexpr std::string_view content_name = "::DataSpace/Storage/MSCompressed/Content";
constexpr std::string_view reset_name =
    "::DataSpace/Storage/MSCompressed/Transform/{7FC28940-9D31-11D0-9B27-00A0C91E9C7C}/InstanceData/ResetTable";

std::uint32_t le32(ByteView d, std::size_t at) {
  return std::to_integer<std::uint32_t>(d[at]) | (std::to_integer<std::uint32_t>(d[at + 1]) << 8) |
         (std::to_integer<std::uint32_t>(d[at + 2]) << 16) | (std::to_integer<std::uint32_t>(d[at + 3]) << 24);
}
std::uint64_t le64(ByteView d, std::size_t at) {
  return static_cast<std::uint64_t>(le32(d, at)) | (static_cast<std::uint64_t>(le32(d, at + 4)) << 32);
}

struct Item {
  std::uint64_t section = 0;
  std::uint64_t offset = 0;
  std::uint64_t length = 0;
};

std::optional<std::uint64_t> encint(ByteView d, std::size_t& pos, std::size_t end) {
  std::uint64_t v = 0;
  for (int i = 0; i < 9; ++i) {
    if (pos >= end) return std::nullopt;
    const auto b = std::to_integer<std::uint8_t>(d[pos++]);
    v = (v << 7) | (b & 0x7Fu);
    if ((b & 0x80u) == 0) return v;
  }
  return std::nullopt;
}

class Chm {
 public:
  Chm(ByteView data, EntrySink& sink, const Context& ctx) : data_(data), sink_(sink), ctx_(ctx) {}

  Result<void> run() {
    CVT_TRY(read_directory());
    std::size_t index = 0;
    for (const auto& [name, item] : items_) {
      if ((++index & 63u) == 0) CVT_TRY(ctx_.checkpoint(static_cast<double>(index) / static_cast<double>(items_.size())));
      if (name.starts_with("::") || name.starts_with("/#") || name.starts_with("/$") || name == "/") continue;
      ir::ArchiveEntry entry;
      entry.path = util::sanitize_utf8(name);
      if (name.ends_with('/')) {
        entry.is_directory = true;
        CVT_TRY(sink_.add(std::move(entry)));
        continue;
      }
      CVT_TRY_ASSIGN(entry.bytes, content(item));
      CVT_TRY(sink_.charge(entry.bytes.size()));
      CVT_TRY(sink_.add(std::move(entry)));
    }
    return {};
  }

 private:
  Result<void> read_directory() {
    if (data_.size() < 0x58 || as_chars(data_.first(4)) != "ITSF") return fail(Status::Decode, "not a CHM file");
    const std::uint32_t version = le32(data_, 4);
    const std::uint32_t header_len = le32(data_, 8);
    const std::uint64_t dir_offset = le64(data_, 72);
    const std::uint64_t dir_length = le64(data_, 80);
    content_offset_ = (version >= 3 && header_len >= 0x60 && data_.size() >= 0x60) ? le64(data_, 88) : dir_offset + dir_length;
    if (dir_offset > data_.size() || dir_length > data_.size() - dir_offset || dir_length < 0x54) {
      return fail(Status::Decode, "CHM directory is truncated");
    }
    const ByteView dir = data_.subspan(static_cast<std::size_t>(dir_offset), static_cast<std::size_t>(dir_length));
    if (as_chars(dir.first(4)) != "ITSP") return fail(Status::Decode, "CHM directory header is damaged");
    const std::uint32_t itsp_len = le32(dir, 8);
    const std::uint32_t chunk_size = le32(dir, 16);
    const std::uint32_t chunks = le32(dir, 44);
    if (itsp_len > dir.size() || chunk_size < 32 || chunk_size > (1u << 20)) return fail(Status::Decode, "CHM directory header is damaged");
    const ByteView area = dir.subspan(itsp_len);
    const std::uint64_t available = area.size() / chunk_size;
    if (chunks > available) return fail(Status::Decode, "CHM directory is truncated");
    for (std::uint32_t c = 0; c < chunks; ++c) {
      const ByteView chunk = area.subspan(static_cast<std::size_t>(c) * chunk_size, chunk_size);
      if (as_chars(chunk.first(4)) != "PMGL") continue;  // index chunks (PMGI) are not needed
      const std::uint32_t free = le32(chunk, 4);
      if (free > chunk_size - 20) return fail(Status::Decode, "CHM listing chunk is damaged");
      const std::size_t end = chunk_size - free;
      std::size_t pos = 20;
      while (pos < end) {
        const auto name_len = encint(chunk, pos, end);
        if (!name_len || *name_len > end - pos) return fail(Status::Decode, "CHM listing entry is damaged");
        std::string name(as_chars(chunk.subspan(pos, static_cast<std::size_t>(*name_len))));
        pos += static_cast<std::size_t>(*name_len);
        const auto section = encint(chunk, pos, end);
        const auto offset = encint(chunk, pos, end);
        const auto length = encint(chunk, pos, end);
        if (!section || !offset || !length) return fail(Status::Decode, "CHM listing entry is damaged");
        if (items_.size() >= ctx_.limits.max_archive_entries + 64) return fail(Status::Limit, "CHM file lists too many entries");
        items_[std::move(name)] = {*section, *offset, *length};
      }
    }
    return {};
  }

  Result<ByteView> section0(const Item& item) const {
    const std::uint64_t at = content_offset_ + item.offset;
    if (at < content_offset_ || at > data_.size() || item.length > data_.size() - at) {
      return fail(Status::Decode, "CHM content is truncated");
    }
    return data_.subspan(static_cast<std::size_t>(at), static_cast<std::size_t>(item.length));
  }

  Result<ByteView> internal(std::string_view name) const {
    const auto found = items_.find(std::string(name));
    if (found == items_.end() || found->second.section != 0) return fail(Status::Decode, "CHM file lacks {}", name);
    return section0(found->second);
  }

  Result<void> setup_compressed() {
    if (lzx_ready_) return {};
    CVT_TRY_ASSIGN(const ByteView control, internal(control_name));
    CVT_TRY_ASSIGN(const ByteView reset, internal(reset_name));
    CVT_TRY_ASSIGN(content_, internal(content_name));
    if (control.size() < 24 || as_chars(control.subspan(4, 4)) != "LZXC") return fail(Status::Decode, "CHM compression control data is damaged");
    std::uint64_t interval = le32(control, 12);
    std::uint64_t window = le32(control, 16);
    if (le32(control, 8) == 2) {
      interval *= 0x8000;
      window *= 0x8000;
    }
    if (reset.size() < 40) return fail(Status::Decode, "CHM reset table is damaged");
    const std::uint32_t entries = le32(reset, 4);
    const std::uint32_t entry_size = le32(reset, 8);
    const std::uint32_t header = le32(reset, 12);
    uncompressed_ = le64(reset, 16);
    const std::uint64_t compressed = le64(reset, 24);
    frame_ = le64(reset, 32);
    if (entry_size != 8 || header > reset.size() || entries > (reset.size() - header) / 8 || frame_ != 0x8000) {
      return fail(Status::Decode, "CHM reset table is damaged");
    }
    if (!std::has_single_bit(window) || window < (1u << 15) || window > (1u << 21) || interval < frame_ ||
        interval % frame_ != 0) {
      return fail(Status::Decode, "CHM compression parameters are not supported");
    }
    window_bits_ = std::countr_zero(window);
    frames_per_interval_ = interval / frame_;
    if (compressed > content_.size()) return fail(Status::Decode, "CHM compressed content is truncated");
    content_ = content_.first(static_cast<std::size_t>(compressed));
    for (std::uint32_t i = 0; i < entries; ++i) offsets_.push_back(le64(reset, header + static_cast<std::size_t>(i) * 8));
    if ((uncompressed_ + frame_ - 1) / frame_ > offsets_.size()) return fail(Status::Decode, "CHM reset table is too short");
    lzx_ready_ = true;
    return {};
  }

  Result<const Bytes*> interval(std::uint64_t k) {
    if (const auto found = cache_.find(k); found != cache_.end()) return &found->second;
    const std::uint64_t first = k * frames_per_interval_;
    const std::uint64_t span = frames_per_interval_ * frame_;
    const std::uint64_t start_out = k * span;
    if (start_out >= uncompressed_) return fail(Status::Decode, "CHM file lies beyond the compressed content");
    const std::uint64_t size = std::min(span, uncompressed_ - start_out);
    if (size > sink_.budget()) return fail(Status::Limit, "CHM content of {} bytes exceeds the limit", size);
    const std::uint64_t from = offsets_[static_cast<std::size_t>(first)];
    const std::uint64_t last = first + frames_per_interval_;
    const std::uint64_t to = last < offsets_.size() ? offsets_[static_cast<std::size_t>(last)] : content_.size();
    if (from > to || to > content_.size()) return fail(Status::Decode, "CHM reset table is damaged");
    std::vector<std::uint32_t> frames;
    for (std::uint64_t done = 0; done < size; done += frame_) frames.push_back(static_cast<std::uint32_t>(std::min(frame_, size - done)));
    compression::DecodeOptions d;
    d.window_bits = window_bits_;
    d.block_sizes = frames;
    d.output_size = size;
    d.max_output = size;
    CVT_TRY_ASSIGN(Bytes out, compression::decompress(compression::Method::Lzx,
                                                     content_.subspan(static_cast<std::size_t>(from), static_cast<std::size_t>(to - from)), d));
    if (out.size() != size) return fail(Status::Decode, "CHM content decompressed to the wrong size");
    // Keep the cache small: files are listed in name order, not content order.
    if (cache_.size() >= 8) cache_.erase(cache_.begin());
    return &cache_.emplace(k, std::move(out)).first->second;
  }

  Result<Bytes> content(const Item& item) {
    if (item.length > sink_.budget()) return fail(Status::Limit, "CHM file of {} bytes exceeds the limit", item.length);
    if (item.section == 0) {
      CVT_TRY_ASSIGN(const ByteView view, section0(item));
      return to_bytes(view);
    }
    if (item.section != 1) return fail(Status::Decode, "CHM content section {} is not known", item.section);
    if (!compression::can_decode(compression::Method::Lzx)) {
      return fail(Status::Decode, "LZX compressed CHM content cannot be read by this build");
    }
    CVT_TRY(setup_compressed());
    if (item.offset > uncompressed_ || item.length > uncompressed_ - item.offset) {
      return fail(Status::Decode, "CHM file lies beyond the compressed content");
    }
    Bytes out;
    out.reserve(static_cast<std::size_t>(item.length));
    const std::uint64_t span = frames_per_interval_ * frame_;
    std::uint64_t at = item.offset;
    const std::uint64_t end = item.offset + item.length;
    while (at < end) {
      const std::uint64_t k = at / span;
      CVT_TRY_ASSIGN(const Bytes* block, interval(k));
      const std::uint64_t in = at - k * span;
      if (in >= block->size()) return fail(Status::Decode, "CHM file lies beyond the compressed content");
      const std::uint64_t take = std::min<std::uint64_t>(end - at, block->size() - in);
      out.insert(out.end(), block->begin() + static_cast<std::ptrdiff_t>(in), block->begin() + static_cast<std::ptrdiff_t>(in + take));
      at += take;
    }
    return out;
  }

  ByteView data_;
  EntrySink& sink_;
  const Context& ctx_;
  std::uint64_t content_offset_ = 0;
  std::map<std::string, Item> items_;
  bool lzx_ready_ = false;
  ByteView content_;
  std::uint64_t uncompressed_ = 0;
  std::uint64_t frame_ = 0x8000;
  std::uint64_t frames_per_interval_ = 1;
  int window_bits_ = 16;
  std::vector<std::uint64_t> offsets_;
  std::map<std::uint64_t, Bytes> cache_;
};

}  // namespace

Result<ir::Archive> read_chm(ReadContext& ctx) {
  EntrySink sink(ctx, ctx.input.size());
  Chm chm(ctx.input, sink, ctx);
  CVT_TRY(chm.run());
  return sink.finish();
}

}  // namespace cvt::archive::cab
