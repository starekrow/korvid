// goldberg, end to end: trees written to a temporary directory, the built tool run over
// them as a command line runs it, and what it decided read back from a dump.
#include "korvid/core/array.h"
#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/test.h"
#include "korvid/platform/io.h"
#include "korvid/core/time.h"
#include "korvid/platform/module.h"
#include "korvid/platform/net.h"
#include "korvid/platform/process.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {

using namespace kv;

struct Tree {
    char dir[512] = {};
    char out[1200] = {};  // the last path asked for
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
        if (snprintf(out, sizeof out, "%s/%s", dir, rel) >= (int)sizeof out) out[0] = 0;
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

    // A second cook of an unchanged tree runs no step and reads no source, and leaves the
    // state as it found it: not written again.
    IoStat before{}, after{};
    KV_CHECK(c, io_stat(t.path(".goldberg-state/out.json"), &before));
    KV_CHECK(c, gb("cook", "game", "--root", root, "--project", t.dir, "--out", out, "--dump", dump, "--watch", "none") == 0);
    KV_CHECK(c, has(g_out.data, "goldberg: game: 4 files, 0 cooked, 3 clean, 0 failed"));
    d = t.read("dump.txt");
    KV_CHECK(c, count(d, "request copy") == 0 && count(d, "request hash") == 1 && has(d, "request hash g:goldberg.berg +text"));
    KV_CHECK(c, io_stat(t.path(".goldberg-state/out.json"), &after) && after.mtime_ns == before.mtime_ns && after.size == before.size);

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
    KV_CHECK(c, has(g_out.data, "moved g/ui/hud.json to the attic (its source g:ui/hud.json is gone)") && has(g_out.data, "moved g/stray/left.txt to the attic (nothing here cooks to it)"));
    KV_CHECK(c, has(g_out.data, "2 files, 0 cooked, 1 clean, 0 failed, 3 to the attic"));
    // Nothing cooked, and the state is written: it no longer knows the files that went.
    KV_CHECK(c, !has(t.read(".goldberg-state/out.json"), "g:ui/hud.json") && has(t.read(".goldberg-state/out.json"), "g:a.png"));
    KV_CHECK(c, !io_is_file(t.path("out/g/ui/hud.json")) && !io_is_dir(t.path("out/g/ui")) && !io_is_dir(t.path("out/g/stray")) && io_is_file(t.path("out/g/a.png")));
    // Nothing was deleted: each file is in the attic, under this run's folder, at its own
    // path. A cook says so on its way out, every time, until someone empties it.
    KV_CHECK(c, has(g_err.data, "goldberg found files in the attic: ") && has(g_err.data, "/out/.goldberg.attic"));
    {
        char attic[700];
        snprintf(attic, sizeof attic, "%s/out/.goldberg.attic", t.dir);
        struct Found { u32 n; bool hud, bin, stray; char first[300]; } found{};
        io_list_files(attic, [](void* user, const char* rel) {
            Found* f = (Found*)user;
            if (!f->n++) snprintf(f->first, sizeof f->first, "%s", rel);
            if (strstr(rel, "/g/ui/hud.json")) f->hud = true;
            if (strstr(rel, "/g/ui/big.bin")) f->bin = true;
            if (strstr(rel, "/g/stray/left.txt")) f->stray = true;
            return true;
        }, &found);
        KV_CHECK(c, found.n == 3 && found.hud && found.bin && found.stray);
        char rel[900];
        snprintf(rel, sizeof rel, "out/.goldberg.attic/%s", found.first);
        KV_CHECK(c, found.n && strlen(t.read(rel)) > 0);
    }
    // The attic is not part of the tree: the next cook leaves it alone and still says it is there.
    KV_CHECK(c, gb("cook", "game", "--root", root, "--project", t.dir, "--out", out, "--watch", "none") == 0);
    KV_CHECK(c, has(g_out.data, "2 files, 0 cooked, 1 clean, 0 failed (") && has(g_err.data, "goldberg found files in the attic"));
    KV_CHECK(c, gb("clean", "attic", "--out", out) == 0 && has(g_out.data, "emptied the attic") && !io_is_dir(t.path("out/.goldberg.attic")) && io_is_file(t.path("out/g/a.png")));
    KV_CHECK(c, gb("cook", "game", "--root", root, "--project", t.dir, "--out", out, "--watch", "none") == 0 && !has(g_err.data, "attic"));
    KV_CHECK(c, gb("clean", "attic", "--out", out) == 0 && has(g_out.data, "has no attic"));

