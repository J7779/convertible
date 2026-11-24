// Convertible core - science family: HDF5 reader (also MATLAB 7.3 files).
//
// Superblock versions 0-3 (with user blocks), object headers v1 and v2 with
// continuation blocks, old-style groups (symbol tables, v1 B-trees, local
// heaps) and new-style groups (compact link messages; dense links through the
// v2 B-tree name index and the fractal heap), hard links only. Datasets:
// compact, contiguous and chunked (v1 B-tree; single-chunk, implicit and
// fixed-array indexes), filters deflate, shuffle and Fletcher-32. Types:
// integers, floats (IEEE), fixed and variable-length strings (global heap),
// compound types of those, enums (as their base type). Attributes on
// datasets and groups. Cycles in the group graph and absurd sizes are refused.
// Output: the array collection (dataset paths as names).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstring>
#include <format>
#include <set>

#include "families/science/arrays.hpp"
#include "families/science/science.hpp"
#include "util/deflate.hpp"
#include "util/text.hpp"

namespace cvt::science::arrays {

namespace {

constexpr std::uint64_t undefined_address = ~std::uint64_t{0};

struct H5Type {
  int cls = -1;  // 0 fixed, 1 float, 3 string, 6 compound, 8 enum, 9 vlen
  std::uint32_t size = 0;
  bool big_endian = false;
  bool is_signed = false;
  bool vlen_string = false;
  struct Member {
    std::string name;
    std::uint32_t offset = 0;
    std::shared_ptr<H5Type> type;
  };
  std::vector<Member> members;
  std::shared_ptr<H5Type> base;  // enum / vlen
};

struct Message {
  std::uint16_t type = 0;
  ByteView data;
};

class H5File {
 public:
  H5File(ByteView data, const Context& ctx) : data_(data), ctx_(ctx) {}

  Result<void> open() {
    // The superblock is at 0, 512, 1024, 2048, ...
    std::uint64_t at = 0;
    while (true) {
      if (at + 8 > data_.size()) return fail(Status::Decode, "no HDF5 superblock found");
      if (as_chars(data_.subspan(static_cast<std::size_t>(at), 8)) == "\x89HDF\r\n\x1a\n") break;
      at = at == 0 ? 512 : at * 2;
    }
    util::ByteReader r(data_.subspan(static_cast<std::size_t>(at + 8)));
    version_ = r.u8();
    if (version_ <= 1) {
      (void)r.u8();
      (void)r.u8();
      (void)r.u8();
      (void)r.u8();
      offset_size_ = r.u8();
      length_size_ = r.u8();
      (void)r.u8();
      (void)r.u16le();  // group leaf K
      (void)r.u16le();  // group internal K
      (void)r.u32le();  // flags
      if (version_ == 1) {
        (void)r.u16le();
        (void)r.u16le();
      }
      CVT_TRY(check_sizes());
      base_ = offset(r);
      (void)offset(r);  // free space
      (void)offset(r);  // end of file
      (void)offset(r);  // driver info
      (void)offset(r);  // root link name offset
      root_ = offset(r);
    } else if (version_ == 2 || version_ == 3) {
      offset_size_ = r.u8();
      length_size_ = r.u8();
      (void)r.u8();
      CVT_TRY(check_sizes());
      base_ = offset(r);
      (void)offset(r);  // extension
      (void)offset(r);  // end of file
      root_ = offset(r);
    } else {
      return fail(Status::Decode, "unsupported HDF5 superblock version {}", version_);
    }
    if (!r.ok()) return fail(Status::Decode, "HDF5 superblock is truncated");
    if (base_ == undefined_address) base_ = 0;
    if (base_ == 0) base_ = at;
    return {};
  }

  Result<void> walk(std::uint64_t header, const std::string& path, Collection& out, int depth) {
    if (depth > max_nesting) return fail(Status::Limit, "HDF5 groups are nested too deeply");
    if (!visited_.insert(header).second) {
      ctx_.warn("HDF5 link cycle or shared object at '" + path + "' skipped.");
      return {};
    }
    if (++objects_ > 100000) return fail(Status::Limit, "too many HDF5 objects");
    CVT_TRY(ctx_.checkpoint(0.5));
    CVT_TRY_ASSIGN(const auto messages, object_header(header));
    bool is_dataset = false;
    for (const Message& m : messages) is_dataset = is_dataset || m.type == 0x0008;
    // Attributes.
    Json attributes = Json::object();
    for (const Message& m : messages) {
      if (m.type == 0x000C) {
        auto attr = attribute(m.data);
        if (attr) attributes[attr->first] = std::move(attr->second);
      } else if (m.type == 0x0015) {
        CVT_TRY(dense_attributes(m.data, attributes));
      }
    }
    if (is_dataset) {
      CVT_TRY_ASSIGN(Array a, dataset(messages));
      a.name = path.empty() ? "/" : path;
      a.attributes = std::move(attributes);
      out.arrays.push_back(std::move(a));
      return {};
    }
    if (!attributes.empty()) {
      if (path.empty()) {
        for (auto& [k, v] : attributes.items()) out.attributes[k] = v;
      } else {
        out.attributes[path] = std::move(attributes);
      }
    }
    // Group members.
    std::vector<std::pair<std::string, std::uint64_t>> links;
    for (const Message& m : messages) {
      if (m.type == 0x0011) {
        CVT_TRY(symbol_table(m.data, links));
      } else if (m.type == 0x0006) {
        CVT_TRY(link_message(m.data, links));
      } else if (m.type == 0x0002) {
        CVT_TRY(dense_links(m.data, links));
      }
    }
    for (const auto& [name, address] : links) {
      CVT_TRY(walk(address, path + "/" + name, out, depth + 1));
    }
    return {};
  }

  [[nodiscard]] std::uint64_t root() const { return root_; }

 private:
  Result<void> check_sizes() const {
    const auto ok = [](std::uint8_t s) { return s == 2 || s == 4 || s == 8; };
    if (!ok(offset_size_) || !ok(length_size_)) return fail(Status::Decode, "bad HDF5 offset/length sizes");
    return {};
  }

  std::uint64_t sized(util::ByteReader& r, std::uint8_t size) const {
    std::uint64_t v = 0;
    for (std::uint8_t i = 0; i < size; ++i) v |= std::uint64_t{r.u8()} << (8 * i);
    if (size < 8 && v == (std::uint64_t{1} << (8 * size)) - 1) return undefined_address;
    return v;
  }
  std::uint64_t offset(util::ByteReader& r) const { return sized(r, offset_size_); }
  std::uint64_t length(util::ByteReader& r) const { return sized(r, length_size_); }

  Result<ByteView> at(std::uint64_t address, std::uint64_t size) const {
    if (address == undefined_address) return fail(Status::Decode, "undefined HDF5 address");
    const std::uint64_t absolute = address + base_;
    if (absolute < address || absolute > data_.size() || size > data_.size() - absolute) {
      return fail(Status::Decode, "HDF5 address {} is outside the file", address);
    }
    return data_.subspan(static_cast<std::size_t>(absolute), static_cast<std::size_t>(size));
  }
  Result<ByteView> from(std::uint64_t address) const {
    if (address == undefined_address || address + base_ > data_.size()) return fail(Status::Decode, "HDF5 address {} is outside the file", address);
    return data_.subspan(static_cast<std::size_t>(address + base_));
  }

