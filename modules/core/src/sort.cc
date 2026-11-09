#include "korvid/core/sort.h"

namespace kv {

namespace {

void swap_bytes(u8* a, u8* b, usize n) {
    while (n--) { u8 t = *a; *a++ = *b; *b++ = t; }
}

// Restores the max-heap below root, within [0, end).
void sift_down(u8* base, usize root, usize end, usize size, SortCompare compare, void* ctx) {
    for (;;) {
        usize child = 2 * root + 1;
        if (child >= end) return;
        usize top = root;
        if (compare(base + top * size, base + child * size, ctx) < 0) top = child;
        if (child + 1 < end && compare(base + top * size, base + (child + 1) * size, ctx) < 0) top = child + 1;
        if (top == root) return;
        swap_bytes(base + root * size, base + top * size, size);
        root = top;
    }
}

} // namespace

void sort(void* base, usize count, usize size, SortCompare compare, void* ctx) {
    if (count < 2) return;
    u8* b = (u8*)base;
    if (count <= 16) {
        for (usize i = 1; i < count; ++i)
            for (usize j = i; j > 0 && compare(b + (j - 1) * size, b + j * size, ctx) > 0; --j)
                swap_bytes(b + (j - 1) * size, b + j * size, size);
        return;
    }
    for (usize start = count / 2; start-- > 0;) sift_down(b, start, count, size, compare, ctx);
    for (usize end = count - 1; end > 0; --end) {
        swap_bytes(b, b + end * size, size);
        sift_down(b, 0, end, size, compare, ctx);
    }
}

namespace {
struct NoCtx { SortCompareNoCtx compare; };
int call_no_ctx(const void* a, const void* b, void* ctx) { return ((NoCtx*)ctx)->compare(a, b); }
} // namespace

void sort(void* base, usize count, usize size, SortCompareNoCtx compare) {
    NoCtx c = { compare };
    sort(base, count, size, call_no_ctx, &c);
}

} // namespace kv
