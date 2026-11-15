// Krome's insides: the module's state, a screen, and the Lua side's entry points.
#pragma once
#include "korvid/krome/krome.h"
#include "korvid/core/array.h"
#include "korvid/core/handle.h"

struct lua_State;

namespace kv {

struct TickContext;

// A node whose shown text reads a model key.
struct KromeDep { u32 node; u64 key; };

struct KromeScreen {
    u32        id = 0;          // what Lua's closures name it by
    Name       name;
    i32        layer = KROME_DEFAULT_LAYER;
    KromeTree  tree;
    Array<KromeDep> deps;
    int        env = -2, store = -2, on = -2; // registry refs (LUA_NOREF is -2)
    Handle     io;              // its script's read, while it loads
    char*      source = nullptr; // a source given in memory, until it runs
    usize      source_len = 0;
    bool       loading = false, ready = false, failed = false;
};

struct KromeModule {
    Engine*             e = nullptr;
    Allocator*          general = nullptr;
    lua_State*          L = nullptr;
    Array<KromeScreen*> screens;
    u32                 next_id = 1;
    TickContext*        ctx = nullptr; // the running system's, for emits; null outside one
    u64                 fired = 0, script_errors = 0;
    char                text[512] = {}; // krome_model_text's answer
};

KromeModule*  krome_state(Engine& e);
KromeScreen* krome_screen(KromeModule& s, Name name);
KromeScreen* krome_screen_by_id(KromeModule& s, u32 id);

// ---- the Lua side (lua.cc) ----------------------------------------------------------------------

bool krome_lua_open(KromeModule& s);
void krome_lua_close(KromeModule& s);
// Runs a screen's script and builds its tree; false (logged) when it did not run.
bool krome_lua_run(KromeModule& s, KromeScreen& sc, const char* source, usize len, const char* chunk);
void krome_lua_release(KromeModule& s, KromeScreen& sc); // its refs
// A model value, from Lua or from outside: the store, then every text that reads it.
void krome_model_set_number(KromeModule& s, KromeScreen& sc, const char* key, f64 v, bool boolean);
void krome_model_set_string(KromeModule& s, KromeScreen& sc, const char* key, const char* v);
void krome_model_changed(KromeModule& s, KromeScreen& sc, u64 key);
const char* krome_model_value_text(KromeModule& s, KromeScreen& sc, const char* key, char* out, usize cap);
// A node's event: its handler, then KromeEvent. False when it has none.
bool krome_fire(KromeModule& s, KromeScreen& sc, u32 node, f64 value);
void krome_emit(KromeModule& s, const KromeEvent& ev);

} // namespace kv