  // ------------------------------------------------------------------- object headers

  Result<std::vector<Message>> object_header(std::uint64_t address) {
    CVT_TRY_ASSIGN(const ByteView start, from(address));
    std::vector<Message> messages;
    std::vector<std::pair<ByteView, bool>> blocks;  // (block, v2)
    if (start.size() >= 4 && as_chars(start.first(4)) == "OHDR") {
      util::ByteReader r(start.subspan(4));
      if (r.u8() != 2) return fail(Status::Decode, "unsupported HDF5 object header version");
      const std::uint8_t flags = r.u8();
      if (flags & 0x20) (void)r.bytes(16);
      if (flags & 0x10) (void)r.bytes(4);
      const std::uint8_t width = static_cast<std::uint8_t>(1u << (flags & 3));
      const std::uint64_t size = sized(r, width);
      if (size > r.remaining()) return fail(Status::Decode, "HDF5 object header is truncated");
      creation_order_ = (flags & 0x04) != 0;
      blocks.emplace_back(r.bytes(static_cast<std::size_t>(size)), true);
    } else {
      util::ByteReader r(start);
      if (r.u8() != 1) return fail(Status::Decode, "unsupported HDF5 object header at {}", address);
      (void)r.u8();
      (void)r.u16le();  // message count
      (void)r.u32le();  // reference count
      const std::uint32_t size = r.u32le();
      (void)r.u32le();  // alignment padding
      if (size > r.remaining()) return fail(Status::Decode, "HDF5 object header is truncated");
      blocks.emplace_back(r.bytes(size), false);
    }
    for (std::size_t b = 0; b < blocks.size(); ++b) {
      if (blocks.size() > 1024) return fail(Status::Limit, "too many HDF5 header continuation blocks");
      auto [block, v2] = blocks[b];
      util::ByteReader r(block);
      while (r.remaining() >= (v2 ? 4u : 8u)) {
        Message m;
        std::uint16_t size = 0;
        if (v2) {
          m.type = r.u8();
          size = r.u16le();
          const std::uint8_t flags = r.u8();
          if (creation_order_) (void)r.u16le();
          (void)flags;
        } else {
          m.type = r.u16le();
          size = r.u16le();
          (void)r.u8();
          (void)r.bytes(3);
        }
        if (size > r.remaining()) break;
        m.data = r.bytes(size);
        if (m.type == 0x0010) {
          util::ByteReader c(m.data);
          const std::uint64_t where = offset(c);
          const std::uint64_t len = length(c);
          CVT_TRY_ASSIGN(ByteView next, at(where, len));
          if (next.size() >= 4 && as_chars(next.first(4)) == "OCHK") {
            blocks.emplace_back(next.subspan(4, next.size() >= 8 ? next.size() - 8 : 0), true);
          } else {
            blocks.emplace_back(next, false);
          }
          continue;
        }
        messages.push_back(m);
        if (messages.size() > 65536) return fail(Status::Limit, "too many HDF5 header messages");
      }
    }
    return messages;
  }

  // ------------------------------------------------------------------- groups

  Result<void> symbol_table(ByteView data, std::vector<std::pair<std::string, std::uint64_t>>& links) {
    util::ByteReader r(data);
    const std::uint64_t btree = offset(r);
    const std::uint64_t heap = offset(r);
    CVT_TRY_ASSIGN(const ByteView heap_header, at(heap, 8 + 2 * std::uint64_t{length_size_} + offset_size_));
    util::ByteReader h(heap_header);
    if (h.chars(4) != "HEAP") return fail(Status::Decode, "bad HDF5 local heap");
    (void)h.u8();
    (void)h.bytes(3);
    const std::uint64_t heap_size = length(h);
    (void)length(h);
    const std::uint64_t heap_data = offset(h);
    CVT_TRY_ASSIGN(const ByteView names, at(heap_data, heap_size));
    return group_btree(btree, names, links, 0);
  }

  Result<void> group_btree(std::uint64_t address, ByteView names, std::vector<std::pair<std::string, std::uint64_t>>& links,
                           int depth) {
    if (depth > 32) return fail(Status::Limit, "HDF5 B-tree is too deep");
    CVT_TRY_ASSIGN(const ByteView node, from(address));
    util::ByteReader r(node);
    if (r.chars(4) != "TREE") return fail(Status::Decode, "bad HDF5 group B-tree node");
    const std::uint8_t type = r.u8();
    const std::uint8_t level = r.u8();
    const std::uint16_t entries = r.u16le();
    (void)offset(r);
    (void)offset(r);
    if (type != 0) return fail(Status::Decode, "unexpected HDF5 B-tree type");
    for (std::uint16_t i = 0; i < entries; ++i) {
      (void)length(r);  // key
      const std::uint64_t child = offset(r);
      if (!r.ok()) return fail(Status::Decode, "HDF5 B-tree node is truncated");
      if (level > 0) {
        CVT_TRY(group_btree(child, names, links, depth + 1));
        continue;
      }
      CVT_TRY_ASSIGN(const ByteView snod, from(child));
      util::ByteReader s(snod);
      if (s.chars(4) != "SNOD") return fail(Status::Decode, "bad HDF5 symbol table node");
      (void)s.u8();
      (void)s.u8();
      const std::uint16_t symbols = s.u16le();
      for (std::uint16_t k = 0; k < symbols; ++k) {
        const std::uint64_t name_offset = offset(s);
        const std::uint64_t header = offset(s);
        (void)s.u32le();
        (void)s.u32le();
        (void)s.bytes(16);
        if (!s.ok()) return fail(Status::Decode, "HDF5 symbol table node is truncated");
        if (name_offset >= names.size()) return fail(Status::Decode, "bad HDF5 link name offset");
        std::string_view name = as_chars(names.subspan(static_cast<std::size_t>(name_offset)));
        name = name.substr(0, name.find('\0'));
        links.emplace_back(util::sanitize_utf8(name), header);
      }
    }
    return {};
  }

  Result<void> link_message(ByteView data, std::vector<std::pair<std::string, std::uint64_t>>& links) {
    util::ByteReader r(data);
    if (r.u8() != 1) return fail(Status::Decode, "unsupported HDF5 link message version");
    const std::uint8_t flags = r.u8();
    std::uint8_t link_type = 0;
    if (flags & 0x08) link_type = r.u8();
    if (flags & 0x04) (void)r.u64le();
    if (flags & 0x10) (void)r.u8();
    const std::uint8_t width = static_cast<std::uint8_t>(1u << (flags & 3));
    std::uint64_t name_length = 0;
    for (std::uint8_t i = 0; i < width; ++i) name_length |= std::uint64_t{r.u8()} << (8 * i);
    if (name_length > r.remaining()) return fail(Status::Decode, "HDF5 link name is truncated");
    const std::string name = util::sanitize_utf8(r.chars(static_cast<std::size_t>(name_length)));
    if (link_type != 0) {
      ctx_.warn("HDF5 soft/external link '" + name + "' was not followed.");
      return {};
    }
    const std::uint64_t address = offset(r);
    if (!r.ok()) return fail(Status::Decode, "HDF5 link message is truncated");
    links.emplace_back(name, address);
    return {};
  }

