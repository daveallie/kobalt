function before_lookup(ctx)
    local word = ctx.word
    if word == "title" then
        return { title = string.rep("t", 201), html = "x" }
    elseif word == "footer" then
        return { title = "t", html = "x", footer = string.rep("f", 201) }
    elseif word == "html" then
        return { title = "t", html = string.rep("h", 64 * 1024 + 1) }
    end
    return { title = string.rep("t", 200), html = string.rep("h", 64 * 1024),
        footer = string.rep("f", 200) }
end
