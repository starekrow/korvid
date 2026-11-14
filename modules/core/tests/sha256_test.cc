#include "korvid/core/sha256.h"
#include "korvid/core/test.h"
#include <string.h>

void test_sha256(kv::TestCtx& c) {
    using namespace kv;
    c.begin("sha256");
    u8 d[32];
    char hex[65];
    // The standard's vectors.
    sha256("", 0, d); sha256_hex(d, hex);
    KV_CHECK(c, strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
    sha256("abc", 3, d); sha256_hex(d, hex);
    KV_CHECK(c, strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    const char* two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256(two, strlen(two), d); sha256_hex(d, hex);
    KV_CHECK(c, strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);
    // A million 'a's, fed in pieces of every awkward size: the same digest as in one go.
    static char a[1000000];
    memset(a, 'a', sizeof a);
    sha256(a, sizeof a, d); sha256_hex(d, hex);
    KV_CHECK(c, strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0);
    Sha256 s;
    s.init();
    usize at = 0;
    for (usize step = 1; at < sizeof a; step = step * 3 % 257 + 1) {
        usize n = step < sizeof a - at ? step : sizeof a - at;
        s.update(a + at, n);
        at += n;
    }
    u8 d2[32];
    s.finish(d2);
    KV_CHECK(c, memcmp(d, d2, 32) == 0);
    // Lengths around the padding's edge.
    const usize edges[] = {55, 56, 63, 64, 65, 119, 120};
    for (usize n : edges) {
        sha256(a, n, d);
        s.init(); s.update(a, n / 2); s.update(a + n / 2, n - n / 2); s.finish(d2);
        KV_CHECK(c, memcmp(d, d2, 32) == 0);
    }
}
