// Korvid pack: the zip format, as packs use it. A .kpak is a zip file (PKWARE APPNOTE)
// that kpack lays out for fast reads: every entry stored, its data on a 4 KiB boundary
// (padding in the local header's extra field, as Android's zipalign does), and a first
// entry ".kpak/index" holding every other entry's path hash, exact data offset, sizes,
// method and CRC, sorted by hash. Any zip tool lists and extracts it.
//
// This header has no I/O. Reading is a sequence of steps over bytes the caller fetched:
//
//   1. the archive's last KV_ZIP_TAIL_MAX bytes -> zip_find_end: where the central directory is
//      (or, for Zip64, where the Zip64 end record is: read 56 bytes there -> zip_read_zip64_end)
//   2. the central directory -> zip_read_central: every entry, hashed, sorted (a plain zip)
//   3. if the table has an index entry: its bytes -> kpak_read_index, which replaces the table
//      with one that knows every data offset (a pack), or refuses (an edited pack: keep step 2's)
//   4. per entry, if its data offset is not known: the local header -> zip_local_data_offset
//
// Paths are UTF-8, '/'-separated, relative: pack_normalize makes "a//b", "./a" and
// "a\b" into "a/b" and refuses "..", absolute paths and empty names. Lookups hash the
// normalized path (core's fixed 64-bit hash) and compare the name, so a hash shared by
// two paths never returns the wrong file.
#pragma once
#include "korvid/core/memory.h"

namespace kv {

constexpr u32   KV_PACK_ALIGN      = 4096;
constexpr usize KV_ZIP_TAIL_MAX    = 22 + 65535 + 20; // end record, the longest comment, the Zip64 locator
constexpr usize KV_ZIP_LOCAL_MAX   = 30 + 65535 + 65535;
constexpr usize KV_PACK_PATH_MAX   = 256;
extern const char* const KV_PACK_INDEX_NAME; // ".kpak/index"

enum ZipMethod : u16 { ZipStored = 0, ZipDeflate = 8 };

enum PackEntryFlags : u16 {
    PackEntryUnsupported = 1, // encrypted, or a method other than stored and deflate: reads fail
    PackEntryNotUtf8     = 2, // the name had no UTF-8 flag and is not valid UTF-8: kept as bytes
};

struct PackEntry {
    u64 hash;          // pack_path_hash of the name
    u64 header_offset; // the local header
    u64 data_offset;   // the data; 0 until known (a plain zip learns it from the local header)
    u64 size;          // uncompressed
    u64 csize;         // as stored
    u32 crc;
    u16 method;        // ZipMethod
    u16 flags;         // PackEntryFlags
    u32 name_off, name_len; // in the table's names
};

// One archive's entries, sorted by hash (then by name), one per name.
struct PackTable {
    PackEntry* entries = nullptr;
    u32        count   = 0;
    char*      names   = nullptr; // NUL after each name
    usize      names_len = 0;     // the names allocation's size
    u32        alloc_count = 0;   // the entries allocation's size, in entries
    bool       indexed = false;   // from a valid .kpak/index: every data offset is known
    // What a mount reports.
    u32 deflated = 0, unaligned = 0, unsupported = 0, not_utf8 = 0, duplicates = 0, skipped = 0;
};

// Where the central directory is.
struct ZipTail {
    u64  cd_offset = 0, cd_size = 0;
    u64  count     = 0;
    bool zip64     = false;  // found through the Zip64 records
    u64  zip64_end = 0;      // when needs_zip64: the Zip64 end record's offset; read 56 bytes there
    bool needs_zip64 = false;
};

// Paths.
bool        pack_normalize(const char* in, char* out, usize cap);
u64         pack_path_hash(const char* normalized);
const char* pack_entry_name(const PackTable& t, const PackEntry& e);

// Reading. Each returns false with a reason in *error on bytes that are not what they
// should be; nothing reads outside [p, p + len).
bool zip_find_end(const u8* tail, usize tail_len, u64 tail_offset, ZipTail* out, const char** error);
bool zip_read_zip64_end(const u8* rec, usize len, ZipTail* tail, const char** error);
bool zip_read_central(const u8* cd, usize cd_len, const ZipTail& tail, Allocator* a, PackTable* out, const char** error);
bool kpak_read_index(const u8* data, usize len, const ZipTail& tail, Allocator* a, PackTable* out, const char** error);
// The data offset from the start of a local header. With fewer than 30 bytes, or fewer
// than the header's name and extra need, false with *need set to the bytes required.
bool zip_local_data_offset(const u8* hdr, usize len, u64 header_offset, u64* data_offset, usize* need);
const PackEntry* pack_find(const PackTable& t, const char* normalized);
void pack_table_free(PackTable& t, Allocator* a);

// Writing a .kpak. Files are written in name order whatever order they come in; names
// must normalize, be unique, and hash uniquely. `write` receives the archive in order
// and returns false to stop. Deterministic: the same files make the same bytes.
struct PackWriteFile { const char* name; const void* data; usize size; };
bool kpak_write(Span<const PackWriteFile> files, bool (*write)(void* user, const void* bytes, usize n), void* user,
                Allocator* scratch, const char** error);

} // namespace kv
