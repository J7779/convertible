// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// Windows platform layer: Win32 window and message loop with per-monitor DPI
// (v2), DIB presentation of the software-rendered frame, OLE drag and drop
// (WM_DROPFILES as a fallback), IFileDialog open/save/folder pickers,
// Explorer integration, clipboard, dark title bar and system settings.
// UI Automation (screen readers) is not wired up yet; the toolkit's
// accessibility tree is available through AppHandler::accessibility().
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "platform/fileio.hpp"
#include "platform/platform.hpp"

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>

namespace cvd {

namespace {

constexpr UINT kWakeMessage = WM_APP + 1;
constexpr UINT_PTR kResizeTimer = 1;

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

std::string narrow(const wchar_t* s, int len = -1) {
  if (!s) return {};
  if (len < 0) len = static_cast<int>(wcslen(s));
  if (len == 0) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, s, len, nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s, len, out.data(), n, nullptr, nullptr);
  return out;
}

Modifiers current_mods() {
  Modifiers m;
  m.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  m.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
  m.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
  m.super = ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) != 0;
  return m;
}

Key key_from(WPARAM vk) {
  switch (vk) {
    case VK_TAB: return Key::Tab;
    case VK_RETURN: return Key::Enter;
    case VK_ESCAPE: return Key::Escape;
    case VK_SPACE: return Key::Space;
    case VK_BACK: return Key::Backspace;
    case VK_DELETE: return Key::Delete;
    case VK_LEFT: return Key::Left;
    case VK_RIGHT: return Key::Right;
    case VK_UP: return Key::Up;
    case VK_DOWN: return Key::Down;
    case VK_HOME: return Key::Home;
    case VK_END: return Key::End;
    case VK_PRIOR: return Key::PageUp;
    case VK_NEXT: return Key::PageDown;
    case 'A': return Key::A;
    case 'C': return Key::C;
    case 'F': return Key::F;
    case 'N': return Key::N;
    case 'O': return Key::O;
    case 'Q': return Key::Q;
    case 'S': return Key::S;
    case 'V': return Key::V;
    case 'W': return Key::W;
    case 'X': return Key::X;
    case 'Z': return Key::Z;
    case VK_OEM_COMMA: return Key::Comma;
    case VK_OEM_PLUS:
    case VK_ADD: return Key::Plus;
    case VK_OEM_MINUS:
    case VK_SUBTRACT: return Key::Minus;
    case '0':
    case VK_NUMPAD0: return Key::Digit0;
    case '1':
    case VK_NUMPAD1: return Key::Digit1;
    case '2':
    case VK_NUMPAD2: return Key::Digit2;
    case '3':
    case VK_NUMPAD3: return Key::Digit3;
    case VK_F1: return Key::F1;
    default: return Key::Other;
  }
}

std::vector<std::string> paths_from_hdrop(HDROP drop) {
  std::vector<std::string> out;
  const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
  for (UINT i = 0; i < count; ++i) {
    const UINT len = DragQueryFileW(drop, i, nullptr, 0);
    std::wstring buf(static_cast<std::size_t>(len) + 1, L'\0');
    DragQueryFileW(drop, i, buf.data(), len + 1);
    buf.resize(len);
    out.push_back(narrow(buf.c_str(), static_cast<int>(buf.size())));
  }
  return out;
}

std::string known_folder(REFKNOWNFOLDERID id) {
  PWSTR path = nullptr;
  std::string out;
  if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &path)) && path) out = narrow(path);
  if (path) CoTaskMemFree(path);
  return out;
}

class WinPlatform;

/// OLE drop target forwarding enter/over/leave/drop to the platform.
class DropTarget final : public IDropTarget {
 public:
  explicit DropTarget(WinPlatform* p) : platform_(p) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (riid == IID_IUnknown || riid == IID_IDropTarget) {
      *out = static_cast<IDropTarget*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
  ULONG STDMETHODCALLTYPE Release() override { return static_cast<ULONG>(InterlockedDecrement(&refs_)); }
  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override;
  HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* effect) override;
  HRESULT STDMETHODCALLTYPE DragLeave() override;
  HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override;

 private:
  WinPlatform* platform_;
  LONG refs_ = 1;
  bool accept_ = false;
};

