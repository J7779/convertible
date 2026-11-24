// Convertible core - pdf family: stream filters.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/pdf/filters.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <vector>

#include "families/pdf/bilevel.hpp"
#include "util/deflate.hpp"

namespace cvt::pdf {

namespace {

std::uint8_t byte_at(ByteView data, std::size_t i) { return std::to_integer<std::uint8_t>(data[i]); }

Result<void> grow_check(std::size_t size, std::uint64_t max_output) {
  if (size > max_output) return fail(Status::Limit, "decoded PDF stream exceeds the limit of {} bytes", max_output);
  return {};
}

}  // namespace

Result<Bytes> flate_decode_tolerant(ByteView data, std::uint64_t max_output, bool* damaged) {
  // Skip leading whitespace some producers leave in front of the zlib header.
  std::size_t start = 0;
  while (start < data.size() && start < 4 && (byte_at(data, start) == '\r' || byte_at(data, start) == '\n' ||
                                               byte_at(data, start) == ' ')) {
    ++start;
  }
  data = data.subspan(start);
  bool zlib_header = false;
  if (data.size() >= 2) {
    const unsigned cmf = byte_at(data, 0), flg = byte_at(data, 1);
    zlib_header = (cmf & 0x0F) == 8 && (cmf >> 4) <= 7 && ((cmf << 8) | flg) % 31 == 0 && (flg & 0x20) == 0;
  }
  util::Inflater inflater(zlib_header ? util::Inflater::Format::Zlib : util::Inflater::Format::Raw);
  Bytes out;
  auto progress = inflater.feed(data, out, max_output);
  if (!progress) {
    if (progress.error().code == Status::Limit) return fail(progress.error());
    // Adler mismatch or corrupt tail: keep what was decoded.
    if (damaged != nullptr) *damaged = true;
    if (out.empty() && zlib_header) {
      // Retry as raw deflate after the header (wrong checksum flags etc.).
      util::Inflater raw(util::Inflater::Format::Raw);
      Bytes second;
      auto again = raw.feed(data.subspan(2), second, max_output);
      if (again || !second.empty()) return second;
    }
    if (out.empty()) return fail(progress.error());
    return out;
  }
  if (!progress->finished && damaged != nullptr && !out.empty()) {
    // Truncated stream: tolerated, the data decoded so far is returned.
    *damaged = true;
  }
  return out;
}

Result<Bytes> lzw_decode(ByteView data, bool early_change, std::uint64_t max_output) {
  Bytes out;
  std::vector<std::uint32_t> prefix(4096);
  std::vector<std::uint8_t> suffix(4096), first(4096);
  std::vector<std::uint16_t> length(4096);
  for (std::uint32_t i = 0; i < 256; ++i) {
    prefix[i] = 0xFFFFFFFF;
    suffix[i] = static_cast<std::uint8_t>(i);
    first[i] = static_cast<std::uint8_t>(i);
    length[i] = 1;
  }
  std::uint32_t next_code = 258;
  int code_bits = 9;
  std::uint32_t previous = 0xFFFFFFFF;
  std::uint64_t bit_buffer = 0;
  int bits = 0;
  std::size_t pos = 0;
  std::vector<std::uint8_t> scratch(4096);
  const int early = early_change ? 1 : 0;
  while (true) {
    while (bits < code_bits && pos < data.size()) {
      bit_buffer = (bit_buffer << 8) | byte_at(data, pos++);
      bits += 8;
    }
    if (bits < code_bits) break;
    const auto code = static_cast<std::uint32_t>((bit_buffer >> (bits - code_bits)) & ((1u << code_bits) - 1));
    bits -= code_bits;
    if (code == 257) break;
    if (code == 256) {
      next_code = 258;
      code_bits = 9;
      previous = 0xFFFFFFFF;
      continue;
    }
    std::uint32_t entry = code;
    bool kwkwk = false;
    if (code >= next_code) {
      if (code != next_code || previous == 0xFFFFFFFF) break;  // corrupt: stop with what we have
      entry = previous;
      kwkwk = true;
    }
    // Emit string of `entry`.
    const std::size_t n = length[entry];
    std::uint32_t walk = entry;
    for (std::size_t i = n; i-- > 0;) {
      scratch[i] = suffix[walk];
      walk = prefix[walk];
      if (walk == 0xFFFFFFFF && i > 0) break;
    }
    CVT_TRY(grow_check(out.size() + n + 1, max_output));
    for (std::size_t i = 0; i < n; ++i) out.push_back(static_cast<std::byte>(scratch[i]));
    if (kwkwk) out.push_back(static_cast<std::byte>(first[entry]));
    if (previous != 0xFFFFFFFF && next_code < 4096) {
      prefix[next_code] = previous;
      suffix[next_code] = kwkwk ? first[previous] : first[entry];
      first[next_code] = first[previous];
      length[next_code] = static_cast<std::uint16_t>(std::min<int>(length[previous] + 1, 4095));
      ++next_code;
    }
    previous = code;
    if (next_code + static_cast<std::uint32_t>(early) >= (1u << code_bits) && code_bits < 12) ++code_bits;
  }
  return out;
}

Result<Bytes> ascii85_decode(ByteView data, std::uint64_t max_output) {
  Bytes out;
  std::uint32_t tuple = 0;
  int count = 0;
  std::size_t pos = 0;
  if (data.size() >= 2 && byte_at(data, 0) == '<' && byte_at(data, 1) == '~') pos = 2;
  for (; pos < data.size(); ++pos) {
    const std::uint8_t c = byte_at(data, pos);
    if (c == '~') break;
    if (c == 'z' && count == 0) {
      CVT_TRY(grow_check(out.size() + 4, max_output));
      out.insert(out.end(), 4, std::byte{0});
      continue;
    }
    if (c < '!' || c > 'u') continue;  // whitespace and junk
    tuple = tuple * 85 + static_cast<std::uint32_t>(c - '!');
    if (++count == 5) {
      CVT_TRY(grow_check(out.size() + 4, max_output));
      for (int i = 3; i >= 0; --i) out.push_back(static_cast<std::byte>((tuple >> (8 * i)) & 0xFF));
      tuple = 0;
      count = 0;
    }
  }
  if (count > 1) {
    for (int i = count; i < 5; ++i) tuple = tuple * 85 + 84;
    for (int i = 0; i < count - 1; ++i) out.push_back(static_cast<std::byte>((tuple >> (24 - 8 * i)) & 0xFF));
  }
  return out;
}

Result<Bytes> asciihex_decode(ByteView data, std::uint64_t max_output) {
  Bytes out;
  int high = -1;
  for (std::size_t pos = 0; pos < data.size(); ++pos) {
    const std::uint8_t c = byte_at(data, pos);
    if (c == '>') break;
    int v = -1;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    if (v < 0) continue;
    if (high < 0) {
      high = v;
    } else {
      out.push_back(static_cast<std::byte>(high * 16 + v));
      high = -1;
    }
  }
  if (high >= 0) out.push_back(static_cast<std::byte>(high * 16));
  CVT_TRY(grow_check(out.size(), max_output));
  return out;
}

Result<Bytes> runlength_decode(ByteView data, std::uint64_t max_output) {
  Bytes out;
  std::size_t pos = 0;
  while (pos < data.size()) {
    const std::uint8_t n = byte_at(data, pos++);
    if (n == 128) break;
    if (n < 128) {
      const std::size_t count = std::min<std::size_t>(n + 1u, data.size() - pos);
      CVT_TRY(grow_check(out.size() + count, max_output));
      out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(pos),
                 data.begin() + static_cast<std::ptrdiff_t>(pos + count));
      pos += count;
    } else {
      if (pos >= data.size()) break;
      const std::size_t count = 257u - n;
      CVT_TRY(grow_check(out.size() + count, max_output));
      out.insert(out.end(), count, data[pos++]);
    }
  }
  return out;
}

