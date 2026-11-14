-- The graph of cook steps, and the output tree they write.
--
-- A step is one converter run: a source, an output, and the key of what the output was
-- last made from. A step whose key matches what made the file in the output tree does
-- not run. In this slice's first half every step is a copy; the shape is the one every
-- converter uses.
--
-- The graph owns the output tree. It learns what is there from a scan at startup
-- (events on the output root), and what no step accounts for, it removes: a source
-- deleted while goldberg was not running takes its output with it at the next cook.
local json = kv.json

local Graph = {}
Graph.__index = Graph

local COPY_VERSION = "1"

local STATE = { unchecked = 0, clean = 1, dirty = 2, running = 3, failed = 4 }

-- session: whose files these are. state: the saved state, decoded, or nil.
function Graph.new(session, state)
    local g = setmetatable({
        s = session,
        steps = {},       -- output -> step
        by_file = {},     -- file key -> output
        present = {},     -- output -> { size, mtime }: what is in the output tree
        built = {},       -- output -> { key, size, mtime, source }: what made it
        out_scanned = false,
        swept = false,
        unchecked = 0, running = 0,
        cooked = 0, removed = 0,
        notes = {},       -- lines to print: removals, collisions
    }, Graph)
    if state and state.built then
        for _, out in ipairs(json.keys(state.built)) do
            local b = state.built[out]
            g.built[out] = { key = b.key, size = b.size, mtime = b.mtime, source = b.source }
        end
    end
    return g
end

local function set_state(g, step, state)
    if step.state == "unchecked" then g.unchecked = g.unchecked - 1 end
    if step.state == "running" then g.running = g.running - 1 end
    step.state = state
    if state == "unchecked" then g.unchecked = g.unchecked + 1 end
    if state == "running" then g.running = g.running + 1 end
    if step.h then host.kinds.step:set(step.h, "state", STATE[state]) end
end

