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

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT(expr, msg) do { \
  if (!(expr)) { \
    fprintf(stderr, "  FAIL: %s (line %d): %s\n", __FUNCTION__, __LINE__, msg); \
    g_tests_failed++; \
    return false; \
  } \
} while (0)

#define RUN_TEST(fn) do { \
  g_tests_run++; \
  printf("[TEST %d] %s... ", g_tests_run, #fn); \
  guest_proc_table_reset(); \
  if (fn()) { \
    printf("PASS\n"); \
    g_tests_passed++; \
  } else { \
    printf("FAILED\n"); \
  } \
} while (0)

/* Dummy Machine instances for ownership boundary testing */
static char s_dummy_buf_1[64];
static char s_dummy_buf_2[64];
#define DUMMY_M1 ((struct Machine *)s_dummy_buf_1)
#define DUMMY_M2 ((struct Machine *)s_dummy_buf_2)

/* =========================================================================
 * POSITIVE SUITE: B1-T1 through B1-T9
 * ========================================================================= */

/* B1-T1: Initial process registration */
static bool test_b1_t1_initial_registration(void) {
  struct GuestProcess *p = guest_proc_init_first(DUMMY_M1, 1);
  TEST_ASSERT(p != NULL, "init_first returned NULL");
  TEST_ASSERT(p->pid == 1, "initial PID != 1");
  TEST_ASSERT(p->ppid == 0, "initial PPID != 0");
  TEST_ASSERT(p->state == GUEST_PROC_RUNNABLE, "initial state != RUNNABLE");
  TEST_ASSERT(p->machine == DUMMY_M1, "machine binding mismatch");
  TEST_ASSERT(g_process_table.current == p, "g_process_table.current != p");
  TEST_ASSERT(g_process_table.count == 1, "table count != 1");
  TEST_ASSERT(guest_proc_find(1) == p, "lookup(1) failed");
  return true;
}

/* B1-T2: Unique PID allocation across multiple processes */
static bool test_b1_t2_unique_pid_allocation(void) {
  struct GuestProcess *init = guest_proc_init_first(DUMMY_M1, 1);
  TEST_ASSERT(init != NULL, "init failed");

  #define N_PROCS 30
  struct GuestProcess *procs[N_PROCS];
  int i, j;

  for (i = 0; i < N_PROCS; i++) {
    procs[i] = guest_proc_alloc(1);
    TEST_ASSERT(procs[i] != NULL, "failed to allocate child process");
    TEST_ASSERT(procs[i]->pid > 1, "PID must be > 1");
    TEST_ASSERT(procs[i]->ppid == 1, "PPID must be 1");
    TEST_ASSERT(procs[i]->state == GUEST_PROC_CREATING, "state must be CREATING");
  }

  /* Verify all PIDs are unique */
  for (i = 0; i < N_PROCS; i++) {
    for (j = i + 1; j < N_PROCS; j++) {
      TEST_ASSERT(procs[i]->pid != procs[j]->pid, "duplicate PID detected");
    }
    /* Exact lookup */
    TEST_ASSERT(guest_proc_find(procs[i]->pid) == procs[i], "exact lookup failed");
  }

  TEST_ASSERT(g_process_table.count == N_PROCS + 1, "count mismatch");
  return true;
}

/* B1-T3: Parent/child relationship representation */
static bool test_b1_t3_parent_child_metadata(void) {
  struct GuestProcess *parent = guest_proc_init_first(DUMMY_M1, 1);
  struct GuestProcess *child1 = guest_proc_alloc(1);
  struct GuestProcess *child2 = guest_proc_alloc(1);
  struct GuestProcess *children[4];

  TEST_ASSERT(parent != NULL && child1 != NULL && child2 != NULL, "setup failed");
  TEST_ASSERT(child1->ppid == parent->pid, "child1 ppid != parent pid");
  TEST_ASSERT(child2->ppid == parent->pid, "child2 ppid != parent pid");

  TEST_ASSERT(guest_proc_find_parent(child1) == parent, "find_parent(child1) failed");
  TEST_ASSERT(guest_proc_find_parent(child2) == parent, "find_parent(child2) failed");
  TEST_ASSERT(guest_proc_find_parent(parent) == NULL, "find_parent(init) must be NULL");

  int n = guest_proc_get_children(parent, children, 4);
  TEST_ASSERT(n == 2, "get_children count != 2");
  TEST_ASSERT((children[0] == child1 && children[1] == child2) ||
              (children[0] == child2 && children[1] == child1),
              "get_children returned incorrect processes");
  return true;
}