  // Fractal heap + v2 B-tree (dense links and attributes).
  struct FractalHeap {
    std::uint16_t table_width = 0;
    std::uint64_t starting_block = 0;
    std::uint64_t max_direct = 0;
    std::uint16_t max_heap_bits = 0;
    std::uint16_t root_rows = 0;
    std::uint64_t root = undefined_address;
    std::uint16_t io_filter_size = 0;
    bool checksummed = false;
  };

  Result<FractalHeap> fractal_heap(std::uint64_t address) {
    CVT_TRY_ASSIGN(const ByteView node, from(address));
    util::ByteReader r(node);
    if (r.chars(4) != "FRHP") return fail(Status::Decode, "bad HDF5 fractal heap header");
    (void)r.u8();
    FractalHeap h;
    (void)r.u16le();  // heap id length
    h.io_filter_size = r.u16le();
    const std::uint8_t flags = r.u8();
    h.checksummed = (flags & 0x02) != 0;
    (void)r.u32le();  // max managed object size
    (void)length(r);  // next huge id
    (void)offset(r);  // huge object btree
    (void)length(r);  // free space
    (void)offset(r);  // free space manager
    (void)length(r);  // managed space
    (void)length(r);  // allocated managed
    (void)length(r);  // iterator offset
    (void)length(r);  // managed objects
    (void)length(r);  // huge size
    (void)length(r);  // huge objects
    (void)length(r);  // tiny size
    (void)length(r);  // tiny objects
    h.table_width = r.u16le();
    h.starting_block = length(r);
    h.max_direct = length(r);
    h.max_heap_bits = r.u16le();
    (void)r.u16le();  // starting rows
    h.root = offset(r);
    h.root_rows = r.u16le();
    if (!r.ok()) return fail(Status::Decode, "HDF5 fractal heap header is truncated");
    if (h.io_filter_size != 0) return fail(Status::Decode, "filtered HDF5 fractal heaps are not supported");
    if (h.table_width == 0 || h.table_width > 1024 || h.starting_block == 0 || h.max_heap_bits > 64) {
      return fail(Status::Decode, "bad HDF5 fractal heap geometry");
    }
    return h;
  }

  /// Bytes of a managed object given its heap offset and length.
  Result<ByteView> heap_object(const FractalHeap& h, std::uint64_t heap_offset, std::uint64_t size) {
    const std::size_t block_offset_bytes = static_cast<std::size_t>((h.max_heap_bits + 7) / 8);
    std::uint64_t block_address = h.root;
    std::uint16_t rows = h.root_rows;
    std::uint64_t block_start = 0;  // heap offset of the current block
    for (int depth = 0; depth < 16; ++depth) {
      if (rows == 0) {
        // Direct block.
        CVT_TRY_ASSIGN(const ByteView block, from(block_address));
        const std::uint64_t within = heap_offset - block_start;
        const std::uint64_t header = 5 + offset_size_ + block_offset_bytes;
        (void)header;
        if (within > block.size() || size > block.size() - within) return fail(Status::Decode, "HDF5 heap object out of range");
        return block.subspan(static_cast<std::size_t>(within), static_cast<std::size_t>(size));
      }
      // Indirect block: rows of table_width entries; row sizes double after the second row.
      CVT_TRY_ASSIGN(const ByteView block, from(block_address));
      util::ByteReader r(block);
      if (r.chars(4) != "FHIB") return fail(Status::Decode, "bad HDF5 fractal heap indirect block");
      (void)r.u8();
      (void)offset(r);
      std::uint64_t own_offset = 0;
      for (std::size_t i = 0; i < block_offset_bytes; ++i) own_offset |= std::uint64_t{r.u8()} << (8 * i);
      std::uint64_t position = own_offset;
      bool found = false;
      for (std::uint16_t row = 0; row < rows && !found; ++row) {
        const std::uint64_t row_size = row < 2 ? h.starting_block : h.starting_block << (row - 1);
        const bool direct = row_size <= h.max_direct;
        for (std::uint16_t col = 0; col < h.table_width; ++col) {
          const std::uint64_t child = offset(r);
          if (!r.ok()) return fail(Status::Decode, "HDF5 indirect block is truncated");
          if (heap_offset >= position && heap_offset < position + row_size) {
            if (child == undefined_address) return fail(Status::Decode, "HDF5 heap object in an unallocated block");
            block_address = child;
            block_start = position;
            if (direct) {
              rows = 0;
            } else {
              // Child indirect block rows: log2(row_size / starting_block) - log2(table_width) + 1.
              std::uint64_t ratio = row_size / h.starting_block;
              std::uint16_t bits = 0;
              while (ratio > 1) {
                ratio >>= 1;
                ++bits;
              }
              std::uint16_t width_bits = 0;
              for (std::uint32_t w = h.table_width; w > 1; w >>= 1) ++width_bits;
              rows = static_cast<std::uint16_t>(bits - width_bits + 1);
            }
            found = true;
            break;
          }
          position += row_size;
        }
      }
      if (!found) return fail(Status::Decode, "HDF5 heap offset {} not found", heap_offset);
    }
    return fail(Status::Decode, "HDF5 fractal heap is too deep");
  }

  Result<ByteView> heap_id(const FractalHeap& h, ByteView id) {
    if (id.empty()) return fail(Status::Decode, "empty HDF5 heap id");
    const std::uint8_t first = static_cast<std::uint8_t>(id[0]);
    const int kind = (first >> 4) & 3;
    if (kind != 0) return fail(Status::Decode, "huge/tiny HDF5 heap objects are not supported");
    const std::size_t offset_bytes = static_cast<std::size_t>((h.max_heap_bits + 7) / 8);
    util::ByteReader r(id.subspan(1));
    std::uint64_t heap_offset = 0;
    for (std::size_t i = 0; i < offset_bytes; ++i) heap_offset |= std::uint64_t{r.u8()} << (8 * i);
    std::uint64_t size = 0;
    const std::size_t length_bytes = std::min<std::size_t>(8, id.size() - 1 - offset_bytes);
    for (std::size_t i = 0; i < length_bytes; ++i) size |= std::uint64_t{r.u8()} << (8 * i);
    return heap_object(h, heap_offset, size);
  }

