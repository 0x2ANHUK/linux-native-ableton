/* Negative-test loader: forwards dlopen without changing flags. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stddef.h>
typedef void *(*dlopen_fn)(const char *, int);
__attribute__((visibility("default"))) void *dlopen(const char *path, int flags) {
  dlopen_fn next = (dlopen_fn)dlsym(RTLD_NEXT, "dlopen");
  return next ? next(path, flags) : NULL;
}
