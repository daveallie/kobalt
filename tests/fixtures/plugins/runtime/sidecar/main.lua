function before_lookup(ctx)
    local data, message = kobalt.read_sidecar(ctx.word)
    return { title = "sidecar", html = data or message or "<nil>" }
end
