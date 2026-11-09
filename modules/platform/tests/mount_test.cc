// Mounts on both backends: a pack written by kpak_write (indexed, aligned), a plain zip
// from Python (deflated entries, offsets from the local headers), a loose directory over
// both; lookups by logical path, the last mount first; misses, bad paths and unsupported
// entries; a corrupted pack byte caught by the CRC check.
#include "korvid/platform/io.h"
#include "korvid/core/arena.h"
#include "korvid/core/array.h"
#include "korvid/core/memory.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include "korvid/pack/zip.h"
#include "foreign_zip.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

using namespace kv;

bool to_file(void* user, const void* p, usize n) { return fwrite(p, 1, n, (FILE*)user) == n; }

// Reads one logical path to completion: the status, and the bytes when Done.
IoStatus read_one(IoQueue& q, const char* path, Array<u8>& out, i32* err) {
    IoRequest r;
    r.path = path;
    r.user = 77;
    Handle h = q.read_asset(r);
    out.clear();
    if (h.is_null()) return IoStatus::Failed;
    for (u32 i = 0; i < 2000; ++i) {
        q.pump();
        for (const IoCompletion& c : q.completions()) {
            if (c.request != h) continue;
            IoCompletion done = c;
            q.reap(h);
            for (usize k = 0; k < done.size; ++k) out.push(((const u8*)done.data)[k]);
            if (done.data) free_in(mem_general(), done.data, done.size ? done.size : 1);
            if (err) *err = done.error;
            return done.status;
        }
        time_sleep(0.0005);
    }
    return IoStatus::Pending;
}

void with_backend(TestCtx& c, IoBackend backend, const char* dir, const char* pack, const char* zip, const char* bad) {
    IoQueue q;
    KV_CHECK(c, q.init(nullptr, 64, backend));
    Array<u8> got;
    i32 err = 0;
    KV_CHECK(c, read_one(q, "x/a.txt", got, &err) == IoStatus::Failed && err == ENOENT); // nothing mounted

    KV_CHECK(c, q.mount_pack(zip) && q.mount_pack(pack));
    KV_CHECK(c, read_one(q, "x/a.txt", got, &err) == IoStatus::Done && got.len == 9 && memcmp(got.data, "from pack", 9) == 0);
    KV_CHECK(c, read_one(q, "x//./big.bin", got, &err) == IoStatus::Done && got.len == 20000 && got[19999] == (u8)((19999u * 3u) & 0xFFu));
    KV_CHECK(c, read_one(q, "x/empty", got, &err) == IoStatus::Done && got.len == 0);
    // From the plain zip, under the pack: a deflated map, the second of a duplicated name.
    KV_CHECK(c, read_one(q, "maps/town.tmj", got, &err) == IoStatus::Done && got.len == 600 && memcmp(got.data, "{\"width\": 4}", 12) == 0);
    KV_CHECK(c, read_one(q, "dup.txt", got, &err) == IoStatus::Done && got.len == 6 && memcmp(got.data, "second", 6) == 0);
    KV_CHECK(c, read_one(q, "tiles/grass.png", got, &err) == IoStatus::Done && got.len == 9);
    KV_CHECK(c, q.inflated >= 1 && q.header_reads >= 2);
    // Misses and refusals.
    KV_CHECK(c, read_one(q, "x/none", got, &err) == IoStatus::Failed && err == ENOENT);
    KV_CHECK(c, read_one(q, "../x/a.txt", got, &err) == IoStatus::Failed && err == EINVAL);
    // A loose directory mounted last wins over both packs.
    KV_CHECK(c, q.mount_dir(dir));
    KV_CHECK(c, read_one(q, "x/a.txt", got, &err) == IoStatus::Done && got.len == 10 && memcmp(got.data, "from loose", 10) == 0);
    KV_CHECK(c, read_one(q, "x/big.bin", got, &err) == IoStatus::Done && got.len == 20000); // not loose: falls through to the pack
    KV_CHECK(c, !q.mount_dir("/nonexistent/korvid") && !q.mount_pack("/nonexistent.kpak"));
    q.free();

    // A pack with one byte of an entry's data flipped: the CRC check catches it in dev.
    IoQueue b;
    KV_CHECK(c, b.init(nullptr, 8, backend) && b.mount_pack(bad));
    IoStatus st = read_one(b, "x/big.bin", got, &err);
    KV_CHECK(c, KV_DEV ? (st == IoStatus::Failed && err == EILSEQ && b.crc_failures == 1) : st == IoStatus::Done);
    KV_CHECK(c, read_one(b, "x/a.txt", got, &err) == IoStatus::Done); // the rest still reads
    b.free();
}

} // namespace

void test_io_mounts(kv::TestCtx& c) {
    using namespace kv;
    c.begin("io mounts");
    char dir[256];
    KV_CHECK(c, io_make_temp_dir("korvid-mount", dir, sizeof dir));
    char loose[320], pack[320], zip[320], bad[320], path[352];
    snprintf(loose, sizeof loose, "%s/loose", dir);
    snprintf(pack, sizeof pack, "%s/game.kpak", dir);
    snprintf(zip, sizeof zip, "%s/plain.zip", dir);
    snprintf(bad, sizeof bad, "%s/bad.kpak", dir);
    io_make_dirs(loose);
    snprintf(path, sizeof path, "%s/x", loose);
    io_make_dirs(path);
    snprintf(path, sizeof path, "%s/x/a.txt", loose);
    FILE* f = fopen(path, "wb"); fwrite("from loose", 1, 10, f); fclose(f);

    static u8 big[20000];
    for (u32 i = 0; i < sizeof big; ++i) big[i] = (u8)(i * 3);
    const PackWriteFile files[] = {{"x/a.txt", "from pack", 9}, {"x/big.bin", big, sizeof big}, {"x/empty", "", 0}};
    const char* why = nullptr;
    f = fopen(pack, "wb");
    MemScope scope;
    KV_CHECK(c, kpak_write(files, to_file, f, mem_scratch(), &why));
    fclose(f);
    f = fopen(zip, "wb"); fwrite(k_foreign, 1, sizeof k_foreign, f); fclose(f);
    // The bad pack: the same, with a byte in big.bin's data flipped (the first entry after
    // the index is x/a.txt; big.bin's data starts on a later 4 KiB boundary).
    f = fopen(pack, "rb");
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    u8* bytes = (u8*)alloc_in(mem_general(), (usize)n);
    KV_CHECK(c, fread(bytes, 1, (usize)n, f) == (usize)n); fclose(f);
    usize target = 0;
    for (usize at = 0; at + 4 < (usize)n; at += KV_PACK_ALIGN)
        if (at && bytes[at] == 0 && bytes[at + 1] == 3 && bytes[at + 2] == 6) { target = at + 100; break; }
    KV_CHECK(c, target != 0);
    bytes[target] ^= 0xFF;
    f = fopen(bad, "wb"); fwrite(bytes, 1, (usize)n, f); fclose(f);
    free_in(mem_general(), bytes, (usize)n);

    with_backend(c, IoBackend::Sync, loose, pack, zip, bad);
    with_backend(c, IoBackend::Auto, loose, pack, zip, bad);

    io_remove(path); snprintf(path, sizeof path, "%s/x", loose); io_remove(path); io_remove(loose);
    io_remove(pack); io_remove(zip); io_remove(bad); io_remove(dir);
}
