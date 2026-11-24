// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// macOS platform layer: Cocoa window and event loop, layer-backed presentation
// of the software-rendered frame, drag and drop, open/save panels, Finder
// integration, appearance and accessibility.
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <atomic>
#include <cmath>
#include <deque>
#include <mutex>

#include "platform/fileio.hpp"
#include "platform/platform.hpp"

namespace cvd {
class MacPlatform;
}

@interface CvdView : NSView <NSTextInputClient, NSDraggingDestination>
@property(nonatomic, assign) cvd::MacPlatform* platform;
@end

@interface CvdAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(nonatomic, assign) cvd::MacPlatform* platform;
@end

namespace cvd {

namespace {

Modifiers mods_from(NSEventModifierFlags f) {
  Modifiers m;
  m.shift = (f & NSEventModifierFlagShift) != 0;
  m.ctrl = (f & NSEventModifierFlagControl) != 0;
  m.alt = (f & NSEventModifierFlagOption) != 0;
  m.super = (f & NSEventModifierFlagCommand) != 0;
  return m;
}

Key key_from(NSEvent* e) {
  switch (e.keyCode) {
    case 48: return Key::Tab;
    case 36:
    case 76: return Key::Enter;
    case 53: return Key::Escape;
    case 49: return Key::Space;
    case 51: return Key::Backspace;
    case 117: return Key::Delete;
    case 123: return Key::Left;
    case 124: return Key::Right;
    case 125: return Key::Down;
    case 126: return Key::Up;
    case 115: return Key::Home;
    case 119: return Key::End;
    case 116: return Key::PageUp;
    case 121: return Key::PageDown;
    case 122: return Key::F1;
    default: break;
  }
  NSString* chars = e.charactersIgnoringModifiers.lowercaseString;
  if (chars.length == 0) return Key::Other;
  switch ([chars characterAtIndex:0]) {
    case 'a': return Key::A;
    case 'c': return Key::C;
    case 'f': return Key::F;
    case 'n': return Key::N;
    case 'o': return Key::O;
    case 'q': return Key::Q;
    case 's': return Key::S;
    case 'v': return Key::V;
    case 'w': return Key::W;
    case 'x': return Key::X;
    case 'z': return Key::Z;
    case ',': return Key::Comma;
    case '=':
    case '+': return Key::Plus;
    case '-': return Key::Minus;
    case '0': return Key::Digit0;
    case '1': return Key::Digit1;
    case '2': return Key::Digit2;
    case '3': return Key::Digit3;
    default: return Key::Other;
  }
}

std::string utf8(NSString* s) { return s ? std::string(s.UTF8String) : std::string(); }
NSString* ns(const std::string& s) {
  NSString* out = [NSString stringWithUTF8String:s.c_str()];
  return out ? out : @"";
}

}  // namespace

class MacPlatform final : public Platform {
 public:
  explicit MacPlatform(const PlatformOptions& o) : options_(o) {}

