local opened = false

-- About two million instructions: slow, but under the budget.
function book_opened(book)
  local total = 0
  for i = 1, 500000 do
    total = total + i
  end
  opened = total > 0
end

function before_lookup(ctx)
  return { title = opened and "after" or "before", html = "<p>open</p>" }
end
