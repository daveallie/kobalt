-- C library calls that run no Lua instructions, so only their own limits stop them.
local endless = { __len = function() return math.maxinteger end }
local virtual = { __len = function() return 1 << 22 end, __index = rawlen }

function before_lookup(ctx)
    local word = ctx.word
    if word == "rep" then
        return { title = "rep", html = "[" .. string.rep("", math.maxinteger) .. "]" }
    elseif word == "move" then
        table.move({}, 1, 3e8, 1, {})
    elseif word == "find" then
        string.find(string.rep("a", 4000000), string.rep("a", 199999) .. "b", 1, true)
    elseif word == "insert" then
        table.insert(setmetatable({}, endless), 1, 1)
    elseif word == "concat" then
        table.concat(setmetatable({}, { __index = rawlen }), "", 1, math.maxinteger)
    elseif word == "sort" then
        table.sort(setmetatable({}, virtual), math.ult)
    elseif word == "weak" then
        -- Weak-key tables chained in random order made each collection quadratic.
        local n = 40000
        local keys, tables, order = {}, {}, {}
        for i = 1, n do
            keys[i] = {}
            tables[i] = setmetatable({}, { __mode = "k" })
            order[i] = i
        end
        local seed = 1
        for i = n, 2, -1 do
            seed = (seed * 1103515245 + 12345) % 2147483648
            local j = seed % i + 1
            order[i], order[j] = order[j], order[i]
        end
        for i = 1, n - 1 do
            tables[order[i]][keys[i]] = keys[i + 1]
        end
        local root = keys[1]
        keys = nil
        collectgarbage()
        collectgarbage()
        local values = setmetatable({}, { __mode = "v" })
        values[1] = {}
        collectgarbage()
        return { title = "weak", html = tostring(root ~= nil and values[1] == nil) }
    end
    local plain = string.find(string.rep("ab", 100000) .. "abc", "abc", 1, true)
    return { title = word, html = tostring(plain) }
end
