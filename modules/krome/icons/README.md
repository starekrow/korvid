# krome/std's icons

A subset of [Material Symbols](https://github.com/google/material-design-icons) (Google,
Apache 2.0, `LICENSE`): the Outlined style, weight 400, unfilled, at the 20 px optical size.
The SVG files are in `../assets/icons/`, where screens and games name them
(`image="icons/check.svg"`); this directory keeps what made them.

- `NAMES`: the icons, by Material's names.
- `COMMIT`: the commit of `google/material-design-icons` they come from.
- `vendor.sh`: fetches them (curl) and puts `fill="currentColor"` on each root element, since
  Material's files have no fill and SVG paints that black. With `--latest` it moves `COMMIT`
  to the repository's current commit first.

To add an icon: its name to `NAMES`, then `sh vendor.sh <name>`, then
`tools/patherizer-refs.sh` for patherizer's reference pictures of it.
