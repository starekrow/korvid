// The parser on mutated documents: a few thousand deterministic cases a run, each a seed
// document with bytes flipped, inserted, deleted, duplicated or cut. Every case must end
// in a tree or an error with its place inside the input, and a failed case must leave no
// allocation behind. Each case is parsed from an exact-size heap copy, so a sanitizer build
// catches any read past the input.
#include "korvid/xml/xml.h"
#include "korvid/core/arena.h"
#include "korvid/core/random.h"
#include "korvid/core/test.h"
#include <stdlib.h>
#include <string.h>

namespace {

const char* const k_seeds[] = {
    R"(<?xml version="1.0"?><!DOCTYPE bulletml SYSTEM "bulletml.dtd">
<bulletml type="vertical"><action label="top"><repeat><times>10 + $rank * 20</times><action>
<fire><direction type="sequence">$rand * 30 - 15</direction><speed>1.5</speed><bullet label="b"/></fire>
<wait>5</wait></action></repeat><changeDirection><direction type="aim">0</direction><term>60</term></changeDirection>
<vanish/></action><bullet label="b"><action><accel><horizontal type="relative">0.1</horizontal><term>30</term></accel></action></bullet>
<fireRef label="f"><param>$1 * 2</param></fireRef></bulletml>)",
    "<a x=\"&lt;&#x263A;\">t<![CDATA[ ]] ]]><b/><!-- c --><?p i?>\xC3\xA9</a>",
    "<!DOCTYPE a [<!ENTITY e 'x'><!ELEMENT a ANY>]><a><b><c><d>deep</d></c></b></a>",
};

} // namespace

void test_xml_fuzz(kv::TestCtx& c) {
    using namespace kv;
    c.begin("xml fuzz");
    Rng r = Rng::seeded(0x786d6c, 5);
    u32 cases = 0, parsed = 0, failed_clean = 0, bad = 0;
    char buf[4096];
    for (u32 round = 0; round < 3000; ++round) {
        const char* seed = k_seeds[round % KV_COUNTOF(k_seeds)];
        usize len = strlen(seed);
        memcpy(buf, seed, len);
        for (u32 m = 0, n = 1 + r.below(4); m < n; ++m) {
            u32 at = len ? r.below((u32)len) : 0;
            switch (r.below(5)) {
            case 0: if (len) buf[at] = (char)r.below(256); break;                         // a byte changed
            case 1: if (len + 1 < sizeof buf) { memmove(buf + at + 1, buf + at, len - at); buf[at] = (char)r.below(256); ++len; } break; // inserted
            case 2: { u32 k = r.below(8); if (at + k <= len) { memmove(buf + at, buf + at + k, len - at - k); len -= k; } break; } // deleted
            case 3: { u32 k = r.below(16); if (at + k <= len && len + k < sizeof buf) { memmove(buf + at + k, buf + at, len - at); len += k; } break; } // duplicated
            default: len = at; break;                                                     // cut
            }
        }
        char* exact = (char*)malloc(len ? len : 1);
        memcpy(exact, buf, len);
        ++cases;
        bool ok = false;
        {
            MemScope scope;
            XmlDoc doc;
            ok = xml_parse(exact, len, mem_scratch(), doc);
            if (ok) { ++parsed; if (!doc.count || doc.root().type != XmlType::Element) ++bad; }
            else if (!doc.error || doc.error_at > len || doc.error_line == 0) ++bad;
        }
        // A failure again, in the general heap: nothing may be left behind.
        if (!ok) {
            MemStats before = mem_stats();
            XmlDoc gen;
            if (xml_parse(exact, len, nullptr, gen) || mem_stats().live_allocs != before.live_allocs) ++bad;
            else ++failed_clean;
        }
        free(exact);
    }
    KV_CHECK(c, cases == 3000 && bad == 0 && failed_clean == cases - parsed);
    KV_CHECK(c, parsed > 0 && parsed < cases); // the mutations reach both outcomes
}
