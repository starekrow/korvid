// Korvid core: modules, the unit everything in the engine is built from.
//
// A module is a directory with a module.lua, and in code one ModuleDesc: a record of
// tables that say what the module brings. Its components and ktables (data), its systems
// and handlers (code, and when it runs), its events, resources, triggers and CLI
// commands, plus an optional startup and shutdown. A module of many files can spread its
// tables, and what it loads at startup, over parts, each declared beside the code it
// describes (ModulePart). The engine reads every module's record once, in engine_init,
// and from then on knows its types by name, schedules its code and owns its state. A game
// is a module too.
//
//   // include/korvid/mygame/module.h
//   namespace kv { KV_MODULE_DECLARE(mygame_module); }
//
//   // src/game.cc
//   KV_MODULE_DEFINE(mygame_module) = {
//       .name     = "mygame",
//       .ktables  = k_ktables,
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
//      reverse. Ids are assigned in that order, a module's own tables before its parts'
//      (ModulePart), the parts in the order it lists them; ktables alone go by full name,
//      so moving one between files or modules never renumbers them or changes the order
//      a view walks them in. Within a phase the order of systems is
//      solved from what they read and write (docs/concepts/scheduling.md); list and
//      declaration order only break ties, and number the units, which is the order
//      their commands and events come out in.
//   3. Ids live in zero-initialized slots (ComponentType<T>::id, a KTableDesc's `id`) that
//      engine_init fills and engine_shutdown clears. Id 0 is invalid, so using one
//      before init or after shutdown asserts.
//   4. The engine owns module state. startup builds it in the module's arena and records
//      it with set_state; every system and handler of the module gets it back as
//      ctx.state_as<T>(). Static tables never hold runtime pointers.
#pragma once
#include "korvid/core/arena.h"
#include "korvid/core/cli.h"
#include "korvid/core/component.h"
#include "korvid/core/ktable.h"
#include "korvid/core/phase.h"

namespace kv {

struct Engine;
struct ModuleDesc;
struct ModulePart;

// An append resource's owner taking one record that a unit handed to ctx.append. Called
// at the phase boundary, on the main thread, record by record in unit order, while no
// unit runs.
typedef void (*AppendFn)(Engine& e, const void* record, u32 size);

// A named thing systems read or write that is not a column of a ktable: an input state, a
// score, a camera. The owner declares it here and points it at an object with
// engine_publish (usually in startup); other modules name it in their sets ("rocks.score")
// and reach it with ctx.resource<T>(name).
//
// With `fields`, a field table for the published object (component.h), the resource is
// sim state like a ktable's column: part of the state hash, so a score kept here is checked
// by --hash, thread counts and replays. The object must then be plain data, which
// engine_publish checks.
//   constinit const FieldDesc k_score_fields[] = {KV_FIELDS(Score, total, wave, rocks_shot)};
//   constinit const ResourceDesc k_resources[] = {{.name = ".score", .fields = k_score_fields}};
//
// With `cosmetic` as well, the field table only describes the object: what describe and a
// script read it by, never hashed, and the object need not be plain data (a transport that
// follows a device's clock). It is then what a resource without fields is to the sim.
//   constinit const ResourceDesc k_resources[] = {{.name = ".transport", .fields = k_transport_fields, .cosmetic = true}};
//
// With `apply` it is an append resource, for many writers that need no order between
// them: only the owner's systems write it, everyone else declares `appends` and hands it
// records with ctx.append, which reach `apply` at the boundary. audio.commands is one: any
// system may play a sound without being ordered against every other that does.
//   constinit const ResourceDesc k_resources[] = {{".score"}, {".commands", audio_apply}};
struct ResourceDesc {
    const char*           name;            // ".score" is "<module>.score"
    AppendFn              apply  = nullptr; // set: an append resource
    Span<const FieldDesc> fields = {};      // set: part of the state hash; the object is plain data (checked at publish)
    bool                  cosmetic = false; // with fields: they describe the object, which is not hashed and need not be plain
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
    Rng               rng;                  // the module's sim stream (ctx.rng()), seeded by engine_init
    char              state_name[64];        // that resource's name
    Span<const ModulePart* const> parts = {}; // the module's parts, for their startups and shutdowns
    u32               parts_up   = 0;       // how many of those have started

