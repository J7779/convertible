// Convertible core - table family: Apache Arrow IPC files and streams
// (Feather version 2), read.
//
// The metadata is FlatBuffers-encoded; a small bounds-checked FlatBuffers
// accessor reads the Message, Schema, RecordBatch and DictionaryBatch tables
// of the Arrow format specification. Supported column types: null, integers,
// floating point (half, single, double), booleans, UTF-8 and binary (normal
// and large), fixed-size binary, decimal128, dates, times, timestamps,
// durations, dictionary-encoded columns, structs (flattened into "a.b"
// columns) and lists (JSON arrays in one cell). Record batch bodies may be
// LZ4-frame compressed (decoded in-house); ZSTD bodies are reported as
// unsupported. Feather version 1 files are not read.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "util/bytes.hpp"
#include "engine/registry.hpp"
#include "families/table/common.hpp"
#include "families/table/table.hpp"
#include "util/hex.hpp"
#include "util/json.hpp"
#include "util/text.hpp"

namespace cvt::table {

namespace {

// --------------------------------------------------------------------------- LZ4 frames

Result<void> lz4_block(ByteView input, Bytes& out, std::uint64_t limit) {
  const auto in = as_u8(input);
  std::size_t pos = 0;
  while (pos < in.size()) {
    const std::uint8_t token = in[pos++];
    std::size_t literal = token >> 4;
    if (literal == 15) {
      std::uint8_t b = 0;
      do {
        if (pos >= in.size()) return fail(Status::Decode, "truncated LZ4 block");
        b = in[pos++];
        literal += b;
      } while (b == 255);
    }
    if (literal > in.size() - pos || out.size() + literal > limit) return fail(Status::Decode, "damaged LZ4 literal run");
    out.insert(out.end(), input.begin() + static_cast<std::ptrdiff_t>(pos), input.begin() + static_cast<std::ptrdiff_t>(pos + literal));
    pos += literal;
    if (pos >= in.size()) break;  // the last sequence has literals only
    if (pos + 2 > in.size()) return fail(Status::Decode, "truncated LZ4 block");
    const std::size_t offset = in[pos] | (in[pos + 1] << 8);
    pos += 2;
    std::size_t match = (token & 15u) + 4u;
    if ((token & 15u) == 15) {
      std::uint8_t b = 0;
      do {
        if (pos >= in.size()) return fail(Status::Decode, "truncated LZ4 block");
        b = in[pos++];
        match += b;
      } while (b == 255);
    }
    if (offset == 0 || offset > out.size() || out.size() + match > limit) return fail(Status::Decode, "damaged LZ4 match");
    const std::size_t from = out.size() - offset;
    for (std::size_t i = 0; i < match; ++i) out.push_back(out[from + i]);
  }
  return {};
}

Result<Bytes> lz4_frame_decompress(ByteView input, std::uint64_t limit) {
  util::ByteReader r(input);
  if (r.u32le() != 0x184D2204u) return fail(Status::Decode, "not an LZ4 frame");
  const std::uint8_t flags = r.u8();
  (void)r.u8();  // block maximum size
  if ((flags >> 6) != 1) return fail(Status::Decode, "unsupported LZ4 frame version");
  const bool block_checksum = (flags & 0x10u) != 0;
  const bool content_size = (flags & 0x08u) != 0;
  if (content_size) (void)r.u64le();
  if ((flags & 0x01u) != 0) (void)r.u32le();  // dictionary id
  (void)r.u8();  // header checksum
  Bytes out;
  while (r.ok()) {
    const std::uint32_t size = r.u32le();
    if (size == 0) break;
    const bool stored = (size & 0x80000000u) != 0;
    const ByteView block = r.bytes(size & 0x7FFFFFFFu);
    if (!r.ok()) return fail(Status::Decode, "truncated LZ4 frame");
    if (stored) {
      if (out.size() + block.size() > limit) return fail(Status::Limit, "LZ4 data exceeds the decompression limit");
      out.insert(out.end(), block.begin(), block.end());
    } else {
      CVT_TRY(lz4_block(block, out, limit));
    }
    if (block_checksum) (void)r.u32le();
  }
  if (!r.ok()) return fail(Status::Decode, "truncated LZ4 frame");
  return out;
}

// --------------------------------------------------------------------------- FlatBuffers

class FlatTable {
 public:
  FlatTable() = default;
  FlatTable(ByteView data, std::size_t position) : data_(data), position_(position) {
    util::ByteReader r(data_);
    if (!r.seek(position_)) return;
    const auto soffset = static_cast<std::int32_t>(r.u32le());
    const std::int64_t vtable = static_cast<std::int64_t>(position_) - soffset;
    if (!r.ok() || vtable < 0 || static_cast<std::uint64_t>(vtable) + 4 > data_.size()) return;
    vtable_ = static_cast<std::size_t>(vtable);
    r.seek(vtable_);
    vtable_size_ = r.u16le();
    if (vtable_ + vtable_size_ > data_.size() || vtable_size_ < 4) return;
    valid_ = true;
  }
  [[nodiscard]] bool valid() const noexcept { return valid_; }

