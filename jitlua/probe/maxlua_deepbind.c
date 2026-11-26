/*
 * Experimental GNU/Linux loader probe, NOT enabled in Live or Max.
 * On an explicit opt-in, prefer MaxLua's own symbols only when an explicit
 * dlopen targets the exact MaxLua.so basename. It cannot rebind an already
 * loaded library or intercept a dependency loaded internally by the ELF loader.
 * Never invoke from an audio callback; dlopen and dlsym are not real-time safe.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

typedef void *(*dlopen_fn)(const char *, int);

__attribute__((visibility("default"))) void *dlopen(const char *path, int flags) {
  dlopen_fn next = (dlopen_fn)dlsym(RTLD_NEXT, "dlopen");
  if (!next)
    return NULL;
  const char *option = getenv("LIVE_MAXLUA_DEEPBIND");
  if (path && option && strcmp(option, "1") == 0) {
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    if (strcmp(name, "MaxLua.so") == 0)
      flags |= RTLD_DEEPBIND;
  }
  return next(path, flags);
}
