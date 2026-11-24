// The settings store (KR24): values of each kind written and read back by another store;
// a struct by its field table, a part of it, and JSON a person wrote, kept; a spoiled file
// replaced. Then other processes (this test's own binary, run with --settings-child): one
// killed while it writes the temporary file and one killed before it renames it, each
// leaving the old value; two writing at once, neither losing the other's key.
#include "korvid/platform/settings.h"
#include "korvid/platform/io.h"
#include "korvid/platform/paths.h"
#include "korvid/platform/process.h"
#include "korvid/core/format.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h> // _NSGetExecutablePath: Apple has no /proc
#endif

namespace {

using namespace kv;

struct Window { i32 x, y; u32 w, h; bool maximized; char display[16]; f32 scale[2]; };
constinit const FieldDesc k_window_fields[] = {KV_FIELDS(Window, x, y, w, h, maximized, display, scale)};

f64 number_in(const char* dir, const char* key) {
    SettingsStore* s = settings_open_dir(dir);
    f64 v = settings_number(s, key, -1.0);
    settings_close(s);
    return v;
}

bool file_has(const char* path, const char* text) {
    IoBytes b;
    if (!io_read_file(path, &b)) return false;
    bool found = false;
    for (usize i = 0; i + strlen(text) <= b.size && !found; ++i) found = !memcmp(b.data + i, text, strlen(text));
    b.free();
    return found;
}

// Runs this binary as a child that sets `key` to `value` (`count` times, flushing each),
// with KORVID_SETTINGS_STALL set to `stall` (or not).
bool start_child(Process& p, const char* dir, const char* key, const char* value, const char* count, const char* stall) {
    static char self[1024];
#if defined(_WIN32)
    if (!platform_exe_dir(self, sizeof self - 32)) return false;
    format(self + strlen(self), 32, "platform_tests.exe");
#elif defined(__APPLE__)
    uint32_t cap = sizeof self;
    if (_NSGetExecutablePath(self, &cap) != 0) return false;
#else
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) return false;
    self[n] = 0;
#endif
    const char* argv[] = {self, "--settings-child", dir, key, value, count};
    char env[64];
    format(env, sizeof env, "KORVID_SETTINGS_STALL=%s", stall ? stall : "");
    const char* envs[] = {env};
    return p.spawn(ProcessDesc{argv, nullptr, envs});
}

bool finish(Process& p, f64 seconds) {
    char buf[512];
    f64 end = time_now() + seconds;
    while (p.running() && time_now() < end) { while (p.read_out(buf, sizeof buf) > 0) {} while (p.read_err(buf, sizeof buf) > 0) {} time_sleep(0.005); }
    bool done = !p.running();
    return done && p.code == 0;
}

} // namespace

// The child's side: set the key, as many times as asked, flushing each.
int settings_child(int argc, char** argv) {
    if (argc < 6) return 2;
    SettingsStore* s = settings_open_dir(argv[2]);
    if (!s) return 3;
    int count = atoi(argv[5]);
    for (int i = 1; i <= count; ++i) {
        settings_set_number(s, argv[3], count > 1 ? (f64)i : atof(argv[4]));
        if (!settings_flush(s)) return 4;
    }
    settings_close(s);
    return 0;
}

