// convertible - command-line front end of the Convertible core.
//
// All conversion work goes through the public API (convertible.h /
// convertible.hpp), so this file doubles as a reference client for
// application developers. The only internal header used is the core's JSON
// wrapper, to read the JSON the API returns.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "convertible/convertible.hpp"
#include "util/json.hpp"

#if defined(_WIN32)
#  include <fcntl.h>
#  include <io.h>
#  include <windows.h>
#else
#  include <unistd.h>
#endif

namespace {

using cvt::Json;

constexpr std::string_view usage_text = R"(Usage:
  convertible convert <input> -t <format> [-o <output>] [--from <format>]
                      [--opt key=value]... [--force] [--quiet]
  convertible formats [--json] [--category <name>]
  convertible targets <file|format> [--json]
  convertible detect <file>
  convertible --version | --help

Commands:
  convert   Convert a file. The output name defaults to the input name with
            the target format's extension. Use "-" as input or output for
            standard input / standard output.
  formats   List every known format.
  targets   List the formats a file (or a format id) can be converted to.
  detect    Print the format id of a file.

Options for convert:
  -t, --to <format>     target format id or extension (required)
  -o, --output <path>   output file ("-" for standard output)
  -f, --from <format>   source format, instead of detecting it
      --opt key=value   conversion option; repeatable (see "targets" and the
                        options a conversion accepts with --list-options)
      --list-options    print the options of this conversion and exit
      --force           overwrite an existing output file
  -q, --quiet           no progress bar or summary

Exit status: 0 on success, otherwise the library status code
  1 invalid argument    2 unknown format     3 unsupported conversion
  4 decode error        5 encode error       6 I/O error
  7 cancelled           8 limit exceeded     9 internal error
)";

// ---------------------------------------------------------------------------
// Small platform helpers
// ---------------------------------------------------------------------------

std::filesystem::path path_from_utf8(std::string_view text) {
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string path_to_utf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

bool stderr_is_terminal() {
#if defined(_WIN32)
  return _isatty(_fileno(stderr)) != 0;
#else
  return isatty(fileno(stderr)) != 0;
#endif
}

void use_binary_stdio() {
#if defined(_WIN32)
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
}

std::optional<std::vector<std::uint8_t>> read_all(std::istream& stream) {
  std::vector<std::uint8_t> data;
  char buffer[64 * 1024];
  while (stream) {
    stream.read(buffer, sizeof(buffer));
    const std::streamsize got = stream.gcount();
    if (got <= 0) break;
    data.insert(data.end(), buffer, buffer + got);
  }
  if (stream.bad()) return std::nullopt;
  return data;
}

std::optional<std::vector<std::uint8_t>> read_file(const std::string& path) {
  if (path == "-") {
    use_binary_stdio();
    return read_all(std::cin);
  }
  std::error_code ec;
  if (std::filesystem::is_directory(path_from_utf8(path), ec)) return std::nullopt;
  std::ifstream stream(path_from_utf8(path), std::ios::binary);
  if (!stream) return std::nullopt;
  return read_all(stream);
}

int fail(int status, std::string_view message) {
  std::cerr << "convertible: " << message << '\n';
  return status;
}

Json parse_or(const std::string& text, Json fallback) {
  auto parsed = cvt::util::parse_json(text);
  return parsed ? std::move(*parsed) : std::move(fallback);
}

std::string join(const Json& array, std::string_view separator) {
  std::string out;
  for (const Json& item : array.elements()) {
    if (!out.empty()) out.append(separator);
    out.append(item.as_string());
  }
  return out;
}

std::string pad(std::string text, std::size_t width) {
  // Width in code points is close enough for the ASCII-dominated tables here.
  std::size_t length = 0;
  for (const char c : text) length += (static_cast<unsigned char>(c) & 0xC0) != 0x80 ? 1 : 0;
  if (length < width) {
    text.append(width - length, ' ');
  } else {
    text.append(2, ' ');  // over-long cells still get a gap before the next column
  }
  return text;
}

// ---------------------------------------------------------------------------
// Argument parsing
// ---------------------------------------------------------------------------

struct Arguments {
  std::vector<std::string> positional;
  std::string target;
  std::string source;
  std::string output;
  std::string category;
  std::vector<std::string> options;
  bool json = false;
  bool quiet = false;
  bool force = false;
  bool list_options = false;
  bool help = false;
  bool version = false;
  std::string error;
};

Arguments parse_arguments(const std::vector<std::string>& args) {
  Arguments out;
  bool only_positional = false;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& arg = args[i];
    const auto value = [&](std::string& slot) {
      if (i + 1 >= args.size()) {
        out.error = "option '" + arg + "' needs a value";
        return;
      }
      slot = args[++i];
    };
    if (only_positional || arg == "-" || arg.empty() || arg.front() != '-') {
      out.positional.push_back(arg);
    } else if (arg == "--") {
      only_positional = true;
    } else if (arg == "-t" || arg == "--to") {
      value(out.target);
    } else if (arg == "-f" || arg == "--from") {
      value(out.source);
    } else if (arg == "-o" || arg == "--output") {
      value(out.output);
    } else if (arg == "--category") {
      value(out.category);
    } else if (arg == "--opt") {
      std::string option;
      value(option);
      if (out.error.empty()) out.options.push_back(std::move(option));
    } else if (arg == "--json") {
      out.json = true;
    } else if (arg == "-q" || arg == "--quiet") {
      out.quiet = true;
    } else if (arg == "--force") {
      out.force = true;
    } else if (arg == "--list-options") {
      out.list_options = true;
    } else if (arg == "-h" || arg == "--help") {
      out.help = true;
    } else if (arg == "-V" || arg == "--version") {
      out.version = true;
    } else {
      out.error = "unknown option '" + arg + "'";
    }
    if (!out.error.empty()) break;
  }
  return out;
}

