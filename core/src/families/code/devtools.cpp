// Convertible core - code family: developer interchange formats.
//
//   curl            a cURL command line  <-> Data (method, url, headers, body ...)
//   http            raw HTTP/1.1 request text <-> Data
//   fetch-js        Data -> a JavaScript fetch() snippet
//   python-requests Data -> a Python `requests` snippet
//   cron            crontab lines / cron expressions -> English descriptions (Data, text)
//   datetime        Unix timestamps, ISO 8601 and RFC 2822 dates, converted line by line
//   uuid-info       what a UUID or ULID contains (version, variant, time)
//   color           one colour in hex, rgb(), hsl(), hwb(), oklch(), oklab() or by name
// Diffs, ignore files and .editorconfig/.npmrc live in diff.cpp.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <numbers>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "engine/registry.hpp"
#include "families/code/code.hpp"
#include "families/code/devtools.hpp"
#include "util/base64.hpp"
#include "util/hex.hpp"
#include "util/json.hpp"
#include "util/text.hpp"

namespace cvt::code {

namespace {

Result<std::string> input_text(const ReadContext& ctx) {
  CVT_TRY(ctx.limits.check_decompressed(static_cast<std::uint64_t>(ctx.input.size()) * 3));
  const util::EncodingGuess guess = util::guess_encoding(ctx.input);
  return util::decode_text(ctx.input, guess.encoding).text;
}

std::string json_string(std::string_view text) { return util::dump_json(Json(std::string(text))); }

// ---------------------------------------------------------------------------- shell words

/// Splits a POSIX shell command line into words: quotes, $'...', backslash
/// escapes and line continuations (including cmd.exe '^' at line ends).
Result<std::vector<std::string>> shell_words(std::string_view text) {
  std::vector<std::string> words;
  std::string current;
  bool in_word = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if ((c == '\\' || c == '^') && i + 1 < text.size() && (text[i + 1] == '\n' || text[i + 1] == '\r')) {
      while (i + 1 < text.size() && (text[i + 1] == '\n' || text[i + 1] == '\r')) ++i;
      continue;
    }
    if (util::is_ascii_space(c)) {
      if (in_word) words.push_back(std::move(current));
      current.clear();
      in_word = false;
      continue;
    }
    in_word = true;
    if (c == '\'') {
      const std::size_t end = text.find('\'', i + 1);
      if (end == std::string_view::npos) return fail(Status::Decode, "unterminated single quote");
      current.append(text.substr(i + 1, end - i - 1));
      i = end;
    } else if (c == '$' && i + 1 < text.size() && text[i + 1] == '\'') {
      std::size_t k = i + 2;
      for (; k < text.size() && text[k] != '\''; ++k) {
        if (text[k] != '\\' || k + 1 >= text.size()) {
          current.push_back(text[k]);
          continue;
        }
        const char e = text[++k];
        switch (e) {
          case 'n': current.push_back('\n'); break;
          case 't': current.push_back('\t'); break;
          case 'r': current.push_back('\r'); break;
          case '0': current.push_back('\0'); break;
          case 'x':
            if (k + 2 < text.size() && util::hex_digit_value(text[k + 1]) >= 0 && util::hex_digit_value(text[k + 2]) >= 0) {
              current.push_back(static_cast<char>(util::hex_digit_value(text[k + 1]) * 16 + util::hex_digit_value(text[k + 2])));
              k += 2;
            }
            break;
          default: current.push_back(e);
        }
      }
      if (k >= text.size()) return fail(Status::Decode, "unterminated $'...' string");
      i = k;
    } else if (c == '"') {
      std::size_t k = i + 1;
      for (; k < text.size() && text[k] != '"'; ++k) {
        if (text[k] == '\\' && k + 1 < text.size() && std::string_view("\"\\$`\n").find(text[k + 1]) != std::string_view::npos) {
          ++k;
          if (text[k] == '\n') continue;
        }
        current.push_back(text[k]);
      }
      if (k >= text.size()) return fail(Status::Decode, "unterminated double quote");
      i = k;
    } else if (c == '\\' && i + 1 < text.size()) {
      current.push_back(text[++i]);
    } else {
      current.push_back(c);
    }
  }
  if (in_word) words.push_back(std::move(current));
  return words;
}

std::string shell_quote(std::string_view word) {
  if (!word.empty() && std::ranges::all_of(word, [](char c) {
        return util::is_ascii_alpha(c) || util::is_ascii_digit(c) || std::string_view("-_./:=@%+,").find(c) != std::string_view::npos;
      })) {
    return std::string(word);
  }
  return "'" + util::replace_all(word, "'", "'\\''") + "'";
}

std::string url_encode_component(std::string_view text) {
  std::string out;
  for (const char c : text) {
    if (util::is_ascii_alpha(c) || util::is_ascii_digit(c) || std::string_view("-._~").find(c) != std::string_view::npos) {
      out.push_back(c);
    } else {
      out += std::format("%{:02X}", static_cast<unsigned char>(c));
    }
  }
  return out;
}

void set_header(Json& request, std::string_view name, std::string_view value) {
  if (request.find("headers") == nullptr) request["headers"] = Json::object();
  // Header names are case-insensitive: replace an existing spelling.
  Json& headers = request["headers"];
  for (auto& [key, existing] : headers.items()) {
    if (util::iequals(key, name)) {
      existing = std::string(value);
      return;
    }
  }
  headers[std::string(name)] = std::string(value);
}

const Json* header(const Json& request, std::string_view name) {
  const Json* headers = request.find("headers");
  if (headers == nullptr || !headers->is_object()) return nullptr;
  for (const auto& [key, value] : headers->items()) {
    if (util::iequals(key, name)) return &value;
  }
  return nullptr;
}

std::string string_field(const Json& object, std::string_view key, std::string_view fallback = {}) {
  const Json* value = object.find(key);
  return value != nullptr && value->is_string() ? value->as_string() : std::string(fallback);
}

Result<void> check_request(const Json& request) {
  if (!request.is_object()) return fail(Status::Encode, "an HTTP request is written from an object");
  if (string_field(request, "url").empty()) return fail(Status::Encode, "the request has no \"url\"");
  const Json* headers = request.find("headers");
  if (headers != nullptr && !headers->is_object()) return fail(Status::Encode, "\"headers\" must be an object");
  return {};
}

}  // namespace

// ---------------------------------------------------------------------------- cURL

Result<Json> parse_curl(std::string_view command) {
  CVT_TRY_ASSIGN(std::vector<std::string> words, shell_words(command));
  if (words.empty() || !(words[0] == "curl" || words[0].ends_with("/curl") || util::iequals(words[0], "curl.exe"))) {
    return fail(Status::Decode, "the command does not start with 'curl'");
  }
  Json request = Json::object();
  std::string method, url;
  std::vector<std::string> data;
  Json form = Json::array();
  Json options = Json::object();
  bool get = false;
  const auto value_of = [&](std::size_t& i, std::string_view flag) -> Result<std::string> {
    if (i + 1 >= words.size()) return fail(Status::Decode, "option {} needs a value", flag);
    return words[++i];
  };
  for (std::size_t i = 1; i < words.size(); ++i) {
    std::string word = words[i];
    // Short options with an attached value: -XPOST, -H'...'
    if (word.size() > 2 && word[0] == '-' && word[1] != '-' && std::string_view("XHdFuAebmox").find(word[1]) != std::string_view::npos) {
      words.insert(words.begin() + static_cast<std::ptrdiff_t>(i) + 1, word.substr(2));
      word = word.substr(0, 2);
    } else if (word.size() > 2 && word[0] == '-' && word[1] != '-') {
      // Bundled flags without values: -sSL
      for (std::size_t k = 1; k < word.size(); ++k) {
        const char f = word[k];
        if (f == 'L') options["follow_redirects"] = true;
        if (f == 'k') options["insecure"] = true;
        if (f == 'G') get = true;
        if (f == 'I') method = "HEAD";
      }
      continue;
    }
    if (word == "-X" || word == "--request") {
      CVT_TRY_ASSIGN(method, value_of(i, word));
    } else if (word == "-H" || word == "--header") {
      CVT_TRY_ASSIGN(const std::string line, value_of(i, word));
      const std::size_t colon = line.find(':');
      if (colon == std::string::npos) return fail(Status::Decode, "header '{}' has no ':'", line);
      set_header(request, util::trim(std::string_view(line).substr(0, colon)), util::trim(std::string_view(line).substr(colon + 1)));
    } else if (word == "-d" || word == "--data" || word == "--data-ascii" || word == "--data-binary" || word == "--data-raw") {
      CVT_TRY_ASSIGN(std::string value, value_of(i, word));
      if (word != "--data-binary") value = util::replace_all(util::replace_all(value, "\r", ""), "\n", "");
      data.push_back(std::move(value));
    } else if (word == "--data-urlencode") {
      CVT_TRY_ASSIGN(const std::string value, value_of(i, word));
      const std::size_t equals = value.find('=');
      data.push_back(equals == std::string::npos ? url_encode_component(value)
                                                 : value.substr(0, equals + 1) + url_encode_component(value.substr(equals + 1)));
    } else if (word == "--json") {
      CVT_TRY_ASSIGN(std::string value, value_of(i, word));
      data.push_back(std::move(value));
      if (header(request, "Content-Type") == nullptr) set_header(request, "Content-Type", "application/json");
      if (header(request, "Accept") == nullptr) set_header(request, "Accept", "application/json");
    } else if (word == "-F" || word == "--form" || word == "--form-string") {
      CVT_TRY_ASSIGN(const std::string value, value_of(i, word));
      const std::size_t equals = value.find('=');
      Json field = Json::object();
      field["name"] = value.substr(0, equals);
      field["value"] = equals == std::string::npos ? std::string() : value.substr(equals + 1);
      form.push_back(std::move(field));
    } else if (word == "-u" || word == "--user") {
      CVT_TRY_ASSIGN(const std::string value, value_of(i, word));
      const std::size_t colon = value.find(':');
      Json auth = Json::object();
      auth["user"] = value.substr(0, colon);
      auth["password"] = colon == std::string::npos ? std::string() : value.substr(colon + 1);
      request["auth"] = std::move(auth);
    } else if (word == "-A" || word == "--user-agent") {
      CVT_TRY_ASSIGN(const std::string value, value_of(i, word));
      set_header(request, "User-Agent", value);
    } else if (word == "-e" || word == "--referer") {
      CVT_TRY_ASSIGN(const std::string value, value_of(i, word));
      set_header(request, "Referer", value);
    } else if (word == "-b" || word == "--cookie") {
      CVT_TRY_ASSIGN(const std::string value, value_of(i, word));
      set_header(request, "Cookie", value);
    } else if (word == "--url") {
      CVT_TRY_ASSIGN(url, value_of(i, word));
    } else if (word == "-L" || word == "--location") {
      options["follow_redirects"] = true;
    } else if (word == "-k" || word == "--insecure") {
      options["insecure"] = true;
    } else if (word == "--compressed") {
      options["compressed"] = true;
    } else if (word == "-G" || word == "--get") {
      get = true;
    } else if (word == "-I" || word == "--head") {
      method = "HEAD";
    } else if (word == "-m" || word == "--max-time") {
      CVT_TRY_ASSIGN(const std::string value, value_of(i, word));
      options["timeout_seconds"] = util::parse_double(value).value_or(0.0);
    } else if (word == "-o" || word == "--output" || word == "-x" || word == "--proxy" || word == "--connect-timeout" ||
               word == "-w" || word == "--write-out" || word == "--retry" || word == "--cacert" || word == "-E" ||
               word == "--cert" || word == "--key") {
      CVT_TRY(value_of(i, word));  // values that do not describe the request itself
    } else if (word.starts_with("-")) {
      // Flags without values (-s, -S, -v, -i, --http2, ...) do not change the request.
    } else if (url.empty()) {
      url = word;
    } else {
      return fail(Status::Decode, "unexpected argument '{}'", word);
    }
  }
  if (url.empty()) return fail(Status::Decode, "the curl command has no URL");
  if (url.find("://") == std::string::npos) url = "http://" + url;
  std::string body = util::join(data, "&");
  if (get && !body.empty()) {
    url += (url.find('?') == std::string::npos ? "?" : "&") + body;
    body.clear();
  }
  if (method.empty()) method = (!body.empty() || !form.empty()) ? "POST" : "GET";
  if (!body.empty() && header(request, "Content-Type") == nullptr && form.empty()) {
    set_header(request, "Content-Type", "application/x-www-form-urlencoded");
  }
  Json out = Json::object();
  out["method"] = util::to_upper(method);
  out["url"] = url;
  out["headers"] = request.find("headers") != nullptr ? request["headers"] : Json::object();
  if (!body.empty()) out["body"] = body;
  if (!form.empty()) out["form"] = std::move(form);
  if (request.find("auth") != nullptr) out["auth"] = request["auth"];
  if (!options.empty()) out["options"] = std::move(options);
  return out;
}

