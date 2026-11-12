-- Opening spends hundreds of milliseconds in string copies, so lookups queue behind it.
function book_opened(book)
    local block = string.rep("slow ", 2000)
    for i = 1, 8000 do
        local copy = string.rep(block, 100)
    end
end

function before_lookup(ctx)
    return { title = ctx.word, html = "<p>stale</p>" }
end