  /// Visit every record of a v2 B-tree.
  Result<void> btree2_records(std::uint64_t address, const std::function<Result<void>(ByteView)>& visit) {
    CVT_TRY_ASSIGN(const ByteView node, from(address));
    util::ByteReader r(node);
    if (r.chars(4) != "BTHD") return fail(Status::Decode, "bad HDF5 v2 B-tree header");
    (void)r.u8();
    (void)r.u8();  // type
    (void)r.u32le();
    const std::uint16_t record_size = r.u16le();
    const std::uint16_t depth = r.u16le();
    (void)r.u8();
    (void)r.u8();
    const std::uint64_t root = offset(r);
    const std::uint16_t root_records = r.u16le();
    const std::uint64_t total = length(r);
    if (!r.ok() || depth > 16 || record_size == 0) return fail(Status::Decode, "bad HDF5 v2 B-tree header");
    if (root == undefined_address || total == 0) return {};
    return btree2_node(root, depth, root_records, record_size, total, visit, 0);
  }

  Result<void> btree2_node(std::uint64_t address, std::uint16_t depth, std::uint16_t records, std::uint16_t record_size,
                           std::uint64_t total, const std::function<Result<void>(ByteView)>& visit, int guard) {
    if (guard > 20) return fail(Status::Limit, "HDF5 v2 B-tree is too deep");
    CVT_TRY_ASSIGN(const ByteView node, from(address));
    util::ByteReader r(node);
    const std::string_view sig = r.chars(4);
    if (sig != (depth == 0 ? "BTLF" : "BTIN")) return fail(Status::Decode, "bad HDF5 v2 B-tree node");
    (void)r.u8();
    (void)r.u8();
    std::vector<ByteView> recs;
    for (std::uint16_t i = 0; i < records; ++i) recs.push_back(r.bytes(record_size));
    if (!r.ok()) return fail(Status::Decode, "HDF5 v2 B-tree node is truncated");
    if (depth == 0) {
      for (const ByteView rec : recs) CVT_TRY(visit(rec));
      return {};
    }
    // Child pointers: address, record count (bytes for max records), total records (deeper levels).
    const std::size_t count_bytes = total < 256 ? 1 : total < 65536 ? 2 : total < (1ull << 32) ? 4 : 8;
    for (std::uint16_t i = 0; i <= records; ++i) {
      const std::uint64_t child = offset(r);
      std::uint64_t child_records = 0;
      for (std::size_t b = 0; b < count_bytes; ++b) child_records |= std::uint64_t{r.u8()} << (8 * b);
      if (depth > 1) {
        for (std::size_t b = 0; b < count_bytes; ++b) (void)r.u8();
      }
      if (!r.ok()) return fail(Status::Decode, "HDF5 v2 B-tree node is truncated");
      if (child_records > 65535) return fail(Status::Decode, "bad HDF5 v2 B-tree record count");
      CVT_TRY(btree2_node(child, static_cast<std::uint16_t>(depth - 1), static_cast<std::uint16_t>(child_records), record_size,
                          total, visit, guard + 1));
      if (i < records) CVT_TRY(visit(recs[i]));
    }
    return {};
  }

  Result<void> dense_links(ByteView data, std::vector<std::pair<std::string, std::uint64_t>>& links) {
    util::ByteReader r(data);
    (void)r.u8();
    const std::uint8_t flags = r.u8();
    if (flags & 1) (void)r.u64le();
    const std::uint64_t heap = offset(r);
    const std::uint64_t names = offset(r);
    if (heap == undefined_address || names == undefined_address) return {};
    CVT_TRY_ASSIGN(const FractalHeap h, fractal_heap(heap));
    return btree2_records(names, [&](ByteView record) -> Result<void> {
      if (record.size() < 5) return fail(Status::Decode, "bad HDF5 link record");
      CVT_TRY_ASSIGN(const ByteView message, heap_id(h, record.subspan(4)));
      return link_message(message, links);
    });
  }

  Result<void> dense_attributes(ByteView data, Json& attributes) {
    util::ByteReader r(data);
    (void)r.u8();
    const std::uint8_t flags = r.u8();
    if (flags & 1) (void)r.u16le();
    const std::uint64_t heap = offset(r);
    const std::uint64_t names = offset(r);
    if (heap == undefined_address || names == undefined_address) return {};
    CVT_TRY_ASSIGN(const FractalHeap h, fractal_heap(heap));
    return btree2_records(names, [&](ByteView record) -> Result<void> {
      if (record.size() < 8) return fail(Status::Decode, "bad HDF5 attribute record");
      CVT_TRY_ASSIGN(const ByteView message, heap_id(h, record.first(8)));
      auto attr = attribute(message);
      if (attr) attributes[attr->first] = std::move(attr->second);
      return {};
    });
  }

  // ------------------------------------------------------------------- types and spaces

  Result<std::shared_ptr<H5Type>> datatype(util::ByteReader& r, int depth) {
    if (depth > 8) return fail(Status::Decode, "HDF5 datatype is nested too deeply");
    auto t = std::make_shared<H5Type>();
    const std::uint8_t cv = r.u8();
    const int version = cv >> 4;
    t->cls = cv & 0x0F;
    const std::uint8_t b0 = r.u8();
    const std::uint8_t b1 = r.u8();
    (void)r.u8();
    t->size = r.u32le();
    if (t->size == 0 || t->size > (1u << 24)) return fail(Status::Decode, "bad HDF5 datatype size");
    switch (t->cls) {
      case 0:
        t->big_endian = b0 & 1;
        t->is_signed = (b0 & 0x08) != 0;
        (void)r.u16le();
        (void)r.u16le();
        break;
      case 1:
        t->big_endian = b0 & 1;
        (void)r.bytes(12);
        break;
      case 3: break;
      case 4:  // bitfield
        t->big_endian = b0 & 1;
        (void)r.u32le();
        t->cls = 0;
        break;
      case 5: (void)r.bytes(static_cast<std::size_t>(b0)); break;  // opaque tag
      case 6: {
        const std::uint16_t members = static_cast<std::uint16_t>(b0 | (b1 << 8));
        for (std::uint16_t m = 0; m < members; ++m) {
          H5Type::Member member;
          if (version < 3) {
            const std::size_t start = r.position();
            member.name = std::string(r.cstring());
            const std::size_t used = r.position() - start;
            (void)r.bytes(std::min<std::size_t>((8 - used % 8) % 8, r.remaining()));
            member.offset = r.u32le();
            (void)r.bytes(version == 1 ? 28 : 0);
          } else {
            member.name = std::string(r.cstring());
            std::uint32_t off = 0;
            const std::size_t width = t->size < 256 ? 1 : t->size < 65536 ? 2 : t->size < (1u << 24) ? 3 : 4;
            for (std::size_t i = 0; i < width; ++i) off |= std::uint32_t{r.u8()} << (8 * i);
            member.offset = off;
          }
          CVT_TRY_ASSIGN(member.type, datatype(r, depth + 1));
          if (std::uint64_t{member.offset} + member.type->size > t->size) return fail(Status::Decode, "HDF5 compound member out of range");
          member.name = util::sanitize_utf8(member.name);
          t->members.push_back(std::move(member));
          if (!r.ok()) return fail(Status::Decode, "HDF5 compound type is truncated");
        }
        break;
      }
      case 7: break;  // reference
      case 8: {
        CVT_TRY_ASSIGN(t->base, datatype(r, depth + 1));
        const std::uint16_t members = static_cast<std::uint16_t>(b0 | (b1 << 8));
        for (std::uint16_t m = 0; m < members; ++m) {
          const std::size_t start = r.position();
          (void)r.cstring();
          if (version < 3) (void)r.bytes(std::min<std::size_t>((8 - (r.position() - start) % 8) % 8, r.remaining()));
        }
        (void)r.bytes(std::min<std::size_t>(std::size_t{members} * t->base->size, r.remaining()));
        break;
      }
      case 9: {
        t->vlen_string = (b0 & 0x0F) == 1;
        CVT_TRY_ASSIGN(t->base, datatype(r, depth + 1));
        break;
      }
      case 10: return fail(Status::Decode, "HDF5 array datatypes are not supported");
      default: return fail(Status::Decode, "unsupported HDF5 datatype class {}", t->cls);
    }
    if (!r.ok()) return fail(Status::Decode, "HDF5 datatype is truncated");
    return t;
  }

