-- About four million instructions: slower than the shortened test timeout, under the budget.
function before_lookup(ctx)
  local total = 0
  for i = 1, 2000000 do
    total = total + i
  end
  return { title = "Slow card", html = "<p>" .. total .. "</p>" }
end
