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
#ifndef BLINK_GUEST_PROCESS_H_
#define BLINK_GUEST_PROCESS_H_

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "blink/types.h"

/*
 * M1R-T6-B1 — GuestProcess + ProcessTable substrate.
 *
 * Epistemic contract:
 * - Represents multi-process Unix process hierarchy in Blink.
 * - Establishes explicit ownership boundary: GuestProcess owns/references Machine.
 * - Does NOT implement fork() execution continuation or copy-on-write memory (B2-B4).
 */

/* Process lifecycle states (Fail-closed state machine) */
typedef enum {
  GUEST_PROC_FREE = 0,    /* Unallocated table slot */
  GUEST_PROC_CREATING,    /* Allocated, awaiting Machine attachment */
  GUEST_PROC_RUNNABLE,    /* Eligible for execution */
  GUEST_PROC_BLOCKED,     /* Waiting on I/O, wait4, signal, or timer */
  GUEST_PROC_EXITED,      /* Called _exit()/exit_group(); pending zombie transition */
  GUEST_PROC_ZOMBIE,      /* Terminated; retains exit status until wait4() reaps */
  GUEST_PROC_REAPED       /* Wait4() completed; marked for slot recycle */
} GuestProcState;

/* Block reason for BLOCKED state (determines unblock condition in future gates) */
typedef enum {
  BLOCK_NONE = 0,
  BLOCK_READ,             /* Waiting for data on fd */
  BLOCK_WRITE,            /* Waiting for write space on fd */
  BLOCK_WAIT4,            /* Waiting for child exit via wait4/waitpid */
  BLOCK_SLEEP,            /* nanosleep/alarm timer */
  BLOCK_SIGNAL,           /* Waiting for specific signal delivery */
  BLOCK_PIPE,             /* Waiting for pipe data */
  BLOCK_POLL,             /* poll/select with timeout */
  BLOCK_FUTEX             /* futex wait */
} GuestBlockReason;

struct Machine;

/* The core guest Unix process structure */
struct GuestProcess {
  /* === Identity === */
  pid_t pid;                          /* Unique positive guest PID (> 0) */
  pid_t ppid;                         /* Parent PID (0 for initial process) */

  /* === Machine Continuation / Execution Boundary ===
   * In B1, GuestProcess owns / references the Machine.
   * When alive (RUNNABLE / BLOCKED), machine != NULL.
   * When ZOMBIE / REAPED, machine is detached/freed, preserving
   * process identity without retaining execution resources.
   */
  struct Machine *machine;

  /* === Placeholders for future gates (B2-B7) === */
  void *private_mem;                  /* Future B2/B3: struct PrivateMemoryView* */
  void *shared_mappings;              /* Future B4: struct SharedMappingRef* */
  int n_shared_mappings;
  void *fd_table;                     /* Future B5: struct GuestFdTable* */
  void *signal_state;                 /* Future B6: struct GuestSignalState* */

  /* === Lifecycle & Exit Information === */
  GuestProcState state;
  GuestBlockReason block_reason;
  int block_fd;                      /* B7: host/guest fd the process is blocked on */
  int block_pid;                      /* B8: child PID the process is waiting on via wait4 */
  int exit_status;                    /* Raw wstatus for wait4() */
  bool child_exited;                  /* Kernel-side exited flag */

  /* === Scheduling Placeholders (Future B2) === */
  u64 instruction_budget;
  struct GuestProcess *next_runnable;
  struct GuestProcess *prev_runnable;

  /* === Parent/Child / Zombie Hierarchy === */
  struct GuestProcess *next_zombie;   /* Parent's zombie list */
};

#define MAX_GUEST_PROCESSES 256
#define GUEST_PID_MAX       32767
#define GUEST_INITIAL_PID   1

/* Global Process Table */
struct GuestProcessTable {
  struct GuestProcess procs[MAX_GUEST_PROCESSES];
  int count;                          /* Number of active processes (live + zombies) */
  pid_t next_pid;                     /* Monotonic counter for deterministic allocation */
  struct GuestProcess *run_queue_head;
  struct GuestProcess *run_queue_tail;
  struct GuestProcess *current;       /* Currently executing process */
  bool initialized;
};

extern struct GuestProcessTable g_process_table;

/* Process Table Lifecycle API */
void guest_proc_table_init(void);
void guest_proc_table_reset(void);     /* Reset table to pristine state */

/* PID Allocation */
pid_t guest_proc_alloc_pid(void);

/* Process Allocation & Hierarchy */
struct GuestProcess *guest_proc_alloc(pid_t ppid);
struct GuestProcess *guest_proc_init_first(struct Machine *m, pid_t pid);
/* B2 fork: create runnable child, deep-copy Machine; returns child PID or -1 */
pid_t guest_proc_fork(struct Machine *parent_m, u64 child_stack);
struct GuestProcess *guest_proc_find(pid_t pid);
struct GuestProcess *guest_proc_find_parent(const struct GuestProcess *proc);
int guest_proc_get_children(const struct GuestProcess *parent,
                            struct GuestProcess **out_children,
                            int max_children);

/* Machine Ownership Boundary */
int guest_proc_attach_machine(struct GuestProcess *proc, struct Machine *m);
struct Machine *guest_proc_detach_machine(struct GuestProcess *proc);

/* State Transitions (Fail-Closed) */
bool guest_proc_is_valid_transition(GuestProcState from, GuestProcState to);
int guest_proc_transition(struct GuestProcess *proc, GuestProcState new_state);
int guest_proc_exit(struct GuestProcess *proc, int status);
int guest_proc_reap(struct GuestProcess *proc);

/* Introspection */
const char *guest_proc_state_name(GuestProcState state);
int guest_proc_live_count(void);
int guest_proc_zombie_count(void);

#endif /* BLINK_GUEST_PROCESS_H_ */
