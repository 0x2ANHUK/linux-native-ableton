// CRASHED Max on 2026-11-26; retained for analysis only. Do not load.
// Live diagnostic only: a distinct Max class, never a replacement for jit.gl.lua.
// Max creates and destroys instances on its UI thread; no audio callback is used.
#include "../lua_runtime.hpp"
#include "ext.h"
#include "ext_obex.h"
#include <cstring>
#include <exception>
#include <new>
#include <string>

extern "C" int luaopen_jit(lua_State *state);

namespace {
struct Probe {
  t_object object;
  jitlua::Runtime *runtime;
};
t_class *probe_class;

void *free_probe(void *object) {
  auto *probe = static_cast<Probe *>(object);
  delete probe->runtime;
  probe->runtime = nullptr;
  return nullptr;
}

void *new_probe(void *unused) {
  (void)unused;
  auto *probe = static_cast<Probe *>(object_alloc(probe_class));
  if (!probe)
    return nullptr;
  probe->runtime = nullptr;
  try {
    probe->runtime = new jitlua::Runtime();
    std::string error;
    if (!probe->runtime->registerModule("jit", luaopen_jit, error)) {
      object_error(reinterpret_cast<t_object *>(probe), "jit-lua-vm event=module_failed detail=%s",
                   error.c_str());
      return probe;
    }
    const char *script = "assert(type(jit.new) == 'function')";
    if (!probe->runtime->load(script, std::strlen(script), "jit-lua-vm-probe", error)) {
      object_error(reinterpret_cast<t_object *>(probe), "jit-lua-vm event=script_failed detail=%s",
                   error.c_str());
      return probe;
    }
    object_post(reinterpret_cast<t_object *>(probe), "jit-lua-vm event=module_ready");
  } catch (const std::exception &error) {
    object_error(reinterpret_cast<t_object *>(probe), "jit-lua-vm event=exception detail=%s",
                 error.what());
  }
  return probe;
}
} // namespace

extern "C" void ext_main(void *module) {
  (void)module;
  probe_class = class_new("jit.luavmprobe", new_probe, free_probe, sizeof(Probe), nullptr, 0);
  if (!probe_class) {
    post("jit-lua-vm event=register_failed");
    return;
  }
  class_register(CLASS_BOX, probe_class);
  post("jit-lua-vm event=registered");
}
