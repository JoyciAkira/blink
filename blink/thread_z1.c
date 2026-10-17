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
  if (!cb_guest) {
    LOGF("Z1 thread resume: null control block");
    return;
  }
  zcb = (struct Z1ThreadCb *)cb_guest;
  if (zcb->magic != 0x5a315448554e4f44ULL) {
    LOGF("Z1 thread resume: bad control block magic %#" PRIx64, zcb->magic);
    return;
  }
  m = (struct Machine *)(cb_guest + zcb->machine_offset);
  /* Per-instance host state: g_machine is Thread-local, so this worker's
   * instance gets its own without disturbing any other Machine. m->system
   * and all heap pointers inside the Machine are shared guest memory. */
  g_machine = m;
  LOGF("Z1 thread resume: tid=%d entering x86 loop", m->tid);
  Blink(m); /* _Noreturn — real x86 execution on the child Machine */
}
