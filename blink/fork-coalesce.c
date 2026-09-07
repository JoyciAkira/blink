/* blink/fork-coalesce.c — fork-exec coalescing implementation.
 *
 * Implements the global state machine and spawn_exec_entry for wasm.
 * spawn_exec_entry runs in the child worker: applies fd actions, then
 * execve("/bin/blink", ["-e", "-s", target...]).
 *
 * Flow:
 *   guest fork() -> SysClone -> fork_coalesce_begin() -> returns 0 (child
 *     branch); machine state snapshotted at fork-return point.
 *   child: dup2/close/... -> recorded as fd actions (not applied).
 *   child: execve() -> SysExecve -> fork_coalesce_exec():
 *     builds wasm_spawn_exec_arg, calls kernel SYS_clone(spawn_exec_entry,
 *     &arg, SIGCHLD|CLONE_CHILD_SETTID, ...) -> kernel spawns a child worker
 *     that runs spawn_exec_entry: applies fd actions, then
 *     execve("/bin/blink", ["-e","-s", prog, ...]) -> binfmt_wasm
 *     instantiate(true) -> FRESH blink instance runs the target.
 *     Kernel clone returns the real child pid. fork_coalesce_exec then
 *     restores the machine snapshot (fork-return point) with real_pid in rax
 *     and sets m->interrupted so OpSyscall skips the normal rax write.
 *   parent (guest): resumes at fork-return, rax=real_pid -> signal-pipe read
 *     (EOF once the real child exec'd and closed the write end) -> wait4
 *     (real pid) -> SysWait4 (pid passthrough; fake_pid only used when the
 *     parent captured a fake pid earlier).
 */

#include "blink/machine.h"
#include "blink/endian.h"
#include "blink/fork-coalesce.h"
#include "blink/signal.h"
#include "blink/linux.h"
#include "blink/atomic.h"
#include "blink/thread.h"
#include "blink/log.h"
#include "blink/pml4t.h"
#include "blink/dll.h"
#include "blink/fds.h"
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
#include <stdarg.h>

#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
static void trace_log(const char *msg) {
  write(2, msg, strlen(msg));
}

static void trace_log_f(const char *fmt, ...) {
  char buf[512];
  va_list va;
  va_start(va, fmt);
  vsnprintf(buf, sizeof(buf), fmt, va);
  va_end(va);
  write(2, buf, strlen(buf));
}

/* Global fork state (single-threaded guest, safe) */
struct fork_state g_fork_state = {0};

/* Snapshot of the shared guest signal-disposition table taken at
 * fork_coalesce_begin. The coalesced CHILD branch runs libuv's
 * uv__process_child_init inside the single shared System, which resets
 * SIGCHLD/others to SIG_DFL on the SAME hands[] table the parent uses. On
 * the parent rewind (fork_coalesce_exec) we restore this snapshot so the
 * parent keeps its SIGCHLD handler and the child's exit can be delivered. */
static struct sigaction_linux g_fork_hands_snapshot[64];

static pid_t next_fake_pid = 1000;

#define G12_RW_SNAP_MAX 12288 /* 48 MiB of 4K pages */

static i64 *g_rw_gva;
static u8 *g_rw_data;
static int g_rw_n;
static int g_rw_cap;


static void fork_coalesce_free_rw_snap(void) {
  /* T5N fix: keep the grown snapshot buffer between fork cycles — only reset
   * the fill count. Repeated alloc/copy/free realloc cycles are the owner of
   * the deterministic ncap=512 wasm trap; keeping the high-water buffer
   * removes realloc from every cycle after the first. */
  g_rw_n = 0;
}

static void fork_coalesce_unfreeze(void) {
  g_fork_state.freeze_siblings = false;
}

bool fork_coalesce_should_park(struct Machine *m) {
  /* Sibling machines park while a fork-coalesce freeze snapshots the VAS;
   * the forking parent (g_fork_state.m) keeps executing. */
  if (!g_fork_state.freeze_siblings) return false;
  if (m == g_fork_state.m) return false;
  return true;
}
void fork_coalesce_dump_actors(struct System *s, const char *why) {
  struct Dll *e;
  struct Machine *o;
  if (!s) return;
  LOCK(&s->machines_lock);
  for (e = dll_first(s->machines); e; e = dll_next(s->machines, e)) {
    o = MACHINE_CONTAINER(e);
    ERRF("G12-ACTOR why=%s tid=%d ip=%#llx insyscall=%d sysdepth=%d killed=%d parked=%d leader=%d",
         why ? why : "?", o->tid, (unsigned long long)o->ip, (int)o->insyscall,
         o->sysdepth,
         (int)atomic_load_explicit(&o->killed, memory_order_relaxed),
         (int)atomic_load_explicit(&o->g12_parked, memory_order_relaxed),
         o->tid == s->pid);
  }
  UNLOCK(&s->machines_lock);
}

