// cvt_pack: several named files into one archive ("Save all as one archive").
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "integration/integration_support.hpp"
#include "ir/archive.hpp"

using namespace cvt;

TEST_CASE("integration/pack: several files pack into one archive") {
  const Bytes a = test::fixture("detect/text/plain.txt");
  const Bytes b = test::fixture("detect/image/pattern.png");
  const cvt_pack_input inputs[] = {
      {reinterpret_cast<const std::uint8_t*>(a.data()), a.size(), "notes.txt"},
      {reinterpret_cast<const std::uint8_t*>(b.data()), b.size(), "../pictures/pattern.png"},
      {reinterpret_cast<const std::uint8_t*>(a.data()), a.size(), "notes.txt"},
  };
  for (const char* target : {"zip", "7z", "tar", "tar.gz"}) {
    INFO(target);
    cvt_result* result = cvt_pack(inputs, 3, target, R"({"compression_level":9})", nullptr, nullptr);
    REQUIRE(cvt_result_status(result) == CVT_OK);
    CHECK(std::string_view(cvt_result_format(result)) == target);
    const Bytes packed(reinterpret_cast<const std::byte*>(cvt_result_data(result)),
                       reinterpret_cast<const std::byte*>(cvt_result_data(result)) + cvt_result_size(result));
    cvt_result_free(result);
    const auto archive = test::read_as<ir::Archive>(target, packed);
    REQUIRE_OK(archive);
    std::vector<std::string> names;
    for (const ir::ArchiveEntry& e : archive->entries) {
      if (e.is_file()) names.push_back(e.path);
    }
    std::ranges::sort(names);
    CHECK(names == std::vector<std::string>{"notes (2).txt", "notes.txt", "pictures/pattern.png"});
    for (const ir::ArchiveEntry& e : archive->entries) {
      if (e.path == "pictures/pattern.png") CHECK(e.bytes == b);
    }
  }
  // Progress reaches the caller and cancelling stops the pack.
  int calls = 0;
  cvt_result* cancelled = cvt_pack(inputs, 3, "zip", nullptr,
                                   [](double, const char*, void* user) {
                                     ++*static_cast<int*>(user);
                                     return 1;
                                   },
                                   &calls);
  CHECK(cvt_result_status(cancelled) == CVT_ERR_CANCELLED);
  CHECK(calls >= 1);
  cvt_result_free(cancelled);
  cvt_result* empty = cvt_pack(nullptr, 0, "zip", nullptr, nullptr, nullptr);
  CHECK(cvt_result_status(empty) == CVT_ERR_INVALID_ARGUMENT);
  cvt_result_free(empty);
  cvt_result* picture = cvt_pack(inputs, 3, "png", nullptr, nullptr, nullptr);
  CHECK(cvt_result_status(picture) == CVT_ERR_UNSUPPORTED_CONVERSION);
  cvt_result_free(picture);
}
