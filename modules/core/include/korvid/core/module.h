// Korvid core: modules, the unit everything in the engine is built from.
//
// A module is a directory with a module.lua, and in code one ModuleDesc: a record of
// tables that say what the module brings. Its components and kinds (data), its systems
// and handlers (code, and when it runs), its events, resources, triggers and CLI
// commands, plus an optional startup and shutdown. The engine reads every module's
// record once, in engine_init, and from then on knows its types by name, schedules its
// code and owns its state. A game is a module too.
//
//   // include/korvid/mygame/module.h
//   namespace kv { KV_MODULE_DECLARE(mygame_module); }
//
//   // src/game.cc
//   KV_MODULE_DEFINE(mygame_module) = {
//       .name     = "mygame",
//       .kinds    = k_kinds,
//       .systems  = k_systems,
//       .handlers = k_handlers,
//       .startup  = mygame_startup,
//       .memory   = {.reserve = 64 * 1024},   // an arena for the module's state
//   };
//
// Nothing else registers it: kormake lists every module an executable depends on, in
// dependency order (kv_module_list), and frameloop_main or tool_main hands the list to
// engine_init. docs/concepts/modules.md walks through every table with examples.
//
// The rules the engine holds every module to:
//   1. Nothing runs before main. A ModuleDesc and every table it points at is constinit,
//      so the compiler rejects anything that would need a dynamic initializer.
//   2. Order is list order, then declaration order. Modules start in list order (so
//      everything a module depends on is up before its startup runs) and stop in
//      reverse. Ids are assigned in that order. Within a phase the order of systems is
//      solved from what they read and write (docs/concepts/scheduling.md); list and
//      declaration order only break ties, and number the units, which is the order
//      their commands and events come out in.
//   3. Ids live in zero-initialized slots (ComponentType<T>::id, a KindDesc's `id`) that
//      engine_init fills and engine_shutdown clears. Id 0 is invalid, so using one
//      before init or after shutdown asserts.
//   4. The engine owns module state. startup builds it in the module's arena and records
//      it with set_state; every system and handler of the module gets it back as
//      ctx.state_as<T>(). Static tables never hold runtime pointers.
#pragma once
#include "korvid/core/arena.h"
#include "korvid/core/cli.h"
#include "korvid/core/component.h"
#include "korvid/core/kind.h"
#include "korvid/core/phase.h"

namespace kv {

struct Engine;
struct ModuleDesc;

// An append resource's owner taking one record that a unit handed to ctx.append. Called
// at the phase boundary, on the main thread, record by record in unit order, while no
// unit runs.
typedef void (*AppendFn)(Engine& e, const void* record, u32 size);

// A named thing systems read or write that is not a column of a kind: an input state, a
// score, a camera. The owner declares it here and points it at an object with
// engine_publish (usually in startup); other modules name it in their sets ("rocks.score")
// and reach it with ctx.resource<T>(name).
//
// With `apply` it is an append resource, for many writers that need no order between
// them: only the owner's systems write it, everyone else declares `appends` and hands it
// records with ctx.append, which reach `apply` at the boundary. audio.commands is one: any
// system may play a sound without being ordered against every other that does.
//   constinit const ResourceDesc k_resources[] = {{".score"}, {".commands", audio_apply}};
struct ResourceDesc {
    const char* name;            // ".score" is "<module>.score"
    AppendFn    apply = nullptr; // set: an append resource
};

// What a module needs from the memory system, declared rather than requested at runtime.
// Which heap its allocations count against is the executable's decision
// (EngineConfig::module_heaps), not the module's.
struct ModuleMemoryDesc {
    usize reserve = 0; // address space for the module's arena; pages commit as used. 0: no arena
};

// The engine's record of a started module, handed to startup and shutdown as `self`.
// Systems don't see it; they get its state through ctx.state_as<T>().
//
//   bool mygame_startup(Engine& engine, ModuleInstance& self) {
//       Game* g = arena_push<Game>(self.arena);   // lives until the module stops
//       if (!g) return false;                      // a false startup fails engine_init
//       self.set_state(g);
//       g->sprites.alloc = &self.general;          // growable containers: the module's allocator
//       return true;
//   }
struct ModuleInstance {
    const ModuleDesc* desc       = nullptr;
    void*             state      = nullptr; // startup sets it with set_state; the arena's release frees it
    const void*       state_type = nullptr; // type key of what set_state stored
    Arena             arena;                // the module's persistent memory, if memory.reserve asked for it
    Allocator         general;              // the general allocator, charged to this module; free what you take in shutdown
    u32               index      = 0;       // position in the module list
    bool              started    = false;
    u32               state_resource = 0;   // id of "<module>.state": declared with SystemDesc::state, never by name
    char              state_name[64];        // that resource's name

    // Records the state and its type, so systems and shutdown get it back checked: a
    // state_as<T> for any other T fails a dev build's check.
    template <class T>
    T* set_state(T* p) { state = p; state_type = type_key<T>(); return p; }
    template <class T>
    T* state_as() const { KV_VERIFY(!state || state_type == type_key<T>()); return (T*)state; }
};

// Everything a module brings, as tables the engine reads in engine_init. Every member has a
// default, so a descriptor names only what it has; a library with nothing to register
// needs no ModuleDesc at all.
struct ModuleDesc {
    const char*               name      = nullptr; // as in module.lua; ".x" in the tables means "<name>.x"
    Span<const EventTypeDesc> events     = {};      // event types: KV_EVENT, KV_EVENT_EX
    Span<const ComponentDesc> components = {}; // schemas; the engine lays out storage from them
    Span<const KindDesc>      kinds      = {}; // entity tables the engine creates from those schemas
    Span<const ResourceDesc>  resources  = {}; // named things that are not columns (above)
    Span<const SystemDesc>    systems    = {}; // code in a phase, with what it reads, writes, appends and emits
    Span<const HandlerDesc>   handlers   = {}; // systems on one event type, scheduled in their phase, handed their batch
    Span<const TriggerDesc>   triggers   = {}; // state edges on component fields, as StateEdge events
    Span<const CliCommandDesc> commands  = {}; // what the CLI can be told; the cli module collects them
    Span<const CliMountDesc>   mounts    = {}; // the module's pieces of the CLI's filesystem
    bool (*startup)(Engine& engine, ModuleInstance& self)  = nullptr; // build state, publish resources; false fails engine_init
    void (*shutdown)(Engine& engine, ModuleInstance& self) = nullptr; // free what startup took from `general`
    ModuleMemoryDesc          memory     = {};
};

} // namespace kv

// Declares a module's record in its header (inside namespace kv), and defines it in one
// source file with designated initializers, so every table is named (example at the top).
// The constinit in the definition is what holds rule 1.
#define KV_MODULE_DECLARE(sym) extern const ::kv::ModuleDesc sym
#define KV_MODULE_DEFINE(sym)  constinit const ::kv::ModuleDesc sym

// The executable's module list, in startup order: generated by kormake into
// gen/<exe>/modules.gen.cc from module.lua's deps. What frameloop_main and tool_main pass
// to engine_init; a test or a custom main passes it (or a list of its own) itself.
kv::Span<const kv::ModuleDesc* const> kv_module_list();
