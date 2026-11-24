// Convertible core - table family: Apache Parquet.
//
// Reader: the Thrift compact-protocol footer (decoded generically, then
// interpreted), every row group and column chunk, data pages v1 and v2 and
// dictionary pages; PLAIN, PLAIN_DICTIONARY/RLE_DICTIONARY, RLE and
// bit-packed hybrid levels and booleans, DELTA_BINARY_PACKED,
// DELTA_LENGTH_BYTE_ARRAY, DELTA_BYTE_ARRAY and BYTE_STREAM_SPLIT values;
// uncompressed, Snappy and gzip pages (ZSTD, LZ4 and Brotli are reported as
// unsupported). Logical and converted types become typed cells: strings,
// dates, times, timestamps (all units and INT96), decimals, unsigned and
// small integers, UUIDs, half floats. Nested columns are flattened: struct
// fields become "a.b" columns and repeated values a JSON array in one cell.
//
// Writer: a flat schema of optional columns (BOOLEAN, INT64, DOUBLE,
// INT32 DATE, INT64 TIMESTAMP/TIME in microseconds, UTF-8 BYTE_ARRAY), one
// row group, PLAIN-encoded v1 data pages with RLE definition levels,
// uncompressed, Snappy or gzip.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/table/common.hpp"
#include "families/table/snappy.hpp"
#include "families/table/table.hpp"
#include "util/deflate.hpp"
#include "util/hex.hpp"
#include "util/json.hpp"
#include "util/text.hpp"

namespace cvt::table {

namespace {

// --------------------------------------------------------------------------- Thrift compact protocol

struct Thrift {
  enum Type : std::uint8_t { Stop = 0, True = 1, False = 2, Byte = 3, I16 = 4, I32 = 5, I64 = 6, Double = 7, Binary = 8,
                             List = 9, Set = 10, Map = 11, Struct = 12 };
  Type type = Stop;
  std::int64_t integer = 0;
  double real = 0.0;
  std::string binary;
  std::vector<Thrift> elements;                       // list / set / map (key, value, key, value ...)
  std::vector<std::pair<std::int16_t, Thrift>> fields;  // struct

  [[nodiscard]] const Thrift* field(std::int16_t id) const {
    for (const auto& [key, value] : fields) {
      if (key == id) return &value;
    }
    return nullptr;
  }
  [[nodiscard]] std::int64_t int_field(std::int16_t id, std::int64_t fallback = 0) const {
    const Thrift* f = field(id);
    return f != nullptr ? f->integer : fallback;
  }
  [[nodiscard]] bool has(std::int16_t id) const { return field(id) != nullptr; }
};

class ThriftReader {
 public:
  explicit ThriftReader(ByteView data) : data_(as_u8(data)) {}
  [[nodiscard]] std::size_t position() const noexcept { return pos_; }

  Result<Thrift> read_struct(int depth = 0) {
    if (depth > 32) return fail(Status::Decode, "Parquet metadata is nested too deeply");
    Thrift out;
    out.type = Thrift::Struct;
    std::int16_t last = 0;
    while (true) {
      CVT_TRY_ASSIGN(const std::uint8_t header, byte());
      if (header == 0) break;
      const auto type = static_cast<Thrift::Type>(header & 0x0F);
      std::int16_t id = 0;
      if ((header >> 4) != 0) {
        id = static_cast<std::int16_t>(last + (header >> 4));
      } else {
        CVT_TRY_ASSIGN(const std::int64_t value, zigzag());
        id = static_cast<std::int16_t>(value);
      }
      last = id;
      CVT_TRY_ASSIGN(Thrift value, read_value(type, depth));
      if (++elements_ > 50'000'000) return fail(Status::Limit, "Parquet metadata is too large");
      out.fields.emplace_back(id, std::move(value));
    }
    return out;
  }

 private:
  Result<std::uint8_t> byte() {
    if (pos_ >= data_.size()) return fail(Status::Decode, "truncated Parquet metadata");
    return data_[pos_++];
  }
  Result<std::uint64_t> varint() {
    std::uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 7) {
      CVT_TRY_ASSIGN(const std::uint8_t b, byte());
      value |= static_cast<std::uint64_t>(b & 0x7F) << shift;
      if ((b & 0x80) == 0) return value;
    }
    return fail(Status::Decode, "damaged Parquet varint");
  }
  Result<std::int64_t> zigzag() {
    CVT_TRY_ASSIGN(const std::uint64_t v, varint());
    return static_cast<std::int64_t>(v >> 1) ^ -static_cast<std::int64_t>(v & 1);
  }
  Result<Thrift> read_value(Thrift::Type type, int depth) {
    Thrift value;
    value.type = type;
    switch (type) {
      case Thrift::True: value.integer = 1; break;
      case Thrift::False: value.integer = 0; break;
      case Thrift::Byte: {
        CVT_TRY_ASSIGN(const std::uint8_t b, byte());
        value.integer = static_cast<std::int8_t>(b);
        break;
      }
      case Thrift::I16:
      case Thrift::I32:
      case Thrift::I64: {
        CVT_TRY_ASSIGN(value.integer, zigzag());
        break;
      }
      case Thrift::Double: {
        if (data_.size() - pos_ < 8) return fail(Status::Decode, "truncated Parquet metadata");
        std::uint64_t bits = 0;
        for (int i = 0; i < 8; ++i) bits |= static_cast<std::uint64_t>(data_[pos_ + static_cast<std::size_t>(i)]) << (8 * i);
        pos_ += 8;
        value.real = std::bit_cast<double>(bits);
        break;
      }
      case Thrift::Binary: {
        CVT_TRY_ASSIGN(const std::uint64_t length, varint());
        if (length > data_.size() - pos_) return fail(Status::Decode, "truncated Parquet metadata");
        value.binary.assign(reinterpret_cast<const char*>(data_.data() + pos_), static_cast<std::size_t>(length));
        pos_ += static_cast<std::size_t>(length);
        break;
      }
      case Thrift::List:
      case Thrift::Set: {
        CVT_TRY_ASSIGN(const std::uint8_t header, byte());
        std::uint64_t count = header >> 4;
        if (count == 15) {
          CVT_TRY_ASSIGN(count, varint());
        }
        const auto element = static_cast<Thrift::Type>(header & 0x0F);
        if (count > data_.size() - pos_ + 1) return fail(Status::Decode, "damaged Parquet metadata list");
        for (std::uint64_t i = 0; i < count; ++i) {
          if (element == Thrift::True || element == Thrift::False) {
            CVT_TRY_ASSIGN(const std::uint8_t b, byte());
            Thrift flag;
            flag.type = Thrift::True;
            flag.integer = b == 1 ? 1 : 0;
            value.elements.push_back(std::move(flag));
          } else {
            CVT_TRY_ASSIGN(Thrift item, read_value(element, depth + 1));
            value.elements.push_back(std::move(item));
          }
          if (++elements_ > 50'000'000) return fail(Status::Limit, "Parquet metadata is too large");
        }
        break;
      }
      case Thrift::Map: {
        CVT_TRY_ASSIGN(const std::uint64_t count, varint());
        if (count == 0) break;
        CVT_TRY_ASSIGN(const std::uint8_t types, byte());
        if (count > data_.size() - pos_ + 1) return fail(Status::Decode, "damaged Parquet metadata map");
        for (std::uint64_t i = 0; i < count; ++i) {
          CVT_TRY_ASSIGN(Thrift key, read_value(static_cast<Thrift::Type>(types >> 4), depth + 1));
          CVT_TRY_ASSIGN(Thrift item, read_value(static_cast<Thrift::Type>(types & 0x0F), depth + 1));
          value.elements.push_back(std::move(key));
          value.elements.push_back(std::move(item));
        }
        break;
      }
      case Thrift::Struct: {
        CVT_TRY_ASSIGN(value, read_struct(depth + 1));
        break;
      }
      case Thrift::Stop: return fail(Status::Decode, "unexpected Thrift stop field");
      default: return fail(Status::Decode, "unknown Thrift type {}", static_cast<int>(type));
    }
    return value;
  }

