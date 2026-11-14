-- Shell converters: where a step's script is, the command line that runs it, and the
-- names it said it would write. Used by the cook (graph.lua) and by `goldberg learn`.
--
-- A converter is registered in a .berg file under an asset root:
--   "cut": { "from": "sheet", "to": "texture", "shell": "tools/cut",
--            "args": "{in} {work}", "outputs": ["{stem}_*.png"] }
-- `shell` names a script without its extension, relative to that .berg file. It runs as
-- `/bin/sh tools/cut.sh <args>`, in the directory it is to write into ({work}).
-- In `args` and `outputs`: {in} the source's full path, {work} the directory to write
-- into, {name} the source's file name, {stem} that name up to its first dot, and any key
-- of the asset's metadata ({filter}, {atlas.pad}). The same are in the environment, as
-- GOLDBERG_IN, GOLDBERG_WORK, GOLDBERG_NAME, GOLDBERG_STEM and GOLDBERG_META_<KEY>.
local berg = require("berg")

local M = {}

local function dirname(path) return path:match("^(.*)/[^/]*$") or "" end
local function basename(path) return path:match("([^/]*)$") end

-- A step's directory in the work root: one path component, the same for the same source
-- and converter, and readable enough to find by eye.
function M.work_dir(key, conv_name)
    return kv.sha256(key .. "\0" .. conv_name):sub(1, 8) .. "-" .. basename(key:match("^[^:]*:(.*)$") or key) .. "." .. conv_name
end

-- The converter's registration as text, the same for the same registration.
function M.signature(conv)
    local parts = {}
    for _, k in ipairs{ "from", "to", "shell", "args", "outputs", "settle" } do
        parts[#parts + 1] = k .. "=" .. (conv[k] == nil and "" or berg.show(conv[k]))
    end
    return table.concat(parts, " ")
end

-- script(conv, has): the key ("<root>:<path>") of the script's file and its kind ("sh" or
-- "bat"), or nil and why. has(key) says whether the session knows such a file.
function M.script(conv, has)
    local root, berg_path = (conv.key or ""):match("^([^:]+):(.*)$")
    if not root then
        return nil, string.format('the converter "%s" is registered in %s: a shell converter\'s script is found beside the .berg file of an asset root, so register it in one', conv.name, conv.file)
    end
    if conv.shell:find("..", 1, true) or conv.shell:sub(1, 1) == "/" then
        return nil, string.format('the converter "%s": "shell" names a script under its .berg file\'s directory, not "%s"', conv.name, conv.shell)
    end
    local dir = dirname(berg_path)
    local base = root .. ":" .. (dir == "" and "" or dir .. "/") .. conv.shell
    if host.windows and has(base .. ".bat") then return base .. ".bat", "bat" end
    if not has(base .. ".sh") then
        return nil, string.format('the converter "%s" runs %s.sh, which is not there (registered in %s)', conv.name, conv.shell, conv.file)
    end
    return base .. ".sh", "sh"
end

local function lookup(params, name)
    local v = params
    for part in name:gmatch("[^.]+") do
        if type(v) ~= "table" then return nil end
        v = v[part]
    end
    return v
end

local function subst(word, vars, params)
    local bad
    local out = word:gsub("{([%w_.%-]+)}", function(name)
        local v = vars[name]
        if v == nil then v = lookup(params, name) end
        if v == nil then bad = "{" .. name .. "} is not {in}, {work}, {name}, {stem} or a key of the asset's metadata" return "" end
        if type(v) == "table" then bad = "{" .. name .. "} is not a single value" return "" end
        return tostring(v)
    end)
    if bad then return nil, bad end
    return out
end

-- vars(input, work): the names every command line may use.
function M.vars(input, work)
    local name = basename(input)
    return { ["in"] = input, work = work, name = name, stem = name:match("^([^.]*)") }
end

-- command(conv, script, kind, vars, params): argv and env for one run, or nil and why.
function M.command(conv, script, kind, vars, params)
    local argv
    if kind == "bat" then argv = { "cmd", "/c", script }
    else argv = { host.windows and "sh" or "/bin/sh", script } end
    for word in (conv.args or "{in} {work}"):gmatch("%S+") do
        local w, bad = subst(word, vars, params or {})
        if not w then return nil, string.format('the converter "%s": in "args", %s (%s)', conv.name, bad, conv.file) end
        argv[#argv + 1] = w
    end
    local env = { "GOLDBERG_IN=" .. vars["in"], "GOLDBERG_WORK=" .. vars.work, "GOLDBERG_NAME=" .. vars.name, "GOLDBERG_STEM=" .. vars.stem }
    local keys = {}
    for k, v in pairs(params or {}) do if type(v) ~= "table" then keys[#keys + 1] = k end end
    table.sort(keys)
    for _, k in ipairs(keys) do env[#env + 1] = "GOLDBERG_META_" .. k:upper():gsub("[^%w]", "_") .. "=" .. tostring(params[k]) end
    return argv, env
end

-- patterns(conv, vars, params): the declared outputs with their names filled in: a list,
-- "scan", or nil when the converter declares none. Or nil and why.
function M.patterns(conv, vars, params)
    if type(conv.outputs) ~= "table" then return conv.outputs end
    local out = {}
    for i, p in ipairs(conv.outputs) do
        local w, bad = subst(p, vars, params or {})
        if not w then return nil, string.format('the converter "%s": in "outputs", %s (%s)', conv.name, bad, conv.file) end
        out[i] = w
    end
    return out
end

return M
