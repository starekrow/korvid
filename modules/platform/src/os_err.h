// Korvid platform: the one way a blocking call says why it failed. Its last parameter is
// `i32* err`, an errno on every OS; a failure sets it if it was given, success leaves it.
#pragma once
#include "korvid/core/types.h"

namespace kv {

// `return os_fail(err, ENOENT);`
inline bool os_fail(i32* err, i32 why) { if (err) *err = why; return false; }

} // namespace kv
