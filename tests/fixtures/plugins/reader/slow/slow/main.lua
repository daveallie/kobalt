-- Hundreds of milliseconds in string copies, yet few instructions, far under the budget.
function before_lookup(ctx)
  local block = string.rep("slow ", 2000)
  local total = 0
  for i = 1, 8000 do
    total = total + #string.rep(block, 100)
  end
  return { title = "Slow card", html = "<p>" .. total .. "</p>" }
end
