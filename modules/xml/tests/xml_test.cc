// The tree, the walk, and every edge the header states, one check each.
#include "korvid/xml/xml.h"
#include "korvid/core/arena.h"
#include "korvid/core/test.h"
#include <string.h>

namespace {

bool parse(const char* text, kv::XmlDoc& doc) { return kv::xml_parse(text, strlen(text), kv::mem_scratch(), doc); }

bool fails(const char* text, const char* why_part, kv::u32 line = 0) {
    kv::XmlDoc doc;
    if (parse(text, doc)) return false;
    return doc.error && strstr(doc.error, why_part) && (!line || doc.error_line == line);
}

} // namespace

void test_xml(kv::TestCtx& c) {
    using namespace kv;
    c.begin("xml");
    MemScope scope;
    XmlDoc doc;

    // A small BulletML document: elements, attributes, own text trimmed, the walk.
    const char* bml = R"(<?xml version="1.0" ?>
<!DOCTYPE bulletml SYSTEM "bulletml.dtd">
<bulletml type="vertical" xmlns="http://www.asahi-net.or.jp/~cs8k-cyu/bulletml">
  <!-- a ring -->
  <action label="top">
    <repeat> <times>4 + $rank * 8</times>
      <action>
        <fire><direction type="sequence">360 / (4 + $rank * 8)</direction><bullet/></fire>
      </action>
    </repeat>
    <wait>30</wait>
  </action>
</bulletml>)";
    KV_CHECK(c, parse(bml, doc));
    const XmlNode& root = doc.root();
    KV_CHECK(c, !strcmp(root.name, "bulletml") && !strcmp(xml_attr_or(doc, root, "type", ""), "vertical"));
    KV_CHECK(c, xml_attr(doc, root, "xmlns") != nullptr && xml_attr(doc, root, "nope") == nullptr);
    KV_CHECK(c, !strcmp(xml_attr_or(doc, root, "nope", "dflt"), "dflt"));
    const XmlNode* top = xml_child(doc, root, "action");
    KV_CHECK(c, top && !strcmp(xml_attr_or(doc, *top, "label", ""), "top") && xml_count(doc, *top) == 2);
    const XmlNode* rep = xml_child(doc, *top, "repeat");
    const XmlNode* times = rep ? xml_child(doc, *rep, "times") : nullptr;
    KV_CHECK(c, times && !strcmp(xml_text(*times), "4 + $rank * 8") && times->line == 6);
    const XmlNode* wait = rep ? xml_next_named(doc, *rep, "wait") : nullptr;
    KV_CHECK(c, wait && !strcmp(xml_text(*wait), "30") && wait->parent == (u32)(top - doc.nodes));
    const XmlNode* dir = xml_child(doc, *xml_child(doc, *xml_child(doc, *rep, "action"), "fire"), "direction");
    KV_CHECK(c, dir && !strcmp(xml_text(*dir), "360 / (4 + $rank * 8)") && !strcmp(xml_attr_or(doc, *dir, "type", ""), "sequence"));
    KV_CHECK(c, xml_child(doc, root, nullptr) == top && xml_child(doc, *top, "nothing") == nullptr);

    // Mixed content: text nodes in order; the element's own text is theirs, joined, trimmed.
    KV_CHECK(c, parse("<p>  one <b>two</b> three  </p>", doc));
    const XmlNode& p = doc.root();
    KV_CHECK(c, !strcmp(xml_text(p), "one  three"));
    const XmlNode& t1 = doc.nodes[p.first];
    KV_CHECK(c, t1.type == XmlType::Text && !strcmp(t1.text, "  one ") && t1.len == 6);
    const XmlNode& b = doc.nodes[t1.next];
    KV_CHECK(c, b.type == XmlType::Element && !strcmp(xml_text(b), "two") && doc.nodes[b.next].type == XmlType::Text);

    // Entities: the five and character references, to UTF-8; nothing else.
    KV_CHECK(c, parse("<a v=\"&lt;&amp;&quot;\">&gt;&apos;&#65;&#x263A;</a>", doc));
    KV_CHECK(c, !strcmp(xml_attr(doc, doc.root(), "v"), "<&\"") && !strcmp(xml_text(doc.root()), ">'A\xE2\x98\xBA"));
    KV_CHECK(c, doc.attrs[0].len == 3);
    KV_CHECK(c, fails("<a>&nbsp;</a>", "entity"));
    KV_CHECK(c, fails("<a>&#0;</a>", "entity"));
    KV_CHECK(c, fails("<a>&#xD800;</a>", "entity"));       // a surrogate: yxml lets it by, the wrapper does not
    KV_CHECK(c, fails("<a b=\"&#xDFFF;\"/>", "entity"));
    KV_CHECK(c, fails("<a>&#xE000;</a>", "entity"));       // yxml's range test also refuses U+E000-U+E7FE (stated in the README)
    KV_CHECK(c, parse("<a>&#xE800;\xEE\x80\x80</a>", doc) && !strcmp(xml_text(doc.root()), "\xEE\xA0\x80\xEE\x80\x80")); // past it, and the character itself

    // A DOCTYPE and its internal subset are skipped, never applied.
    KV_CHECK(c, parse("<!DOCTYPE a [ <!ENTITY e \"boom\"> <!ATTLIST a x CDATA \"dflt\"> ]><a/>", doc));
    KV_CHECK(c, xml_attr(doc, doc.root(), "x") == nullptr);
    KV_CHECK(c, fails("<!DOCTYPE a [ <!ENTITY e \"boom\"> ]><a>&e;</a>", "entity"));

    // CDATA is character data; comments and processing instructions are skipped.
    KV_CHECK(c, parse("<a><![CDATA[<x> & ]]><!-- no --><?pi skipped?>y</a>", doc));
    KV_CHECK(c, !strcmp(xml_text(doc.root()), "<x> & y") && xml_child(doc, doc.root()) == nullptr);

    // Namespaces are names; a byte-order mark is skipped.
    KV_CHECK(c, parse("\xEF\xBB\xBF<ns:a ns:b=\"1\"><ns:c/></ns:a>", doc));
    KV_CHECK(c, !strcmp(doc.root().name, "ns:a") && !strcmp(xml_attr(doc, doc.root(), "ns:b"), "1") && xml_child(doc, doc.root(), "ns:c"));

    // UTF-8 only, no NUL; errors say where.
    KV_CHECK(c, parse("<a>caf\xC3\xA9</a>", doc) && !strcmp(xml_text(doc.root()), "caf\xC3\xA9"));
    KV_CHECK(c, fails("<a>\n\n  caf\xE9</a>", "UTF-8", 3));
    {
        XmlDoc bad;
        KV_CHECK(c, !parse("<a>\n\n  caf\xE9</a>", bad) && bad.error_at == 10 && bad.error_col == 6);
        const char nul[] = {'<', 'a', '>', 0, '<', '/', 'a', '>'};
        KV_CHECK(c, !xml_parse(nul, sizeof nul, mem_scratch(), bad) && strstr(bad.error, "NUL") && bad.error_at == 3);
    }

    // One root; tags match; a document ends whole; attributes once each.
    KV_CHECK(c, fails("<a/><b/>", "syntax"));
    KV_CHECK(c, fails("<a></b>", "closing tag"));
    KV_CHECK(c, fails("<a><b></a>", "closing tag"));
    KV_CHECK(c, fails("<a>", "ends early"));
    KV_CHECK(c, fails("", "ends early"));
    KV_CHECK(c, fails("<!-- only -->", "ends early"));
    KV_CHECK(c, fails("<a x=\"1\" x=\"2\"/>", "repeated"));
    KV_CHECK(c, fails("<a x=1/>", "syntax"));
    KV_CHECK(c, fails("<a>\n<b>\n<c x=\"1></c></b></a>", "syntax", 3));

    // Depth: 64 nested elements parse, 65 do not; a name too long for the stack fails.
    char deep[1024];
    auto nest = [&](u32 n) {
        usize at = 0;
        for (u32 i = 0; i < n; ++i) { memcpy(deep + at, "<e>", 3); at += 3; }
        for (u32 i = 0; i < n; ++i) { memcpy(deep + at, "</e>", 4); at += 4; }
        deep[at] = 0;
    };
    nest(KV_XML_MAX_DEPTH);
    KV_CHECK(c, parse(deep, doc) && doc.count == KV_XML_MAX_DEPTH);
    nest(KV_XML_MAX_DEPTH + 1);
    KV_CHECK(c, fails(deep, "nested too deeply"));
    static char long_name[6000];
    long_name[0] = '<';
    memset(long_name + 1, 'n', 5000);
    memcpy(long_name + 5001, "/>", 3);
    KV_CHECK(c, fails(long_name, "too long"));

    // A failed parse in the general allocator leaves nothing behind.
    MemStats before = mem_stats();
    XmlDoc gen;
    KV_CHECK(c, !xml_parse("<a><b x=\"1\">text</b><c></a>", 27, nullptr, gen));
    KV_CHECK(c, mem_stats().live_allocs == before.live_allocs);
}
