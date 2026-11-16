-- goldberg's five screens' models and handlers (screens.kim names this file). The models are
-- samples in the shapes goldberg has: an asset graph, a lockfile's re-routes, a cook's steps
-- (the game, main.cc, plays the cook into `steps` and the counts), warnings, a .berg's rules.

-- ---- inspection ---------------------------------------------------------------------------

local assets = {
  hero = { name = "hero.png", type = "texture", path = "textures/hero.ktx2", converter = "texconv",
           params = { { name = "format", value = "bc7", rule = "assets.berg:12" },
                      { name = "mips", value = "true", rule = "assets.berg:12" },
                      { name = "srgb", value = "true", rule = "conventions.berg:31" } } },
  hero_src = { name = "hero.aseprite", type = "source", path = "art/hero.aseprite", converter = "aseprite",
               params = { { name = "layers", value = "visible", rule = "art.berg:4" } } },
  atlas = { name = "sprites.atlas", type = "atlas", path = "textures/sprites.atlas", converter = "packer",
            params = { { name = "pad", value = "2", rule = "assets.berg:20" }, { name = "max", value = "4096", rule = "assets.berg:21" } } },
  pack = { name = "main.pack", type = "pack", path = "dist/main.pack", converter = "kpack",
           params = { { name = "deflate", value = "true", rule = "packs.berg:3" } } },
}
local graph_tree = {
  { id = "hero", edge = "", kids = {
    { id = "up", label = "upstream", kids = { { id = "hero_src", edge = "<- converts" } } },
    { id = "down", label = "downstream", kids = { { id = "atlas", edge = "-> packs into" }, { id = "pack", edge = "-> ships in" } } },
  } },
}
local open = { hero = true, up = true, down = true }

local function flatten()
  local out = {}
  local function walk(list, depth)
    for _, n in ipairs(list) do
      local a = assets[n.id]
      out[#out + 1] = { id = n.id, name = a and a.name or n.label, path = a and a.path or "", edge = n.edge or "",
                        indent = depth * 16, arrow = n.kids and (open[n.id] and "krome/icons/arrow_drop_down.svg" or "krome/icons/arrow_right.svg") or "" }
      if n.kids and open[n.id] then walk(n.kids, depth + 1) end
    end
  end
  walk(graph_tree, 0)
  return out
end

local function selection(id)
  local a = assets[id] or assets.hero
  local params = {}
  for i, p in ipairs(a.params) do params[i] = { n = i, name = p.name, value = p.value, rule = p.rule } end
  return { name = a.name, type = a.type, path = a.path, converter = a.converter, params = params }
end

local berg_files = {
  ["assets.berg"] = { "-- the game's own rules", "{ match = \"textures/*.png\",", "  type = \"texture\",",
                      "  meta = { format = \"bc7\", mips = true } },", "{ match = \"sprites/*\", type = \"atlas\", pad = 2 }" },
  ["conventions.berg"] = { "-- goldberg's conventions", "{ match = \"*.png\", meta = { srgb = true } }" },
  ["art.berg"] = { "{ match = \"*.aseprite\", converter = \"aseprite\", layers = \"visible\" }" },
  ["packs.berg"] = { "{ pack = \"main\", deflate = true }" },
}

-- ---- diff review --------------------------------------------------------------------------

local reroutes = {}
for i = 1, 24 do
  reroutes[i] = { n = i, asset = string.format("textures/tile_%02d.png", i), old = "texconv bc7 -> tiles.pack",
                  new = (i % 3 == 0) and "texconv bc1 -> tiles.pack" or "texconv bc7 -> world.pack", open = true, verdict = "" }
end

-- ---- reports -------------------------------------------------------------------------------

local reports = { fallback = {}, outside = {}, budgets = {} }
for i = 1, 300 do
  reports.fallback[i] = { id = "f" .. i, asset = string.format("misc/file_%03d.dat", i), detail = "matched *, the fallback: a blob", size = (i * 37) % 900 + 12 }
end
for i = 1, 120 do
  reports.outside[i] = { id = "o" .. i, asset = string.format("levels/level_%02d.map", i % 40), detail = "loaded from loose files, not a pack", size = (i * 53) % 4000 + 100 }
end
for i = 1, 40 do
  reports.budgets[i] = { id = "b" .. i, asset = string.format("textures/big_%02d.png", i), detail = "over the 4 MB texture budget", size = 4096 + (i * 211) % 8000 }
end
local sort_key, sort_up = "asset", true

