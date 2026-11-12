function before_lookup(ctx)
    local book = (kobalt.book_store.get("n") or 0) + 1
    local global = (kobalt.store.get("n") or 0) + 1
    kobalt.book_store.set("n", book)
    kobalt.store.set("n", global)
    return { title = "book " .. book .. " global " .. global, html = "<p>remember</p>" }
end
