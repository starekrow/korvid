-- The demo's form as slice 1 wrote it: a screen script that returns its model, its
-- handlers and its tree, all Lua tables. `kromedemo --lua` opens it; form.kim is the same
-- screen in markup, and draws the same pixels (`picture compare` on --shots).

local muted, accent = "#8b95a7", "#2f6fed"

local function button(label, t)
  local b = { pad = { 14, 8 }, radius = 6, bg = accent, color = "#e6e9ef", border = 1, ["border-color"] = "#ffffff22",
              looks = { hover = { bg = "#4a84f5" }, pressed = { bg = "#245ad0" },
                        disabled = { bg = "#2a3140", color = "#5b6475", ["border-color"] = "#ffffff0d" } },
              center = true, middle = true,
              text { label } }
  for k, v in pairs(t or {}) do b[k] = v end
  return hit(b)
end

local function field(label, value)
  return row { gap = 8, middle = true,
    text { label, w = 90, color = muted },
    text { value, w = "fill" },
  }
end

local function menu_item(label, event)
  return hit { id = event, event = event, w = "fill", pad = { 12, 6 }, radius = 4,
               hover = { bg = "#2f3a52" }, text { label } }
end

local items = { id = "list", scroll = true, w = "fill", h = 220, bg = "#151922", radius = 6, pad = 4 }
for i = 1, 40 do
  items[#items + 1] = hit { id = "item" .. i, event = "item", w = "fill", pad = { 10, 5 }, radius = 4,
                            hover = { bg = "#232a38" }, text ("asset_" .. i .. ".png") }
end

return {
  model = { name = "Rowan", level = 3, saves = 0, picked = "nothing", ticks = 0, choice = "none", abouts = 0 },

  on = {
    up     = function() model.level = model.level + 1 end,
    down   = function() if model.level > 1 then model.level = model.level - 1 end end,
    save   = function() model.saves = model.saves + 1; emit("saved", model.saves) end,
    item   = function(id) model.picked = id end,
    menu   = function()
      open_overlay(col { bg = "#1f2533", radius = 8, pad = 4, w = 160,
                         shadow = { 0, 6, 16 }, ["shadow-color"] = "#00000099",
                         border = 1, ["border-color"] = "#ffffff1a",
                         menu_item("Compact", "compact"), menu_item("Roomy", "roomy"), menu_item("Close", "close_menu") },
                   { anchor = "menu" })
    end,
    compact    = function() model.choice = "compact"; close_overlay() end,
    roomy      = function() model.choice = "roomy"; close_overlay() end,
    close_menu = function() close_overlay() end,
    about  = function()
      open_overlay(col { bg = "#1f2533", radius = 10, pad = 20, gap = 14, w = 320,
                         shadow = { 0, 10, 30 }, ["shadow-color"] = "#000000aa",
                         text { "About", size = 18 },
                         text { "Krome: one screen, two ways to write it.", color = muted },
                         row { button("OK", { id = "ok", event = "ok", w = 80 }), right = true, w = "fill" } },
                   { modal = true, dim = "#00000088" })
    end,
    ok     = function() model.abouts = model.abouts + 1; close_overlay() end,
    quit   = function() emit("quit") end,
  },

  root = col { bg = "#10131a", color = "#e6e9ef",
    row { pad = { 16, 10 }, bg = "#1b2030", middle = true, w = "fill",
      text { "Krome demo", size = 18 },
      spacer {},
      text { "{ticks} ticks", color = muted },
    },
    row { pad = 16, gap = 16, w = "fill", h = "fill",
      col { gap = 12, w = "260..360", h = "fill",
        field("Name", "{name}"),
        field("Level", "{level}"),
        row { gap = 8,
          button("-", { id = "down", event = "down", w = 40 }),
          button("+", { id = "up", event = "up", w = 40 }),
        },
        field("Saved", "{saves} times"),
        field("Picked", "{picked}"),
        field("Menu", "{choice}"),
        spacer {},
        row { gap = 8,
          button("Save", { id = "save", event = "save" }),
          button("Menu", { id = "menu", event = "menu" }),
          button("Later", { id = "later", event = "save", disabled = true }),
        },
      },
      col { gap = 12, w = "fill", h = "fill",
        row { middle = true, gap = 8, w = "fill",
          text { "Assets", size = 16 }, spacer {},
          button("About", { id = "about", event = "about" }),
          button("Quit", { id = "quit", event = "quit", bg = "#3a2430",
                           looks = { hover = { bg = "#4d2f3d" }, pressed = { bg = "#2c1b25" } } }),
        },
        col(items),
        text { "Scroll the list; pick an asset.", color = muted },
        box { w = "fill", h = 60, radius = 8, bg = "#2f6fed", bg2 = "#8a3ffc", angle = 0,
              shadow = { 0, 4, 12 }, ["shadow-color"] = "#2f6fed66" },
      },
    },
  },
}