Result<std::string> to_curl(const Json& request) {
  CVT_TRY(check_request(request));
  const std::string method = util::to_upper(string_field(request, "method", "GET"));
  const std::string body = string_field(request, "body");
  const Json* form = request.find("form");
  std::vector<std::string> parts = {"curl"};
  const bool implied = method == "GET" || (method == "POST" && (!body.empty() || (form != nullptr && !form->empty())));
  if (method == "HEAD") {
    parts.emplace_back("--head");
  } else if (!implied) {
    parts.push_back("-X " + method);
  }
  parts.push_back(shell_quote(string_field(request, "url")));
  if (const Json* headers = request.find("headers"); headers != nullptr) {
    for (const auto& [name, value] : headers->items()) parts.push_back("-H " + shell_quote(name + ": " + (value.is_string() ? value.as_string() : util::dump_json(value))));
  }
  if (const Json* auth = request.find("auth"); auth != nullptr && auth->is_object()) {
    parts.push_back("-u " + shell_quote(string_field(*auth, "user") + ":" + string_field(*auth, "password")));
  }
  if (!body.empty()) parts.push_back("--data-raw " + shell_quote(body));
  if (form != nullptr && form->is_array()) {
    for (const Json& field : form->elements()) parts.push_back("-F " + shell_quote(string_field(field, "name") + "=" + string_field(field, "value")));
  }
  if (const Json* options = request.find("options"); options != nullptr && options->is_object()) {
    if (const Json* v = options->find("follow_redirects"); v != nullptr && v->as_bool()) parts.emplace_back("-L");
    if (const Json* v = options->find("insecure"); v != nullptr && v->as_bool()) parts.emplace_back("-k");
    if (const Json* v = options->find("compressed"); v != nullptr && v->as_bool()) parts.emplace_back("--compressed");
  }
  return util::join(parts, " \\\n  ") + "\n";
}

// ---------------------------------------------------------------------------- raw HTTP

namespace {
struct UrlParts {
  std::string scheme, authority, target;
};
UrlParts split_url(std::string_view url) {
  UrlParts parts;
  const std::size_t scheme_end = url.find("://");
  std::string_view rest = url;
  if (scheme_end != std::string_view::npos) {
    parts.scheme = util::to_lower(url.substr(0, scheme_end));
    rest = url.substr(scheme_end + 3);
  }
  const std::size_t slash = rest.find_first_of("/?#");
  parts.authority = std::string(rest.substr(0, slash));
  parts.target = slash == std::string_view::npos ? "/" : std::string(rest.substr(slash));
  if (const std::size_t hash = parts.target.find('#'); hash != std::string::npos) parts.target.resize(hash);
  if (parts.target.empty() || parts.target[0] == '?') parts.target.insert(parts.target.begin(), '/');
  return parts;
}
}  // namespace

Result<Json> parse_http_request(std::string_view text) {
  const std::string normal = util::convert_line_endings(text, util::LineEnding::Lf);
  const std::string_view all = normal;
  const std::size_t first_end = all.find('\n');
  const std::string_view first = util::trim(all.substr(0, first_end));
  const std::vector<std::string_view> start = util::split(first, ' ', false);
  if (start.size() < 2 || !std::ranges::all_of(start[0], [](char c) { return c >= 'A' && c <= 'Z'; })) {
    return fail(Status::Decode, "not an HTTP request line: '{}'", first);
  }
  Json request = Json::object();
  request["method"] = std::string(start[0]);
  Json headers = Json::object();
  std::size_t at = first_end == std::string_view::npos ? all.size() : first_end + 1;
  std::string host;
  while (at < all.size()) {
    const std::size_t end = all.find('\n', at);
    const std::string_view line = all.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
    at = end == std::string_view::npos ? all.size() : end + 1;
    if (util::trim(line).empty()) break;
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) return fail(Status::Decode, "malformed header line '{}'", line);
    const std::string name(util::trim(line.substr(0, colon)));
    const std::string value(util::trim(line.substr(colon + 1)));
    if (util::iequals(name, "Host")) {
      host = value;
      continue;
    }
    headers[name] = value;
  }
  std::string target(start[1]);
  if (target.find("://") == std::string::npos) {
    if (host.empty()) return fail(Status::Decode, "the request has neither an absolute URL nor a Host header");
    const bool tls = host.ends_with(":443") || !host.ends_with(":80");
    target = std::string(tls ? "https://" : "http://") + host + target;
  }
  request["url"] = target;
  request["headers"] = std::move(headers);
  std::string body(all.substr(std::min(at, all.size())));
  if (!body.empty()) request["body"] = std::move(body);
  return request;
}

Result<std::string> to_http_request(const Json& request) {
  CVT_TRY(check_request(request));
  const UrlParts url = split_url(string_field(request, "url"));
  std::string out = std::format("{} {} HTTP/1.1\r\nHost: {}\r\n", util::to_upper(string_field(request, "method", "GET")),
                                url.target, url.authority);
  if (const Json* headers = request.find("headers"); headers != nullptr) {
    for (const auto& [name, value] : headers->items()) out += name + ": " + (value.is_string() ? value.as_string() : util::dump_json(value)) + "\r\n";
  }
  if (const Json* auth = request.find("auth"); auth != nullptr && auth->is_object() && header(request, "Authorization") == nullptr) {
    out += "Authorization: Basic " +
           util::base64_encode(as_bytes(string_field(*auth, "user") + ":" + string_field(*auth, "password"))) + "\r\n";
  }
  const std::string body = string_field(request, "body");
  if (!body.empty() && header(request, "Content-Length") == nullptr) out += std::format("Content-Length: {}\r\n", body.size());
  out += "\r\n" + body;
  return out;
}

// ---------------------------------------------------------------------------- snippets

