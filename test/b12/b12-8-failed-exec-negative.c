#include "b12-common.h"

void _start(void) {
    long pid = syscall0(SYS_fork);
    
    if (pid == 0) {
        const char *path = "/nonexistent_binary_12345";
        const char *argv[] = { "nonexistent", 0 };
        const char *envp[] = { 0 };
        long rc = syscall3(SYS_execve, (long)path, (long)argv, (long)envp);
        /* execve should return -1 with ENOENT (2) */
        if (rc == -2) {
            write_str("[B12.8-EXEC-FAILED-ENOENT]\n");
            syscall1(SYS_exit, 0);
        } else {
            write_str("[B12.8-EXEC-UNEXPECTED-RC]\n");
            write_int(rc);
            write_str("\n");
            syscall1(SYS_exit, 1);
        }
        __builtin_unreachable();
    }
    
    if (pid < 0) {
        write_str("[B12.8-FORK-FAILED]\n");
        syscall1(SYS_exit, 1);
    }
    
    syscall0(SYS_sched_yield);
    
    int wstatus = 0;
    long rc = syscall4(SYS_wait4, pid, (long)&wstatus, 0, 0);
    
    if (rc != pid) {
        write_str("[B12.8-WAIT-FAILED]\n");
        syscall1(SYS_exit, 1);
    }
    
    long exit_code = (wstatus >> 8) & 0xff;
    if (exit_code != 0) {
        write_str("[B12.8-CHILD-EXIT-NONZERO]\n");
        syscall1(SYS_exit, 1);
    }
    
    write_str("[B12.8-PASS]\n");
    syscall1(SYS_exit, 0);
    __builtin_unreachable();
}