  bool create_window(const std::string& title, int width, int height) override {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    delegate_ = [[CvdAppDelegate alloc] init];
    delegate_.platform = this;
    [NSApp setDelegate:delegate_];
    build_menu();

    const NSRect frame = NSMakeRect(0, 0, width, height);
    window_ = [[NSWindow alloc] initWithContentRect:frame
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                    NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    window_.title = ns(title);
    window_.titlebarAppearsTransparent = YES;
    window_.minSize = NSMakeSize(520, 460);
    window_.delegate = delegate_;
    window_.releasedWhenClosed = NO;
    window_.tabbingMode = NSWindowTabbingModeDisallowed;
    view_ = [[CvdView alloc] initWithFrame:frame];
    view_.platform = this;
    view_.wantsLayer = YES;
    view_.layer.contentsGravity = kCAGravityTopLeft;
    view_.layer.magnificationFilter = kCAFilterNearest;
    [view_ registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
    window_.contentView = view_;
    [window_ makeFirstResponder:view_];
    [window_ center];
    [window_ setFrameAutosaveName:@"ConvertibleMainWindow"];
    return true;
  }

  int run(AppHandler& handler) override {
    handler_ = &handler;
    for (auto& paths : pending_open_) {
      Event e;
      e.type = EventType::OpenFiles;
      e.paths = paths;
      handler.on_event(e);
    }
    pending_open_.clear();
    update_chrome();
    [window_ makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    schedule(0.0);
    [NSApp run];
    handler_ = nullptr;
    return 0;
  }

  // Called on the main thread.
  void frame() {
    frame_pending_ = false;
    if (!handler_) return;
    if (!handler_->keep_running()) {
      [NSApp terminate:nil];
      return;
    }
    const float sc = scale();
    const NSSize size = view_.bounds.size;
    const int w = std::max(1, static_cast<int>(std::lround(size.width * sc)));
    const int h = std::max(1, static_cast<int>(std::lround(size.height * sc)));
    if (bitmap_.width != w || bitmap_.height != h) bitmap_.resize(w, h);
    handler_->on_frame(bitmap_, sc, now());
    present();
    update_cursor();
    update_chrome();
    NSAccessibilityPostNotification(view_, NSAccessibilityLayoutChangedNotification);
    if (!handler_->keep_running()) {
      [NSApp terminate:nil];
      return;
    }
    if (handler_->wants_frame()) {
      schedule(1.0 / 60.0);
    } else if (const double wake = handler_->wake_time(); wake > 0.0) {
      schedule(std::max(0.0, wake - now()));
    }
  }

  void schedule(double delay) {
    if (frame_pending_ && delay >= pending_delay_) return;
    if (timer_) [timer_ invalidate];
    frame_pending_ = true;
    pending_delay_ = delay;
    timer_ = [NSTimer timerWithTimeInterval:delay
                                    repeats:NO
                                      block:^(NSTimer*) {
                                        timer_ = nil;
                                        frame();
                                      }];
    timer_.tolerance = 0.0;
    // Common modes keep frames coming during live resize and menu tracking.
    [[NSRunLoop currentRunLoop] addTimer:timer_ forMode:NSRunLoopCommonModes];
  }

  void present() {
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CFDataRef data = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(bitmap_.pixels.data()),
                                  static_cast<CFIndex>(bitmap_.pixels.size() * 4));
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CGImageRef image = CGImageCreate(static_cast<size_t>(bitmap_.width), static_cast<size_t>(bitmap_.height), 8, 32,
                                     static_cast<size_t>(bitmap_.width) * 4, cs,
                                     static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedFirst) | static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Little), provider, nullptr,
                                     false, kCGRenderingIntentDefault);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    view_.layer.contentsScale = scale();
    view_.layer.contents = (__bridge id)image;
    [CATransaction commit];
    CGImageRelease(image);
    CGDataProviderRelease(provider);
    CFRelease(data);
    CGColorSpaceRelease(cs);
  }

  void update_cursor() {
    if (!handler_) return;
    const CursorShape c = handler_->cursor();
    if (c == cursor_) return;
    cursor_ = c;
    [view_.window invalidateCursorRectsForView:view_];
    [cursor_ns() set];
  }

  NSCursor* cursor_ns() const {
    switch (cursor_) {
      case CursorShape::Hand: return NSCursor.pointingHandCursor;
      case CursorShape::Text: return NSCursor.IBeamCursor;
      case CursorShape::ResizeH: return NSCursor.resizeLeftRightCursor;
      default: return NSCursor.arrowCursor;
    }
  }

