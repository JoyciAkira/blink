/*-*-mode:c;indent-tabs-mode:nil;c-basic-offset:2;tab-width:8;coding:utf-8-*-│
│vi: set et ft=c ts=2 sts=2 sw=2 fenc=utf-8                               :vi│
╞══════════════════════════════════════════════════════════════════════════════╡
│ Copyright 2026 Daniele Corrao / SocrateFlow AI                               │
│                                                                              │
│ Permission to use, copy, modify, and/or distribute this software for         │
│ any purpose with or without fee is hereby granted, provided that the         │
│ above copyright notice and this permission notice appear in all copies.      │
│                                                                              │
│ THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL               │
│ WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED               │
│ WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE            │
│ AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL        │
│ DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR        │
│ PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER               │
│ TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR             │
│ PERFORMANCE OF THIS SOFTWARE.                                                │
╚─────────────────────────────────────────────────────────────────────────────*/
#include "blink/guest-process.h"

#include <errno.h>
#include <string.h>

#include "blink/assert.h"
#include "blink/endian.h"
#include "blink/machine.h"
#include "blink/log.h"

/* Global process table instance */
struct GuestProcessTable g_process_table;

void guest_proc_table_init(void) {
  if (g_process_table.initialized) return;
  memset(&g_process_table, 0, sizeof(g_process_table));
  g_process_table.next_pid = GUEST_INITIAL_PID;
  g_process_table.initialized = true;
}

void guest_proc_table_reset(void) {
  memset(&g_process_table, 0, sizeof(g_process_table));
  g_process_table.next_pid = GUEST_INITIAL_PID;
  g_process_table.initialized = true;
}

/*
 * Deterministic, collision-free PID allocation.
 * - PIDs are strictly positive integers in [1, GUEST_PID_MAX].
 * - Never returns 0.
 * - Wraps to 2 upon reaching GUEST_PID_MAX (PID 1 is reserved for init).
 * - Fails closed (returns 0) if all candidate PIDs are currently allocated
 *   to active or zombie processes.
 */
pid_t guest_proc_alloc_pid(void) {
  int attempts;
  pid_t cand;

  if (!g_process_table.initialized) {
    guest_proc_table_init();
  }

  if (g_process_table.count >= MAX_GUEST_PROCESSES) {
    return 0; /* Table is full, fail closed */
  }

  for (attempts = 0; attempts < GUEST_PID_MAX; attempts++) {
    cand = g_process_table.next_pid++;
    if (g_process_table.next_pid > GUEST_PID_MAX) {
      g_process_table.next_pid = 2; /* Wrap to 2, reserving 1 for init */
    }

    if (cand <= 0) continue;

    /* Check if candidate PID is already in use by any process (live or zombie) */
    if (guest_proc_find(cand) == NULL) {
      return cand;
    }
  }

  return 0; /* Exhaustion / collision lock, fail closed */
}

/* B2 scheduler wiring: run-queue append/remove + quantum init. */
#define GUEST_QUANTUM_INSTRUCTIONS 5000

static void proc_runq_append(struct GuestProcess *proc) {
  proc->next_runnable = NULL;
  proc->prev_runnable = g_process_table.run_queue_tail;
  if (g_process_table.run_queue_tail) {
    g_process_table.run_queue_tail->next_runnable = proc;
  } else {
    g_process_table.run_queue_head = proc;
  }
  g_process_table.run_queue_tail = proc;
}

static void proc_runq_remove(struct GuestProcess *proc) {
  if (proc->prev_runnable) {
    proc->prev_runnable->next_runnable = proc->next_runnable;
  } else if (g_process_table.run_queue_head == proc) {
    g_process_table.run_queue_head = proc->next_runnable;
  }
  if (proc->next_runnable) {
    proc->next_runnable->prev_runnable = proc->prev_runnable;
  } else if (g_process_table.run_queue_tail == proc) {
    g_process_table.run_queue_tail = proc->prev_runnable;
  }
  proc->next_runnable = NULL;
  proc->prev_runnable = NULL;
}

