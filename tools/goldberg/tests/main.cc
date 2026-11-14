// goldberg, end to end: trees written to a temporary directory, the built tool run over
// them as a command line runs it, and what it decided read back from a dump.
#include "korvid/core/array.h"
#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/test.h"
#include "korvid/platform/io.h"
#include "korvid/core/time.h"
#include "korvid/platform/module.h"
#include "korvid/platform/process.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {

using namespace kv;

struct Tree {
    char dir[512] = {};
    char out[600] = {};   // the last dump read, as text
    Array<char> text;

    bool make(const char* prefix) { return io_make_temp_dir(prefix, dir, sizeof dir); }
    bool put(const char* rel, const char* content) {
        char path[1024], parent[1024];
        snprintf(path, sizeof path, "%s/%s", dir, rel);
        snprintf(parent, sizeof parent, "%s", path);
        if (char* slash = strrchr(parent, '/')) { *slash = 0; io_make_dirs(parent); }
        return io_write_now(path, content, strlen(content));
    }
    const char* path(const char* rel) {
        snprintf(out, sizeof out, "%s/%s", dir, rel);
        return out;
    }
    // A file's text, valid until the next read.
    const char* read(const char* rel) {
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dir, rel);
        IoBytes b;
        text.len = 0;
        if (io_read_now(p, &b)) { memcpy(text.push_n((u32)b.size), b.data, b.size); b.free(); }
        text.push(0);
        return text.data;
    }
    void free() { io_remove_tree(dir); text.free(); }
};

// The goldberg beside this test, run with these words: its exit status, and what it
// wrote to both streams in g_out and g_err.
Array<char> g_out, g_err;
int gb_run(const char* const* words, u32 n) {
    char exe[700];
    if (!platform_exe_dir(exe, sizeof exe)) return -100;
    usize len = strlen(exe);
    snprintf(exe + len, sizeof exe - len, "goldberg");
    Array<const char*> argv{};
    argv.push(exe);
    for (u32 i = 0; i < n; ++i) argv.push(words[i]);
    ProcessDesc d;
    d.argv = Span<const char* const>(argv.data, argv.len);
    Process p;
    g_out.len = g_err.len = 0;
    if (!p.spawn(d)) { KV_ERROR("test", "cannot run %s: %s", exe, p.error); argv.free(); return -101; }
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
    argv.free();
    return code;
}
template <class... A>
int gb(A... words) {
    const char* const list[] = {words...};
    return gb_run(list, (u32)sizeof...(words));
}

bool has(const char* text, const char* line) { return strstr(text, line) != nullptr; }
u32 count(const char* text, const char* what) {
    u32 n = 0;
    for (const char* p = text; (p = strstr(p, what)); p += strlen(what)) ++n;
    return n;
}

