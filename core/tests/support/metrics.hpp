// Quality metrics and synthetic inputs for format tests.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
//
// Everything here is a pure, deterministic function of its arguments: no
// I/O, no global state, no clocks, explicit seeds.
//
// Quick reference (thresholds are STARTING POINTS; tighten them once a codec
// is stable, and never loosen one without understanding why it fails):
//
//   kind       metric                         identical   typical assertion
//   ---------  -----------------------------  ----------  ---------------------------------------
//   image      pixels_equal(a, b)             true        lossless formats (PNG, BMP, QOI, ...)
//              psnr(a, b)  [dB]               +inf        JPEG q90 >= 35, q50 >= 30
//              ssim(a, b)  [0..1]             1           JPEG q90 >= 0.95, q50 >= 0.90
//              max_abs_diff(a, b)  [0..255]   0           colour-space round trips <= 2
//   audio      snr_db(ref, test)  [dB]        +inf        16-bit PCM >= 90, 8-bit PCM >= 40
//              aligned_snr_db(ref, test, n)   +inf        MP3/AAC/Vorbis/Opus at q90 >= 20
//              correlation(a, b)  [-1..1]     1           lossy codecs >= 0.99
//   video      psnr(frame, frame)  [dB]       +inf        visually lossless >= 40, q50 >= 30
//   text       text_similarity(a, b)  [0..1]  1           document round trips >= 0.98
//   data       data_equal(a, b, tol)          true        JSON/YAML/TOML/... round trips
//   table      tables_equal(a, b, tol)        true        CSV/XLSX/ODS round trips
//   mesh       meshes_close(a, b, tol)        true        text formats: tol = 1e-4 * bbox size
//
// A metric that cannot be computed (sizes, formats, sample rates or channel
// counts differ, or an input is invalid) returns a quiet NaN. NaN compares
// false with everything, so `CHECK(psnr(a, b) >= 35.0)` fails as it should;
// always write the assertion with the metric on the left of `>=` / `<=`.
//
// The *_diff functions return "" when the values match and otherwise a short
// description of the first difference, made for failure messages:
//
//   CHECK_MESSAGE(tables_equal(a, b), table_diff(a, b));
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <map>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ir/audio.hpp"
#include "ir/document.hpp"
#include "ir/image.hpp"
#include "ir/mesh.hpp"
#include "ir/table.hpp"
#include "ir/video.hpp"
#include "support/test_support.hpp"
#include "util/bytes.hpp"
#include "util/json.hpp"
#include "util/text.hpp"