static void fork_coalesce_freeze_siblings(struct Machine *parent) {
  struct Dll *e;
  struct Machine *o;
  int i, parked = 0, total = 0;
  if (!parent || !parent->system) return;
  g_fork_state.freeze_siblings = true;
  LOCK(&parent->system->machines_lock);
  for (e = dll_first(parent->system->machines); e;
       e = dll_next(parent->system->machines, e)) {
    o = MACHINE_CONTAINER(e);
    if (o == parent) continue;
    atomic_store_explicit(&o->attention, true, memory_order_release);
#ifdef HAVE_THREADS
    if (o->thread && !pthread_equal(o->thread, pthread_self())) {
      pthread_kill(o->thread, SIGSYS);
    }
#endif
  }
  UNLOCK(&parent->system->machines_lock);
  for (i = 0; i < 25; ++i) {
    parked = total = 0;
    LOCK(&parent->system->machines_lock);
    for (e = dll_first(parent->system->machines); e;
         e = dll_next(parent->system->machines, e)) {
      o = MACHINE_CONTAINER(e);
      if (o == parent) continue;
      ++total;
      if (atomic_load_explicit(&o->g12_parked, memory_order_acquire) ||
          !o->insyscall) {
        ++parked;
      }
    }
    UNLOCK(&parent->system->machines_lock);
    if (total == 0 || parked >= total) break;
    {
      struct timespec ts = {0, 1000 * 1000};
      nanosleep(&ts, 0);
    }
  }
  ERRF("G12-FREEZE parent_tid=%d parked_or_idle=%d total_sib=%d wait_ms=%d",
       parent->tid, parked, total, i);
}

static int fork_coalesce_snap_rw(struct Machine *m) {
  struct ContiguousMemoryRanges ranges;
  unsigned i;
  ERRF("[T5N] SNAP_ENTER tid=%d", m->tid);
  int skipped = 0;
  g_rw_n = 0; /* retained buffer: reset fill only (arena kept) */
  memset(&ranges, 0, sizeof(ranges));
  if (FindContiguousMemoryRanges(m, &ranges) != 0) {
    ERRF("[T5N] SNAP_FIND_FAILED tid=%d", m->tid);
    return -1;
  }
  ERRF("[T5N] SNAP_RANGES=%u tid=%d", ranges.i, m->tid);
  for (i = 0; i < ranges.i; ++i) {
    i64 addr, end;
    addr = ranges.p[i].a & -4096;
    end = ranges.p[i].b;
    for (; addr < end; addr += 4096) {
      if (!IsValidMemory(m, addr, 4096, PROT_WRITE)) continue;
      if (g_rw_n >= G12_RW_SNAP_MAX) {
        ++skipped;
        continue;
      }
      if (g_rw_n == g_rw_cap) {
        int ncap = g_rw_cap ? g_rw_cap * 2 : 256;
        ERRF("[T5N] SNAP_REALLOC ncap=%d tid=%d", ncap, m->tid);
        i64 *ngva = (i64 *)realloc(g_rw_gva, (size_t)ncap * sizeof(i64));
        u8 *ndata = (u8 *)realloc(g_rw_data, (size_t)ncap * 4096);
        if (!ngva || !ndata) {
          free(ngva);
          ERRF("G12-SNAP-OOM n=%d", g_rw_n);
          free(ranges.p);
          return g_rw_n ? 0 : -1;
        }
        g_rw_gva = ngva;
        g_rw_data = ndata;
        g_rw_cap = ncap;
      }
      if (CopyFromUser(m, g_rw_data + (size_t)g_rw_n * 4096, addr, 4096) == -1) {
        continue;
      }
      g_rw_gva[g_rw_n++] = addr;
    }
  }
  free(ranges.p);
  ERRF("G12-SNAP-RW pages=%d skipped=%d cap=%d", g_rw_n, skipped, g_rw_cap);
  return 0;
}