Result<std::string> to_fetch(const Json& request) {
  CVT_TRY(check_request(request));
  const std::string method = util::to_upper(string_field(request, "method", "GET"));
  std::vector<std::string> fields;
  if (method != "GET") fields.push_back("  method: " + json_string(method));
  std::vector<std::string> headers;
  if (const Json* h = request.find("headers"); h != nullptr) {
    for (const auto& [name, value] : h->items()) headers.push_back("    " + json_string(name) + ": " + json_string(value.is_string() ? value.as_string() : util::dump_json(value)));
  }
  if (const Json* auth = request.find("auth"); auth != nullptr && auth->is_object()) {
    headers.push_back("    \"Authorization\": \"Basic \" + btoa(" +
                      json_string(string_field(*auth, "user") + ":" + string_field(*auth, "password")) + ")");
  }
  if (!headers.empty()) fields.push_back("  headers: {\n" + util::join(headers, ",\n") + "\n  }");
  const Json* form = request.find("form");
  std::string prelude;
  if (form != nullptr && form->is_array() && !form->empty()) {
    prelude = "const form = new FormData();\n";
    for (const Json& field : form->elements()) {
      prelude += "form.append(" + json_string(string_field(field, "name")) + ", " + json_string(string_field(field, "value")) + ");\n";
    }
    fields.emplace_back("  body: form");
  } else if (const std::string body = string_field(request, "body"); !body.empty()) {
    fields.push_back("  body: " + json_string(body));
  }
  if (const Json* options = request.find("options"); options != nullptr && options->is_object()) {
    if (const Json* v = options->find("follow_redirects"); v != nullptr && !v->as_bool(true)) fields.emplace_back("  redirect: \"manual\"");
  }
  std::string out = prelude + "const response = await fetch(" + json_string(string_field(request, "url"));
  if (!fields.empty()) out += ", {\n" + util::join(fields, ",\n") + "\n}";
  out += ");\nconst text = await response.text();\n";
  return out;
}

Result<std::string> to_python_requests(const Json& request) {
  CVT_TRY(check_request(request));
  const std::string method = util::to_lower(string_field(request, "method", "GET"));
  constexpr std::array<std::string_view, 7> shortcuts = {"get", "post", "put", "patch", "delete", "head", "options"};
  const bool shortcut = std::ranges::find(shortcuts, method) != shortcuts.end();
  std::vector<std::string> args;
  if (!shortcut) args.push_back("    " + json_string(util::to_upper(method)));
  args.push_back("    " + json_string(string_field(request, "url")));
  if (const Json* h = request.find("headers"); h != nullptr && !h->empty()) {
    std::vector<std::string> pairs;
    for (const auto& [name, value] : h->items()) pairs.push_back("        " + json_string(name) + ": " + json_string(value.is_string() ? value.as_string() : util::dump_json(value)) + ",");
    args.push_back("    headers={\n" + util::join(pairs, "\n") + "\n    }");
  }
  if (const Json* auth = request.find("auth"); auth != nullptr && auth->is_object()) {
    args.push_back("    auth=(" + json_string(string_field(*auth, "user")) + ", " + json_string(string_field(*auth, "password")) + ")");
  }
  if (const Json* form = request.find("form"); form != nullptr && form->is_array() && !form->empty()) {
    std::vector<std::string> pairs;
    for (const Json& field : form->elements()) {
      pairs.push_back("        " + json_string(string_field(field, "name")) + ": (None, " + json_string(string_field(field, "value")) + "),");
    }
    args.push_back("    files={\n" + util::join(pairs, "\n") + "\n    }");
  } else if (const std::string body = string_field(request, "body"); !body.empty()) {
    args.push_back("    data=" + json_string(body));
  }
  if (const Json* options = request.find("options"); options != nullptr && options->is_object()) {
    if (const Json* v = options->find("insecure"); v != nullptr && v->as_bool()) args.emplace_back("    verify=False");
    if (const Json* v = options->find("timeout_seconds"); v != nullptr && v->is_number()) {
      args.push_back("    timeout=" + util::format_double(v->as_double()));
    }
  }
  return std::string("import requests\n\nresponse = requests.") + (shortcut ? method : "request") + "(\n" +
         util::join(args, ",\n") + ",\n)\nprint(response.status_code)\n";
}

// ---------------------------------------------------------------------------- cron

namespace {

constexpr std::array<std::string_view, 12> month_names = {"January", "February", "March",     "April",   "May",      "June",
                                                          "July",    "August",   "September", "October", "November", "December"};
constexpr std::array<std::string_view, 7> day_names = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

struct CronField {
  std::string_view name;
  int low, high;
  std::set<int> values;
  bool any = false;    // '*'
  int step = 0;        // '*/n' (0 = none)
  std::string source;
};

Result<int> cron_number(std::string_view token, const CronField& field) {
  if (field.name == "month" || field.name == "day-of-week") {
    for (std::size_t i = 0; i < (field.name == "month" ? 12u : 7u); ++i) {
      const std::string_view full = field.name == "month" ? month_names[i] : day_names[i];
      if (token.size() == 3 && util::iequals(token, full.substr(0, 3))) return static_cast<int>(field.name == "month" ? i + 1 : i);
    }
  }
  const auto value = util::parse_int(token);
  if (!value || *value < field.low || *value > field.high) {
    return fail(Status::Decode, "'{}' is not a valid {} (allowed {}-{})", token, field.name, field.low, field.high);
  }
  return static_cast<int>(*value);
}

Result<void> parse_cron_field(std::string_view text, CronField& field) {
  field.source = std::string(text);
  if (text == "*" || text == "?") {
    field.any = true;
    for (int v = field.low; v <= field.high; ++v) field.values.insert(v);
    return {};
  }
  for (const std::string_view part : util::split(text, ',', true)) {
    if (part.empty()) return fail(Status::Decode, "empty list element in the {} field", field.name);
    std::string_view range = part;
    int step = 1;
    if (const std::size_t slash = part.find('/'); slash != std::string_view::npos) {
      const auto s = util::parse_int(part.substr(slash + 1));
      if (!s || *s < 1 || *s > field.high) return fail(Status::Decode, "bad step '{}' in the {} field", part, field.name);
      step = static_cast<int>(*s);
      range = part.substr(0, slash);
    }
    int from = field.low, to = field.high;
    if (range != "*") {
      const std::size_t dash = range.find('-');
      CVT_TRY_ASSIGN(from, cron_number(range.substr(0, dash), field));
      to = from;
      if (dash != std::string_view::npos) {
        CVT_TRY_ASSIGN(to, cron_number(range.substr(dash + 1), field));
      } else if (step > 1) {
        to = field.high;  // "5/15" means from 5 in steps of 15
      }
      if (to < from) return fail(Status::Decode, "range '{}' runs backwards in the {} field", range, field.name);
    } else if (part.find('/') != std::string_view::npos) {
      field.step = step;
    }
    for (int v = from; v <= to; v += step) field.values.insert(v);
  }
  if (field.name == "day-of-week" && field.values.contains(7)) {  // 7 is Sunday too
    field.values.erase(7);
    field.values.insert(0);
  }
  return {};
}

std::string join_words(const std::vector<std::string>& items) {
  if (items.empty()) return {};
  if (items.size() == 1) return items[0];
  std::string out;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i > 0) out += i + 1 == items.size() ? " and " : ", ";
    out += items[i];
  }
  return out;
}

/// "1 through 5", "Monday, Wednesday and Friday": consecutive runs collapse.
std::string describe_values(const CronField& field, const std::function<std::string(int)>& name) {
  std::vector<std::string> parts;
  std::vector<int> values(field.values.begin(), field.values.end());
  for (std::size_t i = 0; i < values.size();) {
    std::size_t j = i;
    while (j + 1 < values.size() && values[j + 1] == values[j] + 1) ++j;
    if (j - i >= 2) {
      parts.push_back(name(values[i]) + " through " + name(values[j]));
    } else {
      for (std::size_t k = i; k <= j; ++k) parts.push_back(name(values[k]));
    }
    i = j + 1;
  }
  return join_words(parts);
}

std::string two_digits(int v) { return std::format("{:02}", v); }

struct CronSchedule {
  CronField minute{"minute", 0, 59, {}, false, 0, {}};
  CronField hour{"hour", 0, 23, {}, false, 0, {}};
  CronField dom{"day-of-month", 1, 31, {}, false, 0, {}};
  CronField month{"month", 1, 12, {}, false, 0, {}};
  CronField dow{"day-of-week", 0, 7, {}, false, 0, {}};
};

Result<CronSchedule> parse_schedule(std::string_view expression) {
  std::string_view expr = util::trim(expression);
  const std::pair<std::string_view, std::string_view> macros[] = {
      {"@yearly", "0 0 1 1 *"}, {"@annually", "0 0 1 1 *"}, {"@monthly", "0 0 1 * *"}, {"@weekly", "0 0 * * 0"},
      {"@daily", "0 0 * * *"},  {"@midnight", "0 0 * * *"}, {"@hourly", "0 * * * *"},
  };
  for (const auto& [macro, replacement] : macros) {
    if (util::iequals(expr, macro)) expr = replacement;
  }
  const std::vector<std::string_view> fields = util::split(expr, ' ', false);
  std::vector<std::string_view> parts;
  for (const std::string_view f : fields) {
    if (!util::trim(f).empty()) parts.push_back(util::trim(f));
  }
  if (parts.size() != 5) return fail(Status::Decode, "a cron expression has 5 fields; '{}' has {}", expression, parts.size());
  for (const std::string_view p : parts) {
    for (const std::string_view item : util::split(p, ',', true)) {
      // Quartz-style L (last), W (weekday) and # (nth weekday): "L", "15W", "5L", "2#3".
      const bool quartz = item.find('#') != std::string_view::npos ||
                          ((item.ends_with('L') || item.ends_with('W')) &&
                           std::ranges::all_of(item.substr(0, item.size() - 1), [](char c) { return util::is_ascii_digit(c) || c == 'L'; }));
      if (quartz) return fail(Status::Decode, "the Quartz extensions L, W and # are not supported ('{}')", p);
    }
  }
  CronSchedule s;
  CVT_TRY(parse_cron_field(parts[0], s.minute));
  CVT_TRY(parse_cron_field(parts[1], s.hour));
  CVT_TRY(parse_cron_field(parts[2], s.dom));
  CVT_TRY(parse_cron_field(parts[3], s.month));
  CVT_TRY(parse_cron_field(parts[4], s.dow));
  return s;
}

