// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// Whole-app tests on the headless platform: screens render in both themes,
// the picker and keyboard work, the intro and logo follow the storyboard,
// the in-house file browser, and a full conversion through the UI.
#include <cmath>

#include "app/app.hpp"
#include "app/automation.hpp"
#include "app/file_browser.hpp"
#include "app/logo.hpp"
#include "base/resources.hpp"
#include "platform/fileio.hpp"
#include "platform/platform.hpp"
#include "support/test.hpp"

using namespace cvd;
using namespace cvd::app;

namespace {

std::string scratch(const std::string& name) {
  const std::string dir = fileio::join(CVD_TEST_TMP_DIR, name);
  fileio::remove_all(dir);
  fileio::make_directories(dir);
  return dir;
}

struct AppHarness {
  std::unique_ptr<Platform> platform;
  std::unique_ptr<App> app;
  gfx::Bitmap frame;
  double t = 0.0;

  explicit AppHarness(const std::string& name, bool skip_intro = true) {
    PlatformOptions po;
    po.headless = true;
    platform = create_headless_platform(po);
    AppOptions o;
    o.skip_intro = skip_intro;
    const std::string dir = scratch(name);
    o.config_dir = fileio::join(dir, "config");
    o.temp_dir = fileio::join(dir, "tmp");
    fileio::make_directories(o.temp_dir);
    o.workers = 2;
    app = std::make_unique<App>(*platform, o);
    frame.resize(960, 640);
  }
  void step(int n = 1) {
    for (int i = 0; i < n; ++i) {
      app->on_frame(frame, 1.0f, t);
      t += 1.0 / 60.0;
    }
  }
  void key(Key k, Modifiers m = {}) {
    Event e;
    e.type = EventType::KeyDown;
    e.key = k;
    e.mods = m;
    app->on_event(e);
  }
  void type(const std::string& s) {
    Event e;
    e.type = EventType::Text;
    e.text = s;
    app->on_event(e);
  }
  /// Clicks the last element with this label (later = drawn on top / further down).
  bool click(const std::string& label) {
    const auto& nodes = app->ui().access_nodes();
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
      const auto& n = *it;
      if (n.label == label && !n.rect.empty()) {
        Event e;
        e.pos = n.rect.center();
        e.type = EventType::MouseMove;
        app->on_event(e);
        e.type = EventType::MouseDown;
        app->on_event(e);
        e.type = EventType::MouseUp;
        app->on_event(e);
        return true;
      }
    }
    return false;
  }
  /// Fraction of pixels that differ from the background colour.
  double ink() const {
    const std::uint32_t bg = frame.at(frame.width - 2, frame.height / 2);
    std::size_t n = 0;
    for (const auto p : frame.pixels) n += p != bg ? 1 : 0;
    return static_cast<double>(n) / static_cast<double>(frame.pixels.size());
  }
};

}  // namespace

TEST_CASE("desktop/logo: storyboard geometry and tracks from animation.json") {
  AppHarness h("logo");
  const Logo& logo = Logo::instance();
  REQUIRE(logo.valid());
  CHECK_APPROX(logo.centerline_length(), 1511.841f, 0.5f);
  CHECK(logo.intro_duration_ms() == 1800.0f);
  CHECK(logo.spinner_duration_ms() == 1400.0f);
  const auto* seed = logo.intro().find("seed");
  REQUIRE(seed != nullptr);
  CHECK(seed->value("scale", 0.0f, 1.0f) == 0.0f);
  CHECK(seed->value("scale", 380.0f, 1.0f) == 1.0f);
  CHECK(seed->value("scale", 200.0f, 1.0f) > 0.9f);  // overshoot easing is well past half way
  const auto* trace = logo.intro().find("trace");
  REQUIRE(trace != nullptr);
  CHECK(trace->value("trimEnd", 100.0f, 1.0f) == 0.0f);
  CHECK(trace->value("trimEnd", 1240.0f, 0.0f) == 1.0f);
  const auto* stage = logo.intro().find("stage");
  REQUIRE(stage != nullptr);
  CHECK(stage->value("rotationDeg", 0.0f, 0.0f) == -8.0f);
  CHECK_APPROX(stage->value("scale", 1350.0f, 1.0f), 1.03f, 1e-4f);
  CHECK(stage->value("scale", 1800.0f, 0.0f) == 1.0f);
  const auto* runner = logo.spinner().find("runner");
  REQUIRE(runner != nullptr);
  CHECK(runner->value("trimEnd", 630.0f, 0.0f) == 1.0f);
  CHECK(runner->value("trimStart", 1400.0f, 0.0f) == 1.0f);

  // The last intro frame equals the static mark; progress p = 1 too.
  gfx::Bitmap a(128, 128), b(128, 128), c(128, 128);
  gfx::Canvas ca(a), cb(b), cc(c);
  logo.draw_intro(ca, {0, 0, 128, 128}, 1800.0f, false);
  logo.draw_static(cb, {0, 0, 128, 128}, false);
  logo.draw_progress(cc, {0, 0, 128, 128}, 1.0f, false);
  CHECK(a.pixels == b.pixels);
  CHECK(c.pixels == b.pixels);
  // Mid-intro frames differ from the end state and are not empty.
  gfx::Bitmap mid(128, 128);
  gfx::Canvas cm(mid);
  logo.draw_intro(cm, {0, 0, 128, 128}, 600.0f, false);
  CHECK(mid.pixels != b.pixels);
  int lit = 0;
  for (const auto p : mid.pixels) lit += (p >> 24) > 0 ? 1 : 0;
  CHECK(lit > 200);
}

