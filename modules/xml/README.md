# xml

XML reading for the rest of the engine: BulletML patterns first (`arcfire`), and
any format that is XML (Tiled's TMX, later). It is yxml (MIT, vendored and pinned in
`src/vendor/`) behind a tree API in the shape of the JSON reader's, and it is present only
in an executable whose modules depend on it.

## How it works

yxml is a streaming, non-validating parser fed one byte at a time: it answers each byte
with a token (an element starts, a byte of an attribute's value, a byte of content, an
element ends) and keeps only a small stack of the open elements' names. `src/xml.cc`
includes `yxml.c` once (its warnings silenced for its own lines) and turns the tokens into
a flat tree, checking on the way what yxml leaves to its caller: that the input is UTF-8
and has no NUL, that no attribute is repeated, the depth. Nothing of yxml is visible
outside that file.

The tree is three arrays in the caller's allocator: nodes (elements and runs of text, by
index, with parent, first child and next sibling), attributes, and one pool of the
strings, unescaped and NUL-terminated. An element also carries its own character data,
joined and trimmed, which is what a format like BulletML wants from `<wait>30</wait>`.
Nothing is written; tools write.

## Using it

```cpp
#include "korvid/xml/xml.h"

MemScope scope;
XmlDoc doc;
if (!xml_parse(text, len, mem_scratch(), doc))
    KV_ERROR("patterns", "%s: %s at line %u, column %u", path, doc.error, doc.error_line, doc.error_col);
const XmlNode& root = doc.root();                       // <bulletml type="vertical">
const char* type = xml_attr_or(doc, root, "type", "none");
for (const XmlNode* a = xml_child(doc, root, "action"); a; a = xml_next_named(doc, *a, "action"))
    if (const XmlNode* w = xml_child(doc, *a, "wait")) use(xml_attr(doc, *a, "label"), xml_text(*w));
```

`xml_child` and `xml_next_named` walk elements (a null name is any element); `xml_count`
counts them; text runs, for mixed content, are the nodes of type `Text` reached through
`first` and `next`. Every node has the line it starts on.

## What it reads, and what it does not

Each of these is pinned by a test (`tests/xml_test.cc`):

- **UTF-8 only.** A byte-order mark is skipped. Bytes that are not UTF-8, and NUL, are an
  error at the byte. The XML declaration's `encoding` is not read (a UTF-16 document fails
  at its first NUL).
- **One root element.** A second root is an error, as is a document that ends inside an
  element, a comment or an instruction.
- **Entities:** `&lt; &gt; &amp; &apos; &quot;` and numeric character references, expanded
  to UTF-8. Any other entity is an error, never an expansion. Two quirks of yxml's range
  check, both refusals: surrogate references (`&#xD800;`–`&#xDFFF;`), which yxml would let
  through as invalid UTF-8, are refused here; references to U+E000–U+E7FE are refused by
  yxml itself (the characters, written as UTF-8, are fine).
- **The DOCTYPE** and its internal subset are skipped and never applied: no defaults, no
  entities, no validation.
- **Skipped:** comments, processing instructions, the XML declaration. **Kept:** CDATA, as
  character data.
- **Namespaces are not processed:** `ns:a` is a name like `a`.
- **Attributes** are unescaped, not normalized; a repeated attribute is an error.
- **Limits:** 64 nested elements (`KV_XML_MAX_DEPTH`); the open elements' names together
  within yxml's 4 KiB name stack. No limit on size but the input's.
- **Errors** name a byte offset, a line and a column (bytes from 1); a failed parse leaves
  nothing allocated.

`tests/fuzz_test.cc` parses a few thousand mutated documents a run (bytes changed,
inserted, deleted, duplicated, cut; a fixed seed) from exact-size heap copies: each ends
in a tree or an error placed inside the input, and a failure leaves no allocation. It has
been run with AddressSanitizer and UBSan at 200,000 cases.

## Quick reference

Header `korvid/xml/xml.h`:

| name | kind | what |
|---|---|---|
| `xml_parse(text, len, alloc, doc)` | function | the tree, or false with `doc.error`, `error_at`, `error_line`, `error_col` |
| `XmlDoc` | record | `nodes` (0 is the root), `count`, `attrs`, `attr_count`, the error, `root()` |
| `XmlNode` | record | `type` (`Element`, `Text`), `parent`, `first`, `next` (`KV_XML_NONE` for none), `name`, `text`, `len`, `attr_first`, `attr_count`, `line` |
| `XmlAttr` | record | `name`, `value`, `len` |
| `xml_child`, `xml_next_named`, `xml_count` | functions | elements by name, or any |
| `xml_attr`, `xml_attr_or` | functions | an attribute's value, or null / a default |
| `xml_text(node)` | function | an element's own character data, trimmed; a run's text |

## Not in this module

Writing XML, validation against a DTD or a schema, entities beyond the five, encodings
other than UTF-8, namespace resolution, XPath or any query beyond the walk, and streaming
the tree to a caller (the tree is built whole; a document the size of a level is
milliseconds).
