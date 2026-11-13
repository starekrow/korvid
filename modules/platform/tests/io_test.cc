#include "korvid/platform/io.h"
#include "korvid/core/format.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <unistd.h> // symlink
#endif

namespace {

struct Files { char dir[200]; char a[256], b[256], missing[256]; };

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
    format(big_path, sizeof big_path, "%s/big.bin", f.dir);
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
    io_remove(big_path);

    q.free();
}

} // namespace

void test_io(kv::TestCtx& c) {
    using namespace kv;
    c.begin("io");

    // Two files to read, one with a known payload.
    Files f;
    KV_CHECK(c, io_make_temp_dir("korvid-io", f.dir, sizeof f.dir));
    format(f.a, sizeof f.a, "%s/a.bin", f.dir);
    format(f.b, sizeof f.b, "%s/b.bin", f.dir);
    format(f.missing, sizeof f.missing, "%s/nope.bin", f.dir);
    FILE* fp = fopen(f.a, "wb"); fwrite("hello io", 1, 8, fp); fclose(fp);
    fp = fopen(f.b, "wb"); fclose(fp); // empty

    test_backend(c, f, IoBackend::Sync);
    test_backend(c, f, IoBackend::Auto); // io_uring where the kernel allows it, sync otherwise

    // What the default picked, for the log.
    IoQueue q;
    KV_CHECK(c, q.init(nullptr, 8));
    KV_INFO("test", "io: default backend is %s", q.backend_name());
    q.free();

    io_remove(f.a); io_remove(f.b); io_remove(f.dir);
}

// The budget: a pump admits reads by bytes, idle reads wait for idle time.
void test_io_budget(kv::TestCtx& c) {
    using namespace kv;
    c.begin("io budget");
    char dir[200];
    KV_CHECK(c, io_make_temp_dir("korvid-iob", dir, sizeof dir));
    char paths[6][256];
    const u32 size = 1024 * 1024;
    u8* buf = (u8*)mem_alloc(size);
    memset(buf, 0x5A, size);
    for (u32 i = 0; i < 6; ++i) {
        format(paths[i], sizeof paths[i], "%s/f%u.bin", dir, i);
        FILE* fp = fopen(paths[i], "wb"); fwrite(buf, 1, size, fp); fclose(fp);
    }
    mem_free(buf);

    for (u32 pass = 0; pass < 2; ++pass) {
        IoBackend backend = pass == 0 ? IoBackend::Sync : IoBackend::Auto;
        IoQueue q;
        KV_CHECK(c, q.init(nullptr, 64, backend));
        q.budget.bytes_per_pump = 2 * size; // two files per pump
        Handle h[6];
        for (u32 i = 0; i < 6; ++i) {
            IoRequest r; r.path = paths[i]; r.user = i;
            r.priority = i >= 4 ? IoPriority::Idle : IoPriority::Normal; // the last two wait for idle time
            h[i] = q.read_file(r);
        }
        q.pump();
        if (!q.ring) KV_CHECK(c, q.completions().len == 2 && q.queued.len == 4);         // sync: two done, the budget stopped it
        else         KV_CHECK(c, q.in_flight + q.completions().len == 2 && q.queued.len == 4); // ring: two admitted
        KV_CHECK(c, q.pumps_capped == 1 || !q.ring);
        for (u32 i = 0; i < 2000 && q.completions().len < 4; ++i) { q.pump(); time_sleep(0.001); }
        KV_CHECK(c, q.completions().len == 4 && q.queued.len == 2 && q.pending() == 2); // the idle two never go in a pump
        for (u32 i = 0; i < 20; ++i) q.pump();
        KV_CHECK(c, q.completions().len == 4);
        // Idle time serves them, one file's worth per call here.
        KV_CHECK(c, q.pump_idle(size) == size && q.queued.len == 1);
        KV_CHECK(c, q.pump_idle(size) == size && q.queued.len == 0);
        KV_CHECK(c, q.pump_idle(size) == 0);
        for (u32 i = 0; i < 2000 && q.completions().len < 6; ++i) { q.pump(); time_sleep(0.001); }
        KV_CHECK(c, q.completions().len == 6 && q.bytes_submitted == 6 * size && q.idle_bytes == 2 * size);
        for (const IoCompletion& k : q.completions()) { KV_CHECK(c, k.status == IoStatus::Done && k.size == size); mem_free(k.data); }
        for (u32 i = 0; i < 6; ++i) q.reap(h[i]);
        q.free();
    }
    for (u32 i = 0; i < 6; ++i) io_remove(paths[i]);
    io_remove(dir);
}

