// Convertible core - audio_lossless family: tag formats.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/audio_lossless/tags.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "util/text.hpp"

namespace cvt::audio::tags {

void put(TagMap& tags, std::string_view key, std::string_view value) {
  std::string clean;
  for (const char c : value) {
    if (c != '\0') clean.push_back(c);
  }
  const std::string_view trimmed = util::trim(clean);
  if (trimmed.empty() || key.empty()) return;
  tags.emplace(util::to_lower(key), util::sanitize_utf8(trimmed));
}

namespace {

std::string legacy_text(ByteView bytes) {
  const std::string_view text = as_chars(bytes);
  if (util::is_valid_utf8(text)) return std::string(text);
  return util::decode_text(bytes, util::TextEncoding::Windows1252).text;
}

struct Pair {
  std::string_view a;
  std::string_view b;
};

constexpr std::array<Pair, 12> riff_keys = {{{"INAM", "title"},
                                             {"IART", "artist"},
                                             {"IPRD", "album"},
                                             {"ICRD", "date"},
                                             {"ITRK", "track"},
                                             {"IPRT", "track"},
                                             {"IGNR", "genre"},
                                             {"ICMT", "comment"},
                                             {"ICOP", "copyright"},
                                             {"ISFT", "encoder"},
                                             {"IMUS", "composer"},
                                             {"IENG", "engineer"}}};

}  // namespace

void parse_riff_info(ByteView body, TagMap& tags) {
  util::ByteReader r(body);
  while (r.remaining() >= 8) {
    const std::string_view id = r.chars(4);
    const std::uint32_t size = r.u32le();
    if (size > r.remaining()) break;
    const ByteView value = r.bytes(size);
    if (size % 2 == 1 && !r.at_end()) r.skip(1);
    for (const Pair& pair : riff_keys) {
      if (pair.a == id) put(tags, pair.b, legacy_text(value));
    }
  }
}

Bytes riff_info_chunk(const TagMap& tags) {
  util::ByteWriter body;
  body.text("INFO");
  bool any = false;
  for (const Pair& pair : riff_keys) {
    if (pair.a == "IPRT") continue;
    const auto it = tags.find(std::string(pair.b));
    if (it == tags.end()) continue;
    body.text(pair.a);
    body.u32le(static_cast<std::uint32_t>(it->second.size() + 1));
    body.text(it->second);
    body.u8(0);
    body.align(2);
    any = true;
  }
  if (!any) return {};
  util::ByteWriter out;
  out.text("LIST");
  out.u32le(static_cast<std::uint32_t>(body.size()));
  out.bytes(body.view());
  return out.take();
}

// --------------------------------------------------------------------------- ID3v2

namespace {

constexpr std::array<Pair, 26> id3_keys = {{{"TIT2", "title"},   {"TT2", "title"},       {"TPE1", "artist"},
                                            {"TP1", "artist"},   {"TALB", "album"},      {"TAL", "album"},
                                            {"TPE2", "albumartist"}, {"TP2", "albumartist"}, {"TYER", "date"},
                                            {"TYE", "date"},     {"TDRC", "date"},       {"TRCK", "track"},
                                            {"TRK", "track"},    {"TPOS", "disc"},       {"TPA", "disc"},
                                            {"TCON", "genre"},   {"TCO", "genre"},       {"TCOM", "composer"},
                                            {"TCM", "composer"}, {"TCOP", "copyright"},  {"TCR", "copyright"},
                                            {"TSSE", "encoder"}, {"TSS", "encoder"},     {"COMM", "comment"},
                                            {"COM", "comment"},  {"TXXX", ""}}};

std::uint32_t syncsafe(std::uint32_t v) noexcept {
  return ((v >> 24) & 0x7F) << 21 | ((v >> 16) & 0x7F) << 14 | ((v >> 8) & 0x7F) << 7 | (v & 0x7F);
}

Bytes remove_unsync(ByteView data) {
  Bytes out;
  out.reserve(data.size());
  for (std::size_t i = 0; i < data.size(); ++i) {
    out.push_back(data[i]);
    if (std::to_integer<std::uint8_t>(data[i]) == 0xFF && i + 1 < data.size() &&
        std::to_integer<std::uint8_t>(data[i + 1]) == 0x00) {
      ++i;
    }
  }
  return out;
}

// Decode one string of an ID3 text encoding; `consumed` receives the bytes
// used including the terminator.
std::string id3_string(ByteView data, int encoding, std::size_t* consumed) {
  const bool wide = encoding == 1 || encoding == 2;
  std::size_t end = 0;
  if (wide) {
    while (end + 1 < data.size() && !(data[end] == std::byte{0} && data[end + 1] == std::byte{0})) end += 2;
    if (end + 1 >= data.size()) end = data.size() & ~std::size_t{1};
  } else {
    while (end < data.size() && data[end] != std::byte{0}) ++end;
  }
  if (consumed != nullptr) *consumed = std::min(data.size(), end + (wide ? 2 : 1));
  const ByteView text = data.first(end);
  switch (encoding) {
    case 0: return util::decode_text(text, util::TextEncoding::Latin1).text;
    case 1: {
      if (text.size() >= 2) {
        const auto b0 = std::to_integer<std::uint8_t>(text[0]);
        const auto b1 = std::to_integer<std::uint8_t>(text[1]);
        if (b0 == 0xFE && b1 == 0xFF) return util::decode_text(text.subspan(2), util::TextEncoding::Utf16BE).text;
        if (b0 == 0xFF && b1 == 0xFE) return util::decode_text(text.subspan(2), util::TextEncoding::Utf16LE).text;
      }
      return util::decode_text(text, util::TextEncoding::Utf16LE).text;
    }
    case 2: return util::decode_text(text, util::TextEncoding::Utf16BE).text;
    default: return util::sanitize_utf8(as_chars(text));
  }
}

std::string id3_text_list(ByteView data, int encoding) {
  std::string out;
  while (!data.empty()) {
    std::size_t used = 0;
    const std::string part = id3_string(data, encoding, &used);
    if (!part.empty()) {
      if (!out.empty()) out += "; ";
      out += part;
    }
    if (used == 0) break;
    data = data.subspan(used);
  }
  return out;
}

std::string clean_genre(std::string genre) {
  // "(13)" or "(13)Pop" -> keep the text part; numeric-only references are kept as is.
  if (genre.size() > 2 && genre.front() == '(') {
    const std::size_t close = genre.find(')');
    if (close != std::string::npos && close + 1 < genre.size()) return genre.substr(close + 1);
  }
  return genre;
}

void handle_frame(std::string_view id, ByteView body, TagMap& tags) {
  if (body.empty()) return;
  const int encoding = std::to_integer<int>(body[0]);
  if (encoding > 3) return;
  const ByteView rest = body.subspan(1);
  if (id == "COMM" || id == "COM") {
    if (rest.size() < 3) return;
    std::size_t used = 0;
    const std::string description = id3_string(rest.subspan(3), encoding, &used);
    if (!description.empty()) return;  // only the plain comment
    put(tags, "comment", id3_string(rest.subspan(3 + used), encoding, nullptr));
    return;
  }
  if (id == "TXXX" || id == "TXX") {
    std::size_t used = 0;
    const std::string description = id3_string(rest, encoding, &used);
    if (description.empty() || description.size() > 64) return;
    put(tags, util::to_lower(description), id3_text_list(rest.subspan(used), encoding));
    return;
  }
  for (const Pair& pair : id3_keys) {
    if (pair.a == id && !pair.b.empty()) {
      std::string value = id3_text_list(rest, encoding);
      if (pair.b == "genre") value = clean_genre(std::move(value));
      put(tags, pair.b, value);
      return;
    }
  }
}

}  // namespace

void parse_id3v2(ByteView tag, TagMap& tags) {
  util::ByteReader r(tag);
  if (!r.consume("ID3") || r.remaining() < 7) return;
  const std::uint8_t major = r.u8();
  r.skip(1);
  const std::uint8_t flags = r.u8();
  const std::uint32_t size = syncsafe(r.u32be());
  if (major < 2 || major > 4 || size > r.remaining()) return;
  Bytes body_storage;
  ByteView body = r.bytes(size);
  if ((flags & 0x80) != 0 && major < 4) {
    body_storage = remove_unsync(body);
    body = body_storage;
  }
  util::ByteReader frames(body);
  if ((flags & 0x40) != 0 && major >= 3) {
    const std::uint32_t ext = frames.u32be();
    const std::uint32_t skip = major == 4 ? syncsafe(ext) - 4 : ext;
    if (!frames.skip(skip)) return;
  }
  const std::size_t id_size = major == 2 ? 3 : 4;
  const std::size_t header_size = major == 2 ? 6 : 10;
  while (frames.remaining() >= header_size) {
    const std::string_view id = frames.chars(id_size);
    if (id[0] == '\0') break;  // padding
    std::uint32_t frame_size;
    std::uint16_t frame_flags = 0;
    if (major == 2) {
      frame_size = frames.u24be();
    } else {
      frame_size = frames.u32be();
      if (major == 4) frame_size = syncsafe(frame_size);
      frame_flags = frames.u16be();
    }
    if (frame_size > frames.remaining()) break;
    ByteView frame = frames.bytes(frame_size);
    Bytes storage;
    if (major == 4) {
      if ((frame_flags & 0x000C) != 0) continue;  // compressed or encrypted
      if ((frame_flags & 0x0001) != 0) {
        if (frame.size() < 4) continue;
        frame = frame.subspan(4);  // data length indicator
      }
      if ((frame_flags & 0x0002) != 0) {
        storage = remove_unsync(frame);
        frame = storage;
      }
    } else if (major == 3 && (frame_flags & 0x00C0) != 0) {
      continue;  // compressed or encrypted (v2.3 flag bits)
    }
    handle_frame(id, frame, tags);
  }
}

Bytes id3v2_tag(const TagMap& tags) {
  util::ByteWriter frames;
  const auto syncsafe_bytes = [](util::ByteWriter& w, std::uint32_t v) {
    w.u8(static_cast<std::uint8_t>((v >> 21) & 0x7F));
    w.u8(static_cast<std::uint8_t>((v >> 14) & 0x7F));
    w.u8(static_cast<std::uint8_t>((v >> 7) & 0x7F));
    w.u8(static_cast<std::uint8_t>(v & 0x7F));
  };
  for (const auto& [key, value] : tags) {
    std::string_view id;
    for (const Pair& pair : id3_keys) {
      if (pair.b == key && pair.a.size() == 4 && pair.a != "TYER") {
        id = pair.a;
        break;
      }
    }
    util::ByteWriter body;
    body.u8(3);  // UTF-8
    if (id == "COMM") {
      body.text("und");
      body.u8(0);
      body.text(value);
    } else if (id.empty()) {
      id = "TXXX";
      body.text(key);
      body.u8(0);
      body.text(value);
    } else {
      body.text(value);
    }
    frames.text(id);
    syncsafe_bytes(frames, static_cast<std::uint32_t>(body.size()));
    frames.u16be(0);
    frames.bytes(body.view());
  }
  if (frames.size() == 0) return {};
  util::ByteWriter out;
  out.text("ID3");
  out.u8(4).u8(0).u8(0);
  syncsafe_bytes(out, static_cast<std::uint32_t>(frames.size()));
  out.bytes(frames.view());
  return out.take();
}

// --------------------------------------------------------------------------- Vorbis comments

namespace {
constexpr std::array<Pair, 13> vorbis_keys = {{{"TITLE", "title"},
                                               {"ARTIST", "artist"},
                                               {"ALBUM", "album"},
                                               {"ALBUMARTIST", "albumartist"},
                                               {"DATE", "date"},
                                               {"TRACKNUMBER", "track"},
                                               {"DISCNUMBER", "disc"},
                                               {"GENRE", "genre"},
                                               {"COMMENT", "comment"},
                                               {"DESCRIPTION", "comment"},
                                               {"COMPOSER", "composer"},
                                               {"COPYRIGHT", "copyright"},
                                               {"ENCODER", "encoder"}}};
}  // namespace

std::string vorbis_field(std::string_view key) {
  for (const Pair& pair : vorbis_keys) {
    if (pair.b == key) return std::string(pair.a);
  }
  return util::to_upper(key);
}

std::string key_from_vorbis(std::string_view field) {
  const std::string upper = util::to_upper(field);
  for (const Pair& pair : vorbis_keys) {
    if (pair.a == upper) return std::string(pair.b);
  }
  return util::to_lower(field);
}

bool parse_vorbis_comment(ByteView block, TagMap& tags) {
  util::ByteReader r(block);
  const std::uint32_t vendor = r.u32le();
  if (!r.skip(vendor)) return false;
  const std::uint32_t count = r.u32le();
  if (!r.ok()) return false;
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint32_t length = r.u32le();
    if (!r.ok() || length > r.remaining()) return false;
    const std::string_view entry = r.chars(length);
    const std::size_t eq = entry.find('=');
    if (eq == std::string_view::npos || eq == 0 || eq > 64) continue;
    put(tags, key_from_vorbis(entry.substr(0, eq)), entry.substr(eq + 1));
  }
  return true;
}