Result<Bytes> apply_predictor(Bytes data, int predictor, int colors, int bpc, int columns) {
  if (predictor <= 1) return data;
  colors = std::clamp(colors, 1, 32);
  if (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8 && bpc != 16) bpc = 8;
  columns = std::max(columns, 1);
  const std::uint64_t row_bits = static_cast<std::uint64_t>(colors) * static_cast<std::uint64_t>(bpc) *
                                 static_cast<std::uint64_t>(columns);
  if (row_bits > (1ull << 32)) return fail(Status::Decode, "PDF predictor row is too large");
  const auto row_bytes = static_cast<std::size_t>((row_bits + 7) / 8);
  const std::size_t bpp = std::max<std::size_t>(1, static_cast<std::size_t>(colors * bpc) / 8);
  if (predictor == 2) {
    // TIFF predictor 2: horizontal differencing per component.
    const std::size_t rows = data.size() / row_bytes;
    for (std::size_t r = 0; r < rows; ++r) {
      std::byte* row = data.data() + r * row_bytes;
      if (bpc == 8) {
        for (std::size_t i = bpp; i < row_bytes; ++i) {
          row[i] = static_cast<std::byte>(std::to_integer<int>(row[i]) + std::to_integer<int>(row[i - bpp]));
        }
      } else if (bpc == 16) {
        for (std::size_t i = bpp; i + 1 < row_bytes; i += 2) {
          const int prev = (std::to_integer<int>(row[i - bpp]) << 8) | std::to_integer<int>(row[i - bpp + 1]);
          const int cur = (std::to_integer<int>(row[i]) << 8) | std::to_integer<int>(row[i + 1]);
          const int sum = (prev + cur) & 0xFFFF;
          row[i] = static_cast<std::byte>(sum >> 8);
          row[i + 1] = static_cast<std::byte>(sum & 0xFF);
        }
      } else {
        // Sub-byte samples: unpack, accumulate, repack.
        const int mask = (1 << bpc) - 1;
        std::vector<int> prev(static_cast<std::size_t>(colors), 0);
        std::size_t bit = 0;
        for (int x = 0; x < columns; ++x) {
          for (int c = 0; c < colors; ++c) {
            const std::size_t byte = bit / 8;
            const int shift = 8 - bpc - static_cast<int>(bit % 8);
            const int v = (std::to_integer<int>(row[byte]) >> shift) & mask;
            const int s = (v + prev[static_cast<std::size_t>(c)]) & mask;
            prev[static_cast<std::size_t>(c)] = s;
            row[byte] = static_cast<std::byte>((std::to_integer<int>(row[byte]) & ~(mask << shift)) | (s << shift));
            bit += static_cast<std::size_t>(bpc);
          }
        }
      }
    }
    return data;
  }
  // PNG predictors: each row starts with a filter type byte.
  Bytes out;
  const std::size_t stride = row_bytes + 1;
  const std::size_t rows = (data.size() + stride - 1) / stride;
  out.resize(rows * row_bytes);
  std::vector<std::uint8_t> previous(row_bytes, 0);
  std::vector<std::uint8_t> current(row_bytes, 0);
  for (std::size_t r = 0; r < rows; ++r) {
    const std::size_t base = r * stride;
    const int type = base < data.size() ? std::to_integer<int>(data[base]) : 0;
    for (std::size_t i = 0; i < row_bytes; ++i) {
      const std::size_t at = base + 1 + i;
      current[i] = at < data.size() ? std::to_integer<std::uint8_t>(data[at]) : 0;
    }
    for (std::size_t i = 0; i < row_bytes; ++i) {
      const int left = i >= bpp ? current[i - bpp] : 0;
      const int up = previous[i];
      const int upleft = i >= bpp ? previous[i - bpp] : 0;
      int value = current[i];
      switch (type) {
        case 1: value += left; break;
        case 2: value += up; break;
        case 3: value += (left + up) / 2; break;
        case 4: {
          const int p = left + up - upleft;
          const int pa = std::abs(p - left), pb = std::abs(p - up), pc = std::abs(p - upleft);
          value += (pa <= pb && pa <= pc) ? left : (pb <= pc ? up : upleft);
          break;
        }
        default: break;
      }
      current[i] = static_cast<std::uint8_t>(value);
    }
    std::copy(current.begin(), current.end(), reinterpret_cast<std::uint8_t*>(out.data()) + r * row_bytes);
    std::swap(previous, current);
  }
  return out;
}

