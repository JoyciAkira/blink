/*-*- mode:c;-*-*/
/* Z1-G6-THREADS: child-side thread resume.
 *
 * The M115 worker instantiates this blink module in the child worker (shared
 * guest memory) and calls blink_user_thread_resume with the control block
 * guest address written by the parent's SysSpawnZ1. The child Machine is
 * reconstructed zero-copy (shared heap) and enters the real x86 interpreter.
 *
 * Contract: docs/design-locks/g6-threads-blink-contract-v1.md §9.4 */
#include "blink/machine.h"
#include "blink/builtin.h"
#include "blink/log.h"

struct Z1ThreadCb {
  u64 magic;
  u64 machine_offset;
  u64 system_ptr;
  u64 clone_entry;
  u64 args_ptr;
  u64 stack_top;
  u64 tls_base;
  int tid;
  int pad;
};

__attribute__((export_name("blink_user_thread_resume")))
void blink_user_thread_resume(i64 cb_guest) {
  struct Z1ThreadCb *zcb;
  struct Machine *m;
  /* Error mailbox: zcb->pad (offset 56) carries a diagnostic code the worker
   * reads if this export returns early. Blink(m) is _Noreturn — reaching the
   * end of this function means an early-return path was taken. */
  if (!cb_guest) return;
  zcb = (struct Z1ThreadCb *)cb_guest;
  if (zcb->magic != 0x5a315448554e4f44ULL) {
    zcb->pad = 2; /* bad magic */
    return;
  }
  m = (struct Machine *)(cb_guest + zcb->machine_offset);
  g_machine = m;
  zcb->pad = 0; /* entered x86 loop */
  Blink(m);     /* _Noreturn — real x86 execution on the child Machine */
  zcb->pad = 9; /* Blink returned (must not happen) */
}
