local value = kobalt.json.decode('{"name": "Maren", "at": 41.5, "count": 3, "alive": true, '
    .. '"lost": false, "aliases": ["Holt", "Harbourmaster"], "ship": {"name": "Gull", "crew": [1, 2]}}')
assert(type(value) == "table", "the object did not decode to a table")
assert(value.name == "Maren", "the string field is wrong")
assert(value.at == 41.5, "the fractional number is wrong")
assert(value.count == 3 and math.type(value.count) == "integer", "the whole number is not an integer")
assert(value.alive == true and value.lost == false, "the booleans are wrong")
assert(#value.aliases == 2 and value.aliases[1] == "Holt" and value.aliases[2] == "Harbourmaster",
    "the array is wrong")
assert(value.ship.name == "Gull" and value.ship.crew[2] == 2, "the nested values are wrong")
local list = kobalt.json.decode("[10, [20], {\"a\": 30}]")
assert(list[1] == 10 and list[2][1] == 20 and list[3].a == 30, "the top-level array is wrong")
local invalid, message = kobalt.json.decode("{ broken")
assert(invalid == nil and type(message) == "string" and #message > 0, "invalid JSON did not fail")

function before_lookup(ctx)
    return { title = "json", html = "ok" }
end
