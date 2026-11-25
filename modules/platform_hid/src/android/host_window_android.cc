// The host window on Android, native: a NativeActivity and nothing of Java's. The OS loads
// the game as a shared library into a process it made and calls ANativeActivity_onCreate on
// its main thread, which must return: so the game's own main() runs on a thread made there,
// and reaches platform_app_main as on every platform (a plain call here). The activity's
// callbacks arrive on the OS's thread and post to a ring; pump, on the game's thread, drains
// it and the input queue through an ALooper, so `frameloop` is the loop it is everywhere.
//
// What needs the two threads to agree is handed over, not posted: the window and the input
// queue. The OS's thread sets the pending one, wakes the game's thread and waits until it
// has taken it (or let the old one go), because when the callback returns the old one is
// gone.
//
// Lifecycle: resumed with window focus is the window's focus (FocusChanged), and a window
// without it holds the sim (hid_backend_holds_unfocused). With no surface (Home, the recents
// screen, the screen off) pump waits in the looper until there is one again: no frame, no
// GPU work. Touches are TouchPoints in framebuffer pixels; keys from a hardware keyboard go
// through input_android.cc's table, and Back is Escape; gamepads are there too.
//
// Arguments come from the launching intent's string extra "args" (adb shell am start -n
// <package>/android.app.NativeActivity --es args "--demo --hash"), split at spaces. Output
// goes to logcat under the tag "korvid" (core's output sink). The working directory is the
// app's external files directory, where adb pull reaches what a run writes.
#include "korvid/platform_hid/host_window.h"
#include "korvid/core/os_error.h"
#include "korvid/platform_hid/events.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/time.h"
#include "korvid/platform/android.h"
#include "../hid_backend.h"
#include "android_pads.h"

#include <android/configuration.h>
#include <android/input.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char** argv); // the game's, in the same library

namespace kv {

namespace {

enum class DroidMsgType : u8 { Focus, Resumed, Close, Window, InputQueue, Poke };

struct DroidMsg {
    DroidMsgType type;
    bool         on; // Focus: has window focus; Resumed: resumed
};

constexpr u32 kRing = 256;
constexpr int kLooperWake  = 1; // the pipe the OS's thread writes a byte to
constexpr int kLooperInput = 2; // the input queue

struct DroidApp {
    ANativeActivity* activity = nullptr;
    pthread_mutex_t  lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t   cond = PTHREAD_COND_INITIALIZER;
    pthread_t        thread;
    int              wake[2] = {-1, -1};
    char*            args = nullptr; // the intent's "args", owned

    // Under the lock: what the OS's thread posts and hands over.
    DroidMsg       ring[kRing];
    u32            head = 0, tail = 0, dropped = 0;
    ANativeWindow* pending_window = nullptr;
    AInputQueue*   pending_queue  = nullptr;
    bool           window_handoff = false, queue_handoff = false; // one waits to be taken
    bool           running   = false; // the game's thread is inside main
    bool           destroyed = false; // the activity is gone

    // The game's thread's own.
    ALooper*       looper = nullptr;
    ANativeWindow* window = nullptr; // what the surface is now, or null
    AInputQueue*   queue  = nullptr;
    bool           resumed = false, has_focus = false;
    f32            density = 1.0f;
    JNIEnv*        env = nullptr; // the game's thread, attached
    bool           opened = false; // a HostWindow is open
    HostWindow*    host = nullptr; // it, for its surface hook