/*
 * Allocate a new process slot.
 * - Enforces PPID validity: ppid must be 0 (init) or belong to an existing process.
 * - Allocated process starts in GUEST_PROC_CREATING state.
 * - Machine pointer is initially NULL until explicitly attached.
 */
struct GuestProcess *guest_proc_alloc(pid_t ppid) {
  int i;
  pid_t pid;
  struct GuestProcess *proc;

  if (!g_process_table.initialized) {
    guest_proc_table_init();
  }

  if (g_process_table.count >= MAX_GUEST_PROCESSES) {
    return NULL; /* Table capacity exceeded */
  }

  /* Negative PPID is strictly invalid */
  if (ppid < 0) {
    return NULL;
  }

  /* Non-zero PPID must resolve to an existing active/zombie parent */
  if (ppid > 0) {
    struct GuestProcess *parent = guest_proc_find(ppid);
    if (!parent || parent->state == GUEST_PROC_FREE || parent->state == GUEST_PROC_REAPED) {
      return NULL; /* Orphaned/invalid parent reference */
    }
  }

  pid = guest_proc_alloc_pid();
  if (pid <= 0) {
    return NULL; /* PID allocation failed */
  }

  for (i = 0; i < MAX_GUEST_PROCESSES; i++) {
    if (g_process_table.procs[i].state == GUEST_PROC_FREE) {
      proc = &g_process_table.procs[i];
      memset(proc, 0, sizeof(*proc));
      proc->pid = pid;
      proc->ppid = ppid;
      proc->state = GUEST_PROC_CREATING;
      proc->block_reason = BLOCK_NONE;
      proc->machine = NULL;
      proc->instruction_budget = GUEST_QUANTUM_INSTRUCTIONS;
      proc_runq_append(proc);
      g_process_table.count++;
      return proc;
    }
  }

  return NULL;
}

/*
 * Bootstrap helper for the initial guest execution context.
 * Binds the initial Machine to PID 1 (or designated initial PID).
 */
struct GuestProcess *guest_proc_init_first(struct Machine *m, pid_t pid) {
  struct GuestProcess *proc;

  if (pid <= 0) return NULL;

  if (!g_process_table.initialized) {
    guest_proc_table_init();
  }

  /* Check for PID collision */
  if (guest_proc_find(pid) != NULL) {
    return NULL;
  }

  if (g_process_table.count >= MAX_GUEST_PROCESSES) {
    return NULL;
  }

  /* Use slot 0 if available */
  proc = &g_process_table.procs[0];
  if (proc->state != GUEST_PROC_FREE) {
    proc = guest_proc_alloc(0);
    if (!proc) return NULL;
  } else {
    memset(proc, 0, sizeof(*proc));
    proc->pid = pid;
    proc->ppid = 0;
    proc->instruction_budget = GUEST_QUANTUM_INSTRUCTIONS;
    proc_runq_append(proc);
    g_process_table.count++;
  }

  proc->state = GUEST_PROC_RUNNABLE;
  proc->block_reason = BLOCK_NONE;
  proc->machine = m;

  if (g_process_table.next_pid <= pid) {
    g_process_table.next_pid = pid + 1;
  }

  g_process_table.current = proc;
  return proc;
}
/*
 * B2 fork: create runnable child with deep-copied Machine continuation.
 * - Allocates child GuestProcess (auto-enqueued, CREATING state).
 * - NewMachine(system, parent) deep-copies registers, IP, FPU, segments.
 * - Child RSP set to child_stack when non-zero (clone); otherwise inherits.
 * - Child RAX forced to 0 (fork child return value).
 * - Child IP advanced past trapping syscall instruction (oplen).
 * - Child attached (CREATING->RUNNABLE), parent resumes normally.
 * - Parent return value (child PID) is set by caller via SysFork return.
 * Returns child PID (>0) on success, -1 with errno=EAGAIN/ENOMEM on failure.
 * NOTE: private memory is still shared with parent until B4 eager-copy lands.
 */