namespace cvt::test {

namespace detail {

inline constexpr double not_a_number = std::numeric_limits<double>::quiet_NaN();
inline constexpr double infinity = std::numeric_limits<double>::infinity();

/// 10 * log10(peak^2 / (sum_squared_error / count)); +inf when the error is 0.
[[nodiscard]] inline double psnr_from_error(double sum_squared_error, double count, double peak) {
  if (!(count > 0.0)) return not_a_number;
  if (!(sum_squared_error > 0.0)) return infinity;
  return 10.0 * std::log10(peak * peak * count / sum_squared_error);
}

/// Absolute-or-relative comparison used by data_equal and tables_equal.
[[nodiscard]] inline bool numbers_close(double a, double b, double tolerance) {
  if (a == b) return true;
  if (std::isnan(a) && std::isnan(b)) return true;
  if (!(tolerance > 0.0) || !std::isfinite(a) || !std::isfinite(b)) return false;
  const double difference = std::abs(a - b);
  return difference <= tolerance || difference <= tolerance * std::max(std::abs(a), std::abs(b));
}

/// `text` cut to at most `limit` bytes on a UTF-8 boundary, with "..." when cut.
[[nodiscard]] inline std::string abbreviated(std::string_view text, std::size_t limit = 60) {
  if (text.size() <= limit) return std::string(text);
  std::size_t cut = limit;
  while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u) --cut;
  return std::string(text.substr(0, cut)) + "...";
}

// ---- images ---------------------------------------------------------------

/// Accumulated alpha-aware error of RGBA pixels, in units of 1/255 of an
/// 8-bit step (so that everything stays an exact integer).
struct RgbaError {
  std::uint64_t sum_squared = 0;  ///< sum over R, G, B (premultiplied) and A of difference^2, times 255^2
  std::uint32_t max_abs = 0;      ///< largest absolute difference, times 255
};

inline void accumulate_rgba(const std::uint8_t* a, const std::uint8_t* b, std::size_t pixels, RgbaError& error) {
  for (std::size_t i = 0; i < pixels * 4; i += 4) {
    const std::int32_t alpha_a = a[i + 3];
    const std::int32_t alpha_b = b[i + 3];
    for (std::size_t c = 0; c < 4; ++c) {
      // Colour: value * alpha / 255, kept as value * alpha. Alpha: scaled by 255 to share the unit.
      const std::int32_t difference =
          c < 3 ? a[i + c] * alpha_a - b[i + c] * alpha_b : (alpha_a - alpha_b) * 255;
      const auto magnitude = static_cast<std::uint32_t>(difference < 0 ? -difference : difference);
      error.sum_squared += static_cast<std::uint64_t>(magnitude) * magnitude;
      error.max_abs = std::max(error.max_abs, magnitude);
    }
  }
}

[[nodiscard]] inline double rgba_psnr(const RgbaError& error, std::uint64_t pixels) {
  return psnr_from_error(static_cast<double>(error.sum_squared) / (255.0 * 255.0), static_cast<double>(pixels) * 4.0,
                         255.0);
}

[[nodiscard]] inline bool comparable(const ir::Frame& a, const ir::Frame& b) noexcept {
  return a.valid() && b.valid() && a.width == b.width && a.height == b.height;
}

[[nodiscard]] inline RgbaError frame_error(const ir::Frame& a, const ir::Frame& b) {
  RgbaError error;
  accumulate_rgba(a.rgba.data(), b.rgba.data(), a.rgba.size() / 4, error);
  return error;
}

/// 0 at position 0 rising linearly to 255 at position `count - 1` (0 when `count` is 1).
[[nodiscard]] constexpr std::uint8_t ramp(std::uint64_t position, std::uint64_t count) noexcept {
  return count > 1 ? static_cast<std::uint8_t>(position * 255u / (count - 1)) : std::uint8_t{0};
}

/// Top-left coordinates of the SSIM windows along one axis: every `stride`
/// pixels, plus one window flush with the far edge so no pixel is left out.
[[nodiscard]] inline std::vector<std::size_t> window_origins(std::size_t size, std::size_t window,
                                                             std::size_t stride) {
  std::vector<std::size_t> origins;
  for (std::size_t origin = 0; origin + window <= size; origin += stride) origins.push_back(origin);
  if (!origins.empty() && origins.back() + window != size) origins.push_back(size - window);
  return origins;
}

/// Mean SSIM of two equally sized single-channel planes with values 0..255.
[[nodiscard]] inline double mean_ssim(const std::vector<float>& x, const std::vector<float>& y, std::size_t width,
                                      std::size_t height) {
  constexpr double c1 = (0.01 * 255.0) * (0.01 * 255.0);
  constexpr double c2 = (0.03 * 255.0) * (0.03 * 255.0);
  const std::size_t window_w = std::min<std::size_t>(width, 8);
  const std::size_t window_h = std::min<std::size_t>(height, 8);
  const std::vector<std::size_t> xs = window_origins(width, window_w, 4);
  const std::vector<std::size_t> ys = window_origins(height, window_h, 4);
  const double n = static_cast<double>(window_w * window_h);
  double total = 0.0;
  for (const std::size_t oy : ys) {
    for (const std::size_t ox : xs) {
      double sum_x = 0.0, sum_y = 0.0, sum_xx = 0.0, sum_yy = 0.0, sum_d = 0.0, sum_dd = 0.0;
      for (std::size_t row = 0; row < window_h; ++row) {
        const float* px = x.data() + (oy + row) * width + ox;
        const float* py = y.data() + (oy + row) * width + ox;
        for (std::size_t column = 0; column < window_w; ++column) {
          const double vx = px[column];
          const double vy = py[column];
          const double d = vx - vy;
          sum_x += vx;
          sum_y += vy;
          sum_xx += vx * vx;
          sum_yy += vy * vy;
          sum_d += d;
          sum_dd += d * d;
        }
      }
      const double mean_x = sum_x / n;
      const double mean_y = sum_y / n;
      const double mean_d = sum_d / n;
      const double var_x = sum_xx / n - mean_x * mean_x;
      const double var_y = sum_yy / n - mean_y * mean_y;
      // var(x - y) = var_x + var_y - 2 cov(x, y), so the usual
      //   (2 mx my + C1)(2 cov + C2) / ((mx^2 + my^2 + C1)(var_x + var_y + C2))
      // equals the product below; this form is exactly 1 for identical windows.
      const double var_d = std::max(0.0, sum_dd / n - mean_d * mean_d);
      const double luminance = 1.0 - (mean_d * mean_d) / (mean_x * mean_x + mean_y * mean_y + c1);
      const double structure = 1.0 - var_d / (std::max(0.0, var_x) + std::max(0.0, var_y) + c2);
      total += luminance * structure;
    }
  }
  return total / static_cast<double>(xs.size() * ys.size());
}

/// Worst value of a per-frame metric over two images; NaN when the frame
/// counts differ, there are no frames, or any frame pair gives NaN.
template <class Metric>
[[nodiscard]] double worst_over_frames(const ir::Image& a, const ir::Image& b, bool lower_is_worse, Metric metric) {
  if (a.frames.size() != b.frames.size() || a.frames.empty()) return not_a_number;
  double worst = lower_is_worse ? infinity : -infinity;
  for (std::size_t i = 0; i < a.frames.size(); ++i) {
    const double value = metric(a.frames[i], b.frames[i]);
    if (std::isnan(value)) return not_a_number;
    worst = lower_is_worse ? std::min(worst, value) : std::max(worst, value);
  }
  return worst;
}

// ---- audio ----------------------------------------------------------------

[[nodiscard]] inline bool comparable(const ir::Audio& a, const ir::Audio& b) noexcept {
  return a.valid() && b.valid() && a.sample_rate == b.sample_rate && a.channels == b.channels;
}

[[nodiscard]] inline std::vector<double> mono_mixdown(const ir::Audio& audio) {
  std::vector<double> mono(audio.frame_count());
  for (std::size_t frame = 0; frame < mono.size(); ++frame) {
    double sum = 0.0;
    for (std::size_t channel = 0; channel < audio.channels; ++channel) {
      sum += audio.samples[frame * audio.channels + channel];
    }
    mono[frame] = sum / audio.channels;
  }
  return mono;
}

/// In-place radix-2 FFT (inverse when `inverse`; the inverse is not scaled).
/// The size must be a power of two.
inline void fft(std::vector<double>& re, std::vector<double>& im, bool inverse) {
  const std::size_t n = re.size();
  for (std::size_t i = 1, j = 0; i < n; ++i) {
    std::size_t bit = n >> 1;
    for (; (j & bit) != 0; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      std::swap(re[i], re[j]);
      std::swap(im[i], im[j]);
    }
  }
  std::vector<double> cosine(n / 2), sine(n / 2);
  for (std::size_t k = 0; k < n / 2; ++k) {
    const double angle = (inverse ? 2.0 : -2.0) * std::numbers::pi * static_cast<double>(k) / static_cast<double>(n);
    cosine[k] = std::cos(angle);
    sine[k] = std::sin(angle);
  }
  // Raw pointers keep unoptimised (sanitizer) builds fast.
  double* const r = re.data();
  double* const m = im.data();
  const double* const c = cosine.data();
  const double* const s = sine.data();
  for (std::size_t length = 2; length <= n; length <<= 1) {
    const std::size_t half = length / 2;
    const std::size_t step = n / length;
    for (std::size_t start = 0; start < n; start += length) {
      for (std::size_t k = 0; k < half; ++k) {
        const double wr = c[k * step];
        const double wi = s[k * step];
        const std::size_t lo = start + k;
        const std::size_t hi = lo + half;
        const double vr = r[hi] * wr - m[hi] * wi;
        const double vi = r[hi] * wi + m[hi] * wr;
        r[hi] = r[lo] - vr;
        m[hi] = m[lo] - vi;
        r[lo] += vr;
        m[lo] += vi;
      }
    }
  }
}

/// Range of reference frames [first, last) that overlap `test` when the test
/// signal is delayed by `shift` frames (reference[i] pairs with test[i + shift]).
struct Overlap {
  std::size_t first = 0;
  std::size_t last = 0;
  [[nodiscard]] std::size_t size() const noexcept { return last > first ? last - first : 0; }
};
[[nodiscard]] inline Overlap overlap(std::size_t reference_frames, std::size_t test_frames, std::ptrdiff_t shift) {
  const auto ref = static_cast<std::ptrdiff_t>(reference_frames);
  const auto tst = static_cast<std::ptrdiff_t>(test_frames);
  const std::ptrdiff_t first = std::max<std::ptrdiff_t>(0, -shift);
  const std::ptrdiff_t last = std::min(ref, tst - shift);
  if (last <= first) return {};
  return {static_cast<std::size_t>(first), static_cast<std::size_t>(last)};
}

// ---- video ----------------------------------------------------------------

/// One stored sample of a plane row: a byte, or a little-endian 16-bit word.
[[nodiscard]] inline std::uint32_t load_sample(const std::uint8_t* row, std::size_t index, bool wide) noexcept {
  if (!wide) return row[index];
  return static_cast<std::uint32_t>(row[index * 2]) | (static_cast<std::uint32_t>(row[index * 2 + 1]) << 8);
}
inline void store_sample(std::uint8_t* row, std::size_t index, bool wide, std::uint32_t value) noexcept {
  if (!wide) {
    row[index] = static_cast<std::uint8_t>(value);
  } else {
    row[index * 2] = static_cast<std::uint8_t>(value & 0xFFu);
    row[index * 2 + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
  }
}

/// Triangle wave 0, 1, ..., 255, 254, ..., 1, 0, ... (period 510).
[[nodiscard]] constexpr std::uint32_t triangle_wave(std::uint64_t position) noexcept {
  const auto phase = static_cast<std::uint32_t>(position % 510u);
  return phase <= 255u ? phase : 510u - phase;
}

// ---- text -----------------------------------------------------------------

/// Unicode White_Space code points.
[[nodiscard]] constexpr bool is_unicode_space(char32_t c) noexcept {
  return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0x85 || c == 0xA0 || c == 0x1680 ||
         (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

[[nodiscard]] inline std::u32string normalized_code_points(std::string_view text) {
  const std::u32string decoded = util::utf8_to_utf32(text);
  std::u32string out;
  out.reserve(decoded.size());
  bool pending_space = false;
  for (std::size_t i = 0; i < decoded.size(); ++i) {
    const char32_t c = decoded[i];
    if (i == 0 && c == 0xFEFF) continue;  // byte order mark
    if (is_unicode_space(c)) {
      pending_space = !out.empty();
      continue;
    }
    if (pending_space) out.push_back(U' ');
    pending_space = false;
    out.push_back(c);
  }
  return out;
}

/// Levenshtein distance (insert, delete, substitute; each costs 1) in
/// O(|a| * |b|) time and O(min(|a|, |b|)) memory.
template <class T>
[[nodiscard]] std::size_t levenshtein(std::span<const T> a, std::span<const T> b) {
  // A common prefix or suffix never changes the distance.
  while (!a.empty() && !b.empty() && a.front() == b.front()) {
    a = a.subspan(1);
    b = b.subspan(1);
  }
  while (!a.empty() && !b.empty() && a.back() == b.back()) {
    a = a.first(a.size() - 1);
    b = b.first(b.size() - 1);
  }
  if (a.size() < b.size()) std::swap(a, b);
  if (b.empty()) return a.size();
  std::vector<std::size_t> row(b.size() + 1);
  for (std::size_t j = 0; j <= b.size(); ++j) row[j] = j;
  // Raw pointers keep unoptimised (sanitizer) builds fast.
  std::size_t* const cost = row.data();
  const T* const shorter = b.data();
  const std::size_t width = b.size();
  for (std::size_t i = 1; i <= a.size(); ++i) {
    const T& item = a[i - 1];
    std::size_t diagonal = cost[0];
    cost[0] = i;
    for (std::size_t j = 1; j <= width; ++j) {
      const std::size_t above = cost[j];
      std::size_t best = diagonal + (item == shorter[j - 1] ? 0u : 1u);
      if (above + 1 < best) best = above + 1;
      if (cost[j - 1] + 1 < best) best = cost[j - 1] + 1;
      cost[j] = best;
      diagonal = above;
    }
  }
  return cost[width];
}

/// Replace each space-separated word by a number, equal words getting equal numbers.
inline void append_token_ids(std::u32string_view text, std::map<std::u32string_view, std::uint32_t>& ids,
                             std::vector<std::uint32_t>& out) {
  std::size_t start = 0;
  while (start < text.size()) {
    std::size_t end = text.find(U' ', start);
    if (end == std::u32string_view::npos) end = text.size();
    if (end > start) {
      const auto next_id = static_cast<std::uint32_t>(ids.size());
      out.push_back(ids.try_emplace(text.substr(start, end - start), next_id).first->second);
    }
    start = end + 1;
  }
}

// ---- data -----------------------------------------------------------------

[[nodiscard]] inline std::string json_summary(const Json& value) { return abbreviated(util::dump_json(value)); }

[[nodiscard]] inline bool json_numbers_equal(const Json& a, const Json& b, double tolerance) {
  if (a.is_integer() && b.is_integer()) {
    // Compare integers as integers: doubles cannot hold every 64-bit value.
    // (Json equality is by mathematical value across Int64 and UInt64.)
    if (a == b) return true;
    if (!(tolerance > 0.0)) return false;
  }
  return numbers_close(a.as_double(), b.as_double(), tolerance);
}

/// RFC 6901 escaping of one JSON-pointer reference token.
inline void append_pointer_token(std::string& path, std::string_view key) {
  path.push_back('/');
  for (const char c : key) {
    if (c == '~') {
      path.append("~0");
    } else if (c == '/') {
      path.append("~1");
    } else {
      path.push_back(c);
    }
  }
}

[[nodiscard]] inline std::string data_difference(const Json& a, const Json& b, double tolerance, std::string& path) {
  const auto at = [&path](std::string_view what) {
    return std::format("{}: {}", path.empty() ? std::string_view("(root)") : std::string_view(path), what);
  };
  if (a.is_number() && b.is_number()) {
    if (json_numbers_equal(a, b, tolerance)) return {};
    return at(std::format("{} vs {}", json_summary(a), json_summary(b)));
  }
  if (a.type() != b.type()) {
    return at(std::format("{} {} vs {} {}", a.type_name(), json_summary(a), b.type_name(), json_summary(b)));
  }
  if (a.is_object()) {
    const std::size_t length = path.size();
    for (const auto& member : a.items()) {
      path.resize(length);
      append_pointer_token(path, member.key());
      const Json* const other = b.find(member.key());
      if (other == nullptr) return at("only in the first value");
      std::string difference = data_difference(member.value(), *other, tolerance, path);
      if (!difference.empty()) return difference;
    }
    for (const auto& member : b.items()) {
      if (a.contains(member.key())) continue;
      path.resize(length);
      append_pointer_token(path, member.key());
      return at("only in the second value");
    }
    path.resize(length);
    return {};
  }
  if (a.is_array()) {
    const std::size_t length = path.size();
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
      path.resize(length);
      path.append(std::format("/{}", i));
      std::string difference = data_difference(a[i], b[i], tolerance, path);
      if (!difference.empty()) return difference;
    }
    path.resize(length);
    if (a.size() != b.size()) return at(std::format("array length {} vs {}", a.size(), b.size()));
    return {};
  }
  if (a == b) return {};
  return at(std::format("{} vs {}", json_summary(a), json_summary(b)));
}

// ---- tables ---------------------------------------------------------------

[[nodiscard]] inline std::size_t significant_cells(const std::vector<ir::Cell>& row) noexcept {
  std::size_t count = row.size();
  while (count > 0 && row[count - 1].empty()) --count;
  return count;
}
[[nodiscard]] inline std::size_t significant_rows(const ir::Sheet& sheet) noexcept {
  std::size_t count = sheet.rows.size();
  while (count > 0 && significant_cells(sheet.rows[count - 1]) == 0) --count;
  return count;
}

[[nodiscard]] inline std::string cell_summary(const ir::Cell& cell) {
  switch (cell.type) {
    case ir::CellType::Empty: return "Empty";
    case ir::CellType::String: return std::format("String \"{}\"", abbreviated(cell.text));
    case ir::CellType::Number: return std::format("Number {}", cell.number);
    case ir::CellType::Bool: return std::format("Bool {}", cell.number);
    case ir::CellType::DateTime: return std::format("DateTime {}", cell.number);
  }
  return "?";
}

[[nodiscard]] inline bool cells_equal(const ir::Cell& a, const ir::Cell& b, double tolerance) {
  if (a.type != b.type) return false;
  switch (a.type) {
    case ir::CellType::Empty: return true;
    case ir::CellType::String: return a.text == b.text;
    case ir::CellType::Number:
    case ir::CellType::Bool:
    case ir::CellType::DateTime: return numbers_close(a.number, b.number, tolerance);
  }
  return false;
}

// ---- meshes ---------------------------------------------------------------

[[nodiscard]] inline bool is_finite(const ir::Vec3& v) noexcept {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

/// Index of the first point of `from` with no point of `to` within
/// `tolerance` (Euclidean), or `from.size()` when every point has one.
[[nodiscard]] inline std::size_t first_unmatched_point(const std::vector<ir::Vec3>& from,
                                                       const std::vector<ir::Vec3>& to, double tolerance) {
  std::vector<ir::Vec3> sorted;
  sorted.reserve(to.size());
  for (const ir::Vec3& point : to) {
    if (is_finite(point)) sorted.push_back(point);
  }
  std::sort(sorted.begin(), sorted.end(), [](const ir::Vec3& l, const ir::Vec3& r) { return l.x < r.x; });
  for (std::size_t i = 0; i < from.size(); ++i) {
    const ir::Vec3& point = from[i];
    if (!is_finite(point)) return i;
    const double low = static_cast<double>(point.x) - tolerance;
    const double high = static_cast<double>(point.x) + tolerance;
    auto candidate = std::lower_bound(sorted.begin(), sorted.end(), low,
                                      [](const ir::Vec3& element, double value) { return element.x < value; });
    bool matched = false;
    for (; candidate != sorted.end() && candidate->x <= high; ++candidate) {
      const double dx = static_cast<double>(candidate->x) - point.x;
      const double dy = static_cast<double>(candidate->y) - point.y;
      const double dz = static_cast<double>(candidate->z) - point.z;
      if (dx * dx + dy * dy + dz * dz <= tolerance * tolerance) {
        matched = true;
        break;
      }
    }
    if (!matched) return i;
  }
  return from.size();
}

}  // namespace detail

// ===========================================================================
// Images (ir::Frame / ir::Image: 8-bit sRGB RGBA, straight alpha)
// ===========================================================================
//
// All image metrics are ALPHA-AWARE: colours are compared after multiplying
// by alpha (c * a / 255, not rounded), and alpha is compared as a fourth
// channel. A colour difference under a fully transparent pixel therefore
// does not count, and one under a half-transparent pixel counts half; an
// alpha difference always counts. For opaque images this is the plain
// per-channel comparison. `Frame::delay_ms` and all Image metadata are
// ignored; compare those separately.
//
// "Invalid" below means !Frame::valid() (zero size or a buffer that does not
// match the dimensions).

/// True when both frames have the same width and height (valid or not).
[[nodiscard]] inline bool same_size(const ir::Frame& a, const ir::Frame& b) noexcept {
  return a.width == b.width && a.height == b.height;
}

/// Peak signal-to-noise ratio in dB over the four channels premultiplied R,
/// G, B and A, each weighted equally: 10 * log10(255^2 / MSE) with the MSE
/// taken over all `4 * width * height` values.
///
/// Range: 0 .. +infinity, higher is better. Identical frames (see the
/// alpha-aware definition above) give +infinity. Frames of different size,
/// or an invalid frame, give NaN.
///
/// Reference points: every value off by 1 = 48.13 dB; one channel off by 1
/// everywhere = 54.15 dB. Starting thresholds: JPEG/WebP/AVIF q90 >= 35 dB,
/// q50 >= 30 dB; 256-colour palette formats on photos >= 28 dB; lossless
/// formats: use pixels_equal instead.
[[nodiscard]] inline double psnr(const ir::Frame& a, const ir::Frame& b) {
  if (!detail::comparable(a, b)) return detail::not_a_number;
  return detail::rgba_psnr(detail::frame_error(a, b), a.rgba.size() / 4);
}

/// Largest absolute difference of any channel of any pixel, in 8-bit steps
/// (0 .. 255), alpha-aware as described above (so the result can be
/// fractional under partial transparency). Identical = 0. Different sizes
/// or an invalid frame give NaN.
///
/// Starting thresholds: lossless = 0 (or use pixels_equal); integer colour
/// conversions and rounding differences between decoders <= 2.
[[nodiscard]] inline double max_abs_diff(const ir::Frame& a, const ir::Frame& b) {
  if (!detail::comparable(a, b)) return detail::not_a_number;
  return static_cast<double>(detail::frame_error(a, b).max_abs) / 255.0;
}

/// True when both frames are valid, have the same size and show the same
/// picture: every pixel has the same alpha and, unless that alpha is 0, the
/// same R, G and B. (Encoders may legitimately discard the colour of fully
/// transparent pixels; to insist on byte equality compare `a.rgba == b.rgba`.)
/// This is THE assertion for lossless formats.
[[nodiscard]] inline bool pixels_equal(const ir::Frame& a, const ir::Frame& b) {
  if (!detail::comparable(a, b)) return false;
  return detail::frame_error(a, b).max_abs == 0;
}

/// Mean structural similarity (SSIM, Wang et al. 2004), a perceptual score
/// that tolerates small uniform errors but punishes blur, blocking and
/// ringing. Range: 1 = identical, about 0 = unrelated (slightly negative
/// values are possible for inverted structure). Different sizes or an
/// invalid frame give NaN.
///
/// Exactly what is computed:
///   - one plane per frame: BT.601 luma of the premultiplied colour,
///     (0.299 R + 0.587 G + 0.114 B) * A / 255, range 0..255 (the picture
///     composited over black);
///   - 8x8 windows (uniform weights, population variance) every 4 pixels in
///     both directions, plus a last window flush with the right/bottom edge
///     so every pixel is covered; a dimension shorter than 8 pixels uses
///     the whole dimension as the window size;
///   - C1 = (0.01 * 255)^2, C2 = (0.03 * 255)^2; the result is the mean
///     over all windows;
///   - when either frame has any transparency, the same computation is
///     repeated on the alpha plane and the LOWER of the two means is
///     returned, so a wrong alpha channel cannot hide behind black pixels.
///
/// Starting thresholds: JPEG/WebP/AVIF q90 >= 0.95, q50 >= 0.90, q10 >= 0.70.
[[nodiscard]] inline double ssim(const ir::Frame& a, const ir::Frame& b) {
  if (!detail::comparable(a, b)) return detail::not_a_number;
  const std::size_t pixels = a.rgba.size() / 4;
  const auto luma_plane = [pixels](const ir::Frame& frame) {
    std::vector<float> plane(pixels);
    for (std::size_t i = 0; i < pixels; ++i) {
      const std::uint8_t* p = frame.rgba.data() + i * 4;
      plane[i] = static_cast<float>((0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]) * p[3] / 255.0);
    }
    return plane;
  };
  double result = detail::mean_ssim(luma_plane(a), luma_plane(b), a.width, a.height);
  if (a.has_transparency() || b.has_transparency()) {
    const auto alpha_plane = [pixels](const ir::Frame& frame) {
      std::vector<float> plane(pixels);
      for (std::size_t i = 0; i < pixels; ++i) plane[i] = static_cast<float>(frame.rgba[i * 4 + 3]);
      return plane;
    };
    result = std::min(result, detail::mean_ssim(alpha_plane(a), alpha_plane(b), a.width, a.height));
  }
  return result;
}

/// Image overloads: compare frame by frame and return the WORST frame's
/// value (lowest PSNR / SSIM, highest max_abs_diff). NaN (false for
/// pixels_equal) when the frame counts differ, an image has no frames, or
/// any frame pair is not comparable. Thresholds as for the Frame versions.
[[nodiscard]] inline double psnr(const ir::Image& a, const ir::Image& b) {
  return detail::worst_over_frames(a, b, true, [](const ir::Frame& x, const ir::Frame& y) { return psnr(x, y); });
}
[[nodiscard]] inline double ssim(const ir::Image& a, const ir::Image& b) {
  return detail::worst_over_frames(a, b, true, [](const ir::Frame& x, const ir::Frame& y) { return ssim(x, y); });
}
[[nodiscard]] inline double max_abs_diff(const ir::Image& a, const ir::Image& b) {
  return detail::worst_over_frames(a, b, false,
                                   [](const ir::Frame& x, const ir::Frame& y) { return max_abs_diff(x, y); });
}
[[nodiscard]] inline bool pixels_equal(const ir::Image& a, const ir::Image& b) {
  if (a.frames.size() != b.frames.size() || a.frames.empty()) return false;
  for (std::size_t i = 0; i < a.frames.size(); ++i) {
    if (!pixels_equal(a.frames[i], b.frames[i])) return false;
  }
  return true;
}

/// A smooth opaque colour gradient, the easy case for every lossy codec:
/// R rises left to right and G top to bottom (0 at the first pixel, 255 at
/// the last), B falls along the diagonal (255 top-left, 0 bottom-right).
/// Integer arithmetic only: identical on every platform. `width` and
/// `height` must be > 0.
[[nodiscard]] inline ir::Frame gradient_frame(std::uint32_t width, std::uint32_t height) {
  ir::Frame frame = ir::Frame::filled(width, height, {0, 0, 0, 255});
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      std::uint8_t* p = frame.rgba.data() + frame.offset(x, y);
      p[0] = detail::ramp(x, width);
      p[1] = detail::ramp(y, height);
      p[2] = static_cast<std::uint8_t>(255 - detail::ramp(static_cast<std::uint64_t>(x) + y,
                                                          static_cast<std::uint64_t>(width) + height - 1));
    }
  }
  return frame;
}