  std::span<const std::uint8_t> data_;
  std::size_t pos_ = 0;
  std::uint64_t elements_ = 0;
};

class ThriftWriter {
 public:
  void field(std::int16_t id, Thrift::Type type) {
    const int delta = id - last_.back();
    if (delta > 0 && delta <= 15) {
      out_.push_back(static_cast<std::byte>((delta << 4) | type));
    } else {
      out_.push_back(static_cast<std::byte>(type));
      zigzag(id);
    }
    last_.back() = id;
  }
  void i32(std::int16_t id, std::int64_t value) {
    field(id, Thrift::I32);
    zigzag(value);
  }
  void i64(std::int16_t id, std::int64_t value) {
    field(id, Thrift::I64);
    zigzag(value);
  }
  void boolean(std::int16_t id, bool value) { field(id, value ? Thrift::True : Thrift::False); }
  void binary(std::int16_t id, std::string_view text) {
    field(id, Thrift::Binary);
    varint(text.size());
    append(out_, text);
  }
  void begin_struct(std::int16_t id) {
    field(id, Thrift::Struct);
    last_.push_back(0);
  }
  void begin_list_element() { last_.push_back(0); }
  void end_struct() {
    out_.push_back(std::byte{0});
    last_.pop_back();
  }
  void list(std::int16_t id, Thrift::Type element, std::size_t count) {
    field(id, Thrift::List);
    if (count < 15) {
      out_.push_back(static_cast<std::byte>((count << 4) | element));
    } else {
      out_.push_back(static_cast<std::byte>(0xF0 | element));
      varint(count);
    }
  }
  void list_i32(std::int64_t value) { zigzag(value); }
  void list_binary(std::string_view text) {
    varint(text.size());
    append(out_, text);
  }
  void varint(std::uint64_t value) {
    do {
      out_.push_back(static_cast<std::byte>((value & 0x7F) | (value >= 0x80 ? 0x80 : 0)));
      value >>= 7;
    } while (value != 0);
  }
  void zigzag(std::int64_t value) { varint((static_cast<std::uint64_t>(value) << 1) ^ static_cast<std::uint64_t>(value >> 63)); }
  [[nodiscard]] Bytes take() { return std::exchange(out_, {}); }

