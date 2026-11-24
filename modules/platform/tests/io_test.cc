#include "korvid/platform/io.h"
#include "korvid/core/format.h"
#include "korvid/core/sha256.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include <errno.h>
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

    // The queue has no path limit of its own: a name the OS refuses (one of 299 bytes, past
    // the file system's 255) fails at the pump with the OS's error; no path at all is
    // refused up front.
    char too_long[300]; memset(too_long, 'x', sizeof too_long - 1); too_long[sizeof too_long - 1] = 0;
    IoRequest rl; rl.path = too_long; rl.user = 7;
    Handle hl = q.read_file(rl);
    KV_CHECK(c, !hl.is_null() && wait_for(q, 1) == 1 && q.completions()[0].status == IoStatus::Failed && q.completions()[0].error != 0);
    KV_CHECK(c, q.reap(hl));
    IoRequest rn;
    KV_CHECK(c, q.read_file(rn).is_null() && q.stat_file(rn).is_null());

    // A larger file comes back whole, and a cancel while in flight still completes.
    char big_path[256];
    format(big_path, sizeof big_path, "%s/big.bin", f.dir);
    const u32 big = 3 * 1024 * 1024 + 123;
    {
        u8* buf = (u8*)mem_alloc(big);
        for (u32 i = 0; i < big; ++i) buf[i] = (u8)(i * 7 + (i >> 8));
        KV_CHECK(c, io_write_now(big_path, buf, big));
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
    KV_CHECK(c, io_write_now(f.a, "hello io", 8));
    KV_CHECK(c, io_write_now(f.b, "", 0)); // empty

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
        KV_CHECK(c, io_write_now(paths[i], buf, size));
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
        KV_CHECK(c, !io_is_file(gone)); // the cancelled write never happened
        q.free();
    }
    io_remove(path); io_remove(empty); io_remove(dir);
}

// io_make_dirs: several missing levels at once, a directory already there, a trailing
// slash, and a file in the way (false, at the file and below it).
void test_io_make_dirs(kv::TestCtx& c) {
    using namespace kv;
    c.begin("io_make_dirs");
    char dir[200], path[400];
    KV_CHECK(c, io_make_temp_dir("korvid-dirs", dir, sizeof dir));
    format(path, sizeof path, "%s/a/b/c/d", dir);  KV_CHECK(c, io_make_dirs(path) && io_is_dir(path));
    format(path, sizeof path, "%s/a/b", dir);      KV_CHECK(c, io_make_dirs(path) && io_is_dir(path));
    format(path, sizeof path, "%s/a/b/c/d", dir);  KV_CHECK(c, io_make_dirs(path));
    format(path, sizeof path, "%s/a/e/", dir);     KV_CHECK(c, io_make_dirs(path) && io_is_dir(path));
    format(path, sizeof path, "%s/a/file", dir);   KV_CHECK(c, io_write_now(path, "f", 1));
    KV_CHECK(c, !io_make_dirs(path));
    format(path, sizeof path, "%s/a/file/under", dir); KV_CHECK(c, !io_make_dirs(path));
    KV_CHECK(c, !io_make_dirs(""));
    KV_CHECK(c, io_remove_tree(dir));
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
        KV_CHECK(c, io_write_now(path, f, strlen(f)));
    }
    char kept[400];
    format(kept, sizeof kept, "%s/kept.txt", outside);
    KV_CHECK(c, io_write_now(kept, "kept", 4));
#if !defined(_WIN32)
    format(path, sizeof path, "%s/sub/link", dir);
    KV_CHECK(c, symlink(outside, path) == 0);
#endif
    KV_CHECK(c, io_remove_tree(dir));
    KV_CHECK(c, !io_is_dir(dir));    // gone
    KV_CHECK(c, io_is_file(kept));   // the link's target untouched
    KV_CHECK(c, io_remove_tree(dir)); // gone already
    KV_CHECK(c, io_remove_tree(outside));
    KV_CHECK(c, !io_remove_tree(""));
}