TEST_CASE("desktop/app: intro plays, can be skipped and leaves the home screen") {
  AppHarness h("intro", false);
  h.step(10);
  CHECK(h.app->intro_active());
  CHECK(h.app->dump_state()["intro"].str() == "playing");
  h.key(Key::Space);  // any key skips
  h.step(40);
  CHECK_FALSE(h.app->intro_active());
  CHECK(h.app->dump_state()["screen"].str() == "home");
  CHECK(h.ink() > 0.01);
}

TEST_CASE("desktop/app: every screen renders in both themes") {
  AppHarness h("screens");
  for (const char* theme : {"dark", "light"}) {
    Settings s = h.app->state().settings;
    s.theme = std::string(theme) == "dark" ? ThemeMode::Dark : ThemeMode::Light;
    h.app->apply_settings(s);
    for (const Screen screen : {Screen::Home, Screen::Formats, Screen::Settings, Screen::Licenses}) {
      h.app->navigate(screen);
      h.step(30);
      INFO(theme, " ", screen_name(screen));
      CHECK(h.app->dump_state()["theme"].str() == theme);
      CHECK(h.app->dump_state()["screen"].str() == screen_name(screen));
      CHECK(h.ink() > 0.02);
      CHECK_FALSE(h.app->ui().access_nodes().empty());
    }
  }
}

TEST_CASE("desktop/app: queue, picker search and keyboard selection") {
  AppHarness h("picker");
  const std::string dir = scratch("picker-files");
  const std::string a = fileio::join(dir, "one.txt");
  const std::string b = fileio::join(dir, "two.txt");
  fileio::write_text(a, "one");
  fileio::write_text(b, "two");
  Event drop;
  drop.type = EventType::Drop;
  drop.paths = {a, b};
  h.app->on_event(drop);
  h.step(30);
  REQUIRE(h.app->state().items().size() == 2u);
  // The shared-format control is offered.
  bool group = false;
  for (const auto& n : h.app->ui().access_nodes()) group = group || n.label.find("TXT files") != std::string::npos;
  (void)group;
  CHECK(h.app->state().shared_sources().size() == 1u);

  const std::uint64_t id = h.app->state().items()[0].id;
  h.app->open_picker(id);
  h.step(5);
  CHECK(h.app->picker_open());
  h.type("hexa");
  h.step(3);
  CHECK(h.app->picker_query == "hexa");
  h.key(Key::Enter);
  h.step(3);
  CHECK_FALSE(h.app->picker_open());
  CHECK(h.app->state().items()[0].target == "hex");
  // Escape closes without changing anything.
  h.app->open_picker(id);
  h.step(3);
  h.key(Key::Down);
  h.key(Key::Escape);
  h.step(3);
  CHECK_FALSE(h.app->picker_open());
  CHECK(h.app->state().items()[0].target == "hex");
  // Group picker applies to every file of the format.
  h.app->open_picker(id, "txt");
  h.step(3);
  h.app->select_target("base64");
  CHECK(h.app->state().items()[0].target == "base64");
  CHECK(h.app->state().items()[1].target == "base64");
}

