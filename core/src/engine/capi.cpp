// Convertible core - implementation of the C ABI (convertible.h).
//
// Nothing may throw across this boundary: every entry point is wrapped so
// that any exception becomes an error result or an empty answer.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "util/bytes.hpp"
#include "convertible/convertible.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "engine/convert.hpp"
#include "engine/detect.hpp"
#include "engine/planner.hpp"
#include "engine/registry.hpp"
#include "util/fs.hpp"
#include "util/json.hpp"
#include "util/text.hpp"

#ifndef CVT_VERSION_STRING
#  define CVT_VERSION_STRING "0.1.0"
#endif

struct cvt_result {
  int status = CVT_ERR_INTERNAL;
  std::string message;
  cvt::Bytes data;
  std::string format;
  std::string extension;
  std::string mime;
  std::string filename;
  std::string warnings_json = "[]";
};

static_assert(CVT_OK == static_cast<int>(cvt::Status::Ok));
static_assert(CVT_ERR_INVALID_ARGUMENT == static_cast<int>(cvt::Status::InvalidArgument));
static_assert(CVT_ERR_UNKNOWN_FORMAT == static_cast<int>(cvt::Status::UnknownFormat));
static_assert(CVT_ERR_UNSUPPORTED_CONVERSION == static_cast<int>(cvt::Status::UnsupportedConversion));
static_assert(CVT_ERR_DECODE == static_cast<int>(cvt::Status::Decode));
static_assert(CVT_ERR_ENCODE == static_cast<int>(cvt::Status::Encode));
static_assert(CVT_ERR_IO == static_cast<int>(cvt::Status::Io));
static_assert(CVT_ERR_CANCELLED == static_cast<int>(cvt::Status::Cancelled));
static_assert(CVT_ERR_LIMIT == static_cast<int>(cvt::Status::Limit));
static_assert(CVT_ERR_INTERNAL == static_cast<int>(cvt::Status::Internal));

namespace {

using cvt::Json;

// Returned when not even the result object could be allocated. Never freed.
const cvt_result& out_of_memory_result() {
  static const cvt_result result = [] {
    cvt_result r;
    r.status = CVT_ERR_INTERNAL;
    r.message = "out of memory";
    return r;
  }();
  return result;
}

char* duplicate(std::string_view text) noexcept {
  char* out = static_cast<char*>(std::malloc(text.size() + 1));
  if (out == nullptr) return nullptr;
  if (!text.empty()) ::cvt::util::copy_bytes(out, text.data(), text.size());
  out[text.size()] = '\0';
  return out;
}

// Runs `build` and returns its JSON as a malloc'd string, or `fallback`.
template <class Build>
char* json_or(std::string_view fallback, Build&& build) noexcept {
  try {
    return duplicate(cvt::util::dump_json(build()));
  } catch (...) {
    return duplicate(fallback);
  }
}

std::string_view view_or_empty(const char* text) noexcept { return text != nullptr ? std::string_view(text) : std::string_view(); }

cvt_result* make_result() noexcept { return new (std::nothrow) cvt_result(); }

cvt_result* failure(cvt_result* result, cvt::Status status, std::string message) {
  result->status = static_cast<int>(status);
  result->message = cvt::util::sanitize_utf8(message);
  return result;
}

void fill_success(cvt_result& result, cvt::ConvertOutput&& output) {
  result.status = CVT_OK;
  result.message.clear();
  result.data = std::move(output.data);
  result.format = std::move(output.format_id);
  result.extension = std::move(output.extension);
  result.mime = std::move(output.mime);
  result.filename = std::move(output.filename);
  Json warnings = Json::array();
  for (std::string& warning : output.warnings) warnings.push_back(std::move(warning));
  result.warnings_json = cvt::util::dump_json(warnings);
}

cvt::Progress make_progress(cvt_progress_fn progress, void* user) {
  if (progress == nullptr) return {};
  return cvt::Progress([progress, user](double fraction, std::string_view stage) {
    const std::string label(stage);
    return progress(fraction, label.c_str(), user) != 0;
  });
}

// Shared shape of the two convert entry points: allocate the result, run the
// body, and translate every failure mode into a status.
template <class Body>
cvt_result* convert_guarded(Body&& body) noexcept {
  cvt_result* result = make_result();
  if (result == nullptr) return const_cast<cvt_result*>(&out_of_memory_result());
  try {
    body(*result);
  } catch (const std::bad_alloc&) {
    result->status = CVT_ERR_LIMIT;
    result->data.clear();
    try {
      result->message = "not enough memory to complete the conversion";
    } catch (...) {
      result->message.clear();
    }
  } catch (...) {
    result->status = CVT_ERR_INTERNAL;
    result->data.clear();
    try {
      result->message = "unexpected internal error";
    } catch (...) {
      result->message.clear();
    }
  }
  return result;
}

}  // namespace