  void update_chrome() {
    if (!handler_ || !window_) return;
    const Color c = handler_->chrome_color();
    window_.backgroundColor = [NSColor colorWithSRGBRed:c.r green:c.g blue:c.b alpha:1.0];
    const bool dark = handler_->chrome_dark();
    if (dark != chrome_dark_ || !chrome_set_) {
      chrome_dark_ = dark;
      chrome_set_ = true;
      window_.appearance = [NSAppearance appearanceNamed:dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
    }
  }

  void dispatch(const Event& e) {
    if (!handler_) {
      if (e.type == EventType::OpenFiles) pending_open_.push_back(e.paths);
      return;
    }
    handler_->on_event(e);
    request_frame_main();
  }

  void request_frame_main() {
    if (!frame_pending_ || pending_delay_ > 0.0) schedule(0.0);
  }

  void request_frame() override {
    dispatch_async(dispatch_get_main_queue(), ^{
      request_frame_main();
    });
  }

  void post(std::function<void()> fn) override {
    auto shared = std::make_shared<std::function<void()>>(std::move(fn));
    dispatch_async(dispatch_get_main_queue(), ^{
      (*shared)();
      request_frame_main();
    });
  }

  void quit() override { [NSApp terminate:nil]; }

  [[nodiscard]] double now() const override { return CACurrentMediaTime(); }
  [[nodiscard]] float scale() const override {
    if (options_.scale > 0.0f) return options_.scale;
    return window_ ? static_cast<float>(window_.backingScaleFactor) : 2.0f;
  }
  [[nodiscard]] Vec2 window_size() const override {
    const NSSize s = view_.bounds.size;
    return {static_cast<float>(s.width), static_cast<float>(s.height)};
  }
  void set_title(const std::string& title) override { window_.title = ns(title); }

  [[nodiscard]] bool system_dark_mode() const override {
    NSAppearanceName name = [NSApp.effectiveAppearance
        bestMatchFromAppearancesWithNames:@[ NSAppearanceNameAqua, NSAppearanceNameDarkAqua ]];
    return [name isEqualToString:NSAppearanceNameDarkAqua];
  }
  [[nodiscard]] bool system_reduce_motion() const override {
    return NSWorkspace.sharedWorkspace.accessibilityDisplayShouldReduceMotion;
  }

  [[nodiscard]] std::string clipboard_text() const override {
    return utf8([NSPasteboard.generalPasteboard stringForType:NSPasteboardTypeString]);
  }
  void set_clipboard_text(const std::string& text) override {
    [NSPasteboard.generalPasteboard clearContents];
    [NSPasteboard.generalPasteboard setString:ns(text) forType:NSPasteboardTypeString];
  }

  [[nodiscard]] bool has_native_dialogs() const override { return true; }

  void open_files_dialog(const std::string& title, PathsCallback done) override {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.message = ns(title);
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = YES;
    panel.prompt = @"Add";
    auto cb = std::make_shared<PathsCallback>(std::move(done));
    [panel beginSheetModalForWindow:window_
                  completionHandler:^(NSModalResponse r) {
                    std::vector<std::string> out;
                    if (r == NSModalResponseOK) {
                      for (NSURL* url in panel.URLs) out.push_back(utf8(url.path));
                    }
                    (*cb)(out);
                    request_frame_main();
                  }];
  }

  void save_file_dialog(const std::string& title, const std::string& directory, const std::string& suggested,
                        PathCallback done) override {
    NSSavePanel* panel = [NSSavePanel savePanel];
    panel.message = ns(title);
    panel.nameFieldStringValue = ns(suggested);
    panel.canCreateDirectories = YES;
    panel.extensionHidden = NO;
    if (!directory.empty()) panel.directoryURL = [NSURL fileURLWithPath:ns(directory) isDirectory:YES];
    auto cb = std::make_shared<PathCallback>(std::move(done));
    [panel beginSheetModalForWindow:window_
                  completionHandler:^(NSModalResponse r) {
                    if (r == NSModalResponseOK && panel.URL) (*cb)(utf8(panel.URL.path));
                    else (*cb)(std::nullopt);
                    request_frame_main();
                  }];
  }

  void choose_folder_dialog(const std::string& title, const std::string& directory, PathCallback done) override {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.message = ns(title);
    panel.canChooseFiles = NO;
    panel.canChooseDirectories = YES;
    panel.canCreateDirectories = YES;
    panel.allowsMultipleSelection = NO;
    panel.prompt = @"Save Here";
    if (!directory.empty()) panel.directoryURL = [NSURL fileURLWithPath:ns(directory) isDirectory:YES];
    auto cb = std::make_shared<PathCallback>(std::move(done));
    [panel beginSheetModalForWindow:window_
                  completionHandler:^(NSModalResponse r) {
                    if (r == NSModalResponseOK && panel.URL) (*cb)(utf8(panel.URL.path));
                    else (*cb)(std::nullopt);
                    request_frame_main();
                  }];
  }

  bool reveal_in_file_manager(const std::string& path) override {
    NSURL* url = [NSURL fileURLWithPath:ns(path)];
    if (!url) return false;
    [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[ url ]];
    return true;
  }
  bool open_with_default_app(const std::string& path) override {
    NSURL* url = [NSURL fileURLWithPath:ns(path)];
    return url && [NSWorkspace.sharedWorkspace openURL:url];
  }
  [[nodiscard]] std::string file_manager_name() const override { return "Finder"; }

  [[nodiscard]] std::string config_directory() const override {
    NSArray<NSURL*>* urls = [NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory
                                                                 inDomains:NSUserDomainMask];
    const std::string base = urls.count ? utf8(urls.firstObject.path) : fileio::home_directory();
    return fileio::join(base, "Convertible");
  }
  [[nodiscard]] std::string documents_directory() const override {
    NSArray<NSURL*>* urls = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory
                                                                 inDomains:NSUserDomainMask];
    return urls.count ? utf8(urls.firstObject.path) : fileio::home_directory();
  }

