// Korvid core: module registration. A module declares everything it contributes as data:
// one constinit ModuleDesc with tables of event types, resources and systems, plus
// startup/shutdown for OS resources and module state. Nothing registers itself.
//
// Rules, stated once:
//   1. Nothing runs before main. Every ModuleDesc and every table it points at is
//      constinit; the compiler rejects anything that would need a dynamic initializer.
//   2. The build generates the module list (example_testbed/src/modules.gen.cc stands in by hand),
//      in dependency order. engine_init is the only thing that reads it.
//   3. Order is list order, then declaration order. Modules start in list order and stop
//      in reverse. Event, component and kind ids are assigned in that order. Within a tick phase, systems run
//      in module order, then in the order written in the module's table.
//   4. Ids live in zero-initialized slots that engine_init fills and engine_shutdown
//      clears. Id 0 is invalid, so use before init or after shutdown asserts.
//   5. Module state is engine-owned: it lives in the module's arena, which engine_init
//      creates from ModuleDesc::memory and hands to startup in the ModuleInstance. Each
//      system receives its own module's state. Static tables never hold runtime pointers.
#pragma once
#include "korvid/core/arena.h"
#include "korvid/core/cli.h"
#include "korvid/core/component.h"
#include "korvid/core/kind.h"
#include "korvid/core/phase.h"

namespace kv {

struct Engine;
struct ModuleDesc;

// Something systems read or write: a component store, an input state, a world.
// Declared by the module that owns it; referenced by name in system read/write sets.
struct ResourceDesc {
    const char* name;
    bool        sink = false; // a command queue many units append to: writers need no order between them; appends land in unit order
};

// What a module needs from the memory system. Declared, not requested at runtime.
// Which heap it lands in is the root's budgeting decision, not the module's (pending).
struct ModuleMemoryDesc {
    usize reserve = 0; // address space for the module's arena; pages commit as it is used
};

// The engine's record of a started module. Passed to startup/shutdown and reachable
// from TickContext::state (for the system's own module).
struct ModuleInstance {
    const ModuleDesc* desc       = nullptr;
    void*             state      = nullptr; // startup sets it with set_state; the arena's release frees it
    const void*       state_type = nullptr; // type key of what set_state stored
    Arena             arena;                // the module's persistent memory, if it declared any
    Allocator         general;              // the general allocator, attributed to this module
    u32               index      = 0;       // position in the module list
    bool              started    = false;
    u32               state_resource = 0;   // id of "<module>.state", the implicit resource its systems share
    char              state_name[64];        // that resource's name

    // Records the state and its type, so systems and shutdown get it back checked.
    template <class T>
    T* set_state(T* p) { state = p; state_type = type_key<T>(); return p; }
    template <class T>
    T* state_as() const { KV_VERIFY(!state || state_type == type_key<T>()); return (T*)state; }
};

// Every member has a default so a descriptor names only what it has.
struct ModuleDesc {
    const char*               name      = nullptr;
    Span<const EventTypeDesc> events     = {};
    Span<const ComponentDesc> components = {}; // schemas; the engine lays out storage from them
    Span<const KindDesc>      kinds      = {}; // entity tables the engine creates from those schemas
    Span<const ResourceDesc>  resources  = {};
    Span<const SystemDesc>    systems    = {};
    Span<const HandlerDesc>   handlers   = {}; // run at phase boundaries, on the events they subscribe to
    Span<const TriggerDesc>   triggers   = {}; // state edges on component fields, as StateEdge events
    Span<const CliCommandDesc> commands  = {}; // what the CLI can be told; the cli module collects them
    bool (*startup)(Engine& engine, ModuleInstance& self)  = nullptr;
    void (*shutdown)(Engine& engine, ModuleInstance& self) = nullptr;
    ModuleMemoryDesc          memory     = {};
};

} // namespace kv

// Declares a module's symbol in its header, and defines it in one source file, with
// designated initializers so every table is named:
//   KV_MODULE_DECLARE(platform_module);                          // header, inside namespace kv
//   KV_MODULE_DEFINE(platform_module) = {.name = "platform", .events = k_events, ...};
// constinit is the guarantee behind rule 1.
#define KV_MODULE_DECLARE(sym) extern const ::kv::ModuleDesc sym
#define KV_MODULE_DEFINE(sym)  constinit const ::kv::ModuleDesc sym

// Defined by the generated module list of the executable being built, in startup order.
kv::Span<const kv::ModuleDesc* const> kv_module_list();
