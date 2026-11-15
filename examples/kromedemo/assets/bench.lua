-- --bench: a thousand rows (two thousand nodes, about thirteen thousand glyphs), one text
-- reading the model so every tick lays out and paints the whole screen again.
local rows = { scroll = true, w = "fill", h = "fill", pad = 4 }
for i = 1, 1000 do
  rows[#rows + 1] = hit { w = "fill", pad = { 8, 1 }, bg = (i % 2 == 0) and "#151922" or "#10131a", radius = 3,
                          text ("asset_" .. i .. ".png") }
end
return {
  model = { tick = 0 },
  root = col { bg = "#0c0e13", color = "#e6e9ef",
    row { pad = 8, bg = "#1b2030", w = "fill", text "bench: tick {tick}" },
    col(rows),
  },
}
