// Korvid core: what an OS error is called. Every blocking call in the engine that can fail
// says why with an errno value, the same on every OS (the platform module translates
// Windows' own), in a last `i32* err` or a completion's `error`. This is the text for one,
// so that a caller needs neither <string.h> nor the C library's strerror, whose words differ
// from one platform to the next and whose buffer is not every thread's own.
#pragma once
#include "korvid/core/types.h"

namespace kv {

// The words for an errno value: "no such file or directory", "address in use". The same
// text on every platform, lower case, no full stop; never null. A value it has no words
// for is "error N", in a buffer that is the calling thread's until its next such call. 0 is
// "no error".
const char* os_error_text(i32 err);

} // namespace kv