    // A rule change that stops a file cooking takes its output away; one that starts it, brings it.
    t.put("assets/goldberg.berg", R"({ "rules": [ { "match": "*.png", "meta": { "cook": false } } ] })");
    KV_CHECK(c, gb("cook", "game", "--root", root, "--project", t.dir, "--out", out, "--watch", "none") == 0);
    KV_CHECK(c, has(g_out.data, "2 files, 1 cooked, 0 clean, 0 failed, 1 to the attic") && !io_is_file(t.path("out/g/a.png")) && strcmp(t.read("out/g/notes/todo.txt"), "todo") == 0);
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

// Shell converters: a script per step, a chain of two, and the script that streams.
void test_scripts(TestCtx& c) {
    c.begin("goldberg scripts");
    if (!process_supported()) return;
    Tree t;
    KV_CHECK(c, t.make("kv-goldberg-sh"));
    char root[600], out[600], dump[600], log[600], dump2[600];
    snprintf(root, sizeof root, "g=%s/assets", t.dir);
    snprintf(out, sizeof out, "%s/out", t.dir);
    snprintf(dump, sizeof dump, "%s/dump.txt", t.dir);
    snprintf(dump2, sizeof dump2, "%s/dump2.txt", t.dir);
    snprintf(log, sizeof log, "%s/log.jsonl", t.dir);
    const char* berg = R"({
  "types": { "sheet": { "runtime": false }, "frames": { "runtime": false } },
  "converters": {
    "cut":  { "from": "sheet",  "to": "frames",  "shell": "tools/cut",  "outputs": ["{stem}.frames"] },
    "pack": { "from": "frames", "to": "texture", "shell": "tools/pack", "args": "{in} {work} {tint}", "outputs": ["{stem}.png"] }
  },
  "rules": [
    { "match": "*.sheet",  "meta": { "type": "sheet" } },
    { "match": "*.frames", "meta": { "tint": "red" } }
  ]
})";
    t.put("assets/goldberg.berg", berg);
    t.put("assets/tools/cut.sh", "tr a-z A-Z < \"$1\" > \"$2/$GOLDBERG_STEM.frames\"\n");
    t.put("assets/tools/pack.sh", "{ echo \"packed $3 $GOLDBERG_META_TINT\"; cat \"$1\"; } > \"$2/$GOLDBERG_STEM.png\"\n");
    t.put("assets/ships/big.sheet", "big sheet\n");
    t.put("assets/a.png", "plain");
