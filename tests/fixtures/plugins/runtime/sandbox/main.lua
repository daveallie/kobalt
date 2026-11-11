for _, name in ipairs({ "io", "os", "package", "debug", "dofile", "loadfile", "require", "print" }) do
    assert(_G[name] == nil, name .. " is visible")
end
local chunk, message = load(string.dump(function() return 1 end))
assert(chunk == nil and message, "a binary chunk loaded")
chunk, message = load("return 1", "text", "b")
assert(chunk == nil and message, "binary mode was accepted")
chunk, message = load("return 1", "text", "bt")
assert(chunk == nil and message, "binary and text mode was accepted")
assert(load("return 1")() == 1, "a text chunk did not load")
assert(load("return 2", "text", "t")() == 2, "text mode did not load")
assert(load("return x", "env", "t", { x = 3 })() == 3, "the environment argument was lost")

function before_lookup(ctx)
    return { title = "sandbox", html = "ok" }
end
