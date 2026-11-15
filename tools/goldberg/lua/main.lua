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
local Learn = require("learn")

local json = kv.json
local goldberg = {}

local run -- the running cook or watch: { session, mode, opts, log, t0 }
local learning -- or the `goldberg learn` under way

local function say(fmt, ...) host.print(string.format(fmt, ...)) end
local function complain(fmt, ...) host.warn("goldberg: " .. string.format(fmt, ...)) end

local function usage(why)
    complain("%s", why)
    host.finish(2)
end

-- ---- arguments ------------------------------------------------------------------------

local VALUE = { ["--lock-wait"] = true, ["--root"] = true, ["--roots"] = true, ["--out"] = true, ["--state"] = true, ["--project"] = true, ["--log"] = true, ["--dump"] = true, ["--watch"] = true, ["--stamp"] = true, ["--share"] = true }
local FLAG = { ["-v"] = "verbose", ["--quiet"] = "quiet", ["-q"] = "quiet", ["--dry-run"] = "dry_run", ["--scan"] = "scan", ["--keep"] = "keep" }

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
            o[FLAG[w]] = true
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

local function absolute(path)
    if path:sub(1, 1) == "/" or path:match("^%a:[/\\]") then return path end
    return host.cwd() .. "/" .. (path:gsub("^%./", ""))
end


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

