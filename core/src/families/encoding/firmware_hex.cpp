// Convertible core - encoding family: firmware image text formats.
//
//   ihex      Intel HEX (record types 00-05; extended linear addresses past 64 KiB)
//   srec      Motorola S-record (S0 header, S1/S2/S3 data, S5/S6 count, S7/S8/S9 end)
//   ti-txt    TI-TXT as used by MSP430 tools ("@ADDR" lines, hex bytes, "q")
//   readmemh  Verilog $readmemh / VMEM hex memory images (words of 1-8 bytes, "@addr")
//
// Readers turn the records into one flat binary image starting at the lowest
// address (like `objcopy -O binary`), filling gaps with the `fill` byte, and
// verify every checksum. Writers take a start address and a record size.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "engine/registry.hpp"
#include "families/encoding/detail.hpp"
#include "families/encoding/encoding.hpp"
#include "families/encoding/simple.hpp"
#include "util/hex.hpp"
#include "util/text.hpp"

namespace cvt::encoding {

namespace {

using detail::byte_at;

/// Data found at addresses, merged into a flat image on demand.
class Image {
 public:
  Result<void> put(std::uint64_t address, ByteView data, std::uint64_t limit) {
    if (data.empty()) return {};
    if (address > 0xFFFF'FFFFull + 1 || address + data.size() > 0x1'0000'0000ull) {
      return fail(Status::Decode, "record address 0x{:X} is beyond the 32-bit address space", address);
    }
    stored_ += data.size();
    if (stored_ > limit) return fail(Status::Limit, "the image holds more than {} bytes", limit);
    segments_.push_back({address, Bytes(data.begin(), data.end())});
    return {};
  }
  [[nodiscard]] bool empty() const { return segments_.empty(); }

  Result<Bytes> flatten(std::uint8_t fill, std::uint64_t limit, std::uint64_t* base, const Context& ctx) {
    if (segments_.empty()) return Bytes{};
    std::uint64_t low = ~std::uint64_t{0}, high = 0;
    for (const auto& [address, bytes] : segments_) {
      low = std::min(low, address);
      high = std::max(high, address + bytes.size());
    }
    if (high - low > limit) {
      return fail(Status::Limit, "the image spans {} bytes (0x{:X}-0x{:X}), more than the limit of {}", high - low, low,
                  high, limit);
    }
    Bytes out(static_cast<std::size_t>(high - low), static_cast<std::byte>(fill));
    std::vector<bool> written(out.size(), false);
    bool overlap = false;
    for (const auto& [address, bytes] : segments_) {
      const auto at = static_cast<std::size_t>(address - low);
      for (std::size_t i = 0; i < bytes.size(); ++i) {
        overlap = overlap || (written[at + i] && out[at + i] != bytes[i]);
        written[at + i] = true;
        out[at + i] = bytes[i];
      }
    }
    if (overlap) ctx.warn("Some addresses are written more than once with different values; the last record wins.");
    if (std::ranges::count(written, false) > 0) {
      ctx.warn(std::format("Gaps between records were filled with 0x{:02X}.", fill));
    }
    if (base != nullptr) *base = low;
    return out;
  }

