// Korvid renderer-d3d12: shaders. A DXIL blob holds one entry point, where a SPIR-V module
// or a Metal library holds them all, so the shader rule has slangc write its container: a
// zip with one signed blob per entry point (children/<n>/<n>.dxil, numbered, with Slang's IR
// beside them, which nothing here reads). Each blob is found by the entry point's name,
// which its PSV0 part (pipeline state validation, version 3 and up) carries in its string
// table. Nothing compiles at run time.
#include "renderer_internal.h"
#include "korvid/core/bytes.h"
#include "korvid/core/format.h"
#include "korvid/inflate/inflate.h"
#include "korvid/pack/zip.h"
#include "korvid/platform/io.h"
#include <string.h>

namespace kv { namespace KV_RB_NS {

namespace {

// The entry point a DXIL container was compiled for, from its PSV0 part; null when the
// container has none (a validator older than 1.8 writes PSV0 without the name).
const char* dxil_entry_name(const u8* blob, usize size) {
    if (size < 32 || memcmp(blob, "DXBC", 4) != 0) return nullptr;
    u32 parts = load_le32(blob + 28);
    if (32 + (u64)parts * 4 > size) return nullptr;
    for (u32 i = 0; i < parts; ++i) {
        u32 off = load_le32(blob + 32 + i * 4);
        if ((u64)off + 8 > size) return nullptr;
        u32 part_size = load_le32(blob + off + 4);
        if (memcmp(blob + off, "PSV0", 4) != 0) continue;
        const u8* p = blob + off + 8;
        const u8* end = p + part_size;
        if (end > blob + size || p + 4 > end) return nullptr;
        u32 info = load_le32(p);           // sizeof(PSVRuntimeInfo<version>)
        if (info < 52 || p + 4 + info + 4 > end) return nullptr; // version 3 is 52 bytes
        u32 name_off = load_le32(p + 4 + 48); // PSVRuntimeInfo3::EntryFunctionName
        const u8* q = p + 4 + info;
        u32 resources = load_le32(q); q += 4;
        if (resources) {
            if (q + 4 > end) return nullptr;
            u32 bind_size = load_le32(q); q += 4;
            q += (u64)resources * bind_size;
        }
        if (q + 4 > end) return nullptr;
        u32 strings = load_le32(q); q += 4;
        if (q + strings > end || name_off >= strings) return nullptr;
        const char* s = (const char*)q + name_off;
        if (!memchr(s, 0, strings - name_off)) return nullptr;
        return s;
    }
    return nullptr;
}

} // namespace

void rb_free_shader(ShaderBlob& b) {
    free_in(mem_general(), b.data, b.size);
    b = ShaderBlob{};
}

// Every one of `entries` out of <exe>/shaders/<file>, read once. All or nothing.
bool rb_load_shader_entries(Renderer* r, const char* file, const char* const* entries, ShaderBlob* outs, u32 n) {
    char path[1024];
    format(path, sizeof path, "%sshaders/%s", r->base_path, file);
    IoBytes zip;
    i32 read_err = 0;
    if (!io_read_now(path, &zip, nullptr, &read_err)) { KV_ERROR("d3d12", "cannot read shader '%s': %s", path, strerror(read_err)); return false; }
    const char* err = "";
    usize tail_len = zip.size < KV_ZIP_TAIL_MAX ? zip.size : KV_ZIP_TAIL_MAX;
    ZipTail tail;
    PackTable table;
    bool ok = zip_find_end(zip.data + zip.size - tail_len, tail_len, zip.size - tail_len, &tail, &err) && !tail.needs_zip64 &&
              tail.cd_offset + tail.cd_size <= zip.size &&
              zip_read_central(zip.data + tail.cd_offset, (usize)tail.cd_size, tail, mem_general(), &table, &err);
    if (!ok) { KV_ERROR("d3d12", "shader '%s' is not slangc's container: %s", path, err); zip.free(); return false; }

    for (u32 i = 0; i < n; ++i) outs[i] = ShaderBlob{};
    u32 found = 0;
    for (u32 e = 0; e < table.count && found < n; ++e) {
        const PackEntry& pe = table.entries[e];
        const char* name = pack_entry_name(table, pe);
        usize len = strlen(name);
        if (len < 5 || strcmp(name + len - 5, ".dxil") != 0 || (pe.flags & PackEntryUnsupported)) continue;
        u64 data_off = pe.data_offset;
        usize need = 0;
        if (!data_off && (pe.header_offset >= zip.size ||
                          !zip_local_data_offset(zip.data + pe.header_offset, (usize)(zip.size - pe.header_offset), pe.header_offset, &data_off, &need)))
            continue;
        if (data_off + pe.csize > zip.size) continue;
        u8* blob = (u8*)alloc_in(mem_general(), (usize)pe.size ? (usize)pe.size : 1);
        bool good = false;
        if (pe.method == ZipStored && pe.csize == pe.size) { memcpy(blob, zip.data + data_off, (usize)pe.size); good = true; }
        else if (pe.method == ZipDeflate) {
            InflateOut o = inflate_raw(zip.data + data_off, (usize)pe.csize, blob, (usize)pe.size);
            good = o.result == InflateResult::Ok && o.produced == pe.size;
        }
        const char* entry = good ? dxil_entry_name(blob, (usize)pe.size) : nullptr;
        u32 want = n;
        for (u32 i = 0; entry && i < n; ++i) if (!outs[i].data && strcmp(entries[i], entry) == 0) want = i;
        if (want == n) { free_in(mem_general(), blob, (usize)pe.size ? (usize)pe.size : 1); continue; }
        outs[want].data = blob;
        outs[want].size = (usize)pe.size;
        ++found;
    }
    pack_table_free(table, mem_general());
    zip.free();
    if (found == n) {
        KV_INFO("d3d12", "shader '%s': %u entry point%s", file, n, n == 1 ? "" : "s");
        return true;
    }
    for (u32 i = 0; i < n; ++i) {
        if (!outs[i].data) KV_ERROR("d3d12", "shader '%s' has no entry point '%s'", path, entries[i]);
        rb_free_shader(outs[i]);
    }
    return false;
}

bool rb_load_shader(Renderer* r, const char* file, const char* entry, ShaderBlob* out) {
    return rb_load_shader_entries(r, file, &entry, out, 1);
}

} } // namespace kv::KV_RB_NS