/// An opaque black and white checkerboard of `cell` x `cell` pixel squares
/// (0 is treated as 1), black at the top-left: hard edges, the difficult
/// case for transform codecs and the easy one for palette formats.
[[nodiscard]] inline ir::Frame checker_frame(std::uint32_t width, std::uint32_t height, std::uint32_t cell) {
  ir::Frame frame = ir::Frame::filled(width, height, {0, 0, 0, 255});
  const std::uint32_t size = cell == 0 ? 1 : cell;
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      if ((x / size + y / size) % 2 == 0) continue;
      std::uint8_t* p = frame.rgba.data() + frame.offset(x, y);
      p[0] = p[1] = p[2] = 255;
    }
  }
  return frame;
}

/// Opaque uniform noise: R, G and B of every pixel are independent
/// pseudo-random bytes from `random_bytes(width * height * 3, seed)`.
/// Incompressible, so it is the worst case for lossy codecs (expect low
/// PSNR) and a good check that lossless ones really are lossless. The same
/// seed gives the same frame on every platform.
[[nodiscard]] inline ir::Frame noise_frame(std::uint32_t width, std::uint32_t height, std::uint32_t seed) {
  ir::Frame frame = ir::Frame::filled(width, height, {0, 0, 0, 255});
  const Bytes noise = random_bytes(static_cast<std::size_t>(width) * height * 3, seed);
  for (std::size_t i = 0; i < noise.size() / 3; ++i) {
    frame.rgba[i * 4] = std::to_integer<std::uint8_t>(noise[i * 3]);
    frame.rgba[i * 4 + 1] = std::to_integer<std::uint8_t>(noise[i * 3 + 1]);
    frame.rgba[i * 4 + 2] = std::to_integer<std::uint8_t>(noise[i * 3 + 2]);
  }
  return frame;
}

