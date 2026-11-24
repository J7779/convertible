// Convertible core - office family: Palm databases, PalmDOC and HUFF/CDIC.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/office/palm.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace cvt::office::palm {

Result<Database> open(ByteView data, const Limits& limits) {
  util::ByteReader reader(data);
  if (data.size() < 78) return fail(Status::Decode, "the Palm database header is truncated");
  Database db;
  const std::string_view name = reader.chars(32);
  db.name = std::string(name.substr(0, name.find('\0')));
  reader.seek(60);
  db.type = std::string(reader.chars(4));
  db.creator = std::string(reader.chars(4));
  reader.seek(76);
  const std::uint16_t count = reader.u16be();
  CVT_TRY(limits.check_archive_entries(count));
  if (count == 0) return fail(Status::Decode, "the Palm database has no records");
  std::vector<std::uint32_t> offsets;
  offsets.reserve(count);
  for (std::uint16_t i = 0; i < count; ++i) {
    offsets.push_back(reader.u32be());
    reader.skip(4);
  }
  if (!reader.ok()) return fail(Status::Decode, "the Palm database record list is truncated");
  for (std::uint16_t i = 0; i < count; ++i) {
    const std::uint64_t start = offsets[i];
    const std::uint64_t end = i + 1 < count ? offsets[i + 1] : data.size();
    if (start > data.size() || end > data.size() || end < start) {
      // A damaged record list: keep what is valid.
      if (i == 0) return fail(Status::Decode, "the Palm database record offsets are invalid");
      db.records.emplace_back();
      continue;
    }
    db.records.push_back(data.subspan(static_cast<std::size_t>(start), static_cast<std::size_t>(end - start)));
  }
  return db;
}

Result<void> palmdoc_decompress(ByteView in, Bytes& out, std::uint64_t max_out) {
  const auto bytes = as_u8(in);
  std::size_t i = 0;
  while (i < bytes.size()) {
    if (out.size() >= max_out) return fail(Status::Limit, "the decompressed text exceeds the limit");
    const std::uint8_t c = bytes[i++];
    if (c == 0 || (c >= 0x09 && c <= 0x7F)) {
      out.push_back(static_cast<std::byte>(c));
    } else if (c <= 0x08) {
      for (std::uint8_t k = 0; k < c && i < bytes.size(); ++k) out.push_back(static_cast<std::byte>(bytes[i++]));
    } else if (c >= 0xC0) {
      out.push_back(std::byte{' '});
      out.push_back(static_cast<std::byte>(c ^ 0x80));
    } else {
      if (i >= bytes.size()) break;
      const std::uint32_t pair = (static_cast<std::uint32_t>(c) << 8) | bytes[i++];
      const std::uint32_t distance = (pair & 0x3FFF) >> 3;
      const std::uint32_t length = (pair & 7) + 3;
      if (distance == 0 || distance > out.size()) return fail(Status::Decode, "corrupt PalmDOC compressed data");
      for (std::uint32_t k = 0; k < length; ++k) out.push_back(out[out.size() - distance]);
    }
  }
  return {};
}

Bytes palmdoc_compress(ByteView in) {
  const auto data = as_u8(in);
  const std::size_t n = data.size();
  Bytes out;
  out.reserve(n);
  constexpr std::size_t hash_size = 4096;
  std::array<std::int32_t, hash_size> head{};
  head.fill(-1);
  std::vector<std::int32_t> prev(n, -1);
  const auto hash = [&](std::size_t at) {
    return (static_cast<std::size_t>(data[at]) * 2654435761u ^ static_cast<std::size_t>(data[at + 1]) * 40503u ^
            data[at + 2]) & (hash_size - 1);
  };
  const auto insert = [&](std::size_t at) {
    if (at + 2 >= n) return;
    const std::size_t h = hash(at);
    prev[at] = head[h];
    head[h] = static_cast<std::int32_t>(at);
  };
  std::size_t i = 0;
  while (i < n) {
    // Longest match within 2047 bytes, length 3..10.
    std::size_t best_len = 0;
    std::size_t best_dist = 0;
    if (i + 2 < n) {
      int chain = 0;
      for (std::int32_t cand = head[hash(i)]; cand >= 0 && chain < 64; cand = prev[static_cast<std::size_t>(cand)], ++chain) {
        const std::size_t dist = i - static_cast<std::size_t>(cand);
        if (dist > 2047) break;
        std::size_t len = 0;
        while (len < 10 && i + len < n && data[static_cast<std::size_t>(cand) + len] == data[i + len]) ++len;
        if (len > best_len) {
          best_len = len;
          best_dist = dist;
          if (len == 10) break;
        }
      }
    }
    if (best_len >= 3) {
      const std::uint32_t code = 0x8000u | (static_cast<std::uint32_t>(best_dist) << 3) | static_cast<std::uint32_t>(best_len - 3);
      out.push_back(static_cast<std::byte>(code >> 8));
      out.push_back(static_cast<std::byte>(code & 0xFF));
      for (std::size_t k = 0; k < best_len; ++k) insert(i + k);
      i += best_len;
      continue;
    }
    const std::uint8_t c = data[i];
    if (c == ' ' && i + 1 < n && data[i + 1] >= 0x40 && data[i + 1] <= 0x7F) {
      out.push_back(static_cast<std::byte>(data[i + 1] ^ 0x80));
      insert(i);
      insert(i + 1);
      i += 2;
      continue;
    }
    if (c == 0 || (c >= 0x09 && c <= 0x7F)) {
      out.push_back(static_cast<std::byte>(c));
      insert(i);
      ++i;
      continue;
    }
    // Bytes 01..08 and 80..FF: a literal run of up to 8 bytes.
    std::size_t run = 1;
    while (run < 8 && i + run < n && (data[i + run] >= 0x80 || (data[i + run] >= 1 && data[i + run] <= 8))) ++run;
    out.push_back(static_cast<std::byte>(run));
    for (std::size_t k = 0; k < run; ++k) {
      out.push_back(static_cast<std::byte>(data[i + k]));
      insert(i + k);
    }
    i += run;
  }
  return out;
}

