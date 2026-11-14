-- goldberg, the application. The host (src/goldberg.cc) calls three functions:
--
--   goldberg.command(words)   a verb and its arguments, from the command line or the CLI
--   goldberg.event(ev)        one event, in sequence order
--   goldberg.tick(quiet)      once a tick; quiet: the host has nothing left to deliver
--
-- A cook and a watch are the same thing here: a session fed by the scan and the
-- watchers until it is stable. A cook then finishes; a watch says so and keeps going.
local Session = require("session")
local Rules = require("rules")
local solve = require("solve")
local explain = require("explain")

local json = kv.json
local goldberg = {}

local run -- the running cook or watch: { session, mode, opts, log, t0 }

local function say(fmt, ...) host.print(string.format(fmt, ...)) end
local function complain(fmt, ...) host.warn("goldberg: " .. string.format(fmt, ...)) end

local function usage(why)
    complain("%s", why)
    host.finish(2)
end

-- ---- arguments ------------------------------------------------------------------------

local VALUE = { ["--root"] = true, ["--roots"] = true, ["--out"] = true, ["--state"] = true, ["--project"] = true, ["--log"] = true, ["--dump"] = true, ["--watch"] = true }
local FLAG = { ["-v"] = true }

local function parse(words)
    local o = { roots = {}, names = {} }
    local i = 2
    while i <= #words do
        local w = words[i]
        if VALUE[w] then
            local v = words[i + 1]
            if not v then return nil, w .. " needs a value" end
            if w == "--root" then
                local name, dir = v:match("^([^=]+)=(.+)$")
                if not name then return nil, "--root takes NAME=DIR" end
                o.roots[#o.roots + 1] = { name = name, dir = dir }
            elseif w == "--roots" then
                o.roots_file = v
            else
                o[w:sub(3)] = v
            end
            i = i + 2
        elseif FLAG[w] then
            o.verbose = true
            i = i + 1
        elseif w:sub(1, 1) == "-" then
            return nil, "unknown option " .. w
        else
            o.names[#o.names + 1] = w
            i = i + 1
        end
    end
    return o
end

local function dirname(path) return path:match("^(.*)/[^/]*$") or "" end

-- The project's directory: the nearest one at or above `from` that holds korvid.config
-- or kormake.lua.
local function find_project(from)
    local dir = from
    while dir and dir ~= "" do
        if kv.fs.is_file(dir .. "/korvid.config") or kv.fs.is_file(dir .. "/kormake.lua") then return dir end
        local up = dirname(dir)
        if up == dir then break end
        dir = up
    end
    return nil
end

local function read_conventions()
    local path = host.lua_dir .. "/../conventions.berg"
    local text, err = kv.fs.read(path)
    if not text then error("the conventions are missing: " .. tostring(err)) end
    return text
end

local function read_project(o)
    local dir = o.project or find_project(host.cwd())
    if not dir then return nil end
    local path = dir .. "/goldberg.berg"
    local text = kv.fs.read(path)
    if not text then return nil end
    return { text = text, display = path }
end

-- A roots file, as kormake writes it: { "game", "out", "project", "roots": [ { "name", "dir" } ] }.
local function read_roots_file(o)
    local text, err = kv.fs.read(o.roots_file)
    if not text then return nil, err end
    local doc, why, line = json.decode(text)
    if not doc then return nil, string.format("%s:%d: %s", o.roots_file, line, why) end
    o.game = o.game or doc.game
    o.out = o.out or doc.out
    o.project = o.project or doc.project
    for _, r in ipairs(doc.roots or {}) do o.roots[#o.roots + 1] = { name = r.name, dir = r.dir } end
    return true
end

-- ---- the log ----------------------------------------------------------------------------

local function log_start(cfg)
    local roots = {}
    for i, r in ipairs(cfg.roots) do roots[i] = json.object("name", r.name, "dir", r.dir) end
    return json.encode(json.object("kind", "start", "game", cfg.game or json.null, "roots", roots, "conventions", cfg.conventions,
        "project", cfg.project and json.object("text", cfg.project.text, "display", cfg.project.display) or json.null,
        "out_root", cfg.out_root or json.null, "state", cfg.state or json.null))
end

-- ---- the output tree -----------------------------------------------------------------------

local MARKER = ".goldberg"

-- The output tree is goldberg's: what no source accounts for is removed from it. So it
-- only ever takes a directory that is new, empty, or already marked as one of its own.
local function claim_out(out, game)
    if kv.fs.is_dir(out) and not kv.fs.is_file(out .. "/" .. MARKER) then
        local there = kv.fs.list(out)
        if there and #there > 0 then
            return nil, string.format("%s holds files and is not a goldberg output tree (no %s in it). goldberg removes what it did not cook from its output tree, so it will not take this one", out, MARKER)
        end
    end
    if not kv.fs.mkdir(out) then return nil, "cannot make " .. out end
    if not kv.fs.is_file(out .. "/" .. MARKER) then
        kv.fs.write(out .. "/" .. MARKER, "goldberg's output tree for " .. game .. ". Files here that no source accounts for are removed at the next cook.\n")
    end
    return true
end

local function state_path(o)
    if o.state then return o.state end
    local out = o.out:gsub("/+$", "")
    local dir, name = out:match("^(.*)/([^/]*)$")
    if not dir then dir, name = ".", out end
    return dir .. "/.goldberg-state/" .. name .. ".json"
end

local function log_event(ev)
    local o = json.object("kind", ev.kind, "root", ev.root, "path", ev.path)
    if ev.size then o.size = ev.size end
    if ev.mtime then o.mtime = ev.mtime end
    if ev.hash then o.hash = ev.hash end
    if ev.text then o.text = ev.text end
    return json.encode(o)
end

-- ---- cook and watch -----------------------------------------------------------------------

local function start(mode, words)
    local o, why = parse(words)
    if not o then return usage(why) end
    o.game = o.names[1]
    o.config = o.names[2]
    if o.roots_file then
        local ok, err = read_roots_file(o)
        if not ok then return usage(err) end
    end
    if #o.roots == 0 then
        return usage(mode .. ": no asset roots. Give --root NAME=DIR for each, or --roots FILE (kormake writes one)")
    end
    for _, r in ipairs(o.roots) do
        if not kv.fs.is_dir(r.dir) then return usage(string.format("the asset root '%s' is not a directory: %s", r.name, r.dir)) end
    end

    local cfg = { game = o.game or o.roots[1].name, roots = o.roots, conventions = read_conventions(), project = read_project(o) }
    cfg.io = { hash = host.hash, scan = host.scan, copy = host.copy, remove_output = host.remove_output }
    for _, r in ipairs(cfg.roots) do host.add_root(r.name, r.dir) end
    if o.out then
        local ok, err = claim_out(o.out, cfg.game)
        if not ok then return usage(err) end
        -- The output tree is a root that is scanned and never watched.
        cfg.roots[#cfg.roots + 1] = { name = "(out)", dir = o.out }
        cfg.out_root = host.add_root("(out)", o.out, false)
        host.set_out(o.out)
        o.state_file = state_path(o)
        cfg.state = kv.fs.read(o.state_file)
    end
    run = { session = Session.new(cfg), mode = mode, opts = o, cfg = cfg, t0 = host.now() }
    if o.log then run.log = { log_start(cfg) } end
    -- Watchers first, then the scan: a file written during the scan is not missed, and
    -- the hash drops the duplicate.
    run.backend = host.watch(o.watch or "auto")
    for i = 1, #cfg.roots do run.session:scan(i) end
    host.active(true)
end

local function report(final)
    local s = run.session
    local t = s:totals()
    for _, e in ipairs(t.errors) do complain("%s", e) end
    if run.opts.verbose then for _, w in ipairs(t.warnings) do complain("%s", w) end end
    local ms = (host.now() - run.t0) * 1000
    if s.graph then
        for _, n in ipairs(s.graph.notes) do say("goldberg: %s", n) end
        s.graph.notes = {}
        say("goldberg: %s: %d files, %d cooked, %d clean, %d failed%s%s (%.0f ms%s)", run.cfg.game, t.files, t.cooked, t.clean, t.failed,
            t.removed > 0 and string.format(", %d removed", t.removed) or "", t.warned > 0 and string.format(", %d warned", t.warned) or "", ms,
            final and "" or ", watching with " .. run.backend)
    else
        say("goldberg: %s: %d files, %d rule files, %d failed, %d warned, nothing cooked: no --out (%.0f ms%s)", run.cfg.game, t.files, t.rules, t.failed, t.warned, ms,
            final and "" or ", watching with " .. run.backend)
    end
    return t
end

local function write_outputs()
    if run.session.graph and run.opts.state_file then
        if not host.write_atomic(run.opts.state_file, run.session.graph:state_text()) then complain("cannot write the state to %s", run.opts.state_file) end
    end
    if run.opts.dump then
        local ok, err = kv.fs.write(run.opts.dump, run.session:dump())
        if not ok then complain("%s", err) end
    end
    if run.log then
        local ok, err = kv.fs.write(run.opts.log, table.concat(run.log, "\n") .. "\n")
        if not ok then complain("%s", err) end
    end
end

function goldberg.event(ev)
    if not run then return end
    if run.log then run.log[#run.log + 1] = log_event(ev) end
    run.session:event(ev)
    run.said_stable = false
end

function goldberg.tick(quiet)
    if not run then return end
    local s = run.session
    if s:has_work() then
        if run.log then run.log[#run.log + 1] = '{"kind": "settle"}' end
        s:advance()
        return -- what that asked for goes out before anything is called stable
    end
    if not (quiet and s:stable()) then return end
    if run.mode == "cook" then
        local t = report(true)
        write_outputs()
        host.finish((t.failed > 0 or #t.errors > 0) and 1 or 0)
        run = nil
    elseif not run.said_stable then
        report(false)
        write_outputs()
        run.said_stable = true
    end
end

-- ---- replay ---------------------------------------------------------------------------------

-- Feeds a log to a fresh session whose host does nothing, settling where the recorded
-- session did. What comes out depends on the log alone.
local function replay(words)
    local o, why = parse(words)
    if not o then return usage(why) end
    local path = o.names[1]
    if not path then return usage("replay LOG [--dump FILE]") end
    local text, err = kv.fs.read(path)
    if not text then return usage(err) end
    local session
    local n = 0
    for line in text:gmatch("[^\n]+") do
        n = n + 1
        local rec, bad = json.decode(line)
        if not rec then return usage(string.format("%s:%d: %s", path, n, bad)) end
        if rec.kind == "start" then
            local roots = {}
            for i, r in ipairs(rec.roots) do roots[i] = { name = r.name, dir = r.dir } end
            local project = rec.project ~= json.null and { text = rec.project.text, display = rec.project.display } or nil
            session = Session.new{ game = rec.game ~= json.null and rec.game or nil, roots = roots, conventions = rec.conventions, project = project,
                                   out_root = rec.out_root ~= json.null and rec.out_root or nil,
                                   state = rec.state ~= json.null and rec.state or nil,
                                   io = { hash = function() end, scan = function() end, copy = function() end, remove_output = function() end } }
            for i = 1, #roots do session:scan(i) end
        elseif not session then
            return usage(path .. ": the log does not begin with a start record")
        elseif rec.kind == "settle" then
            session:advance()
        else
            session:event{ kind = rec.kind, root = rec.root, path = rec.path, size = rec.size, mtime = rec.mtime, hash = rec.hash, text = rec.text }
        end
    end
    if not session then return usage(path .. ": an empty log") end
    local t = session:totals()
    say("goldberg: replay: %d events, %d files, %d rule files, %d failed, %d warned", n - 1, t.files, t.rules, t.failed, t.warned)
    if o.dump then
        local ok, werr = kv.fs.write(o.dump, session:dump())
        if not ok then complain("%s", werr) end
    end
    host.finish(session:stable() and 0 or 1)
end

-- ---- explain ----------------------------------------------------------------------------------

local function absolute(path)
    if path:sub(1, 1) == "/" or path:match("^%a:[/\\]") then return path end
    return host.cwd() .. "/" .. path
end

-- Without a session: finds the file's asset root (given with --root, or the nearest
-- `assets` directory above it that sits beside a module.lua), reads the few rule files
-- that speak for it, and solves it. Nothing is cooked and nothing is watched.
local function explain_now(words)
    local o, why = parse(words)
    if not o then return usage(why) end
    if #o.names == 0 then return usage("explain PATH [--root NAME=DIR]") end
    local failed = false
    local explained = {}
    for _, arg in ipairs(o.names) do
        local full = absolute(arg):gsub("/%./", "/")
        if not kv.fs.is_file(full) then
            complain("%s: no such file", arg)
            failed = true
            goto continue
        end
        do
            local root_name, root_dir, rel
            for _, r in ipairs(o.roots) do
                local dir = absolute(r.dir):gsub("/+$", "")
                if full:sub(1, #dir + 1) == dir .. "/" then root_name, root_dir, rel = r.name, r.dir:gsub("/+$", ""), full:sub(#dir + 2) end
            end
            if not root_name then
                local dir = dirname(full)
                while dir ~= "" do
                    if Rules.basename(dir) == "assets" and kv.fs.is_file(dirname(dir) .. "/module.lua") then
                        local text = kv.fs.read(dirname(dir) .. "/module.lua") or ""
                        root_name = text:match('module%s*"([^"]+)"') or Rules.basename(dirname(dir))
                        rel = full:sub(#dir + 2)
                        -- Shown as the user wrote it: the argument, less the part under the root.
                        root_dir = arg:sub(-#rel) == rel and #arg > #rel and arg:sub(1, #arg - #rel - 1) or dir
                        break
                    end
                    dir = dirname(dir)
                end
                if not root_name then
                    complain("%s: not under an asset root. Give --root NAME=DIR, or keep it under a module's assets/", arg)
                    failed = true
                    goto continue
                end
                root_dir = root_dir:gsub("/+$", "")
            end
            local abs_root = full:sub(1, #full - #rel - 1)
            local rules = Rules.new()
            rules:set("conventions", read_conventions(), "conventions")
            local project = read_project{ project = o.project or find_project(abs_root) }
            if project then rules:set("project", project.text, project.display) end
            for _, layer in ipairs(Rules.chain(root_name, rel)) do
                local file = layer.key:match("^[^:]*:(.*)$")
                if file and layer.key ~= "conventions" and layer.key ~= "project" then
                    local text = kv.fs.read(abs_root .. "/" .. file)
                    if text then rules:set(layer.key, text, root_dir .. "/" .. file) end
                end
            end
            local eff, prov, reg, _, warn = rules:effective(root_name, rel)
            local a
            if not eff then a = { action = "error", error = prov, why = {} }
            else a = solve.solve(root_name, rel, eff, prov, reg); a.rule_warn = warn end
            for _, line in ipairs(explain.lines(rel, a, eff, prov or {}, o.out)) do
                say("%s", line)
                explained[#explained + 1] = line
            end
            if a.error then failed = true end
        end
        ::continue::
    end
    if o.dump then kv.fs.write(o.dump, table.concat(explained, "\n") .. "\n") end
    host.finish(failed and 1 or 0)
end

-- ---- commands -----------------------------------------------------------------------------------

function goldberg.command(words)
    local verb = words[1]
    if verb == "cook" or verb == "watch" then
        if run then complain("a %s is already running", run.mode) return end
        start(verb, words)
    elseif verb == "explain" then explain_now(words)
    elseif verb == "replay" then replay(words)
    elseif verb == "clean" then
        local o, why = parse(words)
        if not o then return usage(why) end
        if not o.out then return usage("clean --out DIR [--state FILE]") end
        if kv.fs.is_dir(o.out) then
            if not kv.fs.is_file(o.out .. "/" .. MARKER) then return usage(o.out .. " is not a goldberg output tree (no " .. MARKER .. " in it): not removed") end
            kv.fs.remove_tree(o.out)
        end
        kv.fs.remove(state_path(o))
        say("goldberg: removed %s and its state", o.out)
        host.finish(0)
    elseif verb == "status" then
        if not run then say("goldberg: no session is running here") host.finish(0) return end
        report(false)
    else
        usage("unknown command " .. tostring(verb))
    end
end

return goldberg
