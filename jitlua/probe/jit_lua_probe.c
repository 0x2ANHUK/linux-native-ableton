/* Isolated loader probe: never registers jit.gl.lua or changes device behavior. */
#include "ext.h"
#include "ext_obex.h"

typedef struct {
  t_object object;
} t_jit_linux_probe;

static t_class *probe_class;

static void *probe_new(void *unused) {
  (void)unused;
  t_jit_linux_probe *probe = (t_jit_linux_probe *)object_alloc(probe_class);
  if (probe)
    object_post((t_object *)probe, "jit-lua-probe event=created");
  return probe;
}

void ext_main(void *module) {
  (void)module;
  probe_class =
      class_new("jit.linuxprobe", (method)probe_new, NULL, sizeof(t_jit_linux_probe), NULL, 0);
  if (!probe_class) {
    post("jit-lua-probe event=register_failed");
    return;
  }
  class_register(CLASS_BOX, probe_class);
  post("jit-lua-probe event=registered");
}
