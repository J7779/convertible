// Tests of the C ABI: everything an application can observe.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <atomic>
#include <cstring>
#include <filesystem>
#include <memory>
#include <thread>

#include "util/bytes.hpp"
#include "convertible/convertible.h"
#include "support/test_support.hpp"
#include "util/fs.hpp"
#include "util/json.hpp"

using namespace cvt;

namespace {

static_assert(CVT_OK == 0 && CVT_ERR_INVALID_ARGUMENT == 1 && CVT_ERR_UNKNOWN_FORMAT == 2 &&
              CVT_ERR_UNSUPPORTED_CONVERSION == 3 && CVT_ERR_DECODE == 4 && CVT_ERR_ENCODE == 5 && CVT_ERR_IO == 6 &&
              CVT_ERR_CANCELLED == 7 && CVT_ERR_LIMIT == 8 && CVT_ERR_INTERNAL == 9);

std::string take(char* text) {
  REQUIRE(text != nullptr);
  std::string out(text);
  cvt_string_free(text);
  return out;
}

Json take_json(char* text) {
  auto parsed = util::parse_json(take(text));
  REQUIRE_OK(parsed);
  return std::move(*parsed);
}

struct ResultDeleter {
  void operator()(cvt_result* result) const { cvt_result_free(result); }
};
using ResultPtr = std::unique_ptr<cvt_result, ResultDeleter>;

ResultPtr convert(std::string_view input, const char* filename, const char* source, const char* target,
                  const char* options = nullptr, cvt_progress_fn progress = nullptr, void* user = nullptr) {
  cvt_result* result = cvt_convert(reinterpret_cast<const std::uint8_t*>(input.data()), input.size(), filename, source,
                                   target, options, progress, user);
  REQUIRE(result != nullptr);
  return ResultPtr(result);
}

std::string output_text(const ResultPtr& result) {
  const std::uint8_t* data = cvt_result_data(result.get());
  return data == nullptr ? std::string() : std::string(reinterpret_cast<const char*>(data), cvt_result_size(result.get()));
}

const Json* find_by_id(const Json& array, std::string_view id) {
  for (const Json& entry : array.elements()) {
    if (entry["id"].as_string() == id) return &entry;
  }
  return nullptr;
}

}  // namespace

TEST_CASE("capi: version and string ownership") {
  CHECK(std::string_view(cvt_version()) == "0.1.0");
  CHECK(cvt_version() == cvt_version());  // static storage
  cvt_string_free(nullptr);               // tolerated
  cvt_result_free(nullptr);
}

TEST_CASE("capi: the format list") {
  const Json formats = take_json(cvt_formats_json());
  REQUIRE(formats.is_array());
  REQUIRE(formats.size() >= 100);
  int previous_popularity = 101;
  for (const Json& format : formats.elements()) {
    // Exactly the documented fields, in the documented order.
    std::vector<std::string> keys;
    for (const auto& [key, value] : format.items()) keys.emplace_back(key);
    CHECK(keys == std::vector<std::string>{"id", "name", "description", "category", "extensions", "mime", "readable",
                                           "writable", "popularity", "output_only"});
    CHECK_FALSE(format["id"].as_string().empty());
    CHECK_FALSE(format["name"].as_string().empty());
    CHECK(format["extensions"].is_array());
    CHECK_FALSE(format["mime"].as_string().empty());
    CHECK(format["readable"].is_bool());
    CHECK(format["writable"].is_bool());
    const int popularity = static_cast<int>(format["popularity"].as_int64(-1));
    CHECK(popularity <= previous_popularity);  // most popular first
    previous_popularity = popularity;
  }
  const Json* base64 = find_by_id(formats, "base64");
  REQUIRE(base64 != nullptr);
  CHECK(util::dump_json(*base64) ==
        R"({"id":"base64","name":"Base64 text","description":"Binary data as printable ASCII (RFC 4648).",)"
        R"("category":"encoding","extensions":["b64","base64"],"mime":"text/plain","readable":true,"writable":true,)"
        R"("popularity":70,"output_only":false})");
  // Detection-only formats are listed, neither readable nor writable (yet).
  const Json* pdf = find_by_id(formats, "pdf");
  REQUIRE(pdf != nullptr);
  CHECK((*pdf)["category"].as_string() == "document");
  const Json* checksums = find_by_id(formats, "checksums");
  REQUIRE(checksums != nullptr);
  CHECK_FALSE((*checksums)["readable"].as_bool(true));
  CHECK((*checksums)["writable"].as_bool(false));
}