/* B1-T4: Duplicate PID rejection */
static bool test_b1_t4_duplicate_pid_rejection(void) {
  struct GuestProcess *p1 = guest_proc_init_first(DUMMY_M1, 1);
  TEST_ASSERT(p1 != NULL, "p1 init failed");

  /* Attempting to initialize another process with PID 1 must fail closed */
  struct GuestProcess *p_dup = guest_proc_init_first(DUMMY_M2, 1);
  TEST_ASSERT(p_dup == NULL, "duplicate PID 1 allowed");
  TEST_ASSERT(g_process_table.count == 1, "count must remain 1");
  return true;
}

/* B1-T5: Lifecycle legal transitions */
static bool test_b1_t5_lifecycle_legal_transitions(void) {
  struct GuestProcess *p = guest_proc_alloc(0);
  TEST_ASSERT(p != NULL, "alloc failed");
  TEST_ASSERT(p->state == GUEST_PROC_CREATING, "initial state != CREATING");

  /* CREATING -> RUNNABLE via attach_machine */
  int rc = guest_proc_attach_machine(p, DUMMY_M1);
  TEST_ASSERT(rc == 0, "attach_machine failed");
  TEST_ASSERT(p->state == GUEST_PROC_RUNNABLE, "state != RUNNABLE");

  /* RUNNABLE -> BLOCKED */
  rc = guest_proc_transition(p, GUEST_PROC_BLOCKED);
  TEST_ASSERT(rc == 0, "RUNNABLE -> BLOCKED failed");
  TEST_ASSERT(p->state == GUEST_PROC_BLOCKED, "state != BLOCKED");

  /* BLOCKED -> RUNNABLE */
  rc = guest_proc_transition(p, GUEST_PROC_RUNNABLE);
  TEST_ASSERT(rc == 0, "BLOCKED -> RUNNABLE failed");
  TEST_ASSERT(p->state == GUEST_PROC_RUNNABLE, "state != RUNNABLE");

  /* RUNNABLE -> EXITED */
  rc = guest_proc_transition(p, GUEST_PROC_EXITED);
  TEST_ASSERT(rc == 0, "RUNNABLE -> EXITED failed");
  TEST_ASSERT(p->state == GUEST_PROC_EXITED, "state != EXITED");

  /* EXITED -> ZOMBIE */
  rc = guest_proc_transition(p, GUEST_PROC_ZOMBIE);
  TEST_ASSERT(rc == 0, "EXITED -> ZOMBIE failed");
  TEST_ASSERT(p->state == GUEST_PROC_ZOMBIE, "state != ZOMBIE");

  /* ZOMBIE -> REAPED */
  rc = guest_proc_transition(p, GUEST_PROC_REAPED);
  TEST_ASSERT(rc == 0, "ZOMBIE -> REAPED failed");
  TEST_ASSERT(p->state == GUEST_PROC_REAPED, "state != REAPED");

  /* REAPED -> FREE */
  rc = guest_proc_transition(p, GUEST_PROC_FREE);
  TEST_ASSERT(rc == 0, "REAPED -> FREE failed");
  TEST_ASSERT(p->state == GUEST_PROC_FREE, "state != FREE");
  return true;
}

