// Convertible desktop - end-to-end test harness.
//
// Every test case gets fresh input, output, config, temp and home folders,
// writes or loads an automation script, runs the REAL executable on it
// (headless by default) and then checks what the app left behind: the saved
// files byte for byte, the JSON state dumps and the screenshots.
//
// Only the C++ standard library, the desktop toolkit's JSON type and the
// core's in-house test framework are used; process launching is the one
// OS-specific piece (std::system on POSIX, CreateProcessW on Windows).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.hpp"

namespace e2e {

using cvd::Json;
using Bytes = std::vector<std::uint8_t>;

// ---------------------------------------------------------------------------
// Configuration (from the command line, see e2e_main.cpp)
// ---------------------------------------------------------------------------

struct Config {
  std::string app;        // the desktop executable
  std::string work;       // scratch root (inside the build tree, git-ignored)
  std::string e2e_dir;    // apps/desktop/e2e (scripts, expected data)
  std::string assets;     // repository assets (design tokens)
  bool windowed = false;  // CVD_E2E_WINDOWED=1: also run the real-window subset
};
Config& config();

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

std::string join(const std::string& a, const std::string& b);
/// Same file system path after normalisation (separators differ on Windows).
bool same_path(const std::string& a, const std::string& b);
bool exists(const std::string& path);
bool is_dir(const std::string& path);
void make_dirs(const std::string& path);
void remove_all(const std::string& path);
std::optional<Bytes> read_bytes(const std::string& path);
std::string read_text(const std::string& path);  // "" when missing
bool write_bytes(const std::string& path, const Bytes& data);
bool write_text(const std::string& path, std::string_view text);
/// Names of the entries of a folder (sorted); empty when missing.
std::vector<std::string> list_dir(const std::string& path);
/// Every regular file below `path` (relative names, sorted).
std::vector<std::string> list_files_recursive(const std::string& path);
Bytes to_bytes(std::string_view s);
std::string to_string(const Bytes& b);

// ---------------------------------------------------------------------------
// Independent decoders, encoders and digests (written for the tests)
// ---------------------------------------------------------------------------

/// Standard or URL-safe Base64; whitespace ignored; nullopt on bad input.
std::optional<Bytes> base64_decode(std::string_view text);
/// Hex digits; whitespace, ':' and '-' separators ignored.
std::optional<Bytes> hex_decode(std::string_view text);
/// UTF-8 to UTF-16LE (nullopt on invalid UTF-8).
std::optional<Bytes> utf8_to_utf16le(std::string_view text);
std::uint32_t crc32(const Bytes& data);
std::string hex_lower(const std::uint8_t* data, std::size_t size);
/// From the core's internal util (independent of the app; cross-checked
/// against published vectors in the tests).
std::string md5_hex(const Bytes& data);
std::string sha1_hex(const Bytes& data);
std::string sha256_hex(const Bytes& data);

/// Deterministic pseudo-random bytes.
Bytes pseudo_random(std::size_t n, std::uint32_t seed);
/// An RGB PNG (stored deflate blocks) with a smooth gradient plus noise.
Bytes make_png(int width, int height, std::uint32_t seed);
/// A 16-bit mono PCM WAV with a sine tone.
Bytes make_wav(int sample_rate, double seconds, double frequency);

struct ZipEntry {
  std::string name;
  std::uint16_t method = 0;
  std::uint32_t crc = 0;
  std::uint64_t compressed = 0;
  std::uint64_t uncompressed = 0;
  std::uint64_t local_offset = 0;
};
/// Parses the end-of-central-directory record and the central directory and
/// checks every local header signature; nullopt when anything is malformed.
std::optional<std::vector<ZipEntry>> parse_zip(const Bytes& data);

// ---------------------------------------------------------------------------
// Screenshots (BMP as written by the app: 32-bit, top-down, BGRA)
// ---------------------------------------------------------------------------

struct Rgb {
  int r = 0, g = 0, b = 0;
  bool operator==(const Rgb&) const = default;
};
Rgb parse_hex_colour(std::string_view hex);  // "#RRGGBB"
int colour_distance(Rgb a, Rgb b);           // max channel difference

struct Image {
  int width = 0;
  int height = 0;
  std::vector<Rgb> px;
  [[nodiscard]] Rgb at(int x, int y) const { return px[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)]; }
  [[nodiscard]] bool valid() const { return width > 0 && height > 0; }
};
std::optional<Image> load_bmp(const std::string& path);

