// Convertible - C++ convenience layer over the C API (convertible.h).
//
// Header-only and dependency-free: it adds RAII and standard-library types on
// top of the C functions and nothing else, so it works identically whether
// the core is linked statically or loaded as a shared library. JSON-valued
// calls return the JSON text unchanged; parse it with the library of your
// choice.
//
//   cvt::ConvertParams params;
//   params.target = "base64";
//   cvt::Conversion result = cvt::convert(bytes, params);
//   if (result) save(result.data(), "out." + std::string(result.extension()));
//   else        show(result.message());
//
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#ifndef CONVERTIBLE_CONVERTIBLE_HPP
#define CONVERTIBLE_CONVERTIBLE_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "convertible/convertible.h"

namespace cvt {

namespace api_detail {

struct StringDeleter {
  void operator()(char* text) const noexcept { cvt_string_free(text); }
};
struct ResultDeleter {
  void operator()(cvt_result* result) const noexcept { cvt_result_free(result); }
};

/// Take ownership of a library string and copy it into a std::string.
inline std::string take(char* text) {
  const std::unique_ptr<char, StringDeleter> owned(text);
  return owned ? std::string(owned.get()) : std::string();
}

inline std::string path_utf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

inline const char* c_str_or_null(const std::string& text) { return text.empty() ? nullptr : text.c_str(); }

}  // namespace api_detail

/// Called with overall progress (0..1) and a short stage label.
/// Return true to cancel the conversion.
using ProgressCallback = std::function<bool(double fraction, std::string_view stage)>;

/// Arguments shared by `convert` and `convert_file`.
struct ConvertParams {
  /// Target format id or extension. Required.
  std::string target;
  /// Source format id or extension; empty = detect automatically.
  std::string source;
  /// Original file name for in-memory conversions (helps detection); may be empty.
  std::string filename;
  /// Option values as a JSON object, e.g. R"({"quality":70})"; empty = defaults.
  std::string options_json;
  /// Optional progress / cancellation callback.
  ProgressCallback progress;
};

/// Owns the outcome of a conversion (a `cvt_result`). Move-only.
class Conversion {
 public:
  explicit Conversion(cvt_result* result) noexcept : result_(result) {}

  /// True when the conversion succeeded.
  [[nodiscard]] bool ok() const noexcept { return status() == CVT_OK; }
  explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] cvt_status status() const noexcept { return static_cast<cvt_status>(cvt_result_status(result_.get())); }
  /// Error description; empty on success.
  [[nodiscard]] std::string_view message() const noexcept { return cvt_result_message(result_.get()); }
  /// The converted bytes (empty for file-to-file conversions and failures).
  /// Valid for the lifetime of this object.
  [[nodiscard]] std::span<const std::uint8_t> data() const noexcept {
    return {cvt_result_data(result_.get()), cvt_result_size(result_.get())};
  }
  /// Id of the produced format.
  [[nodiscard]] std::string_view format() const noexcept { return cvt_result_format(result_.get()); }
  /// Recommended output extension, without the dot.
  [[nodiscard]] std::string_view extension() const noexcept { return cvt_result_extension(result_.get()); }
  [[nodiscard]] std::string_view mime() const noexcept { return cvt_result_mime(result_.get()); }
  /// The output's own file name ("extract"), or empty (see cvt_result_filename).
  [[nodiscard]] std::string_view filename() const noexcept { return cvt_result_filename(result_.get()); }
  /// JSON array of warning strings.
  [[nodiscard]] std::string_view warnings_json() const noexcept { return cvt_result_warnings_json(result_.get()); }

  /// The underlying C handle (still owned by this object).
  [[nodiscard]] const cvt_result* get() const noexcept { return result_.get(); }

 private:
  std::unique_ptr<cvt_result, api_detail::ResultDeleter> result_;
};

/// Library version, e.g. "0.1.0".
[[nodiscard]] inline std::string version() { return cvt_version(); }

/// See cvt_formats_json().
[[nodiscard]] inline std::string formats_json() { return api_detail::take(cvt_formats_json()); }

/// See cvt_detect(). Returns the format id, or nullopt when unrecognised.
[[nodiscard]] inline std::optional<std::string> detect(std::span<const std::uint8_t> data,
                                                       const std::string& filename = {}) {
  const std::unique_ptr<char, api_detail::StringDeleter> id(
      cvt_detect(data.data(), data.size(), api_detail::c_str_or_null(filename)));
  if (!id) return std::nullopt;
  return std::string(id.get());
}

/// See cvt_targets_json().
[[nodiscard]] inline std::string targets_json(const std::string& source_format) {
  return api_detail::take(cvt_targets_json(source_format.c_str()));
}

/// See cvt_options_schema_json().
[[nodiscard]] inline std::string options_schema_json(const std::string& source_format,
                                                     const std::string& target_format) {
  return api_detail::take(cvt_options_schema_json(source_format.c_str(), target_format.c_str()));
}

namespace api_detail {
inline int progress_trampoline(double fraction, const char* stage, void* user) {
  const auto* callback = static_cast<const ProgressCallback*>(user);
  try {
    return (*callback)(fraction, stage != nullptr ? stage : "") ? 1 : 0;
  } catch (...) {
    return 1;  // an exception must not cross the C boundary: treat it as "cancel"
  }
}
}  // namespace api_detail

/// See cvt_convert(). Blocks until the conversion finishes.
[[nodiscard]] inline Conversion convert(std::span<const std::uint8_t> data, const ConvertParams& params) {
  const bool has_progress = static_cast<bool>(params.progress);
  return Conversion(cvt_convert(data.data(), data.size(), api_detail::c_str_or_null(params.filename),
                                api_detail::c_str_or_null(params.source), params.target.c_str(),
                                api_detail::c_str_or_null(params.options_json),
                                has_progress ? &api_detail::progress_trampoline : nullptr,
                                has_progress ? const_cast<ProgressCallback*>(&params.progress) : nullptr));
}

/// See cvt_convert_file(). `params.filename` is ignored.
[[nodiscard]] inline Conversion convert_file(const std::filesystem::path& input, const std::filesystem::path& output,
                                             const ConvertParams& params) {
  const bool has_progress = static_cast<bool>(params.progress);
  const std::string input_utf8 = api_detail::path_utf8(input);
  const std::string output_utf8 = api_detail::path_utf8(output);
  return Conversion(cvt_convert_file(input_utf8.c_str(), output_utf8.c_str(), api_detail::c_str_or_null(params.source),
                                     params.target.c_str(), api_detail::c_str_or_null(params.options_json),
                                     has_progress ? &api_detail::progress_trampoline : nullptr,
                                     has_progress ? const_cast<ProgressCallback*>(&params.progress) : nullptr));
}

}  // namespace cvt

#endif  // CONVERTIBLE_CONVERTIBLE_HPP
