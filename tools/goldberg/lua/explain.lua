-- explain: one file's answer as text. What it is, what happens to it, where it lands, and
-- for each of those the file and the rule that decided.
local berg = require("berg")

local M = {}

local function row(out, label, value, why)
    if why and why ~= "" then out[#out + 1] = string.format("  %-8s%-22s %s", label, value, why)
    else out[#out + 1] = string.format("  %-8s%s", label, value) end
end

local function where(p)
    if not p then return "" end
    local s = string.format("%s: %s", p.file, p.where)
    if p.over then s = s .. string.format(" (over %s: %s)", p.over.file, p.over.where) end
    return s
end

-- lines(path, answer, eff, prov, out_tree): the explanation, a list of lines.
function M.lines(path, a, eff, prov, out_tree)
    local out = { path }
    if a.error and not a.type and not eff then
        row(out, "error", a.error)
        return out
    end
    if a.action == "ignore" then
        row(out, "cook", "no", a.why.action)
        return out
    end
    row(out, "type", a.type or "(none)", prov.type and where(prov.type) or a.why.type)
    -- The metadata nobody but a converter reads, in a fixed order.
    local keys = {}
    for k in pairs(a.params or {}) do keys[#keys + 1] = k end
    table.sort(keys)
    for _, k in ipairs(keys) do row(out, k, berg.show(a.params[k]), where(prov[k])) end
    if a.tags and #a.tags > 0 then row(out, "tags", table.concat(a.tags, ", "), where(prov.tags)) end
    if a.target and a.target ~= a.type then row(out, "to", a.target, eff.to and where(prov.to) or "") end
    if a.error then
        row(out, "error", a.error)
        return out
    end
    row(out, "path", #a.path > 0 and table.concat(a.path, " -> ") or "(nothing)", a.why.path)
    row(out, "lands", a.lands and "yes" or "no", a.why.lands)
    if a.output then row(out, "output", (out_tree or "<output tree>") .. "/" .. a.output, eff.out and where(prov.out) or "") end
    if a.warn then row(out, "warning", a.warn) end
    if a.rule_warn then row(out, "warning", a.rule_warn) end
    return out
end

return M