extern "C" {

const char* cvt_version(void) { return CVT_VERSION_STRING; }

void cvt_string_free(char* s) { std::free(s); }

char* cvt_formats_json(void) {
  return json_or("[]", [] {
    const cvt::Registry& registry = cvt::Registry::instance();
    Json out = Json::array();
    for (const cvt::FormatInfo* format : registry.formats()) {
      Json entry = Json::object();
      entry["id"] = format->id;
      entry["name"] = format->name;
      entry["description"] = format->description;
      entry["category"] = cvt::category_name(format->category);
      entry["extensions"] = format->extensions;
      entry["mime"] = format->mime.empty() ? std::string("application/octet-stream") : format->mime;
      entry["readable"] = !format->output_only && registry.has_reader(format->id);
      entry["writable"] = registry.has_writer(format->id);
      entry["popularity"] = format->popularity;
      entry["output_only"] = format->output_only;
      out.push_back(std::move(entry));
    }
    return out;
  });
}

char* cvt_detect(const uint8_t* data, size_t size, const char* filename) {
  try {
    if (data == nullptr) size = 0;
    const cvt::ByteView view = cvt::as_bytes(std::span<const std::uint8_t>(data, size));
    const std::string name = cvt::util::sanitize_utf8(view_or_empty(filename));
    const auto detected = cvt::detect_format(cvt::Registry::instance(), view, name);
    return detected ? duplicate(detected->format->id) : nullptr;
  } catch (...) {
    return nullptr;
  }
}

char* cvt_targets_json(const char* source_format_id) {
  return json_or("[]", [source_format_id] {
    Json out = Json::array();
    const cvt::Registry& registry = cvt::Registry::instance();
    const cvt::FormatInfo* source = registry.resolve(view_or_empty(source_format_id));
    if (source == nullptr) return out;
    for (const cvt::TargetInfo& target : cvt::list_targets(registry, source->id)) {
      Json entry = Json::object();
      entry["id"] = target.format->id;
      entry["name"] = target.format->name;
      entry["category"] = cvt::category_name(target.format->category);
      entry["extensions"] = target.format->extensions;
      entry["mime"] = target.format->mime.empty() ? std::string("application/octet-stream") : target.format->mime;
      entry["lossy"] = target.lossy;
      entry["compress"] = target.same_format;
      entry["role"] = std::string(cvt::target_role(*source, target));
      out.push_back(std::move(entry));
    }
    return out;
  });
}

char* cvt_options_schema_json(const char* source_format_id, const char* target_format_id) {
  return json_or("[]", [source_format_id, target_format_id] {
    const cvt::Registry& registry = cvt::Registry::instance();
    const cvt::FormatInfo* source = registry.resolve(view_or_empty(source_format_id));
    const cvt::FormatInfo* target = registry.resolve(view_or_empty(target_format_id));
    if (source == nullptr || target == nullptr) return Json::array();
    const auto plan = cvt::plan_conversion(registry, source->id, target->id);
    if (!plan) return Json::array();
    return cvt::option_schema_json(plan->option_specs());
  });
}

cvt_result* cvt_convert(const uint8_t* data, size_t size, const char* filename, const char* source_format_id,
                        const char* target_format_id, const char* options_json, cvt_progress_fn progress, void* user) {
  return convert_guarded([&](cvt_result& result) {
    if (data == nullptr && size != 0) {
      failure(&result, cvt::Status::InvalidArgument, "data is NULL but size is not zero");
      return;
    }
    if (target_format_id == nullptr || *target_format_id == '\0') {
      failure(&result, cvt::Status::InvalidArgument, "no target format was given");
      return;
    }
    auto options = cvt::Options::parse(view_or_empty(options_json));
    if (!options) {
      failure(&result, options.error().code, options.error().message);
      return;
    }
    cvt::ConvertRequest request;
    request.input = cvt::as_bytes(std::span<const std::uint8_t>(data, size));
    request.filename = cvt::util::sanitize_utf8(view_or_empty(filename));
    request.source_id = std::string(view_or_empty(source_format_id));
    request.target_id = target_format_id;
    request.options = std::move(*options);
    request.progress = make_progress(progress, user);
    auto output = cvt::run_conversion(cvt::Registry::instance(), request);
    if (!output) {
      failure(&result, output.error().code, output.error().message);
      return;
    }
    fill_success(result, std::move(*output));
  });
}

cvt_result* cvt_convert_file(const char* input_path, const char* output_path, const char* source_format_id,
                             const char* target_format_id, const char* options_json, cvt_progress_fn progress,
                             void* user) {
  return convert_guarded([&](cvt_result& result) {
    if (input_path == nullptr || *input_path == '\0') {
      failure(&result, cvt::Status::InvalidArgument, "no input path was given");
      return;
    }
    if (output_path == nullptr || *output_path == '\0') {
      failure(&result, cvt::Status::InvalidArgument, "no output path was given");
      return;
    }
    if (target_format_id == nullptr || *target_format_id == '\0') {
      failure(&result, cvt::Status::InvalidArgument, "no target format was given");
      return;
    }
    auto options = cvt::Options::parse(view_or_empty(options_json));
    if (!options) {
      failure(&result, options.error().code, options.error().message);
      return;
    }
    cvt::FileConvertRequest request;
    request.input = cvt::util::path_from_utf8(input_path);
    request.output = cvt::util::path_from_utf8(output_path);
    request.source_id = std::string(view_or_empty(source_format_id));
    request.target_id = target_format_id;
    request.options = std::move(*options);
    request.progress = make_progress(progress, user);
    auto output = cvt::run_file_conversion(cvt::Registry::instance(), request);
    if (!output) {
      failure(&result, output.error().code, output.error().message);
      return;
    }
    fill_success(result, std::move(*output));
  });
}

cvt_result* cvt_pack(const cvt_pack_input* inputs, size_t count, const char* target_format_id, const char* options_json,
                     cvt_progress_fn progress, void* user) {
  return convert_guarded([&](cvt_result& result) {
    if (inputs == nullptr && count != 0) {
      failure(&result, cvt::Status::InvalidArgument, "inputs is NULL but count is not zero");
      return;
    }
    if (target_format_id == nullptr || *target_format_id == '\0') {
      failure(&result, cvt::Status::InvalidArgument, "no target format was given");
      return;
    }
    auto options = cvt::Options::parse(view_or_empty(options_json));
    if (!options) {
      failure(&result, options.error().code, options.error().message);
      return;
    }
    cvt::PackRequest request;
    for (size_t i = 0; i < count; ++i) {
      const cvt_pack_input& input = inputs[i];
      if (input.data == nullptr && input.size != 0) {
        failure(&result, cvt::Status::InvalidArgument, "input " + std::to_string(i) + " has no data but a size");
        return;
      }
      request.inputs.push_back({cvt::as_bytes(std::span<const std::uint8_t>(input.data, input.size)),
                                std::string(view_or_empty(input.name))});
    }
    request.target_id = target_format_id;
    request.options = std::move(*options);
    request.progress = make_progress(progress, user);
    auto output = cvt::run_pack(cvt::Registry::instance(), request);
    if (!output) {
      failure(&result, output.error().code, output.error().message);
      return;
    }
    fill_success(result, std::move(*output));
  });
}

int cvt_result_status(const cvt_result* result) { return result != nullptr ? result->status : CVT_ERR_INVALID_ARGUMENT; }

const char* cvt_result_message(const cvt_result* result) { return result != nullptr ? result->message.c_str() : ""; }

const uint8_t* cvt_result_data(const cvt_result* result) {
  if (result == nullptr || result->data.empty()) return nullptr;
  return reinterpret_cast<const uint8_t*>(result->data.data());
}

size_t cvt_result_size(const cvt_result* result) { return result != nullptr ? result->data.size() : 0; }

const char* cvt_result_format(const cvt_result* result) { return result != nullptr ? result->format.c_str() : ""; }

const char* cvt_result_extension(const cvt_result* result) { return result != nullptr ? result->extension.c_str() : ""; }

const char* cvt_result_mime(const cvt_result* result) { return result != nullptr ? result->mime.c_str() : ""; }

const char* cvt_result_filename(const cvt_result* result) { return result != nullptr ? result->filename.c_str() : ""; }

const char* cvt_result_warnings_json(const cvt_result* result) {
  return result != nullptr ? result->warnings_json.c_str() : "[]";
}

void cvt_result_free(cvt_result* result) {
  if (result == nullptr || result == &out_of_memory_result()) return;
  delete result;
}

}  // extern "C"
