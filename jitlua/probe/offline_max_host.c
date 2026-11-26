/* Linker-only stubs for a disposable process. Any attempted Max call aborts.
 * This is not a Max implementation and must never be loaded into Live or Max.
 */
#include <stdlib.h>
#include <unistd.h>

void *stdinletinfo;
static void unexpected(void) {
  static const char message[] = "OFFLINE_MAX_HOST_CALL_UNSUPPORTED\n";
  (void)write(2, message, sizeof(message) - 1);
  _Exit(125);
}
long method_false(void) {
  unexpected();
  return 0;
}
void *sysmem_ptrsize(void) {
  unexpected();
  return NULL;
}
void *sysmem_resizeptr(void) {
  unexpected();
  return NULL;
}
void *sysmem_freeptr(void) {
  unexpected();
  return NULL;
}
void *sysmem_newptr(void) {
  unexpected();
  return NULL;
}
void *sysmem_resizeptrclear(void) {
  unexpected();
  return NULL;
}
void *sysmem_newptrclear(void) {
  unexpected();
  return NULL;
}
void *freeobject(void) {
  unexpected();
  return NULL;
}