/// The colours of gradient_frame with alpha rising along the diagonal from
/// 0 (top-left, fully transparent) to 255 (bottom-right, opaque), for
/// testing alpha channels. A 1x1 frame is opaque.
[[nodiscard]] inline ir::Frame alpha_gradient_frame(std::uint32_t width, std::uint32_t height) {
  ir::Frame frame = gradient_frame(width, height);
  if (width == 1 && height == 1) return frame;
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      frame.rgba[frame.offset(x, y) + 3] =
          detail::ramp(static_cast<std::uint64_t>(x) + y, static_cast<std::uint64_t>(width) + height - 1);
    }
  }
  return frame;
}

// ===========================================================================
// Audio (ir::Audio: interleaved float samples, nominal range -1..+1)
// ===========================================================================
//
// Two signals are comparable when both are valid() and have the same sample
// rate and channel count; otherwise the metrics return NaN (best_alignment:
// 0). Tags are ignored.
//
// rms_error, snr_db and correlation compare sample i with sample i. When the
// lengths differ, the shorter signal is treated as if followed by silence,
// so missing or extra audio counts as error. Lossy codecs shift and pad the
// signal (encoder delay): use aligned_snr_db for those.

/// Root-mean-square difference over all samples of all channels, in sample
/// units (1.0 = full scale). Identical = 0. Reference points: 16-bit
/// quantisation about 1e-5, 8-bit about 1.1e-3. Starting threshold for PCM
/// round trips: <= 2 / 2^bits.
[[nodiscard]] inline double rms_error(const ir::Audio& a, const ir::Audio& b) {
  if (!detail::comparable(a, b)) return detail::not_a_number;
  const std::size_t count = std::max(a.samples.size(), b.samples.size());
  if (count == 0) return 0.0;
  double sum = 0.0;
  for (std::size_t i = 0; i < count; ++i) {
    const double x = i < a.samples.size() ? a.samples[i] : 0.0f;
    const double y = i < b.samples.size() ? b.samples[i] : 0.0f;
    sum += (x - y) * (x - y);
  }
  return std::sqrt(sum / static_cast<double>(count));
}

