// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// Linux platform layer on plain Xlib (also runs on Wayland through XWayland):
// window and event loop, XIM text input, XDND file drops, CLIPBOARD,
// XShm/XPutImage presentation, _NET_WM_ICON drawn from the brand mark.
// File dialogs are drawn in-app by the toolkit (has_native_dialogs() = false).
// Our headers come first: Xlib defines macros such as None, Bool and Status.
#include "app/logo.hpp"
#include "base/text_util.hpp"
#include "platform/fileio.hpp"
#include "platform/platform.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#if defined(CVD_HAVE_XSHM)
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#endif
// X.h names an event mask bit "CursorShape"; the platform interface uses that name.
#undef CursorShape
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>

namespace cvd {

namespace {

double monotonic_seconds() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

Modifiers mods_from(unsigned state) {
  Modifiers m;
  m.shift = (state & ShiftMask) != 0;
  m.ctrl = (state & ControlMask) != 0;
  m.alt = (state & Mod1Mask) != 0;
  m.super = (state & Mod4Mask) != 0;
  return m;
}

Key key_from(KeySym ks) {
  switch (ks) {
    case XK_Tab:
    case XK_ISO_Left_Tab: return Key::Tab;
    case XK_Return:
    case XK_KP_Enter: return Key::Enter;
    case XK_Escape: return Key::Escape;
    case XK_space: return Key::Space;
    case XK_BackSpace: return Key::Backspace;
    case XK_Delete:
    case XK_KP_Delete: return Key::Delete;
    case XK_Left:
    case XK_KP_Left: return Key::Left;
    case XK_Right:
    case XK_KP_Right: return Key::Right;
    case XK_Up:
    case XK_KP_Up: return Key::Up;
    case XK_Down:
    case XK_KP_Down: return Key::Down;
    case XK_Home:
    case XK_KP_Home: return Key::Home;
    case XK_End:
    case XK_KP_End: return Key::End;
    case XK_Page_Up: return Key::PageUp;
    case XK_Page_Down: return Key::PageDown;
    case XK_a:
    case XK_A: return Key::A;
    case XK_c:
    case XK_C: return Key::C;
    case XK_f:
    case XK_F: return Key::F;
    case XK_n:
    case XK_N: return Key::N;
    case XK_o:
    case XK_O: return Key::O;
    case XK_q:
    case XK_Q: return Key::Q;
    case XK_s:
    case XK_S: return Key::S;
    case XK_v:
    case XK_V: return Key::V;
    case XK_w:
    case XK_W: return Key::W;
    case XK_x:
    case XK_X: return Key::X;
    case XK_z:
    case XK_Z: return Key::Z;
    case XK_comma: return Key::Comma;
    case XK_plus:
    case XK_equal:
    case XK_KP_Add: return Key::Plus;
    case XK_minus:
    case XK_KP_Subtract: return Key::Minus;
    case XK_0: return Key::Digit0;
    case XK_1: return Key::Digit1;
    case XK_2: return Key::Digit2;
    case XK_3: return Key::Digit3;
    case XK_F1: return Key::F1;
    default: return Key::Other;
  }
}

std::string read_file_text(const std::string& path) { return fileio::read_text(path).value_or(std::string()); }

std::string percent_decode(std::string_view s) {
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      const int hi = hex(s[i + 1]);
      const int lo = hex(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out += static_cast<char>(hi * 16 + lo);
        i += 2;
        continue;
      }
    }
    out += s[i];
  }
  return out;
}

/// text/uri-list -> local paths.
std::vector<std::string> parse_uri_list(std::string_view text) {
  std::vector<std::string> out;
  for (std::string line : split(text, '\n')) {
    const std::string_view l = trim(line);
    if (l.empty() || l[0] == '#') continue;
    if (l.rfind("file://", 0) == 0) {
      std::string_view rest = l.substr(7);
      const std::size_t slash = rest.find('/');  // skip an optional host name
      if (slash == std::string_view::npos) continue;
      out.push_back(percent_decode(rest.substr(slash)));
    } else if (l[0] == '/') {
      out.emplace_back(l);
    }
  }
  return out;
}

/// Starts a helper (xdg-open) fully detached; true when it could be started.
bool spawn_detached(const std::vector<std::string>& argv) {
  const pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    const pid_t inner = fork();
    if (inner == 0) {
      setsid();
      std::vector<char*> args;
      for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
      args.push_back(nullptr);
      const int devnull = open("/dev/null", O_RDWR);
      if (devnull >= 0) {
        dup2(devnull, 0);
        dup2(devnull, 1);
        dup2(devnull, 2);
      }
      execvp(args[0], args.data());
      _exit(127);
    }
    _exit(inner < 0 ? 1 : 0);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

}  // namespace

