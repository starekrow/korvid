// Krome's documents: each .kimb and script it reads, once, by asset path, through the I/O
// queue or from what krome_provide gave; and a screen's scope, once its files are in.
#include "internal.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/platform/io.h"
#include <stdarg.h>
#include <string.h>

namespace kv {

void krome_error(KromeModule& s, const char* file, u32 line, const char* used_file, u32 used_line, const char* fmt, ...) {
    char msg[200];
    va_list args;
    va_start(args, fmt);
    vformat(msg, sizeof msg, fmt, args);
    va_end(args);
    KimError* e = s.errors.add(file, line, 0, "%s", msg);
    if (e && used_file) { e->used_file = used_file; e->used_line = used_line; }
    if (used_file) KV_ERROR("krome", "%s:%u: %s (used at %s:%u)", file ? file : "?", line, msg, used_file, used_line);
    else KV_ERROR("krome", "%s:%u: %s", file ? file : "?", line, msg);
    ++s.script_errors;
}

Name krome_use_path(const char* use) {
    char p[300];
    format(p, sizeof p, "%s.kimb", use);
    return name_intern(p);
}

KromeDoc* krome_doc_loaded(KromeModule& s, Name path) {
    for (KromeDoc* d : s.docs) if (d->path == path) return d;
    return nullptr;
}

KromeDoc* krome_doc(KromeModule& s, Name path, bool script) {
    if (KromeDoc* d = krome_doc_loaded(s, path)) return d;
    KromeDoc* d = alloc_items<KromeDoc>(s.general, 1);
    *d = KromeDoc{};
    d->path = path;
    d->script = script;
    s.docs.push(d);
    return d;
}

namespace {

// The bytes have landed (or were given): a script keeps its text, a .kimb is read.
void take(KromeModule& s, KromeDoc& d, const void* data, usize size) {
    d.loading = false;
    if (d.script) {
        d.source = (char*)alloc_in(s.general, size + 1, 1);
        if (size) memcpy(d.source, data, size);
        d.source[size] = 0;
        d.source_len = size;
        d.ready = true;
        return;
    }
    // A document is at most a node (24 bytes) per byte of KBOR, and its strings no more
    // than its bytes: reserved, and committed as used.
    if (!arena_create(d.arena, size * 32 + (64u << 10), "krome.doc", MemTag{})) {
        krome_error(s, name_str(d.path), 0, nullptr, 0, "out of memory reading it");
        d.failed = true;
        return;
    }
    u32 before = s.errors.total;
    if (!kimb_read(data, size, &d.arena.a, d.file, s.errors, name_str(d.path))) {
        s.script_errors += s.errors.total - before;
        for (u32 i = before; i < s.errors.count; ++i) {
            char line[300];
            s.errors.format_one(i, line, sizeof line);
            KV_ERROR("krome", "%s", line);
        }
        d.failed = true;
        return;
    }
    d.ready = true;
}

} // namespace

void krome_pump_docs(KromeModule& s) {
    IoQueue* io = platform_io(*s.e);
    for (u32 i = 0; i < s.docs.len; ++i) {
        KromeDoc& d = *s.docs[i];
        if (!d.loading) continue;
        bool given = false;
        for (const KromeModule::Provided& p : s.provided)
            if (p.path == d.path) { take(s, d, p.data, p.size); given = true; break; }
        if (given || !io) continue;
        if (d.io.is_null()) {
            IoRequest r;
            r.path = name_str(d.path);
            r.alloc = s.general;
            d.io = io->read_asset(r);
            if (d.io.is_null()) { krome_error(s, name_str(d.path), 0, nullptr, 0, "cannot read it"); d.loading = false; d.failed = true; }
            continue;
        }
        for (const IoCompletion& c : io->completions()) {
            if (c.request != d.io) continue;
            IoCompletion done = c;
            io->reap(done.request);
            d.io = Handle{};
            if (done.status != IoStatus::Done) {
                krome_error(s, name_str(d.path), 0, nullptr, 0, "cannot read it (errno %d)", done.error);
                d.loading = false;
                d.failed = true;
            } else {
                take(s, d, done.data, done.size);
            }
            free_in(s.general, done.data, done.size ? done.size : 1);
            break;
        }
    }
}

void krome_free_docs(KromeModule& s) {
    IoQueue* io = platform_io(*s.e);
    for (KromeDoc* d : s.docs) {
        if (!d->io.is_null() && io) io->cancel(d->io);
        if (d->arena.base) arena_destroy(d->arena);
        free_in(s.general, d->source, d->source_len + 1);
        free_items(s.general, d, 1);
    }
    s.docs.free();
    for (KromeModule::Provided& p : s.provided) free_in(s.general, p.data, p.size ? p.size : 1);
    s.provided.free();
}

namespace {

struct UseCtx { KromeModule* s; bool waiting; bool failed; };

// A use's file while the scope is built: asked for when it is not yet, and null until it is
// in (the scope is built again once it lands).
const KimbFile* find_use(void* user, const char* use) {
    UseCtx& c = *(UseCtx*)user;
    KromeDoc* d = krome_doc(*c.s, krome_use_path(use), false);
    if (d->ready) return &d->file;
    if (d->failed) c.failed = true;
    else c.waiting = true;
    return nullptr;
}

} // namespace

bool krome_file_scope(KromeModule& s, const KimbFile& f, KimbScope& scope, bool* failed) {
    *failed = false;
    // Built into a scratch first: while a use is still on its way, the errors it would
    // report are not errors yet.
    KimErrors quiet;
    UseCtx c{&s, false, false};
    KimbScope out;
    bool ok = kimb_scope(out, f, find_use, &c, quiet);
    if (c.waiting && !c.failed) return false;
    if (!ok) {
        for (u32 i = 0; i < quiet.count; ++i) krome_error(s, quiet.list[i].file, quiet.list[i].line, nullptr, 0, "%s", quiet.list[i].text);
        *failed = true;
        return false;
    }
    scope = out;
    return true;
}

bool krome_scope_ready(KromeModule& s, KromeScreen& sc, bool* failed) {
    *failed = false;
    const KimbFile* f = sc.doc ? &sc.doc->file : sc.has_lua_file ? &sc.lua_file : nullptr;
    if (!f) return false;
    return krome_file_scope(s, *f, sc.scope, failed);
}

} // namespace kv