static void fork_coalesce_restore_rw(struct Machine *m) {
  int i, fail = 0;
  for (i = 0; i < g_rw_n; ++i) {
    if (CopyToUser(m, g_rw_gva[i], g_rw_data + (size_t)i * 4096, 4096) == -1)
      ++fail;
  }
  ERRF("G12-SNAP-RESTORE pages=%d fail=%d", g_rw_n, fail);
  fork_coalesce_free_rw_snap();
}


/* musl-wasm32 syscall entry: linux.syscall import (kernel wasm_syscall export) */
__attribute__((import_module("linux"), import_name("syscall")))
extern long __wasm_syscall(long n, long a, long b, long c, long d, long e,
                           long f);
#define SYS_clone 220 /* wasm arch __NR_clone; 56 is wasm __NR_openat */
#define SYS_wait4 260 /* wasm arch __NR_wait4 (asm-generic 64-bit ABI; musl wasm32) */
static int fork_coalesce_host_fd(int guest_fd) {
  struct Fd *fd;
  int host_fd = guest_fd;
  /* Fd::fildes is the native descriptor backing the Blink descriptor. */
  if (g_fork_state.m &&
      (fd = GetFd(&g_fork_state.m->system->fds, guest_fd)) != NULL)
    host_fd = fd->fildes;
  return host_fd;
}

int fork_coalesce_begin(struct Machine *m, u64 child_stack) {
  pid_t fpid = next_fake_pid++;
  trace_log_f("[G12-BLINK-TRACE] 1. fork_coalesce_begin: fake_pid=%d rip=%#llx\n", fpid, (unsigned long long)m->ip);
  memset(&g_fork_state, 0, sizeof(g_fork_state));
  g_fork_state.pending = true;
  g_fork_state.m = m;
  g_fork_state.fake_pid = fpid;
  g_fork_state.real_pid = 0;
  /* Snapshot the parent's signal dispositions before the child branch runs. */
  memcpy(g_fork_hands_snapshot, m->system->hands, sizeof(m->system->hands));
  /* Snapshot the machine at the fork-return point. m->ip was already advanced
   * past the syscall instruction by the dispatcher before SysClone ran. */
  g_fork_state.fork_ret_state.ip = m->ip;
  g_fork_state.fork_ret_state.cs = m->cs;
  g_fork_state.fork_ret_state.ss = m->ss;
  g_fork_state.fork_ret_state.es = m->es;
  g_fork_state.fork_ret_state.ds = m->ds;
  g_fork_state.fork_ret_state.fs = m->fs;
  g_fork_state.fork_ret_state.gs = m->gs;
  memcpy(g_fork_state.fork_ret_state.weg, m->weg, sizeof(m->weg));
  memcpy(g_fork_state.fork_ret_state.xmm, m->xmm, sizeof(m->xmm));
  g_fork_state.fork_ret_state.mxcsr = m->mxcsr;
  g_fork_state.fork_ret_state.fpu = m->fpu;
  u64 sp = Read64(m->sp);
  i64 sp_page = (i64)(sp & -4096);
  g_fork_state.stack_snapshot_base = sp_page - 16 * 4096;
  for (int i = 0; i < 32; i++) {
    i64 page_addr = g_fork_state.stack_snapshot_base + i * 4096;
    if (IsValidMemory(m, page_addr, 4096, PROT_READ)) {
      CopyFromUser(m, &g_fork_state.stack_snapshot[i * 4096], page_addr, 4096);
      g_fork_state.stack_snapshot_valid[i] = true;
    } else {
      g_fork_state.stack_snapshot_valid[i] = false;
    }
  }
  /* Stop siblings then snapshot writable guest pages so pthread_atfork(CHILD)
   * / uv__process_child_init heap mutations can be undone on rewind. */
  fork_coalesce_freeze_siblings(m);
  fork_coalesce_snap_rw(m);
  if (child_stack) Put64(m->sp, child_stack);
  return 0; /* child branch: libuv runs uv__process_child_init */
}

