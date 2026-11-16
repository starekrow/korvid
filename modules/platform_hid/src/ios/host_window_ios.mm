// The host window on iOS, native UIKit. The OS owns the main thread's loop: platform_app_main
// starts the application (UIApplicationMain), and once it has launched the delegate runs the
// engine's own frame loop from a run-loop timer, from inside UIKit's loop. Each pump then
// drains that loop (CFRunLoopRunInMode until nothing is handled), so UIKit's events, the
// scene's lifecycle included, are delivered between frames, and the frame loop is the same
// as on every other platform. An app off screen (backgrounded) does nothing until it is back:
// pump waits in the run loop, no frame runs, no GPU work is submitted (iOS refuses it there).
//
// The window is the scene's: one UIWindowScene, one UIWindow, its root view controller's
// view a UIView whose layer is a CAMetalLayer (native_handle() is that view). Focus is the
// scene's activation: active is focused, and on iOS an unfocused window holds the sim
// (hid_backend_holds_unfocused). A hardware keyboard's keys arrive as presses with their HID
// usages (input_ios.cc). Touches go to Input's touch list, with the safe area's insets on the
// window (the touchpad module turns them into a pad). Gamepads and text input are slice 8's.
//
// A software window (the CPU renderer's, where Metal cannot draw: the Simulator) draws into
// one of three buffers of ours; each present wraps it in a CGImage and makes it the contents
// of a plain layer over the Metal one. Core Animation holds the image until it has shown it,
// and the image's release hands the buffer back, so a frame never writes pixels on screen.
// A frame may be smaller than the window (software_size): the layer scales it up on the GPU,
// nearest-neighbour. The presents do not wait for the display: the frame loop paces them.
//
// Objective-C without ARC, as renderer-metal: the few objects here live as long as the
// window, made and released explicitly. UIKit calls arrive on the main thread, which is the
// engine's, so the message ring needs no lock.
#include "korvid/platform_hid/host_window.h"
#include "korvid/platform_hid/events.h"
#include "korvid/core/atomic.h"
#include "korvid/core/log.h"
#include "korvid/core/time.h"
#include "../hid_backend.h"
#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#include <stdlib.h>
#include <unistd.h>

namespace kv {

namespace {

enum class IosMsgType : u8 { Size, Focus, Key, Close, TouchBegan, TouchMoved, TouchEnded };

struct IosMsg {
    IosMsgType type;
    bool       on;   // Focus: focused; Key: down
    u32        a, b; // Size: width, height in pixels; Key: the HID usage; Touch*: the touch's id in a
    f32        f[4]; // Size: the safe area's insets, pixels (left, top, right, bottom); Touch*: x, y in pixels
};

constexpr u32 kRing = 512; // a frame's worth of UIKit's events, many times over

struct IosApp {
    int    argc = 0;
    char** argv = nullptr;
    int  (*run)(int, char**, void*) = nullptr;
    void*  ctx = nullptr;
    bool   launched   = false; // UIKit has finished launching; run is running
    bool   background = false; // the scene is off screen
    bool   active     = false; // the scene is the foreground's active one
    UIWindowScene* scene = nil;
    UIWindow*      window = nil;
    UIViewController* controller = nil;
    UIInterfaceOrientationMask orientations = UIInterfaceOrientationMaskAll; // what the window asked for
    IosMsg ring[kRing];
    u32    head = 0, tail = 0; // UIKit pushes at tail, pump takes from head
    u32    dropped = 0;
};

IosApp g_app; // one application per process

// A software window's pixels: B, G, R, X in memory, rows `pitch` bytes apart. `held` while a
// frame draws into it or Core Animation shows it (the image's release clears it, on any
// thread).
struct SoftBuffer {
    u8* pixels = nullptr;
    u32 width = 0, height = 0, pitch = 0;
    u32 held = 0;
};

struct SoftSurface {
    SoftBuffer buf[3];
    SoftBuffer* drawing = nullptr; // software_frame's, until present_software
    CALayer* layer = nil;          // over the view's Metal layer, made by the first present
    CGColorSpaceRef space = nullptr;
    bool told = false;             // the "every buffer is held" warning, once
};

SoftSurface g_soft;

void soft_release(void* info, const void*, size_t) { atomic_store(&((SoftBuffer*)info)->held, 0u, MO_RELEASE); }

void post(const IosMsg& m) {
    if (g_app.tail - g_app.head >= kRing) { ++g_app.dropped; return; }
    g_app.ring[g_app.tail++ % kRing] = m;
}

void post_size(UIView* v) {
    CGFloat s = v.contentScaleFactor;
    IosMsg m{};
    m.type = IosMsgType::Size;
    m.a = (u32)(v.bounds.size.width * s + 0.5);
    m.b = (u32)(v.bounds.size.height * s + 0.5);
    UIEdgeInsets in = v.safeAreaInsets;
    m.f[0] = (f32)(in.left * s); m.f[1] = (f32)(in.top * s); m.f[2] = (f32)(in.right * s); m.f[3] = (f32)(in.bottom * s);
    post(m);
}

// A UITouch is the same object for the touch's whole life: its address, folded, is the id
// (never 0).
u32 touch_id(UITouch* t) {
    uintptr_t p = (uintptr_t)t;
    return (u32)((p >> 4) ^ (p >> 36)) | 0x80000000u;
}

void post_touches(UIView* v, NSSet<UITouch*>* touches, IosMsgType type) {
    CGFloat s = v.contentScaleFactor;
    for (UITouch* t in touches) {
        CGPoint p = [t locationInView:v];
        IosMsg m{};
        m.type = type;
        m.a = touch_id(t);
        m.f[0] = (f32)(p.x * s);
        m.f[1] = (f32)(p.y * s);
        post(m);
    }
}

void post_focus(bool on) {
    if (g_app.active == on) return;
    g_app.active = on;
    IosMsg m{};
    m.type = IosMsgType::Focus;
    m.on = on;
    post(m);
}

} // namespace
} // namespace kv