  Result<std::vector<std::uint64_t>> dataspace(util::ByteReader& r) {
    const std::uint8_t version = r.u8();
    const std::uint8_t rank = r.u8();
    const std::uint8_t flags = r.u8();
    std::uint8_t type = 1;
    if (version == 1) {
      (void)r.u8();
      (void)r.u32le();
    } else if (version == 2) {
      type = r.u8();
    } else {
      return fail(Status::Decode, "unsupported HDF5 dataspace version {}", version);
    }
    if (rank > 32) return fail(Status::Decode, "HDF5 dataspace rank is too high");
    std::vector<std::uint64_t> dims;
    for (std::uint8_t i = 0; i < rank; ++i) dims.push_back(length(r));
    if (flags & 1) {
      for (std::uint8_t i = 0; i < rank; ++i) (void)length(r);
    }
    if (!r.ok()) return fail(Status::Decode, "HDF5 dataspace is truncated");
    if (type == 2) return std::vector<std::uint64_t>{0};  // null space
    return dims;
  }

  /// Our dtype for an HDF5 type (records for compounds).
  Result<std::pair<DType, std::vector<Field>>> map_type(const H5Type& t) {
    switch (t.cls) {
      case 0:
        if (t.size != 1 && t.size != 2 && t.size != 4 && t.size != 8) return fail(Status::Decode, "unsupported HDF5 integer size {}", t.size);
        return std::pair{DType{t.is_signed ? ScalarKind::Int : ScalarKind::UInt, t.size}, std::vector<Field>{}};
      case 1:
        if (t.size != 2 && t.size != 4 && t.size != 8) return fail(Status::Decode, "unsupported HDF5 float size {}", t.size);
        return std::pair{DType{ScalarKind::Float, t.size}, std::vector<Field>{}};
      case 3:
      case 5: return std::pair{DType{ScalarKind::Bytes, t.size}, std::vector<Field>{}};
      case 8: return map_type(*t.base);
      case 9: return std::pair{DType{ScalarKind::Unicode, 4 * 256}, std::vector<Field>{}};
      case 6: {
        std::vector<Field> fields;
        std::uint32_t offset = 0;
        for (const auto& m : t.members) {
          CVT_TRY_ASSIGN(const auto mapped, map_type(*m.type));
          if (mapped.first.kind == ScalarKind::Record) return fail(Status::Decode, "nested HDF5 compound types are not supported");
          fields.push_back(Field{m.name, mapped.first, offset});
          offset += mapped.first.size;
        }
        return std::pair{DType{ScalarKind::Record, offset}, fields};
      }
      default: return fail(Status::Decode, "unsupported HDF5 datatype class {}", t.cls);
    }
  }

  /// Convert one raw element to our little-endian layout.
  Result<void> convert_element(const H5Type& t, const std::byte* src, DType dt, const std::vector<Field>& fields, std::byte* dst) {
    if (t.cls == 6) {
      for (std::size_t i = 0; i < t.members.size(); ++i) {
        CVT_TRY(convert_element(*t.members[i].type, src + t.members[i].offset, fields[i].type, {}, dst + fields[i].offset));
      }
      return {};
    }
    if (t.cls == 8) return convert_element(*t.base, src, dt, fields, dst);
    if (t.cls == 9) {
      // Variable-length: length (4) + global heap id (collection address + index).
      util::ByteReader r(ByteView(src, t.size));
      const std::uint32_t n = r.u32le();
      const std::uint64_t collection = offset(r);
      const std::uint32_t index = r.u32le();
      std::string text;
      if (n > 0) {
        CVT_TRY_ASSIGN(const ByteView object, global_heap(collection, index));
        text = util::sanitize_utf8(as_chars(object.first(std::min<std::size_t>(object.size(), std::size_t{n} * (t.base ? t.base->size : 1)))));
      }
      const std::u32string u = util::utf8_to_utf32(text);
      for (std::size_t k = 0; k < u.size() && k < dt.size / 4; ++k) {
        for (int b = 0; b < 4; ++b) dst[k * 4 + static_cast<std::size_t>(b)] = static_cast<std::byte>((u[k] >> (8 * b)) & 0xFF);
      }
      return {};
    }
    std::memcpy(dst, src, std::min<std::size_t>(t.size, dt.size));
    if (t.big_endian && (t.cls == 0 || t.cls == 1)) std::reverse(dst, dst + t.size);
    return {};
  }

  Result<ByteView> global_heap(std::uint64_t collection, std::uint32_t index) {
    CVT_TRY_ASSIGN(const ByteView block, from(collection));
    util::ByteReader r(block);
    if (r.chars(4) != "GCOL") return fail(Status::Decode, "bad HDF5 global heap collection");
    (void)r.u8();
    (void)r.bytes(3);
    const std::uint64_t size = length(r);
    if (size > block.size()) return fail(Status::Decode, "HDF5 global heap is truncated");
    util::ByteReader objects(block.subspan(r.position(), static_cast<std::size_t>(size) - std::min<std::size_t>(r.position(), static_cast<std::size_t>(size))));
    while (objects.remaining() >= 8 + length_size_) {
      const std::uint16_t id = objects.u16le();
      (void)objects.u16le();
      (void)objects.u32le();
      const std::uint64_t object_size = length(objects);
      if (id == 0) break;
      if (object_size > objects.remaining()) return fail(Status::Decode, "HDF5 global heap object is truncated");
      const ByteView object = objects.bytes(static_cast<std::size_t>(object_size));
      (void)objects.bytes(std::min<std::size_t>((8 - object_size % 8) % 8, objects.remaining()));
      if (id == index) return object;
    }
    return fail(Status::Decode, "HDF5 global heap object {} not found", index);
  }