  bool set_automation_size(int width, int height, float sc) override {
    if (sc > 0.0f) options_.scale = sc;
    [window_ setContentSize:NSMakeSize(width, height)];
    return true;
  }

  // Accessibility elements, rebuilt lazily from the handler's tree.
  NSArray* accessibility_children() {
    if (!handler_) return @[];
    NSMutableArray* out = [NSMutableArray array];
    const NSRect bounds = view_.bounds;
    for (const AccessibleItem& item : handler_->accessibility()) {
      NSAccessibilityElement* el = [[NSAccessibilityElement alloc] init];
      NSAccessibilityRole role = NSAccessibilityStaticTextRole;
      switch (item.role) {
        case AccessibleItem::Role::Button:
        case AccessibleItem::Role::Tab:
        case AccessibleItem::Role::MenuItem:
        case AccessibleItem::Role::ListItem: role = NSAccessibilityButtonRole; break;
        case AccessibleItem::Role::Toggle: role = NSAccessibilityCheckBoxRole; break;
        case AccessibleItem::Role::Slider: role = NSAccessibilitySliderRole; break;
        case AccessibleItem::Role::TextField: role = NSAccessibilityTextFieldRole; break;
        case AccessibleItem::Role::Group: role = NSAccessibilityGroupRole; break;
        case AccessibleItem::Role::Image: role = NSAccessibilityImageRole; break;
        case AccessibleItem::Role::Progress: role = NSAccessibilityProgressIndicatorRole; break;
        case AccessibleItem::Role::Link: role = NSAccessibilityLinkRole; break;
        default: break;
      }
      el.accessibilityRole = role;
      el.accessibilityLabel = ns(item.label);
      if (!item.value.empty()) el.accessibilityValue = ns(item.value);
      el.accessibilityEnabled = item.enabled;
      el.accessibilityParent = view_;
      // View coordinates are flipped (top-left origin).
      const NSRect local = NSMakeRect(item.rect.x, bounds.size.height - item.rect.y - item.rect.h, item.rect.w,
                                      item.rect.h);
      el.accessibilityFrame = [window_ convertRectToScreen:[view_ convertRect:local toView:nil]];
      [out addObject:el];
    }
    return out;
  }