namespace {

std::string canonical_filter(const std::string& name) {
  if (name == "AHx") return "ASCIIHexDecode";
  if (name == "A85") return "ASCII85Decode";
  if (name == "LZW") return "LZWDecode";
  if (name == "Fl") return "FlateDecode";
  if (name == "RL") return "RunLengthDecode";
  if (name == "CCF") return "CCITTFaxDecode";
  if (name == "DCT") return "DCTDecode";
  return name;
}

}  // namespace

Result<DecodedData> apply_filters(ByteView raw, const Object& filter_in, const Object& params_in,
                                  const FilterEnv& env) {
  const auto resolve = [&](const Object& o) -> Object { return env.resolve ? env.resolve(o) : o; };
  std::vector<std::string> filters;
  std::vector<Object> params;
  const Object filter = resolve(filter_in);
  const Object param = resolve(params_in);
  if (filter.is_name()) {
    filters.push_back(filter.as_name());
    params.push_back(param.is_array() ? (param.as_array().empty() ? Object{} : resolve(param.as_array()[0])) : param);
  } else if (filter.is_array()) {
    const Array& names = filter.as_array();
    for (std::size_t i = 0; i < names.size() && i < 16; ++i) {
      filters.push_back(resolve(names[i]).as_name());
      if (param.is_array() && i < param.as_array().size()) {
        params.push_back(resolve(param.as_array()[i]));
      } else if (param.is_dict() && names.size() == 1) {
        params.push_back(param);
      } else {
        params.push_back(Object{});
      }
    }
  }
  DecodedData result;
  Bytes current(raw.begin(), raw.end());
  for (std::size_t i = 0; i < filters.size(); ++i) {
    const std::string name = canonical_filter(filters[i]);
    const Dict& p = params[i].as_dict();
    if ((name == "DCTDecode" && env.stop_at_dct) || (name == "JPXDecode" && env.stop_at_jpx)) {
      result.pending_filter = name;
      result.params = p;
      if (i + 1 < filters.size()) {
        // Filters after an image codec are not meaningful; ignore them.
      }
      break;
    }
    Result<Bytes> next = Bytes{};
    if (name == "FlateDecode") {
      next = flate_decode_tolerant(current, env.max_output, &result.damaged);
    } else if (name == "LZWDecode") {
      next = lzw_decode(current, resolve(p.get("EarlyChange")).as_int(1) != 0, env.max_output);
    } else if (name == "ASCII85Decode") {
      next = ascii85_decode(current, env.max_output);
    } else if (name == "ASCIIHexDecode") {
      next = asciihex_decode(current, env.max_output);
    } else if (name == "RunLengthDecode") {
      next = runlength_decode(current, env.max_output);
    } else if (name == "CCITTFaxDecode") {
      bilevel::CcittParams cp;
      cp.k = static_cast<int>(resolve(p.get("K")).as_int(0));
      cp.end_of_line = resolve(p.get("EndOfLine")).as_bool(false);
      cp.encoded_byte_align = resolve(p.get("EncodedByteAlign")).as_bool(false);
      cp.columns = static_cast<std::uint32_t>(std::clamp<std::int64_t>(resolve(p.get("Columns")).as_int(1728), 1, 1 << 20));
      cp.rows = static_cast<std::uint32_t>(std::clamp<std::int64_t>(resolve(p.get("Rows")).as_int(0), 0, 1 << 24));
      cp.end_of_block = resolve(p.get("EndOfBlock")).as_bool(true);
      cp.black_is_1 = resolve(p.get("BlackIs1")).as_bool(false);
      cp.damaged_rows_before_error = static_cast<int>(resolve(p.get("DamagedRowsBeforeError")).as_int(0));
      next = bilevel::ccitt_decode(current, cp, env.max_output);
    } else if (name == "JBIG2Decode") {
      Bytes globals;
      const Object g = p.get("JBIG2Globals");
      if (!g.is_null() && env.decode_stream) {
        auto decoded = env.decode_stream(g);
        if (decoded) globals = std::move(*decoded);
      }
      next = bilevel::jbig2_decode(current, globals, env.max_pixels);
    } else if (name == "Crypt") {
      next = std::move(current);
    } else if (name == "DCTDecode" || name == "JPXDecode") {
      return fail(Status::Decode, "image filter {} cannot be applied here", name);
    } else {
      return fail(Status::Decode, "unsupported PDF filter /{}", name);
    }
    if (!next) return fail(next.error());
    current = std::move(*next);
    // Predictors (Flate and LZW).
    if (name == "FlateDecode" || name == "LZWDecode") {
      const int predictor = static_cast<int>(resolve(p.get("Predictor")).as_int(1));
      if (predictor > 1) {
        auto predicted = apply_predictor(std::move(current), predictor,
                                         static_cast<int>(resolve(p.get("Colors")).as_int(1)),
                                         static_cast<int>(resolve(p.get("BitsPerComponent")).as_int(8)),
                                         static_cast<int>(resolve(p.get("Columns")).as_int(1)));
        if (!predicted) return fail(predicted.error());
        current = std::move(*predicted);
      }
    }
    CVT_TRY(grow_check(current.size(), env.max_output));
  }
  result.data = std::move(current);
  return result;
}

