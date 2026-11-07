#include "korvid/core/name.h"
#include "korvid/core/test.h"
#include <string.h>

void test_name(kv::TestCtx& c) {
    using namespace kv;
    c.begin("name");

    NameTable t;
    KV_CHECK(c, t.init(nullptr, 1u << 20, "test.names"));

    // Interning copies; the same string is the same name; the bytes come back.
    char buf[32];
    memcpy(buf, "hello/ball.png", 15);
    Name a = t.intern(buf);
    memset(buf, 'x', sizeof buf);
    Name b = t.intern("hello/ball.png");
    KV_CHECK(c, !a.is_null() && a == b && strcmp(t.str(a), "hello/ball.png") == 0 && t.count == 1);
    KV_CHECK(c, t.intern("hello/other.png") != a && t.count == 2);
    KV_CHECK(c, t.lookup("hello/ball.png") == a && t.lookup("never").is_null());
    KV_CHECK(c, t.str(Name{}) [0] == 0 && t.str(Name{12345})[0] == 0);
    KV_CHECK(c, t.intern(nullptr).is_null());
    Name e = t.intern("");
    KV_CHECK(c, !e.is_null() && t.str(e)[0] == 0);

    // A Name is a map key like any other.
    Map<Name, u32> by_name;
    by_name.set(a, 1);
    KV_CHECK(c, *by_name.find(t.intern("hello/ball.png")) == 1);
    by_name.free();
    t.free();

    // The process table.
    Name g = name_intern("engine.core");
    KV_CHECK(c, name_intern("engine.core") == g && strcmp(name_str(g), "engine.core") == 0);
    KV_CHECK(c, name_lookup("engine.core") == g);
}