std::string describe_schedule(const CronSchedule& s) {
  std::string time;
  const auto number = [](int v) { return std::to_string(v); };
  if (s.minute.values.size() == 1 && !s.hour.any && s.hour.step == 0 && s.hour.values.size() <= 6) {
    std::vector<std::string> times;
    for (const int h : s.hour.values) times.push_back(two_digits(h) + ":" + two_digits(*s.minute.values.begin()));
    time = "At " + join_words(times);
  } else {
    std::string minute;
    if (s.minute.any) {
      minute = "every minute";
    } else if (s.minute.step > 0) {
      minute = std::format("every {} minutes", s.minute.step);
    } else {
      minute = (s.minute.values.size() == 1 ? "minute " : "minutes ") + describe_values(s.minute, number);
    }
    std::string hour;
    if (s.hour.any) {
      hour = "";
    } else if (s.hour.step > 0) {
      hour = std::format("every {} hours", s.hour.step);
    } else {
      hour = describe_values(s.hour, [](int h) { return two_digits(h) + ":00"; });
      hour = (s.hour.values.size() == 1 ? "the hour " : "the hours ") + hour;
    }
    if (s.minute.any) {
      time = hour.empty() ? "Every minute" : "Every minute of " + hour;
    } else if (s.minute.step > 0) {
      time = "Every " + minute.substr(6) + (hour.empty() ? "" : " during " + hour);
      time[0] = 'E';
    } else {
      time = "At " + minute + " past " + (hour.empty() ? "every hour" : hour);
    }
  }
  std::vector<std::string> days;
  if (!s.dom.any) {
    days.push_back((s.dom.step > 0 ? std::format("every {} days", s.dom.step)
                                   : "day-of-month " + describe_values(s.dom, number)));
  }
  if (!s.dow.any) {
    days.push_back(s.dow.step > 0 ? std::format("every {} days of the week", s.dow.step)
                                  : describe_values(s.dow, [](int d) { return std::string(day_names[static_cast<std::size_t>(d)]); }));
  }
  std::string out = time;
  if (!days.empty()) out += " on " + (days.size() == 2 ? days[0] + " or " + days[1] : days[0]);
  if (!s.month.any) {
    out += " in " + describe_values(s.month, [](int m) { return std::string(month_names[static_cast<std::size_t>(m - 1)]); });
  }
  return out + ".";
}

Json values_json(const CronField& field) {
  Json out = Json::array();
  for (const int v : field.values) out.push_back(v);
  return out;
}

}  // namespace

Result<std::string> describe_cron(std::string_view expression) {
  if (util::iequals(util::trim(expression), "@reboot")) return std::string("At system startup.");
  CVT_TRY_ASSIGN(const CronSchedule schedule, parse_schedule(expression));
  return describe_schedule(schedule);
}

Result<Json> cron_to_data(std::string_view expression) {
  Json out = Json::object();
  out["expression"] = std::string(util::trim(expression));
  CVT_TRY_ASSIGN(const std::string description, describe_cron(expression));
  out["description"] = description;
  if (util::iequals(util::trim(expression), "@reboot")) return out;
  CVT_TRY_ASSIGN(const CronSchedule s, parse_schedule(expression));
  out["minute"] = values_json(s.minute);
  out["hour"] = values_json(s.hour);
  out["day_of_month"] = values_json(s.dom);
  out["month"] = values_json(s.month);
  out["day_of_week"] = values_json(s.dow);
  return out;
}

namespace {

// A crontab: comments, VAR=value lines, and "<5 fields or @macro> command".
Result<Json> crontab_to_data(std::string_view text) {
  Json entries = Json::array();
  std::size_t number = 0;
  for (std::string_view line : util::split_lines(text)) {
    ++number;
    line = util::trim(line);
    if (line.empty() || line.starts_with('#')) continue;
    const std::size_t equals = line.find('=');
    if (equals != std::string_view::npos && line.find(' ') > equals) continue;  // environment assignment
    std::string_view schedule, command;
    if (line.starts_with('@')) {
      const std::size_t space = line.find_first_of(" \t");
      schedule = line.substr(0, space);
      command = space == std::string_view::npos ? std::string_view{} : util::trim(line.substr(space));
    } else {
      std::size_t at = 0;
      for (int field = 0; field < 5 && at != std::string_view::npos; ++field) {
        at = line.find_first_not_of(" \t", at);
        at = at == std::string_view::npos ? at : line.find_first_of(" \t", at);
      }
      schedule = line.substr(0, at);
      command = at == std::string_view::npos ? std::string_view{} : util::trim(line.substr(at));
    }
    auto entry = cron_to_data(schedule);
    if (!entry) return fail(Status::Decode, "line {}: {}", number, entry.error().message);
    if (!command.empty()) (*entry)["command"] = std::string(command);
    entries.push_back(std::move(*entry));
  }
  if (entries.empty()) return fail(Status::Decode, "no cron schedule found");
  if (entries.size() == 1) return std::move(entries[0]);
  return entries;
}

}  // namespace

// ---------------------------------------------------------------------------- dates

namespace {

// Howard Hinnant's algorithms: days since 1970-01-01 of a proleptic Gregorian date and back.
constexpr std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}
struct Civil {
  std::int64_t year;
  unsigned month, day;
};
constexpr Civil civil_from_days(std::int64_t z) {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const auto doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  return {y + (m <= 2), m, d};
}
static_assert(days_from_civil(1970, 1, 1) == 0 && days_from_civil(2000, 3, 1) == 11017);

constexpr unsigned days_in_month(std::int64_t y, unsigned m) {
  constexpr std::array<unsigned, 12> days = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
  return m == 2 && leap ? 29 : days[m - 1];
}

constexpr std::int64_t min_seconds = -62135596800;   // 0001-01-01T00:00:00Z
constexpr std::int64_t max_seconds = 253402300799;   // 9999-12-31T23:59:59Z

std::optional<int> digits(std::string_view text, std::size_t at, std::size_t count) {
  if (at + count > text.size()) return std::nullopt;
  int value = 0;
  for (std::size_t i = at; i < at + count; ++i) {
    if (!util::is_ascii_digit(text[i])) return std::nullopt;
    value = value * 10 + (text[i] - '0');
  }
  return value;
}

Result<Instant> make_instant(std::int64_t y, int mo, int d, int h, int mi, int s, std::int32_t ns, std::int32_t offset,
                             bool has_offset) {
  if (mo < 1 || mo > 12 || d < 1 || d > static_cast<int>(days_in_month(y, static_cast<unsigned>(mo))) || h > 24 || mi > 59 ||
      s > 60 || (h == 24 && (mi != 0 || s != 0))) {
    return fail(Status::Decode, "the date or time is out of range");
  }
  Instant out;
  out.seconds = days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 86400 + h * 3600 + mi * 60 +
                std::min(s, 59) - static_cast<std::int64_t>(offset) * 60;
  out.nanoseconds = ns;
  if (has_offset) out.offset_minutes = offset;
  return out;
}

Result<Instant> parse_iso(std::string_view t) {
  // YYYY-MM-DD or YYYYMMDD, then optional T/space and time.
  const auto year = digits(t, 0, 4);
  if (!year) return fail(Status::Decode, "not an ISO 8601 date");
  std::size_t at = 4;
  const bool extended = t.size() > 4 && t[4] == '-';
  if (extended) ++at;
  const auto month = digits(t, at, 2);
  at += 2;
  if (extended && (at >= t.size() || t[at] != '-')) return fail(Status::Decode, "not an ISO 8601 date");
  if (extended) ++at;
  const auto day = digits(t, at, 2);
  at += 2;
  if (!month || !day) return fail(Status::Decode, "not an ISO 8601 date");
  int hour = 0, minute = 0, second = 0;
  std::int32_t nanos = 0, offset = 0;
  bool has_offset = false;
  if (at < t.size() && (t[at] == 'T' || t[at] == 't' || t[at] == ' ')) {
    ++at;
    const auto h = digits(t, at, 2);
    if (!h) return fail(Status::Decode, "malformed ISO 8601 time");
    hour = *h;
    at += 2;
    if (at < t.size() && t[at] == ':') ++at;
    if (const auto m = digits(t, at, 2)) {
      minute = *m;
      at += 2;
      if (at < t.size() && t[at] == ':') ++at;
      if (const auto s = digits(t, at, 2)) {
        second = *s;
        at += 2;
        if (at < t.size() && (t[at] == '.' || t[at] == ',')) {
          ++at;
          int scale = 100'000'000;
          while (at < t.size() && util::is_ascii_digit(t[at])) {
            nanos += (t[at] - '0') * scale;
            scale /= 10;
            ++at;
          }
        }
      }
    }
    if (at < t.size() && (t[at] == 'Z' || t[at] == 'z')) {
      has_offset = true;
      ++at;
    } else if (at < t.size() && (t[at] == '+' || t[at] == '-')) {
      const int sign = t[at] == '-' ? -1 : 1;
      ++at;
      const auto oh = digits(t, at, 2);
      if (!oh) return fail(Status::Decode, "malformed UTC offset");
      at += 2;
      if (at < t.size() && t[at] == ':') ++at;
      const auto om = digits(t, at, 2);
      if (om) at += 2;
      offset = sign * (*oh * 60 + om.value_or(0));
      has_offset = true;
    }
  }
  if (at != t.size()) return fail(Status::Decode, "unexpected text after the date: '{}'", t.substr(at));
  return make_instant(*year, *month, *day, hour, minute, second, nanos, offset, has_offset);
}

