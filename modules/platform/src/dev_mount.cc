// Korvid platform: `/dev` in the CLI's tree. What the OS has open for the process, where
// a person would look for it: /dev/handles/<kind>/<slot>, one node an open handle.
//
//   ls /dev/handles            file/  socket/  pipe/  notify/  process/
//   ls /dev/handles/socket     3   listener 127.0.0.1:4100, opened by cli, 0 read, 0 written
//   print /dev/handles/file/7  the handle, what it is, who opened it, how long ago, the bytes
#include "platform_parts.h"
#include "korvid/platform/os_handle.h"
#include "korvid/core/format.h"
#include "korvid/core/time.h"
#include <stdlib.h>
#include <string.h>

namespace kv {

namespace {

// "listener 127.0.0.1:4100, opened by cli, 0 read, 0 written": what a listing shows of one.
void summary_of(const OsHandleInfo& info, char* out, usize cap) {
#if KV_DEV || KV_DEBUG
    format(out, cap, "%s%s%s, %llu read, %llu written", info.what[0] ? info.what : "(unnamed)", info.by[0] ? ", opened by " : "", info.by,
           (unsigned long long)info.bytes_in, (unsigned long long)info.bytes_out);
#else
    (void)info;
    format(out, cap, "open");
#endif
}

struct Listing { CliTree* tree; };
void list_one(void* user, Handle h, const OsHandleInfo& info) {
    Listing& l = *(Listing*)user;
    if (!l.tree->more) return;
    char name[64], text[260];
    format(name, sizeof name, "handles/%s/%u", os_kind_name(info.kind), h.index());
    summary_of(info, text, sizeof text);
    l.tree->add(name, false, text);
}

struct Find { OsKind kind; u32 index; Handle found; OsHandleInfo info; u32 of_kind; };
void find_one(void* user, Handle h, const OsHandleInfo& info) {
    Find& f = *(Find*)user;
    if (info.kind != f.kind) return;
    ++f.of_kind;
    if (h.index() == f.index) { f.found = h; f.info = info; }
}

OsKind kind_named(const char* name, usize len) {
    for (u32 k = 1; k <= (u32)OsKind::Process; ++k) {
        const char* n = os_kind_name((OsKind)k);
        if (strlen(n) == len && strncmp(n, name, len) == 0) return (OsKind)k;
    }
    return OsKind::None;
}

} // namespace

void platform_dev_list(CliContext&, const char* dir, CliEmitFn emit, void* user) {
    CliTree tree{dir, emit, user};
    // Every kind is a directory, with or without anything open in it: a path that is
    // always there, to look in.
    for (u32 k = 1; k <= (u32)OsKind::Process && tree.more; ++k) {
        char name[48];
        format(name, sizeof name, "handles/%s", os_kind_name((OsKind)k));
        tree.add(name, true, "");
    }
    Listing l{&tree};
    os_handles_each(list_one, &l);
}

bool platform_dev_describe(CliContext& ctx, const char* rel) {
    if (strcmp(rel, "handles") == 0) {
        ctx.print("%u OS handles open", os_handles_open());
        return true;
    }
    if (strncmp(rel, "handles/", 8) != 0) return false;
    const char* kind_at = rel + 8;
    const char* slash = strchr(kind_at, '/');
    OsKind kind = kind_named(kind_at, slash ? (usize)(slash - kind_at) : strlen(kind_at));
    if (kind == OsKind::None) return false;
    Find f{kind, slash ? (u32)strtoul(slash + 1, nullptr, 10) : ~0u, Handle{}, OsHandleInfo{}, 0};
    os_handles_each(find_one, &f);
    if (!slash) {
        ctx.print("%u %s handle%s open", f.of_kind, os_kind_name(kind), f.of_kind == 1 ? "" : "s");
        return true;
    }
    if (f.found.is_null()) return false;
    char text[400];
    kv_handle_describe(f.found.bits, text, sizeof text);
    ctx.print("%s", text);
#if KV_DEV || KV_DEBUG
    ctx.print("  kind     %s", os_kind_name(f.info.kind));
    ctx.print("  what     %s", f.info.what[0] ? f.info.what : "(unnamed)");
    ctx.print("  by       %s", f.info.by[0] ? f.info.by : "(no module)");
    ctx.print("  open     %.1f s", time_now() - f.info.opened_at);
    ctx.print("  read     %llu bytes", (unsigned long long)f.info.bytes_in);
    ctx.print("  written  %llu bytes", (unsigned long long)f.info.bytes_out);
#endif
    return true;
}

} // namespace kv