class X11Platform final : public Platform {
 public:
  explicit X11Platform(const PlatformOptions& o) : options_(o) {}

  ~X11Platform() override {
    destroy_image();
    if (display_) {
      if (ic_) XDestroyIC(ic_);
      if (im_) XCloseIM(im_);
      if (window_) XDestroyWindow(display_, window_);
      XCloseDisplay(display_);
    }
    if (pipe_[0] >= 0) close(pipe_[0]);
    if (pipe_[1] >= 0) close(pipe_[1]);
  }

  bool create_window(const std::string& title, int width, int height) override {
    display_ = XOpenDisplay(nullptr);
    if (!display_) return false;
    if (pipe(pipe_) != 0) return false;
    fcntl(pipe_[0], F_SETFL, O_NONBLOCK);
    fcntl(pipe_[1], F_SETFL, O_NONBLOCK);
    screen_ = DefaultScreen(display_);
    scale_ = options_.scale > 0.0f ? options_.scale : detect_scale();
    XVisualInfo vinfo{};
    if (XMatchVisualInfo(display_, screen_, 24, TrueColor, &vinfo)) {
      visual_ = vinfo.visual;
      depth_ = 24;
    } else {
      visual_ = DefaultVisual(display_, screen_);
      depth_ = DefaultDepth(display_, screen_);
    }
    const Window root = RootWindow(display_, screen_);
    XSetWindowAttributes attrs{};
    attrs.colormap = XCreateColormap(display_, root, visual_, AllocNone);
    attrs.background_pixel = 0x0D0F16;
    attrs.border_pixel = 0;
    attrs.event_mask = ExposureMask | StructureNotifyMask | KeyPressMask | KeyReleaseMask | ButtonPressMask |
                       ButtonReleaseMask | PointerMotionMask | EnterWindowMask | LeaveWindowMask | FocusChangeMask |
                       PropertyChangeMask;
    const int pw = static_cast<int>(std::lround(static_cast<float>(width) * scale_));
    const int ph = static_cast<int>(std::lround(static_cast<float>(height) * scale_));
    window_ = XCreateWindow(display_, root, 0, 0, static_cast<unsigned>(pw), static_cast<unsigned>(ph), 0, depth_,
                            InputOutput, visual_, CWColormap | CWBackPixel | CWBorderPixel | CWEventMask, &attrs);
    width_px_ = pw;
    height_px_ = ph;
    gc_ = XCreateGC(display_, window_, 0, nullptr);

    atoms_init();
    set_title(title);
    XClassHint* hint = XAllocClassHint();
    hint->res_name = const_cast<char*>("convertible-desktop");
    hint->res_class = const_cast<char*>("Convertible");
    XSetClassHint(display_, window_, hint);
    XFree(hint);
    XSizeHints* size = XAllocSizeHints();
    size->flags = PMinSize;
    size->min_width = static_cast<int>(520.0f * scale_);
    size->min_height = static_cast<int>(460.0f * scale_);
    XSetWMNormalHints(display_, window_, size);
    XFree(size);
    Atom protocols[] = {wm_delete_};
    XSetWMProtocols(display_, window_, protocols, 1);
    const long xdnd_version = 5;
    XChangeProperty(display_, window_, xdnd_aware_, XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&xdnd_version), 1);
    set_icon();