    // Typed text from Java (KorvidActivity), under the lock: UTF-8 and a count of
    // backspaces, taken by drain. text_on is the game's thread's: a text field wants typing.
    char           typed[256];
    u32            typed_len = 0, typed_backspaces = 0;
    bool           text_on = false;
    bool           java = false; // the activity is KorvidActivity
};

DroidApp g_app; // one application per process

// ---- output: logcat, a line a call -------------------------------------------------------

struct LineBuf { char text[1024]; u32 len = 0; };
pthread_mutex_t g_out_lock = PTHREAD_MUTEX_INITIALIZER;
LineBuf g_out[2];

void logcat_sink(bool err, const char* text, usize len) {
    pthread_mutex_lock(&g_out_lock);
    LineBuf& b = g_out[err ? 1 : 0];
    for (usize i = 0; i < len; ++i) {
        char c = text[i];
        if (c == '\n' || b.len == sizeof b.text - 1) {
            b.text[b.len] = 0;
            __android_log_write(err ? ANDROID_LOG_WARN : ANDROID_LOG_INFO, "korvid", b.text);
            b.len = 0;
            if (c == '\n') continue;
        }
        b.text[b.len++] = c;
    }
    pthread_mutex_unlock(&g_out_lock);
}

// ---- the OS's thread -----------------------------------------------------------------------

void wake() {
    char b = 0;
    while (write(g_app.wake[1], &b, 1) < 0 && errno == EINTR) {}
}

void post(DroidMsgType type, bool on = false) {
    pthread_mutex_lock(&g_app.lock);
    if (g_app.tail - g_app.head >= kRing) ++g_app.dropped;
    else g_app.ring[g_app.tail++ % kRing] = DroidMsg{type, on};
    pthread_mutex_unlock(&g_app.lock);
    wake();
}

// Hands the game's thread a new window (or none) and waits until it has taken it: on
// return the old one may be destroyed. The game's thread, when it is not running (it has
// returned from main, or not started), has nothing to let go of.
void hand_window(ANativeWindow* w) {
    pthread_mutex_lock(&g_app.lock);
    g_app.pending_window = w;
    g_app.window_handoff = true;
    wake();
    while (g_app.window_handoff && g_app.running) pthread_cond_wait(&g_app.cond, &g_app.lock);
    pthread_mutex_unlock(&g_app.lock);
}

void hand_queue(AInputQueue* q) {
    pthread_mutex_lock(&g_app.lock);
    g_app.pending_queue = q;
    g_app.queue_handoff = true;
    wake();
    while (g_app.queue_handoff && g_app.running) pthread_cond_wait(&g_app.cond, &g_app.lock);
    pthread_mutex_unlock(&g_app.lock);
}

// A pending Java exception would abort the next JNI call: log it and go on.
bool jni_ok(JNIEnv* env) {
    if (!env->ExceptionCheck()) return true;
    env->ExceptionDescribe();
    env->ExceptionClear();
    return false;
}

// The whole screen: the system's bars hidden (a swipe shows them for a moment), and the
// window laid out under a camera cutout, whose edges safe_insets reports. On the OS's
// thread, which is the only one that may touch the window's view.
void go_immersive(ANativeActivity* a) {
    JNIEnv* env = a->env;
    jclass activity = env->GetObjectClass(a->clazz);
    jobject window = env->CallObjectMethod(a->clazz, env->GetMethodID(activity, "getWindow", "()Landroid/view/Window;"));
    if (!jni_ok(env) || !window) return;
    jclass window_class = env->GetObjectClass(window);
    jobject lp = env->CallObjectMethod(window, env->GetMethodID(window_class, "getAttributes", "()Landroid/view/WindowManager$LayoutParams;"));
    if (jni_ok(env) && lp) {
        jfieldID mode = env->GetFieldID(env->GetObjectClass(lp), "layoutInDisplayCutoutMode", "I");
        if (jni_ok(env) && mode) {
            env->SetIntField(lp, mode, 3); // LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
            env->CallVoidMethod(window, env->GetMethodID(window_class, "setAttributes", "(Landroid/view/WindowManager$LayoutParams;)V"), lp);
            jni_ok(env);
        }
    }
    jobject ctl = env->CallObjectMethod(window, env->GetMethodID(window_class, "getInsetsController", "()Landroid/view/WindowInsetsController;"));
    if (!jni_ok(env) || !ctl) return;
    jclass ctl_class = env->GetObjectClass(ctl);
    jclass type = env->FindClass("android/view/WindowInsets$Type");
    jint bars = env->CallStaticIntMethod(type, env->GetStaticMethodID(type, "systemBars", "()I"));
    env->CallVoidMethod(ctl, env->GetMethodID(ctl_class, "setSystemBarsBehavior", "(I)V"), 2); // BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
    jni_ok(env);
    env->CallVoidMethod(ctl, env->GetMethodID(ctl_class, "hide", "(I)V"), bars);
    jni_ok(env);
}

// KorvidActivity's native methods: what the input method committed, on the UI thread.
void JNICALL java_text(JNIEnv* env, jclass, jbyteArray utf8) {
    jsize n = utf8 ? env->GetArrayLength(utf8) : 0;
    if (n <= 0) return;
    pthread_mutex_lock(&g_app.lock);
    u32 room = (u32)sizeof g_app.typed - 1 - g_app.typed_len;
    if ((u32)n <= room) { // whole or not at all: never half a code point
        env->GetByteArrayRegion(utf8, 0, n, (jbyte*)(g_app.typed + g_app.typed_len));
        g_app.typed_len += (u32)n;
        g_app.typed[g_app.typed_len] = 0;
    }
    pthread_mutex_unlock(&g_app.lock);
    wake();
}

void JNICALL java_backspace(JNIEnv*, jclass, jint count) {
    if (count <= 0) return;
    pthread_mutex_lock(&g_app.lock);
    g_app.typed_backspaces += (u32)count;
    pthread_mutex_unlock(&g_app.lock);
    wake();
}

// The activity is KorvidActivity when the package was built with a JDK, NativeActivity
// itself when not: only the first has these methods, and only then is there composed text.
bool register_java(ANativeActivity* a) {
    JNIEnv* env = a->env;
    jclass activity = env->GetObjectClass(a->clazz);
    if (!env->GetMethodID(activity, "textInput", "(Z)V")) { env->ExceptionClear(); return false; }
    static const JNINativeMethod methods[] = {
        {"nativeText", "([B)V", (void*)java_text},
        {"nativeBackspace", "(I)V", (void*)java_backspace},
    };
    if (env->RegisterNatives(activity, methods, 2) != 0) { jni_ok(env); return false; }
    return true;
}

// The intent's string extra "args", malloc'd, or null.
char* intent_args(ANativeActivity* a) {
    JNIEnv* env = a->env;
    jclass activity = env->GetObjectClass(a->clazz);
    jobject intent = env->CallObjectMethod(a->clazz, env->GetMethodID(activity, "getIntent", "()Landroid/content/Intent;"));
    if (!jni_ok(env) || !intent) return nullptr;
    jstring key = env->NewStringUTF("args");
    jstring val = (jstring)env->CallObjectMethod(intent, env->GetMethodID(env->GetObjectClass(intent), "getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;"), key);
    if (!jni_ok(env) || !val) return nullptr;
    const char* utf = env->GetStringUTFChars(val, nullptr);
    char* out = utf ? strdup(utf) : nullptr;
    if (utf) env->ReleaseStringUTFChars(val, utf);
    return out;
}

// The installed package's path, for the layer below: where the app's files are.
void tell_platform(ANativeActivity* a) {
    JNIEnv* env = a->env;
    jclass activity = env->GetObjectClass(a->clazz);
    jstring path = (jstring)env->CallObjectMethod(a->clazz, env->GetMethodID(activity, "getPackageResourcePath", "()Ljava/lang/String;"));
    if (!jni_ok(env) || !path) { __android_log_write(ANDROID_LOG_ERROR, "korvid", "the package's path is unknown: no assets"); return; }
    const char* utf = env->GetStringUTFChars(path, nullptr);
    platform_android_app(utf, a->internalDataPath);
    if (utf) env->ReleaseStringUTFChars(path, utf);
}

void* game_thread(void*) {
    DroidApp& app = g_app;
    app.activity->vm->AttachCurrentThread(&app.env, nullptr);
    app.looper = ALooper_prepare(0);
    ALooper_addFd(app.looper, app.wake[0], kLooperWake, ALOOPER_EVENT_INPUT, nullptr, nullptr);

    // A relative path the engine writes (--screenshot, --profile) lands in the app's
    // external files directory: /sdcard/Android/data/<package>/files, which adb pull reads.
    const char* out_dir = app.activity->externalDataPath ? app.activity->externalDataPath : app.activity->internalDataPath;
    if (out_dir) {
        mkdir(out_dir, 0770);
        if (chdir(out_dir) != 0) KV_WARN("window", "cannot enter %s: %s", out_dir, os_error_text(errno));
    }

    // argv: the program's name, then the intent's "args" split at spaces. The name is the
    // library's without "lib" and ".so" (librocks.so is rocks), as the loader knows it:
    // what the frame loop names the game's pack by.
    static char  name[128] = "korvid";
    Dl_info self;
    if (dladdr((void*)&game_thread, &self) && self.dli_fname) {
        const char* base = strrchr(self.dli_fname, '/');
        base = base ? base + 1 : self.dli_fname;
        if (strncmp(base, "lib", 3) == 0) base += 3;
        usize n = strlen(base);
        if (n > 3 && strcmp(base + n - 3, ".so") == 0) n -= 3;
        if (n > 0 && n < sizeof name) { memcpy(name, base, n); name[n] = 0; }
    }
    static char* argv[64];
    int argc = 0;
    argv[argc++] = name;
    for (char* p = app.args; p && *p && argc < 63;) {
        while (*p == ' ') ++p;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') ++p;
        if (*p) *p++ = 0;
    }
    argv[argc] = nullptr;

    int rc = main(argc, argv);

    // The game is done: the activity goes with it, and so does the process (the library
    // stays loaded in a process that lives on, and its statics would greet the next
    // launch as they were left).
    print_flush();
    pthread_mutex_lock(&app.lock);
    app.running = false;
    bool destroyed = app.destroyed;
    pthread_cond_broadcast(&app.cond);
    pthread_mutex_unlock(&app.lock);
    if (!destroyed) {
        ANativeActivity_finish(app.activity);
        f64 start = time_now();
        for (;;) {
            pthread_mutex_lock(&app.lock);
            destroyed = app.destroyed;
            pthread_mutex_unlock(&app.lock);
            if (destroyed || time_now() - start > 2.0) break;
            usleep(10000);
        }
    }
    __android_log_print(ANDROID_LOG_INFO, "korvid", "exit %d", rc);
    _exit(rc);
    return nullptr;
}

// The activity's callbacks, on the OS's thread.
void on_resume(ANativeActivity*) { platform_android_foreground(true); post(DroidMsgType::Resumed, true); }
void on_pause(ANativeActivity*) { platform_android_foreground(false); post(DroidMsgType::Resumed, false); }
void on_focus(ANativeActivity* a, int focused) {
    if (focused) go_immersive(a); // the bars come back with a dialog or the recents screen
    post(DroidMsgType::Focus, focused != 0);
}
void on_window_created(ANativeActivity*, ANativeWindow* w) { hand_window(w); }
void on_window_destroyed(ANativeActivity*, ANativeWindow*) { hand_window(nullptr); }
void on_window_changed(ANativeActivity*, ANativeWindow*) { post(DroidMsgType::Poke); }
void on_rect_changed(ANativeActivity*, const ARect*) { post(DroidMsgType::Poke); }
void on_config_changed(ANativeActivity*) { post(DroidMsgType::Poke); }
void on_queue_created(ANativeActivity*, AInputQueue* q) { hand_queue(q); }
void on_queue_destroyed(ANativeActivity*, AInputQueue*) { hand_queue(nullptr); }
void on_destroy(ANativeActivity*) {
    // The system is taking the activity (swiped from the recents, or finish): ask the game
    // to close and give it a moment to, then the process ends either way.
    pthread_mutex_lock(&g_app.lock);
    g_app.destroyed = true;
    bool running = g_app.running;
    pthread_mutex_unlock(&g_app.lock);
    if (!running) return;
    post(DroidMsgType::Close);
    f64 start = time_now();
    while (time_now() - start < 2.0) usleep(10000); // the game's thread _exits when main returns
    __android_log_write(ANDROID_LOG_WARN, "korvid", "the activity was destroyed and the game did not close in 2 s: exiting");
    _exit(0);
}

// ---- the game's thread ---------------------------------------------------------------------

// Takes what the OS's thread is handing over, letting the old one go first. When the
// window changes, a HostWindow sees it in drain.
void take_handoffs() {
    DroidApp& app = g_app;
    pthread_mutex_lock(&app.lock);
    if (app.queue_handoff) {
        if (app.queue) AInputQueue_detachLooper(app.queue);
        app.queue = app.pending_queue;
        if (app.queue) AInputQueue_attachLooper(app.queue, app.looper, kLooperInput, nullptr, nullptr);
        app.queue_handoff = false;
    }
    bool window_changed = app.window_handoff;
    ANativeWindow* next = app.pending_window;
    pthread_mutex_unlock(&app.lock);
    if (window_changed) {
        // Whoever draws lets go of the old surface before the OS's thread is told it may
        // return (and destroy it), and takes the new one after it is ours.
        HostWindow* host = app.opened ? app.host : nullptr;
        if (host && app.window && host->surface_fn_) host->surface_fn_(host->surface_ctx_, nullptr);
        app.window = next;
        if (host) host->native_ = next;
        if (host && next && host->surface_fn_) host->surface_fn_(host->surface_ctx_, next);
    }
    pthread_mutex_lock(&app.lock);
    if (window_changed) app.window_handoff = false;
    pthread_cond_broadcast(&app.cond);
    pthread_mutex_unlock(&app.lock);
}

// One pass over the looper: waits up to timeout_ms (-1: until something arrives), empties
// the wake pipe. Input events stay in their queue for drain.
void poll_looper(int timeout_ms) {
    int ident;
    while ((ident = ALooper_pollOnce(timeout_ms, nullptr, nullptr, nullptr)) >= 0) {
        if (ident == kLooperWake) {
            char buf[64];
            while (read(g_app.wake[0], buf, sizeof buf) > 0) {}
        }
        timeout_ms = 0;
    }
    take_handoffs();
}

// The system's bars and the cutout, in pixels: left, top, right, bottom. Read through JNI
// from the decor view's root insets; zeros if the view is not attached yet.
void read_insets(f32 out[4]) {
    for (u32 i = 0; i < 4; ++i) out[i] = 0.0f;
    JNIEnv* env = g_app.env;
    ANativeActivity* a = g_app.activity;
    if (env->PushLocalFrame(16) != 0) return;
    jclass activity = env->GetObjectClass(a->clazz);
    jobject window = env->CallObjectMethod(a->clazz, env->GetMethodID(activity, "getWindow", "()Landroid/view/Window;"));
    jobject decor = jni_ok(env) && window ? env->CallObjectMethod(window, env->GetMethodID(env->GetObjectClass(window), "getDecorView", "()Landroid/view/View;")) : nullptr;
    jobject insets = jni_ok(env) && decor ? env->CallObjectMethod(decor, env->GetMethodID(env->GetObjectClass(decor), "getRootWindowInsets", "()Landroid/view/WindowInsets;")) : nullptr;
    if (jni_ok(env) && insets) {
        jclass type = env->FindClass("android/view/WindowInsets$Type");
        jint mask = env->CallStaticIntMethod(type, env->GetStaticMethodID(type, "systemBars", "()I")) |
                    env->CallStaticIntMethod(type, env->GetStaticMethodID(type, "displayCutout", "()I"));
        // What is there even while the bars are hidden: where a swipe would bring them, and the cutout.
        jobject in = env->CallObjectMethod(insets, env->GetMethodID(env->GetObjectClass(insets), "getInsetsIgnoringVisibility", "(I)Landroid/graphics/Insets;"), mask);
        if (jni_ok(env) && in) {
            jclass ic = env->GetObjectClass(in);
            const char* names[4] = {"left", "top", "right", "bottom"};
            for (u32 i = 0; i < 4; ++i) out[i] = (f32)env->GetIntField(in, env->GetFieldID(ic, names[i], "I"));
        }
    }
    jni_ok(env);
    env->PopLocalFrame(nullptr);
}

// Landscape either way up, or portrait: the shape the window asked for.
void request_orientation(bool landscape) {
    JNIEnv* env = g_app.env;
    jclass activity = env->GetObjectClass(g_app.activity->clazz);
    env->CallVoidMethod(g_app.activity->clazz, env->GetMethodID(activity, "setRequestedOrientation", "(I)V"),
                        landscape ? 6 : 7); // SCREEN_ORIENTATION_SENSOR_LANDSCAPE, _SENSOR_PORTRAIT
    jni_ok(env);
}

f32 read_density() {
    AConfiguration* c = AConfiguration_new();
    AConfiguration_fromAssetManager(c, g_app.activity->assetManager);
    int dpi = AConfiguration_getDensity(c);
    AConfiguration_delete(c);
    return dpi > 0 && dpi < 0xfffe ? (f32)dpi / 160.0f : 1.0f;
}

bool on_screen() { return g_app.window != nullptr; }

} // namespace

} // namespace kv