Result<Instant> parse_rfc2822(std::string_view t) {
  std::vector<std::string_view> parts;
  for (std::string_view p : util::split(t, ' ', false)) {
    p = util::trim(p);
    if (!p.empty()) parts.push_back(p);
  }
  if (!parts.empty() && parts[0].ends_with(',')) parts.erase(parts.begin());  // day of week
  if (parts.size() < 4) return fail(Status::Decode, "not an RFC 2822 date");
  const auto day = util::parse_int(parts[0]);
  int month = 0;
  for (std::size_t i = 0; i < 12; ++i) {
    if (util::iequals(parts[1].substr(0, 3), month_names[i].substr(0, 3)) && parts[1].size() >= 3) month = static_cast<int>(i) + 1;
  }
  auto year = util::parse_int(parts[2]);
  if (!day || month == 0 || !year) return fail(Status::Decode, "not an RFC 2822 date");
  if (parts[2].size() <= 2) *year += *year < 50 ? 2000 : 1900;
  if (parts[2].size() == 3) *year += 1900;
  const std::vector<std::string_view> clock = util::split(parts[3], ':', true);
  if (clock.size() < 2) return fail(Status::Decode, "malformed RFC 2822 time");
  const auto h = util::parse_int(clock[0]), mi = util::parse_int(clock[1]);
  const auto s = clock.size() > 2 ? util::parse_int(clock[2]) : std::optional<std::int64_t>(0);
  if (!h || !mi || !s) return fail(Status::Decode, "malformed RFC 2822 time");
  std::int32_t offset = 0;
  bool has_offset = false;
  if (parts.size() > 4) {
    const std::string_view zone = parts[4];
    if ((zone[0] == '+' || zone[0] == '-') && zone.size() == 5) {
      const auto hh = digits(zone, 1, 2), mm = digits(zone, 3, 2);
      if (!hh || !mm) return fail(Status::Decode, "malformed zone '{}'", zone);
      offset = (zone[0] == '-' ? -1 : 1) * (*hh * 60 + *mm);
      has_offset = true;
    } else {
      constexpr std::array<std::pair<std::string_view, int>, 11> zones = {
          {{"UT", 0}, {"GMT", 0}, {"Z", 0}, {"EST", -300}, {"EDT", -240}, {"CST", -360}, {"CDT", -300}, {"MST", -420},
           {"MDT", -360}, {"PST", -480}, {"PDT", -420}}};
      const auto found = std::ranges::find_if(zones, [zone](const auto& z) { return util::iequals(z.first, zone); });
      if (found == zones.end()) return fail(Status::Decode, "unknown zone '{}'", zone);
      offset = found->second;
      has_offset = true;
    }
  }
  return make_instant(*year, month, static_cast<int>(*day), static_cast<int>(*h), static_cast<int>(*mi),
                      static_cast<int>(*s), 0, offset, has_offset);
}

}  // namespace

Result<Instant> parse_datetime(std::string_view text) {
  const std::string_view t = util::trim(text);
  if (t.empty()) return fail(Status::Decode, "empty date");
  const bool numeric = std::ranges::all_of(t.substr(t[0] == '-' ? 1 : 0), [](char c) { return util::is_ascii_digit(c) || c == '.'; });
  if (numeric && t.find('-', 1) == std::string_view::npos && (t.size() != 8 || t.find('.') != std::string_view::npos)) {
    const std::size_t dot = t.find('.');
    const std::string_view whole = t.substr(0, dot);
    const auto value = util::parse_int(whole);
    if (!value) return fail(Status::Decode, "number out of range");
    const std::size_t magnitude = whole.size() - (whole.starts_with('-') ? 1 : 0);
    Instant out;
    // 13+ digits are milliseconds, 16+ microseconds, 19+ nanoseconds (as tools usually print them).
    if (magnitude >= 19) {
      out.seconds = *value / 1'000'000'000;
      out.nanoseconds = static_cast<std::int32_t>(*value % 1'000'000'000);
    } else if (magnitude >= 16) {
      out.seconds = *value / 1'000'000;
      out.nanoseconds = static_cast<std::int32_t>(*value % 1'000'000 * 1000);
    } else if (magnitude >= 13) {
      out.seconds = *value / 1000;
      out.nanoseconds = static_cast<std::int32_t>(*value % 1000 * 1'000'000);
    } else {
      out.seconds = *value;
      if (dot != std::string_view::npos) {
        int scale = 100'000'000;
        for (const char c : t.substr(dot + 1)) {
          if (c == '.' || scale == 0) break;
          out.nanoseconds += (c - '0') * scale;
          scale /= 10;
        }
        if (*value < 0) out.nanoseconds = -out.nanoseconds;
      }
    }
    if (out.nanoseconds < 0) {
      out.nanoseconds += 1'000'000'000;
      --out.seconds;
    }
    if (out.seconds < min_seconds || out.seconds > max_seconds) return fail(Status::Decode, "timestamp outside years 1-9999");
    return out;
  }
  if (t.size() >= 8 && util::is_ascii_digit(t[0]) && util::is_ascii_digit(t[3]) && (t.size() < 5 || t[4] == '-' || util::is_ascii_digit(t[4]))) {
    return parse_iso(t);
  }
  return parse_rfc2822(t);
}

std::string format_datetime(const Instant& instant, std::string_view style) {
  const std::int32_t offset = instant.offset_minutes.value_or(0);
  const std::int64_t local = instant.seconds + offset * 60;
  std::int64_t days = local / 86400;
  std::int64_t rest = local % 86400;
  if (rest < 0) {
    rest += 86400;
    --days;
  }
  const Civil c = civil_from_days(days);
  const auto h = static_cast<int>(rest / 3600), mi = static_cast<int>(rest % 3600 / 60), s = static_cast<int>(rest % 60);
  const auto zone = [offset](bool colon) {
    if (offset == 0) return std::string(colon ? "Z" : "+0000");
    const int a = std::abs(offset);
    return std::format("{}{:02}{}{:02}", offset < 0 ? '-' : '+', a / 60, colon ? ":" : "", a % 60);
  };
  std::string fraction;
  if (instant.nanoseconds != 0) {
    fraction = std::format(".{:09}", instant.nanoseconds);
    while (fraction.back() == '0') fraction.pop_back();
  }
  const std::string iso = std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}{}{}", c.year, c.month, c.day, h, mi, s, fraction, zone(true));
  const auto weekday = static_cast<std::size_t>(((days % 7) + 11) % 7);  // 1970-01-01 was a Thursday
  const std::string rfc = std::format("{}, {:02} {} {:04} {:02}:{:02}:{:02} {}", day_names[weekday].substr(0, 3), c.day,
                                      month_names[c.month - 1].substr(0, 3), c.year, h, mi, s, zone(false));
  const std::string unix_s = std::to_string(instant.seconds);
  const std::string unix_ms = std::to_string(instant.seconds * 1000 + instant.nanoseconds / 1'000'000);
  if (style == "rfc2822") return rfc;
  if (style == "unix") return unix_s;
  if (style == "unix_ms") return unix_ms;
  if (style == "all") {
    return std::format("ISO 8601: {}\nRFC 2822: {}\nUnix seconds: {}\nUnix milliseconds: {}\nDay of week: {}", iso, rfc, unix_s,
                       unix_ms, day_names[weekday]);
  }
  return iso;
}

// ---------------------------------------------------------------------------- UUID / ULID

