#include "korvid/platform/io.h"
#include "korvid/core/test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void test_io(kv::TestCtx& c) {
    using namespace kv;
    c.begin("io");

    // Two files to read, one with a known payload.
    char dir[] = "/tmp/korvid-io-XXXXXX";
    KV_CHECK(c, mkdtemp(dir) != nullptr);
    char path_a[256], path_b[256], path_missing[256];
    snprintf(path_a, sizeof path_a, "%s/a.bin", dir);
    snprintf(path_b, sizeof path_b, "%s/b.bin", dir);
    snprintf(path_missing, sizeof path_missing, "%s/nope.bin", dir);
    FILE* f = fopen(path_a, "wb"); fwrite("hello io", 1, 8, f); fclose(f);
    f = fopen(path_b, "wb"); fclose(f); // empty

    IoQueue q;
    KV_CHECK(c, q.init(nullptr, 64) && q.kind != 0);

    // Nothing completes inline: submissions are pending until a pump.
    IoRequest ra; ra.path = path_a; ra.user = 1; ra.priority = IoPriority::Low;
    IoRequest rb; rb.path = path_b; rb.user = 2; rb.priority = IoPriority::High;
    IoRequest rm; rm.path = path_missing; rm.user = 3;
    IoRequest rc; rc.path = path_a; rc.user = 4;
    Handle ha = q.read_file(ra), hb = q.read_file(rb), hm = q.read_file(rm), hc = q.read_file(rc);
    KV_CHECK(c, !ha.is_null() && !hb.is_null() && !hm.is_null() && !hc.is_null());
    KV_CHECK(c, q.pending() == 4 && q.completions().len == 0);
    KV_CHECK(c, q.cancel(hc) && !q.cancel(hc));
    KV_CHECK(c, resolve(ha) != nullptr); // the request is a handle like any other

    q.pump();
    Span<const IoCompletion> done = q.completions();
    KV_CHECK(c, done.len == 4 && q.pending() == 0);
    // Priority order: the High request first, then Normal in submission order, then Low.
    KV_CHECK(c, done[0].user == 2 && done[1].user == 3 && done[2].user == 4 && done[3].user == 1);
    KV_CHECK(c, done[0].status == IoStatus::Done && done[0].size == 0);
    KV_CHECK(c, done[1].status == IoStatus::Failed && done[1].error != 0);
    KV_CHECK(c, done[2].status == IoStatus::Cancelled && done[2].data == nullptr);
    KV_CHECK(c, done[3].status == IoStatus::Done && done[3].size == 8 && memcmp(done[3].data, "hello io", 8) == 0);
    KV_CHECK(c, q.reads_done == 2 && q.reads_failed == 1);
    mem_free(done[0].data);
    mem_free(done[3].data);

    // Completions persist across pumps until reaped; reaping retires the request.
    q.pump();
    KV_CHECK(c, q.completions().len == 4 && resolve(ha) != nullptr);
    KV_CHECK(c, q.reap(ha) && !q.reap(ha) && q.completions().len == 3 && resolve(ha) == nullptr);
    KV_CHECK(c, q.reap(hb) && q.reap(hm) && q.reap(hc) && q.completions().len == 0);

    // A path that does not fit is refused up front.
    char too_long[300]; memset(too_long, 'x', sizeof too_long - 1); too_long[sizeof too_long - 1] = 0;
    IoRequest rl; rl.path = too_long;
    KV_CHECK(c, q.read_file(rl).is_null());

    q.free();
    unlink(path_a); unlink(path_b); rmdir(dir);
}