#define COOK(...) gb("cook", "game", "--root", root, "--project", t.dir, "--out", out, "--watch", "none" __VA_OPT__(,) __VA_ARGS__)

    // A chain of two cooks in order and lands only what the last one made, beside the source.
    KV_CHECK(c, COOK("--dump", dump, "--log", log) == 0);
    KV_CHECK(c, has(g_out.data, "goldberg: game: 4 files, 4 cooked, 0 clean, 0 failed"));
    KV_CHECK(c, strcmp(t.read("out/g/ships/big.png"), "packed red red\nBIG SHEET\n") == 0 && strcmp(t.read("out/g/a.png"), "plain") == 0);
    KV_CHECK(c, !io_is_file(t.path("out/g/ships/big.frames")) && !io_is_file(t.path("out/g/ships/big.sheet")) && !io_is_dir(t.path("out/g/tools")));
    const char* d = t.read("dump.txt");
    KV_CHECK(c, has(d, "file g:ships/big.sheet convert type=sheet path=cut>pack lands=no #") && has(d, "file g:tools/cut.sh ignore #"));
    KV_CHECK(c, has(d, "/big.frames convert type=frames path=pack lands=no tint=\"red\" #") && has(d, "/big.png copy type=texture path=copy lands=yes out=g/ships/big.png #"));
    {
        const char* cut = strstr(d, "request run g:ships/big.sheet cut");
        const char* pack = cut ? strstr(cut, "big.frames pack") : nullptr;
        const char* land = pack ? strstr(pack, "-> g/ships/big.png") : nullptr;
        KV_CHECK(c, cut && pack && land && count(d, "request run ") == 2);
    }
    // The log replays to the same graph, with no script run.
    KV_CHECK(c, gb("replay", log, "--dump", dump2) == 0);
    {
        IoBytes a, b;
        KV_CHECK(c, io_read_now(dump, &a) && io_read_now(dump2, &b) && a.size == b.size && memcmp(a.data, b.data, a.size) == 0);
        a.free(); b.free();
    }
    // Nothing changed: nothing runs.
    KV_CHECK(c, COOK("--dump", dump) == 0 && has(g_out.data, "4 files, 0 cooked, 4 clean, 0 failed") && count(t.read("dump.txt"), "request run ") == 0);

    // The second script edited: its step runs again, the first does not.
    t.put("assets/tools/pack.sh", "{ echo \"PACKED $3\"; cat \"$1\"; } > \"$2/$GOLDBERG_STEM.png\"\n");
    KV_CHECK(c, COOK("--dump", dump) == 0);
    d = t.read("dump.txt");
    KV_CHECK(c, count(d, "request run ") == 1 && has(d, "big.frames pack") && strcmp(t.read("out/g/ships/big.png"), "PACKED red\nBIG SHEET\n") == 0);
    // The first script edited: both run, since what it makes changed.
    t.put("assets/tools/cut.sh", "tr a-z A-Z < \"$1\" | tr ' ' '_' > \"$2/$GOLDBERG_STEM.frames\"\n");
    KV_CHECK(c, COOK("--dump", dump) == 0 && count(t.read("dump.txt"), "request run ") == 2 && strcmp(t.read("out/g/ships/big.png"), "PACKED red\nBIG_SHEET\n") == 0);
    // Edited without changing what it makes: it runs, and nothing after it does.
    t.put("assets/tools/cut.sh", "# the same\ntr a-z A-Z < \"$1\" | tr ' ' '_' > \"$2/$GOLDBERG_STEM.frames\"\n");
    KV_CHECK(c, COOK("--dump", dump) == 0 && count(t.read("dump.txt"), "request run ") == 1 && has(g_out.data, "1 cooked, 3 clean"));
    // The source changes, and so does a parameter a rule gives the second step.
    t.put("assets/ships/big.sheet", "bigger\n");
    KV_CHECK(c, COOK() == 0 && strcmp(t.read("out/g/ships/big.png"), "PACKED red\nBIGGER\n") == 0);
    t.put("assets/ships/goldberg.berg", R"({ "rules": [ { "match": "*.frames", "meta": { "tint": "blue" } } ] })");
    KV_CHECK(c, COOK("--dump", dump) == 0 && count(t.read("dump.txt"), "request run ") == 1 && strcmp(t.read("out/g/ships/big.png"), "PACKED blue\nBIGGER\n") == 0);

    // A script that fails fails its step, with what it said; what was cooked before stays.
    t.put("assets/tools/pack.sh", "echo 'no ink left' >&2\nexit 3\n");
    KV_CHECK(c, COOK() == 1);
    KV_CHECK(c, has(g_err.data, "big.frames: the converter \"pack\" failed (exit 3): no ink left") && strcmp(t.read("out/g/ships/big.png"), "PACKED blue\nBIGGER\n") == 0);
    // A declared output it did not write is a failure that names it; one it wrote and did
    // not declare is taken in, with a line.
    t.put("assets/tools/pack.sh", "echo x > \"$2/other.png\"\n");
    KV_CHECK(c, COOK() == 1 && has(g_err.data, "the converter \"pack\" did not write \"big.png\", which it declares"));
    t.put("assets/tools/pack.sh", "echo x > \"$2/$GOLDBERG_STEM.png\"\necho y > \"$2/extra.png\"\n");
    KV_CHECK(c, COOK() == 0 && has(g_out.data, "the converter \"pack\" wrote extra.png, which it does not declare"));
    KV_CHECK(c, strcmp(t.read("out/g/ships/extra.png"), "y\n") == 0 && strcmp(t.read("out/g/ships/big.png"), "x\n") == 0);
    // It stops writing the extra: that output leaves (for the attic), the other stays.
    t.put("assets/tools/pack.sh", "echo x > \"$2/$GOLDBERG_STEM.png\"\n");
    KV_CHECK(c, COOK() == 0 && !io_is_file(t.path("out/g/ships/extra.png")) && io_is_file(t.path("out/g/ships/big.png")) && has(g_out.data, "moved g/ships/extra.png to the attic"));
    // No script, and a name the command line cannot fill in: each says which.
    KV_CHECK(c, io_remove(t.path("assets/tools/pack.sh")));
    KV_CHECK(c, COOK() == 1 && has(g_err.data, "the converter \"pack\" runs tools/pack.sh, which is not there"));
    t.put("assets/tools/pack.sh", "echo x > \"$2/$GOLDBERG_STEM.png\"\n");
    t.put("assets/ships/goldberg.berg", R"({ "rules": [ { "match": "*.frames", "meta": { "tint": null } } ] })");
    KV_CHECK(c, COOK() == 1 && has(g_err.data, "in \"args\", {tint} is not {in}, {work}, {name}, {stem} or a key of the asset's metadata"));
    t.put("assets/ships/goldberg.berg", R"({ "rules": [ { "match": "*.frames", "meta": { "tint": "blue" } } ] })");
    KV_CHECK(c, COOK() == 0);
    // The source goes: so does everything made from it, down to the output tree.
    KV_CHECK(c, io_remove(t.path("assets/ships/big.sheet")));
    KV_CHECK(c, COOK("--dump", dump) == 0 && !io_is_file(t.path("out/g/ships/big.png")) && io_is_file(t.path("out/g/a.png")));
    KV_CHECK(c, !has(t.read("dump.txt"), "(work)") || !has(t.read("dump.txt"), "file (work)"));
    KV_CHECK(c, COOK("--dump", dump) == 0 && !has(t.read("dump.txt"), "file (work)") && has(g_out.data, "0 cooked"));

    // The streaming converter: it makes a file, sleeps, writes it, sleeps, writes another,
    // sleeps, and ends. Each file is taken once it has been quiet, what follows it is
    // cooked while the script still runs, and the cook does not end before the script.
    t.put("assets/goldberg.berg", R"({
  "types": { "level": { "runtime": false } },
  "converters": { "hum": { "from": "level", "to": "map", "shell": "tools/hum", "outputs": "scan", "settle": 0.25 } },
  "rules": [ { "match": "*.level", "meta": { "type": "level" } } ]
})");
    io_remove(t.path("assets/ships/goldberg.berg"));
    t.put("assets/tools/hum.sh", ": > \"$2/one.tmj\"\nsleep 0.1\necho first >> \"$2/one.tmj\"\nsleep 1.2\necho second > \"$2/two.tmj\"\nsleep 0.4\n");
    t.put("assets/world.level", "a level");
    for (int pass = 0; pass < 2; ++pass) {
        const char* watch = pass ? "poll" : "auto";
        if (pass) { KV_CHECK(c, gb("clean", "--out", out) == 0); }
        f64 t0 = time_now();
        KV_CHECK(c, gb("cook", "game", "--root", root, "--project", t.dir, "--out", out, "--watch", watch, "--dump", dump, "--log", log) == 0);
        f64 took = time_now() - t0;
        KV_CHECK(c, took >= 1.65 && strcmp(t.read("out/g/one.tmj"), "first\n") == 0 && strcmp(t.read("out/g/two.tmj"), "second\n") == 0);
        d = t.read("dump.txt");
        KV_CHECK(c, has(d, "/one.tmj copy type=map path=copy lands=yes out=g/one.tmj #") && count(d, "/one.tmj -> g/one.tmj") == 1);
        const char* l = t.read("log.jsonl");
        const char* one = nullptr;
        for (const char* p = l; (p = strstr(p, "\"kind\": \"cook-done\"")); ++p) {
            const char* end = strchr(p, '\n');
            const char* hit = strstr(p, "g/one.tmj");
            if (hit && end && hit < end) { one = p; break; }
        }
        const char* exited = strstr(l, "\"kind\": \"step-exit\"");
        KV_CHECK(c, one && exited && one < exited);
    }
    // Run again with nothing changed: the script does not run.
    KV_CHECK(c, COOK("--dump", dump) == 0 && count(t.read("dump.txt"), "request run ") == 0);
    // It fails after delivering: what it delivered is taken back, with what was cooked from it.
    t.put("assets/tools/hum.sh", "echo early > \"$2/one.tmj\"\nsleep 0.6\necho 'the hum stopped' >&2\nexit 1\n");
    KV_CHECK(c, gb("cook", "game", "--root", root, "--project", t.dir, "--out", out) == 1);
    KV_CHECK(c, has(g_err.data, "the converter \"hum\" failed (exit 1): the hum stopped") && !io_is_file(t.path("out/g/one.tmj")) && !io_is_file(t.path("out/g/two.tmj")));
