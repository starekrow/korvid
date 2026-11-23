#include "korvid/platform/io.h"
#include "korvid/core/format.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include "../src/wtf8.h"
#include <errno.h>
#include <string.h>
#if !defined(_WIN32)
#include <sys/stat.h> // chmod
#include <unistd.h>   // symlink, geteuid
#endif

namespace {

// What a listing said, in order: "name:kind" with kind f, d or n (none), an l before it
// for a link and an h after it for hidden; "?" for a name that is not UTF-8. a.txt's
// size and time are kept to check.
struct Seen {
    char      text[1024] = {};
    kv::usize len        = 0;
    kv::u32   count      = 0;
    kv::u32   stop_after = ~0u;
    kv::u64   a_size     = ~0ull;
    kv::i64   a_mtime    = 0;
};

bool seen_one(void* user, const kv::IoDirEntry& e) {
    Seen* s = (Seen*)user;
    const char* kind = e.st.is_file ? "f" : e.st.is_dir ? "d" : "n";
    s->len += (kv::usize)kv::format(s->text + s->len, sizeof s->text - s->len, "%s:%s%s%s ",
                                    e.utf8 ? e.name : "?", e.is_link ? "l" : "", kind, e.hidden ? "h" : "");
    if (strcmp(e.name, "a.txt") == 0) { s->a_size = e.st.size; s->a_mtime = e.st.mtime_ns; }
    return ++s->count < s->stop_after;
}

kv::u32 wait_for(kv::IoQueue& q, kv::u32 count) {
    for (kv::u32 i = 0; i < 2000 && q.completions().len < count; ++i) { q.pump(); kv::time_sleep(0.001); }
    return q.completions().len;
}

} // namespace

void test_wtf8(kv::TestCtx& c) {
    using namespace kv;
    c.begin("wtf8");
    // Valid UTF-16, a pair included, is UTF-8: "aé€😀".
    const u16 w1[] = {0x61, 0xE9, 0x20AC, 0xD83D, 0xDE00, 0};
    char s[32];
    KV_CHECK(c, wtf8_from_utf16(w1, s, sizeof s) == 11);
    KV_CHECK(c, strcmp(s, "a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80") == 0);
    u16 back[16];
    KV_CHECK(c, utf16_from_wtf8(s, back, 16) == 6 && memcmp(back, w1, sizeof w1) == 0);
    // A lone high surrogate, and a lone low one after a letter: three bytes each, and back.
    const u16 w2[] = {0xD800, 0x62, 0xDC01, 0};
    KV_CHECK(c, wtf8_from_utf16(w2, s, sizeof s) == 8);
    KV_CHECK(c, memcmp(s, "\xed\xa0\x80" "b" "\xed\xb0\x81", 8) == 0);
    KV_CHECK(c, utf16_from_wtf8(s, back, 16) == 4 && memcmp(back, w2, sizeof w2) == 0);
    // Measuring, and a buffer one short.
    KV_CHECK(c, wtf8_from_utf16(w2, nullptr, 0) == 8 && wtf8_from_utf16(w2, s, 7) == 0);
    KV_CHECK(c, utf16_from_wtf8("abc", nullptr, 0) == 4 && utf16_from_wtf8("abc", back, 3) == 0);
    // Not WTF-8: a stray continuation, a truncated sequence, an overlong '/', past U+10FFFF.
    KV_CHECK(c, utf16_from_wtf8("\x80", back, 16) == -1);
    KV_CHECK(c, utf16_from_wtf8("\xe2\x82", back, 16) == -1);
    KV_CHECK(c, utf16_from_wtf8("\xc0\xaf", back, 16) == -1);
    KV_CHECK(c, utf16_from_wtf8("\xf4\x90\x80\x80", back, 16) == -1);
}

