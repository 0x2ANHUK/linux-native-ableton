/* Minimal loader contract test; it is not a substitute for running Max. */
#include "ext.h"
#include "ext_obex.h"
#include <assert.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static method constructor;
static int registered, created;
static const char *expected_class;
static char class_storage[1024];
static t_symbol symbol;

t_symbol *gensym(const char *name) {
  (void)name;
  return &symbol;
}
t_class *class_new(const char *name, method make, method destroy, long size, method menu,
                   short type, ...) {
  (void)destroy;
  (void)menu;
  (void)type;
  assert(strcmp(name, expected_class) == 0 && size > 0);
  constructor = make;
  return (t_class *)class_storage;
}
t_max_err class_register(t_symbol *name, t_class *cls) {
  assert(name == &symbol && cls == (t_class *)class_storage);
  registered++;
  return 0;
}
void *object_alloc(t_class *cls) {
  assert(cls == (t_class *)class_storage);
  return calloc(1, 1024);
}
void object_post(t_object *object, const char *message, ...) {
  assert(object);
  if (strcmp(message, "jit-lua-probe event=created") == 0 ||
      strcmp(message, "jit-lua-origin event=completed") == 0 ||
      strcmp(message, "jit-lua-new event=close_ok") == 0 ||
      strcmp(message, "jit-lua-stdlib event=close_ok") == 0)
    created++;
}
void post(const char *message, ...) {
  assert(strcmp(message, "jit-lua-probe event=registered") == 0 ||
         strcmp(message, "jit-lua-origin event=registered") == 0 ||
         strcmp(message, "jit-lua-new event=registered") == 0 ||
         strcmp(message, "jit-lua-stdlib event=registered") == 0);
}
int main(int argc, char **argv) {
  assert(argc == 3 || argc == 4);
  if (argc == 4) {
    void *fake_lua = dlopen(argv[3], RTLD_NOW | RTLD_GLOBAL);
    assert(fake_lua);
  }
  expected_class = argv[2];
  void *module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!module) {
    fprintf(stderr, "dlopen: %s\n", dlerror());
    return 1;
  }
  void (*entry)(void *) = (void (*)(void *))dlsym(module, "ext_main");
  assert(entry);
  entry(NULL);
  assert(registered == 1 && constructor);
  void *object = constructor(NULL);
  assert(object && created == 1);
  free(object);
  assert(dlclose(module) == 0);
  puts("PROBE_HOST_TEST_PASS");
}
