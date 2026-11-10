# Vendored

`yxml.c`, `yxml.h`, `COPYING`: yxml by Yoran Heling (MIT), from the author's repository
https://code.blicky.net/yorhel/yxml at commit 66507906673bc6159d5d620414479954c9c21c24
(2020-11-08, "Use reinterpret-casting in yxml_setchar()"; the project has no tags),
downloaded 2026-11-09 from its `raw/branch/master/` paths, unmodified:

| file | bytes | sha256 |
|---|---|---|
| `yxml.c` | 25,000 | 7699a9c0e9d9b987b74ff86bf3fc28be6deed062d22d9a0f8376b868b15864e5 |
| `yxml.h` | 6,185 | 6f79158a3811bf1fab575ef512b91ffc87a795f070b562f7fb6bcbf71fdd9078 |
| `COPYING` | 1,061 | b9b5ca63904b3cf589d5719be2968436550914dc3fdf5d81b37ddbdd67986d06 |

`yxml.c` is generated upstream from `yxml.c.in` and `yxml-states`; the generated file is
what is vendored. `src/xml.cc` includes it once (its warnings silenced for its own lines),
and `module.lua`'s `private_includes` puts this directory on the module's own include path
because `yxml.c` includes `<yxml.h>`. Nothing of yxml is visible outside `xml.cc`.
Upgrading is replacing the three files and the table above, then running the module's
tests, the fuzz test among them.

**A known quirk, handled in `xml.cc`.** yxml's check of a character reference's code point
(`(ch-0xDFFF) < 0x7FF`, in `yxml_refend`) is off by the surrogate range: it lets
`&#xD800;`–`&#xDFFF;` through, encoded as invalid UTF-8, and refuses U+E000–U+E7FE.
`xml.cc` refuses any reference whose expansion is not valid UTF-8; the second refusal
stands and is stated in the module's README. Both are pinned by tests.
