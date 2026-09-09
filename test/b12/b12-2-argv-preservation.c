#include "b12-common.h"

#define TOKEN_PATH "/tmp/b12-2-token"
#define SELF_PATH "/Users/capyco/tombl-build/b12-test/test/b12/b12-2-argv-preservation"

void _start(void) {
    /* Child path: read token and verify "SENTINEL" */
    long fd = syscall2(SYS_open, (long)TOKEN_PATH, O_RDONLY);
    if (fd >= 0) {
        char buf[16];
        long rd = syscall3(SYS_read, fd, (long)buf, 15);
        syscall1(SYS_close, fd);
        syscall1(SYS_unlink, (long)TOKEN_PATH);
        
        if (rd == 8 && buf[0] == 'S' && buf[1] == 'E' && buf[2] == 'N' && buf[3] == 'T' &&
            buf[4] == 'I' && buf[5] == 'N' && buf[6] == 'E' && buf[7] == 'L') {
            syscall1(SYS_exit, 0);
        } else {
            write_str("[B12.2-CHILD-TOKEN-MISMATCH]\n");
            syscall1(SYS_exit, 1);
        }
    }

    /* Parent path: write "SENTINEL" to token, fork, exec */
    fd = syscall3(SYS_open, (long)TOKEN_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        write_str("[B12.2-TOKEN-CREATE-FAILED]\n");
        syscall1(SYS_exit, 1);
    }
    syscall3(SYS_write, fd, (long)"SENTINEL", 8);
    syscall1(SYS_close, fd);

    long pid = syscall0(SYS_fork);
    if (pid == 0) {
        const char *argv[] = { SELF_PATH, 0 };
        const char *envp[] = { 0 };
        syscall3(SYS_execve, (long)SELF_PATH, (long)argv, (long)envp);
        write_str("[B12.2-EXEC-FAILED]\n");
        syscall1(SYS_exit, 99);
    }
    if (pid < 0) {
        write_str("[B12.2-FORK-FAILED]\n");
        syscall1(SYS_exit, 1);
    }

    syscall0(SYS_sched_yield);
    int wstatus = 0;
    long rc = syscall4(SYS_wait4, pid, (long)&wstatus, 0, 0);
    if (rc != pid) {
        write_str("[B12.2-WAIT-FAILED]\n");
        syscall1(SYS_exit, 1);
    }
    long exit_code = (wstatus >> 8) & 0xff;
    if (exit_code != 0) {
        write_str("[B12.2-CHILD-EXIT-NONZERO]\n");
        syscall1(SYS_exit, 1);
    }

    syscall1(SYS_unlink, (long)TOKEN_PATH);
    write_str("[B12.2-PASS]\n");
    syscall1(SYS_exit, 0);
}