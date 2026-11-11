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
    return { title = "ok", html = "ok" }
end