TEST_CASE("capi: detection") {
  const std::string png = "\x89PNG\r\n\x1a\n";
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(png.data());
  CHECK(take(cvt_detect(bytes, png.size(), nullptr)) == "png");
  CHECK(take(cvt_detect(bytes, png.size(), "misnamed.txt")) == "png");
  CHECK(take(cvt_detect(nullptr, 0, "clip.MP4")) == "mp4");
  CHECK(take(cvt_detect(nullptr, 123, "clip.mp4")) == "mp4");  // NULL data: the size is ignored
  CHECK(take(cvt_detect(reinterpret_cast<const std::uint8_t*>("hello\n"), 6, nullptr)) == "txt");
  CHECK(cvt_detect(nullptr, 0, nullptr) == nullptr);
  CHECK(cvt_detect(bytes, 0, "") == nullptr);
  const std::string noise = "\x01\x02\x03\xff\xfe\xfd";
  CHECK(cvt_detect(reinterpret_cast<const std::uint8_t*>(noise.data()), noise.size(), "x.unknown") == nullptr);
  CHECK(take(cvt_detect(bytes, png.size(), "bad\xFFname.png")) == "png");  // invalid UTF-8 in the name is tolerated
}

TEST_CASE("capi: targets") {
  CHECK(take(cvt_targets_json(nullptr)) == "[]");
  CHECK(take(cvt_targets_json("")) == "[]");
  CHECK(take(cvt_targets_json("no-such-format")) == "[]");

  const Json targets = take_json(cvt_targets_json("txt"));
  REQUIRE(targets.is_array());
  REQUIRE(targets.size() >= 10);
  for (const Json& target : targets.elements()) {
    std::vector<std::string> keys;
    for (const auto& [key, value] : target.items()) keys.emplace_back(key);
    CHECK(keys == std::vector<std::string>{"id", "name", "category", "extensions", "mime", "lossy", "compress", "role"});
    CHECK(target["compress"].as_bool() == (target["id"].as_string() == "txt"));
  }
  // The source's category comes first, most popular format first; the text
  // re-encode ("compress") is among them, flagged.
  const std::string first_category = targets[std::size_t{0}]["category"].as_string();
  CHECK(first_category == "document");
  const Json* txt = find_by_id(targets, "txt");
  REQUIRE(txt != nullptr);
  CHECK((*txt)["category"].as_string() == first_category);
  CHECK(find_by_id(targets, "base64") != nullptr);
  CHECK(find_by_id(targets, "rot13") != nullptr);

  // Accepts extensions and odd spellings; binary sources get the universal targets only.
  CHECK(take(cvt_targets_json(".TXT")) == util::dump_json(targets));
  const Json for_png = take_json(cvt_targets_json("png"));
  CHECK(find_by_id(for_png, "base64") != nullptr);
  CHECK(find_by_id(for_png, "checksums") != nullptr);
  CHECK(find_by_id(for_png, "rot13") == nullptr);
  // PNG has a real reader and writer: it is offered as "compress", and only it.
  const Json* png = find_by_id(for_png, "png");
  REQUIRE(png != nullptr);
  CHECK((*png)["compress"].as_bool(false));
  for (const Json& target : for_png.elements()) {
    CHECK(target["compress"].as_bool(true) == (target["id"].as_string() == "png"));
  }
  // Same category first: an image converts to images before anything else.
  CHECK(for_png[std::size_t{0}]["category"].as_string() == "image");
}

