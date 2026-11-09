#include "korvid/core/os_thread.h"
#include "korvid/core/thread.h"
#include <pthread.h>
#include <string.h>
#include <unistd.h>

namespace kv {

static_assert(sizeof(pthread_mutex_t) <= sizeof(OsMutex::storage), "OsMutex is too small for pthread_mutex_t");
static_assert(sizeof(pthread_cond_t) <= sizeof(OsCond::storage), "OsCond is too small for pthread_cond_t");
static_assert(sizeof(pthread_t) <= sizeof(u64), "pthread_t does not fit OsThread::handle");

static pthread_mutex_t* native(OsMutex* m) { return reinterpret_cast<pthread_mutex_t*>(m->storage); }
static pthread_cond_t*  native(OsCond* c)  { return reinterpret_cast<pthread_cond_t*>(c->storage); }

u32 os_cpu_count() {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (u32)n : 1;
}

void os_mutex_init(OsMutex* m)    { pthread_mutex_init(native(m), nullptr); }
void os_mutex_destroy(OsMutex* m) { pthread_mutex_destroy(native(m)); }
void os_mutex_lock(OsMutex* m)    { pthread_mutex_lock(native(m)); }
void os_mutex_unlock(OsMutex* m)  { pthread_mutex_unlock(native(m)); }

void os_cond_init(OsCond* c)                { pthread_cond_init(native(c), nullptr); }
void os_cond_destroy(OsCond* c)             { pthread_cond_destroy(native(c)); }
void os_cond_wait(OsCond* c, OsMutex* m)    { pthread_cond_wait(native(c), native(m)); }
void os_cond_signal(OsCond* c)              { pthread_cond_signal(native(c)); }
void os_cond_broadcast(OsCond* c)           { pthread_cond_broadcast(native(c)); }

static void* thread_start(void* arg) {
    OsThread* t = (OsThread*)arg;
    t->fn(t->arg);
    return nullptr;
}

bool os_thread_start(OsThread* t, OsThreadFn fn, void* arg) {
    t->fn = fn;
    t->arg = arg;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    pthread_t tid;
    int rc = pthread_create(&tid, &attr, thread_start, t);
    pthread_attr_destroy(&attr);
    if (rc != 0) return false;
    t->handle = 0;
    memcpy(&t->handle, &tid, sizeof tid);
    return true;
}

void os_thread_join(OsThread* t) {
    pthread_t tid;
    memcpy(&tid, &t->handle, sizeof tid);
    pthread_join(tid, nullptr);
    t->handle = 0;
}

} // namespace kv