void test_rules(TestCtx& c) {
    c.begin("goldberg rules");
    Tree t;
    KV_CHECK(c, t.make("kv-goldberg"));
    char root[600], dump[600], log[600], dump2[600];
    snprintf(root, sizeof root, "t=%s/assets", t.dir);
    snprintf(dump, sizeof dump, "%s/dump.txt", t.dir);
    snprintf(dump2, sizeof dump2, "%s/dump2.txt", t.dir);
    snprintf(log, sizeof log, "%s/log.jsonl", t.dir);

    t.put("assets/a.png", "a");
    t.put("assets/ui/hud.json", "{}");
    t.put("assets/ui/hud.png", "hud");
    t.put("assets/ships/big.png", "big");
    t.put("assets/ships/big.sheet", "sheet");
    t.put("assets/ships/ship.png", "ship");
    t.put("assets/notes/todo.txt", "todo");
    t.put("assets/weird.xyz", "?");
    t.put("assets/.hidden", "h");
    t.put("assets/ships/.swp", "s");
    t.put("assets/goldberg.berg", R"({
  // the module's rules
  "//": "a comment as a key",
  "meta": { "tags": ["base"] },
  "types": { "sheet": { "runtime": false } },
  "converters": {
    "cut": { "from": "sheet", "to": "texture", "shell": "tools/cut", "args": "{in} {work}", "outputs": ["{stem}_*.png"] }
  },
  "rules": [
    { "match": "*.sheet",  "meta": { "type": "sheet" } },
    { "match": "ui/**",    "meta": { "filter": "nearest", "tags": ["ui"] } },
    { "match": "notes/**", "meta": { "cook": false } },
    { "match": "*.png",    "meta": { "atlas": { "pad": 2, "group": "main" } } }
  ]
})");
    t.put("assets/ui/goldberg.berg", R"({ "rules": [ { "match": "*.png", "meta": { "filter": null, "atlas": { "pad": 4 }, "tags": ["-base", "hud"] } } ] })");
    t.put("assets/ships/ship.berg", R"({
  "meta": { "type": "texture.color.srgb", "pivot": [0.5, 0.8] },
  "rules": [ { "match": "ship.png", "meta": { "land": false } } ]
})");

    KV_CHECK(c, gb("cook", "--root", root, "--project", t.dir, "--dump", dump, "--log", log, "--watch", "none") == 0);
    const char* d = t.read("dump.txt");
    // Conventions, the root's rules, and the order they are laid in.
    KV_CHECK(c, has(d, "file t:a.png copy type=texture path=copy lands=yes out=t/a.png atlas={group=\"main\", pad=2} tags=base #"));
    KV_CHECK(c, has(d, "file t:ui/hud.json copy type=data path=copy lands=yes out=t/ui/hud.json filter=\"nearest\" tags=base,ui #"));
    // A nearer directory wins: null removes a key, an object merges key by key, tags are a set.
    KV_CHECK(c, has(d, "file t:ui/hud.png copy type=texture path=copy lands=yes out=t/ui/hud.png atlas={group=\"main\", pad=4} tags=ui,hud #"));
    // A converter in scope takes a type the game does not read to one it does.
    KV_CHECK(c, has(d, "file t:ships/big.sheet convert type=sheet path=cut lands=no tags=base #"));
    // A sidecar speaks for its stem; a rule in it narrows by full name; land overrides the type.
    KV_CHECK(c, has(d, "file t:ships/ship.png keep type=texture.color.srgb lands=no atlas={group=\"main\", pad=2} pivot=[0.5, 0.8] tags=base #"));
    KV_CHECK(c, has(d, "file t:ships/big.png copy type=texture path=copy lands=yes out=t/ships/big.png atlas="));
    KV_CHECK(c, has(d, "file t:notes/todo.txt ignore #"));
    // No type: the fallback copies it and warns.
    KV_CHECK(c, has(d, "file t:weird.xyz copy path=copy lands=yes out=t/weird.xyz tags=base warn #"));
    // Rule files are known and never cooked; hidden files are not seen at all.
    KV_CHECK(c, has(d, "file t:goldberg.berg rules #") && has(d, "file t:ships/ship.berg rules #") && !has(d, "hidden") && !has(d, ".swp"));
    // One settle, after every rule file was read: each file solved once.
    KV_CHECK(c, count(d, "\nsettle ") == 1 && has(d, "settle solved=8 changed=") && count(d, "request hash") == 11 && count(d, "+text") == 3);

    // The log replays to the same session with no file system under it.
    KV_CHECK(c, gb("replay", log, "--dump", dump2) == 0);
    {
        IoBytes a, b;
        KV_CHECK(c, io_read_now(dump, &a) && io_read_now(dump2, &b) && a.size == b.size && memcmp(a.data, b.data, a.size) == 0);
        a.free(); b.free();
    }
    // Moving the tree away changes nothing about the replay.
    char moved[600];
    snprintf(moved, sizeof moved, "%s/assets-gone", t.dir);
    KV_CHECK(c, rename(t.path("assets"), moved) == 0 && gb("replay", log, "--dump", dump2) == 0);
    {
        IoBytes a, b;
        KV_CHECK(c, io_read_now(dump, &a) && io_read_now(dump2, &b) && a.size == b.size && memcmp(a.data, b.data, a.size) == 0);
        a.free(); b.free();
    }
    KV_CHECK(c, rename(moved, t.path("assets")) == 0);

    // Rules change under a running session. Added to the log by hand: the ui directory's
    // rule file is saved with a different rule, then the sidecar is deleted.
    {
        IoBytes old;
        KV_CHECK(c, io_read_now(log, &old));
        Array<char> more{};
        memcpy(more.push_n((u32)old.size), old.data, old.size);
        old.free();
        const char* extra =
            "{\"kind\": \"file-changed\", \"root\": 1, \"path\": \"ui/goldberg.berg\"}\n"
            "{\"kind\": \"hashed\", \"root\": 1, \"path\": \"ui/goldberg.berg\", \"size\": 1, \"mtime\": 1, \"hash\": \"00000000000000000000000000000000000000000000000000000000000000aa\", "
            "\"text\": \"{ \\\"rules\\\": [ { \\\"match\\\": \\\"*.png\\\", \\\"meta\\\": { \\\"filter\\\": \\\"linear\\\" } } ] }\"}\n"
            "{\"kind\": \"settle\"}\n"
            "{\"kind\": \"file-removed\", \"root\": 1, \"path\": \"ships/ship.berg\"}\n"
            "{\"kind\": \"settle\"}\n"
            // The same text again, a different time: hashes decide, and nothing is solved.
            "{\"kind\": \"file-changed\", \"root\": 1, \"path\": \"goldberg.berg\"}\n";
        memcpy(more.push_n((u32)strlen(extra)), extra, strlen(extra));
        char log2[600];
        snprintf(log2, sizeof log2, "%s/log2.jsonl", t.dir);
        KV_CHECK(c, io_write_now(log2, more.data, more.len));
        more.free();
        KV_CHECK(c, gb("replay", log2, "--dump", dump2) == 1); // not stable: the last hash was never answered
        d = t.read("dump2.txt");
        // The directory's rule file re-solves exactly the files under it, and only the
        // one whose answer is different counts as changed.
        KV_CHECK(c, has(d, "settle solved=2 changed=t:ui/hud.png\n"));
        KV_CHECK(c, has(d, "file t:ui/hud.png copy type=texture path=copy lands=yes out=t/ui/hud.png atlas={group=\"main\", pad=2} filter=\"linear\" tags=base,ui #"));
        // The sidecar gone: its stem re-solved, and nothing else in the directory.
        KV_CHECK(c, has(d, "settle solved=1 changed=t:ships/ship.png\n") && !has(d, "ship.berg rules"));
        KV_CHECK(c, has(d, "file t:ships/ship.png copy type=texture path=copy lands=yes out=t/ships/ship.png atlas={group=\"main\", pad=2} tags=base #"));
        KV_CHECK(c, count(d, "\nsettle ") == 3);
    }

    // explain: the same answer, with who decided.
    char ex[600];
    snprintf(ex, sizeof ex, "%s/explain.txt", t.dir);
    KV_CHECK(c, gb("explain", t.path("assets/ui/hud.png"), "--root", root, "--project", t.dir, "--out", "OUT", "--dump", ex) == 0);
    d = t.read("explain.txt");
    KV_CHECK(c, has(d, "ui/hud.png\n") && has(d, "  type    texture                conventions: rule \"*.png\""));
    KV_CHECK(c, has(d, "/assets/ui/goldberg.berg: rule \"*.png\" (over ") && has(d, "/assets/goldberg.berg: rule \"*.png\")"));
    KV_CHECK(c, has(d, "  path    copy") && has(d, "  lands   yes                    texture is a runtime type") && has(d, "  output  OUT/t/ui/hud.png"));
    KV_CHECK(c, gb("explain", t.path("assets/ships/ship.png"), "--root", root, "--project", t.dir, "--dump", ex) == 0);
    d = t.read("explain.txt");
    KV_CHECK(c, has(d, "  type    texture.color.srgb") && has(d, "ship.berg: meta (over conventions: rule \"*.png\")") && has(d, "  lands   no") && has(d, "ship.berg: rule \"ship.png\""));
    KV_CHECK(c, gb("explain", t.path("assets/ships/big.sheet"), "--root", root, "--project", t.dir, "--dump", ex) == 0);
    d = t.read("explain.txt");
    KV_CHECK(c, has(d, "  path    cut") && has(d, "the only path from sheet to a runtime type") && has(d, "goldberg.berg: converter \"cut\""));
    KV_CHECK(c, gb("explain", t.path("assets/notes/todo.txt"), "--root", root, "--project", t.dir, "--dump", ex) == 0);
    KV_CHECK(c, has(t.read("explain.txt"), "  cook    no") && has(t.text.data, "rule \"notes/**\""));
    KV_CHECK(c, gb("explain", t.path("assets/nope.png"), "--root", root) == 1);

    t.free();
}

