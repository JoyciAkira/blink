/*
 * B12 Fork-Exec Path Preservation Probe
 * Freestanding x86-64 ELF, no libc.
 * Tests: fork() -> child execve() replaces image in-place, parent survives,
 *        wait4 retrieves exact PID and status. Failed exec negative control.
 *        Two-child composition.
 */

#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_wait4 61
#define SYS_exit 60
#define SYS_fork 57
#define SYS_execve 59
#define SYS_getpid 39

#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define O_RDONLY 0

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

static inline long syscall3(long n, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall4(long n, long a1, long a2, long a3, long a4) {
    long ret;
    register long r10 __asm__("r10") = a4;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10) : "rcx", "r11", "memory");
    return ret;
}

static void write_str(int fd, const char *s) {
    long len = 0;
    while (s[len]) len++;
    syscall3(SYS_write, fd, (long)s, len);
}

static void write_int(int fd, long v) {
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
    write_str(fd, &buf[i]);
}

void _start(void) {
    /* Phase 1: Standard fork-exec path */
    long pid1 = syscall0(SYS_fork);
    if (pid1 == 0) {
        /* Child 1: pre-exec then execve target */
        write_str(STDOUT_FILENO, "[B12-CHILD-PRE-EXEC]\n");
        
        static char *argv[] = {"b12-target", "arg1", 0};
        static char *envp[] = {"B12_ENV=pass", 0};
        
        /* Attempt execve. If it returns, it failed. */
        long rc = syscall3(SYS_execve, (long)"/tmp/b12-target", (long)argv, (long)envp);
        
        /* If we reach here, execve failed */
        write_str(STDERR_FILENO, "EXECVE FAILED: rc=");
        write_int(STDERR_FILENO, rc);
        write_str(STDERR_FILENO, "\n");
        syscall1(SYS_exit, 99);
        __builtin_unreachable();
    }
    
    if (pid1 > 0) {
        write_str(STDOUT_FILENO, "[B12-PARENT-FORK]\n");
        
        int wstatus = 0;
        long rc = syscall4(SYS_wait4, pid1, (long)&wstatus, 0, 0);
        
        if (rc == pid1) {
            write_str(STDOUT_FILENO, "[B12-WAIT-EXACT]\n");
            long exit_code = (wstatus >> 8) & 0xff;
            if (exit_code == 37) {
                write_str(STDOUT_FILENO, "[B12-EXIT-37-PASS]\n");
            } else {
                write_str(STDOUT_FILENO, "UNEXPECTED EXIT CODE: ");
                write_int(STDOUT_FILENO, exit_code);
                write_str(STDOUT_FILENO, "\n");
            }
        } else {
            write_str(STDOUT_FILENO, "WAIT4 FAILED\n");
        }
        write_str(STDOUT_FILENO, "[B12-PARENT-SURVIVED]\n");
    } else {
        write_str(STDERR_FILENO, "FORK1 FAILED\n");
        syscall1(SYS_exit, 1);
    }

    /* Phase 2: Failed exec negative control */
    long pid2 = syscall0(SYS_fork);
    if (pid2 == 0) {
        static char *argv_bad[] = {"nonexistent", 0};
        static char *envp_bad[] = {0};
        long rc = syscall3(SYS_execve, (long)"/nonexistent/path", (long)argv_bad, (long)envp_bad);
        if (rc < 0) {
            write_str(STDOUT_FILENO, "[B12-FAILED-EXEC-CLOSED]\n");
            syscall1(SYS_exit, 0);
        } else {
            write_str(STDERR_FILENO, "EXECVE SHOULD HAVE FAILED\n");
            syscall1(SYS_exit, 1);
        }
        __builtin_unreachable();
    }
    if (pid2 > 0) {
        int wstatus2 = 0;
        syscall4(SYS_wait4, pid2, (long)&wstatus2, 0, 0);
    }

    /* Phase 3: Two-child composition */
    long pid3 = syscall0(SYS_fork);
    if (pid3 == 0) {
        /* Child 3: just exit 10 */
        syscall1(SYS_exit, 10);
        __builtin_unreachable();
    }
    long pid4 = syscall0(SYS_fork);
    if (pid4 == 0) {
        /* Child 4: just exit 20 */
        syscall1(SYS_exit, 20);
        __builtin_unreachable();
    }
    
    if (pid3 > 0 && pid4 > 0) {
        int w3 = 0, w4 = 0;
        long r3 = syscall4(SYS_wait4, pid3, (long)&w3, 0, 0);
        long r4 = syscall4(SYS_wait4, pid4, (long)&w4, 0, 0);
        if (r3 == pid3 && r4 == pid4) {
            long e3 = (w3 >> 8) & 0xff;
            long e4 = (w4 >> 8) & 0xff;
            if ((e3 == 10 && e4 == 20) || (e3 == 20 && e4 == 10)) {
                write_str(STDOUT_FILENO, "[B12-TWO-CHILD-PASS]\n");
            }
        }
    }

    write_str(STDOUT_FILENO, "[B12-PASS]\n");
    syscall1(SYS_exit, 0);
    __builtin_unreachable();
}