    // Input method for composed text.
    XSetLocaleModifiers("");
    im_ = XOpenIM(display_, nullptr, nullptr, nullptr);
    if (im_) {
      ic_ = XCreateIC(im_, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, window_, XNFocusWindow,
                      window_, nullptr);
    }
#if defined(CVD_HAVE_XSHM)
    int major = 0, minor = 0;
    Bool pixmaps = False;
    use_shm_ = XShmQueryVersion(display_, &major, &minor, &pixmaps) == True;
    if (std::getenv("CVD_NO_XSHM")) use_shm_ = false;
#endif
    cursors_[0] = XCreateFontCursor(display_, XC_left_ptr);
    cursors_[1] = XCreateFontCursor(display_, XC_hand2);
    cursors_[2] = XCreateFontCursor(display_, XC_xterm);
    cursors_[3] = XCreateFontCursor(display_, XC_sb_h_double_arrow);
    return true;
  }

  int run(AppHandler& handler) override {
    handler_ = &handler;
    XMapWindow(display_, window_);
    XFlush(display_);
    bool dirty = true;
    double last_frame = 0.0;
    while (running_) {
      // Drain X events.
      while (XPending(display_) > 0) {
        XEvent ev;
        XNextEvent(display_, &ev);
        if (XFilterEvent(&ev, None)) continue;
        handle(ev, dirty);
        if (!running_) break;
      }
      drain_posted(dirty);
      if (!running_ || !handler.keep_running()) break;
      const double t = now();
      const bool animate = handler.wants_frame();
      const double wake = handler.wake_time();
      if (dirty || (animate && t - last_frame >= 1.0 / 62.0) || (wake > 0.0 && t >= wake)) {
        render();
        last_frame = now();
        dirty = false;
        if (!handler.keep_running()) break;
        continue;
      }
      int timeout = -1;
      if (animate) timeout = std::max(0, static_cast<int>((last_frame + 1.0 / 60.0 - t) * 1000.0));
      else if (wake > 0.0) timeout = std::max(0, static_cast<int>((wake - t) * 1000.0) + 1);
      pollfd fds[2] = {{ConnectionNumber(display_), POLLIN, 0}, {pipe_[0], POLLIN, 0}};
      XFlush(display_);
      poll(fds, 2, timeout);
      if (fds[1].revents & POLLIN) {
        char buf[64];
        while (read(pipe_[0], buf, sizeof buf) > 0) {
        }
        dirty = true;
      }
    }
    handler_ = nullptr;
    return 0;
  }

  void request_frame() override { wake(); }

  void post(std::function<void()> fn) override {
    {
      std::lock_guard lock(mutex_);
      posted_.push_back(std::move(fn));
    }
    wake();
  }

  void quit() override { running_ = false; }
  [[nodiscard]] double now() const override { return monotonic_seconds(); }
  [[nodiscard]] float scale() const override { return scale_; }
  [[nodiscard]] Vec2 window_size() const override {
    return {static_cast<float>(width_px_) / scale_, static_cast<float>(height_px_) / scale_};
  }

  void set_title(const std::string& title) override {
    XStoreName(display_, window_, title.c_str());
    XChangeProperty(display_, window_, net_wm_name_, utf8_string_, 8, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(title.c_str()), static_cast<int>(title.size()));
  }

  [[nodiscard]] bool system_dark_mode() const override {
    if (const char* gtk = std::getenv("GTK_THEME")) {
      const std::string v = ascii_lower(gtk);
      return v.find("dark") != std::string::npos;
    }
    const std::string ini = read_file_text(config_home() + "/gtk-3.0/settings.ini") +
                            read_file_text(config_home() + "/gtk-4.0/settings.ini");
    const std::string low = ascii_lower(ini);
    if (low.find("gtk-application-prefer-dark-theme=1") != std::string::npos ||
        low.find("gtk-application-prefer-dark-theme=true") != std::string::npos) {
      return true;
    }
    const std::size_t p = low.find("gtk-theme-name");
    if (p != std::string::npos) {
      const std::string line = low.substr(p, low.find('\n', p) - p);
      return line.find("dark") != std::string::npos;
    }
    return true;  // the brand's primary appearance
  }

  [[nodiscard]] bool system_reduce_motion() const override {
    const std::string low = ascii_lower(read_file_text(config_home() + "/gtk-3.0/settings.ini"));
    return low.find("gtk-enable-animations=0") != std::string::npos ||
           low.find("gtk-enable-animations=false") != std::string::npos;
  }

  [[nodiscard]] std::string clipboard_text() const override {
    auto* self = const_cast<X11Platform*>(this);
    return self->read_clipboard();
  }

  void set_clipboard_text(const std::string& text) override {
    clipboard_ = text;
    XSetSelectionOwner(display_, clipboard_atom_, window_, CurrentTime);
  }

  [[nodiscard]] bool has_native_dialogs() const override { return false; }
  void open_files_dialog(const std::string&, PathsCallback done) override { done({}); }
  void save_file_dialog(const std::string&, const std::string&, const std::string&, PathCallback done) override {
    done(std::nullopt);
  }
  void choose_folder_dialog(const std::string&, const std::string&, PathCallback done) override { done(std::nullopt); }

  bool reveal_in_file_manager(const std::string& path) override {
    // There is no portable "select this file" request without D-Bus; open the folder.
    const std::string dir = fileio::is_directory(path) ? path : path_parent(path);
    return spawn_detached({"xdg-open", dir.empty() ? "/" : dir});
  }
  bool open_with_default_app(const std::string& path) override { return spawn_detached({"xdg-open", path}); }
  [[nodiscard]] std::string file_manager_name() const override { return "Files"; }

  [[nodiscard]] std::string config_directory() const override { return config_home() + "/convertible"; }

  [[nodiscard]] std::string documents_directory() const override {
    const std::string dirs = read_file_text(config_home() + "/user-dirs.dirs");
    const std::size_t p = dirs.find("XDG_DOCUMENTS_DIR=\"");
    if (p != std::string::npos) {
      const std::size_t start = p + 19;
      std::string v = dirs.substr(start, dirs.find('"', start) - start);
      if (v.rfind("$HOME", 0) == 0) v = fileio::home_directory() + v.substr(5);
      if (fileio::is_directory(v)) return v;
    }
    const std::string docs = fileio::home_directory() + "/Documents";
    return fileio::is_directory(docs) ? docs : fileio::home_directory();
  }

  bool set_automation_size(int width, int height, float sc) override {
    if (sc > 0.0f) scale_ = sc;
    XResizeWindow(display_, window_, static_cast<unsigned>(std::lround(static_cast<float>(width) * scale_)),
                  static_cast<unsigned>(std::lround(static_cast<float>(height) * scale_)));
    XFlush(display_);
    return true;
  }

 private:
  static std::string config_home() {
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) return x;
    return fileio::home_directory() + "/.config";
  }

  float detect_scale() const {
    if (const char* gdk = std::getenv("GDK_SCALE")) {
      const float s = static_cast<float>(std::atof(gdk));
      if (s >= 1.0f && s <= 4.0f) return s;
    }
    if (const char* rm = XResourceManagerString(display_)) {
      const std::string res(rm);
      const std::size_t p = res.find("Xft.dpi:");
      if (p != std::string::npos) {
        const float dpi = static_cast<float>(std::atof(res.c_str() + p + 8));
        if (dpi >= 72.0f && dpi <= 480.0f) return std::round(dpi / 96.0f * 4.0f) / 4.0f;
      }
    }
    return 1.0f;
  }

  void atoms_init() {
    const auto a = [&](const char* name) { return XInternAtom(display_, name, False); };
    wm_delete_ = a("WM_DELETE_WINDOW");
    net_wm_name_ = a("_NET_WM_NAME");
    net_wm_icon_ = a("_NET_WM_ICON");
    utf8_string_ = a("UTF8_STRING");
    clipboard_atom_ = a("CLIPBOARD");
    targets_ = a("TARGETS");
    cvd_selection_ = a("CVD_SELECTION");
    xdnd_aware_ = a("XdndAware");
    xdnd_enter_ = a("XdndEnter");
    xdnd_position_ = a("XdndPosition");
    xdnd_status_ = a("XdndStatus");
    xdnd_leave_ = a("XdndLeave");
    xdnd_drop_ = a("XdndDrop");
    xdnd_finished_ = a("XdndFinished");
    xdnd_selection_ = a("XdndSelection");
    xdnd_type_list_ = a("XdndTypeList");
    xdnd_action_copy_ = a("XdndActionCopy");
    uri_list_ = a("text/uri-list");
  }

  void set_icon() {
    // _NET_WM_ICON: width, height, then ARGB pixels as CARDINALs (longs).
    std::vector<long> data;
    for (const int size : {16, 32, 48, 64, 128}) {
      gfx::Bitmap bmp(size, size);
      gfx::Canvas c(bmp);
      const float r = static_cast<float>(size) * 0.22f;
      c.fill_rrect({0, 0, static_cast<float>(size), static_cast<float>(size)}, r,
                   gfx::Paint::linear({0, 0}, {0, static_cast<float>(size)},
                                      {{0.0f, Color::hex("#1B1E2C")}, {1.0f, Color::hex("#0A0B11")}}));
      const float m = static_cast<float>(size) * 0.14f;
      app::Logo::instance().draw_static(c, {m, m, static_cast<float>(size) - 2 * m, static_cast<float>(size) - 2 * m},
                                        false);
      data.push_back(size);
      data.push_back(size);
      for (const std::uint32_t p : bmp.pixels) {
        // Unpremultiply for the window manager.
        const std::uint32_t a = p >> 24;
        std::uint32_t r8 = (p >> 16) & 0xFF, g8 = (p >> 8) & 0xFF, b8 = p & 0xFF;
        if (a > 0 && a < 255) {
          r8 = std::min(255u, r8 * 255 / a);
          g8 = std::min(255u, g8 * 255 / a);
          b8 = std::min(255u, b8 * 255 / a);
        }
        data.push_back(static_cast<long>((a << 24) | (r8 << 16) | (g8 << 8) | b8));
      }
    }
    XChangeProperty(display_, window_, net_wm_icon_, XA_CARDINAL, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(data.data()), static_cast<int>(data.size()));
  }

  void wake() {
    const char b = 1;
    [[maybe_unused]] const auto n = write(pipe_[1], &b, 1);
  }

  void drain_posted(bool& dirty) {
    std::deque<std::function<void()>> work;
    {
      std::lock_guard lock(mutex_);
      work.swap(posted_);
    }
    for (auto& fn : work) {
      fn();
      dirty = true;
    }
  }

  void dispatch(const Event& e, bool& dirty) {
    if (handler_) handler_->on_event(e);
    dirty = true;
  }

  Vec2 logical(int x, int y) const { return {static_cast<float>(x) / scale_, static_cast<float>(y) / scale_}; }

  void handle(XEvent& ev, bool& dirty) {
    switch (ev.type) {
      case Expose: dirty = true; break;
      case ConfigureNotify:
        if (ev.xconfigure.width != width_px_ || ev.xconfigure.height != height_px_) {
          width_px_ = ev.xconfigure.width;
          height_px_ = ev.xconfigure.height;
          Event e;
          e.type = EventType::Resize;
          dispatch(e, dirty);
        }
        break;
      case MotionNotify: {
        Event e;
        e.type = EventType::MouseMove;
        e.pos = logical(ev.xmotion.x, ev.xmotion.y);
        e.mods = mods_from(ev.xmotion.state);
        dispatch(e, dirty);
        break;
      }
      case ButtonPress:
      case ButtonRelease: {
        const unsigned b = ev.xbutton.button;
        Event e;
        e.pos = logical(ev.xbutton.x, ev.xbutton.y);
        e.mods = mods_from(ev.xbutton.state);
        if (b >= 4 && b <= 7) {
          if (ev.type == ButtonRelease) break;
          e.type = EventType::Scroll;
          const float step = 48.0f;
          if (b == 4) e.scroll = {0.0f, -step};
          if (b == 5) e.scroll = {0.0f, step};
          if (b == 6) e.scroll = {-step, 0.0f};
          if (b == 7) e.scroll = {step, 0.0f};
          if (e.mods.shift) std::swap(e.scroll.x, e.scroll.y);
          dispatch(e, dirty);
          break;
        }
        e.type = ev.type == ButtonPress ? EventType::MouseDown : EventType::MouseUp;
        e.button = b == 3 ? MouseButton::Right : b == 2 ? MouseButton::Middle : MouseButton::Left;
        if (ev.type == ButtonPress && b == 1) {
          const Time t = ev.xbutton.time;
          const bool near = std::abs(ev.xbutton.x - last_click_x_) < 5 && std::abs(ev.xbutton.y - last_click_y_) < 5;
          click_count_ = (t - last_click_time_ < 400 && near) ? click_count_ + 1 : 1;
          last_click_time_ = t;
          last_click_x_ = ev.xbutton.x;
          last_click_y_ = ev.xbutton.y;
        }
        e.clicks = click_count_;
        dispatch(e, dirty);
        break;
      }
      case LeaveNotify: {
        Event e;
        e.type = EventType::MouseLeave;
        dispatch(e, dirty);
        break;
      }
      case FocusIn:
        if (ic_) XSetICFocus(ic_);
        break;
      case FocusOut: {
        if (ic_) XUnsetICFocus(ic_);
        Event e;
        e.type = EventType::FocusLost;
        dispatch(e, dirty);
        break;
      }
      case KeyPress: {
        char buf[64];
        KeySym ks = 0;
        int len = 0;
        if (ic_) {
          Status status = 0;
          len = Xutf8LookupString(ic_, &ev.xkey, buf, sizeof buf - 1, &ks, &status);
          if (status != XLookupChars && status != XLookupBoth) len = 0;
          if (status == XLookupChars) ks = XLookupKeysym(&ev.xkey, 0);
        } else {
          len = XLookupString(&ev.xkey, buf, sizeof buf - 1, &ks, nullptr);
        }
        Event e;
        e.type = EventType::KeyDown;
        e.key = key_from(ks != 0 ? ks : XLookupKeysym(&ev.xkey, 0));
        e.mods = mods_from(ev.xkey.state);
        dispatch(e, dirty);
        if (len > 0 && !e.mods.ctrl && !e.mods.super) {
          std::string text(buf, static_cast<std::size_t>(len));
          if (!text.empty() && static_cast<unsigned char>(text[0]) >= 0x20 && text[0] != 0x7F) {
            Event t;
            t.type = EventType::Text;
            t.text = std::move(text);
            dispatch(t, dirty);
          }
        }
        break;
      }
      case KeyRelease: {
        Event e;
        e.type = EventType::KeyUp;
        e.key = key_from(XLookupKeysym(&ev.xkey, 0));
        e.mods = mods_from(ev.xkey.state);
        dispatch(e, dirty);
        break;
      }
      case ClientMessage: handle_client_message(ev.xclient, dirty); break;
      case SelectionNotify: handle_selection_notify(ev.xselection, dirty); break;
      case SelectionRequest: serve_selection(ev.xselectionrequest); break;
      case SelectionClear:
        if (ev.xselectionclear.selection == clipboard_atom_) clipboard_.clear();
        break;
      default: break;
    }
  }

  void handle_client_message(const XClientMessageEvent& cm, bool& dirty) {
    if (cm.message_type == None) return;
    if (static_cast<Atom>(cm.data.l[0]) == wm_delete_ && cm.format == 32) {
      Event e;
      e.type = EventType::CloseRequested;
      dispatch(e, dirty);
      running_ = false;
      return;
    }
    if (cm.message_type == xdnd_enter_) {
      dnd_source_ = static_cast<Window>(cm.data.l[0]);
      dnd_version_ = static_cast<int>(static_cast<unsigned long>(cm.data.l[1]) >> 24);
      dnd_has_uris_ = false;
      if (cm.data.l[1] & 1) {
        // More than three types: read XdndTypeList from the source.
        Atom type = None;
        int format = 0;
        unsigned long count = 0, after = 0;
        unsigned char* data = nullptr;
        if (XGetWindowProperty(display_, dnd_source_, xdnd_type_list_, 0, 1024, False, XA_ATOM, &type, &format, &count,
                               &after, &data) == Success && data) {
          const auto* atoms = reinterpret_cast<const Atom*>(data);
          for (unsigned long i = 0; i < count; ++i) dnd_has_uris_ = dnd_has_uris_ || atoms[i] == uri_list_;
          XFree(data);
        }
      } else {
        for (int i = 2; i <= 4; ++i) dnd_has_uris_ = dnd_has_uris_ || static_cast<Atom>(cm.data.l[i]) == uri_list_;
      }
      Event e;
      e.type = EventType::DragEnter;
      dispatch(e, dirty);
      return;
    }
    if (cm.message_type == xdnd_position_) {
      const int rx = static_cast<int>((static_cast<unsigned long>(cm.data.l[2]) >> 16) & 0xFFFF);
      const int ry = static_cast<int>(static_cast<unsigned long>(cm.data.l[2]) & 0xFFFF);
      int wx = 0, wy = 0;
      Window child = 0;
      XTranslateCoordinates(display_, RootWindow(display_, screen_), window_, rx, ry, &wx, &wy, &child);
      Event e;
      e.type = EventType::DragOver;
      e.pos = logical(wx, wy);
      dispatch(e, dirty);
      XClientMessageEvent reply{};
      reply.type = ClientMessage;
      reply.display = display_;
      reply.window = dnd_source_;
      reply.message_type = xdnd_status_;
      reply.format = 32;
      reply.data.l[0] = static_cast<long>(window_);
      reply.data.l[1] = dnd_has_uris_ ? 1 : 0;
      reply.data.l[4] = dnd_has_uris_ ? static_cast<long>(xdnd_action_copy_) : 0;
      XSendEvent(display_, dnd_source_, False, NoEventMask, reinterpret_cast<XEvent*>(&reply));
      return;
    }
    if (cm.message_type == xdnd_leave_) {
      Event e;
      e.type = EventType::DragLeave;
      dispatch(e, dirty);
      return;
    }
    if (cm.message_type == xdnd_drop_) {
      if (!dnd_has_uris_) {
        finish_drop(false);
        Event e;
        e.type = EventType::DragLeave;
        dispatch(e, dirty);
        return;
      }
      const Time t = dnd_version_ >= 1 ? static_cast<Time>(cm.data.l[2]) : CurrentTime;
      XConvertSelection(display_, xdnd_selection_, uri_list_, xdnd_selection_, window_, t);
    }
  }

  void finish_drop(bool accepted) {
    if (!dnd_source_) return;
    XClientMessageEvent reply{};
    reply.type = ClientMessage;
    reply.display = display_;
    reply.window = dnd_source_;
    reply.message_type = xdnd_finished_;
    reply.format = 32;
    reply.data.l[0] = static_cast<long>(window_);
    reply.data.l[1] = accepted ? 1 : 0;
    reply.data.l[2] = accepted ? static_cast<long>(xdnd_action_copy_) : 0;
    XSendEvent(display_, dnd_source_, False, NoEventMask, reinterpret_cast<XEvent*>(&reply));
    dnd_source_ = 0;
  }

  std::string read_property(Window w, Atom prop) {
    Atom type = None;
    int format = 0;
    unsigned long count = 0, after = 0;
    unsigned char* data = nullptr;
    std::string out;
    if (XGetWindowProperty(display_, w, prop, 0, 0x7FFFFFF, True, AnyPropertyType, &type, &format, &count, &after,
                           &data) == Success && data) {
      out.assign(reinterpret_cast<const char*>(data), count * static_cast<unsigned long>(format / 8));
      XFree(data);
    }
    return out;
  }

  void handle_selection_notify(const XSelectionEvent& se, bool& dirty) {
    if (se.selection == xdnd_selection_) {
      std::vector<std::string> paths;
      if (se.property != None) paths = parse_uri_list(read_property(window_, se.property));
      finish_drop(!paths.empty());
      Event e;
      e.type = EventType::Drop;
      e.paths = std::move(paths);
      dispatch(e, dirty);
    }
  }

  void serve_selection(const XSelectionRequestEvent& req) {
    XSelectionEvent reply{};
    reply.type = SelectionNotify;
    reply.display = req.display;
    reply.requestor = req.requestor;
    reply.selection = req.selection;
    reply.target = req.target;
    reply.time = req.time;
    reply.property = None;
    const Atom prop = req.property != None ? req.property : req.target;
    if (req.target == targets_) {
      const Atom offered[] = {targets_, utf8_string_, XA_STRING};
      XChangeProperty(display_, req.requestor, prop, XA_ATOM, 32, PropModeReplace,
                      reinterpret_cast<const unsigned char*>(offered), 3);
      reply.property = prop;
    } else if (req.target == utf8_string_ || req.target == XA_STRING) {
      XChangeProperty(display_, req.requestor, prop, req.target, 8, PropModeReplace,
                      reinterpret_cast<const unsigned char*>(clipboard_.data()), static_cast<int>(clipboard_.size()));
      reply.property = prop;
    }
    XSendEvent(display_, req.requestor, False, NoEventMask, reinterpret_cast<XEvent*>(&reply));
  }

  std::string read_clipboard() {
    const Window owner = XGetSelectionOwner(display_, clipboard_atom_);
    if (owner == window_) return clipboard_;
    if (owner == None) return {};
    XConvertSelection(display_, clipboard_atom_, utf8_string_, cvd_selection_, window_, CurrentTime);
    XFlush(display_);
    const double deadline = now() + 0.3;
    while (now() < deadline) {
      XEvent ev;
      if (XCheckTypedWindowEvent(display_, window_, SelectionNotify, &ev)) {
        if (ev.xselection.selection == clipboard_atom_) {
          if (ev.xselection.property == None) return {};
          return read_property(window_, cvd_selection_);
        }
        bool dirty = false;
        handle_selection_notify(ev.xselection, dirty);
      }
      pollfd fd{ConnectionNumber(display_), POLLIN, 0};
      poll(&fd, 1, 10);
    }
    return {};
  }

  void destroy_image() {
    if (!image_) return;
#if defined(CVD_HAVE_XSHM)
    if (shm_attached_) {
      XShmDetach(display_, &shm_);
      shmdt(shm_.shmaddr);
      shm_attached_ = false;
    }
#endif
    image_->data = nullptr;  // pixels belong to us (or to the shm segment)
    XDestroyImage(image_);
    image_ = nullptr;
  }

  void ensure_image(int w, int h) {
    if (image_ && image_->width == w && image_->height == h) return;
    destroy_image();
#if defined(CVD_HAVE_XSHM)
    if (use_shm_) {
      image_ = XShmCreateImage(display_, visual_, static_cast<unsigned>(depth_), ZPixmap, nullptr, &shm_,
                               static_cast<unsigned>(w), static_cast<unsigned>(h));
      if (image_) {
        shm_.shmid = shmget(IPC_PRIVATE, static_cast<std::size_t>(image_->bytes_per_line) * static_cast<std::size_t>(h),
                            IPC_CREAT | 0600);
        if (shm_.shmid >= 0) {
          shm_.shmaddr = image_->data = static_cast<char*>(shmat(shm_.shmid, nullptr, 0));
          shm_.readOnly = False;
          if (shm_.shmaddr != reinterpret_cast<char*>(-1) && XShmAttach(display_, &shm_)) {
            XSync(display_, False);
            shmctl(shm_.shmid, IPC_RMID, nullptr);  // freed once both sides detach
            shm_attached_ = true;
            return;
          }
          shmctl(shm_.shmid, IPC_RMID, nullptr);
        }
        image_->data = nullptr;
        XDestroyImage(image_);
        image_ = nullptr;
      }
      use_shm_ = false;
    }
#endif
    image_ = XCreateImage(display_, visual_, static_cast<unsigned>(depth_), ZPixmap, 0, nullptr,
                          static_cast<unsigned>(w), static_cast<unsigned>(h), 32, w * 4);
  }

  void render() {
    if (!handler_) return;
    const int w = std::max(1, width_px_);
    const int h = std::max(1, height_px_);
    if (bitmap_.width != w || bitmap_.height != h) bitmap_.resize(w, h);
    handler_->on_frame(bitmap_, scale_, now());
    ensure_image(w, h);
    if (!image_) return;
#if defined(CVD_HAVE_XSHM)
    if (shm_attached_) {
      std::memcpy(image_->data, bitmap_.pixels.data(), bitmap_.pixels.size() * 4);
      XShmPutImage(display_, window_, gc_, image_, 0, 0, 0, 0, static_cast<unsigned>(w), static_cast<unsigned>(h), False);
      XSync(display_, False);
    } else
#endif
    {
      image_->data = reinterpret_cast<char*>(bitmap_.pixels.data());
      XPutImage(display_, window_, gc_, image_, 0, 0, 0, 0, static_cast<unsigned>(w), static_cast<unsigned>(h));
      image_->data = nullptr;
      XFlush(display_);
    }
    const CursorShape c = handler_->cursor();
    if (c != cursor_) {
      cursor_ = c;
      XDefineCursor(display_, window_, cursors_[static_cast<int>(c)]);
    }
  }

  PlatformOptions options_;
  AppHandler* handler_ = nullptr;
  Display* display_ = nullptr;
  int screen_ = 0;
  Visual* visual_ = nullptr;
  int depth_ = 24;
  Window window_ = 0;
  GC gc_ = nullptr;
  XIM im_ = nullptr;
  XIC ic_ = nullptr;
  XImage* image_ = nullptr;