local function remove_output(g, output, why)
    g.s:request("remove", output)
    g.s.io.remove_output(output)
    g.present[output] = nil
    g.built[output] = nil
    g.removed = g.removed + 1
    g.notes[#g.notes + 1] = string.format("removed %s (%s)", output, why)
end

local function drop_step(g, output, why)
    local step = g.steps[output]
    if not step then return end
    set_state(g, step, "clean") -- out of the counts
    if step.h then host.kinds.step:destroy(step.h) end
    g.steps[output] = nil
    if g.by_file[step.file] == output then g.by_file[step.file] = nil end
    if g.present[output] or g.built[output] then remove_output(g, output, why) end
end

-- A file's answer or content changed: make, move, or drop its step.
function Graph:file_changed(key)
    local f = self.s.files[key]
    local a = f and f.answer
    local want = a and a.action == "copy" and a.output or nil
    local have = self.by_file[key]
    if have and have ~= want then drop_step(self, have, "its source no longer cooks to it") end
    if not want then return end
    local step = self.steps[want]
    if step and step.file ~= key then
        -- Two sources, one output: neither is right.
        local msg = string.format("%s and %s both cook to %s", step.file, key, want)
        step.error = msg
        set_state(self, step, "failed")
        f.collision = msg
        self.notes[#self.notes + 1] = msg
        return
    end
    f.collision = nil
    if not step then
        step = { output = want, file = key, root = f.root, source = f.path }
        step.h = host.kinds.step:spawn{ output = want, source = f.path, root = f.root }
        self.steps[want] = step
        self.by_file[key] = want
        step.state = "clean"
    end
    step.error = nil
    if step.state == "running" then step.again = true -- its result will be of the old content
    else set_state(self, step, "unchecked") end
end

function Graph:file_removed(key)
    local have = self.by_file[key]
    if have then drop_step(self, have, "its source is gone") end
end

-- What a copy of this content to this place is made from.
local function step_key(source_hash, output)
    return kv.sha256("copy\0" .. COPY_VERSION .. "\0" .. source_hash .. "\0" .. output)
end

-- An event on the output root: the startup scan saying what is there.
function Graph:out_event(ev)
    if ev.kind == "file-seen" then self.present[ev.path] = { size = ev.size, mtime = ev.mtime }
    elseif ev.kind == "scan-done" then self.out_scanned = true end
end

function Graph:ready()
    return self.out_scanned and (self.unchecked > 0 or not self.swept)
end

-- With every source hashed and solved and the output tree read: sweep what nothing
-- accounts for (once), decide which steps are dirty, and start them.
function Graph:plan()
    if not self.swept then
        self.swept = true
        local outs = {}
        for out in pairs(self.present) do if not self.steps[out] then outs[#outs + 1] = out end end
        table.sort(outs)
        for _, out in ipairs(outs) do
            local b = self.built[out]
            remove_output(self, out, b and ("its source " .. b.source .. " is gone") or "nothing here cooks to it")
        end
        -- What the state remembers of files that are no longer there is forgotten too.
        for out in pairs(self.built) do if not self.steps[out] then self.built[out] = nil end end
    end
    local outs = {}
    for out, step in pairs(self.steps) do if step.state == "unchecked" then outs[#outs + 1] = out end end
    table.sort(outs)
    for _, out in ipairs(outs) do
        local step = self.steps[out]
        local f = self.s.files[step.file]
        local key = step_key(f.hash, out)
        local b, p = self.built[out], self.present[out]
        if b and p and b.key == key and b.size == p.size and b.mtime == p.mtime then
            set_state(self, step, "clean")
        else
            set_state(self, step, "running")
            self.s:request("copy", self.s.roots[step.root].name .. ":" .. step.source .. " -> " .. out)
            self.s.io.copy(step.root, step.source, out)
        end
    end
end

function Graph:cook_done(ev)
    local step = self.steps[ev.path]
    if not step then
        -- Dropped while it ran: what it wrote has no owner.
        self.present[ev.path] = { size = ev.size, mtime = ev.mtime }
        remove_output(self, ev.path, "its source went while it was being cooked")
        return
    end
    local f = self.s.files[step.file]
    self.present[ev.path] = { size = ev.size, mtime = ev.mtime }
    self.built[ev.path] = { key = step_key(ev.hash, ev.path), size = ev.size, mtime = ev.mtime, source = step.file }
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
    local step = self.steps[ev.path]
    if not step then return end
    step.error = ev.text or "the cook failed"
    step.again = false
    set_state(self, step, "failed")
end

function Graph:stable()
    return self.unchecked == 0 and self.running == 0 and self.swept
end

-- { cooked, clean, failed, errors } for the last line.
function Graph:totals()
    local t = { cooked = self.cooked, clean = 0, failed = 0, removed = self.removed, errors = {} }
    local outs = {}
    for out in pairs(self.steps) do outs[#outs + 1] = out end
    table.sort(outs)
    for _, out in ipairs(outs) do
        local step = self.steps[out]
        if step.state == "failed" then
            t.failed = t.failed + 1
            t.errors[#t.errors + 1] = string.format("%s: %s", step.file, step.error)
        elseif step.state == "clean" and not step.cooked then t.clean = t.clean + 1 end
    end
    return t
end

-- The state to save: what each source was when it was last hashed, and what made each output.
function Graph:state_text()
    local files, built = json.object(), json.object()
    local keys = {}
    for key, f in pairs(self.s.files) do if not f.is_berg and f.hash then keys[#keys + 1] = key end end
    table.sort(keys)
    local rows = host.kinds.file
    for _, key in ipairs(keys) do
        local f = self.s.files[key]
        files[key] = json.object("size", rows:get(f.h, "size"), "mtime", rows:get(f.h, "mtime"), "hash", f.hash)
    end
    local outs = {}
    for out in pairs(self.built) do outs[#outs + 1] = out end
    table.sort(outs)
    for _, out in ipairs(outs) do
        local b = self.built[out]
        built[out] = json.object("key", b.key, "size", b.size, "mtime", b.mtime, "source", b.source)
    end
    return json.encode(json.object("version", 1, "files", files, "built", built), { indent = 1 }) .. "\n"
end

function Graph:dump(out)
    local outs = {}
    for o in pairs(self.steps) do outs[#outs + 1] = o end
    table.sort(outs)
    for _, o in ipairs(outs) do
        local step = self.steps[o]
        out[#out + 1] = string.format("step %s <- %s %s%s", o, step.file, step.state, step.error and (" error=" .. step.error) or "")
    end
end

return Graph