TEST_CASE("capi: option schemas") {
  CHECK(take(cvt_options_schema_json(nullptr, nullptr)) == "[]");
  CHECK(take(cvt_options_schema_json("txt", nullptr)) == "[]");
  CHECK(take(cvt_options_schema_json("txt", "png")) == "[]");  // no such conversion
  CHECK(take(cvt_options_schema_json("png", "bin")) == "[]");  // possible, but no options

  const Json schema = take_json(cvt_options_schema_json("txt", "txt"));
  REQUIRE(schema.size() == 4);
  CHECK(schema[std::size_t{0}]["key"].as_string() == "source_encoding");
  CHECK(util::dump_json(schema[std::size_t{2}]) ==
        R"({"key":"bom","label":"Byte order mark","type":"bool","default":false,"min":null,"max":null,"choices":[],)"
        R"("description":"Start the output with a Unicode byte order mark.","category":"basic"})");
  const Json base64 = take_json(cvt_options_schema_json("PNG", ".b64"));
  REQUIRE(base64.size() == 3);
  CHECK(base64[std::size_t{0}]["key"].as_string() == "line_width");
  CHECK(base64[std::size_t{0}]["type"].as_string() == "int");
  CHECK(base64[std::size_t{0}]["default"].as_int64() == 76);
  CHECK(base64[std::size_t{0}]["max"].as_int64() == 1024);
  CHECK(base64[std::size_t{2}]["category"].as_string() == "advanced");
}

TEST_CASE("capi: a successful conversion") {
  const ResultPtr result = convert("Hello", "greeting.txt", nullptr, "base64", R"({"line_width":0})");
  CHECK(cvt_result_status(result.get()) == CVT_OK);
  CHECK(std::string_view(cvt_result_message(result.get())).empty());
  CHECK(output_text(result) == "SGVsbG8=");
  CHECK(std::string_view(cvt_result_format(result.get())) == "base64");
  CHECK(std::string_view(cvt_result_extension(result.get())) == "b64");
  CHECK(std::string_view(cvt_result_mime(result.get())) == "text/plain");
  CHECK(std::string_view(cvt_result_warnings_json(result.get())) == "[]");

  // Round trip back, with explicit source and forgiving ids.
  const ResultPtr back = convert("SGVsbG8=", nullptr, ".B64", "BIN");
  CHECK(cvt_result_status(back.get()) == CVT_OK);
  CHECK(output_text(back) == "Hello");
  CHECK(std::string_view(cvt_result_mime(back.get())) == "application/octet-stream");

  // Empty input, with data NULL.
  const ResultPtr empty(cvt_convert(nullptr, 0, nullptr, "bin", "hex", nullptr, nullptr, nullptr));
  CHECK(cvt_result_status(empty.get()) == CVT_OK);
  CHECK(cvt_result_size(empty.get()) == 0);
  CHECK(cvt_result_data(empty.get()) == nullptr);

  // Warnings surface as a JSON array of strings.
  const ResultPtr warned = convert("caf\xE9", "menu.txt", "txt", "txt", R"({"typo":1})");
  CHECK(cvt_result_status(warned.get()) == CVT_OK);
  CHECK(output_text(warned) == "caf\xC3\xA9");
  const auto warnings = util::parse_json(cvt_result_warnings_json(warned.get()));
  REQUIRE_OK(warnings);
  REQUIRE(warnings->size() == 2);
  CHECK((*warnings)[std::size_t{0}].as_string() == "option 'typo' is not used by this conversion");
  CHECK((*warnings)[std::size_t{1}].as_string().find("Windows-1252") != std::string::npos);
}