Result<std::string> describe_identifier(std::string_view text) {
  std::string_view t = util::trim(text);
  if (util::istarts_with(t, "urn:uuid:")) t.remove_prefix(9);
  if (t.starts_with('{') && t.ends_with('}')) t = t.substr(1, t.size() - 2);
  std::array<std::uint8_t, 16> bytes{};
  std::string kind;
  if (t.size() == 26 && std::ranges::all_of(t, [](char c) { return util::is_ascii_alpha(c) || util::is_ascii_digit(c); })) {
    // ULID: Crockford Base32, 48-bit millisecond time + 80 random bits.
    constexpr std::string_view alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    std::array<std::uint8_t, 26> digits{};
    for (std::size_t i = 0; i < 26; ++i) {
      char c = util::ascii_upper(t[i]);
      if (c == 'O') c = '0';
      if (c == 'I' || c == 'L') c = '1';
      const std::size_t at = alphabet.find(c);
      if (at == std::string_view::npos) return fail(Status::Decode, "'{}' is not a ULID character", t[i]);
      digits[i] = static_cast<std::uint8_t>(at);
    }
    if (digits[0] > 7) return fail(Status::Decode, "the ULID is out of range");
    // 26 digits * 5 bits = 130 bits; the top two are zero. Shift them into 16 bytes.
    std::array<std::uint8_t, 17> acc{};
    for (const std::uint8_t d : digits) {
      unsigned carry = d;
      for (std::size_t k = acc.size(); k-- > 0;) {
        const unsigned v = (static_cast<unsigned>(acc[k]) << 5) | carry;
        acc[k] = static_cast<std::uint8_t>(v & 0xFF);
        carry = v >> 8;
      }
    }
    std::copy(acc.begin() + 1, acc.end(), bytes.begin());
    kind = "ULID";
  } else {
    std::string hex;
    for (const char c : t) {
      if (c == '-') continue;
      if (util::hex_digit_value(c) < 0) return fail(Status::Decode, "'{}' is neither a UUID nor a ULID", t);
      hex.push_back(util::ascii_lower(c));
    }
    if (hex.size() != 32) return fail(Status::Decode, "a UUID has 32 hexadecimal digits; '{}' has {}", t, hex.size());
    for (std::size_t i = 0; i < 16; ++i) bytes[i] = static_cast<std::uint8_t>(util::hex_digit_value(hex[2 * i]) * 16 + util::hex_digit_value(hex[2 * i + 1]));
    kind = "UUID";
  }
  std::string hex;
  for (const std::uint8_t b : bytes) hex += std::format("{:02x}", b);
  const std::string canonical = std::format("{}-{}-{}-{}-{}", hex.substr(0, 8), hex.substr(8, 4), hex.substr(12, 4), hex.substr(16, 4), hex.substr(20));
  std::string out;
  const auto time_line = [&out](std::int64_t ms_since_epoch) {
    Instant i;
    i.seconds = ms_since_epoch / 1000;
    i.nanoseconds = static_cast<std::int32_t>(ms_since_epoch % 1000 * 1'000'000);
    if (i.nanoseconds < 0) {
      i.nanoseconds += 1'000'000'000;
      --i.seconds;
    }
    out += "Time: " + format_datetime(i, "iso8601") + "\n";
  };
  std::uint64_t first48 = 0;
  for (std::size_t i = 0; i < 6; ++i) first48 = (first48 << 8) | bytes[i];
  if (kind == "ULID") {
    out += "Type: ULID\n";
    time_line(static_cast<std::int64_t>(first48));
    out += "Randomness: " + hex.substr(12) + "\n";
    out += "As UUID: " + canonical + "\n";
    return out;
  }
  const int version = bytes[6] >> 4;
  const int variant_bits = bytes[8] >> 5;
  std::string variant = (variant_bits & 0b100) == 0 ? "NCS (reserved)" : (variant_bits & 0b110) == 0b100 ? "RFC 9562" : (variant_bits & 0b111) == 0b110 ? "Microsoft (reserved)" : "future (reserved)";
  out += "Type: UUID\nCanonical: " + canonical + "\n";
  if (hex == std::string(32, '0')) {
    out += "Special: nil UUID\n";
  } else if (hex == std::string(32, 'f')) {
    out += "Special: max UUID\n";
  } else {
    constexpr std::array<std::string_view, 9> names = {"", "time-based (MAC address)", "DCE security", "name-based (MD5)", "random",
                                                       "name-based (SHA-1)", "reordered time-based", "Unix time-based", "custom"};
    out += std::format("Version: {}{}\nVariant: {}\n", version,
                       version >= 1 && version <= 8 ? " - " + std::string(names[static_cast<std::size_t>(version)]) : std::string(" (unknown)"),
                       variant);
    // Gregorian 100-ns ticks since 1582-10-15 for v1/v6; milliseconds since 1970 for v7.
    if (version == 1 || version == 6) {
      std::uint64_t ticks = 0;
      if (version == 1) {
        const std::uint64_t low = (std::uint64_t{bytes[0]} << 24) | (bytes[1] << 16) | (bytes[2] << 8) | bytes[3];
        const std::uint64_t mid = (std::uint64_t{bytes[4]} << 8) | bytes[5];
        const std::uint64_t high = (std::uint64_t{bytes[6] & 0x0Fu} << 8) | bytes[7];
        ticks = (high << 48) | (mid << 32) | low;
      } else {
        ticks = (first48 << 12) | (std::uint64_t{bytes[6] & 0x0Fu} << 8) | bytes[7];
      }
      const std::int64_t unix_100ns = static_cast<std::int64_t>(ticks) - 122192928000000000LL;
      time_line(unix_100ns / 10000);
      const std::uint16_t clock = static_cast<std::uint16_t>(((bytes[8] & 0x3Fu) << 8) | bytes[9]);
      out += std::format("Clock sequence: {}\nNode: {}:{}:{}:{}:{}:{}\n", clock, hex.substr(20, 2), hex.substr(22, 2),
                         hex.substr(24, 2), hex.substr(26, 2), hex.substr(28, 2), hex.substr(30, 2));
    } else if (version == 7) {
      time_line(static_cast<std::int64_t>(first48));
    }
  }
  out += "URN: urn:uuid:" + canonical + "\n";
  std::array<std::byte, 16> raw{};
  for (std::size_t i = 0; i < 16; ++i) raw[i] = static_cast<std::byte>(bytes[i]);
  out += "Base64: " + util::base64_encode(raw) + "\n";
  return out;
}

// ---------------------------------------------------------------------------- colours

