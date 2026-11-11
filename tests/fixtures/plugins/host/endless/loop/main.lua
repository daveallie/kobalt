-- Each iteration copies 100 KB, so the budget alone would take many seconds to stop it.
function before_lookup(ctx)
  kobalt.log("looping")
  while true do
    local block = string.rep("x", 100000)
  end
end
