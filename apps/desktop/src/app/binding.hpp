// Convertible desktop - thin binding over the core's C API
// (core/include/convertible/convertible.h). Everything the app knows about
// formats comes from here at run time, so new formats appear without app changes.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.hpp"

namespace cvd::app {

struct FormatInfo {
  std::string id;
  std::string name;
  std::string description;
  std::string category;
  std::vector<std::string> extensions;
  std::string mime;
  bool readable = false;
  bool writable = false;
  int popularity = 0;
};

struct TargetInfo {
  std::string id;
  std::string name;
  std::string category;
  std::vector<std::string> extensions;
  std::string mime;
  bool lossy = false;
  bool compress = false;
  /// Optional, from newer cores: "compress", "archive", "extract" or empty.
  std::string role;
};

enum class OptionType { Int, Float, Bool, Enum, String };

struct OptionSpec {
  std::string key;
  std::string label;
  OptionType type = OptionType::String;
  Json default_value;
  std::optional<double> min;
  std::optional<double> max;
  std::vector<std::string> choices;
  std::string description;
  bool advanced = false;
};

/// The category ids in the order the core documents them.
const std::vector<std::string>& category_order();
/// Human label for a category id ("model3d" -> "3D models").
std::string category_label(std::string_view id);

std::vector<FormatInfo> parse_formats(std::string_view json);
std::vector<TargetInfo> parse_targets(std::string_view json);
std::vector<OptionSpec> parse_options(std::string_view json);

/// Catalogue of every format the core knows (cvt_formats_json), loaded once.
class Catalog {
 public:
  static const Catalog& instance();
  [[nodiscard]] const std::vector<FormatInfo>& formats() const { return formats_; }
  [[nodiscard]] const FormatInfo* find(std::string_view id) const;
  [[nodiscard]] std::size_t count_readable() const;
  [[nodiscard]] std::size_t count_writable() const;

 private:
  std::vector<FormatInfo> formats_;
};

std::string core_version();
std::vector<TargetInfo> targets_for(const std::string& source_id);
std::vector<OptionSpec> options_for(const std::string& source_id, const std::string& target_id);
/// Detects from the first 64 KiB of the file and its name; empty when unknown.
std::string detect_file(const std::string& path);

/// Status codes as in enum cvt_status.
enum Status : int {
  kOk = 0,
  kInvalidArgument = 1,
  kUnknownFormat = 2,
  kUnsupported = 3,
  kDecode = 4,
  kEncode = 5,
  kIo = 6,
  kCancelled = 7,
  kLimit = 8,
  kInternal = 9
};

/// A friendly, short title for a status ("This file looks damaged").
std::string status_title(int status);
/// A sentence explaining what to try next.
std::string status_hint(int status);

struct ConvertRequest {
  std::string input_path;
  std::string output_path;
  std::string source;  // format id; empty = detect
  std::string target;
  std::string options_json;  // flat object; empty = defaults
};

struct ConvertOutcome {
  int status = kInternal;
  std::string message;
  std::string format;
  std::string extension;
  std::string mime;
  std::vector<std::string> warnings;
  std::uint64_t output_size = 0;
};

/// Progress callback: return true to cancel.
using ProgressFn = std::function<bool(double fraction, std::string_view stage)>;

/// File-to-file conversion through cvt_convert_file (the input is memory-mapped by the core).
ConvertOutcome convert_file(const ConvertRequest& request, const ProgressFn& progress);
/// In-memory conversion through cvt_convert (used by tests and small inputs).
ConvertOutcome convert_bytes(std::string_view data, const std::string& filename, const std::string& source,
                             const std::string& target, const std::string& options_json, std::string* output,
                             const ProgressFn& progress = {});

}  // namespace cvd::app
