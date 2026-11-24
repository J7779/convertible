// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "harness.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>

#include "platform/fileio.hpp"
#include "support/test.hpp"
#include "util/hash.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;

namespace e2e {

Config& config() {
  static Config c;
  return c;
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

std::string join(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  return cvd::fileio::join(a, b);
}
bool same_path(const std::string& a, const std::string& b) {
  return cvd::fileio::to_path(a).lexically_normal() == cvd::fileio::to_path(b).lexically_normal();
}
bool exists(const std::string& path) { return cvd::fileio::exists(path); }
bool is_dir(const std::string& path) { return cvd::fileio::is_directory(path); }
void make_dirs(const std::string& path) { cvd::fileio::make_directories(path); }
void remove_all(const std::string& path) { cvd::fileio::remove_all(path); }
std::optional<Bytes> read_bytes(const std::string& path) { return cvd::fileio::read_file(path); }
std::string read_text(const std::string& path) { return cvd::fileio::read_text(path).value_or(std::string()); }
bool write_bytes(const std::string& path, const Bytes& data) {
  return cvd::fileio::write_file(path, data.data(), data.size());
}
bool write_text(const std::string& path, std::string_view text) { return cvd::fileio::write_text(path, text); }

std::vector<std::string> list_dir(const std::string& path) {
  std::vector<std::string> out;
  for (const auto& e : cvd::fileio::list_directory(path)) out.push_back(e.name);
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::string> list_files_recursive(const std::string& path) {
  std::vector<std::string> out;
  std::error_code ec;
  const fs::path base = cvd::fileio::to_path(path);
  if (!fs::is_directory(base, ec)) return out;
  for (fs::recursive_directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->is_regular_file(ec)) out.push_back(cvd::fileio::from_path(it->path().lexically_relative(base)));
  }
  std::sort(out.begin(), out.end());
  return out;
}

Bytes to_bytes(std::string_view s) { return Bytes(s.begin(), s.end()); }
std::string to_string(const Bytes& b) { return std::string(b.begin(), b.end()); }

// ---------------------------------------------------------------------------
// Decoders, encoders, digests
// ---------------------------------------------------------------------------

std::optional<Bytes> base64_decode(std::string_view text) {
  Bytes out;
  std::uint32_t acc = 0;
  int bits = 0;
  bool padding = false;
  for (const char c : text) {
    int v = -1;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+' || c == '-') v = 62;
    else if (c == '/' || c == '_') v = 63;
    else if (c == '=') {
      padding = true;
      continue;
    } else if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      continue;
    } else {
      return std::nullopt;
    }
    if (padding) return std::nullopt;  // data after padding
    acc = (acc << 6) | static_cast<std::uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xFF));
    }
  }
  return out;
}

std::optional<Bytes> hex_decode(std::string_view text) {
  Bytes out;
  int hi = -1;
  for (const char c : text) {
    int v = -1;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == ':' || c == '-') continue;
    else return std::nullopt;
    if (hi < 0) {
      hi = v;
    } else {
      out.push_back(static_cast<std::uint8_t>(hi * 16 + v));
      hi = -1;
    }
  }
  if (hi >= 0) return std::nullopt;
  return out;
}