// The OS's entry: the library is loaded and the activity is being created.
extern "C" JNIEXPORT void ANativeActivity_onCreate(ANativeActivity* activity, void*, size_t) {
    using namespace kv;
    DroidApp& app = g_app;
    if (app.activity) {
        // A second activity in a process that still holds the first one's game: the statics
        // are not ours to reuse. The process is ending; let this launch be the next one's.
        __android_log_write(ANDROID_LOG_WARN, "korvid", "an activity created while the last game is still closing: finishing it");
        ANativeActivity_finish(activity);
        return;
    }
    output_set_sink(logcat_sink);
    app.activity = activity;
    activity->callbacks->onResume               = on_resume;
    activity->callbacks->onPause                = on_pause;
    activity->callbacks->onDestroy              = on_destroy;
    activity->callbacks->onWindowFocusChanged   = on_focus;
    activity->callbacks->onNativeWindowCreated  = on_window_created;
    activity->callbacks->onNativeWindowDestroyed = on_window_destroyed;
    activity->callbacks->onNativeWindowResized  = on_window_changed;
    activity->callbacks->onNativeWindowRedrawNeeded = on_window_changed;
    activity->callbacks->onContentRectChanged   = on_rect_changed;
    activity->callbacks->onConfigurationChanged = on_config_changed;
    activity->callbacks->onInputQueueCreated    = on_queue_created;
    activity->callbacks->onInputQueueDestroyed  = on_queue_destroyed;
    app.args = intent_args(activity);
    app.java = register_java(activity);
    tell_platform(activity);
    go_immersive(activity);
    if (pipe2(app.wake, O_NONBLOCK | O_CLOEXEC) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, "korvid", "pipe: %s", os_error_text(errno));
        ANativeActivity_finish(activity);
        return;
    }
    app.running = true;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20); // a main thread's stack, not a worker's
    if (pthread_create(&app.thread, &attr, game_thread, nullptr) != 0) {
        app.running = false;
        __android_log_write(ANDROID_LOG_ERROR, "korvid", "cannot start the game's thread");
        ANativeActivity_finish(activity);
    }
    pthread_attr_destroy(&attr);
}

