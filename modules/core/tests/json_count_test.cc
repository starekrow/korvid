// The JSON parser's count of its own nodes. Before it parses, json_parse scans the text
// once and allocates that many nodes, no more and never fewer than the parse will use;
// these cases try to make the scan and the parser disagree about where strings, comments
// and empty containers are. Every case is parsed from an exact-size heap copy through an
// allocator that notes the size of the node block, and must end in a tree of exactly that
// many nodes or in an error that is not the parser running out of counted nodes. The
// allocator gives every block its own malloc, so a sanitizer build catches a read past
// the input and a write past the nodes alike.
#include "korvid/core/json.h"
#include "korvid/core/random.h"
#include "korvid/core/test.h"
#include <stdlib.h>
#include <string.h>

namespace {

using namespace kv;

struct Tally {
    Allocator a;
    u32       calls = 0;
    usize     first = 0; // the node block: json_parse allocates it before any string
    void**    blocks = nullptr;
    u32       held = 0, room = 0;
};

void* tally_alloc(Allocator* a, usize size, u32 align) {
    Tally* t = (Tally*)a;
    if (!t->calls++) t->first = size;
    if (t->held == t->room) { t->room = t->room ? t->room * 2 : 64; t->blocks = (void**)realloc(t->blocks, sizeof(void*) * t->room); }
    void* p = align <= KV_MEM_DEFAULT_ALIGN ? malloc(size ? size : 1) : nullptr; // exact, so one byte past it is caught
    t->blocks[t->held++] = p;
    return p;
}
void tally_free(Allocator*, void*, usize) {}

// Every node reachable from `v`, each child inside the block and each run ended by its flag.
bool walk(const JsonDoc& doc, const JsonValue& v, u32& seen) {
    ++seen;
    u32 n = 0;
    for (const JsonValue* k = json_first(doc, v); k; k = json_next(doc, *k), ++n) {
        if (k <= doc.nodes || k >= doc.nodes + doc.count || n >= v.count()) return false;
        if (!walk(doc, *k, seen)) return false;
    }
    return n == v.count();
}

enum Outcome { Parsed, Failed, Broken };

// `nodes` gets the tree's size when it parses.
Outcome probe(const char* text, usize len, u32 flags, u32* nodes = nullptr) {
    char* exact = (char*)malloc(len ? len : 1);
    memcpy(exact, text, len);
    Tally t;
    t.a.alloc = tally_alloc;
    t.a.free  = tally_free;
    JsonDoc doc;
    bool ok = json_parse(exact, len, &t.a, doc, flags);
    Outcome out = Broken;
    if (ok) {
        u32 seen = 0;
        if (!doc.error && doc.count && t.first == sizeof(JsonValue) * doc.count && walk(doc, doc.root(), seen) && seen == doc.count) out = Parsed;
        if (nodes) *nodes = doc.count;
    } else if (doc.error && strcmp(doc.error, "more values than were counted") != 0 && doc.error_at <= len
               && doc.count == 1 && doc.root().is_null() && !json_first(doc, doc.root()) && t.first >= sizeof(JsonValue)) {
        out = Failed;
    }
    for (u32 i = 0; i < t.held; ++i) free(t.blocks[i]);
    free(t.blocks);
    free(exact);
    return out;
}

struct Case { const char* text; u32 flags; u32 nodes; }; // nodes 0: must fail
constexpr u32 C = KV_JSON_LINE_COMMENTS;

const Case k_cases[] = {
    // Empty containers have no children, however much stands between the brackets.
    {"[]", 0, 1}, {"{}", 0, 1}, {"[[]]", 0, 2}, {"[ [ ] , { } ]", 0, 3}, {"[\n\t\r ]", 0, 1}, {"[[],[[]],{}]", 0, 5},
    {"[//]\n]", C, 1}, {"{// }\n\n // {\n}", C, 1}, {"[ // one\n // two\n [ // three\n ] ]", C, 2},
    // Structure inside strings is text.
    {"\"a,b,[,{\"", 0, 1}, {"{\"a,\":\",\",\"[\":\"]\"}", 0, 3}, {"[\"[\",\"{\",\"]\",\"}\",\",\"]", 0, 6},
    {"[\"\\\",\", \"\\\\\", \",\"]", 0, 4},        // ["\",", "\\", ","]: an escaped quote, then a string ending in a backslash
    {"[\"\\\\\\\",,\", 1]", 0, 3},                 // ["\\\",,", 1]
    {"[\"\\u005b,\\u002c\", \"\\/,\"]", 0, 3},
    {"{\"\\\"\":[\"\\\"\"],\",\\\"\":{}}", 0, 4}, // {"\"":["\""],",\"":{}}
    // Comments: only with the flag, and never inside a string.
    {"\"//,,,\"", 0, 1}, {"\"//,,,\"", C, 1}, {"[\"//\", \"/\", \"a//b,\", \"http://x/,\"]", C, 5},
    {"[1,2]//,,,", C, 3}, {"//[,\n[1]//]\n", C, 2}, {"[// ,\n 1 // ,\n , 2]", C, 3}, {"[1,//\n2]", C, 3},
    {"// \"\n[1,2] // \"", C, 3}, {"[1, // \"open , ,\n 2, \"// not one\"]", C, 4},
    {"{\"a\"// ,\n:// ,\n1// ,\n,\"b\":[]// ,\n}", C, 3}, {"[1]//", C, 2}, {"[1]// \\", C, 2},
    {"[1,2]//,,,", 0, 0}, {"[1 // x\n]", 0, 0}, {"[1,/ /\n2]", C, 0}, {"[1,/\n/2]", C, 0}, {"[/]", C, 0}, {"/", C, 0}, {"//", C, 0}, {"// [1,2]", C, 0},
    {"[1 // a comment ends at a line feed, not a return\r 2]", C, 0},
    // Text that does not parse, counted all the same.
    {"[,]", 0, 0}, {"[}", 0, 0}, {"{]", 0, 0}, {"[1,,2]", 0, 0}, {"[[[[", 0, 0}, {"]]]]", 0, 0}, {",,,,", 0, 0}, {"[1,2", 0, 0},
    {"\"abc\\", 0, 0}, {"\"\\\"", 0, 0}, {"[\"a\\\"]", 0, 0}, {"[\"x\\", 0, 0}, {"[\"a\", \"b]", 0, 0}, {"[\"a\" \"b\"]", 0, 0},
    {"{\"a\":1,}", 0, 0}, {"[1 2 3]", 0, 0}, {"{\"a\" \"b\"}", 0, 0}, {"[tru,e]", 0, 0}, {"[1]]", 0, 0}, {"[1],[2]", 0, 0},
    {"{{}}", 0, 0}, {"[{]}", 0, 0}, {"{\"a\":[}", 0, 0}, {"[1,2,3,4,5,6,7,8,x]", 0, 0}, {"{,}", 0, 0}, {"[\"\n\",1,2]", 0, 0},
    {"", 0, 0}, {" ", 0, 0}, {"[", 0, 0}, {"{", 0, 0}, {"\"", 0, 0}, {",", 0, 0},
};

// A random document that parses, with the strings and (when allowed) the comments that
// would mislead a careless count. Returns how many nodes it must make.
struct Gen {
    Rng   r;
    char* buf;
    usize len = 0, cap;
    bool  comments;