void test_settings(TestCtx& c) {
    c.begin("settings");
    char dir[200], path[300];
    KV_CHECK(c, io_make_temp_dir("korvid-settings", dir, sizeof dir));
    format(path, sizeof path, "%s/settings.json", dir);

    // Written by one store, read by the next.
    {
        SettingsStore* s = settings_open_dir(dir);
        KV_CHECK(c, s && !settings_has(s, "volume") && settings_number(s, "volume", 0.8) == 0.8);
        settings_set_number(s, "volume", 0.25);
        settings_set_int(s, "track", -42);
        settings_set_int(s, "big", 1ll << 60);
        settings_set_string(s, "face", "dev \xc3\xa9");
        Window w{-10, 20, 1280, 720, true, "DP-1", {1.5f, 2.0f}};
        settings_set_struct(s, "window", k_window_fields, &w);
        settings_set_string(s, "gone", "soon");
        settings_remove(s, "gone");
        KV_CHECK(c, settings_number(s, "volume", 0) == 0.25 && !settings_has(s, "gone")); // at once, in memory
        KV_CHECK(c, settings_flush(s) && io_is_file(path));
        settings_close(s);
    }
    {
        SettingsStore* s = settings_open_dir(dir);
        KV_CHECK(c, settings_number(s, "volume", 0) == 0.25 && settings_int(s, "track", 0) == -42 && settings_int(s, "big", 0) == (1ll << 60));
        KV_CHECK(c, !strcmp(settings_string(s, "face", ""), "dev \xc3\xa9") && !settings_has(s, "gone"));
        KV_CHECK(c, settings_number(s, "track", 0) == -42.0 && settings_int(s, "face", 7) == 7); // an int is a number; a string is neither
        Window w{};
        KV_CHECK(c, settings_struct(s, "window", k_window_fields, &w));
        KV_CHECK(c, w.x == -10 && w.y == 20 && w.w == 1280 && w.h == 720 && w.maximized && !strcmp(w.display, "DP-1") && w.scale[0] == 1.5f && w.scale[1] == 2.0f);
        KV_CHECK(c, !settings_struct(s, "volume", k_window_fields, &w) && !settings_struct(s, "nothing", k_window_fields, &w));
        settings_close(s);
    }
    // A person's edit: a key the program does not know, and a field left out, are kept.
    {
        const char* text = "{\"korvid_settings\": 1, \"values\": {\"volume\": 0.5, \"recent\": [\"a.flac\", \"b.flac\"], \"window\": {\"w\": 640}}}";
        KV_CHECK(c, io_write_file(path, text, strlen(text)));
        SettingsStore* s = settings_open_dir(dir);
        Window w{1, 2, 3, 4, false, "x", {9, 9}};
        KV_CHECK(c, settings_struct(s, "window", k_window_fields, &w) && w.w == 640 && w.x == 1 && w.h == 4 && !strcmp(w.display, "x"));
        settings_set_number(s, "volume", 0.75);
        KV_CHECK(c, settings_flush(s));
        settings_close(s);
        KV_CHECK(c, file_has(path, "\"recent\"") && file_has(path, "b.flac") && number_in(dir, "volume") == 0.75);
    }
    // A spoiled file: an empty store, and the first write replaces it.
    {
        KV_CHECK(c, io_write_file(path, "{\"values\": {", 12));
        SettingsStore* s = settings_open_dir(dir);
        KV_CHECK(c, s && !settings_has(s, "volume"));
        settings_set_number(s, "volume", 0.1);
        KV_CHECK(c, settings_flush(s));
        settings_close(s);
        KV_CHECK(c, number_in(dir, "volume") == 0.1);
    }

    if (process_supported()) {
#if KV_DEV
        // Killed mid-write and before the rename (the stalls are dev builds'): the old
        // value, each time.
        char tmp[320];
        format(tmp, sizeof tmp, "%s/settings.json.tmp", dir);
        const char* stalls[] = {"write", "rename"};
        for (const char* stall : stalls) {
            io_remove(tmp);
            Process p;
            KV_CHECK(c, start_child(p, dir, "volume", "0.9", "1", stall));
            bool there = false;
            for (u32 i = 0; i < 1000 && !there; ++i) { time_sleep(0.005); there = io_is_file(tmp); }
            time_sleep(0.05); // into the stall
            KV_CHECK(c, there && p.running());
            p.kill();
            finish(p, 5.0);
            p.close();
            KV_CHECK(c, number_in(dir, "volume") == 0.1);
        }
#endif
        // Not killed: the new value.
        {
            Process p;
            KV_CHECK(c, start_child(p, dir, "volume", "0.9", "1", nullptr) && finish(p, 10.0));
            p.close();
            KV_CHECK(c, number_in(dir, "volume") == 0.9);
        }
        // Two at once, each setting its own key forty times: both keys, at their last values.
        {
            Process a, b;
            KV_CHECK(c, start_child(a, dir, "left", "0", "40", nullptr) && start_child(b, dir, "right", "0", "40", nullptr));
            KV_CHECK(c, finish(a, 30.0) && finish(b, 30.0));
            a.close();
            b.close();
            KV_CHECK(c, number_in(dir, "left") == 40.0 && number_in(dir, "right") == 40.0 && number_in(dir, "volume") == 0.9);
        }
    }
    KV_CHECK(c, io_remove_tree(dir));
}
