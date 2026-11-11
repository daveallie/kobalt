-- book_closed spends a few hundred milliseconds in one C call, which no stop can end.
local values = {}

function book_opened(book)
  for i = 1, 300000 do values[i] = (i * 7919) % 300007 end
end

function book_closed()
  kobalt.log("sorting")
  table.sort(values, math.ult)
end
