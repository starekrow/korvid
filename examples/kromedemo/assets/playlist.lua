-- --two: the playlist window (docs/PRDs/Engine-Slice-13-PRD.md, KR40), a second window beside
-- the form: a search box and two hundred tracks in a scroll, each lit while the pointer is on
-- it. The keyboard is one node across both windows: what is typed goes to the box last
-- pressed, in either.
local rows = { id = "tracks", scroll = true, w = "fill", h = "fill", pad = 4 }
for i = 1, 200 do
  rows[#rows + 1] = hit { id = "track" .. i, w = "fill", pad = { 8, 2 }, radius = 3,
                          bg = (i % 2 == 0) and "#151922" or "#10131a", hover = { bg = "#2f3a52" },
                          text (string.format("%03d  Track %d", i, i)) }
end
return {
  model = { query = "" },
  root = col { bg = "#0c0e13", color = "#e6e9ef", w = "fill", h = "fill",
    row { pad = 8, bg = "#1b2030", w = "fill", text "playlist" },
    row { pad = 4, w = "fill", edit { id = "query", bind = "query", placeholder = "Search", w = "fill" } },
    col(rows),
  },
}