Bytes ascii85_encode(ByteView data) {
  Bytes out;
  std::size_t column = 0;
  const auto put = [&](char c) {
    out.push_back(static_cast<std::byte>(c));
    if (++column == 75) {
      out.push_back(std::byte{'\n'});
      column = 0;
    }
  };
  std::size_t i = 0;
  for (; i + 4 <= data.size(); i += 4) {
    const std::uint32_t v = (std::uint32_t{byte_at(data, i)} << 24) | (std::uint32_t{byte_at(data, i + 1)} << 16) |
                            (std::uint32_t{byte_at(data, i + 2)} << 8) | byte_at(data, i + 3);
    if (v == 0) {
      put('z');
      continue;
    }
    char digits[5];
    std::uint32_t t = v;
    for (int k = 4; k >= 0; --k) {
      digits[k] = static_cast<char>('!' + t % 85);
      t /= 85;
    }
    for (const char d : digits) put(d);
  }
  if (i < data.size()) {
    std::uint32_t v = 0;
    const std::size_t rest = data.size() - i;
    for (std::size_t k = 0; k < 4; ++k) v = (v << 8) | (k < rest ? byte_at(data, i + k) : 0);
    char digits[5];
    for (int k = 4; k >= 0; --k) {
      digits[k] = static_cast<char>('!' + v % 85);
      v /= 85;
    }
    for (std::size_t k = 0; k < rest + 1; ++k) put(digits[k]);
  }
  out.push_back(std::byte{'~'});
  out.push_back(std::byte{'>'});
  return out;
}

