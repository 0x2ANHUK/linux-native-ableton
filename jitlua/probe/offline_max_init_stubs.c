/*
 * Linker stubs for a disposable, non-Max process only.
 *
 * These return inert values so MaxLua's own max_lua_init() can run far enough
 * to be observed. They are NOT a Max implementation: no host object is really
 * registered, no atom or dictionary is really created, and anything that
 * depends on real Max data is expected to misbehave. Never load into Max or Live.
 */
#include <stdlib.h>
#include <string.h>

static char inert[4096];
static long inert_long;

void *stdinletinfo;
void *method_false(void) { return NULL; }
void *sysmem_ptrsize(void) { return NULL; }
void *sysmem_resizeptr(void) { return NULL; }
void *sysmem_freeptr(void) { return NULL; }
void *sysmem_newptr(void) { return &inert; }
void *sysmem_resizeptrclear(void) { return &inert; }
void *sysmem_newptrclear(void) { return &inert; }
void *freeobject(void) { return NULL; }

void *class_new(const char *name, const void *make, const void *destroy, long size,
                const void *menu, long flags) {
  (void)name;
  (void)make;
  (void)destroy;
  (void)size;
  (void)menu;
  (void)flags;
  return &inert;
}
void *class_findbyname(void *namespace, void *name) {
  (void)namespace;
  (void)name;
  return &inert;
}
void *class_register(void *namespace, void *object) {
  (void)namespace;
  (void)object;
  return &inert;
}
void *class_addmethod(void *object, void *name, const void *method, const char *types) {
  (void)object;
  (void)name;
  (void)method;
  (void)types;
  return &inert;
}
void *class_addattr(void *object, void *attribute) {
  (void)object;
  (void)attribute;
  return &inert;
}
void *class_mess(void *object, void *name, const char *types, ...) {
  (void)object;
  (void)name;
  (void)types;
  return &inert;
}
void *class_nameget(void *object, char *name, long size) {
  (void)object;
  (void)name;
  (void)size;
  return &inert;
}
void *gensym(const char *name) {
  (void)name;
  return &inert;
}
void *attr_offset_new(const char *name, void *type, long flags, const void *get, const void *set) {
  (void)name;
  (void)type;
  (void)flags;
  (void)get;
  (void)set;
  return &inert;
}
void *atomarray_new(long size, void *atoms) {
  (void)size;
  (void)atoms;
  return &inert;
}
void *atomarray_appendatom(void *array, void *atom) {
  (void)array;
  (void)atom;
  return &inert;
}
void *atomarray_setatoms(void *array, long index, void *atom) {
  (void)array;
  (void)index;
  (void)atom;
  return &inert;
}
long atomarray_getsize(void *array) {
  (void)array;
  return 0;
}
long atomarray_getindex(void *array, long index, void *atom) {
  (void)array;
  (void)index;
  (void)atom;
  return 0;
}
void *atom_getobj(const void *atom) {
  (void)atom;
  return &inert;
}
void atom_setobj(void *atom, void *object) {
  (void)atom;
  (void)object;
}
void atom_setfloat(void *atom, double value) {
  (void)atom;
  (void)value;
}
void atom_setlong(void *atom, long value) {
  (void)atom;
  (void)value;
}
void atom_setsym(void *atom, void *symbol) {
  (void)atom;
  (void)symbol;
}
void *dictionary_new(long size, long flags) {
  (void)size;
  (void)flags;
  return &inert;
}
void dictionary_appendlong(void *dict, void *key, long value) {
  (void)dict;
  (void)key;
  (void)value;
}
void dictionary_appendfloat(void *dict, void *key, double value) {
  (void)dict;
  (void)key;
  (void)value;
}
void dictionary_appendstring(void *dict, void *key, const char *value) {
  (void)dict;
  (void)key;
  (void)value;
}
void dictionary_appendsym(void *dict, void *key, void *value) {
  (void)dict;
  (void)key;
  (void)value;
}
void dictionary_appendatom(void *dict, void *key, void *value) {
  (void)dict;
  (void)key;
  (void)value;
}
void dictionary_appendatomarray(void *dict, void *key, void *value) {
  (void)dict;
  (void)key;
  (void)value;
}
void dictionary_appenddictionary(void *dict, void *key, void *value) {
  (void)dict;
  (void)key;
  (void)value;
}
long dictionary_getentrycount(void *dict) {
  (void)dict;
  return 0;
}
long dictionary_hasentry(void *dict, void *key) {
  (void)dict;
  (void)key;
  return 0;
}
void *dictionary_getatom(long value, void *key, void *atom) {
  (void)value;
  (void)key;
  (void)atom;
  return NULL;
}
void dictionary_freekeys(void *dict) { (void)dict; }
void *hashtab_new(long size) {
  (void)size;
  return &inert;
}
void hashtab_store(void *table, void *key, long value) {
  (void)table;
  (void)key;
  (void)value;
}
long hashtab_lookup(void *table, void *key) {
  (void)table;
  (void)key;
  return 0;
}
void *linklist_new(void) { return &inert; }
void linklist_append(void *list, void *value) {
  (void)list;
  (void)value;
}
long linklist_getsize(void *list) {
  (void)list;
  return 0;
}
long linklist_getindex(void *list, long index) {
  (void)list;
  (void)index;
  return 0;
}
void *common_symbols_gettable(void) { return &inert; }
long *inert_storage(void) { return &inert_long; }