 private:
  struct Segment {
    std::uint64_t address;
    Bytes bytes;
  };
  std::vector<Segment> segments_;
  std::uint64_t stored_ = 0;
};

/// Hex pairs of a record body; nullopt when a character is not hexadecimal.
std::optional<Bytes> hex_bytes(std::string_view digits) {
  if (digits.size() % 2 != 0) return std::nullopt;
  Bytes out;
  out.reserve(digits.size() / 2);
  for (std::size_t i = 0; i < digits.size(); i += 2) {
    const int high = util::hex_digit_value(digits[i]);
    const int low = util::hex_digit_value(digits[i + 1]);
    if (high < 0 || low < 0) return std::nullopt;
    out.push_back(static_cast<std::byte>(high * 16 + low));
  }
  return out;
}

void put_hex(std::string& out, std::uint8_t value) { detail::append_hex_byte(out, value, true); }

std::uint8_t fill_of(const Options& options) {
  return static_cast<std::uint8_t>(options.get_int("fill", 0xFF, 0, 255));
}

Result<Bytes> finish_image(Image& image, const ReadContext& ctx) {
  std::uint64_t base = 0;
  CVT_TRY_ASSIGN(Bytes out, image.flatten(fill_of(ctx.options), ctx.limits.max_decompressed_bytes, &base, ctx));
  if (base != 0) ctx.warn(std::format("The image starts at address 0x{:X}; the output begins with that address.", base));
  return out;
}

// ---------------------------------------------------------------------------- Intel HEX

std::string ihex_record(std::uint8_t type, std::uint16_t address, ByteView data) {
  std::string line = ":";
  std::uint32_t sum = static_cast<std::uint32_t>(data.size()) + (address >> 8) + (address & 0xFFu) + type;
  put_hex(line, static_cast<std::uint8_t>(data.size()));
  put_hex(line, static_cast<std::uint8_t>(address >> 8));
  put_hex(line, static_cast<std::uint8_t>(address & 0xFF));
  put_hex(line, type);
  for (const std::byte b : data) {
    put_hex(line, std::to_integer<std::uint8_t>(b));
    sum += std::to_integer<std::uint32_t>(b);
  }
  put_hex(line, static_cast<std::uint8_t>((0x100u - (sum & 0xFFu)) & 0xFFu));
  line.push_back('\n');
  return line;
}

Result<std::string> ihex_encode(ByteView data, std::uint64_t start, std::size_t record_size) {
  if (start + data.size() > 0x1'0000'0000ull) return fail(Status::Encode, "Intel HEX addresses are limited to 4 GiB");
  std::string out;
  std::uint64_t upper = 0;  // implicit until the first extended address record (as objcopy writes it)
  for (std::size_t at = 0; at < data.size();) {
    const std::uint64_t address = start + at;
    if ((address >> 16) != upper) {
      upper = address >> 16;
      const std::array<std::byte, 2> segment = {static_cast<std::byte>(upper >> 8), static_cast<std::byte>(upper & 0xFF)};
      out += ihex_record(4, 0, segment);
    }
    // A record never crosses a 64 KiB boundary.
    const std::size_t room = static_cast<std::size_t>(0x10000 - (address & 0xFFFF));
    const std::size_t n = std::min({record_size, data.size() - at, room});
    out += ihex_record(0, static_cast<std::uint16_t>(address & 0xFFFF), data.subspan(at, n));
    at += n;
  }
  out += ":00000001FF\n";
  return out;
}

Result<Bytes> ihex_decode(std::string_view text, const ReadContext& ctx) {
  Image image;
  std::uint64_t base = 0;
  bool ended = false;
  std::size_t number = 0;
  for (const std::string_view raw : util::split_lines(text)) {
    ++number;
    const std::string_view line = util::trim(raw);
    if (line.empty()) continue;
    if (ended) break;
    if (line[0] != ':') return fail(Status::Decode, "line {} is not an Intel HEX record", number);
    const auto bytes = hex_bytes(line.substr(1));
    if (!bytes || bytes->size() < 5) return fail(Status::Decode, "malformed Intel HEX record on line {}", number);
    const ByteView record = *bytes;
    const std::size_t length = byte_at(record, 0);
    if (record.size() != length + 5) return fail(Status::Decode, "Intel HEX record length mismatch on line {}", number);
    std::uint32_t sum = 0;
    for (const std::byte b : record) sum += std::to_integer<std::uint32_t>(b);
    if ((sum & 0xFFu) != 0) return fail(Status::Decode, "Intel HEX checksum error on line {}", number);
    const std::uint32_t offset = (std::uint32_t{byte_at(record, 1)} << 8) | byte_at(record, 2);
    const std::uint8_t type = byte_at(record, 3);
    const ByteView payload = record.subspan(4, length);
    switch (type) {
      case 0x00: CVT_TRY(image.put(base + offset, payload, ctx.limits.max_decompressed_bytes)); break;
      case 0x01: ended = true; break;
      case 0x02:  // extended segment address: paragraph number
        if (length != 2) return fail(Status::Decode, "bad extended segment record on line {}", number);
        base = ((std::uint64_t{byte_at(payload, 0)} << 8) | byte_at(payload, 1)) << 4;
        break;
      case 0x04:  // extended linear address: upper 16 bits
        if (length != 2) return fail(Status::Decode, "bad extended linear address record on line {}", number);
        base = ((std::uint64_t{byte_at(payload, 0)} << 8) | byte_at(payload, 1)) << 16;
        break;
      case 0x03:
      case 0x05: break;  // start addresses: not part of the image
      default: return fail(Status::Decode, "unknown Intel HEX record type {:02X} on line {}", type, number);
    }
  }
  if (!ended) ctx.warn("The Intel HEX file has no end-of-file record; it may be truncated.");
  return finish_image(image, ctx);
}

int sniff_ihex(ByteView data, std::string_view filename) {
  const std::string_view text = as_chars(data).substr(0, 512);
  const std::string_view line = util::trim(text.substr(0, text.find('\n')));
  if (line.size() < 11 || line[0] != ':') return 0;
  const auto bytes = hex_bytes(line.substr(1));
  if (!bytes || bytes->size() != std::to_integer<std::size_t>((*bytes)[0]) + 5) return 0;
  std::uint32_t sum = 0;
  for (const std::byte b : *bytes) sum += std::to_integer<std::uint32_t>(b);
  if ((sum & 0xFF) != 0) return 0;
  (void)filename;
  return 92;
}

// ---------------------------------------------------------------------------- Motorola S-record

std::string srec_record(char type, std::size_t address_bytes, std::uint64_t address, ByteView data) {
  std::string line = "S";
  line.push_back(type);
  const std::size_t count = address_bytes + data.size() + 1;
  std::uint32_t sum = static_cast<std::uint32_t>(count);
  put_hex(line, static_cast<std::uint8_t>(count));
  for (std::size_t i = address_bytes; i-- > 0;) {
    const auto b = static_cast<std::uint8_t>(address >> (8 * i));
    put_hex(line, b);
    sum += b;
  }
  for (const std::byte b : data) {
    put_hex(line, std::to_integer<std::uint8_t>(b));
    sum += std::to_integer<std::uint32_t>(b);
  }
  put_hex(line, static_cast<std::uint8_t>(~sum & 0xFFu));
  line.push_back('\n');
  return line;
}

Result<std::string> srec_encode(ByteView data, std::uint64_t start, std::size_t record_size, std::string_view header) {
  const std::uint64_t end = start + data.size();
  if (end > 0x1'0000'0000ull) return fail(Status::Encode, "S-record addresses are limited to 4 GiB");
  const std::size_t address_bytes = end <= 0x10000 ? 2 : end <= 0x1000000 ? 3 : 4;
  const char data_type = static_cast<char>('0' + address_bytes - 1);  // S1, S2, S3
  const char end_type = static_cast<char>('0' + 11 - address_bytes);   // S9, S8, S7
  record_size = std::min<std::size_t>(record_size, 255 - address_bytes - 1);
  std::string out = srec_record('0', 2, 0, as_bytes(header));
  std::size_t records = 0;
  for (std::size_t at = 0; at < data.size(); at += record_size, ++records) {
    out += srec_record(data_type, address_bytes, start + at, data.subspan(at, std::min(record_size, data.size() - at)));
  }
  if (records <= 0xFFFF) {
    out += srec_record('5', 2, records, {});
  } else {
    out += srec_record('6', 3, records, {});
  }
  out += srec_record(end_type, address_bytes, start, {});
  return out;
}

Result<Bytes> srec_decode(std::string_view text, const ReadContext& ctx) {
  Image image;
  std::size_t number = 0;
  for (const std::string_view raw : util::split_lines(text)) {
    ++number;
    const std::string_view line = util::trim(raw);
    if (line.empty()) continue;
    if (line.size() < 4 || (line[0] != 'S' && line[0] != 's') || !util::is_ascii_digit(line[1])) {
      return fail(Status::Decode, "line {} is not an S-record", number);
    }
    const auto bytes = hex_bytes(line.substr(2));
    if (!bytes || bytes->empty() || bytes->size() != std::to_integer<std::size_t>((*bytes)[0]) + 1) {
      return fail(Status::Decode, "malformed S-record on line {}", number);
    }
    std::uint32_t sum = 0;
    for (const std::byte b : *bytes) sum += std::to_integer<std::uint32_t>(b);
    if ((sum & 0xFFu) != 0xFFu) return fail(Status::Decode, "S-record checksum error on line {}", number);
    const char type = line[1];
    std::size_t address_bytes = 0;
    switch (type) {
      case '1': case '9': case '0': case '5': address_bytes = 2; break;
      case '2': case '8': case '6': address_bytes = 3; break;
      case '3': case '7': address_bytes = 4; break;
      default: return fail(Status::Decode, "unknown S-record type S{} on line {}", type, number);
    }
    const ByteView record = *bytes;
    if (record.size() < address_bytes + 2) return fail(Status::Decode, "short S-record on line {}", number);
    std::uint64_t address = 0;
    for (std::size_t i = 0; i < address_bytes; ++i) address = (address << 8) | byte_at(record, 1 + i);
    if (type >= '1' && type <= '3') {
      const ByteView payload = record.subspan(1 + address_bytes, record.size() - address_bytes - 2);
      CVT_TRY(image.put(address, payload, ctx.limits.max_decompressed_bytes));
    }
    if (type >= '7' && type <= '9') break;
  }
  return finish_image(image, ctx);
}

int sniff_srec(ByteView data, std::string_view) {
  const std::string_view text = as_chars(data).substr(0, 512);
  const std::string_view line = util::trim(text.substr(0, text.find('\n')));
  if (line.size() < 10 || line[0] != 'S' || line[1] < '0' || line[1] > '9') return 0;
  const auto bytes = hex_bytes(line.substr(2));
  if (!bytes || bytes->size() != std::to_integer<std::size_t>((*bytes)[0]) + 1) return 0;
  std::uint32_t sum = 0;
  for (const std::byte b : *bytes) sum += std::to_integer<std::uint32_t>(b);
  return (sum & 0xFF) == 0xFF ? 92 : 0;
}

// ---------------------------------------------------------------------------- TI-TXT

std::string titxt_encode(ByteView data, std::uint64_t start, std::size_t per_line) {
  std::string out = std::format("@{:04X}\n", start);
  for (std::size_t at = 0; at < data.size(); at += per_line) {
    const std::size_t n = std::min(per_line, data.size() - at);
    for (std::size_t k = 0; k < n; ++k) {
      if (k > 0) out.push_back(' ');
      put_hex(out, byte_at(data, at + k));
    }
    out.push_back('\n');
  }
  out += "q\n";
  return out;
}

Result<Bytes> titxt_decode(std::string_view text, const ReadContext& ctx) {
  Image image;
  std::uint64_t address = 0;
  Bytes run;
  std::uint64_t run_start = 0;
  const auto flush = [&]() -> Result<void> {
    CVT_TRY(image.put(run_start, run, ctx.limits.max_decompressed_bytes));
    run.clear();
    return {};
  };
  std::size_t number = 0;
  for (const std::string_view raw : util::split_lines(text)) {
    ++number;
    const std::string_view line = util::trim(raw);
    if (line.empty()) continue;
    if (line[0] == 'q' || line[0] == 'Q') break;
    if (line[0] == '@') {
      CVT_TRY(flush());
      const auto value = util::parse_int(line.substr(1), 16);
      if (!value || *value < 0) return fail(Status::Decode, "bad TI-TXT address on line {}", number);
      address = static_cast<std::uint64_t>(*value);
      run_start = address;
      continue;
    }
    for (const std::string_view token : util::split(line, ' ', false)) {
      const auto byte = hex_bytes(util::trim(token));
      if (!byte || byte->size() != 1) return fail(Status::Decode, "bad TI-TXT byte '{}' on line {}", token, number);
      run.push_back((*byte)[0]);
      ++address;
    }
  }
  CVT_TRY(flush());
  return finish_image(image, ctx);
}

// ---------------------------------------------------------------------------- $readmemh

std::string readmemh_encode(ByteView data, std::uint64_t start_word, std::size_t word_bytes, bool big_endian,
                            std::size_t words_per_line) {
  std::string out;
  if (start_word != 0) out += std::format("@{:X}\n", start_word);
  std::size_t on_line = 0;
  for (std::size_t at = 0; at < data.size(); at += word_bytes) {
    std::string word;
    for (std::size_t k = 0; k < word_bytes; ++k) {
      const std::size_t index = big_endian ? at + k : at + word_bytes - 1 - k;
      put_hex(word, index < data.size() ? byte_at(data, index) : 0);
    }
    if (on_line > 0) out.push_back(' ');
    out += word;
    if (++on_line == words_per_line) {
      out.push_back('\n');
      on_line = 0;
    }
  }
  if (on_line > 0) out.push_back('\n');
  return out;
}

Result<Bytes> readmemh_decode(std::string_view text, const ReadContext& ctx) {
  // Strip comments first.
  std::string clean;
  clean.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text.substr(i, 2) == "//") {
      while (i < text.size() && text[i] != '\n') ++i;
    } else if (text.substr(i, 2) == "/*") {
      const std::size_t end = text.find("*/", i + 2);
      i = end == std::string_view::npos ? text.size() : end + 1;
      clean.push_back(' ');
      continue;
    }
    if (i < text.size()) clean.push_back(text[i]);
  }
  std::vector<std::string_view> tokens;
  std::size_t widest = 0;
  for (std::size_t i = 0; i < clean.size();) {
    while (i < clean.size() && util::is_ascii_space(clean[i])) ++i;
    std::size_t end = i;
    while (end < clean.size() && !util::is_ascii_space(clean[end])) ++end;
    if (end > i) {
      const std::string_view token = std::string_view(clean).substr(i, end - i);
      tokens.push_back(token);
      if (token[0] != '@') widest = std::max(widest, token.size() - static_cast<std::size_t>(std::ranges::count(token, '_')));
    }
    i = end;
  }
  std::size_t word_bytes = static_cast<std::size_t>(ctx.options.get_int("word_bytes", 0, 0, 8));
  if (word_bytes == 0) word_bytes = std::max<std::size_t>(1, (widest + 1) / 2);
  if (word_bytes > 8) return fail(Status::Decode, "memory words wider than 64 bits are not supported");
  const bool big_endian = ctx.options.get_bool("big_endian", true);
  Image image;
  std::uint64_t word_address = 0;
  Bytes run;
  std::uint64_t run_start = 0;
  for (const std::string_view token : tokens) {
    if (token[0] == '@') {
      CVT_TRY(image.put(run_start, run, ctx.limits.max_decompressed_bytes));
      run.clear();
      const auto value = util::parse_int(token.substr(1), 16);
      if (!value || *value < 0 || static_cast<std::uint64_t>(*value) > 0xFFFF'FFFFull) {
        return fail(Status::Decode, "bad $readmemh address '{}'", token);
      }
      word_address = static_cast<std::uint64_t>(*value);
      run_start = word_address * word_bytes;
      continue;
    }
    std::uint64_t value = 0;
    std::size_t digits = 0;
    for (const char c : token) {
      if (c == '_') continue;
      const int d = util::hex_digit_value(c);
      if (d < 0) return fail(Status::Decode, "bad $readmemh word '{}'", token);  // x/z values cannot become bytes
      value = (value << 4) | static_cast<std::uint64_t>(d);
      ++digits;
    }
    if (digits > word_bytes * 2) return fail(Status::Decode, "word '{}' is wider than {} bytes", token, word_bytes);
    for (std::size_t k = 0; k < word_bytes; ++k) {
      const std::size_t shift = big_endian ? (word_bytes - 1 - k) * 8 : k * 8;
      run.push_back(static_cast<std::byte>((value >> shift) & 0xFF));
    }
    ++word_address;
    if (run.size() > ctx.limits.max_decompressed_bytes) return fail(Status::Limit, "the memory image is too large");
  }
  CVT_TRY(image.put(run_start, run, ctx.limits.max_decompressed_bytes));
  return finish_image(image, ctx);
}

