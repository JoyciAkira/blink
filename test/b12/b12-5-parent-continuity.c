#include "b12-common.h"

volatile long sentinel = 0xDEADBEEFCAFEBABEL;

void _start(void) {
    long pid = syscall0(SYS_fork);
    
    if (pid == 0) {
        syscall1(SYS_exit, 0);
        __builtin_unreachable();
    }
    
    if (pid < 0) {
        write_str("[B12.5-FORK-FAILED]\n");
        syscall1(SYS_exit, 1);
    }
    
    syscall0(SYS_sched_yield);
    
    int wstatus = 0;
    long rc = syscall4(SYS_wait4, pid, (long)&wstatus, 0, 0);
    
    if (rc != pid) {
        write_str("[B12.5-WAIT-FAILED]\n");
        syscall1(SYS_exit, 1);
    }
    
    if (sentinel != 0xDEADBEEFCAFEBABEL) {
        write_str("[B12.5-SENTINEL-CORRUPTED]\n");
        syscall1(SYS_exit, 1);
    }
    
    write_str("[B12.5-PASS]\n");
    syscall1(SYS_exit, 0);
    __builtin_unreachable();
}