-- krome/std's behaviours (docs/PRDs/Krome-Slice-4-PRD.md, "The widgets"): what the pointer
-- and the keys do to std's widgets. std.kim names this file in a `script` line; it runs once
-- per Lua state, before any screen that uses std, and returns the behaviours by name. Each
-- gets the instance `w` (id, rect, lo, hi, step, path, value, state; set, emit, part, open,
-- open_slot, close, focus_next, focus) and positions in the node's own logical pixels.

local function clamp(v, lo, hi) if v < lo then return lo elseif v > hi then return hi end return v end

-- A value held to the node's range and stepped to its step (a range of none is 0..100).
local function range(w) if w.hi > w.lo then return w.lo, w.hi end return 0, 100 end
local function settle(w, v)
  local lo, hi = range(w)
  if w.step > 0 then v = lo + math.floor((v - lo) / w.step + 0.5) * w.step end
  return clamp(v, lo, hi)
end

-- ---- slider: a knob over a track --------------------------------------------------------

local knob = 14 -- the knob's width: the value runs over the track less it

local function slider_at(w, x)
  local lo, hi = range(w)
  local k = clamp((x - knob / 2) / math.max(1, w.rect.w - knob), 0, 1)
  return settle(w, lo + (hi - lo) * k)
end

local slider = {
  press = function(w, x) w:set(slider_at(w, x)) end,
  drag = function(w, x) w:set(slider_at(w, x)) end,
  key = function(w, name)
    local lo, hi = range(w)
    local step = w.step > 0 and w.step or (hi - lo) / 100
    local v = tonumber(w.value) or lo
    if name == "left" or name == "down" then w:set(settle(w, v - step))
    elseif name == "right" or name == "up" then w:set(settle(w, v + step))
    elseif name == "pagedown" then w:set(settle(w, v - (hi - lo) / 10))
    elseif name == "pageup" then w:set(settle(w, v + (hi - lo) / 10))
    elseif name == "home" then w:set(lo)
    elseif name == "end" then w:set(hi)
    else return false end
    return true
  end,
  wheel = function(w, dy)
    local lo, hi = range(w)
    local step = w.step > 0 and w.step or (hi - lo) / 100
    w:set(settle(w, (tonumber(w.value) or lo) + (dy > 0 and step or -step)))
    return true
  end,
  update = function(w)
    local lo, hi = range(w)
    local k = clamp(((tonumber(w.value) or lo) - lo) / (hi - lo), 0, 1)
    local pct = string.format("%.3f%%", k * 100)
    w:part("fill", "w", pct)
    w:part("lead", "w", pct)
  end,
}

-- ---- dropdown: its options below it -------------------------------------------------------

local dropdown = {
  press = function(w) w:open_slot("menu") end,
  key = function(w, name)
    if name == "down" then w:focus_next(1) return true end
    if name == "up" then w:focus_next(-1) return true end
    if name == "enter" or name == "space" then w:open_slot("menu"); w:focus_next(1) return true end
    return false
  end,
  pick = function(w, id, value)
    w:set(value)
    w:close()
    w:focus(true)
    return true
  end,
}

-- ---- a menu bar's title: its items below it, an item's id the event it fires ------------

local menu = {
  press = function(w) w:open_slot("menu") end,
  key = function(w, name)
    if name == "down" then w:focus_next(1) return true end
    if name == "up" then w:focus_next(-1) return true end
    if name == "enter" or name == "space" then w:open_slot("menu"); w:focus_next(1) return true end
    return false
  end,
  pick = function(w, id)
    w:close()
    if id ~= "" then w:emit(id) end
    return true
  end,
}

-- ---- a table's column: a press sorts, a drag of its right edge sets its width ------------

local edge = 6

local column = {
  press = function(w, x)
    w.state.resizing = x > w.rect.w - edge
    w.state.grab = x
    w.state.width = w.rect.w
    w.state.moved = false
  end,
  drag = function(w, x)
    if not w.state.resizing then return end
    w.state.moved = true
    w:set(math.max(40, math.floor(w.state.width + x - w.state.grab + 0.5)))
  end,
  release = function(w, x, y)
    if not w.state.resizing and x >= 0 and x < w.rect.w and y >= 0 and y < w.rect.h then w:emit("sort") end
  end,
  key = function(w, name)
    if name == "enter" or name == "space" then w:emit("sort") return true end
    local width = tonumber(w.value) or w.rect.w
    if name == "left" then w:set(math.max(40, width - 10)) return true end
    if name == "right" then w:set(width + 10) return true end
    return false
  end,
}

-- ---- splitter: a drag moves the first pane's edge ------------------------------------------

local splitter = {
  press = function(w, x) w.state.grab = x end,
  drag = function(w, x)
    local v = (tonumber(w.value) or 0) + x - (w.state.grab or 0)
    w:set(math.max(80, math.floor(v + 0.5)))
  end,
  key = function(w, name)
    local v = tonumber(w.value) or 0
    if name == "left" then w:set(math.max(80, v - 16)) return true end
    if name == "right" then w:set(v + 16) return true end
    return false
  end,
}

return { slider = slider, dropdown = dropdown, menu = menu, column = column, splitter = splitter }
