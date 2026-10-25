/* Resume the emulated x86 machine, never a nonexistent native C stack. */
#ifdef __wasm__
#include <errno.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "blink/atomic.h"
#include "blink/bus.h"
#include "blink/endian.h"
#include "blink/linux.h"
#include "blink/machine.h"
#include "blink/pml4t.h"
#include "blink/thread.h"

extern void __wasm_blink_fork_prepare(void);
extern void __wasm_blink_fork_parent(void);
extern void __wasm_blink_fork_child(void);
__asm__(".globaltype __tls_base, i32\n");

/* exec_lock serializes this record in the one-System Wasm image. The child
 * gets an independent byte-for-byte copy before the parent leaves clone. */
static struct {
  struct Machine *machine;
  u64 flags, stack, ctid;
  unsigned tls_base;
} fork_image;

static void ForkLocks(struct Machine *m, int lock) {
  if (!m->threaded) return;
  if (lock) {
    LOCK(&m->system->exec_lock);
    LOCK(&m->system->sig_lock);
    LOCK(&m->system->mmap_lock);
    LOCK(&m->system->pagelocks_lock);
    LOCK(&m->system->fds.lock);
    LOCK(&m->system->machines_lock);
#ifndef HAVE_PTHREAD_PROCESS_SHARED
    LOCK(&g_bus->futexes.lock);
#endif
  } else {
#ifndef HAVE_PTHREAD_PROCESS_SHARED
    UNLOCK(&g_bus->futexes.lock);
#endif
    UNLOCK(&m->system->machines_lock);
    UNLOCK(&m->system->fds.lock);
    UNLOCK(&m->system->pagelocks_lock);
    UNLOCK(&m->system->mmap_lock);
    UNLOCK(&m->system->sig_lock);
    UNLOCK(&m->system->exec_lock);
  }
}

int ForkWasm(struct Machine *m, u64 flags, u64 stack, u64 ctid) {
  /* Handle clone/vfork as private snapshot fork */
  ForkLocks(m, 1);
  fork_image.machine = m;
  fork_image.flags = flags;
  fork_image.stack = stack;
  fork_image.ctid = ctid;
  __asm__ volatile("global.get __tls_base\nlocal.set %0" : "=r"(fork_image.tls_base));
  __wasm_blink_fork_prepare();
  int pid = syscall(SYS_clone, 0, 0, SIGCHLD_LINUX, 0, 0, 0);
  __wasm_blink_fork_parent();
  ForkLocks(m, 0);
  return pid;
}

__attribute__((export_name("blink_user_fork_resume"), noreturn))
void blink_user_fork_resume(void) {
  __asm__ volatile("local.get %0\nglobal.set __tls_base" :: "r"(fork_image.tls_base));
  struct Machine *m = fork_image.machine;
  if (!m) __builtin_trap();
  g_machine = m;
  __wasm_blink_fork_child();
  ForkLocks(m, 0);
  if (fork_image.stack) Put64(m->sp, fork_image.stack);
#ifndef HAVE_PTHREAD_PROCESS_SHARED
  InitBus();
#endif
  m->tid = m->system->pid = getpid();
  m->system->isfork = true;
  RemoveOtherThreads(m->system);
  if ((fork_image.flags & (CLONE_CHILD_SETTID_LINUX | CLONE_CHILD_CLEARTID_LINUX)) &&
      !(fork_image.ctid & 3)) {
    _Atomic(i32) *word = (_Atomic(i32) *)LookupAddress(m, fork_image.ctid);
    if (word) {
      if (fork_image.flags & CLONE_CHILD_SETTID_LINUX)
        atomic_store_explicit(word, Little32(m->tid), memory_order_release);
      if (fork_image.flags & CLONE_CHILD_CLEARTID_LINUX) m->ctid = fork_image.ctid;
    }
  }
  Put64(m->ax, 0);
  Blink(m);
}
#endif
