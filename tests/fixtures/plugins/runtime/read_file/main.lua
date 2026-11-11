function before_lookup(ctx)
    return { title = "read", html = kobalt.read_file(ctx.word) or "<nil>" }
end
