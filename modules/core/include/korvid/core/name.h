// Korvid core: names. A Name is the 64-bit hash of a string interned once; from then on
// comparing names is comparing integers and a map over names is a map over u64. The
// table keeps the bytes so a name can be printed. Two different strings with one hash is
// a 64-bit collision: detected at intern time, reported, never silent.
#pragma once
#include "korvid/core/arena.h"
#include "korvid/core/map.h"

namespace kv {

struct Name {
    u64 hash = 0; // 0 is the null name

    bool is_null() const { return hash == 0; }
    bool operator==(Name o) const { return hash == o.hash; }
    bool operator!=(Name o) const { return hash != o.hash; }
};
inline u64 hash_key(Name n) { return n.hash; } // already mixed

struct NameTable {
    Arena         strings; // interned bytes, each NUL terminated
    Map<u64, u32> offsets; // hash -> offset into strings
    u32           count = 0;

    bool        init(Allocator* map_storage, usize string_reserve, const char* label = "names");
    void        free();
    Name        intern(const char* s);        // copies on first sight
    Name        intern(const char* s, usize len);
    Name        lookup(const char* s) const;  // null if never interned
    const char* str(Name n) const;            // "" if unknown
};

// The process's table: names for modules, resources, assets and the CLI. Interning
// takes a lock and is startup-ish work; str() and lookup() are plain reads.
Name        name_intern(const char* s);
Name        name_intern(const char* s, usize len);
Name        name_lookup(const char* s);
const char* name_str(Name n);
void        names_shutdown(); // frees the process table; call before mem_shutdown

} // namespace kv