Result<void> HuffCdic::load(ByteView huff, const std::vector<ByteView>& cdics) {
  util::ByteReader reader(huff);
  if (!reader.starts_with(std::string_view("HUFF\x00\x00\x00\x18", 8))) return fail(Status::Decode, "bad HUFF record");
  reader.skip(8);
  const std::uint32_t off1 = reader.u32be();
  const std::uint32_t off2 = reader.u32be();
  if (!reader.ok() || std::uint64_t{off1} + 1024 > huff.size() || std::uint64_t{off2} + 256 > huff.size()) {
    return fail(Status::Decode, "bad HUFF record");
  }
  reader.seek(off1);
  for (Code& code : dict1_) {
    const std::uint32_t v = reader.u32be();
    code.length = v & 0x1F;
    code.terminal = (v & 0x80) != 0;
    const std::uint64_t max = v >> 8;
    if (code.length == 0) return fail(Status::Decode, "bad HUFF code table");
    code.max = ((max + 1) << (32 - code.length)) - 1;
  }
  reader.seek(off2);
  min_code_[0] = 0;
  max_code_[0] = 0;
  for (int len = 1; len <= 32; ++len) {
    const std::uint64_t min = reader.u32be();
    const std::uint64_t max = reader.u32be();
    min_code_[len] = min << (32 - len);
    max_code_[len] = ((max + 1) << (32 - len)) - 1;
  }
  for (ByteView cdic : cdics) {
    util::ByteReader c(cdic);
    if (!c.starts_with(std::string_view("CDIC\x00\x00\x00\x10", 8))) return fail(Status::Decode, "bad CDIC record");
    c.skip(8);
    const std::uint32_t phrases = c.u32be();
    const std::uint32_t bits = c.u32be();
    if (!c.ok() || bits > 31) return fail(Status::Decode, "bad CDIC record");
    if (phrases > 4'000'000) return fail(Status::Limit, "too many CDIC phrases");
    const std::uint64_t remaining = phrases > phrases_.size() ? phrases - phrases_.size() : 0;
    const std::uint64_t n = std::min<std::uint64_t>(std::uint64_t{1} << bits, remaining);
    for (std::uint64_t k = 0; k < n; ++k) {
      util::ByteReader offset_reader(cdic);
      offset_reader.seek(static_cast<std::size_t>(16 + 2 * k));
      const std::uint16_t off = offset_reader.u16be();
      util::ByteReader entry(cdic);
      if (!offset_reader.ok() || !entry.seek(16u + off)) return fail(Status::Decode, "bad CDIC entry");
      const std::uint16_t blen = entry.u16be();
      const ByteView slice = entry.bytes(blen & 0x7FFF);
      if (!entry.ok()) return fail(Status::Decode, "bad CDIC entry");
      Phrase phrase;
      phrase.data = slice;
      phrase.literal = (blen & 0x8000) != 0;
      phrases_.push_back(std::move(phrase));
    }
  }
  return {};
}

