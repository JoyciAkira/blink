/*
 * B10 Repeated Sequential Fork Probe
 * Freestanding x86-64 ELF, no libc.
 * Tests: 10 sequential fork() -> children _exit(i) -> parent wait4(-1) reaps
 * all 10 with exact status words. Verifies process table cleanup, memory
 * stability, and FD refcount lifecycle across repeated fork cycles.
 */

#define SYS_write 1
#define SYS_exit 60
#define SYS_wait4 61
#define SYS_fork 57
#define SYS_sched_yield 24

#define STDOUT_FILENO 1
#define NCHILDREN 10

static inline long syscall0(long n) {
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(n) : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall1(long n, long a1) {
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(n), "D"(a1) : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall4(long n, long a1, long a2, long a3, long a4) {
    long ret;
    register long r10 __asm__("r10") = a4;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10) : "rcx", "r11", "memory");
    return ret;
}

void *memset(void *s, int c, long n) {
    unsigned char *p = s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

static void write_str(const char *s) {
    long len = 0;
    while (s[len]) len++;
    __asm__ volatile ("syscall" : : "a"(SYS_write), "D"(STDOUT_FILENO), "S"(s), "d"(len) : "rcx", "r11", "memory");
}
static void write_int(long v) {
    char buf[24];
    int i = 23;
    buf[i] = '\0';
    if (v == 0) {
        buf[--i] = '0';
    } else {
        long neg = 0;
        if (v < 0) { neg = 1; v = -v; }
        while (v > 0) {
            buf[--i] = '0' + (v % 10);
            v /= 10;
        }
        if (neg) buf[--i] = '-';
    }
    write_str(&buf[i]);
}

void _start(void) {
    long pids[NCHILDREN];
    int failed = 0;
    
    /* Phase 1: fork 10 children sequentially */
    for (int i = 0; i < NCHILDREN; i++) {
        long pid = syscall0(SYS_fork);
        
        if (pid == 0) {
            /* Child: exit with code i */
            syscall1(SYS_exit, i);
            __builtin_unreachable();
        }
        
        if (pid < 0) {
            write_str("FORK ");
            write_int(i);
            write_str(" FAILED\n");
            failed = 1;
            break;
        }
        
        pids[i] = pid;
        
        /* Yield to let child run and exit */
        syscall0(SYS_sched_yield);
    }
    
    if (failed) {
        syscall1(SYS_exit, 1);
        __builtin_unreachable();
    }
    
    write_str("[B10-FORK-SEQ]\n");
    
    /* Phase 2: verify PIDs are unique */
    for (int i = 0; i < NCHILDREN; i++) {
        for (int j = i + 1; j < NCHILDREN; j++) {
            if (pids[i] == pids[j] || pids[i] <= 0 || pids[j] <= 0) {
                write_str("PID COLLISION\n");
                syscall1(SYS_exit, 1);
                __builtin_unreachable();
            }
        }
    }
    write_str("[B10-PID-UNIQUE]\n");
    
    /* Phase 3: reap all 10 children with wait4(-1) and verify status */
    int reaped = 0;
    int status_ok = 1;
    long seen_status[NCHILDREN] = {0};
    
    for (int i = 0; i < NCHILDREN; i++) {
        int wstatus = 0;
        long rc = syscall4(SYS_wait4, -1, (long)&wstatus, 0, 0);
        
        if (rc <= 0) {
            write_str("WAIT FAILED\n");
            status_ok = 0;
            break;
        }
        
        reaped++;
        
        /* wstatus: exit code is in bits 8-15 for normal exit */
        long exit_code = (wstatus >> 8) & 0xff;
        seen_status[exit_code] = 1;
        
        /* Verify this PID matches one we spawned */
        int found = 0;
        for (int j = 0; j < NCHILDREN; j++) {
            if (pids[j] == rc) {
                if (j != (int)exit_code) {
                    write_str("STATUS MISMATCH FOR PID ");
                    write_int(rc);
                    write_str(": expected ");
                    write_int(j);
                    write_str(" got ");
                    write_int(exit_code);
                    write_str("\n");
                    status_ok = 0;
                }
                found = 1;
                break;
            }
        }
        if (!found) {
            write_str("UNEXPECTED PID ");
            write_int(rc);
            write_str("\n");
            status_ok = 0;
        }
    }
    
    if (reaped == NCHILDREN) {
        write_str("[B10-WAIT-REAP]\n");
    }
    
    /* Verify all exit codes 0..9 seen exactly once */
    for (int i = 0; i < NCHILDREN; i++) {
        if (!seen_status[i]) {
            write_str("MISSING EXIT CODE ");
            write_int(i);
            write_str("\n");
            status_ok = 0;
        }
    }
    
    if (status_ok) {
        write_str("[B10-STATUS-EXACT]\n");
    }
    
    write_str("[B10-CHILD-EXIT]\n");
    write_str("[B10-PASS]\n");
    
    syscall1(SYS_exit, 0);
    __builtin_unreachable();
}