struct Box {
  int x = 0, y = 0, w = 0, h = 0;
};
/// Rectangle from a state-dump "rect" ([x, y, w, h] in points) scaled to pixels.
Box box_from(const Json& rect, double scale);
/// Fraction of pixels in `b` whose colour differs from `ref` by more than `tol`.
double ink(const Image& img, Box b, Rgb ref, int tol = 24);
/// Pixels in `b` within `tol` of `c`.
int count_colour(const Image& img, Box b, Rgb c, int tol);
/// Number of pixels that differ between two equally sized images inside `b`.
int diff_pixels(const Image& a, const Image& b, Box box, int tol = 0);

// ---------------------------------------------------------------------------
// State dumps
// ---------------------------------------------------------------------------

/// Parsed JSON file (null Json when missing or malformed).
Json load_json(const std::string& path);
/// Dotted lookup ("items.0.status"); null Json when missing.
const Json& at(const Json& root, std::string_view dotted);
/// The first accessibility node matching label (exact) and optional role.
const Json* node(const Json& state, std::string_view label, std::string_view role = {});
std::vector<const Json*> nodes_with_role(const Json& state, std::string_view role);
/// The design tokens (assets/brand/tokens.json).
const Json& tokens();
Rgb token_colour(bool dark, std::string_view key);

// ---------------------------------------------------------------------------
// Running the app
// ---------------------------------------------------------------------------

struct RunOptions {
  int width = 1120;
  int height = 760;
  double scale = 1.0;
  bool headless = true;
  bool skip_intro = true;
  int workers = 2;
  std::vector<std::string> files;       // positional file arguments (command line)
  std::vector<std::string> extra_args;  // anything else
  std::string config_dir;               // default: the session's config folder
  bool expect_failures = false;         // the script is expected to report failures
};

struct RunResult {
  int exit_code = -1;
  int failures = -1;      // as reported in automation.log ("failures N")
  std::string log;        // automation.log
  std::string console;    // stdout + stderr
  double seconds = 0.0;
};

/// Script placeholders ${NAME} (inside JSON strings) are replaced by the
/// JSON-escaped value; IN, OUT, WORK, FIXTURES are always defined.
using Vars = std::map<std::string, std::string>;

class Session {
 public:
  explicit Session(std::string name);
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  [[nodiscard]] const std::string& name() const { return name_; }
  [[nodiscard]] const std::string& root() const { return root_; }
  [[nodiscard]] std::string in(const std::string& rel = {}) const;
  [[nodiscard]] std::string out(const std::string& rel = {}) const;
  [[nodiscard]] std::string config_dir() const { return join(root_, "config"); }
  [[nodiscard]] std::string temp_dir() const { return join(root_, "tmp"); }
  [[nodiscard]] std::string home_dir() const { return join(root_, "home"); }

  std::string write_input(const std::string& rel, const Bytes& data);
  std::string write_input(const std::string& rel, std::string_view text);
  /// Copies a fixture from apps/desktop/tests/fixtures into the input folder.
  std::string fixture(const std::string& name);

  /// Runs a script file from e2e/scripts.
  RunResult run(const std::string& script_name, const Vars& vars = {}, const RunOptions& options = {});
  /// Runs a script given as JSON text (already substituted).
  RunResult run_inline(const Json& script, const RunOptions& options = {});

  /// Out-folder JSON / BMP helpers.
  [[nodiscard]] Json state(const std::string& rel) const { return load_json(out(rel)); }
  [[nodiscard]] Image shot(const std::string& rel) const;

  /// Copies a picture into the git-ignored failures folder (called by checks).
  void keep_failure(const std::string& path, const std::string& tag);
  /// Checks the run succeeded, that the app removed its session temp folder
  /// and never wrote outside the sandbox; returns false (with messages) otherwise.
  bool ok(const RunResult& r);

 private:
  RunResult launch(const std::string& script_path, const RunOptions& options);

  std::string name_;
  std::string root_;
  int runs_ = 0;
  bool had_failure_ = false;
};

/// Expands ${VAR} placeholders in script text (values JSON-escaped).
std::string substitute(std::string text, const Vars& vars);
/// JSON string-escaped (no quotes).
std::string json_escape(std::string_view s);

// ---------------------------------------------------------------------------
// Layout checks shared by several groups
// ---------------------------------------------------------------------------

/// Interactive roles (button, toggle, slider, textfield, tab, menuitem, link).
bool interactive_role(std::string_view role);
/// Problems found in the accessibility tree's layout: unlabelled controls,
/// controls outside the window or cut off by it, partial overlaps, tiny targets.
std::vector<std::string> layout_problems(const Json& state);

}  // namespace e2e