/* B1-T6: Lifecycle illegal transitions rejection */
static bool test_b1_t6_lifecycle_illegal_transitions(void) {
  struct GuestProcess *p = guest_proc_alloc(0);
  guest_proc_attach_machine(p, DUMMY_M1);
  TEST_ASSERT(p->state == GUEST_PROC_RUNNABLE, "state != RUNNABLE");

  /* RUNNABLE -> REAPED (illegal, cannot bypass exit/zombie) */
  TEST_ASSERT(!guest_proc_is_valid_transition(GUEST_PROC_RUNNABLE, GUEST_PROC_REAPED), "RUNNABLE -> REAPED allowed");
  TEST_ASSERT(guest_proc_transition(p, GUEST_PROC_REAPED) != 0, "transition succeeded");

  /* RUNNABLE -> FREE (illegal) */
  TEST_ASSERT(!guest_proc_is_valid_transition(GUEST_PROC_RUNNABLE, GUEST_PROC_FREE), "RUNNABLE -> FREE allowed");
  TEST_ASSERT(guest_proc_transition(p, GUEST_PROC_FREE) != 0, "transition succeeded");

  /* Transition to ZOMBIE */
  guest_proc_exit(p, 0);
  TEST_ASSERT(p->state == GUEST_PROC_ZOMBIE, "state != ZOMBIE");

  /* ZOMBIE -> RUNNABLE (illegal resurrection) */
  TEST_ASSERT(!guest_proc_is_valid_transition(GUEST_PROC_ZOMBIE, GUEST_PROC_RUNNABLE), "ZOMBIE -> RUNNABLE allowed");
  TEST_ASSERT(guest_proc_transition(p, GUEST_PROC_RUNNABLE) != 0, "resurrection succeeded");

  /* ZOMBIE -> BLOCKED (illegal) */
  TEST_ASSERT(!guest_proc_is_valid_transition(GUEST_PROC_ZOMBIE, GUEST_PROC_BLOCKED), "ZOMBIE -> BLOCKED allowed");
  TEST_ASSERT(guest_proc_transition(p, GUEST_PROC_BLOCKED) != 0, "transition succeeded");

  /* ZOMBIE -> ZOMBIE (illegal) */
  TEST_ASSERT(!guest_proc_is_valid_transition(GUEST_PROC_ZOMBIE, GUEST_PROC_ZOMBIE), "ZOMBIE -> ZOMBIE allowed");
  TEST_ASSERT(guest_proc_transition(p, GUEST_PROC_ZOMBIE) != 0, "transition succeeded");

  /* Transition to REAPED */
  guest_proc_transition(p, GUEST_PROC_REAPED);
  TEST_ASSERT(p->state == GUEST_PROC_REAPED, "state != REAPED");

  /* REAPED -> RUNNABLE (illegal) */
  TEST_ASSERT(!guest_proc_is_valid_transition(GUEST_PROC_REAPED, GUEST_PROC_RUNNABLE), "REAPED -> RUNNABLE allowed");
  TEST_ASSERT(guest_proc_transition(p, GUEST_PROC_RUNNABLE) != 0, "transition succeeded");

  /* REAPED -> ZOMBIE (illegal) */
  TEST_ASSERT(!guest_proc_is_valid_transition(GUEST_PROC_REAPED, GUEST_PROC_ZOMBIE), "REAPED -> ZOMBIE allowed");
  TEST_ASSERT(guest_proc_transition(p, GUEST_PROC_ZOMBIE) != 0, "transition succeeded");
  return true;
}

/* B1-T7: Zombie retention */
static bool test_b1_t7_zombie_retention(void) {
  struct GuestProcess *parent = guest_proc_init_first(DUMMY_M1, 1);
  struct GuestProcess *child = guest_proc_alloc(1);
  guest_proc_attach_machine(child, DUMMY_M2);
  pid_t child_pid = child->pid;

  /* Child exits with status 42 */
  int rc = guest_proc_exit(child, 42);
  TEST_ASSERT(rc == 0, "exit failed");

  /* Invariants:
   * - Record remains in ProcessTable
   * - PID remains reserved (findable)
   * - State is ZOMBIE
   * - Exit status is retained
   * - Child is on parent's zombie list
   */
  TEST_ASSERT(child->state == GUEST_PROC_ZOMBIE, "state != ZOMBIE");
  TEST_ASSERT(child->exit_status == 42, "exit_status != 42");
  TEST_ASSERT(guest_proc_find(child_pid) == child, "zombie not found by PID");
  TEST_ASSERT(parent->next_zombie == child, "parent zombie link missing");
  TEST_ASSERT(guest_proc_zombie_count() == 1, "zombie_count != 1");
  return true;
}

/* B1-T8: Reap transition */
static bool test_b1_t8_reap_transition(void) {
  struct GuestProcess *parent = guest_proc_init_first(DUMMY_M1, 1);
  struct GuestProcess *child = guest_proc_alloc(1);
  guest_proc_attach_machine(child, DUMMY_M2);
  pid_t child_pid = child->pid;

  guest_proc_exit(child, 0);
  TEST_ASSERT(child->state == GUEST_PROC_ZOMBIE, "must be ZOMBIE");

  int prev_count = g_process_table.count;
  int rc = guest_proc_reap(child);
  TEST_ASSERT(rc == 0, "reap failed");

  /* Invariants:
   * - Record unlinked and freed
   * - Lookup returns NULL
   * - Table count decremented
   * - PID becomes eligible for allocation
   */
  TEST_ASSERT(guest_proc_find(child_pid) == NULL, "reaped process still findable");
  TEST_ASSERT(parent->next_zombie == NULL, "zombie still in parent list");
  TEST_ASSERT(g_process_table.count == prev_count - 1, "count not decremented");
  return true;
}

