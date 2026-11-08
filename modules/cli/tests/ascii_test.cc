// The ASCII-only transport form: non-ASCII as \u{XXXX} out, back to UTF-8 in.
#include "korvid/cli/module.h"
#include "korvid/core/test.h"
#include <string.h>

void test_cli_ascii(kv::TestCtx& c) {
    using namespace kv;
    c.begin("cli ascii");
    char out[64], back[64];
    const char* s = "caf\xc3\xa9 \xe2\x80\x94 \\ \xf0\x9f\x98\x80";
    usize n = cli_ascii_escape(s, out, sizeof out);
    KV_CHECK(c, strcmp(out, "caf\\u{E9} \\u{2014} \\\\ \\u{1F600}") == 0 && n == strlen(out));
    for (const char* p = out; *p; ++p) KV_CHECK(c, (unsigned char)*p < 0x80);
    KV_CHECK(c, cli_ascii_unescape(out, back, sizeof back) == strlen(s) && strcmp(back, s) == 0);
    // A malformed byte goes out as the replacement character.
    cli_ascii_escape("a\xff" "b", out, sizeof out);
    KV_CHECK(c, strcmp(out, "a\\u{FFFD}b") == 0);
    // Backslashes that are not escapes come through; a bad escape is left as typed.
    cli_ascii_unescape("C:\\dir \\u{zz} \\u{} \\u{1234567}", back, sizeof back);
    KV_CHECK(c, strcmp(back, "C:\\dir \\u{zz} \\u{} \\u{1234567}") == 0);
    // Truncation never cuts an escape or a sequence, and reports the wanted length.
    char tiny[8];
    KV_CHECK(c, cli_ascii_escape("ab\xc3\xa9" "cd", tiny, sizeof tiny) == 10 && strcmp(tiny, "ab") == 0);
    KV_CHECK(c, cli_ascii_unescape("abcd\xc3\xa9xyz", tiny, 6) == 9 && strcmp(tiny, "abcd") == 0);
}
