// kbor, end to end: files written to a temporary directory, the built tool run over them
// as a command line runs it, and what it wrote read back. The format's own tests are
// core's (modules/core/tests/kbor_test.cc); these are the tool's: its verbs, its exit
// status, its output going to a file or stdout, and normalizing in place.
#include "korvid/core/array.h"
#include "korvid/core/json.h"
#include "korvid/core/kbor.h"
#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
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

// The kbor beside this test, run with these words: its exit status, and its two streams.
int run_words(const char* const* words, u32 n) {
    char exe[700];
    if (!platform_exe_dir(exe, sizeof exe)) return -100;
    usize len = strlen(exe);
    snprintf(exe + len, sizeof exe - len, "kbor");
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
int kbor(A... words) {
    const char* const list[] = {words...};
    return run_words(list, (u32)sizeof...(words));
}

bool has(const char* text, const char* what) {
    if (strstr(text, what)) return true;
    KV_ERROR("test", "'%s' not in:\n%s", what, text);
    return false;
}

const char* k_json = R"({"name": "level", "objects": [{"kind": "rock", "pos": [1, 2.5]}, {"kind": "ship", "pos": [-3, 0.0]}], "blob": null})";
const char* k_pretty =
    "{\n"
    "  \"name\": \"level\",\n"
    "  \"objects\": [\n"
    "    {\n"
    "      \"kind\": \"rock\",\n"
    "      \"pos\": [1, 2.5]\n"
    "    },\n"
    "    {\n"
    "      \"kind\": \"ship\",\n"
    "      \"pos\": [-3, 0.0]\n"
    "    }\n"
    "  ],\n"
    "  \"blob\": null\n"
    "}\n";

void test_convert(TestCtx& c) {
    c.begin("kbor convert");
    KV_CHECK(c, put("level.json", k_json));
    KV_CHECK(c, kbor("encode", path("level.json"), path("level.kbor")) == 0 && has(g_out.data, "level.kbor: "));
    KV_CHECK(c, read("level.kbor") && g_file.len > 3 && g_file.data[0] == 0xD9 && g_file.data[1] == 0xCB && g_file.data[2] == 0x0B);
    KV_CHECK(c, !io_is_file(path("level.kbor.kbor-tmp"))); // renamed into place

    // What was written is what core's reader makes of it, and the JSON back is the same tree.
    JsonDoc doc;
    KborInfo info;
    KV_CHECK(c, kbor_parse(g_file.data, g_file.len, mem_scratch(), doc, &info) && info.summarized && info.index_keys == 2);

    // To stdout, pretty unless told; to a file.
    KV_CHECK(c, kbor("decode", path("level.kbor")) == 0 && strcmp(g_out.data, k_pretty) == 0);
    KV_CHECK(c, kbor("decode", path("level.kbor"), "--compact") == 0 &&
                strcmp(g_out.data, R"({"name":"level","objects":[{"kind":"rock","pos":[1,2.5]},{"kind":"ship","pos":[-3,0.0]}],"blob":null})") == 0);
    KV_CHECK(c, kbor("decode", path("level.kbor"), path("back.json")) == 0);
    KV_CHECK(c, read("back.json") && g_file.len == strlen(k_pretty) && memcmp(g_file.data, k_pretty, g_file.len) == 0);

    KV_CHECK(c, kbor("info", path("level.kbor")) == 0 && has(g_out.data, "normalized (summary)") && has(g_out.data, "index keys 2, distinct names 5"));
}

void test_normalize(TestCtx& c) {
    c.begin("kbor normalize");
    // A stock encoder's file: indefinite lengths, a name in full every time.
    const u8 stock[] = {0x9F, 0xA2, 0x64, 'n', 'a', 'm', 'e', 0x61, 'a', 0x62, 'h', 'p', 0x01,
                        0xBF, 0x64, 'n', 'a', 'm', 'e', 0x61, 'b', 0x62, 'h', 'p', 0x02, 0xFF, 0xFF};
    const u8 tight[] = {0xD9, 0xCB, 0x0B, 0x82, 0x84, 0x07, 0x02, 0x02, 0x08,
                        0x82, 0xA2, 0x64, 'n', 'a', 'm', 'e', 0x61, 'a', 0x62, 'h', 'p', 0x01, 0xA2, 0x00, 0x61, 'b', 0x01, 0x02};
    KV_CHECK(c, put("stock.kbor", stock, sizeof stock));
    KV_CHECK(c, kbor("info", path("stock.kbor")) == 0 && has(g_out.data, "not normalized (no summary), indefinite lengths"));
    KV_CHECK(c, kbor("normalize", path("stock.kbor"), path("tight.kbor")) == 0);
    KV_CHECK(c, read("tight.kbor") && g_file.len == sizeof tight && memcmp(g_file.data, tight, sizeof tight) == 0);
    KV_CHECK(c, read("stock.kbor") && g_file.len == sizeof stock); // the input is left alone

    // In place.
    KV_CHECK(c, kbor("normalize", path("stock.kbor")) == 0);
    KV_CHECK(c, read("stock.kbor") && g_file.len == sizeof tight && memcmp(g_file.data, tight, sizeof tight) == 0);
    KV_CHECK(c, !io_is_file(path("stock.kbor.kbor-tmp")));
    // Already normalized: nothing to write.
    IoStat before{}, after{};
    KV_CHECK(c, io_stat(path("stock.kbor"), &before));
    KV_CHECK(c, kbor("normalize", path("stock.kbor")) == 0 && has(g_out.data, "already normalized"));
    KV_CHECK(c, io_stat(path("stock.kbor"), &after) && memcmp(&before, &after, sizeof before) == 0);
}

void test_failures(TestCtx& c) {
    c.begin("kbor failures");
    KV_CHECK(c, put("bad.json", "{\"a\": [1, 2,]}"));
    KV_CHECK(c, kbor("encode", path("bad.json"), path("bad.kbor")) == 1 && has(g_err.data, "trailing comma"));
    KV_CHECK(c, !io_is_file(path("bad.kbor")) && !io_is_file(path("bad.kbor.kbor-tmp")));
    const u8 bad[] = {0xA1, 0x05, 0x01}; // an index into an empty table
    KV_CHECK(c, put("bad.kbor", bad, sizeof bad));
    KV_CHECK(c, kbor("decode", path("bad.kbor")) == 1 && has(g_err.data, "a key index past the table at byte 1"));
    KV_CHECK(c, kbor("normalize", path("bad.kbor")) == 1 && read("bad.kbor") && g_file.len == sizeof bad); // untouched
    KV_CHECK(c, kbor("info", path("missing.kbor")) == 1 && has(g_err.data, "cannot read"));
    KV_CHECK(c, kbor("encode", path("level.json")) == 2);           // no output
    KV_CHECK(c, kbor("decode", "a", "b", "c") == 2);                // too many
    KV_CHECK(c, kbor("normalize", "a", "--compact") == 2);          // not its option
    KV_CHECK(c, kbor("help") == 0 && has(g_out.data, "kbor normalize"));
}

} // namespace

int main() {
    TestCtx c;
    if (!process_supported()) { KV_INFO("test", "kbor: no processes here; skipped"); return 0; }
    if (!io_make_temp_dir("kv-kbor", g_dir, sizeof g_dir)) { KV_ERROR("test", "no temporary directory"); return 1; }
    test_convert(c);
    test_normalize(c);
    test_failures(c);
    io_remove_tree(g_dir);
    g_out.free();
    g_err.free();
    g_file.free();
    mem_scratch_reset();
    names_shutdown();
    mem_shutdown();
    return test_report(c, "kbor_tests");
}
