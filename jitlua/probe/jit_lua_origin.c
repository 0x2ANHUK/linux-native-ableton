/* Read-only loader diagnostic: resolve symbols; never create or execute Lua. */
#define _GNU_SOURCE
#include "ext.h"
#include "ext_obex.h"
#include <dlfcn.h>
#include <stddef.h>

typedef struct {
  t_object object;
} t_origin_probe;
static t_class *probe_class;

static void log_symbol(t_origin_probe *probe, void *handle, const char *scope, const char *name) {
  void *symbol = dlsym(handle, name);
  Dl_info info;
  if (symbol && dladdr(symbol, &info) && info.dli_fname)
    object_post((t_object *)probe, "jit-lua-origin scope=%s symbol=%s provider=%s", scope, name,
                info.dli_fname);
  else
    object_post((t_object *)probe, "jit-lua-origin scope=%s symbol=%s provider=unavailable", scope,
                name);
}

static void *probe_new(void *unused) {
  (void)unused;
  t_origin_probe *probe = (t_origin_probe *)object_alloc(probe_class);
  if (!probe)
    return NULL;
  void *lua = dlopen("MaxLua.so", RTLD_NOW | RTLD_NOLOAD);
  log_symbol(probe, RTLD_DEFAULT, "default", "luaL_newstate");
  log_symbol(probe, RTLD_DEFAULT, "default", "luaL_openlibs");
  log_symbol(probe, RTLD_DEFAULT, "default", "lua_pushstring");
  if (lua) {
    log_symbol(probe, lua, "maxlua", "luaL_newstate");
    log_symbol(probe, lua, "maxlua", "luaL_openlibs");
    log_symbol(probe, lua, "maxlua", "lua_pushstring");
    dlclose(lua);
  } else {
    object_post((t_object *)probe, "jit-lua-origin scope=maxlua provider=not_loaded");
  }
  object_post((t_object *)probe, "jit-lua-origin event=completed");
  return probe;
}

void ext_main(void *module) {
  (void)module;
  probe_class = class_new("jit.luaorigin", probe_new, NULL, sizeof(t_origin_probe), NULL, 0);
  if (!probe_class) {
    post("jit-lua-origin event=register_failed");
    return;
  }
  class_register(CLASS_BOX, probe_class);
  post("jit-lua-origin event=registered");
}