  // Absolute position of field `index`, or 0 when absent.
  [[nodiscard]] std::size_t field(int index) const {
    if (!valid_) return 0;
    const std::size_t entry = 4 + static_cast<std::size_t>(index) * 2;
    if (entry + 2 > vtable_size_) return 0;
    util::ByteReader r(data_);
    r.seek(vtable_ + entry);
    const std::uint16_t offset = r.u16le();
    if (offset == 0 || position_ + offset >= data_.size()) return 0;
    return position_ + offset;
  }
  template <class T>
  [[nodiscard]] T scalar(int index, T fallback) const {
    const std::size_t at = field(index);
    if (at == 0 || at + sizeof(T) > data_.size()) return fallback;
    util::ByteReader r(data_);
    r.seek(at);
    return r.read<T>(std::endian::little);
  }
  // Follow an offset field to the referenced position.
  [[nodiscard]] std::size_t reference(int index) const {
    const std::size_t at = field(index);
    if (at == 0 || at + 4 > data_.size()) return 0;
    util::ByteReader r(data_);
    r.seek(at);
    const std::uint64_t target = at + std::uint64_t{r.u32le()};
    return target < data_.size() ? static_cast<std::size_t>(target) : 0;
  }
  [[nodiscard]] FlatTable table(int index) const {
    const std::size_t at = reference(index);
    return at == 0 ? FlatTable() : FlatTable(data_, at);
  }
  [[nodiscard]] std::string string(int index) const {
    const std::size_t at = reference(index);
    if (at == 0) return {};
    util::ByteReader r(data_);
    r.seek(at);
    const std::uint32_t length = r.u32le();
    const std::string_view text = r.chars(length);
    return r.ok() ? util::sanitize_utf8(text) : std::string();
  }
  // Vector: (element position, count).
  [[nodiscard]] std::pair<std::size_t, std::uint32_t> vector(int index) const {
    const std::size_t at = reference(index);
    if (at == 0) return {0, 0};
    util::ByteReader r(data_);
    r.seek(at);
    const std::uint32_t count = r.u32le();
    if (!r.ok() || count > (data_.size() - at - 4) / 4 + 1) return {0, 0};
    return {at + 4, count};
  }
  [[nodiscard]] FlatTable vector_table(int index, std::uint32_t element) const {
    const auto [start, count] = vector(index);
    if (element >= count) return {};
    const std::size_t slot = start + std::size_t{element} * 4;
    util::ByteReader r(data_);
    if (!r.seek(slot)) return {};
    const std::uint64_t target = slot + std::uint64_t{r.u32le()};
    return r.ok() && target < data_.size() ? FlatTable(data_, static_cast<std::size_t>(target)) : FlatTable();
  }
  [[nodiscard]] ByteView data() const noexcept { return data_; }