void test_mistakes(TestCtx& c) {
    c.begin("goldberg mistakes");
    Tree t;
    KV_CHECK(c, t.make("kv-goldberg-bad"));
    char root[600], dump[600];
    snprintf(root, sizeof root, "m=%s/assets", t.dir);
    snprintf(dump, sizeof dump, "%s/dump.txt", t.dir);
    t.put("assets/fine.png", "x");
    t.put("assets/bad/a.png", "x");
    t.put("assets/bad/deep/b.png", "x");
    t.put("assets/bad/goldberg.berg", "{\n  \"rules\": [\n  }\n");                 // does not parse
    t.put("assets/odd/c.png", "x");
    t.put("assets/odd/goldberg.berg", "{\n  \"meta\": {},\n  \"rule\": []\n}");     // an unknown key
    t.put("assets/two/d.sheet", "x");
    t.put("assets/two/goldberg.berg", R"({
  "types": { "sheet": {}, "frames": {} },
  "converters": {
    "cut":  { "from": "sheet", "to": "texture", "shell": "cut", "outputs": ["x.png"] },
    "hum":  { "from": "sheet", "to": "sound",   "shell": "hum", "outputs": "scan", "settle": 0.1 }
  },
  "rules": [
    { "match": "*.sheet",  "meta": { "type": "sheet" } },
    { "match": "*.frames", "meta": { "type": "frames" } },
    { "match": "*.what",   "meta": { "type": "mystery" } },
    { "match": "picked.sheet", "meta": { "to": "sound" } },
    { "match": "as-is.frames", "meta": { "land": true } }
  ]
})");
    t.put("assets/two/e.frames", "x");
    t.put("assets/two/f.what", "x");
    t.put("assets/two/picked.sheet", "x");
    t.put("assets/two/as-is.frames", "x");
    t.put("assets/conv/goldberg.berg", R"({ "converters": { "copy": { "from": "a", "to": "b", "shell": "x", "outputs": [] } } })");
    t.put("assets/conv/g.png", "x");

    KV_CHECK(c, gb("cook", "--root", root, "--project", t.dir, "--dump", dump, "--watch", "none") == 1);
    const char* d = t.read("dump.txt");
    // A rule file that does not parse fails what it speaks for, with the file and the line, and nothing else.
    KV_CHECK(c, has(d, "file m:fine.png copy type=texture path=copy lands=yes out=m/fine.png #"));
    KV_CHECK(c, has(d, "file m:bad/a.png error error=") && has(d, "/assets/bad/goldberg.berg:3: ") && has(d, "file m:bad/deep/b.png error error="));
    KV_CHECK(c, has(d, "file m:odd/c.png error error=") && has(d, "/assets/odd/goldberg.berg:3: unknown key \"rule\""));
    // Two ways and no way are errors that say what to write; a named target picks one.
    KV_CHECK(c, has(d, "file m:two/d.sheet error type=sheet error=more than one way to cook a sheet: cut (pick it with { \"to\": \"texture\" }") && has(d, "; hum (pick it with { \"to\": \"sound\" }"));
    KV_CHECK(c, has(d, "file m:two/picked.sheet convert type=sheet to=sound path=hum lands=no #"));
    KV_CHECK(c, has(d, "file m:two/e.frames error type=frames error=nothing cooks a frames: it is not a runtime type"));
    KV_CHECK(c, has(d, "file m:two/as-is.frames copy type=frames path=copy lands=yes out=m/two/as-is.frames #"));
    KV_CHECK(c, has(d, "file m:two/f.what error type=mystery error=the type \"mystery\" is not declared"));
    KV_CHECK(c, has(d, "file m:conv/g.png error error=") && has(d, "\"copy\" is built in"));

    // A root that is not there, and no roots at all, are refused before anything runs.
    KV_CHECK(c, gb("cook", "--root", "x=/no/such/dir/anywhere") == 2 && gb("cook") == 2 && gb("cook", "--frobnicate") == 2);
    t.free();
}

