-- Editing a .berg file in place: one value set at a path of keys, and every other byte
-- of the file left as its author wrote it: the comments, the spacing, the order.
--
-- set(text, path, value) returns the new text. `path` is a list of keys from the top
-- object down; `value` is JSON text. A key that is there has its value replaced. One
-- that is not is added after the last member of its object, on a line of its own where
-- that object's members have lines of their own, and with any objects on the way down
-- that are missing too. Raises a string if the text is not the JSON it expects; a caller
-- edits a file that already parsed.
local M = {}

-- Past whitespace and // comments.
local function skip(text, i)
    while true do
        local c = text:sub(i, i)
        if c == " " or c == "\t" or c == "\r" or c == "\n" then i = i + 1
        elseif text:sub(i, i + 1) == "//" then
            local nl = text:find("\n", i, true)
            i = nl and nl + 1 or #text + 1
        else return i end
    end
end

-- i at an opening quote: the index after the closing one.
local function string_end(text, i)
    local j = i + 1
    while j <= #text do
        local c = text:sub(j, j)
        if c == "\\" then j = j + 2
        elseif c == '"' then return j + 1
        else j = j + 1 end
    end
    error("a string that does not end", 0)
end

-- i at the first character of a value: the index after its last.
local function value_end(text, i)
    local c = text:sub(i, i)
    if c == '"' then return string_end(text, i) end
    if c == "{" or c == "[" then
        local depth, j = 0, i
        while j <= #text do
            c = text:sub(j, j)
            if c == '"' then j = string_end(text, j)
            elseif text:sub(j, j + 1) == "//" then
                local nl = text:find("\n", j, true)
                j = nl and nl + 1 or #text + 1
            else
                if c == "{" or c == "[" then depth = depth + 1
                elseif c == "}" or c == "]" then
                    depth = depth - 1
                    if depth == 0 then return j + 1 end
                end
                j = j + 1
            end
        end
        error("an object or a list that does not end", 0)
    end
    local _, e = text:find("^[^,%]}%s/]+", i)
    if not e then error("a value is missing", 0) end
    return e + 1
end

-- i at "{": its members as { key, at (the key's quote), from, to (the value, to exclusive) },
-- and the index of the closing "}".
local function members(text, i)
    local out = {}
    local j = skip(text, i + 1)
    while text:sub(j, j) ~= "}" do
        if text:sub(j, j) ~= '"' then error("a key is missing", 0) end
        local key_end = string_end(text, j)
        local colon = skip(text, key_end)
        if text:sub(colon, colon) ~= ":" then error("a ':' is missing", 0) end
        local from = skip(text, colon + 1)
        local to = value_end(text, from)
        out[#out + 1] = { key = text:sub(j + 1, key_end - 2), at = j, from = from, to = to }
        j = skip(text, to)
        if text:sub(j, j) == "," then j = skip(text, j + 1) end
        if j > #text then error("an object that does not end", 0) end
    end
    return out, j
end

function M.set(text, path, value)
    local i = skip(text, 1)
    if text:sub(i, i) ~= "{" then error("not an object", 0) end
    for n, key in ipairs(path) do
        local ms = members(text, i)
        local found
        for _, m in ipairs(ms) do if m.key == key then found = m end end
        if found and n == #path then
            return text:sub(1, found.from - 1) .. value .. text:sub(found.to)
        elseif found then
            if text:sub(found.from, found.from) ~= "{" then error('"' .. key .. '" is not an object', 0) end
            i = found.from
        else
            local v = value
            for k = #path, n + 1, -1 do v = '{ "' .. path[k] .. '": ' .. v .. ' }' end
            local member = '"' .. key .. '": ' .. v
            if #ms == 0 then
                local tight = text:sub(i + 1, i + 1) == "}"
                return text:sub(1, i) .. " " .. member .. (tight and " " or "") .. text:sub(i + 1)
            end
            local last = ms[#ms]
            local line = last.at
            while line > 1 and text:sub(line - 1, line - 1) ~= "\n" do line = line - 1 end
            local indent = text:sub(line, last.at - 1)
            local sep = indent:match("^[ \t]*$") and (",\n" .. indent) or ", "
            return text:sub(1, last.to - 1) .. sep .. member .. text:sub(last.to)
        end
    end
    error("an empty path", 0)
end

return M
