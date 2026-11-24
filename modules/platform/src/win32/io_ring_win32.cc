// Korvid platform: Windows I/O rings behind IoQueue (ioringapi.h, Windows 11). The same
// shape as Linux's io_uring, call for call: build a read or a write with a user value,
// submit once per pump, pop the completions. Version 3 (Windows 11 22H2) is the first with
// writes, so it is the floor; below it, or on Windows 10, create returns null and the queue
// reads synchronously, as Linux does when the kernel refuses a ring.
//
// The functions come from KernelBase.dll by name, not from an import library, so an
// executable starts on a Windows without them. The handles are os_file_open's, which are
// FILE_FLAG_OVERLAPPED for this: a ring accepts a synchronous handle too, but then Windows
// performs its reads one after another inside SubmitIoRing, and nothing is asynchronous
// (file_win32.cc has the numbers). The sync path, the pack mounts and the ring share them.
//
// Results: a completion's HRESULT and byte count become the queue's convention, bytes or a
// negative errno. A read that starts at or past the end of the file fails with
// ERROR_HANDLE_EOF; that is 0 bytes, the end, as a read() past the end is on Linux.
#include "../io_ring.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "wide.h"
#include <ioringapi.h>

namespace kv {

namespace {

typedef decltype(&CreateIoRing)            RingCreateFn;
typedef decltype(&QueryIoRingCapabilities) RingCapsFn;
typedef decltype(&BuildIoRingReadFile)     RingReadFn;
typedef decltype(&BuildIoRingWriteFile)    RingWriteFn;
typedef decltype(&SubmitIoRing)            RingSubmitFn;
typedef decltype(&PopIoRingCompletion)     RingPopFn;
typedef decltype(&CloseIoRing)             RingCloseFn;

struct RingApi {
    bool          loaded = false, present = false;
    RingCreateFn  create = nullptr;
    RingCapsFn    caps   = nullptr;
    RingReadFn    read   = nullptr;
    RingWriteFn   write  = nullptr;
    RingSubmitFn  submit = nullptr;
    RingPopFn     pop    = nullptr;
    RingCloseFn   close  = nullptr;
};
RingApi g_ring_api;

// Looked up once, on the thread that makes the first queue (the main thread, at startup).
const RingApi& ring_api() {
    RingApi& a = g_ring_api;
    if (a.loaded) return a;
    a.loaded = true;
    HMODULE k = GetModuleHandleW(L"kernelbase.dll");
    if (!k) return a;
    a.create = (RingCreateFn)(void*)GetProcAddress(k, "CreateIoRing");
    a.caps   = (RingCapsFn)(void*)GetProcAddress(k, "QueryIoRingCapabilities");
    a.read   = (RingReadFn)(void*)GetProcAddress(k, "BuildIoRingReadFile");
    a.write  = (RingWriteFn)(void*)GetProcAddress(k, "BuildIoRingWriteFile");
    a.submit = (RingSubmitFn)(void*)GetProcAddress(k, "SubmitIoRing");
    a.pop    = (RingPopFn)(void*)GetProcAddress(k, "PopIoRingCompletion");
    a.close  = (RingCloseFn)(void*)GetProcAddress(k, "CloseIoRing");
    a.present = a.create && a.caps && a.read && a.write && a.submit && a.pop && a.close;
    return a;
}

HANDLE ring_file_handle(KHandle f) { return (HANDLE)(intptr_t)f.os(); }

constexpr IORING_VERSION kRingVersionWrites = (IORING_VERSION)300; // IORING_VERSION_3

} // namespace

// A failed build is answered by a completion of its own, popped before the ring's, so a
// request is never lost between push and pop. The queue keeps can_push true only while
// there is room, so this is for errors, not for a full queue.
struct IoRing {
    HIORING handle = nullptr;
    u32     entries = 0;        // the submission queue's size
    u32     unsubmitted = 0;    // built since the last submit
    u32     in_flight = 0;      // built and not yet popped, synthetic ones included
    struct Failed { u64 user; i32 res; };
    Failed  failed[64];
    u32     failed_count = 0;
};

namespace {

void ring_fail(IoRing* r, u64 user, HRESULT hr) {
    i32 err = HRESULT_FACILITY(hr) == FACILITY_WIN32 ? errno_from_win32(HRESULT_CODE(hr)) : EIO;
    if (r->failed_count < 64) r->failed[r->failed_count++] = IoRing::Failed{user, -err};
    else KV_ERROR("io", "I/O ring: a failed submission could not be answered (%#lx)", (unsigned long)hr);
}

} // namespace

IoRing* io_ring_create(u32 entries) {
    const RingApi& a = ring_api();
    if (!a.present) { KV_INFO("io", "Windows I/O rings unavailable (no ioringapi in KernelBase): synchronous reads"); return nullptr; }
    IORING_CAPABILITIES caps{};
    if (FAILED(a.caps(&caps)) || caps.MaxVersion < kRingVersionWrites) {
        KV_INFO("io", "Windows I/O rings at version %d, writes need %d: synchronous reads", (int)caps.MaxVersion, (int)kRingVersionWrites);
        return nullptr;
    }
    if (entries > caps.MaxSubmissionQueueSize) entries = caps.MaxSubmissionQueueSize;
    u32 cq = entries * 2 <= caps.MaxCompletionQueueSize ? entries * 2 : caps.MaxCompletionQueueSize;
    IORING_CREATE_FLAGS flags{IORING_CREATE_REQUIRED_FLAGS_NONE, IORING_CREATE_ADVISORY_FLAGS_NONE};
    HIORING h = nullptr;
    HRESULT hr = a.create(kRingVersionWrites, flags, entries, cq, &h);
    if (FAILED(hr)) { KV_INFO("io", "CreateIoRing failed (%#lx): synchronous reads", (unsigned long)hr); return nullptr; }
    IoRing* r = alloc_items<IoRing>(mem_general());
    *r = IoRing{};
    r->handle  = h;
    r->entries = entries;
    KV_INFO("io", "Windows I/O ring: %u entries, version %d of %d", entries, (int)kRingVersionWrites, (int)caps.MaxVersion);
    return r;
}

void io_ring_destroy(IoRing* ring) {
    if (!ring) return;
    if (ring->handle) ring_api().close(ring->handle);
    free_items(mem_general(), ring);
}

bool io_ring_can_push(const IoRing* ring) {
    return ring && ring->unsubmitted < ring->entries && ring->in_flight < ring->entries;
}

void io_ring_push_read(IoRing* ring, KHandle fd, void* buf, u32 len, u64 offset, u64 user) {
    ++ring->in_flight;
    ++ring->unsubmitted;
    HRESULT hr = ring_api().read(ring->handle, IoRingHandleRefFromHandle(ring_file_handle(fd)), IoRingBufferRefFromPointer(buf), len, offset,
                                 (UINT_PTR)user, IOSQE_FLAGS_NONE);
    if (FAILED(hr)) { --ring->unsubmitted; ring_fail(ring, user, hr); }
}

void io_ring_push_write(IoRing* ring, KHandle fd, const void* buf, u32 len, u64 offset, u64 user) {
    ++ring->in_flight;
    ++ring->unsubmitted;
    // The builder takes the buffer as writable; it only reads it.
    HRESULT hr = ring_api().write(ring->handle, IoRingHandleRefFromHandle(ring_file_handle(fd)), IoRingBufferRefFromPointer((void*)buf), len,
                                  offset, FILE_WRITE_FLAGS_NONE, (UINT_PTR)user, IOSQE_FLAGS_NONE);
    if (FAILED(hr)) { --ring->unsubmitted; ring_fail(ring, user, hr); }
}

bool io_ring_enter(IoRing* ring, u32 min_complete) {
    if (!ring) return false;
    // Wait only for what the kernel holds: a failed build's answer is already here.
    u32 real = ring->in_flight - ring->failed_count;
    if (min_complete > real) min_complete = real;
    UINT32 submitted = 0;
    HRESULT hr = ring_api().submit(ring->handle, min_complete, min_complete ? INFINITE : 0, &submitted);
    ring->unsubmitted = 0;
    // A wait that ran out is not an error for a caller that asked for none.
    return SUCCEEDED(hr) || (min_complete == 0 && hr == HRESULT_FROM_WIN32(WAIT_TIMEOUT));
}

bool io_ring_pop(IoRing* ring, u64* user, i32* res) {
    if (!ring) return false;
    if (ring->failed_count) {
        IoRing::Failed f = ring->failed[--ring->failed_count];
        *user = f.user;
        *res  = f.res;
        --ring->in_flight;
        return true;
    }
    IORING_CQE cqe{};
    if (ring_api().pop(ring->handle, &cqe) != S_OK) return false;
    *user = (u64)cqe.UserData;
    if (SUCCEEDED(cqe.ResultCode)) *res = cqe.Information > 0x7FFFFFFFu ? 0x7FFFFFFF : (i32)cqe.Information;
    else if (cqe.ResultCode == HRESULT_FROM_WIN32(ERROR_HANDLE_EOF)) *res = 0;
    else *res = -(HRESULT_FACILITY(cqe.ResultCode) == FACILITY_WIN32 ? errno_from_win32(HRESULT_CODE(cqe.ResultCode)) : EIO);
    --ring->in_flight;
    return true;
}

u32 io_ring_in_flight(const IoRing* ring) { return ring ? ring->in_flight : 0; }
u32 io_ring_entries(const IoRing* ring) { return ring ? ring->entries : 0; }
const char* io_ring_name(const IoRing* ring) { return ring ? "ioring" : "sync"; }

} // namespace kv
