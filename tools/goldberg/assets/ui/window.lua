-- goldberg's window: what its look starts with, and the one thing the screen does by itself (the
-- context menu). Everything else is goldberg's (tools/goldberg/lua/window.lua), which sends the
-- models and takes the presses.
return {
  model = {
    page = "status", report = "failed", split = 360,
    game = "", state = "", summary = "", steps_count = "", programs_none = "",
    files_filter = "", files_count = "", report_filter = "", shown_count = "",
    failed_count = 0, warned_count = 0, menu_key = "",
    steps = {}, programs = {}, files = {}, shown = {},
    sel = { key = "", state = "", none = "Pick a file on the left.", lands = "", rows = {}, up = {}, down = {} },
    sw = { key = 360, conv = 110, state = 110, out = 320 },
    rw = { file = 360, what = 600 },
    dw = { key = 300, field = 90, old = 300, new = 300 },
    bergs = {}, bergs_none = "", form = { key = "", broken = "", entries = {} }, add_scope_label = "meta",
    add_key = "", add_value = "", form_state = "", diff = {}, diff_summary = "", diff_error = "",
  },
  on = {
    report_menu = function(id, x, y)
      model.menu_key = id
      open_overlay(kim("goldberg/ui/window.kimb", "report_menu"), { side = "pointer" })
    end,
    menu_inspect = function() close_overlay() end,
    menu_touch = function() close_overlay() end,
    menu_copy = function() close_overlay() end,
  },
}
