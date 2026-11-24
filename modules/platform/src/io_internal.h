// Korvid platform: what an IoQueue keeps for itself. A request while the queue has it, and
// a mount. Neither is in io.h: both hold the OS's handles and the backends' bookkeeping,
// which nothing outside the module has a use for.
#pragma once
#include "korvid/platform/io.h"

namespace kv {

struct IoEntry {
    char       path[256];     // the file, or "<pack>:<entry>" for a read from a pack; a longer one cut, for logs
    char*      long_path;     // a path of 256 bytes or more, whole, from the entry's allocator; null: `path` is it
    Allocator* alloc;
    IoPriority priority;
    IoStatus   status;
    u64        user;
    u64        sequence;  // submission order, for stable priority ordering
    bool       write;     // a write: `buf` holds the copy to write, `size` its length
    bool       stat;      // a stat: on the ring, `buf` holds the kernel's answer and a copy of the path
    IoTask*    task;      // a task, or a stat on the task workers: its node, until the pump completes it
    // The ring's bookkeeping while a request is in flight.
    KHandle     fd;
    u8*        buf;
    usize      size;      // the file's size at open, or the bytes to write
    usize      done;      // bytes read or written so far
    bool       in_flight;
    // A ranged read: `length` bytes at `offset` (from a pack, on the pack's own descriptor,
    // which the entry borrows). Then, for a deflated entry, `unpacked` bytes inflated;
    // `check` compares the CRC-32 of what is delivered with `crc`. `clip`: a read_file
    // range, cut to the file at open, where the end of the file is not an error.
    bool       ranged;
    bool       clip;
    bool       fd_borrowed;
    bool       check;
    u16        method;
    i32        fail_errno; // resolved as missing or unreadable: completes Failed at the pump
    u64        offset;
    usize      length;
    usize      unpacked;
    u32        crc;
};

// Where read_asset looks: a directory, or a pack (a zip) whose table was read at mount.
struct IoMount {
    char      path[256] = {}; // the directory (with a trailing '/') or the pack file
    bool      is_pack = false;
    KHandle    fd;          // the pack's file, open for the queue's life
    u64       base = 0;    // where the archive starts in that file (a pack inside another archive)
    char      prefix[64] = {}; // only the archive's entries under this are seen, without it ("assets/")
    PackTable table;
};

} // namespace kv