void test_io_list_dir(kv::TestCtx& c) {
    using namespace kv;
    c.begin("io_list_dir");
    char dir[200], path[400];
    KV_CHECK(c, io_make_temp_dir("korvid-list", dir, sizeof dir));
    auto at = [&](const char* name) { format(path, sizeof path, "%s/%s", dir, name); return path; };
    KV_CHECK(c, io_write_now(at("a.txt"), "abc", 3));
    KV_CHECK(c, io_write_now(at(".hidden"), "h", 1));
    KV_CHECK(c, io_make_dirs(at("b/deeper")));
    KV_CHECK(c, io_write_now(at("b/nested.txt"), "nested", 6));
    // A name that is not UTF-8, which old collections have: bytes on POSIX, a lone
    // surrogate on Windows (WTF-8 through the engine).
#if defined(_WIN32)
    const char* odd = "\xed\xa0\x80-old.flac";
#else
    const char* odd = "\xff\xfe-old.flac";
#endif
    KV_CHECK(c, io_write_now(at(odd), "odd bytes", 9));
    bool links = false;
#if !defined(_WIN32)
    links = symlink("a.txt", at("link-a")) == 0 && symlink("b", at("link-b")) == 0 && symlink("nowhere", at("link-gone")) == 0;
    KV_CHECK(c, links);
#endif

    // Kinds only: the hidden entry left out, b's own entries not listed, links marked.
    Seen s1;
    i32 err = -1;
    KV_CHECK(c, io_list_dir(dir, 0, seen_one, &s1, &err));
    // Bytewise order puts the odd name (0xED or 0xFF first) after every ASCII one.
    const char* want1 = links ? "a.txt:f b:d link-a:lf link-b:ld link-gone:ln ?:f " : "a.txt:f b:d ?:f ";
    KV_CHECK(c, strcmp(s1.text, want1) == 0);
    KV_CHECK(c, err == -1);                    // untouched on success
    KV_CHECK(c, s1.a_size == 0 && s1.a_mtime == 0); // no sizes asked

    // Hidden entries and sizes.
    Seen s2;
    KV_CHECK(c, io_list_dir(dir, IoListHidden | IoListSizes, seen_one, &s2));
    KV_CHECK(c, strncmp(s2.text, ".hidden:fh a.txt:f ", 19) == 0);
    IoStat st;
    KV_CHECK(c, io_stat(at("a.txt"), &st) && s2.a_size == 3 && s2.a_mtime == st.mtime_ns);

    // The odd name, joined to the directory, opens the same file through the queue.
    struct Odd { char name[64] = {}; bool utf8 = true; } o;
    KV_CHECK(c, io_list_dir(dir, 0, [](void* user, const IoDirEntry& e) {
        Odd* got = (Odd*)user;
        if (!e.utf8) { format(got->name, sizeof got->name, "%s", e.name); got->utf8 = false; }
        return true;
    }, &o));
    KV_CHECK(c, !o.utf8 && strcmp(o.name, odd) == 0);
    {
        IoQueue q;
        KV_CHECK(c, q.init(nullptr, 8));
        IoRequest r; r.path = at(o.name); r.user = 1;
        Handle h = q.read_file(r);
        KV_CHECK(c, !h.is_null() && wait_for(q, 1) == 1);
        const IoCompletion& cc = q.completions()[0];
        KV_CHECK(c, cc.status == IoStatus::Done && cc.size == 9 && memcmp(cc.data, "odd bytes", 9) == 0);
        if (cc.data) mem_free(cc.data);
        q.reap(h);
        q.free();
    }

    // A stop is not a failure.
    Seen s3;
    s3.stop_after = 1;
    KV_CHECK(c, io_list_dir(dir, 0, seen_one, &s3) && s3.count == 1);

    // An empty directory lists nothing; failures say why.
    Seen s4;
    KV_CHECK(c, io_list_dir(at("b/deeper"), 0, seen_one, &s4) && s4.count == 0);
    err = 0;
    KV_CHECK(c, !io_list_dir(at("not-here"), 0, seen_one, &s4, &err) && err == ENOENT);
    err = 0;
    KV_CHECK(c, !io_list_dir(at("a.txt"), 0, seen_one, &s4, &err) && err == ENOTDIR);
    err = 0;
    KV_CHECK(c, !io_list_dir("", 0, seen_one, &s4, &err) && err == ENOENT);
#if !defined(_WIN32)
    if (geteuid() != 0) { // root lists anything
        KV_CHECK(c, chmod(at("b"), 0) == 0);
        err = 0;
        KV_CHECK(c, !io_list_dir(at("b"), 0, seen_one, &s4, &err) && err == EACCES);
        chmod(at("b"), 0755);
    }
#endif
    KV_CHECK(c, io_remove_tree(dir));
}
