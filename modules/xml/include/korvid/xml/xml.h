// Korvid xml: an XML reader. Parses a text into a flat tree of plain nodes in an allocator
// of the caller's choosing (the scratch arena, usually: nothing here frees), the way the
// JSON reader does, so a document's structure can be walked and copied out without any
// allocation of its own. The parser underneath is yxml (vendored, MIT), non-validating.
//
//   XmlDoc doc;
//   if (!xml_parse(text, len, mem_scratch(), doc)) KV_ERROR("...", "%s at line %u", doc.error, doc.error_line);
//   for (const XmlNode* f = xml_child(doc, doc.root(), "fire"); f; f = xml_next_named(doc, *f, "fire"))
//       const char* label = xml_attr(doc, *f, "label");
//
// What it reads, and what it does not:
//   - UTF-8 only. A byte-order mark is skipped; any byte sequence that is not UTF-8, and
//     any NUL, is an error. The XML declaration's `encoding` is not read.
//   - One root element. Comments, processing instructions and the XML declaration are
//     skipped. CDATA sections are character data.
//   - The five predefined entities (&lt; &gt; &amp; &apos; &quot;) and numeric character
//     references, expanded to UTF-8. Any other entity is an error, never an expansion.
//   - A DOCTYPE and its internal subset are skipped, never applied (no defaults, no
//     entities from it).
//   - Namespaces are not processed: `a:b` is a name like any other.
//   - Attribute values are unescaped, not normalized; a repeated attribute is an error.
//   - Nesting is at most KV_XML_MAX_DEPTH elements; a name (with its open ancestors')
//     must fit the parser's 4 KiB name stack.
// Every error names its byte offset, line and column (a column counts bytes, from 1).
#pragma once
#include "korvid/core/memory.h"

namespace kv {

constexpr u32 KV_XML_MAX_DEPTH = 64;
constexpr u32 KV_XML_NONE      = 0xFFFFFFFFu;

enum class XmlType : u8 { Element, Text };

struct XmlAttr {
    const char* name;
    const char* value; // unescaped, NUL-terminated
    u32         len;   // the value's bytes
};

struct XmlNode {
    XmlType     type  = XmlType::Element;
    u32         parent = KV_XML_NONE; // node indices; KV_XML_NONE for none
    u32         first  = KV_XML_NONE; // first child, element or text, in document order
    u32         next   = KV_XML_NONE; // next sibling
    const char* name   = "";          // an element's name; "" for text
    // Text: the run of character data, unescaped. An element: all of its own character
    // data (not its children's), concatenated and trimmed of whitespace at both ends.
    const char* text   = "";
    u32         len    = 0;
    u32         attr_first = 0, attr_count = 0; // into XmlDoc::attrs
    u32         line   = 0;           // where it starts
};

struct XmlDoc {
    XmlNode*    nodes  = nullptr;     // node 0 is the root element
    u32         count  = 0;
    XmlAttr*    attrs  = nullptr;
    u32         attr_count = 0;
    const char* error  = nullptr;     // static text; null when the parse succeeded
    u32         error_at = 0;         // byte offset
    u32         error_line = 0, error_col = 0;
    const XmlNode& root() const { return nodes[0]; }
};

// Parses [text, text + len). Nodes, attributes and strings come from `alloc`; the caller
// owns them (and frees nothing when the allocator is an arena). False with doc.error set.
bool xml_parse(const char* text, usize len, Allocator* alloc, XmlDoc& doc);

// Walking, over elements only (text nodes are reachable through `first` and `next`). Null
// when there is none; a null name matches any element.
const XmlNode* xml_child(const XmlDoc& doc, const XmlNode& parent, const char* name = nullptr);
const XmlNode* xml_next_named(const XmlDoc& doc, const XmlNode& sibling, const char* name = nullptr);
u32            xml_count(const XmlDoc& doc, const XmlNode& parent, const char* name = nullptr);
// An attribute's value, or null (xml_attr) or `dflt` (xml_attr_or) when it is absent.
const char*    xml_attr(const XmlDoc& doc, const XmlNode& element, const char* name);
const char*    xml_attr_or(const XmlDoc& doc, const XmlNode& element, const char* name, const char* dflt);
// An element's own character data, trimmed ("" for none); a text node's run.
inline const char* xml_text(const XmlNode& node) { return node.text; }

} // namespace kv