// --------------------------------------------------------------------------- APEv2

namespace {
constexpr std::array<Pair, 12> ape_keys = {{{"Title", "title"},
                                            {"Artist", "artist"},
                                            {"Album", "album"},
                                            {"Album Artist", "albumartist"},
                                            {"Year", "date"},
                                            {"Track", "track"},
                                            {"Disc", "disc"},
                                            {"Genre", "genre"},
                                            {"Comment", "comment"},
                                            {"Composer", "composer"},
                                            {"Copyright", "copyright"},
                                            {"Encoder", "encoder"}}};
}  // namespace

std::size_t parse_apev2(ByteView file, TagMap& tags) {
  std::size_t end = file.size();
  if (end >= 128 && as_chars(file.subspan(end - 128, 3)) == "TAG") end -= 128;  // ID3v1 after the APE tag
  if (end < 32) return 0;
  util::ByteReader f(file.subspan(end - 32, 32));
  if (!f.consume("APETAGEX")) return 0;
  const std::uint32_t version = f.u32le();
  const std::uint32_t size = f.u32le();  // items + footer
  const std::uint32_t count = f.u32le();
  const std::uint32_t flags = f.u32le();
  if (!f.ok() || (version != 1000 && version != 2000) || size < 32 || size > end) return 0;
  const bool has_header = (flags & 0x80000000u) != 0;
  const std::size_t total = size + (has_header ? 32u : 0u) + (file.size() - end);
  util::ByteReader r(file.subspan(end - size, size - 32));
  for (std::uint32_t i = 0; i < count && r.remaining() >= 9 && i < 4096; ++i) {
    const std::uint32_t length = r.u32le();
    const std::uint32_t item_flags = r.u32le();
    const std::string_view key = r.cstring();
    if (!r.ok() || length > r.remaining()) break;
    const std::string_view value = r.chars(length);
    if (((item_flags >> 1) & 3u) != 0) continue;  // binary or external
    bool known = false;
    for (const Pair& pair : ape_keys) {
      if (util::iequals(pair.a, key)) {
        put(tags, pair.b, util::replace_all(value, std::string_view("\0", 1), "; "));
        known = true;
      }
    }
    if (!known && key.size() <= 64) put(tags, util::to_lower(key), value);
  }
  return std::min<std::size_t>(total, file.size());
}