local function show()
  local list, f = reports[model.report], (model.filter or ""):lower()
  local out = {}
  for _, w in ipairs(list) do
    if f == "" or w.asset:lower():find(f, 1, true) or w.detail:lower():find(f, 1, true) then out[#out + 1] = w end
  end
  table.sort(out, function(a, b) if sort_up then return a[sort_key] < b[sort_key] end return a[sort_key] > b[sort_key] end)
  model.shown = out
  model.shown_count = #out
  model.report_count = #list
end

-- ---- forms ---------------------------------------------------------------------------------

local rules = {
  { id = "rule1", n = 1, match = "textures/*.png", type = "texture", quality = 80, mips = true, runtime = true, meta = "format = bc7" },
  { id = "rule2", n = 2, match = "sounds/*.wav", type = "sound", quality = 60, mips = false, runtime = true, meta = "" },
  { id = "rule3", n = 3, match = "fonts/*.ttf", type = "font", quality = 100, mips = false, runtime = true, meta = "sizes = 12, 14, 18" },
  { id = "rule4", n = 4, match = "scripts/*.lua", type = "script", quality = 100, mips = false, runtime = true, meta = "" },
  { id = "rule5", n = 5, match = "*", type = "blob", quality = 50, mips = false, runtime = false, meta = "the fallback" },
}
local function copy(t) local c = {}; for k, v in pairs(t) do c[k] = v end; return c end
local editing = 1

return {
  model = {
    page = "inspect", note = "", split = 280,
    graph = flatten(), sel = selection("hero"), pw = { param = 120, value = 120, rule = 180 },
    berg_shown = "", berg_lines = {},
    reroutes = reroutes, reroutes_left = #reroutes, accepted = 0, rejected = 0,
    dw = { asset = 220, old = 230, new = 230, act = 170 },
    confirm_title = "", confirm_text = "", confirming = "",
    done = 0, total = 0, running = 0, failed = 0, elapsed = "", overall = "0%", steps = {},
    sw = { step = 300, conv = 120, state = 110, prog = 180 },
    targets = { { name = "linux-dev", state = "watching" }, { name = "android emulator", state = "connected" }, { name = "devbox", state = "idle" } },
    report = "fallback", filter = "", shown = reports.fallback, shown_count = #reports.fallback, report_count = #reports.fallback,
    picked_warning = "none picked",
    rw = { asset = 280, detail = 320, size = 80 },
    berg = "assets.berg", rules = rules, form = copy(rules[1]), form_state = "", saves = 0,
  },

  on = {
    -- inspection
    inspect_node = function(id)
      if assets[id] then model.sel = selection(id) end
      local has_kids = false
      for _, n in ipairs(model.graph) do if n.id == id and n.arrow ~= "" then has_kids = true end end
      if has_kids then open[id] = not open[id]; model.graph = flatten() end
    end,
    open_berg = function(id)
      local n = tonumber(id:match("rule_(%d+)"))
      local rule = model.sel.params[n] and model.sel.params[n].rule or ""
      local file = rule:match("^([^:]+)")
      local lines = {}
      for i, l in ipairs(berg_files[file] or { "(no such file)" }) do lines[i] = { text = string.format("%3d  %s", i, l) } end
      model.berg_shown = rule
      model.berg_lines = lines
      model.confirming = "berg"
      open_overlay(kim("kromescreens/screens.kimb", "berg"), { modal = true, dim = "#00000088" })
    end,
    -- diff review
    accept = function(id)
      local n = tonumber(id:match("_(%d+)$"))
      local r = reroutes[n]
      if r and r.open then r.open = false; r.verdict = "accepted"; model.accepted = model.accepted + 1; model.reroutes_left = model.reroutes_left - 1 end
      model.reroutes = reroutes
    end,
    reject = function(id)
      local n = tonumber(id:match("_(%d+)$"))
      local r = reroutes[n]
      if r and r.open then r.open = false; r.verdict = "rejected"; model.rejected = model.rejected + 1; model.reroutes_left = model.reroutes_left - 1 end
      model.reroutes = reroutes
    end,
    ask_accept_all = function()
      model.confirming = "accept"
      model.confirm_title = "Accept every re-route?"
      model.confirm_text = model.reroutes_left .. " re-routes will be written to the lockfile."
      open_overlay(kim("kromescreens/screens.kimb", "confirm"), { modal = true, dim = "#00000088" })
    end,
    ask_reject_all = function()
      model.confirming = "reject"
      model.confirm_title = "Reject every re-route?"
      model.confirm_text = model.reroutes_left .. " assets keep their old routes."
      open_overlay(kim("kromescreens/screens.kimb", "confirm"), { modal = true, dim = "#00000088" })
    end,
    ok = function()
      local verdict = model.confirming == "accept" and "accepted" or model.confirming == "reject" and "rejected" or nil
      if verdict then
        for _, r in ipairs(reroutes) do
          if r.open then
            r.open = false; r.verdict = verdict
            if verdict == "accepted" then model.accepted = model.accepted + 1 else model.rejected = model.rejected + 1 end
          end
        end
        model.reroutes_left = 0
        model.reroutes = reroutes
      end
      model.confirming = ""
      close_overlay()
    end,
    cancel = function() model.confirming = ""; close_overlay() end,
    -- reports
    filtered = function() show() end,
    show_report = function() show() end,
    pick_warning = function(id) model.picked_warning = id end,
    warning_menu = function(id) model.picked_warning = id; open_overlay(kim("kromescreens/screens.kimb", "warning_menu"), { side = "pointer" }) end,
    inspect_warning = function() model.note = "inspect " .. model.picked_warning; close_overlay() end,
    copy_warning = function() model.note = "copied " .. model.picked_warning; close_overlay() end,
    sort = function(id)
      local key = ({ col_rasset = "asset", col_rdetail = "detail", col_rsize = "size" })[id]
      if not key then return end
      if key == sort_key then sort_up = not sort_up else sort_key, sort_up = key, true end
      show()
    end,
    -- forms
    pick_rule = function(id)
      for i, r in ipairs(rules) do if r.id == id then editing = i end end
      model.form = copy(rules[editing])
      model.form_state = ""
    end,
    save = function()
      rules[editing] = copy(model.form)
      model.rules = rules
      model.saves = model.saves + 1
      model.form_state = "saved"
    end,
    revert = function()
      model.form = copy(rules[editing])
      model.form_state = "reverted"
    end,
  },
}
