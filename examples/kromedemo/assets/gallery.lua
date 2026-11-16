-- The widget gallery's handlers (gallery.kim names this file): a small model for each of
-- krome/std's widgets, and what their events do.

-- A tree, flattened into rows by what is open.
local tree = {
  { id = "assets", name = "assets", kids = {
    { id = "textures", name = "textures", kids = {
      { id = "hero", name = "hero.png" }, { id = "tiles", name = "tiles.png" }, { id = "sky", name = "sky.png" } } },
    { id = "sounds", name = "sounds", kids = {
      { id = "jump", name = "jump.wav" }, { id = "hit", name = "hit.wav" } } },
    { id = "fonts", name = "fonts", kids = { { id = "dejavu", name = "DejaVuSans.ttf" } } },
  } },
  { id = "berg", name = "assets.berg" },
}
local open = { assets = true, textures = true }

local function flatten()
  local out = {}
  local function walk(list, depth)
    for _, n in ipairs(list) do
      local arrow = n.kids and (open[n.id] and "\u{25BE}" or "\u{25B8}") or ""
      out[#out + 1] = { id = n.id, name = n.name, indent = depth * 16, arrow = arrow }
      if n.kids and open[n.id] then walk(n.kids, depth + 1) end
    end
  end
  walk(tree, 0)
  return out
end

-- Ten thousand rows for the table: it builds only what is in view.
local kinds = { "texture", "sound", "font", "shader", "script" }
local rows = {}
for i = 1, 10000 do
  rows[i] = { id = "r" .. i, name = string.format("asset_%05d", i), size = (i * 7919) % 4096, kind = kinds[i % #kinds + 1] }
end
local order = { name = true }

return {
  model = { level = 40, done = "40%", mode = "fast", wrap = true, kind = "texture", name = "", secret = "",
            tab = "tree", nodes = flatten(), rows = rows, rows_count = #rows, picked_row = "none picked",
            widths = { name = 200, size = 90, kind = 110 }, split = 240, rename = "", renamed = "not renamed",
            last = "" },

  on = {
    nav = function() end, -- the game opens the page
    more = function()
      local v = tonumber((model.done:gsub("%%", ""))) + 10
      if v > 100 then v = 0 end
      model.done = v .. "%"
    end,
    toggle = function(id)
      open[id] = not open[id]
      model.nodes = flatten()
    end,
    sort = function(id)
      local key = (id or ""):gsub("^col_", "")
      if key ~= "name" and key ~= "size" and key ~= "kind" then return end
      local up = not order[key]
      order = { [key] = up }
      table.sort(rows, function(a, b) if up then return a[key] < b[key] end return a[key] > b[key] end)
      model.rows = rows
    end,
    pick_row = function(id) model.picked_row = id end,
    row_menu = function(id) model.picked_row = id; open_overlay(kim("kromedemo/gallery.kimb", "row_menu"), { side = "pointer" }) end,
    inspect = function() model.last = "inspect " .. model.picked_row; close_overlay() end,
    copy_path = function() model.last = "copied " .. model.picked_row; close_overlay() end,
    open_dialog = function() open_overlay(kim("kromedemo/gallery.kimb", "dialog"), { modal = true, dim = "#00000088" }) end,
    ok = function() model.renamed = "renamed to " .. model.rename; close_overlay() end,
    cancel = function() close_overlay() end,
    open = function() model.last = "file: open" end,
    save_all = function() model.last = "file: save" end,
    close_all = function() model.last = "file: close" end,
    dark = function() theme("dark") end,
    light = function() theme("light") end,
  },
}