Bytes apev2_tag(const TagMap& tags) {
  util::ByteWriter items;
  std::uint32_t count = 0;
  for (const auto& [key, value] : tags) {
    std::string name;
    for (const Pair& pair : ape_keys) {
      if (pair.b == key) name = std::string(pair.a);
    }
    if (name.empty()) {
      if (key.size() < 2 || key.size() > 255) continue;
      name = util::to_upper(key);
    }
    items.u32le(static_cast<std::uint32_t>(value.size()));
    items.u32le(0);
    items.text(name);
    items.u8(0);
    items.text(value);
    ++count;
  }
  if (count == 0) return {};
  const auto block = [&](util::ByteWriter& w, bool header) {
    w.text("APETAGEX");
    w.u32le(2000);
    w.u32le(static_cast<std::uint32_t>(items.size() + 32));
    w.u32le(count);
    w.u32le(0x80000000u | (header ? 0x20000000u : 0u));
    w.zeros(8);
  };
  util::ByteWriter out;
  block(out, true);
  out.bytes(items.view());
  block(out, false);
  return out.take();
}

Bytes vorbis_comment(const TagMap& tags) {
  util::ByteWriter out;
  out.u32le(0);  // empty vendor string: no software identification
  out.u32le(static_cast<std::uint32_t>(tags.size()));
  for (const auto& [key, value] : tags) {
    const std::string entry = vorbis_field(key) + "=" + value;
    out.u32le(static_cast<std::uint32_t>(entry.size()));
    out.text(entry);
  }
  return out.take();
}

}  // namespace cvt::audio::tags