/// Signal-to-noise ratio in dB: 10 * log10(power of `reference` / power of
/// (reference - test)), over all samples of all channels. Higher is better;
/// identical = +infinity; 0 dB means the error is as loud as the signal;
/// a silent reference with a non-silent test gives -infinity.
///
/// Reference points: a gain error of 10 % = 20 dB, of 1 % = 40 dB. Starting
/// thresholds for a half-scale sine: 16-bit PCM >= 90 dB, 8-bit PCM / G.711
/// >= 35 dB, 24-bit and float PCM >= 130 dB. Not meaningful for lossy
/// codecs without alignment (see aligned_snr_db).
[[nodiscard]] inline double snr_db(const ir::Audio& reference, const ir::Audio& test) {
  if (!detail::comparable(reference, test)) return detail::not_a_number;
  const std::size_t count = std::max(reference.samples.size(), test.samples.size());
  double signal = 0.0;
  double noise = 0.0;
  for (std::size_t i = 0; i < count; ++i) {
    const double x = i < reference.samples.size() ? reference.samples[i] : 0.0f;
    const double y = i < test.samples.size() ? test.samples[i] : 0.0f;
    signal += x * x;
    noise += (x - y) * (x - y);
  }
  if (!(noise > 0.0)) return detail::infinity;
  return 10.0 * std::log10(signal / noise);
}

/// Normalised cross-correlation at zero lag over all samples of all
/// channels: sum(a * b) / sqrt(sum(a^2) * sum(b^2)). No mean is removed.
/// Range -1 .. +1: 1 = same waveform (any positive gain), -1 = inverted
/// polarity, about 0 = unrelated. Two silent signals give 1, exactly one
/// silent signal gives 0. Insensitive to gain, so pair it with snr_db or a
/// level check. Starting threshold for lossy codecs after alignment: >= 0.99.
[[nodiscard]] inline double correlation(const ir::Audio& a, const ir::Audio& b) {
  if (!detail::comparable(a, b)) return detail::not_a_number;
  double energy_a = 0.0, energy_b = 0.0, cross = 0.0;
  for (const float sample : a.samples) energy_a += static_cast<double>(sample) * sample;
  for (const float sample : b.samples) energy_b += static_cast<double>(sample) * sample;
  for (std::size_t i = 0; i < std::min(a.samples.size(), b.samples.size()); ++i) {
    cross += static_cast<double>(a.samples[i]) * b.samples[i];
  }
  if (!(energy_a > 0.0) && !(energy_b > 0.0)) return 1.0;
  if (!(energy_a > 0.0) || !(energy_b > 0.0)) return 0.0;
  return cross / std::sqrt(energy_a * energy_b);
}

/// The delay of `test` relative to `reference` in sample frames: the shift
/// d in [-max_shift_frames, +max_shift_frames] that maximises the normalised
/// cross-correlation of reference[i] with test[i + d], computed on the mono
/// mixdowns (mean of the channels) over the overlapping part. Positive =
/// test is late (it has d extra frames at the start, e.g. encoder delay);
/// negative = test is early (its first -d frames are missing).
///
/// Only shifts that leave at least half of the shorter signal overlapping
/// are considered. Among (numerically) equal maxima the smallest |d| wins,
/// so periodic signals such as a pure sine report the nearest match; use
/// sweep() or white_noise() when the exact delay matters. Returns 0 when
/// the signals are not comparable, empty or silent.
///
/// Cost: O(n log n) time (FFT) and about 32 bytes of memory per frame of
/// the longer signal plus max shift, rounded up to a power of two; meant
/// for clips of seconds, not minutes. Typical `max_shift_frames`: 4096
/// covers the delay of MP3 (1105), AAC (2112) and Opus (312 at 48 kHz).
[[nodiscard]] inline std::ptrdiff_t best_alignment(const ir::Audio& reference, const ir::Audio& test,
                                                   std::size_t max_shift_frames) {
  if (!detail::comparable(reference, test)) return 0;
  const std::vector<double> ref = detail::mono_mixdown(reference);
  const std::vector<double> tst = detail::mono_mixdown(test);
  if (ref.empty() || tst.empty()) return 0;
  const std::size_t longest = std::max(ref.size(), tst.size());
  const std::size_t min_overlap = (std::min(ref.size(), tst.size()) + 1) / 2;
  const std::size_t max_shift = std::min(max_shift_frames, longest - 1);

  // Circular cross-correlation via FFT; a transform of at least
  // `longest + max_shift` points keeps the lags of interest free of wrap-around.
  std::size_t size = 1;
  while (size < longest + max_shift) size <<= 1;
  std::vector<double> ref_re(size, 0.0), ref_im(size, 0.0), tst_re(size, 0.0), tst_im(size, 0.0);
  std::copy(ref.begin(), ref.end(), ref_re.begin());
  std::copy(tst.begin(), tst.end(), tst_re.begin());
  detail::fft(ref_re, ref_im, false);
  detail::fft(tst_re, tst_im, false);
  for (std::size_t i = 0; i < size; ++i) {
    // conj(R) * T
    const double re = ref_re[i] * tst_re[i] + ref_im[i] * tst_im[i];
    const double im = ref_re[i] * tst_im[i] - ref_im[i] * tst_re[i];
    tst_re[i] = re;
    tst_im[i] = im;
  }
  detail::fft(tst_re, tst_im, true);
  const std::vector<double>& cross = tst_re;  // cross[d mod size] * size = sum reference[i] * test[i + d]

  // Running energies, to normalise each lag by the energy of its overlap.
  std::vector<double> ref_energy(ref.size() + 1, 0.0), tst_energy(tst.size() + 1, 0.0);
  for (std::size_t i = 0; i < ref.size(); ++i) ref_energy[i + 1] = ref_energy[i] + ref[i] * ref[i];
  for (std::size_t i = 0; i < tst.size(); ++i) tst_energy[i + 1] = tst_energy[i] + tst[i] * tst[i];

  std::ptrdiff_t best_shift = 0;
  double best_value = -detail::infinity;
  const auto consider = [&](std::ptrdiff_t shift) {
    const detail::Overlap range = detail::overlap(ref.size(), tst.size(), shift);
    if (range.size() < min_overlap) return;
    const auto test_first = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(range.first) + shift);
    const double energy = (ref_energy[range.last] - ref_energy[range.first]) *
                          (tst_energy[test_first + range.size()] - tst_energy[test_first]);
    if (!(energy > 0.0)) return;
    const std::size_t index = shift >= 0 ? static_cast<std::size_t>(shift) : size - static_cast<std::size_t>(-shift);
    const double value = cross[index] / static_cast<double>(size) / std::sqrt(energy);
    // Candidates arrive in order of increasing |shift|; a later one must be clearly better.
    if (value > best_value + 1e-9) {
      best_value = value;
      best_shift = shift;
    }
  };
  consider(0);
  for (std::size_t magnitude = 1; magnitude <= max_shift; ++magnitude) {
    consider(static_cast<std::ptrdiff_t>(magnitude));
    consider(-static_cast<std::ptrdiff_t>(magnitude));
  }
  return best_shift;
}

/// snr_db after compensating for a delay: finds d = best_alignment(reference,
/// test, max_shift_frames), then computes the SNR in dB between reference[i]
/// and test[i + d] over the frames where both exist (all channels). This is
/// THE metric for lossy audio codecs, which delay and pad the signal.
/// Identical after shifting = +infinity; NaN when not comparable or when
/// nothing overlaps.
///
/// Only the overlap is compared, so truncated or over-long output is NOT
/// penalised here: assert on `frame_count()` separately. Gain changes are
/// not compensated (10 % gain error caps the result at 20 dB).
///
/// Starting thresholds on sine()/sweep() input: MP3, AAC, Vorbis, Opus at
/// q90 >= 20 dB, at q50 >= 12 dB; ADPCM >= 25 dB. Transform codecs do not
/// preserve the waveform of white_noise(); do not use noise for thresholds.
[[nodiscard]] inline double aligned_snr_db(const ir::Audio& reference, const ir::Audio& test,
                                           std::size_t max_shift_frames) {
  if (!detail::comparable(reference, test)) return detail::not_a_number;
  const std::ptrdiff_t shift = best_alignment(reference, test, max_shift_frames);
  const detail::Overlap range = detail::overlap(reference.frame_count(), test.frame_count(), shift);
  if (range.size() == 0) return detail::not_a_number;
  const std::size_t channels = reference.channels;
  const auto test_first = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(range.first) + shift);
  double signal = 0.0;
  double noise = 0.0;
  for (std::size_t i = 0; i < range.size() * channels; ++i) {
    const double x = reference.samples[range.first * channels + i];
    const double y = test.samples[test_first * channels + i];
    signal += x * x;
    noise += (x - y) * (x - y);
  }
  if (!(noise > 0.0)) return detail::infinity;
  return 10.0 * std::log10(signal / noise);
}