TEST_CASE("capi: every failure has a status, a message and no data") {
  struct Case {
    ResultPtr result;
    int status;
  };
  std::vector<Case> cases;
  cases.push_back({convert("x", nullptr, "txt", nullptr), CVT_ERR_INVALID_ARGUMENT});
  cases.push_back({convert("x", nullptr, "txt", ""), CVT_ERR_INVALID_ARGUMENT});
  cases.push_back({ResultPtr(cvt_convert(nullptr, 5, nullptr, "txt", "hex", nullptr, nullptr, nullptr)), CVT_ERR_INVALID_ARGUMENT});
  cases.push_back({convert("x", nullptr, "txt", "hex", "{not json"), CVT_ERR_INVALID_ARGUMENT});
  cases.push_back({convert("x", nullptr, "txt", "hex", "[1,2]"), CVT_ERR_INVALID_ARGUMENT});
  cases.push_back({convert("x", nullptr, "txt", "base64", R"({"line_width":"wide"})"), CVT_ERR_INVALID_ARGUMENT});
  cases.push_back({convert("x", nullptr, "txt", "no-such-format"), CVT_ERR_UNKNOWN_FORMAT});
  cases.push_back({convert("x", nullptr, "no-such-format", "hex"), CVT_ERR_UNKNOWN_FORMAT});
  cases.push_back({convert(std::string_view("\x01\x02\xff", 3), nullptr, nullptr, "hex"), CVT_ERR_UNKNOWN_FORMAT});
  cases.push_back({convert("x", nullptr, "txt", "png"), CVT_ERR_UNSUPPORTED_CONVERSION});
  cases.push_back({convert("x", nullptr, "bin", "bin"), CVT_ERR_UNSUPPORTED_CONVERSION});
  cases.push_back({convert("not base64 !!", nullptr, "base64", "bin"), CVT_ERR_DECODE});
  cases.push_back({convert("abc", nullptr, "hex", "bin"), CVT_ERR_DECODE});
  cases.push_back({convert("0123456789", nullptr, "bin", "hex", R"({"limits":{"max_output_bytes":10}})"), CVT_ERR_LIMIT});
  cases.push_back({convert("0123456789", nullptr, "bin", "hex", R"({"limits":{"max_input_bytes":9}})"), CVT_ERR_LIMIT});
  cases.push_back({convert("x", nullptr, "bin", "hex", nullptr, [](double, const char*, void*) { return 1; }), CVT_ERR_CANCELLED});
  for (const Case& c : cases) {
    CHECK(cvt_result_status(c.result.get()) == c.status);
    CHECK(std::strlen(cvt_result_message(c.result.get())) > 0);
    CHECK(cvt_result_size(c.result.get()) == 0);
    CHECK(cvt_result_data(c.result.get()) == nullptr);
    CHECK(std::string_view(cvt_result_format(c.result.get())).empty());
    CHECK(std::string_view(cvt_result_extension(c.result.get())).empty());
    CHECK(std::string_view(cvt_result_warnings_json(c.result.get())) == "[]");
  }
}

TEST_CASE("capi: result accessors tolerate NULL") {
  CHECK(cvt_result_status(nullptr) == CVT_ERR_INVALID_ARGUMENT);
  CHECK(std::string_view(cvt_result_message(nullptr)).empty());
  CHECK(cvt_result_data(nullptr) == nullptr);
  CHECK(cvt_result_size(nullptr) == 0);
  CHECK(std::string_view(cvt_result_format(nullptr)).empty());
  CHECK(std::string_view(cvt_result_extension(nullptr)).empty());
  CHECK(std::string_view(cvt_result_mime(nullptr)).empty());
  CHECK(std::string_view(cvt_result_warnings_json(nullptr)) == "[]");
}

TEST_CASE("capi: the progress callback") {
  struct Seen {
    std::vector<double> fractions;
    std::vector<std::string> stages;
    int cancel_after = 0;
  } seen;
  const cvt_progress_fn callback = [](double fraction, const char* stage, void* user) {
    auto* state = static_cast<Seen*>(user);
    state->fractions.push_back(fraction);
    state->stages.emplace_back(stage != nullptr ? stage : "(null)");
    return state->cancel_after > 0 && static_cast<int>(state->fractions.size()) >= state->cancel_after ? 1 : 0;
  };
  const ResultPtr result = convert("progress", nullptr, "txt", "hex", nullptr, callback, &seen);
  CHECK(cvt_result_status(result.get()) == CVT_OK);
  REQUIRE(seen.fractions.size() >= 3);
  CHECK(seen.fractions.front() == 0.0);
  CHECK(seen.fractions.back() == 1.0);
  CHECK(seen.stages.front() == "Reading");
  CHECK(seen.stages.back() == "Done");
  for (std::size_t i = 1; i < seen.fractions.size(); ++i) CHECK(seen.fractions[i - 1] <= seen.fractions[i]);

  Seen second;
  second.cancel_after = 2;
  const ResultPtr cancelled = convert("progress", nullptr, "txt", "hex", nullptr, callback, &second);
  CHECK(cvt_result_status(cancelled.get()) == CVT_ERR_CANCELLED);
  CHECK(second.fractions.size() == 2);
}