namespace kv {

int platform_app_main(int argc, char** argv, int (*run)(int argc, char** argv, void* ctx), void* ctx) {
    return run(argc, argv, ctx); // already on the game's thread (or a test, under adb's shell)
}

bool hid_backend_startup() { return true; }
void hid_backend_shutdown() {}
// A window needs the activity: a test run from adb's shell has none.
bool hid_video_up() { return g_app.activity != nullptr; }
void hid_video_down() {}
bool hid_backend_holds_unfocused() { return true; }

bool HostWindow::open(const WindowDesc& desc) {
    KV_ASSERT(!native_);
    if (!hid_video_up()) { KV_ERROR("window", "no activity (the process is not an Android app)"); return false; }
    DroidApp& app = g_app;
    // The screen is the window; what the size asked for says is its shape, as on iOS.
    bool landscape = desc.width > desc.height;
    request_orientation(landscape);
    // The surface arrives through the looper soon after launch, and again after the turn.
    f64 start = time_now();
    while (!app.window && time_now() - start < 5.0) poll_looper(50);
    if (!app.window) { KV_ERROR("window", "no surface arrived in 5 s"); return false; }
    start = time_now();
    while (app.window && (ANativeWindow_getWidth(app.window) > ANativeWindow_getHeight(app.window)) != landscape && time_now() - start < 2.0)
        poll_looper(20);
    if (!app.window) { KV_ERROR("window", "the surface went away during the turn"); return false; }
    native_  = app.window;
    surface_ = desc.surface;
    vsync_   = desc.vsync;
    width_   = (u32)ANativeWindow_getWidth(app.window);
    height_  = (u32)ANativeWindow_getHeight(app.window);
    app.density = read_density();
    read_insets(insets_);
    app.opened = true;
    app.host   = this;
    // What came before the window is in its fields: only the state survives the ring.
    pthread_mutex_lock(&app.lock);
    while (app.head != app.tail) {
        DroidMsg m = app.ring[app.head++ % kRing];
        if (m.type == DroidMsgType::Focus) app.has_focus = m.on;
        if (m.type == DroidMsgType::Resumed) app.resumed = m.on;
    }
    pthread_mutex_unlock(&app.lock);
    focused_ = app.resumed && app.has_focus;
    KV_INFO("window", "opened '%s' %ux%u px, %s (the screen at %.2fx, insets %.0f %.0f %.0f %.0f; asked for %ux%u, which Android takes as the shape)",
            desc.title, width_, height_, landscape ? "landscape" : "portrait", (f64)app.density,
            (f64)insets_[0], (f64)insets_[1], (f64)insets_[2], (f64)insets_[3], desc.width, desc.height);
    return true;
}

void HostWindow::close() {
    if (!native_ && !g_app.opened) return;
    g_app.opened = false;
    g_app.host   = nullptr;
    native_ = nullptr;
    width_ = height_ = 0;
    focused_ = false;
    hidden_  = 0;
    hidden_sent_ = WindowHidden::None;
    hid_window_parts_reset(*this);
    KV_INFO("window", "closed");
}

void HostWindow::present_blank() {} // the system shows the activity's background
bool HostWindow::software_frame(SoftwareFrame*) { return false; } // not yet on this host (the CPU renderer PRD's SR21)
void HostWindow::present_software() {}
bool HostWindow::software_vsync(bool) { return false; }
// The on-screen keyboard up or down, and typed text wanted or not: KorvidActivity's
// textInput, which does its work on the UI thread. A package without the class (built
// without a JDK) has keys and no text.
// Not yet on this host (Krome slice 3): the input method's area is not told, the clipboard is
// the platform module's own (inside this process only), the cursor keeps its shape.
void HostWindow::text_area(rect2, f32) {}
bool HostWindow::clipboard_get(Array<char>&) { return false; }
bool HostWindow::clipboard_set(const char*) { return false; }
void HostWindow::cursor(CursorShape) {}
void HostWindow::minimize(bool) {}
bool HostWindow::capture_mouse(bool) { return false; }

void HostWindow::text_input(bool on) {
    DroidApp& app = g_app;
    app.text_on = on;
    if (!app.java) {
        if (on) KV_WARN("window", "no typed text: this package has no KorvidActivity (it was built without a JDK)");
        return;
    }
    JNIEnv* env = app.env;
    jclass activity = env->GetObjectClass(app.activity->clazz);
    env->CallVoidMethod(app.activity->clazz, env->GetMethodID(activity, "textInput", "(Z)V"), on ? JNI_TRUE : JNI_FALSE);
    jni_ok(env);
}
bool HostWindow::touch_screen() const { return true; }
f32  HostWindow::pixel_scale() const { return g_app.density; }

void HostWindow::pump(EventBus& bus, Input& input) {
    input.raw.touches_settle(); // the touches that ended in the last pump are gone
    poll_looper(0);
    // No surface: nothing may be drawn and nothing is seen. Wait in the looper until there
    // is one again, or the activity is gone (the Close is in the ring then).
    if (g_app.opened && !on_screen()) {
        native_ = nullptr; // whoever draws has let go already (on_surface, from the handoff)
        set_hidden(bus, HiddenUnmapped, true); // seen by the loop when it comes back (KR21)
        KV_INFO("window", "no surface: waiting");
        for (;;) {
            poll_looper(250);
            pthread_mutex_lock(&g_app.lock);
            bool gone = g_app.destroyed;
            pthread_mutex_unlock(&g_app.lock);
            if (on_screen() || gone) break;
        }
        if (on_screen()) KV_INFO("window", "a surface again");
    }
    drain(bus, input);
}

void HostWindow::drain(EventBus& bus, Input& input) {
    DroidApp& app = g_app;
    if (!app.opened) return;
    // The ring: focus and the lifecycle, a close.
    bool poked = false;
    for (;;) {
        pthread_mutex_lock(&app.lock);
        if (app.dropped) { KV_WARN("window", "%u activity events dropped: the ring was full", app.dropped); app.dropped = 0; }
        bool any = app.head != app.tail;
        DroidMsg m = any ? app.ring[app.head++ % kRing] : DroidMsg{};
        pthread_mutex_unlock(&app.lock);
        if (!any) break;
        switch (m.type) {
        case DroidMsgType::Focus:   app.has_focus = m.on; break;
        case DroidMsgType::Resumed: app.resumed = m.on; break;
        case DroidMsgType::Close:   bus.emit(CloseRequested{0}); break;
        case DroidMsgType::Poke:    poked = true; break; // the size is read below, the insets with it
        default: break;
        }
    }
    // No surface is not seen (KR21). Paused is not hidden: in split screen a paused app
    // is on screen.
    set_hidden(bus, HiddenUnmapped, !on_screen());
    bool focused = app.resumed && app.has_focus && on_screen();
    if (focused != focused_) {
        focused_ = focused;
        if (!focused) input.raw.clear(); // keys released while unfocused never report an up edge
        bus.emit(FocusChanged{focused});
    }
    // The surface: the one there is now, at the size it has now.
    native_ = app.window;
    if (app.window) {
        u32 w = (u32)ANativeWindow_getWidth(app.window), h = (u32)ANativeWindow_getHeight(app.window);
        bool resized = w != width_ || h != height_;
        if (resized || poked) {
            // The insets settle a little after the surface does (a turn's bars arrive with
            // the layout that follows it): read again whenever the activity says its
            // content or configuration changed.
            f32 before[4];
            for (u32 i = 0; i < 4; ++i) before[i] = insets_[i];
            app.density = read_density();
            read_insets(insets_);
            if (memcmp(before, insets_, sizeof before) != 0)
                KV_INFO("window", "insets %.0f %.0f %.0f %.0f", (f64)insets_[0], (f64)insets_[1], (f64)insets_[2], (f64)insets_[3]);
        }
        if (resized) {
            width_ = w;
            height_ = h;
            bus.emit(WindowResized{w, h});
        }
    }
    // What Java's input connection committed since the last pump.
    pthread_mutex_lock(&app.lock);
    char typed[sizeof app.typed];
    u32 backspaces = app.typed_backspaces;
    memcpy(typed, app.typed, app.typed_len + 1u);
    typed[app.typed_len] = 0;
    app.typed_len = 0;
    app.typed_backspaces = 0;
    pthread_mutex_unlock(&app.lock);
    if (app.text_on) {
        input.raw.backspaces += backspaces;
        if (typed[0]) input_typed(bus, input, typed);
    }
    // Input: touches and a hardware keyboard's keys.
    AInputEvent* ev = nullptr;
    while (app.queue && AInputQueue_getEvent(app.queue, &ev) >= 0) {
        if (AInputQueue_preDispatchEvent(app.queue, ev)) continue; // an input method took it
        int handled = 0;
        int type = AInputEvent_getType(ev);
        if (input_android_pad_event(ev, bus, input)) {
            handled = 1;
        } else if (type == AINPUT_EVENT_TYPE_KEY) {
            Key k = key_from_scancode((u32)AKeyEvent_getKeyCode(ev));
            int action = AKeyEvent_getAction(ev);
            if (k != Key::Unknown && (action == AKEY_EVENT_ACTION_DOWN || action == AKEY_EVENT_ACTION_UP)) {
                bool down = action == AKEY_EVENT_ACTION_DOWN;
                bool repeat = down && AKeyEvent_getRepeatCount(ev) > 0;
                if (k == Key::Backspace && down) ++input.raw.backspaces;
                input.raw.set(k, down);
                if (down) bus.emit(KeyDown{k, 0, repeat});
                else      bus.emit(KeyUp{k, 0});
                // Back among them: it is Escape, not "leave the app". While a text field
                // wants typing, a key that types goes on to Java unhandled, where the
                // layout's character for it is known (KorvidActivity.dispatchKeyEvent).
                bool types = (k >= Key::A && k <= Key::Num9) || k == Key::Space;
                handled = app.text_on && app.java && types ? 0 : 1;
            } else if (app.text_on && app.java) {
                handled = 0; // punctuation and the rest have no Key: Java has their characters
            }
        } else if (type == AINPUT_EVENT_TYPE_MOTION && (AInputEvent_getSource(ev) & AINPUT_SOURCE_CLASS_POINTER)) {
            int action = AMotionEvent_getAction(ev);
            int what = action & AMOTION_EVENT_ACTION_MASK;
            usize index = (usize)((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
            usize count = AMotionEvent_getPointerCount(ev);
            auto id_of = [&](usize i) { return (u32)AMotionEvent_getPointerId(ev, i) + 1; }; // 0 is a free slot
            switch (what) {
            case AMOTION_EVENT_ACTION_DOWN:
            case AMOTION_EVENT_ACTION_POINTER_DOWN:
                if (index < count) input.raw.touch_begin(id_of(index), AMotionEvent_getX(ev, index), AMotionEvent_getY(ev, index));
                break;
            case AMOTION_EVENT_ACTION_MOVE:
                for (usize i = 0; i < count; ++i)
                    if (TouchPoint* t = input.raw.touch(id_of(i))) { t->x = AMotionEvent_getX(ev, i); t->y = AMotionEvent_getY(ev, i); }
                break;
            case AMOTION_EVENT_ACTION_UP:
            case AMOTION_EVENT_ACTION_POINTER_UP:
                if (index < count)
                    if (TouchPoint* t = input.raw.touch(id_of(index))) { t->x = AMotionEvent_getX(ev, index); t->y = AMotionEvent_getY(ev, index); t->ended = true; }
                break;
            case AMOTION_EVENT_ACTION_CANCEL: // the system took the gesture: every finger is up
                for (usize i = 0; i < count; ++i)
                    if (TouchPoint* t = input.raw.touch(id_of(i))) t->ended = true;
                break;
            default: break;
            }
            handled = 1;
        }
        AInputQueue_finishEvent(app.queue, ev, handled);
    }
}

// Not written yet (slice 12 left it open): a wait is the 4 ms poll the loop had. The OS's
// wait goes here (docs/PRDs/Engine-Slice-12-PRD.md, "Waiting").
bool HostWindow::wait(f64 seconds) {
    if (seconds > 0.0) time_sleep(seconds < 0.004 ? seconds : 0.004);
    return false;
}
void HostWindow::wake() {}

void HostWindow::on_modal(void (*fn)(void* ctx), void* ctx) {
    modal_fn_  = fn; // never called: nothing on Android holds the game's thread in a loop of its own
    modal_ctx_ = ctx;
}

// A window that is not a rectangle: not offered on a phone, where the window is the screen.
bool hid_window_shape(HostWindow&) { return false; }
bool hid_window_grips(HostWindow&) { return false; }
void hid_window_bounds(HostWindow&) {}

} // namespace kv