/* B1-T9: Machine ownership boundary & lifetime */
static bool test_b1_t9_machine_lifetime(void) {
  struct GuestProcess *proc = guest_proc_alloc(0);
  TEST_ASSERT(proc != NULL, "alloc failed");
  TEST_ASSERT(proc->machine == NULL, "new proc must have NULL machine");

  /* Attach */
  int rc = guest_proc_attach_machine(proc, DUMMY_M1);
  TEST_ASSERT(rc == 0, "attach failed");
  TEST_ASSERT(proc->machine == DUMMY_M1, "machine pointer mismatch");

  /* Detach */
  struct Machine *detached = guest_proc_detach_machine(proc);
  TEST_ASSERT(detached == DUMMY_M1, "detached mismatch");
  TEST_ASSERT(proc->machine == NULL, "machine not cleared");

  /* Re-attach */
  rc = guest_proc_attach_machine(proc, DUMMY_M1);
  TEST_ASSERT(rc == 0, "re-attach failed");

  /* Process exit automatically detaches machine */
  guest_proc_exit(proc, 0);
  TEST_ASSERT(proc->state == GUEST_PROC_ZOMBIE, "state != ZOMBIE");
  TEST_ASSERT(proc->machine == NULL, "exit did not detach machine pointer");

  /* No dangling pointer remains */
  return true;
}

/* =========================================================================
 * NEGATIVE CONTROLS
 * ========================================================================= */

/* NC-1: Unknown PID lookup returns NULL */
static bool test_nc1_unknown_pid_lookup(void) {
  guest_proc_init_first(DUMMY_M1, 1);
  TEST_ASSERT(guest_proc_find(99999) == NULL, "lookup(99999) must be NULL");
  TEST_ASSERT(guest_proc_find(0) == NULL, "lookup(0) must be NULL");
  TEST_ASSERT(guest_proc_find(-5) == NULL, "lookup(-5) must be NULL");
  return true;
}

/* NC-2: Invalid PPID fails closed */
static bool test_nc2_invalid_ppid_fails_closed(void) {
  guest_proc_init_first(DUMMY_M1, 1);
  /* PPID -1 is forbidden */
  TEST_ASSERT(guest_proc_alloc(-1) == NULL, "alloc(-1) must return NULL");
  /* Non-existent parent PID 8888 fails closed */
  TEST_ASSERT(guest_proc_alloc(8888) == NULL, "alloc(8888) must return NULL");
  return true;
}

/* NC-3: Machine double attachment to same process fails closed */
static bool test_nc3_double_machine_attach_fails(void) {
  struct GuestProcess *p = guest_proc_alloc(0);
  TEST_ASSERT(guest_proc_attach_machine(p, DUMMY_M1) == 0, "first attach failed");
  /* Second attach must return -EEXIST */
  int rc = guest_proc_attach_machine(p, DUMMY_M2);
  TEST_ASSERT(rc == -EEXIST, "double attach did not return -EEXIST");
  TEST_ASSERT(p->machine == DUMMY_M1, "machine overwritten");
  return true;
}

/* NC-4: Machine attached to two distinct processes fails closed */
static bool test_nc4_machine_aliasing_fails(void) {
  struct GuestProcess *p1 = guest_proc_alloc(0);
  struct GuestProcess *p2 = guest_proc_alloc(0);
  TEST_ASSERT(guest_proc_attach_machine(p1, DUMMY_M1) == 0, "p1 attach failed");
  /* Attaching DUMMY_M1 to p2 must return -EBUSY */
  int rc = guest_proc_attach_machine(p2, DUMMY_M1);
  TEST_ASSERT(rc == -EBUSY, "machine aliasing did not return -EBUSY");
  TEST_ASSERT(p2->machine == NULL, "p2 acquired aliased machine");
  return true;
}

/* NC-5: Reap live process fails closed */
static bool test_nc5_reap_live_process_fails(void) {
  struct GuestProcess *p = guest_proc_alloc(0);
  guest_proc_attach_machine(p, DUMMY_M1);
  TEST_ASSERT(p->state == GUEST_PROC_RUNNABLE, "state != RUNNABLE");

  int rc = guest_proc_reap(p);
  TEST_ASSERT(rc == -EINVAL, "reap of live process did not return -EINVAL");
  TEST_ASSERT(p->state == GUEST_PROC_RUNNABLE, "live process corrupted by reap");
  TEST_ASSERT(guest_proc_find(p->pid) == p, "process disappeared");
  return true;
}