-- A roots file, as kormake writes it: { "game", "out", "state", "project", "roots": [ { "name", "dir" } ] }.
local function read_roots_file(o)
    local text, err = kv.fs.read(o.roots_file)
    if not text then return nil, err end
    local doc, why, line = json.decode(text)
    if not doc then return nil, string.format("%s:%d: %s", o.roots_file, line, why) end
    o.game = o.game or doc.game
    o.out = o.out or doc.out
    o.project = o.project or doc.project
    o.state = o.state or doc.state
    for _, r in ipairs(doc.roots or {}) do o.roots[#o.roots + 1] = { name = r.name, dir = r.dir } end
    return true
end

-- ---- the log ----------------------------------------------------------------------------

local function log_start(cfg)
    local roots = {}
    for i, r in ipairs(cfg.roots) do roots[i] = json.object("name", r.name, "dir", r.dir) end
    return json.encode(json.object("kind", "start", "game", cfg.game or json.null, "roots", roots, "conventions", cfg.conventions,
        "project", cfg.project and json.object("text", cfg.project.text, "display", cfg.project.display) or json.null,
        "out_root", cfg.out_root or json.null, "work_root", cfg.work_root or json.null, "run_dir", cfg.run_dir or json.null,
        "state", cfg.state or json.null))
end

-- ---- the output tree -----------------------------------------------------------------------

local MARKER = ".goldberg"
local ATTIC = ".goldberg.attic"

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
        kv.fs.write(out .. "/" .. MARKER, "goldberg's output tree for " .. game .. ". Files here that no source accounts for are moved to " .. ATTIC .. "/ at the next cook.\n")
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
    if ev.code then o.code = ev.code end
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
    cfg.io = { hash = host.hash, scan = host.scan, copy = host.copy, remove_output = host.remove_output,
               run = host.run, clear_work = host.clear_work, remove_work = host.remove_work }
    for _, r in ipairs(cfg.roots) do
        r.abs = absolute(r.dir)
        host.add_root(r.name, r.dir)
    end
    if o.out then
        local ok, err = claim_out(o.out, cfg.game)
        if not ok then return usage(err) end
        -- The output tree is a root that is scanned and never watched.
        cfg.roots[#cfg.roots + 1] = { name = "(out)", dir = o.out }
        cfg.out_root = host.add_root("(out)", o.out, false)
        host.set_out(o.out, os.date("%Y%m%d-%H%M%S"))
        o.state_file = state_path(o)
        -- One goldberg on a tree at a time. Another cook is waited for; a watch is not:
        -- it stays, and it is already cooking what changes.
        local lock = o.state_file:gsub("%.json$", "") .. ".lock"
        kv.fs.mkdir(dirname(lock))
        local waited, limit, told = 0, tonumber(o["lock-wait"]) or 60, false
        while true do
            local got, holder = host.lock(lock, mode)
            if got then break end
            if got == nil then return usage("cannot make the lock file " .. lock) end
            holder = holder or ""
            if holder:match("^watch") and mode == "cook" then
                if not o.quiet then say("goldberg: %s: a goldberg watch has this tree (%s) and cooks what changes; nothing done here", cfg.game, holder) end
                if o.stamp and not kv.fs.is_file(o.stamp) then host.write_atomic(o.stamp, "cooked by a watch\n") end
                host.finish(0)
                return
            end
            if waited >= limit then
                return usage(string.format("another goldberg (%s) has had %s for %d s: not cooked. --lock-wait SECONDS waits longer", holder, o.out, limit))
            end
            if not told and waited >= 1 then
                told = true
                complain("waiting for another goldberg (%s) to finish with %s", holder, o.out)
            end
            host.sleep(0.05)
            waited = waited + 0.05
        end
        cfg.state = kv.fs.read(o.state_file)
        -- Where converters write, beside the state: a root like the others, watched, so
        -- a file a converter makes arrives as any file does.
        local scratch = absolute(o.state_file):gsub("%.json$", "")
        if not kv.fs.mkdir(scratch .. ".work") then return usage("cannot make " .. scratch .. ".work") end
        cfg.roots[#cfg.roots + 1] = { name = "(work)", dir = scratch .. ".work", abs = scratch .. ".work" }
        cfg.work_root = host.add_root("(work)", scratch .. ".work")
        cfg.run_dir = scratch .. ".run"
    end
    host.zone("lua.session new")
    run = { session = Session.new(cfg), mode = mode, opts = o, cfg = cfg, t0 = host.now() }
    host.zone_end()
    -- A watch keeps an eye on the project's goldberg.berg too: one file, looked at a few
    -- times a second, since watching its directory would be watching the whole project.
    if mode == "watch" then
        local dir = o.project or find_project(host.cwd())
        if dir then
            run.project_file = dir .. "/goldberg.berg"
            run.project_size, run.project_time = host.stat(run.project_file)
            run.project_next = host.now() + 0.25
        end
    end
    if o.log then run.log = { log_start(cfg) } end
    -- Watchers first, then the scan: a file written during the scan is not missed, and
    -- the hash drops the duplicate.
    run.backend = host.watch(o.watch or "auto")
    for i = 1, #cfg.roots do run.session:scan(i) end
    host.active(true)
    -- Its share of each tick for its events: a cook takes all of it; a watch, beside
    -- whatever else the machine is doing, 40%, until it is asked to cook.
    run.share = (tonumber(o.share) or (mode == "cook" and 100 or 40)) / 100
    host.share(run.share)
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
        -- Under a build, a cook with nothing to do says nothing.
        local idle = t.cooked == 0 and t.removed == 0 and t.failed == 0 and #t.errors == 0
        if not (run.opts.quiet and final and idle) then say("goldberg: %s: %d files, %d cooked, %d clean, %d failed%s%s (%.0f ms%s)", run.cfg.game, t.files, t.cooked, t.clean, t.failed,
            t.removed > 0 and string.format(", %d to the attic", t.removed) or "", t.warned > 0 and string.format(", %d warned", t.warned) or "", ms,
            final and "" or ", watching with " .. run.backend) end
    else
        say("goldberg: %s: %d files, %d rule files, %d failed, %d warned, nothing cooked: no --out (%.0f ms%s)", run.cfg.game, t.files, t.rules, t.failed, t.warned, ms,
            final and "" or ", watching with " .. run.backend)
    end
    if not final then host.flush() end -- a watch's lines are read as they are said
    return t
end

-- ---- the state ----------------------------------------------------------------------------
-- A cook writes it whole at its end. A watch begins a write each time it is stable and the
-- state changed, and makes it a slice a tick, within its share, so that tens of thousands
-- of files never stop the main thread for the length of their encoding; the file is
-- replaced once the text is complete. A quit finishes a write under way first.

local TICK = 1 / 60

local function wants_state()
    return run.session.graph and run.opts.state_file and (run.session:state_changed() or not kv.fs.is_file(run.opts.state_file))
end

local function put_state(text)
    if host.write_atomic(run.opts.state_file, text) then return end
    complain("cannot write the state to %s", run.opts.state_file)
    run.session.state_dirty = true -- tried again at the next chance
end

-- From here on, what changes is the next write's.
local function begin_state()
    run.session:state_taken()
    return run.session.graph:state_writer()
end

local function state_slice()
    if not run.state_job then return end
    local text = run.state_job(host.now() + run.share * TICK)
    if text then run.state_job = nil; put_state(text) end
end

-- The dump and the log: for tests and for people, written whole.
local function write_debug()
    if run.opts.dump then
        local ok, err = kv.fs.write(run.opts.dump, run.session:dump())
        if not ok then complain("%s", err) end
    end
    if run.log then
        local ok, err = kv.fs.write(run.opts.log, table.concat(run.log, "\n") .. "\n")
        if not ok then complain("%s", err) end
    end
end

-- Everything, now: a write under way finished first, then a whole one if more changed.
local function write_outputs()
    if run.state_job then put_state(run.state_job(nil)); run.state_job = nil end
    if wants_state() then put_state(begin_state()(nil)) end
    write_debug()
end

function goldberg.event(ev)
    if learning then learning:event(ev) return end
    if not run then return end
    if run.log then run.log[#run.log + 1] = log_event(ev) end
    run.session:event(ev)
    run.said_stable = false
end

-- The project's rule file, if it changed: an event like any other, so a log holds it.
local function poll_project()
    if not run.project_file or host.now() < run.project_next then return end
    run.project_next = host.now() + 0.25
    local size, time = host.stat(run.project_file)
    if size == run.project_size and time == run.project_time then return end
    run.project_size, run.project_time = size, time
    local text = size and kv.fs.read(run.project_file) or nil
    goldberg.event{ kind = "project-changed", root = 0, path = run.project_file, text = text }
end

function goldberg.quit()
    if run then
        write_outputs()
        say("goldberg: %s: stopped", run.cfg.game)
        run = nil
    end
    host.finish(0)
end

-- A verb asked of the running session, answered at once: the lines, and a message if it
-- could not be done.
function goldberg.ask(words)
    if not run then return {}, "no session is running here" end
    local s, verb, out = run.session, words[1], {}
    if verb == "status" then
        local t = s:totals()
        local busy = s.graph and s.graph:busy() or { running = {}, waiting = 0 }
        local state = s:stable() and "stable" or "working"
        out[#out + 1] = string.format("goldberg: %s: %s: %d files, %d cooked, %d clean, %d failed, %d warned, %d running, %d waiting (%s, watching with %s)",
            run.cfg.game, state, t.files, t.cooked or 0, t.clean or 0, t.failed, t.warned, #busy.running, busy.waiting + s.hashing, run.mode, run.backend)
        for _, r in ipairs(busy.running) do out[#out + 1] = "  running  " .. r end
        for _, e in ipairs(t.errors) do out[#out + 1] = "  failed   " .. e end
        for _, w in ipairs(t.warnings) do out[#out + 1] = "  warned   " .. w end
        return out
    elseif verb == "cook" then
        -- Everything looked at again, now and with the whole of each tick: every root
        -- scanned (only what changed is read), every step checked against the output
        -- tree, and what no source accounts for taken out, as a cook would.
        if #words > 1 then return out, "cook takes nothing in a session: it looks at every file again" end
        if not s.graph then return out, "this session has no output tree to cook into" end
        s.graph:recheck()
        for i = 1, #s.roots do s:scan(i) end
        run.recook = host.now()
        host.share(1)
        out[#out + 1] = "cooking"
        return out
    elseif verb == "explain" or verb == "touch" then
        if #words < 2 then return out, verb .. " PATH" end
        for i = 2, #words do
            -- A path on disk, or a file's key as a dump prints it ("arcfire:ships/ship.png").
            local arg, f = words[i], s.files[words[i]]
            if not f then
                local full = absolute(arg):gsub("/%./", "/")
                for n, r in ipairs(s.roots) do
                    local dir = (r.abs or r.dir):gsub("/+$", "")
                    if n ~= s.out_root and full:sub(1, #dir + 1) == dir .. "/" then f = s.files[s:key(n, full:sub(#dir + 2))] end
                end
            end
            if not f then return out, arg .. ": not a file this session knows (give its path, or its key as `root:path`)" end
            if verb == "touch" then
                host.touch(f.root, f.path)
                out[#out + 1] = "touched " .. f.key
            elseif f.is_berg then
                local rec = s.rules:get(f.key)
                out[#out + 1] = f.key
                out[#out + 1] = "  a rule file" .. (rec and rec.err and (", broken: " .. rec.err .. (rec.doc and " (its last good version is in force)" or "")) or "")
            elseif not f.answer then
                out[#out + 1] = f.key
                out[#out + 1] = "  not solved yet"
            else
                local eff, prov = s:metadata(f)
                local _, path = s:place(f)
                for _, line in ipairs(explain.lines(path, f.answer, eff, eff and prov or {}, run.opts.out)) do out[#out + 1] = line end
                local step = s.graph and s.graph.steps[f.key]
                if step then out[#out + 1] = string.format("  %-8s%s%s", "step", step.state, step.error and (": " .. step.error) or "") end
            end
        end
        return out
    end
    return out, "not something a running session answers: " .. tostring(verb)
end

function goldberg.tick(quiet)
    if not run then return end
    poll_project()
    state_slice()
    local s = run.session
    if s:has_work() then
        if run.log then run.log[#run.log + 1] = '{"kind": "settle"}' end
        s:advance()
        return -- what that asked for goes out before anything is called stable
    end
    if not (quiet and s:stable()) then return end
    if run.recook then
        say("goldberg: %s: cooked again (%.0f ms)", run.cfg.game, (host.now() - run.recook) * 1000)
        run.recook = nil
        host.share(run.share)
        run.said_stable = false -- and the usual line, below
    end
    if run.mode == "cook" then
        host.zone("lua.report") local t = report(true) host.zone_end()
        host.zone("lua.write outputs") write_outputs() host.zone_end()
        local failed = t.failed > 0 or #t.errors > 0
        -- For a build: a file that changes only when the output tree did.
        if run.opts.stamp and not failed and (t.cooked > 0 or t.removed > 0 or not kv.fs.is_file(run.opts.stamp)) then
            host.write_atomic(run.opts.stamp, "cooked " .. os.date("%Y-%m-%d %H:%M:%S") .. "\n")
        end
        -- The escape hatch is not a place to forget things in.
        if run.opts.out and kv.fs.is_dir(run.opts.out .. "/" .. ATTIC) then
            complain("goldberg found files in the attic: %s/%s (what it took out of the output tree; `goldberg clean attic --out %s` empties it)", run.opts.out, ATTIC, run.opts.out)
        end
        host.finish((t.failed > 0 or #t.errors > 0) and 1 or 0)
        run = nil
    elseif not run.said_stable then
        report(false)
        if not run.state_job and wants_state() then run.state_job = begin_state() end
        write_debug()
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
                                   work_root = rec.work_root ~= json.null and rec.work_root or nil,
                                   run_dir = rec.run_dir ~= json.null and rec.run_dir or nil,
                                   state = rec.state ~= json.null and rec.state or nil,
                                   io = { hash = function() end, scan = function() end, copy = function() end, remove_output = function() end,
                                          run = function() end, clear_work = function() end, remove_work = function() end } }
            for i = 1, #roots do session:scan(i) end
        elseif not session then
            return usage(path .. ": the log does not begin with a start record")
        elseif rec.kind == "settle" then
            session:advance()
        else
            session:event{ kind = rec.kind, root = rec.root, path = rec.path, size = rec.size, mtime = rec.mtime, hash = rec.hash, text = rec.text, code = rec.code }
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

-- Where a file given on the command line is: its asset root (given with --root, or the
-- nearest `assets` directory above it that sits beside a module.lua) and its path under
-- it. { name, dir (the root as the user would write it), abs (the root, absolute), rel },
-- or nil and why.
local function locate(arg, o)
    local full = absolute(arg):gsub("/%./", "/")
    if not kv.fs.is_file(full) then return nil, arg .. ": no such file" end
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
        if not root_name then return nil, arg .. ": not under an asset root. Give --root NAME=DIR, or keep it under a module's assets/" end
        root_dir = root_dir:gsub("/+$", "")
    end
    return { name = root_name, dir = root_dir, abs = full:sub(1, #full - #rel - 1), rel = rel }
end

-- The few rule files that speak for one located file, read now.
local function rules_for(loc, o)
    local rules = Rules.new()
    rules:set("conventions", read_conventions(), "conventions")
    local project = read_project{ project = o.project or find_project(loc.abs) }
    if project then rules:set("project", project.text, project.display) end
    for _, layer in ipairs(Rules.chain(loc.name, loc.rel)) do
        local file = layer.key:match("^[^:]*:(.*)$")
        if file and layer.key ~= "conventions" and layer.key ~= "project" then
            local text = kv.fs.read(loc.abs .. "/" .. file)
            if text then rules:set(layer.key, text, loc.dir .. "/" .. file) end
        end
    end
    return rules
end

-- Without a session: finds the file's asset root, reads the rule files that speak for
-- it, and solves it. Nothing is cooked and nothing is watched.
local function explain_now(words)
    local o, why = parse(words)
    if not o then return usage(why) end
    if #o.names == 0 then return usage("explain PATH [--root NAME=DIR]") end
    local failed = false
    local explained = {}
    for _, arg in ipairs(o.names) do
        local loc, where = locate(arg, o)
        if not loc then
            complain("%s", where)
            failed = true
        else
            local eff, prov, reg, _, warn = rules_for(loc, o):effective(loc.name, loc.rel)
            local a
            if not eff then a = { action = "error", error = prov, why = {} }
            else a = solve.solve(loc.name, loc.rel, eff, prov, reg); a.rule_warn = warn end
            for _, line in ipairs(explain.lines(loc.rel, a, eff, prov or {}, o.out)) do
                say("%s", line)
                explained[#explained + 1] = line
            end
            if a.error then failed = true end
        end
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
    elseif verb == "learn" then
        if run or learning then complain("goldberg is busy") return end
        local o, why = parse(words)
        if not o then return usage(why) end
        if #o.names ~= 1 then return usage("learn FILE [--root NAME=DIR] [--dry-run] [--scan] [--keep]: FILE is one source a shell converter takes") end
        local loc, where = locate(o.names[1], o)
        if not loc then return usage(where) end
        learning = Learn.start(loc, rules_for(loc, o), o)
    elseif verb == "replay" then replay(words)
    elseif verb == "clean" then
        local o, why = parse(words)
        if not o then return usage(why) end
        if not o.out then return usage("clean [attic] --out DIR [--state FILE]") end
        if o.names[1] == "attic" then
            local attic = o.out .. "/" .. ATTIC
            if not kv.fs.is_file(o.out .. "/" .. MARKER) then return usage(o.out .. " is not a goldberg output tree (no " .. MARKER .. " in it)") end
            if kv.fs.is_dir(attic) then kv.fs.remove_tree(attic); say("goldberg: emptied the attic of %s", o.out)
            else say("goldberg: %s has no attic", o.out) end
            host.finish(0)
            return
        end
        if kv.fs.is_dir(o.out) then
            if not kv.fs.is_file(o.out .. "/" .. MARKER) then return usage(o.out .. " is not a goldberg output tree (no " .. MARKER .. " in it): not removed") end
            kv.fs.remove_tree(o.out)
        end
        local state = state_path(o)
        kv.fs.remove(state)
        -- And what converters made on the way: scratch, beside the state.
        local scratch = state:gsub("%.json$", "")
        if kv.fs.is_dir(scratch .. ".work") then kv.fs.remove_tree(scratch .. ".work") end
        if kv.fs.is_dir(scratch .. ".run") then kv.fs.remove_tree(scratch .. ".run") end
        say("goldberg: removed %s and its state", o.out)
        host.finish(0)
    elseif verb == "status" then
        say("goldberg: no session is running here. status is asked of a running `goldberg watch`, on its stdin or its --cli-port")
        host.finish(0)
    else
        usage("unknown command " .. tostring(verb))
    end
end

return goldberg