#undef COOK
    t.free();
}

// goldberg learn: the converter run once, what it wrote turned into a declaration, and
// that written into the .berg file with the rest of the file untouched.
void test_learn(TestCtx& c) {
    c.begin("goldberg learn");
    if (!process_supported()) return;
    Tree t;
    KV_CHECK(c, t.make("kv-goldberg-learn"));
    char root[600], out[600], sheet[600], level[600], png[600];
    snprintf(root, sizeof root, "g=%s/assets", t.dir);
    snprintf(out, sizeof out, "%s/out", t.dir);
    snprintf(sheet, sizeof sheet, "%s/assets/ships/big.sheet", t.dir);
    snprintf(level, sizeof level, "%s/assets/world.level", t.dir);
    snprintf(png, sizeof png, "%s/assets/a.png", t.dir);
    const char* berg = R"({
  // the module's rules, with a comment that must survive
  "types": { "sheet": { "runtime": false }, "level": {} },
  "converters": {
    "cut": {
      "from": "sheet",   // what it takes
      "to": "texture",
      "shell": "tools/cut"
    },
    "hum": { "from": "level", "to": "map", "shell": "tools/hum", "outputs": ["wrong.tmj"] }
  },
  "rules": [
    { "match": "*.sheet", "meta": { "type": "sheet" } },
    { "match": "*.level", "meta": { "type": "level" } }
  ]
})";
    t.put("assets/goldberg.berg", berg);
    t.put("assets/tools/cut.sh", "for i in 0 1 2; do echo \"frame $i\" > \"$2/${GOLDBERG_STEM}_$i.png\"; done\necho '{}' > \"$2/$GOLDBERG_STEM.atlas.json\"\n");
    t.put("assets/tools/hum.sh", "echo a > \"$2/one.tmj\"\nsleep 0.3\necho b >> \"$2/one.tmj\"\nsleep 0.7\necho c > \"$2/two.tmj\"\n");
    t.put("assets/ships/big.sheet", "sheet");
    t.put("assets/world.level", "level");
    t.put("assets/a.png", "png");

    // With no outputs declared a cook takes what the script writes, and says what to run.
    KV_CHECK(c, gb("cook", "game", "--root", root, "--project", t.dir, "--out", out, "--watch", "none") == 1); // hum's declaration is wrong
    KV_CHECK(c, has(g_out.data, "the converter \"cut\" declares no outputs; it wrote 4 (big.atlas.json, big_0.png, big_1.png, big_2.png). `goldberg learn`"));
    KV_CHECK(c, has(g_err.data, "the converter \"hum\" did not write \"wrong.tmj\", which it declares"));

    // --dry-run says what it would write and writes nothing.
    KV_CHECK(c, gb("learn", sheet, "--root", root, "--dry-run") == 0);
    KV_CHECK(c, has(g_out.data, "\"cut\" on ships/big.sheet wrote 4 files") && has(g_out.data, "would write into") && strcmp(t.read("assets/goldberg.berg"), berg) == 0);
    KV_CHECK(c, has(g_out.data, "big.atlas.json   (by its name a data; as an output of \"cut\" it is a texture)"));
    // For real: one member added, on a line of its own like its neighbours; every other byte as it was.
    KV_CHECK(c, gb("learn", sheet, "--root", root) == 0);
    KV_CHECK(c, has(g_out.data, "converter \"cut\": \"outputs\": [\"{stem}.atlas.json\", \"{stem}_*.png\"] (it declared none)"));
    const char* b = t.read("assets/goldberg.berg");
    KV_CHECK(c, has(b, "      \"shell\": \"tools/cut\",\n      \"outputs\": [\"{stem}.atlas.json\", \"{stem}_*.png\"]\n    },"));
    KV_CHECK(c, has(b, "// the module's rules, with a comment that must survive") && has(b, "\"from\": \"sheet\",   // what it takes") && has(b, "\"outputs\": [\"wrong.tmj\"] }"));
    KV_CHECK(c, strlen(b) == strlen(berg) + strlen(",\n      \"outputs\": [\"{stem}.atlas.json\", \"{stem}_*.png\"]"));
    // Again: the declaration holds, and nothing is written.
    KV_CHECK(c, gb("learn", sheet, "--root", root) == 0 && has(g_out.data, "already declares this") && has(g_out.data, "Nothing was changed"));

    // A wrong declaration is replaced, in place, on its one line. The script takes a
    // second over two files, and learn says what that could mean.
    KV_CHECK(c, gb("learn", level, "--root", root) == 0);
    KV_CHECK(c, has(g_out.data, "converter \"hum\": \"outputs\": [\"one.tmj\", \"two.tmj\"] (it declared [\"wrong.tmj\"])") && has(g_out.data, "`goldberg learn --scan`"));
    KV_CHECK(c, has(t.read("assets/goldberg.berg"), "\"hum\": { \"from\": \"level\", \"to\": \"map\", \"shell\": \"tools/hum\", \"outputs\": [\"one.tmj\", \"two.tmj\"] }"));
    // The cook now has nothing to say about either.
    KV_CHECK(c, gb("cook", "game", "--root", root, "--project", t.dir, "--out", out, "--watch", "none") == 0 && !has(g_out.data, "declare") && io_is_file(t.path("out/g/ships/big_2.png")) && io_is_file(t.path("out/g/two.tmj")));
    // --scan declares it as a streaming converter, with a settle time from what was seen.
    KV_CHECK(c, gb("learn", level, "--root", root, "--scan") == 0 && has(g_out.data, "\"outputs\": \"scan\", \"settle\": "));
    b = t.read("assets/goldberg.berg");
    KV_CHECK(c, has(b, "\"shell\": \"tools/hum\", \"outputs\": \"scan\", \"settle\": 0.") && has(b, "\"outputs\": [\"{stem}.atlas.json\", \"{stem}_*.png\"]\n"));
    KV_CHECK(c, gb("cook", "game", "--root", root, "--project", t.dir, "--out", out) == 0 && strcmp(t.read("out/g/one.tmj"), "a\nb\n") == 0);

    // A script that fails, writes nothing, or writes into the source tree: said, and nothing changed.
    char before[2000];
    snprintf(before, sizeof before, "%s", t.read("assets/goldberg.berg"));
    t.put("assets/tools/cut.sh", "echo 'blunt knife' >&2\nexit 4\n");
    KV_CHECK(c, gb("learn", sheet, "--root", root) == 1 && has(g_err.data, "the converter \"cut\" failed (exit 4) on ships/big.sheet: blunt knife. Nothing was changed"));
    t.put("assets/tools/cut.sh", "echo x > \"$(dirname \"$1\")/beside.png\"\n");
    KV_CHECK(c, gb("learn", sheet, "--root", root) == 1 && has(g_err.data, "wrote nothing into its work directory") && has(g_err.data, "1 file under the asset root") && has(g_err.data, "ships/beside.png"));
    KV_CHECK(c, strcmp(t.read("assets/goldberg.berg"), before) == 0);
    // A file no converter takes, and one that is not there.
    KV_CHECK(c, gb("learn", png, "--root", root) == 1 && has(g_err.data, "no converter takes this file"));
    KV_CHECK(c, gb("learn", "/no/such/file.sheet", "--root", root) == 2 && gb("learn") == 2);
    t.free();
}

