/* fork-coalesce.h — Fork-exec coalescing for environments without host fork()
 *
 * When HAVE_FORK is disabled, we intercept SysClone (fork/vfork), record
 * subsequent FD actions (dup2/close), and on SysExecve we trigger the kernel's
 * fn-based clone with a wrapper that applies the recorded FD actions before
 * execing. This bypasses the need for host fork() while preserving fork-exec
 * semantics.
 *
 * Return contract (IMPORTANT):
 *   fork_coalesce_begin() returns 0 — the guest treats this as the CHILD
 *   branch (libuv's uv__process_child_init gating on *pid == 0). The child's
 *   dup2/close syscalls are recorded as fd actions. When the child reaches
 *   execve, fork_coalesce_exec() triggers the kernel fn-based clone
 *   (spawn_exec_entry → fd actions → execve) and rewinds the machine to the
 *   fork-return point with the REAL pid in rax, so the guest's PARENT path
 *   resumes (signal-pipe read → EOF after child exec → wait4 → stdout read).
 */

#ifndef BLINK_FORK_COALESCE_H_
#define BLINK_FORK_COALESCE_H_

#include "blink/machine.h"

// Maximum number of pending FD actions per fork
#define MAX_FORK_FD_ACTIONS 64
// Maximum exec argv/envp entries carried into the child
#define MAX_FORK_EXEC_ARGS 64
#define MAX_FORK_EXEC_ENV 64

// FD action opcodes (mirrored in the wasm_spawn_exec_arg ABI)
#define WASM_SPAWN_FD_DUP2 1
#define WASM_SPAWN_FD_CLOSE 2
#define WASM_SPAWN_FD_DUP3 3
#define WASM_SPAWN_FD_OPEN 4

// Deferred child fd operation
struct fd_action {
  uint8_t op;  // WASM_SPAWN_FD_*
  int src_fd;
  int dst_fd;
  int flags;
  int mode;
  char path[256];
};

// Pending fork state (single global; single-threaded guest, safe)
struct fork_state {
  bool pending;      // a fork is outstanding (child path in flight)
  pid_t fake_pid;    // pid returned to the guest at rewind (parent's view)
  pid_t real_pid;    // kernel pid from fn-based clone (0 until execve)
  struct Machine *m; // parent machine (for register rewind)
  struct MachineState fork_ret_state; // snapshot at SysClone return point
  struct fd_action actions[MAX_FORK_FD_ACTIONS];
  int nactions;
  char *exec_argv[MAX_FORK_EXEC_ARGS]; // copied guest argv pointers
  char *exec_envp[MAX_FORK_EXEC_ENV];  // copied guest envp pointers
  int nargv;
  int nenvp;
  i64 stack_snapshot_base;
  u8 stack_snapshot[32 * 4096];
  bool stack_snapshot_valid[32];
  /* Child-exit delivery (G12 close-event fix): the kernel child is reaped
   * by our own kernel wait4() probe (check_child); the reaped raw status is
   * stashed here so SysWait4 can satisfy libuv's waitpid() exactly once, and
   * SIGCHLD is queued to the parent machine so the guest reaps promptly. */
  int child_status;   // raw host wstatus of the reaped kernel child
  bool child_exited;  // true once check_child reaped the real child
  /* Stop-the-world while the fake child path mutates shared guest memory. */
  bool freeze_siblings;
};

// Argument block passed to spawn_exec_entry (child worker ABI)
struct wasm_spawn_exec_arg {
  struct fd_action actions[MAX_FORK_FD_ACTIONS];
  int nactions;
  char *blink_argv[MAX_FORK_EXEC_ARGS];
  char *envp[MAX_FORK_EXEC_ENV];
};

// Global pending fork state
extern struct fork_state g_fork_state;

// Called from SysClone when HAVE_FORK is disabled (wasm build).
// Snapshots the machine at the fork-return point, switches to child_stack,
// and returns 0 so the guest runs uv__process_child_init on its own stack.
int fork_coalesce_begin(struct Machine *m, u64 child_stack);

// Record child fd actions during the pending-fork window. They are applied
// in the child worker before execve.
void fork_coalesce_add_fd_action(uint8_t op, int src_fd, int dst_fd);
void fork_coalesce_add_open_action(const char *path, int flags, int mode,
                                   int dst_fd);
bool fork_coalesce_fd_is_open(int fd, bool parent_open);

// Called from SysExecve. If a pending fork is active, builds the
// wasm_spawn_exec_arg, triggers the kernel fn-based clone (SYS_clone with
// spawn_exec_entry), then rewinds the machine registers to the fork-return
// point with real_pid in rax. Returns the real pid (>= 0) on success, -1 on
// error. On success the caller must NOT continue the execve — the guest
// resumes the parent path.
int fork_coalesce_exec(struct Machine *m, const char *prog, char **argv,
                       char **envp);

// Called from SysWait4. If fake_pid maps to a coalesced fork, returns the real
// pid; otherwise returns fake_pid unchanged.
pid_t fork_coalesce_wait(pid_t fake_pid);

// Called from SysWait4 when the coalesced child was reaped: clears pending
// state.
void fork_coalesce_reap(pid_t fake_pid);

// Reaps the kernel child if it exited (WNOHANG) and, on first reaping,
// stashes the status and enqueues SIGCHLD on the parent machine so libuv's
// child-close path runs.
void fork_coalesce_check_child(struct Machine *m);

// True while a coalesced child exists that has not been reaped yet.
bool fork_coalesce_unreaped_child(void);

/* N0D: reap-or-mark the coalesced child and poke the parent machine so it
 * can run fork_coalesce_check_child / guest SIGCHLD instead of staying
 * blocked in host epoll/clone. Safe to call from a sibling worker. */
void fork_coalesce_wake_parent(void);

/* True when a CLONE_THREAD sibling must park while the fake child runs. */
bool fork_coalesce_should_park(struct Machine *m);

/* Dump live machines (no malloc). */
void fork_coalesce_dump_actors(struct System *s, const char *why);

// True once the coalesced child was reaped by check_child.
bool fork_coalesce_child_done(void);

// Cleanup if execve is never reached (e.g. child abort): clear pending state.
void fork_coalesce_abort(void);

#endif /* BLINK_FORK_COALESCE_H_ */
