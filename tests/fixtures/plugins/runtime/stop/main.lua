-- A stop unwinds nested pcalls and thousands of to-be-closed variables, each
-- with a Lua __close, over a live heap of several MiB.
local closer = setmetatable({}, { __close = function() local unused = 1 end })
local keep, extra, count = {}, {}, 0

-- Fills the heap to within a few bytes of the memory cap.
local function fill()
    for _ = 1, 3 do
        local size = 1 << 20
        while size >= 1 do
            local ok, text = pcall(string.rep, "y", size)
            if ok and size > 1 then
                count = count + 1
                extra[count] = text
            else
                size = size // 2
            end
        end
    end
    for i = 1, 2000 do
        local ok, text = pcall(string.format, "%x", count * 7919 + i)
        if ok then
            count = count + 1
            extra[count] = text
        end
    end
end

local function hold(depth, full)
    local a <close> = closer
    local b <close> = closer
    local c <close> = closer
    local d <close> = closer
    if depth == 0 then
        if full then
            pcall(fill)
            -- Literal strings and the count need no allocation here.
            if collectgarbage("count") * 1024 > 32 * 1024 * 1024 - 256 then
                kobalt.log("at cap")
            end
        end
        kobalt.log("ready")
        while true do end
    end
    hold(depth - 1, full)
end

local function nest(depth, full)
    if depth == 0 then return hold(1000, full) end
    local a <close> = closer
    pcall(nest, depth - 1, full)
end

function before_lookup(ctx)
    keep, extra, count = {}, {}, 0
    for i = 1, 100000 do keep[i] = { i } end
    for i = 1, 60000 do extra[i] = false end
    nest(150, ctx.word == "full")
    return { title = "stop", html = tostring(#keep) }
end
