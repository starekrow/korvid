#include "korvid/pack/zip.h"
#include "korvid/core/sort.h"
#include "korvid/core/array.h"
#include "korvid/core/bytes.h"
#include "korvid/core/hash.h"
#include "korvid/core/utf8.h"
#include "korvid/inflate/inflate.h"
#include <stdlib.h>
#include <string.h>

namespace kv {

const char* const KV_PACK_INDEX_NAME = ".kpak/index";

namespace {

constexpr u32 kSigLocal   = 0x04034b50;
constexpr u32 kSigCentral = 0x02014b50;
constexpr u32 kSigEnd     = 0x06054b50;
constexpr u32 kSigEnd64   = 0x06064b50;
constexpr u32 kSigLoc64   = 0x07064b50;
constexpr u16 kExtraZip64 = 0x0001;
constexpr u16 kExtraAlign = 0xD935; // zipalign's: u16 alignment, then padding
constexpr u16 kFlagUtf8   = 1u << 11;
constexpr u16 kDosDate    = (0 << 9) | (1 << 5) | 1; // 1980-01-01, the zip epoch
constexpr u32 kIndexMagic = 0x5849504B; // "KPIX"
constexpr usize kIndexHeader = 32, kIndexEntry = 48;

bool fail(const char** error, const char* why) { if (error) *error = why; return false; }

int compare_entry(const void* a, const void* b, void* ctx) {
    const PackEntry& x = *(const PackEntry*)a; const PackEntry& y = *(const PackEntry*)b;
    if (x.hash != y.hash) return x.hash < y.hash ? -1 : 1;
    int c = strcmp((const char*)ctx + x.name_off, (const char*)ctx + y.name_off);
    if (c) return c;
    return x.header_offset < y.header_offset ? -1 : x.header_offset > y.header_offset ? 1 : 0; // later in the archive last
}

// Sorts by hash and name and keeps the last of each name (a zip updated in place appends).
void sort_and_dedupe(PackTable& t) {
    if (t.count > 1) kv::sort(t.entries, t.count, sizeof(PackEntry), compare_entry, t.names);
    u32 m = 0;
    for (u32 i = 0; i < t.count; ++i) {
        if (m && t.entries[m - 1].hash == t.entries[i].hash &&
            strcmp(t.names + t.entries[m - 1].name_off, t.names + t.entries[i].name_off) == 0) {
            t.entries[m - 1] = t.entries[i];
            ++t.duplicates;
            continue;
        }
        t.entries[m++] = t.entries[i];
    }
    t.count = m;
}

void tally(PackTable& t) {
    t.deflated = t.unaligned = t.unsupported = t.not_utf8 = 0;
    for (u32 i = 0; i < t.count; ++i) {
        const PackEntry& e = t.entries[i];
        if (e.method == ZipDeflate) ++t.deflated;
        if (e.flags & PackEntryUnsupported) ++t.unsupported;
        if (e.flags & PackEntryNotUtf8) ++t.not_utf8;
        if (e.data_offset == 0 || e.data_offset % KV_PACK_ALIGN) ++t.unaligned;
    }
}

} // namespace

// ---- paths -------------------------------------------------------------------------------------

bool pack_normalize(const char* in, char* out, usize cap) {
    if (!in || !cap) return false;
    usize n = 0;
    const char* p = in;
    while (*p) {
        // One segment: up to a separator.
        while (*p == '/' || *p == '\\') ++p;
        if (!*p) break;
        const char* seg = p;
        while (*p && *p != '/' && *p != '\\') ++p;
        usize len = (usize)(p - seg);
        if (len == 1 && seg[0] == '.') continue;
        if (len == 2 && seg[0] == '.' && seg[1] == '.') return false;
        if (n && n + 1 >= cap) return false;
        if (n) out[n++] = '/';
        if (n + len >= cap) return false;
        memcpy(out + n, seg, len);
        n += len;
    }
    if (in[0] == '/' || in[0] == '\\') return false; // absolute
    out[n] = 0;
    return n > 0;
}

u64 pack_path_hash(const char* normalized) { return hash_str(normalized); }

const char* pack_entry_name(const PackTable& t, const PackEntry& e) { return t.names + e.name_off; }

// ---- reading -----------------------------------------------------------------------------------

bool zip_find_end(const u8* tail, usize tail_len, u64 tail_offset, ZipTail* out, const char** error) {
    *out = ZipTail{};
    if (tail_len < 22) return fail(error, "too short to be a zip");
    // The end record is the last thing; its comment makes its start uncertain. Scan back
    // for a signature whose comment length lands exactly on the end.
    for (usize at = tail_len - 22 + 1; at-- > 0;) {
        const u8* p = tail + at;
        if (load_le32(p) != kSigEnd) continue;
        u16 comment = load_le16(p + 20);
        if (at + 22 + comment != tail_len) continue;
        if (load_le16(p + 4) != 0 || load_le16(p + 6) != 0) return fail(error, "split archives are not supported");
        out->count     = load_le16(p + 10);
        out->cd_size   = load_le32(p + 12);
        out->cd_offset = load_le32(p + 16);
        bool overflow = out->count == 0xFFFF || out->cd_size == 0xFFFFFFFFu || out->cd_offset == 0xFFFFFFFFu;
        if (at >= 20 && load_le32(p - 20) == kSigLoc64) {
            out->needs_zip64 = true;
            out->zip64_end   = load_le64(p - 20 + 8);
        } else if (overflow) {
            return fail(error, "Zip64 sizes without a Zip64 locator");
        }
        if (!out->needs_zip64 && out->cd_offset + out->cd_size > tail_offset + at) return fail(error, "central directory past the end record");
        return true;
    }
    return fail(error, "no end of central directory record");
}

bool zip_read_zip64_end(const u8* rec, usize len, ZipTail* tail, const char** error) {
    if (len < 56 || load_le32(rec) != kSigEnd64) return fail(error, "bad Zip64 end record");
    if (load_le32(rec + 16) != 0 || load_le32(rec + 20) != 0) return fail(error, "split archives are not supported");
    tail->count       = load_le64(rec + 32);
    tail->cd_size     = load_le64(rec + 40);
    tail->cd_offset   = load_le64(rec + 48);
    tail->zip64       = true;
    tail->needs_zip64 = false;
    return true;
}

bool zip_read_central(const u8* cd, usize cd_len, const ZipTail& tail, Allocator* a, PackTable* out, const char** error) {
    *out = PackTable{};
    if (tail.count > cd_len / 46) return fail(error, "more entries than the central directory holds");
    u32 count = (u32)tail.count;
    PackEntry* entries = (PackEntry*)alloc_in(a, sizeof(PackEntry) * (count ? count : 1));
    // Names: at most the directory's own size, plus a NUL each.
    char* names = (char*)alloc_in(a, cd_len + count + 1);
    if (!entries || !names) { free_in(a, entries, sizeof(PackEntry) * (count ? count : 1)); free_in(a, names, cd_len + count + 1); return fail(error, "out of memory"); }
    usize at = 0, names_len = 0;
    u32 n = 0, skipped = 0;
    for (u32 i = 0; i < count; ++i) {
        if (at + 46 > cd_len || load_le32(cd + at) != kSigCentral) { free_in(a, entries, sizeof(PackEntry) * (count ? count : 1)); free_in(a, names, cd_len + count + 1); return fail(error, "bad central directory entry"); }
        const u8* h = cd + at;
        u16 gp = load_le16(h + 8), method = load_le16(h + 10);
        u32 crc = load_le32(h + 16);
        u64 csize = load_le32(h + 20), size = load_le32(h + 24);
        u16 name_len = load_le16(h + 28), extra_len = load_le16(h + 30), comment_len = load_le16(h + 32);
        u64 offset = load_le32(h + 42);
        usize next = at + 46 + name_len + extra_len + comment_len;
        if (next > cd_len) { free_in(a, entries, sizeof(PackEntry) * (count ? count : 1)); free_in(a, names, cd_len + count + 1); return fail(error, "central directory entry runs past the directory"); }
        // Zip64 values, in order, for the fields that overflowed.
        const u8* x = h + 46 + name_len;
        for (usize xa = 0; xa + 4 <= extra_len;) {
            u16 id = load_le16(x + xa), sz = load_le16(x + xa + 2);
            if (xa + 4 + sz > extra_len) break;
            if (id == kExtraZip64) {
                const u8* v = x + xa + 4;
                usize vi = 0;
                if (size == 0xFFFFFFFFu && vi + 8 <= sz) { size = load_le64(v + vi); vi += 8; }
                if (csize == 0xFFFFFFFFu && vi + 8 <= sz) { csize = load_le64(v + vi); vi += 8; }
                if (offset == 0xFFFFFFFFu && vi + 8 <= sz) { offset = load_le64(v + vi); vi += 8; }
            }
            xa += 4 + sz;
        }
        at = next;
        char raw[KV_PACK_PATH_MAX], norm[KV_PACK_PATH_MAX];
        bool dir = name_len && (h[46 + name_len - 1] == '/' || h[46 + name_len - 1] == '\\');
        if (dir) continue;
        if (name_len == 0 || name_len >= sizeof raw) { ++skipped; continue; }
        memcpy(raw, h + 46, name_len);
        raw[name_len] = 0;
        if (memchr(raw, 0, name_len) || !pack_normalize(raw, norm, sizeof norm)) { ++skipped; continue; }
        if (strcmp(norm, KV_PACK_INDEX_NAME) != 0 && strncmp(norm, ".kpak/", 6) == 0) { ++skipped; continue; } // reserved
        u16 flags = 0;
        if ((gp & 1) || (method != ZipStored && method != ZipDeflate)) flags |= PackEntryUnsupported;
        if (!(gp & kFlagUtf8) && !utf8_valid(norm, strlen(norm))) flags |= PackEntryNotUtf8;
        usize nl = strlen(norm);
        PackEntry& e = entries[n++];
        e = PackEntry{};
        e.hash = pack_path_hash(norm);
        e.header_offset = offset;
        e.size = size; e.csize = csize; e.crc = crc; e.method = method; e.flags = flags;
        e.name_off = (u32)names_len; e.name_len = (u32)nl;
        memcpy(names + names_len, norm, nl + 1);
        names_len += nl + 1;
    }
    out->entries = entries; out->count = n; out->names = names; out->names_len = cd_len + count + 1; // the allocations' sizes, for the free
    out->alloc_count = count ? count : 1;
    out->skipped = skipped;
    sort_and_dedupe(*out);
    tally(*out);
    return true;
}

bool kpak_read_index(const u8* d, usize len, const ZipTail& tail, Allocator* a, PackTable* out, const char** error) {
    *out = PackTable{};
    if (len < kIndexHeader || load_le32(d) != kIndexMagic) return fail(error, "not a kpak index");
    if (load_le32(d + 4) != 1) return fail(error, "unknown kpak index version");
    u32 count = load_le32(d + 8);
    if (load_le64(d + 16) != tail.cd_offset || load_le64(d + 24) != tail.cd_size) return fail(error, "the archive changed after packing");
    if ((len - kIndexHeader) / kIndexEntry < count) return fail(error, "index entries run past the index");
    usize names_at = kIndexHeader + (usize)count * kIndexEntry, names_len = len - names_at;
    PackEntry* entries = (PackEntry*)alloc_in(a, sizeof(PackEntry) * (count ? count : 1));
    char* names = (char*)alloc_in(a, names_len + 1);
    if (!entries || !names) { free_in(a, entries, sizeof(PackEntry) * (count ? count : 1)); free_in(a, names, names_len + 1); return fail(error, "out of memory"); }
    memcpy(names, d + names_at, names_len);
    names[names_len] = 0;
    for (u32 i = 0; i < count; ++i) {
        const u8* p = d + kIndexHeader + (usize)i * kIndexEntry;
        PackEntry& e = entries[i];
        e.hash = load_le64(p); e.data_offset = load_le64(p + 8); e.size = load_le64(p + 16); e.csize = load_le64(p + 24);
        e.crc = load_le32(p + 32); e.method = load_le16(p + 36); e.flags = load_le16(p + 38);
        e.name_off = load_le32(p + 40); e.name_len = load_le32(p + 44);
        e.header_offset = 0;
        bool sorted = i == 0 || entries[i - 1].hash <= e.hash;
        bool named = (usize)e.name_off + e.name_len < names_len + 1 && e.name_off + e.name_len <= names_len &&
                     names[e.name_off + e.name_len] == 0 && pack_path_hash(names + e.name_off) == e.hash;
        if (!sorted || !named || e.data_offset == 0) {
            free_in(a, entries, sizeof(PackEntry) * (count ? count : 1)); free_in(a, names, names_len + 1);
            return fail(error, "a damaged index entry");
        }
    }
    out->entries = entries; out->count = count; out->names = names; out->names_len = names_len + 1;
    out->alloc_count = count ? count : 1;
    out->indexed = true;
    tally(*out);
    return true;
}

bool zip_local_data_offset(const u8* h, usize len, u64 header_offset, u64* data_offset, usize* need) {
    *need = 30;
    if (len < 30) return false;
    if (load_le32(h) != kSigLocal) { *need = 0; return false; }
    usize total = 30 + (usize)load_le16(h + 26) + load_le16(h + 28);
    *need = total;
    if (len < total) return false;
    *data_offset = header_offset + total;
    return true;
}

const PackEntry* pack_find(const PackTable& t, const char* normalized) {
    u64 hash = pack_path_hash(normalized);
    u32 lo = 0, hi = t.count;
    while (lo < hi) { u32 mid = (lo + hi) / 2; if (t.entries[mid].hash < hash) lo = mid + 1; else hi = mid; }
    for (u32 i = lo; i < t.count && t.entries[i].hash == hash; ++i)
        if (strcmp(t.names + t.entries[i].name_off, normalized) == 0) return &t.entries[i];
    return nullptr;
}

void pack_table_free(PackTable& t, Allocator* a) {
    if (t.entries) free_in(a, t.entries, sizeof(PackEntry) * t.alloc_count);
    if (t.names) free_in(a, t.names, t.names_len);
    t = PackTable{};
}

// ---- writing -----------------------------------------------------------------------------------

namespace {

struct Out {
    bool (*write)(void*, const void*, usize);
    void* user;
    u64   at = 0;
    bool  ok = true;
    void bytes(const void* p, usize n) { if (ok && n) ok = write(user, p, n); at += n; }
    void u16v(u16 v) { u8 b[2] = {(u8)v, (u8)(v >> 8)}; bytes(b, 2); }
    void u32v(u32 v) { u8 b[4] = {(u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24)}; bytes(b, 4); }
    void u64v(u64 v) { u32v((u32)v); u32v((u32)(v >> 32)); }
    void zeros(usize n) { static const u8 z[256] = {}; while (n) { usize k = n < sizeof z ? n : sizeof z; bytes(z, k); n -= k; } }
};

struct Planned {
    const char* name;  // normalized, in scratch
    const void* data;
    usize       size;
    u64         hash;
    u32         crc;
    u64         header_offset, data_offset;
    u16         pad;   // the alignment extra's total length (0 for none)
    bool        zip64; // sizes past 32 bits
};

int compare_planned(const void* a, const void* b) { return strcmp(((const Planned*)a)->name, ((const Planned*)b)->name); }

// The alignment extra that puts data starting at `data_at` on the boundary: 0, or at
// least 6 bytes (id, size, the alignment value).
u16 align_pad(u64 data_at) {
    u64 r = data_at % KV_PACK_ALIGN;
    if (r == 0) return 0;
    u64 pad = KV_PACK_ALIGN - r;
    while (pad < 6) pad += KV_PACK_ALIGN;
    return (u16)pad;
}

void local_header(Out& o, const Planned& f) {
    o.u32v(kSigLocal);
    o.u16v(f.zip64 ? 45 : 20);
    o.u16v(kFlagUtf8);
    o.u16v(ZipStored);
    o.u16v(0); o.u16v(kDosDate);
    o.u32v(f.crc);
    o.u32v(f.zip64 ? 0xFFFFFFFFu : (u32)f.size); o.u32v(f.zip64 ? 0xFFFFFFFFu : (u32)f.size);
    u16 nl = (u16)strlen(f.name);
    o.u16v(nl);
    o.u16v((u16)((f.zip64 ? 20 : 0) + f.pad));
    o.bytes(f.name, nl);
    if (f.zip64) { o.u16v(kExtraZip64); o.u16v(16); o.u64v(f.size); o.u64v(f.size); }
    if (f.pad) { o.u16v(kExtraAlign); o.u16v((u16)(f.pad - 4)); o.u16v((u16)KV_PACK_ALIGN); o.zeros(f.pad - 6); }
}

void central_header(Out& o, const Planned& f) {
    bool big_off = f.header_offset >= 0xFFFFFFFFu;
    u16 x64 = (u16)((f.zip64 ? 16 : 0) + (big_off ? 8 : 0));
    o.u32v(kSigCentral);
    o.u16v((u16)(3 << 8 | ((f.zip64 || big_off) ? 45 : 20))); // made by UNIX
    o.u16v((f.zip64 || big_off) ? 45 : 20);
    o.u16v(kFlagUtf8);
    o.u16v(ZipStored);
    o.u16v(0); o.u16v(kDosDate);
    o.u32v(f.crc);
    o.u32v(f.zip64 ? 0xFFFFFFFFu : (u32)f.size); o.u32v(f.zip64 ? 0xFFFFFFFFu : (u32)f.size);
    u16 nl = (u16)strlen(f.name);
    o.u16v(nl);
    o.u16v(x64 ? (u16)(4 + x64) : 0);
    o.u16v(0); o.u16v(0); o.u16v(0);
    o.u32v(0100644u << 16); // a regular file, rw-r--r--
    o.u32v(big_off ? 0xFFFFFFFFu : (u32)f.header_offset);
    o.bytes(f.name, nl);
    if (x64) {
        o.u16v(kExtraZip64); o.u16v(x64);
        if (f.zip64) { o.u64v(f.size); o.u64v(f.size); }
        if (big_off) o.u64v(f.header_offset);
    }
}

} // namespace

bool kpak_write(Span<const PackWriteFile> files, bool (*write)(void*, const void*, usize), void* user, Allocator* scratch, const char** error) {
    u32 n = (u32)files.len;
    Planned* plan = (Planned*)alloc_in(scratch, sizeof(Planned) * (n + 1));
    char* names = (char*)alloc_in(scratch, (usize)(n + 1) * KV_PACK_PATH_MAX);
    if (!plan || !names) return fail(error, "out of memory");
    for (u32 i = 0; i < n; ++i) {
        char* nm = names + (usize)(i + 1) * KV_PACK_PATH_MAX;
        if (!files[i].name || !pack_normalize(files[i].name, nm, KV_PACK_PATH_MAX)) return fail(error, "a name that does not normalize");
        if (strncmp(nm, ".kpak/", 6) == 0) return fail(error, "names under .kpak/ are reserved");
        if (!utf8_valid(nm, strlen(nm))) return fail(error, "a name that is not UTF-8");
        Planned& p = plan[i + 1];
        p = Planned{};
        p.name = nm; p.data = files[i].data; p.size = files[i].size;
        p.hash = pack_path_hash(nm);
        p.crc  = crc32(0, files[i].data, files[i].size);
        p.zip64 = files[i].size >= 0xFFFFFFFFu;
    }
    kv::sort(plan + 1, n, sizeof(Planned), compare_planned);
    for (u32 i = 1; i < n; ++i)
        if (strcmp(plan[i].name, plan[i + 1].name) == 0) return fail(error, "a name given twice");
    // Hash collisions: sort hashes, look for neighbours.
    u64* hashes = (u64*)alloc_in(scratch, sizeof(u64) * (n ? n : 1));
    for (u32 i = 0; i < n; ++i) hashes[i] = plan[i + 1].hash;
    kv::sort(hashes, n, sizeof(u64), [](const void* a, const void* b) { u64 x = *(const u64*)a, y = *(const u64*)b; return x < y ? -1 : x > y ? 1 : 0; });
    for (u32 i = 1; i < n; ++i) if (hashes[i] == hashes[i - 1]) return fail(error, "two names share a path hash; rename one");

    // The index: header, entries by hash, names in name order. Its size is known before
    // the offsets it records, so the layout is one pass.
    usize names_len = 0;
    for (u32 i = 1; i <= n; ++i) names_len += strlen(plan[i].name) + 1;
    usize index_size = kIndexHeader + (usize)n * kIndexEntry + names_len;
    u8* index = (u8*)alloc_in(scratch, index_size);
    if (!index) return fail(error, "out of memory");
    Planned& ix = plan[0];
    ix = Planned{};
    ix.name = KV_PACK_INDEX_NAME; ix.data = index; ix.size = index_size;

    // Layout.
    u64 at = 0;
    for (u32 i = 0; i <= n; ++i) {
        Planned& p = plan[i];
        p.header_offset = at;
        u64 fixed = 30 + strlen(p.name) + (p.zip64 ? 20 : 0);
        p.pad = align_pad(at + fixed);
        p.data_offset = at + fixed + p.pad;
        at = p.data_offset + p.size;
    }
    u64 cd_offset = at, cd_size = 0;
    for (u32 i = 0; i <= n; ++i) {
        bool big_off = plan[i].header_offset >= 0xFFFFFFFFu;
        u16 x64 = (u16)((plan[i].zip64 ? 16 : 0) + (big_off ? 8 : 0));
        cd_size += 46 + strlen(plan[i].name) + (x64 ? 4 + x64 : 0);
    }

    // Fill the index now that the offsets are known.
    struct ByHash { u64 hash; u32 i; };
    ByHash* order = (ByHash*)alloc_in(scratch, sizeof(ByHash) * (n ? n : 1));
    for (u32 i = 0; i < n; ++i) order[i] = ByHash{plan[i + 1].hash, i + 1};
    kv::sort(order, n, sizeof(ByHash), [](const void* a, const void* b) { u64 x = ((const ByHash*)a)->hash, y = ((const ByHash*)b)->hash; return x < y ? -1 : x > y ? 1 : 0; });
    auto put16 = [](u8* p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); };
    auto put32 = [](u8* p, u32 v) { for (u32 k = 0; k < 4; ++k) p[k] = (u8)(v >> (8 * k)); };
    auto put64 = [&](u8* p, u64 v) { put32(p, (u32)v); put32(p + 4, (u32)(v >> 32)); };
    memset(index, 0, index_size);
    put32(index, kIndexMagic); put32(index + 4, 1); put32(index + 8, n); put64(index + 16, cd_offset); put64(index + 24, cd_size);
    usize name_at = 0;
    u32* name_offs = (u32*)alloc_in(scratch, sizeof(u32) * (n + 1));
    for (u32 i = 1; i <= n; ++i) {
        usize l = strlen(plan[i].name);
        memcpy(index + kIndexHeader + (usize)n * kIndexEntry + name_at, plan[i].name, l + 1);
        name_offs[i] = (u32)name_at;
        name_at += l + 1;
    }
    for (u32 k = 0; k < n; ++k) {
        const Planned& p = plan[order[k].i];
        u8* e = index + kIndexHeader + (usize)k * kIndexEntry;
        put64(e, p.hash); put64(e + 8, p.data_offset); put64(e + 16, p.size); put64(e + 24, p.size);
        put32(e + 32, p.crc); put16(e + 36, ZipStored); put16(e + 38, 0);
        put32(e + 40, name_offs[order[k].i]); put32(e + 44, (u32)strlen(p.name));
    }
    ix.crc = crc32(0, index, index_size);