namespace detail {

[[nodiscard]] inline ir::Audio empty_audio(std::uint32_t sample_rate, std::uint16_t channels, double seconds) {
  ir::Audio audio;
  audio.sample_rate = sample_rate;
  audio.channels = channels;
  const double frames = std::round(seconds * static_cast<double>(sample_rate));
  audio.samples.resize(frames > 0.0 ? static_cast<std::size_t>(frames) * channels : 0);
  return audio;
}

}  // namespace detail

/// A sine tone: `round(seconds * sample_rate)` frames, every channel
/// carrying amplitude * sin(2 pi * frequency_hz * n / sample_rate), starting
/// at phase 0. The default amplitude 0.5 (-6 dBFS) leaves headroom for
/// codec overshoot. Deterministic; values agree across platforms to within
/// one float rounding step of the maths library.
[[nodiscard]] inline ir::Audio sine(std::uint32_t sample_rate, std::uint16_t channels, double frequency_hz,
                                    double seconds, double amplitude = 0.5) {
  ir::Audio audio = detail::empty_audio(sample_rate, channels, seconds);
  const double step = 2.0 * std::numbers::pi * frequency_hz / static_cast<double>(sample_rate);
  for (std::size_t frame = 0; frame < audio.frame_count(); ++frame) {
    const auto value = static_cast<float>(amplitude * std::sin(step * static_cast<double>(frame)));
    for (std::size_t channel = 0; channel < channels; ++channel) audio.samples[frame * channels + channel] = value;
  }
  return audio;
}

/// A linear sine sweep (chirp) from `f0_hz` to `f1_hz` over `seconds`, the
/// same on every channel: amplitude * sin(2 pi * (f0 t + (f1 - f0) t^2 /
/// (2 * seconds))). It has a single sharp correlation peak, which makes it
/// the right input for delay detection, and it exercises a codec's whole
/// frequency range. Same determinism as sine().
[[nodiscard]] inline ir::Audio sweep(std::uint32_t sample_rate, std::uint16_t channels, double f0_hz, double f1_hz,
                                     double seconds, double amplitude = 0.5) {
  ir::Audio audio = detail::empty_audio(sample_rate, channels, seconds);
  const double rate = seconds > 0.0 ? (f1_hz - f0_hz) / (2.0 * seconds) : 0.0;
  for (std::size_t frame = 0; frame < audio.frame_count(); ++frame) {
    const double t = static_cast<double>(frame) / static_cast<double>(sample_rate);
    const auto value = static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * (f0_hz + rate * t) * t));
    for (std::size_t channel = 0; channel < channels; ++channel) audio.samples[frame * channels + channel] = value;
  }
  return audio;
}

/// Uniform white noise in [-amplitude, +amplitude), independent per channel,
/// drawn from `random_bytes(..., seed)` (16 bits per sample). Bit-identical
/// on every platform for the default amplitude. Use it for lossless and PCM
/// round trips and for alignment; lossy codecs do not preserve its waveform.
[[nodiscard]] inline ir::Audio white_noise(std::uint32_t sample_rate, std::uint16_t channels, double seconds,
                                           std::uint32_t seed, double amplitude = 0.5) {
  ir::Audio audio = detail::empty_audio(sample_rate, channels, seconds);
  const Bytes noise = random_bytes(audio.samples.size() * 2, seed);
  for (std::size_t i = 0; i < audio.samples.size(); ++i) {
    const int word = std::to_integer<int>(noise[i * 2]) | (std::to_integer<int>(noise[i * 2 + 1]) << 8);
    audio.samples[i] = static_cast<float>(amplitude * (word - 32768) / 32768.0);
  }
  return audio;
}

// ===========================================================================
// Video (ir::VideoFrame)
// ===========================================================================

/// PSNR in dB between two frames of the SAME pixel format, width and height.
/// Higher is better; identical samples = +infinity; NaN when format or size
/// differ or a frame is not valid(). Strides may differ; row padding is
/// never read. `color`, `pts_ms` and `duration_ms` are ignored.
///
/// What is compared, per format:
///   - RGBA8: exactly as psnr(ir::Frame, ir::Frame) (alpha-aware, peak 255).
///   - I420, I422, I444, NV12: every stored byte of every plane, peak 255.
///   - I420P10, I422P10, I444P10: every little-endian 16-bit word, peak 1023.
///   - P010: every little-endian 16-bit word shifted right by 6 (its 10
///     significant bits; the 6 padding bits are ignored), peak 1023, so the
///     same picture scores the same as in I420P10.
/// For the YCbCr formats the squared errors of all planes are pooled into
/// one MSE over all samples (4:2:0 therefore weights Y : Cb : Cr as
/// 4 : 1 : 1); it is not the mean of per-plane PSNRs.
///
/// Reference point: every sample off by 1 = 48.13 dB (8 bit), 60.20 dB
/// (10 bit). Starting thresholds: lossless/intra-PCM = +infinity; visually
/// lossless (CRF <= 18, q90) >= 40 dB; q50 >= 30 dB.
[[nodiscard]] inline double psnr(const ir::VideoFrame& a, const ir::VideoFrame& b) {
  if (a.format != b.format || a.width != b.width || a.height != b.height || !a.valid() || !b.valid()) {
    return detail::not_a_number;
  }
  if (a.format == ir::PixelFormat::RGBA8) {
    detail::RgbaError error;
    for (std::size_t y = 0; y < a.height; ++y) {
      detail::accumulate_rgba(a.planes[0].data.data() + y * a.planes[0].stride,
                              b.planes[0].data.data() + y * b.planes[0].stride, a.width, error);
    }
    return detail::rgba_psnr(error, static_cast<std::uint64_t>(a.width) * a.height);
  }
  const bool wide = ir::bytes_per_sample(a.format) == 2;
  const unsigned shift = a.format == ir::PixelFormat::P010 ? 6u : 0u;
  const double peak = static_cast<double>((1u << ir::bit_depth(a.format)) - 1u);
  std::uint64_t sum_squared = 0;
  std::uint64_t count = 0;
  for (std::size_t plane = 0; plane < ir::plane_count(a.format); ++plane) {
    const std::size_t rows = ir::plane_height(a.format, plane, a.height);
    const std::size_t samples = ir::plane_row_bytes(a.format, plane, a.width) / ir::bytes_per_sample(a.format);
    for (std::size_t y = 0; y < rows; ++y) {
      const std::uint8_t* row_a = a.planes[plane].data.data() + y * a.planes[plane].stride;
      const std::uint8_t* row_b = b.planes[plane].data.data() + y * b.planes[plane].stride;
      for (std::size_t i = 0; i < samples; ++i) {
        const std::int64_t difference = static_cast<std::int64_t>(detail::load_sample(row_a, i, wide) >> shift) -
                                        static_cast<std::int64_t>(detail::load_sample(row_b, i, wide) >> shift);
        sum_squared += static_cast<std::uint64_t>(difference * difference);
      }
    }
    count += static_cast<std::uint64_t>(rows) * samples;
  }
  return detail::psnr_from_error(static_cast<double>(sum_squared), static_cast<double>(count), peak);
}

/// psnr(a[i], b[i]) for every frame index of the LONGER sequence; indices
/// present in only one sequence get NaN, so a dropped or duplicated frame at
/// the end is visible. Frames are paired by position, not by timestamp.
[[nodiscard]] inline std::vector<double> per_frame_psnr(const std::vector<ir::VideoFrame>& a,
                                                        const std::vector<ir::VideoFrame>& b) {
  std::vector<double> values(std::max(a.size(), b.size()), detail::not_a_number);
  for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) values[i] = psnr(a[i], b[i]);
  return values;
}

/// The lowest per_frame_psnr value: the quality of the worst frame, which is
/// what a video round-trip test should assert on. NaN when the sequences
/// are empty, differ in length or contain a pair that cannot be compared.
/// Thresholds as for psnr(VideoFrame, VideoFrame).
[[nodiscard]] inline double min_psnr(const std::vector<ir::VideoFrame>& a, const std::vector<ir::VideoFrame>& b) {
  const std::vector<double> values = per_frame_psnr(a, b);
  if (values.empty()) return detail::not_a_number;
  double lowest = detail::infinity;
  for (const double value : values) {
    if (std::isnan(value)) return detail::not_a_number;
    lowest = std::min(lowest, value);
  }
  return lowest;
}