pid_t guest_proc_fork(struct Machine *parent_m, u64 child_stack) {
  struct GuestProcess *parent_proc = NULL;
  struct GuestProcess *child_proc = NULL;
  struct Machine *child_m = NULL;
  int i;

  if (!parent_m || !parent_m->system) {
    errno = EINVAL;
    return -1;
  }

  /* Resolve parent proc: prefer current, fall back to machine-pointer scan */
  if (g_process_table.current &&
      g_process_table.current->machine == parent_m) {
    parent_proc = g_process_table.current;
  } else {
    for (i = 0; i < MAX_GUEST_PROCESSES; i++) {
      struct GuestProcess *p = &g_process_table.procs[i];
      if (p->state != GUEST_PROC_FREE && p->state != GUEST_PROC_REAPED &&
          p->machine == parent_m) {
        parent_proc = p;
        break;
      }
    }
  }
  if (!parent_proc) {
    errno = ESRCH;
    return -1;
  }

  child_proc = guest_proc_alloc(parent_proc->pid);
  if (!child_proc) {
    ERRF("GUEST-FORK-FAIL parent=%d reason=alloc", parent_proc->pid);
    errno = EAGAIN;
    return -1;
  }

  struct System *child_s = CloneSystemForFork(parent_m->system);
  if (!child_s) {
    proc_runq_remove(child_proc);
    child_proc->state = GUEST_PROC_FREE;
    child_proc->pid = 0;
    g_process_table.count--;
    errno = ENOMEM;
    return -1;
  }
  if (DeepCopyPageTables(child_s, parent_m->system) != 0) {
    FreeSystem(child_s);
    proc_runq_remove(child_proc);
    child_proc->state = GUEST_PROC_FREE;
    child_proc->pid = 0;
    g_process_table.count--;
    errno = ENOMEM;
    return -1;
  }
  child_m = NewMachine(child_s, parent_m);
  if (!child_m) {
    FreeSystem(child_s);
    proc_runq_remove(child_proc);
    child_proc->state = GUEST_PROC_FREE;
    child_proc->pid = 0;
    g_process_table.count--;
    errno = ENOMEM;
    return -1;
  }

  if (child_stack != 0) {
    Put64(child_m->sp, child_stack);
  }

  /* Child observes fork() returning 0, resuming after the trap */
  Put64(child_m->ax, 0);
  child_m->ip += child_m->oplen;

  ERRF("GUEST-FORK parent=%d -> child=%d", parent_proc->pid, child_proc->pid);
  if (guest_proc_attach_machine(child_proc, child_m) != 0) {
    proc_runq_remove(child_proc);
    child_proc->state = GUEST_PROC_FREE;
    child_proc->pid = 0;
    g_process_table.count--;
    errno = ENOMEM;
    return -1;
  }

  return child_proc->pid;
}


/*
 * Lookup active process by PID.
 * Returns NULL if not found, or if slot is FREE or REAPED.
 */
struct GuestProcess *guest_proc_find(pid_t pid) {
  int i;
  if (pid <= 0) return NULL;

  for (i = 0; i < MAX_GUEST_PROCESSES; i++) {
    struct GuestProcess *p = &g_process_table.procs[i];
    if (p->state != GUEST_PROC_FREE && p->state != GUEST_PROC_REAPED && p->pid == pid) {
      return p;
    }
  }
  return NULL;
}

/*
 * Lookup parent process.
 */
struct GuestProcess *guest_proc_find_parent(const struct GuestProcess *proc) {
  if (!proc || proc->ppid <= 0) return NULL;
  return guest_proc_find(proc->ppid);
}

/*
 * Enumerate direct children of a parent process.
 */
int guest_proc_get_children(const struct GuestProcess *parent,
                            struct GuestProcess **out_children,
                            int max_children) {
  int i, found = 0;
  if (!parent || parent->pid <= 0) return 0;

  for (i = 0; i < MAX_GUEST_PROCESSES; i++) {
    struct GuestProcess *p = &g_process_table.procs[i];
    if (p->state != GUEST_PROC_FREE && p->state != GUEST_PROC_REAPED && p->ppid == parent->pid) {
      if (out_children && found < max_children) {
        out_children[found] = p;
      }
      found++;
    }
  }
  return found;
}