// The view: its layer is the Metal layer the renderer draws into, at the screen's scale.
@interface KVMetalView : UIView
@end

@implementation KVMetalView
+ (Class)layerClass { return [CAMetalLayer class]; }
- (void)layoutSubviews {
    [super layoutSubviews];
    self.layer.contentsScale = self.contentScaleFactor;
    kv::post_size(self);
}
- (void)safeAreaInsetsDidChange {
    [super safeAreaInsetsDidChange];
    kv::post_size(self);
}
- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event     { (void)event; kv::post_touches(self, touches, kv::IosMsgType::TouchBegan); }
- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event     { (void)event; kv::post_touches(self, touches, kv::IosMsgType::TouchMoved); }
- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event     { (void)event; kv::post_touches(self, touches, kv::IosMsgType::TouchEnded); }
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event { (void)event; kv::post_touches(self, touches, kv::IosMsgType::TouchEnded); }
@end

// The view's controller: full screen, no status bar, and the first responder, so a hardware
// keyboard's presses come here.
@interface KVViewController : UIViewController
@end

@implementation KVViewController
- (void)loadView {
    UIScreen* screen = kv::g_app.scene.screen; // the controller is made once the scene has connected
    KVMetalView* v = [[KVMetalView alloc] initWithFrame:screen.bounds];
    v.contentScaleFactor = screen.nativeScale;
    v.multipleTouchEnabled = YES;
    self.view = v;
    [v release];
}
- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (BOOL)canBecomeFirstResponder { return YES; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return kv::g_app.orientations; }
- (void)kvPresses:(NSSet<UIPress*>*)presses down:(BOOL)down {
    for (UIPress* p in presses) {
        if (!p.key) continue;
        kv::IosMsg m{};
        m.type = kv::IosMsgType::Key;
        m.on = down;
        m.a = (kv::u32)p.key.keyCode;
        kv::post(m);
    }
}
- (void)pressesBegan:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event { (void)event; [self kvPresses:presses down:YES]; }
- (void)pressesEnded:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event { (void)event; [self kvPresses:presses down:NO]; }
- (void)pressesCancelled:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event { (void)event; [self kvPresses:presses down:NO]; }
@end

// The scene's lifecycle: its activation is the window's focus, its place on screen whether
// the engine runs at all.
@interface KVSceneDelegate : UIResponder <UIWindowSceneDelegate>
@end