Bytes runlength_encode(ByteView data) {
  Bytes out;
  std::size_t i = 0;
  while (i < data.size()) {
    std::size_t run = 1;
    while (i + run < data.size() && run < 128 && data[i + run] == data[i]) ++run;
    if (run >= 2) {
      out.push_back(static_cast<std::byte>(257 - run));
      out.push_back(data[i]);
      i += run;
      continue;
    }
    std::size_t literal = 1;
    while (i + literal < data.size() && literal < 128 &&
           !(i + literal + 1 < data.size() && data[i + literal] == data[i + literal + 1])) {
      ++literal;
    }
    out.push_back(static_cast<std::byte>(literal - 1));
    out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(i),
               data.begin() + static_cast<std::ptrdiff_t>(i + literal));
    i += literal;
  }
  out.push_back(std::byte{128});
  return out;
}

Bytes png_predict(ByteView data, int colors, int bpc, int columns) {
  const std::size_t row_bytes = (static_cast<std::size_t>(colors) * static_cast<std::size_t>(bpc) *
                                     static_cast<std::size_t>(columns) + 7) / 8;
  const std::size_t bpp = std::max<std::size_t>(1, static_cast<std::size_t>(colors * bpc) / 8);
  Bytes out;
  if (row_bytes == 0) return out;
  const std::size_t rows = data.size() / row_bytes;
  out.reserve(rows * (row_bytes + 1));
  std::vector<std::uint8_t> prev(row_bytes, 0), filtered(row_bytes), best(row_bytes);
  for (std::size_t r = 0; r < rows; ++r) {
    const std::uint8_t* cur = reinterpret_cast<const std::uint8_t*>(data.data()) + r * row_bytes;
    std::uint64_t best_score = ~0ull;
    int best_type = 0;
    for (int type = 0; type <= 4; ++type) {
      std::uint64_t score = 0;
      for (std::size_t i = 0; i < row_bytes; ++i) {
        const int left = i >= bpp ? cur[i - bpp] : 0;
        const int up = prev[i];
        const int upleft = i >= bpp ? prev[i - bpp] : 0;
        int pred = 0;
        switch (type) {
          case 1: pred = left; break;
          case 2: pred = up; break;
          case 3: pred = (left + up) / 2; break;
          case 4: {
            const int p = left + up - upleft;
            const int pa = std::abs(p - left), pb = std::abs(p - up), pc = std::abs(p - upleft);
            pred = (pa <= pb && pa <= pc) ? left : (pb <= pc ? up : upleft);
            break;
          }
          default: break;
        }
        const auto v = static_cast<std::uint8_t>(cur[i] - pred);
        filtered[i] = v;
        score += static_cast<std::uint64_t>(v < 128 ? v : 256 - v);
      }
      if (score < best_score) {
        best_score = score;
        best_type = type;
        best = filtered;
      }
    }
    out.push_back(static_cast<std::byte>(best_type));
    for (const std::uint8_t v : best) out.push_back(static_cast<std::byte>(v));
    std::copy(cur, cur + row_bytes, prev.begin());
  }
  return out;
}

}  // namespace cvt::pdf
