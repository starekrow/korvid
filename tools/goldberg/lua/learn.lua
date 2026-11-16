-- goldberg learn FILE: runs FILE's converter once, watches what it writes, and writes
-- that down as the converter's "outputs" in the .berg file that registers it.
--
-- The script runs as a cook would run it, on FILE, into a directory nobody else uses.
-- Nothing is cooked, nothing lands, and no state is read or written. What is seen:
--   which files were there when it ended (its outputs), and when each was written;
--   what it made and took away again (scratch: not an output);
--   whether it wrote anywhere under the asset root, where a converter should not.
-- The names are turned into patterns: the source's stem becomes {stem}, and names that
-- differ only in a number become one pattern with a `*`. Then one value in one .berg
-- file is changed, and the rest of that file is left as it was written.
--
-- This is not the session: it is not replayed, so it reads the clock and the files it
-- needs for itself.
local berg = require("berg")
local solve = require("solve")
local convert = require("convert")
local bergedit = require("bergedit")

local Learn = {}
Learn.__index = Learn

local function say(fmt, ...) host.print(string.format(fmt, ...)) end
local function complain(fmt, ...) host.warn("goldberg: " .. string.format(fmt, ...)) end

local function escape(s) return (s:gsub("%W", "%%%0")) end

-- The names as patterns. `stem` standing alone in a name becomes {stem}; names that then
-- differ only in their digits share one pattern, with * where the digits were.
function Learn.patterns(names, stem)
    local shapes, order = {}, {}
    for _, name in ipairs(names) do
        local t = name
        if stem ~= "" then t = t:gsub("%f[%w]" .. escape(stem) .. "%f[%W]", "{stem}") end
        -- Digits, but not those of a name a brace holds.
        local shape = t:gsub("%d+", "*")
        if not shapes[shape] then shapes[shape] = {}; order[#order + 1] = shape end
        shapes[shape][#shapes[shape] + 1] = t
    end
    local out = {}
    for _, shape in ipairs(order) do
        local group = shapes[shape]
        if #group > 1 then out[#out + 1] = shape else out[#out + 1] = group[1] end
    end
    table.sort(out)
    return out
end

local function json_list(list)
    local parts = {}
    for i, v in ipairs(list) do parts[i] = kv.json.encode(v) end
    return "[" .. table.concat(parts, ", ") .. "]"
end

-- loc: the located file. rules: the rule files that speak for it. o: the options.
-- Returns the learn under way, or nil when there was nothing to run (the host is told).
function Learn.start(loc, rules, o)
    local eff, prov, reg = rules:effective(loc.name, loc.rel)
    if not eff then complain("%s", prov) host.finish(1) return nil end
    local a = solve.solve(loc.name, loc.rel, eff, prov, reg)
    if a.error then complain("%s: %s", loc.rel, a.error) host.finish(1) return nil end
    if a.action ~= "convert" then
        complain('%s: no converter takes this file (%s), so there is nothing to learn. learn is for a source a shell converter takes: register one in a goldberg.berg ("converters": { "name": { "from": "%s", "to": "...", "shell": "tools/name" } }) and run this again',
            loc.rel, a.action == "ignore" and '"cook": false' or (a.type and ("a " .. a.type .. ", " .. (a.why.path or a.action)) or "it has no type"), a.type or "its-type")
        host.finish(1)
        return nil
    end
    local conv = a.conv
    local script, kind, outside = convert.script(conv, function(key) return kv.fs.is_file(loc.abs .. "/" .. key:match("^[^:]*:(.*)$")) end)
    if script == nil then complain("%s", kind) host.finish(1) return nil end
    local temp = kv.fs.temp_dir("goldberg-learn")
    local work = temp .. "/out"
    local input = loc.abs .. "/" .. loc.rel
    local vars = convert.vars(input, work)
    local program = script and (loc.abs .. "/" .. script:match("^[^:]*:(.*)$")) or outside -- outside: on PATH, or absolute
    local argv, env = convert.command(conv, program, kind, vars, a.params)
    if not argv then complain("%s", env) kv.fs.remove_tree(temp) host.finish(1) return nil end

    local self = setmetatable({
        loc = loc, o = o, conv = conv, answer = a, temp = temp, vars = vars, rules = rules,
        berg_file = loc.abs .. "/" .. conv.key:match("^[^:]*:(.*)$"),
        seen = {},    -- name -> { first, last, gap, gone }
        stray = {},   -- paths under the asset root that changed while it ran
        t0 = host.now(),
    }, Learn)
    self.backend = host.watch(o.watch or "auto")
    -- What the script wrote under the asset root must be heard before its exit is, on any
    -- watcher (FSEvents and the polling one report a moment late): strays are judged at exit.
    host.watch_sync(true)
    self.source_root = host.add_root(loc.name, loc.abs)
    self.work_root = host.add_root("(learn)", temp)
    -- It scans, whatever the converter declares: every file is reported as it is written.
    host.run{ root = self.source_root, path = loc.rel, argv = argv, env = env, work_root = self.work_root, dir = "out", write_dir = work, scan = true, settle = 0 }
    host.active(true)
    return self
end

function Learn:event(ev)
    local now = host.now() - self.t0
    if ev.kind == "step-exit" then return self:finish(ev, now) end
    if ev.root == self.work_root then
        local name = ev.path:match("^out/(.+)$")
        if not name then return end
        local rec = self.seen[name]
        if ev.kind == "file-removed" then
            if rec then rec.gone = true end
        elseif ev.kind == "file-added" or ev.kind == "file-changed" then
            if not rec then self.seen[name] = { first = now, last = now, gap = 0 }
            else
                rec.gone = false
                if now - rec.last > rec.gap then rec.gap = now - rec.last end
                rec.last = now
            end
        elseif ev.kind == "file-seen" then
            -- The look round at the end: there, whether or not the watcher said so.
            if not rec then self.seen[name] = { first = now, last = now, gap = 0 } else rec.gone = false end
        end
    elseif ev.root == self.source_root then
        if (ev.kind == "file-added" or ev.kind == "file-changed" or ev.kind == "file-removed") and ev.path:sub(1, 1) ~= "." and not ev.path:find("/.", 1, true) then
            self.stray[ev.path] = true
        end
    end
end

function Learn:done(code)
    if not self.o.keep then kv.fs.remove_tree(self.temp) else say("goldberg: learn: what it wrote is kept in %s/out", self.temp) end
    host.finish(code)
end

function Learn:finish(ev, took)
    local conv, o = self.conv, self.o
    if ev.code ~= 0 then
        local text = (ev.text or ""):gsub("%s+$", "")
        complain('learn: the converter "%s" %s on %s%s. Nothing was changed', conv.name, ev.code < 0 and "could not be started" or string.format("failed (exit %d)", ev.code), self.loc.rel, text ~= "" and (": " .. text) or "")
        return self:done(1)
    end
    local names, scratch = {}, {}
    for name, rec in pairs(self.seen) do
        if rec.gone then scratch[#scratch + 1] = name else names[#names + 1] = name end
    end
    table.sort(names)
    table.sort(scratch)
    local stray = {}
    for path in pairs(self.stray) do stray[#stray + 1] = path end
    table.sort(stray)
    if #stray > 0 then
        complain("learn: %d file%s under the asset root %s changed while it ran: %s. If the script wrote there, it should not: a converter writes into {work} (its second argument, unless \"args\" says otherwise) and nowhere else",
            #stray, #stray == 1 and "" or "s", self.loc.dir, table.concat(stray, ", "))
    end
    if #names == 0 then
        complain('learn: the converter "%s" wrote nothing into its work directory on %s. Nothing was changed', conv.name, self.loc.rel)
        return self:done(1)
    end
    say('goldberg: learn: "%s" on %s wrote %d file%s in %.2f s', conv.name, self.loc.rel, #names, #names == 1 and "" or "s", took)
    local first_done, last_done, gap = math.huge, 0, 0
    for _, name in ipairs(names) do
        local rec = self.seen[name]
        -- What its name alone would make it, where that is not what the converter says.
        local dir = self.loc.rel:match("^(.*)/[^/]*$")
        local eff = self.rules:effective(self.loc.name, (dir and dir .. "/" or "") .. name)
        local by_name = eff and type(eff.type) == "string" and eff.type or nil
        if by_name and not solve.is_a(by_name, conv.to) and not solve.is_a(conv.to, by_name) then
            say('  %s   (by its name a %s; as an output of "%s" it is a %s)', name, by_name, conv.name, conv.to)
        else
            say("  %s", name)
        end
        first_done, last_done = math.min(first_done, rec.last), math.max(last_done, rec.last)
        gap = math.max(gap, rec.gap)
    end
    if #scratch > 0 then say("  (made and removed again, so not outputs: %s)", table.concat(scratch, ", ")) end

    -- What to declare.
    local path = { "converters", conv.name, "outputs" }
    local edits, what = {}, nil
    local was = conv.outputs == nil and "it declared none" or ("it declared " .. (type(conv.outputs) == "table" and json_list(conv.outputs) or '"scan"'))
    local spread = last_done - first_done
    if o.scan then
        -- Twice the longest pause inside one file's writing, and never less than a tenth.
        local settle = math.max(0.1, math.ceil(gap * 2 * 20) / 20)
        if conv.outputs == "scan" and conv.settle and conv.settle >= settle then
            say('goldberg: learn: %s already says "outputs": "scan" with "settle": %s, which is enough for what was seen (the longest pause in one file was %.2f s). Nothing was changed', conv.file, tostring(conv.settle), gap)
            return self:done(0)
        end
        edits[1] = { path = path, value = '"scan"' }
        edits[2] = { path = { "converters", conv.name, "settle" }, value = string.format("%.2f", settle):gsub("0+$", ""):gsub("%.$", "") }
        what = string.format('"outputs": "scan", "settle": %s', edits[2].value)
    else
        if conv.outputs == "scan" then
            say('goldberg: learn: "%s" says "outputs": "scan", which takes whatever it writes: there is nothing to narrow. Remove that line and run this again to have the names declared. Nothing was changed', conv.name)
            return self:done(0)
        end
        if type(conv.outputs) == "table" then
            -- A declaration that already holds is left alone, however it is spelled.
            local declared = convert.patterns(conv, self.vars, self.answer.params) or {}
            local hit, all = {}, true
            for _, name in ipairs(names) do
                local found = false
                for i, p in ipairs(declared) do if host.glob_path(p, name) then found, hit[i] = true, true end end
                if not found then all = false end
            end
            for i = 1, #declared do if not hit[i] then all = false end end
            if all then
                say('goldberg: learn: %s already declares this: "outputs": %s. Nothing was changed', conv.file, json_list(conv.outputs))
                return self:done(0)
            end
        end
        local patterns = Learn.patterns(names, self.vars.stem)
        edits[1] = { path = path, value = json_list(patterns) }
        what = '"outputs": ' .. edits[1].value
        if spread > 0.5 then
            say('goldberg: learn: its files were finished %.1f s apart. If what follows should start before it ends, `goldberg learn --scan` declares "outputs": "scan" with a settle time instead', spread)
        end
    end

    local text, err = kv.fs.read(self.berg_file)
    if not text then complain("learn: %s", tostring(err)) return self:done(1) end
    local ok, new = pcall(function()
        local t = text
        for _, e in ipairs(edits) do t = bergedit.set(t, e.path, e.value) end
        return t
    end)
    local parsed, bad
    if ok then parsed, bad = berg.parse(new, self.berg_file) end
    if not ok or not parsed then
        complain('learn: could not write that into %s (%s). Declare it by hand: in the converter "%s", %s', conv.file, tostring(ok and bad or new), conv.name, what)
        return self:done(1)
    end
    if o.dry_run then
        say('goldberg: learn: would write into %s, converter "%s": %s (%s). Not written: --dry-run', conv.file, conv.name, what, was)
        return self:done(0)
    end
    if not host.write_atomic(self.berg_file, new) then
        complain("learn: cannot write %s", conv.file)
        return self:done(1)
    end
    say('goldberg: learn: wrote into %s, converter "%s": %s (%s)', conv.file, conv.name, what, was)
    return self:done(0)
end

return Learn