@implementation KVSceneDelegate
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session options:(UISceneConnectionOptions*)options {
    (void)session; (void)options;
    if ([scene isKindOfClass:[UIWindowScene class]] && !kv::g_app.scene) kv::g_app.scene = (UIWindowScene*)[scene retain];
}
- (void)sceneDidDisconnect:(UIScene*)scene {
    if (scene != kv::g_app.scene) return;
    kv::IosMsg m{};
    m.type = kv::IosMsgType::Close;
    kv::post(m);
}
- (void)sceneDidBecomeActive:(UIScene*)scene     { (void)scene; kv::post_focus(true); }
- (void)sceneWillResignActive:(UIScene*)scene    { (void)scene; kv::post_focus(false); }
- (void)sceneDidEnterBackground:(UIScene*)scene  { (void)scene; kv::g_app.background = true; }
- (void)sceneWillEnterForeground:(UIScene*)scene { (void)scene; kv::g_app.background = false; }
@end

// The application: one scene, ours; the engine's run once launching is done.
@interface KVAppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation KVAppDelegate
- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)options {
    (void)application; (void)options;
    // After this returns: a timer, so UIKit's loop is running (and the main queue free) when
    // the engine starts pumping it.
    [self performSelector:@selector(kvRun) withObject:nil afterDelay:0.0];
    return YES;
}
- (UISceneConfiguration*)application:(UIApplication*)application
    configurationForConnectingSceneSession:(UISceneSession*)session options:(UISceneConnectionOptions*)options {
    (void)application; (void)options;
    UISceneConfiguration* c = [UISceneConfiguration configurationWithName:@"korvid" sessionRole:session.role];
    c.delegateClass = [KVSceneDelegate class];
    return c;
}
- (void)kvRun {
    kv::g_app.launched = true;
    int rc = kv::g_app.run(kv::g_app.argc, kv::g_app.argv, kv::g_app.ctx);
    exit(rc); // an iOS app does not return from its loop; the run's end is the process's
}
@end

namespace kv {

int platform_app_main(int argc, char** argv, int (*run)(int argc, char** argv, void* ctx), void* ctx) {
    g_app.argc = argc;
    g_app.argv = argv;
    g_app.run  = run;
    g_app.ctx  = ctx;
    // An app starts in /, which it cannot write: a relative path the engine writes
    // (--screenshot, --profile) lands in the app's own tmp/ instead, where devicectl can
    // copy it from (--domain-type appDataContainer, tmp/<file>).
    if (const char* tmp = getenv("TMPDIR")) chdir(tmp);
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([KVAppDelegate class]));
    }
}

bool hid_backend_startup() { return true; }
void hid_backend_shutdown() {}
// A window needs the application: in a test run by simctl spawn there is none.
bool hid_video_up() { return g_app.launched; }
void hid_video_down() {}
bool hid_backend_holds_unfocused() { return true; }