std::optional<Bytes> utf8_to_utf16le(std::string_view text) {
  Bytes out;
  std::size_t i = 0;
  while (i < text.size()) {
    const auto b0 = static_cast<unsigned char>(text[i]);
    std::uint32_t cp = 0;
    int extra = 0;
    if (b0 < 0x80) cp = b0;
    else if ((b0 & 0xE0) == 0xC0) { cp = b0 & 0x1F; extra = 1; }
    else if ((b0 & 0xF0) == 0xE0) { cp = b0 & 0x0F; extra = 2; }
    else if ((b0 & 0xF8) == 0xF0) { cp = b0 & 0x07; extra = 3; }
    else return std::nullopt;
    if (i + static_cast<std::size_t>(extra) >= text.size() && extra > 0) return std::nullopt;
    for (int k = 1; k <= extra; ++k) {
      const auto b = static_cast<unsigned char>(text[i + static_cast<std::size_t>(k)]);
      if ((b & 0xC0) != 0x80) return std::nullopt;
      cp = (cp << 6) | (b & 0x3F);
    }
    i += static_cast<std::size_t>(extra) + 1;
    const auto put = [&](std::uint32_t u) {
      out.push_back(static_cast<std::uint8_t>(u & 0xFF));
      out.push_back(static_cast<std::uint8_t>((u >> 8) & 0xFF));
    };
    if (cp >= 0x10000) {
      cp -= 0x10000;
      put(0xD800 + (cp >> 10));
      put(0xDC00 + (cp & 0x3FF));
    } else {
      put(cp);
    }
  }
  return out;
}

std::uint32_t crc32(const Bytes& data) {
  std::uint32_t c = 0xFFFFFFFFu;
  for (const std::uint8_t b : data) {
    c ^= b;
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return c ^ 0xFFFFFFFFu;
}

std::string hex_lower(const std::uint8_t* data, std::size_t size) {
  static const char* digits = "0123456789abcdef";
  std::string s;
  for (std::size_t i = 0; i < size; ++i) {
    s += digits[data[i] >> 4];
    s += digits[data[i] & 15];
  }
  return s;
}

namespace {
cvt::ByteView view(const Bytes& b) { return {reinterpret_cast<const std::byte*>(b.data()), b.size()}; }
}  // namespace

std::string md5_hex(const Bytes& data) { return cvt::util::md5_hex(view(data)); }
std::string sha1_hex(const Bytes& data) { return cvt::util::sha1_hex(view(data)); }
std::string sha256_hex(const Bytes& data) { return cvt::util::sha256_hex(view(data)); }

Bytes pseudo_random(std::size_t n, std::uint32_t seed) {
  Bytes out(n);
  std::uint32_t x = seed ? seed : 0x9E3779B9u;
  for (auto& b : out) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    b = static_cast<std::uint8_t>(x >> 24);
  }
  return out;
}

namespace {

void put_be32(Bytes& out, std::uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<std::uint8_t>((v >> s) & 0xFF));
}
void put_le16(Bytes& out, std::uint32_t v) {
  out.push_back(static_cast<std::uint8_t>(v & 0xFF));
  out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}
void put_le32(Bytes& out, std::uint32_t v) {
  for (int s = 0; s < 32; s += 8) out.push_back(static_cast<std::uint8_t>((v >> s) & 0xFF));
}

void png_chunk(Bytes& out, const char* type, const Bytes& data) {
  put_be32(out, static_cast<std::uint32_t>(data.size()));
  Bytes body(type, type + 4);
  body.insert(body.end(), data.begin(), data.end());
  out.insert(out.end(), body.begin(), body.end());
  put_be32(out, crc32(body));
}

}  // namespace

