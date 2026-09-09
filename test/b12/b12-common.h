/*
 * B12 Common Freestanding Helpers
 * Shared syscall wrappers and utilities for B12.x probes.
 * x86_64-linux-musl, no libc.
 */
#ifndef B12_COMMON_H
#define B12_COMMON_H

#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_exit 60
#define SYS_wait4 61
#define SYS_fork 57
#define SYS_execve 59
#define SYS_sched_yield 24
#define SYS_getpid 39
#define SYS_pipe2 293
#define SYS_unlink 87

#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT 64
#define O_TRUNC 512

static inline long syscall0(long n) {
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(n) : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall1(long n, long a1) {
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(n), "D"(a1) : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall2(long n, long a1, long a2) {
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2) : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall3(long n, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall4(long n, long a1, long a2, long a3, long a4) {
    long ret;
    register long r10 __asm__("r10") = a4;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10) : "rcx", "r11", "memory");
    return ret;
}

typedef unsigned long size_t;

void *memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    while (n--) *dp++ = *sp++;
    return d;
}

static void write_str(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    syscall3(SYS_write, STDOUT_FILENO, (long)s, (long)len);
}

static int streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static void write_int(long v) {
    char buf[24];
    int i = 23;
    buf[i] = '\0';
    if (v == 0) {
        buf[--i] = '0';
    } else {
        long neg = 0;
        if (v < 0) { neg = 1; v = -v; }
        while (v > 0) {
            buf[--i] = '0' + (v % 10);
            v /= 10;
        }
        if (neg) buf[--i] = '-';
    }
    write_str(&buf[i]);
}

#endif /* B12_COMMON_H */