-- The graph of cook steps, the output tree they write, and the work directories.
--
-- A step is one converter run on one file. Its key is the hash of everything the result
-- depends on; a step whose key matches what made its outputs last time does not run.
-- There are two kinds, and nothing outside this file treats them differently:
--   copy    the file lands in the output tree as it is
--   shell   a script takes it somewhere: it writes files into the step's directory in
--           the work root, and each of those is a file like any other, solved in its
--           turn. A chain of converters is that, repeated.
--
-- A step whose file was made by another step waits for that step, unless the maker
-- scans: then its files arrive while it runs, and what follows them runs too.
--
-- The graph owns the output tree. It learns what is there from a scan at startup
-- (events on the output root), and what no step accounts for, it takes out: a source
-- deleted while goldberg was not running takes its output with it at the next cook.
-- Nothing taken out of the output tree is deleted. It is moved to the attic
-- (.goldberg.attic/<stamp>/ in the output tree), which only `goldberg clean attic`
-- empties. The work root is scratch: what leaves it is deleted.
local json = kv.json
local convert = require("convert")
local berg = require("berg")

local Graph = {}
Graph.__index = Graph

local COPY_VERSION = "1"
local SHELL_VERSION = "1"

local STATE = { unchecked = 0, clean = 1, dirty = 2, running = 3, failed = 4 }

-- session: whose files these are. state: the saved state, decoded, or nil.
function Graph.new(session, state)
    local g = setmetatable({
        s = session,
        steps = {},       -- file key -> step
        claim = {},       -- output -> the file key whose copy writes it
        by_work = {},     -- a step's directory in the work root -> the shell step
        by_script = {},   -- a script's file key -> { [file key] = true }: the steps that run it
        present = {},     -- output -> { size, mtime }: what is in the output tree
        built = {},       -- output -> { key, size, mtime, source }: what made it
        ran = {},         -- file key -> { key, outputs = { name -> hash } }: a shell step's last good run
        orphan_runs = {}, -- file key -> directory: runs whose step went while they ran
        out_scanned = false,
        swept = false,
        replan = false,   -- something changed that plan() should look at
        todo = {},        -- file key -> true: the steps plan() looks at (unchecked, or ran and ended)
        unchecked = 0, running = 0,
        cooked = 0, removed = 0,
        notes = {},       -- lines to print: removals, collisions, undeclared outputs
    }, Graph)
    if state and state.built then
        for _, out in ipairs(json.keys(state.built)) do
            local b = state.built[out]
            g.built[out] = { key = b.key, size = b.size, mtime = b.mtime, source = b.source }
        end
    end
    if state and json.is_object(state.ran) then
        for _, key in ipairs(json.keys(state.ran)) do
            local r = state.ran[key]
            local outputs = {}
            if json.is_object(r.outputs) then for _, name in ipairs(json.keys(r.outputs)) do outputs[name] = r.outputs[name] end end
            g.ran[key] = { key = r.key, outputs = outputs }
        end
    end
    return g
end

local function set_state(g, step, state)
    if step.state == "unchecked" then g.unchecked = g.unchecked - 1 end
    if step.state == "running" then g.running = g.running - 1 end
    step.state = state
    -- plan() looks only at these: a graph of tens of thousands of steps is not walked
    -- for every converter that ends.
    if state == "unchecked" or (state == "running" and step.exited) then g.todo[step.file] = true
    else g.todo[step.file] = nil end
    if state == "unchecked" then g.unchecked = g.unchecked + 1 end
    if state == "running" then g.running = g.running + 1 end
    g.replan = true
    if step.h then host.kinds.step:set(step.h, "state", STATE[state]) end
end