Bytes make_png(int width, int height, std::uint32_t seed) {
  Bytes raw;
  const Bytes noise = pseudo_random(static_cast<std::size_t>(width * height * 3), seed);
  std::size_t k = 0;
  for (int y = 0; y < height; ++y) {
    raw.push_back(0);  // filter: none
    for (int x = 0; x < width; ++x) {
      const int r = (x * 255) / std::max(1, width - 1);
      const int g = (y * 255) / std::max(1, height - 1);
      const int b = 128 + static_cast<int>(100.0 * std::sin((x + y) * 0.15));
      raw.push_back(static_cast<std::uint8_t>(std::clamp(r + (noise[k++] % 24) - 12, 0, 255)));
      raw.push_back(static_cast<std::uint8_t>(std::clamp(g + (noise[k++] % 24) - 12, 0, 255)));
      raw.push_back(static_cast<std::uint8_t>(std::clamp(b + (noise[k++] % 24) - 12, 0, 255)));
    }
  }
  // zlib stream with stored blocks.
  Bytes z = {0x78, 0x01};
  std::size_t pos = 0;
  while (pos < raw.size() || raw.empty()) {
    const std::size_t n = std::min<std::size_t>(65535, raw.size() - pos);
    const bool last = pos + n >= raw.size();
    z.push_back(last ? 1 : 0);
    put_le16(z, static_cast<std::uint32_t>(n));
    put_le16(z, static_cast<std::uint32_t>(~n & 0xFFFF));
    z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos), raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
    pos += n;
    if (last) break;
  }
  std::uint32_t a = 1, b = 0;
  for (const std::uint8_t c : raw) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  put_be32(z, (b << 16) | a);

  Bytes out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  Bytes ihdr;
  put_be32(ihdr, static_cast<std::uint32_t>(width));
  put_be32(ihdr, static_cast<std::uint32_t>(height));
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit RGB
  png_chunk(out, "IHDR", ihdr);
  png_chunk(out, "IDAT", z);
  png_chunk(out, "IEND", {});
  return out;
}

Bytes make_wav(int sample_rate, double seconds, double frequency) {
  const auto n = static_cast<std::uint32_t>(sample_rate * seconds);
  Bytes out;
  const auto tag = [&](const char* t) { out.insert(out.end(), t, t + 4); };
  tag("RIFF");
  put_le32(out, 36 + n * 2);
  tag("WAVE");
  tag("fmt ");
  put_le32(out, 16);
  put_le16(out, 1);  // PCM
  put_le16(out, 1);  // mono
  put_le32(out, static_cast<std::uint32_t>(sample_rate));
  put_le32(out, static_cast<std::uint32_t>(sample_rate * 2));
  put_le16(out, 2);
  put_le16(out, 16);
  tag("data");
  put_le32(out, n * 2);
  for (std::uint32_t i = 0; i < n; ++i) {
    const double v = std::sin(2.0 * 3.14159265358979 * frequency * i / sample_rate) * 0.5;
    put_le16(out, static_cast<std::uint32_t>(static_cast<std::int16_t>(v * 32767.0)) & 0xFFFF);
  }
  return out;
}

std::optional<std::vector<ZipEntry>> parse_zip(const Bytes& d) {
  const auto u16 = [&](std::size_t p) -> std::uint32_t { return d[p] | (d[p + 1] << 8); };
  const auto u32 = [&](std::size_t p) -> std::uint32_t {
    return static_cast<std::uint32_t>(d[p]) | (static_cast<std::uint32_t>(d[p + 1]) << 8) |
           (static_cast<std::uint32_t>(d[p + 2]) << 16) | (static_cast<std::uint32_t>(d[p + 3]) << 24);
  };
  if (d.size() < 22) return std::nullopt;
  // End of central directory: the last "PK\5\6" whose comment fits the file.
  std::size_t eocd = std::string::npos;
  for (std::size_t p = d.size() - 22 + 1; p-- > 0;) {
    if (u32(p) == 0x06054b50u && p + 22 + u16(p + 20) == d.size()) {
      eocd = p;
      break;
    }
    if (d.size() - p > 22 + 65535) break;
  }
  if (eocd == std::string::npos) return std::nullopt;
  const std::uint32_t count = u16(eocd + 10);
  const std::uint32_t cd_size = u32(eocd + 12);
  const std::uint32_t cd_offset = u32(eocd + 16);
  if (static_cast<std::uint64_t>(cd_offset) + cd_size > eocd) return std::nullopt;
  std::vector<ZipEntry> entries;
  std::size_t p = cd_offset;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (p + 46 > d.size() || u32(p) != 0x02014b50u) return std::nullopt;
    ZipEntry e;
    e.method = static_cast<std::uint16_t>(u16(p + 10));
    e.crc = u32(p + 16);
    e.compressed = u32(p + 20);
    e.uncompressed = u32(p + 24);
    const std::uint32_t name_len = u16(p + 28);
    const std::uint32_t extra_len = u16(p + 30);
    const std::uint32_t comment_len = u16(p + 32);
    e.local_offset = u32(p + 42);
    if (p + 46 + name_len > d.size()) return std::nullopt;
    e.name.assign(reinterpret_cast<const char*>(d.data() + p + 46), name_len);
    if (e.local_offset + 30 > d.size() || u32(static_cast<std::size_t>(e.local_offset)) != 0x04034b50u) return std::nullopt;
    const std::uint64_t data_start = e.local_offset + 30 + u16(static_cast<std::size_t>(e.local_offset + 26)) +
                                     u16(static_cast<std::size_t>(e.local_offset + 28));
    if (data_start + e.compressed > cd_offset) return std::nullopt;
    entries.push_back(e);
    p += 46 + name_len + extra_len + comment_len;
  }
  if (p != static_cast<std::size_t>(cd_offset) + cd_size) return std::nullopt;
  return entries;
}

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------

