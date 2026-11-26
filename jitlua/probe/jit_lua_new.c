/* Narrow diagnostic: create and close one MaxLua state, without opening libraries. */
#define _GNU_SOURCE
#include "ext.h"
#include "ext_obex.h"
#include <dlfcn.h>
#include <stddef.h>

typedef struct lua_State lua_State;
typedef lua_State *(*lua_newstate_fn)(void);
typedef void (*lua_close_fn)(lua_State *);
typedef struct {
  t_object object;
} t_new_probe;
static t_class *probe_class;

static void *probe_new(void *unused) {
  (void)unused;
  t_new_probe *probe = (t_new_probe *)object_alloc(probe_class);
  if (!probe)
    return NULL;
  object_post((t_object *)probe, "jit-lua-new event=begin");
  void *library = dlopen("MaxLua.so", RTLD_NOW | RTLD_NOLOAD);
  if (!library) {
    object_post((t_object *)probe, "jit-lua-new event=maxlua_unavailable");
    return probe;
  }
  lua_newstate_fn new_state = (lua_newstate_fn)dlsym(library, "luaL_newstate");
  lua_close_fn close_state = (lua_close_fn)dlsym(library, "lua_close");
  if (!new_state || !close_state) {
    object_post((t_object *)probe, "jit-lua-new event=symbol_unavailable");
    dlclose(library);
    return probe;
  }
  object_post((t_object *)probe, "jit-lua-new event=before_newstate");
  lua_State *state = new_state();
  if (state) {
    object_post((t_object *)probe, "jit-lua-new event=newstate_ok");
    close_state(state);
    object_post((t_object *)probe, "jit-lua-new event=close_ok");
  } else {
    object_post((t_object *)probe, "jit-lua-new event=newstate_failed");
  }
  dlclose(library);
  return probe;
}

void ext_main(void *module) {
  (void)module;
  probe_class = class_new("jit.luanewprobe", probe_new, NULL, sizeof(t_new_probe), NULL, 0);
  if (!probe_class) {
    post("jit-lua-new event=register_failed");
    return;
  }
  class_register(CLASS_BOX, probe_class);
  post("jit-lua-new event=registered");
}
