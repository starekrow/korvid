-- explain: one file's answer as text. What it is, what happens to it, where it lands, and
-- for each of those the file and the rule that decided.
local berg = require("berg")

local M = {}

local function where(p)
    if not p then return "" end
    local s = string.format("%s: %s", p.file, p.where)
    if p.over then s = s .. string.format(" (over %s: %s)", p.over.file, p.over.where) end
    return s
end

-- rows(answer, eff, prov, out_tree): the explanation as { label, value, why, file } rows, the
-- `.berg` file behind each where there is one (goldberg's window links them).
function M.rows(a, eff, prov, out_tree)
    local out = {}
    local function row(label, value, why, p) out[#out + 1] = { label = label, value = value, why = why or "", file = p and p.file or "" } end
    if a.error and not a.type and not eff then
        row("error", a.error)
        return out
    end
    if a.action == "ignore" then
        row("cook", "no", a.why.action)
        return out
    end
    row("type", a.type or "(none)", prov.type and where(prov.type) or a.why.type, prov.type)
    -- The metadata nobody but a converter reads, in a fixed order.
    local keys = {}
    for k in pairs(a.params or {}) do keys[#keys + 1] = k end
    table.sort(keys)
    for _, k in ipairs(keys) do row(k, berg.show(a.params[k]), where(prov[k]), prov[k]) end
    if a.tags and #a.tags > 0 then row("tags", table.concat(a.tags, ", "), where(prov.tags), prov.tags) end
    if a.target and a.target ~= a.type then row("to", a.target, eff.to and where(prov.to) or "", eff.to and prov.to) end
    if a.error then
        row("error", a.error)
        return out
    end
    row("path", #a.path > 0 and table.concat(a.path, " -> ") or "(nothing)", a.why.path)
    row("lands", a.lands and "yes" or "no", a.why.lands)
    if a.output then row("output", (out_tree or "<output tree>") .. "/" .. a.output, eff.out and where(prov.out) or "", eff.out and prov.out) end
    if a.warn then row("warning", a.warn) end
    if a.rule_warn then row("warning", a.rule_warn) end
    return out
end

-- lines(path, answer, eff, prov, out_tree): the explanation, a list of lines.
function M.lines(path, a, eff, prov, out_tree)
    local out = { path }
    for _, r in ipairs(M.rows(a, eff, prov, out_tree)) do
        if r.why ~= "" then out[#out + 1] = string.format("  %-8s%-22s %s", r.label, r.value, r.why)
        else out[#out + 1] = string.format("  %-8s%s", r.label, r.value) end
    end
    return out
end

return M