    // Out it goes: entries, the central directory, the end records.
    Out o{write, user};
    for (u32 i = 0; i <= n; ++i) {
        local_header(o, plan[i]);
        o.bytes(plan[i].data, plan[i].size);
    }
    for (u32 i = 0; i <= n; ++i) central_header(o, plan[i]);
    u64 total = (u64)n + 1;
    bool z64 = total >= 0xFFFF || cd_offset >= 0xFFFFFFFFu || cd_size >= 0xFFFFFFFFu;
    if (z64) {
        u64 end64 = o.at;
        o.u32v(kSigEnd64); o.u64v(44); o.u16v(3 << 8 | 45); o.u16v(45); o.u32v(0); o.u32v(0);
        o.u64v(total); o.u64v(total); o.u64v(cd_size); o.u64v(cd_offset);
        o.u32v(kSigLoc64); o.u32v(0); o.u64v(end64); o.u32v(1);
    }
    static const char comment[] = "kpak 1";
    o.u32v(kSigEnd); o.u16v(0); o.u16v(0);
    o.u16v(z64 ? 0xFFFF : (u16)total); o.u16v(z64 ? 0xFFFF : (u16)total);
    o.u32v(z64 ? 0xFFFFFFFFu : (u32)cd_size); o.u32v(z64 ? 0xFFFFFFFFu : (u32)cd_offset);
    o.u16v(sizeof comment - 1);
    o.bytes(comment, sizeof comment - 1);
    if (!o.ok) return fail(error, "the write failed");
    return true;
}

} // namespace kv