// ---- a goldberg that stays up -------------------------------------------------------------------

// `goldberg watch` (or any goldberg) left running, its output gathered as it comes.
struct Running {
    Process p;
    Array<char> out, err;
    Array<const char*> argv;
    char exe[700] = {};

    template <class... A>
    bool start(A... words) {
        if (!platform_exe_dir(exe, sizeof exe)) return false;
        usize len = strlen(exe);
        snprintf(exe + len, sizeof exe - len, "goldberg");
        const char* const list[] = {words...};
        argv.push(exe);
        for (const char* w : list) argv.push(w);
        ProcessDesc d;
        d.argv = Span<const char* const>(argv.data, argv.len);
        return p.spawn(d);
    }
    void pump() {
        char buf[4096];
        for (;;) { i32 r = p.read_out(buf, sizeof buf); if (r <= 0) break; if (out.len) --out.len; memcpy(out.push_n((u32)r), buf, (usize)r); out.push(0); }
        for (;;) { i32 r = p.read_err(buf, sizeof buf); if (r <= 0) break; if (err.len) --err.len; memcpy(err.push_n((u32)r), buf, (usize)r); err.push(0); }
    }
    const char* text() { pump(); return out.len ? out.data : ""; }
    const char* errors() { pump(); return err.len ? err.data : ""; }
    // Waits for the process to end by itself; its exit status, or -1 after `seconds`.
    int wait(f64 seconds) {
        f64 t0 = time_now();
        while (p.running()) { pump(); if (time_now() - t0 > seconds) return -1; time_sleep(0.005); }
        pump();
        return p.code;
    }
    void free() { p.free(); out.free(); err.free(); argv.free(); }
};