OptionSpec line_ending_option() {
  return OptionSpec::choice("line_ending", "Line ending", "lf", {"lf", "crlf"},
                            "Line terminator: lf (Unix) or crlf (what many Windows programmers expect).")
      .advanced();
}

std::vector<OptionSpec> writer_options(int default_record, int max_record) {
  return {OptionSpec::integer("address", "Start address", 0, 0, 0xFFFF'FFFFLL, "Address of the first byte."),
          OptionSpec::integer("record_size", "Bytes per record", default_record, 1, max_record,
                              "Data bytes in each record line."),
          line_ending_option()};
}

/// Applies the line_ending option to text written with '\n'.
Result<std::string> with_line_endings(Result<std::string> text, const Options& options) {
  if (!text || options.get_enum("line_ending", "lf", {"lf", "crlf"}) == "lf") return text;
  return util::convert_line_endings(*text, util::LineEnding::CrLf);
}
OptionSpec fill_option() {
  return OptionSpec::integer("fill", "Gap fill byte", 0xFF, 0, 255,
                             "Byte value written where the records leave gaps (0xFF = erased flash).");
}

}  // namespace

void register_firmware_hex(Registry& registry) {
  add_binary_encoding(registry, {
      .id = "ihex", .name = "Intel HEX", .description = "Firmware image as Intel HEX records with addresses and checksums.",
      .extensions = {"hex", "ihex", "ihx", "a43", "a90"}, .mime = "text/x-hex", .popularity = 35,
      .encode = [](const ir::Blob& blob, const WriteContext& ctx) {
        return with_line_endings(
            ihex_encode(blob.bytes, static_cast<std::uint64_t>(ctx.options.get_int("address", 0, 0, 0xFFFF'FFFFLL)),
                        static_cast<std::size_t>(ctx.options.get_int("record_size", 16, 1, 255))),
            ctx.options);
      },
      .decode = ihex_decode,
      .encode_options = writer_options(16, 255),
      .decode_options = {fill_option()},
      .expansion = 4,
      .sniff = sniff_ihex,
  });
  add_binary_encoding(registry, {
      .id = "srec", .name = "Motorola S-record",
      .description = "Firmware image as Motorola S-records (S19/S28/S37) with addresses and checksums.",
      .extensions = {"srec", "s19", "s28", "s37", "mot", "s3", "sx"}, .mime = "text/x-srecord", .popularity = 25,
      .encode = [](const ir::Blob& blob, const WriteContext& ctx) {
        return with_line_endings(
            srec_encode(blob.bytes, static_cast<std::uint64_t>(ctx.options.get_int("address", 0, 0, 0xFFFF'FFFFLL)),
                        static_cast<std::size_t>(ctx.options.get_int("record_size", 32, 1, 250)),
                        ctx.options.get_string("header", "")),
            ctx.options);
      },
      .decode = srec_decode,
      .encode_options = [] {
        std::vector<OptionSpec> specs = writer_options(32, 250);
        specs.push_back(OptionSpec::text("header", "Header text", "", "Text stored in the S0 header record.").advanced());
        return specs;
      }(),
      .decode_options = {fill_option()},
      .expansion = 4,
      .sniff = sniff_srec,
  });
  add_binary_encoding(registry, {
      .id = "ti-txt", .name = "TI-TXT", .description = "Texas Instruments TI-TXT firmware image (MSP430 programmers).",
      .extensions = {"txt", "ti-txt"}, .popularity = 10,
      .encode = [](const ir::Blob& blob, const WriteContext& ctx) -> Result<std::string> {
        return with_line_endings(
            titxt_encode(blob.bytes, static_cast<std::uint64_t>(ctx.options.get_int("address", 0, 0, 0xFFFF'FFFFLL)),
                         static_cast<std::size_t>(ctx.options.get_int("record_size", 16, 1, 64))),
            ctx.options);
      },
      .decode = titxt_decode,
      .encode_options = writer_options(16, 64),
      .decode_options = {fill_option()},
      .expansion = 4,
      .sniff = [](ByteView data, std::string_view) {
        const std::string_view text = util::trim_left(as_chars(data).substr(0, 256));
        if (!text.starts_with('@')) return 0;
        const std::size_t nl = text.find('\n');
        if (nl == std::string_view::npos || nl < 2) return 0;
        for (const char c : util::trim(text.substr(1, nl - 1))) {
          if (util::hex_digit_value(c) < 0) return 0;
        }
        const std::string_view second = text.substr(nl + 1, 3);
        return second.size() == 3 && util::hex_digit_value(second[0]) >= 0 && util::hex_digit_value(second[1]) >= 0 &&
                       second[2] == ' '
                   ? 88
                   : 0;
      },
  });
  add_binary_encoding(registry, {
      .id = "readmemh", .name = "Verilog $readmemh memory image",
      .description = "Hexadecimal memory words for Verilog $readmemh / VMEM, one word per entry, with @address jumps.",
      .extensions = {"mem", "memh", "vmem"}, .popularity = 10,
      .encode = [](const ir::Blob& blob, const WriteContext& ctx) -> Result<std::string> {
        const auto word = static_cast<std::size_t>(ctx.options.get_int("word_bytes", 1, 1, 8));
        if (blob.bytes.size() % word != 0) ctx.warn("The last word was padded with zero bytes.");
        return readmemh_encode(blob.bytes, static_cast<std::uint64_t>(ctx.options.get_int("address", 0, 0, 0xFFFF'FFFFLL)),
                               word, ctx.options.get_bool("big_endian", true),
                               static_cast<std::size_t>(ctx.options.get_int("words_per_line", 1, 1, 64)));
      },
      .decode = readmemh_decode,
      .encode_options = {OptionSpec::integer("word_bytes", "Word size", 1, 1, 8, "Bytes per memory word."),
                         OptionSpec::boolean("big_endian", "Big-endian words", true,
                                             "The first byte of each word is its most significant byte."),
                         OptionSpec::integer("words_per_line", "Words per line", 1, 1, 64, "Memory words on each line."),
                         OptionSpec::integer("address", "Start address", 0, 0, 0xFFFF'FFFFLL,
                                             "Word address of the first word (written as @address).")},
      .decode_options = {OptionSpec::integer("word_bytes", "Word size", 0, 0, 8,
                                             "Bytes per memory word; 0 = from the widest word in the file."),
                         OptionSpec::boolean("big_endian", "Big-endian words", true,
                                             "The first byte of each word is its most significant byte."),
                         fill_option()},
      .expansion = 4,
      .sniff = [](ByteView data, std::string_view filename) {
        const bool extension = util::iends_with(filename, ".mem") || util::iends_with(filename, ".memh") ||
                               util::iends_with(filename, ".vmem");
        const std::string_view text = as_chars(data).substr(0, 512);
        const bool shaped = !text.empty() && std::ranges::all_of(text, [](char c) {
          return util::hex_digit_value(c) >= 0 || util::is_ascii_space(c) || c == '@' || c == '_' || c == '/' ||
                 c == '*' || (c >= 0x20 && c < 0x7F);
        });
        return extension && shaped ? 85 : 0;
      },
  });
}

}  // namespace cvt::encoding