void fork_coalesce_add_fd_action(uint8_t op, int src_fd, int dst_fd) {
  if (!g_fork_state.pending)
    return;
  if (g_fork_state.nactions >= MAX_FORK_FD_ACTIONS)
    return;
  /* Keep pipe dup actions in the snapshot so fork_coalesce_exec can perform
   * the host-fd remap before clone.  spawn_exec_entry skips these actions
   * after clone; dropping them here makes the parent pre-dup pass impossible. */
  trace_log_f("[G12-BLINK-TRACE] 2. fd_action: op=%d src=%d dst=%d (total=%d)\n",
              (int)op, src_fd, dst_fd, g_fork_state.nactions + 1);
  struct fd_action *a = &g_fork_state.actions[g_fork_state.nactions++];
  a->op = op;
  a->src_fd = src_fd;
  a->dst_fd = dst_fd;
}

void fork_coalesce_add_open_action(const char *path, int flags, int mode,
                                   int dst_fd) {
  if (!g_fork_state.pending ||
      g_fork_state.nactions >= MAX_FORK_FD_ACTIONS)
    return;
  // For /dev/null with dst 0 (ignore stdio), just close 0 instead of open
  if (dst_fd == 0 && path && strcmp(path, "/dev/null") == 0) {
    trace_log_f("[G12-BLINK-TRACE] 2. fd_action: open /dev/null -> close 0 (total=%d)\n", g_fork_state.nactions+1);
    struct fd_action *a = &g_fork_state.actions[g_fork_state.nactions++];
    memset(a, 0, sizeof(*a));
    a->op = WASM_SPAWN_FD_CLOSE;
    a->src_fd = 0;
    return;
  }
  struct fd_action *a = &g_fork_state.actions[g_fork_state.nactions++];
  memset(a, 0, sizeof(*a));
  a->op = WASM_SPAWN_FD_OPEN;
  a->dst_fd = dst_fd;
  a->flags = flags;
  a->mode = mode;
  snprintf(a->path, sizeof(a->path), "%s", path);
  trace_log_f("[G12-BLINK-TRACE] 2. fd_action: op=%d path=%s dst=%d "
              "(total=%d)\n",
              (int)a->op, a->path, dst_fd, g_fork_state.nactions);
}

bool fork_coalesce_fd_is_open(int fd, bool parent_open) {
  bool open = parent_open;
  for (int i = 0; i < g_fork_state.nactions; ++i) {
    const struct fd_action *a = &g_fork_state.actions[i];
    if (a->op == WASM_SPAWN_FD_CLOSE && a->src_fd == fd) {
      open = false;
    } else if ((a->op == WASM_SPAWN_FD_DUP2 ||
                a->op == WASM_SPAWN_FD_DUP3 ||
                a->op == WASM_SPAWN_FD_OPEN) &&
               a->dst_fd == fd) {
      open = true;
    }
  }
  return open;
}

/* spawn_exec_entry: runs in the child worker (resolved from the shared module
 * via the kernel's wasm_user_switch_entry). Applies the recorded fd actions to
 * the child's kernel fd table, then execve("/bin/blink", ["-e","-s", ...]) so
 * binfmt_wasm instantiates a fresh blink that runs the target program. */
int spawn_exec_entry(void *arg) {
  struct wasm_spawn_exec_arg *req = (struct wasm_spawn_exec_arg *)arg;
  trace_log_f("[G12-BLINK-TRACE] 3. spawn_exec_entry in child worker: nactions=%d target=%s\\n", req->nactions, req->blink_argv[3] ? req->blink_argv[3] : "null");
  for (int i = 0; i < req->nactions; i++) {
    trace_log_f("[G12-BLINK-TRACE] 3a. action[%d] op=%d src=%d dst=%d\\n", i, (int)req->actions[i].op, req->actions[i].src_fd, req->actions[i].dst_fd);
    if (req->actions[i].op == WASM_SPAWN_FD_DUP2) {
      if (req->actions[i].dst_fd == 1 || req->actions[i].dst_fd == 2) {
        trace_log_f("[G12-BLINK-TRACE] 3a-pipe-parent-done skip %d->%d\\n", req->actions[i].src_fd, req->actions[i].dst_fd);
      } else {
        dup2(req->actions[i].src_fd, req->actions[i].dst_fd);
      }
    } else if (req->actions[i].op == WASM_SPAWN_FD_CLOSE) {
      close(req->actions[i].src_fd);
    } else if (req->actions[i].op == WASM_SPAWN_FD_DUP3) {
      dup3(req->actions[i].src_fd, req->actions[i].dst_fd, 0);
    } else if (req->actions[i].op == WASM_SPAWN_FD_OPEN) {
      int fd = open(req->actions[i].path, req->actions[i].flags, req->actions[i].mode);
      if (fd >= 0 && fd != req->actions[i].dst_fd) {
        dup2(fd, req->actions[i].dst_fd);
        close(fd);
      } else if (fd < 0) {
        close(req->actions[i].dst_fd);
      }
    }
  }
  trace_log_f("[T5M] CHILD_SPAWN_BEGIN pid=%d uid=%d target=%s\n", (int)getpid(), (int)getuid(), req->blink_argv[0]);
  trace_log_f("[G12-BLINK-TRACE] 4. child calling execve for: %s\\n", req->blink_argv[0]);
  /* T4 generic fix (branch zeronode-v2/m1r-t4-patches, adapted): exec the
   * TARGET directly. The kernel's binfmt routes any native format: x86-64
   * ELF via the blink emu path, wasm guests natively. No blink re-exec and
   * no hardcoded interpreter path — works for /bin/sh (wasm busybox),
   * postgres, or any kernel-executable program. */
  int rc = execve(req->blink_argv[0], req->blink_argv, req->envp);

  /* If execve fails */
  trace_log_f("[G12-BLINK-TRACE] 5. CHILD EXECVE FAILED: rc=%d errno=%d -> _exit(127)\n", rc, errno);
  _exit(127);
}

