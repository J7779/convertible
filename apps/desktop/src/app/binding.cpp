// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "app/binding.hpp"

#include <algorithm>
#include <memory>

#include "base/text_util.hpp"
#include "convertible/convertible.h"
#include "platform/fileio.hpp"

namespace cvd::app {

namespace {

std::string take(char* text) {
  if (!text) return {};
  std::string out(text);
  cvt_string_free(text);
  return out;
}

std::vector<std::string> string_list(const Json& arr) {
  std::vector<std::string> out;
  for (const Json& v : arr.items()) {
    if (v.is_string()) out.push_back(v.str());
  }
  return out;
}

struct ProgressBridge {
  const ProgressFn* fn;
};

int progress_trampoline(double fraction, const char* stage, void* user) {
  const auto* bridge = static_cast<const ProgressBridge*>(user);
  if (!bridge || !bridge->fn || !*bridge->fn) return 0;
  try {
    return (*bridge->fn)(fraction, stage ? stage : "") ? 1 : 0;
  } catch (...) {
    return 1;
  }
}

ConvertOutcome outcome_from(cvt_result* r) {
  ConvertOutcome o;
  o.status = cvt_result_status(r);
  o.message = cvt_result_message(r);
  o.format = cvt_result_format(r);
  o.extension = cvt_result_extension(r);
  o.mime = cvt_result_mime(r);
  if (auto w = Json::parse(cvt_result_warnings_json(r))) o.warnings = string_list(*w);
  return o;
}

}  // namespace

const std::vector<std::string>& category_order() {
  static const std::vector<std::string> order = {"image", "audio",    "video",    "document", "ebook", "spreadsheet",
                                                 "presentation", "data", "archive", "font", "subtitle", "model3d",
                                                 "geo",   "pim",      "code",     "encoding", "vector", "other"};
  return order;
}

std::string category_label(std::string_view id) {
  static const std::vector<std::pair<std::string_view, std::string_view>> labels = {
      {"image", "Images"},        {"audio", "Audio"},           {"video", "Video"},       {"document", "Documents"},
      {"ebook", "E-books"},       {"spreadsheet", "Spreadsheets"}, {"presentation", "Presentations"},
      {"data", "Data"},           {"archive", "Archives"},      {"font", "Fonts"},        {"subtitle", "Subtitles"},
      {"model3d", "3D models"},   {"geo", "Maps & GPS"},        {"pim", "Calendars & contacts"},
      {"code", "Code"},           {"encoding", "Encodings"},    {"vector", "Vector graphics"},
      {"other", "Other"}};
  for (const auto& [k, v] : labels) {
    if (k == id) return std::string(v);
  }
  if (id.empty()) return "Other";
  std::string s(id);
  s[0] = static_cast<char>(ascii_upper(s.substr(0, 1))[0]);
  return s;
}

std::vector<FormatInfo> parse_formats(std::string_view json) {
  std::vector<FormatInfo> out;
  const auto doc = Json::parse(json);
  if (!doc) return out;
  for (const Json& f : doc->items()) {
    FormatInfo info;
    info.id = f["id"].str();
    if (info.id.empty()) continue;
    info.name = f["name"].as_string(info.id);
    info.description = f["description"].str();
    info.category = f["category"].as_string("other");
    info.extensions = string_list(f["extensions"]);
    info.mime = f["mime"].str();
    info.readable = f["readable"].as_bool();
    info.writable = f["writable"].as_bool();
    info.popularity = f["popularity"].as_int();
    out.push_back(std::move(info));
  }
  return out;
}

std::vector<TargetInfo> parse_targets(std::string_view json) {
  std::vector<TargetInfo> out;
  const auto doc = Json::parse(json);
  if (!doc) return out;
  for (const Json& f : doc->items()) {
    TargetInfo t;
    t.id = f["id"].str();
    if (t.id.empty()) continue;
    t.name = f["name"].as_string(t.id);
    t.category = f["category"].as_string("other");
    t.extensions = string_list(f["extensions"]);
    t.mime = f["mime"].str();
    t.lossy = f["lossy"].as_bool();
    t.compress = f["compress"].as_bool();
    out.push_back(std::move(t));
  }
  return out;
}

std::vector<OptionSpec> parse_options(std::string_view json) {
  std::vector<OptionSpec> out;
  const auto doc = Json::parse(json);
  if (!doc) return out;
  for (const Json& o : doc->items()) {
    OptionSpec s;
    s.key = o["key"].str();
    if (s.key.empty()) continue;
    s.label = o["label"].as_string(s.key);
    const std::string& type = o["type"].str();
    if (type == "int") s.type = OptionType::Int;
    else if (type == "float") s.type = OptionType::Float;
    else if (type == "bool") s.type = OptionType::Bool;
    else if (type == "enum") s.type = OptionType::Enum;
    else s.type = OptionType::String;
    s.default_value = o["default"];
    if (o["min"].is_number()) s.min = o["min"].as_number();
    if (o["max"].is_number()) s.max = o["max"].as_number();
    s.choices = string_list(o["choices"]);
    s.description = o["description"].str();
    s.advanced = o["category"].str() == "advanced";
    if (s.type == OptionType::Enum && s.choices.empty()) s.type = OptionType::String;
    out.push_back(std::move(s));
  }
  return out;
}

const Catalog& Catalog::instance() {
  static const Catalog catalog = [] {
    Catalog c;
    c.formats_ = parse_formats(take(cvt_formats_json()));
    return c;
  }();
  return catalog;
}

const FormatInfo* Catalog::find(std::string_view id) const {
  for (const auto& f : formats_) {
    if (f.id == id) return &f;
  }
  return nullptr;
}

std::size_t Catalog::count_readable() const {
  return static_cast<std::size_t>(std::count_if(formats_.begin(), formats_.end(), [](const auto& f) { return f.readable; }));
}

std::size_t Catalog::count_writable() const {
  return static_cast<std::size_t>(std::count_if(formats_.begin(), formats_.end(), [](const auto& f) { return f.writable; }));
}

std::string core_version() { return cvt_version(); }

std::vector<TargetInfo> targets_for(const std::string& source_id) {
  if (source_id.empty()) return {};
  return parse_targets(take(cvt_targets_json(source_id.c_str())));
}

std::vector<OptionSpec> options_for(const std::string& source_id, const std::string& target_id) {
  if (source_id.empty() || target_id.empty()) return {};
  return parse_options(take(cvt_options_schema_json(source_id.c_str(), target_id.c_str())));
}

std::string detect_file(const std::string& path) {
  const auto head = fileio::read_file(path, 64 * 1024);
  if (!head) return {};
  const std::string name = path_filename(path);
  return take(cvt_detect(head->data(), head->size(), name.c_str()));
}

std::string status_title(int status) {
  switch (status) {
    case kOk: return "Done";
    case kInvalidArgument: return "Those settings don\xE2\x80\x99t work together";
    case kUnknownFormat: return "We couldn\xE2\x80\x99t tell what kind of file this is";
    case kUnsupported: return "This conversion isn\xE2\x80\x99t available yet";
    case kDecode: return "This file looks damaged or uses features we can\xE2\x80\x99t read";
    case kEncode: return "The content doesn\xE2\x80\x99t fit the chosen format";
    case kIo: return "The file couldn\xE2\x80\x99t be read or written";
    case kCancelled: return "Cancelled";
    case kLimit: return "This file is too large to convert safely";
    default: return "Something went wrong";
  }
}

std::string status_hint(int status) {
  switch (status) {
    case kInvalidArgument: return "Reset the options to their defaults and try again.";
    case kUnknownFormat: return "Check the file name extension, or pick a different file.";
    case kUnsupported: return "Choose another target format.";
    case kDecode: return "Try opening it in the app that made it and saving a fresh copy.";
    case kEncode: return "Try a different target format or other options.";
    case kIo: return "Make sure the file still exists and there is free disk space.";
    case kCancelled: return "Convert again whenever you\xE2\x80\x99re ready.";
    case kLimit: return "Try a smaller file, or a smaller size or quality setting.";
    case kOk: return "";
    default: return "Please try again. If it keeps happening, report it with the file type.";
  }
}

ConvertOutcome convert_file(const ConvertRequest& request, const ProgressFn& progress) {
  ProgressBridge bridge{&progress};
  cvt_result* r = cvt_convert_file(request.input_path.c_str(), request.output_path.c_str(),
                                   request.source.empty() ? nullptr : request.source.c_str(), request.target.c_str(),
                                   request.options_json.empty() ? nullptr : request.options_json.c_str(),
                                   progress ? &progress_trampoline : nullptr, progress ? &bridge : nullptr);
  ConvertOutcome o = outcome_from(r);
  cvt_result_free(r);
  if (o.status == kOk) o.output_size = fileio::file_size(request.output_path).value_or(0);
  return o;
}

ConvertOutcome convert_bytes(std::string_view data, const std::string& filename, const std::string& source,
                             const std::string& target, const std::string& options_json, std::string* output,
                             const ProgressFn& progress) {
  ProgressBridge bridge{&progress};
  cvt_result* r = cvt_convert(reinterpret_cast<const std::uint8_t*>(data.data()), data.size(),
                              filename.empty() ? nullptr : filename.c_str(), source.empty() ? nullptr : source.c_str(),
                              target.c_str(), options_json.empty() ? nullptr : options_json.c_str(),
                              progress ? &progress_trampoline : nullptr, progress ? &bridge : nullptr);
  ConvertOutcome o = outcome_from(r);
  o.output_size = cvt_result_size(r);
  if (output) {
    const std::uint8_t* d = cvt_result_data(r);
    output->assign(reinterpret_cast<const char*>(d), d ? cvt_result_size(r) : 0);
  }
  cvt_result_free(r);
  return o;
}

}  // namespace cvd::app
