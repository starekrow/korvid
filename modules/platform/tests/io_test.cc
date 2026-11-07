#include "korvid/platform/io.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace {

struct Files { char dir[64]; char a[256], b[256], missing[256]; };

kv::u32 wait_for(kv::IoQueue& q, kv::u32 count) {
    for (kv::u32 i = 0; i < 2000 && q.completions().len < count; ++i) { q.pump(); kv::time_sleep(0.001); }
    return q.completions().len;
}

const kv::IoCompletion* by_user(const kv::IoQueue& q, kv::u64 user) {
    for (const kv::IoCompletion& c : q.completions()) if (c.user == user) return &c;
    return nullptr;
}

// The whole contract, on whichever backend the queue chose.
void test_backend(kv::TestCtx& c, const Files& f, kv::IoBackend backend) {
    using namespace kv;
    IoQueue q;
    KV_CHECK(c, q.init(nullptr, 64, backend) && q.kind != 0);
    if (backend == IoBackend::Sync) KV_CHECK(c, strcmp(q.backend_name(), "sync") == 0);

    // Nothing completes inline: submissions are pending until a pump.
    IoRequest ra; ra.path = f.a; ra.user = 1; ra.priority = IoPriority::Low;
    IoRequest rb; rb.path = f.b; rb.user = 2; rb.priority = IoPriority::High;
    IoRequest rm; rm.path = f.missing; rm.user = 3;
    IoRequest rc; rc.path = f.a; rc.user = 4;
    Handle ha = q.read_file(ra), hb = q.read_file(rb), hm = q.read_file(rm), hc = q.read_file(rc);
    KV_CHECK(c, !ha.is_null() && !hb.is_null() && !hm.is_null() && !hc.is_null());
    KV_CHECK(c, q.pending() == 4 && q.completions().len == 0);
    KV_CHECK(c, q.cancel(hc) && !q.cancel(hc));
    KV_CHECK(c, resolve(ha) != nullptr); // the request is a handle like any other

    KV_CHECK(c, wait_for(q, 4) == 4 && q.pending() == 0);
    Span<const IoCompletion> done = q.completions();
    if (backend == IoBackend::Sync) // the synchronous path performs in priority order: High, Normal by submission, Low
        KV_CHECK(c, done[0].user == 2 && done[1].user == 3 && done[2].user == 4 && done[3].user == 1);
    const IoCompletion* cb = by_user(q, 2);
    const IoCompletion* cm = by_user(q, 3);
    const IoCompletion* cc = by_user(q, 4);
    const IoCompletion* ca = by_user(q, 1);
    KV_CHECK(c, cb && cb->status == IoStatus::Done && cb->size == 0);
    KV_CHECK(c, cm && cm->status == IoStatus::Failed && cm->error != 0);
    KV_CHECK(c, cc && cc->status == IoStatus::Cancelled && cc->data == nullptr);
    KV_CHECK(c, ca && ca->status == IoStatus::Done && ca->size == 8 && memcmp(ca->data, "hello io", 8) == 0);
    KV_CHECK(c, q.reads_done == 2 && q.reads_failed == 1);
    mem_free(cb->data);
    mem_free(ca->data);

    // Completions persist across pumps until reaped; reaping retires the request.
    q.pump();
    KV_CHECK(c, q.completions().len == 4 && resolve(ha) != nullptr);
    KV_CHECK(c, q.reap(ha) && !q.reap(ha) && q.completions().len == 3 && resolve(ha) == nullptr);
    KV_CHECK(c, q.reap(hb) && q.reap(hm) && q.reap(hc) && q.completions().len == 0);

    // A path that does not fit is refused up front.
    char too_long[300]; memset(too_long, 'x', sizeof too_long - 1); too_long[sizeof too_long - 1] = 0;
    IoRequest rl; rl.path = too_long;
    KV_CHECK(c, q.read_file(rl).is_null());

    // A larger file comes back whole, and a cancel while in flight still completes.
    char big_path[256];
    snprintf(big_path, sizeof big_path, "%s/big.bin", f.dir);
    const u32 big = 3 * 1024 * 1024 + 123;
    {
        u8* buf = (u8*)mem_alloc(big);
        for (u32 i = 0; i < big; ++i) buf[i] = (u8)(i * 7 + (i >> 8));
        FILE* fp = fopen(big_path, "wb"); fwrite(buf, 1, big, fp); fclose(fp);
        mem_free(buf);
    }
    IoRequest rbig; rbig.path = big_path; rbig.user = 5;
    IoRequest rcan; rcan.path = big_path; rcan.user = 6;
    Handle hbig = q.read_file(rbig), hcan = q.read_file(rcan);
    q.pump();                 // hands both to the backend
    q.cancel(hcan);           // in flight on the ring, queued nowhere on the sync path (already done)
    KV_CHECK(c, wait_for(q, 2) == 2);
    const IoCompletion* cbig = by_user(q, 5);
    KV_CHECK(c, cbig && cbig->status == IoStatus::Done && cbig->size == big);
    bool ok = cbig && cbig->size == big;
    for (u32 i = 0; ok && i < big; i += 4099) ok = ((const u8*)cbig->data)[i] == (u8)(i * 7 + (i >> 8));
    KV_CHECK(c, ok);
    const IoCompletion* ccan = by_user(q, 6);
    KV_CHECK(c, ccan && (ccan->status == IoStatus::Cancelled || ccan->status == IoStatus::Done));
    if (cbig) mem_free(cbig->data);
    if (ccan && ccan->data) mem_free(ccan->data);
    KV_CHECK(c, q.reap(hbig) && q.reap(hcan));
    unlink(big_path);

    q.free();
}

} // namespace

void test_io(kv::TestCtx& c) {
    using namespace kv;
    c.begin("io");

    // Two files to read, one with a known payload.
    Files f;
    strcpy(f.dir, "/tmp/korvid-io-XXXXXX");
    KV_CHECK(c, mkdtemp(f.dir) != nullptr);
    snprintf(f.a, sizeof f.a, "%s/a.bin", f.dir);
    snprintf(f.b, sizeof f.b, "%s/b.bin", f.dir);
    snprintf(f.missing, sizeof f.missing, "%s/nope.bin", f.dir);
    FILE* fp = fopen(f.a, "wb"); fwrite("hello io", 1, 8, fp); fclose(fp);
    fp = fopen(f.b, "wb"); fclose(fp); // empty

    test_backend(c, f, IoBackend::Sync);
    test_backend(c, f, IoBackend::Auto); // io_uring where the kernel allows it, sync otherwise

    // What the default picked, for the log.
    IoQueue q;
    KV_CHECK(c, q.init(nullptr, 8));
    KV_INFO("test", "io: default backend is %s", q.backend_name());
    q.free();

    unlink(f.a); unlink(f.b); rmdir(f.dir);
}
