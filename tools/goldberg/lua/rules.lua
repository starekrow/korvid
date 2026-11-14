-- The rule files a session knows, and what they say together about one file.
--
-- A rule file is kept under a key: "conventions", "project", or "<root>:<path>" for one
-- inside an asset root. Its record holds the last version that parsed (`doc`) and, if the
-- version on disk does not, why (`err`): a broken file keeps its last good rules in
-- force while it is broken, and fails what it speaks for if it never had any.
local berg = require("berg")

local Rules = {}
Rules.__index = Rules

function Rules.new()
    return setmetatable({ recs = {} }, Rules)
end

-- set(key, text, display): reads a new version. Returns true if what the file says changed.
function Rules:set(key, text, display)
    local rec = self.recs[key] or { version = 0 }
    self.recs[key] = rec
    rec.display = display
    rec.version = rec.version + 1
    local doc, err = berg.parse(text, display)
    local before = rec.doc
    if doc then rec.doc, rec.err = doc, nil else rec.err = err end
    rec.ok = doc ~= nil
    return doc ~= nil or before == nil
end

function Rules:remove(key)
    local had = self.recs[key] ~= nil
    self.recs[key] = nil
    return had
end

function Rules:get(key) return self.recs[key] end

local function dirname(path)
    return path:match("^(.*)/[^/]*$") or ""
end
local function basename(path)
    return path:match("([^/]*)$")
end
-- The stem a sidecar is named for: the name up to its first dot ("ship.material.json" -> "ship").
local function stem(name)
    return name:match("^([^.]*)")
end
Rules.dirname, Rules.basename, Rules.stem = dirname, basename, stem

-- The keys of the rule files that could speak for root's `path`, outermost first, each
-- with how its patterns are matched: { key, kind, base }.
function Rules.chain(root, path)
    local layers = { { key = "conventions", kind = "top" }, { key = "project", kind = "top" } }
    local dir = dirname(path)
    local at = ""
    layers[#layers + 1] = { key = root .. ":goldberg.berg", kind = "dir", base = "" }
    for part in dir:gmatch("[^/]+") do
        at = at == "" and part or (at .. "/" .. part)
        layers[#layers + 1] = { key = root .. ":" .. at .. "/goldberg.berg", kind = "dir", base = at }
    end
    local name = basename(path)
    local s = stem(name)
    if s ~= "" and s ~= "goldberg" then
        layers[#layers + 1] = { key = root .. ":" .. (dir == "" and "" or dir .. "/") .. s .. ".berg", kind = "sidecar", base = dir }
    end
    return layers
end

-- What the layers say about root's `path`:
--   eff   the effective metadata
--   prov  per key, who said it last
--   reg   { types, converters }, each entry with the file that declared it
--   used  the keys of the rule files that exist in the chain
-- or nil and a message when a rule file in the chain is broken and never parsed.
function Rules:effective(root, path)
    local eff, prov, reg, used, warn = {}, {}, { types = {}, converters = {} }, {}, nil
    local name = basename(path)
    for _, layer in ipairs(Rules.chain(root, path)) do
        local rec = self.recs[layer.key]
        if rec then
            used[#used + 1] = layer.key
            if not rec.doc then return nil, rec.err end
            if rec.err then warn = rec.err .. " (its last good version is still in force)" end
            local doc = rec.doc
            for tname, t in pairs(doc.types) do reg.types[tname] = { runtime = t.runtime, to = t.to, file = rec.display, line = t.line } end
            for cname, c in pairs(doc.converters) do
                local conv = { name = cname, file = rec.display, key = layer.key }
                for k, v in pairs(c) do conv[k] = v end
                reg.converters[cname] = conv
            end
            if doc.meta then berg.apply(eff, prov, doc.meta, { file = rec.display, where = "meta" }) end
            local rel = path
            if layer.kind == "dir" and layer.base ~= "" then rel = path:sub(#layer.base + 2) end
            for _, rule in ipairs(doc.rules) do
                local hit
                if layer.kind == "sidecar" then hit = host.glob(rule.match, name)
                else hit = host.glob_path(rule.match, rel) end
                if hit then berg.apply(eff, prov, rule.meta, { file = rec.display, where = 'rule "' .. rule.match .. '"' }) end
            end
        end
    end
    return eff, prov, reg, used, warn
end

-- Whether the rule file at `key` (a "<root>:<path>" key) speaks for root's `path`.
function Rules.speaks_for(key, root, path)
    for _, layer in ipairs(Rules.chain(root, path)) do
        if layer.key == key then return true end
    end
    return false
end

return Rules
