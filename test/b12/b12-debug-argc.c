#include "b12-common.h"

void _start(void) {
    long argc;
    char **argv, **envp;
    __asm__ volatile (
        "mov (%%rsp), %0\n\t"
        "lea 8(%%rsp), %1\n\t"
        : "=r" (argc), "=r" (argv)
    );
    envp = argv + argc + 1;
    
    write_str("argc=");
    write_int(argc);
    write_str("\n");
    
    if (argc > 0 && argv) {
        write_str("argv[0]=");
        write_str(argv[0] ? argv[0] : "(null)");
        write_str("\n");
    }
    if (argc > 1 && argv) {
        write_str("argv[1]=");
        write_str(argv[1] ? argv[1] : "(null)");
        write_str("\n");
    }
    
    syscall1(SYS_exit, 0);
    __builtin_unreachable();
}