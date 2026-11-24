// The settings store (settings.h). In memory, an entry a key: an integer, a number, a
// string, or any other JSON as its text (a struct's object among them). A set marks the
// entry changed and wakes the writer, a thread of the store's own, which takes what has
// changed, and under the lock between processes reads the file again, writes it with the
// changes over it to settings.json.tmp, flushes that, and renames it over settings.json.
#include "korvid/platform/settings.h"
#include "korvid/platform/io.h"
#include "korvid/platform/module.h"
#include "korvid/core/arena.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/json.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/os_thread.h"
#include "korvid/core/time.h"
#include "os_file.h"
#include <errno.h>
#include <new>
#include <stdlib.h>
#include <string.h>

namespace kv {

// A store's entries (outside the anonymous namespace: SettingsStore holds them).
enum class SettingsKind : u8 { Int, Number, String, Json, Removed };

struct SettingsEntry {
    char* key   = nullptr;
    SettingsKind kind = SettingsKind::Int;
    i64   i     = 0;
    f64   n     = 0.0;
    char* text  = nullptr; // String: the text; Json: the value's JSON
    bool  dirty = false;   // set since the writer last took it
};

namespace {

using Kind  = SettingsKind;
using Entry = SettingsEntry;

char* dup(const char* s, usize n) {
    char* p = (char*)mem_alloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}
char* dup(const char* s) { return dup(s, strlen(s)); }

// A value of one document into a builder: what a merge keeps of the file.
JsonRef copy_value(JsonBuilder& b, const JsonDoc& doc, const JsonValue& v) {
    switch (v.type) {
    case JsonType::Null:   return b.null();
    case JsonType::Bool:   return b.boolean(json_bool(&v, false));
    case JsonType::Number:
        if (!v.integer()) return b.number(v.number());
        if (v.flags & KV_JSON_VALUE_NEGATIVE) return b.integer((i64)v.bits());
        return v.bits() > (u64)0x7fffffffffffffffull ? b.unsigned_integer(v.bits()) : b.integer((i64)v.bits());
    case JsonType::String: return b.string(v.str(), v.len());
    case JsonType::Bytes:  return b.bytes(v.bytes(), v.len());
    case JsonType::Array: {
        JsonRef a = b.array();
        for (const JsonValue* k = json_first(doc, v); k; k = json_next(doc, *k)) b.push(a, copy_value(b, doc, *k));
        return a;
    }
    case JsonType::Object: {
        JsonRef o = b.object();
        for (const JsonValue* k = json_first(doc, v); k; k = json_next(doc, *k)) b.set(o, k->key, copy_value(b, doc, *k));
        return o;
    }
    }
    return b.null();
}

// The JSON text of one value, compact.
char* value_text(const JsonDoc& doc, const JsonValue& v) {
    Arena ar;
    if (!arena_create(ar, 64u << 20, "settings.value", MemTag{})) return dup("null");
    JsonBuilder b(&ar.a);
    JsonRef r = copy_value(b, doc, v);
    JsonDoc out;
    char* text = nullptr;
    if (b.finish(r, out)) {
        Array<u8> bytes{};
        json_write(out, bytes, 0);
        text = dup((const char*)bytes.data, bytes.len);
        bytes.free();
    }
    b.free();
    arena_destroy(ar);
    return text ? text : dup("null");
}

// One field of a struct as JSON, and back: as the script binding maps them (a char[N] is
// a string, any other array a list, a handle its bits).
JsonRef field_value(JsonBuilder& b, const FieldDesc& f, const u8* row) {
    const u8* p = row + f.offset;
    if (f.type == FieldType::Char) {
        usize n = 0;
        while (n < f.count && p[n]) ++n;
        return b.string((const char*)p, (u32)n);
    }
    auto scalar = [&](const u8* q) -> JsonRef {
        switch (f.type) {
        case FieldType::Bool:   return b.boolean(*q != 0);
        case FieldType::U8:     return b.integer(*q);
        case FieldType::I8:     return b.integer(*(const i8*)q);
        case FieldType::Char:   return b.integer(*(const char*)q);
        case FieldType::U16:    { u16 v; memcpy(&v, q, sizeof v); return b.integer(v); }
        case FieldType::I16:    { i16 v; memcpy(&v, q, sizeof v); return b.integer(v); }
        case FieldType::U32:    { u32 v; memcpy(&v, q, sizeof v); return b.integer(v); }
        case FieldType::I32:    { i32 v; memcpy(&v, q, sizeof v); return b.integer(v); }
        case FieldType::I64:    { i64 v; memcpy(&v, q, sizeof v); return b.integer(v); }
        case FieldType::U64:
        case FieldType::Handle: { u64 v; memcpy(&v, q, sizeof v); return b.unsigned_integer(v); }
        case FieldType::F32:    { f32 v; memcpy(&v, q, sizeof v); return b.number(v); }
        case FieldType::F64:    { f64 v; memcpy(&v, q, sizeof v); return b.number(v); }
        case FieldType::Count:  break;
        }
        return b.null();
    };
    if (f.count == 1) return scalar(p);
    JsonRef a = b.array();
    u32 size = field_type_size(f.type);
    for (u32 i = 0; i < f.count; ++i) b.push(a, scalar(p + (usize)i * size));
    return a;
}

void set_scalar(const FieldDesc& f, u8* q, const JsonValue& v) {
    if (!v.is_number() && !v.is_bool()) return;
    f64 d = v.number();
    i64 i = v.integer() ? (i64)v.bits() : (i64)d;
    switch (f.type) {
    case FieldType::Bool:   *q = d != 0.0; return;
    case FieldType::U8:     *q = (u8)i; return;
    case FieldType::I8:     *(i8*)q = (i8)i; return;
    case FieldType::Char:   *(char*)q = (char)i; return;
    case FieldType::U16:    { u16 x = (u16)i; memcpy(q, &x, sizeof x); return; }
    case FieldType::I16:    { i16 x = (i16)i; memcpy(q, &x, sizeof x); return; }
    case FieldType::U32:    { u32 x = (u32)i; memcpy(q, &x, sizeof x); return; }
    case FieldType::I32:    { i32 x = (i32)i; memcpy(q, &x, sizeof x); return; }
    case FieldType::I64:    memcpy(q, &i, sizeof i); return;
    case FieldType::U64:
    case FieldType::Handle: { u64 x = v.integer() ? v.bits() : (u64)d; memcpy(q, &x, sizeof x); return; }
    case FieldType::F32:    { f32 x = (f32)d; memcpy(q, &x, sizeof x); return; }
    case FieldType::F64:    memcpy(q, &d, sizeof d); return;
    case FieldType::Count:  return;
    }
}

void set_field(const JsonDoc& doc, const FieldDesc& f, u8* row, const JsonValue& v) {
    u8* p = row + f.offset;
    if (f.type == FieldType::Char) {
        if (!v.is_string()) return;
        u32 cap = (u32)f.count - 1;
        u32 n = v.len() < cap ? v.len() : cap; // cut to fit, NUL kept
        memcpy(p, v.str(), n);
        memset(p + n, 0, f.count - n);
        return;
    }
    if (f.count == 1) { set_scalar(f, p, v); return; }
    if (!v.is_array()) return;
    u32 size = field_type_size(f.type), i = 0;
    for (const JsonValue* k = json_first(doc, v); k && i < f.count; k = json_next(doc, *k), ++i) set_scalar(f, p + (usize)i * size, *k);
}

} // namespace

struct SettingsStore {
    char         dir[KV_PLATFORM_PATH_MAX];
    Array<SettingsEntry> entries;
    OsMutex      m;
    OsCond       wake, done;
    OsThread     writer;
    bool         started = false, quit = false;
    u64          asked = 0, written = 0; // sets made; sets the writer has put on the disk
    u32          failures = 0;           // writes that failed since the last flush
    u8           stall = 0;              // dev: KORVID_SETTINGS_STALL, for the kill tests
};

namespace {

Entry* find(SettingsStore* s, const char* key) {
    for (Entry& e : s->entries) if (!strcmp(e.key, key)) return &e;
    return nullptr;
}
const Entry* find(const SettingsStore* s, const char* key) {
    for (const Entry& e : s->entries) if (!strcmp(e.key, key) && e.kind != Kind::Removed) return &e;
    return nullptr;
}

void free_value(Entry& e) {
    if (e.text) mem_free(e.text);
    e.text = nullptr;
}

// The values in a file's document into the store.
void take_doc(SettingsStore* s, const JsonDoc& doc) {
    const JsonValue* values = json_get(doc, doc.root(), "values");
    if (!values || !values->is_object()) return;
    for (const JsonValue* v = json_first(doc, *values); v; v = json_next(doc, *v)) {
        Entry e;
        e.key = dup(v->key);
        if (v->is_number() && v->integer() && ((v->flags & KV_JSON_VALUE_NEGATIVE) || v->bits() <= (u64)0x7fffffffffffffffull)) { e.kind = Kind::Int; e.i = (i64)v->bits(); }
        else if (v->is_number()) { e.kind = Kind::Number; e.n = v->number(); }
        else if (v->is_string()) { e.kind = Kind::String; e.text = dup(v->str(), v->len()); }
        else { e.kind = Kind::Json; e.text = value_text(doc, *v); }
        s->entries.push(e);
    }
}

JsonRef entry_value(JsonBuilder& b, const Entry& e) {
    switch (e.kind) {
    case Kind::Int:     return b.integer(e.i);
    case Kind::Number:  return b.number(e.n);
    case Kind::String:  return b.string(e.text);
    case Kind::Json: {
        JsonDoc d;
        if (!json_parse(e.text, strlen(e.text), b.alloc, d)) return b.null();
        return copy_value(b, d, d.root());
    }
    case Kind::Removed: break;
    }
    return b.null();
}

bool write_all(const char* path, const void* data, usize size, i32* err) {
    OsFile f = os_file_open(path, true, err);
    if (f < 0) return false;
    const u8* p = (const u8*)data;
    usize left = size;
    while (left) {
        i64 n = os_file_write(f, p, left, err);
        if (n <= 0) { os_file_close(f); return false; }
        p += n;
        left -= (usize)n;
    }
    bool ok = os_file_sync(f, err);
    os_file_close(f);
    return ok;
}

// The file written again with `changes` over what it holds now, under the lock.
bool write_merged(SettingsStore* s, Span<const Entry> changes) {
    char path[KV_PLATFORM_PATH_MAX + 32], tmp[KV_PLATFORM_PATH_MAX + 32], lock_path[KV_PLATFORM_PATH_MAX + 32];
    format(path, sizeof path, "%ssettings.json", s->dir);
    format(tmp, sizeof tmp, "%ssettings.json.tmp", s->dir);
    format(lock_path, sizeof lock_path, "%ssettings.lock", s->dir);
    i32 err = 0;
    OsFile lock = os_file_lock(lock_path, &err);
    if (lock < 0) { KV_WARN("settings", "cannot lock %s (errno %d)", lock_path, err); return false; }
    Arena ar;
    if (!arena_create(ar, 256u << 20, "settings.write", MemTag{})) { os_file_close(lock); return false; }
    JsonDoc now;
    bool have_now = false;
    IoBytes cur;
    if (io_read_now(path, &cur, &ar.a)) have_now = json_parse((const char*)cur.data, cur.size, &ar.a, now);
    JsonBuilder b(&ar.a);
    JsonRef root = b.object(), values = b.object();
    b.set(root, "korvid_settings", b.integer(1));
    // What the file has that these changes do not touch, in its order; then the changes.
    const JsonValue* had = have_now ? json_get(now, now.root(), "values") : nullptr;
    if (had && had->is_object())
        for (const JsonValue* v = json_first(now, *had); v; v = json_next(now, *v)) {
            bool changed = false;
            for (const Entry& c : changes) if (!strcmp(c.key, v->key)) { changed = true; break; }
            if (!changed) b.set(values, v->key, copy_value(b, now, *v));
        }
    for (const Entry& c : changes) if (c.kind != Kind::Removed) b.set(values, c.key, entry_value(b, c));
    b.set(root, "values", values);
    JsonDoc out;
    bool ok = b.finish(root, out);
    Array<u8> bytes{};
    if (ok) { json_write(out, bytes); bytes.push('\n'); }
    b.free();
#if KV_DEV
    if (ok && s->stall == 1) { write_all(tmp, bytes.data, bytes.len / 2, &err); time_sleep(60.0); }
#endif
    ok = ok && write_all(tmp, bytes.data, bytes.len, &err);
#if KV_DEV
    if (ok && s->stall == 2) time_sleep(60.0);
#endif
    ok = ok && os_rename(tmp, path);
    if (ok) os_dir_sync(s->dir);
    else KV_WARN("settings", "cannot write %s (errno %d)", path, err);
    bytes.free();
    arena_destroy(ar);
    os_file_close(lock);
    return ok;
}

void writer_main(void* arg) {
    SettingsStore* s = (SettingsStore*)arg;
    os_mutex_lock(&s->m);
    for (;;) {
        while (!s->quit && s->asked == s->written) os_cond_wait(&s->wake, &s->m);
        if (s->asked == s->written) break; // quitting with nothing pending
        u64 target = s->asked;
        // What changed, copied: the store goes on being set while the file is written.
        Array<Entry> changes{};
        for (Entry& e : s->entries) {
            if (!e.dirty) continue;
            Entry c = e;
            c.key = dup(e.key);
            c.text = e.text ? dup(e.text) : nullptr;
            changes.push(c);
            e.dirty = false;
        }
        os_mutex_unlock(&s->m);
        bool ok = write_merged(s, changes.span());
        os_mutex_lock(&s->m);
        if (ok) s->written = target;
        else {
            ++s->failures;
            // Still pending: marked again, unless set since, and tried at the next set.
            for (const Entry& c : changes) if (Entry* e = find(s, c.key)) e->dirty = true;
            s->written = target;
        }
        for (Entry& c : changes) { mem_free(c.key); free_value(c); }
        changes.free();
        os_cond_broadcast(&s->done);
    }
    os_mutex_unlock(&s->m);
}

// A set: the entry changed, and the writer woken (started at the first).
Entry* begin_set(SettingsStore* s, const char* key) {
    os_mutex_lock(&s->m);
    Entry* e = find(s, key);
    if (!e) {
        Entry n;
        n.key = dup(key);
        s->entries.push(n);
        e = &s->entries.last();
    }
    free_value(*e);
    return e;
}
void end_set(SettingsStore* s, Entry* e) {
    e->dirty = true;
    ++s->asked;
    if (!s->started) s->started = os_thread_start(&s->writer, writer_main, s);
    os_cond_signal(&s->wake);
    os_mutex_unlock(&s->m);
}

} // namespace

SettingsStore* settings_open_dir(const char* dir, i32* err) {
    if (!dir || !dir[0] || !io_make_dirs(dir)) { if (err) *err = ENOENT; return nullptr; }
    SettingsStore* s = new (mem_alloc(sizeof(SettingsStore))) SettingsStore{};
    usize n = strlen(dir);
    format(s->dir, sizeof s->dir, n && dir[n - 1] == '/' ? "%s" : "%s/", dir);
    os_mutex_init(&s->m);
    os_cond_init(&s->wake);
    os_cond_init(&s->done);
#if KV_DEV
    if (const char* stall = getenv("KORVID_SETTINGS_STALL")) s->stall = !strcmp(stall, "write") ? 1 : !strcmp(stall, "rename") ? 2 : 0;
#endif
    char path[KV_PLATFORM_PATH_MAX + 32];
    format(path, sizeof path, "%ssettings.json", s->dir);
    IoBytes bytes;
    if (io_read_now(path, &bytes)) {
        Arena ar;
        if (arena_create(ar, 256u << 20, "settings.read", MemTag{})) {
            JsonDoc doc;
            if (json_parse((const char*)bytes.data, bytes.size, &ar.a, doc)) take_doc(s, doc);
            else KV_WARN("settings", "%s is not JSON (%s, line %u): starting empty, and the first change replaces it", path, doc.error, doc.error_line);
            arena_destroy(ar);
        }
        bytes.free();
    }
    return s;
}

bool settings_flush(SettingsStore* s) {
    if (!s) return false;
    os_mutex_lock(&s->m);
    while (s->started && s->written != s->asked) os_cond_wait(&s->done, &s->m);
    bool ok = s->failures == 0;
    s->failures = 0;
    os_mutex_unlock(&s->m);
    return ok;
}

void settings_close(SettingsStore* s) {
    if (!s) return;
    os_mutex_lock(&s->m);
    s->quit = true;
    os_cond_signal(&s->wake);
    os_mutex_unlock(&s->m);
    if (s->started) os_thread_join(&s->writer);
    for (Entry& e : s->entries) { mem_free(e.key); free_value(e); }
    s->entries.free();
    os_cond_destroy(&s->wake);
    os_cond_destroy(&s->done);
    os_mutex_destroy(&s->m);
    s->~SettingsStore();
    mem_free(s);
}

bool settings_has(const SettingsStore* s, const char* key) { return s && find(s, key); }

i64 settings_int(const SettingsStore* s, const char* key, i64 fallback) {
    const Entry* e = s ? find(s, key) : nullptr;
    if (!e) return fallback;
    return e->kind == Kind::Int ? e->i : e->kind == Kind::Number ? (i64)e->n : fallback;
}

f64 settings_number(const SettingsStore* s, const char* key, f64 fallback) {
    const Entry* e = s ? find(s, key) : nullptr;
    if (!e) return fallback;
    return e->kind == Kind::Number ? e->n : e->kind == Kind::Int ? (f64)e->i : fallback;
}

const char* settings_string(const SettingsStore* s, const char* key, const char* fallback) {
    const Entry* e = s ? find(s, key) : nullptr;
    return e && e->kind == Kind::String ? e->text : fallback;
}

bool settings_struct(const SettingsStore* s, const char* key, Span<const FieldDesc> fields, void* out) {
    const Entry* e = s ? find(s, key) : nullptr;
    if (!e || e->kind != Kind::Json) return false;
    MemScope scope;
    JsonDoc doc;
    if (!json_parse(e->text, strlen(e->text), mem_scratch(), doc) || !doc.root().is_object()) return false;
    for (const FieldDesc& f : fields)
        if (const JsonValue* v = json_get(doc, doc.root(), f.name)) set_field(doc, f, (u8*)out, *v);
    return true;
}

void settings_set_int(SettingsStore* s, const char* key, i64 v) {
    if (!s) return;
    Entry* e = begin_set(s, key);
    e->kind = Kind::Int;
    e->i = v;
    end_set(s, e);
}

void settings_set_number(SettingsStore* s, const char* key, f64 v) {
    if (!s) return;
    Entry* e = begin_set(s, key);
    e->kind = Kind::Number;
    e->n = v;
    end_set(s, e);
}

void settings_set_string(SettingsStore* s, const char* key, const char* v) {
    if (!s) return;
    Entry* e = begin_set(s, key);
    e->kind = Kind::String;
    e->text = dup(v ? v : "");
    end_set(s, e);
}

void settings_set_struct(SettingsStore* s, const char* key, Span<const FieldDesc> fields, const void* v) {
    if (!s) return;
    // The object's text, made before the store is locked.
    char* text = nullptr;
    {
        Arena ar;
        if (!arena_create(ar, 64u << 20, "settings.struct", MemTag{})) return;
        JsonBuilder b(&ar.a);
        JsonRef o = b.object();
        for (const FieldDesc& f : fields) b.set(o, f.name, field_value(b, f, (const u8*)v));
        JsonDoc doc;
        if (b.finish(o, doc)) {
            Array<u8> bytes{};
            json_write(doc, bytes, 0);
            text = dup((const char*)bytes.data, bytes.len);
            bytes.free();
        }
        b.free();
        arena_destroy(ar);
    }
    if (!text) return;
    Entry* e = begin_set(s, key);
    e->kind = Kind::Json;
    e->text = text;
    end_set(s, e);
}

void settings_remove(SettingsStore* s, const char* key) {
    if (!s || !find((const SettingsStore*)s, key)) return;
    Entry* e = begin_set(s, key);
    e->kind = Kind::Removed;
    end_set(s, e);
}

SettingsStore* platform_settings(Engine& e, const char* org, const char* app) {
    PlatformState* ps = engine_module_state_as<PlatformState>(e, "platform");
    if (!ps) return nullptr;
    if (!ps->settings) {
        const char* dir = platform_user_dir(e, UserDir::Settings, org, app);
        if (!dir[0]) return nullptr;
        ps->settings = settings_open_dir(dir);
    }
    return ps->settings;
}

} // namespace kv
