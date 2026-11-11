function before_lookup(ctx)
    if ctx.word == "grow" then
        local text = "x"
        while true do
            text = text .. text
        end
    end
    if ctx.word == "json" then
        local value, message = kobalt.json.decode("[" .. string.rep("1,", 200000) .. "1]")
        return { title = "json", html = value and "decoded" or message }
    end
    if ctx.word == "arrays" then
        -- Three bytes of text become a whole table each.
        local value, message = kobalt.json.decode("[" .. string.rep("[],", 40000) .. "[]]")
        return { title = "json", html = value and "decoded" or message }
    end
    if ctx.word == "big" then
        return { title = "big", html = tostring(#string.rep("x", 4000000)) }
    end
    return { title = "ok", html = "ok" }
end
