// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/convert.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <system_error>

#include "engine/detect.hpp"
#include "engine/planner.hpp"
#include "util/fs.hpp"
#include "util/mapped_file.hpp"
#include "util/text.hpp"

namespace cvt {

namespace {

constexpr std::string_view limits_key = "limits";

std::string base_name(std::string_view filename) {
  if (const std::size_t slash = filename.find_last_of("/\\"); slash != std::string_view::npos) {
    filename.remove_prefix(slash + 1);
  }
  return util::sanitize_utf8(filename);
}

void fill_format_fields(ConvertOutput& output, const FormatInfo& source, const FormatInfo& target) {
  output.source_format_id = source.id;
  output.format_id = target.id;
  output.extension = target.extensions.empty() ? target.id : target.extensions.front();
  output.mime = target.mime.empty() ? std::string("application/octet-stream") : target.mime;
}

Result<const FormatInfo*> resolve_source(const Registry& registry, std::string_view source_id, ByteView data,
                                         std::string_view filename) {
  if (!util::trim(source_id).empty()) {
    const FormatInfo* source = registry.resolve(source_id);
    if (source == nullptr) return fail(Status::UnknownFormat, "unknown source format '{}'", util::sanitize_utf8(source_id));
    return source;
  }
  const auto detected = detect_format(registry, data, filename);
  if (!detected) return fail(Status::UnknownFormat, "the format of the input could not be detected");
  return detected->format;
}

Result<const FormatInfo*> resolve_target(const Registry& registry, std::string_view target_id) {
  if (util::trim(target_id).empty()) return fail(Status::InvalidArgument, "no target format was given");
  const FormatInfo* target = registry.resolve(target_id);
  if (target == nullptr) return fail(Status::UnknownFormat, "unknown target format '{}'", util::sanitize_utf8(target_id));
  return target;
}

// Options as the steps see them: without the reserved "limits" object.
Options step_options(const Options& options) {
  Options out = options;
  out.erase(limits_key);
  return out;
}

void warn_unknown_options(const Options& options, const std::vector<OptionSpec>& specs, const Warnings& warnings) {
  if (!options.json().is_object()) return;
  for (const auto& item : options.json().items()) {
    const std::string& key = item.key();
    const bool known = std::ranges::any_of(specs, [&](const OptionSpec& spec) { return spec.key == key; });
    if (!known) warnings.add("option '" + util::sanitize_utf8(key) + "' is not used by this conversion");
  }
}

Error cancelled_error() { return Error{Status::Cancelled, "the conversion was cancelled"}; }

// A step failed: if the user had asked to stop, that is the real story.
std::unexpected<Error> step_failure(Error error, const Progress& progress) {
  if (progress.cancelled()) return std::unexpected(cancelled_error());
  return std::unexpected(std::move(error));
}

Result<Bytes> run_native(const Registry& registry, const Plan& plan, const ConvertRequest& request, const Limits& limits,
                         const Options& options, const Warnings& warnings) {
  const Progress& progress = request.progress;
  const auto base_context = [&](Context& ctx, const std::vector<OptionSpec>& specs, double begin, double end,
                                std::string_view stage) -> Result<void> {
    auto normalized = options.normalized(specs);
    if (!normalized) return std::unexpected(std::move(normalized).error());
    ctx.options = std::move(*normalized);
    ctx.progress = progress.sub(begin, end, stage);
    ctx.limits = limits;
    ctx.warnings = warnings;
    ctx.registry = &registry;
    return {};
  };
  const std::string filename = base_name(request.filename);

  // -- read ------------------------------------------------------------------
  if (!progress.report(0.0, "Reading")) return std::unexpected(cancelled_error());
  AnyIR value;
  if (plan.reader != nullptr) {
    ReadContext ctx;
    CVT_TRY(base_context(ctx, plan.reader->read_options, 0.0, 0.45, "Reading"));
    ctx.input = request.input;
    ctx.filename = filename;
    ctx.format_id = plan.source->id;
    auto decoded = plan.reader->read(ctx);
    if (!decoded) return step_failure(std::move(decoded).error(), progress);
    value = std::move(*decoded);
  } else {
    ir::Blob blob;
    blob.bytes.assign(request.input.begin(), request.input.end());
    blob.filename = filename;
    blob.format_id = plan.source->id;
    blob.mime = plan.source->mime;
    value = std::move(blob);
  }
  if (value.kind() != plan.reader_kind) {
    return fail(Status::Internal, "the {} reader produced {} instead of {}", plan.source->id, kind_name(value.kind()),
                kind_name(plan.reader_kind));
  }

  // -- bridges ---------------------------------------------------------------
  const double bridge_span = plan.bridges.empty() ? 0.0 : 0.10 / static_cast<double>(plan.bridges.size());
  for (std::size_t i = 0; i < plan.bridges.size(); ++i) {
    const Bridge& bridge = *plan.bridges[i];
    const double begin = 0.45 + bridge_span * static_cast<double>(i);
    if (!progress.report(begin, "Converting")) return std::unexpected(cancelled_error());
    Context ctx;
    CVT_TRY(base_context(ctx, bridge.options, begin, begin + bridge_span, "Converting"));
    auto bridged = bridge.fn(std::move(value), ctx);
    if (!bridged) return step_failure(std::move(bridged).error(), progress);
    value = std::move(*bridged);
    if (value.kind() != bridge.to) {
      return fail(Status::Internal, "bridge '{}' produced {} instead of {}", bridge.label, kind_name(value.kind()),
                  kind_name(bridge.to));
    }
  }

  // -- write -----------------------------------------------------------------
  if (!progress.report(0.55, "Writing")) return std::unexpected(cancelled_error());
  if (value.kind() != plan.writer->kind) {
    return fail(Status::Internal, "route ends in {} but the {} writer expects {}", kind_name(value.kind()),
                plan.target->id, kind_name(plan.writer->kind));
  }
  WriteContext ctx;
  CVT_TRY(base_context(ctx, plan.writer->write_options, 0.55, 1.0, "Writing"));
  ctx.format_id = plan.target->id;
  ctx.source_filename = filename;
  ctx.source_format_id = plan.source->id;
  auto encoded = plan.writer->write(value, ctx);
  if (!encoded) return step_failure(std::move(encoded).error(), progress);
  if (progress.cancelled()) return std::unexpected(cancelled_error());
  return encoded;
}

Result<ConvertOutput> convert_checked(const Registry& registry, const ConvertRequest& request) {
  auto limits = effective_limits(request.limits, request.options);
  if (!limits) return std::unexpected(std::move(limits).error());
  CVT_TRY(limits->check_input(request.input.size()));

  CVT_TRY_ASSIGN(const FormatInfo* target, resolve_target(registry, request.target_id));
  CVT_TRY_ASSIGN(const FormatInfo* source, resolve_source(registry, request.source_id, request.input, request.filename));
  CVT_TRY_ASSIGN(const Plan plan, plan_conversion(registry, source->id, target->id));

  const Options options = step_options(request.options);
  Warnings warnings;
  warn_unknown_options(options, plan.option_specs(), warnings);

  ConvertOutput output;
  fill_format_fields(output, *source, *target);
  CVT_TRY_ASSIGN(output.data, run_native(registry, plan, request, *limits, options, warnings));
  CVT_TRY(limits->check_output(output.data.size()));
  request.progress.report(1.0, "Done");
  output.warnings = warnings.list();
  return output;
}

Result<ConvertOutput> convert_file_checked(const Registry& registry, const FileConvertRequest& request) {
  if (request.input.empty()) return fail(Status::InvalidArgument, "no input path was given");
  if (request.output.empty()) return fail(Status::InvalidArgument, "no output path was given");
  auto limits = effective_limits(request.limits, request.options);
  if (!limits) return std::unexpected(std::move(limits).error());

  ConvertOutput output;
  {
    // Map the input instead of reading it: codecs see one contiguous span
    // and the operating system pages it in as they touch it. The mapping
    // lives exactly as long as the conversion (streaming IRs may point into
    // it) and is gone before the output file is opened, so converting a
    // file "in place" is safe.
    CVT_TRY_ASSIGN(const util::MappedFile file, util::MappedFile::open(request.input, limits->max_input_bytes));
    ConvertRequest memory;
    memory.input = file.bytes();
    memory.filename = util::filename_utf8(request.input);
    memory.source_id = request.source_id;
    memory.target_id = request.target_id;
    memory.options = request.options;
    memory.progress = request.progress;
    memory.limits = request.limits;
    CVT_TRY_ASSIGN(output, convert_checked(registry, memory));
  }
  CVT_TRY(util::write_file(request.output, output.data));
  output.data.clear();
  output.data.shrink_to_fit();
  return output;
}

template <class Function>
Result<ConvertOutput> guarded(Function&& function) {
  try {
    return function();
  } catch (const std::bad_alloc&) {
    return fail(Status::Limit, "not enough memory to complete the conversion");
  } catch (const std::exception& error) {
    return fail(Status::Internal, "unexpected error: {}", util::sanitize_utf8(error.what()));
  } catch (...) {
    return fail(Status::Internal, "unexpected error");
  }
}

}  // namespace

Result<Limits> effective_limits(const Limits& base, const Options& options) {
  Limits limits = base;
  const Json& all = options.json();
  if (!all.is_object()) return limits;
  const Json* const found = all.find(limits_key);
  if (found == nullptr || found->is_null()) return limits;
  if (!found->is_object()) return fail(Status::InvalidArgument, "option 'limits' must be an object");
  for (const auto& item : found->items()) {
    const Json& value = item.value();
    if (!value.is_number() || value.as_double() < 0) {
      return fail(Status::InvalidArgument, "limit '{}' must be a non-negative number", util::sanitize_utf8(item.key()));
    }
    // Non-negative here: integers are taken exactly, reals truncate and saturate.
    const std::uint64_t amount = value.is_integer() ? value.as_uint64()
                                 : value.as_double() >= 18446744073709551615.0
                                       ? std::numeric_limits<std::uint64_t>::max()
                                       : static_cast<std::uint64_t>(value.as_double());
    const std::string& key = item.key();
    if (key == "max_input_bytes") {
      limits.max_input_bytes = amount;
    } else if (key == "max_output_bytes") {
      limits.max_output_bytes = amount;
    } else if (key == "max_pixels") {
      limits.max_pixels = amount;
    } else if (key == "max_archive_entries") {
      limits.max_archive_entries = amount;
    } else if (key == "max_decompressed_bytes") {
      limits.max_decompressed_bytes = amount;
    } else {
      return fail(Status::InvalidArgument, "unknown limit '{}'", util::sanitize_utf8(key));
    }
  }
  return limits;
}

Result<ConvertOutput> run_conversion(const Registry& registry, const ConvertRequest& request) {
  return guarded([&] { return convert_checked(registry, request); });
}

Result<ConvertOutput> run_file_conversion(const Registry& registry, const FileConvertRequest& request) {
  return guarded([&] { return convert_file_checked(registry, request); });
}

}  // namespace cvt