// The blocking calls: a whole file written and read back, with the NUL after it; a file
// written in pieces; what a missing file and a directory give.
void test_io_now(kv::TestCtx& c) {
    using namespace kv;
    c.begin("io_read_now, io_write_now, IoFileWriter");
    char dir[200], path[300], missing[300];
    KV_CHECK(c, io_make_temp_dir("korvid-now", dir, sizeof dir));
    format(path, sizeof path, "%s/a.bin", dir);
    format(missing, sizeof missing, "%s/none", dir);
    static u8 bytes[70000];
    for (u32 i = 0; i < sizeof bytes; ++i) bytes[i] = (u8)(i * 7 + 1);
    KV_CHECK(c, io_write_now(path, bytes, sizeof bytes) && io_is_file(path) && !io_is_dir(path));
    IoBytes got;
    KV_CHECK(c, io_read_now(path, &got) && got.size == sizeof bytes && memcmp(got.data, bytes, sizeof bytes) == 0 && got.data[got.size] == 0);
    got.free();
    KV_CHECK(c, got.data == nullptr && got.size == 0);
    i32 err = 0;
    KV_CHECK(c, !io_read_now(missing, &got, nullptr, &err) && err != 0 && got.data == nullptr);
    KV_CHECK(c, !io_read_now(dir, &got) || got.size == 0); // a directory: refused, or nothing in it
    got.free();
    IoFileWriter w;
    KV_CHECK(c, w.open(path) && w.print("P6\n%u %u\n", 3u, 2u) && w.write("abc", 3) && w.close());
    KV_CHECK(c, io_read_now(path, &got) && got.size == 10 && memcmp(got.data, "P6\n3 2\nabc", 10) == 0);
    got.free();
    format(missing, sizeof missing, "%s/no/such/dir/x", dir);
    IoFileWriter bad;
    KV_CHECK(c, !bad.open(missing) && !bad.write("x", 1) && !bad.close() && bad.error != 0);
    KV_CHECK(c, io_write_now(path, "", 0) && io_read_now(path, &got) && got.size == 0 && got.data && got.data[0] == 0);
    got.free();

    // io_read_each and io_copy_now: a file of three pieces and a bit, in order, and a copy
    // with the same bytes and the hash of them.
    {
        const usize n = 3 * 65536 + 5;
        u8* big = (u8*)malloc(n);
        for (usize i = 0; i < n; ++i) big[i] = (u8)(i * 131 + (i >> 9));
        u8 want[32], got_hash[32];
        sha256(big, n, want);
        format(path, sizeof path, "%s/big.bin", dir);
        KV_CHECK(c, io_write_now(path, big, n));
        struct Each { u32 pieces; usize bytes; Sha256 h; } each{};
        each.h.init();
        auto piece = [](void* user, const u8* data, usize len) { Each& e = *(Each*)user; ++e.pieces; e.bytes += len; e.h.update(data, len); return true; };
        KV_CHECK(c, io_read_each(path, piece, &each, nullptr, 65536) && each.pieces == 4 && each.bytes == n);
        each.h.finish(got_hash);
        KV_CHECK(c, memcmp(got_hash, want, 32) == 0);
        u32 calls = 0;
        auto stop = [](void* user, const u8*, usize) { ++*(u32*)user; return false; };
        KV_CHECK(c, io_read_each(path, stop, &calls, nullptr, 65536) && calls == 1); // stopped, not failed
        char copy[300];
        format(copy, sizeof copy, "%s/copy.bin", dir);
        memset(got_hash, 0, 32);
        KV_CHECK(c, io_copy_now(path, copy, got_hash) && memcmp(got_hash, want, 32) == 0);
        KV_CHECK(c, io_read_now(copy, &got) && got.size == n && memcmp(got.data, big, n) == 0);
        got.free();
        err = 0;
        KV_CHECK(c, !io_read_each(missing, piece, &each, &err) && err != 0);
        KV_CHECK(c, !io_copy_now(path, missing, nullptr, &err) && err != 0);
        free(big);
    }
    KV_CHECK(c, io_remove_tree(dir));
}

