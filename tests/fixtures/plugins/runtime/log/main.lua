function before_lookup(ctx)
    for i = 1, 30 do
        kobalt.log("line " .. i)
    end
    return { title = "log", html = "ok" }
end