  void build_menu() {
    NSMenu* bar = [[NSMenu alloc] init];
    NSMenuItem* app_item = [[NSMenuItem alloc] init];
    [bar addItem:app_item];
    NSMenu* app_menu = [[NSMenu alloc] initWithTitle:@"Convertible"];
    [app_menu addItemWithTitle:@"About Convertible" action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
    [app_menu addItem:[NSMenuItem separatorItem]];
    [app_menu addItemWithTitle:@"Hide Convertible" action:@selector(hide:) keyEquivalent:@"h"];
    NSMenuItem* others = [app_menu addItemWithTitle:@"Hide Others" action:@selector(hideOtherApplications:)
                                      keyEquivalent:@"h"];
    others.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [app_menu addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
    [app_menu addItem:[NSMenuItem separatorItem]];
    [app_menu addItemWithTitle:@"Quit Convertible" action:@selector(terminate:) keyEquivalent:@"q"];
    app_item.submenu = app_menu;
    NSMenuItem* window_item = [[NSMenuItem alloc] init];
    [bar addItem:window_item];
    NSMenu* window_menu = [[NSMenu alloc] initWithTitle:@"Window"];
    [window_menu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [window_menu addItemWithTitle:@"Close" action:@selector(performClose:) keyEquivalent:@"w"];
    window_item.submenu = window_menu;
    NSApp.mainMenu = bar;
    NSApp.windowsMenu = window_menu;
  }

  AppHandler* handler_ = nullptr;
  PlatformOptions options_;
  NSWindow* window_ = nil;
  CvdView* view_ = nil;
  CvdAppDelegate* delegate_ = nil;
  NSTimer* timer_ = nil;
  bool frame_pending_ = false;
  double pending_delay_ = 0.0;
  gfx::Bitmap bitmap_;
  CursorShape cursor_ = CursorShape::Arrow;
  bool chrome_dark_ = true;
  bool chrome_set_ = false;
  std::vector<std::vector<std::string>> pending_open_;
  NSString* marked_ = nil;
};

std::unique_ptr<Platform> create_platform(const PlatformOptions& options) {
  if (options.headless) return create_headless_platform(options);
  return std::make_unique<MacPlatform>(options);
}

}  // namespace cvd

// ---------------------------------------------------------------------------
// Objective-C classes
// ---------------------------------------------------------------------------

@implementation CvdAppDelegate

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
  return YES;
}

- (void)application:(NSApplication*)application openURLs:(NSArray<NSURL*>*)urls {
  cvd::Event e;
  e.type = cvd::EventType::OpenFiles;
  for (NSURL* url in urls) {
    if (url.isFileURL) e.paths.push_back(url.path.UTF8String);
  }
  if (self.platform && !e.paths.empty()) self.platform->dispatch(e);
}

- (BOOL)windowShouldClose:(NSWindow*)sender {
  return YES;
}

- (void)windowWillClose:(NSNotification*)notification {
  cvd::Event e;
  e.type = cvd::EventType::CloseRequested;
  if (self.platform) self.platform->dispatch(e);
  [NSApp terminate:nil];
}

- (void)windowDidChangeBackingProperties:(NSNotification*)notification {
  if (self.platform) self.platform->request_frame_main();
}

- (void)windowDidResignKey:(NSNotification*)notification {
  cvd::Event e;
  e.type = cvd::EventType::FocusLost;
  if (self.platform) self.platform->dispatch(e);
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
  [NSWorkspace.sharedWorkspace.notificationCenter
      addObserverForName:NSWorkspaceAccessibilityDisplayOptionsDidChangeNotification
                  object:nil
                   queue:nil
              usingBlock:^(NSNotification*) {
                cvd::Event e;
                e.type = cvd::EventType::AppearanceChanged;
                if (self.platform) self.platform->dispatch(e);
              }];
}

@end

@implementation CvdView

- (BOOL)isFlipped {
  return YES;
}
- (BOOL)acceptsFirstResponder {
  return YES;
}
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
  return YES;
}
- (BOOL)wantsUpdateLayer {
  return YES;
}

- (void)updateTrackingAreas {
  [super updateTrackingAreas];
  for (NSTrackingArea* area in self.trackingAreas) [self removeTrackingArea:area];
  NSTrackingArea* area = [[NSTrackingArea alloc]
      initWithRect:self.bounds
           options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow |
                   NSTrackingInVisibleRect | NSTrackingCursorUpdate
             owner:self
          userInfo:nil];
  [self addTrackingArea:area];
}

- (void)setFrameSize:(NSSize)size {
  [super setFrameSize:size];
  if (self.platform && self.platform->handler_) self.platform->frame();
}

- (void)viewDidChangeEffectiveAppearance {
  cvd::Event e;
  e.type = cvd::EventType::AppearanceChanged;
  if (self.platform) self.platform->dispatch(e);
}

- (void)cursorUpdate:(NSEvent*)event {
  if (self.platform) [self.platform->cursor_ns() set];
}

- (cvd::Vec2)pos:(NSEvent*)event {
  const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
  return {static_cast<float>(p.x), static_cast<float>(p.y)};
}

- (void)sendMouse:(NSEvent*)event type:(cvd::EventType)type button:(cvd::MouseButton)button {
  cvd::Event e;
  e.type = type;
  e.pos = [self pos:event];
  e.button = button;
  e.clicks = static_cast<int>(event.clickCount);
  e.mods = cvd::mods_from(event.modifierFlags);
  if (self.platform) self.platform->dispatch(e);
}

- (void)mouseMoved:(NSEvent*)event {
  [self sendMouse:event type:cvd::EventType::MouseMove button:cvd::MouseButton::Left];
}
- (void)mouseDragged:(NSEvent*)event {
  [self sendMouse:event type:cvd::EventType::MouseMove button:cvd::MouseButton::Left];
}
- (void)mouseDown:(NSEvent*)event {
  [self sendMouse:event type:cvd::EventType::MouseDown button:cvd::MouseButton::Left];
}
- (void)mouseUp:(NSEvent*)event {
  [self sendMouse:event type:cvd::EventType::MouseUp button:cvd::MouseButton::Left];
}
- (void)rightMouseDown:(NSEvent*)event {
  [self sendMouse:event type:cvd::EventType::MouseDown button:cvd::MouseButton::Right];
}
- (void)rightMouseUp:(NSEvent*)event {
  [self sendMouse:event type:cvd::EventType::MouseUp button:cvd::MouseButton::Right];
}
- (void)mouseExited:(NSEvent*)event {
  cvd::Event e;
  e.type = cvd::EventType::MouseLeave;
  if (self.platform) self.platform->dispatch(e);
}

- (void)scrollWheel:(NSEvent*)event {
  cvd::Event e;
  e.type = cvd::EventType::Scroll;
  e.pos = [self pos:event];
  e.precise = event.hasPreciseScrollingDeltas;
  const CGFloat k = event.hasPreciseScrollingDeltas ? 1.0 : 32.0;
  e.scroll = {static_cast<float>(-event.scrollingDeltaX * k), static_cast<float>(-event.scrollingDeltaY * k)};
  e.mods = cvd::mods_from(event.modifierFlags);
  if (self.platform) self.platform->dispatch(e);
}

- (BOOL)performKeyEquivalent:(NSEvent*)event {
  // Command shortcuts the app handles itself (the menu keeps Quit, Hide, Minimize, Close).
  const NSEventModifierFlags f = event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
  if (!(f & NSEventModifierFlagCommand) || event.type != NSEventTypeKeyDown) return [super performKeyEquivalent:event];
  const cvd::Key k = cvd::key_from(event);
  switch (k) {
    case cvd::Key::Q:
    case cvd::Key::W: return [super performKeyEquivalent:event];
    case cvd::Key::Other: return [super performKeyEquivalent:event];
    default: break;
  }
  if (k == cvd::Key::Other) return NO;
  [self keyDown:event];
  return YES;
}

- (void)keyDown:(NSEvent*)event {
  cvd::Event e;
  e.type = cvd::EventType::KeyDown;
  e.key = cvd::key_from(event);
  e.mods = cvd::mods_from(event.modifierFlags);
  e.repeat = event.isARepeat;
  if (self.platform) self.platform->dispatch(e);
  if (!e.mods.super && !e.mods.ctrl) [self interpretKeyEvents:@[ event ]];
}

- (void)keyUp:(NSEvent*)event {
  cvd::Event e;
  e.type = cvd::EventType::KeyUp;
  e.key = cvd::key_from(event);
  e.mods = cvd::mods_from(event.modifierFlags);
  if (self.platform) self.platform->dispatch(e);
}

// NSTextInputClient (text arrives here; editing commands are handled from keyDown).
- (void)insertText:(id)string replacementRange:(NSRange)replacementRange {
  NSString* s = [string isKindOfClass:[NSAttributedString class]] ? [string string] : string;
  cvd::Event e;
  e.type = cvd::EventType::Text;
  e.text = s.UTF8String ? s.UTF8String : "";
  if (self.platform && !e.text.empty()) self.platform->dispatch(e);
}
- (void)doCommandBySelector:(SEL)selector {
}
- (void)setMarkedText:(id)string selectedRange:(NSRange)selectedRange replacementRange:(NSRange)replacementRange {
}
- (void)unmarkText {
}
- (NSRange)selectedRange {
  return NSMakeRange(NSNotFound, 0);
}
- (NSRange)markedRange {
  return NSMakeRange(NSNotFound, 0);
}
- (BOOL)hasMarkedText {
  return NO;
}
- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
  return nil;
}
- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText {
  return @[];
}
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
  return NSMakeRect(0, 0, 0, 0);
}
- (NSUInteger)characterIndexForPoint:(NSPoint)point {
  return NSNotFound;
}