/// Frame number `frame_index` of a deterministic moving test sequence,
/// written directly in `format` (no colour conversion involved), with
/// tightly packed planes. Smooth diagonal ramps that scroll by a few pixels
/// per frame: compressible, but every frame differs from its neighbours.
///
///   - RGBA8: R, G and B are triangle waves (0..255) of different
///     directions and speeds; alpha is 255.
///   - YCbCr formats: limited ("video") range. Y is a triangle wave over
///     16..235 of (2x + y + 4 * frame_index); Cb and Cr are triangle waves
///     over 16..240 of the horizontal and vertical position at their own
///     speeds. 10-bit formats use the 10-bit range (64..940 / 64..960) at
///     full 10-bit resolution; P010 stores those values shifted left by 6.
///   - `color` is left at its default (BT.709, limited range, left-sited
///     chroma), `pts_ms` is frame_index * 40 and `duration_ms` is 40
///     (25 fps); change them if the test needs something else.
///
/// Integer arithmetic only: identical on every platform. `width` and
/// `height` must be > 0.
[[nodiscard]] inline ir::VideoFrame test_pattern_frame(ir::PixelFormat format, std::uint32_t width,
                                                       std::uint32_t height, std::uint32_t frame_index) {
  ir::VideoFrame frame = ir::VideoFrame::allocate(format, width, height);
  frame.pts_ms = static_cast<std::int64_t>(frame_index) * 40;
  frame.duration_ms = 40;
  const std::uint64_t t = frame_index;
  if (format == ir::PixelFormat::RGBA8) {
    for (std::size_t y = 0; y < height; ++y) {
      std::uint8_t* row = frame.planes[0].data.data() + y * frame.planes[0].stride;
      for (std::size_t x = 0; x < width; ++x) {
        const std::uint64_t u = x;
        const std::uint64_t v = y;
        row[x * 4] = static_cast<std::uint8_t>(detail::triangle_wave(2 * u + v + 4 * t));
        row[x * 4 + 1] = static_cast<std::uint8_t>(detail::triangle_wave(u + 2 * v + 2 * t + 170));
        row[x * 4 + 2] = static_cast<std::uint8_t>(detail::triangle_wave(3 * u + 3 * v + 6 * t + 340));
        row[x * 4 + 3] = 255;
      }
    }
    return frame;
  }
  const bool wide = ir::bytes_per_sample(format) == 2;
  const unsigned shift = format == ir::PixelFormat::P010 ? 6u : 0u;
  // Limited-range black level and luma / chroma excursions for 8 and 10 bit.
  const std::uint32_t base = wide ? 64u : 16u;
  const std::uint32_t luma_span = wide ? 876u : 219u;
  const std::uint32_t chroma_span = wide ? 896u : 224u;
  const auto scaled = [base, shift](std::uint32_t wave, std::uint32_t span) {
    return (base + (wave * span + 127u) / 255u) << shift;
  };
  for (std::size_t y = 0; y < height; ++y) {
    std::uint8_t* row = frame.planes[0].data.data() + y * frame.planes[0].stride;
    for (std::size_t x = 0; x < width; ++x) {
      const std::uint64_t position = 2 * static_cast<std::uint64_t>(x) + y + 4 * t;
      detail::store_sample(row, x, wide, scaled(detail::triangle_wave(position), luma_span));
    }
  }
  const bool interleaved = ir::plane_count(format) == 2;
  const std::size_t chroma_width = ir::plane_width(format, 1, width);
  const std::size_t chroma_height = ir::plane_height(format, 1, height);
  for (std::size_t cy = 0; cy < chroma_height; ++cy) {
    const std::uint64_t y = static_cast<std::uint64_t>(cy) << ir::chroma_shift_y(format);
    std::uint8_t* row_cb = frame.planes[1].data.data() + cy * frame.planes[1].stride;
    std::uint8_t* row_cr = interleaved ? row_cb : frame.planes[2].data.data() + cy * frame.planes[2].stride;
    for (std::size_t cx = 0; cx < chroma_width; ++cx) {
      const std::uint64_t x = static_cast<std::uint64_t>(cx) << ir::chroma_shift_x(format);
      const std::uint32_t cb = scaled(detail::triangle_wave(x + 2 * t), chroma_span);
      const std::uint32_t cr = scaled(detail::triangle_wave(y + 3 * t + 255), chroma_span);
      detail::store_sample(row_cb, interleaved ? cx * 2 : cx, wide, cb);
      detail::store_sample(row_cr, interleaved ? cx * 2 + 1 : cx, wide, cr);
    }
  }
  return frame;
}

// ===========================================================================
// Text and documents
// ===========================================================================

/// Above this product of the two normalised lengths (in code points),
/// text_similarity compares words instead of code points.
inline constexpr std::uint64_t text_similarity_exact_limit = 25'000'000;

/// The form in which text_similarity compares text: decoded as UTF-8
/// (malformed bytes become U+FFFD), a leading byte order mark dropped, every
/// run of Unicode white space (spaces, tabs, CR, LF, NBSP, ...) replaced by
/// ONE ASCII space, and leading/trailing white space removed. Line structure
/// is deliberately lost: "a\r\n b" and "a b" normalise to the same string.
/// No case folding and no Unicode normalisation (NFC/NFD) is applied.
[[nodiscard]] inline std::string normalized_text(std::string_view text) {
  return util::utf32_to_utf8(detail::normalized_code_points(text));
}

/// How alike two texts are, 0 .. 1: 1 - distance / max(length a, length b),
/// where distance is the Levenshtein edit distance (insertions, deletions
/// and substitutions, each costing 1) between the normalized_text() forms,
/// counted in Unicode code points (an accented letter is one unit, not two bytes).
/// 1 = identical after normalisation, including two empty texts; 0 = nothing
/// in common.
///
/// Large inputs: when length a * length b exceeds text_similarity_exact_limit
/// (25 million, e.g. two texts of more than 5000 code points), whole
/// words (space-separated) are compared instead of code points, with the
/// same formula over word counts. The result is close to, but not the same
/// as, the code-point value. Time O(n * m) in the differing middle part
/// (common prefixes and suffixes are free), memory O(min(n, m)).
///
/// Starting thresholds: lossless text formats and encodings == 1.0;
/// document round trips (DOCX/ODT/HTML/Markdown) >= 0.98; extraction from
/// laid-out formats (PDF) >= 0.90.
[[nodiscard]] inline double text_similarity(std::string_view a, std::string_view b) {
  const std::u32string x = detail::normalized_code_points(a);
  const std::u32string y = detail::normalized_code_points(b);
  if (x.empty() && y.empty()) return 1.0;
  if (static_cast<std::uint64_t>(x.size()) * y.size() <= text_similarity_exact_limit) {
    const std::size_t distance = detail::levenshtein(std::span<const char32_t>(x), std::span<const char32_t>(y));
    return 1.0 - static_cast<double>(distance) / static_cast<double>(std::max(x.size(), y.size()));
  }
  std::map<std::u32string_view, std::uint32_t> ids;
  std::vector<std::uint32_t> words_x, words_y;
  detail::append_token_ids(x, ids, words_x);
  detail::append_token_ids(y, ids, words_y);
  const std::size_t distance =
      detail::levenshtein(std::span<const std::uint32_t>(words_x), std::span<const std::uint32_t>(words_y));
  return 1.0 - static_cast<double>(distance) / static_cast<double>(std::max(words_x.size(), words_y.size()));
}

/// text_similarity of the plain-text renditions (`ir::plain_text`) of two
/// documents: compares the words and their order, not formatting, heading
/// levels, links, images or metadata. Same range and thresholds as above.
[[nodiscard]] inline double text_similarity(const ir::Document& a, const ir::Document& b) {
  return text_similarity(std::string_view(ir::plain_text(a)), std::string_view(ir::plain_text(b)));
}

// ===========================================================================
// Structured data (cvt::Json / ir::Data::value)
// ===========================================================================

/// "" when the two values are structurally equal, otherwise the location and
/// nature of the FIRST difference, e.g.
///   `/users/0/age: 41 vs 42`
///   `/config/name: only in the first value`
///   `(root): array [1,2] vs object {"a":1}`
/// The location is a JSON pointer (RFC 6901; "(root)" for the top level).
///
/// Equality rules:
///   - objects: same set of keys, order ignored, values equal per key;
///   - arrays: same length, elements equal in order;
///   - numbers: compared by value, so integer 1 equals float 1.0 (integers
///     are compared exactly as 64-bit integers); with `number_tolerance`
///     t > 0 two numbers are also equal when |a - b| <= t (absolute) or
///     |a - b| <= t * max(|a|, |b|) (relative). NaN equals NaN;
///   - strings, booleans, null: exactly equal, and never equal to another
///     type ("1" differs from 1, null differs from a missing key).
///
/// Typical tolerance: 0 for JSON/YAML/TOML/MessagePack round trips; 1e-12
/// where numbers pass through decimal text with fewer than 17 digits.
[[nodiscard]] inline std::string data_diff(const Json& a, const Json& b, double number_tolerance = 0.0) {
  std::string path;
  return detail::data_difference(a, b, number_tolerance, path);
}