int fork_coalesce_exec(struct Machine *m, const char *prog, char **argv,
                       char **envp) {
  if (!g_fork_state.pending) {
    trace_log("[G12-BLINK-TRACE] execve but NO pending fork!\n");
    return -1;
  }
  trace_log_f("[G12-BLINK-TRACE] 6. fork_coalesce_exec intercept: prog=%s nactions=%d\n", prog, g_fork_state.nactions);

  /* DEEP-COPY prog/argv/envp into static storage. The guest strings live in
   * the parent machine's syscall freelist; OpSyscall's epilogue CollectGarbage
   * frees them before the child worker runs spawn_exec_entry, so storing
   * guest-host pointers here produced dangling argv -> ENOEXEC garbage paths
   * in the child. Static copies outlive both machines. */
  static char prog_copy[PATH_MAX];
  static char argv_copy[MAX_FORK_EXEC_ARGS][256];
  static char envp_copy[MAX_FORK_EXEC_ENV][256];
  snprintf(prog_copy, sizeof(prog_copy), "%s", prog);

  int nargv = 0;
  for (int i = 0; argv && argv[i] && nargv < MAX_FORK_EXEC_ARGS - 1; i++) {
    snprintf(argv_copy[nargv], sizeof(argv_copy[0]), "%s", argv[i]);
    ++nargv;
  }
  int nenvp = 0;
  for (int i = 0; envp && envp[i] && nenvp < MAX_FORK_EXEC_ENV - 1; i++) {
    snprintf(envp_copy[nenvp], sizeof(envp_copy[0]), "%s", envp[i]);
    ++nenvp;
  }
  /* Build blink_argv: [prog, argv[1]..., NULL] — T4 generic fix: the child
   * execve's the TARGET itself; kernel binfmt selects the right executor. */
  static char *blink_argv[MAX_FORK_EXEC_ARGS];
  int bi = 0;
  blink_argv[bi++] = prog_copy;
  for (int i = 1; i < nargv && bi < MAX_FORK_EXEC_ARGS - 1; i++) {
    blink_argv[bi++] = argv_copy[i];
  }
  blink_argv[bi] = NULL;

  /* Build spawn_exec_arg (static; child worker reads it asynchronously). */
  static struct wasm_spawn_exec_arg arg;
  memset(&arg, 0, sizeof(arg));
  memcpy(arg.actions, g_fork_state.actions,
         g_fork_state.nactions * sizeof(struct fd_action));
  arg.nactions = g_fork_state.nactions;
  for (int i = 0; i < bi; i++)
    arg.blink_argv[i] = blink_argv[i];
  for (int i = 0; i < nenvp; i++)
    arg.envp[i] = envp_copy[i];

  /* Deferred actions carry Blink/VFS numbers.  Resolve only the sources that
   * cross into the worker's native fd syscalls. */
  for (int i = 0; i < arg.nactions; i++) {
    struct fd_action *a = &arg.actions[i];
    if (a->op == WASM_SPAWN_FD_DUP2 || a->op == WASM_SPAWN_FD_DUP3)
      a->src_fd = fork_coalesce_host_fd(a->src_fd);
  }
  int _saved_out = -1, _saved_err = -1;
  for (int i = 0; i < arg.nactions; i++) {
    if (arg.actions[i].op != WASM_SPAWN_FD_DUP2)
      continue;
    int dst = arg.actions[i].dst_fd;
    int src = arg.actions[i].src_fd;
    if (dst == 1 && _saved_out == -1) {
      _saved_out = dup(1);
      if (_saved_out != -1 && dup2(src, 1) == -1)
        trace_log_f("[G12-BLINK-TRACE] 7-pre-dup stdout src=%d errno=%d\\n", src, errno);
    } else if (dst == 2 && _saved_err == -1) {
      _saved_err = dup(2);
      if (_saved_err != -1 && dup2(src, 2) == -1)
        trace_log_f("[G12-BLINK-TRACE] 7-pre-dup stderr src=%d errno=%d\\n", src, errno);
    }
  }
  trace_log_f("[G12-BLINK-TRACE] 7-pre-dup done stdout=%d stderr=%d\\n", _saved_out, _saved_err);
  long ret = __wasm_syscall(SYS_clone, (long)spawn_exec_entry, (long)&arg,
                            0x00000011 | 0x00000100, 0, 0, 0);
  trace_log_f("[G12-BLINK-TRACE] 8. kernel clone returned: ret=%ld\\n", ret);
  if (_saved_out != -1) { dup2(_saved_out, 1); close(_saved_out); }
  if (_saved_err != -1) { dup2(_saved_err, 2); close(_saved_err); }

  if (ret < 0) {
    trace_log_f("[G12-BLINK-TRACE] kernel clone failed with %ld\n", ret);
    fork_coalesce_restore_rw(m);
    fork_coalesce_free_rw_snap();
    fork_coalesce_unfreeze();
    return -1;
  }

  g_fork_state.real_pid = (pid_t)ret;
  g_fork_state.pending = false;

  trace_log_f("[G12-BLINK-TRACE] 9. rewinding machine to saved_ip=%#llx real_pid=%d\n",
              (unsigned long long)g_fork_state.fork_ret_state.ip, (int)g_fork_state.real_pid);

  /* Rewind the machine to the fork-return point with real_pid in rax (weg[0]).
   * m->interrupted causes OpSyscall to skip the normal rax write, so the
   * guest resumes the PARENT path exactly as after a real fork(). */
  m->ip = g_fork_state.fork_ret_state.ip;
  m->cs = g_fork_state.fork_ret_state.cs;
  m->ss = g_fork_state.fork_ret_state.ss;
  m->es = g_fork_state.fork_ret_state.es;
  m->ds = g_fork_state.fork_ret_state.ds;
  m->fs = g_fork_state.fork_ret_state.fs;
  m->gs = g_fork_state.fork_ret_state.gs;
  memcpy(m->weg, g_fork_state.fork_ret_state.weg, sizeof(m->weg));
  memcpy(m->xmm, g_fork_state.fork_ret_state.xmm, sizeof(m->xmm));
  m->mxcsr = g_fork_state.fork_ret_state.mxcsr;
  m->fpu = g_fork_state.fork_ret_state.fpu;
  /* rax = real child pid (zero-extended to 64-bit; x86-64 syscall return
   * convention requires full 64-bit rax. Partial memcpy left upper bytes
   * with snapshot garbage, causing parent to read corrupted pid.) */
  memset(m->weg[0], 0, 8);
  memcpy(m->weg[0], &g_fork_state.real_pid, sizeof(pid_t));
  m->interrupted = true; /* skip OpSyscall's rax overwrite */

  /* Restore writable guest memory mutated by the fake child, then unfreeze
   * siblings. Stack snapshot remains as a fallback for the parent frame. */
  fork_coalesce_restore_rw(m);
  for (int i = 0; i < 32; i++) {
    if (g_fork_state.stack_snapshot_valid[i]) {
      i64 page_addr = g_fork_state.stack_snapshot_base + i * 4096;
      CopyToUser(m, page_addr, &g_fork_state.stack_snapshot[i * 4096], 4096);
    }
  }
  fork_coalesce_free_rw_snap();
  fork_coalesce_unfreeze();

  /* Restore the parent's signal dispositions clobbered by the fake child's
   * uv__process_child_init (SIG_DFL resets). Without this the parent loses
   * its SIGCHLD handler and the close event can never fire. */
  memcpy(m->system->hands, g_fork_hands_snapshot, sizeof(m->system->hands));

  /* On this 1-CPU wasm kernel the coalesced child often exits before clone
   * returns. Reap immediately so guest SIGCHLD is pending as soon as the
   * parent path resumes — do not wait for a later poll/epoll entry. */
  fork_coalesce_check_child(m);

  return 0; /* success: machine rewound; guest resumes parent path */
}