  Result<Array> build_array(const H5Type& t, const std::vector<std::uint64_t>& dims, ByteView raw) {
    CVT_TRY_ASSIGN(const auto mapped, map_type(t));
    Array a;
    a.dtype = mapped.first;
    a.fields = mapped.second;
    a.shape = dims;
    CVT_TRY_ASSIGN(const std::uint64_t n, shape_count(dims));
    CVT_TRY(check_array_bytes(ctx_, n, std::max<std::uint32_t>(a.dtype.size, t.size)));
    if (raw.size() < n * t.size) return fail(Status::Decode, "HDF5 data is shorter than its dataspace");
    a.data.assign(static_cast<std::size_t>(n * a.dtype.size), std::byte{0});
    for (std::uint64_t i = 0; i < n; ++i) {
      CVT_TRY(convert_element(t, raw.data() + i * t.size, a.dtype, a.fields, a.data.data() + i * a.dtype.size));
    }
    if (t.cls == 9) {
      // Variable-length strings: shrink the fixed width to the longest one.
      std::uint32_t longest = 1;
      for (std::uint64_t i = 0; i < n; ++i) {
        std::uint32_t chars = 0;
        while (chars < a.dtype.size / 4 && scalar_double(DType{ScalarKind::UInt, 4}, a.element(i) + chars * 4) != 0) ++chars;
        longest = std::max(longest, chars);
      }
      Array packed = a;
      packed.dtype = DType{ScalarKind::Unicode, longest * 4};
      packed.data.assign(static_cast<std::size_t>(n * packed.dtype.size), std::byte{0});
      for (std::uint64_t i = 0; i < n; ++i) std::memcpy(packed.data.data() + i * packed.dtype.size, a.element(i), packed.dtype.size);
      return packed;
    }
    return a;
  }

  std::optional<std::pair<std::string, Json>> attribute(ByteView data) {
    util::ByteReader r(data);
    const std::uint8_t version = r.u8();
    if (version < 1 || version > 3) return std::nullopt;
    (void)r.u8();
    const std::uint16_t name_size = r.u16le();
    const std::uint16_t type_size = r.u16le();
    const std::uint16_t space_size = r.u16le();
    if (version == 3) (void)r.u8();
    const auto pad = [version](std::size_t n) { return version == 1 ? (n + 7) / 8 * 8 : n; };
    std::string name(r.chars(name_size));
    name = util::sanitize_utf8(name.substr(0, name.find('\0')));
    (void)r.bytes(pad(name_size) - name_size);
    util::ByteReader type_reader = r.sub(type_size);
    (void)r.bytes(pad(type_size) - type_size);
    util::ByteReader space_reader = r.sub(space_size);
    (void)r.bytes(pad(space_size) - space_size);
    if (!r.ok()) return std::nullopt;
    auto type = datatype(type_reader, 0);
    auto dims = dataspace(space_reader);
    if (!type || !dims) return std::nullopt;
    const ByteView raw = r.rest();
    auto array = build_array(**type, *dims, raw);
    if (!array) return std::nullopt;
    if (array->dtype.kind == ScalarKind::Record) {
      auto json = array_to_json(*array, ctx_);
      return json ? std::optional(std::pair{name, (*json)["data"]}) : std::nullopt;
    }
    if (array->count() == 1) return std::pair{name, scalar_json(array->dtype, array->element(0))};
    Json list = Json::array();
    for (std::uint64_t i = 0; i < array->count() && i < 65536; ++i) list.push_back(scalar_json(array->dtype, array->element(i)));
    return std::pair{name, std::move(list)};
  }

  // ------------------------------------------------------------------- datasets

  struct Filter {
    std::uint16_t id = 0;
    std::vector<std::uint32_t> values;
  };

  Result<std::vector<Filter>> filters(ByteView data) {
    util::ByteReader r(data);
    const std::uint8_t version = r.u8();
    const std::uint8_t count = r.u8();
    if (version == 1) (void)r.bytes(6);
    std::vector<Filter> out;
    for (std::uint8_t i = 0; i < count; ++i) {
      Filter f;
      f.id = r.u16le();
      std::uint16_t name_length = 0;
      if (version == 1 || f.id >= 256) name_length = r.u16le();
      (void)r.u16le();  // flags
      const std::uint16_t values = r.u16le();
      if (name_length > 0) (void)r.bytes(version == 1 ? (name_length + 7u) / 8 * 8 : name_length);
      for (std::uint16_t v = 0; v < values; ++v) f.values.push_back(r.u32le());
      if (version == 1 && values % 2 == 1) (void)r.u32le();
      if (!r.ok()) return fail(Status::Decode, "HDF5 filter pipeline is truncated");
      out.push_back(std::move(f));
    }
    return out;
  }

  Result<Bytes> unfilter(ByteView chunk, const std::vector<Filter>& pipeline, std::uint32_t mask, std::uint64_t expected,
                         std::uint32_t element_size) {
    Bytes data(chunk.begin(), chunk.end());
    for (std::size_t i = pipeline.size(); i-- > 0;) {
      if (mask & (1u << i)) continue;
      const Filter& f = pipeline[i];
      if (f.id == 1) {
        CVT_TRY_ASSIGN(data, util::zlib_decompress(data, std::min<std::uint64_t>(expected + 64, ctx_.limits.max_decompressed_bytes)));
        decompressed_ += data.size();
        CVT_TRY(ctx_.limits.check_decompressed(decompressed_));
      } else if (f.id == 2) {
        const std::size_t size = f.values.empty() ? element_size : f.values[0];
        if (size > 1 && data.size() >= size) {
          const std::size_t n = data.size() / size;
          Bytes out(data.size());
          for (std::size_t b = 0; b < size; ++b) {
            for (std::size_t k = 0; k < n; ++k) out[k * size + b] = data[b * n + k];
          }
          std::copy(data.begin() + static_cast<std::ptrdiff_t>(n * size), data.end(), out.begin() + static_cast<std::ptrdiff_t>(n * size));
          data = std::move(out);
        }
      } else if (f.id == 3) {
        if (data.size() < 4) return fail(Status::Decode, "bad Fletcher-32 chunk");
        data.resize(data.size() - 4);
      } else {
        return fail(Status::Decode, "HDF5 filter {} is not supported (only deflate, shuffle and Fletcher-32)", f.id);
      }
    }
    return data;
  }

  /// Copy one chunk (C order, `chunk` dims) into the dataset buffer at `origin`.
  static void place(Bytes& out, const std::vector<std::uint64_t>& dims, ByteView chunk, const std::vector<std::uint64_t>& chunk_dims,
                    const std::vector<std::uint64_t>& origin, std::size_t element) {
    const std::size_t rank = dims.size();
    if (rank == 0) {
      std::memcpy(out.data(), chunk.data(), std::min(element, chunk.size()));
      return;
    }
    std::vector<std::uint64_t> index(rank, 0);
    const std::uint64_t inner = chunk_dims.back();
    while (true) {
      // Row of the innermost dimension.
      std::uint64_t dst = 0, src = 0;
      bool inside = true;
      for (std::size_t d = 0; d < rank; ++d) {
        const std::uint64_t pos = origin[d] + index[d];
        if (pos >= dims[d]) inside = false;
        dst = dst * dims[d] + pos;
        src = src * chunk_dims[d] + index[d];
      }
      if (inside) {
        const std::uint64_t count = std::min<std::uint64_t>(inner, dims.back() - origin.back());
        if ((src + count) * element <= chunk.size()) {
          std::memcpy(out.data() + dst * element, chunk.data() + src * element, static_cast<std::size_t>(count * element));
        }
      }
      std::size_t d = rank - 1;
      while (true) {
        if (d == 0) return;
        --d;
        if (++index[d] < chunk_dims[d]) break;
        index[d] = 0;
      }
    }
  }