// Polls until `fn` holds, for five seconds at most.
template <class F>
bool eventually(Running& r, F fn, f64 seconds = 5.0) {
    f64 t0 = time_now();
    for (;;) {
        r.pump();
        if (fn()) return true;
        if (time_now() - t0 > seconds) return false;
        time_sleep(0.01);
    }
}

// One request to a goldberg's CLI port: the response lines in g_out, true if it ended "ok".
bool ask(Running& r, u16 port, const char* line) {
    g_out.len = 0;
    NetSocket sock;
    if (!net_connect_local(port, sock)) { g_out.push(0); return false; }
    sock.write_str(line);
    sock.write_str("\n");
    bool ok = false, done = false;
    f64 t0 = time_now();
    while (!done && time_now() - t0 < 5.0) {
        sock.flush();
        r.pump();
        char buf[4096];
        i32 n = sock.read(buf, sizeof buf);
        if (n < 0) break;
        if (n > 0) memcpy(g_out.push_n((u32)n), buf, (usize)n);
        else time_sleep(0.002);
        // The last line says how it went.
        if (g_out.len && g_out.data[g_out.len - 1] == '\n') {
            u32 at = g_out.len - 1;
            while (at > 0 && g_out.data[at - 1] != '\n') --at;
            if (strncmp(g_out.data + at, "ok\n", 3) == 0) { ok = true; done = true; }
            else if (strncmp(g_out.data + at, "err", 3) == 0) done = true;
        }
    }
    sock.close();
    sock.out.free();
    g_out.push(0);
    return ok;
}

bool file_is(Tree& t, const char* rel, const char* content) { return io_is_file(t.path(rel)) && strcmp(t.read(rel), content) == 0; }