Rgb parse_hex_colour(std::string_view hex) {
  if (!hex.empty() && hex[0] == '#') hex.remove_prefix(1);
  const auto byte = [&](std::size_t i) {
    return static_cast<int>(std::strtol(std::string(hex.substr(i, 2)).c_str(), nullptr, 16));
  };
  if (hex.size() < 6) return {};
  return {byte(0), byte(2), byte(4)};
}

int colour_distance(Rgb a, Rgb b) {
  return std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)});
}

std::optional<Image> load_bmp(const std::string& path) {
  const auto data = read_bytes(path);
  if (!data || data->size() < 54 || (*data)[0] != 'B' || (*data)[1] != 'M') return std::nullopt;
  const Bytes& d = *data;
  const auto u32 = [&](std::size_t p) {
    return static_cast<std::uint32_t>(d[p]) | (static_cast<std::uint32_t>(d[p + 1]) << 8) |
           (static_cast<std::uint32_t>(d[p + 2]) << 16) | (static_cast<std::uint32_t>(d[p + 3]) << 24);
  };
  const std::uint32_t offset = u32(10);
  const auto w = static_cast<std::int32_t>(u32(18));
  const auto h = static_cast<std::int32_t>(u32(22));
  const int bpp = d[28] | (d[29] << 8);
  if (bpp != 32 || w <= 0 || h == 0) return std::nullopt;
  Image img;
  img.width = w;
  img.height = std::abs(h);
  const std::size_t need = static_cast<std::size_t>(offset) + static_cast<std::size_t>(img.width) * img.height * 4;
  if (d.size() < need) return std::nullopt;
  img.px.resize(static_cast<std::size_t>(img.width) * img.height);
  for (int y = 0; y < img.height; ++y) {
    const int src_y = h < 0 ? y : img.height - 1 - y;
    for (int x = 0; x < img.width; ++x) {
      const std::size_t p = offset + (static_cast<std::size_t>(src_y) * img.width + x) * 4;
      img.px[static_cast<std::size_t>(y) * img.width + x] = {d[p + 2], d[p + 1], d[p]};
    }
  }
  return img;
}

Box box_from(const Json& rect, double scale) {
  Box b;
  b.x = static_cast<int>(std::lround(rect[0].as_number() * scale));
  b.y = static_cast<int>(std::lround(rect[1].as_number() * scale));
  b.w = static_cast<int>(std::lround(rect[2].as_number() * scale));
  b.h = static_cast<int>(std::lround(rect[3].as_number() * scale));
  return b;
}

namespace {
Box clamp_box(const Image& img, Box b) {
  const int x0 = std::clamp(b.x, 0, img.width);
  const int y0 = std::clamp(b.y, 0, img.height);
  const int x1 = std::clamp(b.x + b.w, 0, img.width);
  const int y1 = std::clamp(b.y + b.h, 0, img.height);
  return {x0, y0, x1 - x0, y1 - y0};
}
}  // namespace

