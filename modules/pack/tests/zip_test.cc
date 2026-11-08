// Packs end to end in memory: write, then read back through the index (every data offset
// known, every one aligned) and through the central directory alone (offsets from the
// local headers); a zip from Python with the awkward cases; an edited pack refused by
// its index; path normalization; and mutated archives that must never be read out of
// bounds.
#include "korvid/pack/zip.h"
#include "korvid/core/array.h"
#include "korvid/core/arena.h"
#include "korvid/core/test.h"
#include "korvid/inflate/inflate.h"
#include "foreign.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

using namespace kv;

bool collect(void* user, const void* p, usize n) {
    Array<u8>& a = *(Array<u8>*)user;
    for (usize i = 0; i < n; ++i) a.push(((const u8*)p)[i]);
    return true;
}

// Steps 1 and 2 of reading: the tail, then the central directory.
bool read_table(const u8* zip, usize len, ZipTail& tail, PackTable& t, const char** why) {
    usize tail_len = len < KV_ZIP_TAIL_MAX ? len : KV_ZIP_TAIL_MAX;
    if (!zip_find_end(zip + len - tail_len, tail_len, len - tail_len, &tail, why)) return false;
    if (tail.needs_zip64 && !zip_read_zip64_end(zip + tail.zip64_end, len - tail.zip64_end, &tail, why)) return false;
    if (tail.cd_offset + tail.cd_size > len) { *why = "cd past end"; return false; }
    return zip_read_central(zip + tail.cd_offset, (usize)tail.cd_size, tail, mem_general(), &t, why);
}

// Step 4: the data offset from the local header.
u64 data_at(const u8* zip, usize len, u64 header) {
    u64 off = 0; usize need = 0;
    if (header >= len) return 0;
    if (!zip_local_data_offset(zip + header, len - header, header, &off, &need)) return 0;
    return off;
}

} // namespace

