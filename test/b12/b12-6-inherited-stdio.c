#include "b12-common.h"

void _start(void) {
    long pid = syscall0(SYS_fork);
    
    if (pid == 0) {
        write_str("[B12.6-CHILD-STDOUT]\n");
        syscall1(SYS_exit, 0);
        __builtin_unreachable();
    }
    
    if (pid < 0) {
        write_str("[B12.6-FORK-FAILED]\n");
        syscall1(SYS_exit, 1);
    }
    
    syscall0(SYS_sched_yield);
    
    int wstatus = 0;
    long rc = syscall4(SYS_wait4, pid, (long)&wstatus, 0, 0);
    
    if (rc != pid) {
        write_str("[B12.6-WAIT-FAILED]\n");
        syscall1(SYS_exit, 1);
    }
    
    write_str("[B12.6-PASS]\n");
    syscall1(SYS_exit, 0);
    __builtin_unreachable();
}