/// True when data_diff(a, b, number_tolerance) finds no difference; see
/// there for the rules. In a test, print the reason on failure:
///   CHECK_MESSAGE(data_equal(a, b), data_diff(a, b));
[[nodiscard]] inline bool data_equal(const Json& a, const Json& b, double number_tolerance = 0.0) {
  return data_diff(a, b, number_tolerance).empty();
}

// ===========================================================================
// Tables (ir::Table)
// ===========================================================================

/// "" when the two workbooks are equal, otherwise the FIRST difference with
/// 0-based sheet, row and column indices, e.g.
///   `sheet 0 ("Prices") row 2 column 1: Number 3.5 vs String "3.50"`
///
/// Equality rules:
///   - same number of sheets, same sheet names, in order;
///   - cell by cell: same CellType; String cells have the same `text`;
///     Number, Bool and DateTime cells have the same `number` within
///     `number_tolerance` (absolute or relative, as for data_diff). The
///     display `text` of non-string cells is NOT compared ("3.50" vs "3.5");
///   - trailing Empty cells of a row and trailing rows without any non-empty
///     cell are insignificant, so ragged and padded grids compare equal;
///     an Empty cell is NOT equal to a String cell with empty text;
///   - `Sheet::has_header` is not compared.
///
/// Typical tolerance: 0 for CSV/TSV and XLSX/ODS numbers; 1e-9 where
/// numbers are reformatted as text; DateTime through formats with day or
/// second resolution needs an absolute tolerance to match.
[[nodiscard]] inline std::string table_diff(const ir::Table& a, const ir::Table& b, double number_tolerance = 0.0) {
  if (a.sheets.size() != b.sheets.size()) {
    return std::format("sheet count {} vs {}", a.sheets.size(), b.sheets.size());
  }
  const std::vector<ir::Cell> no_cells;
  const ir::Cell empty_cell;
  for (std::size_t s = 0; s < a.sheets.size(); ++s) {
    const ir::Sheet& sheet_a = a.sheets[s];
    const ir::Sheet& sheet_b = b.sheets[s];
    if (sheet_a.name != sheet_b.name) {
      return std::format("sheet {}: name \"{}\" vs \"{}\"", s, detail::abbreviated(sheet_a.name),
                         detail::abbreviated(sheet_b.name));
    }
    const std::size_t rows_a = detail::significant_rows(sheet_a);
    const std::size_t rows_b = detail::significant_rows(sheet_b);
    for (std::size_t r = 0; r < std::max(rows_a, rows_b); ++r) {
      const std::vector<ir::Cell>& row_a = r < rows_a ? sheet_a.rows[r] : no_cells;
      const std::vector<ir::Cell>& row_b = r < rows_b ? sheet_b.rows[r] : no_cells;
      const std::size_t cells_a = detail::significant_cells(row_a);
      const std::size_t cells_b = detail::significant_cells(row_b);
      for (std::size_t c = 0; c < std::max(cells_a, cells_b); ++c) {
        const ir::Cell& cell_a = c < cells_a ? row_a[c] : empty_cell;
        const ir::Cell& cell_b = c < cells_b ? row_b[c] : empty_cell;
        if (!detail::cells_equal(cell_a, cell_b, number_tolerance)) {
          return std::format("sheet {} (\"{}\") row {} column {}: {} vs {}", s, detail::abbreviated(sheet_a.name), r,
                             c, detail::cell_summary(cell_a), detail::cell_summary(cell_b));
        }
      }
    }
  }
  return {};
}

/// True when table_diff(a, b, number_tolerance) finds no difference; see
/// there for the rules. In a test:
///   CHECK_MESSAGE(tables_equal(a, b), table_diff(a, b));
[[nodiscard]] inline bool tables_equal(const ir::Table& a, const ir::Table& b, double number_tolerance = 0.0) {
  return table_diff(a, b, number_tolerance).empty();
}

// ===========================================================================
// Meshes (ir::Mesh)
// ===========================================================================

/// Axis-aligned bounding box.
struct BBox {
  ir::Vec3 min;
  ir::Vec3 max;
};

/// The smallest box containing every entry of `mesh.positions` (referenced
/// by a face or not), in the mesh's own units. A mesh without positions
/// gives a box with min = max = (0, 0, 0).
[[nodiscard]] inline BBox bounding_box(const ir::Mesh& mesh) {
  BBox box;
  if (mesh.positions.empty()) return box;
  box.min = box.max = mesh.positions.front();
  for (const ir::Vec3& p : mesh.positions) {
    box.min = {std::min(box.min.x, p.x), std::min(box.min.y, p.y), std::min(box.min.z, p.z)};
    box.max = {std::max(box.max.x, p.x), std::max(box.max.y, p.y), std::max(box.max.z, p.z)};
  }
  return box;
}

/// "" when the meshes describe the same shape within `tolerance` (a
/// distance in the mesh's units, >= 0), otherwise the first failed check:
///   1. the same number of triangles after `Mesh::triangle_indices()`
///      (polygons count as their fan triangulation);
///   2. every coordinate of both bounding_box() corners within `tolerance`;
///   3. every position of `a` has some position of `b` within `tolerance`
///      (Euclidean distance), and every position of `b` has one of `a`.
/// Vertex order, vertex duplication/welding, face order and winding may
/// differ. NOT checked: which vertices each face connects, normals,
/// texture coordinates, colours, groups and names; assert those separately
/// when the format carries them.
///
/// Cost: O(n log n) (positions sorted by x, candidates taken from the
/// window x +- tolerance); slower when very many vertices share the same x.
///
/// Typical tolerance: binary float formats (STL, PLY, glTF) 0 or 1e-6 of
/// the bounding-box size; text formats (OBJ, ASCII STL/PLY) 1e-4 of it.
[[nodiscard]] inline std::string mesh_diff(const ir::Mesh& a, const ir::Mesh& b, float tolerance) {
  if (!(tolerance >= 0.0f)) return "tolerance must be >= 0";
  const std::size_t triangles_a = a.triangle_indices().size() / 3;
  const std::size_t triangles_b = b.triangle_indices().size() / 3;
  if (triangles_a != triangles_b) return std::format("triangle count {} vs {}", triangles_a, triangles_b);
  const BBox box_a = bounding_box(a);
  const BBox box_b = bounding_box(b);
  const auto corner = [tolerance](std::string_view name, const ir::Vec3& p, const ir::Vec3& q) -> std::string {
    const bool close = std::abs(static_cast<double>(p.x) - q.x) <= tolerance &&
                       std::abs(static_cast<double>(p.y) - q.y) <= tolerance &&
                       std::abs(static_cast<double>(p.z) - q.z) <= tolerance;
    if (close) return {};
    return std::format("bounding box {} ({}, {}, {}) vs ({}, {}, {})", name, p.x, p.y, p.z, q.x, q.y, q.z);
  };
  if (std::string difference = corner("min", box_a.min, box_b.min); !difference.empty()) return difference;
  if (std::string difference = corner("max", box_a.max, box_b.max); !difference.empty()) return difference;
  const auto unmatched = [tolerance](std::string_view from_name, std::string_view to_name,
                                     const std::vector<ir::Vec3>& from, const std::vector<ir::Vec3>& to) {
    const std::size_t index = detail::first_unmatched_point(from, to, tolerance);
    if (index == from.size()) return std::string();
    const ir::Vec3& p = from[index];
    return std::format("vertex {} of the {} mesh ({}, {}, {}) has no vertex of the {} mesh within {}", index,
                       from_name, p.x, p.y, p.z, to_name, tolerance);
  };
  if (std::string difference = unmatched("first", "second", a.positions, b.positions); !difference.empty()) {
    return difference;
  }
  return unmatched("second", "first", b.positions, a.positions);
}

/// True when mesh_diff(a, b, tolerance) finds no difference; see there for
/// what is and is not compared. In a test:
///   CHECK_MESSAGE(meshes_close(a, b, 1e-4f), mesh_diff(a, b, 1e-4f));
[[nodiscard]] inline bool meshes_close(const ir::Mesh& a, const ir::Mesh& b, float tolerance) {
  return mesh_diff(a, b, tolerance).empty();
}

}  // namespace cvt::test