// io_make_dirs as deep as the OS goes (KR15): thirteen levels of 230 bytes, about 3,000
// in all, past the 1,024 it used to copy into; a file in the deepest, and the tree removed.
void test_io_make_dirs_deep(kv::TestCtx& c) {
    using namespace kv;
    c.begin("io_make_dirs_deep");
    char dir[200];
    KV_CHECK(c, io_make_temp_dir("korvid-deep", dir, sizeof dir));
    char deep[3400];
    usize n = (usize)format(deep, sizeof deep, "%s", dir);
    for (u32 level = 0; level < 13; ++level) {
        deep[n++] = '/';
        for (u32 i = 0; i < 230; ++i) deep[n++] = (char)('a' + (level + i) % 26);
        deep[n] = 0;
    }
    KV_CHECK(c, n > 3000);
    KV_CHECK(c, io_make_dirs(deep));
    KV_CHECK(c, io_make_dirs(deep)); // already there
    IoStat st;
    KV_CHECK(c, io_stat(deep, &st) && st.is_dir);
    format(deep + n, sizeof deep - n, "/x.txt");
    KV_CHECK(c, io_write_now(deep, "x", 1));
    KV_CHECK(c, !io_make_dirs(deep)); // a file is not a directory
    KV_CHECK(c, io_remove_tree(dir));
    KV_CHECK(c, !io_stat(dir, &st));
}

