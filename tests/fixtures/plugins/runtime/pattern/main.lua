function before_lookup(ctx)
    if ctx.word == "slow" then
        string.find(string.rep("a", 40), string.rep("a*", 20) .. "b")
    end
    local count = select(2, string.gsub(string.rep("word ", 200000), "%a+", ""))
    return { title = "pattern", html = tostring(count) }
end
