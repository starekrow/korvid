// Mounts on both backends: a pack written by kpak_write (indexed, aligned), a plain zip
// from Python (deflated entries, offsets from the local headers), a loose directory over
// both; lookups by logical path, the last mount first; misses, bad paths and unsupported
// entries; a corrupted pack byte caught by the CRC check.
#include "korvid/platform/io.h"
#include "korvid/core/format.h"
#include "korvid/core/arena.h"
#include "korvid/core/array.h"
#include "korvid/core/memory.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include "korvid/pack/zip.h"
#include "foreign_zip.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

namespace {

using namespace kv;

bool to_file(void* user, const void* p, usize n) { return ((IoFileWriter*)user)->write(p, n); }

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
    got.free();
}

} // namespace

void test_io_mounts(kv::TestCtx& c) {
    using namespace kv;
    c.begin("io mounts");
    char dir[256];
    KV_CHECK(c, io_make_temp_dir("korvid-mount", dir, sizeof dir));
    char loose[320], pack[320], zip[320], bad[320], path[352];
    format(loose, sizeof loose, "%s/loose", dir);
    format(pack, sizeof pack, "%s/game.kpak", dir);
    format(zip, sizeof zip, "%s/plain.zip", dir);
    format(bad, sizeof bad, "%s/bad.kpak", dir);
    io_make_dirs(loose);
    format(path, sizeof path, "%s/x", loose);
    io_make_dirs(path);
    format(path, sizeof path, "%s/x/a.txt", loose);
    KV_CHECK(c, io_write_now(path, "from loose", 10));

    static u8 big[20000];
    for (u32 i = 0; i < sizeof big; ++i) big[i] = (u8)(i * 3);
    const PackWriteFile files[] = {{"x/a.txt", "from pack", 9}, {"x/big.bin", big, sizeof big}, {"x/empty", "", 0}};
    const char* why = nullptr;
    IoFileWriter w;
    KV_CHECK(c, w.open(pack));
    MemScope scope;
    KV_CHECK(c, kpak_write(files, to_file, &w, mem_scratch(), &why));
    KV_CHECK(c, w.close());
    KV_CHECK(c, io_write_now(zip, k_foreign, sizeof k_foreign));
    // The bad pack: the same, with a byte in big.bin's data flipped (the first entry after
    // the index is x/a.txt; big.bin's data starts on a later 4 KiB boundary).
    IoBytes packed;
    KV_CHECK(c, io_read_now(pack, &packed));
    u8* bytes = packed.data;
    usize n = packed.size;
    usize target = 0;
    for (usize at = 0; at + 4 < n; at += KV_PACK_ALIGN)
        if (at && bytes[at] == 0 && bytes[at + 1] == 3 && bytes[at + 2] == 6) { target = at + 100; break; }
    KV_CHECK(c, target != 0);
    bytes[target] ^= 0xFF;
    KV_CHECK(c, io_write_now(bad, bytes, n));
    packed.free();

    with_backend(c, IoBackend::Sync, loose, pack, zip, bad);
    with_backend(c, IoBackend::Auto, loose, pack, zip, bad);

    // An archive that holds this program's files under a prefix, a pack among them, as an
    // Android package does: the pack mounted where it lies inside the outer file, and the
    // outer one's entries under the prefix, named without it.
    {
        char outer[320];
        format(outer, sizeof outer, "%s/outer.zip", dir);
        IoBytes inner;
        KV_CHECK(c, io_read_now(pack, &inner));
        const PackWriteFile outer_files[] = {{"assets/game.kpak", inner.data, inner.size}, {"assets/x/a.txt", "from outer", 10}, {"other/secret", "no", 2}};
        IoFileWriter ow;
        KV_CHECK(c, ow.open(outer));
        KV_CHECK(c, kpak_write(outer_files, to_file, &ow, mem_scratch(), &why));
        KV_CHECK(c, ow.close());
        u64 at = 0, size = 0;
        KV_CHECK(c, io_archive_entry(outer, "assets/game.kpak", &at, &size) && size == inner.size && at > 0);
        KV_CHECK(c, !io_archive_entry(outer, "assets/none", &at, &size) && !io_archive_entry(zip, "maps/town.tmj", &at, &size)); // missing; deflated
        KV_CHECK(c, io_archive_entry(outer, "assets/game.kpak", &at, &size));
        const IoBackend backends[] = {IoBackend::Sync, IoBackend::Auto};
        for (IoBackend backend : backends) {
            IoQueue q;
            Array<u8> got;
            i32 err = 0;
            KV_CHECK(c, q.init(nullptr, 16, backend));
            KV_CHECK(c, q.mount_pack(outer, "assets/"));
            KV_CHECK(c, read_one(q, "x/a.txt", got, &err) == IoStatus::Done && got.len == 10 && memcmp(got.data, "from outer", 10) == 0);
            KV_CHECK(c, read_one(q, "other/secret", got, &err) == IoStatus::Failed && err == ENOENT); // outside the prefix
            KV_CHECK(c, read_one(q, "assets/x/a.txt", got, &err) == IoStatus::Failed && err == ENOENT); // named without it
            KV_CHECK(c, read_one(q, "game.kpak", got, &err) == IoStatus::Done && got.len == inner.size);
            KV_CHECK(c, q.mount_pack_at(outer, at, size));
            KV_CHECK(c, read_one(q, "x/a.txt", got, &err) == IoStatus::Done && got.len == 9 && memcmp(got.data, "from pack", 9) == 0);
            KV_CHECK(c, read_one(q, "x/big.bin", got, &err) == IoStatus::Done && got.len == 20000 && got[19999] == (u8)((19999u * 3u) & 0xFFu));
            KV_CHECK(c, !q.mount_pack_at(outer, at, size + (1u << 30)) && !q.mount_pack_at(outer, at + 1, size - 1)); // past the end; not an archive
            q.free();
            // A prefix on a plain zip, its entry deflated.
            IoQueue z;
            KV_CHECK(c, z.init(nullptr, 16, backend) && z.mount_pack(zip, "maps/"));
            KV_CHECK(c, read_one(z, "town.tmj", got, &err) == IoStatus::Done && got.len == 600);
            KV_CHECK(c, read_one(z, "dup.txt", got, &err) == IoStatus::Failed && err == ENOENT);
            z.free();
        }
        inner.free();
        io_remove(outer);
    }

    io_remove(path); format(path, sizeof path, "%s/x", loose); io_remove(path); io_remove(loose);
    io_remove(pack); io_remove(zip); io_remove(bad); io_remove(dir);
}