#if defined(CVD_HAVE_XSHM)
  XShmSegmentInfo shm_{};
  bool shm_attached_ = false;
#endif
  bool use_shm_ = false;
  Cursor cursors_[4] = {};
  CursorShape cursor_ = CursorShape::Arrow;
  float scale_ = 1.0f;
  int width_px_ = 0;
  int height_px_ = 0;
  bool running_ = true;
  int pipe_[2] = {-1, -1};
  std::mutex mutex_;
  std::deque<std::function<void()>> posted_;
  gfx::Bitmap bitmap_;
  std::string clipboard_;
  Time last_click_time_ = 0;
  int last_click_x_ = 0;
  int last_click_y_ = 0;
  int click_count_ = 1;
  Window dnd_source_ = 0;
  int dnd_version_ = 0;
  bool dnd_has_uris_ = false;
  Atom wm_delete_ = 0, net_wm_name_ = 0, net_wm_icon_ = 0, utf8_string_ = 0, clipboard_atom_ = 0, targets_ = 0,
       cvd_selection_ = 0;
  Atom xdnd_aware_ = 0, xdnd_enter_ = 0, xdnd_position_ = 0, xdnd_status_ = 0, xdnd_leave_ = 0, xdnd_drop_ = 0,
       xdnd_finished_ = 0, xdnd_selection_ = 0, xdnd_type_list_ = 0, xdnd_action_copy_ = 0, uri_list_ = 0;
};

std::unique_ptr<Platform> create_platform(const PlatformOptions& options) {
  if (options.headless) return create_headless_platform(options);
  return std::make_unique<X11Platform>(options);
}

}  // namespace cvd
