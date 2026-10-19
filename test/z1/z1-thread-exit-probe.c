/* SPDX-License-Identifier: MIT
 * Z1 G6 guest-level exit-vs-exit_group regression witness.
 * Build for x86-64 Linux guest, preferably static musl:
 *   x86_64-linux-musl-gcc -static -O2 -pthread -o z1-thread-exit-probe z1-thread-exit-probe.c
 * Positive: ./z1-thread-exit-probe thread -> Z1_THREAD_EXIT_PARENT_ALIVE, exit 0.
 * Negative: ./z1-thread-exit-probe group -> NO parent-alive marker, process exit 37.
 * This intentionally does not pthread_join: that is a separate CHILD_CLEARTID gate.
 */
#if !defined(__linux__) || !defined(__x86_64__)
#error "This regression must be compiled as a Linux x86-64 guest"
#endif
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static _Atomic int entered = 0;
static int group_exit = 0;

static void *child_main(void *ignored) {
  (void)ignored;
  atomic_store_explicit(&entered, 1, memory_order_release);
  if (group_exit) {
    syscall(SYS_exit_group, 37);
  } else {
    syscall(SYS_exit, 0);
  }
  /* Reaching this is a violation: neither syscall returns on success. */
  __builtin_trap();
}

int main(int argc, char **argv) {
  pthread_t thread;
  struct timespec start, now;
  double elapsed;
  int rc;

  if (argc != 2 || (strcmp(argv[1], "thread") && strcmp(argv[1], "group"))) {
    fputs("usage: z1-thread-exit-probe thread|group\n", stderr);
    return 64;
  }
  group_exit = strcmp(argv[1], "group") == 0;
  if (clock_gettime(CLOCK_MONOTONIC, &start)) return 65;
  rc = pthread_create(&thread, NULL, child_main, NULL);
  if (rc) {
    fprintf(stderr, "Z1_THREAD_CREATE_FAILED %d\n", rc);
    return 66;
  }

  do {
    if (atomic_load_explicit(&entered, memory_order_acquire)) {
      if (!group_exit) {
        puts("Z1_THREAD_EXIT_PARENT_ALIVE");
        fflush(stdout);
        return 0;
      }
      /* In group mode, the child must terminate the *whole* process.
       * If the parent continues, it is an invalid group-exit witness. */
    }
    sched_yield();
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 67;
    elapsed = (double)(now.tv_sec - start.tv_sec) +
              1e-9 * (double)(now.tv_nsec - start.tv_nsec);
  } while (elapsed < 5.0);

  if (group_exit) {
    fputs("Z1_GROUP_EXIT_FAILED_PARENT_SURVIVED\n", stderr);
    return 68;
  }
  fputs("Z1_THREAD_EXIT_TIMEOUT\n", stderr);
  return 69;
}