void test_zip(kv::TestCtx& c) {
    using namespace kv;
    c.begin("zip");
    const char* why = nullptr;

    // Normalization.
    char n[KV_PACK_PATH_MAX];
    KV_CHECK(c, pack_normalize("a//b/./c", n, sizeof n) && strcmp(n, "a/b/c") == 0);
    KV_CHECK(c, pack_normalize("./a\\b", n, sizeof n) && strcmp(n, "a/b") == 0);
    KV_CHECK(c, !pack_normalize("a/../b", n, sizeof n) && !pack_normalize("/abs", n, sizeof n) && !pack_normalize("./", n, sizeof n));
    KV_CHECK(c, !pack_normalize("abcdef", n, 4));

    // Write a pack.
    static u8 big[10000];
    for (u32 i = 0; i < sizeof big; ++i) big[i] = (u8)(i * 7);
    const PackWriteFile files[] = {
        {"example/b.png", big, sizeof big},
        {"example/a.txt", "hello", 5},
        {"example//empty.bin", "", 0},
        {"text/caf\xc3\xa9.ttf", "font", 4},
    };
    Array<u8> zip;
    MemScope scope;
    KV_CHECK(c, kpak_write(files, collect, &zip, mem_scratch(), &why));
    Array<u8> again;
    KV_CHECK(c, kpak_write(files, collect, &again, mem_scratch(), &why) && again.len == zip.len && memcmp(again.data, zip.data, zip.len) == 0); // deterministic

    // For a person checking the output against real zip tools: KV_PACK_DUMP=<file>.
    if (const char* dump = getenv("KV_PACK_DUMP")) {
        if (FILE* df = fopen(dump, "wb")) { fwrite(zip.data, 1, zip.len, df); fclose(df); }
    }

    // Back through the central directory: every name, normalized, the index among them.
    ZipTail tail;
    PackTable t;
    KV_CHECK(c, read_table(zip.data, zip.len, tail, t, &why) && t.count == 5 && !t.indexed);
    const PackEntry* ie = pack_find(t, KV_PACK_INDEX_NAME);
    const PackEntry* be = pack_find(t, "example/b.png");
    KV_CHECK(c, ie && be && pack_find(t, "example/empty.bin") && pack_find(t, "text/caf\xc3\xa9.ttf") && !pack_find(t, "example/c"));
    u64 bat = data_at(zip.data, zip.len, be->header_offset);
    KV_CHECK(c, bat && bat % KV_PACK_ALIGN == 0 && be->size == sizeof big && memcmp(zip.data + bat, big, sizeof big) == 0);
    KV_CHECK(c, be->crc == crc32(0, big, sizeof big) && be->method == ZipStored);

    // Then through the index: every offset known and aligned.
    u64 iat = data_at(zip.data, zip.len, ie->header_offset);
    PackTable x;
    KV_CHECK(c, iat % KV_PACK_ALIGN == 0 && kpak_read_index(zip.data + iat, (usize)ie->size, tail, mem_general(), &x, &why));
    KV_CHECK(c, x.indexed && x.count == 4 && x.unaligned == 0);
    const PackEntry* xb = pack_find(x, "example/b.png");
    const PackEntry* xa = pack_find(x, "example/a.txt");
    KV_CHECK(c, xb && xb->data_offset == bat && xa && memcmp(zip.data + xa->data_offset, "hello", 5) == 0);
    pack_table_free(x, mem_general());

    // An edited pack: the central directory moved, so the index is refused.
    ZipTail moved = tail;
    moved.cd_offset += 1;
    KV_CHECK(c, !kpak_read_index(zip.data + iat, (usize)ie->size, moved, mem_general(), &x, &why));
    pack_table_free(t, mem_general());

    // Refusals when writing.
    const PackWriteFile dup[] = {{"a", "1", 1}, {"./a", "2", 1}};
    const PackWriteFile up[] = {{"../a", "1", 1}};
    const PackWriteFile reserved[] = {{".kpak/x", "1", 1}};
    Array<u8> junk_out;
    KV_CHECK(c, !kpak_write(dup, collect, &junk_out, mem_scratch(), &why));
    KV_CHECK(c, !kpak_write(up, collect, &junk_out, mem_scratch(), &why));
    KV_CHECK(c, !kpak_write(reserved, collect, &junk_out, mem_scratch(), &why));

    // A zip from elsewhere.
    PackTable f;
    KV_CHECK(c, read_table(k_foreign, sizeof k_foreign, tail, f, &why));
    KV_CHECK(c, f.count == 4 && f.duplicates == 1 && f.skipped == 1 && f.deflated == 1 && !f.indexed);
    const PackEntry* map = pack_find(f, "maps/town.tmj");
    const PackEntry* grass = pack_find(f, "tiles/grass.png");
    const PackEntry* cafe = pack_find(f, "caf\xc3\xa9.txt");
    const PackEntry* d2 = pack_find(f, "dup.txt");
    KV_CHECK(c, map && grass && cafe && d2 && !pack_find(f, "dir") && !pack_find(f, "evil.txt"));
    if (map) {
        u64 at = data_at(k_foreign, sizeof k_foreign, map->header_offset);
        static u8 out[1024];
        InflateOut r = inflate_raw(k_foreign + at, (usize)map->csize, out, sizeof out);
        KV_CHECK(c, map->method == ZipDeflate && r.result == InflateResult::Ok && r.produced == map->size && map->size == 600);
        KV_CHECK(c, crc32(0, out, r.produced) == map->crc && memcmp(out, "{\"width\": 4}", 12) == 0);
    }
    if (d2) {
        u64 at = data_at(k_foreign, sizeof k_foreign, d2->header_offset);
        KV_CHECK(c, d2->size == 6 && memcmp(k_foreign + at, "second", 6) == 0);
    }
    pack_table_free(f, mem_general());

    // Mutations: flipped bytes and cut archives never read past what they were given.
    // (The checks are only that nothing crashes and that counts stay inside the input.)
    static u8 m[sizeof k_foreign];
    u32 rng = 12345;
    for (u32 trial = 0; trial < 3000; ++trial) {
        memcpy(m, k_foreign, sizeof m);
        u32 flips = 1 + trial % 8;
        for (u32 k = 0; k < flips; ++k) { rng = rng * 1664525u + 1013904223u; m[(rng >> 8) % sizeof m] ^= (u8)(rng >> 24 | 1); }
        rng = rng * 1664525u + 1013904223u;
        usize len = trial % 3 == 0 ? (rng >> 8) % sizeof m : sizeof m;
        PackTable mt;
        ZipTail mtl;
        if (read_table(m, len, mtl, mt, &why)) {
            KV_CHECK(c, mt.count <= len / 46 + 1);
            for (u32 i = 0; i < mt.count; ++i) data_at(m, len, mt.entries[i].header_offset);
            pack_table_free(mt, mem_general());
        }
    }
    // The same over a pack's index.
    static u8 pk[64 * 1024];
    usize plen = zip.len < sizeof pk ? zip.len : sizeof pk;
    for (u32 trial = 0; trial < 2000; ++trial) {
        memcpy(pk, zip.data, plen);
        rng = rng * 1664525u + 1013904223u;
        usize at = iat + (rng >> 8) % 300;
        pk[at] ^= (u8)(rng | 1);
        PackTable it;
        if (kpak_read_index(pk + iat, 300, tail, mem_general(), &it, &why)) pack_table_free(it, mem_general());
    }
    zip.free();
    again.free();
    junk_out.free();
}
