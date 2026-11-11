local replies = {
    text = function() return "text" end,
    empty = function() return {} end,
    untitled = function() return { title = "t" } end,
    blank = function() return { title = "", html = "h" } end,
    number = function() return { title = 1, html = "h" } end,
}

function before_lookup(ctx)
    return replies[ctx.word]()
end