/*
 * Machine Ownership Boundary:
 * Attach a Machine execution context to a GuestProcess.
 * Invariants:
 * - proc must be valid and currently have NO attached machine.
 * - m must not already be attached to any other active GuestProcess.
 */
int guest_proc_attach_machine(struct GuestProcess *proc, struct Machine *m) {
  int i;
  if (!proc || !m) return -EINVAL;
  if (proc->machine != NULL) return -EEXIST; /* Already attached */

  /* Ensure machine is not aliased by another process */
  for (i = 0; i < MAX_GUEST_PROCESSES; i++) {
    struct GuestProcess *p = &g_process_table.procs[i];
    if (p->state != GUEST_PROC_FREE && p->state != GUEST_PROC_REAPED && p->machine == m) {
      return -EBUSY; /* Machine already bound to another process */
    }
  }

  proc->machine = m;
  if (proc->state == GUEST_PROC_CREATING) {
    proc->state = GUEST_PROC_RUNNABLE;
  }
  return 0;
}

/*
 * Detach Machine from GuestProcess.
 * Returns detached pointer, sets proc->machine = NULL.
 */
struct Machine *guest_proc_detach_machine(struct GuestProcess *proc) {
  struct Machine *m;
  if (!proc || !proc->machine) return NULL;
  m = proc->machine;
  proc->machine = NULL;
  return m;
}

/*
 * Legal lifecycle transition validator.
 * Enforces fail-closed transitions centrally.
 */
bool guest_proc_is_valid_transition(GuestProcState from, GuestProcState to) {
  switch (from) {
    case GUEST_PROC_CREATING:
      return (to == GUEST_PROC_RUNNABLE || to == GUEST_PROC_EXITED);

    case GUEST_PROC_RUNNABLE:
      return (to == GUEST_PROC_BLOCKED || to == GUEST_PROC_EXITED || to == GUEST_PROC_ZOMBIE);

    case GUEST_PROC_BLOCKED:
      return (to == GUEST_PROC_RUNNABLE || to == GUEST_PROC_EXITED || to == GUEST_PROC_ZOMBIE);

    case GUEST_PROC_EXITED:
      return (to == GUEST_PROC_ZOMBIE);

    case GUEST_PROC_ZOMBIE:
      return (to == GUEST_PROC_REAPED);

    case GUEST_PROC_REAPED:
      return (to == GUEST_PROC_FREE);

    case GUEST_PROC_FREE:
      return (to == GUEST_PROC_CREATING || to == GUEST_PROC_RUNNABLE);

    default:
      return false;
  }
}

/*
 * Generic state transition with strict validation.
 */
int guest_proc_transition(struct GuestProcess *proc, GuestProcState new_state) {
  GuestProcState old_state;
  if (!proc) return -EINVAL;
  if (!guest_proc_is_valid_transition(proc->state, new_state)) {
    return -EPERM; /* Illegal state transition rejected */
  }
  
  old_state = proc->state;
  proc->state = new_state;
  
  /* B8: Manage run queue membership based on state transitions */
  if (old_state == GUEST_PROC_RUNNABLE && new_state != GUEST_PROC_RUNNABLE) {
    proc_runq_remove(proc);
  } else if (new_state == GUEST_PROC_RUNNABLE && old_state != GUEST_PROC_RUNNABLE) {
    proc_runq_append(proc);
  }
  
  return 0;
}

/*
 * Process exit implementation:
 * - Records exit status.
 * - Releases/detaches execution Machine state (CPU continuation torn down).
 * - Transitions to GUEST_PROC_ZOMBIE.
 * - Links into parent's zombie chain.
 */
