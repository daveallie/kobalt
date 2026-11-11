function before_lookup(ctx)
    return { title = "sidecar", html = kobalt.read_sidecar(ctx.word) or "<nil>" }
end
