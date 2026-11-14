-- The solver: one file and its effective metadata in, one answer out. A pure function of
-- the file's name, its metadata and the types and converters in scope, so it can run for
-- one file, for a thousand, or for none (explain).
--
-- The answer:
--   action   "ignore" (cook is false), "copy" (it lands as it is), "convert" (a
--            converter takes it somewhere), "keep" (an ingredient: nothing to do), or
--            "error"
--   type     the file's type, or nil when no rule gave it one
--   target   the type it is wanted as, or nil
--   path     the converters from here to the target, by name; { "copy" } when it lands as it is
--   step     the converter this file is handed to next (path[1]), for "convert"
--   lands    whether a copy of this file goes to the output tree
--   output   where, relative to the output tree: "<module>/<path>"
--   params   the metadata the solver does not read itself, for the converter
--   why      for each of type, path and lands: a line saying what decided
--   warn     a soft warning, or nil;  error  what is wrong, or nil
local berg = require("berg")

local M = {}

-- Keys the solver reads itself; the rest are the converter's.
local OWN = { type = true, cook = true, land = true, to = true, out = true, tags = true }

local function is_a(t, ancestor)
    return t == ancestor or t:sub(1, #ancestor + 1) == ancestor .. "."
end
M.is_a = is_a

-- The declaration of t or of its nearest declared supertype ("texture.color.srgb" is a
-- "texture"), and the name it was found under.
local function type_info(reg, t)
    while t do
        if reg.types[t] then return reg.types[t], t end
        t = t:match("^(.*)%.[^.]*$")
    end
    return nil
end
M.type_info = type_info

local function from_where(p)
    if not p then return nil end
    return string.format("%s: %s", p.file, p.where)
end

-- Every shortest chain of converters from `from` to a type that satisfies `goal`.
local function search(reg, from, goal)
    local names = {}
    for name in pairs(reg.converters) do names[#names + 1] = name end
    table.sort(names)
    local frontier = { { type = from, path = {} } }
    for _ = 1, 8 do
        local found, next_frontier = {}, {}
        for _, node in ipairs(frontier) do
            for _, name in ipairs(names) do
                local c = reg.converters[name]
                local used = false
                for _, n in ipairs(node.path) do if n == name then used = true end end
                if not used and is_a(node.type, c.from) then
                    local path = {}
                    for i, n in ipairs(node.path) do path[i] = n end
                    path[#path + 1] = name
                    if goal(c.to) then found[#found + 1] = { path = path, type = c.to }
                    else next_frontier[#next_frontier + 1] = { type = c.to, path = path } end
                end
            end
        end
        if #found > 0 then return found end
        if #next_frontier == 0 then return {} end
        frontier = next_frontier
    end
    return {}
end

-- solve(module, path, eff, prov, reg): the answer for `path` of the asset root `module`.
function M.solve(module, path, eff, prov, reg)
    local a = { why = {} }
    if eff.cook == false then
        a.action = "ignore"
        a.why.action = from_where(prov.cook)
        return a
    end
    local out = module .. "/" .. (type(eff.out) == "string" and eff.out or path)
    a.params = {}
    for k, v in pairs(eff) do if not OWN[k] then a.params[k] = v end end
    a.tags = eff.tags

    local t = eff.type
    if t ~= nil and type(t) ~= "string" then
        a.action, a.error = "error", string.format('"type" takes a name, not %s (%s)', berg.show(t), from_where(prov.type))
        return a
    end
    if t == nil then
        -- The fallback: nobody said what this is. It is copied, and says so.
        a.action, a.path, a.lands, a.output = "copy", { "copy" }, eff.land ~= false, out
        if not a.lands then a.action, a.path, a.output = "keep", {}, nil end
        a.why.type = "no rule gives this file a type"
        a.why.path = "the fallback: copied as it is"
        a.why.lands = eff.land == nil and "the fallback" or from_where(prov.land)
        a.warn = 'no type: copied as it is. Consider declaring one: { "match": "*' .. (path:match("%.[^./]*$") or path) .. '", "meta": { "type": "..." } }'
        return a
    end
    a.type = t
    a.why.type = from_where(prov.type)
    local info, declared_as = type_info(reg, t)
    if not info then
        a.action = "error"
        a.error = string.format('the type "%s" is not declared (%s). Declare it: "types": { "%s": { "runtime": true } }', t, from_where(prov.type), t)
        return a
    end
    local runtime_why = declared_as == t and (t .. " is a runtime type") or (t .. " is a " .. declared_as .. ", a runtime type")
    local target, target_why = nil, nil
    if eff.to ~= nil then
        if type(eff.to) ~= "string" then
            a.action, a.error = "error", string.format('"to" takes a type name (%s)', from_where(prov.to))
            return a
        end
        target, target_why = eff.to, from_where(prov.to)
    elseif info.to then
        target, target_why = info.to, string.format('%s: type "%s" cooks to %s', info.file, declared_as, info.to)
    elseif info.runtime then
        target = t
    end
    a.target = target

    local arrived = target ~= nil and is_a(t, target)
    if arrived then
        local lands = info.runtime
        if eff.land ~= nil then lands = eff.land == true end
        a.lands = lands
        a.why.lands = eff.land ~= nil and from_where(prov.land) or (info.runtime and runtime_why or (t .. " is not a runtime type"))
        if lands then
            a.action, a.path, a.output = "copy", { "copy" }, out
            a.why.path = target_why and ("already a " .. target .. " (" .. target_why .. ")") or (t .. " is already what the game reads")
        else
            a.action, a.path = "keep", {}
            a.why.path = "an ingredient: nothing cooks it on its own"
        end
        return a
    end

    -- Somewhere to go: to the target, or, with none named, to the nearest runtime type.
    local goal
    if target then goal = function(x) return is_a(x, target) end
    else goal = function(x) local i = type_info(reg, x); return i ~= nil and i.runtime end end
    local found = search(reg, t, goal)
    if #found == 0 then
        a.action = "error"
        if target then a.error = string.format("nothing cooks a %s into a %s (%s)", t, target, target_why or "its target")
        else a.error = string.format('nothing cooks a %s: it is not a runtime type and no converter takes it. Register one, or say { "cook": false }, or { "land": true } to ship it as it is', t) end
        if eff.land == true then
            -- Told to ship it as it is: that is an answer.
            a.action, a.error, a.path, a.lands, a.output = "copy", nil, { "copy" }, true, out
            a.why.path, a.why.lands = "shipped as it is", from_where(prov.land)
        end
        return a
    end
    if #found > 1 then
        local lines = {}
        for _, f in ipairs(found) do
            lines[#lines + 1] = string.format('%s (pick it with { "to": "%s" } or by removing the other)', table.concat(f.path, " -> "), f.type)
        end
        a.action = "error"
        a.error = string.format("more than one way to cook a %s%s: %s", t, target and (" into a " .. target) or "", table.concat(lines, "; "))
        return a
    end
    a.action, a.path, a.step = "convert", found[1].path, found[1].path[1]
    a.result = found[1].type
    local c = reg.converters[a.step]
    a.why.path = string.format("the only path from %s to %s (%s: converter \"%s\")", t, target or "a runtime type", c.file, a.step)
    a.lands = eff.land == true
    a.why.lands = eff.land == true and from_where(prov.land) or (t .. " is not what the game reads: its converter's outputs land, if they are")
    if a.lands then a.output = out end
    return a
end

-- The answer as one line, the same for the same answer: what a re-solve is compared by,
-- and what a dump prints.
function M.line(a)
    local parts = { a.action }
    if a.type then parts[#parts + 1] = "type=" .. a.type end
    if a.target and a.target ~= a.type then parts[#parts + 1] = "to=" .. a.target end
    if a.path and #a.path > 0 then parts[#parts + 1] = "path=" .. table.concat(a.path, ">") end
    if a.lands ~= nil then parts[#parts + 1] = "lands=" .. (a.lands and "yes" or "no") end
    if a.output then parts[#parts + 1] = "out=" .. a.output end
    if a.params and next(a.params) then
        local keys = {}
        for k in pairs(a.params) do keys[#keys + 1] = k end
        table.sort(keys)
        for _, k in ipairs(keys) do parts[#parts + 1] = k .. "=" .. berg.show(a.params[k]) end
    end
    if a.tags and #a.tags > 0 then parts[#parts + 1] = "tags=" .. table.concat(a.tags, ",") end
    if a.error then parts[#parts + 1] = "error=" .. a.error end
    if a.warn then parts[#parts + 1] = "warn" end
    return table.concat(parts, " ")
end

return M
