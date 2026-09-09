#include "b12-common.h"

void _start(void) {
    int fds[2];
    long prc = syscall2(SYS_pipe2, (long)fds, 0);
    
    if (prc < 0) {
        write_str("[B12.7-PIPE-FAILED]\n");
        syscall1(SYS_exit, 1);
    }
    
    long pid = syscall0(SYS_fork);
    
    if (pid == 0) {
        syscall1(SYS_close, fds[0]);
        const char *msg = "CHILD_DATA";
        syscall3(SYS_write, fds[1], (long)msg, 10);
        syscall1(SYS_close, fds[1]);
        syscall1(SYS_exit, 0);
        __builtin_unreachable();
    }
    
    if (pid < 0) {
        write_str("[B12.7-FORK-FAILED]\n");
        syscall1(SYS_exit, 1);
    }
    
    syscall1(SYS_close, fds[1]);
    
    syscall0(SYS_sched_yield);
    
    int wstatus = 0;
    long rc = syscall4(SYS_wait4, pid, (long)&wstatus, 0, 0);
    
    if (rc != pid) {
        write_str("[B12.7-WAIT-FAILED]\n");
        syscall1(SYS_exit, 1);
    }
    
    char buf[32];
    long rd = syscall3(SYS_read, fds[0], (long)buf, 31);
    syscall1(SYS_close, fds[0]);
    
    if (rd != 10) {
        write_str("[B12.7-READ-FAILED]\n");
        syscall1(SYS_exit, 1);
    }
    
    buf[rd] = '\0';
    if (!streq(buf, "CHILD_DATA")) {
        write_str("[B12.7-DATA-MISMATCH]\n");
        syscall1(SYS_exit, 1);
    }
    
    write_str("[B12.7-PASS]\n");
    syscall1(SYS_exit, 0);
    __builtin_unreachable();
}