// Child processes on Windows: not written yet (CreateProcessW with pipes and a job
// object). Goldberg's slice 1 leaves it for when the Windows machine is at hand; until
// then spawn says so, and a cook that needs no shell converter does not ask.
#include "korvid/platform/process.h"
#include "korvid/core/format.h"

namespace kv {

bool process_supported() { return false; }

bool Process::spawn(const ProcessDesc&) {
    format(error, sizeof error, "child processes are not implemented on Windows yet");
    return false;
}
bool Process::running() { return false; }
i32  Process::read_out(void*, u32) { return -1; }
i32  Process::read_err(void*, u32) { return -1; }
void Process::kill() {}
void Process::free() { pid = out_fd = err_fd = -1; exited = false; code = 0; }

} // namespace kv
