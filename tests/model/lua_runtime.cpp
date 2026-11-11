#include "check.h"

#include <lua.hpp>

int main()
{
    lua_State *state = luaL_newstate();
    require(state != nullptr, "Lua state was not created");
    require(LUA_VERSION_NUM == 504, "Lua is not 5.4");
    require(luaL_loadstring(state, "return 6 * 7") == LUA_OK, "Chunk did not compile");
    require(lua_pcall(state, 0, 1, 0) == LUA_OK, "Chunk did not run");
    require(lua_tointeger(state, -1) == 42, "Chunk returned the wrong value");
    lua_close(state);
    puts("PASS: Lua 5.4 runs a chunk");
}