TEST_CASE("capi: converting files") {
  auto temp = util::TempDir::create("cvt-test");
  REQUIRE_OK(temp);
  const std::string input = util::path_to_utf8(temp->path() / util::path_from_utf8("donn\xC3\xA9" "es.txt"));
  const std::string output = util::path_to_utf8(temp->path() / "out.hex");
  REQUIRE_OK(util::write_file(util::path_from_utf8(input), as_bytes("file body")));

  const ResultPtr ok(cvt_convert_file(input.c_str(), output.c_str(), nullptr, "hex", nullptr, nullptr, nullptr));
  CHECK(cvt_result_status(ok.get()) == CVT_OK);
  CHECK(cvt_result_size(ok.get()) == 0);  // the data went to the file
  CHECK(std::string_view(cvt_result_format(ok.get())) == "hex");
  CHECK(std::string_view(cvt_result_extension(ok.get())) == "hex");
  CHECK(test::str(*util::read_file(util::path_from_utf8(output))) == "66696c6520626f6479");

  const std::string missing = util::path_to_utf8(temp->path() / "missing.txt");
  const std::string never = util::path_to_utf8(temp->path() / "never.hex");
  const auto status_of = [](cvt_result* raw) {
    const ResultPtr result(raw);
    REQUIRE(result != nullptr);
    return cvt_result_status(result.get());
  };
  CHECK(status_of(cvt_convert_file(missing.c_str(), never.c_str(), nullptr, "hex", nullptr, nullptr, nullptr)) == CVT_ERR_IO);
  CHECK(status_of(cvt_convert_file(nullptr, never.c_str(), nullptr, "hex", nullptr, nullptr, nullptr)) == CVT_ERR_INVALID_ARGUMENT);
  CHECK(status_of(cvt_convert_file(input.c_str(), nullptr, nullptr, "hex", nullptr, nullptr, nullptr)) == CVT_ERR_INVALID_ARGUMENT);
  CHECK(status_of(cvt_convert_file(input.c_str(), "", nullptr, "hex", nullptr, nullptr, nullptr)) == CVT_ERR_INVALID_ARGUMENT);
  CHECK(status_of(cvt_convert_file(input.c_str(), never.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr)) == CVT_ERR_INVALID_ARGUMENT);
  CHECK(status_of(cvt_convert_file(input.c_str(), never.c_str(), nullptr, "png", nullptr, nullptr, nullptr)) == CVT_ERR_UNSUPPORTED_CONVERSION);
  CHECK(status_of(cvt_convert_file(input.c_str(), never.c_str(), "hex", "bin", nullptr, nullptr, nullptr)) == CVT_ERR_DECODE);
  CHECK(status_of(cvt_convert_file(input.c_str(), never.c_str(), nullptr, "hex", "{", nullptr, nullptr)) == CVT_ERR_INVALID_ARGUMENT);
  CHECK(status_of(cvt_convert_file(input.c_str(), never.c_str(), nullptr, "hex", nullptr,
                                   [](double, const char*, void*) { return 1; }, nullptr)) == CVT_ERR_CANCELLED);
  CHECK_FALSE(std::filesystem::exists(util::path_from_utf8(never)));
}

TEST_CASE("capi: conversions run concurrently from many threads") {
  constexpr int thread_count = 8;
  constexpr int rounds = 200;
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < thread_count; ++t) {
    threads.emplace_back([t, &failures] {
      const Bytes data = test::random_bytes(512 + static_cast<std::size_t>(t), static_cast<std::uint32_t>(t) + 1);
      for (int i = 0; i < rounds; ++i) {
        cvt_result* encoded = cvt_convert(reinterpret_cast<const std::uint8_t*>(data.data()), data.size(), nullptr,
                                          "bin", i % 2 == 0 ? "base64" : "hex", nullptr, nullptr, nullptr);
        cvt_result* decoded = cvt_convert(cvt_result_data(encoded), cvt_result_size(encoded), nullptr,
                                          cvt_result_format(encoded), "bin", nullptr, nullptr, nullptr);
        const bool same = cvt_result_status(decoded) == CVT_OK && cvt_result_size(decoded) == data.size() &&
                          ::cvt::util::compare_bytes(cvt_result_data(decoded), data.data(), data.size()) == 0;
        if (!same) ++failures;
        cvt_result_free(encoded);
        cvt_result_free(decoded);
        char* formats = i % 50 == 0 ? cvt_formats_json() : nullptr;
        char* detected = cvt_detect(reinterpret_cast<const std::uint8_t*>(data.data()), data.size(), "x.b64");
        cvt_string_free(formats);
        cvt_string_free(detected);
      }
    });
  }
  for (std::thread& thread : threads) thread.join();
  CHECK(failures.load() == 0);
}