int guest_proc_exit(struct GuestProcess *proc, int status) {
  struct GuestProcess *parent;

  if (!proc) return -EINVAL;
  if (proc->state == GUEST_PROC_FREE || proc->state == GUEST_PROC_ZOMBIE || proc->state == GUEST_PROC_REAPED) {
    return -ESRCH; /* Process not in an exitable state */
  }

  proc->exit_status = status;
  proc->child_exited = true;

  /* Detach Machine: CPU execution context ends, process identity remains */
  proc->machine = NULL;

  /* Transition to zombie (auto-removes from run queue if RUNNABLE) */
  guest_proc_transition(proc, GUEST_PROC_ZOMBIE);
  proc->block_reason = BLOCK_NONE;

  /* Link into parent's zombie list if parent exists */
  parent = guest_proc_find_parent(proc);
  if (parent) {
    proc->next_zombie = parent->next_zombie;
    parent->next_zombie = proc;
  }

  /* B8: Wake any processes blocked in wait4() waiting for this child */
  {
    int i;
    for (i = 0; i < MAX_GUEST_PROCESSES; i++) {
      struct GuestProcess *waiter = &g_process_table.procs[i];
      if (waiter->state == GUEST_PROC_BLOCKED &&
          waiter->block_reason == BLOCK_WAIT4 &&
          (waiter->block_pid == -1 || waiter->block_pid == proc->pid)) {
        guest_proc_transition(waiter, GUEST_PROC_RUNNABLE);
        waiter->block_reason = BLOCK_NONE;
        waiter->block_pid = 0;
      }
    }
  }

  /* Clear current if the exiting process was active */
  if (g_process_table.current == proc) {
    g_process_table.current = NULL;
  }

  return 0;
}

/*
 * Process reap implementation:
 * - Only ZOMBIE processes may be reaped.
 * - Unlinks from parent's zombie chain.
 * - Transitions to GUEST_PROC_REAPED then GUEST_PROC_FREE.
 * - Frees process slot and decrements table count.
 */
int guest_proc_reap(struct GuestProcess *proc) {
  struct GuestProcess *parent;

  if (!proc) return -EINVAL;
  if (proc->state != GUEST_PROC_ZOMBIE) {
    return -EINVAL; /* Cannot reap a non-zombie or already reaped process */
  }

  /* Unlink from parent zombie list */
  parent = guest_proc_find_parent(proc);
  if (parent && parent->next_zombie) {
    if (parent->next_zombie == proc) {
      parent->next_zombie = proc->next_zombie;
    } else {
      struct GuestProcess *curr = parent->next_zombie;
      while (curr && curr->next_zombie) {
        if (curr->next_zombie == proc) {
          curr->next_zombie = proc->next_zombie;
          break;
        }
        curr = curr->next_zombie;
      }
    }
  }

  proc->state = GUEST_PROC_REAPED;
  proc->next_zombie = NULL;

  /* Release table slot for recycling */
  proc_runq_remove(proc);
  proc->state = GUEST_PROC_FREE;
  proc->pid = 0;
  proc->ppid = 0;
  proc->exit_status = 0;
  proc->child_exited = false;
  g_process_table.count--;

  return 0;
}

/*
 * Diagnostic introspection
 */
const char *guest_proc_state_name(GuestProcState state) {
  switch (state) {
    case GUEST_PROC_FREE:     return "FREE";
    case GUEST_PROC_CREATING: return "CREATING";
    case GUEST_PROC_RUNNABLE: return "RUNNABLE";
    case GUEST_PROC_BLOCKED:  return "BLOCKED";
    case GUEST_PROC_EXITED:   return "EXITED";
    case GUEST_PROC_ZOMBIE:   return "ZOMBIE";
    case GUEST_PROC_REAPED:   return "REAPED";
    default:                  return "UNKNOWN";
  }
}

int guest_proc_live_count(void) {
  int i, live = 0;
  for (i = 0; i < MAX_GUEST_PROCESSES; i++) {
    GuestProcState s = g_process_table.procs[i].state;
    if (s == GUEST_PROC_CREATING || s == GUEST_PROC_RUNNABLE || s == GUEST_PROC_BLOCKED) {
      live++;
    }
  }
  return live;
}

int guest_proc_zombie_count(void) {
  int i, zombies = 0;
  for (i = 0; i < MAX_GUEST_PROCESSES; i++) {
    if (g_process_table.procs[i].state == GUEST_PROC_ZOMBIE) {
      zombies++;
    }
  }
  return zombies;
}