TEST_CASE("desktop/app: converting through the UI and saving the results") {
  AppHarness h("convert-ui");
  const std::string dir = scratch("convert-ui-files");
  const std::string a = fileio::join(dir, "hello.txt");
  fileio::write_text(a, "Hello, world!\n");
  h.app->add_files({a});
  h.step(30);
  REQUIRE(h.app->state().set_target(h.app->state().items()[0].id, "base64"));
  h.step(2);
  REQUIRE(h.click("Convert"));
  h.step(2);
  h.app->state().wait_idle();
  h.step(30);
  const Json state = h.app->dump_state();
  CHECK(state["items"][0]["status"].str() == "done");
  CHECK(state["items"][0]["outputName"].str() == "hello.b64");
  CHECK(state["items"][0]["outputSize"].as_int() == 21);
  const std::string out = fileio::join(dir, "out");
  CHECK(h.app->state().save_all_to(out) == 1);
  CHECK(fileio::read_text(fileio::join(out, "hello.b64")).value_or("") == "SGVsbG8sIHdvcmxkIQo=\n");
  // Saved files can be revealed; the result row announces sizes.
  bool result_label = false;
  for (const auto& n : h.app->ui().access_nodes()) result_label = result_label || n.label.find(" to 21 bytes") != std::string::npos;
  CHECK(result_label);
}

TEST_CASE("desktop/app: settings changes apply and persist") {
  AppHarness h("settings-ui");
  h.app->navigate(Screen::Settings);
  h.step(20);
  Settings s = h.app->state().settings;
  s.text_scale_percent = 130;
  s.theme = ThemeMode::Light;
  h.app->apply_settings(s);
  h.step(5);
  CHECK(h.app->zoom() == 1.3f);
  CHECK_FALSE(h.app->dark());
  CHECK(h.app->dump_state()["window"]["zoom"].as_number() == static_cast<double>(1.3f));
  // Keyboard zoom shortcut.
  Modifiers m;
#if defined(__APPLE__)
  m.super = true;
#else
  m.ctrl = true;
#endif
  h.key(Key::Digit0, m);
  h.step(3);
  CHECK(h.app->zoom() == 1.0f);
}

TEST_CASE("desktop/filebrowser: navigation, selection and results") {
  const std::string dir = scratch("browser");
  fileio::make_directories(fileio::join(dir, "folder"));
  fileio::write_text(fileio::join(dir, "b.txt"), "b");
  fileio::write_text(fileio::join(dir, "a.txt"), "a");
  fileio::write_text(fileio::join(dir, ".hidden"), "h");
  std::vector<std::string> result;
  bool called = false;
  FileBrowser open(FileBrowser::Mode::OpenFiles, "Open", dir, "", [&](std::vector<std::string> r) {
    result = std::move(r);
    called = true;
  });
  REQUIRE(open.entries().size() == 3u);  // hidden files are not listed by default
  CHECK(open.entries()[0].name == "folder");  // folders first, then by name
  CHECK(open.entries()[1].name == "a.txt");
  open.toggle(1, false);
  open.toggle(2, true);  // extend the selection
  CHECK(open.selection().size() == 2u);
  open.accept();
  CHECK(called);
  CHECK(result.size() == 2u);
  CHECK(open.closed());

  FileBrowser nav(FileBrowser::Mode::OpenFiles, "Open", dir, "", [](std::vector<std::string>) {});
  nav.activate(0);  // opens the folder
  CHECK(nav.directory() == fileio::join(dir, "folder"));
  CHECK(nav.entries().empty());
  nav.go_up();
  CHECK(nav.entries().size() == 3u);

  std::vector<std::string> saved;
  FileBrowser save(FileBrowser::Mode::SaveFile, "Save", dir, "out.b64", [&](std::vector<std::string> r) { saved = r; });
  save.accept();
  REQUIRE(saved.size() == 1u);
  CHECK(saved[0] == fileio::join(dir, "out.b64"));

  std::vector<std::string> folder;
  FileBrowser choose(FileBrowser::Mode::ChooseFolder, "Folder", dir, "", [&](std::vector<std::string> r) { folder = r; });
  CHECK(choose.entries().size() == 1u);  // only folders
  choose.accept();
  REQUIRE(folder.size() == 1u);
  CHECK(folder[0] == choose.directory());

  bool cancelled = false;
  FileBrowser cancel(FileBrowser::Mode::OpenFiles, "Open", dir, "", [&](std::vector<std::string> r) { cancelled = r.empty(); });
  cancel.cancel();
  CHECK(cancelled);
}

TEST_CASE("desktop/automation: dotted lookups into the state dump") {
  const Json j = Json::parse(R"({"items":[{"status":"done"},{"status":"ready"}],"screen":"home"})").value();
  REQUIRE(Automation::lookup(j, "items.1.status") != nullptr);
  CHECK(Automation::lookup(j, "items.1.status")->str() == "ready");
  CHECK(Automation::lookup(j, "screen")->str() == "home");
  CHECK(Automation::lookup(j, "items.5.status") == nullptr);
  CHECK(Automation::lookup(j, "nope") == nullptr);
}
