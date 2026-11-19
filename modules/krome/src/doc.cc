// Krome's documents: each .kimb and script it reads, by asset path, as a blob asset (read
// again when the asset reloads) or from what krome_provide gave; and a screen's scope, once
// its files are in.
#include "internal.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/asset/blob.h"
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

// A .kimb's bytes read into a reading of its own. Null (the mistake logged and kept) when
// they are not a .kimb.
KromeDocVersion* read_version(KromeModule& s, KromeDoc& d, const void* data, usize size) {
    KromeDocVersion* v = alloc_items<KromeDocVersion>(s.general, 1);
    *v = KromeDocVersion{};
    // A document is at most a node (24 bytes) per byte of KBOR, and its strings no more
    // than its bytes: reserved, and committed as used.
    if (!arena_create(v->arena, size * 32 + (64u << 10), "krome.doc", MemTag{})) {
        krome_error(s, name_str(d.path), 0, nullptr, 0, "out of memory reading it");
        free_items(s.general, v, 1);
        return nullptr;
    }
    u32 before = s.errors.total;
    if (!kimb_read(data, size, &v->arena.a, v->file, s.errors, name_str(d.path))) {
        s.script_errors += s.errors.total - before;
        for (u32 i = before; i < s.errors.count; ++i) {
            char line[300];
            s.errors.format_one(i, line, sizeof line);
            KV_ERROR("krome", "%s", line);
        }
        arena_destroy(v->arena);
        free_items(s.general, v, 1);
        return nullptr;
    }
    return v;
}

void free_version(KromeModule& s, KromeDocVersion* v) {
    if (!v) return;
    if (v->arena.base) arena_destroy(v->arena);
    free_items(s.general, v, 1);
}

// The bytes have landed (or were given): a script keeps its text, a .kimb is read. A document
// already read takes them as a reload: a script's text is replaced; a .kimb's new reading
// waits in `next` to be checked (reload.cc), and one that does not read leaves it as it was.
// True when a document in use changed.
bool take(KromeModule& s, KromeDoc& d, const void* data, usize size) {
    bool again = d.ready;
    d.loading = false;
    if (d.script) {
        if (again && d.source_len == size && (!size || !memcmp(d.source, data, size))) return false;
        free_in(s.general, d.source, d.source_len + 1);
        d.source = (char*)alloc_in(s.general, size + 1, 1);
        if (size) memcpy(d.source, data, size);
        d.source[size] = 0;
        d.source_len = size;
        d.ready = true;
        d.failed = false;
        if (again) { ++s.reloads; d.changed = true; KV_INFO("krome", "'%s' reloaded", name_str(d.path)); }
        return again;
    }
    KromeDocVersion* v = read_version(s, d, data, size);
    if (!v) {
        if (again) {
            format(s.reload_error, sizeof s.reload_error, "%s: it would not read; the screens keep what they had", name_str(d.path));
            return false;
        }
        d.failed = true;
        return false;
    }
    if (!again) {
        d.cur = v;
        d.file = &v->file;
        d.ready = true;
        d.failed = false;
        return false;
    }
    free_version(s, d.next); // a reload not yet checked, overtaken
    d.next = v;
    ++s.reloads;
    KV_INFO("krome", "'%s' reloaded", name_str(d.path));
    return true;
}

} // namespace

bool krome_pump_docs(KromeModule& s) {
    bool changed = false;
    for (u32 i = 0; i < s.docs.len; ++i) {
        KromeDoc& d = *s.docs[i];
        // Given from memory: taken when it is new, or given again.
        const KromeModule::Provided* given = nullptr;
        for (const KromeModule::Provided& p : s.provided) if (p.path == d.path) { given = &p; break; }
        if (given) {
            if (given->generation != d.provided) {
                d.provided = given->generation;
                changed |= take(s, d, given->data, given->size);
            }
            continue;
        }
        // Read as a blob: taken when it lands, and again whenever it reloads.
        if (d.blob.is_null()) {
            if (!d.loading) continue;
            d.blob = asset_load_blob(*s.e, name_str(d.path));
            if (d.blob.is_null()) { krome_error(s, name_str(d.path), 0, nullptr, 0, "cannot read it"); d.loading = false; d.failed = true; continue; }
        }
        const BlobAsset* b = asset_blob(*s.e, d.blob);
        if (!b) continue;
        if (b->state == AssetState::Failed && d.loading) {
            krome_error(s, name_str(d.path), 0, nullptr, 0, "cannot read it (%s)", asset_error_name(b->error));
            d.loading = false;
            d.failed = true;
            d.generation = ~0u; // whatever it reads as next is new
            continue;
        }
        if (b->state != AssetState::Ready) continue;
        // A first read, a reload (a newer generation), or a file that failed and is there now.
        if (d.loading || b->generation != d.generation) {
            d.generation = b->generation;
            bool was_failed = d.failed;
            changed |= take(s, d, b->data, b->size);
            if (was_failed && d.ready) { // screens that failed on it may open now
                changed = true;
                for (KromeScreen* sc : s.screens)
                    if (sc->failed) { sc->failed = false; sc->loading = true; sc->stage = KromeStage::Read; }
            }
        }
    }
    return changed;
}

void krome_free_docs(KromeModule& s) {
    for (KromeDoc* d : s.docs) {
        free_version(s, d->cur);
        free_version(s, d->next);
        free_in(s.general, d->source, d->source_len + 1);
        free_items(s.general, d, 1);
    }
    s.docs.free();
    krome_free_retired(s, true);
    s.retired.free();
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
    if (d->ready && d->file) return d->file;
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
    const KimbFile* f = sc.doc ? sc.doc->file : sc.has_lua_file ? &sc.lua_file : nullptr;
    if (!f) return false;
    return krome_file_scope(s, *f, sc.scope, failed);
}

} // namespace kv
