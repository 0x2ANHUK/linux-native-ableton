/* Stand-in for exercising probe sequencing; not a MaxLua implementation. */
#include <assert.h>
static int fake_state;
void *luaL_newstate(void) { return &fake_state; }
void lua_close(void *state) { assert(state == &fake_state); }

void luaL_openlibs(void *state) { assert(state == &fake_state); }
