// Korvid core: generational handles. Used across every boundary instead of pointers.
#pragma once
#include "korvid/core/types.h"

namespace kv {

// gen == 0 is the null handle. Live generations start at 1 and skip 0 on wrap.
struct Handle {
    u32 index = 0;
    u32 gen   = 0;

    bool is_null() const { return gen == 0; }
    bool operator==(const Handle& o) const { return index == o.index && gen == o.gen; }
    bool operator!=(const Handle& o) const { return !(*this == o); }
};

} // namespace kv
