/* blink-wasm-stubs.c — stubs for host-only functions not available in wasm32 */
#include "blink/types.h"
#include "blink/machine.h"
#include "blink/linux.h"
#include "blink/errno.h"
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

int GetCpuCount(void) { return 1; }

int sysinfo_linux(struct sysinfo_linux *si) {
  if (si) memset(si, 0, sizeof(*si));
  return -1;
}

int SysIoctl(struct Machine *m, int fildes, u64 request, i64 addr) {
  (void)m; (void)fildes; (void)request; (void)addr;
  return -1;
}

pid_t fork(void) {
  return -1;
}
