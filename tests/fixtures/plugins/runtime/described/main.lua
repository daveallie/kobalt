local opened

function book_opened(book)
    opened = book.title
end

function book_closed()
    opened = nil
end

function before_lookup(ctx)
    local p = ctx.position
    return {
        title = ctx.word,
        html = table.concat({ ctx.book.title, ctx.book.language, tostring(p.percent),
            tostring(p.page), tostring(p.pages), p.chapter, tostring(opened) }, "|"),
        footer = "echo",
    }
end
