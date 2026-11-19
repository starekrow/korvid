-- goldberg's window (docs/PRDs/Krome-Slice-6-PRD.md, "goldberg's window"): the running session
-- as the window's models, and the window's presses as the session's commands. The screen is
-- goldberg/ui/window.kim (assets/ui/), drawn by Krome; this file is all it knows of goldberg.
--
-- Models go out only when they changed (each kept as the JSON last sent), and the big ones at
-- most every quarter second while the session works: a cook of thousands of steps is not sent
-- whole every tick. What the window says comes back as goldberg.ui_event(event, id, value);
-- what a person typed is read back from Krome's model (host.ui_get).
local explain = require("explain")
local bergform = require("bergform")

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

-- The step table holds the first of them, in state order (running, failed, waiting, then
-- clean): a cook of tens of thousands of steps is not sent whole. Inspect finds any file.
local MAX_STEPS = 2000

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
    local total = #rows
    for i = #rows, MAX_STEPS + 1, -1 do rows[i] = nil end
    put("steps", rows)
    put("steps_count", total > #rows and string.format("%d steps, the first %d shown", total, #rows) or string.format("%d steps", total))
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

-- ---- Rules and Diff ------------------------------------------------------------------------------

local form          -- bergform.read's, with what was typed into it kept (entries[i].value)
local form_version  -- its rule file's version when it was read
local add_scope = { scope = "meta", label = "meta" }
local pending       -- the edited text the Diff page shows, until accepted or rejected

local function bergs(run)
    local keys = {}
    for key, f in pairs(run.session.files) do if f.is_berg and run.session.rules:get(key) then keys[#keys + 1] = key end end
    table.sort(keys)
    local rows = {}
    for i, k in ipairs(keys) do rows[i] = { key = k } end
    put("bergs", rows)
    put("bergs_none", #rows == 0 and "No rule file in this session: everything here cooks by the conventions." or "")
end

local function send_form(force)
    if not form then return end
    if force then sent.form = nil end
    local entries = {}
    for i, e in ipairs(form.entries) do entries[i] = { id = e.id, label = e.label, key = e.key, value = e.value } end
    put("form", { key = form.key, broken = form.broken and ("broken on disk, its last good version shown: " .. form.broken) or "", entries = entries })
    put("add_scope_label", add_scope.label)
end

-- What was typed into the form's values, kept in it (so that sending it again loses nothing).
local function take_typed()
    if not form then return end
    for i, e in ipairs(form.entries) do
        local v = host.ui_get("form.entries." .. i .. ".value")
        if v then e.value = v end
    end
end

local function open_form(run, key)
    local f, why = bergform.read(run.session, key)
    form, pending = f, nil
    if not f then put("form_state", why) return end
    for _, e in ipairs(f.entries) do e.was = e.value end
    form_version = run.session.rules:get(key).version
    add_scope = { scope = "meta", label = "meta" }
    put("form_state", "")
    send_form(true)
end

-- The form's edits: each value that differs from the file, and each key added.
local function edits()
    local list = {}
    for _, e in ipairs(form.entries) do
        if e.value ~= e.was then list[#list + 1] = { path = e.path, value = e.value } end
    end
    return list
end

local function preview(run)
    if not form then return end
    take_typed()
    local list = edits()
    put("diff_error", "")
    if #list == 0 then put("form_state", "nothing edited") return end
    local text, why = bergform.apply(form, list)
    if not text then put("form_state", why) put("diff_error", why) return end
    local rows, perr = bergform.preview(run.session, form.key, text)
    if not rows then put("form_state", perr) return end
    pending = text
    local files = {}
    for _, r in ipairs(rows) do files[r.key] = true end
    local n = 0
    for _ in pairs(files) do n = n + 1 end
    put("diff", rows)
    put("diff_summary", n == 0 and string.format("%s, edited: no file's answer changes. Accept saves it.", form.key)
        or string.format("%s, edited: %d %s change%s.", form.key, n, n == 1 and "file's answer" or "files' answers", n == 1 and "s" or ""))
    host.ui_set("page", "diff")
end

-- ---- the loop's side -----------------------------------------------------------------------------

function M.start(run)
    M.tick(run, true)
end

-- Every tick of the session: the small models when they change, the big ones every quarter
-- second while it works and once more when it goes still.
-- What the session has been through, cheaply: when it is the same, nothing it shows changed.
local function mark(run)
    local s, g = run.session, run.session.graph
    return string.format("%d/%d/%d/%d/%d/%s/%d", s.solves or 0, g and g.cooked or 0, g and g.running or 0, g and g.unchecked or 0,
        s.hashing or 0, tostring(s:stable()), #host.programs())
end

local next_small = 0   -- the counts and the state: ten times a second at most
local last_mark

function M.tick(run, now)
    -- Nothing is kept by a screen still loading: everything is sent once it is built.
    if not host.ui_ready() then sent, M.ready = {}, false return end
    if not M.ready then M.ready, now = true, true; host.ui_set("page", host.ui_get("page") or "status") end
    local s = run.session
    local m = mark(run)
    local t_now = host.now()
    if not now and m == last_mark then return end -- nothing moved since the last look
    if not now and t_now < next_small then return end
    next_small = t_now + 0.1
    local t = s:totals()
    status(run, t)
    local stable = s:stable()
    local due = t_now >= next_big
    if now or due or (stable and not M.was_stable) then
        last_mark = m
        -- The big models: every half second while the session works, and once when it stops.
        next_big = t_now + 0.5
        steps(run)
        files(run)
        reports(run, t)
        bergs(run)
        if inspected then put("sel", inspect_model(run, inspected)) end
        -- The rule file shown changed on disk (a save, the Diff's accept): read again.
        if form and not pending then
            local rec = s.rules:get(form.key)
            if rec and rec.version ~= form_version then
                local was_state = host.ui_get("form_state")
                open_form(run, form.key)
                if was_state == "saved, cooking" then put("form_state", "saved") end
            end
        end
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
    elseif event == "pick_berg" and id then
        open_form(run, id)
    elseif event == "scope" and id and form then
        for _, e in ipairs(form.entries) do
            if e.id == id then add_scope = { scope = e.scope, label = e.label } end
        end
        put("add_scope_label", add_scope.label)
    elseif event == "add_entry" and form then
        take_typed()
        local k, v = host.ui_get("add_key") or "", host.ui_get("add_value") or ""
        if k == "" then put("form_state", "a key to add, first") return end
        local path = add_scope.scope == "meta" and { "meta", k } or { "rules", math.tointeger(tonumber(add_scope.scope)), "meta", k }
        form.entries[#form.entries + 1] = { id = "e" .. (#form.entries + 1), scope = add_scope.scope, label = add_scope.label, key = k, value = v, was = nil, path = path }
        host.ui_set("add_key", "")
        host.ui_set("add_value", "")
        send_form(true)
    elseif event == "preview" then
        preview(run)
    elseif event == "revert" and form then
        open_form(run, form.key)
        put("form_state", "reverted")
    elseif event == "accept" and form and pending then
        local ok, why = bergform.save(s, form.key, pending)
        pending = nil
        put("form_state", ok and "saved, cooking" or why)
        host.ui_set("page", "rules")
    elseif event == "reject" and form then
        pending = nil
        open_form(run, form.key)
        put("form_state", "the edit was rejected")
        host.ui_set("page", "rules")
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
local demo_key, demo_edit
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
        local why = same("steps_count", n > MAX_STEPS and string.format("%d steps, the first %d shown", n, MAX_STEPS) or string.format("%d steps", n))
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
    elseif what == "has_bergs" then
        for _, f in pairs(s.files) do if f.is_berg and s.rules:get(f.key) then return "" end end
        return "none"
    elseif what == "first_berg" then
        local keys = {}
        for key, f in pairs(s.files) do if f.is_berg and s.rules:get(key) then keys[#keys + 1] = key end end
        table.sort(keys)
        return keys[1] or ""
    elseif what == "rules" then
        if not form then return "no rule file was picked" end
        local why = same("form.key", form.key)
        if why ~= "" then return why end
        local fresh = bergform.read(s, form.key)
        if #fresh.entries ~= #form.entries then return string.format("the form has %d values, the file %d", #form.entries, #fresh.entries) end
        return ""
    elseif what == "rules_input" then
        -- The value of the first rule's first meta key: the one the demo edits.
        if not form then return "" end
        for _, e in ipairs(form.entries) do
            if e.path[1] == "rules" and e.path[3] == "meta" then demo_edit = e return e.id .. "_v" end
        end
        return ""
    elseif what == "rules_value" then
        -- What the demo types there: a string value the conventions do not give.
        return '"demo-value"'
    elseif what == "diff" then
        if not demo_edit then return "nothing was edited" end
        local why = same("page", "diff")
        if why ~= "" then return why end
        local text = bergform.apply(bergform.read(s, form.key), { { path = demo_edit.path, value = '"demo-value"' } })
        if not text then return "the demo's edit does not apply" end
        local rows = bergform.preview(s, form.key, text)
        if sent.diff ~= json_of(rows) then return "the Diff page was not sent what berg-preview says" end
        if #rows == 0 then return "the demo's edit changed nothing" end
        return ""
    elseif what == "saved" then
        local rec = s.rules:get(form and form.key or "")
        if not rec or not rec.text or not rec.text:find('"demo-value"', 1, true) then return "the rule file was not saved with the edit" end
        return same("form_state", "saved")
    elseif what == "reports" then
        local why = same("failed_count", tostring(#t.errors))
        if why ~= "" then return why end
        return same("shown_count", string.format("%d of %d", #t.errors, #t.errors))
    end
    return "no check named " .. tostring(what)
end

return M
