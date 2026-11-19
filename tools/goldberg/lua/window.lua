-- goldberg's window (docs/PRDs/Krome-Slice-6-PRD.md, "goldberg's window"): the running session
-- as the window's models, and the window's presses as the session's commands. The screen is
-- goldberg/ui/window.kim (assets/ui/), drawn by Krome; this file is all it knows of goldberg.
--
-- Models go out only when they changed (each kept as the JSON last sent), and the big ones at
-- most every quarter second while the session works: a cook of thousands of steps is not sent
-- whole every tick. What the window says comes back as goldberg.ui_event(event, id, value);
-- what a person typed is read back from Krome's model (host.ui_get).
local explain = require("explain")

local M = {}

local sent = {}         -- model key -> the text last sent
local next_big = 0      -- when the step table, the reports and the file list may go again
local inspected = nil   -- the file key the Inspect page shows
local sort = { report = "file", up = true }

-- A model as JSON: a table with a [1] (or empty) is a list, any other an object, its keys
-- sorted so that the same model is the same text. (kv.json.encode writes a plain table as a
-- list; its objects are json.object, which the models are not.)
local function encode(v, out)
    local t = type(v)
    if t == "table" then
        if v[1] ~= nil or next(v) == nil then
            out[#out + 1] = "["
            for i = 1, #v do
                if i > 1 then out[#out + 1] = "," end
                encode(v[i], out)
            end
            out[#out + 1] = "]"
        else
            local keys = {}
            for k in pairs(v) do keys[#keys + 1] = tostring(k) end
            table.sort(keys)
            out[#out + 1] = "{"
            for i, k in ipairs(keys) do
                if i > 1 then out[#out + 1] = "," end
                encode(k, out)
                out[#out + 1] = ":"
                encode(v[k], out)
            end
            out[#out + 1] = "}"
        end
    elseif t == "string" then
        out[#out + 1] = '"' .. v:gsub('[%c"\\]', function(c)
            if c == '"' then return '\\"' elseif c == "\\" then return "\\\\" elseif c == "\n" then return "\\n" end
            return string.format("\\u%04x", c:byte())
        end) .. '"'
    elseif t == "number" then out[#out + 1] = math.type(v) == "integer" and tostring(v) or string.format("%.17g", v)
    elseif t == "boolean" then out[#out + 1] = v and "true" or "false"
    else out[#out + 1] = "null" end
end

local function json_of(v)
    local out = {}
    encode(v, out)
    return table.concat(out)
end

local function put(key, value)
    if type(value) == "table" then
        local text = json_of(value)
        if sent[key] ~= text then sent[key] = text; host.ui_set(key, text, true) end
    else
        local text = tostring(value)
        if sent[key] == text then return end
        sent[key] = text
        -- A string past what one KromeSet carries (56 bytes) goes as a document.
        if type(value) == "string" and #value > 55 then host.ui_set(key, json_of(value), true)
        else host.ui_set(key, value) end
    end
end

-- "key: message" (a failure or a warning as totals gives it) into its file key and the rest.
local function split_key(line)
    local key, rest = line:match("^(%S-:%S+): (.*)$")
    if key then return key, rest end
    return "", line
end

local function contains(hay, needle)
    return needle == "" or hay:lower():find(needle:lower(), 1, true) ~= nil
end

local STATE_ORDER = { running = 1, failed = 2, unchecked = 3, clean = 4 }

-- ---- Status --------------------------------------------------------------------------------------

local function status(run, t)
    local s = run.session
    local busy = s.graph and s.graph:busy() or { running = {}, waiting = 0 }
    local stable = s:stable()
    put("game", run.cfg.game)
    put("state", stable and "stable" or "working")
    put("summary", string.format("%d files: %d cooked, %d clean, %d failed, %d warned; %d running, %d waiting",
        t.files, t.cooked or 0, t.clean or 0, t.failed, t.warned, #busy.running, busy.waiting + (s.hashing or 0)))
    local programs = {}
    for _, p in ipairs(host.programs()) do programs[#programs + 1] = { name = p } end
    put("programs", programs)
    put("programs_none", #programs == 0 and "No program is connected. One that links asset_live connects by itself." or "")
end

local function steps(run)
    local g = run.session.graph
    local rows = {}
    if g then
        for key, step in pairs(g.steps) do
            -- A file a converter made is named by the source that made it, then its own name.
            local f, name = run.session.files[key], key
            local maker = f and f.work and g.by_work[f.work]
            if maker then name = maker.file .. "  >  " .. (f.path:match("([^/]+)$") or f.path) end
            rows[#rows + 1] = { key = key, name = name, conv = step.kind == "copy" and "copy" or (step.conv and step.conv.name or step.kind),
                                state = step.state, output = step.output or "", err = step.error or "" }
        end
    end
    table.sort(rows, function(a, b)
        local x, y = STATE_ORDER[a.state] or 9, STATE_ORDER[b.state] or 9
        if x ~= y then return x < y end
        return a.name < b.name
    end)
    put("steps", rows)
    put("steps_count", string.format("%d steps", #rows))
end

-- ---- Inspect -------------------------------------------------------------------------------------

local function inspect_model(run, key)
    local s = run.session
    local f = key and s.files[key]
    local sel = { key = key or "", state = "", rows = {}, up = {}, down = {}, none = "", lands = "" }
    if not f then
        sel.none = key and (key .. ": not a file this session knows") or "Pick a file on the left."
        return sel
    end
    if f.is_berg then
        local rec = s.rules:get(f.key)
        sel.state = "a rule file" .. (rec and rec.err and (", broken: " .. rec.err) or "")
        return sel
    end
    if not f.answer then sel.state = "not solved yet"; return sel end
    local eff, prov = s:metadata(f)
    sel.rows = explain.rows(f.answer, eff, eff and prov or {}, run.opts.out)
    local g = s.graph
    local step = g and g.steps[f.key]
    if step then sel.state = "step " .. step.state .. (step.error and (": " .. step.error) or "") end
    -- Upstream: the step whose run made this file. Downstream: what this file's step made.
    if g and f.work and g.by_work[f.work] then sel.up[#sel.up + 1] = { key = g.by_work[f.work].file } end
    if step then
        if step.kind == "copy" and step.output then sel.lands = "lands in the output tree as " .. step.output
        else
            local ran = g.ran[f.key]
            if ran then
                local names = {}
                for name in pairs(ran.outputs) do names[#names + 1] = name end
                table.sort(names)
                for _, name in ipairs(names) do sel.down[#sel.down + 1] = { key = s:key(s.work_root, step.work .. "/" .. name) } end
            end
        end
    end
    for _, u in ipairs(step and step.uses or {}) do sel.up[#sel.up + 1] = { key = u } end
    return sel
end

local function files(run)
    local q = host.ui_get("files_filter") or ""
    local keys = {}
    for key, f in pairs(run.session.files) do
        if not f.work and contains(key, q) then keys[#keys + 1] = key end
    end
    table.sort(keys)
    local rows, total = {}, 0
    for _, k in ipairs(keys) do
        total = total + 1
        if #rows < 5000 then rows[#rows + 1] = { key = k } end
    end
    put("files", rows)
    put("files_count", total > #rows and string.format("%d of %d files", #rows, total) or string.format("%d files", total))
end

-- ---- Reports -------------------------------------------------------------------------------------

local function reports(run, t)
    local which = host.ui_get("report") or "failed"
    local q = host.ui_get("report_filter") or ""
    local list = which == "warned" and t.warnings or t.errors
    local shown = {}
    for _, line in ipairs(list) do
        local key, what = split_key(line)
        if contains(key, q) or contains(what, q) then shown[#shown + 1] = { key = key, what = what } end
    end
    local field = sort.report == "what" and "what" or "key"
    table.sort(shown, function(a, b)
        if a[field] == b[field] then return a.key < b.key end
        if sort.up then return a[field] < b[field] end
        return a[field] > b[field]
    end)
    put("failed_count", #t.errors)
    put("warned_count", #t.warnings)
    put("shown", shown)
    put("shown_count", string.format("%d of %d", #shown, #list))
end

-- ---- the loop's side -----------------------------------------------------------------------------

function M.start(run)
    M.tick(run, true)
end

-- Every tick of the session: the small models when they change, the big ones every quarter
-- second while it works and once more when it goes still.
function M.tick(run, now)
    -- Nothing is kept by a screen still loading: everything is sent once it is built.
    if not host.ui_ready() then sent, M.ready = {}, false return end
    if not M.ready then M.ready, now = true, true; host.ui_set("page", host.ui_get("page") or "status") end
    local s = run.session
    local t = s:totals()
    status(run, t)
    local stable = s:stable()
    local due = host.now() >= next_big
    if now or due or (stable and not M.was_stable) then
        next_big = host.now() + 0.25
        steps(run)
        files(run)
        reports(run, t)
        if inspected then put("sel", inspect_model(run, inspected)) end
    end
    M.was_stable = stable
end

-- What the window said.
function M.event(run, event, id, value)
    if not run then return end
    local s = run.session
    if event == "inspect" and id then
        local key = id
        inspected = key
        put("sel", inspect_model(run, key))
        host.ui_set("page", "inspect") -- not through put: the tabs write it too
        sent.page = nil
    elseif event == "touch" then
        local f = inspected and s.files[inspected]
        if f then host.touch(f.root, f.path) end
    elseif event == "menu_inspect" then
        local key = host.ui_get("menu_key")
        if key and key ~= "" then return M.event(run, "inspect", key, value) end
    elseif event == "menu_touch" then
        local key = host.ui_get("menu_key")
        local f = key and s.files[key]
        if f then host.touch(f.root, f.path) end
    elseif event == "menu_copy" then
        local key = host.ui_get("menu_key")
        if key then host.clipboard(key) end
    elseif event == "cook" then
        if M.ask then M.ask({ "cook" }) end
    elseif event == "files_filter" then
        files(run)
    elseif event == "report" or event == "report_filter" then
        reports(run, s:totals())
    elseif event == "sort" and id then
        local field = id == "col_what" and "what" or "file"
        if sort.report == field then sort.up = not sort.up else sort.report, sort.up = field, true end
        reports(run, s:totals())
    end
end

-- For the tests: what was last sent for a key.
function M.sent(key) return sent[key] end

-- goldberg --window --demo: each page checked against the session (the pilot in goldberg.cc
-- presses, then asks). "" when it holds, else what did not; "first_file" answers a key.
local demo_key
function M.demo(run, what)
    local s = run.session
    local t = s:totals()
    local function same(key, want, text)
        local got = host.ui_get(key)
        if got ~= want then return string.format("%s shows '%s', the session says '%s'", text or key, tostring(got), tostring(want)) end
        return ""
    end
    if what == "status" then
        local n = 0
        for _ in pairs(s.graph and s.graph.steps or {}) do n = n + 1 end
        local busy = s.graph and s.graph:busy() or { running = {}, waiting = 0 }
        local why = same("steps_count", string.format("%d steps", n))
        if why ~= "" then return why end
        why = same("state", s:stable() and "stable" or "working")
        if why ~= "" then return why end
        return same("summary", string.format("%d files: %d cooked, %d clean, %d failed, %d warned; %d running, %d waiting",
            t.files, t.cooked or 0, t.clean or 0, t.failed, t.warned, #busy.running, busy.waiting + (s.hashing or 0)))
    elseif what == "files" then
        local q = host.ui_get("files_filter") or ""
        if q == "" then return "the filter was not typed" end
        files(run) -- what the window should have; it is what was sent, if the filter reached it
        local n = 0
        for key in pairs(s.files) do if not s.files[key].work and key:lower():find(q:lower(), 1, true) then n = n + 1 end end
        if n == 0 then return "no file has '" .. q .. "' in it" end
        return same("files_count", string.format("%d files", n))
    elseif what == "query" then
        -- Something to type: the name of the first source, less its extension, past its root.
        local keys = {}
        for key, f in pairs(s.files) do if not f.work and not f.is_berg then keys[#keys + 1] = key end end
        table.sort(keys)
        local name = keys[1] and keys[1]:match("([^/:]+)$") or ""
        return (name:match("^(.-)%.") or name):sub(1, 8)
    elseif what == "first_file" then
        local q = (host.ui_get("files_filter") or ""):lower()
        local keys = {}
        for key, f in pairs(s.files) do if not f.work and key:lower():find(q, 1, true) then keys[#keys + 1] = key end end
        table.sort(keys)
        demo_key = keys[1]
        return demo_key or ""
    elseif what == "inspect" then
        if not demo_key then return "no file was picked" end
        local why = same("page", "inspect")
        if why ~= "" then return why end
        why = same("sel.key", demo_key)
        if why ~= "" then return why end
        if sent.sel ~= json_of(inspect_model(run, demo_key)) then return "what Inspect was sent is not the session's answer for " .. demo_key end
        return ""
    elseif what == "reports" then
        local why = same("failed_count", tostring(#t.errors))
        if why ~= "" then return why end
        return same("shown_count", string.format("%d of %d", #t.errors, #t.errors))
    end
    return "no check named " .. tostring(what)
end

return M