/* NC-6: Double reap fails closed */
static bool test_nc6_double_reap_fails(void) {
  struct GuestProcess *p = guest_proc_alloc(0);
  guest_proc_attach_machine(p, DUMMY_M1);
  guest_proc_exit(p, 0);
  TEST_ASSERT(p->state == GUEST_PROC_ZOMBIE, "state != ZOMBIE");

  TEST_ASSERT(guest_proc_reap(p) == 0, "first reap failed");
  /* Second reap must return -EINVAL */
  TEST_ASSERT(guest_proc_reap(p) == -EINVAL, "second reap succeeded");
  return true;
}

/* NC-7: Table capacity exhaustion fails closed */
static bool test_nc7_table_capacity_exhaustion(void) {
  int i;
  for (i = 0; i < MAX_GUEST_PROCESSES; i++) {
    struct GuestProcess *p = guest_proc_alloc(0);
    TEST_ASSERT(p != NULL, "failed to fill process table");
  }
  TEST_ASSERT(g_process_table.count == MAX_GUEST_PROCESSES, "count != MAX");

  /* Requesting 257th process must return NULL (fail closed) */
  struct GuestProcess *overflow = guest_proc_alloc(0);
  TEST_ASSERT(overflow == NULL, "table overflow permitted");
  TEST_ASSERT(g_process_table.count == MAX_GUEST_PROCESSES, "count exceeded MAX");
  return true;
}

/* NC-8: Zombie PID is not recycled before reap */
static bool test_nc8_zombie_pid_not_recycled_before_reap(void) {
  struct GuestProcess *p1 = guest_proc_alloc(0);
  pid_t p1_pid = p1->pid;
  guest_proc_attach_machine(p1, DUMMY_M1);
  guest_proc_exit(p1, 0);
  TEST_ASSERT(p1->state == GUEST_PROC_ZOMBIE, "state != ZOMBIE");

  /* Allocate new processes; none must get p1_pid */
  int i;
  for (i = 0; i < 10; i++) {
    struct GuestProcess *child = guest_proc_alloc(0);
    TEST_ASSERT(child != NULL, "alloc failed");
    TEST_ASSERT(child->pid != p1_pid, "zombie PID was prematurely reallocated");
  }
  return true;
}

/* NC-9: PID allocator fails closed on exhaustion */
static bool test_nc9_pid_allocator_exhaustion(void) {
  /* Fill table */
  int i;
  for (i = 0; i < MAX_GUEST_PROCESSES; i++) {
    struct GuestProcess *p = guest_proc_alloc(0);
    TEST_ASSERT(p != NULL, "fill table failed");
  }
  /* Next allocation attempt must return 0 */
  pid_t pid = guest_proc_alloc_pid();
  TEST_ASSERT(pid == 0, "alloc_pid did not fail closed on full table");
  return true;
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  printf("====================================================\n");
  printf("M1R-T6-B1: GuestProcess + ProcessTable Substrate Test\n");
  printf("====================================================\n");

  printf("\n--- POSITIVE CONTROLS (B1-T1 to B1-T9) ---\n");
  RUN_TEST(test_b1_t1_initial_registration);
  RUN_TEST(test_b1_t2_unique_pid_allocation);
  RUN_TEST(test_b1_t3_parent_child_metadata);
  RUN_TEST(test_b1_t4_duplicate_pid_rejection);
  RUN_TEST(test_b1_t5_lifecycle_legal_transitions);
  RUN_TEST(test_b1_t6_lifecycle_illegal_transitions);
  RUN_TEST(test_b1_t7_zombie_retention);
  RUN_TEST(test_b1_t8_reap_transition);
  RUN_TEST(test_b1_t9_machine_lifetime);

  printf("\n--- NEGATIVE CONTROLS (NC-1 to NC-9) ---\n");
  RUN_TEST(test_nc1_unknown_pid_lookup);
  RUN_TEST(test_nc2_invalid_ppid_fails_closed);
  RUN_TEST(test_nc3_double_machine_attach_fails);
  RUN_TEST(test_nc4_machine_aliasing_fails);
  RUN_TEST(test_nc5_reap_live_process_fails);
  RUN_TEST(test_nc6_double_reap_fails);
  RUN_TEST(test_nc7_table_capacity_exhaustion);
  RUN_TEST(test_nc8_zombie_pid_not_recycled_before_reap);
  RUN_TEST(test_nc9_pid_allocator_exhaustion);

  printf("\n====================================================\n");
  printf("TEST SUMMARY: %d run, %d passed, %d failed\n",
         g_tests_run, g_tests_passed, g_tests_failed);
  printf("====================================================\n");

  return (g_tests_failed == 0) ? 0 : 1;
}