bool HostWindow::open(const WindowDesc& desc) {
    KV_ASSERT(!native_);
    if (!hid_video_up()) { KV_ERROR("window", "no application (the process did not start through platform_app_main)"); return false; }
    // The scene connects through UIKit's loop, soon after launch; wait for it a while.
    f64 start = time_now();
    while (!g_app.scene && time_now() - start < 5.0) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
    if (!g_app.scene) { KV_ERROR("window", "no window scene connected"); return false; }
    // The screen is the window; what the size asked for says is its shape: wider than tall
    // is landscape (either way up), else portrait.
    bool landscape = desc.width > desc.height;
    g_app.orientations = landscape ? UIInterfaceOrientationMaskLandscape : UIInterfaceOrientationMaskPortrait;
    @autoreleasepool {
        // One window for the process: made once, hidden by close and shown again by open
        // (tearing a window down and making another in the same moment unbalances UIKit's
        // appearance transitions).
        if (!g_app.window) {
            g_app.window = [[UIWindow alloc] initWithWindowScene:g_app.scene];
            g_app.controller = [[KVViewController alloc] init];
            g_app.window.rootViewController = g_app.controller;
        }
        g_app.window.hidden = NO;
        [g_app.window makeKeyAndVisible];
        [g_app.controller becomeFirstResponder];
        [g_app.controller setNeedsUpdateOfSupportedInterfaceOrientations];
        UIWindowSceneGeometryPreferencesIOS* geo = [[UIWindowSceneGeometryPreferencesIOS alloc] initWithInterfaceOrientations:g_app.orientations];
        [g_app.scene requestGeometryUpdateWithPreferences:geo errorHandler:nil];
        [geo release];
        [g_app.controller.view layoutIfNeeded];
    }
    // The turn, when there is one, takes a moment: wait for the view to have its shape, so
    // the first frame is drawn at the size it will keep.
    UIView* v = g_app.controller.view;
    start = time_now();
    while ((v.bounds.size.width > v.bounds.size.height) != landscape && time_now() - start < 2.0)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);
    CGFloat s = v.contentScaleFactor;
    native_  = v;
    surface_ = desc.surface;
    vsync_   = desc.vsync;
    width_   = (u32)(v.bounds.size.width * s + 0.5);
    height_  = (u32)(v.bounds.size.height * s + 0.5);
    UIEdgeInsets in = v.safeAreaInsets;
    insets_[0] = (f32)(in.left * s); insets_[1] = (f32)(in.top * s); insets_[2] = (f32)(in.right * s); insets_[3] = (f32)(in.bottom * s);
    g_app.active = g_app.scene.activationState == UISceneActivationStateForegroundActive;
    focused_ = g_app.active;
    g_app.head = g_app.tail; // what came before the window is in its fields
    KV_INFO("window", "opened '%s' %ux%u px, %s (the screen at %.0fx; asked for %ux%u, which iOS takes as the shape)", desc.title,
            width_, height_, landscape ? "landscape" : "portrait", (f64)s, desc.width, desc.height);
    return true;
}

void HostWindow::close() {
    if (!native_) return;
    g_app.window.hidden = YES; // kept for the next open: see there
    native_ = nullptr;
    width_ = height_ = 0;
    focused_ = false;
    KV_INFO("window", "closed");
}

void HostWindow::present_blank() {} // UIKit shows the view's background; nothing to attach
bool HostWindow::software_frame(SoftwareFrame* out) {
    if (!native_ || surface_ != WindowSurface::Software || !width_ || !height_) return false;
    u32 w = width_, h = height_;
    if (software_want_[0] && software_want_[1] && software_want_[0] <= w && software_want_[1] <= h) {
        w = software_want_[0];
        h = software_want_[1];
    }
    SoftBuffer* b = nullptr;
    for (SoftBuffer& c : g_soft.buf)
        if (!atomic_load(&c.held, MO_ACQUIRE)) { b = &c; break; }
    if (!b) { // the display has all three: this frame is not drawn
        if (!g_soft.told) KV_WARN("window", "every software buffer is still on screen: a frame skipped");
        g_soft.told = true;
        return false;
    }
    if (b->width != w || b->height != h) {
        free(b->pixels);
        b->pitch  = (w * 4 + 63) & ~63u; // rows on 64 bytes, as Core Animation likes them
        b->pixels = (u8*)malloc((size_t)b->pitch * h);
        if (!b->pixels) { b->width = b->height = 0; return false; }
        b->width  = w;
        b->height = h;
    }
    b->held = 1;
    g_soft.drawing = b;
    software_ = b;
    *out = SoftwareFrame{b->pixels, b->width, b->height, b->pitch, false, true};
    return true;
}

void HostWindow::present_software() {
    SoftBuffer* b = g_soft.drawing;
    if (!b) return;
    g_soft.drawing = nullptr;
    software_ = nullptr;
    UIView* v = (UIView*)native_;
    if (!v) { atomic_store(&b->held, 0u, MO_RELEASE); return; }
    if (!g_soft.space) g_soft.space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGDataProviderRef data = CGDataProviderCreateWithData(b, b->pixels, (size_t)b->pitch * b->height, soft_release);
    CGImageRef image = CGImageCreate(b->width, b->height, 8, 32, b->pitch, g_soft.space,
                                     kCGBitmapByteOrder32Little | (CGBitmapInfo)kCGImageAlphaNoneSkipFirst, data, nullptr, false,
                                     kCGRenderingIntentDefault);
    CGDataProviderRelease(data); // the image holds it; its last release frees the buffer
    if (!image) { atomic_store(&b->held, 0u, MO_RELEASE); return; }
    [CATransaction begin];
    [CATransaction setDisableActions:YES]; // no fade between frames
    if (!g_soft.layer) {
        g_soft.layer = [[CALayer alloc] init];
        g_soft.layer.opaque = YES;
        g_soft.layer.contentsGravity = kCAGravityResize;
        g_soft.layer.magnificationFilter = kCAFilterNearest; // a smaller frame's pixels stay square
        [v.layer addSublayer:g_soft.layer];
    }
    g_soft.layer.frame = v.layer.bounds;
    g_soft.layer.contents = (id)image;
    [CATransaction commit];
    CGImageRelease(image);
}

