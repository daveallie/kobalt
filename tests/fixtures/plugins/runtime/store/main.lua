local function check(condition, message)
    if not condition then error(message, 2) end
end

local cases = {}

function cases.types()
    local s = kobalt.store
    check(s.set("missing", nil), "removing a missing key failed")
    check(s.set("b", true) and s.get("b") == true, "a boolean did not round-trip")
    check(s.set("i", 42) and math.type(s.get("i")) == "integer" and s.get("i") == 42,
        "an integer did not round-trip")
    check(s.set("f", 1.5) and s.get("f") == 1.5, "a float did not round-trip")
    check(s.set("s", "héllo") and s.get("s") == "héllo", "a string did not round-trip")
    check(s.set("t", { a = { 1, 2, { b = "c" } }, e = {} }), "a table was refused")
    local t = s.get("t")
    check(#t.a == 3 and t.a[3].b == "c" and type(t.e) == "table", "a table did not round-trip")
    check(s.set("t", nil) and s.get("t") == nil, "a key was not removed")
    return "ok"
end

function cases.refused()
    local s = kobalt.store
    local cyclic = {}
    cyclic.self = cyclic
    local values = {
        fn = function() end, nan = 0 / 0, inf = math.huge, mixed = { 1, a = 2 },
        sparse = { [1] = 1, [3] = 3 }, cyclic = cyclic, number_key = { [1.5] = 1 },
    }
    for name, value in pairs(values) do
        local ok, message = s.set("x", value)
        check(ok == nil and type(message) == "string", name .. " was accepted")
    end
    check(s.get("x") == nil, "a refused value was stored")
    check(s.set("", 1) == nil, "an empty key was accepted")
    return "ok"
end

function cases.copy()
    local s = kobalt.book_store
    check(s.set("t", { n = 1 }), "a table was refused")
    local t = s.get("t")
    t.n = 2
    check(s.get("t").n == 1, "get returned the stored table itself")
    return "ok"
end

function cases.nostore()
    local ok, message = kobalt.store.set("a", 1)
    check(ok == nil and message:find("no plugin data folder"), "a value was stored without a folder")
    check(kobalt.store.get("a") == nil, "a value appeared without a folder")
    return "ok"
end

function before_lookup(ctx)
    local ok, result = pcall(cases[ctx.word])
    return { title = ctx.word, html = ok and result or tostring(result) }
end
