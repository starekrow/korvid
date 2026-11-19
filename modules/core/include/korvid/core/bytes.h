// Korvid core: little-endian integers read out of bytes. The engine's files and the
// formats it reads (zip, gzip, WAV, DXIL containers) store integers little-endian at any
// alignment; these read one from wherever it sits, on any host, without a cast through
// an unaligned pointer. Compilers turn each into a single load where the host allows.
#pragma once
#include "korvid/core/types.h"

namespace kv {

// The 16-bit little-endian integer at `p`, which needs no alignment.
inline u16 load_le16(const u8* p) { return (u16)(p[0] | p[1] << 8); }
// The 32-bit little-endian integer at `p`, which needs no alignment.
inline u32 load_le32(const u8* p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
// The 64-bit little-endian integer at `p`, which needs no alignment.
inline u64 load_le64(const u8* p) { return (u64)load_le32(p) | (u64)load_le32(p + 4) << 32; }

} // namespace kv