// The live session: a watch that stays up while sources and rules change under it.
void test_watch(TestCtx& c) {
    c.begin("goldberg watch");
    if (!process_supported()) return;
    for (int pass = 0; pass < 2; ++pass) {
        const char* backend = pass ? "poll" : "auto";
        Tree t;
        KV_CHECK(c, t.make("kv-goldberg-watch"));
        char root[600], out[600], port_text[16], a_png[600], hud[600];
        snprintf(root, sizeof root, "g=%s/assets", t.dir);
        snprintf(out, sizeof out, "%s/out", t.dir);
        snprintf(a_png, sizeof a_png, "%s/assets/a.png", t.dir);
        snprintf(hud, sizeof hud, "%s/assets/ui/hud.json", t.dir);
        u16 port = (u16)(41000 + (u32)(os_ticks() % 20000));
        snprintf(port_text, sizeof port_text, "%u", port);
        t.put("assets/a.png", "a1");
        t.put("assets/ui/hud.json", "{}");
        t.put("assets/ships/ship.png", "ship");
        t.put("assets/goldberg.berg", "{}");

        Running w;
        KV_CHECK(c, w.start("--cli-port", port_text, "watch", "game", "--root", root, "--project", t.dir, "--out", out, "--watch", backend));
        KV_CHECK(c, eventually(w, [&] { return has(w.text(), "3 files, 3 cooked, 0 clean, 0 failed") && has(w.text(), ", watching with "); }));
        KV_CHECK(c, file_is(t, "out/g/a.png", "a1") && file_is(t, "out/g/ui/hud.json", "{}") && file_is(t, "out/g/ships/ship.png", "ship"));

        // A source saved: that one is cooked again, and nothing else.
        t.put("assets/a.png", "a2, saved");
        KV_CHECK(c, eventually(w, [&] { return file_is(t, "out/g/a.png", "a2, saved") && has(w.text(), "3 files, 4 cooked"); }));
        // Saved with the same bytes: nothing is cooked.
        t.put("assets/a.png", "a2, saved");
        time_sleep(0.6);
        KV_CHECK(c, ask(w, port, "status") && has(g_out.data, "game: stable: 3 files, 4 cooked") && has(g_out.data, "0 failed, 0 warned, 0 running, 0 waiting"));

        // A goldberg.berg saved: what it speaks for is solved again, and only what got a
        // different answer is touched.
        t.put("assets/goldberg.berg", R"({ "rules": [ { "match": "ui/**", "meta": { "cook": false } }, { "match": "*.png", "meta": { "filter": "nearest" } } ] })");
        KV_CHECK(c, eventually(w, [&] { return !io_is_file(t.path("out/g/ui/hud.json")); }));
        KV_CHECK(c, eventually(w, [&] { return ask(w, port, "status") && has(g_out.data, "stable: 3 files, 4 cooked"); }) && file_is(t, "out/g/a.png", "a2, saved"));
        // A sidecar arrives and speaks for its stem; it goes, and the stem is as it was.
        t.put("assets/ships/ship.berg", R"({ "meta": { "out": "ships/hero.png" } })");
        KV_CHECK(c, eventually(w, [&] { return file_is(t, "out/g/ships/hero.png", "ship") && !io_is_file(t.path("out/g/ships/ship.png")); }));
        KV_CHECK(c, io_remove(t.path("assets/ships/ship.berg")));
        KV_CHECK(c, eventually(w, [&] { return file_is(t, "out/g/ships/ship.png", "ship") && !io_is_file(t.path("out/g/ships/hero.png")); }));

        // A rule file that stops parsing: it is reported, with its line, and its last good
        // version stays in force. Nothing under it moves.
        t.put("assets/goldberg.berg", "{\n  \"rules\": [\n  }\n");
        KV_CHECK(c, eventually(w, [&] { return ask(w, port, "status") && has(g_out.data, "/assets/goldberg.berg:3: "); }));
        KV_CHECK(c, !io_is_file(t.path("out/g/ui/hud.json")) && file_is(t, "out/g/a.png", "a2, saved") && file_is(t, "out/g/ships/ship.png", "ship"));
        KV_CHECK(c, ask(w, port, "explain g:a.png") && has(g_out.data, "filter") && has(g_out.data, "its last good version is still in force"));
        // Mended: the error is gone, and the new rules are the rules.
        t.put("assets/goldberg.berg", "{}");
        KV_CHECK(c, eventually(w, [&] { return file_is(t, "out/g/ui/hud.json", "{}"); }));
        KV_CHECK(c, eventually(w, [&] { return ask(w, port, "status") && has(g_out.data, "stable") && has(g_out.data, "0 failed") && !has(g_out.data, "goldberg.berg:3"); }));

        // The verbs: explain by path, touch, and a file nobody typed.
        char line[900];
        snprintf(line, sizeof line, "explain \"%s\"", hud); // quoted: a temp directory may have a space in it (C:/Users/A B/...)
        KV_CHECK(c, ask(w, port, line) && has(g_out.data, "ui/hud.json") && has(g_out.data, "type    data") && has(g_out.data, "step    clean"));
        snprintf(line, sizeof line, "goldberg touch \"%s\"", a_png);
        KV_CHECK(c, ask(w, port, line) && has(g_out.data, "touched g:a.png"));
        KV_CHECK(c, !ask(w, port, "explain /no/such/file") && has(g_out.data, "not a file this session knows"));
        t.put("assets/odd.xyz", "?");
        KV_CHECK(c, eventually(w, [&] { return ask(w, port, "status") && has(g_out.data, "1 warned") && has(g_out.data, "  warned   g:odd.xyz: no type"); }));
        KV_CHECK(c, ask(w, port, "ls /kinds/goldberg") && has(g_out.data, "file") && has(g_out.data, "step"));

        // Asked to cook: what a watch does not watch (its output tree) is checked as a cook
        // would. An output changed behind its back is made again, a stray goes to the attic.
        t.put("out/g/ships/ship.png", "tampered");
        t.put("out/g/stray.txt", "nobody's");
        KV_CHECK(c, ask(w, port, "cook") && has(g_out.data, "cooking"));
        KV_CHECK(c, eventually(w, [&] { return has(w.text(), "cooked again"); }));
        KV_CHECK(c, file_is(t, "out/g/ships/ship.png", "ship") && !io_is_file(t.path("out/g/stray.txt")));
        KV_CHECK(c, !ask(w, port, "cook now") && has(g_out.data, "takes nothing"));

        // The project's rule file, which is in no asset root, is seen too.
        t.put("goldberg.berg", R"({ "rules": [ { "match": "*.xyz", "meta": { "cook": false } } ] })");
        KV_CHECK(c, eventually(w, [&] { return !io_is_file(t.path("out/g/odd.xyz")) && ask(w, port, "status") && has(g_out.data, "0 warned"); }));

        // A source deleted: its output goes. A directory of them too.
        KV_CHECK(c, io_remove(t.path("assets/a.png")));
        KV_CHECK(c, eventually(w, [&] { return !io_is_file(t.path("out/g/a.png")); }));
        KV_CHECK(c, io_remove_tree(t.path("assets/ships")));
        KV_CHECK(c, eventually(w, [&] { return !io_is_file(t.path("out/g/ships/ship.png")); }) && file_is(t, "out/g/ui/hud.json", "{}"));

        // A cook of the same tree while the watch has it does nothing, and says why.
        KV_CHECK(c, gb("cook", "game", "--root", root, "--project", t.dir, "--out", out, "--watch", "none") == 0 && has(g_out.data, "a goldberg watch has this tree (watch "));

        // quit: the state is saved and the watch ends.
        KV_CHECK(c, ask(w, port, "quit") || true);
        KV_CHECK(c, w.wait(5.0) == 0 && has(w.text(), "game: stopped") && io_is_file(t.path(".goldberg-state/out.json")));
        // And the next cook finds everything as the watch left it.
        KV_CHECK(c, gb("cook", "game", "--root", root, "--project", t.dir, "--out", out, "--watch", "none") == 0 && has(g_out.data, "0 cooked, 1 clean, 0 failed"));
        w.free();
        t.free();
    }
}

// Two cooks of one tree: the second waits for the first, then finds nothing to do.
// A watch with a small share takes its events a few a tick and writes its state a slice a
// tick. Started on a tree a cook left up to date, it cooks nothing; a change made while it
// runs is cooked, and what it writes is the whole state: after it quits, a cook of the
// same tree has nothing to do. (That nothing is planned until every root's scan is in
// shows only at a size where the scan outgrows the event pool, ticks apart: the heavy
// test's watch phase is the one that would catch it.)
void test_state_slices(TestCtx& c) {
    c.begin("goldberg, a watch with a small share");
    if (!process_supported()) return;
    Tree t;
    KV_CHECK(c, t.make("kv-goldberg-slices"));
    char root[600], out[600], port_text[16], name[64], body[64];
    snprintf(root, sizeof root, "g=%s/assets", t.dir);
    snprintf(out, sizeof out, "%s/out", t.dir);
    t.put("assets/goldberg.berg", R"({ "types": { "raw": { "runtime": false } },
  "converters": { "up": { "from": "raw", "to": "text", "shell": "up", "outputs": ["{stem}_*.txt"] } },
  "rules": [ { "match": "*.raw", "meta": { "type": "raw" } } ] })");
    // Ten outputs a run: the work root is the biggest of the three, as in a real cook.
    t.put("assets/up.sh", "for i in 0 1 2 3 4 5 6 7 8 9; do cp \"$1\" \"$2/${GOLDBERG_STEM}_$i.txt\"; done\n");
    for (u32 i = 0; i < 300; ++i) {
        snprintf(name, sizeof name, "assets/d%u/f%03u.raw", i / 50, i);
        snprintf(body, sizeof body, "file %u", i);
        t.put(name, body);
    }
    KV_CHECK(c, gb("cook", "game", "--root", root, "--project", t.dir, "--out", out, "--watch", "none") == 0);
    KV_CHECK(c, has(g_out.data, " 3300 cooked, 0 clean, 0 failed"));
    u16 port = (u16)(41000 + (u32)(os_ticks() % 20000));
    snprintf(port_text, sizeof port_text, "%u", port);
    Running w;
    KV_CHECK(c, w.start("--cli-port", port_text, "watch", "game", "--root", root, "--project", t.dir, "--out", out, "--share", "20"));
    KV_CHECK(c, eventually(w, [&] { return has(w.text(), ", watching with "); }, 30.0));
    KV_CHECK(c, has(w.text(), " 0 cooked, 3300 clean, 0 failed"));
    t.put("assets/d3/f170.raw", "changed while watched");
    KV_CHECK(c, eventually(w, [&] { return file_is(t, "out/g/d3/f170_9.txt", "changed while watched"); }, 30.0));
    KV_CHECK(c, eventually(w, [&] { return ask(w, port, "status") && has(g_out.data, "stable"); }, 30.0));
    KV_CHECK(c, ask(w, port, "quit"));
    KV_CHECK(c, w.wait(10.0) == 0 && has(w.text(), "game: stopped"));
    w.free();
    KV_CHECK(c, gb("cook", "game", "--root", root, "--project", t.dir, "--out", out, "--watch", "none") == 0);
    KV_CHECK(c, has(g_out.data, " 0 cooked, 3300 clean, 0 failed"));
    t.free();
}

void test_lock(TestCtx& c) {
    c.begin("goldberg lock");
    if (!process_supported()) return;
    Tree t;
    KV_CHECK(c, t.make("kv-goldberg-lock"));
    char root[600], out[600];
    snprintf(root, sizeof root, "g=%s/assets", t.dir);
    snprintf(out, sizeof out, "%s/out", t.dir);
    t.put("assets/goldberg.berg", R"({ "types": { "slow": {} }, "converters": { "nap": { "from": "slow", "to": "text", "shell": "nap", "outputs": ["{stem}.txt"] } },
  "rules": [ { "match": "*.slow", "meta": { "type": "slow" } } ] })");
    t.put("assets/nap.sh", "sleep 1\necho rested > \"$2/$GOLDBERG_STEM.txt\"\n");
    t.put("assets/one.slow", "z");
    Running first;
    KV_CHECK(c, first.start("cook", "game", "--root", root, "--project", t.dir, "--out", out, "--watch", "none"));
    time_sleep(0.3);
    f64 t0 = time_now();
    KV_CHECK(c, gb("cook", "game", "--root", root, "--project", t.dir, "--out", out, "--watch", "none") == 0);
    KV_CHECK(c, time_now() - t0 > 0.5 && has(g_out.data, "0 cooked, 2 clean") && file_is(t, "out/g/one.txt", "rested\n"));
    KV_CHECK(c, first.wait(5.0) == 0 && has(first.text(), "2 cooked"));
    first.free();
    // One that will not wait says who has the tree.
    Running again;
    t.put("assets/one.slow", "zz");
    KV_CHECK(c, again.start("cook", "game", "--root", root, "--project", t.dir, "--out", out, "--watch", "none"));
    time_sleep(0.3);
    KV_CHECK(c, gb("cook", "game", "--root", root, "--project", t.dir, "--out", out, "--watch", "none", "--lock-wait", "0") == 2 && has(g_err.data, "another goldberg (cook "));
    KV_CHECK(c, again.wait(5.0) == 0);
    again.free();
    t.free();
}

} // namespace

int main() {
    TestCtx c;
    test_rules(c);
    test_mistakes(c);
    test_cook(c);
    test_scripts(c);
    test_learn(c);
    test_watch(c);
    test_state_slices(c);
    test_lock(c);
    g_out.free();
    g_err.free();
    names_shutdown();
    mem_shutdown();
    return test_report(c, "goldberg_tests");
}
