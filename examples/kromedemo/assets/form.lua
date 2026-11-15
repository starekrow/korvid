-- The form's handlers (form.kim names this file in a `script` line). It returns what the
-- screen starts with and what its events do; the tree is the markup's. The menu and the
-- About box are screens of form.kim, opened as overlays.

local assets = {}
for i = 1, 40 do assets[i] = { id = "item" .. i, name = "asset_" .. i .. ".png" } end

return {
  model = { name = "Rowan", level = 3, saves = 0, picked = "nothing", ticks = 0, choice = "none", abouts = 0,
            assets = assets, notes = "", noted = 0 },

  on = {
    up         = function() model.level = model.level + 1 end,
    down       = function() if model.level > 1 then model.level = model.level - 1 end end,
    save       = function() model.saves = model.saves + 1; emit("saved", model.saves) end,
    item       = function(id) model.picked = id end,
    menu       = function() open_overlay(kim("kromedemo/form.kimb", "menu"), { anchor = "menu" }) end,
    compact    = function() model.choice = "compact"; close_overlay() end,
    roomy      = function() model.choice = "roomy"; close_overlay() end,
    close_menu = function() close_overlay() end,
    about      = function() open_overlay(kim("kromedemo/form.kimb", "about"), { modal = true, dim = "#00000088" }) end,
    ok         = function() model.abouts = model.abouts + 1; close_overlay() end,
    quit       = function() emit("quit") end,
    noted      = function() model.noted = model.noted + 1 end,
  },
}
