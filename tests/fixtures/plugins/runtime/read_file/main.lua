function before_lookup(ctx)
    local data, message = kobalt.read_file(ctx.word)
    return { title = "read", html = data or message or "<nil>" }
end
