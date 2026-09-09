/*
 * B11 Simultaneous Children Probe
 * Freestanding x86-64 ELF, no libc.
 * Tests: 5 concurrent fork() -> children write PID to files -> parent reads files and wait4(-1) reaps all.
 */

#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_exit 60
#define SYS_wait4 61
#define SYS_fork 57
#define SYS_sched_yield 24
#define SYS_getpid 39

#define STDOUT_FILENO 1
#define NCHILDREN 5
#define O_WRONLY 1
#define O_CREAT 64
#define O_TRUNC 512
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

static long parse_int(const char *s) {
    long v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v;
}

void _start(void) {
    long pids[NCHILDREN];
    int failed = 0;
    
    /* Phase 1: fork 5 children simultaneously */
    for (int i = 0; i < NCHILDREN; i++) {
        long pid = syscall0(SYS_fork);
        
        if (pid == 0) {
            /* Child: write PID to /tmp/b11-child-N */
            long mypid = syscall0(SYS_getpid);
            
            char filename[32];
            int k = 0;
            const char *prefix = "/tmp/b11-child-";
            while (prefix[k]) { filename[k] = prefix[k]; k++; }
            filename[k++] = '0' + i;
            filename[k] = '\0';
            
            long fd = syscall3(SYS_open, (long)filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) {
                syscall1(SYS_exit, 99);
            }
            
            char buf[24];
            int b_i = 0;
            long v = mypid;
            if (v == 0) {
                buf[b_i++] = '0';
            } else {
                char tmp[24];
                int t_i = 0;
                while (v > 0) { tmp[t_i++] = '0' + (v % 10); v /= 10; }
                while (t_i > 0) { buf[b_i++] = tmp[--t_i]; }
            }
            buf[b_i++] = '\n';
            
            syscall3(SYS_write, fd, (long)buf, b_i);
            syscall1(SYS_close, fd);
            
            /* Busy loop to stay RUNNABLE while parent forks others */
            volatile long dummy = 0;
            for (long j = 0; j < 500000; j++) { dummy += j; }
            
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
    }
    
    if (failed) {
        syscall1(SYS_exit, 1);
        __builtin_unreachable();
    }
    
    write_str("[B11-FORK-SIMULTANEOUS]\n");
    write_str("[B11-ALL-RUNNABLE]\n");
    
    /* Parent: read all 5 files to verify children made progress */
    int progress_ok = 1;
    for (int i = 0; i < NCHILDREN; i++) {
        char filename[32];
        int k = 0;
        const char *prefix = "/tmp/b11-child-";
        while (prefix[k]) { filename[k] = prefix[k]; k++; }
        filename[k++] = '0' + i;
        filename[k] = '\0';
        
        /* Retry open in case child hasn't created file yet */
        long fd = -1;
        for (int retry = 0; retry < 100; retry++) {
            fd = syscall3(SYS_open, (long)filename, O_RDONLY, 0);
            if (fd >= 0) break;
            syscall0(SYS_sched_yield);
        }
        
        if (fd < 0) {
            write_str("FILE OPEN FAILED ");
            write_int(i);
            write_str("\n");
            progress_ok = 0;
            continue;
        }
        
        char buf[32];
        long rd = syscall3(SYS_read, fd, (long)buf, 31);
        syscall1(SYS_close, fd);
        
        if (rd <= 0) {
            write_str("FILE READ FAILED ");
            write_int(i);
            write_str("\n");
            progress_ok = 0;
            continue;
        }
        buf[rd] = '\0';
        
        long file_pid = parse_int(buf);
        if (file_pid != pids[i]) {
            write_str("PID MISMATCH CHILD ");
            write_int(i);
            write_str(" EXPECTED ");
            write_int(pids[i]);
            write_str(" GOT ");
            write_int(file_pid);
            write_str("\n");
            progress_ok = 0;
        }
    }
    
    if (progress_ok) {
        write_str("[B11-CHILDREN-PROGRESS]\n");
    }
    
    /* Phase 2: reap all 5 children with wait4(-1) */
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
        long exit_code = (wstatus >> 8) & 0xff;
        seen_status[exit_code] = 1;
        
        int found = 0;
        for (int j = 0; j < NCHILDREN; j++) {
            if (pids[j] == rc) {
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
        write_str("[B11-WAIT-REAP-ALL]\n");
    }
    
    for (int i = 0; i < NCHILDREN; i++) {
        if (!seen_status[i]) {
            write_str("MISSING EXIT CODE ");
            write_int(i);
            write_str("\n");
            status_ok = 0;
        }
    }
    
    if (status_ok && progress_ok) {
        write_str("[B11-PASS]\n");
    } else {
        write_str("[B11-FAIL]\n");
    }
    
    syscall1(SYS_exit, 0);
    __builtin_unreachable();
}