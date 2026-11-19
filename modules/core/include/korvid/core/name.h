// Korvid core: names, strings that compare as integers.
//
// A Name is the 64-bit hash of a string interned once; from then on comparing names is
// comparing integers and a map over names is a map over u64. The table keeps the bytes so a
// name can be printed. Modules, resources, assets, UI events and the CLI are named this
// way. The use is: intern where the string arrives (a file, a command, a literal in
// code), keep the Name, and compare Names from then on.
//
// A Name is the string's hash_str (hash.h), or 1 in the one case where that is 0, so the
// same string makes the same Name in every run and on every platform: a Name can be saved,
// recorded or sent, and compared with one made elsewhere. Two different strings with one
// hash is a 64-bit collision: detected at intern time, reported, never silent.
//
// A literal compared against names arriving at run time, interned once (from
// examples/arcanum/src/game.cc, which handles UI events):
//
//   static const u64 k_select = name_intern("select").hash;
//   for (const UiEvent& ev : events) {
//       if (ev.event != k_select) continue;
//       const char* item = name_str(Name{ev.node}); // printable again
//       ...
//   }
#pragma once
#include "korvid/core/arena.h"
#include "korvid/core/map.h"

namespace kv {

// An interned string, by its hash. 8 bytes of plain data, compared with ==.
struct Name {
    u64 hash = 0; // 0 is the null name

    // The null name: no string, or one that was never interned.
    bool is_null() const { return hash == 0; }
    // Equal when the strings are (a collision is caught when interning).
    bool operator==(Name o) const { return hash == o.hash; }
    bool operator!=(Name o) const { return hash != o.hash; }
};
// Map's hash for a Name: the hash itself.
inline u64 hash_key(Name n) { return n.hash; } // already mixed

// A table of names. The process has one (below) and most code uses that; a table of your
// own keeps a set of names apart, and takes no lock.
struct NameTable {
    Arena         strings; // interned bytes, each NUL terminated
    Map<u64, u32> offsets; // hash -> offset into strings
    u32           count = 0; // names interned

    // Sets up an empty table: the map in `map_storage` (null: the general allocator), and
    // `string_reserve` bytes of address space for the strings, committed as they fill;
    // `label` names the arena. False when the address space cannot be reserved.
    bool        init(Allocator* map_storage, usize string_reserve, const char* label = "names");
    // Frees the strings and the map; every string str() gave out goes with them.
    void        free();
    // The Name of `s`, copied into the table the first time it is seen. The null name for
    // a null `s`, and when the strings are full (logged: "name table is full"). Another
    // string with the same hash is logged as a collision and, in debug and dev builds,
    // stops the process; a ship build returns the Name already there.
    Name        intern(const char* s);        // copies on first sight
    // The same for `len` bytes at `s`, which need not be NUL-terminated.
    Name        intern(const char* s, usize len);
    // The Name of `s` if it was interned; never interns.
    Name        lookup(const char* s) const;  // null if never interned
    // The interned string, valid until free(); strings do not move as the table grows.
    const char* str(Name n) const;            // "" if unknown
};

// ---- the process's table ---------------------------------------------------------------
// Names for modules, resources, assets, UI events and the CLI, made on first use, with
// 16 MiB for strings. These behave as NameTable's functions do. Any thread may call them;
// each takes the table's lock (a spin lock), so interning is startup-ish work: keep the
// Name rather than interning the same string every tick.

// The Name of `s` in the process's table, copied the first time it is seen.
Name        name_intern(const char* s);
// The same for `len` bytes at `s`, which need not be NUL-terminated.
Name        name_intern(const char* s, usize len);
// The Name of `s` if it was interned; the null name if not.
Name        name_lookup(const char* s);
// The interned string, valid until names_shutdown; "" for the null name or an unknown one.
const char* name_str(Name n);
// The frame loop's and the tool runner's mains call this at exit; a program with a main
// of its own calls it itself.
void        names_shutdown(); // frees the process table; call before mem_shutdown

} // namespace kv
