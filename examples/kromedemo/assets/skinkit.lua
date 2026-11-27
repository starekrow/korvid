-- kromedemo/skinkit's behaviours (Krome slice 8, KR36): written in Lua on what the kernel
-- gives every behaviour, to find what a skin's widgets cannot be written with.
--
-- The path slider: its thumb follows a quadratic Bezier through three points. A behaviour
-- has no properties of its own to be told the points by, so they are here, as fractions of
-- the node's size: from its bottom left, bowed up through its top, to its bottom right. Its
-- update runs before the node is laid out (at build) as well as after, so the thumb and the
-- dots are placed by fractions too (l=..% and t=..%, KR30's edges), not by pixels: they are
-- right whatever size the node turns out to be.

local function clamp(v, lo, hi) if v < lo then return lo elseif v > hi then return hi end return v end
local function range(w) if w.hi > w.lo then return w.lo, w.hi end return 0, 100 end
local function settle(w, v)
  local lo, hi = range(w)
  if w.step > 0 then v = lo + math.floor((v - lo) / w.step + 0.5) * w.step end
  return clamp(v, lo, hi)
end

local p0, p1, p2 = { 0.06, 0.85 }, { 0.5, -0.55 }, { 0.94, 0.85 }
local thumb, dot = 12, 4 -- the parts' sizes, logical pixels: their styles' (a behaviour cannot read them)

-- The curve at t, as fractions of the node.
local function at(t)
  local a, b, c = (1 - t) * (1 - t), 2 * (1 - t) * t, t * t
  return a * p0[1] + b * p1[1] + c * p2[1], a * p0[2] + b * p1[2] + c * p2[2]
end

-- A part's centre at fraction (fx, fy) of the node: its edges by fraction, less half its size.
local function place(w, part, fx, fy, size)
  w:part(part, "l", string.format("%.4f%%-%g", fx * 100, size / 2))
  w:part(part, "t", string.format("%.4f%%-%g", fy * 100, size / 2))
end

-- The t whose point is nearest (x, y), in the node's logical pixels: 64 samples, then a
-- finer look either side of the best.
local function nearest(w, x, y)
  local W, H = math.max(1, w.rect.w), math.max(1, w.rect.h)
  local function d2(t) local fx, fy = at(t); return (fx * W - x) ^ 2 + (fy * H - y) ^ 2 end
  local best, bd = 0, d2(0)
  for i = 1, 64 do local t = i / 64; local d = d2(t); if d < bd then best, bd = t, d end end
  local lo, hi = math.max(0, best - 1 / 64), math.min(1, best + 1 / 64)
  for i = 0, 32 do local t = lo + (hi - lo) * i / 32; local d = d2(t); if d < bd then best, bd = t, d end end
  return best
end

local function set_at(w, x, y)
  local lo, hi = range(w)
  w:set(settle(w, lo + (hi - lo) * nearest(w, x, y)))
end

local pathslider = {
  press = function(w, x, y) set_at(w, x, y) end,
  drag = function(w, x, y) set_at(w, x, y) end,
  -- std's slider's keys and wheel.
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
    if not w.state.dots then
      for i = 0, 8 do local fx, fy = at(i / 8); place(w, "dot" .. i, fx, fy, dot) end
      w.state.dots = true
    end
    local lo, hi = range(w)
    local fx, fy = at(clamp(((tonumber(w.value) or lo) - lo) / (hi - lo), 0, 1))
    place(w, "thumb", fx, fy, thumb)
  end,
}

return { pathslider = pathslider }