double ink(const Image& img, Box b, Rgb ref, int tol) {
  b = clamp_box(img, b);
  if (b.w <= 0 || b.h <= 0) return 0.0;
  std::size_t n = 0;
  for (int y = b.y; y < b.y + b.h; ++y) {
    for (int x = b.x; x < b.x + b.w; ++x) n += colour_distance(img.at(x, y), ref) > tol ? 1 : 0;
  }
  return static_cast<double>(n) / static_cast<double>(b.w * b.h);
}

int count_colour(const Image& img, Box b, Rgb c, int tol) {
  b = clamp_box(img, b);
  int n = 0;
  for (int y = b.y; y < b.y + b.h; ++y) {
    for (int x = b.x; x < b.x + b.w; ++x) n += colour_distance(img.at(x, y), c) <= tol ? 1 : 0;
  }
  return n;
}

int diff_pixels(const Image& a, const Image& b, Box box, int tol) {
  if (a.width != b.width || a.height != b.height) return -1;
  box = clamp_box(a, box);
  int n = 0;
  for (int y = box.y; y < box.y + box.h; ++y) {
    for (int x = box.x; x < box.x + box.w; ++x) n += colour_distance(a.at(x, y), b.at(x, y)) > tol ? 1 : 0;
  }
  return n;
}

// ---------------------------------------------------------------------------
// State dumps
// ---------------------------------------------------------------------------

Json load_json(const std::string& path) {
  const auto text = cvd::fileio::read_text(path);
  if (!text) return Json();
  auto j = Json::parse(*text);
  return j ? *j : Json();
}

const Json& at(const Json& root, std::string_view dotted) {
  static const Json null;
  const Json* cur = &root;
  std::size_t start = 0;
  while (start <= dotted.size()) {
    const std::size_t dot = dotted.find('.', start);
    const std::string part(dotted.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start));
    if (!part.empty()) {
      if (cur->is_array()) {
        char* end = nullptr;
        const long idx = std::strtol(part.c_str(), &end, 10);
        if (end == part.c_str() || idx < 0 || static_cast<std::size_t>(idx) >= cur->size()) return null;
        cur = &(*cur)[static_cast<std::size_t>(idx)];
      } else if (cur->is_object() && cur->contains(part)) {
        cur = &(*cur)[part];
      } else {
        return null;
      }
    }
    if (dot == std::string_view::npos) break;
    start = dot + 1;
  }
  return *cur;
}

const Json* node(const Json& state, std::string_view label, std::string_view role) {
  for (const Json& n : state["accessibility"].items()) {
    if (n["label"].str() == label && (role.empty() || n["role"].str() == role)) return &n;
  }
  return nullptr;
}

std::vector<const Json*> nodes_with_role(const Json& state, std::string_view role) {
  std::vector<const Json*> out;
  for (const Json& n : state["accessibility"].items()) {
    if (n["role"].str() == role) out.push_back(&n);
  }
  return out;
}

const Json& tokens() {
  static const Json t = load_json(join(join(config().assets, "brand"), "tokens.json"));
  return t;
}

Rgb token_colour(bool dark, std::string_view key) {
  return parse_hex_colour(tokens()["color"][dark ? "dark" : "light"][key].str());
}

// ---------------------------------------------------------------------------
// Scripts
// ---------------------------------------------------------------------------

std::string json_escape(std::string_view s) {
  const std::string quoted = cvd::json_quote(s);
  return quoted.substr(1, quoted.size() - 2);
}

std::string substitute(std::string text, const Vars& vars) {
  for (const auto& [name, value] : vars) {
    const std::string key = "${" + name + "}";
    const std::string rep = json_escape(value);
    for (std::size_t p = text.find(key); p != std::string::npos; p = text.find(key, p + rep.size())) {
      text.replace(p, key.size(), rep);
    }
  }
  return text;
}

// ---------------------------------------------------------------------------
// Processes
// ---------------------------------------------------------------------------

