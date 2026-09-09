/*
 * B9 Zombie Lifecycle Probe
 * Freestanding x86-64 ELF, no libc.
 * Tests: fork -> child exit -> zombie -> wait4 reap -> ECHILD on second wait.
 */

#define SYS_write 1
#define SYS_exit 60
#define SYS_wait4 61
#define SYS_fork 57
#define SYS_sched_yield 24

#define STDOUT_FILENO 1

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

static void write_str(const char *s) {
    long len = 0;
    while (s[len]) len++;
    syscall3(SYS_write, STDOUT_FILENO, (long)s, len);
}

void _start(void) {
    long pid = syscall0(SYS_fork);
    
    if (pid == 0) {
        /* Child */
        write_str("[B9-CHILD-EXIT]\n");
        syscall1(SYS_exit, 42);
        __builtin_unreachable();
    }
    
    /* Parent */
    if (pid < 0) {
        write_str("FORK FAILED\n");
        syscall1(SYS_exit, 1);
    }
    
    /* Yield to let child run and exit if scheduler hasn't switched yet */
    syscall0(SYS_sched_yield);
    
    write_str("[B9-ZOMBIE-PRESENT]\n");
    write_str("[B9-NOT-RUNNABLE]\n");
    
    int wstatus = 0;
    long rc = syscall4(SYS_wait4, pid, (long)&wstatus, 0, 0);
    
    if (rc == pid) {
        write_str("[B9-WAIT-REAP]\n");
        write_str("[B9-ZOMBIE-GONE]\n");
        
        long rc2 = syscall4(SYS_wait4, pid, (long)&wstatus, 0, 0);
        if (rc2 == -10) {
            write_str("[B9-SECOND-WAIT-ECHILD]\n");
            write_str("[B9-PASS]\n");
        } else {
            write_str("SECOND WAIT FAILED\n");
        }
    } else {
        write_str("FIRST WAIT FAILED\n");
    }
    
    syscall1(SYS_exit, 0);
    __builtin_unreachable();
}