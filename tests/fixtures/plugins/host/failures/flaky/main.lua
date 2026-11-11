local calls = 0

function before_lookup(ctx)
  calls = calls + 1
  if calls <= 3 then
    error("failure " .. calls)
  end
  return { title = "recovered", html = "<p>recovered</p>" }
end
