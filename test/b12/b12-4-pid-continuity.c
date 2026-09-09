#include "b12-common.h"

void _start(void) {
    long pid_before = syscall0(SYS_getpid);
    
    long pid = syscall0(SYS_fork);
    
    if (pid == 0) {
        syscall1(SYS_exit, 0);
        __builtin_unreachable();
    }
    
    if (pid < 0) {
        write_str("[B12.4-FORK-FAILED]\n");
        syscall1(SYS_exit, 1);
    }
    
    syscall0(SYS_sched_yield);
    
    int wstatus = 0;
    long rc = syscall4(SYS_wait4, pid, (long)&wstatus, 0, 0);
    
    if (rc != pid) {
        write_str("[B12.4-WAIT-FAILED]\n");
        syscall1(SYS_exit, 1);
    }
    
    long pid_after = syscall0(SYS_getpid);
    
    if (pid_before != pid_after) {
        write_str("[B12.4-PID-CHANGED]\n");
        syscall1(SYS_exit, 1);
    }
    
    write_str("[B12.4-PASS]\n");
    syscall1(SYS_exit, 0);
    __builtin_unreachable();
}