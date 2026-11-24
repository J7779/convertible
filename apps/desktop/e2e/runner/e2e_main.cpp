// Convertible desktop - end-to-end test runner (the core's in-house framework).
//
//   convertible_desktop_e2e --app=<executable> --work=<dir> --e2e=<apps/desktop/e2e> --assets=<assets>
//                           [framework options, e.g. --test-case=e2e/convert/*]
//
// CVD_E2E_WINDOWED=1 also runs the subset that drives the real OS window.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#define CVT_TEST_IMPLEMENTATION
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <filesystem>

#include "harness.hpp"
#include "platform/fileio.hpp"
#include "support/test.hpp"

int main(int argc, char** argv) {
  e2e::Config& c = e2e::config();
  std::vector<const char*> rest;
  rest.push_back(argv[0]);
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const auto value = [&](const char* prefix) -> const char* {
      const std::string p = prefix;
      return a.rfind(p, 0) == 0 ? argv[i] + p.size() : nullptr;
    };
    if (const char* v = value("--app=")) c.app = v;
    else if (const char* v2 = value("--work=")) c.work = v2;
    else if (const char* v3 = value("--e2e=")) c.e2e_dir = v3;
    else if (const char* v4 = value("--assets=")) c.assets = v4;
    else rest.push_back(argv[i]);
  }
  if (const char* w = std::getenv("CVD_E2E_WINDOWED")) c.windowed = std::string(w) == "1";
  const bool listing = std::any_of(rest.begin(), rest.end(), [](const char* a) {
    const std::string s = a;
    return s == "-ltc" || s == "--list-test-cases" || s == "-c" || s == "--count" || s == "-h" || s == "--help";
  });
  if (!listing && (c.app.empty() || c.work.empty() || c.e2e_dir.empty() || c.assets.empty())) {
    std::fprintf(stderr, "usage: convertible_desktop_e2e --app=<exe> --work=<dir> --e2e=<dir> --assets=<dir> [options]\n");
    return 2;
  }
  // Scripts hand absolute paths to the app (relative ones resolve against the script).
  for (std::string* p : {&c.app, &c.work, &c.e2e_dir, &c.assets}) {
    if (!p->empty()) *p = cvd::fileio::from_path(std::filesystem::absolute(cvd::fileio::to_path(*p)).lexically_normal());
  }
  if (!c.work.empty()) e2e::make_dirs(c.work);
  return cvt::test::run(static_cast<int>(rest.size()), rest.data());
}
