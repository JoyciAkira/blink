#include "b12-common.h"

void _start(void) {
    long rdi, rsi, rdx, rsp_val;
    __asm__ volatile (
        "mov %%rdi, %0\n\t"
        "mov %%rsi, %1\n\t"
        "mov %%rdx, %2\n\t"
        "mov %%rsp, %3\n\t"
        : "=r" (rdi), "=r" (rsi), "=r" (rdx), "=r" (rsp_val)
    );
    write_str("rdi="); write_int(rdi); write_str("\n");
    write_str("rsi="); write_int(rsi); write_str("\n");
    write_str("rdx="); write_int(rdx); write_str("\n");
    write_str("rsp="); write_int(rsp_val); write_str("\n");
    
    long *sp = (long *)rsp_val;
    write_str("stack[0]="); write_int(sp[0]); write_str("\n");
    write_str("stack[1]="); write_int(sp[1]); write_str("\n");
    write_str("stack[2]="); write_int(sp[2]); write_str("\n");
    
    syscall1(SYS_exit, 0);
    __builtin_unreachable();
}