    void put(const char* s) { usize n = strlen(s); if (len + n < cap) { memcpy(buf + len, s, n); len += n; } }
    void gap() {
        static const char* const plain[] = {"", "", " ", "\n", "\t", " \r\n "};
        static const char* const noted[] = {"//\n", "// , [ { \" \\\n", " //]}\n\t", "//,,,,\n//\"\n"};
        put(plain[r.below(KV_COUNTOF(plain))]);
        if (comments && r.below(4) == 0) put(noted[r.below(KV_COUNTOF(noted))]);
    }
    void text() {
        static const char* const strs[] = {
            "\"\"", "\"a\"", "\",\"", "\"[\"", "\"]\"", "\"{,}\"", "\"//\"", "\"// , \"", "\"\\\"\"", "\"\\\\\"", "\"\\\\\\\"\"",
            "\"\\\",[\"", "\"a\\\\\"", "\"\\u005b\\u002c\"", "\"/\\/\"", "\"\\\"//\\\"\"", "\"[[[[,,,,\"",
        };
        put(strs[r.below(KV_COUNTOF(strs))]);
    }
    u32 value(u32 depth) {
        gap();
        u32 kind = r.below(depth >= 6 ? 4 : 8);
        u32 nodes = 1;
        if (kind < 2) { static const char* const lit[] = {"0", "-1.5e3", "true", "false", "null", "18446744073709551615"}; put(lit[r.below(KV_COUNTOF(lit))]); }
        else if (kind < 4) text();
        else {
            bool object = kind >= 6;
            put(object ? "{" : "[");
            for (u32 i = 0, n = r.below(5); i < n && len + 64 < cap; ++i) {
                if (i) { gap(); put(","); }
                if (object) { gap(); text(); gap(); put(":"); }
                nodes += value(depth + 1);
            }
            gap();
            put(object ? "}" : "]");
        }
        gap();
        return nodes;
    }
};

} // namespace

