-- .berg files: reading one, and laying several over each other.
--
-- A .berg file is one JSON object. `meta` speaks for the file's whole subject; `rules`
-- narrow it by pattern; `types` and `converters` declare what the solver may use. `//`
-- comments are allowed and a "//" member is dropped wherever it appears.
--
-- parse() turns the text into a plain record or says what is wrong and on which line.
-- apply() lays one record's metadata for one file over what the layers before it said,
-- remembering for every key who said it last and whom that overrode.
local M = {}

local json = kv.json
M.null = json.null

-- Marks a table that was a JSON object, so merging can tell it from a list.
local OBJECT = {}
function M.is_object(v) return type(v) == "table" and getmetatable(v) == OBJECT end

-- A decoded JSON value as plain Lua: objects become marked tables (key order is not kept:
-- whoever needs an order sorts), lists stay lists, null stays json.null.
local function plain(v)
    if json.is_object(v) then
        local t = setmetatable({}, OBJECT)
        for k, x in pairs(v) do t[k] = plain(x) end
        return t
    elseif type(v) == "table" and v ~= json.null then
        local t = {}
        for i, x in ipairs(v) do t[i] = plain(x) end
        return t
    end
    return v
end

local function copy(v)
    if type(v) ~= "table" or v == json.null then return v end
    local t = M.is_object(v) and setmetatable({}, OBJECT) or {}
    for k, x in pairs(v) do t[k] = copy(x) end
    return t
end
M.copy = copy

local function fail(name, line, fmt, ...)
    return nil, string.format("%s:%d: " .. fmt, name, line or 1, ...)
end

