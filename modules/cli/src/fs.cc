// The filesystem verbs: ls, cd, pwd, print, patch. A path is resolved against the
// session's working directory, its first component picks a mount, and the rest goes to
// the mount. The root lists the mounts.
#include "cli_internal.h"
#include "korvid/core/engine.h"
#include "korvid/core/memory.h"
#include <stdlib.h>
#include <string.h>

namespace kv {

namespace {

// Splits an absolute path into its mount and the relative rest ("" at the mount's root).
const CliMountDesc* mount_of(CliState& cli, const char* path, const char** rel) {
    if (path[0] != '/') return nullptr;
    const char* slash = strchr(path + 1, '/');
    usize len = slash ? (usize)(slash - path) : strlen(path);
    for (const CliMountDesc* m : cli.mounts)
        if (strlen(m->path) == len && strncmp(m->path, path, len) == 0) {
            *rel = slash ? slash + 1 : "";
            return m;
        }
    return nullptr;
}

struct Entry { char name[64]; char summary[160]; };

struct Collect {
    Array<Entry>* out;
    u32 cap;
    u32 total = 0;
};

bool collect_emit(void* user, const char* name, const char* summary) {
    Collect& c = *(Collect*)user;
    ++c.total;
    if (c.out->len >= c.cap) return c.total <= c.cap + 1000; // count a little past the cap, then stop
    Entry e;
    strncpy(e.name, name, sizeof e.name - 1); e.name[sizeof e.name - 1] = 0;
    strncpy(e.summary, summary ? summary : "", sizeof e.summary - 1); e.summary[sizeof e.summary - 1] = 0;
    c.out->push(e);
    return true;
}

int entry_compare(const void* a, const void* b) { return strcmp(((const Entry*)a)->name, ((const Entry*)b)->name); }

// What a path is: the root, a mount's root, a directory the parent lists, a leaf, or nothing.
enum class Node { None, Root, Dir, Leaf };

struct Probe { const char* want; Node found; };
bool probe_emit(void* user, const char* name, const char*) {
    Probe& p = *(Probe*)user;
    usize n = strlen(name);
    bool dir = n && name[n - 1] == '/';
    if (dir) --n;
    if (strlen(p.want) == n && strncmp(p.want, name, n) == 0) { p.found = dir ? Node::Dir : Node::Leaf; return false; }
    return true;
}

Node classify(CliContext& ctx, CliState& cli, const char* path) {
    if (strcmp(path, "/") == 0) return Node::Root;
    const char* rel = nullptr;
    const CliMountDesc* m = mount_of(cli, path, &rel);
    if (!m) return Node::None;
    if (!rel[0]) return Node::Dir;
    // Ask the parent directory for the last component.
    char parent[KV_CLI_PATH_MAX];
    const char* last = strrchr(rel, '/');
    usize plen = last ? (usize)(last - rel) + 1 : 0; // includes the slash
    memcpy(parent, rel, plen);
    parent[plen] = 0;
    Probe p{last ? last + 1 : rel, Node::None};
    m->list(ctx, parent, probe_emit, &p);
    return p.found;
}

void list_dir(CliContext& ctx, CliState& cli, const char* path, u32 cap) {
    MemScope scope;
    Array<Entry> entries = array_in<Entry>(mem_scratch());
    Collect col{&entries, cap};
    if (strcmp(path, "/") == 0) {
        for (const CliMountDesc* m : cli.mounts) {
            char name[64];
            snprintf(name, sizeof name, "%s/", m->path + 1);
            collect_emit(&col, name, "");
        }
    } else {
        const char* rel = nullptr;
        const CliMountDesc* m = mount_of(cli, path, &rel);
        if (!m) { ctx.fail("no such path: %s", path); return; }
        char dir[KV_CLI_PATH_MAX];
        snprintf(dir, sizeof dir, "%s%s", rel, rel[0] ? "/" : "");
        m->list(ctx, dir, collect_emit, &col);
    }
    if (entries.len > 1) qsort(entries.data, entries.len, sizeof(Entry), entry_compare);
    u32 width = 0;
    for (const Entry& e : entries) { u32 w = (u32)strlen(e.name); if (w > width) width = w; }
    if (width > 40) width = 40;
    for (const Entry& e : entries) {
        if (e.summary[0]) ctx.print("%-*s  %s", (int)width, e.name, e.summary);
        else ctx.print("%s", e.name);
    }
    if (col.total > entries.len) {
        if (col.total > cap + 1000) ctx.print("... and more than %u more (ls -n N for a longer listing)", col.total - entries.len);
        else ctx.print("... and %u more (ls -n N for a longer listing)", col.total - entries.len);
    }
    if (entries.len == 0 && col.total == 0) ctx.print("(empty)");
}

} // namespace

void cli_fs_ls(CliContext& ctx, Span<const char* const> args) {
    CliState& cli = *cli_state(*ctx.engine);
    u32 cap = 200;
    const char* target = nullptr;
    for (u32 i = 0; i < args.len; ++i) {
        if (strcmp(args[i], "-n") == 0 && i + 1 < args.len) { if (!cli_arg_u32(ctx, args[++i], "count", &cap)) return; }
        else if (target) { ctx.fail("usage: ls [-n N] [path]"); return; }
        else target = args[i];
    }
    char path[KV_CLI_PATH_MAX];
    if (!cli_resolve(ctx, target ? target : ".", path, sizeof path)) return;
    Node n = classify(ctx, cli, path);
    if (n == Node::None) { ctx.fail("no such path: %s", path); return; }
    if (n == Node::Leaf) { cli_fs_print(ctx, Span<const char* const>(&target, 1)); return; }
    list_dir(ctx, cli, path, cap);
}

void cli_fs_cd(CliContext& ctx, Span<const char* const> args) {
    CliState& cli = *cli_state(*ctx.engine);
    if (args.len > 1) { ctx.fail("usage: cd [path]"); return; }
    if (!ctx.cwd) { ctx.fail("no session to change directory in"); return; }
    char path[KV_CLI_PATH_MAX];
    if (!cli_resolve(ctx, args.len ? args[0] : "/", path, sizeof path)) return;
    Node n = classify(ctx, cli, path);
    if (n == Node::None) { ctx.fail("no such path: %s", path); return; }
    if (n == Node::Leaf) { ctx.fail("not a directory: %s", path); return; }
    strncpy(ctx.cwd, path, KV_CLI_PATH_MAX - 1);
    ctx.cwd[KV_CLI_PATH_MAX - 1] = 0;
    ctx.print("%s", ctx.cwd);
}

void cli_fs_pwd(CliContext& ctx, Span<const char* const>) {
    ctx.print("%s", ctx.cwd ? ctx.cwd : "/");
}

void cli_fs_print(CliContext& ctx, Span<const char* const> args) {
    CliState& cli = *cli_state(*ctx.engine);
    if (args.len == 0) { ctx.fail("usage: print <path>..."); return; }
    for (const char* arg : args) {
        char path[KV_CLI_PATH_MAX];
        if (!cli_resolve(ctx, arg, path, sizeof path)) return;
        if (strcmp(path, "/") == 0) { list_dir(ctx, cli, path, 200); continue; }
        const char* rel = nullptr;
        const CliMountDesc* m = mount_of(cli, path, &rel);
        if (!m) { ctx.fail("no such path: %s", path); return; }
        if (m->describe && m->describe(ctx, rel)) continue;
        if (ctx.failed) return;
        Node n = classify(ctx, cli, path);
        if (n == Node::Dir) { list_dir(ctx, cli, path, 200); continue; }
        ctx.fail("no such path: %s", path);
        return;
    }
}

void cli_fs_patch(CliContext& ctx, Span<const char* const> args) {
    CliState& cli = *cli_state(*ctx.engine);
    if (args.len < 2) { ctx.fail("usage: patch <path> <value>..."); return; }
    char path[KV_CLI_PATH_MAX];
    if (!cli_resolve(ctx, args[0], path, sizeof path)) return;
    const char* rel = nullptr;
    const CliMountDesc* m = mount_of(cli, path, &rel);
    if (!m) { ctx.fail("no such path: %s", path); return; }
    if (!m->patch) { ctx.fail("%s is read-only", m->path); return; }
    if (!m->patch(ctx, rel, Span<const char* const>(args.data + 1, args.len - 1)) && !ctx.failed) ctx.fail("cannot patch %s", path);
}

} // namespace kv