namespace {

void set_env(const std::string& name, const std::string& value) {
#if defined(_WIN32)
  const std::wstring n = cvd::fileio::to_path(name).wstring();
  const std::wstring v = cvd::fileio::to_path(value).wstring();
  SetEnvironmentVariableW(n.c_str(), v.c_str());
#else
  setenv(name.c_str(), value.c_str(), 1);
#endif
}

#if defined(_WIN32)
std::wstring quote_arg(const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
  std::wstring out = L"\"";
  std::size_t backslashes = 0;
  for (const wchar_t c : arg) {
    if (c == L'\\') {
      ++backslashes;
    } else if (c == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out += c;
      backslashes = 0;
    } else {
      out.append(backslashes, L'\\');
      out += c;
      backslashes = 0;
    }
  }
  out.append(backslashes * 2, L'\\');
  out += L"\"";
  return out;
}

int spawn(const std::vector<std::string>& args, const std::string& console_path) {
  std::wstring cmd;
  for (const auto& a : args) {
    if (!cmd.empty()) cmd += L' ';
    cmd += quote_arg(cvd::fileio::to_path(a).wstring());
  }
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE log = CreateFileW(cvd::fileio::to_path(console_path).wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = log;
  si.hStdError = log;
  PROCESS_INFORMATION pi{};
  const std::wstring app = cvd::fileio::to_path(args.front()).wstring();
  if (!CreateProcessW(app.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    return -1;
  }
  int code = -1;
  if (WaitForSingleObject(pi.hProcess, 300000) == WAIT_OBJECT_0) {
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    code = static_cast<int>(exit_code);
  } else {
    TerminateProcess(pi.hProcess, 124);
    WaitForSingleObject(pi.hProcess, 10000);
    code = 124;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
  return code;
}
#else
std::string quote_arg(const std::string& arg) {
  std::string out = "'";
  for (const char c : arg) {
    if (c == '\'') out += "'\\''";
    else out += c;
  }
  return out + "'";
}

int spawn(const std::vector<std::string>& args, const std::string& console_path) {
  std::string cmd;
  for (const auto& a : args) cmd += quote_arg(a) + " ";
  cmd += "> " + quote_arg(console_path) + " 2>&1";
  const int status = std::system(cmd.c_str());
  if (status == -1) return -1;
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}
#endif

}  // namespace

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

Session::Session(std::string name) : name_(std::move(name)) {
  root_ = join(join(config().work, "runs"), name_);
  remove_all(root_);
  remove_all(join(join(config().work, "failures"), name_));
  for (const char* d : {"in", "out", "config", "tmp", "home"}) make_dirs(join(root_, d));
}

Session::~Session() {
  if (std::uncaught_exceptions() > 0 || had_failure_) {
    // Keep every screenshot of the failed test next to the app's own failure pictures.
    const std::string dest = join(join(config().work, "failures"), name_);
    make_dirs(dest);
    for (const auto& f : list_files_recursive(out())) {
      const std::string ext = f.size() > 4 ? f.substr(f.size() - 4) : f;
      if (ext == ".png" || ext == ".bmp" || ext == "json" || ext == ".log") {
        std::string flat = f;
        std::replace(flat.begin(), flat.end(), '/', '_');
        std::replace(flat.begin(), flat.end(), '\\', '_');
        cvd::fileio::copy_file(join(out(), f), join(dest, flat));
      }
    }
  }
}

std::string Session::in(const std::string& rel) const { return rel.empty() ? join(root_, "in") : join(join(root_, "in"), rel); }
std::string Session::out(const std::string& rel) const {
  return rel.empty() ? join(root_, "out") : join(join(root_, "out"), rel);
}

std::string Session::write_input(const std::string& rel, const Bytes& data) {
  const std::string p = in(rel);
  make_dirs(cvd::fileio::from_path(cvd::fileio::to_path(p).parent_path()));
  write_bytes(p, data);
  return p;
}

std::string Session::write_input(const std::string& rel, std::string_view text) { return write_input(rel, to_bytes(text)); }

std::string Session::fixture(const std::string& name) {
  const auto data = read_bytes(join(join(join(config().e2e_dir, ".."), "tests"), join("fixtures", name)));
  return write_input(name, data.value_or(Bytes{}));
}

Image Session::shot(const std::string& rel) const { return load_bmp(out(rel)).value_or(Image{}); }

void Session::keep_failure(const std::string& path, const std::string& tag) {
  had_failure_ = true;
  const std::string dest = join(join(config().work, "failures"), name_);
  make_dirs(dest);
  if (exists(path)) cvd::fileio::copy_file(path, join(dest, tag + "-" + cvd::fileio::from_path(cvd::fileio::to_path(path).filename())));
}

RunResult Session::run(const std::string& script_name, const Vars& vars, const RunOptions& options) {
  const std::string src = join(join(config().e2e_dir, "scripts"), script_name);
  const std::string text = read_text(src);
  REQUIRE_MESSAGE(!text.empty(), "missing script ", src);
  Vars all = vars;
  all.emplace("IN", in());
  all.emplace("OUT", out());
  all.emplace("WORK", root_);
  const std::string script = join(root_, "script-" + std::to_string(++runs_) + ".json");
  write_text(script, substitute(text, all));
  REQUIRE_MESSAGE(Json::parse(read_text(script)).has_value(), "script is not valid JSON after substitution: ", script_name);
  return launch(script, options);
}

RunResult Session::run_inline(const Json& script, const RunOptions& options) {
  const std::string path = join(root_, "script-" + std::to_string(++runs_) + ".json");
  write_text(path, script.dump(2));
  return launch(path, options);
}

RunResult Session::launch(const std::string& script_path, const RunOptions& o) {
  std::vector<std::string> args = {config().app};
  if (o.headless) args.push_back("--headless");
  if (o.skip_intro) args.push_back("--skip-intro");
  args.insert(args.end(), {"--automation", script_path, "--out", out()});
  args.insert(args.end(), {"--size", std::to_string(o.width) + "x" + std::to_string(o.height)});
  if (o.scale > 0.0) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3g", o.scale);
    args.insert(args.end(), {"--scale", buf});
  }
  if (o.workers > 0) args.insert(args.end(), {"--workers", std::to_string(o.workers)});
  args.insert(args.end(), o.extra_args.begin(), o.extra_args.end());
  args.insert(args.end(), o.files.begin(), o.files.end());

  // Sandbox: settings through the environment variable, temp files and any
  // per-user location inside this test's folders.
  set_env("CONVERTIBLE_CONFIG_DIR", o.config_dir.empty() ? config_dir() : o.config_dir);
  for (const char* v : {"TMPDIR", "TMP", "TEMP"}) set_env(v, temp_dir());
#if defined(_WIN32)
  set_env("APPDATA", join(home_dir(), "AppData"));
  set_env("LOCALAPPDATA", join(home_dir(), "LocalAppData"));
#else
  set_env("HOME", home_dir());
  set_env("XDG_CONFIG_HOME", join(home_dir(), ".config"));
#endif
  remove_all(join(out(), "automation.log"));
  const std::string console = join(root_, "console-" + std::to_string(runs_) + ".txt");
  const auto t0 = std::chrono::steady_clock::now();
  RunResult r;
  r.exit_code = spawn(args, console);
  r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  r.console = read_text(console);
  r.log = read_text(join(out(), "automation.log"));
  const std::size_t p = r.log.rfind("failures ");
  if (p != std::string::npos) r.failures = std::atoi(r.log.c_str() + p + 9);
  if (!o.expect_failures && (r.exit_code != 0 || r.failures != 0)) had_failure_ = true;
  return r;
}

bool Session::ok(const RunResult& r) {
  bool good = true;
  if (r.exit_code != 0 || r.failures != 0) {
    good = false;
    had_failure_ = true;
    FAIL_CHECK("app run failed (exit ", r.exit_code, ", failures ", r.failures, ")\n--- automation.log\n", r.log,
               "--- console\n", r.console);
  }
  // The app deletes its private session folder on exit.
  for (const auto& e : list_dir(temp_dir())) {
    if (e.rfind("convertible-", 0) == 0) {
      good = false;
      FAIL_CHECK("session temp folder left behind: ", e);
    }
  }
  // Nothing was written to the (sandboxed) per-user locations.
  const auto home_files = list_files_recursive(home_dir());
  if (!home_files.empty()) {
    good = false;
    FAIL_CHECK("the app wrote to the user's folders: ", home_files.front());
  }
  return good;
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

bool interactive_role(std::string_view role) {
  return role == "button" || role == "toggle" || role == "slider" || role == "textfield" || role == "tab" ||
         role == "menuitem" || role == "link";
}

std::vector<std::string> layout_problems(const Json& state) {
  std::vector<std::string> problems;
  const double w = state["window"]["width"].as_number();
  const double h = state["window"]["height"].as_number();
  struct R {
    double x, y, w, h;
    std::string label;
    int layer;
  };
  std::vector<R> controls;
  // Controls under a modal sheet (layer 2) are covered on purpose.
  int top_modal = 0;
  for (const Json& n : state["accessibility"].items()) top_modal = std::max(top_modal, n["layer"].as_int(0) == 2 ? 2 : 0);
  const auto describe = [](const Json& n) { return n["role"].str() + " '" + n["label"].str() + "'"; };
  for (const Json& n : state["accessibility"].items()) {
    const std::string& role = n["role"].str();
    if (!interactive_role(role)) continue;
    const Json& rect = n["rect"];
    const Json& full = n.contains("full") ? n["full"] : n["rect"];
    const R r{rect[0].as_number(), rect[1].as_number(), rect[2].as_number(), rect[3].as_number(), describe(n),
              n["layer"].as_int(0)};
    if (r.layer < top_modal) continue;
    if (n["label"].str().empty()) problems.push_back("unlabelled " + role + " at " + rect.dump());
    // Off-window or cut off by the window or a clip that is not a scroll view.
    if (r.x < -0.5 || r.y < -0.5 || r.x + r.w > w + 0.5 || r.y + r.h > h + 0.5) {
      problems.push_back(r.label + " outside the window " + rect.dump());
    }
    if (!n["scrolled"].as_bool() && !(full == rect)) problems.push_back(r.label + " clipped: " + full.dump() + " -> " + rect.dump());
    // Fully scrolled-away controls are fine; visible ones must be usable.
    if (!n["scrolled"].as_bool() && (r.w < 16.0 || r.h < 16.0)) problems.push_back(r.label + " too small " + rect.dump());
    if (!n["scrolled"].as_bool()) controls.push_back(r);
  }
  // Partial overlaps between controls (nesting, e.g. a button inside the drop zone, is fine).
  for (std::size_t i = 0; i < controls.size(); ++i) {
    for (std::size_t j = i + 1; j < controls.size(); ++j) {
      const R& a = controls[i];
      const R& b = controls[j];
      if (a.layer != b.layer) continue;  // popovers and sheets sit above the page
      const double ix = std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x);
      const double iy = std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y);
      if (ix <= 1.0 || iy <= 1.0) continue;
      const bool a_in_b = a.x >= b.x && a.y >= b.y && a.x + a.w <= b.x + b.w && a.y + a.h <= b.y + b.h;
      const bool b_in_a = b.x >= a.x && b.y >= a.y && b.x + b.w <= a.x + a.w && b.y + b.h <= a.y + a.h;
      if (!a_in_b && !b_in_a) problems.push_back(a.label + " overlaps " + b.label);
    }
  }
  return problems;
}

}  // namespace e2e