/* Reap-notify: called from blink when the guest may want to know the child
 * exited (epoll/poll caps and generic syscall entries while a coalesced child
 * is outstanding). Uses the wasm kernel's own wait4 so the kernel reaps the
 * real child; the status is stashed and SIGCHLD is enqueued on the parent
 * machine so the guest's rt_sigaction(SIGCHLD) handler runs. */
void fork_coalesce_check_child(struct Machine *m) {
  struct Machine *parent;
  if (!g_fork_state.real_pid || g_fork_state.child_exited)
    return;
  parent = g_fork_state.m ? g_fork_state.m : m;
  int status = 0;
  long ret = __wasm_syscall(SYS_wait4, g_fork_state.real_pid, (long)&status,
                            WNOHANG /* options */, 0, 0, 0);
  if (ret == g_fork_state.real_pid) {
    g_fork_state.child_status = status;
    g_fork_state.child_exited = true;
    trace_log_f("[G12-BLINK-TRACE] child reaped real_pid=%ld status=%#x "
                "exit=%d; enqueue SIGCHLD parent_tid=%d\n",
                ret, status, WEXITSTATUS(status), parent ? parent->tid : -1);
    EnqueueSignal(parent, SIGCHLD_LINUX);
    atomic_store_explicit(&parent->attention, true, memory_order_release);
  } else if (ret == 0) {
    trace_log("[G12-BLINK-TRACE] child still running (wait4 WNOHANG=0)\n");
  } else {
    /* ECHILD / ENOSYS etc: child is gone from the kernel side already. */
    trace_log_f("[G12-BLINK-TRACE] child wait4 probe ret=%ld -> mark exited\n",
                ret);
    g_fork_state.child_status = 0;
    g_fork_state.child_exited = true;
    EnqueueSignal(parent, SIGCHLD_LINUX);
    atomic_store_explicit(&parent->attention, true, memory_order_release);
  }
}

