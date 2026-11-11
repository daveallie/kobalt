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
    end
    local plain = string.find(string.rep("ab", 100000) .. "abc", "abc", 1, true)
    return { title = word, html = tostring(plain) }
end
