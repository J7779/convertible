// The public C++ convenience header, used together with the internal engine
// headers in one translation unit: their names must not collide.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <filesystem>

#include "convertible/convertible.hpp"
#include "engine/convert.hpp"
#include "engine/detect.hpp"
#include "engine/planner.hpp"
#include "support/test_support.hpp"
#include "util/fs.hpp"
#include "util/json.hpp"

namespace {

// Field by field: GCC flags designated initializers that leave members out
// (-Wmissing-field-initializers), and the tests build warning-free.
cvt::ConvertParams make_params(std::string target, std::string source = {}, std::string options = {}) {
  cvt::ConvertParams p;
  p.target = std::move(target);
  p.source = std::move(source);
  p.options_json = std::move(options);
  return p;
}
std::span<const std::uint8_t> u8(std::string_view text) {
  return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}
std::string text_of(const cvt::Conversion& result) {
  const auto data = result.data();
  return std::string(reinterpret_cast<const char*>(data.data()), data.size());
}
}  // namespace

TEST_CASE("cpp api: catalogue calls") {
  CHECK(cvt::version() == "0.1.0");
  CHECK(cvt::util::parse_json(cvt::formats_json()).value().size() >= 100);
  CHECK(cvt::detect(u8("\x89PNG\r\n\x1a\n")) == "png");
  CHECK(cvt::detect(u8(""), "song.flac") == "flac");
  CHECK_FALSE(cvt::detect(u8("")).has_value());
  CHECK(cvt::targets_json("nope") == "[]");
  CHECK(cvt::targets_json("txt").find("\"base64\"") != std::string::npos);
  CHECK(cvt::options_schema_json("txt", "hex").find("\"uppercase\"") != std::string::npos);
}

TEST_CASE("cpp api: conversions") {
  cvt::ConvertParams params;
  params.target = "hex";
  params.filename = "word.txt";
  params.options_json = R"({"uppercase":true})";
  int calls = 0;
  params.progress = [&calls](double, std::string_view) {
    ++calls;
    return false;
  };
  const cvt::Conversion result = cvt::convert(u8("Hi!"), params);
  REQUIRE(result.ok());
  CHECK(static_cast<bool>(result));
  CHECK(result.status() == CVT_OK);
  CHECK(text_of(result) == "486921");
  CHECK(result.format() == "hex");
  CHECK(result.extension() == "hex");
  CHECK(result.mime() == "text/plain");
  CHECK(result.message().empty());
  CHECK(result.warnings_json() == "[]");
  CHECK(result.get() != nullptr);
  CHECK(calls >= 3);

  params.progress = [](double, std::string_view) { return true; };
  CHECK(cvt::convert(u8("Hi!"), params).status() == CVT_ERR_CANCELLED);
  params.progress = [](double, std::string_view) -> bool { throw 1; };  // must not cross the C boundary
  CHECK(cvt::convert(u8("Hi!"), params).status() == CVT_ERR_CANCELLED);
  params.progress = nullptr;
  params.target = "png";
  const cvt::Conversion failed = cvt::convert(u8("Hi!"), params);
  CHECK_FALSE(failed.ok());
  CHECK(failed.status() == CVT_ERR_UNSUPPORTED_CONVERSION);
  CHECK_FALSE(failed.message().empty());
  CHECK(failed.data().empty());

  cvt::Conversion moved = cvt::convert(u8("Hi!"), make_params("hex", "txt"));
  cvt::Conversion owner = std::move(moved);
  CHECK(text_of(owner) == "486921");
}

TEST_CASE("cpp api: file conversions") {
  auto temp = cvt::util::TempDir::create("cvt-test");
  REQUIRE_OK(temp);
  const std::filesystem::path input = temp->path() / cvt::util::path_from_utf8("\xE2\x82\xAC.txt");
  const std::filesystem::path output = temp->path() / "out.b64";
  REQUIRE_OK(cvt::util::write_file(input, cvt::as_bytes("euro")));
  const cvt::Conversion result = cvt::convert_file(input, output, make_params("base64", "", R"({"line_width":0})"));
  REQUIRE(result.ok());
  CHECK(result.data().empty());
  CHECK(result.extension() == "b64");
  CHECK(cvt::test::str(*cvt::util::read_file(output)) == "ZXVybw==");
  CHECK(cvt::convert_file(temp->path() / "missing", output, make_params("hex")).status() == CVT_ERR_IO);
}