local function remove_output(g, output, why)
    g.s:request("remove", output)
    -- Never deleted: moved to the attic, where a person can get it back.
    local moved = g.s.io.remove_output(output)
    g.present[output] = nil
    g.built[output] = nil
    g.s.state_dirty = true
    g.removed = g.removed + 1
    if moved == false then g.notes[#g.notes + 1] = string.format("could not move %s to the attic: it is still in the output tree (%s)", output, why)
    else g.notes[#g.notes + 1] = string.format("moved %s to the attic (%s)", output, why) end
end

local function unlink_script(g, step)
    if step.script and g.by_script[step.script] then
        g.by_script[step.script][step.file] = nil
        if next(g.by_script[step.script]) == nil then g.by_script[step.script] = nil end
    end
    step.script = nil
end

local function drop_step(g, key, why)
    local step = g.steps[key]
    if not step then return end
    local was_running = step.state == "running"
    set_state(g, step, "clean") -- out of the counts
    if step.h then host.kinds.step:destroy(step.h) end
    g.steps[key] = nil
    g.todo[key] = nil
    if step.kind == "copy" then
        if g.claim[step.output] == key then g.claim[step.output] = nil end
        if g.present[step.output] or g.built[step.output] then remove_output(g, step.output, why) end
    else
        unlink_script(g, step)
        g.by_work[step.work] = nil
        g.ran[key] = nil
        g.s.state_dirty = true
        -- What it made goes with it, and so does everything cooked from that. A run
        -- still going is cleared when it ends.
        if was_running and not step.exited then g.orphan_runs[key] = step.work
        else g.s:clear_work(step.work) end
    end
end

-- A file's answer or content changed: make, change, or drop its step.
function Graph:file_changed(key)
    -- A script is an input of every step that runs it.
    if self.by_script[key] then
        local users = {}
        for user in pairs(self.by_script[key]) do users[#users + 1] = user end
        table.sort(users)
        for _, user in ipairs(users) do
            local step = self.steps[user]
            if step and step.state == "running" then step.again = true
            elseif step then set_state(self, step, "unchecked") end
        end
    end
    local f = self.s.files[key]
    local a = f and f.answer
    local kind, out
    if a and a.action == "copy" and a.output then kind, out = "copy", a.output
    elseif a and a.action == "convert" and a.conv then kind = "shell" end
    local step = self.steps[key]
    if step and (step.kind ~= kind or (kind == "copy" and step.output ~= out) or (kind == "shell" and step.conv.name ~= a.conv.name)) then
        drop_step(self, key, "its source no longer cooks to it")
        step = nil
    end
    if not kind then return end
    if kind == "copy" then
        local owner = self.claim[out]
        if owner and owner ~= key then
            -- Two sources, one output: neither is right.
            local msg = string.format("%s and %s both cook to %s", owner, key, out)
            local other = self.steps[owner]
            if other then other.error = msg; set_state(self, other, "failed") end
            f.collision = msg
            self.notes[#self.notes + 1] = msg
            return
        end
        f.collision = nil
        if not step then
            step = { kind = "copy", output = out, file = key, root = f.root, source = f.path, state = "clean" }
            step.h = host.kinds.step:spawn{ output = out, source = f.path, root = f.root }
            self.steps[key] = step
            self.claim[out] = key
        end
    else
        if not step then
            step = { kind = "shell", file = key, root = f.root, source = f.path, state = "clean", work = convert.work_dir(key, a.conv.name) }
            step.h = host.kinds.step:spawn{ output = "(work)/" .. step.work, source = f.path, root = f.root }
            self.steps[key] = step
            self.by_work[step.work] = step
        end
        step.conv, step.params = a.conv, a.params
        -- What it made before is looked at again: its type is this converter's to say.
        self.s:resolve_work(step.work)
    end
    step.error = nil
    if step.state == "running" then step.again = true -- its result will be of the old content
    else set_state(self, step, "unchecked") end
end

function Graph:file_removed(key)
    drop_step(self, key, "its source is gone")
    if self.by_script[key] then
        for user in pairs(self.by_script[key]) do
            local step = self.steps[user]
            if step and step.state ~= "running" then set_state(self, step, "unchecked") end
        end
    end
end

-- What a copy of this content to this place is made from.
local function copy_key(source_hash, output)
    return kv.sha256("copy\0" .. COPY_VERSION .. "\0" .. source_hash .. "\0" .. output)
end

-- An event on the output root: the startup scan saying what is there.
function Graph:out_event(ev)
    if ev.kind == "file-seen" then self.present[ev.path] = { size = ev.size, mtime = ev.mtime }
    elseif ev.kind == "scan-done" then self.out_scanned = true end
end

function Graph:ready()
    if not self.out_scanned then return false end
    return self.replan or (not self.swept and self.unchecked == 0 and self.running == 0)
end

local function fail(g, step, why)
    step.error = why
    step.again = false
    g.ran[step.file] = nil
    g.s.state_dirty = true
    set_state(g, step, "failed")
end

-- Whether the step that made this file may still change it.
local function maker_busy(g, f)
    if not f.work then return false end
    local maker = g.by_work[f.work]
    if not maker then return false end
    if maker.state == "unchecked" then return true end
    return maker.state == "running" and not (maker.scan and not maker.exited)
end

local function params_text(params)
    local keys, parts = {}, {}
    for k in pairs(params or {}) do keys[#keys + 1] = k end
    table.sort(keys)
    for _, k in ipairs(keys) do parts[#parts + 1] = k .. "=" .. berg.show(params[k]) end
    return table.concat(parts, " ")
end

local function check_copy(g, step, f)
    local out = step.output
    local key = copy_key(f.hash, out)
    local b, p = g.built[out], g.present[out]
    if b and p and b.key == key and b.size == p.size and b.mtime == p.mtime then
        set_state(g, step, "clean")
    else
        set_state(g, step, "running")
        g.s:request("copy", step.file .. " -> " .. out)
        g.s.io.copy(step.root, step.source, out)
    end
end

local function check_shell(g, step, f)
    local s, conv = g.s, step.conv
    local script, kind, outside = convert.script(conv, function(k) return s.files[k] ~= nil end)
    unlink_script(g, step)
    if script == nil then return fail(g, step, kind) end
    -- A program on PATH or by an absolute path is run as it is: nothing to hash or watch,
    -- so its steps are keyed by its name (in the signature) and not by its bytes.
    local sf, program, script_hash = nil, outside, ""
    if script then
        step.script = script
        g.by_script[script] = g.by_script[script] or {}
        g.by_script[script][step.file] = true
        sf = s.files[script]
        if not sf.hash then return end -- being read: looked at again when it is in
        program, script_hash = s:root_dir(sf.root) .. "/" .. sf.path, sf.hash
    end
    local scan = conv.outputs == "scan"
    local write_dir = (scan and s:root_dir(s.work_root) or s.run_dir) .. "/" .. step.work
    local vars = convert.vars(s:root_dir(f.root) .. "/" .. f.path, write_dir)
    local argv, env = convert.command(conv, program, kind, vars, step.params)
    if not argv then return fail(g, step, env) end
    local patterns, bad = convert.patterns(conv, vars, step.params)
    if bad then return fail(g, step, bad) end
    local _, place = s:place(f)
    local key = kv.sha256(table.concat({ "shell", SHELL_VERSION, convert.signature(conv), script_hash, f.hash, params_text(step.params), place }, "\0"))
    local saved = g.ran[step.file]
    if saved and saved.key == key then
        local all = true
        for name, hash in pairs(saved.outputs) do
            local made = s.files[s:key(s.work_root, step.work .. "/" .. name)]
            if not made or made.hash ~= hash then all = false end
        end
        if all then set_state(g, step, "clean") return end
    end
    step.run_key, step.patterns, step.scan, step.exited = key, patterns, scan, false
    set_state(g, step, "running")
    s:request("run", step.file .. " " .. conv.name)
    s.io.run{ root = f.root, path = f.path, argv = argv, env = env, work_root = s.work_root, dir = step.work, write_dir = write_dir, scan = scan, settle = conv.settle or 0 }
end

-- A run that ended well, with everything it wrote hashed: hold it to what it declared.
local function finish_shell(g, step)
    local s, conv = g.s, step.conv
    local made = s:work_list(step.work) -- { { name, hash } }, sorted
    local outputs, undeclared = {}, {}
    local patterns = type(step.patterns) == "table" and step.patterns or nil
    local hit = {}
    for _, m in ipairs(made) do
        outputs[m.name] = m.hash
        local declared = false
        for i, p in ipairs(patterns or {}) do
            if host.glob_path(p, m.name) then declared, hit[i] = true, true end
        end
        if not declared then undeclared[#undeclared + 1] = m.name end
    end
    for i, p in ipairs(patterns or {}) do
        if not hit[i] then
            return fail(g, step, string.format('the converter "%s" did not write "%s", which it declares (%s). If it no longer writes that, `goldberg learn` on this file corrects the declaration', conv.name, p, conv.file))
        end
    end
    if conv.outputs == nil then
        g.notes[#g.notes + 1] = string.format('%s: the converter "%s" declares no outputs; it wrote %d (%s). `goldberg learn` on this file writes the declaration into %s',
            step.file, conv.name, #made, table.concat(undeclared, ", "), conv.file)
    elseif patterns and #undeclared > 0 then
        g.notes[#g.notes + 1] = string.format('%s: the converter "%s" wrote %s, which it does not declare. Taken in; declare it in %s, or run `goldberg learn` on this file',
            step.file, conv.name, table.concat(undeclared, ", "), conv.file)
    end
    g.ran[step.file] = { key = step.run_key, outputs = outputs }
    s.state_dirty = true
    if step.h then host.kinds.step:set(step.h, "key", (step.run_key:gsub("%x%x", function(b) return string.char(tonumber(b, 16)) end))) end
    g.cooked = g.cooked + 1
    step.cooked = true
    step.exited = false
    if step.again then step.again = false; set_state(g, step, "unchecked")
    else set_state(g, step, "clean") end
end

-- With every file hashed and solved and the output tree read: finish the runs that
-- ended, decide which steps are dirty and start them, and, once nothing is left to do,
-- sweep what nothing accounts for (once).
function Graph:plan()
    local s = self.s
    local again = true
    while again do
        again = false
        local keys = {}
        for key in pairs(self.todo) do keys[#keys + 1] = key end
        table.sort(keys)
        for _, key in ipairs(keys) do
            local step = self.steps[key]
            local f = step and s.files[key]
            if step and f then
                local before = step.state
                if step.state == "running" then finish_shell(self, step)
                elseif f.hash and not maker_busy(self, f) then
                    if step.kind == "copy" then check_copy(self, step, f) else check_shell(self, step, f) end
                end
                -- A step that came to rest may have freed the ones after it.
                if step.state ~= before and step.state ~= "running" then again = true end
            end
        end
    end
    self.replan = false
    if self.swept or self.unchecked > 0 or self.running > 0 then return end
    self.swept = true
    local outs = {}
    for out in pairs(self.present) do if not self.claim[out] then outs[#outs + 1] = out end end
    table.sort(outs)
    for _, out in ipairs(outs) do
        local b = self.built[out]
        remove_output(self, out, b and ("its source " .. b.source .. " is gone") or "nothing here cooks to it")
    end
    -- What the state remembers of files that are no longer there is forgotten too.
    for out in pairs(self.built) do if not self.claim[out] then self.built[out] = nil; s.state_dirty = true end end
    for key in pairs(self.ran) do if not self.steps[key] then self.ran[key] = nil; s.state_dirty = true end end
    -- And in the work root: directories no step writes to.
    for _, dir in ipairs(s:work_dirs()) do
        if not self.by_work[dir] then s:clear_work(dir) end
    end
end

function Graph:cook_done(ev)
    local key = self.claim[ev.path]
    local step = key and self.steps[key]
    if not step then
        -- Dropped while it ran: what it wrote has no owner.
        self.present[ev.path] = { size = ev.size, mtime = ev.mtime }
        remove_output(self, ev.path, "its source went while it was being cooked")
        return
    end
    local f = self.s.files[step.file]
    self.present[ev.path] = { size = ev.size, mtime = ev.mtime }
    self.built[ev.path] = { key = copy_key(ev.hash, ev.path), size = ev.size, mtime = ev.mtime, source = step.file }
    self.s.state_dirty = true
    if step.h then host.kinds.step:set(step.h, "key", (self.built[ev.path].key:gsub("%x%x", function(b) return string.char(tonumber(b, 16)) end))) end
    self.cooked = self.cooked + 1
    step.cooked = true
    if step.again or (f and f.hash ~= ev.hash) then
        -- The source moved under the copy: look again once its new hash is in.
        step.again = false
        set_state(self, step, "unchecked")
    else
        set_state(self, step, "clean")
    end
end

function Graph:cook_failed(ev)
    local key = self.claim[ev.path]
    local step = key and self.steps[key]
    if not step then return end
    step.error = ev.text or "the cook failed"
    step.again = false
    set_state(self, step, "failed")
end

-- A converter's process ended. Its files' events came first; their hashes may not be in
-- yet, so a run that ended well is finished by plan(), once they are.
function Graph:step_exit(ev)
    local key = self.s:key(ev.root, ev.path)
    local step = self.steps[key]
    if not step or step.kind ~= "shell" or step.state ~= "running" or step.exited then
        local dir = self.orphan_runs[key]
        if dir then self.orphan_runs[key] = nil; self.s:clear_work(dir) end
        return
    end
    if ev.code ~= 0 then
        local text = (ev.text or ""):gsub("%s+$", ""):gsub("\n", " | ")
        local how = ev.code < 0 and "could not be started" or string.format("failed (exit %d)", ev.code)
        local again = step.again
        fail(self, step, string.format('the converter "%s" %s%s', step.conv.name, how, text ~= "" and (": " .. text) or ""))
        -- A scanning run that fails takes back what it delivered, and all cooked from it.
        if step.scan then self.s:clear_work(step.work) end
        if again then set_state(self, step, "unchecked") end
        return
    end
    step.exited = true
    self.todo[key] = true
    self.replan = true
end

-- Every step looked at again once the output tree has been read again (its scan is
-- coming): a session asked to cook checks what it would otherwise trust, and sweeps again.
function Graph:recheck()
    self.out_scanned = false
    self.present = {}
    self.swept = false
    for _, step in pairs(self.steps) do
        if step.state ~= "running" then set_state(self, step, "unchecked") end
    end
end

function Graph:stable()
    return self.unchecked == 0 and self.running == 0 and self.swept
end

-- What is not at rest, for status: { running = { lines }, waiting = n }.
function Graph:busy()
    local t = { running = {}, waiting = 0 }
    local keys = {}
    for key in pairs(self.steps) do keys[#keys + 1] = key end
    table.sort(keys)
    for _, key in ipairs(keys) do
        local step = self.steps[key]
        if step.state == "running" then t.running[#t.running + 1] = string.format("%s (%s)", key, step.kind == "copy" and "copy" or step.conv.name)
        elseif step.state == "unchecked" then t.waiting = t.waiting + 1 end
    end
    return t
end

-- { cooked, clean, failed, errors } for the last line.
function Graph:totals()
    local t = { cooked = self.cooked, clean = 0, failed = 0, removed = self.removed, errors = {} }
    local keys = {}
    for key in pairs(self.steps) do keys[#keys + 1] = key end
    table.sort(keys)
    for _, key in ipairs(keys) do
        local step = self.steps[key]
        if step.state == "failed" then
            t.failed = t.failed + 1
            t.errors[#t.errors + 1] = string.format("%s: %s", step.file, step.error)
        elseif step.state == "clean" and not step.cooked then t.clean = t.clean + 1 end
    end
    return t
end

-- The state to save: what each file was when it was last hashed, what made each output,
-- and what each converter's last good run made.
-- JSON text for a string or a number, without building a JSON value first: a state of
-- tens of thousands of files is mostly these.
local function jstr(v)
    if not v:find('[%c"\\]') then return '"' .. v .. '"' end
    return json.encode(v)
end
local function jnum(v)
    if math.type(v) == "integer" then return tostring(v) end
    return v and string.format("%.17g", v) or "null"
end

-- The state as text, made a piece at a time: returns a function that works until host.now()
-- passes its `deadline` (nil: until it is done) and returns the text once it is complete,
-- nil while it is not. The files, outputs and runs are the ones there when it began; each
-- is read as it is reached, so what changes meanwhile may be written as it was or as it is
-- now. Either is safe (an entry that is out of date costs a hash or a cook at the next
-- start, as a missing state does), and a change sets the session dirty again, so another
-- write follows this one. Keys are not sorted: the state is only ever read by goldberg.
function Graph:state_writer()
    local g, s = self, self.s
    local files, outs, rans = {}, {}, {}
    for key, f in pairs(s.files) do if not f.is_berg and f.hash then files[#files + 1] = key end end
    for out in pairs(g.built) do outs[#outs + 1] = out end
    for key in pairs(g.ran) do rans[#rans + 1] = key end
    local rows = host.kinds.file
    local parts, deadline = { '{"version":1,"files":{' }, nil
    local function each(list, piece)
        local first = true
        for i = 1, #list do
            local text = piece(list[i])
            if text then
                parts[#parts + 1] = first and text or ("," .. text)
                first = false
            end
            if i % 128 == 0 and deadline and host.now() >= deadline then coroutine.yield() end
        end
    end
    local co = coroutine.create(function()
        each(files, function(key)
            local f = s.files[key]
            if not (f and f.hash and f.h) then return nil end
            return string.format('%s:{"size":%s,"mtime":%s,"hash":"%s"}', jstr(key), jnum(rows:get(f.h, "size")), jnum(rows:get(f.h, "mtime")), f.hash)
        end)
        parts[#parts + 1] = '},"built":{'
        each(outs, function(out)
            local b = g.built[out]
            if not b then return nil end
            return string.format('%s:{"key":"%s","size":%s,"mtime":%s,"source":%s}', jstr(out), b.key, jnum(b.size), jnum(b.mtime), jstr(b.source))
        end)
        parts[#parts + 1] = '},"ran":{'
        each(rans, function(key)
            local r = g.ran[key]
            if not r then return nil end
            local o = {}
            for name, hash in pairs(r.outputs) do o[#o + 1] = jstr(name) .. ':"' .. hash .. '"' end
            return string.format('%s:{"key":"%s","outputs":{%s}}', jstr(key), r.key, table.concat(o, ","))
        end)
        parts[#parts + 1] = "}}\n"
    end)
    return function(until_time)
        deadline = until_time
        local ok, err = coroutine.resume(co)
        if not ok then error(err, 0) end
        if coroutine.status(co) == "dead" then return table.concat(parts) end
        return nil
    end
end

-- The whole state as text, now.
function Graph:state_text() return self:state_writer()(nil) end

function Graph:dump(out)
    local keys = {}
    for key in pairs(self.steps) do keys[#keys + 1] = key end
    table.sort(keys)
    for _, key in ipairs(keys) do
        local step = self.steps[key]
        local what = step.kind == "copy" and step.output or ("(work)/" .. step.work .. " by " .. step.conv.name)
        out[#out + 1] = string.format("step %s <- %s %s%s", what, step.file, step.state, step.error and (" error=" .. step.error) or "")
    end
end

return Graph
