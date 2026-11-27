-- The hand-written skin's handlers (player.kim names this file): a pretend player, its
-- state, time and title in the model.
local titles = { "KORVID SKIN DEMO", "PICTURES 7", "REGIONS FRAMES AND FONTS" }
local at = 1
return {
  model = { state = 0, mm = "01", ss = "23", title = titles[1], volume = 0.6, pos = 0.3, shuffle = false, loop = true },
  on = {
    play = function() model.state = 0 end,
    pause = function() model.state = 1 end,
    stop = function() model.state = 2; model.mm = "00"; model.ss = "00"; model.pos = 0 end,
    next = function() at = at % #titles + 1; model.title = titles[at] end,
    prev = function() at = (at - 2) % #titles + 1; model.title = titles[at] end,
    eject = function() end,
    close = function() end,
  },
}