class WinPlatform final : public Platform {
 public:
  explicit WinPlatform(const PlatformOptions& o) : options_(o), drop_target_(this) {}

  ~WinPlatform() override {
    if (ole_ok_ && hwnd_) RevokeDragDrop(hwnd_);
    if (ole_ok_) OleUninitialize();
  }

  bool create_window(const std::string& title, int width, int height) override {
    // The manifest asks for per-monitor v2 already; this covers unusual launches.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ole_ok_ = SUCCEEDED(OleInitialize(nullptr));
    instance_ = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = &WinPlatform::wndproc;
    wc.hInstance = instance_;
    wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"ConvertibleWindow";
    if (!RegisterClassExW(&wc)) return false;
    cursors_[0] = LoadCursorW(nullptr, IDC_ARROW);
    cursors_[1] = LoadCursorW(nullptr, IDC_HAND);
    cursors_[2] = LoadCursorW(nullptr, IDC_IBEAM);
    cursors_[3] = LoadCursorW(nullptr, IDC_SIZEWE);

    // Size for the DPI of the monitor the window will appear on.
    const POINT origin{0, 0};
    HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(monitor, &mi);
    UINT dpi = 96;
    {
      HDC screen = GetDC(nullptr);
      dpi = static_cast<UINT>(GetDeviceCaps(screen, LOGPIXELSX));
      ReleaseDC(nullptr, screen);
    }
    const float s = options_.scale > 0.0f ? options_.scale : static_cast<float>(dpi) / 96.0f;
    RECT r{0, 0, static_cast<LONG>(std::lround(static_cast<float>(width) * s)),
           static_cast<LONG>(std::lround(static_cast<float>(height) * s))};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    const int x = mi.rcWork.left + std::max(0, static_cast<int>((mi.rcWork.right - mi.rcWork.left - w) / 2));
    const int y = mi.rcWork.top + std::max(0, static_cast<int>((mi.rcWork.bottom - mi.rcWork.top - h) / 2));
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, widen(title).c_str(), WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr,
                            instance_, this);
    if (!hwnd_) return false;
    if (!ole_ok_ || FAILED(RegisterDragDrop(hwnd_, &drop_target_))) DragAcceptFiles(hwnd_, TRUE);
    update_size();
    return true;
  }

  int run(AppHandler& handler) override {
    handler_ = &handler;
    update_chrome();
    ShowWindow(hwnd_, SW_SHOWDEFAULT);
    UpdateWindow(hwnd_);
    dirty_ = true;
    double last_frame = 0.0;
    while (running_) {
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
          running_ = false;
          break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      if (!running_) break;
      drain_posted();
      if (!handler.keep_running()) break;
      const double t = now();
      const bool animate = handler.wants_frame();
      const double wake = handler.wake_time();
      if (dirty_ || (animate && t - last_frame >= 1.0 / 62.0) || (wake > 0.0 && t >= wake)) {
        render();
        last_frame = now();
        dirty_ = false;
        if (!handler.keep_running()) break;
        continue;
      }
      DWORD timeout = INFINITE;
      if (animate) timeout = static_cast<DWORD>(std::max(0.0, (last_frame + 1.0 / 60.0 - t) * 1000.0));
      else if (wake > 0.0) timeout = static_cast<DWORD>(std::max(0.0, (wake - t) * 1000.0) + 1.0);
      MsgWaitForMultipleObjectsEx(0, nullptr, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    if (hwnd_ && IsWindow(hwnd_)) DestroyWindow(hwnd_);
    handler_ = nullptr;
    return 0;
  }

  void request_frame() override { PostMessageW(hwnd_, kWakeMessage, 0, 0); }

  void post(std::function<void()> fn) override {
    {
      std::lock_guard lock(mutex_);
      posted_.push_back(std::move(fn));
    }
    PostMessageW(hwnd_, kWakeMessage, 0, 0);
  }

  void quit() override { running_ = false; }

  [[nodiscard]] double now() const override {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
  }
  [[nodiscard]] float scale() const override { return scale_; }
  [[nodiscard]] Vec2 window_size() const override {
    return {static_cast<float>(width_px_) / scale_, static_cast<float>(height_px_) / scale_};
  }
  void set_title(const std::string& title) override { SetWindowTextW(hwnd_, widen(title).c_str()); }

  [[nodiscard]] bool system_dark_mode() const override {
    DWORD value = 1;
    DWORD size = sizeof value;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
      return value == 0;
    }
    return false;
  }

  [[nodiscard]] bool system_reduce_motion() const override {
    BOOL animations = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0)) return animations == FALSE;
    return false;
  }

  [[nodiscard]] std::string clipboard_text() const override {
    std::string out;
    if (!OpenClipboard(hwnd_)) return out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
      if (const auto* text = static_cast<const wchar_t*>(GlobalLock(h))) {
        out = narrow(text);
        GlobalUnlock(h);
      }
    }
    CloseClipboard();
    return out;
  }

  void set_clipboard_text(const std::string& text) override {
    const std::wstring w = widen(text);
    if (!OpenClipboard(hwnd_)) return;
    EmptyClipboard();
    const std::size_t bytes = (w.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
      if (void* dst = GlobalLock(mem)) {
        std::memcpy(dst, w.c_str(), bytes);
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
      } else {
        GlobalFree(mem);
      }
    }
    CloseClipboard();
  }

  [[nodiscard]] bool has_native_dialogs() const override { return true; }

  void open_files_dialog(const std::string& title, PathsCallback done) override {
    std::vector<std::string> out;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
      DWORD flags = 0;
      dlg->GetOptions(&flags);
      dlg->SetOptions(flags | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
      dlg->SetTitle(widen(title).c_str());
      dlg->SetOkButtonLabel(L"Add");
      if (SUCCEEDED(dlg->Show(hwnd_))) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dlg->GetResults(&items))) {
          DWORD count = 0;
          items->GetCount(&count);
          for (DWORD i = 0; i < count; ++i) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(items->GetItemAt(i, &item))) {
              PWSTR path = nullptr;
              if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                out.push_back(narrow(path));
                CoTaskMemFree(path);
              }
              item->Release();
            }
          }
          items->Release();
        }
      }
      dlg->Release();
    }
    done(std::move(out));
    dirty_ = true;
  }

  static void set_folder(IFileDialog* dlg, const std::string& directory) {
    if (directory.empty()) return;
    IShellItem* folder = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(widen(directory).c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
      dlg->SetFolder(folder);
      folder->Release();
    }
  }

  static std::optional<std::string> result_path(IFileDialog* dlg) {
    std::optional<std::string> out;
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        out = narrow(path);
        CoTaskMemFree(path);
      }
      item->Release();
    }
    return out;
  }

  void save_file_dialog(const std::string& title, const std::string& directory, const std::string& suggested,
                        PathCallback done) override {
    std::optional<std::string> out;
    IFileSaveDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
      DWORD flags = 0;
      dlg->GetOptions(&flags);
      dlg->SetOptions(flags | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM);
      dlg->SetTitle(widen(title).c_str());
      dlg->SetFileName(widen(suggested).c_str());
      set_folder(dlg, directory);
      if (SUCCEEDED(dlg->Show(hwnd_))) out = result_path(dlg);
      dlg->Release();
    }
    done(out);
    dirty_ = true;
  }

  void choose_folder_dialog(const std::string& title, const std::string& directory, PathCallback done) override {
    std::optional<std::string> out;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
      DWORD flags = 0;
      dlg->GetOptions(&flags);
      dlg->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
      dlg->SetTitle(widen(title).c_str());
      dlg->SetOkButtonLabel(L"Save here");
      set_folder(dlg, directory);
      if (SUCCEEDED(dlg->Show(hwnd_))) out = result_path(dlg);
      dlg->Release();
    }
    done(out);
    dirty_ = true;
  }

  bool reveal_in_file_manager(const std::string& path) override {
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(widen(path).c_str());
    if (!pidl) return false;
    const HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
    ILFree(pidl);
    return SUCCEEDED(hr);
  }

  bool open_with_default_app(const std::string& path) override {
    const auto r = reinterpret_cast<INT_PTR>(
        ShellExecuteW(hwnd_, L"open", widen(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return r > 32;
  }

  [[nodiscard]] std::string file_manager_name() const override { return "File Explorer"; }

  [[nodiscard]] std::string config_directory() const override {
    const std::string base = known_folder(FOLDERID_RoamingAppData);
    return fileio::join(base.empty() ? fileio::home_directory() : base, "Convertible");
  }

  [[nodiscard]] std::string documents_directory() const override {
    const std::string docs = known_folder(FOLDERID_Documents);
    return docs.empty() ? fileio::home_directory() : docs;
  }

  bool set_automation_size(int width, int height, float sc) override {
    if (sc > 0.0f) {
      options_.scale = sc;
      scale_ = sc;
    }
    const UINT dpi = GetDpiForWindow(hwnd_);
    RECT r{0, 0, static_cast<LONG>(std::lround(static_cast<float>(width) * scale_)),
           static_cast<LONG>(std::lround(static_cast<float>(height) * scale_))};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    SetWindowPos(hwnd_, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    return true;
  }

  // ---- Called by the drop target and the window procedure ------------------

  Vec2 logical_from_screen(POINTL pt) const {
    POINT p{pt.x, pt.y};
    ScreenToClient(hwnd_, &p);
    return {static_cast<float>(p.x) / scale_, static_cast<float>(p.y) / scale_};
  }

  void dispatch(const Event& e) {
    if (handler_) handler_->on_event(e);
    dirty_ = true;
  }

 private:
  static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
      const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
      return DefWindowProcW(hwnd, msg, wp, lp);
    }
    auto* self = reinterpret_cast<WinPlatform*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->handle(hwnd, msg, wp, lp);
  }

  Vec2 logical(LPARAM lp) const {
    return {static_cast<float>(GET_X_LPARAM(lp)) / scale_, static_cast<float>(GET_Y_LPARAM(lp)) / scale_};
  }

  void mouse_button(EventType type, MouseButton button, LPARAM lp, int clicks) {
    Event e;
    e.type = type;
    e.button = button;
    e.pos = logical(lp);
    e.clicks = clicks;
    e.mods = current_mods();
    dispatch(e);
  }

  LRESULT handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
      case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (bitmap_.width > 0) present(dc);
        EndPaint(hwnd, &ps);
        return 0;
      }
      case WM_ERASEBKGND: return 1;
      case WM_SIZE:
        update_size();
        if (handler_ && wp != SIZE_MINIMIZED) render();
        return 0;
      case WM_DPICHANGED: {
        scale_ = options_.scale > 0.0f ? options_.scale : static_cast<float>(HIWORD(wp)) / 96.0f;
        const auto* suggested = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        dirty_ = true;
        return 0;
      }
      case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lp);
        const float s = hwnd_ ? scale_ : 1.0f;
        RECT r{0, 0, static_cast<LONG>(520.0f * s), static_cast<LONG>(460.0f * s)};
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, static_cast<UINT>(96.0f * s));
        info->ptMinTrackSize.x = r.right - r.left;
        info->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
      }
      case WM_ENTERSIZEMOVE: SetTimer(hwnd, kResizeTimer, 16, nullptr); return 0;
      case WM_EXITSIZEMOVE: KillTimer(hwnd, kResizeTimer); return 0;
      case WM_TIMER:
        if (wp == kResizeTimer && handler_ && (handler_->wants_frame() || dirty_)) render();
        return 0;
      case WM_MOUSEMOVE: {
        if (!tracking_) {
          TRACKMOUSEEVENT tme{};
          tme.cbSize = sizeof tme;
          tme.dwFlags = TME_LEAVE;
          tme.hwndTrack = hwnd;
          tracking_ = TrackMouseEvent(&tme) != FALSE;
        }
        Event e;
        e.type = EventType::MouseMove;
        e.pos = logical(lp);
        e.mods = current_mods();
        dispatch(e);
        return 0;
      }
      case WM_MOUSELEAVE: {
        tracking_ = false;
        Event e;
        e.type = EventType::MouseLeave;
        dispatch(e);
        return 0;
      }
      case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        mouse_button(EventType::MouseDown, MouseButton::Left, lp, 1);
        return 0;
      case WM_LBUTTONDBLCLK:
        SetCapture(hwnd);
        mouse_button(EventType::MouseDown, MouseButton::Left, lp, 2);
        return 0;
      case WM_LBUTTONUP:
        ReleaseCapture();
        mouse_button(EventType::MouseUp, MouseButton::Left, lp, 1);
        return 0;
      case WM_RBUTTONDOWN: mouse_button(EventType::MouseDown, MouseButton::Right, lp, 1); return 0;
      case WM_RBUTTONUP: mouse_button(EventType::MouseUp, MouseButton::Right, lp, 1); return 0;
      case WM_MOUSEWHEEL:
      case WM_MOUSEHWHEEL: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &p);
        const float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / static_cast<float>(WHEEL_DELTA) * 48.0f;
        Event e;
        e.type = EventType::Scroll;
        e.pos = {static_cast<float>(p.x) / scale_, static_cast<float>(p.y) / scale_};
        e.mods = current_mods();
        if (msg == WM_MOUSEWHEEL) e.scroll = {0.0f, -delta};
        else e.scroll = {delta, 0.0f};
        dispatch(e);
        return 0;
      }
      case WM_KEYDOWN:
      case WM_SYSKEYDOWN: {
        Event e;
        e.type = EventType::KeyDown;
        e.key = key_from(wp);
        e.mods = current_mods();
        e.repeat = (lp & (1 << 30)) != 0;
        dispatch(e);
        if (msg == WM_SYSKEYDOWN) break;  // keep Alt+F4 and the system menu
        return 0;
      }
      case WM_KEYUP:
      case WM_SYSKEYUP: {
        Event e;
        e.type = EventType::KeyUp;
        e.key = key_from(wp);
        e.mods = current_mods();
        dispatch(e);
        break;
      }
      case WM_CHAR: {
        const auto unit = static_cast<wchar_t>(wp);
        if (unit >= 0xD800 && unit <= 0xDBFF) {
          high_surrogate_ = unit;
          return 0;
        }
        std::wstring w;
        if (unit >= 0xDC00 && unit <= 0xDFFF && high_surrogate_) {
          w.push_back(high_surrogate_);
          w.push_back(unit);
          high_surrogate_ = 0;
        } else {
          w.push_back(unit);
        }
        const Modifiers m = current_mods();
        if (w.size() == 1 && (w[0] < 0x20 || w[0] == 0x7F)) return 0;
        if (m.ctrl && !m.alt) return 0;  // shortcuts arrive as key events
        Event e;
        e.type = EventType::Text;
        e.text = narrow(w.c_str(), static_cast<int>(w.size()));
        dispatch(e);
        return 0;
      }
      case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
          SetCursor(cursors_[static_cast<int>(cursor_)]);
          return TRUE;
        }
        break;
      case WM_DROPFILES: {
        auto* drop = reinterpret_cast<HDROP>(wp);
        POINT p{};
        DragQueryPoint(drop, &p);
        Event e;
        e.type = EventType::Drop;
        e.pos = {static_cast<float>(p.x) / scale_, static_cast<float>(p.y) / scale_};
        e.paths = paths_from_hdrop(drop);
        DragFinish(drop);
        dispatch(e);
        return 0;
      }
      case WM_SETTINGCHANGE:
        if (lp && std::wstring(reinterpret_cast<const wchar_t*>(lp)) == L"ImmersiveColorSet") {
          Event e;
          e.type = EventType::AppearanceChanged;
          dispatch(e);
        }
        break;
      case WM_KILLFOCUS: {
        Event e;
        e.type = EventType::FocusLost;
        dispatch(e);
        break;
      }
      case kWakeMessage: dirty_ = true; return 0;
      case WM_CLOSE: {
        Event e;
        e.type = EventType::CloseRequested;
        dispatch(e);
        running_ = false;
        DestroyWindow(hwnd);
        return 0;
      }
      case WM_DESTROY:
        hwnd_ = nullptr;
        PostQuitMessage(0);
        return 0;
      default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }

  void update_size() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    width_px_ = std::max<LONG>(1, rc.right - rc.left);
    height_px_ = std::max<LONG>(1, rc.bottom - rc.top);
    if (options_.scale <= 0.0f) scale_ = static_cast<float>(GetDpiForWindow(hwnd_)) / 96.0f;
    if (scale_ <= 0.0f) scale_ = 1.0f;
  }

  void update_chrome() {
    if (!handler_ || !hwnd_) return;
    const bool dark = handler_->chrome_dark();
    const Color c = handler_->chrome_color();
    const COLORREF caption = RGB(static_cast<int>(c.r * 255.0f), static_cast<int>(c.g * 255.0f), static_cast<int>(c.b * 255.0f));
    if (dark == chrome_dark_ && caption == chrome_color_) return;
    chrome_dark_ = dark;
    chrome_color_ = caption;
    const BOOL use_dark = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &use_dark, sizeof use_dark);
    DwmSetWindowAttribute(hwnd_, 35 /* DWMWA_CAPTION_COLOR (Windows 11) */, &caption, sizeof caption);
  }

  void drain_posted() {
    std::deque<std::function<void()>> work;
    {
      std::lock_guard lock(mutex_);
      work.swap(posted_);
    }
    for (auto& fn : work) {
      fn();
      dirty_ = true;
    }
  }

  void render() {
    if (!handler_ || !hwnd_) return;
    if (bitmap_.width != width_px_ || bitmap_.height != height_px_) bitmap_.resize(width_px_, height_px_);
    handler_->on_frame(bitmap_, scale_, now());
    HDC dc = GetDC(hwnd_);
    present(dc);
    ReleaseDC(hwnd_, dc);
    cursor_ = handler_->cursor();
    update_chrome();
  }

  void present(HDC dc) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = bitmap_.width;
    bi.bmiHeader.biHeight = -bitmap_.height;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(dc, 0, 0, static_cast<DWORD>(bitmap_.width), static_cast<DWORD>(bitmap_.height), 0, 0, 0,
                      static_cast<UINT>(bitmap_.height), bitmap_.pixels.data(), &bi, DIB_RGB_COLORS);
  }

  friend class DropTarget;

  PlatformOptions options_;
  AppHandler* handler_ = nullptr;
  HINSTANCE instance_ = nullptr;
  HWND hwnd_ = nullptr;
  HCURSOR cursors_[4] = {};
  CursorShape cursor_ = CursorShape::Arrow;
  DropTarget drop_target_;
  bool ole_ok_ = false;
  bool running_ = true;
  bool dirty_ = true;
  bool tracking_ = false;
  bool chrome_dark_ = false;
  COLORREF chrome_color_ = 0xFFFFFFFF;
  wchar_t high_surrogate_ = 0;
  float scale_ = 1.0f;
  int width_px_ = 1;
  int height_px_ = 1;
  gfx::Bitmap bitmap_;
  std::mutex mutex_;
  std::deque<std::function<void()>> posted_;
};