// Slice 10's queue: a path of 1,000 bytes read and written, the first 64 KB of a 16 MB file
// and ranges past its end, and a stat of a file, a directory and a missing path; under the
// synchronous backend, the ring, and (where the ring has its own stat) the task workers.
void test_io_long_ranged_stat(kv::TestCtx& c) {
    using namespace kv;
    c.begin("io_long_ranged_stat");
    char dir[200];
    KV_CHECK(c, io_make_temp_dir("korvid-io10", dir, sizeof dir));

    // Four directories of 230 bytes under the temporary one, and a file: about 1,000 bytes.
    char deep[1100];
    usize n = (usize)format(deep, sizeof deep, "%s", dir);
    for (u32 level = 0; level < 4; ++level) {
        deep[n++] = '/';
        for (u32 i = 0; i < 230; ++i) deep[n++] = (char)('a' + (level + i) % 26);
        deep[n] = 0;
    }
    KV_CHECK(c, io_make_dirs(deep));
    char deep_dir[1100];
    memcpy(deep_dir, deep, n + 1);
    n += (usize)format(deep + n, sizeof deep - n, "/%s", "far.txt");
    KV_CHECK(c, n >= 950 && n < 1024);

    // 16 MB with a pattern a range can be checked against.
    char big[300], missing[300];
    format(big, sizeof big, "%s/big.bin", dir);
    format(missing, sizeof missing, "%s/nothing/here.bin", dir);
    const usize kBig = 16u << 20;
    {
        u8* buf = (u8*)mem_alloc(kBig);
        for (usize i = 0; i < kBig; ++i) buf[i] = (u8)(i * 13 + (i >> 9));
        KV_CHECK(c, io_write_now(big, buf, kBig));
        mem_free(buf);
    }
    auto pattern_ok = [](const IoCompletion* cc, u64 from) {
        if (!cc || !cc->data) return false;
        for (usize i = 0; i < cc->size; i += 997) { usize at = (usize)from + i; if (((const u8*)cc->data)[i] != (u8)(at * 13 + (at >> 9))) return false; }
        return true;
    };

    for (u32 pass = 0; pass < 3; ++pass) {
        IoQueue q;
        KV_CHECK(c, q.init(nullptr, 32, pass == 0 ? IoBackend::Sync : IoBackend::Auto));
        if (pass == 2) { // the task workers' stat, which Windows, Apple and Android use
            KV_CHECK(c, q.start_tasks(2));
            q.stat_on_tasks = true;
        }

        // The long path: written, read back, and its stat.
        char payload[32];
        usize plen = (usize)format(payload, sizeof payload, "far away %u", pass);
        IoWriteRequest w; w.path = deep; w.data = payload; w.size = plen; w.user = 1;
        Handle hw = q.write_file(w);
        KV_CHECK(c, !hw.is_null() && wait_for(q, 1) == 1);
        const IoCompletion* cw = by_user(q, 1);
        KV_CHECK(c, cw && cw->status == IoStatus::Done && cw->size == plen);
        q.reap(hw);
        IoRequest r; r.path = deep; r.user = 2;
        Handle hr = q.read_file(r);
        KV_CHECK(c, wait_for(q, 1) == 1);
        const IoCompletion* cr = by_user(q, 2);
        KV_CHECK(c, cr && cr->status == IoStatus::Done && cr->size == plen && memcmp(cr->data, payload, plen) == 0);
        if (cr) mem_free(cr->data);
        q.reap(hr);

        // Ranges: the first 64 KB alone, one from the middle, one that runs past the end,
        // one that starts past it, and an offset with no length (to the end).
        IoRequest r1; r1.path = big; r1.length = 64u << 10; r1.user = 10;
        IoRequest r2; r2.path = big; r2.offset = 5000001; r2.length = 12345; r2.user = 11;
        IoRequest r3; r3.path = big; r3.offset = kBig - 100; r3.length = 4096; r3.user = 12;
        IoRequest r4; r4.path = big; r4.offset = kBig + 1; r4.length = 10; r4.user = 13;
        IoRequest r5; r5.path = big; r5.offset = kBig - 7; r5.user = 14;
        IoRequest r6; r6.path = missing; r6.length = 10; r6.user = 15;
        u64 before = q.bytes_submitted;
        Handle h1 = q.read_file(r1), h2 = q.read_file(r2), h3 = q.read_file(r3), h4 = q.read_file(r4), h5 = q.read_file(r5), h6 = q.read_file(r6);
        KV_CHECK(c, wait_for(q, 6) == 6);
        const IoCompletion* c1 = by_user(q, 10);
        const IoCompletion* c2 = by_user(q, 11);
        const IoCompletion* c3 = by_user(q, 12);
        const IoCompletion* c4 = by_user(q, 13);
        const IoCompletion* c5 = by_user(q, 14);
        const IoCompletion* c6 = by_user(q, 15);
        KV_CHECK(c, c1 && c1->status == IoStatus::Done && c1->size == (64u << 10) && pattern_ok(c1, 0));
        KV_CHECK(c, c2 && c2->status == IoStatus::Done && c2->size == 12345 && pattern_ok(c2, 5000001));
        KV_CHECK(c, c3 && c3->status == IoStatus::Done && c3->size == 100 && pattern_ok(c3, kBig - 100));
        KV_CHECK(c, c4 && c4->status == IoStatus::Done && c4->size == 0);
        KV_CHECK(c, c5 && c5->status == IoStatus::Done && c5->size == 7 && pattern_ok(c5, kBig - 7));
        KV_CHECK(c, c6 && c6->status == IoStatus::Failed && c6->error == ENOENT);
        // The budget counted what the ranges read, not the file: 16 MB six times over would be 96.
        KV_CHECK(c, q.bytes_submitted - before == (64u << 10) + 12345 + 100 + 7);
        const IoCompletion* ranged[] = {c1, c2, c3, c4, c5};
        for (const IoCompletion* cc : ranged) if (cc && cc->data) mem_free(cc->data);
        q.reap(h1); q.reap(h2); q.reap(h3); q.reap(h4); q.reap(h5); q.reap(h6);

        // Stats: a file, a directory, nothing (Done, all false), and the long path. Nothing
        // is answered at submission.
        IoRequest s1; s1.path = big; s1.user = 20;
        IoRequest s2; s2.path = deep_dir; s2.user = 21;
        IoRequest s3; s3.path = missing; s3.user = 22;
        IoRequest s4; s4.path = deep; s4.user = 23; s4.priority = IoPriority::High;
        IoRequest s5; s5.path = big; s5.user = 24;
        // A mount point: another volume than the directory it is in (KR13).
#if defined(__linux__)
        const char* mount_point = "/proc";
#else
        const char* mount_point = nullptr;
#endif
        IoRequest s7; s7.path = mount_point; s7.user = 25;
        Handle g1 = q.stat_file(s1), g2 = q.stat_file(s2), g3 = q.stat_file(s3), g4 = q.stat_file(s4), g5 = q.stat_file(s5);
        Handle g7 = mount_point ? q.stat_file(s7) : Handle{};
        u32 stats = mount_point ? 6 : 5;
        KV_CHECK(c, !g1.is_null() && q.completions().len == 0 && q.pending() == stats);
        KV_CHECK(c, q.cancel(g5));
        KV_CHECK(c, wait_for(q, stats) == stats && q.pending() == 0);
        IoStat want;
        KV_CHECK(c, io_stat(big, &want) && want.is_file && want.size == kBig);
        const IoCompletion* t1 = by_user(q, 20);
        const IoCompletion* t2 = by_user(q, 21);
        const IoCompletion* t3 = by_user(q, 22);
        const IoCompletion* t4 = by_user(q, 23);
        const IoCompletion* t5 = by_user(q, 24);
        KV_CHECK(c, t1 && t1->status == IoStatus::Done && t1->data == nullptr && t1->stat.is_file && !t1->stat.is_dir &&
                    t1->stat.size == kBig && t1->stat.mtime_ns == want.mtime_ns);
        KV_CHECK(c, t2 && t2->status == IoStatus::Done && t2->stat.is_dir && !t2->stat.is_file);
        KV_CHECK(c, t3 && t3->status == IoStatus::Done && !t3->stat.is_file && !t3->stat.is_dir && t3->stat.size == 0);
        KV_CHECK(c, t4 && t4->status == IoStatus::Done && t4->stat.is_file && t4->stat.size == plen);
        KV_CHECK(c, t5 && t5->status == IoStatus::Cancelled);
        // The volume: io_stat's, the same for a file and the directories above it, 0 for nothing.
        KV_CHECK(c, want.volume != 0 && t1 && t1->stat.volume == want.volume);
        KV_CHECK(c, t2 && t2->stat.volume == want.volume && t4 && t4->stat.volume == want.volume);
        KV_CHECK(c, t3 && t3->stat.volume == 0);
        if (mount_point) {
            IoStat mp, parent;
            KV_CHECK(c, io_stat(mount_point, &mp) && io_stat("/", &parent) && mp.volume != parent.volume);
            const IoCompletion* t7 = by_user(q, 25);
            KV_CHECK(c, t7 && t7->status == IoStatus::Done && t7->stat.is_dir && t7->stat.volume == mp.volume);
            q.reap(g7);
        }
        KV_CHECK(c, q.stats_done == stats - 1 && q.stats_failed == 0);
        if (pass == 2) KV_CHECK(c, q.tasks_done == 0); // the stats are counted as stats
        q.reap(g1); q.reap(g2); q.reap(g3); q.reap(g4); q.reap(g5);

        // A stat still queued at free() is dropped with the queue.
        IoRequest s6; s6.path = deep; s6.user = 30;
        KV_CHECK(c, !q.stat_file(s6).is_null());
        q.free();
    }

    // Past what the OS takes (4,096 bytes on Linux): Failed with the OS's error.
    {
        IoQueue q;
        KV_CHECK(c, q.init(nullptr, 4));
        char* huge = (char*)mem_alloc(5000);
        memset(huge, 'y', 4999);
        huge[0] = '/';
        huge[4999] = 0;
        IoRequest r; r.path = huge; r.user = 40;
        Handle h = q.read_file(r);
        mem_free(huge); // copied
        KV_CHECK(c, !h.is_null() && wait_for(q, 1) == 1);
        const IoCompletion* cc = by_user(q, 40);
        KV_CHECK(c, cc && cc->status == IoStatus::Failed && cc->error != 0);
        q.reap(h);
        q.free();
    }

    io_remove_tree(dir);
}
