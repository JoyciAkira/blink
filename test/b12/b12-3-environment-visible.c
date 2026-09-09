#include "b12-common.h"

#define TOKEN_PATH "/tmp/b12-3-token"
#define SELF_PATH "/Users/capyco/tombl-build/b12-test/test/b12/b12-3-environment-visible"

void _start(void) {
    /* Child path: read token and verify "FOO=bar" */
    long fd = syscall2(SYS_open, (long)TOKEN_PATH, O_RDONLY);
    if (fd >= 0) {
        char buf[16];
        long rd = syscall3(SYS_read, fd, (long)buf, 15);
        syscall1(SYS_close, fd);
        syscall1(SYS_unlink, (long)TOKEN_PATH);
        
        if (rd == 7 && buf[0] == 'F' && buf[1] == 'O' && buf[2] == 'O' && buf[3] == '=' &&
            buf[4] == 'b' && buf[5] == 'a' && buf[6] == 'r') {
            syscall1(SYS_exit, 0);
        } else {
            write_str("[B12.3-CHILD-TOKEN-MISMATCH]\n");
            syscall1(SYS_exit, 1);
        }
    }

    /* Parent path: write "FOO=bar" to token, fork, exec */
    fd = syscall3(SYS_open, (long)TOKEN_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        write_str("[B12.3-TOKEN-CREATE-FAILED]\n");
        syscall1(SYS_exit, 1);
    }
    syscall3(SYS_write, fd, (long)"FOO=bar", 7);
    syscall1(SYS_close, fd);

    long pid = syscall0(SYS_fork);
    if (pid == 0) {
        const char *argv[] = { SELF_PATH, 0 };
        const char *envp[] = { 0 };
        syscall3(SYS_execve, (long)SELF_PATH, (long)argv, (long)envp);
        write_str("[B12.3-EXEC-FAILED]\n");
        syscall1(SYS_exit, 99);
    }
    if (pid < 0) {
        write_str("[B12.3-FORK-FAILED]\n");
        syscall1(SYS_exit, 1);
    }

    syscall0(SYS_sched_yield);
    int wstatus = 0;
    long rc = syscall4(SYS_wait4, pid, (long)&wstatus, 0, 0);
    if (rc != pid) {
        write_str("[B12.3-WAIT-FAILED]\n");
        syscall1(SYS_exit, 1);
    }
    long exit_code = (wstatus >> 8) & 0xff;
    if (exit_code != 0) {
        write_str("[B12.3-CHILD-EXIT-NONZERO]\n");
        syscall1(SYS_exit, 1);
    }

    syscall1(SYS_unlink, (long)TOKEN_PATH);
    write_str("[B12.3-PASS]\n");
    syscall1(SYS_exit, 0);
}