-- A `meta` object as a list of { key, value, line } in the order written.
local function meta_entries(name, obj, line)
    if not json.is_object(obj) then return fail(name, line, '"meta" takes an object') end
    local out = {}
    for _, key in ipairs(json.keys(obj)) do
        out[#out + 1] = { key = key, value = plain(obj[key]), line = json.line(obj, key) }
    end
    return out
end

local TOP = { meta = true, rules = true, types = true, converters = true }
local RULE = { match = true, meta = true }
local TYPE = { runtime = true, to = true }
local CONVERTER = { from = true, to = true, shell = true, exec = true, builtin = true, args = true, outputs = true, settle = true }

-- parse(text, name): the record, or nil and "name:line: what is wrong".
--   { meta = entries or nil, rules = { { match, meta = entries, line } },
--     types = { [name] = { runtime, to, line } }, converters = { [name] = { ..., line } } }
function M.parse(text, name)
    local doc, err, line = json.decode(text, { comments = true, drop = "//" })
    if doc == nil then return fail(name, line, "%s", err) end
    if not json.is_object(doc) then return fail(name, 1, "a .berg file is one object") end
    local rec = { rules = {}, types = {}, converters = {} }
    for _, key in ipairs(json.keys(doc)) do
        if not TOP[key] then
            return fail(name, json.line(doc, key), 'unknown key "%s" (a .berg file holds "meta", "rules", "types" and "converters")', key)
        end
    end
    if doc.meta ~= nil then
        local e, why = meta_entries(name, doc.meta, json.line(doc, "meta"))
        if not e then return nil, why end
        rec.meta = e
    end
    if doc.rules ~= nil then
        local rules = doc.rules
        if type(rules) ~= "table" or json.is_object(rules) or rules == json.null then return fail(name, json.line(doc, "rules"), '"rules" takes a list') end
        for i, r in ipairs(rules) do
            local rline = json.line(rules, i)
            if not json.is_object(r) then return fail(name, rline, "a rule is an object with \"match\" and \"meta\"") end
            for _, key in ipairs(json.keys(r)) do
                if not RULE[key] then return fail(name, json.line(r, key), 'a rule has no "%s" (it holds "match" and "meta")', key) end
            end
            if type(r.match) ~= "string" or r.match == "" then return fail(name, rline, 'a rule needs a "match" pattern') end
            if r.meta == nil then return fail(name, rline, 'the rule "%s" has no "meta"', r.match) end
            local e, why = meta_entries(name, r.meta, json.line(r, "meta"))
            if not e then return nil, why end
            rec.rules[#rec.rules + 1] = { match = r.match, meta = e, line = rline }
        end
    end
    if doc.types ~= nil then
        if not json.is_object(doc.types) then return fail(name, json.line(doc, "types"), '"types" takes an object') end
        for _, tname in ipairs(json.keys(doc.types)) do
            local t, tline = doc.types[tname], json.line(doc.types, tname)
            if not json.is_object(t) then return fail(name, tline, 'the type "%s" takes an object', tname) end
            for _, key in ipairs(json.keys(t)) do
                if not TYPE[key] then return fail(name, json.line(t, key), 'the type "%s" has no "%s" (it holds "runtime" and "to")', tname, key) end
            end
            if t.runtime ~= nil and type(t.runtime) ~= "boolean" then return fail(name, tline, 'the type "%s": "runtime" is true or false', tname) end
            if t.to ~= nil and type(t.to) ~= "string" then return fail(name, tline, 'the type "%s": "to" names a type', tname) end
            rec.types[tname] = { runtime = t.runtime == true, to = t.to, line = tline }
        end
    end
    if doc.converters ~= nil then
        if not json.is_object(doc.converters) then return fail(name, json.line(doc, "converters"), '"converters" takes an object') end
        for _, cname in ipairs(json.keys(doc.converters)) do
            local c, cline = doc.converters[cname], json.line(doc.converters, cname)
            if not json.is_object(c) then return fail(name, cline, 'the converter "%s" takes an object', cname) end
            for _, key in ipairs(json.keys(c)) do
                if not CONVERTER[key] then return fail(name, json.line(c, key), 'the converter "%s" has no "%s"', cname, key) end
            end
            if cname == "copy" then return fail(name, cline, '"copy" is built in and cannot be registered') end
            if type(c.from) ~= "string" or type(c.to) ~= "string" then return fail(name, cline, 'the converter "%s" needs "from" and "to" types', cname) end
            -- "shell": a script, run by sh (or cmd, for a .bat on Windows). "exec": a program,
            -- run directly, with nothing between it and goldberg. One or the other.
            -- "builtin": one goldberg runs itself, in process ("kim"). One of the three.
            local ways = (c.shell ~= nil and 1 or 0) + (c.exec ~= nil and 1 or 0) + (c.builtin ~= nil and 1 or 0)
            if ways ~= 1 then return fail(name, cline, 'the converter "%s" needs one of "shell" (the script it runs, without its extension), "exec" (the program it runs) or "builtin" (one goldberg has)', cname) end
            if c.builtin ~= nil and (type(c.builtin) ~= "string" or not (host.builtins or {})[c.builtin]) then
                local names = {}
                for k in pairs(host.builtins or {}) do names[#names + 1] = k end
                table.sort(names)
                return fail(name, cline, 'the converter "%s": "builtin" names one goldberg has (%s)', cname, table.concat(names, ", "))
            end
            if c.builtin ~= nil and c.args ~= nil then return fail(name, cline, 'the converter "%s": a built-in converter takes no "args"', cname) end
            if c.shell ~= nil and type(c.shell) ~= "string" then return fail(name, cline, 'the converter "%s": "shell" is the script it runs, without its extension', cname) end
            if c.exec ~= nil and (type(c.exec) ~= "string" or c.exec == "") then return fail(name, cline, 'the converter "%s": "exec" is the program it runs: a path under its .berg file, a name on PATH, or an absolute path', cname) end
            if c.args ~= nil and type(c.args) ~= "string" then return fail(name, cline, 'the converter "%s": "args" is one string', cname) end
            if c.settle ~= nil and (type(c.settle) ~= "number" or c.settle < 0) then return fail(name, cline, 'the converter "%s": "settle" is a number of seconds', cname) end
            -- "outputs" may be left out: everything the script writes is then taken in at
            -- its exit, with a line saying so. `goldberg learn` writes the declaration.
            local outputs = c.outputs
            if outputs == "scan" then
                if c.settle == nil then return fail(name, cline, 'the converter "%s": "outputs": "scan" needs a "settle" time', cname) end
            elseif type(outputs) == "table" and not json.is_object(outputs) and outputs ~= json.null then
                outputs = plain(outputs)
                for _, o in ipairs(outputs) do
                    if type(o) ~= "string" then return fail(name, cline, 'the converter "%s": "outputs" is a list of names or patterns, or "scan"', cname) end
                end
            elseif outputs ~= nil then
                return fail(name, cline, 'the converter "%s": "outputs" is a list of names or patterns, or "scan"', cname)
            end
            rec.converters[cname] = { from = c.from, to = c.to, shell = c.shell, exec = c.exec, builtin = c.builtin, args = c.args, outputs = outputs, settle = c.settle, line = cline }
        end
    end
    return rec
end

-- Lays `entries` over eff. `src` is { file, where }; every key written gets
-- prov[key] = { file, where, line, over = whoever had it before }.
function M.apply(eff, prov, entries, src)
    for _, e in ipairs(entries) do
        local key, value = e.key, e.value
        local p = { file = src.file, where = src.where, line = e.line, over = prov[key] }
        if value == json.null then
            eff[key] = nil
            p.removed = true
        elseif key == "tags" then
            -- A set: names add, "-name" takes one away.
            local set, order = {}, {}
            for _, t in ipairs(eff.tags or {}) do set[t] = true; order[#order + 1] = t end
            for _, t in ipairs(type(value) == "table" and value or { value }) do
                if type(t) == "string" and t:sub(1, 1) == "-" then set[t:sub(2)] = nil
                elseif type(t) == "string" and not set[t] then set[t] = true; order[#order + 1] = t end
            end
            local tags = {}
            for _, t in ipairs(order) do if set[t] then tags[#tags + 1] = t end end
            eff.tags = tags
        elseif M.is_object(value) and M.is_object(eff[key]) then
            local function merge(into, from)
                for k, v in pairs(from) do
                    if v == json.null then into[k] = nil
                    elseif M.is_object(v) and M.is_object(into[k]) then merge(into[k], v)
                    else into[k] = copy(v) end
                end
            end
            merge(eff[key], value)
        else
            eff[key] = copy(value)
        end
        prov[key] = p
    end
end

-- A value as text, the same for the same value: for comparing answers and for explain.
function M.show(v)
    if v == json.null then return "null" end
    if type(v) == "string" then return string.format("%q", v) end
    if type(v) ~= "table" then return tostring(v) end
    local parts = {}
    if M.is_object(v) then
        local keys = {}
        for k in pairs(v) do keys[#keys + 1] = k end
        table.sort(keys)
        for _, k in ipairs(keys) do parts[#parts + 1] = k .. "=" .. M.show(v[k]) end
        return "{" .. table.concat(parts, ", ") .. "}"
    end
    for i, x in ipairs(v) do parts[i] = M.show(x) end
    return "[" .. table.concat(parts, ", ") .. "]"
end

return M