  Result<void> chunk_btree(std::uint64_t address, std::size_t rank, const std::function<Result<void>(std::uint64_t, std::uint32_t, std::uint32_t, const std::vector<std::uint64_t>&)>& visit, int depth) {
    if (depth > 32) return fail(Status::Limit, "HDF5 chunk B-tree is too deep");
    CVT_TRY_ASSIGN(const ByteView node, from(address));
    util::ByteReader r(node);
    if (r.chars(4) != "TREE") return fail(Status::Decode, "bad HDF5 chunk B-tree node");
    if (r.u8() != 1) return fail(Status::Decode, "unexpected HDF5 B-tree type");
    const std::uint8_t level = r.u8();
    const std::uint16_t entries = r.u16le();
    (void)offset(r);
    (void)offset(r);
    for (std::uint16_t i = 0; i < entries; ++i) {
      const std::uint32_t size = r.u32le();
      const std::uint32_t mask = r.u32le();
      std::vector<std::uint64_t> origin;
      for (std::size_t d = 0; d <= rank; ++d) origin.push_back(r.u64le());
      const std::uint64_t child = offset(r);
      if (!r.ok()) return fail(Status::Decode, "HDF5 chunk B-tree node is truncated");
      origin.pop_back();
      if (level > 0) {
        CVT_TRY(chunk_btree(child, rank, visit, depth + 1));
      } else {
        CVT_TRY(visit(child, size, mask, origin));
      }
    }
    return {};
  }

  Result<Array> dataset(const std::vector<Message>& messages) {
    std::shared_ptr<H5Type> type;
    std::vector<std::uint64_t> dims;
    std::vector<Filter> pipeline;
    const Message* layout = nullptr;
    for (const Message& m : messages) {
      util::ByteReader r(m.data);
      if (m.type == 0x0003) {
        CVT_TRY_ASSIGN(type, datatype(r, 0));
      } else if (m.type == 0x0001) {
        CVT_TRY_ASSIGN(dims, dataspace(r));
      } else if (m.type == 0x000B) {
        CVT_TRY_ASSIGN(pipeline, filters(m.data));
      } else if (m.type == 0x0008) {
        layout = &m;
      }
    }
    if (!type || layout == nullptr) return fail(Status::Decode, "HDF5 dataset without type or layout");
    CVT_TRY_ASSIGN(const std::uint64_t n, shape_count(dims));
    const std::uint64_t total = n * type->size;
    CVT_TRY(check_array_bytes(ctx_, n, type->size));
    Bytes raw(static_cast<std::size_t>(total), std::byte{0});
    util::ByteReader r(layout->data);
    const std::uint8_t version = r.u8();
    if (version < 3) {
      const std::uint8_t rank = r.u8();
      const std::uint8_t cls = r.u8();
      (void)r.bytes(5);
      if (cls == 1) {
        const std::uint64_t address = offset(r);
        if (address != undefined_address && total > 0) {
          CVT_TRY_ASSIGN(const ByteView data, at(address, total));
          std::memcpy(raw.data(), data.data(), static_cast<std::size_t>(total));
        }
      } else if (cls == 2) {
        const std::uint64_t address = offset(r);
        std::vector<std::uint64_t> chunk;
        for (std::uint8_t d = 0; d < rank; ++d) chunk.push_back(r.u32le());
        CVT_TRY(read_btree_chunks(address, dims, chunk, pipeline, *type, raw));
      } else {
        (void)r.bytes(4 * rank);
        const std::uint32_t size = r.u32le();
        const ByteView data = r.bytes(std::min<std::size_t>(size, r.remaining()));
        std::memcpy(raw.data(), data.data(), std::min<std::size_t>(data.size(), raw.size()));
      }
    } else {
      const std::uint8_t cls = r.u8();
      if (cls == 0) {
        const std::uint16_t size = r.u16le();
        const ByteView data = r.bytes(std::min<std::size_t>(size, r.remaining()));
        std::memcpy(raw.data(), data.data(), std::min<std::size_t>(data.size(), raw.size()));
      } else if (cls == 1) {
        const std::uint64_t address = offset(r);
        (void)length(r);
        if (address != undefined_address && total > 0) {
          CVT_TRY_ASSIGN(const ByteView data, at(address, total));
          std::memcpy(raw.data(), data.data(), static_cast<std::size_t>(total));
        }
      } else if (cls == 2 && version == 3) {
        const std::uint8_t rank = r.u8();
        const std::uint64_t address = offset(r);
        std::vector<std::uint64_t> chunk;
        for (std::uint8_t d = 0; d < rank; ++d) chunk.push_back(r.u32le());
        chunk.pop_back();  // element size
        if (address != undefined_address) CVT_TRY(read_btree_chunks(address, dims, chunk, pipeline, *type, raw));
      } else if (cls == 2) {
        const std::uint8_t flags = r.u8();
        const std::uint8_t rank = r.u8();
        const std::uint8_t width = r.u8();
        std::vector<std::uint64_t> chunk;
        for (std::uint8_t d = 0; d < rank; ++d) {
          std::uint64_t v = 0;
          for (std::uint8_t b = 0; b < width; ++b) v |= std::uint64_t{r.u8()} << (8 * b);
          chunk.push_back(v);
        }
        chunk.pop_back();
        const std::uint8_t index_type = r.u8();
        CVT_TRY(read_v4_chunks(r, flags, index_type, dims, chunk, pipeline, *type, raw));
      } else {
        return fail(Status::Decode, "virtual HDF5 datasets are not supported");
      }
    }
    if (!r.ok()) return fail(Status::Decode, "HDF5 data layout is truncated");
    return build_array(*type, dims, raw);
  }

  Result<void> read_btree_chunks(std::uint64_t address, const std::vector<std::uint64_t>& dims, const std::vector<std::uint64_t>& chunk,
                                 const std::vector<Filter>& pipeline, const H5Type& type, Bytes& raw) {
    if (chunk.size() != dims.size()) return fail(Status::Decode, "HDF5 chunk rank mismatch");
    CVT_TRY_ASSIGN(const std::uint64_t chunk_elements, shape_count(chunk));
    const std::uint64_t chunk_bytes = chunk_elements * type.size;
    if (chunk_bytes > ctx_.limits.max_decompressed_bytes) return fail(Status::Limit, "HDF5 chunk is too large");
    return chunk_btree(address, dims.size(), [&](std::uint64_t child, std::uint32_t size, std::uint32_t mask,
                                                  const std::vector<std::uint64_t>& origin) -> Result<void> {
      CVT_TRY_ASSIGN(const ByteView stored, at(child, size));
      CVT_TRY_ASSIGN(const Bytes data, unfilter(stored, pipeline, mask, chunk_bytes, type.size));
      place(raw, dims, data, chunk, origin, type.size);
      return {};
    }, 0);
  }