 private:
  ByteView data_{};
  std::size_t position_ = 0;
  std::size_t vtable_ = 0;
  std::uint16_t vtable_size_ = 0;
  bool valid_ = false;
};

FlatTable root_table(ByteView data) {
  util::ByteReader r(data);
  const std::uint32_t offset = r.u32le();
  if (!r.ok() || offset >= data.size()) return {};
  return FlatTable(data, offset);
}

// --------------------------------------------------------------------------- schema

enum class ArrowType { Null, Int, Float, Binary, Utf8, Bool, Decimal, Date, Time, Timestamp, Duration, FixedBinary, List, Struct,
                       LargeBinary, LargeUtf8, LargeList, FixedList, Map, Unsupported };

struct ArrowField {
  std::string name;
  ArrowType type = ArrowType::Unsupported;
  int bit_width = 0;
  bool is_signed = true;
  int precision = 0;      // float: 0 half, 1 single, 2 double
  int unit = 0;           // date: 0 day, 1 ms; time/timestamp/duration: 0 s, 1 ms, 2 us, 3 ns
  std::int64_t scale = 0;
  int byte_width = 0;
  int list_size = 0;
  bool dictionary = false;
  std::int64_t dictionary_id = 0;
  int index_bits = 32;
  std::vector<ArrowField> children;
};

Result<ArrowField> parse_field(const FlatTable& field, int depth) {
  if (depth > 32) return fail(Status::Decode, "Arrow schema is nested too deeply");
  ArrowField out;
  out.name = field.string(0);
  const auto type_code = field.scalar<std::uint8_t>(2, 0);
  const FlatTable type = field.table(3);
  switch (type_code) {
    case 1: out.type = ArrowType::Null; break;
    case 2:
      out.type = ArrowType::Int;
      out.bit_width = type.scalar<std::int32_t>(0, 32);
      out.is_signed = type.scalar<std::uint8_t>(1, 0) != 0;
      break;
    case 3: out.type = ArrowType::Float; out.precision = type.scalar<std::int16_t>(0, 0); break;
    case 4: out.type = ArrowType::Binary; break;
    case 5: out.type = ArrowType::Utf8; break;
    case 6: out.type = ArrowType::Bool; break;
    case 7:
      out.type = ArrowType::Decimal;
      out.scale = type.scalar<std::int32_t>(1, 0);
      out.bit_width = type.scalar<std::int32_t>(2, 128);
      break;
    case 8: out.type = ArrowType::Date; out.unit = type.scalar<std::int16_t>(0, 1); break;
    case 9:
      out.type = ArrowType::Time;
      out.unit = type.scalar<std::int16_t>(0, 1);
      out.bit_width = type.scalar<std::int32_t>(1, 32);
      break;
    case 10: out.type = ArrowType::Timestamp; out.unit = type.scalar<std::int16_t>(0, 0); break;
    case 12: out.type = ArrowType::List; break;
    case 13: out.type = ArrowType::Struct; break;
    case 15: out.type = ArrowType::FixedBinary; out.byte_width = type.scalar<std::int32_t>(0, 0); break;
    case 16: out.type = ArrowType::FixedList; out.list_size = type.scalar<std::int32_t>(0, 0); break;
    case 17: out.type = ArrowType::Map; break;
    case 18: out.type = ArrowType::Duration; out.unit = type.scalar<std::int16_t>(0, 1); break;
    case 19: out.type = ArrowType::LargeBinary; break;
    case 20: out.type = ArrowType::LargeUtf8; break;
    case 21: out.type = ArrowType::LargeList; break;
    default: out.type = ArrowType::Unsupported; break;
  }
  if (const FlatTable dict = field.table(4); dict.valid()) {
    out.dictionary = true;
    out.dictionary_id = dict.scalar<std::int64_t>(0, 0);
    const FlatTable index = dict.table(1);
    out.index_bits = index.valid() ? index.scalar<std::int32_t>(0, 32) : 32;
  }
  const auto [start, count] = field.vector(5);
  (void)start;
  for (std::uint32_t i = 0; i < count; ++i) {
    CVT_TRY_ASSIGN(ArrowField child, parse_field(field.vector_table(5, i), depth + 1));
    out.children.push_back(std::move(child));
  }
  return out;
}

// --------------------------------------------------------------------------- batches

class BatchReader {
 public:
  BatchReader(const FlatTable& batch, ByteView body, const Limits& limits, std::uint64_t& decompressed)
      : batch_(batch), body_(body), limits_(limits), decompressed_(decompressed) {
    if (const FlatTable compression = batch.table(3); compression.valid()) codec_ = compression.scalar<std::int8_t>(0, 0);
  }

  struct Node {
    std::int64_t length = 0;
    std::int64_t nulls = 0;
  };

  Result<Node> next_node() {
    const auto [start, count] = batch_.vector(1);
    if (node_ >= count) return fail(Status::Decode, "Arrow record batch has too few field nodes");
    util::ByteReader r(batch_.data());
    r.seek(start + std::size_t{node_} * 16);
    ++node_;
    Node node;
    node.length = static_cast<std::int64_t>(r.u64le());
    node.nulls = static_cast<std::int64_t>(r.u64le());
    // Every value needs at least a bit of the body (nulls included), which
    // bounds what a corrupt length may make us allocate.
    const auto bound = std::max<std::int64_t>(static_cast<std::int64_t>(body_.size()) * 8, 65536);
    if (!r.ok() || node.length < 0 || node.length > bound) return fail(Status::Decode, "damaged Arrow field node");
    return node;
  }

  Result<Bytes> next_buffer() {
    const auto [start, count] = batch_.vector(2);
    if (buffer_ >= count) return fail(Status::Decode, "Arrow record batch has too few buffers");
    util::ByteReader r(batch_.data());
    r.seek(start + std::size_t{buffer_} * 16);
    ++buffer_;
    const std::uint64_t offset = r.u64le();
    const std::uint64_t length = r.u64le();
    if (!r.ok() || offset > body_.size() || length > body_.size() - offset) return fail(Status::Decode, "Arrow buffer lies outside the body");
    const ByteView raw = body_.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(length));
    if (codec_ < 0 || raw.empty()) return to_bytes(raw);
    util::ByteReader c(raw);
    const auto uncompressed = static_cast<std::int64_t>(c.u64le());
    if (!c.ok()) return fail(Status::Decode, "damaged compressed Arrow buffer");
    if (uncompressed == -1) return to_bytes(c.rest());
    if (codec_ != 0) return fail(Status::Decode, "Arrow buffers compressed with Zstandard are not supported");
    if (uncompressed < 0) return fail(Status::Decode, "damaged compressed Arrow buffer");
    decompressed_ += static_cast<std::uint64_t>(uncompressed);
    CVT_TRY(limits_.check_decompressed(decompressed_));
    return lz4_frame_decompress(c.rest(), static_cast<std::uint64_t>(uncompressed));
  }

