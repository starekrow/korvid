#include "korvid/core/thread.h"
#include "korvid/core/atomic.h"

namespace kv {

static u32 g_next_thread_id = 0; // atomic
static thread_local u32 t_thread_id = 0;
static thread_local bool t_realtime = false;

u32 thread_id() {
    if (t_thread_id == 0) t_thread_id = kv::atomic_add_fetch(&g_next_thread_id, 1, kv::MO_RELAXED);
    return t_thread_id;
}

void thread_set_realtime(bool rt) { t_realtime = rt; }
bool thread_is_realtime() { return t_realtime; }

} // namespace kv
