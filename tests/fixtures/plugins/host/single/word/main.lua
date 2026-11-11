function before_lookup(ctx)
  return { title = ctx.word, html = "<p>" .. ctx.word .. "</p>" }
end
