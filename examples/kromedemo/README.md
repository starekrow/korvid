# kromedemo

Krome's demo screen (`modules/krome`): a form, a scrolled list, a menu and a modal, over
the UI primitive. It paces on demand, so it draws only when something happens.

The form is markup: `assets/form.kim`, out of `krome/std`'s widgets and its dark theme,
with its handlers in `assets/form.lua` (the `script` line names it); the menu and the
About box are screens of the same file, opened as overlays by those handlers. The same
form as slice 1 wrote it, in Lua tables, is `assets/form_lua.lua`; `--lua` opens it, and the
two draw the same pixels.

    build/linux-dev/bin/kromedemo                    # the form
    build/linux-dev/bin/kromedemo --theme light      # in std's light theme
    build/linux-dev/bin/kromedemo --lua              # the slice-1 form, in Lua
    build/linux-dev/bin/kromedemo --demo             # clicks through it, checks the model, quits: "demo passed"
    build/linux-dev/bin/kromedemo --demo --shots DIR # and keeps pictures of the form, the menu and the modal,
                                                     # then the form and the menu in the light theme
    build/linux-dev/bin/kromedemo --bench --pacing continuous --prof-report   # 2,000 nodes laid out and painted every tick

The pictures are taken once Krome has settled (`krome_settled`: fonts in, changes painted),
with the tick count held at 0, so two runs compare:

    kromedemo --demo --lua --shots /tmp/lua && kromedemo --demo --shots /tmp/kim
    picture compare /tmp/lua /tmp/kim     # form, menu and about: largest channel difference 0

(the light pictures differ, since the Lua form has no themes).

The C++ side is small: it opens the screen, sets the tick count in its model now and
then, switches the theme in the demo, and quits on the screen's `quit` event.
