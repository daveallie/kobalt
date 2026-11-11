-- A stop unwinds nested pcalls and thousands of to-be-closed variables, each
-- with a Lua __close, over a live heap of several MiB.
local closer = setmetatable({}, { __close = function() local unused = 1 end })

local function hold(depth)
    local a <close> = closer
    local b <close> = closer
    local c <close> = closer
    local d <close> = closer
    if depth == 0 then
        kobalt.log("ready")
        while true do end
    end
    hold(depth - 1)
end

local function nest(depth)
    if depth == 0 then return hold(1000) end
    local a <close> = closer
    pcall(nest, depth - 1)
end

function before_lookup(ctx)
    local keep = {}
    for i = 1, 100000 do keep[i] = { i } end
    nest(150)
    return { title = "stop", html = tostring(#keep) }
end
