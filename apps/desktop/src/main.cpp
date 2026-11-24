// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// Entry point: parses the command line, creates the platform layer and runs the app.
//
//   Convertible [files...]
//   Convertible --headless --automation script.json [--out dir] [--size 1120x760] [--scale 2]
//   Options: --skip-intro, --config-dir <dir>, --version, --help
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "app/app.hpp"
#include "app/binding.hpp"
#include "platform/platform.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

int run_app(const std::vector<std::string>& args) {
  using namespace cvd;
  app::AppOptions options;
  PlatformOptions popts;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    const auto next = [&]() -> std::string { return i + 1 < args.size() ? args[++i] : std::string(); };
    if (a == "--headless") {
      popts.headless = true;
    } else if (a == "--automation") {
      options.automation_script = next();
    } else if (a == "--out") {
      options.automation_out = next();
    } else if (a == "--skip-intro") {
      options.skip_intro = true;
    } else if (a == "--config-dir") {
      options.config_dir = next();
    } else if (a == "--scale") {
      popts.scale = static_cast<float>(std::atof(next().c_str()));
    } else if (a == "--size") {
      const std::string v = next();
      const std::size_t x = v.find('x');
      if (x != std::string::npos) {
        popts.width = std::max(320, std::atoi(v.substr(0, x).c_str()));
        popts.height = std::max(240, std::atoi(v.substr(x + 1).c_str()));
      }
    } else if (a == "--version") {
      std::printf("Convertible %s (core %s)\n", CVD_VERSION_STRING, app::core_version().c_str());
      return 0;
    } else if (a == "--help" || a == "-h") {
      std::printf(
          "Usage: Convertible [files...]\n"
          "       Convertible --headless --automation <script.json> [--out <dir>] [--size WxH] [--scale S]\n"
          "Options: --skip-intro  --config-dir <dir>  --version\n");
      return 0;
    } else if (a.rfind("-psn_", 0) == 0) {
      // Legacy macOS process serial number argument.
    } else if (!a.empty() && a[0] != '-') {
      options.open_files.push_back(a);
    }
  }
  if (popts.headless && options.automation_script.empty()) {
    std::fprintf(stderr, "--headless needs --automation <script.json>\n");
    return 2;
  }
  std::unique_ptr<Platform> platform = popts.headless ? create_headless_platform(popts) : create_platform(popts);
  if (!platform || !platform->create_window("Convertible", popts.width, popts.height)) {
    std::fprintf(stderr, "Convertible: could not open a window\n");
    return 1;
  }
  int code = 0;
  {
    app::App application(*platform, options);
    code = platform->run(application);
    if (application.automation_failures() > 0) code = 1;
  }
  return code;
}

}  // namespace

#if defined(_WIN32)
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::string> args;
  for (int i = 1; argv && i < argc; ++i) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? static_cast<std::size_t>(n - 1) : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, s.data(), n, nullptr, nullptr);
    args.push_back(s);
  }
  if (argv) LocalFree(argv);
  // Automation runs from a console: reuse the parent's console for messages.
  if (AttachConsole(ATTACH_PARENT_PROCESS)) {
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
  }
  return run_app(args);
}
#else
int main(int argc, char** argv) {
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
  return run_app(args);
}
#endif
