function before_lookup(ctx)
    if ctx.word == "finalizer" then
        setmetatable({}, { __gc = function() while true do end end })
    end
    local fallback = setmetatable({}, { __index = function() return "indexed" end })
    return { title = "finalizer", html = fallback.missing }
end
