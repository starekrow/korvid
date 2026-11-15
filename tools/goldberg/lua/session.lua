-- A session: the files goldberg knows, the rules over them, and each file's answer.
--
-- Everything a session learns arrives as an event (session:event), and everything it
-- wants done leaves through its `io` table (io.hash, io.scan). It reads no file and no
-- clock of its own, so the same events in the same order make the same session: that is
-- what `goldberg replay` runs, with an `io` that does nothing.
--
-- The records live in the engine's pools (host.kinds.file, host.kinds.berg): a row per
-- file with its path, size, time and hash. What a row cannot hold stays here: the
-- answer, and who is waiting for what.
local Rules = require("rules")
local solve = require("solve")
local Graph = require("graph")
local json = kv.json

local Session = {}
Session.__index = Session

local function hidden(path)
    return path:sub(1, 1) == "." or path:find("/.", 1, true) ~= nil
end

local function unhex(hex)
    return (hex:gsub("%x%x", function(b) return string.char(tonumber(b, 16)) end))
end

-- cfg: { game, roots = { { name, dir, abs } }, conventions = text, project = { text, display } or nil, io,
--        out_root = index of the output tree among the roots, or nil for a session that cooks nothing,
--        work_root = index of the work root (where converters write), run_dir = where a
--        converter with declared outputs writes until it ends,
--        state = the saved state's text, or nil }
--
-- A file in the work root was made by a converter. It is a file like any other, with two
-- differences: its type is its converter's "to" unless a rule refines it, and rules see
-- it where its source is (`place`), so the goldberg.berg over ships/ also governs what
-- ships/big.sheet turned into.
function Session.new(cfg)
    local s = setmetatable({
        game = cfg.game, roots = cfg.roots, io = cfg.io,
        rules = Rules.new(),
        files = {},        -- "<root>:<path>" -> { key, root, path, h, is_berg, answer, line, ... }
        count = 0,
        unsolved = {},     -- keys whose answer may have changed
        hashing = 0,       -- hash requests not yet answered
        berg_hashing = 0,  -- of those, rule files: nothing is solved while one is being read
        gen = {},          -- per root: the scan now running or last run
        requests = {},     -- everything asked of io, in order, as text
        changed = {},      -- keys whose answer changed at the last settle
        solves = 0,        -- answers computed, ever
        history = {},      -- one line per settle: how many were solved and whose answer changed
        work_root = cfg.work_root, run_dir = cfg.run_dir,
        work_files = {},   -- a step's directory in the work root -> { [key] = true }
    }, Session)
    s.rules:set("conventions", cfg.conventions, "conventions")
    if cfg.project then s.rules:set("project", cfg.project.text, cfg.project.display) end
    for i = 1, #cfg.roots do s.gen[i] = 0 end
    s.known = {}       -- from the saved state: key -> { size, mtime, hash }, for files not yet seen
    -- Whether the state to save differs from the one read: a file hashed, gone, or never
    -- seen again, or a step's record changed (the graph sets it). A cook that changed none
    -- of those does not write the state again: at tens of thousands of files that write
    -- was a third of a cook with nothing to do.
    s.state_dirty = cfg.state == nil
    s.touched = {}     -- keys whose step may need another look: a new answer, or new content
    s.removed = {}
    -- Per root, the scans asked for whose scan-done has not arrived. Nothing is planned
    -- while one is out: a step's outputs in the work root, not seen yet, would look
    -- missing, and the step would run again. (A session that takes its events a share at
    -- a time sees the work root's files ticks after the output tree's scan is done.)
    s.scanning = {}
    local state
    if cfg.state then
        state = json.decode(cfg.state)
        if state and json.is_object(state) and state.version == 1 and json.is_object(state.files) then
            for _, key in ipairs(json.keys(state.files)) do
                local k = state.files[key]
                s.known[key] = { size = k.size, mtime = k.mtime, hash = k.hash }
            end
        else
            state = nil -- unreadable, or another version's: a full cook
            s.state_dirty = true
        end
    end
    if cfg.out_root then
        s.out_root = cfg.out_root
        s.graph = Graph.new(s, state)
    end
    return s
end

function Session:key(root, path) return self.roots[root].name .. ":" .. path end

-- A root's directory as a child process can use it.
function Session:root_dir(root) return self.roots[root].abs or self.roots[root].dir end

-- Where rules see a file: the asset root's name and the path under it. For a source,
-- where it is; for a file a converter made, beside the file it was made from.
function Session:place(f)
    if f.vname then return f.vname, f.vpath, f.vroot end
    return self.roots[f.root].name, f.path, f.root
end

local function request(self, what, root, path, extra)
    self.requests[#self.requests + 1] = string.format("%s %s%s", what, self:key(root, path), extra or "")
end

-- For the graph: something asked of io, as text.
function Session:request(what, text)
    self.requests[#self.requests + 1] = what .. " " .. text
end

function Session:scan(root)
    self.scanning[root] = (self.scanning[root] or 0) + 1
    self.gen[root] = self.gen[root] + 1
    request(self, "scan", root, "")
    self.io.scan(root)
end

local function ask_hash(self, f)
    if f.hashing then f.again = true; return end
    f.hashing = true
    self.hashing = self.hashing + 1
    if f.is_berg then self.berg_hashing = self.berg_hashing + 1 end
    request(self, "hash", f.root, f.path, f.is_berg and " +text" or "")
    self.io.hash(f.root, f.path, f.is_berg)
end

-- Marks every file a rule file speaks for.
local function mark_affected(self, root, berg_path)
    local dir, name = Rules.dirname(berg_path), Rules.basename(berg_path)
    local prefix = dir == "" and "" or (dir .. "/")
    local sidecar = name ~= "goldberg.berg" and Rules.stem(name) or nil
    for key, f in pairs(self.files) do
        local _, path, at = self:place(f)
        if at == root and not f.is_berg and path:sub(1, #prefix) == prefix and not (f.work and not f.vname) then
            if not sidecar then self.unsolved[key] = true
            elseif Rules.dirname(path) == dir and Rules.stem(Rules.basename(path)) == sidecar then self.unsolved[key] = true end
        end
    end
end

local function touch(self, ev)
    if hidden(ev.path) then return end
    local key = self:key(ev.root, ev.path)
    local f = self.files[key]
    local files = host.kinds.file
    if not f then
        local work, rel
        if ev.root == self.work_root then
            work, rel = ev.path:match("^([^/]+)/(.+)$")
            if not work then return end -- loose in the work root: nobody's
        end
        local is_berg = ev.path:sub(-5) == ".berg" and not work
        local h = files:spawn{ path = ev.path, root = ev.root, is_berg = is_berg, scan = self.gen[ev.root] }
        if not h then error("goldberg.file is full: more files than the pool holds") end
        f = { key = key, root = ev.root, path = ev.path, h = h, is_berg = is_berg, work = work, rel = rel }
        self.files[key] = f
        if work then
            self.work_files[work] = self.work_files[work] or {}
            self.work_files[work][key] = true
        end
        self.count = self.count + 1
        if not is_berg then self.unsolved[key] = true end
        -- The state remembers it at this size and time: its hash is known without a read.
        local k = self.known[key]
        self.known[key] = nil -- what is left once the scans are done went while nobody looked
        if k and not is_berg and ev.kind == "file-seen" and k.size == ev.size and k.mtime == ev.mtime and type(k.hash) == "string" and #k.hash == 64 then
            f.hash = k.hash
            files:set(h, "size", ev.size)
            files:set(h, "mtime", ev.mtime)
            files:set(h, "hash", unhex(k.hash))
            files:set(h, "hashed", true)
            self.touched[key] = true
            return
        end
        if ev.kind == "file-made" then return end -- made() has its hash
        ask_hash(self, f)
        return
    end
    files:set(f.h, "scan", self.gen[ev.root])
    -- Seen again with the size and time it was hashed at: nothing to read.
    if ev.kind == "file-seen" and files:get(f.h, "hashed") and files:get(f.h, "size") == ev.size and files:get(f.h, "mtime") == ev.mtime then return end
    ask_hash(self, f)
end

local function forget(self, key)
    local f = self.files[key]
    if not f then return end
    if f.hash and not f.is_berg then self.state_dirty = true end
    host.kinds.file:destroy(f.h)
    self.files[key] = nil
    self.unsolved[key] = nil
    self.count = self.count - 1
    if f.work and self.work_files[f.work] then
        self.work_files[f.work][key] = nil
        if next(self.work_files[f.work]) == nil then self.work_files[f.work] = nil end
    end
    if f.hashing then
        -- Its answer will find nobody.
        self.hashing = self.hashing - 1
        if f.is_berg then self.berg_hashing = self.berg_hashing - 1 end
    end
    if f.is_berg then
        if f.berg_h then host.kinds.berg:destroy(f.berg_h) end
        self.rules:remove(key)
        mark_affected(self, f.root, f.path)
    end
    self.touched[key] = nil
    self.removed[#self.removed + 1] = key
end

local function hashed(self, ev)
    local key = self:key(ev.root, ev.path)
    local f = self.files[key]
    if not f or not f.hashing then return end
    f.hashing = false
    self.hashing = self.hashing - 1
    if f.is_berg then self.berg_hashing = self.berg_hashing - 1 end
    local files = host.kinds.file
    local was = f.hash
    if not f.is_berg then self.state_dirty = true end -- its size and time, at least, are new
    f.hash = ev.hash
    files:set(f.h, "size", ev.size)
    files:set(f.h, "mtime", ev.mtime)
    files:set(f.h, "hash", unhex(ev.hash))
    files:set(f.h, "hashed", true)
    if f.again then f.again = false; ask_hash(self, f) end
    if was == ev.hash then return end -- hashes decide: the same bytes are no change
    if f.is_berg then
        local root = self.roots[f.root]
        self.rules:set(key, ev.text or "", root.dir .. "/" .. f.path)
        local rec = self.rules:get(key)
        local bergs = host.kinds.berg
        if not f.berg_h then f.berg_h = bergs:spawn{ path = f.path, root = f.root } end
        if f.berg_h then bergs:set(f.berg_h, "version", rec.version); bergs:set(f.berg_h, "ok", rec.ok) end
        mark_affected(self, f.root, f.path)
    else
        self.touched[key] = true
    end
end

-- A converter's output, moved into the work root by the host and hashed on its way in:
-- seen and hashed in one event. If a hash of it is already out (a watcher's event got
-- there first), that one finishes and the file is asked for again.
local function made(self, ev)
    local key = self:key(ev.root, ev.path)
    if not self.files[key] then touch(self, ev) end
    local f = self.files[key]
    if not f then return end -- hidden
    if f.hashing then f.again = true return end
    f.hashing = true
    self.hashing = self.hashing + 1
    hashed(self, ev)
end

-- A step's directory in the work root, emptied: what a converter made is taken back, and
-- everything cooked from it follows as it would for a source that was deleted.
function Session:clear_work(dir)
    self:request("clear", "(work)/" .. dir)
    self.io.clear_work(self.work_root, dir)
    local keys = {}
    for key in pairs(self.work_files[dir] or {}) do keys[#keys + 1] = key end
    table.sort(keys)
    for _, key in ipairs(keys) do forget(self, key) end
end

-- The files of a step's directory are solved again: who made them changed.
function Session:resolve_work(dir)
    for key in pairs(self.work_files[dir] or {}) do self.unsolved[key] = true end
end

-- What a step's directory holds: { { name, hash } }, by name.
function Session:work_list(dir)
    local out = {}
    for key in pairs(self.work_files[dir] or {}) do
        local f = self.files[key]
        out[#out + 1] = { name = f.rel, hash = f.hash }
    end
    table.sort(out, function(a, b) return a.name < b.name end)
    return out
end

function Session:work_dirs()
    local out = {}
    for dir in pairs(self.work_files) do out[#out + 1] = dir end
    table.sort(out)
    return out
end

function Session:event(ev)
    local k = ev.kind
    if k == "scan-done" then
        local n = self.scanning[ev.root]
        if n then self.scanning[ev.root] = n > 1 and n - 1 or nil end
    end
    if k == "step-exit" then if self.graph then self.graph:step_exit(ev) end return end
    if k == "project-changed" then
        -- The project's goldberg.berg, read again (or gone): it speaks for every file.
        if ev.text then self.rules:set("project", ev.text, ev.path) else self.rules:remove("project") end
        for key, f in pairs(self.files) do if not f.is_berg and not (f.work and not f.vname) then self.unsolved[key] = true end end
        return
    end
    if k == "cook-done" then if self.graph then self.graph:cook_done(ev) end return end
    if k == "cook-failed" then if self.graph then self.graph:cook_failed(ev) end return end
    if self.graph and ev.root == self.out_root then self.graph:out_event(ev) return end
    if k == "file-seen" or k == "file-added" or k == "file-changed" then touch(self, ev)
    elseif k == "file-made" then made(self, ev)
    elseif k == "file-removed" or k == "hash-failed" then
        local key = self:key(ev.root, ev.path)
        local f = self.files[key]
        -- A hash that failed for a file asked about again is not a removal yet.
        if k == "hash-failed" and f and f.again then
            f.hashing, f.again = false, false
            self.hashing = self.hashing - 1
            if f.is_berg then self.berg_hashing = self.berg_hashing - 1 end
            ask_hash(self, f)
        else
            forget(self, key)
        end
    elseif k == "dir-removed" then
        local prefix = ev.path .. "/"
        local gone = {}
        for key, f in pairs(self.files) do
            if f.root == ev.root and f.path:sub(1, #prefix) == prefix then gone[#gone + 1] = key end
        end
        table.sort(gone)
        for _, key in ipairs(gone) do forget(self, key) end
    elseif k == "hashed" then hashed(self, ev)
    elseif k == "scan-done" then
        -- What this scan did not see is gone: it went while nobody was watching.
        local gen, gone = self.gen[ev.root], {}
        for key, f in pairs(self.files) do
            if f.root == ev.root and host.kinds.file:get(f.h, "scan") ~= gen then gone[#gone + 1] = key end
        end
        table.sort(gone)
        for _, key in ipairs(gone) do forget(self, key) end
    elseif k == "rescan" then self:scan(ev.root)
    end
end

-- Whether settle() has anything to do, and may do it: no rule file is being read.
function Session:can_settle()
    return self.berg_hashing == 0 and next(self.unsolved) ~= nil
end

-- Solves every file whose answer may have changed. Returns how many answers did.
function Session:settle()
    local keys = {}
    for key in pairs(self.unsolved) do keys[#keys + 1] = key end
    table.sort(keys)
    self.unsolved = {}
    self.changed = {}
    local done = {}
    local function solve_one(key)
        if done[key] then return end
        done[key] = true
        local f = self.files[key]
        if not f then return end
        local base
        if f.work then
            -- Made by a converter: placed beside its source, which is placed first.
            local maker = self.graph and self.graph.by_work[f.work]
            local src = maker and self.files[maker.file]
            if not src then
                -- Nobody's yet: its maker's step is made when the source is solved, and
                -- asks for this again. One that never is, the sweep removes.
                f.answer, f.line, f.vname, f.vpath, f.vroot = nil, nil, nil, nil, nil
                return
            end
            if self.unsolved_now[maker.file] then solve_one(maker.file) end
            local sname, spath, sroot = self:place(src)
            local dir = Rules.dirname(spath)
            f.vname, f.vpath, f.vroot = sname, (dir == "" and "" or dir .. "/") .. f.rel, sroot
            base = { type = maker.conv.to, file = maker.conv.file, where = 'converter "' .. maker.conv.name .. '"' }
        end
        local name, path = self:place(f)
        local eff, prov, reg, used, warn = self.rules:effective(name, path, base)
        local a
        if not eff then a = { action = "error", error = prov, why = {} }
        else
            a = solve.solve(name, path, eff, prov, reg)
            a.rule_warn = warn
        end
        self.solves = self.solves + 1
        local line = solve.line(a)
        if line ~= f.line then
            f.answer, f.line = a, line
            self.changed[#self.changed + 1] = key
            self.touched[key] = true
        else
            f.answer = a -- the same answer; the provenance may have moved
            -- A converter's registration is not in the line: its step looks for itself.
            if a.action == "convert" then self.touched[key] = true end
        end
    end
    self.unsolved_now = {}
    for _, key in ipairs(keys) do self.unsolved_now[key] = true end
    for _, key in ipairs(keys) do solve_one(key) end
    self.unsolved_now = nil
    self.history[#self.history + 1] = string.format("settle solved=%d changed=%s", #keys, table.concat(self.changed, ","))
    return #self.changed
end

-- Whether advance() would do anything now. The driver logs a marker each time it calls
-- advance, so a replay advances at the same points.
function Session:has_work()
    if self:can_settle() then return true end
    if not self.graph then return false end
    if next(self.touched) ~= nil or #self.removed > 0 then return true end
    return self.hashing == 0 and next(self.unsolved) == nil and next(self.scanning) == nil and self.graph:ready()
end

-- One step of the session's own work: solve what can be solved, bring the graph up to
-- date with the answers, and, once every source is hashed and solved and the output tree
-- has been read, plan and start the cooks.
function Session:advance()
    if self:can_settle() then self:settle() end
    local g = self.graph
    if not g then self.touched, self.removed = {}, {} return end
    if self.berg_hashing > 0 then return end -- answers may be about to change
    local removed = self.removed
    self.removed = {}
    for _, key in ipairs(removed) do g:file_removed(key) end
    local keys = {}
    for key in pairs(self.touched) do if not self.unsolved[key] then keys[#keys + 1] = key end end
    table.sort(keys)
    for _, key in ipairs(keys) do
        self.touched[key] = nil
        g:file_changed(key)
    end
    if self.hashing == 0 and next(self.unsolved) == nil and next(self.scanning) == nil and g:ready() then g:plan() end
end

-- Whether the state written now would differ from the one read, or last written.
function Session:state_changed()
    return self.state_dirty or next(self.known) ~= nil
end

-- A write of the state begins: it accounts for everything changed so far, the files the
-- state knew and nobody saw included.
function Session:state_taken()
    self.state_dirty = false
    self.known = {}
end

function Session:stable()
    if self.hashing ~= 0 or next(self.unsolved) ~= nil or next(self.scanning) ~= nil then return false end
    if not self.graph then return true end
    return next(self.touched) == nil and #self.removed == 0 and self.graph:stable()
end

-- The session as text, the same for the same session: every file and its answer, then
-- everything asked of the host.
function Session:dump()
    local keys, out = {}, {}
    for key in pairs(self.files) do keys[#keys + 1] = key end
    table.sort(keys)
    for _, key in ipairs(keys) do
        local f = self.files[key]
        out[#out + 1] = string.format("file %s %s%s", key, f.is_berg and "rules" or (f.line or "unsolved"), f.hash and (" #" .. f.hash:sub(1, 12)) or "")
    end
    if self.graph then self.graph:dump(out) end
    for _, h in ipairs(self.history) do out[#out + 1] = h end
    for _, r in ipairs(self.requests) do out[#out + 1] = "request " .. r end
    return table.concat(out, "\n") .. "\n"
end

-- One file's effective metadata and who said each key, as its answer was made from.
function Session:metadata(f)
    local base
    if f.work then
        local maker = self.graph and self.graph.by_work[f.work]
        if maker then base = { type = maker.conv.to, file = maker.conv.file, where = 'converter "' .. maker.conv.name .. '"' } end
    end
    local name, path = self:place(f)
    return self.rules:effective(name, path, base)
end

-- Counts for the last line of a cook.
function Session:totals()
    local t = { files = 0, rules = 0, failed = 0, warned = 0, errors = {}, warnings = {} }
    local keys = {}
    for key in pairs(self.files) do keys[#keys + 1] = key end
    table.sort(keys)
    for _, key in ipairs(keys) do
        local f = self.files[key]
        if f.is_berg then
            t.rules = t.rules + 1
            local rec = self.rules:get(key)
            if rec and rec.err then t.errors[#t.errors + 1] = rec.err end
        else
            if not f.work then t.files = t.files + 1 end -- "files" are sources: what converters made is not counted
            local a = f.answer
            if a and a.error then
                t.failed = t.failed + 1
                -- A broken rule file is reported once, above, not once per file under it.
                local from_rules = false
                for _, e in ipairs(t.errors) do if e == a.error then from_rules = true end end
                if not from_rules then t.errors[#t.errors + 1] = string.format("%s: %s", key, a.error) end
            elseif a and a.warn then
                t.warned = t.warned + 1
                t.warnings[#t.warnings + 1] = string.format("%s: %s", key, a.warn)
            end
        end
    end
    if self.graph then
        local g = self.graph:totals()
        t.cooked, t.clean, t.removed = g.cooked, g.clean, g.removed
        t.failed = t.failed + g.failed
        for _, e in ipairs(g.errors) do t.errors[#t.errors + 1] = e end
    end
    return t
end

return Session