Result<void> HuffCdic::unpack(ByteView in, Bytes& out, std::uint64_t max_out, int depth) {
  if (depth > 32) return fail(Status::Decode, "HUFF/CDIC phrases nest too deeply");
  const auto data = as_u8(in);
  const auto word = [&](std::size_t pos) {
    std::uint64_t x = 0;
    for (std::size_t k = 0; k < 8; ++k) x = (x << 8) | (pos + k < data.size() ? data[pos + k] : 0u);
    return x;
  };
  std::int64_t bits_left = static_cast<std::int64_t>(data.size()) * 8;
  std::size_t pos = 0;
  std::uint64_t x = word(0);
  int n = 32;
  while (true) {
    if (n <= 0) {
      pos += 4;
      x = word(pos);
      n += 32;
    }
    const std::uint64_t code = (x >> n) & 0xFFFFFFFFull;
    const Code& entry = dict1_[code >> 24];
    std::uint32_t length = entry.length;
    std::uint64_t max = entry.max;
    if (!entry.terminal) {
      while (length < 32 && code < min_code_[length]) ++length;
      max = max_code_[length];
    }
    n -= static_cast<int>(length);
    bits_left -= length;
    if (bits_left < 0) break;
    if (max < code) return fail(Status::Decode, "corrupt Huffman data");
    const std::uint64_t r = (max - code) >> (32 - length);
    if (r >= phrases_.size()) return fail(Status::Decode, "corrupt Huffman data");
    Phrase& phrase = phrases_[static_cast<std::size_t>(r)];
    if (phrase.literal) {
      out.insert(out.end(), phrase.data.begin(), phrase.data.end());
    } else if (phrase.done) {
      out.insert(out.end(), phrase.cache.begin(), phrase.cache.end());
    } else {
      if (phrase.busy) return fail(Status::Decode, "corrupt HUFF/CDIC dictionary (cycle)");
      phrase.busy = true;
      Bytes expanded;
      CVT_TRY(unpack(phrase.data, expanded, max_out, depth + 1));
      phrase.busy = false;
      phrase.cache = std::move(expanded);
      phrase.done = true;
      out.insert(out.end(), phrase.cache.begin(), phrase.cache.end());
    }
    if (out.size() > max_out) return fail(Status::Limit, "the decompressed text exceeds the limit");
  }
  return {};
}

Result<void> HuffCdic::decompress(ByteView in, Bytes& out, std::uint64_t max_out) { return unpack(in, out, max_out, 0); }

std::size_t trailing_size(ByteView record, std::uint32_t flags) {
  const auto data = as_u8(record);
  std::size_t size = 0;
  for (std::uint32_t bits = flags >> 1; bits != 0; bits >>= 1) {
    if ((bits & 1) == 0) continue;
    if (size >= data.size()) return data.size();
    // A variable-width length stored backwards at the end.
    std::uint32_t value = 0;
    std::size_t end = data.size() - size;
    std::array<std::uint8_t, 4> bytes{};
    std::size_t count = 0;
    while (count < 4 && end > 0) {
      const std::uint8_t b = data[--end];
      bytes[count++] = b & 0x7F;
      if (b & 0x80) break;
    }
    for (std::size_t k = count; k-- > 0;) value = (value << 7) | bytes[k];
    size += value;
  }
  if (flags & 1) {
    if (size < data.size()) size += (data[data.size() - size - 1] & 3) + 1;
  }
  return std::min(size, data.size());
}

Bytes write_database(std::string_view name, std::string_view type, std::string_view creator, const std::vector<Bytes>& records) {
  util::ByteWriter out;
  std::string db_name(name.substr(0, 31));
  for (char& c : db_name) {
    if (c == ' ') c = '_';
  }
  out.text(db_name);
  out.zeros(32 - db_name.size());
  out.u16be(0);              // attributes
  out.u16be(0);              // version
  out.u32be(0x7C0E2C00u);    // creation date (fixed: output is reproducible)
  out.u32be(0x7C0E2C00u);    // modification date
  out.u32be(0);              // backup date
  out.u32be(0);              // modification number
  out.u32be(0);              // app info
  out.u32be(0);              // sort info
  out.text(type.substr(0, 4));
  out.text(creator.substr(0, 4));
  out.u32be(static_cast<std::uint32_t>(2 * records.size() - 1));  // unique id seed
  out.u32be(0);              // next record list
  out.u16be(static_cast<std::uint16_t>(records.size()));
  std::uint32_t offset = static_cast<std::uint32_t>(78 + 8 * records.size() + 2);
  for (std::size_t i = 0; i < records.size(); ++i) {
    out.u32be(offset);
    const std::uint32_t uid = static_cast<std::uint32_t>(2 * i);
    out.u8(0).u8(static_cast<std::uint8_t>(uid >> 16)).u8(static_cast<std::uint8_t>(uid >> 8)).u8(static_cast<std::uint8_t>(uid));
    offset += static_cast<std::uint32_t>(records[i].size());
  }
  out.zeros(2);
  for (const Bytes& r : records) out.bytes(r);
  return out.take();
}

}  // namespace cvt::office::palm
