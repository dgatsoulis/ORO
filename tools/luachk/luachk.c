/* luachk - compile-only check of Lua 5.1 files (Script\*.lua) with Orbiter's own Lua sources.
   Orbiter's Lua is 5.1 and 32-bit, no lua.exe ships, and a 64-bit Python cannot load lua.dll
   through ctypes - so this is the syntax check for anything ORO ships in Script\. Built by
   build.bat beside it from the clone's fetched sources (out\build\...\_deps\lua-src), ~10 s.
   usage: luachk file.lua [file.lua ...]   exit code = number of files that failed to load */
#include <stdio.h>
#include "lua.h"
#include "lauxlib.h"

int main(int argc, char **argv)
{
	int i, bad = 0;
	lua_State *L = luaL_newstate();
	for (i = 1; i < argc; i++) {
		int r = luaL_loadfile(L, argv[i]);
		if (r == 0) {
			printf("OK    %s\n", argv[i]);
			lua_pop(L, 1);
		} else {
			printf("FAIL  %s\n      %s\n", argv[i], lua_tostring(L, -1));
			lua_pop(L, 1);
			bad++;
		}
	}
	lua_close(L);
	return bad;
}
