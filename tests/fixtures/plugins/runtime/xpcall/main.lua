function before_lookup(ctx)
    local word = ctx.word
    if word == "ok" then
        local ok, sum, name = xpcall(function(x, y) return x + y, "two" end,
            function() return "handled" end, 1, 2)
        return { title = "xpcall", html = tostring(ok) .. "|" .. tostring(sum) .. "|" .. name }
    elseif word == "error" then
        local ok, message = xpcall(function() error("boom", 0) end,
            function(e) return "handled " .. e end)
        return { title = "xpcall", html = tostring(ok) .. "|" .. message }
    elseif word == "failing" then
        local ok, message = xpcall(function() error("boom", 0) end,
            function() error("again", 0) end)
        return { title = "xpcall", html = tostring(ok) .. "|" .. tostring(message) }
    elseif word == "endless" then
        xpcall(function() while true do end end, function() while true do end end)
    elseif word == "stop" then
        xpcall(function() error("first") end, function()
            kobalt.log("ready")
            while true do end
        end)
    end
    return { title = "xpcall", html = "finished" }
end
