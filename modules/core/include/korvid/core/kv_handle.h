/* Korvid core: handles, the C API. One 64-bit value for every handle the engine or a
 * game issues:
 *
 *   63        52 51              32 31                      0
 *   [ kind:12 ] [ generation:20 ] [ index:32 ]
 *
 * Zero is null. Kind 0 is reserved; for generational kinds generation 0 is reserved too.
 * A kind is an issuer: the system instance that can turn its handles into pointers. It
 * registers a resolve function and a context, so anyone holding a handle can resolve it
 * without knowing who issued it, and tools can describe it. Kind ids are assigned in
 * registration order and never reused within a run. Registration is startup work;
 * resolution is lock-free from any thread; unregister only when nothing can still hold
 * the kind in flight (module shutdown).
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t kv_handle;

enum {
    KV_HANDLE_KIND_BITS  = 12,
    KV_HANDLE_GEN_BITS   = 20,
    KV_HANDLE_INDEX_BITS = 32,
    KV_HANDLE_MAX_KIND   = (1 << KV_HANDLE_KIND_BITS) - 1,
    KV_HANDLE_MAX_GEN    = (1 << KV_HANDLE_GEN_BITS) - 1,
};

enum {
    KV_HANDLE_GENERATIONAL = 1, /* generation 0 means null; the issuer bumps it on reuse */
};

typedef struct kv_handle_kind_desc {
    const char* name;     /* for humans; copied, 31 characters kept */
    const void* type_key; /* unique per pointee type; identity only, never dereferenced */
    void*       ctx;      /* the issuer instance, passed back to the functions */
    void* (*resolve)(kv_handle h, void* ctx);                             /* NULL: stale, dead, foreign */
    int   (*describe)(kv_handle h, void* ctx, char* out, size_t cap);     /* optional; returns chars written */
    uint32_t flags;
    /* Optional: what the issuer calls a live handle (NULL: no name, or not live), and the
     * live handle it calls `name` (0: none). Names are stable across runs where ids are not:
     * a save writes a named handle by its name. Lookups are for loading and tools. */
    const char* (*name_of)(kv_handle h, void* ctx);
    kv_handle   (*find_name)(const char* name, void* ctx);
} kv_handle_kind_desc;

static inline kv_handle kv_handle_make(uint16_t kind, uint32_t gen, uint32_t index) {
    return ((kv_handle)(kind & KV_HANDLE_MAX_KIND) << 52) | ((kv_handle)(gen & KV_HANDLE_MAX_GEN) << 32) | index;
}
static inline uint16_t kv_handle_kind(kv_handle h)  { return (uint16_t)(h >> 52); }
static inline uint32_t kv_handle_gen(kv_handle h)   { return (uint32_t)(h >> 32) & KV_HANDLE_MAX_GEN; }
static inline uint32_t kv_handle_index(kv_handle h) { return (uint32_t)h; }

/* Copies the descriptor (name included); the caller's struct may be a temporary. 0 on
 * failure (table full, no resolve function). Warns when three quarters of the kinds are gone. */
uint16_t    kv_handle_kind_register(const kv_handle_kind_desc* desc);
void        kv_handle_kind_unregister(uint16_t kind);
/* The registered copy, valid for the rest of the run; NULL for an unknown kind. */
const kv_handle_kind_desc* kv_handle_kind_desc_get(uint16_t kind);
/* An issuer that moved points its kind at the new location. Atomic: resolves in flight
 * see either the old ctx or the new one, never a torn value. */
void        kv_handle_kind_set_ctx(uint16_t kind, void* ctx);
/* Any kind. NULL for null, stale, unregistered or foreign handles. */
void*       kv_handle_resolve(kv_handle h);
/* Also NULL when the kind's type key is not this one: manual RTTI. */
void*       kv_handle_resolve_as(kv_handle h, const void* type_key);
const char* kv_handle_kind_name(uint16_t kind); /* "" for unknown */
/* The hash of the kind's stored name, 0 for unknown: what a kind is across runs and
 * builds, where its id is only the order of registration. The state hash uses it. */
uint64_t    kv_handle_kind_name_hash(uint16_t kind);
/* The live kind registered under `name` (compared as stored, 31 bytes), the latest if
 * several are; 0 for none. Not fast: for loading a save, not for a tick. */
uint16_t    kv_handle_kind_find(const char* name);
const void* kv_handle_type_key(kv_handle h);    /* NULL for unknown */
/* The issuer's name for a live handle; NULL when it has none or does not name handles. */
const char* kv_handle_name(kv_handle h);
/* The live handle of `kind` its issuer calls `name`; 0 for none. */
kv_handle   kv_handle_find(uint16_t kind, const char* name);
/* Whether the kind's issuer names its handles. */
int         kv_handle_kind_names(uint16_t kind);
int         kv_handle_kind_is_live(uint16_t kind);
uint32_t    kv_handle_kind_count(void);         /* highest id ever assigned + 1 */
/* "name#index:gen" plus the issuer's text; returns chars written, 0 if cap is 0. */
int         kv_handle_describe(kv_handle h, char* out, size_t cap);

#ifdef __cplusplus
}
#endif