 private:
  FlatTable batch_;
  ByteView body_;
  const Limits& limits_;
  std::uint64_t& decompressed_;
  std::int8_t codec_ = -1;
  std::uint32_t node_ = 0;
  std::uint32_t buffer_ = 0;
};

bool valid_bit(const Bytes& validity, std::int64_t index) {
  if (validity.empty()) return true;
  const auto byte = static_cast<std::size_t>(index / 8);
  return byte < validity.size() && ((std::to_integer<unsigned>(validity[byte]) >> (index % 8)) & 1u) != 0;
}

template <class T>
T load(const Bytes& data, std::size_t index) {
  T value{};
  if ((index + 1) * sizeof(T) <= data.size()) ::cvt::util::copy_bytes(&value, data.data() + index * sizeof(T), sizeof(T));
  if constexpr (std::endian::native == std::endian::big && sizeof(T) > 1) {
    if constexpr (std::is_integral_v<T>) value = std::byteswap(value);
  }
  return value;
}

double unit_seconds(int unit) { return unit == 0 ? 1.0 : unit == 1 ? 1e-3 : unit == 2 ? 1e-6 : 1e-9; }

using Dictionaries = std::map<std::int64_t, std::vector<ir::Cell>>;

Result<std::vector<ir::Cell>> read_column(const ArrowField& field, BatchReader& batch, const Dictionaries& dictionaries, int depth);

Json cell_json(const ir::Cell& cell) {
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

Result<std::vector<ir::Cell>> read_primitive(const ArrowField& field, BatchReader& batch, const BatchReader::Node& node,
                                             const Bytes& validity, const Dictionaries& dictionaries) {
  std::vector<ir::Cell> cells(static_cast<std::size_t>(node.length));
  if (field.dictionary) {
    CVT_TRY_ASSIGN(const Bytes indices, batch.next_buffer());
    const auto it = dictionaries.find(field.dictionary_id);
    if (it == dictionaries.end()) return fail(Status::Decode, "Arrow dictionary {} is missing", field.dictionary_id);
    for (std::int64_t i = 0; i < node.length; ++i) {
      if (!valid_bit(validity, i)) continue;
      std::int64_t index = 0;
      const auto k = static_cast<std::size_t>(i);
      switch (field.index_bits) {
        case 8: index = load<std::int8_t>(indices, k); break;
        case 16: index = load<std::int16_t>(indices, k); break;
        case 64: index = load<std::int64_t>(indices, k); break;
        default: index = load<std::int32_t>(indices, k); break;
      }
      if (index < 0 || static_cast<std::uint64_t>(index) >= it->second.size()) return fail(Status::Decode, "Arrow dictionary index out of range");
      cells[k] = it->second[static_cast<std::size_t>(index)];
    }
    return cells;
  }
  switch (field.type) {
    case ArrowType::Null: return cells;
    case ArrowType::Bool: {
      CVT_TRY_ASSIGN(const Bytes bits, batch.next_buffer());
      for (std::int64_t i = 0; i < node.length; ++i) {
        if (valid_bit(validity, i)) cells[static_cast<std::size_t>(i)] = ir::Cell::boolean(valid_bit(bits, i) && !bits.empty());
      }
      return cells;
    }
    case ArrowType::Int:
    case ArrowType::Date:
    case ArrowType::Time:
    case ArrowType::Timestamp:
    case ArrowType::Duration: {
      CVT_TRY_ASSIGN(const Bytes data, batch.next_buffer());
      int width = field.bit_width;
      if (field.type == ArrowType::Date) width = field.unit == 0 ? 32 : 64;
      if (field.type == ArrowType::Timestamp || field.type == ArrowType::Duration) width = 64;
      if (width != 8 && width != 16 && width != 32 && width != 64) return fail(Status::Decode, "unsupported Arrow integer width {}", width);
      if (data.size() < static_cast<std::uint64_t>(node.length) * static_cast<std::uint64_t>(width / 8)) return fail(Status::Decode, "Arrow buffer is too short");
      for (std::int64_t i = 0; i < node.length; ++i) {
        if (!valid_bit(validity, i)) continue;
        const auto k = static_cast<std::size_t>(i);
        std::int64_t v = 0;
        std::uint64_t u = 0;
        switch (width) {
          case 8: v = load<std::int8_t>(data, k); u = load<std::uint8_t>(data, k); break;
          case 16: v = load<std::int16_t>(data, k); u = load<std::uint16_t>(data, k); break;
          case 32: v = load<std::int32_t>(data, k); u = load<std::uint32_t>(data, k); break;
          default: v = load<std::int64_t>(data, k); u = load<std::uint64_t>(data, k); break;
        }
        ir::Cell cell;
        switch (field.type) {
          case ArrowType::Date:
            cell = datetime_cell(field.unit == 0 ? static_cast<double>(v) * 86400.0 : static_cast<double>(v) / 1000.0, DateStyle::Date);
            break;
          case ArrowType::Time:
            cell = datetime_cell(excel_1900_epoch_unix + static_cast<double>(v) * unit_seconds(field.unit), DateStyle::Time);
            break;
          case ArrowType::Timestamp:
            cell = datetime_cell(static_cast<double>(v) * unit_seconds(field.unit), DateStyle::DateTime);
            break;
          case ArrowType::Duration: cell = number_cell(static_cast<double>(v) * unit_seconds(field.unit)); break;
          default:
            cell = field.is_signed ? integer_cell(v) : ir::Cell::numeric(static_cast<double>(u), std::to_string(u));
            break;
        }
        cells[k] = std::move(cell);
      }
      return cells;
    }
    case ArrowType::Float: {
      CVT_TRY_ASSIGN(const Bytes data, batch.next_buffer());
      for (std::int64_t i = 0; i < node.length; ++i) {
        if (!valid_bit(validity, i)) continue;
        const auto k = static_cast<std::size_t>(i);
        double value = 0;
        if (field.precision == 2) {
          value = std::bit_cast<double>(load<std::uint64_t>(data, k));
        } else if (field.precision == 1) {
          value = std::bit_cast<float>(load<std::uint32_t>(data, k));
        } else {
          const std::uint16_t bits = load<std::uint16_t>(data, k);
          const int exponent = (bits >> 10) & 0x1F;
          const int mantissa = bits & 0x3FF;
          value = exponent == 0 ? std::ldexp(mantissa, -24) : exponent == 31 ? (mantissa == 0 ? INFINITY : NAN) : std::ldexp(mantissa + 1024, exponent - 25);
          if ((bits & 0x8000) != 0) value = -value;
        }
        cells[k] = number_cell(value);
      }
      return cells;
    }
    case ArrowType::Decimal: {
      CVT_TRY_ASSIGN(const Bytes data, batch.next_buffer());
      const std::size_t width = static_cast<std::size_t>(field.bit_width / 8);
      if (width != 16 && width != 32) return fail(Status::Decode, "unsupported Arrow decimal width");
      for (std::int64_t i = 0; i < node.length; ++i) {
        if (!valid_bit(validity, i)) continue;
        const std::size_t at = static_cast<std::size_t>(i) * width;
        if (at + width > data.size()) return fail(Status::Decode, "Arrow buffer is too short");
        // Little-endian two's complement -> decimal digits (big-endian for the conversion).
        std::vector<std::uint8_t> magnitude(width);
        for (std::size_t b = 0; b < width; ++b) magnitude[b] = std::to_integer<std::uint8_t>(data[at + width - 1 - b]);
        const bool negative = (magnitude[0] & 0x80) != 0;
        if (negative) {
          for (std::uint8_t& b : magnitude) b = static_cast<std::uint8_t>(~b);
          for (std::size_t b = width; b-- > 0;) {
            if (++magnitude[b] != 0) break;
          }
        }
        std::string digits;
        while (!magnitude.empty()) {
          std::uint32_t remainder = 0;
          std::vector<std::uint8_t> quotient;
          for (const std::uint8_t b : magnitude) {
            const std::uint32_t current = remainder * 256 + b;
            if (!quotient.empty() || current / 10 != 0) quotient.push_back(static_cast<std::uint8_t>(current / 10));
            remainder = current % 10;
          }
          digits.insert(digits.begin(), static_cast<char>('0' + remainder));
          magnitude = std::move(quotient);
        }
        if (field.scale > 0) {
          const auto scale = static_cast<std::size_t>(field.scale);
          if (digits.size() <= scale) digits.insert(0, scale - digits.size() + 1, '0');
          digits.insert(digits.size() - scale, ".");
        }
        if (negative) digits.insert(0, "-");
        cells[static_cast<std::size_t>(i)] = ir::Cell::numeric(util::parse_double(digits).value_or(0.0), digits);
      }
      return cells;
    }
    case ArrowType::Binary:
    case ArrowType::Utf8:
    case ArrowType::LargeBinary:
    case ArrowType::LargeUtf8: {
      CVT_TRY_ASSIGN(const Bytes offsets, batch.next_buffer());
      CVT_TRY_ASSIGN(const Bytes data, batch.next_buffer());
      const bool large = field.type == ArrowType::LargeBinary || field.type == ArrowType::LargeUtf8;
      const bool text = field.type == ArrowType::Utf8 || field.type == ArrowType::LargeUtf8;
      for (std::int64_t i = 0; i < node.length; ++i) {
        if (!valid_bit(validity, i)) continue;
        const auto k = static_cast<std::size_t>(i);
        const std::int64_t begin = large ? load<std::int64_t>(offsets, k) : load<std::int32_t>(offsets, k);
        const std::int64_t end = large ? load<std::int64_t>(offsets, k + 1) : load<std::int32_t>(offsets, k + 1);
        if (begin < 0 || end < begin || static_cast<std::uint64_t>(end) > data.size()) return fail(Status::Decode, "damaged Arrow offsets");
        const ByteView bytes = ByteView(data).subspan(static_cast<std::size_t>(begin), static_cast<std::size_t>(end - begin));
        cells[k] = text || util::is_valid_utf8(as_chars(bytes)) ? ir::Cell::string(util::sanitize_utf8(as_chars(bytes)))
                                                                : ir::Cell::string(util::hex_encode(bytes));
      }
      return cells;
    }
    case ArrowType::FixedBinary: {
      CVT_TRY_ASSIGN(const Bytes data, batch.next_buffer());
      const auto width = static_cast<std::size_t>(std::max(field.byte_width, 0));
      for (std::int64_t i = 0; i < node.length; ++i) {
        if (!valid_bit(validity, i)) continue;
        const std::size_t at = static_cast<std::size_t>(i) * width;
        if (at + width > data.size()) return fail(Status::Decode, "Arrow buffer is too short");
        cells[static_cast<std::size_t>(i)] = ir::Cell::string(util::hex_encode(ByteView(data).subspan(at, width)));
      }
      return cells;
    }
    default: return fail(Status::Decode, "unsupported Arrow column type in column \"{}\"", field.name);
  }
}

Result<std::vector<ir::Cell>> read_column(const ArrowField& field, BatchReader& batch, const Dictionaries& dictionaries, int depth) {
  if (depth > 32) return fail(Status::Decode, "Arrow data is nested too deeply");
  CVT_TRY_ASSIGN(const BatchReader::Node node, batch.next_node());
  Bytes validity;
  if (field.type != ArrowType::Null) {
    CVT_TRY_ASSIGN(validity, batch.next_buffer());
  }
  if (field.type == ArrowType::List || field.type == ArrowType::LargeList || field.type == ArrowType::Map ||
      field.type == ArrowType::FixedList) {
    Bytes offsets;
    if (field.type != ArrowType::FixedList) {
      CVT_TRY_ASSIGN(offsets, batch.next_buffer());
    }
    if (field.children.empty()) return fail(Status::Decode, "Arrow list without a child");
    CVT_TRY_ASSIGN(const std::vector<ir::Cell> items, read_column(field.children[0], batch, dictionaries, depth + 1));
    std::vector<ir::Cell> cells(static_cast<std::size_t>(node.length));
    for (std::int64_t i = 0; i < node.length; ++i) {
      if (!valid_bit(validity, i)) continue;
      const auto k = static_cast<std::size_t>(i);
      std::int64_t begin = 0;
      std::int64_t end = 0;
      if (field.type == ArrowType::FixedList) {
        begin = i * field.list_size;
        end = begin + field.list_size;
      } else if (field.type == ArrowType::LargeList) {
        begin = load<std::int64_t>(offsets, k);
        end = load<std::int64_t>(offsets, k + 1);
      } else {
        begin = load<std::int32_t>(offsets, k);
        end = load<std::int32_t>(offsets, k + 1);
      }
      if (begin < 0 || end < begin || static_cast<std::uint64_t>(end) > items.size()) return fail(Status::Decode, "damaged Arrow list offsets");
      Json list = Json::array();
      for (std::int64_t j = begin; j < end; ++j) list.push_back(cell_json(items[static_cast<std::size_t>(j)]));
      cells[k] = ir::Cell::string(util::dump_json(list));
    }
    return cells;
  }
  if (field.type == ArrowType::Struct) {
    // Nested struct inside a list: render each row as a JSON object.
    std::vector<std::vector<ir::Cell>> children;
    for (const ArrowField& child : field.children) {
      CVT_TRY_ASSIGN(std::vector<ir::Cell> values, read_column(child, batch, dictionaries, depth + 1));
      children.push_back(std::move(values));
    }
    std::vector<ir::Cell> cells(static_cast<std::size_t>(node.length));
    for (std::int64_t i = 0; i < node.length; ++i) {
      if (!valid_bit(validity, i)) continue;
      Json object = Json::object();
      for (std::size_t c = 0; c < children.size(); ++c) {
        object.set(field.children[c].name, static_cast<std::size_t>(i) < children[c].size() ? cell_json(children[c][static_cast<std::size_t>(i)]) : Json());
      }
      cells[static_cast<std::size_t>(i)] = ir::Cell::string(util::dump_json(object));
    }
    return cells;
  }
  return read_primitive(field, batch, node, validity, dictionaries);
}

// Top-level struct columns are flattened into one column per leaf.
struct Leaf {
  std::string name;
  const ArrowField* field = nullptr;
};

void collect_leaves(const ArrowField& field, const std::string& prefix, std::vector<Leaf>& out) {
  const std::string name = prefix.empty() ? field.name : prefix + "." + field.name;
  if (field.type == ArrowType::Struct && !field.dictionary) {
    for (const ArrowField& child : field.children) collect_leaves(child, name, out);
    return;
  }
  out.push_back({name, &field});
}

Result<void> read_top_level(const ArrowField& field, BatchReader& batch, const Dictionaries& dictionaries,
                            std::vector<std::vector<ir::Cell>>& columns, std::size_t& column, std::int64_t rows, int depth,
                            const Bytes* parent_validity) {
  if (field.type == ArrowType::Struct && !field.dictionary) {
    CVT_TRY_ASSIGN(const BatchReader::Node node, batch.next_node());
    (void)node;
    CVT_TRY_ASSIGN(const Bytes validity, batch.next_buffer());
    for (const ArrowField& child : field.children) {
      CVT_TRY(read_top_level(child, batch, dictionaries, columns, column, rows, depth + 1, &validity));
    }
    return {};
  }
  CVT_TRY_ASSIGN(std::vector<ir::Cell> cells, read_column(field, batch, dictionaries, depth));
  if (parent_validity != nullptr) {
    for (std::size_t i = 0; i < cells.size(); ++i) {
      if (!valid_bit(*parent_validity, static_cast<std::int64_t>(i))) cells[i] = ir::Cell{};
    }
  }
  if (column >= columns.size()) return fail(Status::Decode, "Arrow batch does not match its schema");
  cells.resize(static_cast<std::size_t>(std::max<std::int64_t>(rows, 0)));
  for (ir::Cell& cell : cells) columns[column].push_back(std::move(cell));
  ++column;
  return {};
}

Result<AnyIR> read_arrow(ReadContext& ctx) {
  ByteView input = ctx.input;
  std::size_t pos = 0;
  if (as_chars(input).starts_with("ARROW1")) pos = 8;  // file format: magic and padding, then a stream
  if (as_chars(input).starts_with("FEA1")) return fail(Status::Decode, "Feather version 1 files are not supported (only Arrow IPC / Feather 2)");
  std::vector<ArrowField> fields;
  bool have_schema = false;
  Dictionaries dictionaries;
  std::vector<std::vector<ir::Cell>> columns;
  std::vector<Leaf> leaves;
  std::uint64_t decompressed = 0;
  CellBudget budget(ctx.limits);
  std::size_t messages = 0;
  while (pos + 8 <= input.size()) {
    if ((++messages & 63) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(pos) / static_cast<double>(input.size())));
    util::ByteReader r(input.subspan(pos));
    std::uint32_t length = r.u32le();
    if (length == 0xFFFFFFFFu) length = r.u32le();
    if (length == 0) break;  // end of stream
    const std::size_t meta_start = pos + r.position();
    if (length > input.size() - meta_start) return fail(Status::Decode, "Arrow message runs past the end of the file");
    const ByteView meta = input.subspan(meta_start, length);
    const FlatTable message = root_table(meta);
    if (!message.valid()) return fail(Status::Decode, "damaged Arrow message");
    const auto header_type = message.scalar<std::uint8_t>(1, 0);
    const FlatTable header = message.table(2);
    const auto body_length = message.scalar<std::int64_t>(3, 0);
    const std::size_t body_start = meta_start + length;
    if (body_length < 0 || static_cast<std::uint64_t>(body_length) > input.size() - body_start) {
      return fail(Status::Decode, "Arrow message body runs past the end of the file");
    }
    const ByteView body = input.subspan(body_start, static_cast<std::size_t>(body_length));
    pos = body_start + static_cast<std::size_t>(body_length);
    if (header_type == 1) {  // Schema
      const auto [start, count] = header.vector(1);
      (void)start;
      if (count > 100'000) return fail(Status::Limit, "the Arrow schema has too many fields");
      fields.clear();
      for (std::uint32_t i = 0; i < count; ++i) {
        CVT_TRY_ASSIGN(ArrowField field, parse_field(header.vector_table(1, i), 0));
        fields.push_back(std::move(field));
      }
      leaves.clear();
      for (const ArrowField& field : fields) collect_leaves(field, "", leaves);
      columns.assign(leaves.size(), {});
      have_schema = true;
    } else if (header_type == 2 && have_schema) {  // DictionaryBatch
      const auto id = header.scalar<std::int64_t>(0, 0);
      const FlatTable data = header.table(1);
      const bool delta = header.scalar<std::uint8_t>(2, 0) != 0;
      // Find the dictionary's value type: the field that uses this id.
      const ArrowField* owner = nullptr;
      const auto search = [&](const auto& self, const std::vector<ArrowField>& list) -> void {
        for (const ArrowField& f : list) {
          if (f.dictionary && f.dictionary_id == id) owner = &f;
          self(self, f.children);
        }
      };
      search(search, fields);
      if (owner == nullptr) return fail(Status::Decode, "Arrow dictionary {} has no field", id);
      ArrowField value_type = *owner;
      value_type.dictionary = false;
      BatchReader batch(data, body, ctx.limits, decompressed);
      CVT_TRY_ASSIGN(std::vector<ir::Cell> values, read_column(value_type, batch, dictionaries, 0));
      CVT_TRY(budget.take(values.size()));
      auto& target = dictionaries[id];
      if (!delta) target.clear();
      for (ir::Cell& cell : values) target.push_back(std::move(cell));
    } else if (header_type == 3 && have_schema) {  // RecordBatch
      const auto rows = header.scalar<std::int64_t>(0, 0);
      if (rows < 0) return fail(Status::Decode, "negative Arrow row count");
      CVT_TRY(budget.take(static_cast<std::uint64_t>(rows) * std::max<std::size_t>(1, leaves.size())));
      BatchReader batch(header, body, ctx.limits, decompressed);
      std::size_t column = 0;
      for (const ArrowField& field : fields) CVT_TRY(read_top_level(field, batch, dictionaries, columns, column, rows, 0, nullptr));
    }
  }
  if (!have_schema) return fail(Status::Decode, "not an Arrow IPC file (no schema)");
  ir::Sheet sheet;
  {
    std::string stem(ctx.filename);
    if (const std::size_t slash = stem.find_last_of("/\\"); slash != std::string::npos) stem.erase(0, slash + 1);
    if (const std::size_t dot = stem.rfind('.'); dot != std::string::npos && dot > 0) stem.resize(dot);
    sheet.name = stem;
  }
  sheet.has_header = true;
  std::vector<ir::Cell> header;
  for (const Leaf& leaf : leaves) header.push_back(ir::Cell::string(leaf.name));
  sheet.rows.push_back(std::move(header));
  const std::size_t rows = columns.empty() ? 0 : columns[0].size();
  for (std::size_t r = 0; r < rows; ++r) {
    std::vector<ir::Cell> row;
    for (auto& column : columns) row.push_back(r < column.size() ? std::move(column[r]) : ir::Cell{});
    sheet.rows.push_back(std::move(row));
  }
  trim_sheet(sheet);
  ir::Table table;
  table.sheets.push_back(std::move(sheet));
  return table;
}

int sniff_arrow_stream(ByteView data, std::string_view) {
  // A stream starts with the continuation marker and a Schema message.
  return as_chars(data).starts_with("\xFF\xFF\xFF\xFF") && data.size() > 16 ? 40 : 0;
}

}  // namespace

void register_arrow(Registry& registry) {
  FormatInfo info;
  info.id = "arrow";
  info.name = "Apache Arrow IPC / Feather";
  info.description = "Arrow columnar file (also Feather version 2), as written by pandas, R and Arrow libraries (read only).";
  info.category = Category::Data;
  info.extensions = {"arrow", "feather", "ipc"};
  info.mime = "application/vnd.apache.arrow.file";
  info.popularity = 25;
  info.signatures = {Signature::at(0, "'ARROW1' 00 00")};
  registry.add_format(std::move(info));

  FormatInfo stream;
  stream.id = "arrows";
  stream.name = "Apache Arrow IPC stream";
  stream.description = "Arrow streaming format (read only).";
  stream.category = Category::Data;
  stream.extensions = {"arrows"};
  stream.mime = "application/vnd.apache.arrow.stream";
  stream.popularity = 10;
  stream.sniff = sniff_arrow_stream;
  registry.add_format(std::move(stream));

  for (const char* id : {"arrow", "arrows"}) {
    Codec codec;
    codec.format_id = id;
    codec.kind = Kind::Table;
    codec.read = read_arrow;
    registry.add_codec(std::move(codec));
  }
}

}  // namespace cvt::table
