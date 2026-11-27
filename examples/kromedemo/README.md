# kromedemo

Krome's demo screen (`modules/krome`): a form, a scrolled list, a menu and a modal, over
the UI primitive. It paces on demand, so it draws only when something happens.

The form is markup: `assets/form.kim`, out of `krome/std`'s widgets and its dark theme,
with its handlers in `assets/form.lua` (the `script` line names it); the menu and the
About box are screens of the same file, opened as overlays by those handlers. The same
form as slice 1 wrote it, in Lua tables, is `assets/form_lua.lua`; `--lua` opens it, and the
two draw the same pixels.

The second page is the widget gallery (`assets/gallery.kim`, its model and handlers in
`assets/gallery.lua`): every widget `krome/std` has (slider, progress, radio, checkbox,
dropdown, input and password, buttons with a shortcut and a tooltip, a menu bar, tabs over a
tree, a 10,000-row table with sortable, resizable columns and a context menu, a splitter
between scroll views, a modal dialog), and a skin's buttons cut from one sheet
(`assets/skin.png`), their hover and pressed looks regions beside them, drawn `pixel`. The header's navigation (in both forms, and the
gallery) fires `nav`; the game opens the page it names in place of the one shown. The Skin
tab also has std's dial on a 32-frame strip (`assets/dial.png`) and a slider along a curve,
`pathslider`, from a prelude of the demo's own (`assets/skinkit.kim` and `skinkit.lua`): a
behaviour written in Lua on what every behaviour has (Krome slice 8, KR36).

    build/linux-dev/bin/kromedemo                    # the form
    build/linux-dev/bin/kromedemo --theme light      # in std's light theme
    build/linux-dev/bin/kromedemo --lua              # the slice-1 form, in Lua
    build/linux-dev/bin/kromedemo --demo             # clicks through it, checks the model, quits: "demo passed"
    build/linux-dev/bin/kromedemo --demo --shots DIR # and keeps pictures of the form, the menu and the modal,
                                                     # the form and the menu in the light theme, then the
                                                     # gallery: its dropdown, table, split, dialog, light
    build/linux-dev/bin/kromedemo --two              # and a playlist in a second window (F2 hides and shows it)
    build/linux-dev/bin/kromedemo --two-check        # each window presented only when it changes, a hide, one keyboard: "two: passed"
    build/linux-dev/bin/kromedemo --bench --pacing continuous --prof-report   # 2,000 nodes laid out and painted every tick
    build/linux-dev/bin/kromedemo --player           # a skin written by hand (assets/player.kim): a classic player from its sheets
    build/linux-dev/bin/kromedemo --player --demo    # presses through it and checks the model: "demo passed"

The pictures are taken once Krome has settled (`krome_settled`: fonts in, changes painted),
with the tick count held at 0, so two runs compare:

    kromedemo --demo --lua --shots /tmp/lua && kromedemo --demo --shots /tmp/kim
    picture compare /tmp/lua /tmp/kim     # form, menu and about: largest channel difference 0

(the light pictures differ, since the Lua form has no themes; the gallery's are the same
screen in both runs, and match).

The C++ side is small: it opens the screen, sets the tick count in its model now and
then, switches the theme in the demo, and quits on the screen's `quit` event.
