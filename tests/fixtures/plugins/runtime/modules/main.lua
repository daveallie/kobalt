loads = 0

local function try(f)
    local ok, message = pcall(f)
    return ok and "ok" or tostring(message)
end

local cases = {
    upper = function() return require("util.text").upper("a") end,
    once = function()
        local first = require("counter")
        local second = require("counter")
        return tostring(loads) .. ":" .. tostring(first) .. ":" .. tostring(second)
    end,
    bad = function()
        local out = {}
        for _, name in ipairs({ "../x", "/x", "a..b", ".a", "a.", "", "a/b", "a b" }) do
            out[#out + 1] = try(function() require(name) end)
        end
        return table.concat(out, "|")
    end,
    missing = function() return try(function() require("nope") end) end,
    cycle = function() return try(function() require("cycle_a") end) end,
    big = function() return try(function() require("big") end) end,
}

function before_lookup(ctx)
    return { title = ctx.word, html = cases[ctx.word]() }
end
