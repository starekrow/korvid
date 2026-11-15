// kimc, end to end: a .kim written to a temporary directory, the built tool run over it
// as a command line runs it, and what it wrote read back. The language's own tests are
// kim's (modules/kim/tests); these are the tool's: its verbs, its exit status, errors on
// stderr as file:line:col.
#include "korvid/core/array.h"
#include "korvid/core/json.h"
#include "korvid/core/kbor.h"
#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include "korvid/kim/kim.h"
#include "korvid/platform/io.h"
#include "korvid/platform/module.h"
#include "korvid/platform/process.h"
#include <stdio.h>
#include <string.h>

namespace {

using namespace kv;

char g_dir[512];
Array<char> g_out, g_err;

const char* path(const char* rel) {
    static char p[4][1024];
    static u32 next = 0;
    char* out = p[next++ % 4];
    snprintf(out, sizeof p[0], "%s/%s", g_dir, rel);
    return out;
}

bool put(const char* rel, const void* data, usize n) { return io_write_now(path(rel), data, n); }
bool put(const char* rel, const char* text) { return put(rel, text, strlen(text)); }

// A file's bytes, valid until the next read.
Array<u8> g_file;
bool read(const char* rel) {
    IoBytes b;
    g_file.len = 0;
    if (!io_read_now(path(rel), &b)) return false;
    if (b.size) memcpy(g_file.push_n((u32)b.size), b.data, b.size);
    b.free();
    return true;
}

// The kimc beside this test, run with these words: its exit status, and its two streams.
int run_words(const char* const* words, u32 n) {
    char exe[700];
    if (!platform_exe_dir(exe, sizeof exe)) return -100;
    usize len = strlen(exe);
    snprintf(exe + len, sizeof exe - len, "kimc");
    const char* argv[8];
    argv[0] = exe;
    for (u32 i = 0; i < n; ++i) argv[i + 1] = words[i];
    ProcessDesc d;
    d.argv = Span<const char* const>(argv, n + 1);
    Process p;
    g_out.len = g_err.len = 0;
    if (!p.spawn(d)) { KV_ERROR("test", "cannot run %s: %s", exe, p.error); return -101; }
    bool out_open = true, err_open = true;
    char buf[4096];
    f64 start = time_now();
    while (out_open || err_open || p.running()) {
        bool any = false;
        if (out_open) { i32 r = p.read_out(buf, sizeof buf); if (r > 0) { memcpy(g_out.push_n((u32)r), buf, (usize)r); any = true; } else if (r < 0) out_open = false; }
        if (err_open) { i32 r = p.read_err(buf, sizeof buf); if (r > 0) { memcpy(g_err.push_n((u32)r), buf, (usize)r); any = true; } else if (r < 0) err_open = false; }
        if (time_now() - start > 60.0) { p.kill(); break; }
        if (!any) time_sleep(0.001);
    }
    g_out.push(0);
    g_err.push(0);
    int code = p.exited ? p.code : -102;
    p.free();
    return code;
}
template <class... A>
int kimc(A... words) {
    const char* const list[] = {words...};
    return run_words(list, (u32)sizeof...(words));
}

bool has(const char* text, const char* what) {
    if (strstr(text, what)) return true;
    KV_ERROR("test", "'%s' not in:\n%s", what, text);
    return false;
}

const char* k_form =
    "use \"krome/std\"\n"
    "screen form {\n"
    "    col gap=8 { button \"Save\" -> save }\n"
    "}\n";

void test_compile(TestCtx& c) {
    c.begin("kimc compile");
    KV_CHECK(c, put("form.kim", k_form));
    KV_CHECK(c, kimc("compile", path("form.kim"), path("form.kimb"), "--as", "game/form.kim") == 0 && has(g_out.data, "form.kimb: "));
    KV_CHECK(c, !io_is_file(path("form.kimb.kimc-tmp")));
    // The bytes are kim_compile's.
    Array<u8> want{};
    KimErrors e;
    KV_CHECK(c, kim_compile(k_form, strlen(k_form), "game/form.kim", want, e));
    KV_CHECK(c, read("form.kimb") && g_file.len == want.len && memcmp(g_file.data, want.data, want.len) == 0);
    want.free();
    JsonDoc doc;
    KV_CHECK(c, kbor_parse(g_file.data, g_file.len, mem_scratch(), doc));
    KV_CHECK(c, json_get(doc, doc.root(), "lines") && !strcmp(json_string(json_get(doc, doc.root(), "file"), ""), "game/form.kim"));

    KV_CHECK(c, kimc("compile", path("form.kim"), path("bare.kimb"), "--no-lines") == 0);
    KV_CHECK(c, read("bare.kimb") && kbor_parse(g_file.data, g_file.len, mem_scratch(), doc) && !json_get(doc, doc.root(), "lines"));
}

void test_failures(TestCtx& c) {
    c.begin("kimc failures");
    KV_CHECK(c, put("bad.kim", "screen s {\n  box bg=#12 w=1x\n}\n"));
    KV_CHECK(c, kimc("compile", path("bad.kim"), path("bad.kimb"), "--as", "ui/bad.kim") == 1);
    KV_CHECK(c, has(g_err.data, "ui/bad.kim:2:10: a color is #rrggbb or #rrggbbaa\n") && has(g_err.data, "ui/bad.kim:2:16: not a number or a range\n"));
    KV_CHECK(c, !io_is_file(path("bad.kimb")) && !io_is_file(path("bad.kimb.kimc-tmp")));
    KV_CHECK(c, kimc("compile", path("missing.kim"), path("m.kimb")) == 1 && has(g_err.data, "cannot read"));
    KV_CHECK(c, kimc("compile", path("form.kim")) == 2);
    KV_CHECK(c, kimc("compile", "a", "b", "--frob") == 2);
    KV_CHECK(c, kimc("help") == 0 && has(g_out.data, "kimc compile"));
}

} // namespace

int main() {
    TestCtx c;
    if (!process_supported()) { KV_INFO("test", "kimc: no processes here; skipped"); return 0; }
    if (!io_make_temp_dir("kv-kimc", g_dir, sizeof g_dir)) { KV_ERROR("test", "no temporary directory"); return 1; }
    test_compile(c);
    test_failures(c);
    io_remove_tree(g_dir);
    g_out.free();
    g_err.free();
    g_file.free();
    mem_scratch_reset();
    names_shutdown();
    mem_shutdown();
    return test_report(c, "kimc_tests");
}