  Result<void> read_v4_chunks(util::ByteReader& r, std::uint8_t flags, std::uint8_t index_type, const std::vector<std::uint64_t>& dims,
                              const std::vector<std::uint64_t>& chunk, const std::vector<Filter>& pipeline, const H5Type& type,
                              Bytes& raw) {
    (void)flags;
    if (chunk.size() != dims.size()) return fail(Status::Decode, "HDF5 chunk rank mismatch");
    CVT_TRY_ASSIGN(const std::uint64_t chunk_elements, shape_count(chunk));
    const std::uint64_t chunk_bytes = chunk_elements * type.size;
    if (chunk_bytes > ctx_.limits.max_decompressed_bytes) return fail(Status::Limit, "HDF5 chunk is too large");
    // Chunk grid in C order.
    std::vector<std::uint64_t> grid;
    for (std::size_t d = 0; d < dims.size(); ++d) grid.push_back(chunk[d] == 0 ? 0 : (dims[d] + chunk[d] - 1) / chunk[d]);
    CVT_TRY_ASSIGN(const std::uint64_t chunks, shape_count(grid));
    const auto origin_of = [&](std::uint64_t k) {
      std::vector<std::uint64_t> origin(dims.size());
      for (std::size_t d = dims.size(); d-- > 0;) {
        origin[d] = (k % grid[d]) * chunk[d];
        k /= grid[d];
      }
      return origin;
    };
    const bool filtered = !pipeline.empty();
    if (index_type == 1) {  // single chunk
      std::uint64_t size = chunk_bytes;
      std::uint32_t mask = 0;
      if (filtered) {
        size = length(r);
        mask = r.u32le();
      }
      const std::uint64_t address = offset(r);
      if (address == undefined_address) return {};
      CVT_TRY_ASSIGN(const ByteView stored, at(address, size));
      CVT_TRY_ASSIGN(const Bytes data, unfilter(stored, pipeline, mask, chunk_bytes, type.size));
      place(raw, dims, data, chunk, std::vector<std::uint64_t>(dims.size(), 0), type.size);
      return {};
    }
    if (index_type == 2) {  // implicit: chunks stored back to back
      const std::uint64_t address = offset(r);
      if (address == undefined_address) return {};
      for (std::uint64_t k = 0; k < chunks; ++k) {
        CVT_TRY_ASSIGN(const ByteView stored, at(address + k * chunk_bytes, chunk_bytes));
        place(raw, dims, stored, chunk, origin_of(k), type.size);
      }
      return {};
    }
    if (index_type == 3) {  // fixed array
      (void)r.u8();  // page bits
      const std::uint64_t address = offset(r);
      if (address == undefined_address) return {};
      CVT_TRY_ASSIGN(const ByteView header, from(address));
      util::ByteReader h(header);
      if (h.chars(4) != "FAHD") return fail(Status::Decode, "bad HDF5 fixed array header");
      (void)h.u8();
      (void)h.u8();
      const std::uint8_t entry_size = h.u8();
      const std::uint8_t page_bits = h.u8();
      const std::uint64_t entries = length(h);
      const std::uint64_t block = offset(h);
      if (!h.ok() || entries < chunks) return fail(Status::Decode, "bad HDF5 fixed array header");
      if (page_bits < 32 && entries > (std::uint64_t{1} << page_bits)) return fail(Status::Decode, "paged HDF5 fixed arrays are not supported");
      CVT_TRY_ASSIGN(const ByteView data_block, from(block));
      util::ByteReader b(data_block);
      if (b.chars(4) != "FADB") return fail(Status::Decode, "bad HDF5 fixed array data block");
      (void)b.u8();
      (void)b.u8();
      (void)offset(b);
      for (std::uint64_t k = 0; k < chunks; ++k) {
        const std::size_t start = b.position();
        const std::uint64_t chunk_address = offset(b);
        std::uint64_t size = chunk_bytes;
        std::uint32_t mask = 0;
        if (filtered) {
          size = 0;
          const std::size_t size_bytes = entry_size - offset_size_ - 4u;
          for (std::size_t i = 0; i < size_bytes && i < 8; ++i) size |= std::uint64_t{b.u8()} << (8 * i);
          mask = b.u32le();
        }
        (void)start;
        if (!b.ok()) return fail(Status::Decode, "HDF5 fixed array is truncated");
        if (chunk_address == undefined_address) continue;
        CVT_TRY_ASSIGN(const ByteView stored, at(chunk_address, size));
        CVT_TRY_ASSIGN(const Bytes data, unfilter(stored, pipeline, mask, chunk_bytes, type.size));
        place(raw, dims, data, chunk, origin_of(k), type.size);
      }
      return {};
    }
    return fail(Status::Decode, "HDF5 chunk index type {} (extensible array / v2 B-tree) is not supported", index_type);
  }

  ByteView data_;
  const Context& ctx_;
  std::uint8_t version_ = 0;
  std::uint8_t offset_size_ = 8;
  std::uint8_t length_size_ = 8;
  std::uint64_t base_ = 0;
  std::uint64_t root_ = 0;
  bool creation_order_ = false;
  std::set<std::uint64_t> visited_;
  std::uint64_t objects_ = 0;
  std::uint64_t decompressed_ = 0;
};

}  // namespace

Result<Collection> read_hdf5_collection(ByteView input, const Context& ctx) {
  H5File file(input, ctx);
  CVT_TRY(file.open());
  Collection c;
  CVT_TRY(file.walk(file.root(), "", c, 0));
  if (c.arrays.empty()) ctx.warn("The HDF5 file holds no datasets.");
  return c;
}

namespace {

Result<Collection> read_hdf5(ReadContext& ctx) { return read_hdf5_collection(ctx.input, ctx); }

int sniff_hdf5(ByteView data, std::string_view /*name*/) {
  for (std::size_t at = 0; at <= 4096; at = at == 0 ? 512 : at * 2) {
    if (data.size() >= at + 8 && as_chars(data.subspan(at, 8)) == "\x89HDF\r\n\x1a\n") return at == 0 ? 98 : 90;
  }
  return 0;
}

}  // namespace
}  // namespace cvt::science::arrays

namespace cvt::science {

void register_hdf5(Registry& registry) {
  using namespace arrays;
  add_format(registry, {.id = "hdf5",
                        .name = "HDF5 file",
                        .description = "Hierarchical Data Format 5: groups of datasets with attributes (also MATLAB 7.3).",
                        .extensions = {"h5", "hdf5", "he5", "hdf"},
                        .mime = "application/x-hdf5",
                        .popularity = 30,
                        .signatures = {Signature::at(0, "89 'HDF' 0D 0A 1A 0A")},
                        .sniff = sniff_hdf5});
  add_collection_codecs(registry, "hdf5", read_hdf5, nullptr);
}

}  // namespace cvt::science