void test_cook(TestCtx& c) {
    c.begin("goldberg cook");
    Tree t;
    KV_CHECK(c, t.make("kv-goldberg-cook"));
    char root[600], out[600], dump[600], log[600], dump2[600];
    snprintf(root, sizeof root, "g=%s/assets", t.dir);
    snprintf(out, sizeof out, "%s/out", t.dir);
    snprintf(dump, sizeof dump, "%s/dump.txt", t.dir);
    snprintf(dump2, sizeof dump2, "%s/dump2.txt", t.dir);
    snprintf(log, sizeof log, "%s/log.jsonl", t.dir);
    t.put("assets/a.png", "the a");
    t.put("assets/ui/hud.json", "{ \"hud\": 1 }");
    t.put("assets/ui/big.bin", "0123456789");
    t.put("assets/notes/todo.txt", "todo");
    t.put("assets/goldberg.berg", R"({ "rules": [ { "match": "notes/**", "meta": { "cook": false } }, { "match": "*.bin", "meta": { "type": "data" } } ] })");

    // The first cook copies what lands, and nothing else: no rule file, nothing ignored.
    KV_CHECK(c, gb("cook", "game", "--root", root, "--project", t.dir, "--out", out, "--dump", dump, "--log", log, "--watch", "none") == 0);
    KV_CHECK(c, has(g_out.data, "goldberg: game: 4 files, 3 cooked, 0 clean, 0 failed"));
    KV_CHECK(c, strcmp(t.read("out/g/a.png"), "the a") == 0 && strcmp(t.read("out/g/ui/hud.json"), "{ \"hud\": 1 }") == 0 && strcmp(t.read("out/g/ui/big.bin"), "0123456789") == 0);
    KV_CHECK(c, !io_is_file(t.path("out/g/notes/todo.txt")) && !io_is_file(t.path("out/g/goldberg.berg")) && io_is_file(t.path("out/.goldberg")));
    KV_CHECK(c, io_is_file(t.path(".goldberg-state/out.json")));
    const char* d = t.read("dump.txt");
    KV_CHECK(c, has(d, "step g/a.png <- g:a.png clean") && has(d, "request copy g:a.png -> g/a.png") && count(d, "request copy") == 3 && count(d, "\nstep ") == 3);
    // No temporary file is left behind.
    KV_CHECK(c, !has(t.read("out/g/a.png"), "tmp"));

    // The log replays to the same graph and the same list of steps, with nothing under it.
    KV_CHECK(c, gb("replay", log, "--dump", dump2) == 0);
    {
        IoBytes a, b;
        KV_CHECK(c, io_read_now(dump, &a) && io_read_now(dump2, &b) && a.size == b.size && memcmp(a.data, b.data, a.size) == 0);
        a.free(); b.free();
    }

    // A second cook of an unchanged tree runs no step and reads no source.
    KV_CHECK(c, gb("cook", "game", "--root", root, "--project", t.dir, "--out", out, "--dump", dump, "--watch", "none") == 0);
    KV_CHECK(c, has(g_out.data, "goldberg: game: 4 files, 0 cooked, 3 clean, 0 failed"));
    d = t.read("dump.txt");
    KV_CHECK(c, count(d, "request copy") == 0 && count(d, "request hash") == 1 && has(d, "request hash g:goldberg.berg +text"));

    // One source changes: one step runs.
    t.put("assets/a.png", "the a, changed");
    KV_CHECK(c, gb("cook", "game", "--root", root, "--project", t.dir, "--out", out, "--dump", dump, "--watch", "none") == 0);
    KV_CHECK(c, has(g_out.data, "4 files, 1 cooked, 2 clean, 0 failed") && strcmp(t.read("out/g/a.png"), "the a, changed") == 0);
    // Written again with the same bytes: hashed, since its time moved, and not cooked.
    t.put("assets/a.png", "the a, changed");
    KV_CHECK(c, gb("cook", "game", "--root", root, "--project", t.dir, "--out", out, "--dump", dump, "--watch", "none") == 0);
    KV_CHECK(c, has(g_out.data, "4 files, 0 cooked, 3 clean, 0 failed") && has(t.read("dump.txt"), "request hash g:a.png"));

    // An output someone deleted, and one someone changed, are cooked again.
    KV_CHECK(c, io_remove(t.path("out/g/ui/hud.json")));
    t.put("out/g/ui/big.bin", "tampered with");
    KV_CHECK(c, gb("cook", "game", "--root", root, "--project", t.dir, "--out", out, "--watch", "none") == 0);
    KV_CHECK(c, has(g_out.data, "4 files, 2 cooked, 1 clean, 0 failed") && strcmp(t.read("out/g/ui/hud.json"), "{ \"hud\": 1 }") == 0 && strcmp(t.read("out/g/ui/big.bin"), "0123456789") == 0);

    // A source deleted while goldberg was not running: the next cook's scan of the output
    // tree removes what it made, and the directory it leaves empty. So with a file that
    // nothing here ever cooked.
    KV_CHECK(c, io_remove(t.path("assets/ui/hud.json")) && io_remove(t.path("assets/ui/big.bin")));
    t.put("out/g/stray/left.txt", "nobody's");
    KV_CHECK(c, gb("cook", "game", "--root", root, "--project", t.dir, "--out", out, "--watch", "none") == 0);
    KV_CHECK(c, has(g_out.data, "removed g/ui/hud.json (its source g:ui/hud.json is gone)") && has(g_out.data, "removed g/stray/left.txt (nothing here cooks to it)"));
    KV_CHECK(c, has(g_out.data, "2 files, 0 cooked, 1 clean, 0 failed, 3 removed"));
    KV_CHECK(c, !io_is_file(t.path("out/g/ui/hud.json")) && !io_is_dir(t.path("out/g/ui")) && !io_is_dir(t.path("out/g/stray")) && io_is_file(t.path("out/g/a.png")));

    // A rule change that stops a file cooking takes its output away; one that starts it, brings it.
    t.put("assets/goldberg.berg", R"({ "rules": [ { "match": "*.png", "meta": { "cook": false } } ] })");
    KV_CHECK(c, gb("cook", "game", "--root", root, "--project", t.dir, "--out", out, "--watch", "none") == 0);
    KV_CHECK(c, has(g_out.data, "2 files, 1 cooked, 0 clean, 0 failed, 1 removed") && !io_is_file(t.path("out/g/a.png")) && strcmp(t.read("out/g/notes/todo.txt"), "todo") == 0);
    // "out" moves a file's place in the tree: the old place is cleared.
    t.put("assets/goldberg.berg", R"({ "rules": [ { "match": "todo.txt", "meta": { "out": "docs/list.txt" } } ] })");
    KV_CHECK(c, gb("cook", "game", "--root", root, "--project", t.dir, "--out", out, "--watch", "none") == 0);
    KV_CHECK(c, strcmp(t.read("out/g/docs/list.txt"), "todo") == 0 && !io_is_file(t.path("out/g/notes/todo.txt")) && strcmp(t.read("out/g/a.png"), "the a, changed") == 0);

    // Two sources that cook to one place are an error naming both, and neither wins quietly.
    t.put("assets/b.png", "the b");
    t.put("assets/goldberg.berg", R"({ "rules": [ { "match": "b.png", "meta": { "out": "a.png" } } ] })");
    KV_CHECK(c, gb("cook", "game", "--root", root, "--project", t.dir, "--out", out, "--watch", "none") == 1);
    KV_CHECK(c, has(g_err.data, "g:a.png and g:b.png both cook to g/a.png") || has(g_err.data, "g:b.png and g:a.png both cook to g/a.png"));
    t.put("assets/goldberg.berg", "{}");
    KV_CHECK(c, gb("cook", "game", "--root", root, "--project", t.dir, "--out", out, "--watch", "none") == 0 && strcmp(t.read("out/g/b.png"), "the b") == 0);

    // The state deleted: always safe. Everything is cooked again, nothing is lost.
    KV_CHECK(c, io_remove(t.path(".goldberg-state/out.json")));
    KV_CHECK(c, gb("cook", "game", "--root", root, "--project", t.dir, "--out", out, "--watch", "none") == 0);
    KV_CHECK(c, has(g_out.data, "3 files, 3 cooked, 0 clean, 0 failed") && strcmp(t.read("out/g/a.png"), "the a, changed") == 0);
    // And a state that is not one: the same.
    t.put(".goldberg-state/out.json", "not json at all");
    KV_CHECK(c, gb("cook", "game", "--root", root, "--project", t.dir, "--out", out, "--watch", "none") == 0 && has(g_out.data, "3 files, 3 cooked"));

    // A source that cannot be read fails its own step and no other.
    KV_CHECK(c, io_make_dirs(t.path("assets/dir.png")));   // a directory with a file's name is not a file: not seen
    KV_CHECK(c, gb("cook", "game", "--root", root, "--project", t.dir, "--out", out, "--watch", "none") == 0);

    // The output tree is only ever a directory that is goldberg's, new or empty.
    t.put("precious/thesis.txt", "years of work");
    char precious[600];
    snprintf(precious, sizeof precious, "%s/precious", t.dir);
    KV_CHECK(c, gb("cook", "game", "--root", root, "--project", t.dir, "--out", precious, "--watch", "none") == 2);
    KV_CHECK(c, has(g_err.data, "is not a goldberg output tree") && strcmp(t.read("precious/thesis.txt"), "years of work") == 0);
    KV_CHECK(c, gb("clean", "--out", precious) == 2 && io_is_file(t.path("precious/thesis.txt")));

    // clean removes the tree and the state.
    KV_CHECK(c, gb("clean", "--out", out) == 0 && !io_is_dir(out) && !io_is_file(t.path(".goldberg-state/out.json")));
    KV_CHECK(c, gb("clean", "--out", out) == 0); // nothing there: nothing to do

    // With both watchers: the same cook.
    KV_CHECK(c, gb("cook", "game", "--root", root, "--project", t.dir, "--out", out, "--watch", "poll") == 0 && has(g_out.data, "3 files, 3 cooked"));
    KV_CHECK(c, gb("cook", "game", "--root", root, "--project", t.dir, "--out", out) == 0 && has(g_out.data, "3 files, 0 cooked, 3 clean"));

    t.free();
}

} // namespace

int main() {
    TestCtx c;
    test_rules(c);
    test_mistakes(c);
    test_cook(c);
    g_out.free();
    g_err.free();
    names_shutdown();
    mem_shutdown();
    return test_report(c, "goldberg_tests");
}
