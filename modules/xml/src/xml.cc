// yxml, once, and the Korvid tree over it. yxml is fed the document a byte at a time and
// answers with tokens (an element starts, a byte of an attribute's value, a byte of
// content, an element ends); this file turns them into the flat tree of xml.h, checks
// what yxml leaves to its caller (the input is UTF-8, no NUL, no repeated attribute, the
// depth), and names every error's place. Nothing of yxml is visible outside this file.
#include "korvid/xml/xml.h"
#include "korvid/core/utf8.h"
#include <string.h>

// yxml is vendored, not ours: its warnings are silenced for its own lines only.
#if KV_COMPILER_MSVC
#pragma warning(push, 0)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wtype-limits"
#endif
#include "vendor/yxml.c"
#if KV_COMPILER_MSVC
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

namespace kv {

namespace {

constexpr usize kNameStack = 4096; // yxml's stack of open element names and the current attribute's

// A growable array in the caller's allocator, doubling: indices stay valid, pointers do
// not until the parse is done.
template <class T>
struct Grow {
    Allocator* alloc = nullptr;
    T*         p     = nullptr;
    u32        n = 0, cap = 0;
    bool reserve(u32 more) {
        if (n + more <= cap) return true;
        u32 ncap = cap ? cap : 64;
        while (ncap < n + more) {
            if (ncap > 0x40000000u) return false;
            ncap *= 2;
        }
        T* q = alloc_items<T>(alloc, ncap);
        if (!q) return false;
        if (n) memcpy(q, p, sizeof(T) * n);
        free_items(alloc, p, cap);
        p = q;
        cap = ncap;
        return true;
    }
    bool push(const T& v) { if (!reserve(1)) return false; p[n++] = v; return true; }
    bool append(const T* v, u32 count) { if (!reserve(count)) return false; memcpy(p + n, v, sizeof(T) * count); n += count; return true; }
    // Appends a run of the array itself, by index: a reserve may move it.
    bool append_own(u32 from, u32 count) { if (!reserve(count)) return false; memcpy(p + n, p + from, sizeof(T) * count); n += count; return true; }
};

// Strings are offsets into one pool while it grows; xml_parse turns them into pointers
// at the end.
struct NodeAt { u32 name, text; };
struct AttrAt { u32 name, value, len; };
struct Open { u32 node, last; }; // an element still open, and its last child so far

struct Builder {
    const char* text;
    usize       len;
    XmlDoc*     doc;
    Grow<XmlNode> nodes;
    Grow<NodeAt>  node_at;
    Grow<XmlAttr> attrs;
    Grow<AttrAt>  attr_at;
    Grow<char>    pool;
    Open          open[KV_XML_MAX_DEPTH];
    u32           depth = 0;
    u32           run = KV_XML_NONE;    // the pool offset of the text run being read
    u32           attr_name = 0, attr_value = KV_XML_NONE;
    const char*   error = nullptr;
    u32           line = 1, col = 1;    // where the byte being read is
    usize         at = 0;