namespace {

struct NamedColor {
  std::string_view name;
  std::uint32_t rgb;
};
// CSS Color Module Level 4 named colours.
constexpr NamedColor named_colors[] = {
    {"aliceblue", 0xf0f8ff}, {"antiquewhite", 0xfaebd7}, {"aqua", 0x00ffff}, {"aquamarine", 0x7fffd4}, {"azure", 0xf0ffff},
    {"beige", 0xf5f5dc}, {"bisque", 0xffe4c4}, {"black", 0x000000}, {"blanchedalmond", 0xffebcd}, {"blue", 0x0000ff},
    {"blueviolet", 0x8a2be2}, {"brown", 0xa52a2a}, {"burlywood", 0xdeb887}, {"cadetblue", 0x5f9ea0}, {"chartreuse", 0x7fff00},
    {"chocolate", 0xd2691e}, {"coral", 0xff7f50}, {"cornflowerblue", 0x6495ed}, {"cornsilk", 0xfff8dc}, {"crimson", 0xdc143c},
    {"cyan", 0x00ffff}, {"darkblue", 0x00008b}, {"darkcyan", 0x008b8b}, {"darkgoldenrod", 0xb8860b}, {"darkgray", 0xa9a9a9},
    {"darkgreen", 0x006400}, {"darkgrey", 0xa9a9a9}, {"darkkhaki", 0xbdb76b}, {"darkmagenta", 0x8b008b},
    {"darkolivegreen", 0x556b2f}, {"darkorange", 0xff8c00}, {"darkorchid", 0x9932cc}, {"darkred", 0x8b0000},
    {"darksalmon", 0xe9967a}, {"darkseagreen", 0x8fbc8f}, {"darkslateblue", 0x483d8b}, {"darkslategray", 0x2f4f4f},
    {"darkslategrey", 0x2f4f4f}, {"darkturquoise", 0x00ced1}, {"darkviolet", 0x9400d3}, {"deeppink", 0xff1493},
    {"deepskyblue", 0x00bfff}, {"dimgray", 0x696969}, {"dimgrey", 0x696969}, {"dodgerblue", 0x1e90ff}, {"firebrick", 0xb22222},
    {"floralwhite", 0xfffaf0}, {"forestgreen", 0x228b22}, {"fuchsia", 0xff00ff}, {"gainsboro", 0xdcdcdc},
    {"ghostwhite", 0xf8f8ff}, {"gold", 0xffd700}, {"goldenrod", 0xdaa520}, {"gray", 0x808080}, {"green", 0x008000},
    {"greenyellow", 0xadff2f}, {"grey", 0x808080}, {"honeydew", 0xf0fff0}, {"hotpink", 0xff69b4}, {"indianred", 0xcd5c5c},
    {"indigo", 0x4b0082}, {"ivory", 0xfffff0}, {"khaki", 0xf0e68c}, {"lavender", 0xe6e6fa}, {"lavenderblush", 0xfff0f5},
    {"lawngreen", 0x7cfc00}, {"lemonchiffon", 0xfffacd}, {"lightblue", 0xadd8e6}, {"lightcoral", 0xf08080},
    {"lightcyan", 0xe0ffff}, {"lightgoldenrodyellow", 0xfafad2}, {"lightgray", 0xd3d3d3}, {"lightgreen", 0x90ee90},
    {"lightgrey", 0xd3d3d3}, {"lightpink", 0xffb6c1}, {"lightsalmon", 0xffa07a}, {"lightseagreen", 0x20b2aa},
    {"lightskyblue", 0x87cefa}, {"lightslategray", 0x778899}, {"lightslategrey", 0x778899}, {"lightsteelblue", 0xb0c4de},
    {"lightyellow", 0xffffe0}, {"lime", 0x00ff00}, {"limegreen", 0x32cd32}, {"linen", 0xfaf0e6}, {"magenta", 0xff00ff},
    {"maroon", 0x800000}, {"mediumaquamarine", 0x66cdaa}, {"mediumblue", 0x0000cd}, {"mediumorchid", 0xba55d3},
    {"mediumpurple", 0x9370db}, {"mediumseagreen", 0x3cb371}, {"mediumslateblue", 0x7b68ee}, {"mediumspringgreen", 0x00fa9a},
    {"mediumturquoise", 0x48d1cc}, {"mediumvioletred", 0xc71585}, {"midnightblue", 0x191970}, {"mintcream", 0xf5fffa},
    {"mistyrose", 0xffe4e1}, {"moccasin", 0xffe4b5}, {"navajowhite", 0xffdead}, {"navy", 0x000080}, {"oldlace", 0xfdf5e6},
    {"olive", 0x808000}, {"olivedrab", 0x6b8e23}, {"orange", 0xffa500}, {"orangered", 0xff4500}, {"orchid", 0xda70d6},
    {"palegoldenrod", 0xeee8aa}, {"palegreen", 0x98fb98}, {"paleturquoise", 0xafeeee}, {"palevioletred", 0xdb7093},
    {"papayawhip", 0xffefd5}, {"peachpuff", 0xffdab9}, {"peru", 0xcd853f}, {"pink", 0xffc0cb}, {"plum", 0xdda0dd},
    {"powderblue", 0xb0e0e6}, {"purple", 0x800080}, {"rebeccapurple", 0x663399}, {"red", 0xff0000}, {"rosybrown", 0xbc8f8f},
    {"royalblue", 0x4169e1}, {"saddlebrown", 0x8b4513}, {"salmon", 0xfa8072}, {"sandybrown", 0xf4a460},
    {"seagreen", 0x2e8b57}, {"seashell", 0xfff5ee}, {"sienna", 0xa0522d}, {"silver", 0xc0c0c0}, {"skyblue", 0x87ceeb},
    {"slateblue", 0x6a5acd}, {"slategray", 0x708090}, {"slategrey", 0x708090}, {"snow", 0xfffafa}, {"springgreen", 0x00ff7f},
    {"steelblue", 0x4682b4}, {"tan", 0xd2b48c}, {"teal", 0x008080}, {"thistle", 0xd8bfd8}, {"tomato", 0xff6347},
    {"turquoise", 0x40e0d0}, {"violet", 0xee82ee}, {"wheat", 0xf5deb3}, {"white", 0xffffff}, {"whitesmoke", 0xf5f5f5},
    {"yellow", 0xffff00}, {"yellowgreen", 0x9acd32},
};

double srgb_to_linear(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
double linear_to_srgb(double c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055; }

struct Lab {
  double l, a, b;
};
Lab to_oklab(const Rgba& c) {
  const double r = srgb_to_linear(c.r), g = srgb_to_linear(c.g), b = srgb_to_linear(c.b);
  const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
  const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
  const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
  return {0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s, 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
          0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s};
}
Rgba from_oklab(const Lab& lab, double alpha) {
  const double l_ = lab.l + 0.3963377774 * lab.a + 0.2158037573 * lab.b;
  const double m_ = lab.l - 0.1055613458 * lab.a - 0.0638541728 * lab.b;
  const double s_ = lab.l - 0.0894841775 * lab.a - 1.2914855480 * lab.b;
  const double l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
  const auto clamp = [](double v) { return std::clamp(linear_to_srgb(v), 0.0, 1.0); };
  return {clamp(4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s),
          clamp(-1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s),
          clamp(-0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s), alpha};
}

Rgba from_hsl(double h, double s, double l, double alpha) {
  h = std::fmod(std::fmod(h, 360.0) + 360.0, 360.0);
  const auto f = [&](double n) {
    const double k = std::fmod(n + h / 30.0, 12.0);
    const double a = s * std::min(l, 1 - l);
    return l - a * std::max(-1.0, std::min({k - 3, 9 - k, 1.0}));
  };
  return {f(0), f(8), f(4), alpha};
}

struct Hsl {
  double h, s, l;
};
Hsl to_hsl(const Rgba& c) {
  const double max = std::max({c.r, c.g, c.b}), min = std::min({c.r, c.g, c.b});
  const double l = (max + min) / 2, d = max - min;
  double h = 0, s = 0;
  if (d > 1e-12) {
    s = l > 0.5 ? d / (2 - max - min) : d / (max + min);
    if (max == c.r) {
      h = (c.g - c.b) / d + (c.g < c.b ? 6 : 0);
    } else if (max == c.g) {
      h = (c.b - c.r) / d + 2;
    } else {
      h = (c.r - c.g) / d + 4;
    }
    h *= 60;
  }
  return {h, s, l};
}

/// The numbers inside "name(...)": separated by spaces, commas or '/', with
/// optional % and deg units. Percentages are returned divided by 100 and flagged.
struct Arg {
  double value;
  bool percent;
};
Result<std::vector<Arg>> function_args(std::string_view body) {
  std::vector<Arg> out;
  std::string token;
  const auto flush = [&]() -> Result<void> {
    if (token.empty()) return {};
    std::string_view t = token;
    bool percent = false;
    if (t.ends_with('%')) {
      percent = true;
      t.remove_suffix(1);
    } else if (t.ends_with("deg")) {
      t.remove_suffix(3);
    }
    const auto v = util::parse_double(t);
    if (!v || !std::isfinite(*v)) return fail(Status::Decode, "'{}' is not a number", token);
    out.push_back({percent ? *v / 100.0 : *v, percent});
    token.clear();
    return {};
  };
  for (const char c : body) {
    if (c == ',' || c == '/' || util::is_ascii_space(c)) {
      CVT_TRY(flush());
    } else {
      token.push_back(c);
    }
  }
  CVT_TRY(flush());
  return out;
}

std::string fixed(double v, int decimals) {
  std::string s = std::format("{:.{}f}", v, decimals);
  if (s.find('.') != std::string::npos) {
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
  }
  if (s == "-0") s = "0";
  return s;
}

}  // namespace

Result<Rgba> parse_color(std::string_view text) {
  const std::string t = util::to_lower(util::trim(text));
  if (t.empty()) return fail(Status::Decode, "empty colour");
  if (t == "transparent") return Rgba{0, 0, 0, 0};
  for (const NamedColor& named : named_colors) {
    if (t == named.name) return Rgba{(named.rgb >> 16) / 255.0, ((named.rgb >> 8) & 0xFF) / 255.0, (named.rgb & 0xFF) / 255.0, 1};
  }
  std::string_view hex = t;
  if (hex.starts_with('#')) hex.remove_prefix(1);
  if ((hex.size() == 3 || hex.size() == 4 || hex.size() == 6 || hex.size() == 8) &&
      std::ranges::all_of(hex, [](char c) { return util::hex_digit_value(c) >= 0; }) && (t[0] == '#' || hex.size() >= 6)) {
    std::array<int, 4> v = {0, 0, 0, 255};
    const bool short_form = hex.size() <= 4;
    const std::size_t count = short_form ? hex.size() : hex.size() / 2;
    for (std::size_t i = 0; i < count; ++i) {
      v[i] = short_form ? util::hex_digit_value(hex[i]) * 17 : util::hex_digit_value(hex[2 * i]) * 16 + util::hex_digit_value(hex[2 * i + 1]);
    }
    return Rgba{v[0] / 255.0, v[1] / 255.0, v[2] / 255.0, v[3] / 255.0};
  }
  const std::size_t open = t.find('(');
  if (open == std::string::npos || !t.ends_with(')')) return fail(Status::Decode, "'{}' is not a colour", text);
  const std::string name = t.substr(0, open);
  CVT_TRY_ASSIGN(const std::vector<Arg> args, function_args(std::string_view(t).substr(open + 1, t.size() - open - 2)));
  if (args.size() < 3 || args.size() > 4) return fail(Status::Decode, "'{}' needs 3 or 4 values", name);
  const double alpha = args.size() == 4 ? std::clamp(args[3].value, 0.0, 1.0) : 1.0;
  if (name == "rgb" || name == "rgba") {
    const auto channel = [](const Arg& a) { return std::clamp(a.percent ? a.value : a.value / 255.0, 0.0, 1.0); };
    return Rgba{channel(args[0]), channel(args[1]), channel(args[2]), alpha};
  }
  if (name == "hsl" || name == "hsla") {
    const auto unit = [](const Arg& a) { return std::clamp(a.percent ? a.value : a.value / 100.0, 0.0, 1.0); };
    return from_hsl(args[0].value, unit(args[1]), unit(args[2]), alpha);
  }
  if (name == "hwb") {
    double w = std::clamp(args[1].percent ? args[1].value : args[1].value / 100.0, 0.0, 1.0);
    double bl = std::clamp(args[2].percent ? args[2].value : args[2].value / 100.0, 0.0, 1.0);
    if (w + bl >= 1) {
      const double gray = w / (w + bl);
      return Rgba{gray, gray, gray, alpha};
    }
    Rgba c = from_hsl(args[0].value, 1, 0.5, alpha);
    for (double* ch : {&c.r, &c.g, &c.b}) *ch = *ch * (1 - w - bl) + w;
    return c;
  }
  if (name == "oklab" || name == "oklch") {
    const double l = args[0].percent ? args[0].value : args[0].value;
    if (name == "oklab") {
      const double a = args[1].percent ? args[1].value * 0.4 : args[1].value;
      const double b = args[2].percent ? args[2].value * 0.4 : args[2].value;
      return from_oklab({l, a, b}, alpha);
    }
    const double chroma = args[1].percent ? args[1].value * 0.4 : args[1].value;
    const double hue = args[2].value * std::numbers::pi / 180.0;
    return from_oklab({l, chroma * std::cos(hue), chroma * std::sin(hue)}, alpha);
  }
  return fail(Status::Decode, "unknown colour function '{}'", name);
}

std::string format_color(const Rgba& c, std::string_view notation) {
  const auto byte = [](double v) { return static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); };
  const std::string alpha = c.a < 1.0 ? " / " + fixed(c.a * 100, 1) + "%" : std::string();
  std::string hex = std::format("#{:02x}{:02x}{:02x}", byte(c.r), byte(c.g), byte(c.b));
  if (c.a < 1.0) hex += std::format("{:02x}", byte(c.a));
  const std::string rgb = std::format("rgb({} {} {}{})", byte(c.r), byte(c.g), byte(c.b), alpha);
  const Hsl h = to_hsl(c);
  const std::string hsl = std::format("hsl({} {}% {}%{})", fixed(h.h, 1), fixed(h.s * 100, 1), fixed(h.l * 100, 1), alpha);
  const double white = std::min({c.r, c.g, c.b}), black = 1 - std::max({c.r, c.g, c.b});
  const std::string hwb = std::format("hwb({} {}% {}%{})", fixed(h.h, 1), fixed(white * 100, 1), fixed(black * 100, 1), alpha);
  const Lab lab = to_oklab(c);
  const double chroma = std::hypot(lab.a, lab.b);
  double hue = std::atan2(lab.b, lab.a) * 180.0 / std::numbers::pi;
  if (hue < 0) hue += 360;
  const std::string oklch = std::format("oklch({}% {} {}{})", fixed(lab.l * 100, 2), fixed(chroma, 4),
                                        chroma < 1e-6 ? std::string("0") : fixed(hue, 2), alpha);
  const std::string oklab = std::format("oklab({}% {} {}{})", fixed(lab.l * 100, 2), fixed(lab.a, 4), fixed(lab.b, 4), alpha);
  std::string name;
  const std::uint32_t packed = (static_cast<std::uint32_t>(byte(c.r)) << 16) | (static_cast<std::uint32_t>(byte(c.g)) << 8) |
                               static_cast<std::uint32_t>(byte(c.b));
  for (const NamedColor& named : named_colors) {
    if (named.rgb == packed && c.a >= 1.0) {
      name = named.name;
      break;
    }
  }
  if (notation == "hex") return hex;
  if (notation == "rgb") return rgb;
  if (notation == "hsl") return hsl;
  if (notation == "hwb") return hwb;
  if (notation == "oklch") return oklch;
  if (notation == "oklab") return oklab;
  if (notation == "name") return name.empty() ? hex : name;
  return std::format("hex: {}\nrgb: {}\nhsl: {}\nhwb: {}\noklch: {}\noklab: {}{}", hex, rgb, hsl, hwb, oklch, oklab,
                     name.empty() ? "" : "\nname: " + name);
}