    // Records the state and its type, so systems and shutdown get it back checked: a
    // state_as<T> for any other T fails a dev build's check.
    template <class T>
    T* set_state(T* p) { state = p; state_type = type_key<T>(); return p; }
    template <class T>
    T* state_as() const { KV_VERIFY(!state || state_type == type_key<T>()); return (T*)state; }
};

// A piece of a module's tables, declared beside the code it describes: a source file's
// components, ktables, systems and handlers, next to the systems and handlers themselves.
// The module lists its parts (ModuleDesc::parts) and engine_init joins them to its own
// tables, the module's first and then each part's in list order, as if one record had
// declared them all: the same names (".x" is still "<module>.x"), the same checks, ids
// in that order. A part has no name or memory of its own; those are the module's.
//
// It may have a startup and a shutdown: what its file loads and lets go of. A part's
// startup runs after the module's, the parts in list order, and is handed the module's
// instance, so the state the module's startup made is there to fill (self.state_as<T>()).
// A false one fails engine_init, which shuts down what started before it. Shutdown is the
// reverse: the parts' shutdowns, last part first, then the module's.
//
//   // ship.cc
//   constinit const KTableDesc k_ktables[] = {{.name = ".ship", ...}};
//   constinit const SystemDesc k_systems[] = {{.name = ".control", .fn = control, ...}};
//   bool ship_startup(Engine& e, ModuleInstance& self) {
//       self.state_as<Game>()->ship_tex = asset_load_texture(e, "mygame/ship.png");
//       return true;
//   }
//   KV_MODULE_PART(mygame_ship) = {.ktables = k_ktables, .systems = k_systems, .startup = ship_startup};
//
//   // game.cc
//   constinit const ModulePart* const k_parts[] = {&mygame_ship, &mygame_boss};
//   KV_MODULE_DEFINE(mygame_module) = {.name = "mygame", .parts = k_parts, .startup = ...};
struct ModulePart {
    Span<const EventTypeDesc>  events     = {};
    Span<const ComponentDesc>  components = {};
    Span<const KTableDesc>     ktables    = {};
    Span<const ResourceDesc>   resources  = {};
    Span<const SystemDesc>     systems    = {};
    Span<const HandlerDesc>    handlers   = {};
    Span<const TriggerDesc>    triggers   = {};
    Span<const CliCommandDesc> commands   = {};
    Span<const CliMountDesc>   mounts     = {};
    bool (*startup)(Engine& engine, ModuleInstance& self)  = nullptr; // after the module's; false fails engine_init
    void (*shutdown)(Engine& engine, ModuleInstance& self) = nullptr; // before the module's
};

// Everything a module brings, as tables the engine reads in engine_init. Every member has a
// default, so a descriptor names only what it has; a library with nothing to register
// needs no ModuleDesc at all.
struct ModuleDesc {
    const char*               name      = nullptr; // as in module.lua; ".x" in the tables means "<name>.x"
    Span<const EventTypeDesc> events     = {};      // event types: KV_EVENT, KV_EVENT_EX
    Span<const ComponentDesc> components = {}; // schemas; the engine lays out storage from them
    Span<const KTableDesc>    ktables    = {}; // the ktables the engine creates from those schemas
    Span<const ResourceDesc>  resources  = {}; // named things that are not columns (above)
    Span<const SystemDesc>    systems    = {}; // code in a phase, with what it reads, writes, appends and emits
    Span<const HandlerDesc>   handlers   = {}; // systems on one event type, scheduled in their phase, handed their batch
    Span<const TriggerDesc>   triggers   = {}; // state edges on component fields, as StateEdge events
    Span<const CliCommandDesc> commands  = {}; // what the CLI can be told; the cli module collects them
    Span<const CliMountDesc>   mounts    = {}; // the module's pieces of the CLI's filesystem
    Span<const ModulePart* const> parts  = {}; // more of the tables above, and startups, from other files (ModulePart)
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
// The same for a part: declared where the module's list of parts can see it, defined in
// the file whose code it describes.
#define KV_MODULE_PART_DECLARE(sym) extern const ::kv::ModulePart sym
#define KV_MODULE_PART(sym)         constinit const ::kv::ModulePart sym

// The executable's module list, in startup order: generated by kormake into
// gen/<exe>/modules.gen.cc from module.lua's deps. What frameloop_main and tool_main pass
// to engine_init; a test or a custom main passes it (or a list of its own) itself.
kv::Span<const kv::ModuleDesc* const> kv_module_list();
