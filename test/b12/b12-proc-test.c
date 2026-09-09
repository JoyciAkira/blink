#include "b12-common.h"

void _start(void) {
    long fd = syscall2(SYS_open, (long)"/proc/self/cmdline", O_RDONLY);
    if (fd < 0) {
        write_str("CMDLINE_OPEN_FAILED\n");
        write_int(fd);
        write_str("\n");
    } else {
        char buf[64];
        long rd = syscall3(SYS_read, fd, (long)buf, 63);
        syscall1(SYS_close, fd);
        write_str("CMDLINE_RD=");
        write_int(rd);
        write_str("\n");
        if (rd > 0) {
            buf[rd] = '\0';
            write_str("CMDLINE=");
            write_str(buf);
            write_str("\n");
        }
    }

    long fd2 = syscall2(SYS_open, (long)"/proc/self/environ", O_RDONLY);
    if (fd2 < 0) {
        write_str("ENVIRON_OPEN_FAILED\n");
        write_int(fd2);
        write_str("\n");
    } else {
        char buf[64];
        long rd = syscall3(SYS_read, fd2, (long)buf, 63);
        syscall1(SYS_close, fd2);
        write_str("ENVIRON_RD=");
        write_int(rd);
        write_str("\n");
    }
    
    syscall1(SYS_exit, 0);
    __builtin_unreachable();
}