#include "b12-common.h"

void _start(void) {
    long pid1 = syscall0(SYS_fork);
    
    if (pid1 == 0) {
        /* Child 1: write to stdout and exit 11 */
        write_str("[B12.9-CHILD1]\n");
        syscall1(SYS_exit, 11);
        __builtin_unreachable();
    }
    
    if (pid1 < 0) {
        write_str("[B12.9-FORK1-FAILED]\n");
        syscall1(SYS_exit, 1);
    }
    
    long pid2 = syscall0(SYS_fork);
    
    if (pid2 == 0) {
        /* Child 2: write to stdout and exit 22 */
        write_str("[B12.9-CHILD2]\n");
        syscall1(SYS_exit, 22);
        __builtin_unreachable();
    }
    
    if (pid2 < 0) {
        write_str("[B12.9-FORK2-FAILED]\n");
        syscall1(SYS_exit, 1);
    }
    
    syscall0(SYS_sched_yield);
    
    int reaped1 = 0, reaped2 = 0;
    int status_ok = 1;
    
    for (int i = 0; i < 2; i++) {
        int wstatus = 0;
        long rc = syscall4(SYS_wait4, -1, (long)&wstatus, 0, 0);
        
        if (rc < 0) {
            write_str("[B12.9-WAIT-FAILED]\n");
            status_ok = 0;
            break;
        }
        
        long exit_code = (wstatus >> 8) & 0xff;
        
        if (rc == pid1 && exit_code == 11) {
            reaped1 = 1;
        } else if (rc == pid2 && exit_code == 22) {
            reaped2 = 1;
        } else {
            write_str("[B12.9-UNEXPECTED-CHILD]\n");
            status_ok = 0;
        }
    }
    
    if (status_ok && reaped1 && reaped2) {
        write_str("[B12.9-PASS]\n");
    } else {
        write_str("[B12.9-FAIL]\n");
        status_ok = 0;
    }
    
    syscall1(SYS_exit, status_ok ? 0 : 1);
    __builtin_unreachable();
}