// Writes: the bytes are copied at submission, the file appears at the pump, a read of it
// gives them back, an empty write makes an empty file, a write into a missing directory
// fails with an errno, and a cancelled write never happens. Both backends.
void test_io_write(kv::TestCtx& c) {
    using namespace kv;
    c.begin("io_write");
    char dir[200];
    KV_CHECK(c, io_make_temp_dir("korvid-iow", dir, sizeof dir));
    char path[300], empty[300], bad[300], gone[300];
    format(path, sizeof path, "%s/save.txt", dir);
    format(empty, sizeof empty, "%s/empty.txt", dir);
    format(bad, sizeof bad, "%s/no-such-dir/x.txt", dir);
    format(gone, sizeof gone, "%s/never.txt", dir);
    for (u32 pass = 0; pass < 2; ++pass) {
        IoQueue q;
        KV_CHECK(c, q.init(nullptr, 16, pass == 0 ? IoBackend::Sync : IoBackend::Auto));
        char payload[64];
        format(payload, sizeof payload, "score %u\n", 1200 + pass);
        IoWriteRequest w; w.path = path; w.data = payload; w.size = strlen(payload); w.user = 10;
        IoWriteRequest we; we.path = empty; we.user = 11;
        IoWriteRequest wb; wb.path = bad; wb.data = "x"; wb.size = 1; wb.user = 12;
        IoWriteRequest wg; wg.path = gone; wg.data = "x"; wg.size = 1; wg.user = 13;
        Handle hw = q.write_file(w), he = q.write_file(we), hb = q.write_file(wb), hg = q.write_file(wg);
        memset(payload, 0, sizeof payload); // the queue has its own copy
        KV_CHECK(c, !hw.is_null() && !he.is_null() && !hb.is_null() && !hg.is_null() && q.pending() == 4);
        KV_CHECK(c, q.cancel(hg));
        KV_CHECK(c, wait_for(q, 4) == 4);
        const IoCompletion* cw = by_user(q, 10);
        const IoCompletion* ce = by_user(q, 11);
        const IoCompletion* cb = by_user(q, 12);
        const IoCompletion* cg = by_user(q, 13);
        KV_CHECK(c, cw && cw->status == IoStatus::Done && cw->size == 11 && cw->data == nullptr);
        KV_CHECK(c, ce && ce->status == IoStatus::Done && ce->size == 0);
        KV_CHECK(c, cb && cb->status == IoStatus::Failed && cb->error == ENOENT);
        KV_CHECK(c, cg && cg->status == IoStatus::Cancelled);
        KV_CHECK(c, q.writes_done == 2 && q.writes_failed == 1);
        q.reap(hw); q.reap(he); q.reap(hb); q.reap(hg);
        // Read it back through the same queue.
        IoRequest r; r.path = path; r.user = 20;
        Handle hr = q.read_file(r);
        KV_CHECK(c, wait_for(q, 1) == 1);
        const IoCompletion* cr = by_user(q, 20);
        char expect[32];
        format(expect, sizeof expect, "score %u\n", 1200 + pass);
        KV_CHECK(c, cr && cr->status == IoStatus::Done && cr->size == 11 && memcmp(cr->data, expect, 11) == 0);
        mem_free(cr->data);
        q.reap(hr);
        FILE* fp = fopen(gone, "rb");
        KV_CHECK(c, fp == nullptr); // the cancelled write never happened
        if (fp) fclose(fp);
        q.free();
    }
    io_remove(path); io_remove(empty); io_remove(dir);
}

// io_remove_tree: a directory with files, hidden ones and an empty directory under it goes
// whole; a link inside it to a directory outside goes as a link, and what it pointed at
// stays. A path that is not there is gone already.
void test_io_remove_tree(kv::TestCtx& c) {
    using namespace kv;
    c.begin("io_remove_tree");
    char dir[200], outside[200], path[400];
    KV_CHECK(c, io_make_temp_dir("korvid-tree", dir, sizeof dir) && io_make_temp_dir("korvid-tree-out", outside, sizeof outside));
    const char* const files[] = {"a.txt", ".hidden", "sub/b.txt", "sub/deeper/c.txt", "sub/deeper/.d"};
    format(path, sizeof path, "%s/sub/deeper", dir); KV_CHECK(c, io_make_dirs(path));
    format(path, sizeof path, "%s/empty", dir);      KV_CHECK(c, io_make_dirs(path));
    for (const char* f : files) {
        format(path, sizeof path, "%s/%s", dir, f);
        FILE* fp = fopen(path, "wb");
        KV_CHECK(c, fp != nullptr);
        if (fp) { fputs(f, fp); fclose(fp); }
    }
    char kept[400];
    format(kept, sizeof kept, "%s/kept.txt", outside);
    FILE* fp = fopen(kept, "wb");
    if (fp) { fputs("kept", fp); fclose(fp); }
#if !defined(_WIN32)
    format(path, sizeof path, "%s/sub/link", dir);
    KV_CHECK(c, symlink(outside, path) == 0);
#endif
    KV_CHECK(c, io_remove_tree(dir));
    fp = fopen(dir, "rb");
    KV_CHECK(c, fp == nullptr); // gone
    if (fp) fclose(fp);
    fp = fopen(kept, "rb");
    KV_CHECK(c, fp != nullptr); // the link's target untouched
    if (fp) fclose(fp);
    KV_CHECK(c, io_remove_tree(dir)); // gone already
    KV_CHECK(c, io_remove_tree(outside));
    KV_CHECK(c, !io_remove_tree(""));
}
