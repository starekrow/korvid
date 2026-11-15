# kromedemo

Krome's first screen (`modules/krome`): a form written in Lua (`assets/form.lua`), a
scrolled list, a menu and a modal, over the UI primitive. It paces on demand, so it
draws only when something happens.

    build/linux-dev/bin/kromedemo                    # the form
    build/linux-dev/bin/kromedemo --demo             # clicks through it, checks the model, quits: "demo passed"
    build/linux-dev/bin/kromedemo --demo --shots DIR # and keeps pictures of the form, the menu and the modal
    build/linux-dev/bin/kromedemo --bench --pacing continuous --prof-report   # 2,000 nodes laid out and painted every tick

The C++ side is small: it opens the screen, sets the tick count in its model now and
then, and quits on the screen's `quit` event.