// Drag and drop.
- (std::vector<std::string>)pathsFrom:(id<NSDraggingInfo>)info {
  std::vector<std::string> out;
  NSArray<NSURL*>* urls = [info.draggingPasteboard readObjectsForClasses:@[ [NSURL class] ]
                                                                 options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
  for (NSURL* url in urls) out.push_back(url.path.UTF8String);
  return out;
}

- (void)sendDrag:(id<NSDraggingInfo>)info type:(cvd::EventType)type {
  cvd::Event e;
  e.type = type;
  const NSPoint p = [self convertPoint:info.draggingLocation fromView:nil];
  e.pos = {static_cast<float>(p.x), static_cast<float>(p.y)};
  if (self.platform) self.platform->dispatch(e);
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info {
  [self sendDrag:info type:cvd::EventType::DragEnter];
  return NSDragOperationCopy;
}
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)info {
  [self sendDrag:info type:cvd::EventType::DragOver];
  return NSDragOperationCopy;
}
- (void)draggingExited:(id<NSDraggingInfo>)info {
  [self sendDrag:info type:cvd::EventType::DragLeave];
}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)info {
  cvd::Event e;
  e.type = cvd::EventType::Drop;
  const NSPoint p = [self convertPoint:info.draggingLocation fromView:nil];
  e.pos = {static_cast<float>(p.x), static_cast<float>(p.y)};
  e.paths = [self pathsFrom:info];
  if (self.platform) self.platform->dispatch(e);
  return !e.paths.empty();
}

// Accessibility: the view is a container of elements mirrored from the UI tree.
- (BOOL)isAccessibilityElement {
  return NO;
}
- (NSArray*)accessibilityChildren {
  return self.platform ? self.platform->accessibility_children() : @[];
}
- (NSAccessibilityRole)accessibilityRole {
  return NSAccessibilityGroupRole;
}
- (NSString*)accessibilityLabel {
  return @"Convertible";
}

@end