    bool fail(const char* why) {
        if (!error) error = why;
        return false;
    }
    u32 str(const char* s, usize n) {
        u32 off = pool.n;
        if (n > 0x40000000u || !pool.reserve((u32)n + 1)) { fail("out of memory"); return 0; }
        memcpy(pool.p + pool.n, s, n);
        pool.n += (u32)n;
        pool.p[pool.n++] = 0;
        return off;
    }
    // A new node, the last child of the element open now (if any).
    u32 node(XmlType type, u32 name, u32 text_off, u32 text_len) {
        XmlNode nd;
        nd.type = type;
        nd.len = text_len;
        nd.line = line;
        nd.parent = depth ? open[depth - 1].node : KV_XML_NONE;
        u32 i = nodes.n;
        if (!nodes.push(nd) || !node_at.push(NodeAt{name, text_off})) { fail("out of memory"); return KV_XML_NONE; }
        if (depth) {
            Open& o = open[depth - 1];
            if (o.last == KV_XML_NONE) nodes.p[o.node].first = i;
            else nodes.p[o.last].next = i;
            o.last = i;
        }
        return i;
    }
    // The text run being read, if any, becomes a text node of the element open now.
    bool close_run() {
        if (run == KV_XML_NONE) return true;
        u32 n = pool.n - run;
        if (!pool.push(0)) return fail("out of memory");
        u32 empty = pool.n - 1; // a run's name is "", the NUL just written
        bool ok = node(XmlType::Text, empty, run, n) != KV_XML_NONE;
        run = KV_XML_NONE;
        return ok;
    }
    // An element's own character data: its text children, concatenated and trimmed.
    bool own_text(u32 element) {
        u32 start = pool.n;
        for (u32 c = nodes.p[element].first; c != KV_XML_NONE; c = nodes.p[c].next)
            if (nodes.p[c].type == XmlType::Text && !pool.append_own(node_at.p[c].text, nodes.p[c].len)) return fail("out of memory");
        u32 b = start, e = pool.n;
        auto space = [](char ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r'; };
        while (b < e && space(pool.p[b])) ++b;
        while (e > b && space(pool.p[e - 1])) --e;
        // Moved down over the untrimmed start (memmove: the ranges overlap).
        memmove(pool.p + start, pool.p + b, e - b);
        pool.n = start + (e - b);
        if (!pool.push(0)) return fail("out of memory");
        node_at.p[element].text = start;
        nodes.p[element].len = e - b;
        return true;
    }

    // A token's bytes. The input is checked UTF-8 as it is read, a byte at a time; what
    // yxml hands back several bytes at once and not as ASCII is a character reference it
    // expanded, and yxml's own check lets the surrogates through (D800-DFFF: its range
    // test is off), which would put invalid UTF-8 in a document said to have none.
    bool data(const yxml_t& x) {
        u32 n = (u32)strlen(x.data);
        if (n > 1 && (u8)x.data[0] >= 0x80 && !utf8_valid(x.data, n)) return fail("an unknown entity or a bad character reference");
        return pool.append(x.data, n) || fail("out of memory");
    }

    bool token(yxml_t& x, yxml_ret_t r) {
        switch (r) {
        case YXML_ELEMSTART: {
            if (!close_run()) return false;
            if (depth == KV_XML_MAX_DEPTH) return fail("nested too deeply");
            u32 name = str(x.elem, yxml_symlen(&x, x.elem));
            u32 i = node(XmlType::Element, name, 0, 0);
            if (i == KV_XML_NONE || error) return false;
            nodes.p[i].attr_first = attrs.n;
            open[depth++] = Open{i, KV_XML_NONE};
            return true;
        }
        case YXML_ATTRSTART:
            attr_name = str(x.attr, yxml_symlen(&x, x.attr));
            attr_value = pool.n;
            return !error;
        case YXML_ATTRVAL:
            return data(x);
        case YXML_ATTREND: {
            u32 n = pool.n - attr_value;
            if (!pool.push(0)) return fail("out of memory");
            XmlNode& el = nodes.p[open[depth - 1].node];
            for (u32 a = el.attr_first; a < attrs.n; ++a)
                if (!strcmp(pool.p + attr_at.p[a].name, pool.p + attr_name)) return fail("an attribute is repeated");
            if (!attrs.push(XmlAttr{nullptr, nullptr, n}) || !attr_at.push(AttrAt{attr_name, attr_value, n})) return fail("out of memory");
            ++el.attr_count;
            return true;
        }
        case YXML_CONTENT:
            if (run == KV_XML_NONE) run = pool.n;
            return data(x);
        case YXML_ELEMEND:
            if (!close_run()) return false;
            if (!own_text(open[depth - 1].node)) return false;
            --depth;
            return true;
        default:
            return true; // processing instructions are skipped
        }
    }
};

const char* yxml_error_name(yxml_ret_t r) {
    switch (r) {
    case YXML_EEOF:   return "the document ends early";
    case YXML_EREF:   return "an unknown entity or a bad character reference";
    case YXML_ECLOSE: return "a closing tag does not match its opening tag";
    case YXML_ESTACK: return "names nested too deeply or too long";
    default:          return "a syntax error";
    }
}

} // namespace

bool xml_parse(const char* text, usize len, Allocator* alloc, XmlDoc& doc) {
    doc = XmlDoc{};
    if (!alloc) alloc = mem_general();
    Builder b;
    b.text = text; b.len = len; b.doc = &doc;
    b.nodes.alloc = alloc; b.node_at.alloc = alloc; b.attrs.alloc = alloc; b.attr_at.alloc = alloc; b.pool.alloc = alloc;
    // On the stack, not the scratch arena: the caller's document may be in that arena,
    // and a scope here would release it.
    u8 stack[kNameStack];
    yxml_t x;
    yxml_init(&x, stack, kNameStack);
    bool ok = true;
    // UTF-8 checked as it goes, so the error is where the bad byte is.
    u32 cont = 0; // continuation bytes still to check in the current sequence
    for (usize i = 0; ok && i < len; ++i) {
        b.at = i;
        u8 c = (u8)text[i];
        if (c == 0) { ok = b.fail("a NUL byte"); break; }
        if (!cont && c >= 0x80) {
            u32 adv = 0;
            u32 cp = utf8_decode(text + i, len - i, &adv);
            if (cp == KV_UTF8_REPLACEMENT && adv == 1) { ok = b.fail("not UTF-8"); break; }
            cont = adv - 1;
        } else if (cont) --cont;
        yxml_ret_t r = yxml_parse(&x, c);
        if (r < 0) { ok = b.fail(yxml_error_name(r)); break; }
        if (r != YXML_OK && !b.token(x, r)) { ok = false; break; }
        if (c == '\n') { ++b.line; b.col = 1; }
        else if ((c & 0xC0) != 0x80) ++b.col;
    }
    if (ok && yxml_eof(&x) != YXML_OK) { b.at = len; ok = b.fail("the document ends early"); }
    if (ok && b.nodes.n == 0) ok = b.fail("no root element");
    if (!ok) {
        doc.error = b.error ? b.error : "a syntax error";
        doc.error_at = (u32)b.at;
        doc.error_line = b.line;
        doc.error_col = b.col;
        free_items(alloc, b.nodes.p, b.nodes.cap);
        free_items(alloc, b.node_at.p, b.node_at.cap);
        free_items(alloc, b.attrs.p, b.attrs.cap);
        free_items(alloc, b.attr_at.p, b.attr_at.cap);
        free_items(alloc, b.pool.p, b.pool.cap);
        return false;
    }
    // The pool is final: offsets become pointers.
    for (u32 i = 0; i < b.nodes.n; ++i) {
        b.nodes.p[i].name = b.pool.p + b.node_at.p[i].name;
        b.nodes.p[i].text = b.pool.p + b.node_at.p[i].text;
    }
    for (u32 i = 0; i < b.attrs.n; ++i) {
        b.attrs.p[i].name = b.pool.p + b.attr_at.p[i].name;
        b.attrs.p[i].value = b.pool.p + b.attr_at.p[i].value;
    }
    free_items(alloc, b.node_at.p, b.node_at.cap);
    free_items(alloc, b.attr_at.p, b.attr_at.cap);
    doc.nodes = b.nodes.p;
    doc.count = b.nodes.n;
    doc.attrs = b.attrs.p;
    doc.attr_count = b.attrs.n;
    return true;
}

namespace {
const XmlNode* element_from(const XmlDoc& doc, u32 i, const char* name) {
    for (; i != KV_XML_NONE && i < doc.count; i = doc.nodes[i].next)
        if (doc.nodes[i].type == XmlType::Element && (!name || !strcmp(doc.nodes[i].name, name))) return &doc.nodes[i];
    return nullptr;
}
} // namespace

const XmlNode* xml_child(const XmlDoc& doc, const XmlNode& parent, const char* name) { return element_from(doc, parent.first, name); }
const XmlNode* xml_next_named(const XmlDoc& doc, const XmlNode& sibling, const char* name) { return element_from(doc, sibling.next, name); }

u32 xml_count(const XmlDoc& doc, const XmlNode& parent, const char* name) {
    u32 n = 0;
    for (const XmlNode* c = xml_child(doc, parent, name); c; c = xml_next_named(doc, *c, name)) ++n;
    return n;
}

const char* xml_attr(const XmlDoc& doc, const XmlNode& element, const char* name) {
    for (u32 a = element.attr_first; a < element.attr_first + element.attr_count && a < doc.attr_count; ++a)
        if (!strcmp(doc.attrs[a].name, name)) return doc.attrs[a].value;
    return nullptr;
}

const char* xml_attr_or(const XmlDoc& doc, const XmlNode& element, const char* name, const char* dflt) {
    const char* v = xml_attr(doc, element, name);
    return v ? v : dflt;
}

} // namespace kv