bool HostWindow::software_vsync(bool) { return false; } // the frame loop paces: see the top
void HostWindow::text_input(bool) {} // slice 8: the on-screen keyboard

// Not yet on this host (Krome slice 3): the input method's area is not told, the clipboard is
// the platform module's own (inside this process only), the cursor keeps its shape.
void HostWindow::text_area(rect2, f32) {}
bool HostWindow::clipboard_get(Array<char>&) { return false; }
bool HostWindow::clipboard_set(const char*) { return false; }
void HostWindow::cursor(CursorShape) {}
bool HostWindow::capture_mouse(bool) { return false; }

bool HostWindow::touch_screen() const { return true; }

f32 HostWindow::pixel_scale() const {
    return native_ ? (f32)((UIView*)native_).contentScaleFactor : 1.0f;
}

void HostWindow::pump(EventBus& bus, Input& input) {
    input.raw.touches_settle(); // the touches that ended in the last pump are gone
    @autoreleasepool {
        while (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true) == kCFRunLoopRunHandledSource) {}
        // Off screen: nothing may be drawn and nothing is seen; wait in UIKit's loop until
        // the scene is back (or gone).
        if (g_app.background) {
            KV_INFO("window", "in the background: waiting");
            while (g_app.background && g_app.scene.activationState != UISceneActivationStateUnattached)
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, true);
            KV_INFO("window", "back in the foreground");
        }
    }
    drain(bus, input);
}

void HostWindow::drain(EventBus& bus, Input& input) {
    if (g_app.dropped) { KV_WARN("window", "%u UIKit events dropped: the ring was full", g_app.dropped); g_app.dropped = 0; }
    while (g_app.head != g_app.tail) {
        IosMsg m = g_app.ring[g_app.head++ % kRing];
        switch (m.type) {
        case IosMsgType::Size:
            for (u32 i = 0; i < 4; ++i) insets_[i] = m.f[i];
            if (m.a != width_ || m.b != height_) {
                width_ = m.a;
                height_ = m.b;
                bus.emit(WindowResized{m.a, m.b});
            }
            break;
        case IosMsgType::Focus:
            if (focused_ == m.on) break;
            focused_ = m.on;
            if (!m.on) input.raw.clear(); // keys released while unfocused never report an up edge
            bus.emit(FocusChanged{m.on});
            break;
        case IosMsgType::Key: {
            Key k = key_from_scancode(m.a);
            if (k == Key::Backspace && m.on) ++input.raw.backspaces;
            if (k == Key::Unknown) break;
            input.raw.set(k, m.on);
            if (m.on) bus.emit(KeyDown{k, 0, false});
            else      bus.emit(KeyUp{k, 0});
            break;
        }
        case IosMsgType::Close:
            bus.emit(CloseRequested{0});
            break;
        case IosMsgType::TouchBegan:
            input.raw.touch_begin(m.a, m.f[0], m.f[1]); // all ten taken: this one is not seen
            break;
        case IosMsgType::TouchMoved:
            if (TouchPoint* t = input.raw.touch(m.a)) { t->x = m.f[0]; t->y = m.f[1]; }
            break;
        case IosMsgType::TouchEnded:
            if (TouchPoint* t = input.raw.touch(m.a)) { t->x = m.f[0]; t->y = m.f[1]; t->ended = true; }
            break;
        }
    }
}

void HostWindow::on_modal(void (*fn)(void* ctx), void* ctx) {
    modal_fn_  = fn; // never called: UIKit runs no modal loop that holds the engine's thread
    modal_ctx_ = ctx;
}

} // namespace kv
