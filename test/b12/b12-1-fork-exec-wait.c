#include "b12-common.h"

#define TOKEN_PATH "/tmp/b12-1-token"
#define SELF_PATH "/Users/capyco/tombl-build/b12-test/test/b12/b12-1-fork-exec-wait"

void _start(void) {
    /* Child path: token file exists */
    long fd = syscall2(SYS_open, (long)TOKEN_PATH, O_RDONLY);
    if (fd >= 0) {
        syscall1(SYS_close, fd);
        syscall1(SYS_unlink, (long)TOKEN_PATH);
        syscall1(SYS_exit, 0);
    }

    /* Parent path: create token, fork, exec */
    fd = syscall3(SYS_open, (long)TOKEN_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        write_str("[B12.1-TOKEN-CREATE-FAILED]\n");
        syscall1(SYS_exit, 1);
    }
    syscall3(SYS_write, fd, (long)"X", 1);
    syscall1(SYS_close, fd);

    long pid = syscall0(SYS_fork);
    if (pid == 0) {
        const char *argv[] = { SELF_PATH, 0 };
        const char *envp[] = { 0 };
        syscall3(SYS_execve, (long)SELF_PATH, (long)argv, (long)envp);
        write_str("[B12.1-EXEC-FAILED]\n");
        syscall1(SYS_exit, 99);
    }
    if (pid < 0) {
        write_str("[B12.1-FORK-FAILED]\n");
        syscall1(SYS_exit, 1);
    }

    syscall0(SYS_sched_yield);
    int wstatus = 0;
    long rc = syscall4(SYS_wait4, pid, (long)&wstatus, 0, 0);
    if (rc != pid) {
        write_str("[B12.1-WAIT-FAILED]\n");
        syscall1(SYS_exit, 1);
    }
    long exit_code = (wstatus >> 8) & 0xff;
    if (exit_code != 0) {
        write_str("[B12.1-CHILD-EXIT-NONZERO]\n");
        syscall1(SYS_exit, 1);
    }

    syscall1(SYS_unlink, (long)TOKEN_PATH);
    write_str("[B12.1-PASS]\n");
    syscall1(SYS_exit, 0);
}