void test_json_count(kv::TestCtx& c) {
    c.begin("json count");

    for (const Case& k : k_cases) {
        u32 nodes = 0;
        Outcome o = probe(k.text, strlen(k.text), k.flags, &nodes);
        bool good = k.nodes ? o == Parsed && nodes == k.nodes : o == Failed;
        KV_CHECK(c, good);
        if (!good) KV_ERROR("test", "json count '%s' (flags %u): outcome %d, %u nodes, wanted %u", k.text, k.flags, (int)o, nodes, k.nodes);
    }

    // A NUL is a byte like any other to the count, and the text need not end in one.
    KV_CHECK(c, probe("[1,\0,2]", 7, 0) == Failed && probe("[\"a\0,\"]", 7, 0) == Failed && probe("[1,2]]]],,,[[", 5, 0) == Parsed);

    // Deep, and wide: as deep as is allowed, one past it, and a few thousand siblings.
    char deep[2 * (KV_JSON_MAX_DEPTH + 1)];
    for (u32 i = 0; i < KV_JSON_MAX_DEPTH + 1; ++i) { deep[i] = '['; deep[KV_JSON_MAX_DEPTH + 1 + i] = ']'; }
    u32 nodes = 0;
    KV_CHECK(c, probe(deep + 1, 2 * KV_JSON_MAX_DEPTH, 0, &nodes) == Parsed && nodes == KV_JSON_MAX_DEPTH);
    KV_CHECK(c, probe(deep, sizeof deep, 0) == Failed && probe(deep, KV_JSON_MAX_DEPTH + 1, 0) == Failed);
    static char wide[5000 * 12 + 2];
    usize wn = 0;
    wide[wn++] = '[';
    for (u32 i = 0; i < 5000; ++i) { static const char* const el[] = {"1,", "[],", "\",\",", "{\"a\":[2]},"}; const char* e = el[i % 4]; memcpy(wide + wn, e, strlen(e)); wn += strlen(e); }
    wide[wn - 1] = ']';
    KV_CHECK(c, probe(wide, wn, 0, &nodes) == Parsed && nodes == 1 + 1250 * (1 + 1 + 1 + 3));

    // Random documents that parse: the count is exact. Then every prefix of some, and each
    // one with bytes changed, inserted, deleted, doubled or cut: a tree or a clean error.
    Rng r = Rng::seeded(0x6a736f6e, 24);
    static char doc_text[8192], mutated[8192 + 64];
    u32 broken = 0, parsed = 0, failed = 0, generated = 0;
    for (u32 round = 0; round < 1500; ++round) {
        bool comments = round % 2 != 0;
        Gen g{r, doc_text, 0, sizeof doc_text, comments};
        u32 want = g.value(0);
        r = g.r;
        u32 flags = comments ? C : 0;
        u32 got = 0;
        if (probe(doc_text, g.len, flags, &got) != Parsed || got != want) { if (!broken++) KV_ERROR("test", "json count: round %u made %u nodes, wanted %u: %.*s", round, got, want, (int)g.len, doc_text); continue; }
        ++generated;
        // Without the flag a document with a comment in it fails, cleanly.
        if (comments && probe(doc_text, g.len, 0) == Broken) ++broken;
        if (round % 25 == 0)
            for (usize cut = 0; cut < g.len; ++cut)
                if (probe(doc_text, cut, flags) == Broken) { if (!broken++) KV_ERROR("test", "json count: round %u cut at %u", round, (u32)cut); }
        for (u32 variant = 0; variant < 4; ++variant) {
            usize len = g.len;
            memcpy(mutated, doc_text, len);
            for (u32 m = 0, n = 1 + r.below(3); m < n; ++m) {
                static const char alphabet[] = "[]{},:\"\\/ \n1a";
                char ch = alphabet[r.below(sizeof alphabet - 1)];
                u32 at = len ? r.below((u32)len) : 0;
                switch (r.below(5)) {
                case 0: if (len) mutated[at] = ch; break;
                case 1: memmove(mutated + at + 1, mutated + at, len - at); mutated[at] = ch; ++len; break;
                case 2: { u32 k = 1 + r.below(4); if (at + k <= len) { memmove(mutated + at, mutated + at + k, len - at - k); len -= k; } break; }
                case 3: { u32 k = 1 + r.below(8); if (at + k <= len) { memmove(mutated + at + k, mutated + at, len - at); len += k; } break; }
                default: len = at; break;
                }
            }
            for (u32 f = 0; f < 2; ++f) {
                Outcome o = probe(mutated, len, f ? C : 0);
                if (o == Parsed) ++parsed; else if (o == Failed) ++failed;
                else if (!broken++) KV_ERROR("test", "json count: round %u variant %u flags %u: %.*s", round, variant, f, (int)len, mutated);
            }
        }
    }
    KV_CHECK(c, broken == 0 && generated == 1500);
    KV_CHECK(c, parsed > 500 && failed > 500); // the mutations reach both outcomes
}