// ---------------------------------------------------------------------------- registration

namespace {

FormatInfo describe(std::string id, std::string name, std::string description, std::vector<std::string> extensions,
                    std::string mime, int popularity, std::function<int(ByteView, std::string_view)> sniff = {}) {
  FormatInfo info;
  info.id = std::move(id);
  info.name = std::move(name);
  info.description = std::move(description);
  info.category = Category::Code;
  info.extensions = std::move(extensions);
  info.mime = std::move(mime);
  info.popularity = popularity;
  info.sniff = std::move(sniff);
  return info;
}

/// A PlainText writer that rewrites every non-empty line (dates, identifiers, colours).
void add_line_tool(Registry& registry, FormatInfo info, std::vector<OptionSpec> options,
                   std::function<Result<std::string>(std::string_view line, const Options& options)> convert) {
  Codec codec;
  codec.format_id = info.id;
  codec.kind = Kind::PlainText;
  codec.lossy = true;
  codec.write_options = std::move(options);
  codec.write = [convert = std::move(convert)](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
    std::string out;
    std::size_t number = 0;
    for (const std::string_view line : util::split_lines(value.get<ir::PlainText>().text)) {
      ++number;
      if ((number & 0x3FF) == 0) CVT_TRY(ctx.checkpoint(0.5));
      if (util::trim(line).empty()) {
        out += "\n";
        continue;
      }
      auto converted = convert(util::trim(line), ctx.options);
      if (!converted) return fail(Status::Decode, "line {}: {}", number, converted.error().message);
      out += *converted + "\n";
      CVT_TRY(ctx.limits.check_output(out.size()));
    }
    return to_bytes(out);
  };
  registry.add_format(std::move(info));
  registry.add_codec(std::move(codec));
}

void add_request_format(Registry& registry, FormatInfo info, std::function<Result<Json>(std::string_view)> parse,
                        std::function<Result<std::string>(const Json&)> write) {
  Codec codec;
  codec.format_id = info.id;
  codec.kind = Kind::Data;
  if (parse) {
    codec.read = [parse = std::move(parse)](ReadContext& ctx) -> Result<AnyIR> {
      CVT_TRY_ASSIGN(const std::string text, input_text(ctx));
      ir::Data data;
      CVT_TRY_ASSIGN(data.value, parse(text));
      return data;
    };
  }
  codec.write = [write = std::move(write)](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
    CVT_TRY_ASSIGN(const std::string text, write(value.get<ir::Data>().value));
    CVT_TRY(ctx.limits.check_output(text.size()));
    return to_bytes(text);
  };
  registry.add_format(std::move(info));
  registry.add_codec(std::move(codec));
}

}  // namespace

void register_devtools(Registry& registry) {
  register_diff(registry);

  add_request_format(registry,
                     describe("curl", "cURL command", "A curl command line describing an HTTP request.", {"curl", "sh"},
                              "text/x-shellscript", 30,
                              [](ByteView data, std::string_view filename) {
                                const std::string_view text = util::trim_left(as_chars(data).substr(0, 64));
                                const bool shaped = text.starts_with("curl ") || text.starts_with("curl.exe ");
                                return shaped ? (util::iends_with(filename, ".curl") ? 95 : 86) : 0;
                              }),
                     parse_curl, to_curl);
  add_request_format(registry,
                     describe("http", "HTTP request", "A raw HTTP/1.1 request: request line, headers and body.",
                              {"http", "rest"}, "message/http", 20,
                              [](ByteView data, std::string_view) {
                                const std::string_view text = as_chars(data).substr(0, 256);
                                const std::size_t nl = text.find('\n');
                                const std::string_view first = text.substr(0, nl);
                                constexpr std::array<std::string_view, 7> methods = {"GET ", "POST ", "PUT ", "PATCH ",
                                                                                     "DELETE ", "HEAD ", "OPTIONS "};
                                const bool method = std::ranges::any_of(methods, [first](std::string_view m) { return first.starts_with(m); });
                                return method && first.find(" HTTP/") != std::string_view::npos ? 90 : 0;
                              }),
                     parse_http_request, to_http_request);
  add_request_format(registry,
                     describe("fetch-js", "JavaScript fetch() call", "An HTTP request as a JavaScript fetch() snippet.",
                              {"js"}, "text/javascript", 20),
                     {}, to_fetch);
  add_request_format(registry,
                     describe("python-requests", "Python requests call",
                              "An HTTP request as a Python snippet using the requests library.", {"py"}, "text/x-python", 20),
                     {}, to_python_requests);

  // cron: Data (one object, or an array for a crontab) and a plain-text description per line.
  registry.add_format(describe("cron", "Cron schedule", "Cron expressions or a crontab; reads as English descriptions.",
                               {"cron", "crontab"}, "text/plain", 20, [](ByteView data, std::string_view filename) {
                                 const std::size_t slash = filename.find_last_of("/\\");
                                 const std::string_view name = slash == std::string_view::npos ? filename : filename.substr(slash + 1);
                                 if (name == "crontab" || util::iends_with(name, ".cron") || util::iends_with(name, ".crontab")) {
                                   return util::looks_like_text(data.first(std::min<std::size_t>(data.size(), 4096))) ? 92 : 0;
                                 }
                                 return 0;
                               }));
  Codec cron_data;
  cron_data.format_id = "cron";
  cron_data.kind = Kind::Data;
  cron_data.read = [](ReadContext& ctx) -> Result<AnyIR> {
    CVT_TRY_ASSIGN(const std::string text, input_text(ctx));
    ir::Data data;
    CVT_TRY_ASSIGN(data.value, crontab_to_data(text));
    return data;
  };
  registry.add_codec(std::move(cron_data));
  Codec cron_text;
  cron_text.format_id = "cron";
  cron_text.kind = Kind::PlainText;
  cron_text.read = [](ReadContext& ctx) -> Result<AnyIR> {
    CVT_TRY_ASSIGN(const std::string text, input_text(ctx));
    CVT_TRY_ASSIGN(const Json data, crontab_to_data(text));
    ir::PlainText out;
    const auto line = [&out](const Json& entry) {
      out.text += entry["expression"].as_string() + "  →  " + entry["description"].as_string();
      if (const Json* command = entry.find("command"); command != nullptr) out.text += "  (" + command->as_string() + ")";
      out.text += "\n";
    };
    if (data.is_array()) {
      for (const Json& entry : data.elements()) line(entry);
    } else {
      line(data);
    }
    return out;
  };
  registry.add_codec(std::move(cron_text));

  add_line_tool(registry,
                describe("datetime", "Date and time conversion",
                         "Unix timestamps, ISO 8601 and RFC 2822 dates converted line by line (UTC or the written offset).",
                         {"txt"}, "text/plain", 20, [](ByteView, std::string_view) { return 0; }),
                {OptionSpec::choice("to", "Convert to", "iso8601", {"iso8601", "rfc2822", "unix", "unix_ms", "all"},
                                    "Output notation; 'all' lists every notation."),
                 OptionSpec::choice("timezone", "Time zone", "keep", {"keep", "utc"},
                                    "keep: use the offset written in the input (UTC when there is none); utc: always UTC.")},
                [](std::string_view line, const Options& options) -> Result<std::string> {
                  CVT_TRY_ASSIGN(Instant instant, parse_datetime(line));
                  if (options.get_enum("timezone", "keep", {"keep", "utc"}) == "utc") instant.offset_minutes.reset();
                  return format_datetime(instant, options.get_enum("to", "iso8601", {"iso8601", "rfc2822", "unix", "unix_ms", "all"}));
                });
  add_line_tool(registry,
                describe("uuid-info", "UUID / ULID inspection",
                         "What a UUID or ULID contains: version, variant, embedded time and other notations.", {"txt"},
                         "text/plain", 15, [](ByteView, std::string_view) { return 0; }),
                {}, [](std::string_view line, const Options&) { return describe_identifier(line); });
  add_line_tool(registry,
                describe("color", "Colour notation", "A CSS colour converted between hex, rgb(), hsl(), hwb(), oklch() and names.",
                         {"txt"}, "text/plain", 15, [](ByteView, std::string_view) { return 0; }),
                {OptionSpec::choice("to", "Convert to", "all", {"all", "hex", "rgb", "hsl", "hwb", "oklch", "oklab", "name"},
                                    "Output notation; 'all' lists every notation.")},
                [](std::string_view line, const Options& options) -> Result<std::string> {
                  CVT_TRY_ASSIGN(const Rgba color, parse_color(line));
                  return format_color(color, options.get_enum("to", "all", {"all", "hex", "rgb", "hsl", "hwb", "oklch", "oklab", "name"}));
                });
}

}  // namespace cvt::code