bool has_files(IDataObject* data) {
  FORMATETC fmt{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  return data && data->QueryGetData(&fmt) == S_OK;
}

HRESULT DropTarget::DragEnter(IDataObject* data, DWORD, POINTL pt, DWORD* effect) {
  accept_ = has_files(data);
  *effect = accept_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
  Event e;
  e.type = EventType::DragEnter;
  e.pos = platform_->logical_from_screen(pt);
  if (accept_) platform_->dispatch(e);
  return S_OK;
}

HRESULT DropTarget::DragOver(DWORD, POINTL pt, DWORD* effect) {
  *effect = accept_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
  if (accept_) {
    Event e;
    e.type = EventType::DragOver;
    e.pos = platform_->logical_from_screen(pt);
    platform_->dispatch(e);
  }
  return S_OK;
}

HRESULT DropTarget::DragLeave() {
  Event e;
  e.type = EventType::DragLeave;
  platform_->dispatch(e);
  return S_OK;
}

HRESULT DropTarget::Drop(IDataObject* data, DWORD, POINTL pt, DWORD* effect) {
  *effect = DROPEFFECT_NONE;
  Event e;
  e.type = EventType::Drop;
  e.pos = platform_->logical_from_screen(pt);
  FORMATETC fmt{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM medium{};
  if (data && SUCCEEDED(data->GetData(&fmt, &medium))) {
    if (auto* drop = static_cast<HDROP>(GlobalLock(medium.hGlobal))) {
      e.paths = paths_from_hdrop(drop);
      GlobalUnlock(medium.hGlobal);
    }
    ReleaseStgMedium(&medium);
    *effect = DROPEFFECT_COPY;
  }
  platform_->dispatch(e);
  return S_OK;
}

}  // namespace

std::unique_ptr<Platform> create_platform(const PlatformOptions& options) {
  if (options.headless) return create_headless_platform(options);
  return std::make_unique<WinPlatform>(options);
}

}  // namespace cvd
