-- A rule file as a form (docs/PRDs/Krome-Slice-6-PRD.md, "goldberg's window": Rules and Diff):
-- its values as entries, edits made into its new text by bergedit (the file's comments,
-- spacing and order kept), and what an edit would change, before it is saved: every file the
-- rule file speaks for solved again with the new text in place, and nothing in the session
-- touched. The window's Rules and Diff pages and the CLI's `berg`, `berg-preview` and
-- `berg-set` are this.
local berg = require("berg")
local bergedit = require("bergedit")
local Rules = require("rules")
local solve = require("solve")

local M = {}

local function encode(v) return kv.json.encode(v) end

-- The rule file `key` ("<root>:<path>"): { key, display, text, entries }, each entry
-- { id, scope ("meta" or a rule's number), label, key, value (its JSON text), path }; or nil
-- and why. A file whose version on disk does not parse shows its last good one.
function M.read(session, key)
    local f = session.files[key]
    local rec = session.rules:get(key)
    if not f or not f.is_berg then return nil, key .. ": not a rule file this session knows" end
    if not rec or not rec.text then return nil, key .. ": it has never parsed" .. (rec and rec.err and (": " .. rec.err) or "") end
    local doc = kv.json.decode(rec.text, { comments = true, drop = "//" })
    local form = { key = key, display = rec.display, text = rec.text, entries = {}, broken = rec.err }
    local function add(scope, label, k, v, path)
        form.entries[#form.entries + 1] = { id = "e" .. (#form.entries + 1), scope = tostring(scope), label = label, key = k, value = encode(v), path = path }
    end
    if doc.meta and kv.json.is_object(doc.meta) then
        for _, k in ipairs(kv.json.keys(doc.meta)) do add("meta", "meta", k, doc.meta[k], { "meta", k }) end
    end
    for i, r in ipairs(doc.rules or {}) do
        local label = 'rule ' .. i .. ' "' .. tostring(r.match) .. '"'
        add(i, label, "match", r.match, { "rules", i, "match" })
        if kv.json.is_object(r.meta) then
            for _, k in ipairs(kv.json.keys(r.meta)) do add(i, label, k, r.meta[k], { "rules", i, "meta", k }) end
        end
    end
    return form
end

-- The form's text with `edits` made: { { path, value } }, each value JSON text. nil and why when
-- a value is not JSON, or the result is not a rule file.
function M.apply(form, edits)
    local text = form.text
    for _, e in ipairs(edits) do
        local ok, why = pcall(kv.json.decode, e.value)
        if not ok or (why == nil and e.value ~= "null") then return nil, string.format("%s: '%s' is not a JSON value", table.concat(e.path, "."), e.value) end
        local done, new = pcall(bergedit.set, text, e.path, e.value)
        if not done then return nil, new end
        text = new
    end
    local doc, err = berg.parse(text, form.display)
    if not doc then return nil, err end
    return text
end

-- The files whose answer `text` would change, in place of what rule file `key` says now:
-- { { key, field, old, new } }, a row for each field that differs (type, path, lands, output,
-- a parameter), sorted. Nothing in the session changes.
function M.preview(session, key, text)
    local rec = session.rules:get(key)
    if not rec then return nil, key .. ": not a rule file this session knows" end
    local doc, err = berg.parse(text, rec.display)
    if not doc then return nil, err end
    local root = key:match("^(.-):")
    local was_doc, was_err = rec.doc, rec.err
    rec.doc, rec.err = doc, nil
    local rows = {}
    local ok, why = pcall(function()
        local keys = {}
        for fkey, f in pairs(session.files) do
            if not f.is_berg and not f.work and f.answer then
                local name, path = session:place(f)
                if name == root and Rules.speaks_for(key, name, path) then keys[#keys + 1] = fkey end
            end
        end
        table.sort(keys)
        for _, fkey in ipairs(keys) do
            local f = session.files[fkey]
            local name, path = session:place(f)
            local eff, prov, reg = session.rules:effective(name, path)
            local a = eff and solve.solve(name, path, eff, prov, reg) or { action = "error", error = prov, why = {} }
            if solve.line(a) ~= f.line then
                local old = f.answer
                local function show(x, field)
                    if field == "path" then return x.path and table.concat(x.path, " -> ") or "" end
                    if field == "lands" then return x.lands == nil and "" or (x.lands and "yes" or "no") end
                    if field == "params" then
                        local parts, ks = {}, {}
                        for k in pairs(x.params or {}) do ks[#ks + 1] = k end
                        table.sort(ks)
                        for _, k in ipairs(ks) do parts[#parts + 1] = k .. "=" .. berg.show(x.params[k]) end
                        return table.concat(parts, " ")
                    end
                    if field == "error" then return x.error or "" end
                    return tostring(x[field] or "")
                end
                for _, field in ipairs({ "action", "type", "path", "lands", "output", "params", "error" }) do
                    local o, n = show(old, field), show(a, field)
                    if o ~= n then rows[#rows + 1] = { key = fkey, field = field, old = o, new = n } end
                end
            end
        end
    end)
    rec.doc, rec.err = was_doc, was_err
    if not ok then return nil, why end
    return rows
end

-- The new text written over the file (whole, renamed into place); the watch sees the save as
-- it sees any, and cooks what it changes. True, or nil and why.
function M.save(session, key, text)
    local f = session.files[key]
    if not f then return nil, key .. ": not a file this session knows" end
    local path = session:root_dir(f.root) .. "/" .. f.path
    if not host.write_atomic(path, text) then return nil, "cannot write " .. path end
    return true
end

-- "rules 2 meta filter" or "meta tags" or "rules 1 match": a CLI's path into its list form.
function M.path_of(words, from)
    local path = {}
    for i = from, #words do
        local w = words[i]
        path[#path + 1] = tonumber(w) and math.tointeger(tonumber(w)) or w
    end
    return path
end

return M