// "--opt key=value" pairs become a JSON object. Values that parse as JSON
// (numbers, true/false, quoted strings) keep their type; anything else is a
// string, so `--opt encoding=utf-16le` needs no quoting.
std::optional<std::string> options_json(const std::vector<std::string>& pairs, std::string& error) {
  if (pairs.empty()) return std::string();
  Json object = Json::object();
  for (const std::string& pair : pairs) {
    const std::size_t equals = pair.find('=');
    if (equals == std::string::npos || equals == 0) {
      error = "expected key=value after --opt, got '" + pair + "'";
      return std::nullopt;
    }
    const std::string text = pair.substr(equals + 1);
    auto value = cvt::util::parse_json(text);
    object[pair.substr(0, equals)] = value && !value->is_structured() ? std::move(*value) : Json(text);
  }
  return cvt::util::dump_json(object);
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

int command_formats(const Arguments& args) {
  Json formats = parse_or(cvt::formats_json(), Json::array());
  if (!args.category.empty()) {
    Json filtered = Json::array();
    for (const Json& format : formats.elements()) {
      if (format["category"].as_string() == args.category) filtered.push_back(format);
    }
    formats = std::move(filtered);
  }
  if (args.json) {
    std::cout << cvt::util::dump_json(formats, 2) << '\n';
    return CVT_OK;
  }
  std::cout << pad("ID", 14) << pad("CATEGORY", 14) << pad("R/W", 5) << pad("NAME", 36) << "EXTENSIONS\n";
  for (const Json& format : formats.elements()) {
    std::string access;
    access += format["readable"].as_bool() ? 'r' : '-';
    access += format["writable"].as_bool() ? 'w' : '-';
    std::cout << pad(format["id"].as_string(), 14) << pad(format["category"].as_string(), 14) << pad(access, 5)
              << pad(format["name"].as_string(), 36) << join(format["extensions"], " ") << '\n';
  }
  return CVT_OK;
}

// A positional argument that may be a file or a format id.
std::optional<std::string> format_of(const std::string& what, bool allow_format_id, std::string& error) {
  std::error_code ec;
  const bool is_file = what == "-" || std::filesystem::is_regular_file(path_from_utf8(what), ec);
  if (is_file) {
    const auto data = read_file(what);
    if (!data) {
      error = "cannot read '" + what + "'";
      return std::nullopt;
    }
    auto detected = cvt::detect(*data, what == "-" ? std::string() : path_to_utf8(path_from_utf8(what).filename()));
    if (!detected) error = "the format of '" + what + "' was not recognised";
    return detected;
  }
  if (allow_format_id) return what;
  error = "cannot read '" + what + "'";
  return std::nullopt;
}

int command_detect(const Arguments& args) {
  if (args.positional.size() != 1) return fail(CVT_ERR_INVALID_ARGUMENT, "usage: convertible detect <file>");
  std::string error;
  const auto format = format_of(args.positional.front(), false, error);
  if (!format) {
    return fail(error.starts_with("cannot read") ? CVT_ERR_IO : CVT_ERR_UNKNOWN_FORMAT, error);
  }
  std::cout << *format << '\n';
  return CVT_OK;
}

int command_targets(const Arguments& args) {
  if (args.positional.size() != 1) return fail(CVT_ERR_INVALID_ARGUMENT, "usage: convertible targets <file|format>");
  std::string error;
  const auto source = format_of(args.positional.front(), true, error);
  if (!source) return fail(error.starts_with("cannot read") ? CVT_ERR_IO : CVT_ERR_UNKNOWN_FORMAT, error);

  const Json targets = parse_or(cvt::targets_json(*source), Json::array());
  if (args.json) {
    std::cout << cvt::util::dump_json(targets, 2) << '\n';
    return CVT_OK;
  }
  if (targets.empty()) return fail(CVT_ERR_UNKNOWN_FORMAT, "no conversions are available for '" + *source + "'");
  std::cout << pad("ID", 14) << pad("CATEGORY", 14) << pad("NAME", 36) << "NOTES\n";
  for (const Json& target : targets.elements()) {
    std::cout << pad(target["id"].as_string(), 14) << pad(target["category"].as_string(), 14)
              << pad(target["name"].as_string(), 36) << (target["lossy"].as_bool() ? "lossy" : "") << '\n';
  }
  return CVT_OK;
}

void print_options_schema(const std::string& source, const std::string& target) {
  const Json schema = parse_or(cvt::options_schema_json(source, target), Json::array());
  if (schema.empty()) {
    std::cout << "This conversion has no options.\n";
    return;
  }
  for (const Json& option : schema.elements()) {
    std::string range;
    if (option["type"].as_string() == "enum") {
      range = join(option["choices"], " | ");
    } else if (option["min"].is_number() && option["max"].is_number()) {
      range = cvt::util::dump_json(option["min"]) + " .. " + cvt::util::dump_json(option["max"]);
    } else {
      range = option["type"].as_string();
    }
    std::cout << "  " << pad(option["key"].as_string(), 18) << pad(range, 34) << "default "
              << cvt::util::dump_json(option["default"]) << '\n'
              << "  " << pad("", 18) << option["description"].as_string() << '\n';
  }
}

std::string default_extension(const std::string& format_id) {
  const Json formats = parse_or(cvt::formats_json(), Json::array());
  for (const Json& format : formats.elements()) {
    const Json& extensions = format["extensions"];
    const bool matches = format["id"].as_string() == format_id ||
                         std::ranges::any_of(extensions.elements(), [&](const Json& extension) {
                           return extension.as_string() == format_id;
                         });
    if (matches && !extensions.empty()) return extensions[0].as_string();
  }
  return format_id;
}

struct ProgressBar {
  bool enabled = false;
  int last_percent = -1;
  std::string last_stage;

  bool update(double fraction, std::string_view stage) {
    if (!enabled) return false;
    const int percent = static_cast<int>(fraction * 100.0 + 0.5);
    if (percent == last_percent && stage == last_stage) return false;
    last_percent = percent;
    last_stage = std::string(stage);
    constexpr int width = 28;
    const int filled = percent * width / 100;
    std::string bar(static_cast<std::size_t>(filled), '#');
    bar.append(static_cast<std::size_t>(width - filled), '-');
    std::fprintf(stderr, "\r[%s] %3d%% %-12s", bar.c_str(), percent, last_stage.c_str());
    std::fflush(stderr);
    return false;  // never cancels
  }
  void finish() const {
    if (enabled && last_percent >= 0) std::fprintf(stderr, "\r%*s\r", 52, "");
  }
};

int command_convert(const Arguments& args) {
  if (args.positional.size() != 1 || args.target.empty()) {
    return fail(CVT_ERR_INVALID_ARGUMENT, "usage: convertible convert <input> -t <format> [-o <output>]");
  }
  const std::string& input = args.positional.front();
  const bool from_stdin = input == "-";

  std::string error;
  if (args.list_options) {
    std::string source = args.source;
    if (source.empty()) {
      const auto detected = format_of(input, false, error);
      if (!detected) return fail(CVT_ERR_UNKNOWN_FORMAT, error);
      source = *detected;
    }
    print_options_schema(source, args.target);
    return CVT_OK;
  }

  const auto options = options_json(args.options, error);
  if (!options) return fail(CVT_ERR_INVALID_ARGUMENT, error);

  // Decide where the result goes.
  std::string output = args.output;
  if (output.empty()) {
    if (from_stdin) {
      output = "-";
    } else {
      std::filesystem::path derived = path_from_utf8(input);
      derived.replace_extension(path_from_utf8("." + default_extension(args.target)));
      std::error_code ec;
      if (std::filesystem::weakly_canonical(derived, ec) == std::filesystem::weakly_canonical(path_from_utf8(input), ec)) {
        // Same name as the input (e.g. txt -> txt): never clobber the source.
        derived = path_from_utf8(input);
        const std::filesystem::path extension = derived.extension();
        derived.replace_extension();
        derived += path_from_utf8(".converted");
        derived += extension;
      }
      output = path_to_utf8(derived);
    }
  }
  const bool to_stdout = output == "-";
  if (!to_stdout && !args.force) {
    std::error_code ec;
    if (std::filesystem::exists(path_from_utf8(output), ec)) {
      return fail(CVT_ERR_IO, "'" + output + "' already exists (use --force to overwrite)");
    }
  }

  ProgressBar bar;
  bar.enabled = !args.quiet && stderr_is_terminal();
  cvt::ConvertParams params;
  params.target = args.target;
  params.source = args.source;
  params.options_json = *options;
  params.progress = [&bar](double fraction, std::string_view stage) { return bar.update(fraction, stage); };

  const auto report = [&](const cvt::Conversion& result) {
    bar.finish();
    if (!result) {
      return fail(static_cast<int>(result.status()), result.message());
    }
    const Json warnings = parse_or(std::string(result.warnings_json()), Json::array());
    for (const Json& warning : warnings.elements()) {
      if (warning.is_string()) std::cerr << "warning: " << warning.as_string() << '\n';
    }
    return static_cast<int>(CVT_OK);
  };

  if (from_stdin || to_stdout) {
    // Streams: convert in memory.
    const auto data = read_file(input);
    if (!data) return fail(CVT_ERR_IO, "cannot read '" + input + "'");
    if (!from_stdin) params.filename = path_to_utf8(path_from_utf8(input).filename());
    const cvt::Conversion result = cvt::convert(*data, params);
    if (const int status = report(result); status != CVT_OK) return status;
    if (to_stdout) {
      use_binary_stdio();
      const auto bytes = result.data();
      if (!bytes.empty() && std::fwrite(bytes.data(), 1, bytes.size(), stdout) != bytes.size()) {
        return fail(CVT_ERR_IO, "cannot write to standard output");
      }
      std::fflush(stdout);
    } else {
      std::ofstream stream(path_from_utf8(output), std::ios::binary | std::ios::trunc);
      const auto bytes = result.data();
      stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      if (!stream.flush()) return fail(CVT_ERR_IO, "cannot write '" + output + "'");
      if (!args.quiet) std::cerr << input << " -> " << output << " (" << result.format() << ")\n";
    }
    return CVT_OK;
  }

  const cvt::Conversion result = cvt::convert_file(path_from_utf8(input), path_from_utf8(output), params);
  if (const int status = report(result); status != CVT_OK) return status;
  if (!args.quiet) std::cerr << input << " -> " << output << " (" << result.format() << ")\n";
  return CVT_OK;
}

int run(const std::vector<std::string>& argv) {
  if (argv.empty()) {
    std::cerr << usage_text;
    return CVT_ERR_INVALID_ARGUMENT;
  }
  const std::string& command = argv.front();
  const Arguments args = parse_arguments(std::vector<std::string>(argv.begin() + 1, argv.end()));
  if (command == "--help" || command == "-h" || command == "help" || args.help) {
    std::cout << usage_text;
    return CVT_OK;
  }
  if (command == "--version" || command == "-V" || command == "version" || args.version) {
    std::cout << "convertible " << cvt::version() << '\n';
    return CVT_OK;
  }
  if (!args.error.empty()) return fail(CVT_ERR_INVALID_ARGUMENT, args.error);
  if (command == "formats") return command_formats(args);
  if (command == "detect") return command_detect(args);
  if (command == "targets") return command_targets(args);
  if (command == "convert") return command_convert(args);
  return fail(CVT_ERR_INVALID_ARGUMENT, "unknown command '" + command + "' (try --help)");
}

int run_guarded(const std::vector<std::string>& argv) {
  try {
    return run(argv);
  } catch (const std::exception& error) {
    return fail(CVT_ERR_INTERNAL, error.what());
  } catch (...) {
    return fail(CVT_ERR_INTERNAL, "unexpected error");
  }
}

}  // namespace

#if defined(_WIN32)
// Windows hands narrow `main` arguments in the active code page; take the
// UTF-16 command line instead and convert it to UTF-8 ourselves.
int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    const int length = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(length > 0 ? static_cast<std::size_t>(length - 1) : 0, '\0');
    if (length > 1) WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, utf8.data(), length, nullptr, nullptr);
    args.push_back(std::move(utf8));
  }
  return run_guarded(args);
}
#else
int main(int argc, char** argv) {
  return run_guarded(std::vector<std::string>(argv + std::min(argc, 1), argv + argc));
}
#endif