void fork_coalesce_wake_parent(void) {
  struct Machine *parent = g_fork_state.m;
  if (!parent) return;
  if (!g_fork_state.child_exited)
    fork_coalesce_check_child(parent);
  atomic_store_explicit(&parent->attention, true, memory_order_release);
  /* After the coalesced child is reaped, SIGSYS here interrupts Node
   * process.exit and is the observed post-G12-RESULT hang. */
  if (g_fork_state.child_exited) return;
#ifdef HAVE_THREADS
  if (parent->thread && !pthread_equal(parent->thread, pthread_self())) {
    pthread_kill(parent->thread, SIGSYS);
  }
#endif
}

bool fork_coalesce_unreaped_child(void) {
  return g_fork_state.real_pid > 0 && !g_fork_state.child_exited;
}

bool fork_coalesce_child_done(void) {
  return g_fork_state.real_pid > 0 && g_fork_state.child_exited;
}

pid_t fork_coalesce_wait(pid_t fake_pid) {
  if (g_fork_state.fake_pid && fake_pid == g_fork_state.fake_pid &&
      g_fork_state.real_pid > 0) {
    trace_log_f("[G12-BLINK-TRACE] wait4 translate: fake_pid=%d -> real_pid=%d\n", fake_pid, g_fork_state.real_pid);
    return g_fork_state.real_pid;
  }
  return fake_pid; /* no mapping, pass through */
}

void fork_coalesce_reap(pid_t fake_pid) {
  if (g_fork_state.fake_pid && fake_pid == g_fork_state.fake_pid) {
    trace_log_f("[G12-BLINK-TRACE] reap coalesced child: fake_pid=%d\n", fake_pid);
    memset(&g_fork_state, 0, sizeof(g_fork_state));
  }
}

void fork_coalesce_abort(void) {
  trace_log("[G12-BLINK-TRACE] fork_coalesce_abort called!\n");
  if (g_fork_state.m) fork_coalesce_restore_rw(g_fork_state.m);
  fork_coalesce_unfreeze();
  fork_coalesce_free_rw_snap();
  memset(&g_fork_state, 0, sizeof(g_fork_state));
}