 private:
  Bytes out_;
  std::vector<std::int16_t> last_{0};
};

// --------------------------------------------------------------------------- schema

enum PhysicalType { Boolean = 0, Int32 = 1, Int64 = 2, Int96 = 3, Float = 4, Double = 5, ByteArray = 6, FixedLenByteArray = 7 };

struct LeafColumn {
  std::string name;
  std::vector<std::string> path;
  int physical = ByteArray;
  std::int64_t type_length = 0;
  int max_definition = 0;
  int max_repetition = 0;
  // Interpretation.
  std::int64_t converted = -1;
  enum Logical { None, String, Date, Time, Timestamp, Decimal, Integer, Uuid, Float16, Json } logical = None;
  int unit = 1;  // 0 millis, 1 micros, 2 nanos
  std::int64_t scale = 0;
  bool is_signed = true;
};

Result<std::vector<LeafColumn>> parse_schema(const Thrift& metadata) {
  const Thrift* schema = metadata.field(2);
  if (schema == nullptr || schema->elements.empty()) return fail(Status::Decode, "the Parquet file has no schema");
  std::vector<LeafColumn> leaves;
  struct Level {
    std::int64_t remaining;
    int definition;
    int repetition;
    std::vector<std::string> path;
  };
  std::vector<Level> stack;
  const Thrift& root = schema->elements[0];
  stack.push_back({root.int_field(5), 0, 0, {}});
  for (std::size_t i = 1; i < schema->elements.size(); ++i) {
    while (!stack.empty() && stack.back().remaining == 0) stack.pop_back();
    if (stack.empty()) return fail(Status::Decode, "damaged Parquet schema");
    --stack.back().remaining;
    const Thrift& element = schema->elements[i];
    const std::int64_t repetition = element.int_field(3, 0);
    Level level = stack.back();
    level.path.push_back(element.field(4) != nullptr ? element.field(4)->binary : std::string());
    if (repetition == 1) ++level.definition;
    if (repetition == 2) {
      ++level.definition;
      ++level.repetition;
    }
    const std::int64_t children = element.int_field(5, 0);
    if (children > 0) {
      if (stack.size() > 64) return fail(Status::Decode, "the Parquet schema is nested too deeply");
      level.remaining = children;
      stack.push_back(std::move(level));
      continue;
    }
    LeafColumn leaf;
    leaf.path = level.path;
    leaf.max_definition = level.definition;
    leaf.max_repetition = level.repetition;
    leaf.physical = static_cast<int>(element.int_field(1, ByteArray));
    leaf.type_length = element.int_field(2, 0);
    leaf.converted = element.has(6) ? element.int_field(6) : -1;
    leaf.scale = element.int_field(7, 0);
    // Readable name: drop the LIST/MAP wrapper levels.
    std::vector<std::string> parts;
    for (const std::string& part : leaf.path) {
      if (part == "list" || part == "element" || part == "item" || part == "array" || part == "key_value" || part == "bag") continue;
      parts.push_back(part);
    }
    leaf.name = util::join(parts.empty() ? leaf.path : parts, ".");
    if (const Thrift* logical = element.field(10); logical != nullptr && !logical->fields.empty()) {
      const auto& [kind, detail] = logical->fields.front();
      switch (kind) {
        case 1: case 4: leaf.logical = LeafColumn::String; break;
        case 5: leaf.logical = LeafColumn::Decimal; leaf.scale = detail.int_field(1, leaf.scale); break;
        case 6: leaf.logical = LeafColumn::Date; break;
        case 7:
        case 8: {
          leaf.logical = kind == 7 ? LeafColumn::Time : LeafColumn::Timestamp;
          if (const Thrift* unit = detail.field(2); unit != nullptr && !unit->fields.empty()) leaf.unit = unit->fields.front().first - 1;
          break;
        }
        case 10:
          leaf.logical = LeafColumn::Integer;
          leaf.is_signed = detail.int_field(2, 1) != 0;
          break;
        case 12: leaf.logical = LeafColumn::Json; break;
        case 14: leaf.logical = LeafColumn::Uuid; break;
        case 15: leaf.logical = LeafColumn::Float16; break;
        default: break;
      }
    }
    if (leaf.logical == LeafColumn::None) {
      switch (leaf.converted) {
        case 0: case 4: leaf.logical = LeafColumn::String; break;
        case 19: leaf.logical = LeafColumn::Json; break;
        case 5: leaf.logical = LeafColumn::Decimal; break;
        case 6: leaf.logical = LeafColumn::Date; break;
        case 7: leaf.logical = LeafColumn::Time; leaf.unit = 0; break;
        case 8: leaf.logical = LeafColumn::Time; leaf.unit = 1; break;
        case 9: leaf.logical = LeafColumn::Timestamp; leaf.unit = 0; break;
        case 10: leaf.logical = LeafColumn::Timestamp; leaf.unit = 1; break;
        case 11: case 12: case 13: case 14: leaf.logical = LeafColumn::Integer; leaf.is_signed = false; break;
        default: break;
      }
    }
    leaves.push_back(std::move(leaf));
    if (leaves.size() > 100'000) return fail(Status::Limit, "the Parquet file has too many columns");
  }
  return leaves;
}

// --------------------------------------------------------------------------- page decoding

// RLE / bit-packing hybrid.
Result<void> decode_hybrid(std::span<const std::uint8_t> data, int bit_width, std::size_t count, std::vector<std::uint32_t>& out) {
  out.clear();
  if (bit_width == 0) {
    out.assign(count, 0);
    return {};
  }
  if (bit_width > 32) return fail(Status::Decode, "invalid Parquet bit width {}", bit_width);
  std::size_t pos = 0;
  const std::size_t byte_width = static_cast<std::size_t>((bit_width + 7) / 8);
  while (out.size() < count) {
    std::uint64_t header = 0;
    for (int shift = 0;; shift += 7) {
      if (pos >= data.size() || shift > 63) return fail(Status::Decode, "truncated Parquet levels");
      const std::uint8_t b = data[pos++];
      header |= static_cast<std::uint64_t>(b & 0x7F) << shift;
      if ((b & 0x80) == 0) break;
    }
    if ((header & 1) != 0) {
      const std::uint64_t groups = header >> 1;
      const std::uint64_t values = groups * 8;
      const std::uint64_t bytes = groups * static_cast<std::uint64_t>(bit_width);
      if (bytes > data.size() - pos) return fail(Status::Decode, "truncated Parquet bit-packed run");
      std::uint64_t buffer = 0;
      int bits = 0;
      std::size_t at = pos;
      for (std::uint64_t v = 0; v < values && out.size() < count; ++v) {
        while (bits < bit_width) {
          buffer |= static_cast<std::uint64_t>(data[at++]) << bits;
          bits += 8;
        }
        out.push_back(static_cast<std::uint32_t>(buffer & ((1ull << bit_width) - 1)));
        buffer >>= bit_width;
        bits -= bit_width;
      }
      pos += static_cast<std::size_t>(bytes);
    } else {
      const std::uint64_t run = header >> 1;
      if (byte_width > data.size() - pos) return fail(Status::Decode, "truncated Parquet RLE run");
      std::uint32_t value = 0;
      for (std::size_t i = 0; i < byte_width; ++i) value |= static_cast<std::uint32_t>(data[pos + i]) << (8 * i);
      pos += byte_width;
      if (run == 0) return fail(Status::Decode, "empty Parquet RLE run");
      const std::uint64_t take = std::min<std::uint64_t>(run, count - out.size());
      out.insert(out.end(), static_cast<std::size_t>(take), value);
    }
  }
  return {};
}

int bit_width_for(int max_level) {
  int width = 0;
  while ((1 << width) <= max_level) ++width;
  return width;
}

// DELTA_BINARY_PACKED; returns the bytes consumed.
Result<std::size_t> decode_delta(std::span<const std::uint8_t> data, std::size_t expected, std::vector<std::int64_t>& out) {
  std::size_t pos = 0;
  const auto varint = [&]() -> Result<std::uint64_t> {
    std::uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 7) {
      if (pos >= data.size()) return fail(Status::Decode, "truncated Parquet delta encoding");
      const std::uint8_t b = data[pos++];
      value |= static_cast<std::uint64_t>(b & 0x7F) << shift;
      if ((b & 0x80) == 0) return value;
    }
    return fail(Status::Decode, "damaged Parquet varint");
  };
  const auto zigzag = [&]() -> Result<std::int64_t> {
    CVT_TRY_ASSIGN(const std::uint64_t v, varint());
    return static_cast<std::int64_t>(v >> 1) ^ -static_cast<std::int64_t>(v & 1);
  };
  CVT_TRY_ASSIGN(const std::uint64_t block_size, varint());
  CVT_TRY_ASSIGN(const std::uint64_t miniblocks, varint());
  CVT_TRY_ASSIGN(const std::uint64_t total, varint());
  CVT_TRY_ASSIGN(std::int64_t value, zigzag());
  if (block_size == 0 || miniblocks == 0 || block_size % miniblocks != 0 || block_size > 65536 || miniblocks > 1024) {
    return fail(Status::Decode, "invalid Parquet delta block header");
  }
  if (total > expected + 1 || total > 1'000'000'000) return fail(Status::Decode, "Parquet delta encoding holds too many values");
  const std::uint64_t per_miniblock = block_size / miniblocks;
  out.clear();
  if (total == 0) return pos;
  out.push_back(value);
  while (out.size() < total) {
    CVT_TRY_ASSIGN(const std::int64_t min_delta, zigzag());
    if (miniblocks > data.size() - pos) return fail(Status::Decode, "truncated Parquet delta block");
    const std::span<const std::uint8_t> widths = data.subspan(pos, static_cast<std::size_t>(miniblocks));
    pos += static_cast<std::size_t>(miniblocks);
    for (std::uint64_t m = 0; m < miniblocks && out.size() < total; ++m) {
      const int width = widths[static_cast<std::size_t>(m)];
      if (width > 64) return fail(Status::Decode, "invalid Parquet delta bit width");
      const std::uint64_t bytes = per_miniblock * static_cast<std::uint64_t>(width) / 8;
      if (bytes > data.size() - pos) return fail(Status::Decode, "truncated Parquet delta miniblock");
      std::size_t at = pos;
      std::uint64_t buffer = 0;
      int bits = 0;
      for (std::uint64_t k = 0; k < per_miniblock; ++k) {
        std::uint64_t delta = 0;
        if (width > 0) {
          // Read `width` bits LSB-first, possibly spanning more than 64 buffered bits.
          int got = 0;
          while (got < width) {
            if (bits == 0) {
              buffer = at < pos + bytes ? data[at] : 0;
              ++at;
              bits = 8;
            }
            const int take = std::min(width - got, bits);
            delta |= (buffer & ((1ull << take) - 1)) << got;
            buffer >>= take;
            bits -= take;
            got += take;
          }
        }
        if (out.size() < total) {
          value = static_cast<std::int64_t>(static_cast<std::uint64_t>(value) + static_cast<std::uint64_t>(min_delta) + delta);
          out.push_back(value);
        }
      }
      pos += static_cast<std::size_t>(bytes);
    }
  }
  return pos;
}

struct PageValues {
  // Raw values in physical form.
  std::vector<std::int64_t> integers;  // BOOLEAN, INT32, INT64
  std::vector<double> reals;           // FLOAT, DOUBLE
  std::vector<std::string> bytes;      // BYTE_ARRAY, FLBA, INT96
};

Result<void> decode_plain(std::span<const std::uint8_t> data, const LeafColumn& column, std::size_t count, PageValues& out) {
  util::ByteReader r(as_bytes(data));
  switch (column.physical) {
    case Boolean:
      for (std::size_t i = 0; i < count; ++i) {
        if (i / 8 >= data.size()) return fail(Status::Decode, "truncated Parquet booleans");
        out.integers.push_back((data[i / 8] >> (i % 8)) & 1);
      }
      return {};
    case Int32:
      for (std::size_t i = 0; i < count; ++i) out.integers.push_back(r.i32le());
      break;
    case Int64:
      for (std::size_t i = 0; i < count; ++i) out.integers.push_back(r.i64le());
      break;
    case Float:
      for (std::size_t i = 0; i < count; ++i) out.reals.push_back(r.f32le());
      break;
    case Double:
      for (std::size_t i = 0; i < count; ++i) out.reals.push_back(r.f64le());
      break;
    case Int96:
      for (std::size_t i = 0; i < count; ++i) out.bytes.emplace_back(r.chars(12));
      break;
    case ByteArray:
      for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t length = r.u32le();
        out.bytes.emplace_back(r.chars(length));
        if (!r.ok()) break;
      }
      break;
    case FixedLenByteArray:
      if (column.type_length <= 0 || column.type_length > 1 << 20) return fail(Status::Decode, "invalid Parquet fixed length");
      for (std::size_t i = 0; i < count; ++i) out.bytes.emplace_back(r.chars(static_cast<std::size_t>(column.type_length)));
      break;
    default: return fail(Status::Decode, "unknown Parquet physical type {}", column.physical);
  }
  if (!r.ok()) return fail(Status::Decode, "truncated Parquet page values");
  return {};
}

Result<void> decode_values(std::span<const std::uint8_t> data, int encoding, const LeafColumn& column, std::size_t count,
                           const PageValues* dictionary, PageValues& out) {
  switch (encoding) {
    case 0: return decode_plain(data, column, count, out);
    case 2:
    case 8: {
      if (dictionary == nullptr) return fail(Status::Decode, "Parquet dictionary page is missing");
      if (data.empty()) return fail(Status::Decode, "truncated Parquet dictionary indices");
      std::vector<std::uint32_t> indices;
      CVT_TRY(decode_hybrid(data.subspan(1), data[0], count, indices));
      for (const std::uint32_t index : indices) {
        if (!dictionary->integers.empty()) {
          if (index >= dictionary->integers.size()) return fail(Status::Decode, "Parquet dictionary index out of range");
          out.integers.push_back(dictionary->integers[index]);
        } else if (!dictionary->reals.empty()) {
          if (index >= dictionary->reals.size()) return fail(Status::Decode, "Parquet dictionary index out of range");
          out.reals.push_back(dictionary->reals[index]);
        } else {
          if (index >= dictionary->bytes.size()) return fail(Status::Decode, "Parquet dictionary index out of range");
          out.bytes.push_back(dictionary->bytes[index]);
        }
      }
      return {};
    }
    case 3: {  // RLE booleans: 4-byte length, then the hybrid with bit width 1
      if (column.physical != Boolean || data.size() < 4) return fail(Status::Decode, "unsupported Parquet RLE values");
      std::vector<std::uint32_t> bits;
      CVT_TRY(decode_hybrid(data.subspan(4), 1, count, bits));
      for (const std::uint32_t b : bits) out.integers.push_back(b);
      return {};
    }
    case 5: {
      std::vector<std::int64_t> values;
      CVT_TRY(decode_delta(data, count, values));
      for (const std::int64_t v : values) out.integers.push_back(column.physical == Int32 ? static_cast<std::int32_t>(v) : v);
      return {};
    }
    case 6:
    case 7: {
      std::vector<std::int64_t> prefixes;
      std::size_t pos = 0;
      if (encoding == 7) {
        CVT_TRY_ASSIGN(pos, decode_delta(data, count, prefixes));
      }
      std::vector<std::int64_t> lengths;
      CVT_TRY_ASSIGN(const std::size_t used, decode_delta(data.subspan(pos), count, lengths));
      pos += used;
      std::string previous;
      for (std::size_t i = 0; i < lengths.size(); ++i) {
        const std::int64_t length = lengths[i];
        if (length < 0 || static_cast<std::uint64_t>(length) > data.size() - pos) return fail(Status::Decode, "damaged Parquet byte array");
        std::string value(reinterpret_cast<const char*>(data.data() + pos), static_cast<std::size_t>(length));
        pos += static_cast<std::size_t>(length);
        if (encoding == 7) {
          const std::int64_t prefix = i < prefixes.size() ? prefixes[i] : 0;
          if (prefix < 0 || static_cast<std::size_t>(prefix) > previous.size()) return fail(Status::Decode, "damaged Parquet prefix");
          value = previous.substr(0, static_cast<std::size_t>(prefix)) + value;
          previous = value;
        }
        out.bytes.push_back(std::move(value));
      }
      return {};
    }
    case 9: {  // BYTE_STREAM_SPLIT
      std::size_t width = column.physical == Float || column.physical == Int32 ? 4 : column.physical == Double || column.physical == Int64 ? 8 : static_cast<std::size_t>(column.type_length);
      if (width == 0 || width > 64 || data.size() < width * count) return fail(Status::Decode, "truncated Parquet split stream");
      for (std::size_t i = 0; i < count; ++i) {
        std::uint64_t bits = 0;
        std::string raw(width, '\0');
        for (std::size_t b = 0; b < width; ++b) {
          raw[b] = static_cast<char>(data[b * count + i]);
          if (b < 8) bits |= static_cast<std::uint64_t>(data[b * count + i]) << (8 * b);
        }
        if (column.physical == Float) {
          out.reals.push_back(std::bit_cast<float>(static_cast<std::uint32_t>(bits)));
        } else if (column.physical == Double) {
          out.reals.push_back(std::bit_cast<double>(bits));
        } else if (column.physical == Int32) {
          out.integers.push_back(static_cast<std::int32_t>(bits));
        } else if (column.physical == Int64) {
          out.integers.push_back(static_cast<std::int64_t>(bits));
        } else {
          out.bytes.push_back(std::move(raw));
        }
      }
      return {};
    }
    default: return fail(Status::Decode, "unsupported Parquet encoding {}", encoding);
  }
}

Result<Bytes> decompress_page(std::span<const std::uint8_t> data, std::int64_t codec, std::uint64_t size, const Limits& limits) {
  switch (codec) {
    case 0: return to_bytes(data);
    case 1: return snappy_decompress(as_bytes(data), std::min<std::uint64_t>(size, limits.max_decompressed_bytes));
    case 2: {
      auto result = util::gzip_decompress(as_bytes(data), limits.max_decompressed_bytes);
      if (!result) result = util::zlib_decompress(as_bytes(data), limits.max_decompressed_bytes);
      return result;
    }
    case 3: return fail(Status::Decode, "Parquet pages compressed with LZO are not supported");
    case 4: return fail(Status::Decode, "Parquet pages compressed with Brotli are not supported");
    case 5:
    case 7: return fail(Status::Decode, "Parquet pages compressed with LZ4 are not supported");
    case 6: return fail(Status::Decode, "Parquet pages compressed with Zstandard are not supported");
    default: return fail(Status::Decode, "unknown Parquet compression codec {}", codec);
  }
}

// --------------------------------------------------------------------------- values to cells

std::string decimal_text(std::string digits, std::int64_t scale, bool negative) {
  if (scale > 0) {
    if (digits.size() <= static_cast<std::size_t>(scale)) digits.insert(0, static_cast<std::size_t>(scale) - digits.size() + 1, '0');
    digits.insert(digits.size() - static_cast<std::size_t>(scale), ".");
  } else if (scale < 0) {
    digits.append(static_cast<std::size_t>(-scale), '0');
  }
  return negative ? "-" + digits : digits;
}

// Big-endian two's complement bytes -> decimal digits.
std::string big_integer_digits(std::string_view bytes, bool& negative) {
  std::vector<std::uint8_t> magnitude(bytes.begin(), bytes.end());
  negative = !magnitude.empty() && (magnitude[0] & 0x80) != 0;
  if (negative) {
    for (std::uint8_t& b : magnitude) b = static_cast<std::uint8_t>(~b);
    for (std::size_t i = magnitude.size(); i-- > 0;) {
      if (++magnitude[i] != 0) break;
    }
  }
  std::string digits;
  while (!magnitude.empty()) {
    std::uint32_t remainder = 0;
    std::vector<std::uint8_t> quotient;
    for (const std::uint8_t b : magnitude) {
      const std::uint32_t current = remainder * 256 + b;
      const std::uint32_t q = current / 10;
      remainder = current % 10;
      if (!quotient.empty() || q != 0) quotient.push_back(static_cast<std::uint8_t>(q));
    }
    digits.push_back(static_cast<char>('0' + remainder));
    magnitude = std::move(quotient);
  }
  if (digits.empty()) digits = "0";
  std::ranges::reverse(digits);
  return digits;
}

ir::Cell decimal_cell(const std::string& text) {
  const auto value = util::parse_double(text);
  return ir::Cell::numeric(value.value_or(0.0), text);
}

double unit_seconds(int unit) { return unit == 0 ? 1e-3 : unit == 1 ? 1e-6 : 1e-9; }

ir::Cell integer_value_cell(std::int64_t v, const LeafColumn& column) {
  switch (column.logical) {
    case LeafColumn::Date: return datetime_cell(static_cast<double>(v) * 86400.0, DateStyle::Date);
    case LeafColumn::Time: return datetime_cell(excel_1900_epoch_unix + static_cast<double>(v) * unit_seconds(column.physical == Int32 ? 0 : column.unit), DateStyle::Time);
    case LeafColumn::Timestamp: {
      const double seconds = static_cast<double>(v) * unit_seconds(column.unit);
      return datetime_cell(seconds, DateStyle::DateTime);
    }
    case LeafColumn::Decimal: {
      const bool negative = v < 0;
      const std::uint64_t magnitude = negative ? 0 - static_cast<std::uint64_t>(v) : static_cast<std::uint64_t>(v);
      return decimal_cell(decimal_text(std::to_string(magnitude), column.scale, negative));
    }
    case LeafColumn::Integer:
      if (!column.is_signed) {
        const std::uint64_t u = column.physical == Int32 ? static_cast<std::uint32_t>(v) : static_cast<std::uint64_t>(v);
        return ir::Cell::numeric(static_cast<double>(u), std::to_string(u));
      }
      return integer_cell(v);
    default: return integer_cell(v);
  }
}

ir::Cell bytes_value_cell(const std::string& v, const LeafColumn& column) {
  if (column.physical == Int96 && v.size() == 12) {
    std::uint64_t nanos = 0;
    for (int i = 7; i >= 0; --i) nanos = (nanos << 8) | static_cast<std::uint8_t>(v[static_cast<std::size_t>(i)]);
    std::uint32_t day = 0;
    for (int i = 11; i >= 8; --i) day = (day << 8) | static_cast<std::uint8_t>(v[static_cast<std::size_t>(i)]);
    const double seconds = (static_cast<double>(day) - 2440588.0) * 86400.0 + static_cast<double>(nanos) * 1e-9;
    return datetime_cell(seconds, DateStyle::DateTime);
  }
  switch (column.logical) {
    case LeafColumn::String:
    case LeafColumn::Json: return ir::Cell::string(util::sanitize_utf8(v));
    case LeafColumn::Decimal: {
      bool negative = false;
      const std::string digits = big_integer_digits(v, negative);
      return decimal_cell(decimal_text(digits, column.scale, negative));
    }
    case LeafColumn::Uuid:
      if (v.size() == 16) {
        const std::string hex = util::hex_encode(as_bytes(v));
        return ir::Cell::string(hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" + hex.substr(20));
      }
      break;
    case LeafColumn::Float16:
      if (v.size() == 2) {
        const auto bits = static_cast<std::uint16_t>(static_cast<std::uint8_t>(v[0]) | (static_cast<std::uint8_t>(v[1]) << 8));
        const int exponent = (bits >> 10) & 0x1F;
        const int mantissa = bits & 0x3FF;
        double value = exponent == 0 ? std::ldexp(mantissa, -24)
                       : exponent == 31 ? (mantissa == 0 ? INFINITY : NAN)
                                        : std::ldexp(mantissa + 1024, exponent - 25);
        if ((bits & 0x8000) != 0) value = -value;
        return number_cell(value);
      }
      break;
    default: break;
  }
  if (column.physical == ByteArray && util::is_valid_utf8(v)) return ir::Cell::string(v);
  return ir::Cell::string(util::hex_encode(as_bytes(v)));
}

Json cell_to_json(const ir::Cell& cell) {
  switch (cell.type) {
    case ir::CellType::Empty: return nullptr;
    case ir::CellType::String: return Json(cell.text);
    case ir::CellType::Bool: return Json(cell.number != 0);
    case ir::CellType::Number:
      if (const auto exact = util::parse_int(cell.text)) return Json(*exact);
      return Json(cell.number);
    case ir::CellType::DateTime: return Json(cell.text);
  }
  return nullptr;
}

// --------------------------------------------------------------------------- reading

Result<AnyIR> read_parquet(ReadContext& ctx) {
  const ByteView input = ctx.input;
  if (input.size() < 12 || as_chars(input).substr(0, 4) != "PAR1" || as_chars(input).substr(input.size() - 4) != "PAR1") {
    return fail(Status::Decode, "not a Parquet file");
  }
  util::ByteReader tail(input.subspan(input.size() - 8, 4));
  const std::uint32_t footer_length = tail.u32le();
  if (footer_length > input.size() - 12) return fail(Status::Decode, "the Parquet footer length is invalid");
  const ByteView footer = input.subspan(input.size() - 8 - footer_length, footer_length);
  ThriftReader thrift(footer);
  CVT_TRY_ASSIGN(const Thrift metadata, thrift.read_struct());
  CVT_TRY_ASSIGN(std::vector<LeafColumn> columns, parse_schema(metadata));
  const std::int64_t total_rows = metadata.int_field(3, 0);
  if (total_rows < 0) return fail(Status::Decode, "negative Parquet row count");
  // Even run-length encoded nulls cost some bytes: a row count far beyond the
  // file size is corrupt (and would make us allocate the grid up front).
  if (static_cast<std::uint64_t>(total_rows) > std::uint64_t{input.size()} * 1024 + 65536) {
    return fail(Status::Decode, "the Parquet file declares {} rows, more than it can hold", total_rows);
  }
  CellBudget budget(ctx.limits);
  CVT_TRY(budget.take(static_cast<std::uint64_t>(total_rows) * std::max<std::size_t>(1, columns.size())));

  ir::Sheet sheet;
  {
    std::string stem(ctx.filename);
    if (const std::size_t slash = stem.find_last_of("/\\"); slash != std::string::npos) stem.erase(0, slash + 1);
    if (const std::size_t dot = stem.rfind('.'); dot != std::string::npos && dot > 0) stem.resize(dot);
    sheet.name = stem;
  }
  sheet.has_header = true;
  std::vector<ir::Cell> header;
  for (const LeafColumn& column : columns) header.push_back(ir::Cell::string(column.name));
  sheet.rows.push_back(std::move(header));
  sheet.rows.resize(static_cast<std::size_t>(total_rows) + 1, std::vector<ir::Cell>(columns.size()));

  const Thrift* row_groups = metadata.field(4);
  std::uint64_t decompressed = 0;
  std::size_t row_base = 0;
  const std::size_t group_count = row_groups != nullptr ? row_groups->elements.size() : 0;
  for (std::size_t g = 0; g < group_count; ++g) {
    const Thrift& group = row_groups->elements[g];
    const std::int64_t group_rows = group.int_field(3, 0);
    if (group_rows < 0 || static_cast<std::uint64_t>(group_rows) > static_cast<std::uint64_t>(total_rows) - row_base) {
      return fail(Status::Decode, "Parquet row groups hold more rows than the file declares");
    }
    const Thrift* chunks = group.field(1);
    if (chunks == nullptr || chunks->elements.size() != columns.size()) return fail(Status::Decode, "Parquet row group does not match the schema");
    for (std::size_t c = 0; c < columns.size(); ++c) {
      CVT_TRY(ctx.checkpoint((static_cast<double>(g) + static_cast<double>(c) / static_cast<double>(columns.size())) / static_cast<double>(group_count)));
      const LeafColumn& column = columns[c];
      const Thrift* meta = chunks->elements[c].field(3);
      if (meta == nullptr) return fail(Status::Decode, "Parquet column chunk has no metadata (external files are not supported)");
      const std::int64_t codec = meta->int_field(4, 0);
      const std::int64_t num_values = meta->int_field(5, 0);
      const std::int64_t data_offset = meta->int_field(9, 0);
      const std::int64_t dictionary_offset = meta->int_field(11, 0);
      const std::int64_t compressed_size = meta->int_field(7, 0);
      std::int64_t start = data_offset;
      if (meta->has(11) && dictionary_offset > 0 && dictionary_offset < data_offset) start = dictionary_offset;
      if (start < 4 || compressed_size < 0 || static_cast<std::uint64_t>(start) > input.size() ||
          static_cast<std::uint64_t>(compressed_size) > input.size() - static_cast<std::uint64_t>(start)) {
        return fail(Status::Decode, "Parquet column chunk lies outside the file");
      }
      const ByteView chunk = input.subspan(static_cast<std::size_t>(start), static_cast<std::size_t>(compressed_size));
      std::optional<PageValues> dictionary;
      std::size_t pos = 0;
      std::int64_t values_read = 0;
      std::size_t row = row_base;
      // Repeated columns collect a list per row.
      Json list;
      bool list_open = false;
      while (values_read < num_values && pos < chunk.size()) {
        ThriftReader page_reader(chunk.subspan(pos));
        CVT_TRY_ASSIGN(const Thrift page_header, page_reader.read_struct());
        pos += page_reader.position();
        const std::int64_t type = page_header.int_field(1, -1);
        const std::int64_t uncompressed_size = page_header.int_field(2, 0);
        const std::int64_t page_size = page_header.int_field(3, 0);
        if (page_size < 0 || static_cast<std::uint64_t>(page_size) > chunk.size() - pos || uncompressed_size < 0) {
          return fail(Status::Decode, "Parquet page runs past its column chunk");
        }
        const auto raw = as_u8(chunk.subspan(pos, static_cast<std::size_t>(page_size)));
        pos += static_cast<std::size_t>(page_size);
        decompressed += static_cast<std::uint64_t>(uncompressed_size);
        CVT_TRY(ctx.limits.check_decompressed(decompressed));
        if (type == 2) {
          const Thrift* dict_header = page_header.field(7);
          const std::int64_t count = dict_header != nullptr ? dict_header->int_field(1, 0) : 0;
          if (count < 0 || count > uncompressed_size + 1) return fail(Status::Decode, "damaged Parquet dictionary page");
          CVT_TRY_ASSIGN(const Bytes body, decompress_page(raw, codec, static_cast<std::uint64_t>(uncompressed_size), ctx.limits));
          PageValues values;
          CVT_TRY(decode_plain(as_u8(body), column, static_cast<std::size_t>(count), values));
          dictionary = std::move(values);
          continue;
        }
        if (type != 0 && type != 3) continue;  // index pages
        std::int64_t count = 0;
        int encoding = 0;
        std::vector<std::uint32_t> definitions;
        std::vector<std::uint32_t> repetitions;
        Bytes body;
        std::span<const std::uint8_t> values_data;
        if (type == 0) {
          const Thrift* h = page_header.field(5);
          if (h == nullptr) return fail(Status::Decode, "damaged Parquet data page header");
          count = h->int_field(1, 0);
          encoding = static_cast<int>(h->int_field(2, 0));
          if (count < 0 || count > num_values - values_read) return fail(Status::Decode, "Parquet page holds too many values");
          CVT_TRY_ASSIGN(body, decompress_page(raw, codec, static_cast<std::uint64_t>(uncompressed_size), ctx.limits));
          std::span<const std::uint8_t> rest = as_u8(body);
          const auto levels = [&](int max_level, std::vector<std::uint32_t>& out) -> Result<void> {
            if (max_level == 0) {
              out.assign(static_cast<std::size_t>(count), 0);
              return {};
            }
            if (rest.size() < 4) return fail(Status::Decode, "truncated Parquet levels");
            const std::uint32_t length = rest[0] | (rest[1] << 8) | (rest[2] << 16) | (static_cast<std::uint32_t>(rest[3]) << 24);
            if (length > rest.size() - 4) return fail(Status::Decode, "truncated Parquet levels");
            CVT_TRY(decode_hybrid(rest.subspan(4, length), bit_width_for(max_level), static_cast<std::size_t>(count), out));
            rest = rest.subspan(4 + length);
            return {};
          };
          CVT_TRY(levels(column.max_repetition, repetitions));
          CVT_TRY(levels(column.max_definition, definitions));
          values_data = rest;
        } else {
          const Thrift* h = page_header.field(8);
          if (h == nullptr) return fail(Status::Decode, "damaged Parquet data page header");
          count = h->int_field(1, 0);
          encoding = static_cast<int>(h->int_field(4, 0));
          const std::int64_t def_length = h->int_field(5, 0);
          const std::int64_t rep_length = h->int_field(6, 0);
          const bool compressed = h->int_field(7, 1) != 0;
          if (count < 0 || count > num_values - values_read || def_length < 0 || rep_length < 0 ||
              static_cast<std::uint64_t>(def_length + rep_length) > raw.size()) {
            return fail(Status::Decode, "damaged Parquet data page header");
          }
          if (column.max_repetition > 0) {
            CVT_TRY(decode_hybrid(raw.subspan(0, static_cast<std::size_t>(rep_length)), bit_width_for(column.max_repetition), static_cast<std::size_t>(count), repetitions));
          } else {
            repetitions.assign(static_cast<std::size_t>(count), 0);
          }
          if (column.max_definition > 0) {
            CVT_TRY(decode_hybrid(raw.subspan(static_cast<std::size_t>(rep_length), static_cast<std::size_t>(def_length)), bit_width_for(column.max_definition), static_cast<std::size_t>(count), definitions));
          } else {
            definitions.assign(static_cast<std::size_t>(count), 0);
          }
          const auto value_bytes = raw.subspan(static_cast<std::size_t>(rep_length + def_length));
          if (compressed) {
            CVT_TRY_ASSIGN(body, decompress_page(value_bytes, codec, static_cast<std::uint64_t>(uncompressed_size), ctx.limits));
            values_data = as_u8(body);
          } else {
            values_data = value_bytes;
          }
        }
        std::size_t present = 0;
        for (const std::uint32_t d : definitions) present += d == static_cast<std::uint32_t>(column.max_definition) ? 1 : 0;
        PageValues values;
        CVT_TRY(decode_values(values_data, encoding, column, present, dictionary ? &*dictionary : nullptr, values));
        std::size_t next_value = 0;
        const auto take_cell = [&]() -> ir::Cell {
          const std::size_t i = next_value++;
          if (!values.integers.empty()) {
            if (i >= values.integers.size()) return ir::Cell{};
            if (column.physical == Boolean) return ir::Cell::boolean(values.integers[i] != 0);
            return integer_value_cell(values.integers[i], column);
          }
          if (!values.reals.empty()) return i < values.reals.size() ? number_cell(values.reals[i]) : ir::Cell{};
          return i < values.bytes.size() ? bytes_value_cell(values.bytes[i], column) : ir::Cell{};
        };
        for (std::int64_t k = 0; k < count; ++k) {
          const std::uint32_t d = definitions[static_cast<std::size_t>(k)];
          const std::uint32_t r = repetitions[static_cast<std::size_t>(k)];
          if (column.max_repetition == 0) {
            if (row >= row_base + static_cast<std::size_t>(group_rows)) return fail(Status::Decode, "Parquet column holds more values than rows");
            if (d == static_cast<std::uint32_t>(column.max_definition)) sheet.rows[row + 1][c] = take_cell();
            ++row;
            continue;
          }
          if (r == 0) {
            if (list_open) {
              sheet.rows[row][c] = ir::Cell::string(util::dump_json(list));
              list_open = false;
            }
            if (row >= row_base + static_cast<std::size_t>(group_rows)) return fail(Status::Decode, "Parquet column holds more values than rows");
            ++row;
            list = Json::array();
            list_open = true;
          }
          if (d == static_cast<std::uint32_t>(column.max_definition)) {
            list.push_back(cell_to_json(take_cell()));
          } else if (d + 1 == static_cast<std::uint32_t>(column.max_definition) && r > 0) {
            list.push_back(nullptr);
          } else if (d == 0 && r == 0) {
            list_open = false;  // the whole list is null
          }
        }
        values_read += count;
      }
      if (column.max_repetition > 0 && list_open) sheet.rows[row][c] = ir::Cell::string(util::dump_json(list));
    }
    row_base += static_cast<std::size_t>(group_rows);
  }
  trim_sheet(sheet);
  ir::Table table;
  table.sheets.push_back(std::move(sheet));
  return table;
}

// --------------------------------------------------------------------------- writing

struct OutColumn {
  std::string name;
  int physical = ByteArray;
  enum Kind { Text, Integer, Real, Bool, Date, Timestamp, Time } kind = Text;
};

Result<Bytes> write_parquet(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Sheet* sheet, select_sheet(value.get<ir::Table>(), ctx));
  const std::string compression = ctx.options.get_enum("parquet_compression", "snappy", {"none", "snappy", "gzip"});
  const std::int64_t codec = compression == "snappy" ? 1 : compression == "gzip" ? 2 : 0;
  const std::size_t first = sheet->has_header ? 1 : 0;
  const std::size_t rows = sheet->rows.size() > first ? sheet->rows.size() - first : 0;
  const std::size_t width = sheet->column_count();
  std::vector<OutColumn> columns;
  std::vector<std::string> used;
  for (std::size_t c = 0; c < width; ++c) {
    OutColumn column;
    std::string name = sheet->has_header ? std::string(util::trim(sheet->at(0, c).text)) : std::string();
    if (name.empty()) name = std::format("column{}", c + 1);
    std::string candidate = name;
    for (int n = 2; std::ranges::find(used, candidate) != used.end(); ++n) candidate = std::format("{}_{}", name, n);
    used.push_back(candidate);
    column.name = util::sanitize_utf8(candidate);
    bool any = false;
    bool ints = true;
    bool reals = true;
    bool bools = true;
    bool dates = true;
    bool stamps = true;
    bool times = true;
    for (std::size_t r = first; r < sheet->rows.size(); ++r) {
      const ir::Cell& cell = sheet->at(r, c);
      if (cell.empty()) continue;
      any = true;
      const bool number = cell.type == ir::CellType::Number;
      ints = ints && number && std::isfinite(cell.number) && cell.number == std::trunc(cell.number) && std::fabs(cell.number) < 9.2e18;
      reals = reals && number;
      bools = bools && cell.type == ir::CellType::Bool;
      const bool date = cell.type == ir::CellType::DateTime;
      const double day_seconds = cell.number - std::floor(cell.number / 86400.0) * 86400.0;
      dates = dates && date && day_seconds == 0.0 && !is_time_only(cell.number);
      times = times && date && is_time_only(cell.number);
      stamps = stamps && date;
    }
    if (!any) {
      column.kind = OutColumn::Text;
    } else if (bools) {
      column.kind = OutColumn::Bool;
      column.physical = Boolean;
    } else if (ints) {
      column.kind = OutColumn::Integer;
      column.physical = Int64;
    } else if (reals) {
      column.kind = OutColumn::Real;
      column.physical = Double;
    } else if (dates) {
      column.kind = OutColumn::Date;
      column.physical = Int32;
    } else if (times) {
      column.kind = OutColumn::Time;
      column.physical = Int64;
    } else if (stamps) {
      column.kind = OutColumn::Timestamp;
      column.physical = Int64;
    }
    columns.push_back(std::move(column));
  }
  Bytes file = to_bytes(std::string_view("PAR1"));
  struct ChunkInfo {
    std::int64_t offset = 0;
    std::int64_t compressed = 0;
    std::int64_t uncompressed = 0;
  };
  std::vector<ChunkInfo> chunks;
  for (std::size_t c = 0; c < columns.size(); ++c) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(c) / static_cast<double>(std::max<std::size_t>(1, columns.size()))));
    const OutColumn& column = columns[c];
    // Definition levels (1 = present), RLE/bit-packed hybrid: written as bit-packed groups of 8.
    Bytes levels;
    {
      ThriftWriter varints;
      const std::size_t groups = (rows + 7) / 8;
      varints.varint((groups << 1) | 1);
      levels = varints.take();
      for (std::size_t g = 0; g < groups; ++g) {
        std::uint8_t bits = 0;
        for (std::size_t k = 0; k < 8; ++k) {
          const std::size_t r = g * 8 + k;
          if (r < rows && !sheet->at(first + r, c).empty()) bits |= static_cast<std::uint8_t>(1u << k);
        }
        levels.push_back(static_cast<std::byte>(bits));
      }
    }
    util::ByteWriter values;
    std::uint8_t bool_bits = 0;
    std::size_t bool_count = 0;
    for (std::size_t r = 0; r < rows; ++r) {
      const ir::Cell& cell = sheet->at(first + r, c);
      if (cell.empty()) continue;
      switch (column.kind) {
        case OutColumn::Text: {
          const std::string text = util::sanitize_utf8(cell.text);
          values.u32le(static_cast<std::uint32_t>(text.size()));
          values.text(text);
          break;
        }
        case OutColumn::Integer: {
          // The display text keeps integers beyond 2^53 exactly.
          const auto exact = util::parse_int(cell.text);
          const bool use_text = exact && std::fabs(static_cast<double>(*exact) - cell.number) <= std::fabs(cell.number) * 1e-15;
          values.i64le(use_text ? *exact : static_cast<std::int64_t>(cell.number));
          break;
        }
        case OutColumn::Real: values.f64le(cell.number); break;
        case OutColumn::Bool:
          if (cell.number != 0) bool_bits |= static_cast<std::uint8_t>(1u << bool_count);
          if (++bool_count == 8) {
            values.u8(bool_bits);
            bool_bits = 0;
            bool_count = 0;
          }
          break;
        case OutColumn::Date: values.i32le(static_cast<std::int32_t>(std::floor(cell.number / 86400.0))); break;
        case OutColumn::Timestamp: values.i64le(static_cast<std::int64_t>(std::llround(cell.number * 1e6))); break;
        case OutColumn::Time: values.i64le(static_cast<std::int64_t>(std::llround((cell.number - excel_1900_epoch_unix) * 1e6))); break;
      }
    }
    if (bool_count > 0) values.u8(bool_bits);
    Bytes page;
    {
      util::ByteWriter body;
      body.u32le(static_cast<std::uint32_t>(levels.size()));
      body.bytes(levels);
      body.bytes(values.view());
      page = body.take();
    }
    Bytes compressed;
    if (codec == 1) {
      compressed = snappy_compress(page);
    } else if (codec == 2) {
      CVT_TRY_ASSIGN(compressed, util::gzip_compress(page, 9));
    } else {
      compressed = page;
    }
    ThriftWriter header;
    header.i32(1, 0);  // DATA_PAGE
    header.i32(2, static_cast<std::int64_t>(page.size()));
    header.i32(3, static_cast<std::int64_t>(compressed.size()));
    header.begin_struct(5);
    header.i32(1, static_cast<std::int64_t>(rows));
    header.i32(2, 0);  // PLAIN
    header.i32(3, 3);  // RLE definition levels
    header.i32(4, 3);  // RLE repetition levels
    header.end_struct();
    header.end_struct();
    const Bytes header_bytes = header.take();
    ChunkInfo info;
    info.offset = static_cast<std::int64_t>(file.size());
    info.compressed = static_cast<std::int64_t>(header_bytes.size() + compressed.size());
    info.uncompressed = static_cast<std::int64_t>(header_bytes.size() + page.size());
    append(file, header_bytes);
    append(file, compressed);
    chunks.push_back(info);
    CVT_TRY(ctx.limits.check_output(file.size()));
  }
  // Footer.
  ThriftWriter meta;
  meta.i32(1, 1);
  meta.list(2, Thrift::Struct, columns.size() + 1);
  meta.begin_list_element();
  meta.binary(4, "schema");
  meta.i32(5, static_cast<std::int64_t>(columns.size()));
  meta.end_struct();
  for (const OutColumn& column : columns) {
    meta.begin_list_element();
    meta.i32(1, column.physical);
    meta.i32(3, 1);  // OPTIONAL
    meta.binary(4, column.name);
    switch (column.kind) {
      case OutColumn::Text:
        meta.i32(6, 0);  // UTF8
        meta.begin_struct(10);
        meta.begin_struct(1);
        meta.end_struct();
        meta.end_struct();
        break;
      case OutColumn::Date:
        meta.i32(6, 6);
        meta.begin_struct(10);
        meta.begin_struct(6);
        meta.end_struct();
        meta.end_struct();
        break;
      case OutColumn::Timestamp:
      case OutColumn::Time:
        meta.i32(6, column.kind == OutColumn::Time ? 8 : 10);  // TIME_MICROS / TIMESTAMP_MICROS
        meta.begin_struct(10);
        meta.begin_struct(column.kind == OutColumn::Time ? 7 : 8);
        meta.boolean(1, false);  // not adjusted to UTC: spreadsheet times are local
        meta.begin_struct(2);
        meta.begin_struct(2);  // MICROS
        meta.end_struct();
        meta.end_struct();
        meta.end_struct();
        meta.end_struct();
        break;
      default: break;
    }
    meta.end_struct();
  }
  meta.i64(3, static_cast<std::int64_t>(rows));
  meta.list(4, Thrift::Struct, 1);
  meta.begin_list_element();
  meta.list(1, Thrift::Struct, columns.size());
  std::int64_t total = 0;
  for (std::size_t c = 0; c < columns.size(); ++c) {
    meta.begin_list_element();
    meta.i64(2, chunks[c].offset);
    meta.begin_struct(3);
    meta.i32(1, columns[c].physical);
    meta.list(2, Thrift::I32, 2);
    meta.list_i32(0);
    meta.list_i32(3);
    meta.list(3, Thrift::Binary, 1);
    meta.list_binary(columns[c].name);
    meta.i32(4, codec);
    meta.i64(5, static_cast<std::int64_t>(rows));
    meta.i64(6, chunks[c].uncompressed);
    meta.i64(7, chunks[c].compressed);
    meta.i64(9, chunks[c].offset);
    meta.end_struct();
    meta.end_struct();
    total += chunks[c].uncompressed;
  }
  meta.i64(2, total);
  meta.i64(3, static_cast<std::int64_t>(rows));
  meta.end_struct();
  meta.end_struct();
  const Bytes footer = meta.take();
  append(file, footer);
  util::ByteWriter tail;
  tail.u32le(static_cast<std::uint32_t>(footer.size()));
  tail.text("PAR1");
  append(file, tail.view());
  return file;
}

}  // namespace

void register_parquet(Registry& registry) {
  FormatInfo info;
  info.id = "parquet";
  info.name = "Apache Parquet";
  info.description = "Columnar data file used by data-analysis tools (Spark, pandas, DuckDB, Arrow).";
  info.category = Category::Data;
  info.extensions = {"parquet", "pq"};
  info.mime = "application/vnd.apache.parquet";
  info.popularity = 40;
  info.signatures = {Signature::at(0, "'PAR1'")};
  registry.add_format(std::move(info));

  Codec codec;
  codec.format_id = "parquet";
  codec.kind = Kind::Table;
  codec.read = read_parquet;
  codec.write = write_parquet;
  codec.write_options = {
      OptionSpec::choice("parquet_compression", "Compression", "snappy", {"none", "snappy", "gzip"},
                         "How the column pages are compressed; Snappy is the common default, gzip is smaller."),
      sheet_option(),
  };
  registry.add_codec(std::move(codec));
}

}  // namespace cvt::table
