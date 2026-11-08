# pack

The zip format, as Korvid's packs use it. A `.kpak` is an ordinary zip file that any zip
tool lists and extracts, laid out by `kpack` so the engine reads it fast: every entry
stored, its data on a 4 KiB boundary, and a first entry `.kpak/index` that knows every
other entry's exact data offset. This module reads and writes that format and nothing
else: no files, no I/O. The platform's mounts and the `kpack` tool both use it, so the
game and the tool cannot disagree about a pack.

## How it works

**Reading is steps over bytes the caller fetched.** A mount (in the platform's I/O
queue) does them with `pread`:

1. The archive's last `KV_ZIP_TAIL_MAX` bytes: `zip_find_end` finds the end record
   (scanning back over any comment) and says where the central directory is, or, for a
   Zip64 archive, where the Zip64 end record is (`zip_read_zip64_end` on its 56 bytes).
2. The central directory: `zip_read_central` turns every file entry into a `PackEntry`
   (Zip64 sizes and offsets applied), normalizes and hashes its name, sorts by hash and
   keeps the last of a name given twice. Directory entries are skipped; names that climb
   out (`..`), are absolute or empty are skipped and counted; encrypted entries and
   methods other than stored and deflate are kept but marked unsupported; names without
   the UTF-8 flag that are not valid UTF-8 are kept as bytes and counted.
3. If `.kpak/index` is there: its bytes to `kpak_read_index`. The index records the
   central directory's offset and size when it was written; if they still match, its
   table (every data offset known) replaces step 2's. If not, the archive was edited
   after packing and step 2's table stands.
4. For an entry whose data offset is not known (any zip but a pack): its local header
   to `zip_local_data_offset`. The local header's extra field may differ from the
   central directory's, so the offset cannot be computed without reading it. That is the
   degraded path: one more small read per entry, once.

**Writing** (`kpak_write`) takes the files in any order and writes them in name order:
the index first, each entry stored with zipalign's padding extra field (id `0xD935`)
putting its data on the boundary, the central directory, Zip64 records only past 4 GiB
or 65,535 entries, and an end record with the comment `kpak 1`. Every timestamp is the
zip epoch (1980-01-01), every name is flagged UTF-8, so the same files make the same
bytes. A name that does not normalize, a name given twice, a name under `.kpak/`, and two
names that share a path hash are refused.

**Paths.** `pack_normalize` makes `a//b`, `./a` and `a\b` into `a/b` and refuses `..`,
absolute paths and empty names. Lookups hash the normalized path with core's fixed
64-bit hash and compare the name, so a hash shared by two paths never answers wrong.

## The index

`.kpak/index`, little-endian:

| bytes | what |
|---|---|
| 0..31 | `KPIX`, version 1, entry count, reserved, the central directory's offset and size (u64 each) |
| 48 per entry, by hash | path hash (u64), data offset, size, stored size (u64 each), CRC-32 (u32), method (u16), flags (u16), name offset and length (u32 each) |
| the rest | names, NUL after each, in name order |

## Quick reference

Header `korvid/pack/zip.h`:

| name | kind | what |
|---|---|---|
| `PackEntry`, `PackTable` | records | an entry (hash, offsets, sizes, CRC, method, flags, name); a sorted table and what a mount reports |
| `ZipTail` | record | where the central directory is; `needs_zip64` and `zip64_end` |
| `pack_normalize`, `pack_path_hash`, `pack_entry_name`, `pack_find` | functions | paths and lookups |
| `zip_find_end`, `zip_read_zip64_end`, `zip_read_central`, `kpak_read_index`, `zip_local_data_offset` | functions | the reading steps |
| `pack_table_free` | function | a table's two allocations |
| `PackWriteFile`, `kpak_write(files, write, user, scratch, &error)` | record, function | a whole pack, through a write callback |
| `KV_PACK_ALIGN`, `KV_ZIP_TAIL_MAX`, `KV_PACK_PATH_MAX`, `KV_PACK_INDEX_NAME` | constants | 4096, the tail to read, 256-byte names, `.kpak/index` |

Not an engine module: no state, nothing registered.

## Behaviour worth knowing

- **Nothing reads outside the bytes it was given.** The test suite flips bytes in and cuts
  short a zip from Python and a pack's index thousands of times; every step either
  refuses or returns a table whose offsets the next step checks.
- **The writer holds nothing but the plan.** File data goes straight to the callback, so
  a caller that streams to disk holds one file at a time. `kpack` collects into memory
  for now.
- **Names are bytes after normalization.** No Unicode normalization: `é` written two ways
  is two paths, as everywhere in the engine.

## Not in this module

Compression on write, zstd entries, encryption, split archives, data descriptors on write,
and anything that opens a file.
